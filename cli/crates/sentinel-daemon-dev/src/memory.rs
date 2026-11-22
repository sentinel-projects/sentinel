//! Isolated, explicit session memory management; never injected into model context.
use crate::store::{self, WriteFailure};
use rusqlite::{Connection, OptionalExtension, TransactionBehavior, params};
use serde_json::{Value, json};
use std::{error::Error, path::Path, time::Duration};

pub const DATABASE: &str = "daemon-dev-memory.sqlite3";
pub const JOURNAL: &str = "daemon-dev-memory.sqlite3-journal";
pub(crate) const APP_ID: u32 = 0x53444d45;
pub(crate) const VERSION: u32 = 2;
const IMPORT_SCHEMA: &str = "CREATE TABLE qt_imports(entry_id INTEGER PRIMARY KEY REFERENCES entries(id) ON DELETE CASCADE, qt_id TEXT NOT NULL UNIQUE, qt_revision TEXT NOT NULL, provenance INTEGER NOT NULL, source TEXT NOT NULL, corrections TEXT NOT NULL);";
pub struct MemoryStore {
    db: Connection,
}

impl MemoryStore {
    pub fn development_snapshot(&self) -> rusqlite::Result<Vec<u8>> {
        crate::backup::snapshot(&self.db)
    }

    pub fn open(profile: &Path) -> Result<Self, Box<dyn Error>> {
        Self::open_inner(profile, false)
    }
    pub fn open_for_import(profile: &Path) -> Result<Self, Box<dyn Error>> {
        Self::open_inner(profile, true)
    }
    fn open_inner(profile: &Path, importing: bool) -> Result<Self, Box<dyn Error>> {
        let (mut db, new, old_version) =
            store::open_database(profile, DATABASE, APP_ID, VERSION, importing)?;
        db.busy_timeout(Duration::from_millis(250))?;
        if importing {
            db.execute_batch("PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;")?;
        } else {
            db.execute_batch(
                "PRAGMA foreign_keys=ON; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;",
            )?;
        }
        if new {
            let tx = db.transaction()?;
            tx.execute_batch(&format!("PRAGMA application_id={APP_ID}; PRAGMA user_version={VERSION}; CREATE TABLE entries(id INTEGER PRIMARY KEY AUTOINCREMENT, session TEXT NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL, revision INTEGER NOT NULL DEFAULT 1, created TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now')), updated TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ','now'))); CREATE INDEX entries_session ON entries(session,id);"))?;
            tx.execute_batch(IMPORT_SCHEMA)?;
            tx.commit()?;
        } else {
            let check: String = db.query_row("PRAGMA quick_check", [], |r| r.get(0))?;
            if check != "ok" {
                return Err(std::io::Error::other("invalid development memory").into());
            }
            if old_version == 1 {
                let tx = db.transaction()?;
                tx.execute_batch(IMPORT_SCHEMA)?;
                tx.execute_batch("PRAGMA user_version=2;")?;
                tx.commit()?;
            }
        }
        Ok(Self { db })
    }

    pub fn import_snapshot(
        &mut self,
        snapshot: &crate::memory_import::Snapshot,
    ) -> Result<(), Box<dyn Error>> {
        snapshot.validate_unchanged()?;
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        let count: i64 = tx.query_row(
            "SELECT (SELECT count(*) FROM entries)+(SELECT count(*) FROM qt_imports)",
            [],
            |r| r.get(0),
        )?;
        if count != 0 {
            return Err(std::io::Error::new(
                std::io::ErrorKind::InvalidInput,
                "destination memory not empty",
            )
            .into());
        }
        for row in &snapshot.rows {
            tx.execute(
                "INSERT INTO entries(session,key,value,created,updated) VALUES(?1,?2,?3,?4,?5)",
                params![row.session, row.key, row.value, row.created, row.updated],
            )?;
            let id = tx.last_insert_rowid();
            tx.execute("INSERT INTO qt_imports(entry_id,qt_id,qt_revision,provenance,source,corrections) VALUES(?1,?2,?3,?4,?5,?6)",params![id,row.id,row.revision,row.provenance,row.source,row.corrections])?;
        }
        snapshot.validate_unchanged()?;
        tx.commit()?;
        Ok(())
    }

