//! Isolated development chat state. No production stores or tool authority.
use crate::{
    provider::{self, Config, ProviderError},
    store::{self, Store},
};
use sentinel_ipc::{
    Envelope, MessageType, Version,
    contract::{self, RequestPayload, ResponsePayload},
};
use serde_json::{Value, json};
use std::{
    io,
    path::PathBuf,
    sync::{
        Arc, Mutex, Weak,
        atomic::{AtomicBool, Ordering},
        mpsc::{SyncSender, TrySendError, sync_channel},
    },
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

type ChangesAccess = (
    Arc<crate::agent_gateway::Workspace>,
    String,
    crate::agent_gateway::WorkspaceSnapshot,
);
const MAX_SESSIONS: usize = 64;
const BACKUP_CHUNK: usize = 48 * 1024;
const BACKUP_LEASE: Duration = Duration::from_secs(600);
struct BackupTransfer {
    id: String,
    owner: Weak<Peer>,
    data: Vec<u8>,
    touched: Instant,
}

// Six-byte JSON escapes plus envelope remain below the canonical wire cap.
const MAX_OUTPUT: usize = store::MAX_OUTPUT;
pub struct Outbound {
    pub bytes: Vec<u8>,
    pub confirmation: Option<SyncSender<bool>>,
}
pub struct Peer {
    pub sender: SyncSender<Outbound>,
    pub close: Box<dyn Fn() + Send + Sync>,
}
impl Drop for Peer {
    fn drop(&mut self) {
        (self.close)();
    }
}
impl Peer {
    pub fn send(&self, bytes: Vec<u8>) -> io::Result<()> {
        self.send_item(Outbound {
            bytes,
            confirmation: None,
        })
    }
    pub fn send_confirm(&self, bytes: Vec<u8>) -> io::Result<()> {
        let (sender, receiver) = sync_channel(1);
        self.send_item(Outbound {
            bytes,
            confirmation: Some(sender),
        })?;
        if receiver
            .recv_timeout(std::time::Duration::from_secs(4))
            .unwrap_or(false)
        {
            Ok(())
        } else {
            Err(io::Error::new(io::ErrorKind::TimedOut, "reply delivery"))
        }
    }
    fn send_item(&self, item: Outbound) -> io::Result<()> {
        match self.sender.try_send(item) {
            Ok(()) => Ok(()),
            Err(TrySendError::Full(_)) | Err(TrySendError::Disconnected(_)) => {
                (self.close)();
                Err(io::Error::new(
                    io::ErrorKind::BrokenPipe,
                    "peer queue unavailable",
                ))
            }
        }
    }
}
struct Session {
    id: String,
    title: String,
    run_id: String,
    state: String,
    output: String,
    model: String,
    provider_id: String,
    detail: String,
    created_at: String,
    updated_at: String,
    seq: u64,
    agent: bool,
    peers: Vec<Weak<Peer>>,
}
struct Active {
    run_id: String,
    session_id: String,
    cancel: Arc<AtomicBool>,
}
enum Revision {
    Regenerate,
    Retry,
    Edit(String),
}
struct PendingApproval {
    run_id: String,
    session_id: String,
    id: String,
    tool: String,
    detail: String,
    resources: Vec<Value>,
    deadline: Instant,
    sender: SyncSender<bool>,
}
const APPROVAL_TIMEOUT: Duration = Duration::from_secs(300);
pub struct ConfiguredWorkspace {
    pub id: String,
    pub root: String,
    pub handle: Arc<crate::agent_gateway::Workspace>,
}
pub struct AgentOptions {
    pub workspaces: Vec<ConfiguredWorkspace>,
    pub selected_workspace: Option<String>,
    pub approval_required: bool,
    pub process_enabled: bool,
}
pub struct State {
    profile: PathBuf,
    generation: String,
    counter: u64,
    sessions: Vec<Session>,
    active: Option<Active>,
    provider: Option<Config>,
    catalog: Vec<String>,
    catalog_known: bool,
    catalog_generation: u64,
    store: Store,
    settings: crate::settings::Settings,
    agent_store: crate::agent_store::AgentStore,
    model_ops: crate::model_ops::Operations,
    workspace: Option<Arc<crate::agent_gateway::Workspace>>,
    plugin_root: Option<Arc<crate::agent_gateway::Workspace>>,
    workspaces: Vec<ConfiguredWorkspace>,
    selected_workspace: Option<String>,
    backup_transfers: Vec<BackupTransfer>,
    change_baselines:
        std::collections::BTreeMap<String, (String, crate::agent_gateway::WorkspaceSnapshot)>,
    approval_required: bool,
    process_enabled: bool,
    pending_approval: Option<PendingApproval>,
    approval_delivery: Option<(SyncSender<bool>, bool)>,
    settings_failed: bool,
    memory: crate::memory::MemoryStore,
    memory_failed: bool,
    storage_failed: bool,
}
pub struct Job {
    session_id: String,
    run_id: String,
    request: provider::PreparedRequest,
    cancel: Arc<AtomicBool>,
    config: Config,
    agent: Option<(Arc<crate::agent_gateway::Workspace>, String)>,
}
impl State {
    pub fn new(
        provider: Option<Config>,
        store: Store,
        settings: crate::settings::Settings,
        agent_store: crate::agent_store::AgentStore,
        memory: crate::memory::MemoryStore,
        agent: AgentOptions,
        profile: PathBuf,
    ) -> Result<Self, Box<dyn std::error::Error>> {
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos();
        let loaded = store.load()?;
        if loaded.len() > MAX_SESSIONS {
            return Err("session limit exceeded".into());
        }
        let sessions = loaded
            .into_iter()
            .map(|s| -> Result<Session, rusqlite::Error> {
                let owner = agent_store.owner(&s.run_id)?;
                if owner.as_ref().is_some_and(|id| id != &s.id) {
                    return Err(rusqlite::Error::InvalidQuery);
                }
                let agent = owner.is_some();
                Ok(Session {
                    id: s.id,
                    title: s.title,
                    run_id: s.run_id,
                    state: s.state,
                    output: s.output,
                    model: s.model,
                    provider_id: s.provider_id,
                    detail: s.detail,
                    created_at: s.created_at,
                    updated_at: s.updated_at,
                    seq: 0,
                    agent,
                    peers: vec![],
                })
            })
            .collect::<Result<Vec<_>, _>>()?;
        let workspace = agent
            .selected_workspace
            .as_ref()
            .and_then(|id| agent.workspaces.iter().find(|w| &w.id == id))
            .map(|w| w.handle.clone());
        Ok(Self {
            profile,
            generation: format!("dev-{}-{now}", std::process::id()),
            counter: 0,
            sessions,
            active: None,
            provider,
            catalog: Vec::new(),
            catalog_known: false,
            catalog_generation: 0,
            store,
            settings,
            agent_store,
            model_ops: crate::model_ops::Operations::default(),
            workspace,
            plugin_root: None,
            workspaces: agent.workspaces,
            selected_workspace: agent.selected_workspace,
            backup_transfers: Vec::new(),
            change_baselines: Default::default(),
            approval_required: agent.approval_required,
            process_enabled: agent.process_enabled,
            pending_approval: None,
            approval_delivery: None,
            settings_failed: false,
            memory,
            memory_failed: false,
            storage_failed: false,
        })
    }
    fn id(&mut self, kind: &str) -> String {
        self.counter += 1;
        format!("{}-{kind}-{}", self.generation, self.counter)
    }
    fn response_profile(&mut self) -> Result<String, &'static str> {
        let values = self.settings.desktop_values().map_err(|error| {
            self.settings_failed = !store::is_conflict(&error);
            "storage-failure"
        })?;
        values["responseProfileInstructions"]
            .as_str()
            .map(str::to_owned)
            .ok_or("storage-failure")
    }
    pub fn set_plugin_root(&mut self, root: Arc<crate::agent_gateway::Workspace>) {
        self.plugin_root = Some(root);
    }
    pub fn plugin_root(&self) -> Option<Arc<crate::agent_gateway::Workspace>> {
        self.plugin_root.clone()
    }
    pub fn has_agent(&self) -> bool {
        self.provider.is_some() && self.workspace.is_some()
    }
    pub fn has_workspace_selection(&self) -> bool {
        !self.workspaces.is_empty()
    }
    fn workspace_description(&self, workspace: &ConfiguredWorkspace) -> Value {
        let permission = match self
            .settings
            .workspace_access(&workspace.id, &workspace.root)
        {
            Ok(false) => "revoked",
            Ok(true) if self.approval_required => "approval-required",
            Ok(true) => "workspace-preauthorized",
            Err(_) => "unknown",
        };
        json!({"id":workspace.id,"name":workspace.id,"root":workspace.root,"access":"configured","permission":permission,"archived":false})
    }
    fn current_workspace(&self) -> Result<Arc<crate::agent_gateway::Workspace>, &'static str> {
        let id = self
            .selected_workspace
            .as_ref()
            .ok_or("runtime-unavailable")?;
        let workspace = self
            .workspaces
            .iter()
            .find(|w| &w.id == id)
            .ok_or("runtime-unavailable")?;
        if self.settings_failed {
            return Err("storage-failure");
        }
        if !self
            .settings
            .workspace_access(&workspace.id, &workspace.root)
            .map_err(|_| "storage-failure")?
        {
            return Err("permission-denied");
        }
        let actual = workspace
            .handle
            .inventory_root()
            .map_err(|_| "workspace-root-rejected")?;
        if actual.to_str() != Some(workspace.root.as_str()) {
            return Err("workspace-root-rejected");
        }
        Ok(workspace.handle.clone())
    }
    pub fn has_workspace_files(&self) -> bool {
        self.workspace.is_some() && !self.approval_required
    }
    pub fn workspace_files_access(
        &self,
        session_id: &str,
    ) -> Result<Arc<crate::agent_gateway::Workspace>, &'static str> {
        if self.storage_failed {
            return Err("storage-failure");
        }
        if !self.sessions.iter().any(|session| session.id == session_id) {
            return Err("unknown-session");
        }
        if self.approval_required {
            return Err("permission-denied");
        }
        self.current_workspace()
    }
    pub fn baseline_candidate(
        &self,
        session_id: &str,
    ) -> Option<(Arc<crate::agent_gateway::Workspace>, u64)> {
        if self.active.is_some() {
            return None;
        }
        self.workspace_files_access(session_id)
            .ok()
            .map(|workspace| (workspace, self.counter))
    }
    pub fn baseline_candidate_is_current(&self, token: u64) -> bool {
        self.active.is_none() && self.counter == token
    }
    pub fn install_baseline(
        &mut self,
        session_id: &str,
        run_id: &str,
        snapshot: Option<crate::agent_gateway::WorkspaceSnapshot>,
    ) {
        self.change_baselines.remove(session_id);
        if let Some(snapshot) = snapshot {
            if self.change_baselines.len() >= 8
                && let Some(first) = self.change_baselines.keys().next().cloned()
            {
                self.change_baselines.remove(&first);
            }
            self.change_baselines
                .insert(session_id.to_owned(), (run_id.to_owned(), snapshot));
        }
    }
    pub fn changes_access(&self, session_id: &str) -> Result<Option<ChangesAccess>, &'static str> {
        if self.active.is_some() {
            return Err("runtime-busy");
        }
        let workspace = self.workspace_files_access(session_id)?;
        Ok(self
            .change_baselines
            .get(session_id)
            .map(|(run, snapshot)| (workspace, run.clone(), snapshot.clone())))
    }
    pub fn baseline_is_current(
        &self,
        session_id: &str,
        run_id: &str,
    ) -> Result<bool, &'static str> {
        if self.active.is_some() {
            return Err("runtime-busy");
        }
        self.workspace_files_access(session_id)?;
        Ok(self
            .change_baselines
            .get(session_id)
            .is_some_and(|(run, _)| run == run_id))
    }
    pub fn has_process(&self) -> bool {
        self.has_agent() && self.process_enabled && crate::agent_process::available()
    }
    pub fn has_ollama_operations(&self) -> bool {
        self.provider
            .as_ref()
            .is_some_and(|p| p.provider_id == "ollama")
    }
    fn model_operation_active(&self) -> Result<bool, &'static str> {
        self.model_ops
            .is_active()
            .map_err(|_| "runtime-unavailable")
    }
    pub fn requires_approval(&self) -> bool {
        self.has_agent()
    }
    pub fn discard_approval(&mut self) {
        self.approval_delivery.take();
    }
    pub fn deliver_approval(&mut self) {
        if let Some((sender, allow)) = self.approval_delivery.take() {
            let _ = sender.try_send(allow);
        }
    }
    pub fn has_provider(&self) -> bool {
        self.provider.is_some()
    }
    pub fn discovery_config(&self) -> Result<(Config, u64), &'static str> {
        if self.settings_failed {
            return Err("storage-failure");
        }
        if self.model_operation_active()? {
            return Err("runtime-busy");
        }
        self.provider
            .clone()
            .map(|config| (config, self.catalog_generation))
            .ok_or("provider-unavailable")
    }
    pub fn install_catalog(
        &mut self,
        config: &Config,
        generation: u64,
        models: Vec<String>,
    ) -> Result<ResponsePayload, &'static str> {
        if self.model_operation_active()?
            || self.catalog_generation != generation
            || self.provider.as_ref() != Some(config)
        {
            return Err("runtime-busy");
        }
        let rows: Vec<Value> = models
            .iter()
            .map(|model| json!({"provider_id":config.provider_id,"model_id":model}))
            .collect();
        let payload = json!({"models":rows});
        if serde_json::to_vec(&payload)
            .map_err(|_| "runtime-unavailable")?
            .len()
            > contract::MAX_FRAME_BYTES - 2048
        {
            return Err("message-too-large");
        }
        let reply = typed("model.list", payload)?;
        self.catalog = models;
        self.catalog_known = true;
        Ok(reply)
    }
    pub fn counts(&self) -> (u64, u64) {
        (u64::from(self.active.is_some()), self.sessions.len() as u64)
    }
    pub fn drop_peer(&mut self, peer: &Arc<Peer>) {
        self.backup_transfers.retain(|transfer| {
            transfer
                .owner
                .upgrade()
                .is_some_and(|owner| !Arc::ptr_eq(&owner, peer))
        });
    }
    pub fn stop(&mut self) {
        self.backup_transfers.clear();
        let _ = self.model_ops.cancel();
        if let Some(active) = &self.active {
            active.cancel.store(true, Ordering::SeqCst);
        }
    }
    pub fn handle(
        &mut self,
        request: RequestPayload,
        peer: &Arc<Peer>,
        job: &mut Option<Job>,
    ) -> Result<ResponsePayload, &'static str> {
        match request {
            RequestPayload::BackupTransfer { action, value } => {
                use base64::{Engine as _, engine::general_purpose::STANDARD};
                use sha2::{Digest, Sha256};
                self.backup_transfers.retain(|transfer| {
                    transfer.owner.strong_count() > 0 && transfer.touched.elapsed() < BACKUP_LEASE
                });
                let owner = |transfer: &BackupTransfer| {
                    transfer
                        .owner
                        .upgrade()
                        .is_some_and(|candidate| Arc::ptr_eq(&candidate, peer))
                };
                if action == "cancel" {
                    self.backup_transfers.retain(|transfer| !owner(transfer));
                    return typed("backup.transfer", json!({"result":{"succeeded":true}}));
                }
                if action == "export" {
                    if self.storage_failed || self.settings_failed || self.memory_failed {
                        return typed(
                            "backup.transfer",
                            json!({"result":{"succeeded":false,"code":"Unavailable"}}),
                        );
                    }
                    if self.active.is_some() {
                        return typed(
                            "backup.transfer",
                            json!({"result":{"succeeded":false,"code":"RuntimeBusy"}}),
                        );
                    }
                    let Some(domains) = value.get("domains").and_then(Value::as_array) else {
                        return typed(
                            "backup.transfer",
                            json!({"result":{"succeeded":false,"code":"InvalidDomains"}}),
                        );
                    };
                    let set = domains.len() == 4
                        && domains.iter().map(Value::as_str).eq([
                            Some("chat"),
                            Some("settings"),
                            Some("memory"),
                            Some("agent"),
                        ]);
                    let domain = if set {
                        "profile"
                    } else if domains.len() == 1 {
                        domains[0].as_str().unwrap_or("")
                    } else {
                        ""
                    };
                    if !set && !["chat", "settings", "memory", "agent"].contains(&domain) {
                        return typed(
                            "backup.transfer",
                            json!({"result":{"succeeded":false,"code":"InvalidDomains"}}),
                        );
                    }
                    if !self.backup_transfers.iter().any(owner) && self.backup_transfers.len() >= 4
                    {
                        return typed(
                            "backup.transfer",
                            json!({"result":{"succeeded":false,"code":"TransferLimit"}}),
                        );
                    }
                    let bytes = match domain {
                        "profile" => (|| {
                            let mut set = crate::backup::SnapshotSet::default();
                            set.push("chat", self.store.development_snapshot()?)?;
                            set.push("settings", self.settings.development_snapshot()?)?;
                            set.push("memory", self.memory.development_snapshot()?)?;
                            set.push("agent", self.agent_store.development_snapshot()?)?;
                            set.finish()
                        })(),
                        "chat" => self.store.development_snapshot(),
                        "settings" => self.settings.development_snapshot(),
                        "memory" => self.memory.development_snapshot(),
                        "agent" => self.agent_store.development_snapshot(),
                        _ => unreachable!(),
                    }
                    .map_err(|_| "storage-failure")?;
                    let digest = format!("{:x}", Sha256::digest(&bytes));
                    let id = self.id("backup");
                    let size = bytes.len();
                    self.backup_transfers.retain(|transfer| !owner(transfer));
                    self.backup_transfers.push(BackupTransfer {
                        id: id.clone(),
                        owner: Arc::downgrade(peer),
                        data: bytes,
                        touched: Instant::now(),
                    });
                    return typed(
                        "backup.transfer",
                        json!({"result":{"succeeded":true,"transferId":id,"size":size,"sha256":digest,"format":if set { crate::backup::SET_FORMAT } else { "sentinel-dev-sqlite-v1" },"domain":domain}}),
                    );
                }
                let Some(transfer) = self.backup_transfers.iter_mut().find(|transfer| {
                    owner(transfer)
                        && value.get("transferId").and_then(Value::as_str)
                            == Some(transfer.id.as_str())
                }) else {
                    return typed(
                        "backup.transfer",
                        json!({"result":{"succeeded":false,"code":"UnknownTransfer"}}),
                    );
                };
                transfer.touched = Instant::now();
                if action != "read" {
                    return typed(
                        "backup.transfer",
                        json!({"result":{"succeeded":false,"code":"InvalidTransferAction"}}),
                    );
                }
                let Some(offset) = value
                    .get("offset")
                    .and_then(Value::as_u64)
                    .and_then(|offset| usize::try_from(offset).ok())
                    .filter(|offset| *offset < transfer.data.len())
                else {
                    return typed(
                        "backup.transfer",
                        json!({"result":{"succeeded":false,"code":"InvalidOffset"}}),
                    );
                };
                let end = offset.saturating_add(BACKUP_CHUNK).min(transfer.data.len());
                typed(
                    "backup.transfer",
                    json!({"result":{"succeeded":true,"offset":offset,"data":STANDARD.encode(&transfer.data[offset..end]),"nextOffset":end}}),
                )
            }
            RequestPayload::DesktopProjection { page } => {
                if page != 0 {
                    return Err("invalid-page");
                }
                let properties = if let Some(config) = &self.provider {
                    json!({"activeRuntimeProviderLabel":config.provider_id,"activeRuntimeModelLabel":config.model,
                        "activeRuntimeReadinessState":"unknown", "activeRuntimeReadinessSummary":"Provider connectivity has not been verified.",
                        "activeRuntimeLocalOnlySummary":"Configured for a local loopback provider."})
                } else {
                    json!({"activeRuntimeProviderLabel":"","activeRuntimeModelLabel":"",
                        "activeRuntimeReadinessState":"unavailable", "activeRuntimeReadinessSummary":"No local provider configured.",
                        "activeRuntimeLocalOnlySummary":"No provider configured."})
                };
                typed(
                    "desktop.projection",
                    json!({"page":0,"pages":28,"properties":properties}),
                )
            }
            RequestPayload::TerminalState {} => {
                let workspaces: Vec<Value> = self
                    .workspaces
                    .iter()
                    .map(|workspace| self.workspace_description(workspace))
                    .collect();
                let workspace = self
                    .selected_workspace
                    .as_ref()
                    .and_then(|id| self.workspaces.iter().find(|w| &w.id == id))
                    .map(|workspace| self.workspace_description(workspace))
                    .unwrap_or_else(|| json!({}));
                typed(
                    "terminal.state",
                    json!({"properties":{},"providers":[],"workspace":workspace,"workspaces":workspaces}),
                )
            }
            RequestPayload::VoiceState {} => typed(
                "voice.state",
                json!({"state":"Unavailable","available":false,"owned":false,"transcript":"","failure":"RuntimeUnavailable","input_device_id":"","vad_enabled":true}),
            ),
            RequestPayload::WorkspaceSelect { workspace_id } => {
                if self.active.is_some() {
                    return Err("runtime-busy");
                }
                if self.settings_failed {
                    return Err("storage-failure");
                }
                let workspace = self
                    .workspaces
                    .iter()
                    .find(|w| w.id == workspace_id)
                    .ok_or("unknown-workspace")?;
                let actual = workspace
                    .handle
                    .inventory_root()
                    .map_err(|_| "workspace-root-rejected")?;
                if actual.to_str() != Some(workspace.root.as_str()) {
                    return Err("workspace-root-rejected");
                }
                let changing = self.selected_workspace.as_deref() != Some(workspace_id.as_str());
                let next_counter = if changing {
                    Some(self.counter.checked_add(1).ok_or("limit-exceeded")?)
                } else {
                    None
                };
                if let Err(error) = self
                    .settings
                    .select_workspace(&workspace.id, &workspace.root)
                {
                    self.settings_failed = !error.safe_conflict;
                    return Err("storage-failure");
                }
                if let Some(next_counter) = next_counter {
                    self.workspace = Some(workspace.handle.clone());
                    self.selected_workspace = Some(workspace_id.clone());
                    self.counter = next_counter;
                    self.change_baselines.clear();
                }
                typed("workspace.select", json!({"workspace_id":workspace_id}))
            }
            RequestPayload::WorkspaceRoot { workspace_id, path } => {
                if self.active.is_some() {
                    return Err("runtime-busy");
                }
                let workspace = self
                    .workspaces
                    .iter()
                    .find(|w| w.id == workspace_id)
                    .ok_or("unknown-workspace")?;
                let actual = workspace
                    .handle
                    .inventory_root()
                    .map_err(|_| "workspace-root-rejected")?;
                if path != workspace.root || actual.to_str() != Some(workspace.root.as_str()) {
                    return Err("workspace-root-rejected");
                }
                typed(
                    "workspace.root",
                    json!({"workspace_id":workspace_id,"root":workspace.root}),
                )
            }
            RequestPayload::AgentHistory {
                run_id,
                before_time,
                before_id,
            } => {
                if self.storage_failed {
                    return Err("storage-failure");
                }
                if before_time.is_empty() != before_id.is_empty()
                    || (!before_time.is_empty() && !crate::import::timestamp(&before_time))
                    || run_id.len() > 256
                    || before_id.len() > 256
                {
                    return Err("invalid-payload");
                }
                let sessions: Vec<String> = self
                    .sessions
                    .iter()
                    .filter(|session| {
                        session
                            .peers
                            .iter()
                            .any(|p| p.ptr_eq(&Arc::downgrade(peer)))
                    })
                    .map(|s| s.id.clone())
                    .collect();
                let history =
                    match self
                        .agent_store
                        .history(&sessions, &run_id, &before_time, &before_id)
                    {
                        Ok(Some(value)) => value,
                        Ok(None) => return Err("unknown-run"),
                        Err(error) => {
                            self.storage_failed = !store::is_conflict(&error);
                            return Err("storage-failure");
                        }
                    };
                typed("agent.history", json!({"history":history}))
            }
            RequestPayload::DesktopSettingsService { action, arguments }
                if action == "providerSettingsState" =>
            {
                if arguments.len() != 1 {
                    return Err("invalid-payload");
                }
                let id = arguments[0]
                    .as_str()
                    .filter(|id| !id.is_empty() && id.len() <= 128)
                    .ok_or("invalid-payload")?;
                let result = if let Some(config) = self
                    .provider
                    .as_ref()
                    .filter(|config| config.provider_id == id)
                {
                    json!({"id":id,"known":true,"health":"Unknown","catalog":if self.catalog_known {"Cached"} else {"Unknown"},"local":true,"endpoint":config.endpoint,"discoveredModels":self.catalog,"contextWindow":config.context_window_tokens,"configuredModel":config.model})
                } else {
                    json!({"id":id,"known":false,"health":"Unknown","catalog":"Unknown","local":false,"endpoint":"","discoveredModels":[],"contextWindow":0,"configuredModel":""})
                };
                typed("desktop.settings_service", json!({"result":result}))
            }
            RequestPayload::DesktopAction {
                action,
                arguments,
                session_id,
            } => {
                if self.active.is_some() {
                    return Err("runtime-busy");
                }
                if self.storage_failed || self.memory_failed {
                    return Err("storage-failure");
                }
                if !self.sessions.iter().any(|s| s.id == session_id) {
                    return Err("unknown-session");
                }
                match action.as_str() {
                    "manageMemory" if arguments.len() == 1 && arguments[0].is_object() => {
                        let result = match self.memory.manage(&session_id, &arguments[0]) {
                            Ok(value) => value,
                            Err(error) => {
                                self.memory_failed = !error.safe_conflict;
                                return Err("storage-failure");
                            }
                        };
                        typed(
                            "desktop.action",
                            json!({"accepted":true,"result":{"value":result}}),
                        )
                    }
                    "renameConversation" if arguments.len() == 2 => {
                        let target = arguments[0].as_str().ok_or("invalid-desktop-action")?;
                        let raw = arguments[1].as_str().ok_or("invalid-desktop-action")?;
                        if target != session_id || raw.len() > 1024 {
                            return Err("invalid-payload");
                        }
                        let title = if raw.trim().is_empty() {
                            "Untitled Conversation"
                        } else {
                            raw.trim()
                        };
                        if let Err(error) = self.store.rename(target, title) {
                            self.storage_failed = !error.safe_conflict;
                            return Err("storage-failure");
                        }
                        let session = self
                            .sessions
                            .iter_mut()
                            .find(|s| s.id == target)
                            .ok_or("unknown-session")?;
                        session.title = title.into();
                        typed(
                            "desktop.action",
                            json!({"accepted":true,"result":{"value":true}}),
                        )
                    }
                    "requestPermanentDeleteConversation" if arguments.len() == 1 => {
                        let target = arguments[0].as_str().ok_or("invalid-desktop-action")?;
                        if target != session_id {
                            return Err("invalid-payload");
                        }
                        if let Err(error) = self.store.delete(&self.profile, target) {
                            self.storage_failed = !error.safe_conflict;
                            return Err("storage-failure");
                        }
                        self.sessions.retain(|s| s.id != target);
                        self.change_baselines.remove(target);
                        typed(
                            "desktop.action",
                            json!({"accepted":true,"result":{"value":true}}),
                        )
                    }
                    _ => Err("invalid-desktop-action"),
                }
            }
            RequestPayload::DesktopSettings {} => {
                if self.settings_failed {
                    return Err("storage-failure");
                }
                let mut values = match self.settings.desktop_values() {
                    Ok(values) => values,
                    Err(error) => {
                        self.settings_failed = !store::is_conflict(&error);
                        return Err("storage-failure");
                    }
                };
                if let Some(workspace) = self
                    .selected_workspace
                    .as_ref()
                    .and_then(|id| self.workspaces.iter().find(|w| &w.id == id))
                {
                    let enabled = self
                        .settings
                        .workspace_access(&workspace.id, &workspace.root)
                        .map_err(|_| "storage-failure")?;
                    values["devWorkspaceAccess"] = json!(if enabled { "true" } else { "false" });
                }
                typed("desktop.settings", json!({"values":values}))
            }
            RequestPayload::DesktopSetting { key, value } => {
                if self.settings_failed {
                    return Err("storage-failure");
                }
                if key == "devWorkspaceAccess" {
                    let enabled = match value.as_str() {
                        "true" => true,
                        "false" => false,
                        _ => return Err("invalid-payload"),
                    };
                    if enabled && self.active.is_some() {
                        return Err("runtime-busy");
                    }
                    let workspace = self
                        .selected_workspace
                        .as_ref()
                        .and_then(|id| self.workspaces.iter().find(|w| &w.id == id))
                        .ok_or("runtime-unavailable")?;
                    let id = workspace.id.clone();
                    let root = workspace.root.clone();
                    if workspace
                        .handle
                        .inventory_root()
                        .map_err(|_| "workspace-root-rejected")?
                        .to_str()
                        != Some(root.as_str())
                    {
                        return Err("workspace-root-rejected");
                    }
                    if let Err(error) = self.settings.set_workspace_access(&id, &root, enabled) {
                        self.settings_failed = !error.safe_conflict;
                        if !error.safe_conflict
                            && let Some(active) = &self.active
                        {
                            active.cancel.store(true, Ordering::SeqCst);
                        }
                        return Err("storage-failure");
                    }
                    if !enabled {
                        if let Some(active) = &self.active {
                            active.cancel.store(true, Ordering::SeqCst);
                        }
                        self.change_baselines.clear();
                        self.counter = self.counter.checked_add(1).ok_or("limit-exceeded")?;
                        if let Some(pending) = self.pending_approval.take() {
                            if self
                                .agent_store
                                .resolve_approval(
                                    &pending.run_id,
                                    &pending.id,
                                    crate::agent_store::ApprovalResolution::Cancelled,
                                )
                                .is_err()
                            {
                                self.storage_failed = true;
                                return Err("storage-failure");
                            }
                            let _ = pending.sender.try_send(false);
                        }
                        self.approval_delivery.take();
                    }
                    return typed("desktop.setting", json!({"accepted":true}));
                }
                if !crate::settings::valid_preference(&key, &value) {
                    return Err("invalid-payload");
                }
                if self.active.is_some() {
                    return Err("runtime-busy");
                }
                let value = if key == "responseProfileInstructions" {
                    value.trim().to_owned()
                } else {
                    value
                };
                if let Err(error) = self.settings.set_desktop_value(&key, &value) {
                    self.settings_failed = !error.safe_conflict;
                    return Err("storage-failure");
                }
                typed("desktop.setting", json!({"accepted":true}))
            }
            RequestPayload::ModelHelperState {
                component,
                offset,
                retain_models: _,
            } => {
                if component != "ollamaPuller" {
                    return Err("unknown-model-component");
                }
                if !self.has_ollama_operations() {
                    return Err("provider-unavailable");
                }
                if offset.unwrap_or(0) != 0 {
                    return Err("invalid-model-offset");
                }
                let properties = self.model_ops.state().map_err(|_| "runtime-unavailable")?;
                typed(
                    "model.helper_state",
                    json!({"component":component,"properties":properties}),
                )
            }
            RequestPayload::ModelHelperAction {
                component,
                action,
                value,
            } => {
                if component != "ollamaPuller" {
                    return Err("unknown-model-component");
                }
                if !self.has_ollama_operations() {
                    return Err("provider-unavailable");
                }
                if action == "cancel" {
                    let accepted = self.model_ops.cancel().map_err(|_| "runtime-unavailable")?;
                    return typed("model.helper_action", json!({"accepted":accepted}));
                }
                if self.settings_failed {
                    return Err("storage-failure");
                }
                if self.active.is_some() {
                    return Err("runtime-busy");
                }
                if !matches!(action.as_str(), "pull" | "removeModel") {
                    return Err("invalid-payload");
                }
                let config = self.provider.clone().ok_or("provider-unavailable")?;
                let generation = self
                    .catalog_generation
                    .checked_add(1)
                    .ok_or("limit-exceeded")?;
                self.model_ops
                    .start(config, &value, action == "removeModel")
                    .map_err(|error| {
                        if error.kind() == io::ErrorKind::WouldBlock {
                            "runtime-busy"
                        } else if error.kind() == io::ErrorKind::InvalidInput {
                            "invalid-payload"
                        } else {
                            "runtime-unavailable"
                        }
                    })?;
                self.catalog_generation = generation;
                self.catalog.clear();
                self.catalog_known = false;
                typed("model.helper_action", json!({"accepted":true}))
            }
            RequestPayload::ModelCurrent {} => {
                if self.settings_failed {
                    return Err("storage-failure");
                }
                let config = self.provider.as_ref().ok_or("provider-unavailable")?;
                typed(
                    "model.current",
                    json!({"provider_id":config.provider_id,"model_id":config.model}),
                )
            }
            RequestPayload::ProviderSelect { provider_id } => {
                if self.settings_failed {
                    return Err("storage-failure");
                }
                if self.model_operation_active()? || self.active.is_some() {
                    return Err("runtime-busy");
                }
                let config = self.provider.as_ref().ok_or("provider-unavailable")?;
                if provider_id != config.provider_id {
                    return Err("provider-unavailable");
                }
                typed(
                    "provider.select",
                    json!({"provider_id":config.provider_id,"model_id":config.model}),
                )
            }
            RequestPayload::ModelSelect {
                provider_id,
                model_id,
            } => {
                if self.settings_failed {
                    return Err("storage-failure");
                }
                if self.model_operation_active()? || self.active.is_some() {
                    return Err("runtime-busy");
                }
                let config = self.provider.as_mut().ok_or("provider-unavailable")?;
                if provider_id != config.provider_id {
                    return Err("provider-unavailable");
                }
                if !self.catalog.contains(&model_id) {
                    return Err("model-unavailable");
                }
                if let Err(error) = self.settings.select(config, &model_id) {
                    self.settings_failed = !error.safe_conflict;
                    return Err("storage-failure");
                }
                config.model = model_id;
                typed(
                    "model.select",
                    json!({"provider_id":config.provider_id,"model_id":config.model}),
                )
            }
            RequestPayload::SessionCreate { title } => {
                if self.storage_failed {
                    return Err("storage-failure");
                }
                if self.sessions.len() >= MAX_SESSIONS || title.len() > 1024 {
                    return Err("limit-exceeded");
                }
                if self.active.is_some() {
                    return Err("runtime-busy");
                }
                let id = self.id("session");
                if let Err(error) = self.store.create(&id, &title) {
                    self.storage_failed = !error.safe_conflict;
                    return Err("storage-failure");
                }
                self.sessions.push(Session {
                    id: id.clone(),
                    title: title.clone(),
                    run_id: String::new(),
                    state: "idle".into(),
                    output: String::new(),
                    model: String::new(),
                    provider_id: String::new(),
                    detail: String::new(),
                    created_at: String::new(),
                    updated_at: String::new(),
                    seq: 0,
                    agent: false,
                    peers: vec![Arc::downgrade(peer)],
                });
                typed(
                    "session.create",
                    json!({"session_id":id,"type":"chat","created_at":"","title":title,"server_generation":self.generation}),
                )
            }
            RequestPayload::SessionAttach { session_id } => {
                self.attach(&session_id, peer, "session.attach")
            }
            RequestPayload::TerminalAttach { session_id } => {
                self.attach(&session_id, peer, "terminal.attach")
            }
            RequestPayload::SessionList {} => {
                if self.storage_failed {
                    return Err("storage-failure");
                }
                let sessions: Vec<Value> = self
                    .sessions
                    .iter()
                    .map(|s| json!({"session_id":s.id,"title":s.title,"state":s.state,"created_at":s.created_at,"updated_at":s.updated_at}))
                    .collect();
                typed("session.list", json!({"sessions":sessions}))
            }
            RequestPayload::SessionMessages { session_id } => {
                if self.storage_failed {
                    return Err("storage-failure");
                }
                if !self.sessions.iter().any(|s| s.id == session_id) {
                    return Err("unknown-session");
                }
                let messages = match self.store.messages(&session_id) {
                    Ok(messages) => messages,
                    Err(_) => {
                        self.storage_failed = true;
                        return Err("storage-failure");
                    }
                };
                if !store::history_fits(&session_id, &messages) {
                    return Err("message-too-large");
                }
                let payload = json!({"session_id": session_id, "messages": messages});
                typed("session.messages", payload)
            }
            RequestPayload::ChatSend {
                session_id,
                text,
                attachments,
            } => {
                if self.storage_failed || self.settings_failed {
                    return Err("storage-failure");
                }
                if text.trim().is_empty() || text.len() > store::MAX_PROMPT {
                    return Err("invalid-payload");
                }
                let config = self.provider.clone().ok_or("provider-unavailable")?;
                if self.model_operation_active()? || self.active.is_some() {
                    return Err("runtime-busy");
                }
                if !self.sessions.iter().any(|s| s.id == session_id) {
                    return Err("unknown-session");
                }
                let attachment_values = attachments.unwrap_or_default();
                let loaded = if attachment_values.is_empty() {
                    Vec::new()
                } else {
                    let workspace = self.current_workspace()?;
                    crate::attachment::load(&workspace, &attachment_values)
                        .map_err(|_| "invalid-attachments")?
                };
                let encoded = serde_json::to_string(&loaded).map_err(|_| "invalid-attachments")?;
                let provider_text = crate::attachment::context(&text, &encoded)
                    .map_err(|_| "invalid-attachments")?;
                let profile = self.response_profile()?;
                // Reject a mandatory-only request before even reading history or writing a run.
                match provider::prepare_with_profile(&config, &provider_text, &[], &profile) {
                    Ok(_) => {}
                    Err(ProviderError::InvalidRequest(_)) => return Err("message-too-large"),
                    Err(ProviderError::ContextLimitExceeded) => {
                        return Err("context-limit-exceeded");
                    }
                    Err(_) => return Err("runtime-unavailable"),
                }
                let history = match self.store.context_turns(&session_id) {
                    Ok(history) => history,
                    Err(error) => {
                        self.storage_failed = !store::is_conflict(&error);
                        return Err("storage-failure");
                    }
                };
                let request = match provider::prepare_with_profile(
                    &config,
                    &provider_text,
                    &history,
                    &profile,
                ) {
                    Ok(request) => request,
                    Err(ProviderError::InvalidRequest(_)) => return Err("message-too-large"),
                    Err(ProviderError::ContextLimitExceeded) => {
                        return Err("context-limit-exceeded");
                    }
                    Err(_) => return Err("runtime-unavailable"),
                };
                let run_id = self.id("run");
                match self.store.begin_with_attachments(
                    &session_id,
                    &run_id,
                    &text,
                    &config.model,
                    &config.provider_id,
                    &encoded,
                ) {
                    Ok(true) => {}
                    Ok(false) => return Err("limit-exceeded"),
                    Err(error) => {
                        self.storage_failed = !error.safe_conflict;
                        return Err("storage-failure");
                    }
                }
                let session = self
                    .sessions
                    .iter_mut()
                    .find(|s| s.id == session_id)
                    .unwrap();
                session.run_id.clone_from(&run_id);
                session.agent = false;
                session.state = "running".into();
                session.output.clear();
                session.detail.clear();
                session.model.clone_from(&config.model);
                session.provider_id.clone_from(&config.provider_id);
                if !session
                    .peers
                    .iter()
                    .any(|p| p.ptr_eq(&Arc::downgrade(peer)))
                {
                    session.peers.push(Arc::downgrade(peer));
                }
                let cancel = Arc::new(AtomicBool::new(false));
                self.active = Some(Active {
                    run_id: run_id.clone(),
                    session_id: session_id.clone(),
                    cancel: cancel.clone(),
                });
                *job = Some(Job {
                    session_id: session_id.clone(),
                    run_id: run_id.clone(),
                    request,
                    cancel,
                    config,
                    agent: None,
                });
                typed(
                    "chat.send",
                    json!({"session_id":session_id,"run_id":run_id}),
                )
            }
            RequestPayload::ChatRegenerate {
                session_id,
                message_id,
            } => self.start_existing(&session_id, message_id, Revision::Regenerate, peer, job),
            RequestPayload::ChatRetry {
                session_id,
                message_id,
            } => self.start_existing(&session_id, message_id, Revision::Retry, peer, job),
            RequestPayload::ChatEdit {
                session_id,
                message_id,
                text,
            } => self.start_existing(&session_id, message_id, Revision::Edit(text), peer, job),
            RequestPayload::AgentStart {
                session_id,
                text,
                attachments,
            } => {
                if self.settings_failed || self.storage_failed {
                    return Err("storage-failure");
                }
                if self.model_operation_active()? || self.active.is_some() {
                    return Err("runtime-busy");
                }
                if text.trim().is_empty()
                    || text.len() > store::MAX_PROMPT
                    || attachments.as_ref().is_some_and(|a| !a.is_empty())
                {
                    return Err("invalid-payload");
                }
                let config = self.provider.clone().ok_or("provider-unavailable")?;
                let workspace = self.current_workspace()?;
                if crate::agent::requires_process(&text) && !self.has_process() {
                    return Err("runtime-unavailable");
                }
                if !self.sessions.iter().any(|s| s.id == session_id) {
                    return Err("unknown-session");
                }
                let request =
                    crate::agent::prepare(&config, &text).map_err(|error| match error {
                        ProviderError::InvalidRequest(_) => "invalid-payload",
                        ProviderError::ContextLimitExceeded => "context-limit-exceeded",
                        _ => "runtime-unavailable",
                    })?;
                let run_id = self.id("run");
                let plan_metadata = crate::agent::plan_metadata(&text);
                if self
                    .agent_store
                    .start_with_plan(
                        &run_id,
                        &session_id,
                        &text,
                        &config.provider_id,
                        &config.model,
                        plan_metadata.as_deref(),
                    )
                    .is_err()
                {
                    self.storage_failed = true;
                    return Err("storage-failure");
                }
                match self.store.begin_with_provider(
                    &session_id,
                    &run_id,
                    &text,
                    &config.model,
                    &config.provider_id,
                ) {
                    Ok(true) => {}
                    result => {
                        if result.is_err() {
                            self.storage_failed = true;
                            return Err("storage-failure");
                        }
                        if self
                            .agent_store
                            .finish(&run_id, "failed", "", "limit-exceeded")
                            .is_err()
                            || self.agent_store.confirm(&run_id).is_err()
                        {
                            self.storage_failed = true;
                            return Err("storage-failure");
                        }
                        return Err("limit-exceeded");
                    }
                }
                let session = self
                    .sessions
                    .iter_mut()
                    .find(|s| s.id == session_id)
                    .ok_or("unknown-session")?;
                session.run_id.clone_from(&run_id);
                session.agent = true;
                session.state = "running".into();
                session.output.clear();
                session.detail.clear();
                session.model.clone_from(&config.model);
                session.provider_id.clone_from(&config.provider_id);
                if !session
                    .peers
                    .iter()
                    .any(|p| p.ptr_eq(&Arc::downgrade(peer)))
                {
                    session.peers.push(Arc::downgrade(peer));
                }
                let cancel = Arc::new(AtomicBool::new(false));
                self.active = Some(Active {
                    run_id: run_id.clone(),
                    session_id: session_id.clone(),
                    cancel: cancel.clone(),
                });
                *job = Some(Job {
                    session_id: session_id.clone(),
                    run_id: run_id.clone(),
                    request,
                    cancel,
                    config,
                    agent: Some((workspace, text)),
                });
                typed(
                    "agent.start",
                    json!({"session_id":session_id,"run_id":run_id}),
                )
            }
            RequestPayload::ApprovalRespond {
                run_id,
                approval_id,
                allow,
            } => {
                let pending = self
                    .pending_approval
                    .as_ref()
                    .filter(|p| {
                        p.run_id == run_id
                            && p.id == approval_id
                            && Instant::now() < p.deadline
                            && self.active.as_ref().is_some_and(|a| {
                                a.run_id == run_id && !a.cancel.load(Ordering::SeqCst)
                            })
                    })
                    .ok_or("invalid-approval")?;
                let session_id = pending.session_id.clone();
                let resolution = if allow {
                    crate::agent_store::ApprovalResolution::Allow
                } else {
                    crate::agent_store::ApprovalResolution::Deny
                };
                if self
                    .agent_store
                    .resolve_approval(&run_id, &approval_id, resolution)
                    .is_err()
                {
                    self.storage_failed = true;
                    return Err("storage-failure");
                }
                let pending = self.pending_approval.take().ok_or("invalid-approval")?;
                if let Some(session) = self
                    .sessions
                    .iter_mut()
                    .find(|s| s.id == session_id && s.run_id == run_id)
                {
                    session.state = "running".into();
                }
                self.approval_delivery = Some((pending.sender, allow));
                typed("approval.respond", json!({"accepted":true}))
            }
            RequestPayload::RunCancel { run_id } => {
                let active = self
                    .active
                    .as_ref()
                    .filter(|a| a.run_id == run_id)
                    .ok_or("unknown-run")?;
                if active.cancel.swap(true, Ordering::SeqCst) {
                    return Err("unknown-run");
                }
                typed("run.cancel", json!({"accepted":true}))
            }
            _ => Err("runtime-unavailable"),
        }
    }
    fn attach(
        &mut self,
        session_id: &str,
        peer: &Arc<Peer>,
        name: &'static str,
    ) -> Result<ResponsePayload, &'static str> {
        if self.storage_failed {
            return Err("storage-failure");
        }
        let session = self
            .sessions
            .iter_mut()
            .find(|s| s.id == session_id)
            .ok_or("unknown-session")?;
        session.peers.retain(|p| p.strong_count() > 0);
        if !session
            .peers
            .iter()
            .any(|p| p.ptr_eq(&Arc::downgrade(peer)))
        {
            session.peers.push(Arc::downgrade(peer));
        }
        let binding = (!session.provider_id.is_empty()).then_some(session.provider_id.as_str());
        let model = (!session.model.is_empty()).then_some(session.model.as_str());
        let reply = typed(
            name,
            json!({"session_id":session.id,"state":session.state,"output":session.output,"run_id":session.run_id,"run_type":if session.agent {"agent"} else {"chat"},"provider_id":binding,"model_id":model,"server_generation":self.generation,"event_sequence":session.seq,"title":session.title,"detail":session.detail,"created_at":session.created_at}),
        );
        if self
            .pending_approval
            .as_ref()
            .is_some_and(|p| p.session_id == session_id)
        {
            self.approval_event(Some(peer));
        }
        reply
    }
    fn start_existing(
        &mut self,
        sid: &str,
        message_id: u64,
        revision: Revision,
        peer: &Arc<Peer>,
        job: &mut Option<Job>,
    ) -> Result<ResponsePayload, &'static str> {
        if self.storage_failed || self.settings_failed {
            return Err("storage-failure");
        }
        if message_id == 0 || message_id > i64::MAX as u64 {
            return Err("invalid-payload");
        }
        if self.model_operation_active()? || self.active.is_some() {
            return Err("runtime-busy");
        }
        if !self.sessions.iter().any(|s| s.id == sid) {
            return Err("unknown-session");
        }
        let config = self.provider.clone().ok_or("provider-unavailable")?;
        let retry = matches!(revision, Revision::Retry);
        let target = match self.store.retry_target(sid, message_id, retry) {
            Ok(Some(target)) => target,
            Ok(None) => return Err("invalid-payload"),
            Err(error) => {
                self.storage_failed = !store::is_conflict(&error);
                return Err("storage-failure");
            }
        };
        let (user_id, original, replaces, historical) = target;
        let branching = historical || matches!(revision, Revision::Edit(_));
        if branching && self.sessions.len() >= MAX_SESSIONS {
            return Err("limit-exceeded");
        }
        let text = match &revision {
            Revision::Edit(text) if !text.trim().is_empty() && text.len() <= store::MAX_PROMPT => {
                text.clone()
            }
            Revision::Edit(_) => return Err("invalid-payload"),
            _ => original,
        };
        let text = if branching {
            text.trim().to_owned()
        } else {
            text
        };
        let provider_text = self
            .store
            .user_context(sid, user_id, &text)
            .map_err(|_| "storage-failure")?;
        let profile = self.response_profile()?;
        match provider::prepare_with_profile(&config, &provider_text, &[], &profile) {
            Ok(_) => {}
            Err(ProviderError::InvalidRequest(_)) => return Err("message-too-large"),
            Err(ProviderError::ContextLimitExceeded) => return Err("context-limit-exceeded"),
            Err(_) => return Err("runtime-unavailable"),
        }
        let history = match self.store.context_turns_before(sid, Some(user_id)) {
            Ok(history) => history,
            Err(error) => {
                self.storage_failed = !store::is_conflict(&error);
                return Err("storage-failure");
            }
        };
        let request =
            match provider::prepare_with_profile(&config, &provider_text, &history, &profile) {
                Ok(request) => request,
                Err(ProviderError::InvalidRequest(_)) => return Err("message-too-large"),
                Err(ProviderError::ContextLimitExceeded) => return Err("context-limit-exceeded"),
                Err(_) => return Err("runtime-unavailable"),
            };
        let rid = self.id("run");
        let branch_id = branching.then(|| self.id("session"));
        let title = format!(
            "Branch: {}",
            text.split_whitespace()
                .collect::<Vec<_>>()
                .join(" ")
                .chars()
                .take(56)
                .collect::<String>()
        );
        let started = if let Some(branch) = &branch_id {
            self.store.begin_branch(store::BranchRequest {
                source: sid,
                branch,
                run: &rid,
                title: &title,
                user_id,
                text: &text,
                model: &config.model,
                provider: &config.provider_id,
            })
        } else {
            self.store.begin_existing(
                sid,
                &rid,
                user_id,
                replaces,
                &config.model,
                &config.provider_id,
            )
        };
        match started {
            Ok(true) => {}
            Ok(false) => return Err("limit-exceeded"),
            Err(error) => {
                self.storage_failed = !error.safe_conflict;
                return Err("storage-failure");
            }
        }
        if let Some(branch) = &branch_id {
            self.sessions.push(Session {
                id: branch.clone(),
                title,
                run_id: String::new(),
                state: "idle".into(),
                output: String::new(),
                model: String::new(),
                provider_id: String::new(),
                detail: String::new(),
                created_at: String::new(),
                updated_at: String::new(),
                seq: 0,
                agent: false,
                peers: Vec::new(),
            });
        }
        let target_sid = branch_id.as_deref().unwrap_or(sid);
        let session = self
            .sessions
            .iter_mut()
            .find(|s| s.id == target_sid)
            .ok_or("unknown-session")?;
        session.run_id.clone_from(&rid);
        session.agent = false;
        session.state = "running".into();
        session.output.clear();
        session.detail.clear();
        session.model.clone_from(&config.model);
        session.provider_id.clone_from(&config.provider_id);
        if !session
            .peers
            .iter()
            .any(|p| p.ptr_eq(&Arc::downgrade(peer)))
        {
            session.peers.push(Arc::downgrade(peer));
        }
        let cancel = Arc::new(AtomicBool::new(false));
        self.active = Some(Active {
            run_id: rid.clone(),
            session_id: target_sid.into(),
            cancel: cancel.clone(),
        });
        *job = Some(Job {
            session_id: target_sid.into(),
            run_id: rid.clone(),
            request,
            cancel,
            config,
            agent: None,
        });
        typed(
            match revision {
                Revision::Retry => "chat.retry",
                Revision::Regenerate => "chat.regenerate",
                Revision::Edit(_) => "chat.edit",
            },
            json!({"session_id":target_sid,"run_id":rid}),
        )
    }
    fn approval_event(&mut self, peer: Option<&Arc<Peer>>) {
        let Some(pending) = &self.pending_approval else {
            return;
        };
        let Some(session) = self
            .sessions
            .iter_mut()
            .find(|s| s.id == pending.session_id && s.run_id == pending.run_id)
        else {
            return;
        };
        session.seq += 1;
        let payload = json!({"session_id":pending.session_id,"run_id":pending.run_id,"approval_id":pending.id,"tool":pending.tool,"detail":pending.detail,"risk":if matches!(pending.tool.as_str(), "write-file" | "edit-file" | "process-exec") {2} else {0},"resources":pending.resources,"server_generation":self.generation,"event_sequence":session.seq,"state":"approval"});
        let env = Envelope {
            version: Version {
                major: contract::MAJOR,
                minor: contract::MINOR,
            },
            kind: MessageType::Event,
            id: String::new(),
            name: "approval.requested".into(),
            payload,
        };
        let Ok(mut bytes) = serde_json::to_vec(&env) else {
            return;
        };
        if bytes.len() + 1 > contract::MAX_FRAME_BYTES {
            return;
        }
        bytes.push(b'\n');
        if let Some(peer) = peer {
            let _ = peer.send(bytes);
        } else {
            session.peers.retain(|weak| {
                weak.upgrade()
                    .is_some_and(|p| p.send(bytes.clone()).is_ok())
            });
        }
    }
    fn begin_approval(
        &mut self,
        run_id: &str,
        session_id: &str,
        step: usize,
        tool: &str,
        args: &Value,
    ) -> Result<(std::sync::mpsc::Receiver<bool>, Instant), &'static str> {
        if !self.active.as_ref().is_some_and(|a| {
            a.run_id == run_id && a.session_id == session_id && !a.cancel.load(Ordering::SeqCst)
        }) || self.pending_approval.is_some()
        {
            return Err("runtime-unavailable");
        }
        let id = format!("{run_id}-approval-{step}");
        let detail = args.to_string();
        if self
            .agent_store
            .request_approval(run_id, &id, step, tool, &detail)
            .is_err()
        {
            self.storage_failed = true;
            return Err("storage-failure");
        }
        let (sender, receiver) = sync_channel(1);
        self.pending_approval = Some(PendingApproval {
            run_id: run_id.into(),
            session_id: session_id.into(),
            id,
            tool: tool.into(),
            detail,
            resources: if tool == "process-exec" {
                vec![json!({"argv":args["argv"],"access":"execute"})]
            } else {
                vec![
                    json!({"path":args["path"],"access":if matches!(tool, "write-file" | "edit-file") {"write"} else {"read"}}),
                ]
            },
            deadline: Instant::now() + APPROVAL_TIMEOUT,
            sender,
        });
        if let Some(session) = self
            .sessions
            .iter_mut()
            .find(|s| s.id == session_id && s.run_id == run_id)
        {
            session.state = "approval".into();
        }
        let deadline = self
            .pending_approval
            .as_ref()
            .ok_or("runtime-unavailable")?
            .deadline;
        self.approval_event(None);
        Ok((receiver, deadline))
    }
    fn event(&mut self, sid: &str, rid: &str, name: &str, text: &str, state: Option<&str>) {
        let Some(session) = self
            .sessions
            .iter_mut()
            .find(|s| s.id == sid && s.run_id == rid)
        else {
            return;
        };
        session.seq += 1;
        let mut payload = json!({"session_id":sid,"run_id":rid,"server_generation":self.generation,"event_sequence":session.seq});
        if name != "run.started" {
            payload["text"] = json!(text);
        }
        if let Some(state) = state {
            payload["state"] = json!(state);
            if state == "failed" {
                payload["terminal_reason"] = json!(if session.detail.is_empty() {
                    "provider-failed"
                } else {
                    &session.detail
                });
            }
        }
        let Ok(typed) = serde_json::from_value::<contract::EventPayload>(
            json!({"name":name,"payload":payload}),
        ) else {
            return;
        };
        let Ok(value) = serde_json::to_value(typed) else {
            return;
        };
        let mut payload = value["payload"].clone();
        if let Some(object) = payload.as_object_mut() {
            object.retain(|_, v| !v.is_null());
        }
        let env = Envelope {
            version: Version {
                major: contract::MAJOR,
                minor: contract::MINOR,
            },
            kind: MessageType::Event,
            id: String::new(),
            name: name.into(),
            payload,
        };
        let Ok(mut bytes) = serde_json::to_vec(&env) else {
            return;
        };
        if bytes.len() + 1 > contract::MAX_FRAME_BYTES {
            return;
        }
        bytes.push(b'\n');
        session.peers.retain(|weak| {
            weak.upgrade()
                .is_some_and(|peer| peer.send(bytes.clone()).is_ok())
        });
    }
    fn agent_progress(
        &mut self,
        sid: &str,
        rid: &str,
        name: &str,
        step: usize,
        tool: &str,
        phase: &str,
    ) {
        let Some(session) = self
            .sessions
            .iter_mut()
            .find(|s| s.id == sid && s.run_id == rid)
        else {
            return;
        };
        session.seq += 1;
        let payload = if name == "agent.activity" {
            json!({"session_id":sid,"run_id":rid,"activity":phase,"step":step,"server_generation":self.generation,"event_sequence":session.seq})
        } else {
            json!({"session_id":sid,"run_id":rid,"tool":tool,"detail":match phase { "requested" => "Tool requested", "running" => "Tool running", "completed" => "Tool completed", _ => "Tool failed; inspect authoritative run outcome" },"risk":if matches!(tool,"write-file"|"edit-file"|"process-exec") {2} else {0},"resources":[],"tool_call_id":format!("{rid}-step-{step}"),"state":phase,"server_generation":self.generation,"event_sequence":session.seq})
        };
        let Ok(typed) = serde_json::from_value::<contract::EventPayload>(
            json!({"name":name,"payload":payload}),
        ) else {
            return;
        };
        let Ok(value) = serde_json::to_value(typed) else {
            return;
        };
        let mut payload = value["payload"].clone();
        if let Some(object) = payload.as_object_mut() {
            object.retain(|_, value| !value.is_null());
        }
        let env = Envelope {
            version: Version {
                major: contract::MAJOR,
                minor: contract::MINOR,
            },
            kind: MessageType::Event,
            id: String::new(),
            name: name.into(),
            payload,
        };
        let Ok(mut bytes) = serde_json::to_vec(&env) else {
            return;
        };
        if bytes.len() + 1 > contract::MAX_FRAME_BYTES {
            return;
        }
        bytes.push(b'\n');
        session.peers.retain(|weak| {
            weak.upgrade()
                .is_some_and(|peer| peer.send(bytes.clone()).is_ok())
        });
    }
    pub fn start(&mut self, job: &Job) {
        self.event(&job.session_id, &job.run_id, "run.started", "", None);
    }
}
fn typed(name: &str, payload: Value) -> Result<ResponsePayload, &'static str> {
    serde_json::from_value(json!({"name":name,"payload":payload}))
        .map_err(|_| "runtime-unavailable")
}
pub fn worker(state: Arc<Mutex<State>>, job: Job) {
    if job.agent.is_some() {
        agent_worker(state, job);
        return;
    }
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        provider::stream_prepared(&job.config, &job.request, &job.cancel, |delta| {
            let mut state = state.lock().map_err(|_| io::Error::other("state lock"))?;
            if job.cancel.load(Ordering::SeqCst) {
                return Err(io::Error::new(io::ErrorKind::Interrupted, "cancelled"));
            }
            if !state
                .active
                .as_ref()
                .is_some_and(|a| a.run_id == job.run_id && a.session_id == job.session_id)
            {
                return Err(io::Error::other("stale run"));
            }
            let session = state
                .sessions
                .iter_mut()
                .find(|s| s.id == job.session_id)
                .ok_or_else(|| io::Error::other("missing session"))?;
            if session.output.len() + delta.len() > MAX_OUTPUT {
                return Err(io::Error::other("output limit"));
            }
            session.output.push_str(delta);
            state.event(&job.session_id, &job.run_id, "output.delta", delta, None);
            Ok(())
        })
    }));
    let Ok(mut state) = state.lock() else {
        return;
    };
    if !state
        .active
        .as_ref()
        .is_some_and(|a| a.run_id == job.run_id && a.session_id == job.session_id)
    {
        return;
    }
    let cancelled =
        job.cancel.load(Ordering::SeqCst) || matches!(result, Ok(Err(ProviderError::Cancelled)));
    let observed = state
        .sessions
        .iter()
        .find(|s| s.id == job.session_id)
        .map(|s| s.output.clone())
        .unwrap_or_default();
    let coherent = result.as_ref().is_ok_and(|result| {
        result
            .as_ref()
            .is_ok_and(|text| *text == observed && text.len() <= MAX_OUTPUT)
    });
    let (name, terminal, text) = if cancelled {
        ("run.cancelled", "cancelled", observed.clone())
    } else {
        match result {
            Ok(Ok(text)) if coherent => ("run.completed", "completed", text),
            _ => ("run.failed", "failed", observed),
        }
    };
    let committed = state
        .store
        .finish(
            &job.session_id,
            &job.run_id,
            terminal,
            &text,
            &job.config.model,
        )
        .is_ok();
    let (name, terminal) = if committed {
        (name, terminal)
    } else {
        state.storage_failed = true;
        ("run.failed", "failed")
    };
    let session = state
        .sessions
        .iter_mut()
        .find(|s| s.id == job.session_id)
        .unwrap();
    session.state = terminal.into();
    session.output.clone_from(&text);
    session.detail = if !committed {
        "storage-failure"
    } else if terminal == "failed" {
        "provider-failed"
    } else {
        ""
    }
    .into();
    state.event(&job.session_id, &job.run_id, name, &text, Some(terminal));
    state.active = None;
}

