//! Offline reverse export of the supported conversation subset to a new Qt copy.
use rusqlite::{Connection, OpenFlags, params};
use sha2::{Digest, Sha256};
use std::{
    collections::{HashMap, HashSet},
    fs::{self, File, OpenOptions},
    io::{self, Write},
    os::unix::fs::{MetadataExt, OpenOptionsExt},
    path::Path,
};

const SCHEMA: &str = "
CREATE TABLE conversations(
 id TEXT PRIMARY KEY NOT NULL,title TEXT NOT NULL,created_at TEXT NOT NULL,
 updated_at TEXT NOT NULL,archived INTEGER NOT NULL DEFAULT 0,
 pinned INTEGER NOT NULL DEFAULT 0,deleted INTEGER NOT NULL DEFAULT 0,
 user_renamed INTEGER NOT NULL DEFAULT 0);
CREATE TABLE conversation_messages(
 conversation_id TEXT NOT NULL,message_id INTEGER NOT NULL,role TEXT NOT NULL,
 content TEXT NOT NULL,timestamp TEXT NOT NULL,status TEXT NOT NULL,
 provider_id TEXT NOT NULL DEFAULT '',model_id TEXT NOT NULL DEFAULT '',
 reply_to_id INTEGER NOT NULL DEFAULT 0,replaces_id INTEGER NOT NULL DEFAULT 0,
 partial INTEGER NOT NULL DEFAULT 0,error_category TEXT NOT NULL DEFAULT 'None',
 attachments_json TEXT NOT NULL DEFAULT '[]',
 PRIMARY KEY(conversation_id,message_id),
 FOREIGN KEY(conversation_id) REFERENCES conversations(id));
CREATE TABLE conversation_summary_metadata(
 conversation_id TEXT PRIMARY KEY NOT NULL,summary_timestamp TEXT NOT NULL,
 covered_first_message_id INTEGER NOT NULL DEFAULT 0,
 covered_last_message_id INTEGER NOT NULL DEFAULT 0,
 estimated_reduction_percent INTEGER NOT NULL DEFAULT 0,
 readiness_state TEXT NOT NULL,summary_text TEXT NOT NULL DEFAULT '',
 summary TEXT NOT NULL,FOREIGN KEY(conversation_id) REFERENCES conversations(id));
CREATE TABLE conversation_schema_metadata(key TEXT PRIMARY KEY NOT NULL,value INTEGER NOT NULL);
INSERT INTO conversation_schema_metadata VALUES ('schema_version',6);";

fn invalid(detail: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, detail)
}

fn qt_id(id: &str) -> String {
    if crate::import::uuid(id) {
        return id.to_owned();
    }
    derived_id(b"sentinel-qt-conversation-export-v1", id)
}

fn derived_id(namespace: &[u8], id: &str) -> String {
    let mut hash = Sha256::new();
    hash.update(namespace);
    hash.update(id.as_bytes());
    let mut digest = hash.finalize();
    digest[6] = (digest[6] & 0x0f) | 0x80;
    digest[8] = (digest[8] & 0x3f) | 0x80;
    let bytes: String = digest[..16]
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect();
    format!(
        "{}-{}-{}-{}-{}",
        &bytes[..8],
        &bytes[8..12],
        &bytes[12..16],
        &bytes[16..20],
        &bytes[20..]
    )
}

