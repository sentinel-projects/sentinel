//! Offline import of a finalized, disposable Qt v7 session-memory copy.
use crate::import;
use rusqlite::{Connection, OpenFlags};
use std::{
    collections::{HashMap, HashSet},
    error::Error,
    fs, io,
    os::unix::fs::MetadataExt,
    path::{Path, PathBuf},
    time::Duration,
};

pub struct Row {
    pub id: String,
    pub session: String,
    pub key: String,
    pub value: String,
    pub provenance: i64,
    pub source: String,
    pub created: String,
    pub updated: String,
    pub revision: String,
    pub corrections: String,
}
#[derive(serde::Serialize)]
pub struct ExcludedMemory {
    pub legacy: u64,
    pub project: u64,
    pub personal: u64,
    pub workspace_bound_sessions: u64,
    pub legacy_index_present: bool,
    pub scoped_index_present: bool,
}

pub struct Snapshot {
    pub rows: Vec<Row>,
    pub excluded: ExcludedMemory,
    db: Connection,
    source: PathBuf,
    identity: fs::Metadata,
}
impl Snapshot {
    pub fn validate_unchanged(&self) -> io::Result<()> {
        import::no_sidecars(&self.source)?;
        let now = fs::symlink_metadata(&self.source)?;
        if now.dev() != self.identity.dev()
            || now.ino() != self.identity.ino()
            || now.len() != self.identity.len()
            || now.mtime() != self.identity.mtime()
            || now.mtime_nsec() != self.identity.mtime_nsec()
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "source changed during import",
            ));
        }
        let _ = &self.db;
        Ok(())
    }
}
fn invalid(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}
pub fn read(
    path: &Path,
    destination: &Path,
    known_sessions: &[String],
) -> Result<Snapshot, Box<dyn Error>> {
    read_preview(path, destination, known_sessions, false)
}

