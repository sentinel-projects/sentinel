//! Isolated development Agent audit; not compatible with Qt Agent stores.
use crate::store;
use rusqlite::{Connection, OptionalExtension, TransactionBehavior, params};
use serde_json::{Value, json};
use std::{error::Error, path::Path, time::Duration};
pub const DATABASE: &str = "daemon-dev-agent.sqlite3";
pub const JOURNAL: &str = "daemon-dev-agent.sqlite3-journal";
pub(crate) const APP_ID: u32 = 0x53444147;
pub(crate) const VERSION: u32 = 7;
const PLAN_SCHEMA: &str = "CREATE TABLE plan_steps(run_id TEXT NOT NULL REFERENCES runs(id), step INTEGER NOT NULL CHECK(step BETWEEN 1 AND 4), status TEXT NOT NULL CHECK(status IN ('pending','started','succeeded','failed','blocked','skipped','interrupted')), reason TEXT NOT NULL, operation TEXT, dependencies TEXT, PRIMARY KEY(run_id,step));";
const APPROVAL_SCHEMA: &str = "CREATE TABLE approvals(id TEXT PRIMARY KEY, run_id TEXT NOT NULL REFERENCES runs(id), step INTEGER NOT NULL, tool TEXT NOT NULL, args TEXT NOT NULL, decision TEXT NOT NULL CHECK(decision IN ('pending','allow','deny','timeout','cancelled','denied-restart')), UNIQUE(run_id,step));";
pub enum ApprovalResolution {
    Allow,
    Deny,
    Timeout,
    Cancelled,
}
pub(crate) struct AgentAdmission<'a> {
    pub run_id: &'a str,
    pub session_id: &'a str,
    pub goal: &'a str,
    pub provider: &'a str,
    pub model: &'a str,
    pub plan: Option<&'a [(String, Vec<usize>)]>,
}
pub struct AgentStore {
    db: Connection,
}
impl AgentStore {
    pub fn development_snapshot(&self) -> rusqlite::Result<Vec<u8>> {
        crate::backup::snapshot(&self.db)
    }