    fn resolve_id(&self, session: &str, external: &str) -> rusqlite::Result<Option<i64>> {
        self.db.query_row("SELECT e.id FROM entries e LEFT JOIN qt_imports q ON q.entry_id=e.id WHERE e.session=?1 AND COALESCE(q.qt_id,CAST(e.id AS TEXT))=?2",params![session,external],|r|r.get(0)).optional()
    }
    fn decorate(&self, id: i64, mut value: Value, preview: bool) -> rusqlite::Result<Value> {
        let metadata:Option<(String,String,i64,String,String)>=self.db.query_row("SELECT qt_id,qt_revision,provenance,source,corrections FROM qt_imports WHERE entry_id=?1",[id],|r|Ok((r.get(0)?,r.get(1)?,r.get(2)?,r.get(3)?,r.get(4)?))).optional()?;
        if let Some((external, revision, provenance, source, corrections)) = metadata {
            let receipts: Value =
                serde_json::from_str(&corrections).map_err(|_| rusqlite::Error::InvalidQuery)?;
            let list = receipts.as_array().ok_or(rusqlite::Error::InvalidQuery)?;
            value["id"] = json!(external);
            value["revision"] = json!(revision);
            value["provenance"] = json!(provenance);
            value["source"] = json!(source);
            value["correctionCount"] = json!(list.len());
            if !preview {
                value["corrections"] = json!(list.iter().take(16).collect::<Vec<_>>());
            }
        }
        Ok(value)
    }

