//! Separate, isolated development provider-selection settings.
use crate::{
    provider::Config,
    store::{self, WriteFailure},
};
use rusqlite::{Connection, OptionalExtension, TransactionBehavior, params};
use std::{error::Error, io, path::Path, time::Duration};

pub const DATABASE: &str = "daemon-dev-settings.sqlite3";
pub const JOURNAL: &str = "daemon-dev-settings.sqlite3-journal";
pub(crate) const APP_ID: u32 = 0x53445354;
pub(crate) const VERSION: u32 = 4;
pub(crate) const MAX_PREFERENCES: usize = 2 + crate::provider_catalog::ENDPOINT_KEYS.len();

pub struct Settings {
    db: Connection,
}

impl Settings {
    pub fn development_snapshot(&self) -> rusqlite::Result<Vec<u8>> {
        crate::backup::snapshot(&self.db)
    }

    /// Restore only validated settings into an initialized empty store under the profile lock.
    pub fn restore_snapshot(profile: &Path, source: &Path) -> Result<(), Box<dyn Error>> {
        use std::{
            fs::OpenOptions,
            io::Read,
            os::unix::fs::{MetadataExt, OpenOptionsExt},
        };
        let (path, before) = crate::import::checked_source(source, profile)?;
        if before.len() == 0 || before.len() > crate::backup::MAX_BYTES as u64 {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "snapshot size").into());
        }
        let mut file = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(&path)?;
        let mut bytes = Vec::with_capacity(before.len() as usize);
        file.by_ref()
            .take(crate::backup::MAX_BYTES as u64 + 1)
            .read_to_end(&mut bytes)?;
        let after = file.metadata()?;
        let current = std::fs::symlink_metadata(&path)?;
        if bytes.len() != before.len() as usize
            || before.dev() != after.dev()
            || before.ino() != after.ino()
            || before.dev() != current.dev()
            || before.ino() != current.ino()
            || before.len() != after.len()
            || before.mtime() != after.mtime()
            || before.mtime_nsec() != after.mtime_nsec()
        {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "snapshot changed").into());
        }
        crate::import::no_sidecars(&path)?;
        let mut src = Connection::open_in_memory()?;
        src.deserialize_read_exact("main", &mut bytes.as_slice(), bytes.len(), true)?;
        src.execute_batch("PRAGMA trusted_schema=OFF; PRAGMA query_only=ON;")?;
        let app: u32 = src.query_row("PRAGMA application_id", [], |r| r.get(0))?;
        let version: u32 = src.query_row("PRAGMA user_version", [], |r| r.get(0))?;
        let check: String = src.query_row("PRAGMA quick_check", [], |r| r.get(0))?;
        if (app, version) != (APP_ID, VERSION) || check != "ok" {
            return Err(rusqlite::Error::InvalidQuery.into());
        }
        let (mut dst, _, _) = store::open_database(profile, DATABASE, APP_ID, VERSION, true)?;
        dst.busy_timeout(Duration::from_millis(250))?;
        dst.execute_batch("PRAGMA trusted_schema=OFF; PRAGMA synchronous=FULL;")?;
        let dest_check: String = dst.query_row("PRAGMA quick_check", [], |r| r.get(0))?;
        let dest_objects: i64 = dst.query_row("SELECT count(*) FROM sqlite_master WHERE type IN ('view','trigger') OR (type='table' AND name NOT IN ('selection','preferences','workspace_selection','workspace_revocations'))", [], |r| r.get(0))?;
        if dest_check != "ok" || dest_objects != 0 {
            return Err(rusqlite::Error::InvalidQuery.into());
        }
        for table in [
            "selection",
            "preferences",
            "workspace_selection",
            "workspace_revocations",
        ] {
            let query = format!("PRAGMA table_info({table})");
            let schema = |db: &Connection| -> rusqlite::Result<Vec<(String, String, i64, i64)>> {
                let mut stmt = db.prepare(&query)?;
                let rows =
                    stmt.query_map([], |r| Ok((r.get(1)?, r.get(2)?, r.get(3)?, r.get(5)?)))?;
                rows.collect()
            };
            if schema(&src)? != schema(&dst)? {
                return Err(rusqlite::Error::InvalidQuery.into());
            }
        }
        let objects: i64 = src.query_row("SELECT count(*) FROM sqlite_master WHERE type IN ('view','trigger') OR (type='table' AND name NOT IN ('selection','preferences','workspace_selection','workspace_revocations'))", [], |r| r.get(0))?;
        if objects != 0 {
            return Err(rusqlite::Error::InvalidQuery.into());
        }
        let selection: Option<(String, String, String)> = src
            .query_row(
                "SELECT endpoint,provider_id,model FROM selection WHERE id=1",
                [],
                |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
            )
            .optional()?;
        if let Some((endpoint, kind, model)) = &selection {
            let c = Config::parse(endpoint, model)?.with_provider_kind(kind)?;
            if c.endpoint != *endpoint {
                return Err(rusqlite::Error::InvalidQuery.into());
            }
        }
        let workspace: Option<(String, String)> = src
            .query_row(
                "SELECT workspace_id,root FROM workspace_selection WHERE id=1",
                [],
                |r| Ok((r.get(0)?, r.get(1)?)),
            )
            .optional()?;
        if workspace
            .as_ref()
            .is_some_and(|(id, root)| !valid_workspace_binding(id, root))
        {
            return Err(rusqlite::Error::InvalidQuery.into());
        }
        let mut revocation_query =
            src.prepare("SELECT workspace_id,root FROM workspace_revocations LIMIT 65")?;
        let revocations = revocation_query
            .query_map([], |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?)))?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        if revocations.len() > 64
            || revocations
                .iter()
                .any(|(id, root)| !valid_workspace_binding(id, root))
        {
            return Err(rusqlite::Error::InvalidQuery.into());
        }
        drop(revocation_query);
        let mut stmt = src.prepare("SELECT key,value FROM preferences LIMIT ?1")?;
        let preferences = stmt
            .query_map([(MAX_PREFERENCES + 1) as i64], |r| {
                Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?))
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        if preferences.len() > MAX_PREFERENCES
            || preferences.iter().any(|(k, v)| !valid_preference(k, v))
        {
            return Err(rusqlite::Error::InvalidQuery.into());
        }
        for table in ["selection", "workspace_selection"] {
            let n: i64 =
                src.query_row(&format!("SELECT count(*) FROM {table}"), [], |r| r.get(0))?;
            let expected = if table == "selection" {
                selection.is_some()
            } else {
                workspace.is_some()
            };
            if n != i64::from(expected) {
                return Err(rusqlite::Error::InvalidQuery.into());
            }
        }
        let tx = dst.transaction_with_behavior(TransactionBehavior::Immediate)?;
        for table in [
            "selection",
            "preferences",
            "workspace_selection",
            "workspace_revocations",
        ] {
            let n: i64 =
                tx.query_row(&format!("SELECT count(*) FROM {table}"), [], |r| r.get(0))?;
            if n != 0 {
                return Err(rusqlite::Error::InvalidQuery.into());
            }
        }
        if let Some((endpoint, kind, model)) = selection {
            tx.execute(
                "INSERT INTO selection VALUES(1,?1,?2,?3)",
                params![endpoint, kind, model],
            )?;
        }
        if let Some((id, root)) = workspace {
            tx.execute(
                "INSERT INTO workspace_selection VALUES(1,?1,?2)",
                params![id, root],
            )?;
        }
        for (id, root) in revocations {
            tx.execute(
                "INSERT INTO workspace_revocations VALUES(?1,?2)",
                params![id, root],
            )?;
        }
        for (key, value) in preferences {
            tx.execute("INSERT INTO preferences VALUES(?1,?2)", params![key, value])?;
        }
        tx.commit()?;
        Ok(())
    }

    pub fn open(profile: &Path) -> Result<Self, Box<dyn Error>> {
        let (mut db, new, old_version) =
            store::open_database(profile, DATABASE, APP_ID, VERSION, false)?;
        db.busy_timeout(Duration::from_millis(250))?;
        db.execute_batch("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;")?;
        if new {
            let tx = db.transaction()?;
            tx.execute_batch(&format!("PRAGMA application_id={APP_ID}; PRAGMA user_version={VERSION}; CREATE TABLE selection (id INTEGER PRIMARY KEY CHECK(id=1), endpoint TEXT NOT NULL, provider_id TEXT NOT NULL, model TEXT NOT NULL); CREATE TABLE preferences(key TEXT PRIMARY KEY,value TEXT NOT NULL); CREATE TABLE workspace_selection(id INTEGER PRIMARY KEY CHECK(id=1), workspace_id TEXT NOT NULL, root TEXT NOT NULL); CREATE TABLE workspace_revocations(workspace_id TEXT PRIMARY KEY, root TEXT NOT NULL);"))?;
            tx.commit()?;
        } else {
            let check: String = db.query_row("PRAGMA quick_check", [], |row| row.get(0))?;
            if check != "ok" {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "invalid development settings",
                )
                .into());
            }
        }
        if !new && old_version == 1 {
            let tx = db.transaction()?;
            tx.execute_batch("CREATE TABLE preferences(key TEXT PRIMARY KEY, value TEXT NOT NULL); PRAGMA user_version=2;")?;
            tx.commit()?;
        }
        if !new && old_version <= 2 {
            let tx = db.transaction()?;
            tx.execute_batch("CREATE TABLE workspace_selection(id INTEGER PRIMARY KEY CHECK(id=1), workspace_id TEXT NOT NULL, root TEXT NOT NULL); PRAGMA user_version=3;")?;
            tx.commit()?;
        }
        if !new && old_version <= 3 {
            let tx = db.transaction()?;
            tx.execute_batch("CREATE TABLE workspace_revocations(workspace_id TEXT PRIMARY KEY, root TEXT NOT NULL); PRAGMA user_version=4;")?;
            tx.commit()?;
        }
        Ok(Self { db })
    }

    /// The caller must match both values against the independently configured catalog.
    pub fn workspace_selection(&self) -> rusqlite::Result<Option<(String, String)>> {
        let value: Option<(String, String)> = self
            .db
            .query_row(
                "SELECT workspace_id,root FROM workspace_selection WHERE id=1",
                [],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()?;
        if value
            .as_ref()
            .is_some_and(|(id, root)| !valid_workspace_binding(id, root))
        {
            return Err(rusqlite::Error::InvalidQuery);
        }
        Ok(value)
    }

    /// Call only after catalog membership and root admission have been verified.
    pub fn select_workspace(&mut self, id: &str, root: &str) -> Result<(), WriteFailure> {
        if !valid_workspace_binding(id, root) {
            return Err(WriteFailure {
                safe_conflict: true,
            });
        }
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(store::acquisition_failure)?;
        tx.execute("INSERT INTO workspace_selection(id,workspace_id,root) VALUES(1,?1,?2) ON CONFLICT(id) DO UPDATE SET workspace_id=excluded.workspace_id,root=excluded.root", params![id,root]).map_err(store::mutation_failure)?;
        tx.commit().map_err(store::mutation_failure)
    }

    /// A revoked identifier remains denied across root changes until explicitly re-enabled.
    pub fn workspace_access(&self, id: &str, root: &str) -> rusqlite::Result<bool> {
        if !valid_workspace_binding(id, root) {
            return Err(rusqlite::Error::InvalidQuery);
        }
        let stored: Option<String> = self
            .db
            .query_row(
                "SELECT root FROM workspace_revocations WHERE workspace_id=?1",
                [id],
                |r| r.get(0),
            )
            .optional()?;
        if stored
            .as_ref()
            .is_some_and(|value| !valid_workspace_binding(id, value))
        {
            return Err(rusqlite::Error::InvalidQuery);
        }
        Ok(stored.is_none())
    }

    /// Only call for an independently configured and validated workspace binding.
    pub fn set_workspace_access(
        &mut self,
        id: &str,
        root: &str,
        enabled: bool,
    ) -> Result<(), WriteFailure> {
        if !valid_workspace_binding(id, root) {
            return Err(WriteFailure {
                safe_conflict: true,
            });
        }
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(store::acquisition_failure)?;
        if enabled {
            tx.execute(
                "DELETE FROM workspace_revocations WHERE workspace_id=?1",
                [id],
            )
            .map_err(store::mutation_failure)?;
        } else {
            let count: i64 = tx
                .query_row("SELECT count(*) FROM workspace_revocations", [], |r| {
                    r.get(0)
                })
                .map_err(store::mutation_failure)?;
            let exists: bool = tx
                .query_row(
                    "SELECT EXISTS(SELECT 1 FROM workspace_revocations WHERE workspace_id=?1)",
                    [id],
                    |r| r.get(0),
                )
                .map_err(store::mutation_failure)?;
            if count >= 64 && !exists {
                return Err(WriteFailure {
                    safe_conflict: true,
                });
            }
            tx.execute("INSERT INTO workspace_revocations(workspace_id,root) VALUES(?1,?2) ON CONFLICT(workspace_id) DO UPDATE SET root=excluded.root", params![id,root]).map_err(store::mutation_failure)?;
        }
        tx.commit().map_err(store::mutation_failure)
    }

    pub fn desktop_values(&self) -> rusqlite::Result<serde_json::Value> {
        let mut values =
            serde_json::json!({"themeName":"system", "responseProfileInstructions":""});
        let mut statement = self.db.prepare("SELECT key,value FROM preferences")?;
        let rows = statement.query_map([], |row| {
            Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))
        })?;
        for row in rows {
            let (key, value) = row?;
            if !valid_preference(&key, &value) {
                return Err(rusqlite::Error::InvalidQuery);
            }
            values[key] = serde_json::Value::String(value);
        }
        Ok(values)
    }

    pub fn set_desktop_value(&mut self, key: &str, value: &str) -> Result<(), WriteFailure> {
        if !valid_preference(key, value) {
            return Err(WriteFailure {
                safe_conflict: true,
            });
        }
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(store::acquisition_failure)?;
        tx.execute("INSERT INTO preferences(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value", params![key,value])
            .map_err(store::mutation_failure)?;
        tx.commit().map_err(store::mutation_failure)
    }

    pub fn set_provider_endpoint(
        &mut self,
        key: &str,
        startup: &Config,
        binding: &Config,
        active: bool,
    ) -> Result<(), WriteFailure> {
        let value = serde_json::json!([startup.endpoint, binding.endpoint]).to_string();
        if crate::provider_catalog::provider_for_key(key) != Some(binding.provider_id.as_str())
            || startup.provider_id != binding.provider_id
            || !valid_preference(key, &value)
        {
            return Err(WriteFailure {
                safe_conflict: true,
            });
        }
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(store::acquisition_failure)?;
        tx.execute(
            "INSERT INTO preferences(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            params![key,value],
        ).map_err(store::mutation_failure)?;
        if active {
            tx.execute(
                "INSERT INTO selection(id,endpoint,provider_id,model) VALUES(1,?1,?2,?3) ON CONFLICT(id) DO UPDATE SET endpoint=excluded.endpoint,provider_id=excluded.provider_id,model=excluded.model",
                params![binding.endpoint,binding.provider_id,binding.model],
            ).map_err(store::mutation_failure)?;
        }
        tx.commit().map_err(store::mutation_failure)
    }

    pub fn restore(&self, config: &Config) -> Result<Option<String>, Box<dyn Error>> {
        let selection: Option<(String, String, String)> = self
            .db
            .query_row(
                "SELECT endpoint,provider_id,model FROM selection WHERE id=1",
                [],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            )
            .optional()?;
        let Some((endpoint, provider_id, model)) = selection else {
            return Ok(None);
        };
        if !Config::valid_persisted_selection(&endpoint, &provider_id, &model) {
            return Err(
                io::Error::new(io::ErrorKind::InvalidData, "invalid settings endpoint").into(),
            );
        }
        Ok((endpoint == config.endpoint && provider_id == config.provider_id).then_some(model))
    }

    pub fn select(&mut self, config: &Config, model: &str) -> Result<(), WriteFailure> {
        if !Config::valid_persisted_selection(&config.endpoint, &config.provider_id, model) {
            return Err(WriteFailure {
                safe_conflict: false,
            });
        }
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(store::acquisition_failure)?;
        tx.execute("INSERT INTO selection(id,endpoint,provider_id,model) VALUES(1,?1,?2,?3) ON CONFLICT(id) DO UPDATE SET endpoint=excluded.endpoint, provider_id=excluded.provider_id, model=excluded.model", params![config.endpoint,config.provider_id,model]).map_err(store::mutation_failure)?;
        tx.commit().map_err(store::mutation_failure)
    }
}