    pub fn open(profile: &Path) -> Result<Self, Box<dyn Error>> {
        let (mut db, new, old_version) =
            store::open_database(profile, DATABASE, APP_ID, VERSION, false)?;
        db.busy_timeout(Duration::from_millis(250))?;
        db.execute_batch(
            "PRAGMA foreign_keys=ON; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;",
        )?;
        if new {
            let tx = db.transaction()?;
            tx.execute_batch(&format!("PRAGMA application_id={APP_ID}; PRAGMA user_version={VERSION}; CREATE TABLE runs(id TEXT PRIMARY KEY,session_id TEXT NOT NULL,state TEXT NOT NULL,goal TEXT NOT NULL,provider_id TEXT NOT NULL,model TEXT NOT NULL,output TEXT NOT NULL DEFAULT '',reason TEXT NOT NULL DEFAULT '',confirmed INTEGER NOT NULL DEFAULT 0,created_at TEXT); CREATE TABLE observations(run_id TEXT NOT NULL REFERENCES runs(id),step INTEGER NOT NULL,tool TEXT NOT NULL,path TEXT NOT NULL,content TEXT NOT NULL,complete INTEGER NOT NULL,arguments TEXT,descriptor_version TEXT,failure_category TEXT,PRIMARY KEY(run_id,step));"))?;
            tx.execute_batch(APPROVAL_SCHEMA)?;
            tx.execute_batch(PLAN_SCHEMA)?;
            tx.commit()?;
        }

        if !new {
            let check: String = db.query_row("PRAGMA quick_check", [], |row| row.get(0))?;
            if check != "ok" {
                return Err(std::io::Error::other("invalid Agent audit").into());
            }
            if old_version < VERSION {
                let tx = db.transaction()?;
                if old_version == 1 {
                    tx.execute_batch(APPROVAL_SCHEMA)?;
                }
                if old_version < 3 {
                    tx.execute_batch("ALTER TABLE runs ADD COLUMN created_at TEXT;")?;
                }
                if old_version < 4 {
                    tx.execute_batch("ALTER TABLE observations ADD COLUMN arguments TEXT; ALTER TABLE observations ADD COLUMN descriptor_version TEXT;")?;
                }
                if old_version < 5 {
                    tx.execute_batch("ALTER TABLE observations ADD COLUMN failure_category TEXT;")?;
                }
                if old_version < 6 {
                    tx.execute_batch(PLAN_SCHEMA)?;
                } else {
                    tx.execute_batch("ALTER TABLE plan_steps RENAME TO plan_steps_v6;")?;
                    tx.execute_batch(PLAN_SCHEMA)?;
                    tx.execute_batch("INSERT INTO plan_steps(run_id,step,status,reason) SELECT run_id,step,status,reason FROM plan_steps_v6; DROP TABLE plan_steps_v6;")?;
                }
                tx.execute_batch("PRAGMA user_version=7;")?;
                tx.commit()?;
            }
        }
        Ok(Self { db })
    }
    #[cfg(test)]
    pub fn start(
        &mut self,
        id: &str,
        session: &str,
        goal: &str,
        provider: &str,
        model: &str,
    ) -> rusqlite::Result<()> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        Self::record_admission(
            &tx,
            &AgentAdmission {
                run_id: id,
                session_id: session,
                goal,
                provider,
                model,
                plan: None,
            },
            false,
        )?;
        tx.commit()
    }

    pub(crate) fn record_admission(
        tx: &rusqlite::Transaction<'_>,
        admission: &AgentAdmission<'_>,
        attached: bool,
    ) -> rusqlite::Result<()> {
        let AgentAdmission {
            run_id: id,
            session_id: session,
            goal,
            provider,
            model,
            plan,
        } = *admission;
        if let Some(steps) = plan {
            if steps.is_empty() || steps.len() > 4 {
                return Err(rusqlite::Error::InvalidQuery);
            }
            for (index, (operation, dependencies)) in steps.iter().enumerate() {
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
                .contains(&operation.as_str())
                    || dependencies.len() > index
                    || dependencies.iter().any(|d| *d >= index)
                {
                    return Err(rusqlite::Error::InvalidQuery);
                }
                let mut sorted = dependencies.clone();
                sorted.sort_unstable();
                sorted.dedup();
                if sorted.len() != dependencies.len() {
                    return Err(rusqlite::Error::InvalidQuery);
                }
            }
        }
        // The caller selects one of two static schema names.
        let schema = if attached { "agent" } else { "main" };
        tx.execute(
            &format!(
                "INSERT INTO {schema}.runs(id,session_id,state,goal,provider_id,model,created_at)
                      VALUES(?1,?2,'running',?3,?4,?5,strftime('%Y-%m-%dT%H:%M:%fZ','now'))"
            ),
            params![id, session, goal, provider, model],
        )?;
        if let Some(steps) = plan {
            let mut stmt = tx.prepare(&format!(
                "INSERT INTO {schema}.plan_steps(run_id,step,status,reason,operation,dependencies)
                                               VALUES(?1,?2,'pending','',?3,?4)"
            ))?;
            for (index, (operation, dependencies)) in steps.iter().enumerate() {
                let encoded = serde_json::to_string(dependencies)
                    .map_err(|_| rusqlite::Error::InvalidQuery)?;
                stmt.execute(params![id, (index + 1) as i64, operation, encoded])?;
            }
        }
        Ok(())
    }
    pub fn intention(
        &mut self,
        id: &str,
        step: usize,
        tool: &str,
        descriptor_version: &str,
        args: &str,
    ) -> rusqlite::Result<()> {
        let step = i64::try_from(step)
            .ok()
            .filter(|n| *n > 0)
            .ok_or(rusqlite::Error::InvalidQuery)?;
        let descriptor = crate::tool_registry::find(tool).ok_or(rusqlite::Error::InvalidQuery)?;
        let value: Value = serde_json::from_str(args).map_err(|_| rusqlite::Error::InvalidQuery)?;
        if descriptor.version != descriptor_version
            || descriptor.validate(&value).ok().as_ref() != Some(&value)
        {
            return Err(rusqlite::Error::InvalidQuery);
        }
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        if tx.execute("INSERT INTO observations(run_id,step,tool,path,content,complete,arguments,descriptor_version) SELECT ?1,?2,?3,'','',0,?4,?5 WHERE EXISTS(SELECT 1 FROM runs WHERE id=?1 AND state='running')",params![id,step,tool,args,descriptor_version])? != 1 { return Err(rusqlite::Error::QueryReturnedNoRows); }
        tx.commit()
    }
    pub fn request_approval(
        &mut self,
        run_id: &str,
        approval_id: &str,
        step: usize,
        tool: &str,
        args: &str,
    ) -> rusqlite::Result<()> {
        let step = i64::try_from(step).map_err(|_| rusqlite::Error::InvalidQuery)?;
        if approval_id.is_empty()
            || approval_id.len() > 256
            || step == 0
            || ![
                "read-file",
                "stat-path",
                "list-directory",
                "grep",
                "glob",
                "write-file",
                "edit-file",
                "process-exec",
            ]
            .contains(&tool)
            || args.len() > 128 * 1024
        {
            return Err(rusqlite::Error::InvalidQuery);
        }
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        if tx.execute("INSERT INTO approvals(id,run_id,step,tool,args,decision) SELECT ?1,?2,?3,?4,?5,'pending' WHERE EXISTS(SELECT 1 FROM runs WHERE id=?2 AND state='running') AND EXISTS(SELECT 1 FROM observations WHERE run_id=?2 AND step=?3 AND tool=?4 AND arguments=?5 AND descriptor_version IS NOT NULL)", params![approval_id,run_id,step,tool,args])? != 1 { return Err(rusqlite::Error::QueryReturnedNoRows); }
        tx.commit()
    }
    pub fn resolve_approval(
        &mut self,
        run_id: &str,
        approval_id: &str,
        resolution: ApprovalResolution,
    ) -> rusqlite::Result<()> {
        let decision = match resolution {
            ApprovalResolution::Allow => "allow",
            ApprovalResolution::Deny => "deny",
            ApprovalResolution::Timeout => "timeout",
            ApprovalResolution::Cancelled => "cancelled",
        };
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        if tx.execute("UPDATE approvals SET decision=?3 WHERE id=?1 AND run_id=?2 AND decision='pending' AND EXISTS(SELECT 1 FROM runs WHERE id=?2 AND state='running')", params![approval_id,run_id,decision])? != 1 { return Err(rusqlite::Error::QueryReturnedNoRows); }
        tx.commit()
    }
    pub fn observe(
        &mut self,
        id: &str,
        step: usize,
        path: &str,
        content: &str,
        complete: bool,
    ) -> rusqlite::Result<()> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        if tx.execute("UPDATE observations SET path=?3,content=?4,complete=?5 WHERE run_id=?1 AND step=?2 AND EXISTS(SELECT 1 FROM runs WHERE id=?1 AND state='running')",params![id,step as i64,path,content,complete])? != 1 { return Err(rusqlite::Error::QueryReturnedNoRows); }
        tx.commit()
    }
    pub fn plan_step(
        &mut self,
        id: &str,
        step: usize,
        status: &str,
        reason: &str,
    ) -> rusqlite::Result<()> {
        let previous = match (status, reason) {
            ("started", "") | ("blocked", "dependency-failed") => "pending",
            ("succeeded", "") | ("failed", "step-failed") => "started",
            _ => return Err(rusqlite::Error::InvalidQuery),
        };
        if !(1..=4).contains(&step) {
            return Err(rusqlite::Error::InvalidQuery);
        }
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        if tx.execute("UPDATE plan_steps SET status=?3,reason=?4 WHERE run_id=?1 AND step=?2 AND status=?5 AND EXISTS(SELECT 1 FROM runs WHERE id=?1 AND state='running')", params![id,step as i64,status,reason,previous])? != 1 { return Err(rusqlite::Error::QueryReturnedNoRows); }
        tx.commit()
    }
    pub fn observe_failure(
        &mut self,
        id: &str,
        step: usize,
        path: &str,
        category: &str,
    ) -> rusqlite::Result<()> {
        if ![
            "not-found",
            "permission-denied",
            "invalid-path",
            "unavailable",
        ]
        .contains(&category)
            || path.len() > 4096
        {
            return Err(rusqlite::Error::InvalidQuery);
        }
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        if tx.execute("UPDATE observations SET path=?3,content='Observation unavailable',complete=0,failure_category=?4 WHERE run_id=?1 AND step=?2 AND EXISTS(SELECT 1 FROM runs WHERE id=?1 AND state='running')", params![id, step as i64, path, category])? != 1 {
            return Err(rusqlite::Error::QueryReturnedNoRows);
        }
        tx.commit()
    }
    #[cfg(test)]
    pub fn finish(
        &mut self,
        id: &str,
        state: &str,
        output: &str,
        reason: &str,
    ) -> rusqlite::Result<()> {
        let tx = self
            .db
            .transaction_with_behavior(TransactionBehavior::Immediate)?;
        if state == "completed" {
            let unfinished: bool = tx.query_row(
                "SELECT EXISTS(SELECT 1 FROM plan_steps WHERE run_id=?1 AND status!='succeeded')",
                [id],
                |r| r.get(0),
            )?;
            if unfinished {
                return Err(rusqlite::Error::InvalidQuery);
            }
        }
        if tx.execute(
            "UPDATE runs SET state=?2,output=?3,reason=?4 WHERE id=?1 AND state='running' AND NOT EXISTS(SELECT 1 FROM approvals WHERE run_id=?1 AND decision='pending')",
            params![id, state, output, reason],
        )? != 1
        {
            return Err(rusqlite::Error::QueryReturnedNoRows);
        }
        tx.execute("UPDATE plan_steps SET status=CASE status WHEN 'pending' THEN 'skipped' ELSE 'interrupted' END,reason='run-ended' WHERE run_id=?1 AND status IN ('pending','started')", [id])?;
        tx.commit()
    }
    pub fn owner(&self, id: &str) -> rusqlite::Result<Option<String>> {
        self.db
            .query_row("SELECT session_id FROM runs WHERE id=?1", [id], |r| {
                r.get(0)
            })
            .optional()
    }
    #[cfg(test)]
    pub fn contains(&self, id: &str) -> rusqlite::Result<bool> {
        self.db
            .query_row("SELECT EXISTS(SELECT 1 FROM runs WHERE id=?1)", [id], |r| {
                r.get(0)
            })
    }
    /// Bounded inspector data scoped to sessions already attached by this peer.
    pub fn history(
        &self,
        sessions: &[String],
        run_id: &str,
        before_time: &str,
        before_id: &str,
    ) -> rusqlite::Result<Option<Value>> {
        if sessions.len() > 128
            || run_id.len() > 256
            || before_time.len() > 64
            || before_id.len() > 256
            || (before_time.is_empty() != before_id.is_empty())
        {
            return Err(rusqlite::Error::InvalidQuery);
        }
        let mut result = json!({"runs":[],"run":null,"steps":[],"tools":[],"authorizations":[],"evidence":[],"claims":[],"children":[],"error":"","truncated":false});
        if sessions.is_empty() {
            return Ok(if run_id.is_empty() {
                Some(result)
            } else {
                None
            });
        }
        let placeholders = vec!["?"; sessions.len()].join(",");
        let order_time = "COALESCE(created_at,'1970-01-01T00:00:00.000Z')";
        let sql = format!(
            "SELECT id,session_id,state,goal,provider_id,model,output,reason,confirmed,created_at FROM runs WHERE session_id IN ({placeholders}) AND (?='' OR ({order_time} < ? OR ({order_time} = ? AND id < ?))) ORDER BY {order_time} DESC,id DESC LIMIT 27"
        );
        let mut parameters: Vec<rusqlite::types::Value> =
            sessions.iter().cloned().map(Into::into).collect();
        for value in [before_time, before_time, before_time, before_id] {
            parameters.push(value.to_owned().into());
        }
        let mut statement = self.db.prepare(&sql)?;
        let rows = statement
            .query_map(rusqlite::params_from_iter(parameters), |r| {
                Ok((
                    r.get::<_, String>(0)?,
                    r.get::<_, String>(1)?,
                    r.get::<_, String>(2)?,
                    r.get::<_, String>(3)?,
                    r.get::<_, String>(4)?,
                    r.get::<_, String>(5)?,
                    r.get::<_, String>(6)?,
                    r.get::<_, String>(7)?,
                    r.get::<_, bool>(8)?,
                    r.get::<_, Option<String>>(9)?,
                ))
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        result["truncated"] = json!(rows.len() > 26);
        if rows
            .iter()
            .take(26)
            .any(|r| r.3.len() > 512 || r.6.len() > 512)
        {
            result["truncated"] = json!(true);
        }
        result["runs"] = Value::Array(rows.into_iter().take(26).map(|r| json!({"runId":r.0,"sessionId":r.1,"runType":"agent","state":r.2,"goalSummary":preview(&r.3,512),"providerId":r.4,"modelId":r.5,"finalAnswer":preview(&r.6,512),"failure":r.7,"terminalReason":r.7,"confirmed":r.8,"startedAt":r.9.clone().unwrap_or_default(),"cursorTime":r.9.unwrap_or_else(|| "1970-01-01T00:00:00.000Z".into())})).collect());
        if run_id.is_empty() {
            return Ok(Some(bound_history(result)?));
        }
        let detail_sql = format!(
            "SELECT session_id,state,goal,provider_id,model,output,reason,confirmed,created_at FROM runs WHERE id=? AND session_id IN ({placeholders})"
        );
        let mut detail_params: Vec<rusqlite::types::Value> = vec![run_id.to_owned().into()];
        detail_params.extend(sessions.iter().cloned().map(Into::into));
        use rusqlite::OptionalExtension;
        type RunDetail = (
            String,
            String,
            String,
            String,
            String,
            String,
            String,
            bool,
            Option<String>,
        );
        let detail: Option<RunDetail> = self
            .db
            .query_row(
                &detail_sql,
                rusqlite::params_from_iter(detail_params),
                |r| {
                    Ok((
                        r.get(0)?,
                        r.get(1)?,
                        r.get(2)?,
                        r.get(3)?,
                        r.get(4)?,
                        r.get(5)?,
                        r.get(6)?,
                        r.get(7)?,
                        r.get(8)?,
                    ))
                },
            )
            .optional()?;
        let Some((session, state, goal, provider, model, output, reason, confirmed, created)) =
            detail
        else {
            return Ok(None);
        };
        result["run"] = json!({"runId":run_id,"sessionId":session,"runType":"agent","state":state,"goalSummary":preview(&goal,4096),"providerId":provider,"modelId":model,"finalAnswer":preview(&output,4096),"failure":reason,"terminalReason":reason,"confirmed":confirmed,"startedAt":created.clone().unwrap_or_default(),"cursorTime":created.unwrap_or_else(|| "1970-01-01T00:00:00.000Z".into())});
        let mut plan_query = self.db.prepare(
            "SELECT step,status,reason,operation,dependencies FROM plan_steps WHERE run_id=?1 ORDER BY step LIMIT 5",
        )?;
        let plan_rows = plan_query
            .query_map([run_id], |r| {
                Ok((
                    r.get::<_, i64>(0)?,
                    r.get::<_, String>(1)?,
                    r.get::<_, String>(2)?,
                    r.get::<_, Option<String>>(3)?,
                    r.get::<_, Option<String>>(4)?,
                ))
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        if plan_rows.len() > 4 {
            return Err(rusqlite::Error::InvalidQuery);
        }
        result["steps"] = Value::Array(plan_rows.into_iter().map(|(step,status,reason,operation,dependencies)| { let deps = dependencies.and_then(|d| serde_json::from_str::<Value>(&d).ok()); json!({"stepId":format!("{run_id}-plan-{step}"),"runId":run_id,"sequence":step,"status":status,"stepType":"plan","reason":reason,"operation":operation,"dependsOn":deps}) }).collect());
        if goal.len() > 4096 || output.len() > 4096 {
            result["truncated"] = json!(true);
        }
        let mut stmt = self.db.prepare("SELECT step,tool,path,content,complete,arguments,descriptor_version,failure_category FROM observations WHERE run_id=?1 ORDER BY step LIMIT 27")?;
        let observations = stmt
            .query_map([run_id], |r| {
                Ok((
                    r.get::<_, i64>(0)?,
                    r.get::<_, String>(1)?,
                    r.get::<_, String>(2)?,
                    r.get::<_, String>(3)?,
                    r.get::<_, bool>(4)?,
                    r.get::<_, Option<String>>(5)?,
                    r.get::<_, Option<String>>(6)?,
                    r.get::<_, Option<String>>(7)?,
                ))
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        if observations.len() > 26 {
            result["truncated"] = json!(true);
        }
        let mut tools = Vec::new();
        let mut evidence = Vec::new();
        for (step, tool, path, content, complete, arguments, version, failure_category) in
            observations.into_iter().take(26)
        {
            if path.len() > 512
                || content.len() > 1024
                || arguments.as_ref().is_some_and(|v| v.len() > 1024)
            {
                result["truncated"] = json!(true);
            }
            let call_id = format!("{run_id}-step-{step}");
            if complete {
                evidence.push(json!({"toolCallId":call_id,"domain":if tool=="process-exec" {"process"} else {"filesystem"},"resourceSummary":preview(&path,512),"outcome":0}));
            }
            tools.push(json!({"toolCallId":call_id,"sequence":step,"toolId":tool,"descriptorVersion":version,"failureCategory":failure_category,"arguments":arguments.as_deref().map(|v|preview(v,1024)),"resourceSummary":preview(&path,512),"observationSummary":preview(&content,1024),"complete":complete,"previewTruncated":path.len()>512 || content.len()>1024 || arguments.as_ref().is_some_and(|v|v.len()>1024)}));
        }
        result["tools"] = Value::Array(tools);
        result["evidence"] = Value::Array(evidence);
        let mut stmt = self.db.prepare("SELECT id,step,tool,args,decision FROM approvals WHERE run_id=?1 ORDER BY step LIMIT 27")?;
        let approvals = stmt
            .query_map([run_id], |r| {
                Ok((
                    r.get::<_, String>(0)?,
                    r.get::<_, i64>(1)?,
                    r.get::<_, String>(2)?,
                    r.get::<_, String>(3)?,
                    r.get::<_, String>(4)?,
                ))
            })?
            .collect::<rusqlite::Result<Vec<_>>>()?;
        if approvals.len() > 26 {
            result["truncated"] = json!(true);
        }
        let mut entries = Vec::new();
        for (id, step, tool, args, decision) in approvals.into_iter().take(26) {
            if args.len() > 512 {
                result["truncated"] = json!(true);
            }
            entries.push(json!({"approvalId":id,"toolCallId":format!("{run_id}-step-{step}"),"sequence":step,"toolId":tool,"domain":if tool=="process-exec" {"process"} else {"filesystem"},"access":if tool=="process-exec" {"execute"} else if tool=="write-file" || tool=="edit-file" {"write"} else {"read"},"resourceSummary":preview(&args,512),"decision":decision,"scope":"one-invocation","previewTruncated":args.len()>512}));
        }
        result["authorizations"] = Value::Array(entries);
        Ok(Some(bound_history(result)?))
    }
}

fn preview(text: &str, bytes: usize) -> &str {
    &text[..text.floor_char_boundary(text.len().min(bytes))]
}

fn bound_history(mut result: Value) -> rusqlite::Result<Value> {
    while serde_json::to_vec(&result)
        .map_err(|_| rusqlite::Error::InvalidQuery)?
        .len()
        > 200_000
    {
        let mut removed = false;
        for key in ["evidence", "tools", "authorizations", "steps", "runs"] {
            if let Some(rows) = result[key].as_array_mut()
                && rows.pop().is_some()
            {
                removed = true;
                break;
            }
        }
        if !removed {
            return Err(rusqlite::Error::InvalidQuery);
        }
        result["truncated"] = json!(true);
    }
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs, os::unix::fs::DirBuilderExt};
    #[test]
    fn history_filters_session_and_exposes_bounded_real_audit() {
        let dir =
            std::env::temp_dir().join(format!("sentinel-agent-history-{}", std::process::id()));
        fs::DirBuilder::new().mode(0o700).create(&dir).unwrap();
        let dir = dir.canonicalize().unwrap();
        let mut audit = AgentStore::open(&dir).unwrap();
        audit.start("r1", "s1", "goal", "p", "m").unwrap();
        audit.start("r2", "s2", "secret", "p", "m").unwrap();
        audit
            .intention(
                "r1",
                1,
                "read-file",
                "dev-1",
                "{\"path\":\"x\",\"offset\":1,\"limit\":2000}",
            )
            .unwrap();
        audit
            .request_approval(
                "r1",
                "a1",
                1,
                "read-file",
                "{\"path\":\"x\",\"offset\":1,\"limit\":2000}",
            )
            .unwrap();
        audit
            .resolve_approval("r1", "a1", ApprovalResolution::Allow)
            .unwrap();
        audit
            .observe("r1", 1, "x", &"z".repeat(2000), true)
            .unwrap();
        let scope = vec!["s1".to_owned()];
        let history = audit.history(&scope, "r1", "", "").unwrap().unwrap();
        assert_eq!(history["runs"].as_array().unwrap().len(), 1);
        assert_eq!(history["run"]["runId"], "r1");
        assert_eq!(history["tools"][0]["resourceSummary"], "x");
        assert_eq!(history["tools"][0]["descriptorVersion"], "dev-1");
        assert!(
            history["tools"][0]["arguments"]
                .as_str()
                .unwrap()
                .contains("\"offset\":1")
        );
        assert_eq!(history["authorizations"][0]["decision"], "allow");
        assert_eq!(history["truncated"], true);
        assert!(audit.history(&scope, "r2", "", "").unwrap().is_none());
        drop(audit);
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn allowed_write_is_reported_uncertain_after_restart() {
        let dir =
            std::env::temp_dir().join(format!("sentinel-write-recovery-{}", std::process::id()));
        fs::DirBuilder::new().mode(0o700).create(&dir).unwrap();
        let dir = dir.canonicalize().unwrap();
        let mut chat = store::Store::open_deferred(&dir).unwrap();
        chat.create("s", "title").unwrap();
        assert!(
            chat.begin_with_provider("s", "r", "goal", "m", "p")
                .unwrap()
        );
        let mut audit = AgentStore::open(&dir).unwrap();
        audit.start("r", "s", "goal", "p", "m").unwrap();
        let args = r#"{"path":"x","content":"value"}"#;
        audit
            .intention("r", 1, "write-file", "dev-1", args)
            .unwrap();
        audit
            .request_approval("r", "a", 1, "write-file", args)
            .unwrap();
        audit
            .resolve_approval("r", "a", ApprovalResolution::Allow)
            .unwrap();
        drop(audit);
        let audit = AgentStore::open(&dir).unwrap();
        store::Store::recover_agent_runs(&dir).unwrap();
        let session = chat.load().unwrap().remove(0);
        assert_eq!(session.detail, "write-outcome-unknown");
        assert_eq!(chat.messages("s").unwrap()[1]["status"], "interrupted");
        store::Store::recover_agent_runs(&dir).unwrap();
        assert_eq!(chat.messages("s").unwrap().len(), 2);
        let (reason, output): (String, String) = audit
            .db
            .query_row("SELECT reason,output FROM runs WHERE id='r'", [], |r| {
                Ok((r.get(0)?, r.get(1)?))
            })
            .unwrap();
        assert_eq!(reason, "write-outcome-unknown");
        assert!(output.contains("may have been created or modified"));
        drop(audit);
        drop(chat);
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn approval_is_one_use_scoped_and_restart_denies_pending() {
        let dir =
            std::env::temp_dir().join(format!("sentinel-approval-test-{}", std::process::id()));
        fs::DirBuilder::new().mode(0o700).create(&dir).unwrap();
        let dir = dir.canonicalize().unwrap();
        let mut chat = store::Store::open_deferred(&dir).unwrap();
        chat.create("s", "title").unwrap();
        assert!(
            chat.begin_with_provider("s", "r", "goal", "m", "p")
                .unwrap()
        );
        let mut agent = AgentStore::open(&dir).unwrap();
        agent.start("r", "s", "goal", "p", "m").unwrap();
        let read_args = r#"{"path":"x","offset":1,"limit":2000}"#;
        agent
            .intention("r", 1, "read-file", "dev-1", read_args)
            .unwrap();
        assert!(
            agent
                .request_approval("r", "wrong-args", 1, "read-file", "{}")
                .is_err()
        );
        assert!(
            agent
                .request_approval("r", "wrong-step", 2, "read-file", read_args)
                .is_err()
        );
        agent
            .request_approval("r", "one", 1, "read-file", read_args)
            .unwrap();
        assert!(
            agent
                .resolve_approval("wrong", "one", ApprovalResolution::Allow)
                .is_err()
        );
        agent
            .resolve_approval("r", "one", ApprovalResolution::Allow)
            .unwrap();
        assert!(
            agent
                .resolve_approval("r", "one", ApprovalResolution::Allow)
                .is_err()
        );
        let list_args = r#"{"path":".","includeHidden":false}"#;
        agent
            .intention("r", 2, "list-directory", "dev-1", list_args)
            .unwrap();
        agent
            .request_approval("r", "two", 2, "list-directory", list_args)
            .unwrap();
        store::Store::recover_agent_runs(&dir).unwrap();
        let decision: String = agent
            .db
            .query_row("SELECT decision FROM approvals WHERE id='two'", [], |r| {
                r.get(0)
            })
            .unwrap();
        assert_eq!(decision, "denied-restart");
        assert!(
            agent
                .resolve_approval("r", "two", ApprovalResolution::Allow)
                .is_err()
        );
        drop(agent);
        drop(chat);
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn v1_audit_migrates_without_losing_runs() {
        use std::os::unix::fs::PermissionsExt;
        let dir =
            std::env::temp_dir().join(format!("sentinel-approval-upgrade-{}", std::process::id()));
        fs::DirBuilder::new().mode(0o700).create(&dir).unwrap();
        let dir = dir.canonicalize().unwrap();
        let path = dir.join(DATABASE);
        let db = Connection::open(&path).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        db.execute_batch(&format!("PRAGMA application_id={APP_ID}; PRAGMA user_version=1; CREATE TABLE runs(id TEXT PRIMARY KEY,session_id TEXT NOT NULL,state TEXT NOT NULL,goal TEXT NOT NULL,provider_id TEXT NOT NULL,model TEXT NOT NULL,output TEXT NOT NULL DEFAULT '',reason TEXT NOT NULL DEFAULT '',confirmed INTEGER NOT NULL DEFAULT 0); CREATE TABLE observations(run_id TEXT NOT NULL REFERENCES runs(id),step INTEGER NOT NULL,tool TEXT NOT NULL,path TEXT NOT NULL,content TEXT NOT NULL,complete INTEGER NOT NULL,PRIMARY KEY(run_id,step)); INSERT INTO runs(id,session_id,state,goal,provider_id,model) VALUES('r','s','running','g','p','m');")).unwrap();
        drop(db);
        let mut audit = AgentStore::open(&dir).unwrap();
        assert!(audit.contains("r").unwrap());
        let args = r#"{"path":"x","offset":1,"limit":2000}"#;
        audit.intention("r", 1, "read-file", "dev-1", args).unwrap();
        audit
            .request_approval("r", "a", 1, "read-file", args)
            .unwrap();
        assert_eq!(
            audit
                .db
                .query_row("PRAGMA user_version", [], |r| r.get::<_, i64>(0))
                .unwrap(),
            7
        );
        drop(audit);
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn v3_audit_migration_does_not_invent_invocation_arguments() {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join(format!("sentinel-agent-v3-{}", std::process::id()));
        fs::DirBuilder::new().mode(0o700).create(&dir).unwrap();
        let dir = dir.canonicalize().unwrap();
        let path = dir.join(DATABASE);
        let db = Connection::open(&path).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        db.execute_batch(&format!("PRAGMA application_id={APP_ID}; PRAGMA user_version=3; CREATE TABLE runs(id TEXT PRIMARY KEY,session_id TEXT NOT NULL,state TEXT NOT NULL,goal TEXT NOT NULL,provider_id TEXT NOT NULL,model TEXT NOT NULL,output TEXT NOT NULL DEFAULT '',reason TEXT NOT NULL DEFAULT '',confirmed INTEGER NOT NULL DEFAULT 0,created_at TEXT); CREATE TABLE observations(run_id TEXT NOT NULL REFERENCES runs(id),step INTEGER NOT NULL,tool TEXT NOT NULL,path TEXT NOT NULL,content TEXT NOT NULL,complete INTEGER NOT NULL,PRIMARY KEY(run_id,step)); {APPROVAL_SCHEMA} INSERT INTO runs(id,session_id,state,goal,provider_id,model) VALUES('r','s','running','g','p','m'); INSERT INTO observations VALUES('r',1,'read-file','old-path','old-content',1);" )).unwrap();
        drop(db);
        let audit = AgentStore::open(&dir).unwrap();
        let history = audit.history(&["s".into()], "r", "", "").unwrap().unwrap();
        assert!(history["tools"][0]["arguments"].is_null());
        assert!(history["tools"][0]["descriptorVersion"].is_null());
        assert_eq!(history["tools"][0]["resourceSummary"], "old-path");
        drop(audit);
        fs::remove_dir_all(dir).unwrap();
    }
    #[test]
    fn unconfirmed_terminal_is_recovered_without_false_completion() {
        let dir =
            std::env::temp_dir().join(format!("sentinel-agent-recovery-{}", std::process::id()));
        fs::DirBuilder::new().mode(0o700).create(&dir).unwrap();
        let dir = dir.canonicalize().unwrap();
        let mut chat = store::Store::open(&dir).unwrap();
        chat.create("s", "title").unwrap();
        let mut agent = AgentStore::open(&dir).unwrap();
        agent.start("r", "s", "goal", "ollama", "model").unwrap();
        chat.begin_with_provider("s", "r", "goal", "model", "ollama")
            .unwrap();
        agent
            .intention(
                "r",
                1,
                "read-file",
                "dev-1",
                "{\"path\":\"x\",\"offset\":1,\"limit\":2000}",
            )
            .unwrap();
        agent.observe("r", 1, "x", "evidence", true).unwrap();
        agent.finish("r", "completed", "evidence", "").unwrap();
        chat.finish("s", "r", "completed", "evidence", "model")
            .unwrap();
        drop(agent);
        drop(chat);
        let chat = store::Store::open_deferred(&dir).unwrap();
        let agent = AgentStore::open(&dir).unwrap();
        store::Store::recover_agent_runs(&dir).unwrap();
        assert_eq!(chat.load().unwrap()[0].state, "failed");
        let (state, confirmed): (String, i64) = agent
            .db
            .query_row("SELECT state,confirmed FROM runs WHERE id='r'", [], |r| {
                Ok((r.get(0)?, r.get(1)?))
            })
            .unwrap();
        assert_eq!((state.as_str(), confirmed), ("failed", 1));
        drop(agent);
        drop(chat);
        fs::remove_dir_all(dir).unwrap();
    }
}