    pub fn manage(&mut self, session: &str, request: &Value) -> Result<Value, WriteFailure> {
        let fail = || WriteFailure {
            safe_conflict: false,
        };
        let Some(object) = request.as_object() else {
            return Ok(json!({"status":"InvalidInput","requestTag":""}));
        };
        if [
            "operation",
            "id",
            "revision",
            "key",
            "value",
            "session",
            "workspace",
            "confirmation",
            "requestTag",
            "cursor",
            "search",
        ]
        .iter()
        .any(|key| object.get(*key).is_some_and(|value| !value.is_string()))
            || object
                .get("confirmed")
                .is_some_and(|value| !value.is_boolean())
        {
            return Ok(json!({"status":"InvalidInput","requestTag":""}));
        }
        let tag = object
            .get("requestTag")
            .and_then(Value::as_str)
            .unwrap_or("");
        let answer = |status: &str| json!({"status":status,"requestTag":tag});
        if tag.encode_utf16().count() > 64
            || object.keys().any(|key| {
                ![
                    "operation",
                    "scope",
                    "id",
                    "revision",
                    "key",
                    "value",
                    "session",
                    "workspace",
                    "confirmed",
                    "confirmation",
                    "requestTag",
                    "offset",
                    "cursor",
                    "search",
                ]
                .contains(&key.as_str())
            })
            || object.get("scope").and_then(Value::as_u64) != Some(1)
        {
            return Ok(answer("InvalidInput"));
        }
        let Some(operation) = object.get("operation").and_then(Value::as_str) else {
            return Ok(answer("InvalidInput"));
        };
        if matches!(operation, "create" | "correct" | "delete")
            && (object.get("session").and_then(Value::as_str) != Some(session)
                || object.get("workspace").and_then(Value::as_str) != Some(""))
        {
            return Ok(answer("Conflict"));
        }
        let field = |key: &str| object.get(key).and_then(Value::as_str);
        if operation == "list" {
            let cursor = match field("cursor").unwrap_or("") {
                "" => 0,
                s => match s.parse::<i64>() {
                    Ok(n) if n > 0 => n,
                    _ => return Ok(answer("InvalidInput")),
                },
            };
            let search = field("search").unwrap_or("");
            if search.len() > 256 {
                return Ok(answer("InvalidInput"));
            }
            let mut stmt = self.db.prepare("SELECT id,key,value,revision,created,updated FROM entries WHERE session=?1 AND id>?2 AND instr(lower(key),lower(?3))>0 ORDER BY id LIMIT 11").map_err(|_| fail())?;
            let rows = stmt
                .query_map(params![session, cursor, search], |r| {
                    Ok((
                        r.get::<_, i64>(0)?,
                        r.get::<_, String>(1)?,
                        r.get::<_, String>(2)?,
                        r.get::<_, i64>(3)?,
                        r.get::<_, String>(4)?,
                        r.get::<_, String>(5)?,
                    ))
                })
                .map_err(|_| fail())?
                .collect::<rusqlite::Result<Vec<_>>>()
                .map_err(|_| fail())?;
            let next = if rows.len() > 10 {
                rows[9].0.to_string()
            } else {
                String::new()
            };
            let records: Vec<_> = rows
                .into_iter()
                .take(10)
                .map(|r| {
                    let id = r.0;
                    self.decorate(id, record(session, r, true), true)
                })
                .collect::<rusqlite::Result<_>>()
                .map_err(|_| fail())?;
            return Ok(
                json!({"status":"Ready","requestTag":tag,"records":records,"nextCursor":next,"session":session,"workspace":"","index":"Unavailable","semantic":"Unavailable"}),
            );
        }
        if operation == "read" {
            let Some(external) = field("id").filter(|s| !s.is_empty() && s.len() <= 64) else {
                return Ok(answer("InvalidInput"));
            };
            let Some(id) = self.resolve_id(session, external).map_err(|_| fail())? else {
                return Ok(answer("NotFound"));
            };
            let row: Option<(i64, String, String, i64, String, String)> = self.db.query_row("SELECT id,key,value,revision,created,updated FROM entries WHERE session=?1 AND id=?2", params![session,id], |r| Ok((r.get(0)?,r.get(1)?,r.get(2)?,r.get(3)?,r.get(4)?,r.get(5)?))).optional().map_err(|_| fail())?;
            let Some(row) = row else {
                return Ok(answer("NotFound"));
            };
            let offset = object
                .get("offset")
                .map_or(Some(0), Value::as_u64)
                .and_then(|n| usize::try_from(n).ok());
            let Some(offset) = offset else {
                return Ok(answer("InvalidInput"));
            };
            let Some((text, _)) = utf16_page(&row.2, offset, 4096) else {
                return Ok(answer("InvalidInput"));
            };
            let mut output = self
                .decorate(id, record(session, row, false), false)
                .map_err(|_| fail())?;
            output["value"] = json!(text);
            output["offset"] = json!(offset);
            return Ok(json!({"status":"Ready","requestTag":tag,"record":output}));
        }
        if !matches!(operation, "create" | "correct" | "delete") {
            return Ok(answer("InvalidInput"));
        }
        if operation == "delete"
            && (object.get("confirmed") != Some(&json!(true))
                || field("confirmation") != Some("DELETE"))
        {
            return Ok(answer("InvalidInput"));
        }
        let (key, value) = if operation != "delete" {
            match (field("key"), field("value")) {
                (Some(k), Some(v))
                    if !k.is_empty()
                        && k.encode_utf16().count() <= 256
                        && v.encode_utf16().count() <= 65536
                        && v.len() <= 65536 =>
                {
                    (k, v)
                }
                _ => return Ok(answer("InvalidInput")),
            }
        } else {
            ("", "")
        };
        let target = if operation != "create" {
            let Some(external) = field("id").filter(|s| !s.is_empty() && s.len() <= 64) else {
                return Ok(answer("InvalidInput"));
            };
            let Some(revision) = field("revision").filter(|s| !s.is_empty() && s.len() <= 256)
            else {
                return Ok(answer("InvalidInput"));
            };
            let Some(id) = self.resolve_id(session, external).map_err(|_| fail())? else {
                return Ok(answer("NotFound"));
            };
            Some((id, revision.to_owned()))
        } else {
            None
        };
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)
            .map_err(store::acquisition_failure)?;
        if let Some((id, revision)) = target {
            let current: Option<(String,bool)> = tx
                .query_row(
                    "SELECT COALESCE(q.qt_revision,CAST(e.revision AS TEXT)), q.entry_id IS NOT NULL FROM entries e LEFT JOIN qt_imports q ON q.entry_id=e.id WHERE e.session=?1 AND e.id=?2",
                    params![session, id],
                    |r| Ok((r.get(0)?,r.get(1)?)),
                )
                .optional()
                .map_err(store::mutation_failure)?;
            match current {
                None => return Ok(answer("NotFound")),
                Some((v, _)) if v != revision => return Ok(answer("Conflict")),
                Some((_, true)) if operation == "correct" => return Ok(answer("Unavailable")),
                _ => {}
            }
            if operation == "delete" {
                tx.execute(
                    "DELETE FROM entries WHERE session=?1 AND id=?2",
                    params![session, id],
                )
                .map_err(store::mutation_failure)?;
            } else {
                tx.execute("UPDATE entries SET key=?3,value=?4,revision=revision+1,updated=strftime('%Y-%m-%dT%H:%M:%fZ','now') WHERE session=?1 AND id=?2", params![session,id,key,value]).map_err(store::mutation_failure)?;
            }
        } else {
            let count: i64 = tx
                .query_row(
                    "SELECT count(*) FROM entries WHERE session=?1",
                    [session],
                    |r| r.get(0),
                )
                .map_err(store::mutation_failure)?;
            if count >= 1000 {
                return Ok(answer("LimitExceeded"));
            }
            tx.execute(
                "INSERT INTO entries(session,key,value) VALUES(?1,?2,?3)",
                params![session, key, value],
            )
            .map_err(store::mutation_failure)?;
        }
        tx.commit().map_err(store::mutation_failure)?;
        Ok(answer("Ready"))
    }
}

