//! Conservative validation of development snapshot rows before publication.
use crate::{import, provider::Config, settings};
use rusqlite::{Connection, OptionalExtension};
use serde_json::Value;
use std::collections::HashMap;

fn invalid<T>() -> rusqlite::Result<T> {
    Err(rusqlite::Error::InvalidQuery)
}
fn bounded(value: &str, max: usize) -> bool {
    value.len() <= max && !value.contains('\0')
}

pub fn settings_rows(db: &Connection) -> rusqlite::Result<()> {
    let selection: Option<(i64, String, String, String)> = db
        .query_row(
            "SELECT id,endpoint,provider_id,model FROM selection",
            [],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)),
        )
        .optional()?;
    if let Some((id, endpoint, provider, model)) = selection {
        if id != 1 || !Config::valid_persisted_selection(&endpoint, &provider, &model) {
            return invalid();
        }
    }
    let mut stmt = db.prepare("SELECT key,value FROM preferences LIMIT ?1")?;
    let rows = stmt
        .query_map([(settings::MAX_PREFERENCES + 1) as i64], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    if rows.len() > settings::MAX_PREFERENCES
        || rows.iter().any(|(k, v)| !settings::valid_preference(k, v))
    {
        return invalid();
    }
    let mut stmt = db.prepare("SELECT id,workspace_id,root FROM workspace_selection LIMIT 2")?;
    let rows = stmt
        .query_map([], |r| {
            Ok((
                r.get::<_, i64>(0)?,
                r.get::<_, String>(1)?,
                r.get::<_, String>(2)?,
            ))
        })?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    if rows.len() > 1
        || rows
            .iter()
            .any(|(n, id, root)| *n != 1 || !settings::valid_workspace_binding(id, root))
    {
        return invalid();
    }
    let mut stmt = db.prepare("SELECT workspace_id,root FROM workspace_revocations LIMIT 65")?;
    let rows = stmt
        .query_map([], |r| Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?)))?
        .collect::<rusqlite::Result<Vec<_>>>()?;
    if rows.len() > 64
        || rows
            .iter()
            .any(|(id, root)| !settings::valid_workspace_binding(id, root))
    {
        return invalid();
    }
    Ok(())
}

pub fn memory_rows(db: &Connection) -> rusqlite::Result<()> {
    let mut stmt = db
        .prepare("SELECT id,session,key,value,revision,created,updated FROM entries LIMIT 64001")?;
    let mut rows = stmt.query([])?;
    let mut counts = HashMap::<String, usize>::new();
    let mut count = 0usize;
    while let Some(r) = rows.next()? {
        count += 1;
        let id: i64 = r.get(0)?;
        let session: String = r.get(1)?;
        let key: String = r.get(2)?;
        let value: String = r.get(3)?;
        let revision: i64 = r.get(4)?;
        let created: String = r.get(5)?;
        let updated: String = r.get(6)?;
        let n = counts.entry(session.clone()).or_default();
        *n += 1;
        if count > 64000
            || *n > 1000
            || id <= 0
            || session.is_empty()
            || !bounded(&session, 256)
            || key.is_empty()
            || key.encode_utf16().count() > 256
            || value.len() > 65536
            || value.encode_utf16().count() > 65536
            || revision < 1
            || !bounded(&created, 64)
            || !bounded(&updated, 64)
        {
            return invalid();
        }
    }
    let mut stmt = db.prepare(
        "SELECT qt_id,qt_revision,provenance,source,corrections FROM qt_imports LIMIT 64001",
    )?;
    let mut rows = stmt.query([])?;
    let mut count = 0usize;
    while let Some(r) = rows.next()? {
        count += 1;
        let id: String = r.get(0)?;
        let revision: String = r.get(1)?;
        let provenance: i64 = r.get(2)?;
        let source: String = r.get(3)?;
        let corrections: String = r.get(4)?;
        let parsed: Value =
            serde_json::from_str(&corrections).map_err(|_| rusqlite::Error::InvalidQuery)?;
        if count > 64000
            || !import::uuid(&id)
            || id.bytes().any(|b| b.is_ascii_uppercase())
            || id == "00000000-0000-0000-0000-000000000000"
            || revision.is_empty()
            || !bounded(&revision, 256)
            || !(0..=6).contains(&provenance)
            || source.encode_utf16().count() > 512
            || !bounded(&source, 2048)
            || corrections.len() > 65536
            || parsed.as_array().is_none_or(|a| {
                a.len() > 256
                    || a.iter().any(|v| {
                        let Some(o) = v.as_object() else {
                            return true;
                        };
                        let actor = o.get("actor").and_then(Value::as_str).unwrap_or("");
                        let action = o.get("action").and_then(Value::as_str).unwrap_or("");
                        let digest = o
                            .get("previousDigest")
                            .and_then(Value::as_str)
                            .unwrap_or("");
                        o.len() != 4
                            || !o
                                .get("at")
                                .and_then(Value::as_str)
                                .is_some_and(import::timestamp)
                            || actor.is_empty()
                            || actor.encode_utf16().count() > 256
                            || !["correct", "move", "reclassify", "import-replace", "import"]
                                .contains(&action)
                            || digest.len() != 64
                            || !digest
                                .bytes()
                                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
                    })
            })
        {
            return invalid();
        }
    }
    Ok(())
}

