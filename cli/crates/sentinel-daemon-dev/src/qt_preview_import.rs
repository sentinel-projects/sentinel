//! Staged, deliberately incomplete Qt-copy preview. No Qt profile is accessed.
use sha2::{Digest, Sha256};
use std::{
    fs::{self, File, OpenOptions},
    io::{self, BufReader, Read, Seek, SeekFrom, Write},
    os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt},
    path::{Path, PathBuf},
};

fn invalid() -> io::Error {
    io::Error::new(
        io::ErrorKind::InvalidInput,
        "unsafe or unsupported Qt preview copies",
    )
}
pub(crate) struct Stage(pub(crate) Option<PathBuf>);
impl Drop for Stage {
    fn drop(&mut self) {
        if let Some(path) = self.0.take() {
            let _ = fs::remove_dir_all(path);
        }
    }
}
fn same(a: &fs::Metadata, b: &fs::Metadata) -> bool {
    a.dev() == b.dev()
        && a.ino() == b.ino()
        && a.len() == b.len()
        && a.mtime() == b.mtime()
        && a.mtime_nsec() == b.mtime_nsec()
        && a.ctime() == b.ctime()
        && a.ctime_nsec() == b.ctime_nsec()
        && a.uid() == b.uid()
        && a.mode() == b.mode()
        && a.nlink() == b.nlink()
}
const MAX_SOURCE_BYTES: u64 = 128 * 1024 * 1024;

fn read_source(path: &Path, identity: &fs::Metadata, writer: &mut impl Write) -> io::Result<()> {
    if identity.len() > MAX_SOURCE_BYTES {
        return Err(invalid());
    }
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK)
        .open(path)?;
    if !same(identity, &file.metadata()?) {
        return Err(invalid());
    }
    let mut reader = (&file).take(identity.len() + 1);
    let bytes_read = io::copy(&mut reader, writer)?;
    if bytes_read != identity.len()
        || !same(identity, &file.metadata()?)
        || !same(identity, &fs::symlink_metadata(path)?)
    {
        return Err(invalid());
    }
    Ok(())
}

fn fingerprint(path: &Path, identity: &fs::Metadata) -> io::Result<[u8; 32]> {
    let mut hasher = Sha256::new();
    read_source(path, identity, &mut hasher)?;
    Ok(hasher.finalize().into())
}

fn source_file(path: &Path) -> io::Result<fs::Metadata> {
    let file = fs::symlink_metadata(path)?;
    if !file.file_type().is_file()
        || file.uid() != unsafe { libc::geteuid() }
        || file.nlink() != 1
        || !matches!(file.mode() & 0o7777, 0o400 | 0o600)
    {
        return Err(invalid());
    }
    Ok(file)
}

fn sidecar(path: &Path, suffix: &str) -> io::Result<PathBuf> {
    let mut name = path.file_name().ok_or_else(invalid)?.to_os_string();
    name.push(suffix);
    Ok(path.with_file_name(name))
}

pub(crate) fn temporary_stage(parent: &Path, label: &str) -> io::Result<Stage> {
    for n in 0..128 {
        let path = parent.join(format!(".sentinel-qt-{label}-{}-{n}", std::process::id()));
        match fs::DirBuilder::new().mode(0o700).create(&path) {
            Ok(()) => return Ok(Stage(Some(path))),
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(e),
        }
    }
    Err(invalid())
}

fn copy_source(
    from: &Path,
    metadata: &fs::Metadata,
    digest: &[u8; 32],
    to: &Path,
) -> io::Result<()> {
    let mut output = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(to)?;
    read_source(from, metadata, &mut output)?;
    drop(output);
    if fingerprint(to, &fs::symlink_metadata(to)?)? != *digest {
        return Err(invalid());
    }
    Ok(())
}

