//! Coordinated migration of the supported Qt profile subset on Unix.
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::{
    fs::{self, File, OpenOptions},
    io::{self, Read, Write},
    os::{
        fd::AsRawFd,
        unix::fs::{MetadataExt, OpenOptionsExt},
    },
    path::{Path, PathBuf},
};

const MARKER: &str = ".sentinel-rust-cutover";
const INPUTS: [&str; 3] = ["conversations.sqlite3", "settings.json", "memory.sqlite3"];
const LIMIT: u64 = 128 * 1024 * 1024;

fn invalid(reason: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, reason)
}

#[derive(Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
struct Reservation {
    version: u8,
    source_id: String,
    destination: PathBuf,
}

fn regular(path: &Path, private: bool) -> io::Result<File> {
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK | libc::O_CLOEXEC)
        .open(path)?;
    let metadata = file.metadata()?;
    if !metadata.is_file()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.nlink() != 1
        || metadata.mode() & 0o022 != 0
        || (private && metadata.mode() & 0o7777 != 0o600)
        || metadata.len() > LIMIT
    {
        return Err(invalid("unsafe Qt cutover file"));
    }
    Ok(file)
}

fn copy(path: &Path, output: &Path) -> io::Result<()> {
    let mut file = regular(path, false)?;
    let before = file.metadata()?;
    let mut stage = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(output)?;
    let count = io::copy(&mut (&mut file).take(before.len() + 1), &mut stage)?;
    let after = file.metadata()?;
    let current = fs::symlink_metadata(path)?;
    if count != before.len()
        || before.len() != after.len()
        || before.ctime() != after.ctime()
        || before.ctime_nsec() != after.ctime_nsec()
        || after.ino() != current.ino()
        || after.dev() != current.dev()
    {
        return Err(invalid("Qt cutover source changed while copying"));
    }
    stage.sync_all()
}

fn sidecar(filename: &Path, suffix: &str) -> PathBuf {
    let mut name = filename.as_os_str().to_os_string();
    name.push(suffix);
    PathBuf::from(name)
}

fn require_absent(path: &Path) -> io::Result<()> {
    match fs::symlink_metadata(path) {
        Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(error),
        Ok(_) => Err(invalid(
            "Qt cutover requires closed SQLite stores without SHM or rollback journals",
        )),
    }
}

