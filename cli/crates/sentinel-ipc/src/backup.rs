//! Bounded client-side export and atomic publication for development snapshots.
//! These snapshots are not the Qt portable backup format and are never restored here.
use crate::{Client, Error};
use base64::{Engine as _, engine::general_purpose::STANDARD};
use serde_json::{Value, json};
use sha2::{Digest, Sha256};
#[cfg(unix)]
use std::fs::File;
use std::{
    fs::{self, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
};

pub const MAX_SNAPSHOT_BYTES: usize = 16 * 1024 * 1024;
pub const MAX_TRANSFER_CHUNK: usize = 48 * 1024;
const WRITE_CHUNK: usize = 64 * 1024;
const SNAPSHOT_FORMAT: &str = "sentinel-dev-sqlite-v1";
const PROFILE_SNAPSHOT_FORMAT: &str = "sentinel-dev-sqlite-set-v1";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Snapshot {
    pub domain: String,
    pub format: String,
    pub sha256: String,
    pub bytes: Vec<u8>,
}

/// Export one dev-only domain, read all chunks over the same peer, verify the
/// daemon's declared size and SHA-256, and release the transfer before return.
pub fn export(client: &mut Client, domain: &str) -> Result<Snapshot, Error> {
    if !matches!(domain, "chat" | "settings" | "memory" | "agent") {
        return Err(Error::Usage(
            "Backup domain must be chat, settings, memory, or agent.".into(),
        ));
    }
    export_domains(client, &[domain], domain, SNAPSHOT_FORMAT)
}

/// Export the complete four-store development profile as one bounded archive.
pub fn export_profile(client: &mut Client) -> Result<Snapshot, Error> {
    export_domains(
        client,
        &["chat", "settings", "memory", "agent"],
        "profile",
        PROFILE_SNAPSHOT_FORMAT,
    )
}

fn export_domains(
    client: &mut Client,
    domains: &[&str],
    expected_domain: &str,
    expected_format: &str,
) -> Result<Snapshot, Error> {
    let mut transfer_id = None;
    let result = (|| {
        let response = client.request(
            "backup.transfer",
            json!({"action":"export","value":{"domains":domains}}),
        )?;
        let value = &response["result"];
        if value["succeeded"] != true {
            return Err(backup_remote_error(value));
        }
        if value["domain"].as_str() != Some(expected_domain)
            || value["format"].as_str() != Some(expected_format)
        {
            return Err(Error::Protocol(
                "backup metadata does not match the requested domain/format".into(),
            ));
        }
        let id = value["transferId"]
            .as_str()
            .filter(|id| !id.is_empty() && id.len() <= 128 && !id.contains('\0'))
            .ok_or_else(|| Error::Protocol("invalid backup transfer ID".into()))?
            .to_owned();
        transfer_id = Some(id.clone());
        let size = value["size"]
            .as_u64()
            .and_then(|size| usize::try_from(size).ok())
            .filter(|size| (1..=MAX_SNAPSHOT_BYTES).contains(size))
            .ok_or_else(|| Error::Protocol("invalid backup size".into()))?;
        let expected_sha = value["sha256"]
            .as_str()
            .filter(|sha| {
                sha.len() == 64
                    && sha
                        .bytes()
                        .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
            })
            .ok_or_else(|| Error::Protocol("invalid backup SHA-256".into()))?
            .to_owned();
        let mut bytes = Vec::with_capacity(size);
        let mut offset = 0usize;
        while offset < size {
            let response = client.request(
                "backup.transfer",
                json!({"action":"read","value":{"transferId":id,"offset":offset}}),
            )?;
            let value = &response["result"];
            if value["succeeded"] != true {
                return Err(backup_remote_error(value));
            }
            if value["offset"].as_u64() != Some(offset as u64) {
                return Err(Error::Protocol("backup chunk offset mismatch".into()));
            }
            let encoded = value["data"]
                .as_str()
                .filter(|data| data.len() <= 4 * MAX_TRANSFER_CHUNK / 3 + 8)
                .ok_or_else(|| Error::Protocol("invalid backup chunk".into()))?;
            let chunk = STANDARD.decode(encoded).map_err(|error| {
                Error::Protocol(format!("invalid backup chunk encoding: {error}"))
            })?;
            let expected_chunk = (size - offset).min(MAX_TRANSFER_CHUNK);
            if chunk.is_empty() || chunk.len() != expected_chunk {
                return Err(Error::Protocol("backup chunk length mismatch".into()));
            }
            let next = offset
                .checked_add(chunk.len())
                .ok_or_else(|| Error::Protocol("backup offset overflow".into()))?;
            if value["nextOffset"].as_u64() != Some(next as u64) || next > size {
                return Err(Error::Protocol("backup next offset mismatch".into()));
            }
            bytes.extend_from_slice(&chunk);
            offset = next;
        }
        let actual_sha = format!("{:x}", Sha256::digest(&bytes));
        if bytes.len() != size || actual_sha != expected_sha {
            return Err(Error::Protocol(
                "backup length or SHA-256 verification failed".into(),
            ));
        }
        Ok(Snapshot {
            domain: expected_domain.into(),
            format: expected_format.into(),
            sha256: actual_sha,
            bytes,
        })
    })();

    if let Some(id) = transfer_id {
        let released = client.request(
            "backup.transfer",
            json!({"action":"cancel","value":{"transferId":id}}),
        );
        match (result, released) {
            (Ok(snapshot), Ok(value)) if value["result"]["succeeded"] == true => Ok(snapshot),
            (Ok(_), Ok(_)) => Err(Error::Protocol(
                "backup transfer release was not confirmed".into(),
            )),
            (Ok(_), Err(error)) => Err(error),
            (Err(error), _) => Err(error),
        }
    } else {
        result
    }
}

/// Atomically publish verified bytes to a new path in the same directory.
/// Existing files (including symlinks) are never replaced. On Unix the staged
/// file is owner-readable/writable only; other platforms inherit directory ACLs.
pub fn write_new(destination: &Path, snapshot: &Snapshot) -> Result<PathBuf, Error> {
    validate_snapshot(snapshot)?;
    let parent = destination
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let parent = parent.canonicalize().map_err(|error| {
        Error::Usage(format!(
            "Backup destination directory is unavailable: {error}"
        ))
    })?;
    let name = destination
        .file_name()
        .filter(|name| *name != "." && *name != "..")
        .ok_or_else(|| Error::Usage("Choose a new backup file path, not a directory.".into()))?;
    let published = parent.join(name);
    match fs::symlink_metadata(&published) {
        Ok(_) => {
            return Err(Error::Usage(
                "Backup destination already exists; choose a new file path.".into(),
            ));
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(error) => {
            return Err(Error::Failed(format!(
                "Cannot inspect backup destination: {error}"
            )));
        }
    }

    static NEXT_TEMP: AtomicU64 = AtomicU64::new(0);
    let mut temp = None;
    for _ in 0..64 {
        let id = NEXT_TEMP.fetch_add(1, Ordering::Relaxed);
        let candidate = parent.join(format!(".sentinel-backup-{}-{id}.tmp", std::process::id()));
        let mut options = OpenOptions::new();
        options.write(true).create_new(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        match options.open(&candidate) {
            Ok(file) => {
                temp = Some((candidate, file));
                break;
            }
            Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(error) => {
                return Err(Error::Failed(format!(
                    "Cannot create private backup staging file: {error}"
                )));
            }
        }
    }
    let Some((staging_path, mut file)) = temp else {
        return Err(Error::Failed(
            "Cannot reserve a unique backup staging file.".into(),
        ));
    };
    let staged = (|| -> std::io::Result<()> {
        for chunk in snapshot.bytes.chunks(WRITE_CHUNK) {
            file.write_all(chunk)?;
        }
        file.sync_all()
    })();
    drop(file);
    if let Err(error) = staged {
        let _ = fs::remove_file(&staging_path);
        return Err(Error::Failed(format!(
            "Cannot write verified backup staging file: {error}"
        )));
    }
    if let Err(error) = fs::hard_link(&staging_path, &published) {
        let _ = fs::remove_file(&staging_path);
        return if error.kind() == std::io::ErrorKind::AlreadyExists {
            Err(Error::Usage(
                "Backup destination appeared during export; no file was overwritten.".into(),
            ))
        } else {
            Err(Error::Failed(format!(
                "Cannot atomically publish backup without overwriting: {error}"
            )))
        };
    }
    if let Err(error) = fs::remove_file(&staging_path) {
        return Err(Error::Failed(format!(
            "Backup was published at {} but its staging link could not be removed: {error}",
            published.display()
        )));
    }
    #[cfg(unix)]
    File::open(&parent)
        .and_then(|directory| directory.sync_all())
        .map_err(|error| Error::Failed(format!(
            "Backup was published at {} but directory durability could not be confirmed: {error}",
            published.display()
        )))?;
    Ok(published)
}

fn validate_snapshot(snapshot: &Snapshot) -> Result<(), Error> {
    let supported = if snapshot.domain == "profile" {
        snapshot.format == PROFILE_SNAPSHOT_FORMAT
    } else {
        matches!(
            snapshot.domain.as_str(),
            "chat" | "settings" | "memory" | "agent"
        ) && snapshot.format == SNAPSHOT_FORMAT
    };
    if !supported || snapshot.bytes.is_empty() || snapshot.bytes.len() > MAX_SNAPSHOT_BYTES {
        return Err(Error::Usage(
            "Unsupported or invalid development backup snapshot.".into(),
        ));
    }
    let digest = format!("{:x}", Sha256::digest(&snapshot.bytes));
    if digest != snapshot.sha256 {
        return Err(Error::Protocol(
            "backup snapshot changed before publication".into(),
        ));
    }
    Ok(())
}

fn backup_remote_error(value: &Value) -> Error {
    Error::Remote(
        value["code"]
            .as_str()
            .unwrap_or("backup-unavailable")
            .to_owned(),
    )
}