fn open_read_source(
    path: &Path,
    expected_app: u32,
    expected_version: u32,
) -> Result<(Connection, fs::Metadata), Box<dyn std::error::Error>> {
    let before = fs::symlink_metadata(path)?;
    if !before.is_file()
        || before.uid() != unsafe { libc::geteuid() }
        || before.mode() & 0o7777 != 0o600
        || before.nlink() != 1
        || before.len() > 128 * 1024 * 1024
    {
        return Err(invalid("unsafe Rust export source").into());
    }
    crate::import::no_sidecars(path)?;
    let source = Connection::open_with_flags(
        path,
        OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NOFOLLOW,
    )?;
    source.busy_timeout(std::time::Duration::from_millis(250))?;
    source.execute_batch("PRAGMA trusted_schema=OFF; PRAGMA query_only=ON; BEGIN;")?;
    let app_id: u32 = source.query_row("PRAGMA application_id", [], |row| row.get(0))?;
    let version: u32 = source.query_row("PRAGMA user_version", [], |row| row.get(0))?;
    let journal: String = source.query_row("PRAGMA journal_mode", [], |row| row.get(0))?;
    let integrity: String = source.query_row("PRAGMA quick_check", [], |row| row.get(0))?;
    if app_id != expected_app
        || version != expected_version
        || journal != "delete"
        || integrity != "ok"
    {
        return Err(invalid("unsupported Rust export snapshot").into());
    }
    Ok((source, before))
}

fn verify_source(path: &Path, before: &fs::Metadata) -> io::Result<()> {
    crate::import::no_sidecars(path)?;
    let after = fs::symlink_metadata(path)?;
    if before.dev() != after.dev()
        || before.ino() != after.ino()
        || before.len() != after.len()
        || before.mtime() != after.mtime()
        || before.mtime_nsec() != after.mtime_nsec()
        || before.ctime() != after.ctime()
        || before.ctime_nsec() != after.ctime_nsec()
    {
        return Err(invalid("Rust export source changed"));
    }
    Ok(())
}

fn export_settings(profile: &Path, root: &Path) -> Result<usize, Box<dyn std::error::Error>> {
    let path = profile.join(crate::settings::DATABASE);
    let (settings, before) =
        open_read_source(&path, crate::settings::APP_ID, crate::settings::VERSION)?;
    let mut query = settings.prepare(
        "SELECT key,value FROM preferences WHERE key IN ('themeName','responseProfileInstructions') ORDER BY key",
    )?;
    let mut transferable = serde_json::Map::new();
    for row in query.query_map([], |row| {
        Ok((row.get::<_, String>(0)?, row.get::<_, String>(1)?))
    })? {
        let (key, value) = row?;
        if !crate::settings::valid_preference(&key, &value)
            || transferable
                .insert(key, serde_json::Value::String(value))
                .is_some()
        {
            return Err(invalid("invalid transferable Rust preference").into());
        }
    }
    let count = transferable.len();
    transferable.insert("sentinelSettingsSchemaVersion".into(), serde_json::json!(1));
    verify_source(&path, &before)?;
    let mut output = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(root.join("settings.json"))?;
    serde_json::to_writer_pretty(&mut output, &transferable)?;
    output.write_all(b"\n")?;
    output.sync_all()?;
    Ok(count)
}