fn reserve(
    root: &Path,
    directory: &File,
    wanted: &Reservation,
) -> Result<(), Box<dyn std::error::Error>> {
    let path = root.join(MARKER);
    match regular(&path, true) {
        Ok(file) => {
            let existing: Reservation = serde_json::from_reader(file.take(8192))?;
            if existing != *wanted {
                return Err(invalid("Qt profile has a different cutover reservation").into());
            }
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => {
            // A same-filesystem staging directory makes the marker publication
            // atomic and avoids leaving a partially written reservation.
            let stage = crate::qt_preview_import::temporary_stage(root, "reservation")?;
            let staged = stage
                .0
                .as_ref()
                .ok_or_else(|| invalid("missing reservation stage"))?
                .join("marker");
            let mut file = OpenOptions::new()
                .write(true)
                .create_new(true)
                .mode(0o600)
                .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
                .open(&staged)?;
            serde_json::to_writer(&mut file, wanted)?;
            file.write_all(b"\n")?;
            file.sync_all()?;
            drop(file);
            crate::set_restore::publish(&staged, &path)?;
        }
        Err(error) => return Err(error.into()),
    }
    directory.sync_all()?;
    Ok(())
}

fn acquire(source: &Path) -> Result<(PathBuf, File), Box<dyn std::error::Error>> {
    if !source.is_absolute() || !fs::symlink_metadata(source)?.is_dir() {
        return Err(invalid("Qt cutover source must be an absolute, real directory").into());
    }
    let root = source.canonicalize()?;
    let before = fs::symlink_metadata(&root)?;
    let directory = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(&root)?;
    let actual = directory.metadata()?;
    if actual.uid() != unsafe { libc::geteuid() }
        || actual.mode() & 0o022 != 0
        || before.dev() != actual.dev()
        || before.ino() != actual.ino()
    {
        return Err(invalid("unsafe Qt cutover source directory").into());
    }
    if unsafe { libc::flock(directory.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
        return Err(io::Error::other("Qt profile has an active writer or another cutover").into());
    }
    Ok((root, directory))
}

pub fn abort_unpublished(
    source: &Path,
    expected_id: &str,
) -> Result<Value, Box<dyn std::error::Error>> {
    let (root, directory) = acquire(source)?;
    let marker = root.join(MARKER);
    let reservation: Reservation = serde_json::from_reader(regular(&marker, true)?.take(8192))?;
    if reservation.version != 1
        || reservation.source_id != expected_id
        || !reservation.destination.is_absolute()
    {
        return Err(invalid("Qt cutover reservation identity mismatch").into());
    }
    match fs::symlink_metadata(&reservation.destination) {
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => return Err(error.into()),
        Ok(_) => {
            return Err(
                invalid("Rust destination exists; abort requires separate rollback").into(),
            );
        }
    }
    fs::remove_file(&marker)?;
    directory.sync_all()?;
    Ok(
        json!({"schema_version":1,"outcome":"unpublished_reservation_released",
        "source_id":expected_id}),
    )
}

pub fn run(
    source: &Path,
    destination: &Path,
    persistent: bool,
    expected_id: Option<&str>,
) -> Result<Value, Box<dyn std::error::Error>> {
    let (root, directory) = acquire(source)?;
    let actual = directory.metadata()?;
    let parent = destination
        .parent()
        .ok_or_else(|| invalid("missing Rust profile parent"))?
        .canonicalize()?;
    let destination = parent.join(
        destination
            .file_name()
            .ok_or_else(|| invalid("missing Rust profile name"))?,
    );
    if destination.starts_with(&root) || root.starts_with(&destination) {
        return Err(invalid("Qt source and Rust destination must be separate").into());
    }
    let stage =
        crate::qt_preview_import::temporary_stage(&Path::new("/tmp").canonicalize()?, "cutover")?;
    let staged = stage
        .0
        .as_ref()
        .ok_or_else(|| invalid("missing cutover stage"))?;
    for name in INPUTS {
        let from = root.join(name);
        let to = staged.join(name);
        copy(&from, &to)?;
        if name.ends_with(".sqlite3") {
            for suffix in ["-shm", "-journal"] {
                require_absent(&sidecar(&from, suffix))?;
            }
            let wal = sidecar(&from, "-wal");
            match fs::symlink_metadata(&wal) {
                Ok(_) => copy(&wal, &sidecar(&to, "-wal"))?,
                Err(error) if error.kind() == io::ErrorKind::NotFound => {}
                Err(error) => return Err(error.into()),
            }
        }
    }
    let inputs = INPUTS.map(|name| staged.join(name));
    let preview = crate::qt_preview_import::import(
        &destination,
        &inputs[0],
        &inputs[1],
        &inputs[2],
        persistent,
        crate::qt_preview_import::Mode::Check,
        true,
        expected_id,
        false,
    )?;
    if let Some(expected) = expected_id {
        let current = fs::symlink_metadata(&root)?;
        if current.dev() != actual.dev() || current.ino() != actual.ino() {
            return Err(invalid("Qt cutover source directory changed").into());
        }
        reserve(
            &root,
            &directory,
            &Reservation {
                version: 1,
                source_id: expected.to_owned(),
                destination: destination.clone(),
            },
        )?;
        crate::qt_preview_import::import(
            &destination,
            &inputs[0],
            &inputs[1],
            &inputs[2],
            persistent,
            crate::qt_preview_import::Mode::Publish,
            true,
            Some(expected),
            false,
        )
        .map_err(|error| {
            io::Error::other(format!(
                "Qt write reservation retained; Rust publication requires reconciliation: {error}"
            ))
        })?;
    }
    Ok(json!({
        "schema_version":1,
        "outcome":if expected_id.is_some() { "published" } else { "validated" },
        "source_id":preview.source_id,
        "conversations":preview.conversations,
        "messages":preview.messages,
        "session_memory":preview.memory,
        "preferences":preview.settings.preferences,
        "qt_write_reservation":expected_id.is_some(),
        "excluded":["other_settings","permissions","credentials","agent_audit","workspaces","other_storage_domains"],
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shared_writer_blocks_cutover_and_reservation_blocks_abort_after_publication() {
        let temporary = Path::new("/tmp").canonicalize().unwrap();
        let fixture =
            crate::qt_preview_import::temporary_stage(&temporary, "lease-fixture").unwrap();
        let source = fixture.0.as_ref().unwrap();
        let writer = File::open(source).unwrap();
        assert_eq!(
            unsafe { libc::flock(writer.as_raw_fd(), libc::LOCK_SH | libc::LOCK_NB) },
            0
        );
        assert!(acquire(source).is_err());
        drop(writer);

        let (_, exclusive) = acquire(source).unwrap();
        let destination = source.join("fixture-destination");
        let reservation = Reservation {
            version: 1,
            source_id: "a".repeat(64),
            destination: destination.clone(),
        };
        reserve(source, &exclusive, &reservation).unwrap();
        reserve(source, &exclusive, &reservation).unwrap();
        drop(exclusive);
        fs::create_dir(&destination).unwrap();
        assert!(abort_unpublished(source, &reservation.source_id).is_err());
        assert!(source.join(MARKER).exists());
        fs::remove_dir(&destination).unwrap();
        assert!(abort_unpublished(source, "incorrect").is_err());
        abort_unpublished(source, &reservation.source_id).unwrap();
        assert!(!source.join(MARKER).exists());
    }
}
