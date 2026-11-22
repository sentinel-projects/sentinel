//! Isolated development-only SQLite chat store; not compatible with Qt profiles.
use rusqlite::OpenFlags;
use rusqlite::{Connection, OptionalExtension, TransactionBehavior, params};
use serde_json::{Value, json};
use std::{
    fs::{self, OpenOptions},
    io::Read,
    os::unix::fs::{MetadataExt, OpenOptionsExt},
};
use std::{io, path::Path};

pub const DATABASE: &str = "daemon-dev-chat.sqlite3";
pub const JOURNAL: &str = "daemon-dev-chat.sqlite3-journal";
pub const MAX_TURNS: i64 = 16;
pub const MAX_PROMPT: usize = 65536;
pub const MAX_OUTPUT: usize = 32768;
pub(crate) const MAX_IMPORTED_MESSAGES_PER_SESSION: usize = 32;
const MAX_CONTEXT_ROWS: usize = MAX_IMPORTED_MESSAGES_PER_SESSION + 2 * MAX_TURNS as usize;
pub(crate) const APP_ID: u32 = 0x5344_4348; // SDCH
pub(crate) const VERSION: u32 = 4;
const HEADER: &[u8; 16] = b"SQLite format 3\0";

pub struct LoadedSession {
    pub id: String,
    pub title: String,
    pub run_id: String,
    pub state: String,
    pub output: String,
    pub model: String,
    pub provider_id: String,
    pub detail: String,
    pub created_at: String,
    pub updated_at: String,
}
pub struct Store {
    db: Connection,
}
#[derive(Debug)]
pub struct BranchRequest<'a> {
    pub source: &'a str,
    pub branch: &'a str,
    pub run: &'a str,
    pub title: &'a str,
    pub user_id: i64,
    pub text: &'a str,
    pub model: &'a str,
    pub provider: &'a str,
}
/// Only a failed BEGIN IMMEDIATE with BUSY/LOCKED proves no write began.
#[derive(Debug)]
pub struct WriteFailure {
    pub safe_conflict: bool,
}
pub fn is_conflict(error: &rusqlite::Error) -> bool {
    matches!(
        error.sqlite_error_code(),
        Some(rusqlite::ErrorCode::DatabaseBusy | rusqlite::ErrorCode::DatabaseLocked)
    )
}
pub(crate) fn acquisition_failure(error: rusqlite::Error) -> WriteFailure {
    let safe_conflict = is_conflict(&error);
    WriteFailure { safe_conflict }
}
pub(crate) fn mutation_failure(_: rusqlite::Error) -> WriteFailure {
    WriteFailure {
        safe_conflict: false,
    }
}
fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}
fn safe_file(path: &Path) -> io::Result<std::fs::Metadata> {
    let meta = fs::symlink_metadata(path)?;
    if !meta.file_type().is_file()
        || meta.uid() != unsafe { libc::geteuid() }
        || meta.mode() & 0o7777 != 0o600
        || meta.nlink() != 1
    {
        return Err(invalid("unsafe development SQLite artifact"));
    }
    Ok(meta)
}
pub(crate) fn open_database(
    profile: &Path,
    database: &str,
    expected_app_id: u32,
    version_limit: u32,
    importing: bool,
) -> Result<(Connection, bool, u32), Box<dyn std::error::Error>> {
    let path = profile.join(database);
    if importing && fs::symlink_metadata(&path).is_err() {
        return Err(invalid("development destination must already exist").into());
    }
    let new = if importing {
        false
    } else {
        match OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(&path)
        {
            Ok(_) => true,
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => false,
            Err(e) => return Err(e.into()),
        }
    };
    let identity = safe_file(&path)?;
    for suffix in ["-wal", "-shm"] {
        if fs::symlink_metadata(profile.join(format!("{database}{suffix}"))).is_ok() {
            return Err(invalid("unsupported SQLite artifact").into());
        }
    }
    let journal = profile.join(format!("{database}-journal"));
    match safe_file(&journal) {
        Ok(_) if importing => return Err(invalid("destination has pending journal").into()),
        Ok(_) => {}
        Err(e) if e.kind() == io::ErrorKind::NotFound => {}
        Err(e) => return Err(e.into()),
    }
    let mut old_version = version_limit;
    if !new {
        let mut file = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(&path)?;
        let mut header = [0u8; 100];
        file.read_exact(&mut header)?;
        let version = u32::from_be_bytes(header[60..64].try_into()?);
        let app_id = u32::from_be_bytes(header[68..72].try_into()?);
        if &header[..16] != HEADER
            || app_id != expected_app_id
            || !(1..=version_limit).contains(&version)
            || (importing && version != version_limit)
        {
            return Err(invalid("unknown development SQLite schema").into());
        }
        if importing && (header[18] != 1 || header[19] != 1) {
            return Err(invalid("destination must use rollback mode").into());
        }
        old_version = version;
    }
    let db = Connection::open_with_flags(
        &path,
        OpenFlags::SQLITE_OPEN_READ_WRITE
            | OpenFlags::SQLITE_OPEN_NO_MUTEX
            | OpenFlags::SQLITE_OPEN_NOFOLLOW,
    )?;
    let after = safe_file(&path)?;
    if identity.dev() != after.dev() || identity.ino() != after.ino() {
        return Err(invalid("database identity changed").into());
    }
    Ok((db, new, old_version))
}
impl Store {
    pub fn development_snapshot(&self) -> rusqlite::Result<Vec<u8>> {
        crate::backup::snapshot(&self.db)
    }

    pub fn open(profile: &Path) -> Result<Self, Box<dyn std::error::Error>> {
        Self::open_inner(profile, false)
    }
    /// Open and migrate without altering interrupted run projections; caller must
    /// validate cross-store ownership before invoking recovery.
    pub fn open_deferred(profile: &Path) -> Result<Self, Box<dyn std::error::Error>> {
        let (db, new, version) = open_database(profile, DATABASE, APP_ID, VERSION, false)?;
        Self::initialize(db, new, version, true)
    }
    pub fn open_for_import(profile: &Path) -> Result<Self, Box<dyn std::error::Error>> {
        Self::open_inner(profile, true)
    }
    fn open_inner(profile: &Path, importing: bool) -> Result<Self, Box<dyn std::error::Error>> {
        let (db, new, old_version) = open_database(profile, DATABASE, APP_ID, VERSION, importing)?;
        Self::initialize(db, new, old_version, importing)
    }