fn await_approval(
    state: &Arc<Mutex<State>>,
    run_id: &str,
    session_id: &str,
    step: usize,
    invocation: (&str, &Value),
    cancel: &AtomicBool,
    force_approval: bool,
) -> Result<(), &'static str> {
    let (tool, args) = invocation;
    let (receiver, deadline) = {
        let mut state = state.lock().map_err(|_| "storage-failure")?;
        state.current_workspace()?;
        if cancel.load(Ordering::SeqCst) {
            return Err("cancelled");
        }
        if !force_approval
            && !state.approval_required
            && !matches!(
                tool,
                "write-file" | "edit-file" | "process-exec" | "grep" | "glob" | "stat-path"
            )
        {
            return Ok(());
        }
        state.begin_approval(run_id, session_id, step, tool, args)?
    };
    loop {
        if cancel.load(Ordering::SeqCst) || Instant::now() >= deadline {
            let mut state = state.lock().map_err(|_| "storage-failure")?;
            if let Some(pending) = state.pending_approval.take() {
                if pending.run_id == run_id {
                    let resolution = if cancel.load(Ordering::SeqCst) {
                        crate::agent_store::ApprovalResolution::Cancelled
                    } else {
                        crate::agent_store::ApprovalResolution::Timeout
                    };
                    if state
                        .agent_store
                        .resolve_approval(run_id, &pending.id, resolution)
                        .is_err()
                    {
                        state.storage_failed = true;
                        return Err("storage-failure");
                    }
                } else {
                    state.pending_approval = Some(pending);
                }
            }
            return Err(if cancel.load(Ordering::SeqCst) {
                "cancelled"
            } else {
                "approval-timeout"
            });
        }
        match receiver.recv_timeout(Duration::from_millis(100)) {
            Ok(true) if !cancel.load(Ordering::SeqCst) && Instant::now() < deadline => {
                return Ok(());
            }
            Ok(_) => {
                return Err(if cancel.load(Ordering::SeqCst) {
                    "cancelled"
                } else {
                    "approval-denied"
                });
            }
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {}
            Err(_) => return Err("approval-denied"),
        }
    }
}

