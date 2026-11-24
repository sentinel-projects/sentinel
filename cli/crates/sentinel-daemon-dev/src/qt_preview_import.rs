//! Staged, deliberately incomplete Qt-copy preview. No Qt profile is accessed.
use std::{
    fs::{self, File},
    io,
    os::unix::fs::{DirBuilderExt, MetadataExt},
    path::{Path, PathBuf},
};

fn invalid() -> io::Error {
    io::Error::new(
        io::ErrorKind::InvalidInput,
        "unsafe or unsupported Qt preview copies",
    )
}
struct Stage(Option<PathBuf>);
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
        && a.uid() == b.uid()
        && a.mode() == b.mode()
        && a.nlink() == b.nlink()
}
pub struct PreviewImport {
    pub conversations: usize,
    pub archived: usize,
    pub pinned: usize,
    pub messages: usize,
    pub memory: usize,
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
        let file = fs::symlink_metadata(path)?;
        if !file.file_type().is_file()
            || file.uid() != unsafe { libc::geteuid() }
            || file.nlink() != 1
            || !matches!(file.mode() & 0o7777, 0o400 | 0o600)
            || identities
                .iter()
                .any(|(prior, _): &(fs::Metadata, fs::Metadata)| {
                    prior.dev() == file.dev() && prior.ino() == file.ino()
                })
        {
            return Err(invalid().into());
        }
        identities.push((file, directory));
    }
    let mut stage = None;
    for n in 0..128 {
        let path =
            destination_parent.join(format!(".sentinel-qt-preview-{}-{n}", std::process::id()));
        match fs::DirBuilder::new().mode(0o700).create(&path) {
            Ok(()) => {
                stage = Some(path);
                break;
            }
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => {}
            Err(e) => return Err(e.into()),
        }
    }
    let mut stage = Stage(Some(stage.ok_or_else(invalid)?));
    let root = stage.0.as_ref().ok_or_else(invalid)?;
    let _lock = crate::unix::lock_profile(root)?;
    drop(crate::store::Store::open(root)?);
    drop(crate::settings::Settings::open(root)?);
    drop(crate::memory::MemoryStore::open(root)?);
    drop(crate::agent_store::AgentStore::open(root)?);
    let transcript = crate::import::read(conversations, root)?;
    let mut chat = crate::store::Store::open_for_import(root)?;
    chat.import_snapshot(&transcript)?;
    let sessions = chat.load()?.into_iter().map(|s| s.id).collect::<Vec<_>>();
    drop(chat);
    let memory_snapshot = crate::memory_import::read(memory, root, &sessions)?;
    let mut destination_memory = crate::memory::MemoryStore::open_for_import(root)?;
    destination_memory.import_snapshot(&memory_snapshot)?;
    drop(destination_memory);
    let settings_report = crate::qt_settings_import::import_preview(root, settings)?;
    transcript.validate_unchanged()?;
    memory_snapshot.validate_unchanged()?;
    for (path, (file, directory)) in paths.into_iter().zip(&identities) {
        let current = fs::symlink_metadata(path)?;
        let current_directory = fs::symlink_metadata(path.parent().ok_or_else(invalid)?)?;
        if !same(file, &current)
            || directory.dev() != current_directory.dev()
            || directory.ino() != current_directory.ino()
            || directory.uid() != current_directory.uid()
            || directory.mode() != current_directory.mode()
        {
            return Err(invalid().into());
        }
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
        memory: memory_snapshot.rows.len(),
        settings: settings_report,
    };
    File::open(root)?.sync_all()?;
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
