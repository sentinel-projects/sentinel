//! Offline verification for development-evaluation stages. This checks byte integrity,
//! not publisher identity, provenance, signatures, or installability.
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeSet,
    fs::{self, File, OpenOptions},
    io::{self, Read},
    path::Path,
};

const MAX_MANIFEST_BYTES: u64 = 1024 * 1024;
const MAX_ARTIFACT_BYTES: u64 = 512 * 1024 * 1024;
const MAX_LOCKFILE_BYTES: u64 = 64 * 1024 * 1024;

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct StageManifest {
    schema_version: u64,
    app_version: String,
    target: String,
    stage: String,
    artifacts: Vec<Artifact>,
    lockfile_sha256: String,
    assets_integrity: String,
}

#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct Artifact {
    path: String,
    sha256: String,
}

#[derive(Debug, Serialize, PartialEq, Eq)]
pub struct StageVerification {
    pub app_version: String,
    pub target: String,
    pub artifacts_verified: Vec<String>,
    pub lockfile_checked: bool,
    pub assurance: &'static str,
}

pub fn verify_stage(
    directory: &Path,
    lockfile: Option<&Path>,
) -> Result<StageVerification, String> {
    let root_meta = fs::symlink_metadata(directory).map_err(io_message)?;
    if root_meta.file_type().is_symlink() || !root_meta.is_dir() {
        return Err("stage path must be a real directory, not a symlink".into());
    }
    let manifest_path = directory.join("manifest.json");
    let manifest_bytes = read_bounded_regular(&manifest_path, MAX_MANIFEST_BYTES)?;
    let manifest: StageManifest = serde_json::from_slice(&manifest_bytes)
        .map_err(|_| "manifest.json is invalid or has unsupported fields".to_owned())?;

    if manifest.schema_version != 1 {
        return Err("unsupported stage manifest schema version".into());
    }
    if manifest.stage != "development-evaluation" {
        return Err("stage is not marked development-evaluation".into());
    }
    if manifest.app_version != sentinel_ipc::APP_VERSION {
        return Err(format!(
            "stage version {} does not match this CLI version {}",
            manifest.app_version,
            sentinel_ipc::APP_VERSION
        ));
    }
    let target = env!("SENTINEL_CLI_TARGET");
    if manifest.target != target {
        return Err(format!(
            "stage target {} does not match this CLI target {target}",
            manifest.target
        ));
    }
    if manifest.assets_integrity != "not-verified" {
        return Err("stage asset integrity marker is unsupported".into());
    }
    validate_digest(&manifest.lockfile_sha256)?;

    let suffix = if target.split('-').any(|part| part == "windows") {
        ".exe"
    } else {
        ""
    };
    let expected = [
        format!("bin/sentinel{suffix}"),
        format!("bin/sentinel-daemon-dev{suffix}"),
    ];
    if manifest.artifacts.len() != expected.len() {
        return Err(
            "manifest must contain exactly the CLI and development daemon artifacts".into(),
        );
    }
    let mut seen = BTreeSet::new();
    for artifact in &manifest.artifacts {
        if !expected.iter().any(|path| path == &artifact.path) || !seen.insert(&artifact.path) {
            return Err(
                "manifest contains a missing, duplicate, or unexpected artifact path".into(),
            );
        }
        validate_digest(&artifact.sha256)?;
    }
    if expected.iter().any(|path| !seen.contains(path)) {
        return Err("manifest omits a required stage artifact".into());
    }

    let bin_dir = directory.join("bin");
    let bin_meta = fs::symlink_metadata(&bin_dir).map_err(io_message)?;
    if bin_meta.file_type().is_symlink() || !bin_meta.is_dir() {
        return Err("stage bin directory must be a real directory".into());
    }
    let mut verified = Vec::with_capacity(expected.len());
    for relative in expected {
        let artifact = manifest
            .artifacts
            .iter()
            .find(|artifact| artifact.path == relative)
            .expect("validated exact artifact set");
        let path = directory.join(&relative);
        let actual = sha256_regular_file(&path, MAX_ARTIFACT_BYTES)?;
        if actual != artifact.sha256 {
            return Err(format!(
                "SHA-256 mismatch for {}",
                Path::new(&relative).file_name().unwrap().to_string_lossy()
            ));
        }
        verified.push(
            Path::new(&relative)
                .file_name()
                .unwrap()
                .to_string_lossy()
                .into_owned(),
        );
    }

    let lockfile_checked = if let Some(lockfile) = lockfile {
        let actual = sha256_regular_file(lockfile, MAX_LOCKFILE_BYTES)?;
        if actual != manifest.lockfile_sha256 {
            return Err("provided lockfile does not match the manifest digest".into());
        }
        true
    } else {
        false
    };

    Ok(StageVerification {
        app_version: manifest.app_version,
        target: manifest.target,
        artifacts_verified: verified,
        lockfile_checked,
        assurance: "sha256-integrity-only; publisher authenticity and provenance are unverified",
    })
}

fn validate_digest(value: &str) -> Result<(), String> {
    if value.len() != 64
        || !value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
    {
        return Err("manifest contains an invalid lowercase SHA-256 digest".into());
    }
    Ok(())
}

