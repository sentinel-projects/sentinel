//! Inventory detached Qt grants without importing authority or exposing paths.
use rusqlite::{Connection, OpenFlags};
use serde_json::{Value, json};
use std::{io, path::Path};

pub fn inspect(path: &Path) -> Result<Value, Box<dyn std::error::Error>> {
    let invalid = || io::Error::new(io::ErrorKind::InvalidData, "unsupported Qt permission copy");
    let (source, identity) = crate::import::checked_source(path, Path::new("/"))?;
    let db = Connection::open_with_flags(
        &source,
        OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NOFOLLOW,
    )?;
    db.busy_timeout(std::time::Duration::from_millis(250))?;
    db.execute_batch("PRAGMA trusted_schema=OFF; PRAGMA query_only=ON; BEGIN;")?;
    let check: String = db.query_row("PRAGMA quick_check", [], |row| row.get(0))?;
    let version: u32 = db.query_row("PRAGMA user_version", [], |row| row.get(0))?;
    if check != "ok" || !matches!(version, 1 | 2) {
        return Err(invalid().into());
    }
    let objects: u64 = db.query_row(
        "SELECT COUNT(*) FROM sqlite_schema WHERE type IN ('table','view','trigger')
         AND NOT (type='table' AND name='permission_grants' AND sql LIKE 'CREATE TABLE %')",
        [],
        |row| row.get(0),
    )?;
    let mut columns = db.prepare("PRAGMA table_info(permission_grants)")?;
    let mut actual = columns
        .query_map([], |row| row.get::<_, String>(1))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    actual.sort();
    let mut expected = vec![
        "access",
        "created_at",
        "decision",
        "domain",
        "grant_id",
        "resource_kind",
        "resource_scope",
    ];
    if version == 2 {
        expected.push("plugin_owner_id");
    }
    expected.sort_unstable();
    if objects != 0 || actual != expected {
        return Err(invalid().into());
    }
    let (grants, invalid_grants): (u64, u64) = db.query_row(
        "SELECT COUNT(*),COUNT(CASE WHEN decision IS NULL OR decision!=1
          OR typeof(domain)!='integer' OR typeof(access)!='integer'
          OR typeof(resource_kind)!='integer' OR typeof(resource_scope)!='text'
          OR typeof(grant_id)!='text' OR typeof(created_at)!='text' THEN 1 END)
         FROM permission_grants",
        [],
        |row| Ok((row.get(0)?, row.get(1)?)),
    )?;
    if invalid_grants != 0 {
        return Err(invalid().into());
    }
    let plugin_grants: u64 = if version == 2 {
        db.query_row(
            "SELECT COUNT(*) FROM permission_grants WHERE plugin_owner_id!=''",
            [],
            |row| row.get(0),
        )?
    } else {
        0
    };
    crate::import::no_sidecars(&source)?;
    use std::os::unix::fs::MetadataExt;
    let current = std::fs::symlink_metadata(&source)?;
    if identity.dev() != current.dev()
        || identity.ino() != current.ino()
        || identity.len() != current.len()
        || identity.ctime() != current.ctime()
        || identity.ctime_nsec() != current.ctime_nsec()
    {
        return Err(invalid().into());
    }
    Ok(
        json!({"schema_version":1,"qt_schema":version,"existing_grants":grants,
        "plugin_grants":plugin_grants,"requires_reapproval":grants,"imported_grants":0}),
    )
}
