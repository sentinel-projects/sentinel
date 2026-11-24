//! Strict offline restore of a coordinated development-only snapshot set.
use rusqlite::Connection;
use sha2::{Digest, Sha256};
use std::{
    collections::{HashMap, HashSet},
    ffi::CString,
    fs::{self, File, OpenOptions},
    io::{self, Read, Write},
    os::unix::{
        ffi::OsStrExt,
        fs::{DirBuilderExt, MetadataExt, OpenOptionsExt},
    },
    path::{Path, PathBuf},
};

const MAGIC: &[u8] = crate::backup::SET_MAGIC;
const LIMIT: usize = 16 * 1024 * 1024;
const NAMES: [&str; 4] = [
    crate::store::DATABASE,
    crate::settings::DATABASE,
    crate::memory::DATABASE,
    crate::agent_store::DATABASE,
];
const DOMAINS: [&str; 4] = ["chat", "settings", "memory", "agent"];
fn bad() -> io::Error {
    io::Error::new(
        io::ErrorKind::InvalidData,
        "invalid or unsupported development snapshot set",
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
fn schema(db: &Connection) -> rusqlite::Result<Vec<(String, String, String, String)>> {
    let mut stmt = db.prepare("SELECT type,name,tbl_name,COALESCE(sql,'') FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name")?;
    let rows = stmt.query_map([], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)))?;
    rows.collect()
}
fn decode(bytes: &[u8]) -> Result<Connection, Box<dyn std::error::Error>> {
    let mut db = Connection::open_in_memory()?;
    db.deserialize_read_exact("main", &mut &bytes[..], bytes.len(), true)?;
    db.execute_batch("PRAGMA trusted_schema=OFF; PRAGMA query_only=ON; PRAGMA foreign_keys=ON;")?;
    let check: String = db.query_row("PRAGMA quick_check", [], |r| r.get(0))?;
    if check != "ok"
        || db.query_row(
            "SELECT EXISTS(SELECT 1 FROM pragma_foreign_key_check)",
            [],
            |r| r.get::<_, bool>(0),
        )?
    {
        return Err(bad().into());
    }
    Ok(db)
}
fn text_rows(db: &Connection, query: &str) -> rusqlite::Result<Vec<String>> {
    let mut stmt = db.prepare(query)?;
    let rows = stmt.query_map([], |r| r.get(0))?;
    rows.collect()
}
fn validate_relations(dbs: &[Connection]) -> Result<(), Box<dyn std::error::Error>> {
    let chat = &dbs[0];
    let memory = &dbs[2];
    let agent = &dbs[3];
    let sessions: HashSet<String> = text_rows(chat, "SELECT id FROM sessions")?
        .into_iter()
        .collect();
    if sessions.len() > 64 {
        return Err(bad().into());
    }
    let mut statement = chat.prepare("SELECT id,session_id,state FROM runs")?;
    let rows = statement.query_map([], |r| {
        Ok((
            r.get::<_, String>(0)?,
            r.get::<_, String>(1)?,
            r.get::<_, String>(2)?,
        ))
    })?;
    let mut runs = HashMap::new();
    let mut run_states = HashMap::new();
    for row in rows {
        let (id, owner, state) = row?;
        if !sessions.contains(&owner) || matches!(state.as_str(), "running" | "approval") {
            return Err(bad().into());
        }
        run_states.insert(id.clone(), state);
        runs.insert(id, owner);
    }
    let mut statement = chat.prepare("SELECT id,run_id,state FROM sessions WHERE run_id!=''")?;
    let rows = statement.query_map([], |r| {
        Ok((
            r.get::<_, String>(0)?,
            r.get::<_, String>(1)?,
            r.get::<_, String>(2)?,
        ))
    })?;
    for row in rows {
        let (sid, rid, state) = row?;
        if runs.get(&rid) != Some(&sid) || matches!(state.as_str(), "running" | "approval") {
            return Err(bad().into());
        }
    }
    // Per-session logical references and ownership must agree, beyond independent FKs.
    let mut messages = chat.prepare("SELECT session_id,run_id,logical_id,role,content,status,partial,provider_id,model_id,timestamp,reply_to,replaces,attachments FROM messages ORDER BY session_id,logical_id")?;
    let mut rows = messages.query([])?;
    let mut ids: HashMap<String, HashMap<i64, String>> = HashMap::new();
    while let Some(row) = rows.next()? {
        let sid: String = row.get(0)?;
        let rid: String = row.get(1)?;
        let id: i64 = row.get(2)?;
        let role: String = row.get(3)?;
        let content: String = row.get(4)?;
        let status: String = row.get(5)?;
        let partial: i64 = row.get(6)?;
        let provider: String = row.get(7)?;
        let model: String = row.get(8)?;
        let timestamp: String = row.get(9)?;
        let reply: i64 = row.get(10)?;
        let replaces: i64 = row.get(11)?;
        let attachments: String = row.get(12)?;
        if runs.get(&rid) != Some(&sid)
            || id <= 0
            || id > i32::MAX as i64 - 32
            || !["system", "user", "assistant"].contains(&role.as_str())
            || ![
                "sent",
                "received",
                "completed",
                "failed",
                "cancelled",
                "interrupted",
                "error",
            ]
            .contains(&status.as_str())
            || ![0, 1].contains(&partial)
            || content.len() > crate::store::MAX_PROMPT
            || provider.len() > 1024
            || model.len() > 1024
            || !crate::import::timestamp(&timestamp)
            || attachments.len() > 200_000
            || crate::attachment::parts("", &attachments).is_err()
        {
            return Err(bad().into());
        }
        let parsed: serde_json::Value = serde_json::from_str(&attachments)?;
        if role != "user" && parsed.as_array().is_none_or(|items| !items.is_empty()) {
            return Err(bad().into());
        }
        let seen = ids.entry(sid).or_default();
        if seen.len() >= 64
            || seen.contains_key(&id)
            || (reply != 0 && !seen.contains_key(&reply))
            || (replaces != 0 && !seen.contains_key(&replaces))
        {
            return Err(bad().into());
        }
        seen.insert(id, role);
    }
    let invalid_sessions: bool = chat.query_row("SELECT EXISTS(SELECT 1 FROM sessions WHERE state IN ('running','approval') OR length(id)>256 OR length(title)>4096 OR length(output)>65536 OR length(model)>1024 OR length(detail)>4096)", [], |r| r.get(0))?;
    if invalid_sessions {
        return Err(bad().into());
    }
    for sid in text_rows(memory, "SELECT DISTINCT session FROM entries")? {
        if !sessions.contains(&sid) {
            return Err(bad().into());
        }
    }
    let mut statement = agent.prepare("SELECT id,session_id,state,confirmed FROM runs")?;
    let rows = statement.query_map([], |r| {
        Ok((
            r.get::<_, String>(0)?,
            r.get::<_, String>(1)?,
            r.get::<_, String>(2)?,
            r.get::<_, i64>(3)?,
        ))
    })?;
    for row in rows {
        let (id, sid, state, confirmed) = row?;
        if !sessions.contains(&sid)
            || confirmed != 1
            || matches!(state.as_str(), "running" | "approval")
            || runs.get(&id).is_some_and(|owner| owner != &sid)
            || run_states
                .get(&id)
                .is_some_and(|chat_state| chat_state != &state)
        {
            return Err(bad().into());
        }
    }
    let unfinished: bool = agent.query_row("SELECT EXISTS(SELECT 1 FROM approvals WHERE decision='pending') OR EXISTS(SELECT 1 FROM plan_steps WHERE status IN ('pending','started'))", [], |r| r.get(0))?;
    if unfinished {
        return Err(bad().into());
    }
    Ok(())
}
pub(crate) fn publish(from: &Path, to: &Path) -> io::Result<()> {
    let from = CString::new(from.as_os_str().as_bytes()).map_err(|_| bad())?;
    let to = CString::new(to.as_os_str().as_bytes()).map_err(|_| bad())?;
    #[cfg(target_os = "macos")]
    let rc = unsafe { libc::renamex_np(from.as_ptr(), to.as_ptr(), libc::RENAME_EXCL) };
    #[cfg(target_os = "linux")]
    let rc = unsafe {
        libc::renameat2(
            libc::AT_FDCWD,
            from.as_ptr(),
            libc::AT_FDCWD,
            to.as_ptr(),
            libc::RENAME_NOREPLACE,
        )
    };
    if rc != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}
