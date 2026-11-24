//! Windows development Chat with bounded, isolated history snapshots.
use super::{Peer, invalid};
use sentinel_daemon_dev::provider::{self, Config, PreparedRequest, ProviderError, Turn};
use sentinel_ipc::{
    Envelope, MessageType, Version,
    contract::{self, RequestPayload, ResponsePayload},
};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::{
    collections::HashSet,
    io,
    sync::{
        Arc, Mutex, Weak,
        atomic::{AtomicBool, Ordering},
        mpsc::Receiver,
    },
    time::{SystemTime, UNIX_EPOCH},
};

const MAX_SESSIONS: usize = 32;
const MAX_TURNS: usize = 16;
const MAX_PROMPT_BYTES: usize = 8192;
const MAX_OUTPUT_BYTES: usize = 24 * 1024;
const MAX_HISTORY_BYTES: usize = 96 * 1024;

pub struct Job {
    session_id: String,
    run_id: String,
    config: Config,
    request: PreparedRequest,
    cancel: Arc<AtomicBool>,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Run {
    id: String,
    prompt: String,
    output: String,
    state: String,
    provider_id: String,
    model_id: String,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Session {
    id: String,
    title: String,
    created_at: String,
    #[serde(skip)]
    sequence: u64,
    runs: Vec<Run>,
    #[serde(skip)]
    peers: Vec<Weak<Peer>>,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Snapshot {
    version: u32,
    sessions: Vec<Session>,
}

pub struct State {
    generation: String,
    sessions: Vec<Session>,
    active: Option<(String, Arc<AtomicBool>)>,
    counter: u64,
    config: Option<Config>,
    profile: Arc<crate::windows_profile::ProfileGuard>,
    catalog: Option<Vec<String>>,
    catalog_epoch: u64,
    storage_failed: bool,
}

impl State {
    pub fn load(
        config: Option<Config>,
        profile: Arc<crate::windows_profile::ProfileGuard>,
    ) -> io::Result<Self> {
        let mut repaired = false;
        let sessions = if let Some(bytes) = profile.load_chat()? {
            let snapshot = serde_json::from_slice::<Snapshot>(&bytes)?;
            let mut sessions = validate_snapshot(snapshot)?;
            for run in sessions.iter_mut().flat_map(|session| &mut session.runs) {
                if run.state == "running" {
                    run.state = "interrupted".into();
                    repaired = true;
                }
            }
            sessions
        } else {
            Vec::new()
        };
        let state = Self {
            generation: format!("windows-dev-{}-{}", std::process::id(), timestamp()),
            sessions,
            active: None,
            counter: 0,
            config,
            profile,
            catalog: None,
            catalog_epoch: 0,
            storage_failed: false,
        };
        if repaired {
            state.save()?;
        }
        Ok(state)
    }

    fn save(&self) -> io::Result<()> {
        #[derive(Serialize)]
        struct SnapshotRef<'a> {
            version: u32,
            sessions: &'a [Session],
        }
        let bytes = serde_json::to_vec(&SnapshotRef {
            version: 1,
            sessions: &self.sessions,
        })?;
        let pending_output = self.sessions.iter().any(|session| {
            session
                .runs
                .last()
                .is_some_and(|run| run.state == "running")
        });
        let reserved = if pending_output {
            MAX_OUTPUT_BYTES * 6 + 16
        } else {
            0
        };
        if (bytes.len() + reserved) as u64 > crate::windows_profile::MAX_CHAT_FILE_BYTES {
            return Err(io::Error::new(
                io::ErrorKind::FileTooLarge,
                "Windows Chat snapshot limit",
            ));
        }
        self.profile.save_chat(&bytes)
    }

    pub fn has_provider(&self) -> bool {
        self.config.is_some()
    }

    pub fn start_discovery(&mut self) -> Result<(Config, u64), &'static str> {
        let config = self.config.clone().ok_or("provider-unavailable")?;
        self.catalog_epoch = self.catalog_epoch.checked_add(1).ok_or("limit-exceeded")?;
        Ok((config, self.catalog_epoch))
    }

    pub fn install_catalog(
        &mut self,
        models: Vec<String>,
        epoch: u64,
    ) -> Result<ResponsePayload, &'static str> {
        if epoch != self.catalog_epoch {
            return Err("runtime-busy");
        }
        let config = self.config.as_ref().ok_or("provider-unavailable")?;
        let rows: Vec<_> = models
            .iter()
            .map(|id| json!({"provider_id":config.provider_id,"model_id":id}))
            .collect();
        if serde_json::to_vec(&rows)
            .map_err(|_| "runtime-unavailable")?
            .len()
            > contract::MAX_FRAME_BYTES - 2048
        {
            return Err("message-too-large");
        }
        self.catalog = Some(models);
        Ok(ResponsePayload::ModelList { models: rows })
    }