fn record(session: &str, row: (i64, String, String, i64, String, String), preview: bool) -> Value {
    let (id, key, value, revision, created, updated) = row;
    let length = value.encode_utf16().count();
    let text = utf16_page(&value, 0, if preview { 300 } else { 4096 })
        .unwrap()
        .0;
    json!({"id":id.to_string(),"scope":1,"owner":session,"workspace":"","key":key,"value":text,"valueLength":length,"revision":revision.to_string(),"source":format!("manual:{session}"),"created":created,"updated":updated,"corrections":[],"correctionCount":0,"recallEligible":false,"untrusted":true,"offset":0})
}

fn utf16_page(value: &str, offset: usize, limit: usize) -> Option<(String, usize)> {
    let mut units = 0usize;
    let mut start = None;
    let mut end = value.len();
    for (position, ch) in value.char_indices() {
        if units == offset {
            start = Some(position);
        }
        if units > offset && start.is_none() {
            return None;
        }
        if start.is_some() && units + ch.len_utf16() > offset.saturating_add(limit) {
            end = position;
            break;
        }
        units += ch.len_utf16();
    }
    if units == offset && start.is_none() {
        start = Some(value.len());
    }
    let start = start?;
    Some((value[start..end].to_owned(), units.saturating_sub(offset)))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs, os::unix::fs::DirBuilderExt};
    #[test]
    fn v1_migration_keeps_existing_numeric_ids_and_values() {
        use std::os::unix::fs::PermissionsExt;
        let dir =
            std::env::temp_dir().join(format!("sentinel-memory-upgrade-{}", std::process::id()));
        fs::DirBuilder::new().mode(0o700).create(&dir).unwrap();
        let dir = dir.canonicalize().unwrap();
        let path = dir.join(DATABASE);
        let db = Connection::open(&path).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        db.execute_batch(&format!("PRAGMA application_id={APP_ID}; PRAGMA user_version=1; CREATE TABLE entries(id INTEGER PRIMARY KEY AUTOINCREMENT, session TEXT NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL, revision INTEGER NOT NULL DEFAULT 1, created TEXT NOT NULL, updated TEXT NOT NULL); CREATE INDEX entries_session ON entries(session,id); INSERT INTO entries VALUES(42,'s','key','value',7,'2026-01-01','2026-01-01');")).unwrap();
        drop(db);
        let mut memory = MemoryStore::open(&dir).unwrap();
        let row = memory
            .manage("s", &json!({"operation":"read","scope":1,"id":"42"}))
            .unwrap();
        assert_eq!(row["record"]["id"], "42");
        assert_eq!(row["record"]["revision"], "7");
        assert_eq!(row["record"]["value"], "value");
        drop(memory);
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn scoped_crud_revision_and_paging_survive_restart() {
        let dir = std::env::temp_dir().join(format!("sentinel-memory-test-{}", std::process::id()));
        fs::DirBuilder::new().mode(0o700).create(&dir).unwrap();
        let dir = dir.canonicalize().unwrap();
        let mut memory = MemoryStore::open(&dir).unwrap();
        let value = format!("{}😀end", "x".repeat(4095));
        let create = json!({"operation":"create","scope":1,"session":"s","workspace":"","key":"note","value":value});
        assert_eq!(memory.manage("s", &create).unwrap()["status"], "Ready");
        assert!(
            memory
                .manage("other", &json!({"operation":"list","scope":1}))
                .unwrap()["records"]
                .as_array()
                .unwrap()
                .is_empty()
        );
        let row = memory
            .manage("s", &json!({"operation":"list","scope":1}))
            .unwrap()["records"][0]
            .clone();
        let id = row["id"].as_str().unwrap();
        let read = |offset| json!({"operation":"read","scope":1,"id":id,"offset":offset});
        assert_eq!(
            memory.manage("s", &read(4096)).unwrap()["status"],
            "InvalidInput"
        );
        assert_eq!(
            memory.manage("s", &read(4095)).unwrap()["record"]["value"],
            "😀end"
        );
        let stale = json!({"operation":"correct","scope":1,"session":"s","workspace":"","id":id,"revision":"9","key":"k","value":"v"});
        assert_eq!(memory.manage("s", &stale).unwrap()["status"], "Conflict");
        drop(memory);
        let mut memory = MemoryStore::open(&dir).unwrap();
        assert_eq!(
            memory.manage("s", &read(4095)).unwrap()["record"]["value"],
            "😀end"
        );
        let delete = json!({"operation":"delete","scope":1,"session":"s","workspace":"","id":id,"revision":"1","confirmed":true,"confirmation":"DELETE"});
        assert_eq!(memory.manage("s", &delete).unwrap()["status"], "Ready");
        assert_eq!(memory.manage("s", &read(0)).unwrap()["status"], "NotFound");
        drop(memory);
        fs::remove_dir_all(dir).unwrap();
    }
}