pub fn valid_workspace_binding(id: &str, root: &str) -> bool {
    !id.is_empty()
        && id.len() <= 128
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.'))
        && root.len() <= 4096
        && !root.chars().any(char::is_control)
        && Path::new(root).is_absolute()
}

pub fn valid_preference(key: &str, value: &str) -> bool {
    match key {
        "themeName" => matches!(value, "system" | "light" | "dark"),
        "responseProfileInstructions" => {
            value.encode_utf16().count() <= 2000 && !value.contains('\0')
        }
        _ if crate::provider_catalog::provider_for_key(key).is_some() => {
            crate::provider_catalog::endpoint_override(value).is_some()
        }
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs, os::unix::fs::DirBuilderExt};

    #[test]
    fn version_one_upgrade_preserves_selection_and_adds_theme() {
        use std::os::unix::fs::PermissionsExt;
        let dir =
            std::env::temp_dir().join(format!("sentinel-settings-upgrade-{}", std::process::id()));
        fs::DirBuilder::new().mode(0o700).create(&dir).unwrap();
        let dir = dir.canonicalize().unwrap();
        let path = dir.join(DATABASE);
        let db = Connection::open(&path).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        db.execute_batch(&format!("PRAGMA application_id={APP_ID}; PRAGMA user_version=1; CREATE TABLE selection(id INTEGER PRIMARY KEY CHECK(id=1), endpoint TEXT NOT NULL, provider_id TEXT NOT NULL, model TEXT NOT NULL); INSERT INTO selection VALUES(1,'http://127.0.0.1:1234','ollama','saved');")).unwrap();
        drop(db);
        let settings = Settings::open(&dir).unwrap();
        let config = Config::parse("http://127.0.0.1:1234", "default")
            .unwrap()
            .with_provider_kind("ollama")
            .unwrap();
        assert_eq!(settings.restore(&config).unwrap().as_deref(), Some("saved"));
        assert_eq!(settings.desktop_values().unwrap()["themeName"], "system");
        assert_eq!(
            settings
                .db
                .query_row("PRAGMA user_version", [], |r| r.get::<_, u32>(0))
                .unwrap(),
            4
        );
        drop(settings);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn workspace_binding_persists_and_busy_write_does_not_change_it() {
        let dir = std::env::temp_dir().join(format!(
            "sentinel-workspace-settings-{}",
            std::process::id()
        ));
        fs::DirBuilder::new().mode(0o700).create(&dir).unwrap();
        let dir = dir.canonicalize().unwrap();
        let mut settings = Settings::open(&dir).unwrap();
        assert!(settings.workspace_selection().unwrap().is_none());
        settings.select_workspace("one", "/tmp/one").unwrap();
        assert!(settings.select_workspace("invalid/id", "/tmp/two").is_err());
        assert!(settings.select_workspace("two", "relative").is_err());
        let lock = Connection::open(dir.join(DATABASE)).unwrap();
        lock.execute_batch("BEGIN IMMEDIATE").unwrap();
        assert!(
            settings
                .select_workspace("two", "/tmp/two")
                .unwrap_err()
                .safe_conflict
        );
        lock.execute_batch("ROLLBACK").unwrap();
        drop(lock);
        drop(settings);
        let settings = Settings::open(&dir).unwrap();
        assert_eq!(
            settings.workspace_selection().unwrap(),
            Some(("one".to_owned(), "/tmp/one".to_owned()))
        );
        drop(settings);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn version_two_upgrade_preserves_preferences() {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join(format!("sentinel-settings-v2-{}", std::process::id()));
        fs::DirBuilder::new().mode(0o700).create(&dir).unwrap();
        let dir = dir.canonicalize().unwrap();
        let path = dir.join(DATABASE);
        let db = Connection::open(&path).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        db.execute_batch(&format!("PRAGMA application_id={APP_ID}; PRAGMA user_version=2; CREATE TABLE selection(id INTEGER PRIMARY KEY CHECK(id=1), endpoint TEXT NOT NULL, provider_id TEXT NOT NULL, model TEXT NOT NULL); CREATE TABLE preferences(key TEXT PRIMARY KEY,value TEXT NOT NULL); INSERT INTO preferences VALUES('themeName','dark');")).unwrap();
        drop(db);
        let settings = Settings::open(&dir).unwrap();
        assert_eq!(settings.desktop_values().unwrap()["themeName"], "dark");
        assert!(settings.workspace_selection().unwrap().is_none());
        drop(settings);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn binding_survives_restart_and_busy_write_preserves_selection() {
        let dir =
            std::env::temp_dir().join(format!("sentinel-settings-test-{}", std::process::id()));
        fs::DirBuilder::new().mode(0o700).create(&dir).unwrap();
        let dir = dir.canonicalize().unwrap();
        let config = Config::parse("http://127.0.0.1:1234", "default").unwrap();
        let mut settings = Settings::open(&dir).unwrap();
        settings.select(&config, "chosen").unwrap();
        settings.set_desktop_value("themeName", "dark").unwrap();
        drop(settings);
        let mut settings = Settings::open(&dir).unwrap();
        assert_eq!(settings.desktop_values().unwrap()["themeName"], "dark");
        assert!(!valid_preference("unsupported", "dark"));
        assert!(!valid_preference("themeName", "unknown"));
        assert_eq!(
            settings.restore(&config).unwrap().as_deref(),
            Some("chosen")
        );
        let other = Config::parse("http://127.0.0.1:1235", "default").unwrap();
        assert!(settings.restore(&other).unwrap().is_none());
        let lock = Connection::open(dir.join(DATABASE)).unwrap();
        lock.execute_batch("BEGIN IMMEDIATE").unwrap();
        assert!(
            settings
                .select(&config, "not-committed")
                .unwrap_err()
                .safe_conflict
        );
        lock.execute_batch("ROLLBACK").unwrap();
        assert_eq!(
            settings.restore(&config).unwrap().as_deref(),
            Some("chosen")
        );
        drop(settings);
        drop(lock);
        fs::remove_dir_all(dir).unwrap();
    }
}