    pub fn counts(&self) -> (u64, u64) {
        (u64::from(self.active.is_some()), self.sessions.len() as u64)
    }

    pub fn cancel_active(&self) {
        if let Some((_, cancel)) = &self.active {
            cancel.store(true, Ordering::Relaxed);
        }
    }

    fn id(&mut self, kind: &str) -> Result<String, &'static str> {
        self.counter = self.counter.checked_add(1).ok_or("limit-exceeded")?;
        Ok(format!("{kind}-{}-{}", self.generation, self.counter))
    }

    fn subscribe(session: &mut Session, peer: &Arc<Peer>) -> Result<(), &'static str> {
        session
            .peers
            .retain(|subscriber| subscriber.strong_count() > 0);
        let subscriber = Arc::downgrade(peer);
        if !session.peers.iter().any(|other| other.ptr_eq(&subscriber)) {
            if session.peers.len() >= super::MAX_CLIENTS {
                return Err("runtime-busy");
            }
            session.peers.push(subscriber);
        }
        Ok(())
    }

    pub fn dispatch(
        &mut self,
        request: RequestPayload,
        peer: &Arc<Peer>,
        job: &mut Option<Job>,
    ) -> Result<ResponsePayload, &'static str> {
        match request {
            RequestPayload::ModelCurrent {} => {
                let config = self.config.as_ref().ok_or("provider-unavailable")?;
                Ok(ResponsePayload::ModelCurrent {
                    provider_id: config.provider_id.clone(),
                    model_id: config.model.clone(),
                })
            }
            RequestPayload::ModelSelect {
                provider_id,
                model_id,
            } => {
                if self.active.is_some() {
                    return Err("runtime-busy");
                }
                let config = self
                    .config
                    .as_mut()
                    .filter(|config| config.provider_id == provider_id)
                    .ok_or("provider-unavailable")?;
                if !self
                    .catalog
                    .as_ref()
                    .is_some_and(|catalog| catalog.contains(&model_id))
                {
                    return Err("model-unavailable");
                }
                self.profile
                    .save_model(config, &model_id)
                    .map_err(|_| "storage-failure")?;
                config.model = model_id.clone();
                Ok(ResponsePayload::ModelSelect {
                    provider_id,
                    model_id,
                })
            }
            RequestPayload::ProviderSelect { provider_id } => {
                let config = self
                    .config
                    .as_ref()
                    .filter(|config| config.provider_id == provider_id)
                    .ok_or("provider-unavailable")?;
                Ok(ResponsePayload::ProviderSelect {
                    provider_id,
                    model_id: config.model.clone(),
                })
            }
            RequestPayload::TerminalState {} => Ok(ResponsePayload::TerminalState {
                properties: json!({}),
                providers: self
                    .config
                    .iter()
                    .map(|config| {
                        json!({
                            "provider_id":config.provider_id,
                            "health":"Unknown",
                            "catalog":if self.catalog.is_some() { "Cached" } else { "Unknown" },
                            "local":true,
                            "selected":true,
                            "models":self.catalog.iter().flatten().take(16).collect::<Vec<_>>(),
                        })
                    })
                    .collect(),
                workspace: json!({}),
                workspaces: Vec::new(),
            }),
            RequestPayload::SessionList {} => Ok(ResponsePayload::SessionList {
                sessions: self
                    .sessions
                    .iter()
                    .rev()
                    .map(|session| {
                        json!({
                            "session_id":session.id,
                            "title":session.title,
                            "created_at":session.created_at,
                            "state":session.runs.last().map_or("idle", |run| run.state.as_str()),
                        })
                    })
                    .collect(),
            }),
            RequestPayload::SessionCreate { title } => {
                if self.active.is_some() {
                    return Err("runtime-busy");
                }
                if self.storage_failed {
                    return Err("storage-failure");
                }
                if self.sessions.len() >= MAX_SESSIONS || title.len() > 1024 || title.contains('\0')
                {
                    return Err("limit-exceeded");
                }
                let id = self.id("session")?;
                let created_at = timestamp();
                let reply = typed(
                    "session.create",
                    json!({
                        "session_id":id,"title":title,"type":"chat","state":"idle","created_at":created_at,
                        "server_generation":self.generation,"event_sequence":0,
                    }),
                )?;
                self.sessions.push(Session {
                    id,
                    title,
                    created_at,
                    sequence: 0,
                    runs: vec![],
                    peers: vec![],
                });
                if let Err(error) = self.save() {
                    self.sessions.pop();
                    if error.kind() == io::ErrorKind::FileTooLarge {
                        return Err("limit-exceeded");
                    }
                    self.storage_failed = true;
                    return Err("storage-failure");
                }
                Ok(reply)
            }
            RequestPayload::SessionAttach { ref session_id }
            | RequestPayload::TerminalAttach { ref session_id } => {
                let name = if matches!(request, RequestPayload::SessionAttach { .. }) {
                    "session.attach"
                } else {
                    "terminal.attach"
                };
                let session = self
                    .sessions
                    .iter_mut()
                    .find(|session| &session.id == session_id)
                    .ok_or("unknown-session")?;
                Self::subscribe(session, peer)?;
                let run = session.runs.last();
                typed(
                    name,
                    json!({
                        "session_id":session_id,"title":session.title,"type":"chat","run_type":"chat",
                        "state":run.map_or("idle", |run| run.state.as_str()),
                        "output":run.map_or("", |run| run.output.as_str()),
                        "run_id":run.map_or("", |run| run.id.as_str()),"server_generation":self.generation,
                        "event_sequence":session.sequence,"created_at":session.created_at,
                        "provider_id":run.map(|run| run.provider_id.as_str()),
                        "model_id":run.map(|run| run.model_id.as_str()),
                    }),
                )
            }
            RequestPayload::DesktopAction {
                action,
                arguments,
                session_id,
            } if matches!(
                action.as_str(),
                "renameConversation" | "requestPermanentDeleteConversation"
            ) =>
            {
                if self.active.is_some() {
                    return Err("runtime-busy");
                }
                if self.storage_failed {
                    return Err("storage-failure");
                }
                if arguments.first().and_then(Value::as_str) != Some(session_id.as_str()) {
                    return Err("invalid-payload");
                }
                let index = self
                    .sessions
                    .iter()
                    .position(|session| session.id == session_id)
                    .ok_or("unknown-session")?;
                match (action.as_str(), arguments.as_slice()) {
                    ("renameConversation", [_, title]) => {
                        let title = title
                            .as_str()
                            .filter(|title| title.len() <= 1024 && !title.contains('\0'))
                            .ok_or("invalid-payload")?
                            .trim();
                        let title = if title.is_empty() {
                            "Untitled Conversation"
                        } else {
                            title
                        };
                        let previous =
                            std::mem::replace(&mut self.sessions[index].title, title.into());
                        if self.save().is_err() {
                            self.sessions[index].title = previous;
                            self.storage_failed = true;
                            return Err("storage-failure");
                        }
                    }
                    ("requestPermanentDeleteConversation", [_]) => {
                        let removed = self.sessions.remove(index);
                        if self.save().is_err() {
                            self.sessions.insert(index, removed);
                            self.storage_failed = true;
                            return Err("storage-failure");
                        }
                    }
                    _ => return Err("invalid-payload"),
                }
                typed(
                    "desktop.action",
                    json!({"accepted":true,"result":{"value":true}}),
                )
            }
            RequestPayload::SessionMessages { session_id } => {
                let session = self
                    .sessions
                    .iter()
                    .find(|session| session.id == session_id)
                    .ok_or("unknown-session")?;
                let mut messages = Vec::new();
                for (index, run) in session.runs.iter().enumerate() {
                    let id = index * 2 + 1;
                    messages.push(json!({"id":id,"role":"user","content":run.prompt,"status":"sent","partial":false}));
                    if run.state != "running" {
                        messages.push(json!({"id":id + 1,"role":"assistant","content":run.output,
                            "status":run.state,"partial":run.state != "completed","reply_to":id}));
                    }
                }
                Ok(ResponsePayload::SessionMessages {
                    session_id,
                    messages,
                })
            }
            RequestPayload::ChatSend {
                session_id,
                text,
                attachments,
            } => {
                if self.storage_failed {
                    return Err("storage-failure");
                }
                if attachments.is_some_and(|items| !items.is_empty()) {
                    return Err("invalid-attachments");
                }
                if text.trim().is_empty() || text.len() > MAX_PROMPT_BYTES || text.contains('\0') {
                    return Err("invalid-payload");
                }
                if self.active.is_some() {
                    return Err("runtime-busy");
                }
                let config = self.config.clone().ok_or("provider-unavailable")?;
                let session = self
                    .sessions
                    .iter()
                    .find(|session| session.id == session_id)
                    .ok_or("unknown-session")?;
                let used: usize = session
                    .runs
                    .iter()
                    .map(|run| escaped_len(&run.prompt) + escaped_len(&run.output))
                    .sum();
                if session.runs.len() >= MAX_TURNS || used + escaped_len(&text) > MAX_HISTORY_BYTES
                {
                    return Err("limit-exceeded");
                }
                let turns: Vec<_> = session
                    .runs
                    .iter()
                    .filter(|run| run.state == "completed")
                    .map(|run| Turn {
                        user: run.prompt.clone(),
                        assistant: run.output.clone(),
                        images: Vec::new(),
                    })
                    .collect();
                let request =
                    provider::prepare(&config, &text, &turns).map_err(|error| match error {
                        ProviderError::ContextLimitExceeded => "context-limit-exceeded",
                        _ => "invalid-payload",
                    })?;
                let run_id = self.id("run")?;
                let index = self
                    .sessions
                    .iter()
                    .position(|session| session.id == session_id)
                    .ok_or("unknown-session")?;
                Self::subscribe(&mut self.sessions[index], peer)?;
                self.sessions[index].runs.push(Run {
                    id: run_id.clone(),
                    prompt: text,
                    output: String::new(),
                    state: "running".into(),
                    provider_id: config.provider_id.clone(),
                    model_id: config.model.clone(),
                });
                if let Err(error) = self.save() {
                    self.sessions[index].runs.pop();
                    if error.kind() == io::ErrorKind::FileTooLarge {
                        return Err("limit-exceeded");
                    }
                    self.storage_failed = true;
                    return Err("storage-failure");
                }
                let cancel = Arc::new(AtomicBool::new(false));
                self.active = Some((run_id.clone(), cancel.clone()));
                *job = Some(Job {
                    session_id: session_id.clone(),
                    run_id: run_id.clone(),
                    config,
                    request,
                    cancel,
                });
                Ok(ResponsePayload::ChatSend { session_id, run_id })
            }
            RequestPayload::RunCancel { run_id } => {
                let accepted = self.active.as_ref().is_some_and(|(id, cancel)| {
                    id == &run_id && !cancel.swap(true, Ordering::Relaxed)
                });
                Ok(ResponsePayload::RunCancel { accepted })
            }
            _ => Err("runtime-unavailable"),
        }
    }
}

