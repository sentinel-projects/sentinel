//! Import a finalized, quiescent, disposable Qt conversation database copy.
use rusqlite::{Connection, OpenFlags};
use serde_json::Value;
use std::{
    collections::{HashMap, HashSet},
    fs::{self, OpenOptions},
    io::{self, Read},
    os::unix::fs::{MetadataExt, OpenOptionsExt},
    path::{Path, PathBuf},
};

use crate::store::MAX_IMPORTED_MESSAGES_PER_SESSION;

pub struct Snapshot {
    pub conversations: Vec<Conversation>,
    _source_db: Connection,
    source: PathBuf,
    identity: fs::Metadata,
}
impl Snapshot {
    pub fn validate_unchanged(&self) -> io::Result<()> {
        no_sidecars(&self.source)?;
        let current = fs::symlink_metadata(&self.source)?;
        if current.dev() != self.identity.dev()
            || current.ino() != self.identity.ino()
            || current.len() != self.identity.len()
            || current.mtime() != self.identity.mtime()
            || current.mtime_nsec() != self.identity.mtime_nsec()
        {
            return Err(invalid("source changed during import"));
        }
        Ok(())
    }
}
pub struct Conversation {
    pub id: String,
    pub title: String,
    pub created_at: String,
    pub updated_at: String,
    pub messages: Vec<Message>,
}
pub struct Message {
    pub id: i64,
    pub role: String,
    pub content: String,
    pub timestamp: String,
    pub status: String,
    pub provider: String,
    pub model: String,
    pub partial: i64,
    pub reply_to: i64,
    pub replaces: i64,
}
fn invalid(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}
pub(crate) fn no_sidecars(path: &Path) -> io::Result<()> {
    let name = path
        .file_name()
        .ok_or_else(|| invalid("source filename"))?
        .to_str()
        .ok_or_else(|| invalid("source filename must be UTF-8"))?;
    for suffix in ["-wal", "-shm", "-journal"] {
        match fs::symlink_metadata(path.with_file_name(format!("{name}{suffix}"))) {
            Err(e) if e.kind() == io::ErrorKind::NotFound => {}
            _ => return Err(invalid("source has SQLite sidecar")),
        }
    }
    Ok(())
}
pub(crate) fn uuid(id: &str) -> bool {
    id.len() == 36
        && id.bytes().enumerate().all(|(i, b)| {
            if [8, 13, 18, 23].contains(&i) {
                b == b'-'
            } else {
                b.is_ascii_hexdigit()
            }
        })
}
pub(crate) fn timestamp(value: &str) -> bool {
    // Qt UTC ISODateWithMs, no unsupported offset or incomplete value.
    let b = value.as_bytes();
    if b.len() != 24
        || b[4] != b'-'
        || b[7] != b'-'
        || b[10] != b'T'
        || b[13] != b':'
        || b[16] != b':'
        || b[19] != b'.'
        || b[23] != b'Z'
        || !b
            .iter()
            .enumerate()
            .all(|(i, c)| [4, 7, 10, 13, 16, 19, 23].contains(&i) || c.is_ascii_digit())
    {
        return false;
    }
    let number = |start: usize, end: usize| -> u32 {
        b[start..end]
            .iter()
            .fold(0, |n, c| n * 10 + u32::from(c - b'0'))
    };
    let (year, month, day, hour, minute, second) = (
        number(0, 4),
        number(5, 7),
        number(8, 10),
        number(11, 13),
        number(14, 16),
        number(17, 19),
    );
    let leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    let days = match month {
        1 | 3 | 5 | 7 | 8 | 10 | 12 => 31,
        4 | 6 | 9 | 11 => 30,
        2 if leap => 29,
        2 => 28,
        _ => 0,
    };
    year > 0 && day > 0 && day <= days && hour < 24 && minute < 60 && second < 60
}
fn check_columns(
    db: &Connection,
    table: &str,
    expected: &[&str],
) -> Result<(), Box<dyn std::error::Error>> {
    let mut stmt = db.prepare(&format!("PRAGMA table_info({table})"))?;
    let mut names: Vec<String> = stmt
        .query_map([], |r| r.get(1))?
        .collect::<Result<_, _>>()?;
    names.sort();
    let mut wanted: Vec<String> = expected.iter().map(|s| s.to_string()).collect();
    wanted.sort();
    if names != wanted {
        return Err(invalid("unsupported source columns").into());
    }
    Ok(())
}
pub(crate) fn checked_source(
    path: &Path,
    destination: &Path,
) -> Result<(PathBuf, fs::Metadata), Box<dyn std::error::Error>> {
    if !path.is_absolute() {
        return Err(invalid("source must be absolute").into());
    }
    let parent = path.parent().ok_or_else(|| invalid("source directory"))?;
    let meta = fs::symlink_metadata(parent)?;
    let tmp = Path::new("/tmp").canonicalize()?;
    let canonical_parent = parent.canonicalize()?;
    if !meta.file_type().is_dir()
        || meta.uid() != unsafe { libc::geteuid() }
        || meta.mode() & 0o7777 != 0o700
        || canonical_parent.parent() != Some(tmp.as_path())
        || canonical_parent == destination
    {
        return Err(invalid("source must be separate private /tmp directory").into());
    }
    let actual = canonical_parent.join(path.file_name().ok_or_else(|| invalid("source filename"))?);
    let metadata = fs::symlink_metadata(&actual)?;
    if !metadata.file_type().is_file()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.nlink() != 1
        || !matches!(metadata.mode() & 0o7777, 0o400 | 0o600)
        || metadata.len() > 128 * 1024 * 1024
    {
        return Err(invalid("unsafe source copy").into());
    }
    no_sidecars(&actual)?;
    let mut file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
        .open(&actual)?;
    let identity = file.metadata()?;
    if identity.dev() != metadata.dev() || identity.ino() != metadata.ino() {
        return Err(invalid("source identity changed").into());
    }
    let mut header = [0u8; 100];
    file.read_exact(&mut header)?;
    if &header[..16] != b"SQLite format 3\0" || header[18] != 1 || header[19] != 1 {
        return Err(invalid("source must be finalized rollback-mode SQLite copy").into());
    }
    Ok((actual, metadata))
}
/// Source quiescence is an explicit caller precondition; a read transaction pins a snapshot.
pub fn read(path: &Path, destination: &Path) -> Result<Snapshot, Box<dyn std::error::Error>> {
    let (source, identity) = checked_source(path, destination)?;
    let db = Connection::open_with_flags(
        &source,
        OpenFlags::SQLITE_OPEN_READ_ONLY
            | OpenFlags::SQLITE_OPEN_NO_MUTEX
            | OpenFlags::SQLITE_OPEN_NOFOLLOW,
    )?;
    db.busy_timeout(std::time::Duration::from_millis(250))?;
    db.execute_batch("PRAGMA query_only=ON; PRAGMA trusted_schema=OFF; BEGIN;")?;
    let check: String = db.query_row("PRAGMA quick_check", [], |r| r.get(0))?;
    if check != "ok" {
        return Err(invalid("invalid source database").into());
    }
    let mut tables=db.prepare("SELECT name,type FROM sqlite_master WHERE type IN ('table','view','trigger') ORDER BY name")?;
    let types: Vec<(String, String)> = tables
        .query_map([], |r| Ok((r.get(0)?, r.get(1)?)))?
        .collect::<Result<_, _>>()?;
    let wanted = [
        "conversation_messages",
        "conversation_schema_metadata",
        "conversation_summary_metadata",
        "conversations",
    ];
    if types.iter().any(|(name, kind)| {
        kind != "table" || (!wanted.contains(&name.as_str()) && name != "sqlite_sequence")
    }) || wanted
        .iter()
        .any(|name| !types.iter().any(|(n, t)| n == name && t == "table"))
    {
        return Err(invalid("unsupported source schema").into());
    }
    drop(tables);
    check_columns(
        &db,
        "conversations",
        &[
            "id",
            "title",
            "created_at",
            "updated_at",
            "archived",
            "pinned",
            "deleted",
            "user_renamed",
        ],
    )?;
    check_columns(
        &db,
        "conversation_messages",
        &[
            "conversation_id",
            "message_id",
            "role",
            "content",
            "timestamp",
            "status",
            "provider_id",
            "model_id",
            "reply_to_id",
            "replaces_id",
            "partial",
            "error_category",
            "attachments_json",
        ],
    )?;
    check_columns(&db, "conversation_schema_metadata", &["key", "value"])?;
    check_columns(
        &db,
        "conversation_summary_metadata",
        &[
            "conversation_id",
            "summary_timestamp",
            "covered_first_message_id",
            "covered_last_message_id",
            "estimated_reduction_percent",
            "readiness_state",
            "summary_text",
            "summary",
        ],
    )?;
    let version: i64 = db.query_row(
        "SELECT value FROM conversation_schema_metadata WHERE key='schema_version'",
        [],
        |r| r.get(0),
    )?;
    let metadata: i64 = db.query_row(
        "SELECT count(*) FROM conversation_schema_metadata",
        [],
        |r| r.get(0),
    )?;
    let summaries: i64 = db.query_row(
        "SELECT count(*) FROM conversation_summary_metadata",
        [],
        |r| r.get(0),
    )?;
    if version != 6 || metadata != 1 || summaries != 0 {
        return Err(invalid("unsupported source version or summaries").into());
    }
    let mut stmt=db.prepare("SELECT id,title,created_at,updated_at,archived,pinned,deleted,user_renamed FROM conversations ORDER BY rowid LIMIT 65")?;
    let rows = stmt.query_map([], |r| {
        Ok((
            r.get::<_, String>(0)?,
            r.get::<_, String>(1)?,
            r.get::<_, String>(2)?,
            r.get::<_, String>(3)?,
            r.get::<_, i64>(4)?,
            r.get::<_, i64>(5)?,
            r.get::<_, i64>(6)?,
            r.get::<_, i64>(7)?,
        ))
    })?;
    let mut conversations = Vec::new();
    let mut index = HashMap::new();
    for row in rows {
        let (id, title, created_at, updated_at, archived, pinned, deleted, renamed) = row?;
        if conversations.len() >= 64
            || !uuid(&id)
            || title.len() > 1024
            || !timestamp(&created_at)
            || !timestamp(&updated_at)
            || [archived, pinned, deleted, renamed] != [0, 0, 0, 0]
            || index.contains_key(&id)
        {
            return Err(invalid("unsupported conversation").into());
        }
        index.insert(id.clone(), conversations.len());
        conversations.push(Conversation {
            id,
            title,
            created_at,
            updated_at,
            messages: Vec::new(),
        });
    }
    drop(stmt);
    if conversations.is_empty() {
        return Err(invalid("empty source").into());
    }
    let mut stmt=db.prepare("SELECT conversation_id,message_id,role,content,timestamp,status,provider_id,model_id,reply_to_id,replaces_id,partial,error_category,attachments_json FROM conversation_messages ORDER BY conversation_id,message_id LIMIT 2049")?;
    let mut rows = stmt.query([])?;
    let mut total = 0usize;
    while let Some(row) = rows.next()? {
        total += 1;
        if total > 2048 {
            return Err(invalid("source message limit").into());
        }
        let sid: String = row.get(0)?;
        let pos = *index
            .get(&sid)
            .ok_or_else(|| invalid("orphan source message"))?;
        let message = Message {
            id: row.get(1)?,
            role: row.get(2)?,
            content: row.get(3)?,
            timestamp: row.get(4)?,
            status: row.get(5)?,
            provider: row.get(6)?,
            model: row.get(7)?,
            reply_to: row.get(8)?,
            replaces: row.get(9)?,
            partial: row.get(10)?,
        };
        let category: String = row.get(11)?;
        let attachments: String = row.get(12)?;
        let refs: HashSet<i64> = conversations[pos].messages.iter().map(|m| m.id).collect();
        let allowed_status = [
            "sent",
            "received",
            "error",
            "completed",
            "failed",
            "cancelled",
            "interrupted",
        ];
        if conversations[pos].messages.len() >= MAX_IMPORTED_MESSAGES_PER_SESSION
            || message.id <= 0
            || message.id > i32::MAX as i64 - 32
            || !["system", "user", "assistant"].contains(&message.role.as_str())
            || !allowed_status.contains(&message.status.as_str())
            || message.content.len() > 65536
            || message.provider.len() > 1024
            || message.model.len() > 1024
            || !timestamp(&message.timestamp)
            || ![0, 1].contains(&message.partial)
            || category != "None"
            || !matches!(serde_json::from_str::<Value>(&attachments),Ok(Value::Array(a)) if a.is_empty())
            || (message.reply_to != 0 && !refs.contains(&message.reply_to))
            || (message.replaces != 0 && !refs.contains(&message.replaces))
            || refs.contains(&message.id)
        {
            return Err(invalid("unsupported source message").into());
        }
        conversations[pos].messages.push(message);
    }
    drop(rows);
    drop(stmt);
    no_sidecars(&source)?;
    let current = fs::symlink_metadata(&source)?;
    if current.dev() != identity.dev()
        || current.ino() != identity.ino()
        || current.len() != identity.len()
        || current.mtime() != identity.mtime()
        || current.mtime_nsec() != identity.mtime_nsec()
    {
        return Err(invalid("source changed during validation").into());
    }
    // The caller-supplied copy must remain quiescent; read transaction prevents
    // concurrent rollback-mode commits until this function returns.
    Ok(Snapshot {
        conversations,
        _source_db: db,
        source,
        identity,
    })
}

#[cfg(test)]
mod tests {
    use super::timestamp;
    #[test]
    fn utc_timestamps_validate_calendar_and_leap_days() {
        assert!(timestamp("2024-02-29T23:59:59.999Z"));
        for invalid in [
            "2023-02-29T00:00:00.000Z",
            "2026-99-99T99:99:99.999Z",
            "2026-01-01T24:00:00.000Z",
            "2026-01-01T00:00:00Z",
        ] {
            assert!(!timestamp(invalid), "accepted {invalid}");
        }
    }
}
