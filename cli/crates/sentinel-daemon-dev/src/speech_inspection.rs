//! Explicit, read-only speech artifact inspection. This never executes a provider.
use sha2::{Digest, Sha256};
use std::{
    fs::{self, OpenOptions},
    io::{self, Read},
    os::unix::fs::{MetadataExt, OpenOptionsExt},
    path::{Path, PathBuf},
    sync::atomic::{AtomicBool, Ordering},
    time::Instant,
};

const MAX_BINARY: u64 = 256 * 1024 * 1024;
const MAX_MODEL: u64 = 4 * 1024 * 1024 * 1024;

#[derive(Clone)]
pub struct Inspection {
    pub binary_sha256: String,
    pub model_sha256: String,
    pub model_bytes: u64,
    pub(crate) binary: PathBuf,
    pub(crate) model: PathBuf,
}

fn inspect(
    path: &Path,
    expected: &str,
    limit: u64,
    model: bool,
    stop: Option<(&AtomicBool, Instant)>,
) -> io::Result<u64> {
    if !path.is_absolute()
        || expected.len() != 64
        || !expected
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "invalid speech artifact configuration",
        ));
    }
    let before = fs::symlink_metadata(path)?;
    if !before.is_file()
        || before.nlink() != 1
        || before.len() < 4
        || before.len() > limit
        || before.mode() & 0o022 != 0
        || (before.uid() != unsafe { libc::geteuid() } && before.uid() != 0)
        || (!model && before.mode() & 0o111 == 0)
    {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "unsafe speech artifact",
        ));
    }
    let mut file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(path)?;
    let opened = file.metadata()?;
    if opened.dev() != before.dev() || opened.ino() != before.ino() {
        return Err(io::Error::other("speech artifact changed"));
    }
    let mut prefix = [0u8; 4];
    file.read_exact(&mut prefix)?;
    if model && prefix != 0x67676d6cu32.to_le_bytes() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "unsupported speech model header",
        ));
    }
    let mut digest = Sha256::new();
    digest.update(prefix);
    let mut buf = [0u8; 64 * 1024];
    let mut count = 4u64;
    loop {
        if stop.is_some_and(|(cancel, deadline)| {
            cancel.load(Ordering::SeqCst) || Instant::now() >= deadline
        }) {
            return Err(io::Error::new(
                io::ErrorKind::Interrupted,
                "speech inspection stopped",
            ));
        }
        let n = file.read(&mut buf)?;
        if n == 0 {
            break;
        }
        count = count
            .checked_add(n as u64)
            .ok_or_else(|| io::Error::other("artifact size"))?;
        if count > limit {
            return Err(io::Error::other("artifact exceeds limit"));
        }
        digest.update(&buf[..n]);
    }
    let after = file.metadata()?;
    let current = fs::symlink_metadata(path)?;
    for meta in [&after, &current] {
        if meta.dev() != before.dev()
            || meta.ino() != before.ino()
            || meta.len() != before.len()
            || meta.mtime() != before.mtime()
            || meta.mtime_nsec() != before.mtime_nsec()
            || meta.ctime() != before.ctime()
            || meta.ctime_nsec() != before.ctime_nsec()
            || meta.uid() != before.uid()
            || meta.mode() != before.mode()
            || meta.nlink() != 1
        {
            return Err(io::Error::other("speech artifact changed"));
        }
    }
    if count != before.len() || format!("{:x}", digest.finalize()) != expected {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "speech artifact digest mismatch",
        ));
    }
    Ok(count)
}

pub fn inspect_pair(
    binary: &Path,
    binary_hash: &str,
    model: &Path,
    model_hash: &str,
) -> io::Result<Inspection> {
    inspect(binary, binary_hash, MAX_BINARY, false, None)?;
    let model_bytes = inspect(model, model_hash, MAX_MODEL, true, None)?;
    Ok(Inspection {
        binary_sha256: binary_hash.to_owned(),
        model_sha256: model_hash.to_owned(),
        model_bytes,
        binary: binary.canonicalize()?,
        model: model.canonicalize()?,
    })
}

impl Inspection {
    pub fn revalidate(&self, cancel: &AtomicBool, deadline: Instant) -> io::Result<()> {
        inspect(
            &self.binary,
            &self.binary_sha256,
            MAX_BINARY,
            false,
            Some((cancel, deadline)),
        )?;
        inspect(
            &self.model,
            &self.model_sha256,
            MAX_MODEL,
            true,
            Some((cancel, deadline)),
        )?;
        Ok(())
    }
}