fn export_memory(
    profile: &Path,
    root: &Path,
    session_ids: &HashMap<String, String>,
) -> Result<usize, Box<dyn std::error::Error>> {
    let path = profile.join(crate::memory::DATABASE);
    let (source, before) = open_read_source(&path, crate::memory::APP_ID, crate::memory::VERSION)?;
    let output = root.join("memory.sqlite3");
    OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(&output)?;
    let mut db = Connection::open_with_flags(
        &output,
        OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_NOFOLLOW,
    )?;
    db.execute_batch("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;")?;
    let tx = db.transaction()?;
    tx.execute_batch(
        "CREATE TABLE memory_schema_metadata(key TEXT PRIMARY KEY NOT NULL,value INTEGER NOT NULL);
         INSERT INTO memory_schema_metadata VALUES ('schema_version',7);
         CREATE TABLE memory_entries(id INTEGER PRIMARY KEY,key TEXT UNIQUE NOT NULL,value TEXT NOT NULL);
         CREATE TABLE scoped_memory_entries(
           id TEXT PRIMARY KEY NOT NULL,scope INTEGER NOT NULL CHECK(scope IN (1,2,3)),
           owner_id TEXT NOT NULL,workspace_id TEXT NOT NULL,key TEXT NOT NULL,value TEXT NOT NULL,
           provenance INTEGER NOT NULL CHECK(provenance BETWEEN 0 AND 6),source_ref TEXT NOT NULL,
           created_at TEXT NOT NULL,updated_at TEXT NOT NULL,revision TEXT NOT NULL,corrections TEXT NOT NULL,
           UNIQUE(scope,owner_id,workspace_id,key),
           CHECK((scope=1 AND owner_id!='') OR (scope=2 AND owner_id!='' AND owner_id=workspace_id)
                 OR (scope=3 AND owner_id='' AND workspace_id='')));",
    )?;
    let mut stmt = source.prepare(
        "SELECT e.id,e.session,e.key,e.value,e.revision,e.created,e.updated,
                q.qt_id,q.qt_revision,q.provenance,q.source,q.corrections
         FROM entries e LEFT JOIN qt_imports q ON q.entry_id=e.id ORDER BY e.id LIMIT 10001",
    )?;
    let mut rows = stmt.query([])?;
    let mut count = 0;
    let mut bytes = 0usize;
    while let Some(row) = rows.next()? {
        count += 1;
        if count > 10_000 {
            return Err(invalid("Qt memory row limit").into());
        }
        let entry: i64 = row.get(0)?;
        let session: String = row.get(1)?;
        let owner = session_ids
            .get(&session)
            .ok_or_else(|| invalid("orphan Rust session memory"))?;
        let key: String = row.get(2)?;
        let value: String = row.get(3)?;
        let revision: i64 = row.get(4)?;
        let created: String = row.get(5)?;
        let updated: String = row.get(6)?;
        let qt_id: Option<String> = row.get(7)?;
        let qt_revision: Option<String> = row.get(8)?;
        let provenance: Option<i64> = row.get(9)?;
        let reference: Option<String> = row.get(10)?;
        let corrections: Option<String> = row.get(11)?;
        let id = qt_id.unwrap_or_else(|| {
            derived_id(b"sentinel-qt-memory-export-v1", &format!("{owner}:{entry}"))
        });
        let revision = qt_revision.unwrap_or_else(|| {
            derived_id(
                b"sentinel-qt-memory-revision-v1",
                &format!("{id}:{revision}"),
            )
        });
        let reference = reference.unwrap_or_else(|| format!("manual:{owner}"));
        let corrections = corrections.unwrap_or_else(|| "[]".to_owned());
        bytes = bytes
            .checked_add(key.len() + value.len() + reference.len() + corrections.len())
            .ok_or_else(|| invalid("Qt memory byte limit"))?;
        if bytes > 16 * 1024 * 1024 {
            return Err(invalid("Qt memory byte limit").into());
        }
        tx.execute(
            "INSERT INTO scoped_memory_entries VALUES(?1,1,?2,'',?3,?4,?5,?6,?7,?8,?9,?10)",
            params![
                id,
                owner,
                key,
                value,
                provenance.unwrap_or(1),
                reference,
                created,
                updated,
                revision,
                corrections
            ],
        )?;
    }
    tx.commit()?;
    db.close().map_err(|(_, error)| error)?;
    let known = session_ids.values().cloned().collect::<Vec<_>>();
    let checked = crate::memory_import::read(&output, profile, &known)?;
    if checked.rows.len() != count {
        return Err(invalid("Qt memory verification failed").into());
    }
    checked.validate_unchanged()?;
    drop(checked);
    verify_source(&path, &before)?;
    File::open(output)?.sync_all()?;
    Ok(count)
}