fn agent_worker(state: Arc<Mutex<State>>, job: Job) {
    let Some((workspace, goal)) = job.agent else {
        return;
    };
    let mutation_authorized = AtomicBool::new(false);
    let current_tool = Mutex::new(None::<(usize, String)>);
    if let Ok(mut locked) = state.lock() {
        locked.agent_progress(
            &job.session_id,
            &job.run_id,
            "agent.activity",
            0,
            "",
            "Agent started",
        );
    }
    let result =
        std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            crate::agent::run(
                &job.config,
                job.request,
                &goal,
                &workspace,
                &job.cancel,
                |step, tool, descriptor_version, observation, completed| {
                    let Ok(mut locked) = state.lock() else {
                        return false;
                    };
                    if job.cancel.load(Ordering::SeqCst)
                        || !locked.active.as_ref().is_some_and(|a| {
                            a.run_id == job.run_id && a.session_id == job.session_id
                        })
                    {
                        return false;
                    }
                    if observation["_event"] == "plan-step" && !completed {
                        let result = locked.agent_store.plan_step(
                            &job.run_id,
                            step,
                            observation["status"].as_str().unwrap_or(""),
                            observation["reason"].as_str().unwrap_or(""),
                        );
                        if result.is_err() {
                            locked.storage_failed = true;
                        }
                        return result.is_ok();
                    }
                    if observation["_event"] == "running" && !completed {
                        if let Ok(mut current) = current_tool.lock() {
                            *current = Some((step, tool.to_owned()));
                        }
                        locked.agent_progress(
                            &job.session_id,
                            &job.run_id,
                            "tool.running",
                            step,
                            tool,
                            "running",
                        );
                        return true;
                    }
                    let saved = if completed && observation["failureCategory"].is_string() {
                        locked.agent_store.observe_failure(
                            &job.run_id,
                            step,
                            observation["path"].as_str().unwrap_or(""),
                            observation["failureCategory"].as_str().unwrap_or(""),
                        )
                    } else if completed {
                        locked.agent_store.observe(
                            &job.run_id,
                            step,
                            observation["path"].as_str().unwrap_or(""),
                            observation["content"].as_str().unwrap_or(""),
                            observation["complete"] == true,
                        )
                    } else {
                        locked.agent_store.intention(
                            &job.run_id,
                            step,
                            tool,
                            descriptor_version,
                            &observation.to_string(),
                        )
                    };
                    if saved.is_err() {
                        locked.storage_failed = true;
                    } else if completed {
                        if let Ok(mut current) = current_tool.lock() {
                            *current = None;
                        }
                        let phase = if observation["failureCategory"].is_string()
                            || observation["complete"] != true
                        {
                            "failed"
                        } else {
                            "completed"
                        };
                        locked.agent_progress(
                            &job.session_id,
                            &job.run_id,
                            "tool.result",
                            step,
                            tool,
                            phase,
                        );
                        locked.agent_progress(
                            &job.session_id,
                            &job.run_id,
                            "agent.activity",
                            step,
                            tool,
                            if phase == "completed" {
                                "Tool completed"
                            } else {
                                "Tool failed"
                            },
                        );
                    } else {
                        locked.agent_progress(
                            &job.session_id,
                            &job.run_id,
                            "tool.requested",
                            step,
                            tool,
                            "requested",
                        );
                    }
                    saved.is_ok()
                },
                |step, tool, args| {
                    let result = await_approval(
                        &state,
                        &job.run_id,
                        &job.session_id,
                        step,
                        (tool, args),
                        &job.cancel,
                        crate::agent::classified_inspect_goal(&goal).is_some(),
                    );
                    if result.is_err()
                        && let Ok(mut locked) = state.lock()
                        && locked.active.as_ref().is_some_and(|a| {
                            a.run_id == job.run_id && a.session_id == job.session_id
                        })
                    {
                        locked.agent_progress(
                            &job.session_id,
                            &job.run_id,
                            "tool.result",
                            step,
                            tool,
                            "failed",
                        );
                    }
                    if matches!(tool, "write-file" | "edit-file") && result.is_ok() {
                        mutation_authorized.store(true, Ordering::SeqCst);
                    }
                    result
                },
            )
        }));
    let Ok(mut locked) = state.lock() else {
        return;
    };
    if !locked
        .active
        .as_ref()
        .is_some_and(|a| a.run_id == job.run_id && a.session_id == job.session_id)
    {
        return;
    }
    if let Ok(mut current) = current_tool.lock()
        && let Some((step, tool)) = current.take()
    {
        locked.agent_progress(
            &job.session_id,
            &job.run_id,
            "tool.result",
            step,
            &tool,
            "failed",
        );
    }
    let timed_out = matches!(result, Ok(Err("plan-timeout")));
    let cancelled =
        !timed_out && (job.cancel.load(Ordering::SeqCst) || matches!(result, Ok(Err("cancelled"))));
    let (mut terminal, mut text, reason) = if cancelled {
        ("cancelled", String::new(), "cancelled")
    } else {
        match result {
            Ok(Ok(text)) => ("completed", text, ""),
            Ok(Err(reason)) => ("failed", String::new(), reason),
            Err(_) => ("failed", String::new(), "runtime-failed"),
        }
    };
    if terminal != "completed"
        && mutation_authorized.load(Ordering::SeqCst)
        && reason != "write-not-committed"
    {
        text = "A file may have been created or modified. Inspect the workspace before retrying."
            .into();
    }
    let committed = !locked.storage_failed
        && locked
            .agent_store
            .finish(&job.run_id, terminal, &text, reason)
            .is_ok()
        && locked
            .store
            .finish(
                &job.session_id,
                &job.run_id,
                terminal,
                &text,
                &job.config.model,
            )
            .is_ok()
        && locked.agent_store.confirm(&job.run_id).is_ok();
    if !committed {
        locked.storage_failed = true;
        if locked
            .store
            .validate_agent_owner(&job.run_id, &job.session_id)
            .is_ok()
        {
            let _ = locked
                .store
                .invalidate_agent(&job.run_id, mutation_authorized.load(Ordering::SeqCst));
        }
        terminal = "failed";
        if mutation_authorized.load(Ordering::SeqCst) {
            text =
                "A file may have been created or modified. Inspect the workspace before retrying."
                    .into();
        } else {
            text.clear();
        }
    }
    let Some(session) = locked.sessions.iter_mut().find(|s| s.id == job.session_id) else {
        return;
    };
    session.state = terminal.into();
    session.output.clone_from(&text);
    session.detail = if !committed {
        "storage-failure"
    } else {
        reason
    }
    .into();
    let event = match terminal {
        "completed" => "run.completed",
        "cancelled" => "run.cancelled",
        _ => "run.failed",
    };
    locked.event(&job.session_id, &job.run_id, event, &text, Some(terminal));
    locked.active = None;
}