    // SQLite behavior is independent of the platform-specific file admission.
    fn initialize(
        mut db: Connection,
        new: bool,
        old_version: u32,
        importing: bool,
    ) -> Result<Self, Box<dyn std::error::Error>> {
        db.busy_timeout(std::time::Duration::from_millis(250))?;
        if importing {
            db.execute_batch("PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;")?;
        } else {
            db.execute_batch(
                "PRAGMA foreign_keys=ON; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;",
            )?;
        }
        if new {
            let tx = db.transaction()?;
            tx.execute_batch(&format!("PRAGMA application_id={APP_ID}; PRAGMA user_version={VERSION};
                CREATE TABLE sessions(id TEXT PRIMARY KEY, title TEXT NOT NULL, run_id TEXT NOT NULL DEFAULT '', state TEXT NOT NULL DEFAULT 'idle', output TEXT NOT NULL DEFAULT '', model TEXT NOT NULL DEFAULT '', detail TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL DEFAULT '', updated_at TEXT NOT NULL DEFAULT '');
                CREATE TABLE runs(id TEXT PRIMARY KEY, session_id TEXT NOT NULL REFERENCES sessions(id), state TEXT NOT NULL, model TEXT NOT NULL, detail TEXT NOT NULL DEFAULT '', provider_id TEXT NOT NULL DEFAULT 'openai-compatible-local');
                CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT, session_id TEXT NOT NULL REFERENCES sessions(id), run_id TEXT NOT NULL REFERENCES runs(id), role TEXT NOT NULL, content TEXT NOT NULL, status TEXT NOT NULL, partial INTEGER NOT NULL, provider_id TEXT NOT NULL, model_id TEXT NOT NULL, timestamp TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')), logical_id INTEGER NOT NULL, reply_to INTEGER NOT NULL DEFAULT 0, replaces INTEGER NOT NULL DEFAULT 0, attachments TEXT NOT NULL DEFAULT '[]');
                CREATE INDEX messages_session ON messages(session_id,id);
                CREATE UNIQUE INDEX messages_logical ON messages(session_id,logical_id);"))?;
            tx.commit()?;
        } else {
            let check: String = db.query_row("PRAGMA quick_check", [], |r| r.get(0))?;
            if check != "ok" {
                return Err(invalid("invalid development database").into());
            }
        }
        if !new && old_version < VERSION {
            let tx = db.transaction()?;
            if old_version == 1 {
                tx.execute_batch("ALTER TABLE sessions ADD COLUMN created_at TEXT NOT NULL DEFAULT ''; ALTER TABLE sessions ADD COLUMN updated_at TEXT NOT NULL DEFAULT ''; ALTER TABLE messages ADD COLUMN logical_id INTEGER; ALTER TABLE messages ADD COLUMN reply_to INTEGER NOT NULL DEFAULT 0; ALTER TABLE messages ADD COLUMN replaces INTEGER NOT NULL DEFAULT 0; UPDATE messages SET logical_id=id; CREATE UNIQUE INDEX messages_logical ON messages(session_id,logical_id);")?;
            }
            if old_version < 3 {
                tx.execute_batch("ALTER TABLE runs ADD COLUMN provider_id TEXT NOT NULL DEFAULT 'openai-compatible-local'; UPDATE runs SET provider_id='' WHERE state='imported';")?;
            }
            tx.execute_batch("ALTER TABLE messages ADD COLUMN attachments TEXT NOT NULL DEFAULT '[]'; PRAGMA user_version=4;")?;
            tx.commit()?;
        }
        if importing {
            return Ok(Self { db });
        }
        let mut store = Self { db };
        store.recover_interrupted()?;
        Ok(store)
    }
    pub fn recover_interrupted(&mut self) -> rusqlite::Result<()> {
        let tx = self.db.transaction()?;
        tx.execute("INSERT INTO messages(session_id,run_id,role,content,status,partial,provider_id,model_id,logical_id) SELECT session_id,id,'assistant','','interrupted',1,provider_id,model,(SELECT COALESCE(MAX(logical_id),0)+1 FROM messages WHERE messages.session_id=runs.session_id) FROM runs WHERE state='running' AND NOT EXISTS(SELECT 1 FROM messages WHERE messages.run_id=runs.id AND messages.role='assistant')", [])?;
        tx.execute("UPDATE messages SET status='interrupted',partial=1,content='' WHERE role='assistant' AND status='sending' AND run_id IN (SELECT id FROM runs WHERE state='running')", [])?;
        tx.execute("UPDATE sessions SET state='failed',output='',detail='daemon-restarted' WHERE state='running'", [])?;
        tx.execute(
            "UPDATE runs SET state='failed',detail='daemon-restarted' WHERE state='running'",
            [],
        )?;
        tx.commit()?;
        Ok(())
    }
    pub fn load(&self) -> rusqlite::Result<Vec<LoadedSession>> {
        let mut stmt = self.db.prepare(
            "SELECT s.id,s.title,s.run_id,s.state,s.output,s.model,s.detail,s.created_at,s.updated_at,COALESCE(r.provider_id, '') FROM sessions s LEFT JOIN runs r ON r.id=s.run_id AND r.session_id=s.id ORDER BY s.rowid",
        )?;
        stmt.query_map([], |r| {
            Ok(LoadedSession {
                id: r.get(0)?,
                title: r.get(1)?,
                run_id: r.get(2)?,
                state: r.get(3)?,
                output: r.get(4)?,
                model: r.get(5)?,
                detail: r.get(6)?,
                created_at: r.get(7)?,
                updated_at: r.get(8)?,
                provider_id: r.get(9)?,
            })
        })?
        .collect()
    }
    pub fn create(&mut self, id: &str, title: &str) -> Result<(), WriteFailure> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(acquisition_failure)?;
        tx.execute(
            "INSERT INTO sessions(id,title) VALUES(?1,?2)",
            params![id, title],
        )
        .map_err(mutation_failure)?;
        tx.commit().map_err(mutation_failure)
    }
    pub fn rename(&mut self, sid: &str, title: &str) -> Result<(), WriteFailure> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(acquisition_failure)?;
        if tx
            .execute(
                "UPDATE sessions SET title=?2 WHERE id=?1",
                params![sid, title],
            )
            .map_err(mutation_failure)?
            != 1
        {
            return Err(WriteFailure {
                safe_conflict: false,
            });
        }
        tx.commit().map_err(mutation_failure)
    }
    /// Delete all session-owned data across the three isolated rollback-mode stores.
    /// SQLite's super-journal makes the multi-database commit atomic on this private profile.
    pub fn delete(&mut self, profile: &Path, sid: &str) -> Result<(), WriteFailure> {
        let bad = || WriteFailure {
            safe_conflict: false,
        };
        let (mut db, new, _) =
            open_database(profile, DATABASE, APP_ID, VERSION, true).map_err(|_| bad())?;
        if new {
            return Err(bad());
        }
        let mut identities = Vec::new();
        for (name, app, version) in [
            (
                crate::agent_store::DATABASE,
                crate::agent_store::APP_ID,
                crate::agent_store::VERSION,
            ),
            (
                crate::memory::DATABASE,
                crate::memory::APP_ID,
                crate::memory::VERSION,
            ),
        ] {
            let (probe, new, _) =
                open_database(profile, name, app, version, true).map_err(|_| bad())?;
            if new {
                return Err(bad());
            }
            drop(probe);
            identities.push(safe_file(&profile.join(name)).map_err(|_| bad())?);
        }
        let agent_path = profile.join(crate::agent_store::DATABASE);
        let memory_path = profile.join(crate::memory::DATABASE);
        db.execute(
            "ATTACH DATABASE ?1 AS agent",
            [agent_path.to_str().ok_or_else(bad)?],
        )
        .map_err(mutation_failure)?;
        db.execute(
            "ATTACH DATABASE ?1 AS memory",
            [memory_path.to_str().ok_or_else(bad)?],
        )
        .map_err(mutation_failure)?;
        for (index, path) in [agent_path, memory_path].iter().enumerate() {
            let now = safe_file(path).map_err(|_| bad())?;
            if now.dev() != identities[index].dev() || now.ino() != identities[index].ino() {
                return Err(bad());
            }
        }
        let agent_app: u32 = db
            .query_row("PRAGMA agent.application_id", [], |r| r.get(0))
            .map_err(mutation_failure)?;
        let agent_version: u32 = db
            .query_row("PRAGMA agent.user_version", [], |r| r.get(0))
            .map_err(mutation_failure)?;
        let memory_app: u32 = db
            .query_row("PRAGMA memory.application_id", [], |r| r.get(0))
            .map_err(mutation_failure)?;
        let memory_version: u32 = db
            .query_row("PRAGMA memory.user_version", [], |r| r.get(0))
            .map_err(mutation_failure)?;
        if (agent_app, agent_version) != (crate::agent_store::APP_ID, crate::agent_store::VERSION)
            || (memory_app, memory_version) != (crate::memory::APP_ID, crate::memory::VERSION)
        {
            return Err(bad());
        }
        db.execute_batch("PRAGMA foreign_keys=ON; PRAGMA journal_mode=DELETE; PRAGMA agent.journal_mode=DELETE; PRAGMA memory.journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA agent.synchronous=FULL; PRAGMA memory.synchronous=FULL;").map_err(mutation_failure)?;
        let tx = db
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(acquisition_failure)?;
        tx.execute("DELETE FROM agent.plan_steps WHERE run_id IN (SELECT id FROM agent.runs WHERE session_id=?1)",[sid]).map_err(mutation_failure)?;
        tx.execute("DELETE FROM agent.approvals WHERE run_id IN (SELECT id FROM agent.runs WHERE session_id=?1)",[sid]).map_err(mutation_failure)?;
        tx.execute("DELETE FROM agent.observations WHERE run_id IN (SELECT id FROM agent.runs WHERE session_id=?1)",[sid]).map_err(mutation_failure)?;
        tx.execute("DELETE FROM agent.runs WHERE session_id=?1", [sid])
            .map_err(mutation_failure)?;
        tx.execute("DELETE FROM memory.entries WHERE session=?1", [sid])
            .map_err(mutation_failure)?;
        tx.execute("DELETE FROM messages WHERE session_id=?1", [sid])
            .map_err(mutation_failure)?;
        tx.execute("DELETE FROM runs WHERE session_id=?1", [sid])
            .map_err(mutation_failure)?;
        if tx
            .execute("DELETE FROM sessions WHERE id=?1", [sid])
            .map_err(mutation_failure)?
            != 1
        {
            return Err(bad());
        }
        tx.commit().map_err(mutation_failure)
    }
    #[cfg(test)]
    pub fn begin(
        &mut self,
        sid: &str,
        rid: &str,
        text: &str,
        model: &str,
    ) -> Result<bool, WriteFailure> {
        self.begin_with_provider(sid, rid, text, model, "openai-compatible-local")
    }