pub fn export(
    profile: &Path,
    destination: &Path,
    include_settings: bool,
    include_memory: bool,
) -> Result<serde_json::Value, Box<dyn std::error::Error>> {
    let temporary = Path::new("/tmp").canonicalize()?;
    let parent = destination
        .parent()
        .ok_or_else(|| invalid("missing export parent"))?;
    if !destination.is_absolute()
        || parent.canonicalize()? != temporary
        || destination.file_name().is_none()
        || !matches!(fs::symlink_metadata(destination), Err(e) if e.kind() == io::ErrorKind::NotFound)
    {
        return Err(invalid("export requires a new directory directly under /tmp").into());
    }
    let path = profile.join(crate::store::DATABASE);
    let (source, before) = open_read_source(&path, crate::store::APP_ID, crate::store::VERSION)?;
    let active: bool = source.query_row(
        "SELECT EXISTS(SELECT 1 FROM runs WHERE state='running')",
        [],
        |row| row.get(0),
    )?;
    if active {
        return Err(invalid("recover interrupted Rust runs before exporting").into());
    }

    let mut stage = crate::qt_preview_import::temporary_stage(&temporary, "reverse")?;
    let root = stage
        .0
        .as_ref()
        .ok_or_else(|| invalid("missing export stage"))?;
    let output = root.join("conversations.sqlite3");
    OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(&output)?;
    let mut db = Connection::open_with_flags(
        &output,
        OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_NOFOLLOW,
    )?;
    db.execute_batch(
        "PRAGMA foreign_keys=ON; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;",
    )?;
    let tx = db.transaction()?;
    tx.execute_batch(SCHEMA)?;
    let mut sessions = source.prepare(
        "SELECT s.id,s.title,s.created_at,s.updated_at,COALESCE(f.archived,0),COALESCE(f.pinned,0)
         FROM sessions s LEFT JOIN session_flags f ON f.session_id=s.id ORDER BY s.rowid LIMIT 65",
    )?;
    let mut sessions = sessions.query([])?;
    let mut messages = source.prepare(
        "SELECT m.logical_id,m.role,m.content,m.timestamp,m.status,m.provider_id,m.model_id,
                m.reply_to,m.replaces,m.partial,COALESCE(e.category,'None'),m.attachments
         FROM messages m LEFT JOIN imported_message_errors e
              ON e.session_id=m.session_id AND e.logical_id=m.logical_id
         WHERE m.session_id=?1 ORDER BY m.logical_id LIMIT 33",
    )?;
    let mut ids = HashSet::new();
    let mut session_ids = HashMap::new();
    let mut rewritten_ids = 0;
    let mut inferred_timestamps = 0;
    let mut message_count = 0;
    while let Some(row) = sessions.next()? {
        let original: String = row.get(0)?;
        let id = qt_id(&original);
        if !ids.insert(id.to_ascii_lowercase()) || ids.len() > 64 {
            return Err(invalid("Qt conversation limit or ID collision").into());
        }
        rewritten_ids += usize::from(id != original);
        session_ids.insert(original.clone(), id.clone());
        let title: String = row.get(1)?;
        let created: String = row.get(2)?;
        let updated: String = row.get(3)?;
        let archived: bool = row.get(4)?;
        let pinned: bool = row.get(5)?;
        let mut rows = messages.query([&original])?;
        let mut records = Vec::new();
        while let Some(message) = rows.next()? {
            if records.len() >= 32 || message_count >= 2048 {
                return Err(invalid("Qt conversation message limit").into());
            }
            records.push((
                message.get::<_, i64>(0)?,
                message.get::<_, String>(1)?,
                message.get::<_, String>(2)?,
                message.get::<_, String>(3)?,
                message.get::<_, String>(4)?,
                message.get::<_, String>(5)?,
                message.get::<_, String>(6)?,
                message.get::<_, i64>(7)?,
                message.get::<_, i64>(8)?,
                message.get::<_, bool>(9)?,
                message.get::<_, String>(10)?,
                message.get::<_, String>(11)?,
            ));
            message_count += 1;
        }
        inferred_timestamps += usize::from(created.is_empty()) + usize::from(updated.is_empty());
        let created = if created.is_empty() {
            records
                .first()
                .map_or("1970-01-01T00:00:00.000Z", |entry| entry.3.as_str())
        } else {
            &created
        };
        let updated = if updated.is_empty() {
            records.last().map_or(created, |entry| entry.3.as_str())
        } else {
            &updated
        };
        tx.execute(
            "INSERT INTO conversations VALUES (?1,?2,?3,?4,?5,?6,0,1)",
            params![id, title, created, updated, archived, pinned],
        )?;
        for (
            number,
            role,
            content,
            time,
            status,
            provider,
            model,
            reply,
            replaces,
            partial,
            category,
            attachments,
        ) in records
        {
            tx.execute(
                "INSERT INTO conversation_messages VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)",
                params![id,number,role,content,time,status,provider,model,reply,replaces,partial,category,attachments],
            )?;
        }
    }
    tx.commit()?;
    db.close().map_err(|(_, error)| error)?;
    let validated = crate::import::read(&output, profile)?;
    if validated.conversations.len() != ids.len()
        || validated
            .conversations
            .iter()
            .map(|conversation| conversation.messages.len())
            .sum::<usize>()
            != message_count
    {
        return Err(invalid("Qt conversation verification failed").into());
    }
    validated.validate_unchanged()?;
    drop(validated);
    verify_source(&path, &before)?;
    let preferences = include_settings
        .then(|| export_settings(profile, root))
        .transpose()?;
    let memory = include_memory
        .then(|| export_memory(profile, root, &session_ids))
        .transpose()?;
    File::open(&output)?.sync_all()?;
    File::open(root)?.sync_all()?;
    crate::set_restore::publish(root, destination)?;
    stage.0 = None;
    File::open(parent)?.sync_all().map_err(|error| {
        io::Error::new(
            error.kind(),
            format!("Qt export is visible but directory synchronization failed: {error}"),
        )
    })?;
    Ok(serde_json::json!({
        "schema_version":1,"conversations":ids.len(),"messages":message_count,
        "rewritten_ids":rewritten_ids,"inferred_timestamps":inferred_timestamps,
        "exported_preferences":preferences,
        "exported_session_memory":memory,
        "excluded":["other_settings","other_memory_scopes","agent_audit","permissions","credentials","workspaces"],
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn exports_a_closed_rust_conversation_and_refuses_to_replace_the_copy() {
        let tmp = Path::new("/tmp").canonicalize().unwrap();
        let fixture = crate::qt_preview_import::temporary_stage(&tmp, "reverse-test").unwrap();
        let root = fixture.0.as_ref().unwrap();
        let mut store = crate::store::Store::open(root).unwrap();
        store.create("rust-session", "Title").unwrap();
        store
            .begin_with_provider("rust-session", "run", "Question", "model", "ollama")
            .unwrap();
        store
            .finish("rust-session", "run", "completed", "Answer", "model")
            .unwrap();
        store
            .set_session_flags("rust-session", false, true)
            .unwrap();
        drop(store);
        let mut memory = crate::memory::MemoryStore::open(root).unwrap();
        let created = memory.manage("rust-session", &serde_json::json!({
            "operation":"create","scope":1,"session":"rust-session","workspace":"","key":"note","value":"Retained"
        })).unwrap();
        assert_eq!(created["status"], "Ready");
        drop(memory);
        let destination = root.with_extension("qt-copy");
        let published = export(root, &destination, false, true).unwrap();
        let published_stage = crate::qt_preview_import::Stage(Some(destination.clone()));
        assert_eq!(published["conversations"], 1);
        assert_eq!(published["messages"], 2);
        assert_eq!(published["exported_session_memory"], 1);
        let db = crate::import::read(&destination.join("conversations.sqlite3"), root).unwrap();
        let chat = &db.conversations[0];
        assert_eq!(chat.id, qt_id("rust-session"));
        assert_eq!(chat.messages[1].content, "Answer");
        assert!(chat.pinned);
        let returned_memory = crate::memory_import::read(
            &destination.join("memory.sqlite3"),
            root,
            std::slice::from_ref(&chat.id),
        )
        .unwrap();
        assert_eq!(returned_memory.rows[0].session, chat.id);
        assert_eq!(returned_memory.rows[0].value, "Retained");
        drop(returned_memory);
        assert!(export(root, &destination, false, false).is_err());
        drop(db);
        drop(published_stage);
    }
}