fn read_bounded_regular(path: &Path, limit: u64) -> Result<Vec<u8>, String> {
    let metadata = fs::symlink_metadata(path).map_err(io_message)?;
    if metadata.file_type().is_symlink() || !metadata.is_file() {
        return Err(format!(
            "{} must be a regular non-symlink file",
            path.display()
        ));
    }
    if metadata.len() > limit {
        return Err(format!("{} exceeds the permitted size", path.display()));
    }
    let file = File::open(path).map_err(io_message)?;
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    file.take(limit + 1)
        .read_to_end(&mut bytes)
        .map_err(io_message)?;
    if bytes.len() as u64 > limit {
        return Err(format!("{} exceeds the permitted size", path.display()));
    }
    Ok(bytes)
}

fn sha256_regular_file(path: &Path, limit: u64) -> Result<String, String> {
    let metadata = fs::symlink_metadata(path).map_err(io_message)?;
    if metadata.file_type().is_symlink() || !metadata.is_file() {
        return Err(format!(
            "{} must be a regular non-symlink file",
            path.display()
        ));
    }
    if metadata.len() > limit {
        return Err(format!("{} exceeds the permitted size", path.display()));
    }
    let mut file = OpenOptions::new()
        .read(true)
        .open(path)
        .map_err(io_message)?;
    let mut hasher = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    let mut total = 0u64;
    loop {
        let count = file.read(&mut buffer).map_err(io_message)?;
        if count == 0 {
            break;
        }
        total = total.saturating_add(count as u64);
        if total > limit {
            return Err(format!("{} exceeds the permitted size", path.display()));
        }
        hasher.update(&buffer[..count]);
    }
    if total != metadata.len() {
        return Err(format!(
            "{} changed while it was being verified",
            path.display()
        ));
    }
    Ok(hex::encode(hasher.finalize()))
}

fn io_message(error: io::Error) -> String {
    format!("stage verification I/O failed: {error}")
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::{
        fs,
        path::PathBuf,
        sync::atomic::{AtomicU64, Ordering},
    };

    static NEXT: AtomicU64 = AtomicU64::new(0);

    struct Stage(PathBuf);
    impl Stage {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "sentinel-stage-verify-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir_all(path.join("bin")).unwrap();
            fs::write(path.join("bin/sentinel"), b"cli fixture").unwrap();
            fs::write(path.join("bin/sentinel-daemon-dev"), b"daemon fixture").unwrap();
            Self(path)
        }
        fn write_manifest(&self, value: serde_json::Value) {
            fs::write(self.0.join("manifest.json"), value.to_string()).unwrap();
        }
        fn manifest(&self) -> serde_json::Value {
            json!({
                "schema_version":1,
                "app_version":sentinel_ipc::APP_VERSION,
                "target":env!("SENTINEL_CLI_TARGET"),
                "stage":"development-evaluation",
                "artifacts":[
                    {"path":"bin/sentinel","sha256":hex::encode(Sha256::digest(b"cli fixture"))},
                    {"path":"bin/sentinel-daemon-dev","sha256":hex::encode(Sha256::digest(b"daemon fixture"))}
                ],
                "lockfile_sha256":hex::encode(Sha256::digest(b"lock fixture")),
                "assets_integrity":"not-verified"
            })
        }
    }
    impl Drop for Stage {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn verifies_exact_stage_manifest_and_artifact_bytes_without_installing() {
        let stage = Stage::new();
        stage.write_manifest(stage.manifest());
        let result = verify_stage(&stage.0, None).unwrap();
        assert_eq!(result.app_version, sentinel_ipc::APP_VERSION);
        assert_eq!(result.target, env!("SENTINEL_CLI_TARGET"));
        assert_eq!(result.artifacts_verified.len(), 2);
        assert!(!result.lockfile_checked);
        assert!(result.assurance.contains("authenticity"));
    }

    #[test]
    fn refuses_wrong_version_target_artifacts_and_digests() {
        let stage = Stage::new();
        let mut manifest = stage.manifest();
        manifest["app_version"] = json!("99.0.0");
        stage.write_manifest(manifest);
        assert!(
            verify_stage(&stage.0, None)
                .unwrap_err()
                .contains("version")
        );

        let mut manifest = stage.manifest();
        manifest["target"] = json!("other-target");
        stage.write_manifest(manifest);
        assert!(verify_stage(&stage.0, None).unwrap_err().contains("target"));

        let mut manifest = stage.manifest();
        manifest["artifacts"][0]["sha256"] = json!("0".repeat(64));
        stage.write_manifest(manifest);
        assert!(
            verify_stage(&stage.0, None)
                .unwrap_err()
                .contains("mismatch")
        );

        let mut manifest = stage.manifest();
        manifest["artifacts"][0]["path"] = json!("bin/../../outside");
        stage.write_manifest(manifest);
        assert!(
            verify_stage(&stage.0, None)
                .unwrap_err()
                .contains("artifact path")
        );
    }

    #[test]
    fn lockfile_is_compared_only_when_explicitly_supplied() {
        let stage = Stage::new();
        stage.write_manifest(stage.manifest());
        let lockfile = stage.0.join("Cargo.lock");
        fs::write(&lockfile, b"lock fixture").unwrap();
        assert!(
            verify_stage(&stage.0, Some(&lockfile))
                .unwrap()
                .lockfile_checked
        );
        fs::write(&lockfile, b"changed").unwrap();
        assert!(
            verify_stage(&stage.0, Some(&lockfile))
                .unwrap_err()
                .contains("lockfile")
        );
    }
}