    pub fn begin_with_provider(
        &mut self,
        sid: &str,
        rid: &str,
        text: &str,
        model: &str,
        provider_id: &str,
    ) -> Result<bool, WriteFailure> {
        self.begin_user(sid, rid, text, model, provider_id, "[]")
    }
    pub fn begin_with_attachments(
        &mut self,
        sid: &str,
        rid: &str,
        text: &str,
        model: &str,
        provider_id: &str,
        attachments: &str,
    ) -> Result<bool, WriteFailure> {
        self.begin_user(sid, rid, text, model, provider_id, attachments)
    }
    fn begin_user(
        &mut self,
        sid: &str,
        rid: &str,
        text: &str,
        model: &str,
        provider_id: &str,
        attachments: &str,
    ) -> Result<bool, WriteFailure> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(acquisition_failure)?;
        let count: i64 = tx
            .query_row(
                "SELECT count(*) FROM runs WHERE session_id=?1 AND state!='imported' ",
                [sid],
                |r| r.get(0),
            )
            .map_err(mutation_failure)?;
        if count >= MAX_TURNS {
            return Ok(false);
        }
        tx.execute(
            "INSERT INTO runs(id,session_id,state,model,provider_id) VALUES(?1,?2,'running',?3,?4)",
            params![rid, sid, model, provider_id],
        )
        .map_err(mutation_failure)?;
        tx.execute("INSERT INTO messages(session_id,run_id,role,content,status,partial,provider_id,model_id,logical_id,replaces,attachments) VALUES(?1,?2,'user',?3,'sent',0,'','',(SELECT COALESCE(MAX(logical_id),0)+1 FROM messages WHERE session_id=?1),0,?4)",params![sid,rid,text,attachments]).map_err(mutation_failure)?;
        let messages = read_messages(&tx, sid).map_err(mutation_failure)?;
        if !history_fits(sid, &messages) {
            return Ok(false);
        }
        if tx.execute("UPDATE sessions SET run_id=?1,state='running',output='',model=?2,detail='' WHERE id=?3",params![rid,model,sid]).map_err(mutation_failure)? != 1 { return Err(WriteFailure { safe_conflict: false }); }
        tx.commit().map_err(mutation_failure)?;
        Ok(true)
    }
    pub fn finish(
        &mut self,
        sid: &str,
        rid: &str,
        terminal: &str,
        output: &str,
        model: &str,
    ) -> rusqlite::Result<()> {
        let tx = self.db.transaction()?;
        if tx.execute(
            "UPDATE runs SET state=?1 WHERE id=?2 AND session_id=?3 AND model=?4 AND state='running'",
            params![terminal, rid, sid, model],
        )? != 1
        {
            return Err(rusqlite::Error::QueryReturnedNoRows);
        }
        if tx.execute("UPDATE messages SET content=?3,status=?4,partial=?5 WHERE session_id=?1 AND run_id=?2 AND role='assistant' AND status='sending'", params![sid,rid,output,terminal,terminal != "completed"])? == 0 {
            tx.execute("INSERT INTO messages(session_id,run_id,role,content,status,partial,provider_id,model_id,logical_id,reply_to) SELECT ?1,?2,'assistant',?3,?4,?5,provider_id,model,(SELECT COALESCE(MAX(logical_id),0)+1 FROM messages WHERE session_id=?1),COALESCE((SELECT logical_id FROM messages WHERE session_id=?1 AND run_id=?2 AND role='user' LIMIT 1),0) FROM runs WHERE id=?2 AND session_id=?1",params![sid,rid,output,terminal,terminal != "completed"])?;
        }
        if tx.execute(
            "UPDATE sessions SET state=?1,output=?2,detail=?3 WHERE id=?4 AND run_id=?5",
            params![
                terminal,
                output,
                if terminal == "failed" {
                    "provider-failed"
                } else {
                    ""
                },
                sid,
                rid
            ],
        )? != 1
        {
            return Err(rusqlite::Error::QueryReturnedNoRows);
        }
        tx.commit()?;
        Ok(())
    }
    /// Missing Chat run is valid if the process stopped before cross-store admission.
    pub fn validate_agent_owner(&self, rid: &str, sid: &str) -> rusqlite::Result<()> {
        let owner: Option<String> = self
            .db
            .query_row("SELECT session_id FROM runs WHERE id=?1", [rid], |r| {
                r.get(0)
            })
            .optional()?;
        let foreign_session: bool = self.db.query_row(
            "SELECT EXISTS(SELECT 1 FROM sessions WHERE run_id=?1 AND id!=?2)",
            params![rid, sid],
            |r| r.get(0),
        )?;
        let foreign_message: bool = self.db.query_row(
            "SELECT EXISTS(SELECT 1 FROM messages WHERE run_id=?1 AND session_id!=?2)",
            params![rid, sid],
            |r| r.get(0),
        )?;
        if owner.as_deref().is_some_and(|value| value != sid) || foreign_session || foreign_message
        {
            return Err(rusqlite::Error::InvalidQuery);
        }
        Ok(())
    }
    pub fn invalidate_agent(&mut self, rid: &str, possible_write: bool) -> rusqlite::Result<()> {
        let tx = self.db.transaction()?;
        let (output, detail) = if possible_write {
            (
                "A file may have been created or modified. Inspect the workspace before retrying.",
                "write-outcome-unknown",
            )
        } else {
            ("", "daemon-restarted")
        };
        tx.execute(
            "UPDATE sessions SET state='failed',output=?2,detail=?3 WHERE run_id=?1",
            params![rid, output, detail],
        )?;
        tx.execute("UPDATE messages SET status='interrupted',partial=1,content=?2 WHERE run_id=?1 AND role='assistant'", params![rid,output])?;
        tx.execute(
            "UPDATE runs SET state='failed',detail=?2 WHERE id=?1",
            params![rid, detail],
        )?;
        tx.commit()
    }
    /// Resolve the referenced turn and whether it precedes the latest user message.
    pub fn retry_target(
        &self,
        sid: &str,
        id: u64,
        retry: bool,
    ) -> rusqlite::Result<Option<(i64, String, i64, bool)>> {
        let id = i64::try_from(id).map_err(|_| rusqlite::Error::InvalidQuery)?;
        let latest: Option<i64> = self.db.query_row(
            "SELECT MAX(logical_id) FROM messages WHERE session_id=?1 AND role='user'",
            [sid],
            |r| r.get(0),
        )?;
        let Some(latest) = latest else {
            return Ok(None);
        };
        let user_id = if retry {
            self.db.query_row("SELECT CASE WHEN reply_to>0 THEN reply_to ELSE COALESCE((SELECT logical_id FROM messages u WHERE u.session_id=m.session_id AND u.run_id=m.run_id AND u.role='user' AND EXISTS(SELECT 1 FROM runs z WHERE z.id=m.run_id AND z.state!='imported') LIMIT 1),0) END FROM messages m WHERE session_id=?1 AND logical_id=?2 AND role='assistant' AND status IN ('failed','interrupted','error')",params![sid,id],|r|r.get::<_,i64>(0)).optional()?
        } else {
            Some(id)
        };
        let Some(user_id) = user_id.filter(|id| *id > 0 && *id <= latest) else {
            return Ok(None);
        };
        let text:Option<String>=self.db.query_row("SELECT content FROM messages WHERE session_id=?1 AND logical_id=?2 AND role='user' AND partial=0 AND status IN ('sent','received','completed')",params![sid,user_id],|r|r.get(0)).optional()?;
        let Some(text) = text else {
            return Ok(None);
        };
        let replaced = if retry {
            id
        } else {
            self.db.query_row("SELECT COALESCE(MAX(m.logical_id),0) FROM messages m WHERE m.session_id=?1 AND m.role='assistant' AND (m.reply_to=?2 OR (m.reply_to=0 AND EXISTS(SELECT 1 FROM runs z WHERE z.id=m.run_id AND z.state!='imported') AND EXISTS(SELECT 1 FROM messages u WHERE u.session_id=m.session_id AND u.run_id=m.run_id AND u.role='user' AND u.logical_id=?2)))",params![sid,user_id],|r|r.get(0))?
        };
        Ok(Some((user_id, text, replaced, user_id != latest)))
    }
    pub fn user_context(&self, sid: &str, user_id: i64, text: &str) -> rusqlite::Result<String> {
        let attachments: String = self.db.query_row("SELECT attachments FROM messages WHERE session_id=?1 AND logical_id=?2 AND role='user'", params![sid,user_id], |r| r.get(0))?;
        crate::attachment::context(text, &attachments).map_err(|_| rusqlite::Error::InvalidQuery)
    }
    /// Atomically copy a transcript prefix and admit a new branch run.
    pub fn begin_branch(&mut self, request: BranchRequest<'_>) -> Result<bool, WriteFailure> {
        let BranchRequest {
            source,
            branch,
            run,
            title,
            user_id,
            text,
            model,
            provider,
        } = request;
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(acquisition_failure)?;
        let valid: bool = tx.query_row("SELECT EXISTS(SELECT 1 FROM messages WHERE session_id=?1 AND logical_id=?2 AND role='user' AND partial=0 AND status IN ('sent','received','completed'))",params![source,user_id],|r|r.get(0)).map_err(mutation_failure)?;
        if !valid {
            return Ok(false);
        }
        let count: i64 = tx
            .query_row(
                "SELECT COUNT(*) FROM messages WHERE session_id=?1 AND logical_id<=?2",
                params![source, user_id],
                |r| r.get(0),
            )
            .map_err(mutation_failure)?;
        if count < 1 || count as usize >= MAX_CONTEXT_ROWS {
            return Ok(false);
        }
        let copied_run = format!("{run}-branch-history");
        tx.execute(
            "INSERT INTO sessions(id,title,run_id,state,model) VALUES(?1,?2,?3,'running',?4)",
            params![branch, title, run, model],
        )
        .map_err(mutation_failure)?;
        tx.execute("INSERT INTO runs(id,session_id,state,model,provider_id) VALUES(?1,?2,'imported','','')",params![copied_run,branch]).map_err(mutation_failure)?;
        tx.execute("INSERT INTO messages(session_id,run_id,role,content,status,partial,provider_id,model_id,timestamp,logical_id,reply_to,replaces,attachments) SELECT ?1,?2,m.role,CASE WHEN m.logical_id=?4 THEN ?5 ELSE m.content END,CASE WHEN m.logical_id=?4 THEN 'completed' ELSE m.status END,m.partial,m.provider_id,m.model_id,m.timestamp,m.logical_id,CASE WHEN m.role='assistant' AND m.reply_to=0 THEN COALESCE((SELECT u.logical_id FROM messages u WHERE u.session_id=m.session_id AND u.run_id=m.run_id AND u.role='user' AND u.logical_id<m.logical_id LIMIT 1),0) ELSE m.reply_to END,m.replaces,m.attachments FROM messages m WHERE m.session_id=?3 AND m.logical_id<=?4 ORDER BY m.logical_id",params![branch,copied_run,source,user_id,text]).map_err(mutation_failure)?;
        tx.execute(
            "INSERT INTO runs(id,session_id,state,model,provider_id) VALUES(?1,?2,'running',?3,?4)",
            params![run, branch, model, provider],
        )
        .map_err(mutation_failure)?;
        tx.execute("INSERT INTO messages(session_id,run_id,role,content,status,partial,provider_id,model_id,logical_id,reply_to,replaces) VALUES(?1,?2,'assistant','','sending',0,?3,?4,(SELECT COALESCE(MAX(logical_id),0)+1 FROM messages WHERE session_id=?1),?5,0)",params![branch,run,provider,model,user_id]).map_err(mutation_failure)?;
        let messages = read_messages(&tx, branch).map_err(mutation_failure)?;
        if !history_fits(branch, &messages) {
            return Ok(false);
        }
        tx.commit().map_err(mutation_failure)?;
        Ok(true)
    }
    pub fn begin_existing(
        &mut self,
        sid: &str,
        rid: &str,
        user_id: i64,
        replaces: i64,
        model: &str,
        provider_id: &str,
    ) -> Result<bool, WriteFailure> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(acquisition_failure)?;
        let count: i64 = tx
            .query_row(
                "SELECT count(*) FROM runs WHERE session_id=?1 AND state!='imported'",
                [sid],
                |r| r.get(0),
            )
            .map_err(mutation_failure)?;
        if count >= MAX_TURNS {
            return Ok(false);
        }
        let exists:bool=tx.query_row("SELECT EXISTS(SELECT 1 FROM messages WHERE session_id=?1 AND logical_id=?2 AND role='user' AND partial=0)",params![sid,user_id],|r|r.get(0)).map_err(mutation_failure)?;
        if !exists {
            return Err(WriteFailure {
                safe_conflict: false,
            });
        }
        tx.execute(
            "INSERT INTO runs(id,session_id,state,model,provider_id) VALUES(?1,?2,'running',?3,?4)",
            params![rid, sid, model, provider_id],
        )
        .map_err(mutation_failure)?;
        tx.execute("INSERT INTO messages(session_id,run_id,role,content,status,partial,provider_id,model_id,logical_id,reply_to,replaces) VALUES(?1,?2,'assistant','','sending',0,?3,?4,(SELECT COALESCE(MAX(logical_id),0)+1 FROM messages WHERE session_id=?1),?5,?6)",params![sid,rid,provider_id,model,user_id,replaces]).map_err(mutation_failure)?;
        if tx.execute("UPDATE sessions SET run_id=?1,state='running',output='',model=?2,detail='' WHERE id=?3",params![rid,model,sid]).map_err(mutation_failure)? != 1 { return Err(WriteFailure{safe_conflict:false}); }
        tx.commit().map_err(mutation_failure)?;
        Ok(true)
    }
    /// Conservative verified-turn projection for one session. System rows are
    /// never promoted to provider instructions; imported pairs require Qt reply IDs.
    pub fn context_turns(&self, sid: &str) -> rusqlite::Result<Vec<crate::provider::Turn>> {
        self.context_turns_before(sid, None)
    }
    pub fn context_turns_before(
        &self,
        sid: &str,
        before: Option<i64>,
    ) -> rusqlite::Result<Vec<crate::provider::Turn>> {
        struct Row {
            id: i64,
            run: String,
            role: String,
            content: String,
            status: String,
            partial: bool,
            reply: i64,
            replaces: i64,
            run_state: String,
            attachments: String,
        }
        let mut stmt = self.db.prepare("SELECT m.logical_id,m.run_id,m.role,m.content,m.status,m.partial,m.reply_to,m.replaces,r.state,m.attachments FROM messages m JOIN runs r ON r.id=m.run_id AND r.session_id=m.session_id WHERE m.session_id=?1 ORDER BY m.logical_id LIMIT ?2")?;
        let rows: Vec<Row> = stmt
            .query_map(params![sid, (MAX_CONTEXT_ROWS + 1) as i64], |r| {
                Ok(Row {
                    id: r.get(0)?,
                    run: r.get(1)?,
                    role: r.get(2)?,
                    content: r.get(3)?,
                    status: r.get(4)?,
                    partial: r.get(5)?,
                    reply: r.get(6)?,
                    replaces: r.get(7)?,
                    run_state: r.get(8)?,
                    attachments: r.get(9)?,
                })
            })?
            .collect::<rusqlite::Result<_>>()?;
        if rows.len() > MAX_CONTEXT_ROWS {
            return Err(rusqlite::Error::InvalidQuery);
        }
        let valid_user = |r: &Row| {
            r.role == "user"
                && !r.partial
                && ["sent", "received", "completed"].contains(&r.status.as_str())
                && !r.content.trim().is_empty()
        };
        let valid_answer = |r: &Row| {
            r.role == "assistant"
                && !r.partial
                && ["received", "completed"].contains(&r.status.as_str())
                && !r.content.trim().is_empty()
        };
        let mut chosen = std::collections::BTreeMap::<i64, (&Row, &Row)>::new();
        for answer in rows.iter().filter(|r| valid_answer(r)) {
            let user = if answer.run_state == "imported" {
                if answer.reply <= 0 {
                    None
                } else {
                    rows.iter().find(|r| {
                        r.id == answer.reply
                            && r.run == answer.run
                            && r.id < answer.id
                            && valid_user(r)
                    })
                }
            } else if answer.run_state == "completed" && answer.reply > 0 {
                rows.iter()
                    .find(|r| r.id == answer.reply && r.id < answer.id && valid_user(r))
            } else if answer.run_state == "completed" {
                let mut users = rows
                    .iter()
                    .filter(|r| r.run == answer.run && r.id < answer.id && valid_user(r));
                match (users.next(), users.next()) {
                    (Some(user), None) => Some(user),
                    _ => None,
                }
            } else {
                None
            };
            let Some(user) = user else {
                continue;
            };
            if answer.reply != 0 && answer.reply != user.id {
                continue;
            }
            if answer.replaces != 0
                && !rows.iter().any(|r| {
                    r.id == answer.replaces
                        && r.role == "assistant"
                        && r.id < answer.id
                        && (r.reply == user.id
                            || (r.reply == 0
                                && r.run_state != "imported"
                                && rows.iter().any(|u| u.id == user.id && u.run == r.run)))
                })
            {
                continue;
            }
            if before.is_none_or(|limit| user.id < limit && answer.id < limit) {
                chosen.insert(user.id, (user, answer));
            }
        }
        let mut superseded = std::collections::HashSet::new();
        let mut mark_ancestors = |mut id: i64| {
            while let Some(current) = rows.iter().find(|r| r.id == id && r.role == "user") {
                let Some(previous) = rows
                    .iter()
                    .find(|r| r.id == current.replaces && r.role == "user" && r.id < current.id)
                else {
                    break;
                };
                superseded.insert(previous.id);
                id = previous.id;
            }
        };
        for (user, _) in chosen.values() {
            mark_ancestors(user.id);
        }
        if let Some(target) = before {
            mark_ancestors(target);
        }
        chosen.retain(|id, _| !superseded.contains(id));
        chosen
            .into_values()
            .map(|(user, answer)| {
                Ok(crate::provider::Turn {
                    user: crate::attachment::context(&user.content, &user.attachments)
                        .map_err(|_| rusqlite::Error::InvalidQuery)?,
                    assistant: answer.content.clone(),
                })
            })
            .collect()
    }
    pub fn import_snapshot(&mut self, snapshot: &crate::import::Snapshot) -> rusqlite::Result<()> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let existing: i64 = tx.query_row("SELECT (SELECT count(*) FROM sessions)+(SELECT count(*) FROM runs)+(SELECT count(*) FROM messages)", [], |r| r.get(0))?;
        if existing != 0 {
            return Err(rusqlite::Error::InvalidQuery);
        }
        for conversation in &snapshot.conversations {
            tx.execute(
                "INSERT INTO sessions(id,title,created_at,updated_at) VALUES(?1,?2,?3,?4)",
                params![
                    conversation.id,
                    conversation.title,
                    conversation.created_at,
                    conversation.updated_at
                ],
            )?;
            if conversation.messages.is_empty() {
                continue;
            }
            let run = format!("qt-import:{}", conversation.id);
            tx.execute(
                "INSERT INTO runs(id,session_id,state,model,provider_id) VALUES(?1,?2,'imported','','')",
                params![run, conversation.id],
            )?;
            for message in &conversation.messages {
                tx.execute("INSERT INTO messages(session_id,run_id,role,content,status,partial,provider_id,model_id,timestamp,logical_id,reply_to,replaces) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)", params![conversation.id,run,message.role,message.content,message.status,message.partial,message.provider,message.model,message.timestamp,message.id,message.reply_to,message.replaces])?;
            }
            let messages = read_messages(&tx, &conversation.id)?;
            if !history_fits(&conversation.id, &messages) {
                return Err(rusqlite::Error::InvalidQuery);
            }
        }
        snapshot
            .validate_unchanged()
            .map_err(|_| rusqlite::Error::InvalidQuery)?;
        tx.commit()
    }
    pub fn messages(&self, sid: &str) -> rusqlite::Result<Vec<Value>> {
        read_messages(&self.db, sid)
    }
}