fn finalize_wal_copy(path: &Path, wal: &Path) -> Result<(), Box<dyn std::error::Error>> {
    let file = File::open(wal)?;
    let wal_len = file.metadata()?.len();
    let mut reader = BufReader::new(file);
    let mut header = [0; 32];
    reader.read_exact(&mut header)?;
    let page_size = u64::from(u32::from_be_bytes(header[8..12].try_into()?));
    if !matches!(&header[..4], [0x37, 0x7f, 0x06, 0x82 | 0x83])
        || !(512..=65_536).contains(&page_size)
        || !page_size.is_power_of_two()
        || wal_len <= 32
        || (wal_len - 32) % (page_size + 24) != 0
    {
        return Err(invalid().into());
    }
    let expected = (wal_len - 32) / (page_size + 24);
    // A corrupt page number or commit size must not expand the staged database
    // beyond the same size admitted by the regular finalized-copy importer.
    let max_page = MAX_SOURCE_BYTES / page_size;
    let mut frame = [0; 24];
    for _ in 0..expected {
        reader.read_exact(&mut frame)?;
        let page = u64::from(u32::from_be_bytes(frame[..4].try_into()?));
        let commit_pages = u64::from(u32::from_be_bytes(frame[4..8].try_into()?));
        if page == 0 || page > max_page || commit_pages > max_page {
            return Err(invalid().into());
        }
        reader.seek(SeekFrom::Current(i64::try_from(page_size)?))?;
    }
    if frame[4..8] == [0; 4] {
        return Err(invalid().into());
    }

    let db = rusqlite::Connection::open_with_flags(
        path,
        rusqlite::OpenFlags::SQLITE_OPEN_READ_WRITE | rusqlite::OpenFlags::SQLITE_OPEN_NOFOLLOW,
    )?;
    db.busy_timeout(std::time::Duration::from_millis(250))?;
    db.execute_batch("PRAGMA trusted_schema=OFF;")?;
    let (busy, frames, written): (i64, i64, i64) =
        db.query_row("PRAGMA wal_checkpoint(FULL)", [], |r| {
            Ok((r.get(0)?, r.get(1)?, r.get(2)?))
        })?;
    if busy != 0 || frames != written || u64::try_from(frames).ok() != Some(expected) {
        return Err(invalid().into());
    }
    let mode: String = db.query_row("PRAGMA journal_mode=DELETE", [], |r| r.get(0))?;
    if mode != "delete" {
        return Err(invalid().into());
    }
    db.close().map_err(|(_, error)| error)?;
    crate::import::no_sidecars(path)?;
    Ok(())
}

pub struct PreviewImport {
    pub conversations: usize,
    pub archived: usize,
    pub pinned: usize,
    pub messages: usize,
    pub attachments: usize,
    pub reply_references: usize,
    pub replacement_references: usize,
    pub memory: usize,
    pub normalized_wal_copies: usize,
    pub source_id: String,
    pub excluded_memory: crate::memory_import::ExcludedMemory,
    pub settings: crate::qt_settings_import::SettingsImport,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    Check,
    Publish,
}