pub fn restore(
    destination: &Path,
    source: &Path,
    persistent: bool,
) -> Result<(), Box<dyn std::error::Error>> {
    let tmp = Path::new("/tmp").canonicalize()?;
    let destination_parent = destination.parent().ok_or_else(bad)?.canonicalize()?;
    if !destination.is_absolute()
        || (!persistent && destination_parent != tmp)
        || destination.file_name().is_none()
        || !matches!(fs::symlink_metadata(destination), Err(error) if error.kind() == io::ErrorKind::NotFound)
    {
        return Err(bad().into());
    }
    let parent = source.parent().ok_or_else(bad)?;
    let meta = fs::symlink_metadata(parent)?;
    if !source.is_absolute()
        || !meta.is_dir()
        || meta.uid() != unsafe { libc::geteuid() }
        || meta.mode() & 0o7777 != 0o700
        || parent.canonicalize()?.parent() != Some(tmp.as_path())
    {
        return Err(bad().into());
    }
    let meta = fs::symlink_metadata(source)?;
    if !meta.is_file()
        || meta.uid() != unsafe { libc::geteuid() }
        || meta.nlink() != 1
        || !matches!(meta.mode() & 0o7777, 0o400 | 0o600)
        || meta.len() as usize > LIMIT
    {
        return Err(bad().into());
    }
    let mut file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(source)?;
    let opened = file.metadata()?;
    if opened.dev() != meta.dev() || opened.ino() != meta.ino() {
        return Err(bad().into());
    }
    let mut bytes = Vec::with_capacity(meta.len() as usize);
    Read::by_ref(&mut file)
        .take(LIMIT as u64 + 1)
        .read_to_end(&mut bytes)?;
    let after = file.metadata()?;
    let current = fs::symlink_metadata(source)?;
    if bytes.len() != meta.len() as usize
        || after.len() != meta.len()
        || after.mtime() != meta.mtime()
        || after.mtime_nsec() != meta.mtime_nsec()
        || current.dev() != meta.dev()
        || current.ino() != meta.ino()
        || current.uid() != meta.uid()
        || current.mode() != meta.mode()
        || current.nlink() != 1
        || current.len() != meta.len()
        || current.mtime() != meta.mtime()
        || current.mtime_nsec() != meta.mtime_nsec()
        || after.uid() != meta.uid()
        || after.mode() != meta.mode()
        || after.nlink() != 1
    {
        return Err(bad().into());
    }
    if !bytes.starts_with(MAGIC) || bytes.len() < MAGIC.len() + 4 {
        return Err(bad().into());
    }
    let pos = MAGIC.len();
    let size = u32::from_be_bytes(bytes[pos..pos + 4].try_into()?) as usize;
    let mut cursor = pos + 4;
    if size > 4096 || cursor.checked_add(size).is_none_or(|n| n > bytes.len()) {
        return Err(bad().into());
    }
    let manifest: serde_json::Value = serde_json::from_slice(&bytes[cursor..cursor + size])?;
    cursor += size;
    let object = manifest
        .as_object()
        .filter(|v| v.len() == 2)
        .ok_or_else(bad)?;
    let stores = object
        .get("stores")
        .and_then(serde_json::Value::as_array)
        .ok_or_else(bad)?;
    if object.get("format").and_then(serde_json::Value::as_str) != Some(crate::backup::SET_FORMAT)
        || stores.len() != 4
    {
        return Err(bad().into());
    }
    let mut sections = Vec::new();
    for (item, domain) in stores.iter().zip(DOMAINS) {
        let item = item.as_object().filter(|v| v.len() == 3).ok_or_else(bad)?;
        let size = item
            .get("size")
            .and_then(serde_json::Value::as_u64)
            .and_then(|v| usize::try_from(v).ok())
            .ok_or_else(bad)?;
        let hash = item
            .get("sha256")
            .and_then(serde_json::Value::as_str)
            .ok_or_else(bad)?;
        let end = cursor
            .checked_add(size)
            .filter(|n| *n <= bytes.len())
            .ok_or_else(bad)?;
        let section = &bytes[cursor..end];
        if item.get("domain").and_then(serde_json::Value::as_str) != Some(domain)
            || size == 0
            || format!("{:x}", Sha256::digest(section)) != hash
        {
            return Err(bad().into());
        }
        sections.push(section);
        cursor = end;
    }
    if cursor != bytes.len() {
        return Err(bad().into());
    }
    let mut stage = None;
    for i in 0..128 {
        let path = destination_parent.join(format!(".sentinel-restore-{}-{i}", std::process::id()));
        match fs::DirBuilder::new().mode(0o700).create(&path) {
            Ok(()) => {
                stage = Some(path);
                break;
            }
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => {}
            Err(e) => return Err(e.into()),
        }
    }
    let mut stage = Stage(Some(stage.ok_or_else(bad)?));
    let root = stage.0.as_ref().ok_or_else(bad)?;
    let _profile_lock = crate::unix::lock_profile(root)?;
    drop(crate::store::Store::open(root)?);
    drop(crate::settings::Settings::open(root)?);
    drop(crate::memory::MemoryStore::open(root)?);
    drop(crate::agent_store::AgentStore::open(root)?);
    let mut dbs = Vec::new();
    for (section, name) in sections.iter().zip(NAMES) {
        let incoming = decode(section)?;
        let reference = Connection::open(root.join(name))?;
        let id: (u32, u32) = (
            incoming.query_row("PRAGMA application_id", [], |r| r.get(0))?,
            incoming.query_row("PRAGMA user_version", [], |r| r.get(0))?,
        );
        let expected: (u32, u32) = (
            reference.query_row("PRAGMA application_id", [], |r| r.get(0))?,
            reference.query_row("PRAGMA user_version", [], |r| r.get(0))?,
        );
        let mut reference_schema = schema(&reference)?;
        let previous_chat =
            name == NAMES[0] && id.0 == expected.0 && matches!(id.1, 4 | 5) && expected.1 == 6;
        if previous_chat {
            reference_schema.retain(|(_, _, table, _)| {
                table != "imported_message_errors" && (id.1 != 4 || table != "session_flags")
            });
        }
        if (!previous_chat && id != expected) || schema(&incoming)? != reference_schema {
            return Err(bad().into());
        }
        dbs.push(incoming);
    }
    validate_relations(&dbs)?;
    crate::restore_validate::settings_rows(&dbs[1])?;
    crate::restore_validate::memory_rows(&dbs[2])?;
    crate::restore_validate::agent_rows(&dbs[3])?;
    for (section, name) in sections.iter().zip(NAMES) {
        let path = root.join(name);
        let mut out = OpenOptions::new()
            .write(true)
            .truncate(true)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(path)?;
        out.write_all(section)?;
        out.sync_all()?;
    }
    let settings = crate::settings::Settings::open(root)?;
    let _ = settings.desktop_values()?;
    let _ = settings.workspace_selection()?;
    drop(settings);
    let store = crate::store::Store::open_for_import(root)?;
    for session in store.load()? {
        let messages = store.messages(&session.id)?;
        if !crate::store::history_fits(&session.id, &messages) {
            return Err(bad().into());
        }
    }
    drop(store);
    File::open(root)?.sync_all()?;
    let parent = File::open(destination.parent().ok_or_else(bad)?)?;
    publish(root, destination)?;
    stage.0 = None;
    parent.sync_all().map_err(|error| {
        io::Error::new(
            error.kind(),
            format!("restored profile is visible, but directory sync failed: {error}"),
        )
    })?;
    Ok(())
}