pub fn history_fits(sid: &str, messages: &[Value]) -> bool {
    serde_json::to_vec(&json!({"session_id":sid,"messages":messages}))
        .is_ok_and(|bytes| bytes.len() + 2048 <= sentinel_ipc::contract::MAX_FRAME_BYTES)
}
fn read_messages(db: &Connection, sid: &str) -> rusqlite::Result<Vec<Value>> {
    let mut stmt = db.prepare("SELECT logical_id,role,content,status,timestamp,partial,provider_id,model_id,reply_to,replaces,attachments FROM messages WHERE session_id=?1 ORDER BY logical_id")?;
    stmt.query_map([sid], |r| Ok(json!({"id":r.get::<_,i64>(0)?,"role":r.get::<_,String>(1)?,"content":r.get::<_,String>(2)?,"status":r.get::<_,String>(3)?,"timestamp":r.get::<_,String>(4)?,"partial":r.get::<_,bool>(5)?,"provider_id":r.get::<_,String>(6)?,"model_id":r.get::<_,String>(7)?,"reply_to":r.get::<_,i64>(8)?,"replaces":r.get::<_,i64>(9)?,"attachments":serde_json::from_str::<Value>(&r.get::<_,String>(10)?).map_err(|_| rusqlite::Error::InvalidQuery)?})))?.collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        os::unix::fs::{DirBuilderExt, FileExt, symlink},
        sync::atomic::{AtomicUsize, Ordering},
    };
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    struct Temp(std::path::PathBuf);
    impl Temp {
        fn new() -> Self {
            let path = std::env::temp_dir().join(format!(
                "sentinel-store-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            fs::DirBuilder::new().mode(0o700).create(&path).unwrap();
            Self(path.canonicalize().unwrap())
        }
    }
    impl Drop for Temp {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn in_memory_logical_store_preserves_binding_and_order() {
        let db = Connection::open_in_memory().unwrap();
        let mut store = Store::initialize(db, true, VERSION, false).unwrap();
        store.create("session", "Fixture").unwrap();
        assert!(
            store
                .begin_with_provider("session", "run", "prompt", "model", "ollama")
                .unwrap()
        );
        store
            .finish("session", "run", "completed", "answer", "model")
            .unwrap();
        let sessions = store.load().unwrap();
        let messages = store.messages("session").unwrap();
        assert_eq!(sessions[0].provider_id, "ollama");
        assert_eq!(messages.len(), 2);
        assert_eq!(messages[0]["content"], "prompt");
        assert_eq!(messages[1]["content"], "answer");
        assert_eq!(messages[1]["provider_id"], "ollama");
    }

    #[test]
    fn attachments_survive_restart_and_are_used_in_context() {
        let temp = Temp::new();
        let mut store = Store::open(&temp.0).unwrap();
        store.create("session", "title").unwrap();
        let attachments =
            json!([{"name":"note.txt","mime":"text/plain","text":"private fixture","image":""}])
                .to_string();
        assert!(
            store
                .begin_with_attachments(
                    "session",
                    "run",
                    "question",
                    "model",
                    "ollama",
                    &attachments
                )
                .unwrap()
        );
        store
            .finish("session", "run", "completed", "answer", "model")
            .unwrap();
        drop(store);
        let store = Store::open(&temp.0).unwrap();
        assert_eq!(
            store.messages("session").unwrap()[0]["attachments"][0]["text"],
            "private fixture"
        );
        let turns = store.context_turns("session").unwrap();
        assert!(turns[0].user.contains("private fixture"));
        assert!(
            store
                .user_context("session", 1, "edited")
                .unwrap()
                .contains("private fixture")
        );
    }

    #[test]
    fn terminal_transaction_rolls_back_on_wrong_owner() {
        let temp = Temp::new();
        let mut store = Store::open(&temp.0).unwrap();
        store.create("session", "title").unwrap();
        assert!(store.begin("session", "run", "prompt", "model").unwrap());
        assert!(
            store
                .finish("wrong-session", "run", "completed", "answer", "model")
                .is_err()
        );
        let state: String = store
            .db
            .query_row("SELECT state FROM runs WHERE id='run'", [], |r| r.get(0))
            .unwrap();
        assert_eq!(state, "running");
        let messages = store.messages("session").unwrap();
        assert_eq!(messages.len(), 1);
        assert_eq!(messages[0]["content"], "prompt");
        store
            .finish("session", "run", "completed", "answer", "model")
            .unwrap();
        assert_eq!(store.messages("session").unwrap().len(), 2);
    }

    #[test]
    fn import_open_never_creates_missing_destination() {
        let temp = Temp::new();
        assert!(Store::open_for_import(&temp.0).is_err());
        assert!(!temp.0.join(DATABASE).exists());
    }

    #[test]
    fn history_bound_counts_json_escaping() {
        assert!(history_fits("s", &[json!({"content":"small"})]));
        assert!(!history_fits(
            "s",
            &[json!({"content":"\u{0}".repeat(50000)})]
        ));
    }

    #[test]
    fn v1_migration_preserves_message_ids_and_next_native_id() {
        let temp = Temp::new();
        let path = temp.0.join(DATABASE);
        OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&path)
            .unwrap();
        let db = Connection::open(&path).unwrap();
        db.execute_batch(&format!("PRAGMA application_id={APP_ID}; PRAGMA user_version=1;
          CREATE TABLE sessions(id TEXT PRIMARY KEY,title TEXT NOT NULL,run_id TEXT NOT NULL DEFAULT '',state TEXT NOT NULL DEFAULT 'idle',output TEXT NOT NULL DEFAULT '',model TEXT NOT NULL DEFAULT '',detail TEXT NOT NULL DEFAULT '');
          CREATE TABLE runs(id TEXT PRIMARY KEY,session_id TEXT NOT NULL REFERENCES sessions(id),state TEXT NOT NULL,model TEXT NOT NULL,detail TEXT NOT NULL DEFAULT '');
          CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL REFERENCES sessions(id),run_id TEXT NOT NULL REFERENCES runs(id),role TEXT NOT NULL,content TEXT NOT NULL,status TEXT NOT NULL,partial INTEGER NOT NULL,provider_id TEXT NOT NULL,model_id TEXT NOT NULL,timestamp TEXT NOT NULL DEFAULT '');
          CREATE INDEX messages_session ON messages(session_id,id);
          INSERT INTO sessions(id,title,run_id,state,output,model) VALUES('s','title','r','completed','answer','model');
          INSERT INTO runs(id,session_id,state,model) VALUES('r','s','completed','model');
          INSERT INTO messages(id,session_id,run_id,role,content,status,partial,provider_id,model_id,timestamp) VALUES(42,'s','r','user','prompt','sent',0,'','','time');
          INSERT INTO messages(id,session_id,run_id,role,content,status,partial,provider_id,model_id,timestamp) VALUES(99,'s','r','assistant','answer','completed',0,'p','model','time');" )).unwrap();
        drop(db);
        assert!(Store::open_for_import(&temp.0).is_err());
        let mut store = Store::open(&temp.0).unwrap();
        let messages = store.messages("s").unwrap();
        assert_eq!(messages[0]["id"], 42);
        assert_eq!(messages[1]["id"], 99);
        assert!(store.begin("s", "new", "again", "model").unwrap());
        store
            .finish("s", "new", "completed", "done", "model")
            .unwrap();
        let messages = store.messages("s").unwrap();
        assert_eq!(messages[2]["id"], 100);
        assert_eq!(messages[3]["id"], 101);
        let version: i64 = store
            .db
            .query_row("PRAGMA user_version", [], |r| r.get(0))
            .unwrap();
        assert_eq!(version, 4);
        assert_eq!(
            store.load().unwrap()[0].provider_id,
            "openai-compatible-local"
        );
    }

    #[test]
    fn v2_upgrade_preserves_legacy_and_imported_run_identities() {
        let temp = Temp::new();
        let path = temp.0.join(DATABASE);
        OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&path)
            .unwrap();
        let db = Connection::open(&path).unwrap();
        db.execute_batch(&format!("PRAGMA application_id={APP_ID}; PRAGMA user_version=2;
            CREATE TABLE sessions(id TEXT PRIMARY KEY,title TEXT NOT NULL,run_id TEXT NOT NULL DEFAULT '',state TEXT NOT NULL DEFAULT 'idle',output TEXT NOT NULL DEFAULT '',model TEXT NOT NULL DEFAULT '',detail TEXT NOT NULL DEFAULT '',created_at TEXT NOT NULL DEFAULT '',updated_at TEXT NOT NULL DEFAULT '');
            CREATE TABLE runs(id TEXT PRIMARY KEY,session_id TEXT NOT NULL,state TEXT NOT NULL,model TEXT NOT NULL,detail TEXT NOT NULL DEFAULT '');
            CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL,run_id TEXT NOT NULL,role TEXT NOT NULL,content TEXT NOT NULL,status TEXT NOT NULL,partial INTEGER NOT NULL,provider_id TEXT NOT NULL,model_id TEXT NOT NULL,timestamp TEXT NOT NULL DEFAULT '',logical_id INTEGER NOT NULL,reply_to INTEGER NOT NULL DEFAULT 0,replaces INTEGER NOT NULL DEFAULT 0);
            CREATE UNIQUE INDEX messages_logical ON messages(session_id,logical_id);
            INSERT INTO sessions(id,title,run_id,state,model) VALUES('legacy','Legacy','active','running','legacy-model');
            INSERT INTO sessions(id,title) VALUES('copy','Imported');
            INSERT INTO runs(id,session_id,state,model) VALUES('active','legacy','running','legacy-model');
            INSERT INTO runs(id,session_id,state,model) VALUES('qt-import:copy','copy','imported','');")).unwrap();
        drop(db);
        assert!(Store::open_for_import(&temp.0).is_err());
        let store = Store::open(&temp.0).unwrap();
        let sessions = store.load().unwrap();
        assert_eq!(sessions[0].provider_id, "openai-compatible-local");
        assert_eq!(sessions[0].state, "failed");
        assert!(sessions[1].provider_id.is_empty());
        let copied: String = store
            .db
            .query_row(
                "SELECT provider_id FROM runs WHERE id='qt-import:copy'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert!(copied.is_empty());
        let recovered = store.messages("legacy").unwrap();
        assert_eq!(recovered[0]["status"], "interrupted");
        assert_eq!(recovered[0]["provider_id"], "openai-compatible-local");
        let version: i64 = store
            .db
            .query_row("PRAGMA user_version", [], |r| r.get(0))
            .unwrap();
        assert_eq!(version, 4);
    }

    #[test]
    fn failed_v1_upgrade_rolls_back_all_schema_changes() {
        let temp = Temp::new();
        let path = temp.0.join(DATABASE);
        OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&path)
            .unwrap();
        let db = Connection::open(&path).unwrap();
        db.execute_batch(&format!("PRAGMA application_id={APP_ID}; PRAGMA user_version=1;
            CREATE TABLE sessions(id TEXT PRIMARY KEY,title TEXT NOT NULL,run_id TEXT NOT NULL DEFAULT '',state TEXT NOT NULL DEFAULT 'idle',output TEXT NOT NULL DEFAULT '',model TEXT NOT NULL DEFAULT '',detail TEXT NOT NULL DEFAULT '');
            CREATE TABLE runs(id TEXT PRIMARY KEY,session_id TEXT NOT NULL,state TEXT NOT NULL,model TEXT NOT NULL,detail TEXT NOT NULL DEFAULT '',provider_id TEXT NOT NULL DEFAULT 'already-exists');
            CREATE TABLE messages(id INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL,run_id TEXT NOT NULL,role TEXT NOT NULL,content TEXT NOT NULL,status TEXT NOT NULL,partial INTEGER NOT NULL,provider_id TEXT NOT NULL,model_id TEXT NOT NULL,timestamp TEXT NOT NULL DEFAULT '');")).unwrap();
        drop(db);
        assert!(Store::open(&temp.0).is_err());
        let db = Connection::open(&path).unwrap();
        let version: i64 = db
            .query_row("PRAGMA user_version", [], |r| r.get(0))
            .unwrap();
        assert_eq!(version, 1);
        let added_columns: i64 = db
            .query_row(
                "SELECT count(*) FROM pragma_table_info('messages') WHERE name='logical_id'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(added_columns, 0);
        let added_session_columns: i64 = db.query_row("SELECT count(*) FROM pragma_table_info('sessions') WHERE name IN ('created_at','updated_at')", [], |r| r.get(0)).unwrap();
        assert_eq!(added_session_columns, 0);
    }

    #[test]
    fn frozen_provider_survives_terminal_and_interrupted_restart() {
        let temp = Temp::new();
        {
            let mut store = Store::open(&temp.0).unwrap();
            store.create("done", "Done").unwrap();
            assert!(
                store
                    .begin_with_provider("done", "r1", "prompt", "model-a", "ollama")
                    .unwrap()
            );
            assert!(
                store
                    .finish("done", "r1", "completed", "answer", "wrong-model")
                    .is_err()
            );
            store
                .finish("done", "r1", "completed", "answer", "model-a")
                .unwrap();
            store.create("pending", "Pending").unwrap();
            assert!(
                store
                    .begin_with_provider("pending", "r2", "prompt", "model-b", "llama-cpp-server")
                    .unwrap()
            );
        }
        let store = Store::open(&temp.0).unwrap();
        let sessions = store.load().unwrap();
        assert_eq!(sessions[0].provider_id, "ollama");
        assert_eq!(sessions[1].provider_id, "llama-cpp-server");
        assert_eq!(sessions[1].state, "failed");
        let done = store.messages("done").unwrap();
        assert_eq!(done[1]["provider_id"], "ollama");
        assert_eq!(done[1]["model_id"], "model-a");
        let pending = store.messages("pending").unwrap();
        assert_eq!(pending[1]["provider_id"], "llama-cpp-server");
        assert_eq!(pending[1]["model_id"], "model-b");
        assert_eq!(pending[1]["status"], "interrupted");
    }

    #[test]
    fn busy_before_transaction_can_be_retried_manually() {
        let temp = Temp::new();
        let mut store = Store::open(&temp.0).unwrap();
        store.create("session", "title").unwrap();
        let other = Connection::open(temp.0.join(DATABASE)).unwrap();
        other.execute_batch("BEGIN IMMEDIATE").unwrap();
        let error = store
            .begin("session", "first", "prompt", "model")
            .unwrap_err();
        assert!(error.safe_conflict);
        assert!(store.messages("session").unwrap().is_empty());
        other.execute_batch("ROLLBACK").unwrap();
        assert!(store.begin("session", "second", "prompt", "model").unwrap());
        assert_eq!(store.messages("session").unwrap().len(), 1);
    }

    #[test]
    fn newer_schema_is_refused_without_mutation() {
        let temp = Temp::new();
        drop(Store::open(&temp.0).unwrap());
        let path = temp.0.join(DATABASE);
        let file = OpenOptions::new().write(true).open(&path).unwrap();
        file.write_all_at(&(VERSION + 1).to_be_bytes(), 60).unwrap();
        drop(file);
        let before = fs::read(&path).unwrap();
        assert!(Store::open(&temp.0).is_err());
        assert_eq!(fs::read(path).unwrap(), before);
    }

    #[test]
    fn empty_symlink_and_hardlinked_databases_are_refused() {
        let temp = Temp::new();
        let path = temp.0.join(DATABASE);
        fs::write(&path, []).unwrap();
        fs::set_permissions(&path, std::os::unix::fs::PermissionsExt::from_mode(0o600)).unwrap();
        assert!(Store::open(&temp.0).is_err());
        fs::remove_file(&path).unwrap();
        let target = temp.0.join("target");
        fs::write(&target, b"do not touch").unwrap();
        symlink(&target, &path).unwrap();
        assert!(Store::open(&temp.0).is_err());
        assert_eq!(fs::read(&target).unwrap(), b"do not touch");
        fs::remove_file(&path).unwrap();
        fs::hard_link(&target, &path).unwrap();
        assert!(Store::open(&temp.0).is_err());
        assert_eq!(fs::read(target).unwrap(), b"do not touch");
    }
}