pub fn agent_rows(db: &Connection) -> rusqlite::Result<()> {
    let mut stmt=db.prepare("SELECT id,session_id,state,goal,provider_id,model,output,reason,confirmed FROM runs LIMIT 2049")?;
    let mut rows = stmt.query([])?;
    let mut count = 0;
    while let Some(r) = rows.next()? {
        count += 1;
        let id: String = r.get(0)?;
        let session: String = r.get(1)?;
        let state: String = r.get(2)?;
        let goal: String = r.get(3)?;
        let provider: String = r.get(4)?;
        let model: String = r.get(5)?;
        let output: String = r.get(6)?;
        let reason: String = r.get(7)?;
        let confirmed: i64 = r.get(8)?;
        if count > 2048
            || id.is_empty()
            || !bounded(&id, 256)
            || session.is_empty()
            || !bounded(&session, 256)
            || !["completed", "failed", "cancelled"].contains(&state.as_str())
            || !bounded(&goal, 65536)
            || !bounded(&provider, 256)
            || !bounded(&model, 256)
            || !bounded(&output, 32768)
            || !bounded(&reason, 256)
            || confirmed != 1
        {
            return invalid();
        }
    }
    let mut stmt=db.prepare("SELECT run_id,step,tool,path,content,complete,arguments,descriptor_version,failure_category FROM observations LIMIT 65537")?;
    let mut rows = stmt.query([])?;
    let mut count = 0;
    while let Some(r) = rows.next()? {
        count += 1;
        let run: String = r.get(0)?;
        let step: i64 = r.get(1)?;
        let tool: String = r.get(2)?;
        let path: String = r.get(3)?;
        let content: String = r.get(4)?;
        let complete: i64 = r.get(5)?;
        let args: Option<String> = r.get(6)?;
        let version: Option<String> = r.get(7)?;
        let failure: Option<String> = r.get(8)?;
        if count > 65536
            || !bounded(&run, 256)
            || !(1..=32).contains(&step)
            || !bounded(&tool, 128)
            || !bounded(&path, 4096)
            || !bounded(&content, 16384)
            || !(0..=1).contains(&complete)
            || args
                .as_ref()
                .is_some_and(|v| !bounded(v, 131072) || serde_json::from_str::<Value>(v).is_err())
            || version.as_ref().is_some_and(|v| !bounded(v, 128))
            || args.is_some() != version.is_some()
            || failure.as_deref().is_some_and(|v| {
                ![
                    "not-found",
                    "permission-denied",
                    "invalid-path",
                    "unavailable",
                ]
                .contains(&v)
                    || complete != 0
            })
        {
            return invalid();
        }
    }
    let mut stmt = db.prepare(
        "SELECT run_id,step,status,reason,operation,dependencies FROM plan_steps LIMIT 8193",
    )?;
    let mut rows = stmt.query([])?;
    let mut count = 0;
    while let Some(r) = rows.next()? {
        count += 1;
        let run: String = r.get(0)?;
        let step: i64 = r.get(1)?;
        let status: String = r.get(2)?;
        let reason: String = r.get(3)?;
        let operation: Option<String> = r.get(4)?;
        let deps: Option<String> = r.get(5)?;
        if count > 8192
            || !bounded(&run, 256)
            || !(1..=4).contains(&step)
            || !matches!(
                (status.as_str(), reason.as_str()),
                ("succeeded", "")
                    | ("failed", "step-failed")
                    | ("blocked", "dependency-failed")
                    | ("skipped", "run-ended" | "daemon-restarted")
                    | ("interrupted", "run-ended" | "daemon-restarted")
            )
            || operation.is_some() != deps.is_some()
        {
            return invalid();
        }
        if let (Some(op), Some(deps)) = (operation, deps) {
            if ![
                "inspect-file",
                "inspect-files",
                "inspect-directory",
                "search-literal",
                "glob",
                "stat-path",
                "create-file",
                "edit-file",
            ]
            .contains(&op.as_str())
            {
                return invalid();
            }
            let parsed: Vec<usize> =
                serde_json::from_str(&deps).map_err(|_| rusqlite::Error::InvalidQuery)?;
            let mut sorted = parsed.clone();
            sorted.sort_unstable();
            sorted.dedup();
            if parsed.len() != sorted.len() || parsed.iter().any(|d| *d >= (step as usize - 1)) {
                return invalid();
            }
        }
    }
    let mut stmt = db.prepare("SELECT a.id,a.run_id,a.step,a.tool,a.args,a.decision,o.tool,o.arguments FROM approvals a LEFT JOIN observations o ON o.run_id=a.run_id AND o.step=a.step LIMIT 65537")?;
    let mut rows = stmt.query([])?;
    let mut count = 0;
    while let Some(r) = rows.next()? {
        count += 1;
        let id: String = r.get(0)?;
        let run: String = r.get(1)?;
        let step: i64 = r.get(2)?;
        let tool: String = r.get(3)?;
        let args: String = r.get(4)?;
        let decision: String = r.get(5)?;
        let observed_tool: Option<String> = r.get(6)?;
        let observed_args: Option<String> = r.get(7)?;
        if count > 65536
            || id.is_empty()
            || !bounded(&id, 256)
            || run.is_empty()
            || !bounded(&run, 256)
            || !(1..=32).contains(&step)
            || !bounded(&tool, 128)
            || !bounded(&args, 131072)
            || serde_json::from_str::<Value>(&args).is_err()
            || !["allow", "deny", "timeout", "cancelled", "denied-restart"]
                .contains(&decision.as_str())
            || observed_tool.as_deref() != Some(tool.as_str())
            || observed_args.as_ref().is_some_and(|v| v != &args)
        {
            return invalid();
        }
    }
    Ok(())
}
