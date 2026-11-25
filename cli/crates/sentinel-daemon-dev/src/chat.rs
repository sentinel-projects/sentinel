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
    archived: bool,
    pinned: bool,
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
    risk: u8,
    resources: Vec<Value>,
    deadline: Instant,
    sender: SyncSender<bool>,
}
impl PendingApproval {
    fn snapshot(&self) -> Value {
        json!({"approval_id":self.id,"tool":self.tool,"detail":self.detail,"risk":self.risk,"resources":self.resources})
    }
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
struct VoiceSession {
    owner: std::sync::Weak<Peer>,
    pcm: Vec<u8>,
    phase: &'static str,
    transcript: String,
    cancel: Arc<AtomicBool>,
    started: Instant,
}
pub struct VoiceJob {
    inspection: crate::speech_inspection::Inspection,
    profile: PathBuf,
    pcm: Vec<u8>,
    cancel: Arc<AtomicBool>,
}
pub struct State {
    profile: PathBuf,
    generation: String,
    counter: u64,
    sessions: Vec<Session>,
    active: Option<Active>,
    agent_live_activity: Option<(String, Value)>,
    provider: Option<Config>,
    configured_providers: Vec<Config>,
    startup_providers: Vec<Config>,
    catalog: Vec<String>,
    catalog_known: bool,
    catalog_generation: u64,
    store: Store,
    settings: crate::settings::Settings,
    agent_store: crate::agent_store::AgentStore,
    model_ops: crate::model_ops::Operations,
    workspace: Option<Arc<crate::agent_gateway::Workspace>>,
    plugin_root: Option<Arc<crate::agent_gateway::Workspace>>,
    speech_inspection: Option<crate::speech_inspection::Inspection>,
    speech_execution: bool,
    voice: Option<VoiceSession>,
    pending_voice: Option<VoiceJob>,
    voice_worker_token: Option<Arc<AtomicBool>>,
    workspaces: Vec<ConfiguredWorkspace>,
    selected_workspace: Option<String>,
    backup_transfers: Vec<BackupTransfer>,
    change_baselines:
        std::collections::BTreeMap<String, (String, crate::agent_gateway::WorkspaceSnapshot)>,
    approval_required: bool,
    process_enabled: bool,
    pending_approval: Option<PendingApproval>,
    stopping: bool,
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
impl Job {
    pub fn cancel_before_dispatch(&self) {
        self.cancel.store(true, Ordering::SeqCst);
    }
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
                    archived: s.archived,
                    pinned: s.pinned,
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
            agent_live_activity: None,
            configured_providers: provider.iter().cloned().collect(),
            startup_providers: provider.iter().cloned().collect(),
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
            speech_inspection: None,
            speech_execution: false,
            voice: None,
            pending_voice: None,
            voice_worker_token: None,
            workspaces: agent.workspaces,
            selected_workspace: agent.selected_workspace,
            backup_transfers: Vec::new(),
            change_baselines: Default::default(),
            approval_required: agent.approval_required,
            process_enabled: agent.process_enabled,
            pending_approval: None,
            stopping: false,
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
    pub fn set_speech_inspection(&mut self, value: crate::speech_inspection::Inspection) {
        self.speech_inspection = Some(value);
    }
    pub fn enable_speech_execution(&mut self) {
        self.speech_execution = true;
    }
    pub fn has_voice(&self) -> bool {
        self.speech_execution
            && self.speech_inspection.is_some()
            && crate::speech_runner::available()
    }
    pub fn take_voice_job(&mut self) -> Option<VoiceJob> {
        let job = self.pending_voice.take()?;
        self.voice_worker_token = Some(job.cancel.clone());
        Some(job)
    }
    pub fn voice_worker_running(&self) -> bool {
        self.voice_worker_token.is_some()
    }
    fn expire_voice(&mut self) {
        if self.voice.as_ref().is_some_and(|v| {
            v.owner.strong_count() == 0
                || (v.phase == "Capturing" && v.started.elapsed() > Duration::from_secs(120))
        }) {
            if let Some(v) = self.voice.take() {
                v.cancel.store(true, Ordering::SeqCst);
            }
        }
    }
    pub fn has_speech_inspection(&self) -> bool {
        self.speech_inspection.is_some()
    }
    pub fn has_agent(&self) -> bool {
        self.provider
            .as_ref()
            .is_some_and(|provider| !provider.is_cloud())
            && self.workspace.is_some()
    }
    pub fn has_image_attachments(&self) -> bool {
        if let Some(provider) = self
            .provider
            .as_ref()
            .filter(|provider| provider.is_cloud())
        {
            return self.has_workspace_files() && provider.vision_tokens().is_some();
        }
        self.has_workspace_files()
            && self
                .configured_providers
                .iter()
                .any(|p| p.vision_tokens().is_some())
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
        if self.active.is_some() || self.ensure_session_writable(session_id).is_err() {
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
    pub fn has_configured_ollama(&self) -> bool {
        self.configured_providers
            .iter()
            .any(|provider| provider.provider_id == "ollama")
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
    fn set_provider_endpoint(
        &mut self,
        key: &str,
        provider_id: &str,
        endpoint: &str,
    ) -> Result<ResponsePayload, &'static str> {
        if self.active.is_some() || self.model_operation_active()? {
            return Err("runtime-busy");
        }
        let config = self
            .configured_providers
            .iter_mut()
            .find(|config| config.provider_id == provider_id)
            .ok_or("provider-unavailable")?;
        let startup = crate::provider_catalog::find(&self.startup_providers, provider_id)
            .ok_or("provider-unavailable")?;
        let updated =
            crate::provider_catalog::rebind(startup, endpoint).map_err(|_| "invalid-payload")?;
        if updated.endpoint == config.endpoint {
            return typed("desktop.setting", json!({"accepted":true}));
        }
        let active = self
            .provider
            .as_ref()
            .is_some_and(|current| current.provider_id == provider_id);
        let generation = self
            .catalog_generation
            .checked_add(1)
            .ok_or("limit-exceeded")?;
        if let Err(error) = self
            .settings
            .set_provider_endpoint(key, startup, &updated, active)
        {
            self.settings_failed = !error.safe_conflict;
            return Err("storage-failure");
        }
        *config = updated.clone();
        if active {
            self.provider = Some(updated);
            self.catalog_generation = generation;
            self.catalog.clear();
            self.catalog_known = false;
            self.model_ops = crate::model_ops::Operations::default();
        }
        typed("desktop.setting", json!({"accepted":true}))
    }

    pub fn set_configured_providers(&mut self, startup: Vec<Config>, effective: Vec<Config>) {
        self.startup_providers = startup;
        self.configured_providers = effective;
    }
    pub fn has_cloud_history(&self) -> bool {
        self.provider
            .as_ref()
            .is_some_and(|config| config.is_cloud() && config.cloud_history)
    }

    pub fn has_cloud_provider(&self) -> bool {
        self.provider.as_ref().is_some_and(Config::is_cloud)
    }
    pub fn has_provider(&self) -> bool {
        self.provider.is_some()
    }
    pub fn discovery_config(&self) -> Result<(Config, u64), &'static str> {
        if self.stopping {
            return Err("runtime-stopping");
        }
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
        if self.stopping
            || self.model_operation_active()?
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
        self.model_ops
            .observe_catalog(config, &models)
            .map_err(|_| "runtime-unavailable")?;
        self.catalog = models;
        self.catalog_known = true;
        Ok(reply)
    }
    pub fn counts(&self) -> (u64, u64) {
        (u64::from(self.active.is_some()), self.sessions.len() as u64)
    }
    pub fn drop_peer(&mut self, peer: &Arc<Peer>) {
        if self.voice.as_ref().is_some_and(|v| {
            v.owner
                .upgrade()
                .is_some_and(|owner| Arc::ptr_eq(&owner, peer))
        }) {
            if let Some(v) = self.voice.take() {
                v.cancel.store(true, Ordering::SeqCst);
            }
        }

        self.backup_transfers.retain(|transfer| {
            transfer
                .owner
                .upgrade()
                .is_some_and(|owner| !Arc::ptr_eq(&owner, peer))
        });
    }
    pub fn shutdown_pending(&self) -> io::Result<bool> {
        Ok(self.active.is_some() || self.voice_worker_running() || self.model_ops.is_active()?)
    }
    pub fn stop(&mut self) {
        self.stopping = true;
        if let Some(v) = self.voice.take() {
            v.cancel.store(true, Ordering::SeqCst);
        }
        self.pending_voice.take();
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
        if self.stopping {
            return Err("runtime-stopping");
        }
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
                        "activeRuntimeLocalOnlySummary":if config.cloud_history {"Cloud Chat can include prior text turns from the same provider and model."} else if config.is_cloud() {"Cloud Chat sends the current prompt only."} else {"Configured for a local loopback provider."}})
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
                    json!({"properties":{"cloudImageInput":self.has_cloud_provider() && self.has_image_attachments(),"agent_tools":if !self.storage_failed && self.has_agent() && self.current_workspace().is_ok() {
                        crate::tool_registry::catalog(self.has_process(), self.approval_required)
                    } else { vec![] }},"providers":self.configured_providers.iter().map(|config| json!({
                        "provider_id":config.provider_id,"endpoint":config.endpoint,"local":!config.is_cloud(),
                        "health":"Unknown","catalog":if self.catalog_known && self.provider.as_ref().is_some_and(|p| p.provider_id == config.provider_id) {"Cached"} else {"Unknown"},
                        "models":if self.provider.as_ref().is_some_and(|p| p.provider_id == config.provider_id) {self.catalog.iter().take(16).collect::<Vec<_>>()} else {vec![]},
                        "selected": self.provider.as_ref().is_some_and(|current| current.provider_id == config.provider_id)
                    })).collect::<Vec<_>>(),"workspace":workspace,"workspaces":workspaces}),
                )
            }
            RequestPayload::VoiceState {} => {
                self.expire_voice();
                let ready = self.has_voice();
                let owned = self.voice.as_ref().is_some_and(|v| {
                    v.owner
                        .upgrade()
                        .is_some_and(|owner| Arc::ptr_eq(&owner, peer))
                });
                let phase = if !ready {
                    "Unavailable"
                } else if owned {
                    self.voice.as_ref().map_or("Idle", |v| v.phase)
                } else {
                    "Idle"
                };
                let text = if owned {
                    self.voice.as_ref().map_or("", |v| v.transcript.as_str())
                } else {
                    ""
                };
                typed(
                    "voice.state",
                    json!({"state":phase,"available":ready && (owned || (self.voice.is_none() && self.voice_worker_token.is_none())),"owned":owned,"transcript":text,"failure":if phase=="Failed" {"TranscriptionFailed"} else if !ready {"RuntimeUnavailable"} else {"None"},"input_device_id":"","vad_enabled":false}),
                )
            }
            RequestPayload::VoiceAction { action } => {
                self.expire_voice();
                if action != "start" && action != "cancel" {
                    return Err("invalid-voice-action");
                }
                if action == "start" {
                    if !self.has_voice() || self.active.is_some() {
                        return Err("voice-unavailable");
                    }
                    if self.voice.is_some() || self.voice_worker_token.is_some() {
                        return Err("voice-busy");
                    }
                    self.voice = Some(VoiceSession {
                        owner: Arc::downgrade(peer),
                        pcm: Vec::new(),
                        phase: "Capturing",
                        transcript: String::new(),
                        cancel: Arc::new(AtomicBool::new(false)),
                        started: Instant::now(),
                    });
                } else {
                    if !self.voice.as_ref().is_some_and(|v| {
                        v.owner
                            .upgrade()
                            .is_some_and(|owner| Arc::ptr_eq(&owner, peer))
                    }) {
                        return Err("voice-not-owned");
                    }
                    if let Some(v) = self.voice.take() {
                        v.cancel.store(true, Ordering::SeqCst);
                    }
                }
                typed("voice.action", json!({"accepted":true}))
            }
            RequestPayload::VoiceAudio {
                pcm,
                r#final,
                speech,
            } => {
                use base64::{Engine as _, engine::general_purpose::STANDARD};
                self.expire_voice();
                let voice = self
                    .voice
                    .as_mut()
                    .filter(|v| {
                        v.owner
                            .upgrade()
                            .is_some_and(|owner| Arc::ptr_eq(&owner, peer))
                            && v.phase == "Capturing"
                    })
                    .ok_or("voice-not-owned")?;
                let data = if pcm.len() <= 87_384 {
                    STANDARD.decode(&pcm).ok()
                } else {
                    None
                };
                let Some(data) = data.filter(|d| {
                    d.len() <= sentinel_ipc::voice::MAX_PCM_CHUNK
                        && d.len() % 2 == 0
                        && voice
                            .pcm
                            .len()
                            .checked_add(d.len())
                            .is_some_and(|n| n <= sentinel_ipc::voice::MAX_PCM_CAPTURE)
                }) else {
                    if let Some(v) = self.voice.take() {
                        v.cancel.store(true, Ordering::SeqCst);
                    }
                    return Err("invalid-captured-audio");
                };
                voice.pcm.extend_from_slice(&data);
                if r#final {
                    if voice.pcm.is_empty() || !speech {
                        if let Some(v) = self.voice.take() {
                            v.cancel.store(true, Ordering::SeqCst);
                        }
                        return Err("invalid-captured-audio");
                    }
                    voice.phase = "Transcribing";
                    self.pending_voice = Some(VoiceJob {
                        inspection: self.speech_inspection.clone().ok_or("voice-unavailable")?,
                        profile: self.profile.clone(),
                        pcm: std::mem::take(&mut voice.pcm),
                        cancel: voice.cancel.clone(),
                    });
                }
                typed("voice.audio", json!({"accepted":true}))
            }
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
                if action == "speechSettingsState" && arguments.is_empty() =>
            {
                let inspected = self.speech_inspection.as_ref();
                typed(
                    "desktop.settings_service",
                    json!({"result":{
                        "sttProviderId":if inspected.is_some() {"whisper-cpp"} else {""}, "sttModelId":"", "sttAvailable":self.has_voice(),
                        "ttsProviderId":"", "ttsModelId":"", "ttsAvailable":false,
                        "ttsVoices":[], "inputDeviceIds":[], "outputDeviceIds":[],
                        "selectedInputId":"", "selectedOutputId":"", "vadEnabled":false,
                        "rawRecordingRetention":false, "sttLocal":inspected.is_some(), "ttsLocal":false,
                        "inspection":if inspected.is_some() {"DigestAndHeaderMatched"} else {"NotConfigured"},
                        "inspectionMeaning":"Artifacts checked at daemon startup; model compatibility and transcription success are not verified.",
                        "binarySha256":inspected.map(|v|v.binary_sha256.as_str()),
                        "modelSha256":inspected.map(|v|v.model_sha256.as_str()),
                        "modelBytes":inspected.map(|v|v.model_bytes)
                    }}),
                )
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
                let active = self
                    .provider
                    .as_ref()
                    .filter(|config| config.provider_id == id);
                let configured = active
                    .or_else(|| crate::provider_catalog::find(&self.configured_providers, id));
                let result = if let Some(config) = configured {
                    let catalog = if active.is_some() && self.catalog_known {
                        Some(&self.catalog)
                    } else {
                        None
                    };
                    json!({"id":id,"known":true,"health":"Unknown","catalog":if catalog.is_some() {"Cached"} else {"Unknown"},"local":!config.is_cloud(),"endpoint":config.endpoint,"discoveredModels":catalog.map_or(&[][..], Vec::as_slice),"contextWindow":config.context_window_tokens,"configuredModel":config.model})
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
                    "archiveConversation"
                    | "unarchiveConversation"
                    | "pinConversation"
                    | "unpinConversation"
                        if arguments.len() == 1 =>
                    {
                        if arguments[0].as_str() != Some(&session_id) {
                            return Err("invalid-payload");
                        }
                        let session = self
                            .sessions
                            .iter_mut()
                            .find(|s| s.id == session_id)
                            .ok_or("unknown-session")?;
                        let archived = match action.as_str() {
                            "archiveConversation" => true,
                            "unarchiveConversation" => false,
                            _ => session.archived,
                        };
                        let pinned = match action.as_str() {
                            "pinConversation" => true,
                            "unpinConversation" => false,
                            _ => session.pinned,
                        };
                        if let Err(error) =
                            self.store.set_session_flags(&session_id, archived, pinned)
                        {
                            self.storage_failed = !error.safe_conflict;
                            return Err("storage-failure");
                        }
                        session.archived = archived;
                        session.pinned = pinned;
                        typed(
                            "desktop.action",
                            json!({"accepted":true,"result":{"value":true}}),
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
                for (key, provider_id) in crate::provider_catalog::ENDPOINT_KEYS {
                    if let Some(config) =
                        crate::provider_catalog::find(&self.configured_providers, provider_id)
                    {
                        values[key] = json!(config.endpoint);
                    } else if let Some(map) = values.as_object_mut() {
                        map.remove(key);
                    }
                }
                typed("desktop.settings", json!({"values":values}))
            }
            RequestPayload::DesktopSetting { key, value } => {
                if self.settings_failed {
                    return Err("storage-failure");
                }
                if let Some(provider_id) = crate::provider_catalog::provider_for_key(&key) {
                    return self.set_provider_endpoint(&key, provider_id, &value);
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
                confirmation,
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
                if action == "removeModel" && confirmation.as_deref() != Some(value.as_str()) {
                    return Err("confirmation-required");
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
                if self
                    .provider
                    .as_ref()
                    .is_none_or(|current| current.provider_id != provider_id)
                {
                    let config =
                        crate::provider_catalog::find(&self.configured_providers, &provider_id)
                            .cloned()
                            .ok_or("provider-unavailable")?;
                    let generation = self
                        .catalog_generation
                        .checked_add(1)
                        .ok_or("limit-exceeded")?;
                    if let Err(error) = self.settings.select(&config, &config.model) {
                        self.settings_failed = !error.safe_conflict;
                        return Err("storage-failure");
                    }
                    self.provider = Some(config);
                    self.catalog_generation = generation;
                    self.catalog.clear();
                    self.catalog_known = false;
                }
                let config = self.provider.as_ref().ok_or("provider-unavailable")?;
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
                    archived: false,
                    pinned: false,
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
                let mut ordered: Vec<_> = self.sessions.iter().collect();
                ordered.sort_by_key(|session| (session.archived, !session.pinned));
                let sessions: Vec<Value> = ordered
                    .into_iter()
                    .map(|s| json!({"session_id":s.id,"title":s.title,"state":s.state,"created_at":s.created_at,"updated_at":s.updated_at,"archived":s.archived,"pinned":s.pinned}))
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
                let mut messages = match self.store.messages(&session_id) {
                    Ok(messages) => messages,
                    Err(_) => {
                        self.storage_failed = true;
                        return Err("storage-failure");
                    }
                };
                for message in &mut messages {
                    if let Some(attachments) = message["attachments"].as_array_mut() {
                        for attachment in attachments {
                            if crate::attachment::stored_image_mime(&attachment["mime"]).is_some() {
                                attachment["image"] = Value::Null;
                            }
                        }
                    }
                }
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
                self.ensure_session_writable(&session_id)?;
                let attachment_values = attachments.unwrap_or_default();
                if config.is_cloud() && !attachment_values.is_empty() && !self.has_workspace_files()
                {
                    return Err("cloud-attachments-unavailable");
                }
                let loaded = if attachment_values.is_empty() {
                    Vec::new()
                } else {
                    let workspace = self.current_workspace()?;
                    crate::attachment::load(&workspace, &attachment_values)
                        .map_err(|_| "invalid-attachments")?
                };
                let encoded = serde_json::to_string(&loaded).map_err(|_| "invalid-attachments")?;
                let (provider_text, images) =
                    crate::attachment::parts(&text, &encoded).map_err(|_| "invalid-attachments")?;
                if config.is_cloud()
                    && !images.is_empty()
                    && (!config.supports_cloud_vision() || config.vision_tokens().is_none())
                {
                    return Err("cloud-image-attachments-unavailable");
                }
                let profile = if config.is_cloud() {
                    String::new()
                } else {
                    self.response_profile()?
                };
                // Reject a mandatory-only request before even reading history or writing a run.
                match provider::prepare_with_images(&config, &provider_text, &images, &[], &profile)
                {
                    Ok(_) => {}
                    Err(ProviderError::InvalidRequest(_)) => return Err("message-too-large"),
                    Err(ProviderError::InvalidConfig(_)) => return Err("vision-unavailable"),
                    Err(ProviderError::ContextLimitExceeded) => {
                        return Err("context-limit-exceeded");
                    }
                    Err(_) => return Err("runtime-unavailable"),
                }
                let history = {
                    let loaded = if config.is_cloud() {
                        if config.cloud_history {
                            self.store.cloud_context_turns(
                                &session_id,
                                &config.provider_id,
                                &config.model,
                            )
                        } else {
                            Ok(Vec::new())
                        }
                    } else {
                        self.store.context_turns(&session_id)
                    };
                    match loaded {
                        Ok(history) => history,
                        Err(error) => {
                            self.storage_failed = !store::is_conflict(&error);
                            return Err("storage-failure");
                        }
                    }
                };
                let request = match provider::prepare_with_images(
                    &config,
                    &provider_text,
                    &images,
                    &history,
                    &profile,
                ) {
                    Ok(request) => request,
                    Err(ProviderError::InvalidRequest(_)) => return Err("message-too-large"),
                    Err(ProviderError::InvalidConfig(_)) => return Err("vision-unavailable"),
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
                if self.has_cloud_provider() {
                    return Err("unsupported-capability");
                }
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
                self.ensure_session_writable(&session_id)?;
                let config = self.provider.clone().ok_or("provider-unavailable")?;
                let workspace = self.current_workspace()?;
                if crate::agent::requires_process(&text) && !self.has_process() {
                    return Err("runtime-unavailable");
                }
                let request =
                    crate::agent::prepare(&config, &text).map_err(|error| match error {
                        ProviderError::InvalidRequest(_) => "invalid-payload",
                        ProviderError::ContextLimitExceeded => "context-limit-exceeded",
                        _ => "runtime-unavailable",
                    })?;
                let run_id = self.id("run");
                let plan_metadata = crate::agent::plan_metadata(&text);
                let admission = crate::agent_store::AgentAdmission {
                    run_id: &run_id,
                    session_id: &session_id,
                    goal: &text,
                    provider: &config.provider_id,
                    model: &config.model,
                    plan: plan_metadata.as_deref(),
                };
                match self.store.begin_agent(&self.profile, &admission) {
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
                if self.storage_failed || self.settings_failed {
                    return Err("storage-failure");
                }
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
                if !self.sessions.iter().any(|session| {
                    session.id == session_id
                        && session
                            .peers
                            .iter()
                            .any(|candidate| candidate.ptr_eq(&Arc::downgrade(peer)))
                }) {
                    return Err("permission-denied");
                }
                if allow {
                    self.current_workspace()?;
                }
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
        let mut snapshot = json!({"session_id":session.id,"state":session.state,"output":session.output,"run_id":session.run_id,"run_type":if session.agent {"agent"} else {"chat"},"provider_id":binding,"model_id":model,"server_generation":self.generation,"event_sequence":session.seq,"title":session.title,"detail":session.detail,"created_at":session.created_at});
        if let Some(pending) = self.pending_approval.as_ref()
            && pending.session_id == session_id
            && pending.run_id == session.run_id
        {
            snapshot["approval_id"] = json!(pending.id);
            snapshot["approval"] = pending.snapshot();
        }
        if self
            .active
            .as_ref()
            .is_some_and(|active| active.run_id == session.run_id)
            && let Some((run_id, activity)) = &self.agent_live_activity
            && run_id == &session.run_id
        {
            snapshot["properties"] = json!({"agent_activity":activity});
        }
        typed(name, snapshot)
    }
    fn ensure_session_writable(&self, session_id: &str) -> Result<(), &'static str> {
        match self
            .sessions
            .iter()
            .find(|session| session.id == session_id)
        {
            Some(session) if session.archived => Err("session-archived"),
            Some(_) => Ok(()),
            None => Err("unknown-session"),
        }
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
        self.ensure_session_writable(sid)?;
        let config = self.provider.clone().ok_or("provider-unavailable")?;
        if config.is_cloud() {
            return Err("unsupported-capability");
        }
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
        let (provider_text, images) = self
            .store
            .user_parts(sid, user_id, &text)
            .map_err(|_| "storage-failure")?;
        let profile = self.response_profile()?;
        match provider::prepare_with_images(&config, &provider_text, &images, &[], &profile) {
            Ok(_) => {}
            Err(ProviderError::InvalidRequest(_)) => return Err("message-too-large"),
            Err(ProviderError::InvalidConfig(_)) => return Err("vision-unavailable"),
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
        let request = match provider::prepare_with_images(
            &config,
            &provider_text,
            &images,
            &history,
            &profile,
        ) {
            Ok(request) => request,
            Err(ProviderError::InvalidRequest(_)) => return Err("message-too-large"),
            Err(ProviderError::InvalidConfig(_)) => return Err("vision-unavailable"),
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
                archived: false,
                pinned: false,
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
    fn approval_event(&mut self) {
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
        let mut payload = pending.snapshot();
        payload["session_id"] = json!(pending.session_id);
        payload["run_id"] = json!(pending.run_id);
        payload["server_generation"] = json!(self.generation);
        payload["event_sequence"] = json!(session.seq);
        payload["state"] = json!("approval");
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
        session.peers.retain(|weak| {
            weak.upgrade()
                .is_some_and(|peer| peer.send(bytes.clone()).is_ok())
        });
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
        let descriptor = crate::tool_registry::find(tool).ok_or("unknown-tool")?;
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
            risk: descriptor.event_risk(),
            resources: vec![descriptor.approval_resource(args)],
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
        self.approval_event();
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
            json!({"session_id":sid,"run_id":rid,"tool":tool,"detail":match phase { "requested" => "Tool requested", "running" => "Tool running", "completed" => "Tool completed", _ => "Tool failed; inspect authoritative run outcome" },"risk":crate::tool_registry::find(tool).map_or(0, |descriptor| descriptor.event_risk()),"resources":[],"tool_call_id":format!("{rid}-step-{step}"),"state":phase,"server_generation":self.generation,"event_sequence":session.seq})
        };
        self.agent_live_activity = Some((
            rid.to_owned(),
            if name == "agent.activity" {
                json!({"activity":phase,"step":step})
            } else {
                json!({"activity":payload["detail"],"step":step,"tool":tool})
            },
        ));
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
        let descriptor = crate::tool_registry::find(tool).ok_or("unknown-tool")?;
        if !force_approval && !state.approval_required && !descriptor.approval_required {
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
                let state = state.lock().map_err(|_| "storage-failure")?;
                state.current_workspace()?;
                if cancel.load(Ordering::SeqCst) {
                    return Err("cancelled");
                }
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

pub fn voice_worker(state: Arc<Mutex<State>>, job: VoiceJob) {
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        crate::speech_runner::transcribe(&job.inspection, &job.profile, &job.pcm, &job.cancel)
    }));
    if let Ok(mut state) = state.lock() {
        if state
            .voice_worker_token
            .as_ref()
            .is_some_and(|token| Arc::ptr_eq(token, &job.cancel))
        {
            state.voice_worker_token = None;
        }
        if let Some(voice) = state.voice.as_mut()
            && Arc::ptr_eq(&voice.cancel, &job.cancel)
            && voice.phase == "Transcribing"
            && !job.cancel.load(Ordering::SeqCst)
        {
            match result {
                Ok(Ok(text)) if !text.is_empty() && text.len() <= 32_768 => {
                    voice.transcript = text;
                    voice.phase = "Completed";
                }
                _ => voice.phase = "Failed",
            }
        }
    }
}

fn agent_worker(state: Arc<Mutex<State>>, job: Job) {
    let Some((workspace, goal)) = job.agent else {
        return;
    };
    let mutation_authorized = AtomicBool::new(false);
    let current_tool = Mutex::new(None::<(usize, String)>);
    if !job.cancel.load(Ordering::SeqCst)
        && let Ok(mut locked) = state.lock()
    {
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
            if job.cancel.load(Ordering::SeqCst) {
                return Err("cancelled");
            }
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
                    if !completed {
                        let activity = match observation["_event"].as_str() {
                            Some("classifying") => Some("Classifying the inspection request"),
                            Some("deciding") => Some("Waiting for a model decision"),
                            _ => None,
                        };
                        if let Some(activity) = activity {
                            locked.agent_progress(
                                &job.session_id,
                                &job.run_id,
                                "agent.activity",
                                0,
                                "",
                                activity,
                            );
                            return true;
                        }
                    }
                    if observation["_event"] == "plan-step" && !completed {
                        let status = observation["status"].as_str().unwrap_or("");
                        let result = locked.agent_store.plan_step(
                            &job.run_id,
                            step,
                            status,
                            observation["reason"].as_str().unwrap_or(""),
                        );
                        if result.is_err() {
                            locked.storage_failed = true;
                        } else {
                            let activity = match status {
                                "started" => "Plan step started",
                                "succeeded" => "Plan step succeeded",
                                "failed" => "Plan step failed",
                                "blocked" => "Plan step blocked by a dependency",
                                _ => unreachable!("plan_step accepts only defined states"),
                            };
                            locked.agent_progress(
                                &job.session_id,
                                &job.run_id,
                                "agent.activity",
                                step,
                                "",
                                activity,
                            );
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
                        crate::agent::classified_inspect_goal(&goal).is_some()
                            || crate::agent::is_delegated_read(&goal),
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
                    if result.is_ok()
                        && crate::tool_registry::find(tool)
                            .is_some_and(|descriptor| descriptor.access == "write")
                    {
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
    let timed_out = matches!(result, Ok(Err("plan-timeout" | "agent-timeout")));
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
            .store
            .finish_agent(
                &locked.profile,
                &job.session_id,
                &job.run_id,
                &job.config.model,
                store::AgentTerminal {
                    state: terminal,
                    output: &text,
                    reason,
                },
            )
            .is_ok();
    if !committed {
        // An uncertain terminal commit must be reconciled from durable state
        // at restart. Rewriting only the Chat store here could destroy a receipt.
        locked.storage_failed = true;
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