pub fn import(
    destination: &Path,
    conversations: &Path,
    settings: &Path,
    memory: &Path,
    persistent: bool,
    mode: Mode,
    normalize_wal: bool,
    expected_source_id: Option<&str>,
    allow_excluded_memory: bool,
) -> Result<PreviewImport, Box<dyn std::error::Error>> {
    let tmp = Path::new("/tmp").canonicalize()?;
    let destination_parent = destination.parent().ok_or_else(invalid)?.canonicalize()?;
    if !destination.is_absolute()
        || (!persistent && destination_parent != tmp)
        || destination.file_name().is_none()
        || !matches!(fs::symlink_metadata(destination), Err(error) if error.kind() == io::ErrorKind::NotFound)
    {
        return Err(invalid().into());
    }
    let paths = [conversations, settings, memory];
    let mut identities = Vec::new();
    let mut source_directory = None;
    for path in paths {
        let parent = path.parent().ok_or_else(invalid)?;
        let directory = fs::symlink_metadata(parent)?;
        if !path.is_absolute()
            || !directory.file_type().is_dir()
            || directory.uid() != unsafe { libc::geteuid() }
            || directory.mode() & 0o7777 != 0o700
            || parent.canonicalize()?.parent() != Some(tmp.as_path())
        {
            return Err(invalid().into());
        }
        let directory_id = (directory.dev(), directory.ino());
        if source_directory.is_some_and(|prior| prior != directory_id) {
            return Err(invalid().into());
        }
        source_directory = Some(directory_id);
        let file = source_file(path)?;
        if identities
            .iter()
            .any(|(prior, _, _): &(fs::Metadata, fs::Metadata, [u8; 32])| {
                prior.dev() == file.dev() && prior.ino() == file.ino()
            })
        {
            return Err(invalid().into());
        }
        let digest = fingerprint(path, &file)?;
        identities.push((file, directory, digest));
    }
    let mut wal_sources = Vec::new();
    if normalize_wal {
        for (index, path) in [(0, conversations), (2, memory)] {
            for suffix in ["-shm", "-journal"] {
                if !matches!(fs::symlink_metadata(sidecar(path, suffix)?), Err(e) if e.kind() == io::ErrorKind::NotFound)
                {
                    return Err(invalid().into());
                }
            }
            let wal = sidecar(path, "-wal")?;
            if paths.contains(&wal.as_path()) {
                return Err(invalid().into());
            }
            let identity = match source_file(&wal) {
                Ok(identity) => Some((fingerprint(&wal, &identity)?, identity)),
                Err(error) if error.kind() == io::ErrorKind::NotFound => None,
                Err(error) => return Err(error.into()),
            };
            wal_sources.push((index, wal, identity));
        }
    }
    let mut source_hasher = Sha256::new();
    source_hasher.update(b"sentinel-qt-preview-source-v1");
    if allow_excluded_memory {
        source_hasher.update(b"allow-excluded-memory");
    }
    for (_, _, digest) in &identities {
        source_hasher.update(digest);
    }
    for (_, _, identity) in &wal_sources {
        if let Some((digest, _)) = identity {
            source_hasher.update([1]);
            source_hasher.update(digest);
        } else {
            source_hasher.update([0]);
        }
    }
    let source_id = format!("{:x}", source_hasher.finalize());
    if expected_source_id.is_some_and(|expected| expected != source_id) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Qt source differs from the preflight receipt",
        )
        .into());
    }
    let mut normalized = [None, None, None];
    let mut normalized_wal_copies = 0;
    let staging_copies = if wal_sources
        .iter()
        .any(|(_, _, identity)| identity.is_some())
    {
        Some(temporary_stage(&tmp, "wal-source")?)
    } else {
        None
    };
    for (index, wal, identity) in &wal_sources {
        if let Some((digest, metadata)) = identity {
            let source_root = staging_copies
                .as_ref()
                .and_then(|stage| stage.0.as_ref())
                .ok_or_else(invalid)?;
            let copy = source_root.join(format!("source-{index}.sqlite3"));
            let wal_copy = sidecar(&copy, "-wal")?;
            copy_source(
                paths[*index],
                &identities[*index].0,
                &identities[*index].2,
                &copy,
            )?;
            copy_source(wal, metadata, digest, &wal_copy)?;
            finalize_wal_copy(&copy, &wal_copy)?;
            normalized[*index] = Some(copy);
            normalized_wal_copies += 1;
        }
    }
    let mut stage = temporary_stage(&destination_parent, "preview")?;
    let root = stage.0.as_ref().ok_or_else(invalid)?;
    let _lock = crate::unix::lock_profile(root)?;
    drop(crate::store::Store::open(root)?);
    drop(crate::settings::Settings::open(root)?);
    drop(crate::memory::MemoryStore::open(root)?);
    drop(crate::agent_store::AgentStore::open(root)?);
    let transcript = crate::import::read(normalized[0].as_deref().unwrap_or(conversations), root)?;
    let mut chat = crate::store::Store::open_for_import(root)?;
    chat.import_snapshot(&transcript)?;
    let sessions = chat.load()?.into_iter().map(|s| s.id).collect::<Vec<_>>();
    drop(chat);
    let memory_snapshot = crate::memory_import::read_preview(
        normalized[2].as_deref().unwrap_or(memory),
        root,
        &sessions,
        allow_excluded_memory,
    )?;
    let mut destination_memory = crate::memory::MemoryStore::open_for_import(root)?;
    destination_memory.import_snapshot(&memory_snapshot)?;
    drop(destination_memory);
    let settings_report = crate::qt_settings_import::import_preview(root, settings)?;
    for database in [
        crate::store::DATABASE,
        crate::settings::DATABASE,
        crate::memory::DATABASE,
        crate::agent_store::DATABASE,
    ] {
        let path = root.join(database);
        crate::import::no_sidecars(&path)?;
        File::open(path)?.sync_all()?;
    }
    File::open(root)?.sync_all()?;
    transcript.validate_unchanged()?;
    memory_snapshot.validate_unchanged()?;
    for (path, (file, directory, digest)) in paths.into_iter().zip(&identities) {
        let current = fs::symlink_metadata(path)?;
        let current_directory = fs::symlink_metadata(path.parent().ok_or_else(invalid)?)?;
        if !same(file, &current)
            || directory.dev() != current_directory.dev()
            || directory.ino() != current_directory.ino()
            || directory.uid() != current_directory.uid()
            || directory.mode() != current_directory.mode()
            || &fingerprint(path, file)? != digest
        {
            return Err(invalid().into());
        }
    }
    for (index, wal, identity) in &wal_sources {
        for suffix in ["-shm", "-journal"] {
            if !matches!(fs::symlink_metadata(sidecar(paths[*index], suffix)?), Err(e) if e.kind() == io::ErrorKind::NotFound)
            {
                return Err(invalid().into());
            }
        }
        match identity {
            Some((digest, metadata)) if &fingerprint(wal, metadata)? == digest => {}
            None if matches!(fs::symlink_metadata(wal), Err(e) if e.kind() == io::ErrorKind::NotFound) =>
                {}
            _ => return Err(invalid().into()),
        }
    }
    let mut attachments = 0;
    let mut reply_references = 0;
    let mut replacement_references = 0;
    for message in transcript
        .conversations
        .iter()
        .flat_map(|entry| &entry.messages)
    {
        attachments +=
            serde_json::from_str::<Vec<serde::de::IgnoredAny>>(&message.attachments)?.len();
        reply_references += usize::from(message.reply_to > 0);
        replacement_references += usize::from(message.replaces > 0);
    }
    let report = PreviewImport {
        conversations: transcript.conversations.len(),
        archived: transcript
            .conversations
            .iter()
            .filter(|entry| entry.archived)
            .count(),
        pinned: transcript
            .conversations
            .iter()
            .filter(|entry| entry.pinned)
            .count(),
        messages: transcript
            .conversations
            .iter()
            .map(|entry| entry.messages.len())
            .sum(),
        attachments,
        reply_references,
        replacement_references,
        memory: memory_snapshot.rows.len(),
        normalized_wal_copies,
        source_id,
        excluded_memory: memory_snapshot.excluded,
        settings: settings_report,
    };
    if mode == Mode::Check {
        fs::remove_dir_all(root)?;
        stage.0 = None;
        return Ok(report);
    }
    let parent = File::open(destination.parent().ok_or_else(invalid)?)?;
    crate::set_restore::publish(root, destination)?;
    stage.0 = None;
    parent.sync_all().map_err(|e| {
        io::Error::new(
            e.kind(),
            format!("preview profile is visible, but directory sync failed: {e}"),
        )
    })?;
    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn committed_wal_rows_survive_normalization_of_a_detached_copy() {
        let tmp = Path::new("/tmp").canonicalize().unwrap();
        let fixture = temporary_stage(&tmp, "wal-fixture").unwrap();
        let copy = temporary_stage(&tmp, "wal-test-copy").unwrap();
        let original = fixture.0.as_ref().unwrap().join("source.sqlite3");
        let destination = copy.0.as_ref().unwrap().join("source.sqlite3");
        let db = rusqlite::Connection::open(&original).unwrap();
        db.execute_batch(
            "CREATE TABLE sample(value INTEGER);
             PRAGMA journal_mode=WAL;
             PRAGMA wal_autocheckpoint=0;
             INSERT INTO sample VALUES (42);",
        )
        .unwrap();
        let original_wal = sidecar(&original, "-wal").unwrap();
        let original_metadata = fs::symlink_metadata(&original_wal).unwrap();
        let original_digest = fingerprint(&original_wal, &original_metadata).unwrap();
        let destination_wal = sidecar(&destination, "-wal").unwrap();
        fs::copy(&original, &destination).unwrap();
        fs::copy(&original_wal, &destination_wal).unwrap();

        finalize_wal_copy(&destination, &destination_wal).unwrap();
        crate::import::no_sidecars(&destination).unwrap();
        let imported = rusqlite::Connection::open(&destination).unwrap();
        let value: i64 = imported
            .query_row("SELECT value FROM sample", [], |row| row.get(0))
            .unwrap();
        assert_eq!(value, 42);
        assert_eq!(
            fingerprint(&original_wal, &original_metadata).unwrap(),
            original_digest
        );
    }
}