fn validate_snapshot(snapshot: Snapshot) -> io::Result<Vec<Session>> {
    let invalid_snapshot = || invalid("invalid Windows Chat snapshot");
    let valid_id = |id: &str, prefix: &str| {
        id.len() <= 128
            && id.starts_with(prefix)
            && id
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-')
    };
    if snapshot.version != 1 || snapshot.sessions.len() > MAX_SESSIONS {
        return Err(invalid_snapshot());
    }
    let mut ids = HashSet::new();
    let mut active_seen = false;
    for session in &snapshot.sessions {
        if !valid_id(&session.id, "session-")
            || !ids.insert(session.id.as_str())
            || session.title.len() > 1024
            || session.title.contains('\0')
            || session.created_at.parse::<u128>().is_err()
            || session.runs.len() > MAX_TURNS
        {
            return Err(invalid_snapshot());
        }
        let mut wire_bytes = 0;
        for (index, run) in session.runs.iter().enumerate() {
            if !valid_id(&run.id, "run-")
                || !ids.insert(run.id.as_str())
                || run.prompt.trim().is_empty()
                || run.prompt.len() > MAX_PROMPT_BYTES
                || run.prompt.contains('\0')
                || run.output.len() > MAX_OUTPUT_BYTES
                || !matches!(
                    run.state.as_str(),
                    "running" | "completed" | "cancelled" | "failed" | "interrupted"
                )
                || !Config::valid_persisted_selection(
                    "http://127.0.0.1:1",
                    &run.provider_id,
                    &run.model_id,
                )
            {
                return Err(invalid_snapshot());
            }
            if run.state == "running" {
                if active_seen || index + 1 != session.runs.len() || !run.output.is_empty() {
                    return Err(invalid_snapshot());
                }
                active_seen = true;
            }
            if run.state == "completed" && run.output.trim().is_empty() {
                return Err(invalid_snapshot());
            }
            wire_bytes += escaped_len(&run.prompt) + escaped_len(&run.output);
        }
        if wire_bytes > MAX_HISTORY_BYTES + MAX_OUTPUT_BYTES * 6 + 2 {
            return Err(invalid_snapshot());
        }
    }
    drop(ids);
    Ok(snapshot.sessions)
}