pub fn read_preview(
    path: &Path,
    destination: &Path,
    known_sessions: &[String],
    allow_exclusions: bool,
) -> Result<Snapshot, Box<dyn Error>> {
    let (source, identity) = import::checked_source(path, destination)?;
    let db = Connection::open_with_flags(
        &source,
        OpenFlags::SQLITE_OPEN_READ_ONLY
            | OpenFlags::SQLITE_OPEN_NO_MUTEX
            | OpenFlags::SQLITE_OPEN_NOFOLLOW,
    )?;
    db.busy_timeout(Duration::from_millis(250))?;
    db.execute_batch("PRAGMA query_only=ON; PRAGMA trusted_schema=OFF; BEGIN;")?;
    let check: String = db.query_row("PRAGMA quick_check", [], |r| r.get(0))?;
    if check != "ok" {
        return Err(invalid("invalid source database").into());
    }
    let mut schema =
        db.prepare("SELECT name,type FROM sqlite_master WHERE type IN ('table','view','trigger')")?;
    let objects = schema
        .query_map([], |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?)))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    let allowed_tables = [
        "memory_entries",
        "memory_schema_metadata",
        "scoped_memory_entries",
        "memory_entries_fts",
        "memory_entries_fts_data",
        "memory_entries_fts_idx",
        "memory_entries_fts_content",
        "memory_entries_fts_docsize",
        "memory_entries_fts_config",
        "scoped_memory_search",
        "scoped_memory_search_data",
        "scoped_memory_search_idx",
        "scoped_memory_search_content",
        "scoped_memory_search_docsize",
        "scoped_memory_search_config",
        "sqlite_sequence",
    ];
    let allowed_triggers = [
        "memory_fts_insert",
        "memory_fts_delete",
        "memory_fts_update",
        "scoped_search_insert",
        "scoped_search_delete",
        "scoped_search_update",
    ];
    if objects.iter().any(|(name, kind)| match kind.as_str() {
        "table" => !allowed_tables.contains(&name.as_str()),
        "trigger" => !allowed_triggers.contains(&name.as_str()),
        _ => true,
    }) {
        return Err(invalid("unsupported memory source objects").into());
    }
    drop(schema);
    let version: i64 = db.query_row(
        "SELECT value FROM memory_schema_metadata WHERE key='schema_version'",
        [],
        |r| r.get(0),
    )?;
    if version != 7 {
        return Err(invalid("unsupported Qt memory schema").into());
    }
    let extra: i64 = db.query_row(
        "SELECT count(*) FROM memory_schema_metadata WHERE key!='schema_version'",
        [],
        |r| r.get(0),
    )?;
    if extra != 0 {
        return Err(invalid("unsupported memory metadata").into());
    }
    let mut columns = db.prepare("PRAGMA table_info(scoped_memory_entries)")?;
    let mut actual = columns
        .query_map([], |r| r.get::<_, String>(1))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    actual.sort();
    let mut expected = vec![
        "id",
        "scope",
        "owner_id",
        "workspace_id",
        "key",
        "value",
        "provenance",
        "source_ref",
        "created_at",
        "updated_at",
        "revision",
        "corrections",
    ];
    expected.sort();
    if actual != expected {
        return Err(invalid("unsupported Qt memory columns").into());
    }
    drop(columns);
    let legacy: u64 = db.query_row("SELECT count(*) FROM memory_entries", [], |r| r.get(0))?;
    if legacy != 0 && !allow_exclusions {
        return Err(invalid("legacy memory unsupported").into());
    }
    let mut excluded = ExcludedMemory {
        legacy,
        project: 0,
        personal: 0,
        workspace_bound_sessions: 0,
        legacy_index_present: objects.iter().any(|(name, _)| name == "memory_entries_fts"),
        scoped_index_present: objects
            .iter()
            .any(|(name, _)| name == "scoped_memory_search"),
    };
    if allow_exclusions {
        let (project, personal, scoped_sessions, invalid_scopes): (u64, u64, u64, u64) = db.query_row(
            "SELECT
                COUNT(CASE WHEN scope=2 THEN 1 END),
                COUNT(CASE WHEN scope=3 THEN 1 END),
                COUNT(CASE WHEN scope=1 AND workspace_id!='' THEN 1 END),
                COUNT(CASE WHEN scope IS NULL OR scope NOT IN (1,2,3) OR workspace_id IS NULL THEN 1 END)
             FROM scoped_memory_entries",
            [], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
        )?;
        if invalid_scopes != 0 {
            return Err(invalid("unsupported memory scope").into());
        }
        excluded.project = project;
        excluded.personal = personal;
        excluded.workspace_bound_sessions = scoped_sessions;
    }
    let known: HashSet<_> = known_sessions.iter().map(String::as_str).collect();
    let mut statement=db.prepare("SELECT id,scope,owner_id,workspace_id,key,value,provenance,source_ref,created_at,updated_at,revision,corrections FROM scoped_memory_entries WHERE NOT ?1 OR (scope=1 AND workspace_id='') ORDER BY id LIMIT 10001")?;
    let mut query = statement.query([allow_exclusions])?;
    let mut rows = Vec::new();
    let mut per_session: HashMap<String, usize> = HashMap::new();
    let mut keys = HashSet::new();
    let mut bytes = 0usize;
    while let Some(r) = query.next()? {
        if rows.len() >= 10000 {
            return Err(invalid("source memory row limit").into());
        }
        let row = Row {
            id: r.get(0)?,
            session: r.get(2)?,
            key: r.get(4)?,
            value: r.get(5)?,
            provenance: r.get(6)?,
            source: r.get(7)?,
            created: r.get(8)?,
            updated: r.get(9)?,
            revision: r.get(10)?,
            corrections: r.get(11)?,
        };
        let scope: i64 = r.get(1)?;
        let workspace: String = r.get(3)?;
        if scope != 1 || !workspace.is_empty() || !known.contains(row.session.as_str()) {
            return Err(invalid("unsupported memory scope or session").into());
        }
        if !import::uuid(&row.id)
            || row.id.bytes().any(|b| b.is_ascii_uppercase())
            || row.id == "00000000-0000-0000-0000-000000000000"
            || row.key.trim().is_empty()
            || row.value.trim().is_empty()
            || row.key.encode_utf16().count() > 256
            || row.value.encode_utf16().count() > 65536
            || row.value.len() > 65536
            || !(0..=6).contains(&row.provenance)
            || row.source.encode_utf16().count() > 512
            || !import::timestamp(&row.created)
            || !import::timestamp(&row.updated)
            || row.updated < row.created
            || row.revision.is_empty()
            || row.revision.len() > 256
            || row.corrections.len() > 65536
        {
            return Err(invalid("unsupported memory record").into());
        }
        if !keys.insert((row.session.clone(), row.key.clone())) {
            return Err(invalid("duplicate session memory key").into());
        }
        let corrections: serde_json::Value = serde_json::from_str(&row.corrections)?;
        if corrections.as_array().is_none_or(|a| {
            a.len() > 256
                || a.iter().any(|v| {
                    let Some(o) = v.as_object() else {
                        return true;
                    };
                    let actor = o.get("actor").and_then(|v| v.as_str()).unwrap_or("");
                    let action = o.get("action").and_then(|v| v.as_str()).unwrap_or("");
                    let digest = o
                        .get("previousDigest")
                        .and_then(|v| v.as_str())
                        .unwrap_or("");
                    o.len() != 4
                        || !o
                            .get("at")
                            .and_then(|v| v.as_str())
                            .is_some_and(import::timestamp)
                        || actor.is_empty()
                        || actor.encode_utf16().count() > 256
                        || !["correct", "move", "reclassify", "import-replace", "import"]
                            .contains(&action)
                        || digest.len() != 64
                        || !digest
                            .bytes()
                            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
                })
        }) {
            return Err(invalid("unsupported correction receipts").into());
        }
        bytes = bytes
            .checked_add(row.key.len() + row.value.len() + row.source.len() + row.corrections.len())
            .ok_or_else(|| invalid("source too large"))?;
        if bytes > 16 * 1024 * 1024 {
            return Err(invalid("source memory byte limit").into());
        }
        let count = per_session.entry(row.session.clone()).or_default();
        *count += 1;
        if *count > 1000 {
            return Err(invalid("source session memory limit").into());
        }
        rows.push(row);
    }
    drop(query);
    drop(statement);
    let snapshot = Snapshot {
        rows,
        excluded,
        db,
        source,
        identity,
    };
    snapshot.validate_unchanged()?;
    Ok(snapshot)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::memory::MemoryStore;
    use serde_json::json;
    use std::os::unix::fs::{DirBuilderExt, PermissionsExt};
    #[test]
    fn imports_exact_session_metadata_and_rejects_unsupported_without_writes() {
        let nonce = std::process::id();
        let source_dir = Path::new("/tmp").join(format!("sentinel-memory-source-{nonce}"));
        let target_dir = Path::new("/tmp").join(format!("sentinel-memory-target-{nonce}"));
        fs::DirBuilder::new()
            .mode(0o700)
            .create(&source_dir)
            .unwrap();
        fs::DirBuilder::new()
            .mode(0o700)
            .create(&target_dir)
            .unwrap();
        let source = source_dir.join("memory.sqlite3");
        let db = Connection::open(&source).unwrap();
        fs::set_permissions(&source, fs::Permissions::from_mode(0o600)).unwrap();
        db.execute_batch("CREATE TABLE memory_schema_metadata(key TEXT PRIMARY KEY,value INTEGER NOT NULL); INSERT INTO memory_schema_metadata VALUES('schema_version',7); CREATE TABLE memory_entries(id INTEGER PRIMARY KEY,key TEXT,value TEXT); CREATE TABLE scoped_memory_entries(id TEXT PRIMARY KEY,scope INTEGER,owner_id TEXT,workspace_id TEXT,key TEXT,value TEXT,provenance INTEGER,source_ref TEXT,created_at TEXT,updated_at TEXT,revision TEXT,corrections TEXT);").unwrap();
        let id = "8fb66a15-7cb8-4779-892e-d46d7ff026ec";
        let session = "04f12802-5869-4ce8-b3e9-71704cd08e08";
        let receipts=json!([{"at":"2026-01-01T12:00:00.000Z","actor":"reviewer","action":"correct","previousDigest":"a".repeat(64)}]).to_string();
        db.execute("INSERT INTO scoped_memory_entries VALUES(?1,1,?2,'','key','value',2,'source','2026-01-01T12:00:00.000Z','2026-01-01T12:00:01.000Z','qt-revision',?3)",rusqlite::params![id,session,receipts]).unwrap();
        drop(db);
        let source_before = fs::read(&source).unwrap();
        let target = target_dir.canonicalize().unwrap();
        drop(MemoryStore::open(&target).unwrap());
        let snapshot = read(&source, &target, &[session.to_owned()]).unwrap();
        let mut store = MemoryStore::open_for_import(&target).unwrap();
        store.import_snapshot(&snapshot).unwrap();
        let result = store
            .manage(session, &json!({"operation":"read","scope":1,"id":id}))
            .unwrap();
        let record = &result["record"];
        assert_eq!(record["id"], id);
        assert_eq!(record["revision"], "qt-revision");
        assert_eq!(record["provenance"], 2);
        assert_eq!(record["source"], "source");
        assert_eq!(record["corrections"].as_array().unwrap().len(), 1);
        assert!(store.import_snapshot(&snapshot).is_err());
        assert_eq!(fs::read(&source).unwrap(), source_before);
        assert_eq!(
            store
                .manage(session, &json!({"operation":"read","scope":1,"id":id}))
                .unwrap()["record"]["value"],
            "value"
        );
        assert_eq!(store.manage(session,&json!({"operation":"correct","scope":1,"session":session,"workspace":"","id":id,"revision":"qt-revision","key":"k","value":"v"})).unwrap()["status"],"Unavailable");
        drop(store);
        drop(snapshot);
        let db = Connection::open(&source).unwrap();
        db.execute("UPDATE scoped_memory_entries SET scope=2", [])
            .unwrap();
        drop(db);
        assert!(read(&source, &target, &[session.to_owned()]).is_err());
        let inventory = read_preview(&source, &target, &[session.to_owned()], true).unwrap();
        assert!(inventory.rows.is_empty());
        assert_eq!(inventory.excluded.project, 1);
        drop(inventory);

        fs::remove_dir_all(source_dir).unwrap();
        fs::remove_dir_all(target_dir).unwrap();
    }
}