#[cfg(test)]
mod model_tests {
    use super::*;
    use std::{fs, os::unix::fs::DirBuilderExt, sync::mpsc::sync_channel};

    #[test]
    fn model_selection_is_catalog_bound_and_busy_safe() {
        let path = std::env::temp_dir().join(format!("sentinel-model-test-{}", std::process::id()));
        fs::DirBuilder::new().mode(0o700).create(&path).unwrap();
        let path = path.canonicalize().unwrap();
        let store = Store::open(&path).unwrap();
        let config = Config::parse("http://127.0.0.1:12345", "initial").unwrap();
        let settings = crate::settings::Settings::open(&path).unwrap();
        let audit = crate::agent_store::AgentStore::open(&path).unwrap();
        let memory = crate::memory::MemoryStore::open(&path).unwrap();
        let mut state = State::new(
            Some(config.clone()),
            store,
            settings,
            audit,
            memory,
            AgentOptions {
                workspaces: Vec::new(),
                selected_workspace: None,
                approval_required: false,
                process_enabled: false,
            },
            path.clone(),
        )
        .unwrap();
        let (sender, _receiver) = sync_channel(1);
        let peer = Arc::new(Peer {
            sender,
            close: Box::new(|| {}),
        });
        let mut job = None;
        state
            .install_catalog(&config, 0, vec!["next".into()])
            .unwrap();
        assert!(
            state
                .handle(
                    RequestPayload::ModelSelect {
                        provider_id: config.provider_id.clone(),
                        model_id: "missing".into()
                    },
                    &peer,
                    &mut job
                )
                .is_err()
        );
        state
            .handle(
                RequestPayload::ModelSelect {
                    provider_id: config.provider_id.clone(),
                    model_id: "next".into(),
                },
                &peer,
                &mut job,
            )
            .unwrap();
        assert_eq!(state.provider.as_ref().unwrap().model, "next");
        assert!(state.install_catalog(&config, 0, vec![]).is_err());
        state.active = Some(Active {
            run_id: "r".into(),
            session_id: "s".into(),
            cancel: Arc::new(AtomicBool::new(false)),
        });
        assert!(matches!(
            state.handle(
                RequestPayload::ProviderSelect {
                    provider_id: config.provider_id.clone()
                },
                &peer,
                &mut job
            ),
            Err("runtime-busy")
        ));
        assert!(matches!(
            state.handle(
                RequestPayload::ModelSelect {
                    provider_id: config.provider_id.clone(),
                    model_id: "next".into()
                },
                &peer,
                &mut job
            ),
            Err("runtime-busy")
        ));
        state.active = None;
        let external = rusqlite::Connection::open(path.join(crate::settings::DATABASE)).unwrap();
        external.execute_batch("BEGIN IMMEDIATE").unwrap();
        let select = || RequestPayload::ModelSelect {
            provider_id: config.provider_id.clone(),
            model_id: "next".into(),
        };
        assert!(matches!(
            state.handle(select(), &peer, &mut job),
            Err("storage-failure")
        ));
        assert!(!state.settings_failed);
        assert_eq!(state.provider.as_ref().unwrap().model, "next");
        external.execute_batch("ROLLBACK").unwrap();
        state.handle(select(), &peer, &mut job).unwrap();
        external.execute_batch("DROP TABLE selection").unwrap();
        assert!(matches!(
            state.handle(select(), &peer, &mut job),
            Err("storage-failure")
        ));
        assert!(state.settings_failed);
        assert!(matches!(
            state.handle(RequestPayload::ModelCurrent {}, &peer, &mut job),
            Err("storage-failure")
        ));
        drop(external);
        drop(state);
        fs::remove_dir_all(path).unwrap();
    }
}