fn escaped_len(text: &str) -> usize {
    serde_json::to_vec(text).map_or(contract::MAX_FRAME_BYTES, |encoded| encoded.len())
}

fn typed(name: &str, payload: Value) -> Result<ResponsePayload, &'static str> {
    serde_json::from_value(json!({"name":name,"payload":payload}))
        .map_err(|_| "runtime-unavailable")
}

fn timestamp() -> String {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis()
        .to_string()
}

pub fn worker(state: Arc<Mutex<State>>, jobs: Receiver<Job>) {
    for job in jobs {
        let _ = emit(&state, &job, "run.started", "", None);
        let result = provider::stream_prepared(&job.config, &job.request, &job.cancel, |delta| {
            emit(&state, &job, "output.delta", delta, None)
        });
        let (event, terminal) = if job.cancel.load(Ordering::Relaxed) {
            ("run.cancelled", "cancelled")
        } else {
            match result {
                Ok(_) => ("run.completed", "completed"),
                Err(_) => ("run.failed", "failed"),
            }
        };
        let _ = emit(&state, &job, event, "", Some(terminal));
    }
}

pub fn reject_unacknowledged(state: &Mutex<State>, job: &Job) {
    let _ = emit(state, job, "run.failed", "", Some("failed"));
}

fn emit(
    state: &Mutex<State>,
    job: &Job,
    name: &str,
    delta: &str,
    terminal: Option<&'static str>,
) -> io::Result<()> {
    let (peers, event) = {
        let mut state = state
            .lock()
            .map_err(|_| invalid("Chat state lock failed"))?;
        if state
            .active
            .as_ref()
            .is_none_or(|(id, _)| id != &job.run_id)
        {
            return Err(invalid("stale Chat worker"));
        }
        let (mut name, terminal) = if job.cancel.load(Ordering::Relaxed) {
            if terminal.is_some() {
                ("run.cancelled", Some("cancelled"))
            } else {
                return Err(io::Error::new(io::ErrorKind::Interrupted, "Chat cancelled"));
            }
        } else {
            (name, terminal)
        };
        let index = state
            .sessions
            .iter()
            .position(|session| session.id == job.session_id)
            .ok_or_else(|| invalid("missing Chat session"))?;
        let generation = state.generation.clone();
        let (sequence, peers) = {
            let session = &mut state.sessions[index];
            let run = session
                .runs
                .last_mut()
                .filter(|run| run.id == job.run_id)
                .ok_or_else(|| invalid("missing Chat run"))?;
            if run.output.len() + delta.len() > MAX_OUTPUT_BYTES {
                return Err(invalid("Chat output exceeds byte limit"));
            }
            run.output.push_str(delta);
            if let Some(terminal) = terminal {
                run.state = terminal.into();
            }
            session.sequence += 1;
            let peers: Vec<_> = session.peers.iter().filter_map(Weak::upgrade).collect();
            (session.sequence, peers)
        };
        let mut payload = json!({
            "session_id":job.session_id,"run_id":job.run_id,
            "event_sequence":sequence,"server_generation":generation,
        });
        if let Some(mut terminal) = terminal {
            let saved = state.save();
            state.active = None;
            if saved.is_err() {
                state.storage_failed = true;
                name = "run.failed";
                terminal = "failed";
                payload["terminal_reason"] = json!("storage-failure");
            } else if terminal == "failed" {
                payload["terminal_reason"] = json!("provider-failed");
            }
            let run = state.sessions[index]
                .runs
                .last_mut()
                .ok_or_else(|| invalid("missing Chat run"))?;
            run.state = terminal.into();
            payload["state"] = json!(terminal);
            payload["text"] = json!(run.output);
        } else if name == "output.delta" {
            payload["text"] = json!(delta);
        }
        (
            peers,
            Envelope {
                version: Version {
                    major: contract::MAJOR,
                    minor: contract::MINOR,
                },
                kind: MessageType::Event,
                id: String::new(),
                name: name.into(),
                payload,
            },
        )
    };
    for peer in peers {
        let _ = peer.write(&event);
    }
    Ok(())
}
