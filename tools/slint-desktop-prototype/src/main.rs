#[cfg(target_os = "macos")]
mod macos_shortcut;
#[cfg(target_os = "macos")]
mod macos_tray;

use sentinel_ipc::{Client, Envelope, Error, contract::RequestPayload};
use serde_json::Value;
use slint::ComponentHandle;
use std::{
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        mpsc::{Receiver, RecvTimeoutError, SyncSender, TryRecvError, sync_channel},
    },
    time::{Duration, Instant},
};

slint::include_modules!();

// Check Stop while reading active output. With no run, the worker sleeps on the command channel.
const ACTIVE_READ_TIMEOUT: Duration = Duration::from_millis(40);
const REQUEST_TIMEOUT: Duration = Duration::from_secs(10);
const DISPLAY_INTERVAL: Duration = Duration::from_millis(33);
const RESPONSE_LIMIT: usize = 64 * 1024;

enum Command {
    Navigate(String),
    SetTheme(i32),
    SaveResponseProfile(String),
    RefreshProviderDiagnostics,
    RefreshSettingsStatus,
    SetDevWorkspaceAccess(bool),
    ExportDevBackup(i32, String),
    RefreshMemory,
    SelectMemory(usize),
    NewMemory,
    SaveMemory,
    DeleteMemory,
    LoadMoreMemory,
    RefreshAgentHistory,
    LoadAgentRun(usize),
    LoadOlderAgentRuns,
    RefreshWorkspaceFiles,
    RefreshAttachmentFiles,
    RefreshWorkspaceChanges,
    RefreshExtensions,
    SelectWorkspace(usize),
    SelectAttachmentCandidate(usize),
    AddAttachment,
    SelectAttachment(usize),
    RemoveAttachment,
    ClearAttachments,
    RenameSession(String),
    SetSessionFlag { id: String, action: String },
    DeleteSession(String),
    EditLatest(String),
    RefreshOllamaHelper,
    PullOllamaModel(String),
    CancelOllamaPull,
    RemoveOllamaModel { model: String, confirmation: String },
    RegenerateLatest,
    RetryLatest,
    Reconnect,
    Refresh,
    Select(usize),
    RefreshModels,
    SelectModel(usize),
    SelectProvider(usize),
    SaveProviderEndpoint { provider: String, endpoint: String },
    SetAgentMode(bool),
    SetAgentDirectoryMode(bool),
    SetAgentCreateMode(bool),
    SetAgentIncludeHidden(bool),
    SetAgentProcessMode(bool),
    SetAgentProcessProgram(i32),
    SetAgentClassifyMode(bool),
    SetAgentSearchMode(bool),
    SetAgentEditMode(bool),
    SetAgentGlobMode(bool),
    SetAgentReadPlanMode(bool),
    SetAgentWorkPlanMode(bool),
    RespondApproval(bool),
    New,
    Send { text: String, agent: bool },
    Stop,
}

#[derive(Clone, Default)]
struct View {
    active_page: String,
    endpoint: String,
    desktop_settings_capable: bool,
    dev_backup_capable: bool,
    dev_backup_set_capable: bool,
    dev_backup_status: String,
    theme_name: String,
    theme_status: String,
    response_profile_instructions: String,
    response_profile_status: String,
    provider_diagnostics: String,
    provider_diagnostics_status: String,
    provider_diagnostics_capable: bool,
    provider_endpoints_capable: bool,
    provider_endpoint: String,
    provider_endpoint_provider: String,
    provider_endpoint_status: String,
    runtime_projection_capable: bool,
    runtime_projection_status: String,
    runtime_projection_view: String,
    workspace_runtime_capable: bool,
    workspace_runtime_status: String,
    workspace_runtime_view: String,
    workspace_revocation_capable: bool,
    workspace_access_known: bool,
    workspace_access_enabled: bool,
    workspace_access_id: String,
    workspace_access_status: String,
    memory_capable: bool,
    chat_revisions_capable: bool,
    session_management_capable: bool,
    session_flags_capable: bool,
    session_flags: std::collections::HashMap<String, (bool, bool)>,
    session_delete_confirmation: String,
    ollama_helper_capable: bool,
    ollama_helper_advertised: bool,
    ollama_pulling: bool,
    ollama_active_model: String,
    ollama_progress: f64,
    ollama_helper_status: String,
    ollama_remove_confirmation: String,
    memory_records: Vec<(String, String, String)>,
    memory_id: String,
    memory_revision: String,
    memory_next_cursor: String,
    memory_request_counter: u64,
    memory_status: String,
    memory_key: String,
    memory_value: String,
    memory_delete_confirmation: String,
    status: String,
    response: String,
    history: String,
    live_user: String,
    show_overlay: bool,
    last_assistant: String,
    latest_user_id: i64,
    latest_retry_id: i64,
    last_message_id: i64,
    baseline_message_id: i64,
    pending_message_id: i64,
    history_status: String,
    workspace_files_capable: bool,
    workspace_files_view: String,
    workspace_files_status: String,
    workspace_files_root: String,
    workspace_files_truncated: bool,
    attachment_files: Vec<String>,
    attachment_files_session_id: String,
    attachment_files_root: String,
    attachment_files_workspace_id: String,
    selected_attachment_candidate: usize,
    selected_attachment: usize,
    attachments: Vec<String>,
    attachment_status: String,
    attachment_formats: String,
    workspace_changes_capable: bool,
    workspace_changes_view: String,
    workspace_changes_status: String,
    workspace_changes_root: String,
    workspace_changes_truncated: bool,
    workspace_selection_capable: bool,
    workspace_options: Vec<(String, String, String)>,
    archived_workspace_ids: std::collections::HashSet<String>,
    selected_workspace_id: String,
    workspace_selection_status: String,
    extension_catalog_capable: bool,
    extension_catalog_status: String,
    extension_catalog_view: String,
    extension_catalog_truncated: bool,
    sessions: Vec<(String, String)>,
    selected_id: String,
    models: Vec<(String, String, String)>,
    selected_provider: String,
    selected_model: String,
    local_provider_catalog: bool,
    cloud_chat: bool,
    cloud_history: bool,
    cloud_documents: bool,
    local_providers: Vec<String>,
    model_status: String,
    agent_capable: bool,
    agent_restricted: bool,
    agent_multi_file: bool,
    agent_directory_capable: bool,
    agent_create_capable: bool,
    agent_process_capable: bool,
    agent_classify_capable: bool,
    agent_search_capable: bool,
    agent_search_mode: bool,
    agent_search_path: String,
    agent_search_pattern: String,
    agent_search_evidence: String,
    agent_edit_capable: bool,
    agent_edit_mode: bool,
    agent_edit_path: String,
    agent_edit_expected: String,
    agent_edit_proposed: String,
    agent_edit_evidence: String,
    agent_glob_capable: bool,
    agent_glob_mode: bool,
    agent_glob_path: String,
    agent_glob_pattern: String,
    agent_glob_evidence: String,
    agent_read_plan_capable: bool,
    agent_read_plan_mode: bool,
    agent_read_plan_steps: String,
    agent_read_plan_evidence: String,
    agent_read_plan_expected_steps: usize,
    agent_work_plan_capable: bool,
    agent_work_plan_mode: bool,
    agent_work_plan_steps: String,
    agent_work_plan_evidence: String,
    agent_classify_mode: bool,
    agent_process_mode: bool,
    agent_process_program: String,
    agent_history_capable: bool,
    agent_runs: Vec<(String, String, String)>,
    agent_selected_run: String,
    agent_history_view: String,
    agent_plan_lifecycle: String,
    agent_history_status: String,
    agent_runs_before_time: String,
    agent_runs_before_id: String,
    agent_runs_has_more: bool,
    agent_directory_mode: bool,
    agent_create_mode: bool,
    agent_include_hidden: bool,
    approval_capable: bool,
    agent_mode: bool,
    agent_label: String,
    agent_status: String,
    agent_progress: String,
    composer_placeholder: String,
    agent_content: String,
    agent_activity: String,
    approval_id: String,
    approval_detail: String,
    approval_pending: bool,
    connected: bool,
    running: bool,
    stopping: bool,
    truncated: bool,
}

#[derive(Clone, Default)]
struct Session {
    id: String,
    run: String,
    generation: Option<String>,
    sequence: Option<u64>,
}

impl Session {
    fn accepts(&mut self, event: &Envelope) -> bool {
        let payload = &event.payload;
        if payload["session_id"].as_str() != Some(&self.id)
            || payload["run_id"].as_str() != Some(&self.run)
            || self.run.is_empty()
        {
            return false;
        }
        let generation = payload["server_generation"].as_str();
        let sequence = payload["event_sequence"].as_u64();
        if self
            .generation
            .as_deref()
            .is_some_and(|expected| Some(expected) != generation)
            || self
                .sequence
                .is_some_and(|seen| sequence.is_none_or(|next| next <= seen))
        {
            return false;
        }
        self.generation = generation.map(str::to_owned);
        self.sequence = sequence;
        true
    }
}

struct PanelUpdates {
    panel: slint::Weak<Panel>,
    pending: Arc<Mutex<Option<(View, bool)>>>,
}

impl PanelUpdates {
    fn publish(&self, view: &View, clear_draft: bool) {
        let mut pending = self.pending.lock().unwrap();
        let already_scheduled = pending.is_some();
        let clear_draft = clear_draft || pending.as_ref().is_some_and(|(_, clear)| *clear);
        *pending = Some((view.clone(), clear_draft));
        drop(pending);
        if !already_scheduled {
            let pending = Arc::clone(&self.pending);
            let _ = self.panel.upgrade_in_event_loop(move |panel| {
                let update = pending.lock().unwrap().take();
                if let Some((view, clear_draft)) = update {
                    panel.set_transcript(transcript(&view).into());
                    panel.set_status(view.status.into());
                    panel.set_active_page(view.active_page.into());
                    panel.set_endpoint(view.endpoint.into());
                    panel.set_desktop_settings_capable(view.desktop_settings_capable);
                    panel.set_dev_backup_capable(view.dev_backup_capable);
                    panel.set_dev_backup_set_capable(view.dev_backup_set_capable);
                    panel.set_dev_backup_status(view.dev_backup_status.into());
                    panel.set_theme_name(view.theme_name.into());
                    panel.set_theme_status(view.theme_status.into());
                    panel.set_response_profile_instructions(
                        view.response_profile_instructions.clone().into(),
                    );
                    panel.set_response_profile_status(view.response_profile_status.clone().into());
                    panel.set_provider_diagnostics(view.provider_diagnostics.clone().into());
                    panel.set_provider_diagnostics_status(
                        view.provider_diagnostics_status.clone().into(),
                    );
                    panel.set_provider_diagnostics_capable(view.provider_diagnostics_capable);
                    panel.set_runtime_projection_capable(view.runtime_projection_capable);
                    panel.set_runtime_projection_status(
                        view.runtime_projection_status.clone().into(),
                    );
                    panel.set_runtime_projection_view(view.runtime_projection_view.clone().into());
                    panel.set_workspace_runtime_capable(view.workspace_runtime_capable);
                    panel
                        .set_workspace_runtime_status(view.workspace_runtime_status.clone().into());
                    panel.set_workspace_runtime_view(view.workspace_runtime_view.clone().into());
                    panel.set_workspace_revocation_capable(view.workspace_revocation_capable);
                    panel.set_workspace_access_known(view.workspace_access_known);
                    panel.set_workspace_access_enabled(view.workspace_access_enabled);
                    panel.set_workspace_access_status(view.workspace_access_status.clone().into());
                    panel.set_memory_capable(view.memory_capable);
                    panel.set_chat_revisions_capable(view.chat_revisions_capable);
                    panel.set_can_regenerate_latest(view.latest_user_id > 0);
                    panel.set_can_retry_latest(view.latest_retry_id > 0);
                    panel.set_memory_status(view.memory_status.into());
                    panel.set_memory_item_selected(!view.memory_id.is_empty());
                    panel.set_memory_next_page(!view.memory_next_cursor.is_empty());
                    panel.set_memory_key(view.memory_key.into());
                    panel.set_memory_value(view.memory_value.into());
                    panel.set_memory_delete_confirmation(view.memory_delete_confirmation.into());
                    panel.set_memory_entries(slint::ModelRc::new(slint::VecModel::from(
                        view.memory_records
                            .iter()
                            .map(|(_, _, label)| slint::SharedString::from(label.clone()))
                            .collect::<Vec<_>>(),
                    )));
                    panel.set_selected_memory(
                        view.memory_records
                            .iter()
                            .position(|(id, _, _)| id == &view.memory_id)
                            .map_or(-1, |index| index as i32),
                    );
                    panel.set_history_status(view.history_status.into());
                    panel.set_workspace_files_capable(view.workspace_files_capable);
                    panel.set_workspace_files_view(view.workspace_files_view.clone().into());
                    panel.set_workspace_files_status(view.workspace_files_status.clone().into());
                    panel.set_workspace_files_root(view.workspace_files_root.clone().into());
                    panel.set_workspace_files_truncated(view.workspace_files_truncated);
                    panel.set_attachment_files(slint::ModelRc::new(slint::VecModel::from(
                        view.attachment_files
                            .iter()
                            .map(|path| slint::SharedString::from(quoted_path(path)))
                            .collect::<Vec<_>>(),
                    )));
                    panel.set_selected_attachment_candidate(if view.attachment_files.is_empty() {
                        -1
                    } else {
                        view.selected_attachment_candidate as i32
                    });
                    panel.set_attachments(slint::ModelRc::new(slint::VecModel::from(
                        view.attachments
                            .iter()
                            .map(|path| slint::SharedString::from(quoted_path(path)))
                            .collect::<Vec<_>>(),
                    )));
                    panel.set_selected_attachment(if view.attachments.is_empty() {
                        -1
                    } else {
                        view.selected_attachment as i32
                    });
                    panel.set_attachment_status(view.attachment_status.clone().into());
                    panel.set_attachment_formats(view.attachment_formats.clone().into());
                    panel.set_workspace_changes_capable(view.workspace_changes_capable);
                    panel.set_workspace_changes_view(view.workspace_changes_view.clone().into());
                    panel
                        .set_workspace_changes_status(view.workspace_changes_status.clone().into());
                    panel.set_workspace_changes_root(view.workspace_changes_root.clone().into());
                    panel.set_workspace_changes_truncated(view.workspace_changes_truncated);
                    panel.set_workspace_selection_capable(view.workspace_selection_capable);
                    panel.set_workspace_selection_status(
                        view.workspace_selection_status.clone().into(),
                    );
                    panel.set_extension_catalog_capable(view.extension_catalog_capable);
                    panel
                        .set_extension_catalog_status(view.extension_catalog_status.clone().into());
                    panel.set_extension_catalog_view(view.extension_catalog_view.clone().into());
                    panel.set_extension_catalog_truncated(view.extension_catalog_truncated);
                    panel.set_workspaces(slint::ModelRc::new(slint::VecModel::from(
                        view.workspace_options
                            .iter()
                            .map(|(_, label, _)| slint::SharedString::from(label.clone()))
                            .collect::<Vec<_>>(),
                    )));
                    panel.set_selected_workspace(
                        view.workspace_options
                            .iter()
                            .position(|(id, _, _)| id == &view.selected_workspace_id)
                            .map_or(-1, |index| index as i32),
                    );
                    panel.set_agent_capable(view.agent_capable);
                    panel.set_agent_restricted(view.agent_restricted);
                    panel.set_agent_multi_file(view.agent_multi_file);
                    panel.set_agent_directory_capable(view.agent_directory_capable);
                    panel.set_agent_create_capable(view.agent_create_capable);
                    panel.set_agent_process_capable(view.agent_process_capable);
                    panel.set_agent_classify_capable(view.agent_classify_capable);
                    panel.set_agent_classify_mode(view.agent_classify_mode);
                    panel.set_agent_search_capable(view.agent_search_capable);
                    panel.set_agent_search_mode(view.agent_search_mode);
                    panel.set_agent_search_path(view.agent_search_path.clone().into());
                    panel.set_agent_search_pattern(view.agent_search_pattern.clone().into());
                    panel.set_agent_search_evidence(view.agent_search_evidence.clone().into());
                    panel.set_agent_edit_capable(view.agent_edit_capable);
                    panel.set_agent_edit_mode(view.agent_edit_mode);
                    panel.set_agent_edit_path(view.agent_edit_path.clone().into());
                    panel.set_agent_edit_expected(view.agent_edit_expected.clone().into());
                    panel.set_agent_edit_proposed(view.agent_edit_proposed.clone().into());
                    panel.set_agent_edit_evidence(view.agent_edit_evidence.clone().into());
                    panel.set_agent_glob_capable(view.agent_glob_capable);
                    panel.set_agent_glob_mode(view.agent_glob_mode);
                    panel.set_agent_glob_path(view.agent_glob_path.clone().into());
                    panel.set_agent_glob_pattern(view.agent_glob_pattern.clone().into());
                    panel.set_agent_glob_evidence(view.agent_glob_evidence.clone().into());
                    panel.set_agent_read_plan_capable(view.agent_read_plan_capable);
                    panel.set_agent_read_plan_mode(view.agent_read_plan_mode);
                    panel.set_agent_read_plan_steps(view.agent_read_plan_steps.clone().into());
                    panel
                        .set_agent_read_plan_evidence(view.agent_read_plan_evidence.clone().into());
                    panel.set_agent_work_plan_capable(view.agent_work_plan_capable);
                    panel.set_agent_work_plan_mode(view.agent_work_plan_mode);
                    panel.set_agent_work_plan_steps(view.agent_work_plan_steps.clone().into());
                    panel
                        .set_agent_work_plan_evidence(view.agent_work_plan_evidence.clone().into());
                    panel.set_agent_process_mode(view.agent_process_mode);
                    panel.set_agent_process_program(view.agent_process_program.clone().into());
                    panel.set_agent_history_capable(view.agent_history_capable);
                    panel.set_agent_history_status(view.agent_history_status.clone().into());
                    panel.set_agent_history_view(view.agent_history_view.clone().into());
                    panel.set_agent_plan_lifecycle(view.agent_plan_lifecycle.clone().into());
                    panel.set_agent_runs(slint::ModelRc::new(slint::VecModel::from(
                        view.agent_runs
                            .iter()
                            .map(|(_, label, _)| slint::SharedString::from(label.clone()))
                            .collect::<Vec<_>>(),
                    )));
                    panel.set_selected_agent_run(
                        view.agent_runs
                            .iter()
                            .position(|(id, _, _)| id == &view.agent_selected_run)
                            .map_or(-1, |index| index as i32),
                    );
                    panel.set_agent_runs_has_more(view.agent_runs_has_more);
                    panel.set_agent_directory_mode(view.agent_directory_mode);
                    panel.set_agent_create_mode(view.agent_create_mode);
                    panel.set_agent_include_hidden(view.agent_include_hidden);
                    panel.set_agent_content(view.agent_content.clone().into());
                    panel.set_agent_mode(view.agent_mode);
                    panel.set_agent_label(view.agent_label.into());
                    panel.set_agent_status(view.agent_status.into());
                    panel.set_agent_progress(view.agent_progress.clone().into());
                    panel.set_composer_placeholder(view.composer_placeholder.into());
                    panel.set_agent_activity(view.agent_activity.into());
                    panel.set_approval_detail(view.approval_detail.into());
                    panel.set_approval_pending(view.approval_pending);
                    panel.set_approval_capable(view.approval_capable);
                    panel.set_model_status(view.model_status.into());
                    panel.set_provider_endpoint_provider(view.provider_endpoint_provider.into());
                    panel.set_saved_provider_endpoint(view.provider_endpoint.into());
                    panel.set_provider_endpoint_status(view.provider_endpoint_status.into());
                    panel.set_ollama_helper_capable(view.ollama_helper_capable);
                    panel.set_ollama_pulling(view.ollama_pulling);
                    panel.set_ollama_active_model(view.ollama_active_model.clone().into());
                    panel.set_ollama_progress_text(
                        format!("{:.0}%", view.ollama_progress * 100.0).into(),
                    );
                    panel.set_ollama_helper_status(view.ollama_helper_status.clone().into());
                    panel.set_ollama_remove_confirmation(
                        view.ollama_remove_confirmation.clone().into(),
                    );
                    let selected = view
                        .sessions
                        .iter()
                        .position(|(id, _)| *id == view.selected_id)
                        .map_or(-1, |i| i as i32);
                    panel.set_sessions(slint::ModelRc::new(slint::VecModel::from(
                        view.sessions
                            .iter()
                            .map(|(id, title)| {
                                let (archived, pinned) =
                                    view.session_flags.get(id).copied().unwrap_or_default();
                                let archive_label = if archived { "Archived · " } else { "" };
                                let pin_label = if pinned { "Pinned · " } else { "" };
                                slint::SharedString::from(format!(
                                    "{archive_label}{pin_label}{title}"
                                ))
                            })
                            .collect::<Vec<_>>(),
                    )));
                    let (archived, pinned) = view
                        .session_flags
                        .get(&view.selected_id)
                        .copied()
                        .unwrap_or_default();
                    panel.set_selected_session(selected);
                    panel.set_selected_session_id(view.selected_id.clone().into());
                    panel.set_selected_session_archived(archived);
                    panel.set_selected_session_pinned(pinned);
                    panel.set_session_flags_capable(view.session_flags_capable);
                    panel.set_session_management_capable(view.session_management_capable);
                    panel.set_session_title(
                        view.sessions
                            .iter()
                            .find(|(id, _)| id == &view.selected_id)
                            .map(|(_, title)| title.clone())
                            .unwrap_or_default()
                            .into(),
                    );
                    panel.set_session_delete_confirmation(
                        view.session_delete_confirmation.clone().into(),
                    );
                    let selected_model = view
                        .models
                        .iter()
                        .position(|(provider, model, _)| {
                            provider == &view.selected_provider && model == &view.selected_model
                        })
                        .map_or(-1, |i| i as i32);
                    panel.set_models(slint::ModelRc::new(slint::VecModel::from(
                        view.models
                            .into_iter()
                            .map(|(_, _, label)| slint::SharedString::from(label))
                            .collect::<Vec<_>>(),
                    )));
                    panel.set_selected_model(selected_model);
                    let selected_local_provider = view
                        .local_providers
                        .iter()
                        .position(|provider| provider == &view.selected_provider)
                        .map_or(-1, |index| index as i32);
                    panel.set_local_providers(slint::ModelRc::new(slint::VecModel::from(
                        view.local_providers
                            .into_iter()
                            .map(slint::SharedString::from)
                            .collect::<Vec<_>>(),
                    )));
                    panel.set_selected_local_provider(selected_local_provider);
                    let cloud_provider = match view.selected_provider.as_str() {
                        "openai" => "OpenAI",
                        "groq" => "Groq",
                        "mistral" => "Mistral",
                        "claude" => "Claude",
                        "gemini" => "Gemini",
                        _ => "Cloud provider",
                    };
                    panel.set_cloud_provider_name(cloud_provider.into());
                    panel.set_cloud_chat(view.connected && view.cloud_chat);
                    panel.set_cloud_history(view.cloud_history);
                    panel.set_cloud_documents(view.cloud_documents);
                    panel.set_connected(view.connected);
                    panel.set_running(view.running);
                    panel.set_waiting(view.stopping);
                    if view.connected && std::env::var_os("SENTINEL_BENCHMARK_READY").is_some() {
                        eprintln!("PANEL_OBSERVATION_READY");
                    }
                    if clear_draft {
                        panel.set_draft("".into());
                        panel.set_agent_paths("".into());
                        panel.set_agent_content("".into());
                        panel.invoke_focus_composer();
                    }
                }
            });
        }
    }
}

fn request(client: &mut Client, command: RequestPayload) -> Result<Value, Error> {
    let envelope = serde_json::to_value(command).map_err(|e| Error::Protocol(e.to_string()))?;
    client.set_read_timeout(Some(REQUEST_TIMEOUT))?;
    let response = client.request(
        envelope["name"]
            .as_str()
            .ok_or_else(|| Error::Protocol("missing command name".into()))?,
        envelope["payload"].clone(),
    );
    let restore = client.set_read_timeout(Some(ACTIVE_READ_TIMEOUT));
    let response = response?;
    restore?;
    Ok(response)
}

fn refresh(client: &mut Client, view: &mut View) -> Result<(), Error> {
    let data = request(client, RequestPayload::SessionList {})?;
    let rows = data["sessions"]
        .as_array()
        .ok_or_else(|| Error::Protocol("invalid session list".into()))?;
    view.session_flags = rows
        .iter()
        .take(128)
        .filter_map(|row| {
            let id = row["session_id"].as_str()?;
            Some((
                id.to_owned(),
                (row["archived"] == true, row["pinned"] == true),
            ))
        })
        .collect();
    view.sessions = rows
        .iter()
        .take(128)
        .filter_map(|row| {
            let id = row["session_id"].as_str()?.to_owned();
            let title = row["title"]
                .as_str()
                .filter(|s| !s.is_empty())
                .unwrap_or("Untitled session");
            Some((id, title.chars().take(120).collect()))
        })
        .collect();
    if view.selected_id.is_empty() {
        view.history_status = if view.sessions.is_empty() {
            "No saved sessions."
        } else if rows.len() > 128 {
            "Showing the first 128 sessions."
        } else {
            "Choose a saved session."
        }
        .into();
    }
    Ok(())
}

fn workspace_files_projection(data: &Value) -> Result<(String, String, bool, Vec<String>), Error> {
    let root = data["root"]
        .as_str()
        .filter(|root| !root.is_empty() && !root.contains('\0') && Path::new(root).is_absolute())
        .ok_or_else(|| Error::Protocol("invalid workspace root".into()))?;
    let files = data["files"]
        .as_array()
        .ok_or_else(|| Error::Protocol("invalid workspace file list".into()))?;
    let mut rows = Vec::new();
    let mut relative_files = Vec::new();
    let mut truncated = data["truncated"].as_bool().unwrap_or(false);
    for file in files.iter().take(100) {
        let path = file
            .as_str()
            .filter(|path| {
                !path.is_empty() && !path.contains('\0') && Path::new(path).is_absolute()
            })
            .ok_or_else(|| Error::Protocol("invalid workspace file path".into()))?;
        let relative = Path::new(path)
            .strip_prefix(Path::new(root))
            .map_err(|_| Error::Protocol("workspace file is outside the authorized root".into()))?;
        let components = relative.components().collect::<Vec<_>>();
        if components.is_empty()
            || components
                .iter()
                .any(|component| !matches!(component, std::path::Component::Normal(_)))
        {
            return Err(Error::Protocol(
                "invalid relative workspace file path".into(),
            ));
        }
        let display = relative
            .to_str()
            .ok_or_else(|| Error::Protocol("workspace file path is not UTF-8".into()))?;
        let quoted =
            serde_json::to_string(display).map_err(|error| Error::Protocol(error.to_string()))?;
        rows.push(format!("• {quoted}"));
        relative_files.push(display.to_owned());
    }
    if files.len() > 100 {
        truncated = true;
    }
    let quoted_root =
        serde_json::to_string(root).map_err(|error| Error::Protocol(error.to_string()))?;
    let heading = format!(
        "{} regular files in the authorized workspace snapshot. Paths are relative to {}.",
        rows.len(),
        quoted_root
    );
    let body = if rows.is_empty() {
        "No regular files were returned in this snapshot.".to_owned()
    } else {
        rows.join("\n")
    };
    let notice = if truncated {
        "\nSnapshot is truncated; omitted paths may exist."
    } else {
        ""
    };
    let display = format!("{heading}\n\n{body}{notice}");
    let visible = display.chars().take(16_000).collect::<String>();
    let display_was_truncated = visible.len() < display.len();
    let display = if display_was_truncated {
        format!("{visible}\n… display limit reached; refresh for a new snapshot.")
    } else {
        visible
    };
    truncated |= display_was_truncated;
    Ok((quoted_root, display, truncated, relative_files))
}

fn quoted_path(path: &str) -> String {
    serde_json::to_string(path).unwrap_or_else(|_| "\"invalid path\"".into())
}

fn clear_attachments(view: &mut View, message: &str) {
    view.attachment_files.clear();
    view.attachment_files_session_id.clear();
    view.attachment_files_root.clear();
    view.attachment_files_workspace_id.clear();
    view.attachments.clear();
    view.selected_attachment = 0;
    view.selected_attachment_candidate = 0;
    view.attachment_status = message.into();
}

fn add_attachment(view: &mut View) -> Result<(), Error> {
    let Some(path) = view
        .attachment_files
        .get(view.selected_attachment_candidate)
        .cloned()
    else {
        view.attachment_status = "Select a file from the authorized snapshot first.".into();
        return Err(Error::Usage("No authorized file is selected.".into()));
    };
    if view.attachments.contains(&path) {
        view.attachment_status = "That file is already attached.".into();
        return Ok(());
    }
    if view.attachments.len() >= 4 {
        view.attachment_status = "At most 4 files can be attached.".into();
        return Err(Error::Usage("At most 4 files can be attached.".into()));
    }
    view.attachments.push(path);
    view.selected_attachment = view.attachments.len() - 1;
    view.attachment_status = format!(
        "{} file(s) selected. The daemon validates text and size limits before sending.",
        view.attachments.len()
    );
    Ok(())
}

fn remove_attachment(view: &mut View) -> Result<(), Error> {
    if view.selected_attachment >= view.attachments.len() {
        return Err(Error::Usage("Select an attachment to remove.".into()));
    }
    view.attachments.remove(view.selected_attachment);
    view.selected_attachment = view
        .selected_attachment
        .min(view.attachments.len().saturating_sub(1));
    view.attachment_status = if view.attachments.is_empty() {
        "No files selected.".into()
    } else {
        format!("{} file(s) selected.", view.attachments.len())
    };
    Ok(())
}

fn refresh_workspace_files(
    client: &mut Client,
    session: &Session,
    view: &mut View,
) -> Result<(), Error> {
    view.workspace_files_view.clear();
    view.workspace_files_root.clear();
    view.workspace_files_truncated = false;
    if !view.workspace_files_capable {
        view.workspace_files_status = "Workspace browsing is not advertised by this daemon.".into();
        return Err(Error::Remote("unsupported-capability".into()));
    }
    if session.id.is_empty() {
        clear_attachments(
            view,
            "Create or select a Chat session before attaching files.",
        );
        view.workspace_files_status =
            "Create or select a Chat session before listing workspace files.".into();
        return Ok(());
    }
    view.workspace_files_status = "Loading authorized workspace snapshot…".into();
    clear_attachments(view, "Loading the authorized file list…");
    let identity = request(client, RequestPayload::TerminalState {}).and_then(|state| {
        let current = &state["workspace"];
        let id = current["id"]
            .as_str()
            .filter(|id| !id.is_empty())
            .ok_or_else(|| Error::Protocol("workspace identity unavailable".into()))?;
        let root = current["root"]
            .as_str()
            .filter(|root| !root.is_empty() && Path::new(root).is_absolute())
            .ok_or_else(|| Error::Protocol("workspace root unavailable".into()))?;
        if view.workspace_selection_capable
            && !view.selected_workspace_id.is_empty()
            && id != view.selected_workspace_id
        {
            return Err(Error::Protocol(
                "workspace selection changed; refresh it before attaching files".into(),
            ));
        }
        Ok((id.to_owned(), root.to_owned()))
    });
    let (workspace_id, expected_root) = match identity {
        Ok(identity) => identity,
        Err(error) => {
            view.workspace_files_status = format!("Workspace identity unavailable: {error}");
            view.attachment_status =
                "Cannot attach: confirm the configured workspace first.".into();
            return Err(error);
        }
    };
    let result = request(
        client,
        RequestPayload::WorkspaceFiles {
            session_id: session.id.clone(),
        },
    )
    .and_then(|data| {
        let (quoted_root, display, truncated, files) = workspace_files_projection(&data)?;
        let actual_root: String = serde_json::from_str(&quoted_root)
            .map_err(|error| Error::Protocol(error.to_string()))?;
        if actual_root != expected_root {
            return Err(Error::Protocol(
                "workspace changed while loading files; refresh before attaching".into(),
            ));
        }
        Ok((quoted_root, display, truncated, files))
    });
    match result {
        Ok((root, display, truncated, files)) => {
            view.workspace_files_root = root;
            view.workspace_files_view = display;
            view.workspace_files_truncated = truncated;
            view.attachment_files = files;
            view.attachment_files_session_id = session.id.clone();
            view.attachment_files_root = expected_root;
            view.attachment_files_workspace_id = workspace_id;
            view.attachments
                .retain(|path| view.attachment_files.contains(path));
            if view.selected_attachment_candidate >= view.attachment_files.len() {
                view.selected_attachment_candidate = 0;
            }
            if view.selected_attachment >= view.attachments.len() {
                view.selected_attachment = 0;
            }
            view.workspace_files_status = if truncated {
                "Snapshot loaded; results are truncated.".into()
            } else {
                "Snapshot loaded.".into()
            };
            view.attachment_status = if session.id.is_empty() {
                "Select or create a Chat session before attaching files.".into()
            } else if view.attachment_files.is_empty() {
                "No regular workspace files are available to attach.".into()
            } else if view.workspace_files_truncated {
                "Choose from the visible authorized files; the snapshot is truncated.".into()
            } else {
                format!(
                    "{} authorized workspace files available. Text-only attachments: up to 4 files, 8 KiB each, 32 KiB total.",
                    view.attachment_files.len()
                )
            };
            Ok(())
        }
        Err(error) => {
            view.workspace_files_status = format!("Workspace snapshot unavailable: {error}");
            view.attachment_files.clear();
            view.attachments.clear();
            view.attachment_status =
                format!("Cannot attach: authorized workspace file list unavailable ({error}).");
            Err(error)
        }
    }
}

fn workspace_changes_projection(data: &Value) -> Result<(String, String, String, bool), Error> {
    let available = data["available"]
        .as_bool()
        .ok_or_else(|| Error::Protocol("invalid workspace comparison availability".into()))?;
    let mut truncated = data["truncated"]
        .as_bool()
        .ok_or_else(|| Error::Protocol("invalid workspace comparison truncation state".into()))?;
    let files = data["files"]
        .as_array()
        .ok_or_else(|| Error::Protocol("invalid workspace comparison entries".into()))?;
    let reason = data["reason"].as_str().filter(|reason| !reason.is_empty());
    if !available {
        if !files.is_empty() {
            return Err(Error::Protocol(
                "unavailable workspace comparison contained entries".into(),
            ));
        }
        let reason = reason
            .map(|reason| reason.chars().take(1024).collect::<String>())
            .map(|reason| serde_json::to_string(&reason))
            .transpose()
            .map_err(|error| Error::Protocol(error.to_string()))?
            .map(|reason| format!(" Reason: {reason}"))
            .unwrap_or_default();
        return Ok((
            String::new(),
            format!(
                "No trusted baseline is available. The workspace state is unknown; this does not mean it is clean.{reason}"
            ),
            "Comparison unavailable; workspace state is unknown.".into(),
            truncated,
        ));
    }
    let root = data["root"]
        .as_str()
        .filter(|root| !root.is_empty() && !root.contains('\0') && Path::new(root).is_absolute())
        .ok_or_else(|| Error::Protocol("invalid workspace comparison root".into()))?;
    let mut rows = Vec::new();
    let mut budget = 40_000usize;
    for file in files.iter().take(100) {
        let path = file["path"]
            .as_str()
            .filter(|path| {
                !path.is_empty() && !path.contains('\0') && Path::new(path).is_absolute()
            })
            .ok_or_else(|| Error::Protocol("invalid changed workspace path".into()))?;
        let relative = Path::new(path)
            .strip_prefix(Path::new(root))
            .map_err(|_| Error::Protocol("changed path is outside the workspace root".into()))?;
        if relative.as_os_str().is_empty()
            || relative.components().any(|component| {
                matches!(
                    component,
                    std::path::Component::ParentDir | std::path::Component::RootDir
                )
            })
        {
            return Err(Error::Protocol("invalid changed workspace path".into()));
        }
        if file["state"].as_str() != Some("Applied") {
            return Err(Error::Protocol("unknown workspace change state".into()));
        }
        let kind = file["kind"]
            .as_str()
            .filter(|kind| matches!(*kind, "created" | "modified"))
            .ok_or_else(|| Error::Protocol("unknown workspace change kind".into()))?;
        let diff = file["diff"]
            .as_str()
            .ok_or_else(|| Error::Protocol("missing workspace change diff".into()))?;
        let path = serde_json::to_string(&relative.to_string_lossy().as_ref())
            .map_err(|error| Error::Protocol(error.to_string()))?;
        let mut safe_diff = String::new();
        for character in diff.chars() {
            if safe_diff.len() >= 8_192 || safe_diff.len() >= budget {
                truncated = true;
                break;
            }
            if character == '\n' || character == '\t' {
                safe_diff.push(character);
            } else if character.is_control() {
                use std::fmt::Write as _;
                let _ = write!(safe_diff, "\\u{{{:x}}}", character as u32);
            } else {
                safe_diff.push(character);
            }
        }
        budget = budget.saturating_sub(safe_diff.len());
        rows.push(format!(
            "{} · {}\n{}\n{}",
            kind,
            path,
            "─".repeat(48),
            safe_diff
        ));
        if budget == 0 {
            truncated = true;
            break;
        }
    }
    if files.len() > 100 {
        truncated = true;
    }
    let quoted_root =
        serde_json::to_string(root).map_err(|error| Error::Protocol(error.to_string()))?;
    let heading = format!(
        "Changes from the last accepted Agent run, relative to {quoted_root}. This report includes only supported created/modified entries; deletions are not inferred."
    );
    let body = if rows.is_empty() {
        "No supported created/modified entries were returned. This does not prove the workspace has no other changes.".to_owned()
    } else {
        rows.join("\n\n")
    };
    let notice = if truncated {
        "\n\nComparison is truncated; omitted changes may exist."
    } else {
        ""
    };
    let display = format!("{heading}\n\n{body}{notice}");
    let visible = display.chars().take(48_000).collect::<String>();
    let display_was_truncated = visible.len() < display.len();
    let display = if display_was_truncated {
        format!("{visible}\n\nDisplay limit reached; refresh to request a new comparison.")
    } else {
        visible
    };
    truncated |= display_was_truncated;
    Ok((
        quoted_root,
        display,
        if truncated {
            "Comparison loaded; results are truncated.".into()
        } else {
            "Comparison loaded.".into()
        },
        truncated,
    ))
}

fn refresh_workspace_changes(
    client: &mut Client,
    session: &Session,
    view: &mut View,
) -> Result<(), Error> {
    view.workspace_changes_view.clear();
    view.workspace_changes_root.clear();
    view.workspace_changes_truncated = false;
    if !view.workspace_changes_capable {
        view.workspace_changes_status =
            "Workspace comparison is not advertised by this daemon.".into();
        return Err(Error::Remote("unsupported-capability".into()));
    }
    if session.id.is_empty() {
        view.workspace_changes_view = "No trusted baseline is available. The workspace state is unknown; this does not mean it is clean.".into();
        view.workspace_changes_status =
            "Create or select a Chat session before checking workspace changes.".into();
        return Ok(());
    }
    view.workspace_changes_status = "Loading accepted-run workspace comparison…".into();
    let result = request(
        client,
        RequestPayload::WorkspaceChanges {
            session_id: session.id.clone(),
        },
    )
    .and_then(|data| workspace_changes_projection(&data));
    match result {
        Ok((root, display, status, truncated)) => {
            view.workspace_changes_root = root;
            view.workspace_changes_view = display;
            view.workspace_changes_status = status;
            view.workspace_changes_truncated = truncated;
            Ok(())
        }
        Err(error) => {
            view.workspace_changes_status = format!("Workspace comparison unavailable: {error}");
            view.workspace_changes_view.clear();
            view.workspace_changes_root.clear();
            Err(error)
        }
    }
}

fn refresh_workspace_state(client: &mut Client, view: &mut View) -> Result<(), Error> {
    view.workspace_options.clear();
    view.archived_workspace_ids.clear();
    view.selected_workspace_id.clear();
    view.workspace_selection_status = "Loading configured workspaces…".into();
    view.workspace_files_root.clear();
    view.workspace_files_view.clear();
    view.workspace_files_truncated = false;
    clear_attachments(
        view,
        "Workspace selection changed. Refresh files before attaching.",
    );
    view.workspace_files_status = if view.workspace_files_capable {
        "Workspace snapshot not loaded for the current selection.".into()
    } else {
        "Workspace browsing is not advertised by this daemon.".into()
    };
    view.workspace_changes_root.clear();
    view.workspace_changes_view.clear();
    view.workspace_changes_truncated = false;
    view.workspace_changes_status = if view.workspace_changes_capable {
        "Comparison not loaded; workspace state is unknown.".into()
    } else {
        "Workspace comparison is not advertised by this daemon.".into()
    };
    if !view.workspace_selection_capable {
        view.workspace_selection_status =
            "Workspace selection is not advertised by this daemon.".into();
        return Err(Error::Remote("unsupported-capability".into()));
    }
    let data = match request(client, RequestPayload::TerminalState {}) {
        Ok(data) => data,
        Err(error) => {
            view.workspace_selection_status =
                format!("Workspace selection state unavailable: {error}");
            return Err(error);
        }
    };
    let current = &data["workspace"];
    let current_id = current["id"].as_str().unwrap_or_default();
    let current_root = current["root"]
        .as_str()
        .filter(|root| !root.is_empty() && !root.contains('\0') && Path::new(root).is_absolute());
    let rows = data["workspaces"]
        .as_array()
        .ok_or_else(|| Error::Protocol("invalid configured workspace list".into()))?;
    let mut seen = std::collections::HashSet::new();
    for row in rows.iter().take(128) {
        let id = row["id"]
            .as_str()
            .filter(|id| !id.is_empty() && id.len() <= 256 && !id.contains('\0'))
            .ok_or_else(|| Error::Protocol("invalid configured workspace ID".into()))?;
        let root = row["root"]
            .as_str()
            .filter(|root| {
                !root.is_empty() && !root.contains('\0') && Path::new(root).is_absolute()
            })
            .ok_or_else(|| Error::Protocol("invalid configured workspace root".into()))?;
        if !seen.insert(id.to_owned()) {
            return Err(Error::Protocol("duplicate configured workspace ID".into()));
        }
        let name = row["name"]
            .as_str()
            .filter(|name| !name.is_empty())
            .unwrap_or(id);
        let quoted_id =
            serde_json::to_string(id).map_err(|error| Error::Protocol(error.to_string()))?;
        let quoted_name =
            serde_json::to_string(name).map_err(|error| Error::Protocol(error.to_string()))?;
        let quoted_root =
            serde_json::to_string(root).map_err(|error| Error::Protocol(error.to_string()))?;
        let access = serde_json::to_string(row["access"].as_str().unwrap_or("unknown access"))
            .map_err(|error| Error::Protocol(error.to_string()))?;
        let permission =
            serde_json::to_string(row["permission"].as_str().unwrap_or("permission unknown"))
                .map_err(|error| Error::Protocol(error.to_string()))?;
        let archived = row["archived"] == true;
        if archived {
            view.archived_workspace_ids.insert(id.to_owned());
        }
        let label = format!(
            "{} ({}) · {} · access {} · permission {}{}",
            quoted_name,
            quoted_id,
            quoted_root,
            access,
            permission,
            if archived { " · archived" } else { "" }
        );
        view.workspace_options
            .push((id.to_owned(), label, root.to_owned()));
    }
    if rows.len() > 128 {
        view.workspace_selection_status = "Showing the first 128 configured workspaces.".into();
    }
    if !current_id.is_empty() {
        let (id, _, root) = view
            .workspace_options
            .iter()
            .find(|(id, _, _)| id == current_id)
            .ok_or_else(|| Error::Protocol("selected workspace is not configured".into()))?;
        if current_root != Some(root.as_str()) {
            return Err(Error::Protocol(
                "selected workspace root does not match its configured root".into(),
            ));
        }
        view.selected_workspace_id = id.clone();
        if rows.len() <= 128 {
            let name = current["name"].as_str().unwrap_or(id);
            let name =
                serde_json::to_string(name).map_err(|error| Error::Protocol(error.to_string()))?;
            view.workspace_selection_status = format!("Selected workspace: {name}.");
        }
    } else if rows.is_empty() {
        view.workspace_selection_status = "No configured workspace is available.".into();
    } else if rows.len() <= 128 {
        view.workspace_selection_status = "Choose a configured workspace.".into();
    }
    Ok(())
}

fn select_workspace(
    client: &mut Client,
    session: &Session,
    view: &mut View,
    index: usize,
) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    let (workspace_id, _, _) = view
        .workspace_options
        .get(index)
        .cloned()
        .ok_or_else(|| Error::Usage("Configured workspace is no longer available.".into()))?;
    if view.archived_workspace_ids.contains(&workspace_id) {
        view.workspace_selection_status = "Archived workspaces cannot be selected.".into();
        return Err(Error::Usage(
            "Archived workspaces cannot be selected.".into(),
        ));
    }
    view.workspace_selection_status = "Selecting workspace and confirming daemon state…".into();
    view.workspace_access_known = false;
    view.workspace_access_status =
        "Workspace selection changed; refresh scoped access status.".into();
    clear_attachments(
        view,
        "Workspace selection changed. Refresh files before attaching.",
    );
    view.workspace_files_view.clear();
    view.workspace_files_root.clear();
    view.workspace_files_truncated = false;
    view.workspace_changes_view.clear();
    view.workspace_changes_root.clear();
    view.workspace_changes_truncated = false;
    let selected = request(
        client,
        RequestPayload::WorkspaceSelect {
            workspace_id: workspace_id.clone(),
        },
    );
    match selected {
        Ok(reply) if reply["workspace_id"].as_str() == Some(workspace_id.as_str()) => {}
        Ok(_) => {
            let refresh = refresh_workspace_state(client, view);
            if let Err(error) = refresh {
                view.workspace_selection_status =
                    format!("Selection was not confirmed; current workspace is unknown: {error}");
            } else {
                view.workspace_selection_status =
                    "Daemon did not confirm the requested workspace; current selection was refreshed.".into();
            }
            return Err(Error::Failed(
                "workspace selection was not confirmed".into(),
            ));
        }
        Err(error) => {
            match refresh_workspace_state(client, view) {
                Ok(()) => {
                    view.workspace_selection_status = format!(
                        "Workspace selection failed; showing the daemon's current selection: {error}"
                    );
                }
                Err(refresh_error) => {
                    view.workspace_selection_status = format!(
                        "Workspace selection failed and current selection is unknown: {error}; refresh failed: {refresh_error}"
                    );
                }
            }
            return Err(error);
        }
    }
    if let Err(error) = refresh_workspace_state(client, view) {
        view.workspace_selection_status = format!(
            "Selection acknowledgement received, but current workspace could not be verified: {error}"
        );
        return Err(error);
    }
    if view.selected_workspace_id != workspace_id {
        view.workspace_selection_status =
            "Daemon selection differs from the requested workspace; showing authoritative state."
                .into();
        return Err(Error::Failed("workspace selection did not persist".into()));
    }
    view.workspace_selection_status = "Workspace selection confirmed by daemon.".into();
    if view.workspace_revocation_capable
        && let Err(error) = refresh_workspace_access(client, view)
    {
        view.workspace_access_status =
            format!("Selected workspace access state unavailable: {error}");
    }
    if view.active_page == "workspace" {
        if view.workspace_files_capable
            && let Err(error) = refresh_workspace_files(client, session, view)
        {
            view.workspace_files_status = format!("Workspace snapshot unavailable: {error}");
        }
        if view.workspace_changes_capable
            && let Err(error) = refresh_workspace_changes(client, session, view)
        {
            view.workspace_changes_status = format!("Workspace comparison unavailable: {error}");
        }
    }
    Ok(())
}
fn provider_endpoint_key(provider: &str) -> Option<&'static str> {
    match provider {
        "ollama" => Some("ollamaEndpoint"),
        "lm-studio" => Some("lmStudioEndpoint"),
        "llama-cpp-server" => Some("llamaCppEndpoint"),
        _ => None,
    }
}

fn refresh_provider_endpoint(client: &mut Client, view: &mut View) -> Result<(), Error> {
    view.provider_endpoint_provider.clear();
    view.provider_endpoint.clear();
    view.provider_endpoint_status.clear();
    if view.provider_endpoints_capable
        && let Some(key) = provider_endpoint_key(&view.selected_provider)
    {
        let response = request(client, RequestPayload::DesktopSettings {})?;
        if let Some(endpoint) = response["values"][key].as_str().filter(|s| !s.is_empty()) {
            view.provider_endpoint_provider = view.selected_provider.clone();
            view.provider_endpoint = endpoint.into();
        }
    }
    Ok(())
}

fn save_provider_endpoint(
    client: &mut Client,
    view: &mut View,
    provider: &str,
    endpoint: &str,
) -> Result<(), Error> {
    let key = provider_endpoint_key(provider)
        .filter(|_| view.provider_endpoints_capable && provider == view.provider_endpoint_provider)
        .ok_or_else(|| {
            Error::Usage("The editable local provider changed. Refresh the Models page.".into())
        })?;
    let endpoint = endpoint.trim();
    let address = endpoint.strip_prefix("http://").unwrap_or_default();
    let address = address
        .strip_suffix('/')
        .unwrap_or(address)
        .parse::<std::net::SocketAddr>();
    let expected = address
        .ok()
        .filter(|address| address.ip().is_loopback() && address.port() != 0)
        .map(|address| format!("http://{address}"))
        .ok_or_else(|| Error::Usage("Use a loopback HTTP address with a port.".into()))?;
    let response = request(
        client,
        RequestPayload::DesktopSetting {
            key: key.into(),
            value: expected.clone().into(),
        },
    )?;
    if response["accepted"] != true {
        return Err(Error::Failed(
            "The daemon did not save the provider endpoint.".into(),
        ));
    }
    refresh_provider_endpoint(client, view)?;
    if view.provider_endpoint_provider != provider || view.provider_endpoint != expected {
        return Err(Error::Failed(
            "The saved endpoint could not be read back.".into(),
        ));
    }
    let model_refresh = refresh_models(client, view);
    view.provider_endpoint_status = if model_refresh.is_ok() {
        "Saved endpoint and refreshed models.".into()
    } else {
        "Saved endpoint. Model discovery is currently unavailable.".into()
    };
    Ok(())
}

fn refresh_models(client: &mut Client, view: &mut View) -> Result<(), Error> {
    if view.local_provider_catalog {
        let state = request(client, RequestPayload::TerminalState {})?;
        view.local_providers = state["providers"]
            .as_array()
            .ok_or_else(|| Error::Protocol("invalid local provider list".into()))?
            .iter()
            .take(4)
            .filter_map(|row| row["provider_id"].as_str().map(str::to_owned))
            .collect();
    }
    clear_provider_diagnostics(
        view,
        "Refresh diagnostics after checking the selected provider.",
    );
    let data = match request(client, RequestPayload::ModelList {}) {
        Ok(data) => data,
        Err(error) => {
            view.models.clear();
            if view.local_provider_catalog || view.cloud_chat {
                if let Ok(current) = request(client, RequestPayload::ModelCurrent {}) {
                    view.selected_provider =
                        current["provider_id"].as_str().unwrap_or_default().into();
                    view.selected_model = current["model_id"].as_str().unwrap_or_default().into();
                    view.ollama_helper_capable =
                        view.ollama_helper_advertised && view.selected_provider == "ollama";
                }
            } else {
                view.selected_provider.clear();
                view.selected_model.clear();
            }
            let _ = refresh_provider_endpoint(client, view);
            view.model_status = format!("Models unavailable: {error}");
            return Err(error);
        }
    };
    let Some(rows) = data["models"].as_array() else {
        view.models.clear();
        view.selected_provider.clear();
        view.selected_model.clear();
        view.model_status = "Models unavailable: invalid model list".into();
        return Err(Error::Protocol("invalid model list".into()));
    };
    let mut models = Vec::new();
    for row in rows.iter().take(256) {
        let Some(provider) = row["provider_id"]
            .as_str()
            .filter(|value| !value.is_empty())
        else {
            continue;
        };
        let Some(model) = row["model_id"].as_str().filter(|value| !value.is_empty()) else {
            continue;
        };
        let label = format!("{provider} / {model}");
        models.push((provider.to_owned(), model.to_owned(), label));
    }
    let current = match request(client, RequestPayload::ModelCurrent {}) {
        Ok(current) => current,
        Err(error) => {
            view.models = models;
            view.selected_provider.clear();
            view.selected_model.clear();
            view.model_status = format!("Models unavailable: {error}");
            return Err(error);
        }
    };
    view.selected_provider = current["provider_id"]
        .as_str()
        .unwrap_or_default()
        .to_owned();
    view.selected_model = current["model_id"].as_str().unwrap_or_default().to_owned();
    if view.local_provider_catalog {
        view.ollama_helper_capable =
            view.ollama_helper_advertised && view.selected_provider == "ollama";
    }
    let _ = refresh_provider_endpoint(client, view);
    let selected_available = models.iter().any(|(provider, model, _)| {
        provider == &view.selected_provider && model == &view.selected_model
    });
    view.model_status = if models.is_empty() {
        if view.selected_provider.is_empty() || view.selected_model.is_empty() {
            "No models are available.".into()
        } else {
            format!(
                "No models available; current selection: {} / {}",
                view.selected_provider, view.selected_model
            )
        }
    } else if selected_available {
        format!(
            "Saved selection: {} / {}",
            view.selected_provider, view.selected_model
        )
    } else if view.selected_provider.is_empty() || view.selected_model.is_empty() {
        "No saved model selection.".into()
    } else {
        format!(
            "Saved selection unavailable: {} / {}",
            view.selected_provider, view.selected_model
        )
    };
    view.models = models;
    Ok(())
}

fn refresh_ollama_helper(client: &mut Client, view: &mut View) -> Result<(), Error> {
    if !view.ollama_helper_capable {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    let result = request(
        client,
        RequestPayload::ModelHelperState {
            component: "ollamaPuller".into(),
            offset: Some(0),
            retain_models: Some(false),
        },
    )?;
    if result["component"].as_str() != Some("ollamaPuller") || !result["properties"].is_object() {
        return Err(Error::Protocol("invalid Ollama helper state".into()));
    }
    let properties = &result["properties"];
    view.ollama_pulling = properties["pulling"].as_bool().unwrap_or(false);
    view.ollama_active_model = properties["activeModel"]
        .as_str()
        .unwrap_or_default()
        .chars()
        .take(256)
        .collect();
    view.ollama_progress = properties["progress"]
        .as_f64()
        .unwrap_or_default()
        .clamp(0.0, 1.0);
    let status = properties["errorText"]
        .as_str()
        .filter(|s| !s.is_empty())
        .or_else(|| properties["statusText"].as_str())
        .unwrap_or("Ollama model helper is ready.");
    view.ollama_helper_status = status.chars().take(1024).collect();
    Ok(())
}

fn ollama_model_action(
    client: &mut Client,
    view: &mut View,
    action: &str,
    model: &str,
    confirmation: &str,
) -> Result<(), Error> {
    if !view.ollama_helper_capable {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    if !matches!(action, "pull" | "removeModel" | "cancel") {
        return Err(Error::Usage("Unsupported Ollama model operation.".into()));
    }
    if action == "cancel" {
        if !view.ollama_pulling {
            return Err(Error::Remote("invalid-state".into()));
        }
    } else if model.trim().is_empty() || model.len() > 256 || model.contains('\0') {
        return Err(Error::Usage(
            "Enter a model ID up to 256 UTF-8 bytes.".into(),
        ));
    }
    if action == "removeModel" && confirmation != model {
        return Err(Error::Usage(
            "Type the exact model ID to confirm removal.".into(),
        ));
    }
    let result = request(
        client,
        RequestPayload::ModelHelperAction {
            component: "ollamaPuller".into(),
            action: action.into(),
            value: if action == "cancel" {
                String::new()
            } else {
                model.into()
            },
        },
    )?;
    if result["accepted"] != true {
        return Err(Error::Failed(
            "Ollama model operation was not accepted.".into(),
        ));
    }
    if action == "removeModel" {
        view.ollama_remove_confirmation.clear();
    }
    refresh_ollama_helper(client, view)?;
    if action == "cancel" {
        view.ollama_helper_status = "Cancellation stopped the local wait; Ollama may continue. Refresh status and models before retrying.".into();
    } else if action == "removeModel" {
        view.ollama_helper_status = format!(
            "Removal requested for {model}. {} Refresh the model catalog to confirm availability.",
            view.ollama_helper_status
        );
    } else if !view.ollama_pulling {
        view.ollama_helper_status =
            format!("Pull requested for {model}. {}", view.ollama_helper_status);
    }
    Ok(())
}

fn refresh_theme(client: &mut Client, view: &mut View) -> Result<(), Error> {
    if !view.desktop_settings_capable {
        view.theme_status = "Theme preferences are not supported by this daemon.".into();
        view.response_profile_status =
            "Chat response instructions are not supported by this daemon.".into();
        return Ok(());
    }
    let response = request(client, RequestPayload::DesktopSettings {})?;
    let theme = response["values"]["themeName"]
        .as_str()
        .filter(|value| matches!(*value, "system" | "light" | "dark"))
        .ok_or_else(|| Error::Protocol("invalid desktop theme preference".into()))?;
    view.theme_name = theme.into();
    view.theme_status = format!("Using the {theme} theme.");
    let profile = response["values"]["responseProfileInstructions"]
        .as_str()
        .unwrap_or_default();
    if profile.encode_utf16().count() > 2000 || profile.contains('\0') {
        return Err(Error::Protocol(
            "invalid Chat response instructions preference".into(),
        ));
    }
    view.response_profile_instructions = profile.to_owned();
    view.response_profile_status = if profile.is_empty() {
        "No additional Chat response instructions are saved.".into()
    } else {
        "Saved instructions apply to subsequent Chat requests only.".into()
    };
    Ok(())
}

fn save_response_profile(client: &mut Client, view: &mut View, value: &str) -> Result<(), Error> {
    if !view.desktop_settings_capable {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    if value.contains('\0') || value.encode_utf16().count() > 2000 {
        view.response_profile_status =
            "Instructions must be at most 2,000 UTF-16 units and contain no NUL.".into();
        return Err(Error::Usage(
            "Chat response instructions exceed the supported limit.".into(),
        ));
    }
    let normalized = value.trim();
    view.response_profile_status = "Saving Chat response instructions…".into();
    let response = request(
        client,
        RequestPayload::DesktopSetting {
            key: "responseProfileInstructions".into(),
            value: normalized.into(),
        },
    )?;
    if response["accepted"] != true {
        let _ = refresh_theme(client, view);
        view.response_profile_status =
            "Save was not accepted; showing the daemon's saved value.".into();
        return Err(Error::Failed(
            "The daemon did not accept Chat response instructions.".into(),
        ));
    }
    refresh_theme(client, view)?;
    if view.response_profile_instructions != normalized {
        view.response_profile_status =
            "The daemon's saved value did not match; review it before sending another request."
                .into();
        return Err(Error::Failed(
            "Saved Chat response instructions could not be confirmed.".into(),
        ));
    }
    view.response_profile_status = if normalized.is_empty() {
        "Cleared saved Chat response instructions.".into()
    } else {
        "Saved. Instructions apply to subsequent Chat requests only.".into()
    };
    Ok(())
}

fn refresh_provider_diagnostics(client: &mut Client, view: &mut View) -> Result<(), Error> {
    view.provider_diagnostics.clear();
    if !view.provider_diagnostics_capable {
        view.provider_diagnostics_status =
            "Provider diagnostics are not advertised by this daemon.".into();
        return Err(Error::Remote("unsupported-capability".into()));
    }
    if view.selected_provider.is_empty() {
        view.provider_diagnostics_status =
            "Select a model before loading provider diagnostics.".into();
        return Ok(());
    }
    view.provider_diagnostics_status = "Loading local provider configuration…".into();
    let response = request(
        client,
        RequestPayload::DesktopSettingsService {
            action: "providerSettingsState".into(),
            arguments: vec![Value::String(view.selected_provider.clone())],
        },
    )?;
    let result = &response["result"];
    if result["id"].as_str() != Some(&view.selected_provider) {
        return Err(Error::Protocol(
            "provider diagnostics identity mismatch".into(),
        ));
    }
    let known = result["known"]
        .as_bool()
        .ok_or_else(|| Error::Protocol("invalid provider diagnostics state".into()))?;
    if !known || result["local"] != true {
        view.provider_diagnostics_status =
            "The selected provider configuration is not known as local.".into();
        return Ok(());
    }
    let model = result["configuredModel"]
        .as_str()
        .filter(|model| !model.is_empty() && model.len() <= 256)
        .ok_or_else(|| Error::Protocol("invalid configured provider model".into()))?;
    let context = result["contextWindow"]
        .as_u64()
        .filter(|value| (1..=1_000_000).contains(value))
        .ok_or_else(|| Error::Protocol("invalid provider context window".into()))?;
    let catalog = match result["catalog"].as_str() {
        Some("Cached") => "Cached catalog",
        Some("Unknown") => "Catalog not checked",
        _ => return Err(Error::Protocol("invalid provider catalog state".into())),
    };
    let models = result["discoveredModels"]
        .as_array()
        .ok_or_else(|| Error::Protocol("invalid discovered model list".into()))?;
    let model_lines = models
        .iter()
        .take(64)
        .filter_map(|value| value.as_str())
        .map(|value| catalog_text(Some(value), 128))
        .filter(|value| !value.is_empty())
        .collect::<Vec<_>>();
    view.provider_diagnostics = format!(
        "Provider: {}\nConfigured model: {}\nConfigured context window: {context} tokens\n{catalog}\nKnown local catalog entries: {}{}",
        catalog_text(Some(&view.selected_provider), 128),
        catalog_text(Some(model), 256),
        model_lines.len(),
        if model_lines.is_empty() {
            String::new()
        } else {
            format!("\n{}", model_lines.join("\n"))
        },
    );
    view.provider_diagnostics_status = "Connectivity is Unknown; this view reads cached configuration and does not probe the provider.".into();
    Ok(())
}

fn export_dev_backup(
    client: &mut Client,
    view: &mut View,
    index: i32,
    path: &str,
) -> Result<(), Error> {
    if !view.dev_backup_capable {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    let domain_index =
        usize::try_from(index).map_err(|_| Error::Usage("Choose a snapshot domain.".into()))?;
    let profile = domain_index == 4;
    if profile && !view.dev_backup_set_capable {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    let domain = ["chat", "settings", "memory", "agent"]
        .get(domain_index)
        .copied();
    if !profile && domain.is_none() {
        return Err(Error::Usage("Choose a snapshot domain.".into()));
    }
    let destination = Path::new(path);
    if !destination.is_absolute() {
        return Err(Error::Usage(
            "Enter an absolute path for a new snapshot file.".into(),
        ));
    }
    let snapshot = if profile {
        sentinel_ipc::backup::export_profile(client)?
    } else {
        sentinel_ipc::backup::export(client, domain.expect("validated export domain"))?
    };
    let published = sentinel_ipc::backup::write_new(destination, &snapshot)?;
    let label = if profile {
        "development profile set"
    } else {
        domain.expect("validated export domain")
    };
    view.dev_backup_status = format!(
        "Saved verified {label} development snapshot to {}. Store it privately.",
        published.display()
    );
    Ok(())
}

fn clear_provider_diagnostics(view: &mut View, message: &str) {
    view.provider_diagnostics.clear();
    view.provider_diagnostics_status = message.into();
}

fn refresh_runtime_projection(client: &mut Client, view: &mut View) -> Result<(), Error> {
    view.runtime_projection_view.clear();
    if !view.runtime_projection_capable {
        view.runtime_projection_status =
            "Runtime projection is not advertised by this daemon.".into();
        return Err(Error::Remote("unsupported-capability".into()));
    }
    view.runtime_projection_status = "Loading daemon runtime status…".into();
    let response = request(client, RequestPayload::DesktopProjection { page: 0 })?;
    if response["page"] != 0 || response["pages"].as_u64().unwrap_or(0) == 0 {
        return Err(Error::Protocol("invalid runtime projection page".into()));
    }
    let properties = &response["properties"];
    let provider = catalog_text(properties["activeRuntimeProviderLabel"].as_str(), 128);
    let model = catalog_text(properties["activeRuntimeModelLabel"].as_str(), 256);
    let state = catalog_text(properties["activeRuntimeReadinessState"].as_str(), 64);
    let summary = catalog_text(properties["activeRuntimeReadinessSummary"].as_str(), 512);
    let local = catalog_text(properties["activeRuntimeLocalOnlySummary"].as_str(), 512);
    if state.is_empty() || summary.is_empty() {
        return Err(Error::Protocol(
            "runtime projection omitted readiness state".into(),
        ));
    }
    view.runtime_projection_view = format!(
        "Provider: {}\nModel: {}\nReadiness: {}\n{}\n{}",
        if provider.is_empty() {
            "Unavailable"
        } else {
            &provider
        },
        if model.is_empty() {
            "Unavailable"
        } else {
            &model
        },
        state,
        summary,
        local
    );
    view.runtime_projection_status =
        "Status from daemon projection; connectivity is shown only as reported by the daemon."
            .into();
    Ok(())
}

fn refresh_workspace_runtime(client: &mut Client, view: &mut View) -> Result<(), Error> {
    view.workspace_runtime_view.clear();
    if !view.workspace_runtime_capable {
        view.workspace_runtime_status =
            "Workspace runtime details are not advertised by this daemon.".into();
        return Err(Error::Remote("unsupported-capability".into()));
    }
    view.workspace_runtime_status = "Loading configured workspace permissions…".into();
    let response = request(client, RequestPayload::TerminalState {})?;
    let current = &response["workspace"];
    let id = catalog_text(current["id"].as_str(), 128);
    let name = catalog_text(current["name"].as_str(), 160);
    let root = catalog_text(current["root"].as_str(), 1024);
    let access = catalog_text(current["access"].as_str(), 128);
    let permission = catalog_text(current["permission"].as_str(), 128);
    if id.is_empty() {
        view.workspace_runtime_status = "No workspace is currently selected.".into();
        view.workspace_runtime_view =
            "Agent filesystem access is not established by this view.".into();
        return Ok(());
    }
    view.workspace_runtime_view = format!(
        "Workspace: {} ({id})\nRoot: {}\nAccess: {}\nPermission: {}\nArchived: {}",
        if name.is_empty() { id.as_str() } else { &name },
        if root.is_empty() { "Unknown" } else { &root },
        if access.is_empty() {
            "Unknown"
        } else {
            &access
        },
        if permission.is_empty() {
            "Unknown"
        } else {
            &permission
        },
        current["archived"].as_bool().unwrap_or(false)
    );
    view.workspace_runtime_status = "Configured selection from terminal.state. This does not grant access beyond the daemon's policy.".into();
    Ok(())
}

fn refresh_settings_status(client: &mut Client, view: &mut View) -> Result<(), Error> {
    let runtime = if view.runtime_projection_capable {
        refresh_runtime_projection(client, view)
    } else {
        Ok(())
    };
    if let Err(error) = &runtime
        && view.runtime_projection_status.starts_with("Loading")
    {
        view.runtime_projection_status = format!("Runtime status unavailable: {error}");
    }
    let workspace = if view.workspace_runtime_capable {
        refresh_workspace_runtime(client, view)
    } else {
        Ok(())
    };
    if let Err(error) = &workspace
        && view.workspace_runtime_status.starts_with("Loading")
    {
        view.workspace_runtime_status = format!("Workspace status unavailable: {error}");
    }
    let access = if view.workspace_revocation_capable {
        refresh_workspace_access(client, view)
    } else {
        Ok(())
    };
    if let Err(error) = &access
        && view.workspace_access_status.starts_with("Loading")
    {
        view.workspace_access_status = format!("Workspace access state unavailable: {error}");
    }
    match (runtime, workspace, access) {
        (Ok(()), Ok(()), Ok(())) => Ok(()),
        (Err(error), _, _) => Err(error),
        (_, Err(error), _) => Err(error),
        (_, _, Err(error)) => Err(error),
    }
}

fn refresh_workspace_access(client: &mut Client, view: &mut View) -> Result<(), Error> {
    if !view.workspace_revocation_capable {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    view.workspace_access_status = "Loading access for the selected configured workspace…".into();
    view.workspace_access_known = false;
    view.workspace_access_id.clear();
    let terminal = request(client, RequestPayload::TerminalState {})?;
    let workspace_id = terminal["workspace"]["id"]
        .as_str()
        .filter(|id| !id.is_empty())
        .ok_or_else(|| Error::Remote("no-workspace-selected".into()))?;
    let settings = request(client, RequestPayload::DesktopSettings {})?;
    let value = settings["values"]["devWorkspaceAccess"]
        .as_str()
        .ok_or_else(|| Error::Protocol("workspace access setting is unavailable".into()))?;
    view.workspace_access_enabled = match value {
        "true" => true,
        "false" => false,
        _ => return Err(Error::Protocol("invalid workspace access setting".into())),
    };
    view.workspace_access_known = true;
    view.workspace_access_id = workspace_id.to_owned();
    view.workspace_access_status = if view.workspace_access_enabled {
        format!(
            "Development workspace access is enabled for selected workspace {workspace_id}. This is scoped to that configured root."
        )
    } else {
        format!("Development workspace access is revoked for selected workspace {workspace_id}.")
    };
    Ok(())
}

fn set_dev_workspace_access(
    client: &mut Client,
    view: &mut View,
    enabled: bool,
) -> Result<(), Error> {
    if !view.workspace_revocation_capable {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    if enabled && view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    let terminal = request(client, RequestPayload::TerminalState {})?;
    let current_id = terminal["workspace"]["id"].as_str().unwrap_or_default();
    if current_id.is_empty() || current_id != view.workspace_access_id {
        let _ = refresh_workspace_access(client, view);
        return Err(Error::Failed(
            "The selected workspace changed. Review its current access state before changing it."
                .into(),
        ));
    }
    view.workspace_access_status = if enabled {
        "Re-enabling access for the selected configured workspace…".into()
    } else {
        "Revoking access for the selected configured workspace…".into()
    };
    let response = request(
        client,
        RequestPayload::DesktopSetting {
            key: "devWorkspaceAccess".into(),
            value: if enabled { "true" } else { "false" }.into(),
        },
    )?;
    if response["accepted"] != true {
        refresh_workspace_access(client, view)?;
        return Err(Error::Failed(
            "The daemon did not accept the workspace access change.".into(),
        ));
    }
    refresh_workspace_access(client, view)?;
    if !enabled {
        view.workspace_access_status.push_str(
            " Revocation is not a quiescence barrier; an operation already in flight may finish.",
        );
    }
    Ok(())
}

fn refresh_extension_catalog(client: &mut Client, view: &mut View) -> Result<(), Error> {
    if !view.extension_catalog_capable {
        view.extension_catalog_view.clear();
        view.extension_catalog_truncated = false;
        view.extension_catalog_status =
            "Extension catalog is not advertised by this daemon.".into();
        return Err(Error::Remote("unsupported-capability".into()));
    }

    // Clear prior data before the request so an error cannot leave a stale inventory
    // looking current after a failed refresh.
    view.extension_catalog_view.clear();
    view.extension_catalog_truncated = false;
    view.extension_catalog_status = "Loading extension catalog…".into();
    let response = request(
        client,
        RequestPayload::DesktopSettingsService {
            action: "extensionsHubState".into(),
            arguments: vec![],
        },
    )?;
    let snapshot = &response["result"];
    let entries = snapshot["entries"]
        .as_array()
        .ok_or_else(|| Error::Protocol("invalid extension catalog entries".into()))?;
    let tools = snapshot["toolEntries"]
        .as_array()
        .ok_or_else(|| Error::Protocol("invalid extension catalog tools".into()))?;
    let truncated = snapshot["truncated"].as_bool().unwrap_or(false);
    let extension_count = snapshot["extensionCount"]
        .as_u64()
        .unwrap_or(entries.len() as u64);
    let tool_count = snapshot["toolCount"].as_u64().unwrap_or(tools.len() as u64);
    let mut lines = Vec::new();
    for entry in entries.iter().take(64) {
        let name = catalog_text(entry["name"].as_str().or(entry["id"].as_str()), 160);
        if name.is_empty() {
            continue;
        }
        let kind = catalog_text(entry["type"].as_str(), 24);
        let version = catalog_text(entry["version"].as_str(), 80);
        let health = catalog_text(entry["health"].as_str(), 40);
        let maturity = catalog_text(entry["maturity"].as_str(), 40);
        let availability = match (entry["enabledKnown"].as_bool(), entry["enabled"].as_bool()) {
            (Some(true), Some(true)) => "enabled",
            (Some(true), Some(false)) => "disabled",
            _ if entry["available"] == true => "available",
            _ => "not available",
        };
        let mut details = vec![availability.to_owned()];
        if !kind.is_empty() {
            details.push(kind);
        }
        if !version.is_empty() {
            details.push(format!("version {version}"));
        }
        if !health.is_empty() {
            details.push(format!("health {health}"));
        }
        if !maturity.is_empty() {
            details.push(format!("maturity {maturity}"));
        }
        lines.push(format!("{name} — {}", details.join(" · ")));
    }
    for tool in tools.iter().take(128) {
        let name = catalog_text(tool["name"].as_str().or(tool["id"].as_str()), 160);
        if name.is_empty() {
            continue;
        }
        let origin = catalog_text(tool["origin"].as_str(), 40);
        let enabled = match tool["enabled"].as_bool() {
            Some(true) => "enabled",
            Some(false) => "disabled",
            None => "availability unknown",
        };
        lines.push(if origin.is_empty() {
            format!("Tool: {name} — {enabled}")
        } else {
            format!("Tool: {name} — {origin} · {enabled}")
        });
    }
    let text = lines.join("\n");
    view.extension_catalog_view = text.chars().take(32_000).collect();
    view.extension_catalog_truncated =
        truncated || entries.len() > 64 || tools.len() > 128 || text.chars().count() > 32_000;
    view.extension_catalog_status = if extension_count == 0 && tool_count == 0 {
        if view.extension_catalog_truncated {
            "No supported extension or tool metadata was found in the visible catalog; the scan is incomplete.".into()
        } else {
            "No supported extensions or tools were found in the configured catalog.".into()
        }
    } else {
        format!(
            "Catalog lists {extension_count} extensions and {tool_count} tools. Inventory only; nothing is loaded or started."
        )
    };
    Ok(())
}

fn catalog_text(value: Option<&str>, max_chars: usize) -> String {
    value
        .unwrap_or_default()
        .chars()
        .filter(|ch| !ch.is_control())
        .take(max_chars)
        .collect()
}

fn set_theme(client: &mut Client, view: &mut View, index: i32) -> Result<(), Error> {
    let theme = match index {
        0 => "system",
        1 => "light",
        2 => "dark",
        _ => return Err(Error::Usage("Choose a supported theme.".into())),
    };
    if !view.desktop_settings_capable {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    match request(
        client,
        RequestPayload::DesktopSetting {
            key: "themeName".into(),
            value: theme.into(),
        },
    ) {
        Ok(response) if response["accepted"] == true => {
            view.theme_name = theme.into();
            view.theme_status = format!("Saved the {theme} theme.");
            Ok(())
        }
        Ok(_) => {
            let _ = refresh_theme(client, view);
            view.theme_status = format!(
                "Theme change was not accepted. Current theme: {}.",
                view.theme_name
            );
            Err(Error::Failed(
                "The daemon did not accept the theme change.".into(),
            ))
        }
        Err(error) => {
            let _ = refresh_theme(client, view);
            view.theme_status =
                format!("Theme change failed; restored the daemon's saved theme ({error}).");
            Err(error)
        }
    }
}

fn next_memory_tag(view: &mut View) -> String {
    view.memory_request_counter = view.memory_request_counter.wrapping_add(1);
    format!("slint-{}", view.memory_request_counter)
}

fn memory_request(
    client: &mut Client,
    session_id: &str,
    request_value: Value,
) -> Result<Value, Error> {
    let tag = request_value["requestTag"]
        .as_str()
        .ok_or_else(|| Error::Protocol("memory request has no request tag".into()))?
        .to_owned();
    let response = request(
        client,
        RequestPayload::DesktopAction {
            action: "manageMemory".into(),
            arguments: vec![request_value],
            session_id: session_id.into(),
        },
    )?;
    if response["accepted"] != true {
        return Err(Error::Failed(
            "The daemon rejected the memory action.".into(),
        ));
    }
    let value = response["result"]["value"]
        .as_object()
        .map(|_| response["result"]["value"].clone())
        .ok_or_else(|| Error::Protocol("invalid memory action result".into()))?;
    if value["requestTag"].as_str() != Some(&tag) {
        return Err(Error::Protocol(
            "memory response request tag mismatch".into(),
        ));
    }
    Ok(value)
}

fn memory_list(
    client: &mut Client,
    session: &Session,
    view: &mut View,
    append: bool,
) -> Result<(), Error> {
    if !view.memory_capable {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    if session.id.is_empty() {
        view.memory_records.clear();
        view.memory_id.clear();
        view.memory_revision.clear();
        view.memory_status = "Choose or create a Chat session to manage its memory.".into();
        return Ok(());
    }
    let tag = next_memory_tag(view);
    let mut query = serde_json::json!({
        "operation":"list","scope":1,"requestTag":tag
    });
    if append && !view.memory_next_cursor.is_empty() {
        query["cursor"] = Value::String(view.memory_next_cursor.clone());
    } else if append {
        return Ok(());
    }
    let value = memory_request(client, &session.id, query)?;
    if value["status"].as_str() != Some("Ready") || value["requestTag"].as_str() != Some(&tag) {
        view.memory_status =
            memory_status_message(value["status"].as_str().unwrap_or("InvalidInput"));
        return Ok(());
    }
    let Some(rows) = value["records"].as_array() else {
        return Err(Error::Protocol("memory list has no records".into()));
    };
    let mut records = if append {
        view.memory_records.clone()
    } else {
        Vec::new()
    };
    for row in rows.iter().take(10) {
        let (Some(id), Some(revision), Some(key)) = (
            row["id"].as_str(),
            row["revision"].as_str(),
            row["key"].as_str(),
        ) else {
            continue;
        };
        if !records.iter().any(|(known, _, _)| known == id) {
            records.push((id.into(), revision.into(), format!("{key} · #{id}")));
        }
    }
    view.memory_next_cursor = value["nextCursor"].as_str().unwrap_or_default().into();
    view.memory_records = records;
    let selected = view.memory_id.clone();
    if let Some((index, _)) = view
        .memory_records
        .iter()
        .enumerate()
        .find(|(_, (id, _, _))| id == &selected)
    {
        memory_read(client, session, view, index)?;
    } else {
        view.memory_id.clear();
        view.memory_revision.clear();
        view.memory_key.clear();
        view.memory_value.clear();
        view.memory_status = if view.memory_records.is_empty() {
            "No memory records for this chat yet.".into()
        } else {
            format!(
                "Loaded {} session memory records.",
                view.memory_records.len()
            )
        };
    }
    Ok(())
}

fn memory_read(
    client: &mut Client,
    session: &Session,
    view: &mut View,
    index: usize,
) -> Result<(), Error> {
    let Some((id, _, _)) = view.memory_records.get(index) else {
        return Err(Error::Usage(
            "The selected memory record is no longer available.".into(),
        ));
    };
    let id = id.clone();
    let mut offset = 0u64;
    let mut full_value = String::new();
    let mut first_record = Value::Null;
    loop {
        let tag = next_memory_tag(view);
        let page = memory_request(
            client,
            &session.id,
            serde_json::json!({"operation":"read","scope":1,"id":id,"offset":offset,"requestTag":tag}),
        )?;
        if page["status"].as_str() != Some("Ready") || page["requestTag"].as_str() != Some(&tag) {
            view.memory_id.clear();
            view.memory_revision.clear();
            view.memory_key.clear();
            view.memory_value.clear();
            view.memory_status =
                memory_status_message(page["status"].as_str().unwrap_or("NotFound"));
            return Ok(());
        }
        let record = page["record"].clone();
        if !record.is_object() {
            return Err(Error::Protocol("memory read has no record".into()));
        }
        if first_record.is_null() {
            first_record = record.clone();
        }
        let Some(text) = record["value"].as_str() else {
            return Err(Error::Protocol("memory record has no value page".into()));
        };
        if record["offset"].as_u64() != Some(offset) {
            return Err(Error::Protocol("memory page offset mismatch".into()));
        }
        full_value.push_str(text);
        let length = record["valueLength"].as_u64().unwrap_or_default();
        offset = offset.saturating_add(text.encode_utf16().count() as u64);
        if text.is_empty() && offset < length {
            return Err(Error::Protocol(
                "memory read returned an incomplete page".into(),
            ));
        }
        if offset >= length || offset > 65_536 {
            break;
        }
    }
    view.memory_id = first_record["id"].as_str().unwrap_or_default().into();
    view.memory_revision = first_record["revision"].as_str().unwrap_or_default().into();
    view.memory_key = first_record["key"].as_str().unwrap_or_default().into();
    view.memory_value = full_value;
    view.memory_delete_confirmation.clear();
    view.memory_status = format!(
        "Editing memory record {} at revision {}. Stored only for this Chat session.",
        view.memory_id, view.memory_revision
    );
    Ok(())
}

fn memory_status_message(status: &str) -> String {
    match status {
        "Conflict" => {
            "Memory changed elsewhere. Review the refreshed revision before saving.".into()
        }
        "NotFound" => "Memory record no longer exists. Refresh the list.".into(),
        "InvalidInput" => "Memory input is invalid or exceeds the key/value limits.".into(),
        "LimitExceeded" => "This Chat session has reached its memory record limit.".into(),
        _ => format!("Memory action failed: {status}."),
    }
}

fn set_session_flag(
    client: &mut Client,
    session: &Session,
    view: &mut View,
    id: &str,
    action: &str,
) -> Result<(), Error> {
    if view.running || !view.session_flags_capable || id.is_empty() || session.id != id {
        return Err(Error::Usage(
            "The selected conversation is no longer available for this action.".into(),
        ));
    }
    let state = request(
        client,
        RequestPayload::DesktopAction {
            action: action.into(),
            arguments: vec![Value::String(id.into())],
            session_id: id.into(),
        },
    )?;
    if state["accepted"] != true || state["result"]["value"] != true {
        return Err(Error::Failed(
            "The daemon did not confirm the conversation update.".into(),
        ));
    }
    refresh(client, view)
}

fn rename_session(
    client: &mut Client,
    session: &Session,
    view: &mut View,
    title: &str,
) -> Result<(), Error> {
    if !view.session_management_capable || session.id.is_empty() {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    if title.len() > 1024 || title.contains('\0') {
        return Err(Error::Usage(
            "Session titles are limited to 1,024 UTF-8 bytes.".into(),
        ));
    }
    let response = request(
        client,
        RequestPayload::DesktopAction {
            action: "renameConversation".into(),
            arguments: vec![
                Value::String(session.id.clone()),
                Value::String(title.to_owned()),
            ],
            session_id: session.id.clone(),
        },
    )?;
    if response["accepted"] != true || response["result"]["value"] != true {
        return Err(Error::Failed(
            "The daemon did not confirm the session rename.".into(),
        ));
    }
    let normalized = if title.trim().is_empty() {
        "Untitled Conversation"
    } else {
        title.trim()
    };
    if let Some((_, value)) = view.sessions.iter_mut().find(|(id, _)| id == &session.id) {
        *value = normalized.chars().take(120).collect();
    }
    view.status = format!("Renamed Chat session to {normalized}.");
    Ok(())
}

fn delete_session(
    client: &mut Client,
    session: &mut Session,
    view: &mut View,
    confirmation: &str,
) -> Result<(), Error> {
    if !view.session_management_capable || session.id.is_empty() {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    if confirmation != "DELETE" {
        return Err(Error::Usage(
            "Type DELETE to permanently remove this Chat session and its Agent audit and memory."
                .into(),
        ));
    }
    let id = session.id.clone();
    let response = request(
        client,
        RequestPayload::DesktopAction {
            action: "requestPermanentDeleteConversation".into(),
            arguments: vec![Value::String(id.clone())],
            session_id: id.clone(),
        },
    )?;
    if response["accepted"] != true || response["result"]["value"] != true {
        return Err(Error::Failed(
            "The daemon did not confirm permanent deletion.".into(),
        ));
    }
    *session = Session::default();
    view.selected_id.clear();
    view.session_delete_confirmation.clear();
    view.history.clear();
    view.response.clear();
    view.last_message_id = 0;
    view.baseline_message_id = 0;
    view.pending_message_id = 0;
    view.latest_user_id = 0;
    view.latest_retry_id = 0;
    view.last_assistant.clear();
    view.live_user.clear();
    view.show_overlay = false;
    clear_memory_selection(view, "Session deleted with its private memory.");
    clear_agent_history(view, "Session deleted with its Agent audit.");
    refresh(client, view)?;
    view.status = "Chat session, Agent audit, and memory permanently deleted.".into();
    Ok(())
}

fn memory_save(client: &mut Client, session: &Session, view: &mut View) -> Result<(), Error> {
    if !view.memory_capable || session.id.is_empty() {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    if view.memory_key.is_empty()
        || view.memory_key.encode_utf16().count() > 256
        || view.memory_value.encode_utf16().count() > 65_536
        || view.memory_value.len() > 65_536
    {
        view.memory_status = "Key must be 1–256 UTF-16 units; value is limited to 65,536 UTF-16 units and UTF-8 bytes.".into();
        return Ok(());
    }
    let operation = if view.memory_id.is_empty() {
        "create"
    } else {
        "correct"
    };
    let mut value = serde_json::json!({
        "operation":operation,"scope":1,"key":view.memory_key,"value":view.memory_value,
        "session":session.id,"workspace":"","requestTag":next_memory_tag(view)
    });
    if operation == "correct" {
        value["id"] = Value::String(view.memory_id.clone());
        value["revision"] = Value::String(view.memory_revision.clone());
    }
    let result = memory_request(client, &session.id, value)?;
    let status = result["status"].as_str().unwrap_or("InvalidInput");
    let final_status = if status == "Ready" {
        let message = if operation == "create" {
            "Memory created for this Chat session.".into()
        } else {
            "Memory correction saved.".into()
        };
        if operation == "create" {
            view.memory_key.clear();
            view.memory_value.clear();
        }
        message
    } else {
        memory_status_message(status)
    };
    memory_list(client, session, view, false)?;
    view.memory_status = final_status;
    Ok(())
}

fn memory_delete(client: &mut Client, session: &Session, view: &mut View) -> Result<(), Error> {
    if view.memory_id.is_empty() || view.memory_delete_confirmation != "DELETE" {
        return Err(Error::Usage(
            "Select a memory record and type DELETE to confirm.".into(),
        ));
    }
    let request_tag = next_memory_tag(view);
    let result = memory_request(
        client,
        &session.id,
        serde_json::json!({"operation":"delete","scope":1,"id":view.memory_id,
            "revision":view.memory_revision,"session":session.id,"workspace":"",
            "confirmed":true,"confirmation":"DELETE","requestTag":request_tag}),
    )?;
    let status = result["status"].as_str().unwrap_or("InvalidInput");
    let final_status = if status == "Ready" {
        view.memory_id.clear();
        view.memory_revision.clear();
        view.memory_key.clear();
        view.memory_value.clear();
        view.memory_delete_confirmation.clear();
        "Memory deleted from this Chat session.".to_owned()
    } else {
        memory_status_message(status)
    };
    memory_list(client, session, view, false)?;
    view.memory_status = final_status;
    Ok(())
}

fn select_local_provider(client: &mut Client, index: usize, view: &mut View) -> Result<(), Error> {
    let provider_id = view
        .local_providers
        .get(index)
        .cloned()
        .ok_or_else(|| Error::Usage("The selected provider is no longer configured.".into()))?;
    let selection = request(
        client,
        RequestPayload::ProviderSelect {
            provider_id: provider_id.clone(),
        },
    )?;
    if selection["provider_id"].as_str() != Some(provider_id.as_str()) {
        return Err(Error::Protocol(
            "daemon selected a different provider".into(),
        ));
    }
    refresh_models(client, view)?;
    if view.ollama_helper_capable {
        refresh_ollama_helper(client, view)?;
    }
    Ok(())
}

fn select_model(client: &mut Client, index: usize, view: &mut View) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    clear_provider_diagnostics(
        view,
        "Model selection changed. Refresh to inspect its cached configuration.",
    );
    let Some((provider, model, _)) = view.models.get(index) else {
        return Err(Error::Usage(
            "The selected model is no longer available.".into(),
        ));
    };
    let provider = provider.clone();
    let model = model.clone();
    let selected = match request(
        client,
        RequestPayload::ModelSelect {
            provider_id: provider.clone(),
            model_id: model.clone(),
        },
    ) {
        Ok(selected) => selected,
        Err(error) => {
            match request(client, RequestPayload::ModelCurrent {}) {
                Ok(current) => {
                    view.selected_provider = current["provider_id"]
                        .as_str()
                        .unwrap_or_default()
                        .to_owned();
                    view.selected_model =
                        current["model_id"].as_str().unwrap_or_default().to_owned();
                    view.model_status = format!(
                        "Selection failed: {error}. Saved selection: {} / {}",
                        view.selected_provider, view.selected_model
                    );
                }
                Err(current_error) => {
                    view.selected_provider.clear();
                    view.selected_model.clear();
                    view.model_status =
                        format!("Selection failed; current model unavailable: {current_error}");
                }
            }
            return Err(error);
        }
    };
    if selected["provider_id"].as_str() != Some(&provider)
        || selected["model_id"].as_str() != Some(&model)
    {
        view.selected_provider.clear();
        view.selected_model.clear();
        view.model_status =
            "Selection result did not match; refresh to check the current model.".into();
        return Err(Error::Protocol("daemon selected a different model".into()));
    }
    view.selected_provider = provider;
    view.selected_model = model;
    view.model_status = format!(
        "Saved selection: {} / {}",
        view.selected_provider, view.selected_model
    );
    Ok(())
}
fn refresh_after_terminal(
    client: &mut Client,
    session: &Session,
    view: &mut View,
) -> Result<(), Error> {
    let snapshot = request(
        client,
        RequestPayload::SessionAttach {
            session_id: session.id.clone(),
        },
    )?;
    let terminal_confirmed = snapshot["session_id"].as_str() == Some(&session.id)
        && snapshot["run_id"].as_str() == Some(&session.run)
        && !session.run.is_empty()
        && matches!(
            snapshot["state"].as_str(),
            Some("completed" | "failed" | "cancelled")
        )
        && snapshot["output"].as_str() == Some(&view.response);
    let mut updated = view.clone();
    load_history(client, session, &mut updated)?;
    if (updated.last_message_id > view.baseline_message_id
        || (view.pending_message_id > 0 && updated.last_message_id == view.pending_message_id))
        && !view.response.is_empty()
        && updated.last_assistant == view.response
        && terminal_confirmed
    {
        *view = updated;
        view.show_overlay = false;
        view.live_user.clear();
    } else {
        view.history_status =
            "Saved history has not confirmed the terminal response; showing the live preview."
                .into();
    }
    refresh(client, view)?;
    if view.agent_mode && view.agent_history_capable {
        view.agent_selected_run = session.run.clone();
        if let Err(error) = refresh_agent_history(client, view, false) {
            view.agent_history_status = format!("Agent audit unavailable: {error}");
        }
    }
    Ok(())
}
fn load_history(client: &mut Client, session: &Session, view: &mut View) -> Result<(), Error> {
    let data = request(
        client,
        RequestPayload::SessionMessages {
            session_id: session.id.clone(),
        },
    )?;
    if data["session_id"].as_str() != Some(&session.id) {
        return Err(Error::Protocol("history belongs to another session".into()));
    }
    let rows = data["messages"]
        .as_array()
        .ok_or_else(|| Error::Protocol("invalid history".into()))?;
    let numeric_id = |row: &Value| {
        row["id"]
            .as_i64()
            .or_else(|| row["id"].as_str().and_then(|value| value.parse().ok()))
            .unwrap_or_default()
    };
    let latest_user = rows.iter().rev().find(|row| row["role"] == "user");
    view.latest_user_id = latest_user.map(numeric_id).unwrap_or_default();
    view.latest_retry_id = rows
        .iter()
        .rev()
        .find(|row| row["role"] == "assistant")
        .filter(|row| {
            latest_user.is_some_and(|user| {
                matches!(
                    row["status"].as_str(),
                    Some("failed" | "interrupted" | "error")
                ) && row["reply_to"].as_i64().or_else(|| {
                    row["reply_to"]
                        .as_str()
                        .and_then(|value| value.parse().ok())
                }) == Some(numeric_id(user))
            })
        })
        .map(numeric_id)
        .unwrap_or_default();
    view.last_message_id = rows
        .last()
        .and_then(|row| row["id"].as_i64())
        .unwrap_or_default();
    view.pending_message_id = rows
        .last()
        .filter(|row| {
            row["role"] == "assistant"
                && matches!(
                    row["status"].as_str(),
                    Some("queued" | "sending" | "streaming")
                )
        })
        .and_then(|row| row["id"].as_i64())
        .unwrap_or_default();
    view.last_assistant = rows
        .last()
        .filter(|row| {
            row["role"] == "assistant"
                && matches!(
                    row["status"].as_str(),
                    Some(
                        "completed" | "received" | "failed" | "cancelled" | "interrupted" | "error"
                    )
                )
        })
        .and_then(|row| row["content"].as_str())
        .unwrap_or_default()
        .to_owned();
    let mut history = String::new();
    let mut truncated = false;
    for row in rows {
        if view.running
            && row["role"] == "assistant"
            && matches!(
                row["status"].as_str(),
                Some("queued" | "sending" | "streaming")
            )
        {
            continue;
        }
        let role = row["role"].as_str().unwrap_or("message");
        let content = row["content"].as_str().unwrap_or_default();
        history.push_str(&format!("{role}: {content}\n"));
        if let Some(category) = row["error_category"]
            .as_str()
            .filter(|category| !category.is_empty() && *category != "None")
        {
            let label: String = category.chars().take(80).collect();
            history.push_str(&format!("  Failure category: {}\n", quoted_path(&label)));
        }
        if let Some(attachments) = row["attachments"].as_array() {
            for name in attachments
                .iter()
                .take(4)
                .filter_map(|item| item["name"].as_str())
            {
                let label = name.chars().take(180).collect::<String>();
                history.push_str(&format!("  Attachment: {}\n", quoted_path(&label)));
            }
        }
        history.push('\n');
        if history.len() > RESPONSE_LIMIT {
            let mut start = history.len() - RESPONSE_LIMIT;
            while !history.is_char_boundary(start) {
                start += 1;
            }
            history.drain(..start);
            truncated = true;
        }
    }
    view.history = history;
    view.history_status = if rows.is_empty() {
        "No saved messages."
    } else if truncated {
        "History preview truncated."
    } else {
        "Saved history"
    }
    .into();
    Ok(())
}
fn attach(client: &mut Client, session: &mut Session, view: &mut View) -> Result<(), Error> {
    view.selected_id = session.id.clone();
    if view.agent_mode {
        view.agent_selected_run = session.run.clone();
    }
    let snapshot = request(
        client,
        RequestPayload::SessionAttach {
            session_id: session.id.clone(),
        },
    )?;
    if snapshot["session_id"].as_str() != Some(&session.id) {
        return Err(Error::Protocol("wrong attached session".into()));
    }
    session.run = snapshot["run_id"].as_str().unwrap_or_default().into();
    session.generation = snapshot["server_generation"].as_str().map(str::to_owned);
    session.sequence = snapshot["event_sequence"].as_u64();
    view.running = is_active(&snapshot["state"]);
    view.agent_mode = view.agent_capable && snapshot["run_type"].as_str() == Some("agent");
    replace_response(view, snapshot["output"].as_str().unwrap_or_default());
    view.truncated |= snapshot["output_truncated"].as_bool().unwrap_or(false);
    view.status = match snapshot["state"].as_str() {
        Some("running") => "Attached to the active run.",
        Some("approval") => "Approval is pending. Approve once, deny, or cancel the request.",
        Some("cancelled") => "Run stopped.",
        Some("failed") => "Run failed.",
        Some("completed") => "Run completed.",
        _ => "Session attached.",
    }
    .into();
    if view.truncated {
        view.status.push_str(" Response preview truncated.");
    }
    view.live_user.clear();
    view.show_overlay = view.running;
    view.approval_id = snapshot["approval_id"]
        .as_str()
        .unwrap_or_default()
        .to_owned();
    view.approval_pending = view.running
        && view.approval_capable
        && !view.approval_id.is_empty()
        && snapshot["state"].as_str() == Some("approval");
    view.approval_detail = if view.approval_pending {
        approval_summary(&snapshot["approval"])
    } else {
        view.approval_id.clear();
        String::new()
    };
    view.agent_progress.clear();
    if view.running && view.agent_mode {
        view.agent_status = "Attached to an active Agent run.".into();
        view.agent_progress = if view.approval_pending {
            "Waiting for your one-time approval".into()
        } else {
            agent_snapshot_activity(&snapshot).unwrap_or_default()
        };
    }
    if let Err(error) = load_history(client, session, view) {
        if error.requires_reconnect() {
            return Err(error);
        }
        view.show_overlay = true;
        view.history_status = format!("History unavailable: {error}");
    }
    view.baseline_message_id = view.last_message_id;
    Ok(())
}
fn select_session(
    client: &mut Client,
    id: &str,
    session: &mut Session,
    view: &mut View,
) -> Result<(), Error> {
    let mut candidate = Session {
        id: id.to_owned(),
        ..Session::default()
    };
    let mut next_view = view.clone();
    next_view.history.clear();
    next_view.last_message_id = 0;
    next_view.baseline_message_id = 0;
    next_view.pending_message_id = 0;
    next_view.last_assistant.clear();
    next_view.live_user.clear();
    next_view.show_overlay = false;
    next_view.response.clear();
    next_view.agent_search_evidence.clear();
    next_view.agent_edit_evidence.clear();
    next_view.agent_glob_evidence.clear();
    attach(client, &mut candidate, &mut next_view)?;
    if session.id != id {
        clear_attachments(
            &mut next_view,
            "Session changed. Refresh the authorized file list before attaching.",
        );
        next_view.workspace_files_root.clear();
        next_view.workspace_files_view.clear();
        next_view.workspace_files_truncated = false;
        next_view.workspace_files_status =
            "Select Workspace to load this session's snapshot.".into();
        next_view.workspace_changes_root.clear();
        next_view.workspace_changes_view.clear();
        next_view.workspace_changes_truncated = false;
        next_view.workspace_changes_status =
            "Select Workspace to check this session's accepted-run baseline.".into();
        clear_memory_selection(
            &mut next_view,
            "Memory belongs to the selected Chat session.",
        );
        clear_agent_history(
            &mut next_view,
            "Agent audit is scoped to the selected Chat session.",
        );
    }
    *session = candidate;
    *view = next_view;
    Ok(())
}

fn clear_memory_selection(view: &mut View, message: &str) {
    view.memory_records.clear();
    view.memory_id.clear();
    view.memory_revision.clear();
    view.memory_next_cursor.clear();
    view.memory_key.clear();
    view.memory_value.clear();
    view.memory_delete_confirmation.clear();
    view.memory_status = message.into();
}

fn clear_agent_history(view: &mut View, message: &str) {
    view.agent_runs.clear();
    view.agent_selected_run.clear();
    view.agent_history_view.clear();
    view.agent_plan_lifecycle.clear();
    view.agent_runs_before_time.clear();
    view.agent_runs_before_id.clear();
    view.agent_runs_has_more = false;
    view.agent_history_status = message.into();
}
fn apply_capabilities(capabilities: &[String], view: &mut View) {
    view.cloud_documents = capabilities
        .iter()
        .any(|capability| capability == "dev-cloud-document-attachments");
    view.cloud_history = capabilities
        .iter()
        .any(|capability| capability == "dev-cloud-history");
    view.cloud_chat = capabilities
        .iter()
        .any(|value| matches!(value.as_str(), "dev-openai-chat" | "dev-cloud-chat"));
    view.provider_endpoints_capable = capabilities
        .iter()
        .any(|value| value == "dev-local-provider-endpoints");
    view.local_provider_catalog = capabilities
        .iter()
        .any(|value| value == "dev-local-provider-catalog");
    if !view.local_provider_catalog {
        view.local_providers.clear();
    }
    view.ollama_helper_advertised = capabilities
        .iter()
        .any(|value| value == "ollama-model-operations");
    let mut formats = vec!["UTF-8 text"];
    if capabilities
        .iter()
        .any(|value| value == "chat-image-jpeg-webp")
    {
        formats.push("PNG, JPEG and static WebP with an image-enabled model");
    } else if capabilities.iter().any(|value| value == "chat-image-png") {
        formats.push("PNG with an image-enabled model");
    }
    if capabilities
        .iter()
        .any(|value| value == "chat-attachments-fodt")
    {
        formats.push("Flat OpenDocument Text (.fodt)");
    }
    if capabilities
        .iter()
        .any(|value| value == "chat-attachments-docx")
    {
        formats.push("Word document (.docx)");
    }
    if capabilities
        .iter()
        .any(|value| value == "chat-attachments-pdf-text")
    {
        formats.push("Text PDF (.pdf)");
    }
    view.attachment_formats = formats.join(", ");
    view.desktop_settings_capable = capabilities.iter().any(|value| value == "desktop-settings");
    view.dev_backup_capable = capabilities
        .iter()
        .any(|value| value == "dev-backup-export");
    view.dev_backup_set_capable = capabilities.iter().any(|value| value == "dev-backup-set");
    view.dev_backup_status = if view.dev_backup_capable {
        "Choose a domain and a new absolute file path.".into()
    } else {
        "Development snapshots are not supported by this daemon.".into()
    };
    view.provider_diagnostics_capable = capabilities
        .iter()
        .any(|value| value == "provider-diagnostics");
    view.runtime_projection_capable = capabilities
        .iter()
        .any(|value| value == "desktop-projection" || value == "desktop-projection-partial");
    view.workspace_runtime_capable = capabilities.iter().any(|value| value == "terminal-state");
    view.workspace_revocation_capable = capabilities
        .iter()
        .any(|value| value == "dev-workspace-revocation");
    view.workspace_access_status = if view.workspace_revocation_capable {
        "Refresh to inspect access for the selected configured workspace.".into()
    } else {
        "Workspace revocation is not advertised by this daemon.".into()
    };
    view.runtime_projection_status = if view.runtime_projection_capable {
        "Runtime status has not been refreshed.".into()
    } else {
        "Runtime projection is not advertised by this daemon.".into()
    };
    view.workspace_runtime_status = if view.workspace_runtime_capable {
        "Workspace status has not been refreshed.".into()
    } else {
        "Workspace runtime details are not advertised by this daemon.".into()
    };
    view.provider_diagnostics_status = if view.provider_diagnostics_capable {
        "Refresh to inspect cached provider configuration.".into()
    } else {
        "Provider diagnostics are not advertised by this daemon.".into()
    };
    if !view.desktop_settings_capable {
        view.theme_status = "Theme preferences are not supported by this daemon.".into();
    }
    view.memory_capable = capabilities.iter().any(|value| value == "session-memory");
    view.workspace_files_capable = capabilities.iter().any(|value| value == "workspace-files");
    if !view.workspace_files_capable {
        clear_attachments(
            view,
            "File attachments are unavailable without workspace-files support.",
        );
    }
    view.workspace_files_root.clear();
    view.workspace_files_view.clear();
    view.workspace_files_truncated = false;
    if !view.workspace_files_capable {
        view.workspace_files_status = "Workspace browsing is not advertised by this daemon.".into();
    } else {
        view.workspace_files_status = "Workspace snapshot not loaded.".into();
    }
    view.workspace_changes_capable = capabilities
        .iter()
        .any(|value| value == "workspace-changes");
    view.workspace_changes_root.clear();
    view.workspace_changes_view.clear();
    view.workspace_changes_truncated = false;
    view.workspace_changes_status = if view.workspace_changes_capable {
        "Comparison not loaded; no conclusion about workspace state.".into()
    } else {
        "Workspace comparison is not advertised by this daemon.".into()
    };
    view.workspace_selection_capable = capabilities
        .iter()
        .any(|value| value == "workspace-selection");
    view.extension_catalog_capable = capabilities.iter().any(|value| value == "plugin-catalog");
    view.extension_catalog_view.clear();
    view.extension_catalog_truncated = false;
    view.extension_catalog_status = if view.extension_catalog_capable {
        "Extension catalog not loaded.".into()
    } else {
        "Extension catalog is unavailable; this daemon has not advertised a configured plugin catalog.".into()
    };
    if view.active_page == "extensions" && !view.extension_catalog_capable {
        view.active_page = "chat".into();
    }
    view.workspace_options.clear();
    view.archived_workspace_ids.clear();
    view.selected_workspace_id.clear();
    view.workspace_selection_status = if view.workspace_selection_capable {
        "Configured workspaces not loaded.".into()
    } else {
        "Workspace selection is not advertised by this daemon.".into()
    };
    if view.active_page == "workspace"
        && !view.workspace_files_capable
        && !view.workspace_changes_capable
        && !view.workspace_selection_capable
    {
        view.active_page = "chat".into();
    }
    view.chat_revisions_capable = capabilities.iter().any(|value| value == "chat-revisions");
    view.session_flags_capable = capabilities.iter().any(|value| value == "session-flags");
    view.session_management_capable = capabilities
        .iter()
        .any(|value| value == "session-management");
    view.ollama_helper_capable = capabilities
        .iter()
        .any(|value| value == "ollama-model-operations");
    if !view.memory_capable {
        view.memory_status = "Session memory is not supported by this daemon.".into();
        view.memory_records.clear();
        view.memory_id.clear();
    }
    view.agent_multi_file = capabilities
        .iter()
        .any(|value| value == "agent-inspect-files");
    view.agent_directory_capable = capabilities
        .iter()
        .any(|value| value == "agent-inspect-directory");
    view.agent_create_capable = capabilities
        .iter()
        .any(|value| value == "agent-create-file");
    view.agent_process_capable = capabilities
        .iter()
        .any(|value| value == "agent-run-process");
    view.agent_classify_capable = capabilities
        .iter()
        .any(|value| value == "agent-classify-inspect");
    view.agent_search_capable = capabilities
        .iter()
        .any(|value| value == "agent-search-literal");
    view.agent_edit_capable = capabilities.iter().any(|value| value == "agent-edit-file");
    view.agent_glob_capable = capabilities.iter().any(|value| value == "agent-glob");
    view.agent_read_plan_capable = capabilities.iter().any(|value| value == "agent-read-plan");
    view.agent_work_plan_capable = capabilities.iter().any(|value| value == "agent-work-plan");
    view.agent_history_capable = capabilities.iter().any(|value| value == "agent-history");
    view.agent_restricted = capabilities
        .iter()
        .any(|value| value == "agent-inspect-file")
        || view.agent_multi_file
        || view.agent_directory_capable
        || view.agent_create_capable;
    view.agent_restricted |= view.agent_process_capable;
    view.agent_restricted |= view.agent_classify_capable;
    view.agent_restricted |= view.agent_search_capable;
    view.agent_restricted |= view.agent_edit_capable;
    view.agent_restricted |= view.agent_glob_capable;
    view.agent_restricted |= view.agent_read_plan_capable;
    view.agent_restricted |= view.agent_work_plan_capable;
    if !view.agent_process_capable {
        view.agent_process_mode = false;
    }
    if !view.agent_classify_capable {
        view.agent_classify_mode = false;
    }
    if !view.agent_search_capable {
        view.agent_search_mode = false;
        view.agent_search_evidence.clear();
    }
    if !view.agent_edit_capable {
        view.agent_edit_mode = false;
        view.agent_edit_evidence.clear();
    }
    if !view.agent_glob_capable {
        view.agent_glob_mode = false;
        view.agent_glob_evidence.clear();
    }
    if !view.agent_read_plan_capable {
        view.agent_read_plan_mode = false;
        view.agent_read_plan_evidence.clear();
    }
    if !view.agent_work_plan_capable {
        view.agent_work_plan_mode = false;
        view.agent_work_plan_evidence.clear();
    }
    if !view.agent_history_capable {
        view.agent_runs.clear();
        view.agent_selected_run.clear();
        view.agent_history_view.clear();
        view.agent_plan_lifecycle.clear();
        view.agent_history_status = "Agent audit history is not advertised by this daemon.".into();
    }
    if !view.agent_directory_capable {
        view.agent_directory_mode = false;
        view.agent_include_hidden = false;
    }
    if !view.agent_create_capable {
        view.agent_create_mode = false;
    }
    view.agent_capable = capabilities.iter().any(|value| value == "agent") || view.agent_restricted;
    view.approval_capable = capabilities.iter().any(|value| value == "approval")
        || view.agent_create_capable
        || view.agent_process_capable
        || view.agent_classify_capable
        || view.agent_search_capable
        || view.agent_edit_capable
        || view.agent_glob_capable;
    view.approval_capable |= view.agent_read_plan_capable;
    view.approval_capable |= view.agent_work_plan_capable;
    view.agent_capable |= view.agent_search_capable;
    view.agent_capable |= view.agent_edit_capable;
    view.agent_capable |= view.agent_glob_capable;
    view.agent_capable |= view.agent_read_plan_capable;
    view.agent_capable |= view.agent_work_plan_capable;
    if !view.agent_capable {
        view.agent_mode = false;
        view.agent_label = "Agent mode".into();
        view.composer_placeholder = "Message".into();
        view.agent_status =
            "Agent mode unavailable: daemon did not advertise agent capability.".into();
    } else if view.agent_restricted {
        view.agent_label = if view.agent_edit_capable {
            "Agent mode · approved exact file edit".into()
        } else if view.agent_glob_capable {
            "Agent mode · approved filename pattern search".into()
        } else if view.agent_classify_capable {
            "Agent mode · approved file classification".into()
        } else if view.agent_search_capable {
            "Agent mode · approved literal search".into()
        } else if view.agent_process_capable {
            "Agent mode · restricted tasks".into()
        } else if view.agent_multi_file || view.agent_directory_capable {
            "Agent mode · read-only inspection".into()
        } else {
            "Agent mode · inspect-file only".into()
        };
        view.composer_placeholder = "Message".into();
        view.agent_status = if view.agent_classify_mode {
            "Classify a goal into up to four read-only file inspections. You approve each file; original goal coverage is unknown.".into()
        } else if view.agent_search_mode {
            "Search for literal text in the authorized workspace. Hidden entries and regex syntax are excluded; every search requires approval.".into()
        } else if view.agent_edit_mode {
            "Exact file edit: verify the complete current text, propose a replacement, and approve the exact change before writing.".into()
        } else if view.agent_glob_mode {
            "Find file names recursively using a case-sensitive basename pattern. Hidden entries and symbolic links are excluded; each search requires approval.".into()
        } else if view.agent_create_mode {
            "Create-file tasks never overwrite an existing file and require per-action approval."
                .into()
        } else if view.agent_multi_file || view.agent_directory_capable {
            "Read-only inspection: list up to 4 relative workspace paths or choose directory listing. Results report only observed file content.".into()
        } else {
            "Read-only inspection: enter one relative workspace path. Results report only observed file content.".into()
        };
        if view.agent_process_capable {
            view.agent_status.push_str(" A separately selected process task uses only wc, head, tail, or grep and requires approval.");
        }
    } else {
        view.agent_label = "Agent mode".into();
        view.composer_placeholder = "Message".into();
        view.agent_status = "Agent mode available.".into();
    }
}

fn connect(path: &Path, session: &mut Session, view: &mut View) -> Result<Client, Error> {
    let mut client = Client::connect(path, "sentinel-slint-prototype")?;
    apply_capabilities(client.capabilities(), view);
    view.stopping = false;
    refresh(&mut client, view)?;
    if let Err(error) = refresh_models(&mut client, view) {
        view.model_status = format!("Models unavailable: {error}");
    }
    if let Err(error) = refresh_theme(&mut client, view) {
        view.theme_status = format!("Theme preference unavailable: {error}");
    }
    if view.ollama_helper_capable {
        if let Err(error) = refresh_ollama_helper(&mut client, view) {
            view.ollama_helper_status = format!("Ollama model operations unavailable: {error}");
        }
    } else {
        view.ollama_helper_status = "Model pull/removal is unavailable for this provider.".into();
    }
    if session.id.is_empty() {
        view.status = "Connected. Choose a session or send a message.".into();
    } else {
        attach(&mut client, session, view)?;
    }
    if view.workspace_selection_capable {
        if let Err(error) = refresh_workspace_state(&mut client, view) {
            view.workspace_selection_status =
                format!("Configured workspace state unavailable: {error}");
        } else if view.active_page == "workspace" {
            if view.workspace_files_capable
                && let Err(error) = refresh_workspace_files(&mut client, session, view)
            {
                view.workspace_files_status = format!("Workspace snapshot unavailable: {error}");
            }
            if view.workspace_changes_capable
                && let Err(error) = refresh_workspace_changes(&mut client, session, view)
            {
                view.workspace_changes_status =
                    format!("Workspace comparison unavailable: {error}");
            }
        }
    }
    if view.active_page == "extensions"
        && view.extension_catalog_capable
        && let Err(error) = refresh_extension_catalog(&mut client, view)
    {
        view.extension_catalog_status = format!("Extension catalog unavailable: {error}");
    }
    view.connected = true;
    Ok(client)
}

fn is_active(state: &Value) -> bool {
    matches!(state.as_str(), Some("running" | "approval"))
}

fn set_agent_mode(view: &mut View, enabled: bool) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    if enabled && !view.agent_capable {
        view.agent_mode = false;
        view.agent_status =
            "Agent mode unavailable: daemon did not advertise agent capability.".into();
        return Err(Error::Remote("unsupported-capability".into()));
    }
    if enabled {
        clear_attachments(
            view,
            "Attachments are Chat-only. Refresh the file list when you return to Chat.",
        );
    }
    view.agent_mode = enabled;
    view.agent_status = if enabled && view.agent_restricted {
        if view.agent_classify_mode {
            "Classify a goal into up to four read-only file inspections. You approve each file; original goal coverage is unknown.".into()
        } else if view.agent_search_mode {
            "Search for literal text in the authorized workspace. Hidden entries and regex syntax are excluded; every search requires approval.".into()
        } else if view.agent_edit_mode {
            "Exact file edit: verify the complete current text, propose a replacement, and approve the exact change before writing.".into()
        } else if view.agent_glob_mode {
            "Find file names recursively using a case-sensitive basename pattern. Hidden entries and symbolic links are excluded; each search requires approval.".into()
        } else if view.agent_process_mode {
            "Choose a restricted inspect task or enable one read-only command; every process invocation requires approval and no shell is used.".into()
        } else if view.agent_create_mode {
            "Create-file tasks never overwrite an existing file and require per-action approval."
                .into()
        } else if view.agent_multi_file || view.agent_directory_capable {
            "Read-only inspection: list up to 4 relative workspace paths or choose directory listing. Results report only observed file content.".into()
        } else {
            "Read-only inspection: enter one relative workspace path. Results report only observed file content.".into()
        }
    } else if enabled {
        "Agent mode enabled for the next run.".into()
    } else if view.agent_capable {
        "Chat mode selected.".into()
    } else {
        "Agent mode unavailable: daemon did not advertise agent capability.".into()
    };
    Ok(())
}

fn set_agent_directory_mode(view: &mut View, enabled: bool) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    if enabled && !view.agent_directory_capable {
        view.agent_directory_mode = false;
        return Err(Error::Remote("unsupported-capability".into()));
    }
    view.agent_directory_mode = enabled;
    if enabled {
        view.agent_read_plan_mode = false;
        view.agent_work_plan_mode = false;
        view.agent_create_mode = false;
        view.agent_process_mode = false;
        view.agent_classify_mode = false;
        view.agent_search_mode = false;
        view.agent_edit_mode = false;
        view.agent_glob_mode = false;
    }
    if !enabled {
        view.agent_include_hidden = false;
    }
    Ok(())
}

fn set_agent_create_mode(view: &mut View, enabled: bool) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    if enabled && !view.agent_create_capable {
        view.agent_create_mode = false;
        return Err(Error::Remote("unsupported-capability".into()));
    }
    view.agent_create_mode = enabled;
    if enabled {
        view.agent_read_plan_mode = false;
        view.agent_work_plan_mode = false;
        view.agent_directory_mode = false;
        view.agent_process_mode = false;
        view.agent_classify_mode = false;
        view.agent_search_mode = false;
        view.agent_edit_mode = false;
        view.agent_glob_mode = false;
        view.agent_include_hidden = false;
        view.agent_status =
            "Create-file tasks never overwrite an existing file and require per-action approval."
                .into();
    } else if view.agent_mode {
        view.agent_status = if view.agent_multi_file || view.agent_directory_capable {
            "Read-only inspection: list up to 4 relative workspace paths or choose directory listing. Results report only observed file content.".into()
        } else {
            "Read-only inspection: enter one relative workspace path. Results report only observed file content.".into()
        };
    }
    Ok(())
}

fn set_agent_include_hidden(view: &mut View, enabled: bool) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    if enabled && (!view.agent_directory_capable || !view.agent_directory_mode) {
        view.agent_include_hidden = false;
        return Err(Error::Remote("invalid-state".into()));
    }
    view.agent_include_hidden = enabled;
    Ok(())
}

fn set_agent_process_mode(view: &mut View, enabled: bool) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    if enabled && !view.agent_process_capable {
        view.agent_process_mode = false;
        return Err(Error::Remote("unsupported-capability".into()));
    }
    view.agent_process_mode = enabled;
    if enabled {
        view.agent_read_plan_mode = false;
        view.agent_work_plan_mode = false;
        view.agent_create_mode = false;
        view.agent_directory_mode = false;
        view.agent_classify_mode = false;
        view.agent_search_mode = false;
        view.agent_edit_mode = false;
        view.agent_glob_mode = false;
        view.agent_status = "Read-only process tasks use a fixed wc/head/tail/grep allowlist, one approved invocation, and no shell.".into();
    }
    Ok(())
}

fn set_agent_classify_mode(view: &mut View, enabled: bool) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    if enabled && !view.agent_classify_capable {
        view.agent_classify_mode = false;
        return Err(Error::Remote("unsupported-capability".into()));
    }
    view.agent_classify_mode = enabled;
    if enabled {
        view.agent_read_plan_mode = false;
        view.agent_work_plan_mode = false;
        view.agent_search_mode = false;
        view.agent_edit_mode = false;
        view.agent_glob_mode = false;
        view.agent_process_mode = false;
        view.agent_create_mode = false;
        view.agent_directory_mode = false;
        view.agent_include_hidden = false;
        view.agent_status = "Classifies only into approved file reads; each selected file needs your approval. Original goal coverage is not assessed.".into();
    }
    Ok(())
}

fn set_agent_search_mode(view: &mut View, enabled: bool) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    if enabled && !view.agent_search_capable {
        view.agent_search_mode = false;
        return Err(Error::Remote("unsupported-capability".into()));
    }
    view.agent_search_mode = enabled;
    if enabled {
        view.agent_read_plan_mode = false;
        view.agent_work_plan_mode = false;
        view.agent_create_mode = false;
        view.agent_edit_mode = false;
        view.agent_glob_mode = false;
        view.agent_directory_mode = false;
        view.agent_process_mode = false;
        view.agent_classify_mode = false;
        view.agent_include_hidden = false;
        clear_attachments(
            view,
            "Agent search is read-only and uses the authorized workspace.",
        );
        view.agent_search_evidence.clear();
        view.agent_status = "Literal text search only; hidden files and regex syntax are excluded. You approve each search.".into();
    }
    Ok(())
}

fn set_agent_edit_mode(view: &mut View, enabled: bool) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    if enabled && !view.agent_edit_capable {
        view.agent_edit_mode = false;
        view.agent_glob_mode = false;
        return Err(Error::Remote("unsupported-capability".into()));
    }
    view.agent_edit_mode = enabled;
    if enabled {
        view.agent_read_plan_mode = false;
        view.agent_work_plan_mode = false;
        view.agent_search_mode = false;
        view.agent_glob_mode = false;
        view.agent_create_mode = false;
        view.agent_directory_mode = false;
        view.agent_process_mode = false;
        view.agent_classify_mode = false;
        view.agent_include_hidden = false;
        clear_attachments(
            view,
            "Exact edits use only the authorized workspace and require approval.",
        );
        view.agent_edit_evidence.clear();
        view.agent_status = "Exact edit only: existing full text must match, proposed full text is bounded, and the specific replacement requires approval.".into();
    }
    Ok(())
}

fn set_agent_glob_mode(view: &mut View, enabled: bool) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    if enabled && !view.agent_glob_capable {
        view.agent_glob_mode = false;
        return Err(Error::Remote("unsupported-capability".into()));
    }
    view.agent_glob_mode = enabled;
    if enabled {
        view.agent_read_plan_mode = false;
        view.agent_work_plan_mode = false;
        view.agent_search_mode = false;
        view.agent_edit_mode = false;
        view.agent_create_mode = false;
        view.agent_directory_mode = false;
        view.agent_process_mode = false;
        view.agent_classify_mode = false;
        view.agent_include_hidden = false;
        clear_attachments(
            view,
            "Filename search uses only the authorized workspace and requires approval.",
        );
        view.agent_glob_evidence.clear();
        view.agent_status = "Recursive filename search uses case-sensitive basename globs with * and ?. Hidden entries and symbolic links are excluded; each search requires approval.".into();
    }
    Ok(())
}

fn set_agent_read_plan_mode(view: &mut View, enabled: bool) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    if enabled && !view.agent_read_plan_capable {
        view.agent_read_plan_mode = false;
        return Err(Error::Remote("unsupported-capability".into()));
    }
    view.agent_read_plan_mode = enabled;
    if enabled {
        view.agent_work_plan_mode = false;
        view.agent_search_mode = false;
        view.agent_edit_mode = false;
        view.agent_glob_mode = false;
        view.agent_process_mode = false;
        view.agent_classify_mode = false;
        view.agent_create_mode = false;
        view.agent_directory_mode = false;
        view.agent_include_hidden = false;
        clear_attachments(
            view,
            "Read plans use only bounded read-only tasks in the authorized workspace.",
        );
        view.agent_read_plan_evidence.clear();
        view.agent_status = "Read plan is limited to four explicit read-only steps. Each step requires its own approval; dependencies refer only to earlier steps.".into();
    }
    Ok(())
}

fn set_agent_work_plan_mode(view: &mut View, enabled: bool) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    if enabled && !view.agent_work_plan_capable {
        view.agent_work_plan_mode = false;
        return Err(Error::Remote("unsupported-capability".into()));
    }
    view.agent_work_plan_mode = enabled;
    if enabled {
        view.agent_read_plan_mode = false;
        view.agent_search_mode = false;
        view.agent_edit_mode = false;
        view.agent_glob_mode = false;
        view.agent_process_mode = false;
        view.agent_classify_mode = false;
        view.agent_create_mode = false;
        view.agent_directory_mode = false;
        view.agent_include_hidden = false;
        clear_attachments(
            view,
            "Work plans are limited to typed tasks in the authorized workspace.",
        );
        view.agent_work_plan_evidence.clear();
        view.agent_status = "Work plan allows only typed file reads and exact create/edit tasks. Every write needs its own exact approval and read-back verification; unknown outcomes stop the plan.".into();
    }
    Ok(())
}

fn literal_search_request(path: &str, pattern: &str) -> Result<String, Error> {
    let path = path.trim();
    if path.is_empty()
        || path.len() > 1024
        || path.contains('\0')
        || path.chars().any(char::is_control)
        || (path != "."
            && (path.starts_with('/')
                || path.split('/').any(|part| {
                    part.is_empty() || part == "." || part == ".." || part.starts_with('.')
                })))
    {
        return Err(Error::Usage(
            "Choose a workspace-relative search folder (or .), without parent traversal.".into(),
        ));
    }
    if pattern.trim().is_empty()
        || pattern.len() > 256
        || pattern
            .chars()
            .any(|character| character.is_control() || ".*+?[]{}()|^$\\\\".contains(character))
    {
        return Err(Error::Usage(
            "Search text must be 1–256 UTF-8 bytes of literal text without regex syntax or control characters.".into(),
        ));
    }
    serde_json::to_string(&serde_json::json!({
        "operation":"search-literal",
        "path":path,
        "pattern":pattern,
    }))
    .map_err(|error| Error::Protocol(error.to_string()))
}

fn exact_edit_request(path: &str, expected: &str, proposed: &str) -> Result<String, Error> {
    let path = path.trim();
    if path.is_empty()
        || path.len() > 1024
        || path.contains('\0')
        || path.chars().any(char::is_control)
        || path.starts_with('/')
        || path
            .split('/')
            .any(|part| part.is_empty() || part == "." || part == "..")
    {
        return Err(Error::Usage(
            "Choose one file path relative to the authorized workspace.".into(),
        ));
    }
    if expected.is_empty()
        || expected.len() > 2048
        || proposed.len() > 2048
        || expected.contains('\0')
        || proposed.contains('\0')
    {
        return Err(Error::Usage(
            "Expected current text must be nonempty; expected and proposed text must each fit within 2,048 UTF-8 bytes and contain no NUL.".into(),
        ));
    }
    let task = serde_json::json!({
        "operation":"edit-file",
        "path":path,
        "oldString":expected,
        "newString":proposed,
    });
    if serde_json::to_vec(&task)
        .map_err(|error| Error::Protocol(error.to_string()))?
        .len()
        > 3072
    {
        return Err(Error::Usage(
            "This exact edit is too large for the supported request frame.".into(),
        ));
    }
    serde_json::to_string(&task).map_err(|error| Error::Protocol(error.to_string()))
}

fn glob_search_request(path: &str, pattern: &str) -> Result<String, Error> {
    let path = path.trim();
    if path.is_empty()
        || path.len() > 1024
        || path.contains('\0')
        || path.chars().any(char::is_control)
        || (path != "."
            && (path.starts_with('/')
                || path.split('/').any(|part| {
                    part.is_empty() || part == "." || part == ".." || part.starts_with('.')
                })))
    {
        return Err(Error::Usage(
            "Choose a workspace-relative folder (or .), without parent traversal.".into(),
        ));
    }
    if pattern.trim().is_empty()
        || pattern.len() > 128
        || !pattern.is_ascii()
        || pattern
            .bytes()
            .any(|byte| !(byte.is_ascii_graphic() || byte == b' ') || b"/\\[]{}".contains(&byte))
    {
        return Err(Error::Usage(
            "Filename pattern must be 1–128 printable ASCII bytes; only * and ? are wildcards, with no path separators or bracket syntax.".into(),
        ));
    }
    let task = serde_json::json!({
        "operation":"glob",
        "path":path,
        "pattern":pattern,
    });
    if serde_json::to_vec(&task)
        .map_err(|error| Error::Protocol(error.to_string()))?
        .len()
        > 3072
    {
        return Err(Error::Usage("Filename search request is too large.".into()));
    }
    serde_json::to_string(&task).map_err(|error| Error::Protocol(error.to_string()))
}

fn verified_literal_search_evidence(text: &str, path: &str, pattern: &str) -> Option<String> {
    let marker =
        format!("Literal search: {path}\nLiteral search results (hidden entries excluded): ");
    let payload = text.strip_prefix(&marker)?;
    let value: Value = serde_json::from_str(payload).ok()?;
    let matches = value["matches"].as_array()?;
    if value["scope"].as_str() != Some(path)
        || value["query"].as_str() != Some(pattern)
        || value["complete"] != true
        || value["truncated"] != false
        || value["includeHidden"] != false
        || value["followSymlinks"] != false
        || value["matchCount"].as_u64() != Some(matches.len() as u64)
        || matches.iter().any(|item| {
            item["path"].as_str().is_none_or(str::is_empty)
                || item["line"].as_u64().is_none_or(|line| line == 0)
                || item["text"].as_str().is_none()
        })
    {
        return None;
    }
    Some(text.to_owned())
}

fn verified_glob_evidence(text: &str, path: &str, pattern: &str) -> Option<String> {
    let marker = format!("Glob: {path}\nGlob results (hidden entries excluded): ");
    let payload = text.strip_prefix(&marker)?;
    let value: Value = serde_json::from_str(payload).ok()?;
    let matches = value["matches"].as_array()?;
    if value["scope"].as_str() != Some(path)
        || value["root"].as_str().is_some_and(|root| root != path)
        || value["pattern"].as_str() != Some(pattern)
        || value["complete"] != true
        || value["truncated"] != false
        || value["includeHidden"] != false
        || value["followSymlinks"] != false
        || matches.iter().any(|item| {
            item.as_str().is_none_or(|found| {
                found.is_empty() || found.starts_with('.') || found.contains("/.")
            })
        })
    {
        return None;
    }
    Some(text.to_owned())
}

fn classified_inspect_request(goal: &str) -> Result<String, Error> {
    if goal.trim().is_empty() || goal.len() > 2048 || goal.contains('\0') {
        return Err(Error::Usage(
            "Enter a goal of 1–2,048 UTF-8 bytes without NUL.".into(),
        ));
    }
    serde_json::to_string(&serde_json::json!({
        "operation": "classify-inspect",
        "goal": goal
    }))
    .map_err(|error| Error::Protocol(error.to_string()))
}

fn set_agent_process_program(view: &mut View, index: i32) -> Result<(), Error> {
    if view.running || !view.agent_process_capable {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    view.agent_process_program = match index {
        0 => "/usr/bin/wc",
        1 => "/usr/bin/head",
        2 => "/usr/bin/tail",
        3 => "/usr/bin/grep",
        _ => return Err(Error::Usage("Choose wc, head, tail, or grep.".into())),
    }
    .into();
    Ok(())
}

fn restricted_agent_request(
    paths_text: &str,
    multi_file: bool,
    directory_mode: bool,
    create_mode: bool,
    include_hidden: bool,
    content: &str,
) -> Result<String, Error> {
    let paths: Vec<&str> = paths_text.lines().collect();
    let max_paths = if multi_file && !directory_mode && !create_mode {
        4
    } else {
        1
    };
    if paths.is_empty()
        || paths.len() > max_paths
        || paths.iter().any(|path| path.trim().is_empty())
    {
        return Err(Error::Usage(
            if multi_file && !directory_mode && !create_mode {
                "Enter 1–4 relative workspace paths, one per line.".into()
            } else {
                "Enter one relative workspace path.".into()
            },
        ));
    }
    let mut unique = std::collections::HashSet::new();
    for path in &paths {
        if path.len() > 1024
            || path.contains('\0')
            || Path::new(path).is_absolute()
            || Path::new(path)
                .components()
                .any(|component| component == std::path::Component::ParentDir)
            || !unique.insert(*path)
        {
            return Err(Error::Usage(
                "Paths must be unique relative workspace paths (up to 1,024 UTF-8 bytes each) without parent traversal or NUL.".into(),
            ));
        }
    }
    if create_mode && (content.len() > 2048 || content.contains('\0')) {
        return Err(Error::Usage(
            "New file content is limited to 2,048 UTF-8 bytes and may not contain NUL.".into(),
        ));
    }
    let value = if create_mode {
        serde_json::json!({"operation":"create-file","path":paths[0],"content":content})
    } else if directory_mode {
        serde_json::json!({"operation":"inspect-directory","path":paths[0],"includeHidden":include_hidden})
    } else if multi_file {
        serde_json::json!({"operation":"inspect-files","paths":paths})
    } else {
        serde_json::json!({"operation":"inspect-file","path":paths[0]})
    };
    serde_json::to_string(&value).map_err(|error| Error::Protocol(error.to_string()))
}

fn read_plan_request(steps_text: &str, work_plan: bool) -> Result<String, Error> {
    if steps_text.len() > 8 * 1024 {
        return Err(Error::Usage(
            "A read plan is limited to 8 KiB of JSON.".into(),
        ));
    }
    let steps: Value = serde_json::from_str(steps_text)
        .map_err(|_| Error::Usage("Enter a JSON array of read-only plan steps.".into()))?;
    let steps = steps
        .as_array()
        .filter(|steps| (1..=4).contains(&steps.len()))
        .ok_or_else(|| Error::Usage("A read plan needs one to four steps.".into()))?;
    let mut normalized = Vec::with_capacity(steps.len());
    let mut seen = std::collections::HashSet::new();
    for (index, step) in steps.iter().enumerate() {
        let object = step
            .as_object()
            .filter(|object| object.len() == 2)
            .ok_or_else(|| {
                Error::Usage(format!(
                    "Step {} must contain task and dependsOn.",
                    index + 1
                ))
            })?;
        let task = object
            .get("task")
            .and_then(Value::as_object)
            .ok_or_else(|| Error::Usage(format!("Step {} has no task object.", index + 1)))?;
        let operation = task
            .get("operation")
            .and_then(Value::as_str)
            .ok_or_else(|| {
                Error::Usage(format!("Step {} needs a supported operation.", index + 1))
            })?;
        let task_text = match operation {
            "inspect-file" if task.len() == 2 => restricted_agent_request(
                task.get("path").and_then(Value::as_str).unwrap_or_default(),
                false,
                false,
                false,
                false,
                "",
            )?,
            "inspect-files" if task.len() == 2 => {
                let paths = task
                    .get("paths")
                    .and_then(Value::as_array)
                    .filter(|paths| (1..=4).contains(&paths.len()))
                    .ok_or_else(|| {
                        Error::Usage(format!("Step {} needs one to four paths.", index + 1))
                    })?;
                let path_text = paths
                    .iter()
                    .map(|path| path.as_str().unwrap_or_default())
                    .collect::<Vec<_>>()
                    .join("\n");
                restricted_agent_request(&path_text, true, false, false, false, "")?
            }
            "inspect-directory" if task.len() == 3 => {
                let hidden = task
                    .get("includeHidden")
                    .and_then(Value::as_bool)
                    .ok_or_else(|| {
                        Error::Usage(format!(
                            "Step {} must set includeHidden to true or false.",
                            index + 1
                        ))
                    })?;
                restricted_agent_request(
                    task.get("path").and_then(Value::as_str).unwrap_or_default(),
                    false,
                    true,
                    false,
                    hidden,
                    "",
                )?
            }
            "stat-path" if task.len() == 2 => {
                let path = task.get("path").and_then(Value::as_str).unwrap_or_default();
                let path = path.trim();
                if path.is_empty()
                    || path.len() > 1024
                    || path.contains('\0')
                    || path.chars().any(char::is_control)
                    || (path != "."
                        && (path.starts_with('/')
                            || path
                                .split('/')
                                .any(|part| part.is_empty() || part == "." || part == "..")))
                {
                    return Err(Error::Usage(format!(
                        "Step {} needs a workspace-relative path (or .).",
                        index + 1
                    )));
                }
                serde_json::to_string(&serde_json::json!({"operation":"stat-path","path":path}))
                    .map_err(|error| Error::Protocol(error.to_string()))?
            }
            "create-file" if work_plan && task.len() == 3 => restricted_agent_request(
                task.get("path").and_then(Value::as_str).unwrap_or_default(),
                false,
                false,
                true,
                false,
                task.get("content")
                    .and_then(Value::as_str)
                    .unwrap_or_default(),
            )?,
            "edit-file" if work_plan && task.len() == 4 => exact_edit_request(
                task.get("path").and_then(Value::as_str).unwrap_or_default(),
                task.get("oldString")
                    .and_then(Value::as_str)
                    .unwrap_or_default(),
                task.get("newString")
                    .and_then(Value::as_str)
                    .unwrap_or_default(),
            )?,
            "search-literal" if task.len() == 3 => literal_search_request(
                task.get("path").and_then(Value::as_str).unwrap_or_default(),
                task.get("pattern")
                    .and_then(Value::as_str)
                    .unwrap_or_default(),
            )?,
            "glob" if task.len() == 3 => glob_search_request(
                task.get("path").and_then(Value::as_str).unwrap_or_default(),
                task.get("pattern")
                    .and_then(Value::as_str)
                    .unwrap_or_default(),
            )?,
            _ => {
                return Err(Error::Usage(format!(
                    "Step {} uses an unsupported or malformed read-only operation.",
                    index + 1
                )));
            }
        };
        let task_value: Value =
            serde_json::from_str(&task_text).map_err(|error| Error::Protocol(error.to_string()))?;
        let encoded = serde_json::to_string(&task_value)
            .map_err(|error| Error::Protocol(error.to_string()))?;
        if !seen.insert(encoded) {
            return Err(Error::Usage("Read plan steps must be unique.".into()));
        }
        let dependencies = object
            .get("dependsOn")
            .and_then(Value::as_array)
            .ok_or_else(|| Error::Usage(format!("Step {} needs a dependsOn array.", index + 1)))?;
        if dependencies.len() > index {
            return Err(Error::Usage(format!(
                "Step {} may depend only on earlier steps.",
                index + 1
            )));
        }
        let mut deps = Vec::with_capacity(dependencies.len());
        for dependency in dependencies {
            let dependency = dependency
                .as_u64()
                .and_then(|value| usize::try_from(value).ok())
                .filter(|dependency| *dependency < index)
                .ok_or_else(|| {
                    Error::Usage(format!(
                        "Step {} dependencies must be zero-based indexes of earlier steps.",
                        index + 1
                    ))
                })?;
            if deps.contains(&dependency) {
                return Err(Error::Usage(format!(
                    "Step {} lists a dependency more than once.",
                    index + 1
                )));
            }
            deps.push(dependency);
        }
        normalized.push(serde_json::json!({"task":task_value,"dependsOn":deps}));
    }
    serde_json::to_string(&serde_json::json!({
        "operation":if work_plan {"work-plan"} else {"read-plan"},
        "steps":normalized
    }))
    .map_err(|error| Error::Protocol(error.to_string()))
}

fn verified_read_plan_evidence(output: &str, expected_steps: usize) -> Option<String> {
    if !(1..=4).contains(&expected_steps) || !output.starts_with("Step 1:\n") {
        return None;
    }
    for step in 2..=expected_steps {
        if !output.contains(&format!("\n\nStep {step}:\n")) {
            return None;
        }
    }
    Some(output.to_owned())
}

fn restricted_process_request(program: &str, args_text: &str) -> Result<String, Error> {
    let executable = match program {
        "/usr/bin/wc" | "/usr/bin/head" | "/usr/bin/tail" | "/usr/bin/grep" => program,
        _ => {
            return Err(Error::Usage(
                "Choose an advertised read-only command.".into(),
            ));
        }
    };
    let args: Vec<&str> = args_text.lines().collect();
    if args.is_empty()
        || args.len() > 15
        || args.iter().any(|arg| arg.is_empty() || arg.contains('\0'))
    {
        return Err(Error::Usage(
            "Enter 1–15 arguments, one per line, without empty lines or NUL.".into(),
        ));
    }
    let mut argv = Vec::with_capacity(args.len() + 1);
    argv.push(executable);
    argv.extend(args);
    let value = serde_json::json!({"operation":"run-process","argv":argv});
    let text = serde_json::to_string(&value).map_err(|error| Error::Protocol(error.to_string()))?;
    if text.len() > 3072 {
        return Err(Error::Usage(
            "Command arguments exceed the 3,072-byte limit.".into(),
        ));
    }
    Ok(text)
}

fn refresh_agent_history(client: &mut Client, view: &mut View, append: bool) -> Result<(), Error> {
    if !view.agent_history_capable {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    let before_time = if append {
        view.agent_runs_before_time.clone()
    } else {
        String::new()
    };
    let before_id = if append {
        view.agent_runs_before_id.clone()
    } else {
        String::new()
    };
    if append && (before_time.is_empty() || before_id.is_empty()) {
        return Ok(());
    }
    let response = request(
        client,
        RequestPayload::AgentHistory {
            run_id: String::new(),
            before_time,
            before_id,
        },
    )?;
    let history = response.get("history").unwrap_or(&response);
    let Some(rows) = history["runs"].as_array() else {
        return Err(Error::Protocol(
            "agent history response has no run list".into(),
        ));
    };
    let mut runs = if append {
        view.agent_runs.clone()
    } else {
        Vec::new()
    };
    for row in rows.iter().take(26) {
        let (Some(id), Some(cursor)) = (row["runId"].as_str(), row["cursorTime"].as_str()) else {
            continue;
        };
        if id.is_empty() || runs.iter().any(|(known, _, _)| known == id) {
            continue;
        }
        let state = row["state"].as_str().unwrap_or("unknown");
        let goal = row["goalSummary"].as_str().unwrap_or("Agent run");
        let label = format!(
            "{state} · {} · {id}",
            goal.chars().take(48).collect::<String>()
        );
        runs.push((id.into(), label, cursor.into()));
    }
    if let Some(last) = rows.last() {
        view.agent_runs_before_time = last["cursorTime"].as_str().unwrap_or_default().into();
        view.agent_runs_before_id = last["runId"].as_str().unwrap_or_default().into();
    }
    view.agent_runs_has_more = rows.len() >= 26;
    view.agent_runs = runs;
    if view.agent_selected_run.is_empty()
        || !view
            .agent_runs
            .iter()
            .any(|(id, _, _)| id == &view.agent_selected_run)
    {
        view.agent_selected_run = view
            .agent_runs
            .first()
            .map(|(id, _, _)| id.clone())
            .unwrap_or_default();
    }
    view.agent_history_status = if view.agent_runs.is_empty() {
        "No Agent runs are attached to this session.".into()
    } else {
        format!("Loaded {} Agent run records.", view.agent_runs.len())
    };
    if !view.agent_selected_run.is_empty()
        && let Some(index) = view
            .agent_runs
            .iter()
            .position(|(id, _, _)| id == &view.agent_selected_run)
    {
        load_agent_run_history(client, view, index)?;
    }
    Ok(())
}

fn render_agent_plan_lifecycle(history: &Value) -> String {
    let run_state = history["run"]["state"].as_str().unwrap_or("unknown");
    let Some(rows) = history["steps"].as_array() else {
        return "Plan-step lifecycle is unavailable in this history response; no success is inferred.".into();
    };
    if rows.is_empty() {
        return "No persisted plan-step records are available. This run may predate plan lifecycle tracking; step outcomes are unknown.".into();
    }
    if rows.len() > 4 {
        return "Plan-step lifecycle exceeded the supported bound; no success is inferred.".into();
    }
    let mut rendered =
        String::from("Persisted plan steps (outcomes are shown only as recorded):\n");
    let mut seen = std::collections::HashSet::new();
    for row in rows {
        let sequence = row["sequence"]
            .as_u64()
            .and_then(|value| usize::try_from(value).ok());
        let Some(sequence) = sequence.filter(|value| (1..=4).contains(value)) else {
            rendered.push_str("• Step number unavailable · Unknown (no success inferred)\n");
            continue;
        };
        if !seen.insert(sequence) {
            rendered.push_str("• Duplicate step record · lifecycle is incomplete\n");
            continue;
        }
        let operation = row["operation"]
            .as_str()
            .map(|value| catalog_text(Some(value), 64))
            .filter(|value| !value.is_empty())
            .unwrap_or_else(|| "unknown operation".into());
        let raw_status = row["status"].as_str().unwrap_or("unknown");
        let status = match raw_status {
            "succeeded" => "Succeeded",
            "failed" => "Failed",
            "blocked" => "Blocked",
            "skipped" => "Skipped",
            "pending" if run_state == "running" => "Pending",
            "pending" => "Skipped or unfinished (no terminal result recorded)",
            "started" if run_state == "running" => "In progress (no terminal result yet)",
            "started" => "Interrupted (outcome unknown; success not inferred)",
            "interrupted" => "Interrupted (outcome unknown; success not inferred)",
            _ => "Unknown (no success inferred)",
        };
        let dependencies = row["dependsOn"]
            .as_array()
            .map(|values| {
                let deps = values
                    .iter()
                    .filter_map(|value| {
                        value
                            .as_u64()
                            .and_then(|index| usize::try_from(index).ok())
                            .filter(|index| *index < sequence.saturating_sub(1))
                            .map(|index| format!("Step {}", index + 1))
                    })
                    .take(4)
                    .collect::<Vec<_>>();
                if deps.is_empty() {
                    "none".into()
                } else {
                    deps.join(", ")
                }
            })
            .unwrap_or_else(|| "unknown".into());
        let reason = match row["reason"].as_str().unwrap_or_default() {
            "" => String::new(),
            "daemon-restarted" => " · daemon restarted".into(),
            "run-ended" => " · run ended before this step completed".into(),
            reason => format!(" · {}", catalog_text(Some(reason), 96)),
        };
        rendered.push_str(&format!(
            "• Step {sequence} · {operation} · {status} · depends on {dependencies}{reason}\n"
        ));
        if rendered.len() >= 4 * 1024 {
            rendered.truncate(rendered.floor_char_boundary(4 * 1024));
            rendered.push_str("\nStep summary truncated.");
            break;
        }
    }
    if let Some(maximum) = seen.iter().max().copied() {
        for missing in 1..=maximum {
            if !seen.contains(&missing) {
                rendered.push_str(&format!(
                    "• Step {missing} · Unknown (record missing; no success inferred)\n"
                ));
            }
        }
    }
    if rows.iter().any(|row| {
        !matches!(
            row["status"].as_str(),
            Some(
                "succeeded"
                    | "failed"
                    | "blocked"
                    | "skipped"
                    | "pending"
                    | "started"
                    | "interrupted"
            )
        )
    }) {
        rendered.push_str(
            "Unknown lifecycle values were preserved as unknown; no success is inferred.\n",
        );
    }
    rendered
}

fn load_agent_run_history(client: &mut Client, view: &mut View, index: usize) -> Result<(), Error> {
    let Some((run_id, _, _)) = view.agent_runs.get(index) else {
        return Err(Error::Usage("Select an Agent run first.".into()));
    };
    let run_id = run_id.clone();
    view.agent_plan_lifecycle =
        "Loading persisted plan steps; missing or legacy records will remain unknown.".into();
    let response = request(
        client,
        RequestPayload::AgentHistory {
            run_id: run_id.clone(),
            before_time: String::new(),
            before_id: String::new(),
        },
    )?;
    let history = response.get("history").unwrap_or(&response);
    if history["run"]["runId"].as_str() != Some(&run_id) {
        return Err(Error::Protocol(
            "Agent audit response belongs to another run".into(),
        ));
    }
    let mut rendered = serde_json::to_string_pretty(history)
        .map_err(|error| Error::Protocol(error.to_string()))?;
    if rendered.len() > 32_768 {
        rendered.truncate(rendered.floor_char_boundary(32_768));
        rendered.push_str("\n… audit preview truncated by the desktop client …");
    }
    view.agent_selected_run = run_id;
    view.agent_plan_lifecycle = render_agent_plan_lifecycle(history);
    view.agent_history_view = rendered;
    view.agent_history_status = if history["truncated"] == true {
        "Audit records are bounded; the daemon marked this view truncated.".into()
    } else {
        "Loaded persisted Agent audit: run, tools, approvals, and evidence.".into()
    };
    Ok(())
}

fn approval_summary(payload: &Value) -> String {
    let tool = payload["tool"].as_str().unwrap_or("tool");
    let detail = payload["detail"].as_str().unwrap_or("Approval required.");
    let risk = payload["risk"].as_u64().unwrap_or_default();
    let resources = payload["resources"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|resource| resource["resource"].as_str())
        .take(4)
        .map(|resource| resource.chars().take(180).collect::<String>())
        .collect::<Vec<_>>();
    if resources.is_empty() {
        format!(
            "Approval required: {tool} · risk {risk} — {}",
            detail.chars().take(256).collect::<String>()
        )
    } else {
        format!(
            "Approval required: {tool} · risk {risk} — {}\nResources: {}",
            detail.chars().take(256).collect::<String>(),
            resources.join(", ")
        )
    }
}

fn append_agent_activity(view: &mut View, entry: &str) {
    const ACTIVITY_LIMIT: usize = 12 * 1024;
    if view.agent_activity.len() >= ACTIVITY_LIMIT {
        return;
    }
    let available = ACTIVITY_LIMIT - view.agent_activity.len();
    let end = entry.floor_char_boundary(available.min(entry.len()));
    view.agent_activity.push_str(&entry[..end]);
    if !view.agent_activity.ends_with('\n') {
        view.agent_activity.push('\n');
    }
}

fn transcript(view: &View) -> String {
    let mut overlay = String::new();
    if view.show_overlay {
        if !view.live_user.is_empty() {
            let end = view
                .live_user
                .floor_char_boundary(8192.min(view.live_user.len()));
            overlay.push_str("user: ");
            overlay.push_str(&view.live_user[..end]);
            if end < view.live_user.len() {
                overlay.push_str(" [truncated]");
            }
            overlay.push_str("\n\n");
        }
        if !view.response.is_empty() {
            let end = view
                .response
                .floor_char_boundary(32768.min(view.response.len()));
            overlay.push_str("assistant: ");
            overlay.push_str(&view.response[..end]);
            if end < view.response.len() || view.truncated {
                overlay.push_str(" [truncated]");
            }
            overlay.push_str("\n\n");
        }
    }
    let remaining = RESPONSE_LIMIT - overlay.len();
    let notice = "[Earlier history omitted]\n\n";
    let omitted = view.history.len() > remaining;
    let available = remaining - if omitted { notice.len() } else { 0 };
    let mut start = view.history.len().saturating_sub(available);
    while !view.history.is_char_boundary(start) {
        start += 1;
    }
    let mut result = String::new();
    if omitted {
        result.push_str(notice);
    }
    result.push_str(&view.history[start..]);
    result.push_str(&overlay);
    result
}
fn replace_response(view: &mut View, text: &str) {
    view.response.clear();
    view.truncated = false;
    append_response(view, text);
}

fn append_response(view: &mut View, text: &str) {
    if view.truncated {
        return;
    }
    let available = RESPONSE_LIMIT - view.response.len();
    let prefix_len = text.floor_char_boundary(available.min(text.len()));
    view.response.push_str(&text[..prefix_len]);
    view.truncated = prefix_len != text.len();
}

fn send(
    client: &mut Client,
    session: &mut Session,
    view: &mut View,
    mut text: String,
) -> Result<(), Error> {
    if text.trim().is_empty() {
        return Err(Error::Usage("Enter a message.".into()));
    }
    if view.agent_mode && !view.agent_capable {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    let selected_attachments = if view.agent_mode {
        if !view.attachments.is_empty() {
            return Err(Error::Usage(
                "Attachments are available in Chat only.".into(),
            ));
        }
        Vec::new()
    } else {
        if !view.attachments.is_empty() {
            if view.attachments.len() > 4
                || view
                    .attachments
                    .iter()
                    .any(|path| !view.attachment_files.contains(path))
                || view.attachment_files_session_id != session.id
                || view.attachment_files_root.is_empty()
            {
                return Err(Error::Usage(
                    "Refresh the authorized workspace file list before attaching.".into(),
                ));
            }
            let state = request(client, RequestPayload::TerminalState {})?;
            let workspace = &state["workspace"];
            let root = workspace["root"].as_str().unwrap_or_default();
            let id = workspace["id"].as_str().unwrap_or_default();
            if root != view.attachment_files_root
                || id != view.attachment_files_workspace_id
                || (view.workspace_selection_capable
                    && !view.selected_workspace_id.is_empty()
                    && id != view.selected_workspace_id)
            {
                clear_attachments(
                    view,
                    "Workspace changed. Refresh its file list before attaching.",
                );
                return Err(Error::Usage(
                    "Workspace changed; attachment selection was cleared.".into(),
                ));
            }
        }
        view.attachments
            .iter()
            .cloned()
            .map(Value::String)
            .collect()
    };
    let mut display_text = text.clone();
    let mut read_plan_step_count = 0usize;
    if view.agent_mode && view.agent_restricted {
        text = if view.agent_read_plan_mode || view.agent_work_plan_mode {
            if view.agent_read_plan_mode && !view.agent_read_plan_capable {
                return Err(Error::Remote("unsupported-capability".into()));
            }
            if view.agent_work_plan_mode && !view.agent_work_plan_capable {
                return Err(Error::Remote("unsupported-capability".into()));
            }
            let source_steps = if view.agent_work_plan_mode {
                &view.agent_work_plan_steps
            } else {
                &view.agent_read_plan_steps
            };
            let steps: Value = serde_json::from_str(source_steps)
                .map_err(|_| Error::Usage("Enter a JSON array of typed plan steps.".into()))?;
            read_plan_step_count = steps.as_array().map_or(0, Vec::len);
            display_text = format!(
                "{} · {read_plan_step_count} step(s)",
                if view.agent_work_plan_mode {
                    "Explicit workspace work plan"
                } else {
                    "Explicit read-only plan"
                }
            );
            read_plan_request(source_steps, view.agent_work_plan_mode)?
        } else if view.agent_search_mode {
            if !view.agent_search_capable {
                return Err(Error::Remote("unsupported-capability".into()));
            }
            literal_search_request(&view.agent_search_path, &view.agent_search_pattern)?
        } else if view.agent_edit_mode {
            if !view.agent_edit_capable {
                return Err(Error::Remote("unsupported-capability".into()));
            }
            exact_edit_request(
                &view.agent_edit_path,
                &view.agent_edit_expected,
                &view.agent_edit_proposed,
            )?
        } else if view.agent_glob_mode {
            if !view.agent_glob_capable {
                return Err(Error::Remote("unsupported-capability".into()));
            }
            glob_search_request(&view.agent_glob_path, &view.agent_glob_pattern)?
        } else if view.agent_classify_mode {
            if !view.agent_classify_capable {
                return Err(Error::Remote("unsupported-capability".into()));
            }
            classified_inspect_request(&text)?
        } else if view.agent_process_mode {
            if !view.agent_process_capable {
                return Err(Error::Remote("unsupported-capability".into()));
            }
            restricted_process_request(&view.agent_process_program, &text)?
        } else {
            restricted_agent_request(
                &text,
                view.agent_multi_file,
                view.agent_directory_mode,
                view.agent_create_mode,
                view.agent_include_hidden,
                &view.agent_content,
            )?
        };
    }
    // The actual v1 field is limited to 65,536 UTF-16 units. Keep the JSON frame bound as well.
    if text.encode_utf16().count() > 65_536
        || serde_json::to_vec(&text)
            .map_err(|e| Error::Protocol(e.to_string()))?
            .len()
            > sentinel_ipc::contract::MAX_FRAME_BYTES - 1024
    {
        return Err(Error::Usage("This message is too long.".into()));
    }
    if session.id.is_empty() {
        let response = request(
            client,
            RequestPayload::SessionCreate {
                title: "Quick Panel prototype".into(),
            },
        )?;
        session.id = response["session_id"]
            .as_str()
            .filter(|id| !id.is_empty())
            .ok_or_else(|| Error::Protocol("missing session ID".into()))?
            .into();
        session.generation = response["server_generation"].as_str().map(str::to_owned);
    }
    let command = if view.agent_mode {
        view.agent_activity.clear();
        view.agent_read_plan_expected_steps = read_plan_step_count;
        view.agent_read_plan_evidence.clear();
        view.agent_work_plan_evidence.clear();
        view.agent_work_plan_evidence.clear();
        if view.agent_search_mode {
            view.agent_search_evidence.clear();
        }
        if view.agent_edit_mode {
            view.agent_edit_evidence.clear();
        }
        if view.agent_glob_mode {
            view.agent_glob_evidence.clear();
        }
        view.approval_id.clear();
        view.approval_detail.clear();
        view.approval_pending = false;
        RequestPayload::AgentStart {
            session_id: session.id.clone(),
            text: text.clone(),
            attachments: Some(Vec::new()),
        }
    } else {
        RequestPayload::ChatSend {
            session_id: session.id.clone(),
            text: text.clone(),
            attachments: Some(selected_attachments),
        }
    };
    let response = request(client, command)?;
    if response["session_id"].as_str() != Some(&session.id) {
        return Err(Error::Protocol(
            "chat acknowledgement belongs to another session".into(),
        ));
    }
    session.run = response["run_id"]
        .as_str()
        .filter(|id| !id.is_empty())
        .ok_or_else(|| Error::Protocol("missing run ID".into()))?
        .into();
    view.selected_id = session.id.clone();
    view.live_user = if view.agent_mode && view.agent_restricted {
        if view.agent_work_plan_mode || view.agent_read_plan_mode {
            format!(
                "{} · {read_plan_step_count} step(s)",
                if view.agent_work_plan_mode {
                    "Workspace work plan"
                } else {
                    "Read-only plan"
                }
            )
        } else if view.agent_search_mode {
            format!(
                "Search workspace folder {} for literal text: {}",
                view.agent_search_path.trim(),
                view.agent_search_pattern
            )
        } else if view.agent_edit_mode {
            format!(
                "Edit {} after exact preimage and replacement approval",
                view.agent_edit_path.trim()
            )
        } else if view.agent_glob_mode {
            format!(
                "Find file names in {} matching {}",
                view.agent_glob_path.trim(),
                view.agent_glob_pattern
            )
        } else if view.agent_classify_mode {
            format!(
                "Classify for approved file inspection (original goal coverage unknown): {display_text}"
            )
        } else if view.agent_process_mode {
            format!(
                "Run approved read-only command: {}",
                display_text.lines().collect::<Vec<_>>().join(" · ")
            )
        } else if view.agent_create_mode {
            format!("Create file: {display_text}")
        } else if view.agent_directory_mode {
            format!("List directory: {}", display_text)
        } else {
            format!(
                "Inspect files: {}",
                display_text.lines().collect::<Vec<_>>().join(", ")
            )
        }
    } else {
        text
    };
    view.baseline_message_id = view.last_message_id;
    view.pending_message_id = 0;
    view.show_overlay = true;
    view.running = true;
    replace_response(view, "");
    view.status = "Waiting for the assistant…".into();
    view.agent_progress = if view.agent_mode {
        "Agent run accepted; waiting for the first progress event…".into()
    } else {
        String::new()
    };
    if !view.agent_mode {
        clear_attachments(
            view,
            "Attachments were added to this request. Refresh the file list to attach more.",
        );
    }
    Ok(())
}

fn start_existing_turn(
    client: &mut Client,
    session: &mut Session,
    view: &mut View,
    retry: bool,
) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    if !view.chat_revisions_capable || session.id.is_empty() {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    let message_id = if retry {
        view.latest_retry_id
    } else {
        view.latest_user_id
    };
    if message_id <= 0 {
        return Err(Error::Usage(if retry {
            "There is no failed latest response to retry.".into()
        } else {
            "There is no latest user message to regenerate.".into()
        }));
    }
    let command = if retry {
        RequestPayload::ChatRetry {
            session_id: session.id.clone(),
            message_id: message_id as u64,
        }
    } else {
        RequestPayload::ChatRegenerate {
            session_id: session.id.clone(),
            message_id: message_id as u64,
        }
    };
    let response = request(client, command)?;
    let target = response["session_id"]
        .as_str()
        .filter(|id| !id.is_empty())
        .ok_or_else(|| Error::Protocol("missing revision session ID".into()))?;
    if response["run_id"].as_str().is_none_or(str::is_empty) {
        return Err(Error::Protocol("missing revision run ID".into()));
    }
    if target != session.id {
        select_session(client, target, session, view)?;
        return Ok(());
    }
    session.run = response["run_id"]
        .as_str()
        .filter(|id| !id.is_empty())
        .ok_or_else(|| Error::Protocol("missing revision run ID".into()))?
        .into();
    if let Some(generation) = response["server_generation"].as_str() {
        session.generation = Some(generation.to_owned());
    }
    session.sequence = response["event_sequence"].as_u64().or(session.sequence);
    view.baseline_message_id = view.last_message_id;
    view.pending_message_id = 0;
    view.live_user.clear();
    view.show_overlay = true;
    view.running = true;
    view.stopping = false;
    view.truncated = false;
    replace_response(view, "");
    view.status = if retry {
        "Retrying the failed latest response…".into()
    } else {
        "Regenerating the latest response…".into()
    };
    Ok(())
}

fn edit_latest(
    client: &mut Client,
    session: &mut Session,
    view: &mut View,
    text: &str,
) -> Result<(), Error> {
    if view.running {
        return Err(Error::Remote("runtime-busy".into()));
    }
    if !view.chat_revisions_capable || session.id.is_empty() || view.latest_user_id <= 0 {
        return Err(Error::Remote("unsupported-capability".into()));
    }
    if text.trim().is_empty()
        || text.encode_utf16().count() > 65_536
        || serde_json::to_vec(text)
            .map_err(|error| Error::Protocol(error.to_string()))?
            .len()
            > sentinel_ipc::contract::MAX_FRAME_BYTES - 1024
    {
        return Err(Error::Usage(
            "Enter a replacement message within the supported size limit.".into(),
        ));
    }
    let old_user_id = view.latest_user_id;
    let response = request(
        client,
        RequestPayload::ChatEdit {
            session_id: session.id.clone(),
            message_id: old_user_id as u64,
            text: text.into(),
        },
    )?;
    let target = response["session_id"]
        .as_str()
        .filter(|id| !id.is_empty())
        .ok_or_else(|| Error::Protocol("missing edit session ID".into()))?;
    if response["run_id"].as_str().is_none_or(str::is_empty) {
        return Err(Error::Protocol("missing edit run ID".into()));
    }
    if target != session.id {
        select_session(client, target, session, view)?;
        return Ok(());
    }
    session.run = response["run_id"]
        .as_str()
        .filter(|id| !id.is_empty())
        .ok_or_else(|| Error::Protocol("missing edit run ID".into()))?
        .into();
    if let Some(generation) = response["server_generation"].as_str() {
        session.generation = Some(generation.into());
    }
    session.sequence = response["event_sequence"].as_u64().or(session.sequence);
    view.baseline_message_id = view.last_message_id;
    view.pending_message_id = 0;
    view.live_user = format!("Edited latest message: {text}");
    view.show_overlay = true;
    view.running = true;
    view.stopping = false;
    view.truncated = false;
    replace_response(view, "");
    view.status = "Sending edited latest message…".into();
    Ok(())
}

fn respond_approval(
    client: &mut Client,
    session: &mut Session,
    view: &mut View,
    allow: bool,
) -> Result<(), Error> {
    if !view.approval_capable || !view.approval_pending || view.approval_id.is_empty() {
        return Err(Error::Remote("invalid-state".into()));
    }
    let response = request(
        client,
        RequestPayload::ApprovalRespond {
            run_id: session.run.clone(),
            approval_id: view.approval_id.clone(),
            allow,
        },
    );
    match response {
        Ok(response) if response["accepted"] == true => {}
        result => {
            let (reason, original_error) = match result {
                Ok(_) => (
                    "the daemon did not acknowledge the response".to_owned(),
                    None,
                ),
                Err(error) => (error.to_string(), Some(error)),
            };
            if reconcile_approval(client, session, view).is_ok() {
                append_agent_activity(
                    view,
                    &format!(
                        "Approval response unconfirmed · {reason}; authoritative state refreshed, no response retried."
                    ),
                );
                view.status = if view.approval_pending {
                    format!(
                        "Approval remains pending after an unconfirmed response ({reason}). No response was retried."
                    )
                } else {
                    format!(
                        "Approval state refreshed after an unconfirmed response ({reason}). No response was retried."
                    )
                };
            } else {
                view.approval_pending = false;
                view.approval_id.clear();
                view.approval_detail.clear();
                view.status = "Approval response could not be confirmed. Reconnect to check the run before responding again.".into();
                return Err(original_error
                    .filter(Error::requires_reconnect)
                    .unwrap_or_else(|| Error::Failed(view.status.clone())));
            }
            return Ok(());
        }
    }
    view.approval_pending = false;
    view.approval_id.clear();
    view.approval_detail.clear();
    view.status = if allow {
        "Approval sent; waiting for the Agent run to continue.".into()
    } else {
        "Approval denied; waiting for the Agent run to stop.".into()
    };
    let approval_action = if allow { "approved once" } else { "denied" };
    append_agent_activity(
        view,
        &format!("Approval {approval_action}; waiting for daemon progress."),
    );
    view.agent_progress = if allow {
        "Approval accepted by the daemon; the Agent may resume.".into()
    } else {
        "Approval denied by the daemon; waiting for the run to terminate.".into()
    };
    Ok(())
}

fn reconcile_approval(
    client: &mut Client,
    session: &mut Session,
    view: &mut View,
) -> Result<(), Error> {
    let snapshot = request(
        client,
        RequestPayload::SessionAttach {
            session_id: session.id.clone(),
        },
    )?;
    if snapshot["session_id"].as_str() != Some(&session.id)
        || snapshot["run_id"].as_str() != Some(&session.run)
    {
        return Err(Error::Protocol(
            "approval refresh belongs to another run".into(),
        ));
    }
    session.generation = snapshot["server_generation"].as_str().map(str::to_owned);
    session.sequence = snapshot["event_sequence"].as_u64();
    let state = snapshot["state"].as_str().unwrap_or_default();
    view.running = is_active(&snapshot["state"]);
    view.stopping = false;
    replace_response(view, snapshot["output"].as_str().unwrap_or_default());
    view.truncated = snapshot["output_truncated"].as_bool().unwrap_or(false);
    view.approval_id = snapshot["approval_id"]
        .as_str()
        .unwrap_or_default()
        .to_owned();
    view.approval_pending =
        view.approval_capable && state == "approval" && !view.approval_id.is_empty();
    if view.approval_pending {
        view.approval_detail = approval_summary(&snapshot["approval"]);
        view.agent_progress = format!(
            "Waiting for your one-time approval · {}",
            snapshot["approval"]["tool"]
                .as_str()
                .unwrap_or("Agent action")
        );
        view.status = "Approval is still pending.".into();
    } else {
        view.approval_id.clear();
        view.approval_detail.clear();
        view.agent_progress = match state {
            "running" => "Agent run active; waiting for the next event.",
            "cancelled" => "Agent run cancelled.",
            "failed" => "Agent run failed.",
            "completed" => "Agent run completed.",
            _ => "No Agent run is active.",
        }
        .into();
        if state == "running" {
            if let Some(activity) = agent_snapshot_activity(&snapshot) {
                view.agent_progress = activity;
            }
        }
        view.status = match state {
            "running" => "Run is active; no approval is pending.",
            "cancelled" => "Run cancelled.",
            "failed" => "Run failed.",
            "completed" => "Run completed.",
            _ => "No approval is pending.",
        }
        .into();
    }
    if view.truncated {
        view.status.push_str(" Response preview truncated.");
    }
    Ok(())
}

fn agent_snapshot_activity(snapshot: &Value) -> Option<String> {
    let progress = &snapshot["properties"]["agent_activity"];
    let activity = catalog_text(progress["activity"].as_str(), 120);
    if activity.is_empty() {
        return None;
    }
    let tool = catalog_text(progress["tool"].as_str(), 60);
    let detail = if tool.is_empty() {
        activity
    } else {
        format!("{activity} · {tool}")
    };
    let step = progress["step"].as_u64().unwrap_or_default();
    Some(if step == 0 {
        detail
    } else {
        format!("Step {step} · {detail}")
    })
}

fn receive_event(session: &mut Session, view: &mut View, event: Envelope) -> bool {
    let agent_event = matches!(
        event.name.as_str(),
        "agent.activity" | "tool.requested" | "tool.running" | "tool.result" | "subagent.activity"
    );
    if agent_event && !view.agent_capable {
        return false;
    }
    if !view.running {
        return false;
    }
    if !matches!(
        event.name.as_str(),
        "output.delta"
            | "run.completed"
            | "run.failed"
            | "run.cancelled"
            | "approval.requested"
            | "agent.activity"
            | "tool.requested"
            | "tool.running"
            | "tool.result"
            | "subagent.activity"
    ) || !session.accepts(&event)
    {
        return false;
    }
    let text = event.payload["text"].as_str().unwrap_or_default();
    match event.name.as_str() {
        "output.delta" => {
            view.status = if view.stopping {
                "Stopping…"
            } else {
                "Receiving response…"
            }
            .into();
            append_response(view, text);
        }
        "approval.requested" => {
            if !view.approval_capable
                || event.payload["state"].as_str() != Some("approval")
                || event.payload["approval_id"]
                    .as_str()
                    .is_none_or(str::is_empty)
            {
                return false;
            }
            view.approval_id = event.payload["approval_id"]
                .as_str()
                .unwrap_or_default()
                .to_owned();
            view.approval_pending = true;
            view.approval_detail = approval_summary(&event.payload);
            let tool = event.payload["tool"].as_str().unwrap_or("Agent action");
            append_agent_activity(
                view,
                &format!("Approval requested · {tool} · review the approval card."),
            );
            view.agent_progress = format!("Waiting for your one-time approval · {tool}");
            view.status = if view.approval_pending {
                "Agent action needs your approval.".into()
            } else {
                "Approval requested, but this connection cannot respond.".into()
            };
        }
        "agent.activity" => {
            let step = event.payload["step"].as_u64().unwrap_or_default();
            let activity = event.payload["activity"].as_str().unwrap_or("activity");
            let decision = event.payload["decision"].as_str().unwrap_or_default();
            let detail = format!("{activity} {decision}").trim().to_owned();
            let progress = if step == 0 {
                detail
            } else {
                format!("Step {step} · {detail}")
            };
            append_agent_activity(view, &progress);
            view.agent_progress = progress;
            view.status = format!("Agent activity: {activity}");
        }
        "tool.requested" | "tool.running" | "tool.result" => {
            let tool = event.payload["tool"].as_str().unwrap_or("tool");
            let detail = event.payload["detail"].as_str().unwrap_or_default();
            let resources = event.payload["resources"]
                .as_array()
                .into_iter()
                .flatten()
                .filter_map(|resource| resource["resource"].as_str())
                .take(4)
                .map(|resource| resource.chars().take(180).collect::<String>())
                .collect::<Vec<_>>()
                .join(", ");
            let mut line = format!(
                "{} · {tool} · risk {} · {}",
                event.name,
                event.payload["risk"],
                detail.chars().take(220).collect::<String>()
            );
            if let Some(duration) = event.payload["duration_ms"].as_u64() {
                line.push_str(&format!(" · {duration} ms"));
            }
            if !resources.is_empty() {
                line.push_str(&format!(" · resources: {resources}"));
            }
            append_agent_activity(view, &line);
            view.agent_progress = format!(
                "{} · {tool}",
                match event.name.as_str() {
                    "tool.requested" => "Tool requested",
                    "tool.running" => "Tool running",
                    _ => "Tool result",
                }
            );
            view.status = format!("Agent tool: {tool}");
        }
        "subagent.activity" => {
            let line = format!(
                "Subagent {} · {} · {}",
                event.payload["subagent_id"].as_str().unwrap_or("?"),
                event.payload["role"].as_str().unwrap_or(""),
                event.payload["state"].as_str().unwrap_or("?")
            );
            append_agent_activity(view, &line);
            view.agent_progress = format!(
                "Subagent {} · {}",
                event.payload["subagent_id"].as_str().unwrap_or("?"),
                event.payload["state"].as_str().unwrap_or("updated")
            );
            view.status = "Agent subtask updated.".into();
        }
        terminal => {
            replace_response(view, text);
            if view.agent_search_mode && terminal == "run.completed" {
                view.agent_search_evidence = verified_literal_search_evidence(
                    &view.response,
                    view.agent_search_path.trim(),
                    &view.agent_search_pattern,
                )
                .unwrap_or_else(|| {
                    "Search finished, but complete source-labelled evidence could not be verified. Reopen Agent history to inspect the authoritative tool result.".into()
                });
            }
            if view.agent_edit_mode && terminal == "run.completed" {
                let expected_prefix = format!(
                    "Edited and verified file: {}\n",
                    view.agent_edit_path.trim()
                );
                view.agent_edit_evidence = if view.response.starts_with(&expected_prefix) {
                    view.response.clone()
                } else {
                    "Edit completed, but the source-labelled read-back evidence was not present. Reopen Agent history to inspect the authoritative result.".into()
                };
            }
            if view.agent_glob_mode && terminal == "run.completed" {
                view.agent_glob_evidence = verified_glob_evidence(
                    &view.response,
                    view.agent_glob_path.trim(),
                    &view.agent_glob_pattern,
                )
                .unwrap_or_else(|| {
                    "Filename search completed, but complete source-labelled evidence could not be verified. Reopen Agent history to inspect the authoritative tool result.".into()
                });
            }
            if view.agent_read_plan_mode || view.agent_work_plan_mode {
                if terminal == "run.completed" {
                    let evidence = verified_read_plan_evidence(
                        &view.response,
                        view.agent_read_plan_expected_steps,
                    )
                    .unwrap_or_else(|| {
                        "The run completed without a complete ordered Step 1…N result envelope. Reopen Agent history to inspect the authoritative run.".into()
                    });
                    if view.agent_work_plan_mode {
                        view.agent_work_plan_evidence = evidence;
                        view.agent_status = "Work plan completed; inspect per-step output and approval history below.".into();
                    } else {
                        view.agent_read_plan_evidence = evidence;
                        view.agent_status =
                            "Read-only plan completed; per-step output is shown below.".into();
                    }
                } else if terminal == "run.failed" {
                    let reason = event.payload["terminal_reason"]
                        .as_str()
                        .unwrap_or("unknown");
                    let explanation = match reason {
                        "plan-incomplete" => {
                            "A step failed or was skipped because a dependency failed. Dependent steps did not run."
                        }
                        "storage-failure" => {
                            "The daemon could not durably record the run outcome. Reconnect and inspect Agent history."
                        }
                        "provider-failed" => {
                            "The provider failed during a plan step. Review activity and retry explicitly."
                        }
                        "plan-preflight-failed" => {
                            "An initial step request could not be prepared within the model limits. No plan steps ran."
                        }
                        "plan-timeout" => {
                            "The bounded plan deadline expired. Review completed steps before retrying."
                        }
                        "decision-limit" => {
                            "The model-decision limit was reached. Review completed steps before retrying."
                        }
                        "write-outcome-unknown" => {
                            "A write outcome is uncertain. The plan stopped; inspect the workspace before any retry."
                        }
                        "write-not-committed" => {
                            "A write was not committed. The plan stopped without continuing dependent steps."
                        }
                        "verification-failed" => {
                            "A write could not be verified by read-back. Inspect the workspace before retrying."
                        }
                        _ => {
                            "The daemon reported a typed failure. Reconnect and inspect Agent history."
                        }
                    };
                    let plan_name = if view.agent_work_plan_mode {
                        "Work plan"
                    } else {
                        "Read-only plan"
                    };
                    view.agent_status = format!("{plan_name} failed ({reason}). {explanation}");
                    view.agent_read_plan_evidence.clear();
                    view.agent_work_plan_evidence.clear();
                } else {
                    view.agent_status = if view.agent_work_plan_mode {
                        "Work plan cancelled; no successful result is claimed. Inspect any approved write before retrying.".into()
                    } else {
                        "Read-only plan cancelled; no successful plan result is claimed.".into()
                    };
                    view.agent_read_plan_evidence.clear();
                    view.agent_work_plan_evidence.clear();
                }
                let plan_status = view.agent_status.clone();
                append_agent_activity(view, &plan_status);
            }
            view.running = false;
            view.stopping = false;
            view.approval_id.clear();
            view.approval_detail.clear();
            view.approval_pending = false;
            let terminal_progress = match terminal {
                "run.completed" => "Agent run completed.",
                "run.cancelled" => "Agent run cancelled.",
                _ if event.payload["terminal_reason"] == "agent-timeout" => {
                    "Agent run deadline expired."
                }
                _ if event.payload["terminal_reason"] == "decision-limit" => {
                    "Agent model-decision limit reached."
                }
                _ => "Agent run failed.",
            };
            append_agent_activity(view, terminal_progress);
            view.agent_progress = terminal_progress.into();
            view.status = match terminal {
                "run.completed" => "Response complete.",
                "run.cancelled" => "Run stopped.",
                _ => "Run failed.",
            }
            .into();
        }
    }
    if view.truncated {
        view.status.push_str(" Response preview truncated.");
    }
    true
}

fn disconnected(view: &mut View, error: Error) {
    let had_pending_approval = view.approval_pending;
    view.connected = false;
    view.running = false;
    view.stopping = false;
    view.approval_pending = false;
    view.approval_id.clear();
    clear_attachments(
        view,
        "Disconnected. Reconnect and refresh the authorized file list.",
    );
    view.extension_catalog_view.clear();
    view.extension_catalog_truncated = false;
    if view.extension_catalog_capable {
        view.extension_catalog_status =
            "Disconnected; refresh after reconnect to reload the catalog.".into();
    }
    if had_pending_approval {
        view.approval_detail = "Approval state is unavailable while disconnected. Reconnect to inspect it before responding.".into();
    }
    view.status = format!("{error} Reconnect to inspect the session.");
    if view.agent_capable {
        view.agent_progress =
            "Disconnected; live Agent progress is unknown until the session is reattached.".into();
    }
}

fn worker(path: PathBuf, commands: Receiver<Command>, updates: PanelUpdates) {
    let mut session = Session::default();
    let mut view = View {
        active_page: "chat".into(),
        endpoint: path.to_string_lossy().into_owned(),
        theme_name: "system".into(),
        theme_status: "Loading theme preference…".into(),
        response_profile_status: "Loading saved Chat instructions…".into(),
        provider_diagnostics_status:
            "Refresh to inspect cached settings for the selected model provider.".into(),
        agent_process_program: "/usr/bin/wc".into(),
        agent_search_path: ".".into(),
        agent_glob_path: ".".into(),
        ..View::default()
    };
    let mut client = match connect(&path, &mut session, &mut view) {
        Ok(client) => Some(client),
        Err(error) => {
            disconnected(&mut view, error);
            None
        }
    };
    updates.publish(&view, false);
    let mut last_display = Instant::now();
    let mut changed = false;
    loop {
        let command = if view.running {
            match commands.try_recv() {
                Ok(command) => Some(command),
                Err(TryRecvError::Empty) => None,
                Err(TryRecvError::Disconnected) => break,
            }
        } else if view.ollama_pulling {
            match commands.recv_timeout(Duration::from_millis(500)) {
                Ok(command) => Some(command),
                Err(RecvTimeoutError::Timeout) => None,
                Err(RecvTimeoutError::Disconnected) => break,
            }
        } else {
            match commands.recv() {
                Ok(command) => Some(command),
                Err(_) => break,
            }
        };
        let helper_poll_tick = command.is_none() && view.ollama_pulling && !view.running;
        if let Some(command) = command {
            let model_command = matches!(
                &command,
                Command::RefreshModels | Command::SelectModel(_) | Command::SelectProvider(_)
            );
            let settings_command = matches!(
                &command,
                Command::SetTheme(_)
                    | Command::SaveResponseProfile(_)
                    | Command::RefreshProviderDiagnostics
                    | Command::RefreshSettingsStatus
                    | Command::SetDevWorkspaceAccess(_)
                    | Command::ExportDevBackup(_, _)
            );
            let backup_command = matches!(&command, Command::ExportDevBackup(_, _));
            let response_profile_command = matches!(&command, Command::SaveResponseProfile(_));
            let provider_diagnostics_command =
                matches!(&command, Command::RefreshProviderDiagnostics);
            let settings_status_command = matches!(&command, Command::RefreshSettingsStatus);
            let workspace_access_command = matches!(&command, Command::SetDevWorkspaceAccess(_));
            let memory_command = matches!(
                &command,
                Command::RefreshMemory
                    | Command::SelectMemory(_)
                    | Command::NewMemory
                    | Command::SaveMemory
                    | Command::DeleteMemory
                    | Command::LoadMoreMemory
            ) || matches!(&command, Command::Navigate(page) if page == "memory");
            let revision_command = matches!(
                &command,
                Command::RegenerateLatest | Command::RetryLatest | Command::EditLatest(_)
            );
            let session_lifecycle_command = matches!(
                &command,
                Command::RenameSession(_)
                    | Command::DeleteSession(_)
                    | Command::SetSessionFlag { .. }
            );
            let ollama_command = matches!(
                &command,
                Command::RefreshOllamaHelper
                    | Command::PullOllamaModel(_)
                    | Command::CancelOllamaPull
                    | Command::RemoveOllamaModel { .. }
            );
            let workspace_files_command = matches!(&command, Command::RefreshWorkspaceFiles);
            let attachment_command = matches!(
                &command,
                Command::RefreshAttachmentFiles
                    | Command::SelectAttachmentCandidate(_)
                    | Command::AddAttachment
                    | Command::SelectAttachment(_)
                    | Command::RemoveAttachment
                    | Command::ClearAttachments
            );
            let workspace_changes_command = matches!(&command, Command::RefreshWorkspaceChanges);
            let extension_catalog_command = matches!(&command, Command::RefreshExtensions)
                || matches!(&command, Command::Navigate(page) if page == "extensions");
            let workspace_selection_command = matches!(&command, Command::SelectWorkspace(_));
            let workspace_navigation =
                matches!(&command, Command::Navigate(page) if page == "workspace");
            let workspace_command = workspace_files_command
                || attachment_command
                || workspace_changes_command
                || workspace_selection_command
                || workspace_navigation;
            let agent_command = matches!(
                &command,
                Command::SetAgentMode(_)
                    | Command::SetAgentDirectoryMode(_)
                    | Command::SetAgentCreateMode(_)
                    | Command::SetAgentIncludeHidden(_)
                    | Command::SetAgentProcessMode(_)
                    | Command::SetAgentProcessProgram(_)
                    | Command::SetAgentClassifyMode(_)
                    | Command::SetAgentSearchMode(_)
                    | Command::SetAgentEditMode(_)
                    | Command::SetAgentGlobMode(_)
                    | Command::SetAgentReadPlanMode(_)
                    | Command::SetAgentWorkPlanMode(_)
                    | Command::RespondApproval(_)
                    | Command::RefreshAgentHistory
                    | Command::LoadAgentRun(_)
                    | Command::LoadOlderAgentRuns
            ) || (view.agent_mode && matches!(&command, Command::Send { .. }));
            let mut clear_draft = false;
            let result = match command {
                Command::Navigate(ref page)
                    if page == "chat" && view.agent_mode && view.running =>
                {
                    Err(Error::Remote("runtime-busy".into()))
                }
                Command::Navigate(page) => {
                    if page == "chat" && view.agent_mode {
                        let _ = set_agent_mode(&mut view, false);
                    }
                    if matches!(
                        page.as_str(),
                        "chat"
                            | "models"
                            | "agent"
                            | "settings"
                            | "memory"
                            | "workspace"
                            | "extensions"
                    ) && (page != "agent" || view.agent_capable || view.agent_history_capable)
                        && (page != "memory" || view.memory_capable)
                        && (page != "workspace"
                            || view.workspace_files_capable
                            || view.workspace_changes_capable
                            || view.workspace_selection_capable)
                        && (page != "extensions" || view.extension_catalog_capable)
                    {
                        view.active_page = page;
                    }
                    if view.active_page == "extensions" && view.extension_catalog_capable {
                        client
                            .as_mut()
                            .ok_or(Error::Disconnected)
                            .and_then(|client| refresh_extension_catalog(client, &mut view))
                    } else if view.active_page == "memory" {
                        client
                            .as_mut()
                            .ok_or(Error::Disconnected)
                            .and_then(|client| memory_list(client, &session, &mut view, false))
                    } else if view.active_page == "agent" && view.agent_history_capable {
                        client
                            .as_mut()
                            .ok_or(Error::Disconnected)
                            .and_then(|client| refresh_agent_history(client, &mut view, false))
                    } else if view.active_page == "models" && view.ollama_helper_capable {
                        client
                            .as_mut()
                            .ok_or(Error::Disconnected)
                            .and_then(|client| refresh_ollama_helper(client, &mut view))
                    } else if view.active_page == "settings" {
                        client
                            .as_mut()
                            .ok_or(Error::Disconnected)
                            .and_then(|client| refresh_settings_status(client, &mut view))
                    } else if view.active_page == "workspace" {
                        if let Some(client) = client.as_mut() {
                            let mut first_error = None;
                            let state_result = if view.workspace_selection_capable {
                                refresh_workspace_state(client, &mut view)
                            } else {
                                Ok(())
                            };
                            if let Err(error) = state_result {
                                first_error = Some(error);
                            }
                            if first_error.is_none()
                                && view.workspace_files_capable
                                && let Err(error) =
                                    refresh_workspace_files(client, &session, &mut view)
                            {
                                first_error = Some(error);
                            }
                            if first_error.is_none()
                                && view.workspace_changes_capable
                                && let Err(error) =
                                    refresh_workspace_changes(client, &session, &mut view)
                            {
                                first_error.get_or_insert(error);
                            }
                            first_error.map_or(Ok(()), Err)
                        } else {
                            Err(Error::Disconnected)
                        }
                    } else {
                        Ok(())
                    }
                }
                Command::RefreshWorkspaceFiles => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| refresh_workspace_files(client, &session, &mut view)),
                Command::RefreshAttachmentFiles => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| refresh_workspace_files(client, &session, &mut view)),
                Command::SelectAttachmentCandidate(index) => {
                    if index >= view.attachment_files.len() {
                        Err(Error::Usage(
                            "Authorized file selection is no longer available.".into(),
                        ))
                    } else {
                        view.selected_attachment_candidate = index;
                        Ok(())
                    }
                }
                Command::AddAttachment => add_attachment(&mut view),
                Command::SelectAttachment(index) => {
                    if index >= view.attachments.len() {
                        Err(Error::Usage(
                            "Attachment selection is no longer available.".into(),
                        ))
                    } else {
                        view.selected_attachment = index;
                        Ok(())
                    }
                }
                Command::RemoveAttachment => remove_attachment(&mut view),
                Command::ClearAttachments => {
                    view.attachments.clear();
                    view.selected_attachment = 0;
                    view.attachment_status = "No files selected.".into();
                    Ok(())
                }
                Command::RefreshWorkspaceChanges => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| refresh_workspace_changes(client, &session, &mut view)),
                Command::RefreshExtensions => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| refresh_extension_catalog(client, &mut view)),
                Command::SelectWorkspace(index) if !view.running => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| select_workspace(client, &session, &mut view, index)),
                Command::SelectWorkspace(_) if view.running => {
                    Err(Error::Remote("runtime-busy".into()))
                }
                Command::SetTheme(index) => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| set_theme(client, &mut view, index)),
                Command::SaveResponseProfile(value) => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| save_response_profile(client, &mut view, &value)),
                Command::RefreshProviderDiagnostics => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| refresh_provider_diagnostics(client, &mut view)),
                Command::RefreshSettingsStatus => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| refresh_settings_status(client, &mut view)),
                Command::SetDevWorkspaceAccess(enabled) => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| set_dev_workspace_access(client, &mut view, enabled)),
                Command::ExportDevBackup(index, path) => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| export_dev_backup(client, &mut view, index, &path)),
                Command::RefreshMemory => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| memory_list(client, &session, &mut view, false)),
                Command::LoadMoreMemory => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| memory_list(client, &session, &mut view, true)),
                Command::SelectMemory(index) => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| memory_read(client, &session, &mut view, index)),
                Command::NewMemory => {
                    view.memory_id.clear();
                    view.memory_revision.clear();
                    view.memory_key.clear();
                    view.memory_value.clear();
                    view.memory_delete_confirmation.clear();
                    view.memory_status = "Enter a key and value for this Chat session.".into();
                    Ok(())
                }
                Command::SaveMemory => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| memory_save(client, &session, &mut view)),
                Command::DeleteMemory => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| memory_delete(client, &session, &mut view)),
                Command::RegenerateLatest => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| start_existing_turn(client, &mut session, &mut view, false)),
                Command::RetryLatest => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| start_existing_turn(client, &mut session, &mut view, true)),
                Command::EditLatest(text) => {
                    let result = client
                        .as_mut()
                        .ok_or(Error::Disconnected)
                        .and_then(|client| edit_latest(client, &mut session, &mut view, &text));
                    clear_draft = result.is_ok();
                    result
                }
                Command::RenameSession(title) => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| rename_session(client, &session, &mut view, &title)),
                Command::SetSessionFlag { id, action } => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| set_session_flag(client, &session, &mut view, &id, &action)),
                Command::DeleteSession(confirmation) => {
                    view.session_delete_confirmation = confirmation.clone();
                    client
                        .as_mut()
                        .ok_or(Error::Disconnected)
                        .and_then(|client| {
                            delete_session(client, &mut session, &mut view, &confirmation)
                        })
                }
                Command::RefreshOllamaHelper => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| refresh_ollama_helper(client, &mut view)),
                Command::PullOllamaModel(model) => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| ollama_model_action(client, &mut view, "pull", &model, "")),
                Command::CancelOllamaPull => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| ollama_model_action(client, &mut view, "cancel", "", "")),
                Command::RemoveOllamaModel {
                    model,
                    confirmation,
                } => {
                    view.ollama_remove_confirmation = confirmation.clone();
                    client
                        .as_mut()
                        .ok_or(Error::Disconnected)
                        .and_then(|client| {
                            ollama_model_action(
                                client,
                                &mut view,
                                "removeModel",
                                &model,
                                &confirmation,
                            )
                        })
                }
                Command::Reconnect => {
                    connect(&path, &mut session, &mut view).map(|new| client = Some(new))
                }
                Command::Refresh if !view.running => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| refresh(client, &mut view)),
                Command::RefreshModels if !view.running => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| {
                        refresh_models(client, &mut view)?;
                        if view.ollama_helper_capable {
                            refresh_ollama_helper(client, &mut view)?;
                        }
                        Ok(())
                    }),
                Command::SaveProviderEndpoint { provider, endpoint } => {
                    let result = if view.running {
                        Err(Error::Remote("runtime-busy".into()))
                    } else {
                        client
                            .as_mut()
                            .ok_or(Error::Disconnected)
                            .and_then(|client| {
                                save_provider_endpoint(client, &mut view, &provider, &endpoint)
                            })
                    };
                    if let Err(error) = &result {
                        view.provider_endpoint_status = format!("Endpoint update failed: {error}");
                    }
                    result
                }
                Command::SelectProvider(index) if !view.running => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| select_local_provider(client, index, &mut view)),
                Command::SelectProvider(_) => Err(Error::Remote("runtime-busy".into())),
                Command::SelectModel(index) if !view.running => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| select_model(client, index, &mut view)),
                Command::SelectModel(_) if view.running => {
                    Err(Error::Remote("runtime-busy".into()))
                }
                Command::SetAgentMode(enabled) => set_agent_mode(&mut view, enabled),
                Command::SetAgentDirectoryMode(enabled) => {
                    set_agent_directory_mode(&mut view, enabled)
                }
                Command::SetAgentCreateMode(enabled) => set_agent_create_mode(&mut view, enabled),
                Command::SetAgentIncludeHidden(enabled) => {
                    set_agent_include_hidden(&mut view, enabled)
                }
                Command::SetAgentProcessMode(enabled) => set_agent_process_mode(&mut view, enabled),
                Command::SetAgentProcessProgram(index) => {
                    set_agent_process_program(&mut view, index)
                }
                Command::SetAgentClassifyMode(enabled) => {
                    set_agent_classify_mode(&mut view, enabled)
                }
                Command::SetAgentSearchMode(enabled) => set_agent_search_mode(&mut view, enabled),
                Command::SetAgentEditMode(enabled) => set_agent_edit_mode(&mut view, enabled),
                Command::SetAgentGlobMode(enabled) => set_agent_glob_mode(&mut view, enabled),
                Command::SetAgentReadPlanMode(enabled) => {
                    set_agent_read_plan_mode(&mut view, enabled)
                }
                Command::SetAgentWorkPlanMode(enabled) => {
                    set_agent_work_plan_mode(&mut view, enabled)
                }
                Command::RefreshAgentHistory => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| refresh_agent_history(client, &mut view, false)),
                Command::LoadOlderAgentRuns => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| refresh_agent_history(client, &mut view, true)),
                Command::LoadAgentRun(index) => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| load_agent_run_history(client, &mut view, index)),
                Command::RespondApproval(allow) => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| respond_approval(client, &mut session, &mut view, allow)),
                Command::Select(index) if !view.running => {
                    if let Some((id, _)) = view.sessions.get(index) {
                        let id = id.clone();
                        client
                            .as_mut()
                            .ok_or(Error::Disconnected)
                            .and_then(|client| select_session(client, &id, &mut session, &mut view))
                    } else {
                        Err(Error::Usage("Session is no longer available.".into()))
                    }
                }
                Command::New if !view.running => {
                    session = Session::default();
                    view.selected_id.clear();
                    clear_attachments(
                        &mut view,
                        "New Chat session. Select or create a session, then refresh authorized files.",
                    );
                    view.session_delete_confirmation.clear();
                    view.workspace_files_root.clear();
                    view.workspace_files_view.clear();
                    view.workspace_files_truncated = false;
                    view.workspace_files_status =
                        "Create or select a Chat session before loading workspace files.".into();
                    view.workspace_changes_view = "No trusted baseline is available. The workspace state is unknown; this does not mean it is clean.".into();
                    view.workspace_changes_root.clear();
                    view.workspace_changes_truncated = false;
                    view.workspace_changes_status =
                        "Create or select a Chat session before checking workspace changes.".into();
                    clear_memory_selection(
                        &mut view,
                        "New Chat session. Create or select a Chat before managing memory.",
                    );
                    clear_agent_history(
                        &mut view,
                        "New Chat session. Select Agent to load its run history.",
                    );
                    view.history.clear();
                    view.last_message_id = 0;
                    view.baseline_message_id = 0;
                    view.pending_message_id = 0;
                    view.last_assistant.clear();
                    view.live_user.clear();
                    view.show_overlay = false;
                    view.agent_activity.clear();
                    view.agent_search_evidence.clear();
                    view.agent_edit_evidence.clear();
                    view.agent_glob_evidence.clear();
                    view.approval_id.clear();
                    view.approval_detail.clear();
                    view.approval_pending = false;
                    view.history_status = "New session. Send a message to begin.".into();
                    replace_response(&mut view, "");
                    view.status = "New session.".into();
                    Ok(())
                }
                Command::Send { agent, .. } if agent != view.agent_mode => Err(Error::Usage(
                    "The conversation mode changed. Review the draft and send again.".into(),
                )),
                Command::Send { text, .. } if !view.running => {
                    let result = client
                        .as_mut()
                        .ok_or(Error::Disconnected)
                        .and_then(|client| send(client, &mut session, &mut view, text));
                    clear_draft = result.is_ok();
                    result
                }
                Command::Stop if view.running && !view.stopping => client
                    .as_mut()
                    .ok_or(Error::Disconnected)
                    .and_then(|client| {
                        let response = request(
                            client,
                            RequestPayload::RunCancel {
                                run_id: session.run.clone(),
                            },
                        )?;
                        if response["accepted"] != true {
                            return Err(Error::Failed(
                                "The daemon did not acknowledge cancellation.".into(),
                            ));
                        }
                        view.stopping = true;
                        view.status = "Stopping…".into();
                        Ok(())
                    }),
                _ => Ok(()),
            };
            if let Err(error) = result {
                if agent_command {
                    if !view.agent_status.starts_with("Agent mode unavailable:") {
                        view.agent_status = format!("Agent action failed: {error}");
                    }
                } else if model_command {
                    if !view.model_status.starts_with("Models unavailable:")
                        && !view.model_status.starts_with("Selection failed")
                        && !view
                            .model_status
                            .starts_with("Selection result did not match")
                    {
                        view.model_status = format!("Model operation failed: {error}");
                    }
                } else if settings_command {
                    if backup_command {
                        view.dev_backup_status = format!("Snapshot export failed: {error}");
                    } else if provider_diagnostics_command {
                        view.provider_diagnostics.clear();
                        view.provider_diagnostics_status =
                            format!("Provider diagnostics unavailable: {error}");
                    } else if settings_status_command {
                        if view.runtime_projection_status.starts_with("Loading") {
                            view.runtime_projection_status =
                                format!("Runtime status unavailable: {error}");
                        }
                        if view.workspace_runtime_status.starts_with("Loading") {
                            view.workspace_runtime_status =
                                format!("Workspace status unavailable: {error}");
                        }
                        if view.workspace_access_status.starts_with("Loading") {
                            view.workspace_access_status =
                                format!("Workspace access state unavailable: {error}");
                        }
                    } else if workspace_access_command {
                        view.workspace_access_status =
                            format!("Workspace access change could not be confirmed: {error}");
                    } else if !view.theme_status.starts_with("Theme change failed;") {
                        if response_profile_command {
                            if !view
                                .response_profile_status
                                .starts_with("Save was not accepted")
                                && !view
                                    .response_profile_status
                                    .starts_with("The daemon's saved value")
                            {
                                view.response_profile_status = format!("Save failed: {error}");
                            }
                        } else {
                            view.theme_status = format!("Theme operation failed: {error}");
                        }
                    }
                } else if memory_command {
                    view.memory_status = format!("Memory operation failed: {error}");
                } else if revision_command {
                    view.history_status = format!("Latest turn action failed: {error}");
                } else if session_lifecycle_command {
                    view.history_status = format!("Session action failed: {error}");
                } else if ollama_command {
                    view.ollama_helper_status = format!("Ollama model operation failed: {error}");
                } else if workspace_command {
                    view.status = format!("Workspace request failed: {error}");
                    if attachment_command && view.attachment_status.starts_with("Loading") {
                        view.attachment_status =
                            format!("Cannot attach from workspace snapshot: {error}");
                    }
                    if workspace_selection_command
                        && view.workspace_selection_status.starts_with("Selecting")
                    {
                        view.workspace_selection_status =
                            format!("Workspace selection could not be confirmed: {error}");
                    }
                    if (workspace_files_command || workspace_navigation)
                        && view.workspace_files_status.starts_with("Loading")
                    {
                        view.workspace_files_status =
                            format!("Workspace snapshot unavailable: {error}");
                    }
                    if (workspace_changes_command || workspace_navigation)
                        && view.workspace_changes_status.starts_with("Loading")
                    {
                        view.workspace_changes_status =
                            format!("Workspace comparison unavailable: {error}");
                    }
                } else if extension_catalog_command {
                    view.extension_catalog_view.clear();
                    view.extension_catalog_status =
                        format!("Extension catalog unavailable: {error}");
                } else {
                    view.history_status = format!("History unavailable: {error}");
                }
                if error.requires_reconnect() {
                    client = None;
                    disconnected(&mut view, error);
                } else {
                    view.status = error.to_string();
                }
            }
            changed = false;
            updates.publish(&view, clear_draft);
            last_display = Instant::now();
            continue;
        }
        if helper_poll_tick && let Some(client) = client.as_mut() {
            if let Err(error) = refresh_ollama_helper(client, &mut view) {
                view.ollama_helper_status = format!(
                    "Ollama progress unavailable: {error}. Refresh status and the model catalog before retrying."
                );
                view.ollama_pulling = false;
            }
            updates.publish(&view, false);
        }
        let Some(active) = &mut client else { break };
        match active.next_event() {
            Ok(event) => {
                changed |= receive_event(&mut session, &mut view, event);
                if !view.running && changed {
                    let result = refresh_after_terminal(active, &session, &mut view);
                    if let Err(error) = result {
                        view.history_status = format!("History unavailable: {error}");
                        if error.requires_reconnect() {
                            client = None;
                            disconnected(&mut view, error);
                        }
                    }
                }
            }
            Err(Error::Timeout) => {}
            Err(error) => {
                client = None;
                disconnected(&mut view, error);
                changed = true;
            }
        }
        if changed && (!view.running || last_display.elapsed() >= DISPLAY_INTERVAL) {
            updates.publish(&view, false);
            changed = false;
            last_display = Instant::now();
        }
    }
}

fn submit(panel: &slint::Weak<Panel>, sender: &SyncSender<Command>, command: Command) {
    if let Some(panel) = panel.upgrade() {
        panel.set_waiting(true);
        match &command {
            Command::Navigate(page) if page == "workspace" => {
                panel.set_workspace_files_status("Loading authorized workspace snapshot…".into());
                panel.set_workspace_changes_status(
                    "Loading accepted-run workspace comparison…".into(),
                );
                panel.set_workspace_selection_status("Loading configured workspaces…".into());
            }
            Command::Navigate(page) if page == "extensions" => {
                panel.set_extension_catalog_status("Loading extension catalog…".into())
            }
            Command::RefreshExtensions => {
                panel.set_extension_catalog_status("Refreshing extension catalog…".into())
            }
            Command::Navigate(_) => {}
            Command::SetTheme(_) => panel.set_theme_status("Saving theme preference…".into()),
            Command::SaveResponseProfile(_) => {
                panel.set_response_profile_status("Saving Chat response instructions…".into())
            }
            Command::RefreshProviderDiagnostics => panel
                .set_provider_diagnostics_status("Loading local provider configuration…".into()),
            Command::RefreshSettingsStatus => {
                panel.set_runtime_projection_status("Loading daemon runtime status…".into());
                panel.set_workspace_runtime_status(
                    "Loading configured workspace permissions…".into(),
                );
                panel.set_workspace_access_status(
                    "Loading access for the selected configured workspace…".into(),
                );
            }
            Command::SetDevWorkspaceAccess(enabled) => {
                panel.set_workspace_access_status(if *enabled {
                    "Re-enabling selected workspace access…".into()
                } else {
                    "Revoking selected workspace access…".into()
                })
            }
            Command::ExportDevBackup(_, _) => {
                panel.set_dev_backup_status("Exporting private snapshot…".into())
            }
            Command::RefreshMemory => panel.set_memory_status("Loading session memory…".into()),
            Command::SelectMemory(_) => panel.set_memory_status("Reading memory record…".into()),
            Command::NewMemory => {}
            Command::SaveMemory => panel.set_memory_status("Saving session memory…".into()),
            Command::DeleteMemory => panel.set_memory_status("Deleting session memory…".into()),
            Command::LoadMoreMemory => {
                panel.set_memory_status("Loading more session memory…".into())
            }
            Command::RefreshAgentHistory | Command::LoadOlderAgentRuns => {
                panel.set_agent_history_status("Loading persisted Agent run history…".into())
            }
            Command::LoadAgentRun(_) => {
                panel.set_agent_history_status("Loading persisted run details…".into())
            }
            Command::RenameSession(_) => {
                panel.set_history_status("Renaming current Chat session…".into())
            }
            Command::SetSessionFlag { .. } => panel.set_status("Updating current session…".into()),
            Command::DeleteSession(_) => {
                panel.set_history_status("Deleting current session and its private records…".into())
            }
            Command::RegenerateLatest => panel.set_status("Regenerating latest response…".into()),
            Command::RetryLatest => panel.set_status("Retrying failed latest response…".into()),
            Command::EditLatest(_) => panel.set_status("Sending edited latest message…".into()),
            Command::Reconnect => {
                panel.set_status("Connecting…".into());
                panel.set_model_status("Loading saved model selection…".into());
                panel.set_selected_model(-1);
            }
            Command::Refresh => panel.set_history_status("Loading sessions…".into()),
            Command::Select(_) => panel.set_history_status("Loading session history…".into()),
            Command::RefreshModels => panel.set_model_status("Loading models…".into()),
            Command::SelectModel(_) => panel.set_model_status("Selecting model…".into()),
            Command::SelectProvider(_) => panel.set_model_status("Selecting provider…".into()),
            Command::SaveProviderEndpoint { .. } => {
                panel.set_provider_endpoint_status("Saving endpoint…".into())
            }
            Command::RefreshWorkspaceFiles => {
                panel.set_workspace_files_status("Loading authorized workspace snapshot…".into())
            }
            Command::RefreshAttachmentFiles => {
                panel.set_attachment_status("Loading the authorized file list…".into())
            }
            Command::SelectAttachmentCandidate(_) => {
                panel.set_attachment_status("Updating selected workspace file…".into())
            }
            Command::AddAttachment => panel.set_attachment_status("Adding selected file…".into()),
            Command::SelectAttachment(_) => {
                panel.set_attachment_status("Updating selected attachment…".into())
            }
            Command::RemoveAttachment => panel.set_attachment_status("Removing attachment…".into()),
            Command::ClearAttachments => {
                panel.set_attachment_status("Clearing attachments…".into())
            }
            Command::RefreshWorkspaceChanges => panel
                .set_workspace_changes_status("Loading accepted-run workspace comparison…".into()),
            Command::SelectWorkspace(_) => panel.set_workspace_selection_status(
                "Selecting workspace and confirming daemon state…".into(),
            ),
            Command::RefreshOllamaHelper => {
                panel.set_ollama_helper_status("Loading Ollama pull state…".into())
            }
            Command::PullOllamaModel(_) => {
                panel.set_ollama_helper_status("Starting model pull…".into())
            }
            Command::CancelOllamaPull => {
                panel.set_ollama_helper_status("Cancelling local pull wait…".into())
            }
            Command::RemoveOllamaModel { .. } => {
                panel.set_ollama_helper_status("Removing model…".into())
            }
            Command::SetAgentMode(enabled) => panel.set_agent_status(if *enabled {
                "Enabling Agent mode…".into()
            } else {
                "Switching to Chat mode…".into()
            }),
            Command::SetAgentDirectoryMode(_) => {
                panel.set_agent_status("Updating inspection task…".into())
            }
            Command::SetAgentCreateMode(_) => {
                panel.set_agent_status("Updating create-file task…".into())
            }
            Command::SetAgentIncludeHidden(_) => {
                panel.set_agent_status("Updating directory options…".into())
            }
            Command::SetAgentProcessMode(_) => {
                panel.set_agent_status("Updating read-only process task…".into())
            }
            Command::SetAgentProcessProgram(_) => {
                panel.set_agent_status("Selecting approved command…".into())
            }
            Command::SetAgentClassifyMode(_) => {
                panel.set_agent_status("Updating approved file classification mode…".into())
            }
            Command::SetAgentSearchMode(_) => {
                panel.set_agent_status("Updating literal search task…".into())
            }
            Command::SetAgentEditMode(_) => {
                panel.set_agent_status("Updating exact file edit task…".into())
            }
            Command::SetAgentGlobMode(_) => {
                panel.set_agent_status("Updating filename search task…".into())
            }
            Command::SetAgentReadPlanMode(_) => {
                panel.set_agent_status("Updating bounded read-only plan…".into())
            }
            Command::SetAgentWorkPlanMode(_) => {
                panel.set_agent_status("Updating bounded workspace work plan…".into())
            }
            Command::RespondApproval(_) => panel.set_status("Sending approval response…".into()),
            Command::New => {}
            Command::Send { .. } => panel.set_status("Sending…".into()),
            Command::Stop => panel.set_status("Stopping…".into()),
        }
        if sender.try_send(command).is_err() {
            panel.set_waiting(false);
            panel.set_status("The prototype connection is busy or closed.".into());
        }
    }
}

#[cfg(target_os = "macos")]
fn queue_window_toggle(weak: slint::Weak<Panel>, quick_panel: bool) {
    if let Err(error) = slint::invoke_from_event_loop(move || {
        if let Some(panel) = weak.upgrade() {
            if panel.window().is_visible() && (!quick_panel || panel.get_compact()) {
                if let Err(error) = panel.hide() {
                    eprintln!("Could not hide Quick Panel: {error}");
                }
            } else {
                if quick_panel && !panel.get_compact() {
                    panel.invoke_toggle_shell_mode();
                }
                if let Err(error) = macos_shortcut::activate_application() {
                    eprintln!("Could not activate Quick Panel: {error}");
                }
                if let Err(error) = panel.show() {
                    eprintln!("Could not show Quick Panel: {error}");
                } else if panel.get_compact()
                    || matches!(panel.get_active_page().as_str(), "chat" | "agent")
                {
                    panel.invoke_focus_composer();
                }
            }
        }
    }) {
        eprintln!("Could not queue Quick Panel activation: {error}");
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut args = std::env::args_os().skip(1);
    let mut endpoint = None;
    let mut persistent_profile = false;
    let mut compact = false;
    let mut global_shortcut = false;
    let mut start_hidden = false;
    while let Some(argument) = args.next() {
        if argument == "--socket" && endpoint.is_none() && !persistent_profile {
            endpoint = Some(PathBuf::from(
                args.next().ok_or("--socket requires a path")?,
            ));
        } else if argument == "--persistent-profile" && endpoint.is_none() && !persistent_profile {
            persistent_profile = true;
        } else if argument == "--panel" {
            compact = true;
        } else if argument == "--global-shortcut" && !global_shortcut {
            global_shortcut = true;
        } else if argument == "--start-hidden" && !start_hidden {
            start_hidden = true;
        } else {
            return Err(
                "usage: sentinel-slint-prototype (--socket PATH | --persistent-profile) [--panel [--global-shortcut [--start-hidden]]]"
                    .into(),
            );
        }
    }
    let endpoint = if persistent_profile {
        sentinel_ipc::persistent_rust_profile_path()?.join("daemon-dev.sock")
    } else {
        endpoint.ok_or("select --socket PATH or --persistent-profile")?
    };

    if start_hidden && !global_shortcut {
        return Err("--start-hidden requires --global-shortcut".into());
    }
    if global_shortcut && !compact {
        return Err("--global-shortcut requires --panel".into());
    }
    #[cfg(not(target_os = "macos"))]
    if global_shortcut {
        return Err("global shortcut is unavailable on this platform".into());
    }
    let panel = Panel::new()?;
    panel.set_compact(compact);
    let (sender, commands) = sync_channel(4);
    let weak = panel.as_weak();
    let queue = sender.clone();
    let sizes = std::cell::Cell::new([
        slint::LogicalSize::new(1160.0, 780.0),
        slint::LogicalSize::new(580.0, 500.0),
    ]);
    panel.on_toggle_shell_mode(move || {
        if let Some(panel) = weak.upgrade() {
            let window = panel.window();
            let was_compact = panel.get_compact();
            let mut previous = sizes.get();
            let current = window.size().to_logical(window.scale_factor());
            if current.width > 0.0 && current.height > 0.0 {
                previous[usize::from(was_compact)] = current;
                sizes.set(previous);
            }
            panel.set_compact(!was_compact);
            window.set_size(previous[usize::from(!was_compact)]);
            if was_compact && panel.get_agent_mode() {
                submit(&weak, &queue, Command::Navigate("agent".into()));
            }
            if !was_compact || matches!(panel.get_active_page().as_str(), "chat" | "agent") {
                panel.invoke_focus_composer();
            }
        }
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_navigate(move |page| submit(&weak, &queue, Command::Navigate(page.to_string())));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_theme(move |index| submit(&weak, &queue, Command::SetTheme(index)));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_save_response_profile(move |value| {
        submit(&weak, &queue, Command::SaveResponseProfile(value.into()))
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_refresh_provider_diagnostics(move || {
        submit(&weak, &queue, Command::RefreshProviderDiagnostics)
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_refresh_settings_status(move || submit(&weak, &queue, Command::RefreshSettingsStatus));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_dev_workspace_access(move |enabled| {
        submit(&weak, &queue, Command::SetDevWorkspaceAccess(enabled))
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_export_dev_backup(move |domain, path| {
        submit(&weak, &queue, Command::ExportDevBackup(domain, path.into()))
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_refresh_memory(move || submit(&weak, &queue, Command::RefreshMemory));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_select_memory(move |index| {
        if let Ok(index) = usize::try_from(index) {
            submit(&weak, &queue, Command::SelectMemory(index));
        }
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_new_memory(move || submit(&weak, &queue, Command::NewMemory));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_save_memory(move || submit(&weak, &queue, Command::SaveMemory));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_delete_memory(move || submit(&weak, &queue, Command::DeleteMemory));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_load_more_memory(move || submit(&weak, &queue, Command::LoadMoreMemory));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_regenerate_latest(move || submit(&weak, &queue, Command::RegenerateLatest));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_retry_latest(move || submit(&weak, &queue, Command::RetryLatest));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_edit_latest(move |text| submit(&weak, &queue, Command::EditLatest(text.into())));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_reconnect(move || submit(&weak, &queue, Command::Reconnect));
    let weak = panel.as_weak();
    panel.on_quit_app(move || {
        if global_shortcut {
            if let Some(panel) = weak.upgrade() {
                let _ = panel.hide();
            }
        } else {
            let _ = slint::quit_event_loop();
        }
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_refresh_sessions(move || submit(&weak, &queue, Command::Refresh));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_select_session(move |index| {
        if let Ok(index) = usize::try_from(index) {
            if let Some(panel) = weak.upgrade() {
                panel.invoke_focus_composer();
            }
            submit(&weak, &queue, Command::Select(index));
        }
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_rename_session(move |title| {
        submit(&weak, &queue, Command::RenameSession(title.into()))
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_session_flag(move |id, action| {
        submit(
            &weak,
            &queue,
            Command::SetSessionFlag {
                id: id.into(),
                action: action.into(),
            },
        );
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_delete_session(move |confirmation| {
        submit(&weak, &queue, Command::DeleteSession(confirmation.into()))
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_refresh_models(move || submit(&weak, &queue, Command::RefreshModels));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_save_provider_endpoint(move |provider, endpoint| {
        submit(
            &weak,
            &queue,
            Command::SaveProviderEndpoint {
                provider: provider.into(),
                endpoint: endpoint.into(),
            },
        );
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_select_local_provider(move |index| {
        if let Ok(index) = usize::try_from(index) {
            submit(&weak, &queue, Command::SelectProvider(index));
        }
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_select_model(move |index| {
        if let Ok(index) = usize::try_from(index) {
            submit(&weak, &queue, Command::SelectModel(index));
        }
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_refresh_workspace_files(move || submit(&weak, &queue, Command::RefreshWorkspaceFiles));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_refresh_attachment_files(move || {
        submit(&weak, &queue, Command::RefreshAttachmentFiles)
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_select_attachment_candidate(move |index| {
        if let Ok(index) = usize::try_from(index) {
            submit(&weak, &queue, Command::SelectAttachmentCandidate(index));
        }
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_add_attachment(move || submit(&weak, &queue, Command::AddAttachment));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_select_attachment(move |index| {
        if let Ok(index) = usize::try_from(index) {
            submit(&weak, &queue, Command::SelectAttachment(index));
        }
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_remove_attachment(move || submit(&weak, &queue, Command::RemoveAttachment));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_clear_attachments(move || submit(&weak, &queue, Command::ClearAttachments));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_refresh_workspace_changes(move || {
        submit(&weak, &queue, Command::RefreshWorkspaceChanges)
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_select_workspace(move |index| {
        if let Ok(index) = usize::try_from(index) {
            submit(&weak, &queue, Command::SelectWorkspace(index));
        }
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_refresh_extensions(move || submit(&weak, &queue, Command::RefreshExtensions));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_refresh_ollama_helper(move || submit(&weak, &queue, Command::RefreshOllamaHelper));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_pull_ollama_model(move |model| {
        submit(&weak, &queue, Command::PullOllamaModel(model.into()))
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_cancel_ollama_pull(move || submit(&weak, &queue, Command::CancelOllamaPull));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_remove_ollama_model(move |model, confirmation| {
        submit(
            &weak,
            &queue,
            Command::RemoveOllamaModel {
                model: model.into(),
                confirmation: confirmation.into(),
            },
        )
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_agent_mode(move |enabled| {
        submit(&weak, &queue, Command::SetAgentMode(enabled));
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_agent_directory_mode(move |enabled| {
        submit(&weak, &queue, Command::SetAgentDirectoryMode(enabled));
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_agent_create_mode(move |enabled| {
        submit(&weak, &queue, Command::SetAgentCreateMode(enabled));
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_agent_include_hidden(move |enabled| {
        submit(&weak, &queue, Command::SetAgentIncludeHidden(enabled));
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_agent_process_mode(move |enabled| {
        submit(&weak, &queue, Command::SetAgentProcessMode(enabled))
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_agent_process_program(move |index| {
        submit(&weak, &queue, Command::SetAgentProcessProgram(index))
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_agent_classify_mode(move |enabled| {
        submit(&weak, &queue, Command::SetAgentClassifyMode(enabled))
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_agent_search_mode(move |enabled| {
        submit(&weak, &queue, Command::SetAgentSearchMode(enabled))
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_agent_edit_mode(move |enabled| {
        submit(&weak, &queue, Command::SetAgentEditMode(enabled))
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_agent_glob_mode(move |enabled| {
        submit(&weak, &queue, Command::SetAgentGlobMode(enabled))
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_agent_read_plan_mode(move |enabled| {
        submit(&weak, &queue, Command::SetAgentReadPlanMode(enabled))
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_set_agent_work_plan_mode(move |enabled| {
        submit(&weak, &queue, Command::SetAgentWorkPlanMode(enabled))
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_refresh_agent_history(move || submit(&weak, &queue, Command::RefreshAgentHistory));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_select_agent_run(move |index| {
        if let Ok(index) = usize::try_from(index) {
            submit(&weak, &queue, Command::LoadAgentRun(index));
        }
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_load_older_agent_runs(move || submit(&weak, &queue, Command::LoadOlderAgentRuns));
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_respond_approval(move |allow| {
        submit(&weak, &queue, Command::RespondApproval(allow));
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_new_session(move || {
        if let Some(panel) = weak.upgrade() {
            panel.invoke_focus_composer();
        }
        submit(&weak, &queue, Command::New)
    });
    let weak = panel.as_weak();
    let queue = sender.clone();
    panel.on_send(move |text| {
        if let Some(panel) = weak.upgrade() {
            submit(
                &weak,
                &queue,
                Command::Send {
                    text: text.into(),
                    agent: panel.get_agent_mode(),
                },
            );
        }
    });
    let weak = panel.as_weak();
    panel.on_stop(move || submit(&weak, &sender, Command::Stop));
    let updates = PanelUpdates {
        panel: panel.as_weak(),
        pending: Arc::default(),
    };
    std::thread::Builder::new()
        .name("sentinel-prototype-ipc".into())
        .spawn(move || worker(endpoint, commands, updates))?;
    panel.invoke_focus_composer();
    #[cfg(target_os = "macos")]
    if global_shortcut {
        // Showing first initializes the native application and window.
        panel.show()?;
        panel.invoke_focus_composer();
        let weak = panel.as_weak();
        let _shortcut = macos_shortcut::Shortcut::register(move || {
            queue_window_toggle(weak.clone(), true);
        })?;
        let weak = panel.as_weak();
        let _tray = macos_tray::Tray::register(move |action| match action {
            macos_tray::Action::Toggle => queue_window_toggle(weak.clone(), false),
            macos_tray::Action::Quit => {
                if let Err(error) = slint::invoke_from_event_loop(|| {
                    let _ = slint::quit_event_loop();
                }) {
                    eprintln!("Could not queue Quick Panel exit: {error}");
                }
            }
        })?;
        panel
            .window()
            .on_close_requested(|| slint::CloseRequestResponse::HideWindow);
        if start_hidden {
            panel.hide()?;
        }
        slint::run_event_loop_until_quit()?;
        return Ok(());
    }
    panel.run()?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use sentinel_ipc::{MessageType, Version, contract};
    use serde_json::json;
    use std::io::{BufRead, BufReader, Write};
    use std::sync::atomic::{AtomicU64, Ordering};

    #[cfg(unix)]
    fn mock_server(
        responder: impl Fn(&Envelope) -> (MessageType, Value) + Send + 'static,
    ) -> (PathBuf, std::thread::JoinHandle<()>) {
        use std::os::unix::net::UnixListener;
        static NEXT_SOCKET: AtomicU64 = AtomicU64::new(0);
        let path = std::env::temp_dir().join(format!(
            "sentinel-slint-ipc-test-{}-{}.sock",
            std::process::id(),
            NEXT_SOCKET.fetch_add(1, Ordering::Relaxed)
        ));
        let listener = UnixListener::bind(&path).unwrap();
        let handle = std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            let mut reader = BufReader::new(stream.try_clone().unwrap());
            loop {
                let mut line = String::new();
                if reader.read_line(&mut line).unwrap() == 0 {
                    break;
                }
                let request: Envelope = serde_json::from_str(&line).unwrap();
                let (kind, payload) = responder(&request);
                let response = Envelope {
                    version: Version {
                        major: contract::MAJOR,
                        minor: contract::MINOR,
                    },
                    kind,
                    id: request.id,
                    name: request.name,
                    payload,
                };
                serde_json::to_writer(&mut stream, &response).unwrap();
                stream.write_all(b"\n").unwrap();
            }
        });
        (path, handle)
    }

    #[cfg(unix)]
    fn mock_client(
        responder: impl Fn(&Envelope) -> (MessageType, Value) + Send + 'static,
    ) -> (Client, std::thread::JoinHandle<()>, PathBuf) {
        let (path, handle) = mock_server(responder);
        let client = Client::connect(&path, "slint-test").unwrap();
        (client, handle, path)
    }

    fn event(name: &str, run: &str, generation: &str, sequence: u64, text: &str) -> Envelope {
        Envelope {
            version: Version {
                major: contract::MAJOR,
                minor: contract::MINOR,
            },
            kind: MessageType::Event,
            id: String::new(),
            name: name.into(),
            payload: json!({
                "session_id": "session",
                "run_id": run,
                "server_generation": generation,
                "event_sequence": sequence,
                "text": text,
            }),
        }
    }

    #[test]
    fn old_run_generation_and_duplicate_deltas_are_ignored() {
        let mut session = Session {
            id: "session".into(),
            run: "current".into(),
            generation: Some("generation".into()),
            sequence: Some(10),
        };
        let mut view = View {
            running: true,
            ..View::default()
        };
        for (run, generation, sequence) in [
            ("older", "generation", 11),
            ("current", "prior", 11),
            ("current", "generation", 10),
        ] {
            assert!(!receive_event(
                &mut session,
                &mut view,
                event("output.delta", run, generation, sequence, "stale")
            ));
        }
        assert!(receive_event(
            &mut session,
            &mut view,
            event("output.delta", "current", "generation", 11, "partial")
        ));
        assert!(receive_event(
            &mut session,
            &mut view,
            event("run.completed", "current", "generation", 12, "final")
        ));
        assert!(!view.running);
        assert_eq!(view.response, "final");
    }

    #[test]
    fn preview_bound_preserves_utf8_and_terminal_text_replaces_it() {
        let mut view = View::default();
        let start = "x".repeat(RESPONSE_LIMIT - 1);
        append_response(&mut view, &start);
        append_response(&mut view, "ğ");
        append_response(&mut view, "z");
        assert!(view.truncated);
        assert_eq!(view.response, start);
        replace_response(&mut view, "tümü");
        assert!(!view.truncated);
        assert_eq!(view.response, "tümü");
    }

    #[cfg(unix)]
    #[test]
    fn model_picker_loads_current_selects_by_identity_and_blocks_active_run() {
        let selected = Arc::new(Mutex::new(("provider-b".to_owned(), "model-2".to_owned())));
        let selected_for_server = Arc::clone(&selected);
        let (mut client, server, path) = mock_client(move |request| {
            let response = match request.name.as_str() {
                "hello" => json!({"major":1,"minor":1,"daemon_version":"test","capabilities":[]}),
                "model.list" => json!({"models":[
                    {"provider_id":"provider-a","model_id":"model-1"},
                    {"provider_id":"provider-b","model_id":"model-2"}
                ]}),
                "model.current" => {
                    let current = selected_for_server.lock().unwrap();
                    json!({"provider_id":current.0,"model_id":current.1})
                }
                "model.select" => {
                    assert_eq!(request.payload["provider_id"], "provider-a");
                    assert_eq!(request.payload["model_id"], "model-1");
                    *selected_for_server.lock().unwrap() =
                        ("provider-a".to_owned(), "model-1".to_owned());
                    json!({"provider_id":"provider-a","model_id":"model-1"})
                }
                other => panic!("unexpected request {other}"),
            };
            (MessageType::Response, response)
        });
        let mut view = View::default();
        refresh_models(&mut client, &mut view).unwrap();
        assert_eq!(view.models.len(), 2);
        assert_eq!(view.selected_provider, "provider-b");
        assert_eq!(view.selected_model, "model-2");
        assert_eq!(view.model_status, "Saved selection: provider-b / model-2");

        select_model(&mut client, 0, &mut view).unwrap();
        assert_eq!(view.selected_provider, "provider-a");
        assert_eq!(view.selected_model, "model-1");
        assert_eq!(view.model_status, "Saved selection: provider-a / model-1");

        // Re-querying model.current after reconnect must restore the daemon-persisted identity.
        refresh_models(&mut client, &mut view).unwrap();
        assert_eq!(view.selected_provider, "provider-a");
        assert_eq!(view.selected_model, "model-1");
        assert_eq!(view.model_status, "Saved selection: provider-a / model-1");

        view.running = true;
        assert!(matches!(
            select_model(&mut client, 1, &mut view),
            Err(Error::Remote(code)) if code == "runtime-busy"
        ));
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[cfg(unix)]
    #[test]
    fn extension_catalog_is_capability_gated_bounded_read_only_metadata() {
        let (mut client, server, path) = mock_client(|request| match request.name.as_str() {
            "hello" => (
                MessageType::Response,
                json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["plugin-catalog"]}),
            ),
            "desktop.settings_service" => {
                assert_eq!(request.payload["action"], "extensionsHubState");
                assert_eq!(request.payload["arguments"], json!([]));
                (
                    MessageType::Response,
                    json!({"result":{
                        "extensionCount":1,"toolCount":0,"truncated":true,
                        "entries":[{
                            "id":"plugin:demo","type":"plugins","name":"Demo extension",
                            "version":"1.2","enabledKnown":false,"available":false,
                            "health":"Disconnected","maturity":"Unavailable",
                            "hostRunning":false,"actions":[],
                            "sourcePath":"/private/secret/plugin.json","credential":"never-render"
                        }],
                        "toolEntries":[]
                    }}),
                )
            }
            other => panic!("unexpected request {other}"),
        });
        let mut view = View::default();
        apply_capabilities(&["plugin-catalog".into()], &mut view);
        assert!(view.extension_catalog_capable);
        refresh_extension_catalog(&mut client, &mut view).unwrap();
        assert!(view.extension_catalog_view.contains("Demo extension"));
        assert!(view.extension_catalog_view.contains("version 1.2"));
        assert!(view.extension_catalog_view.contains("not available"));
        assert!(!view.extension_catalog_view.contains("/private/secret"));
        assert!(!view.extension_catalog_view.contains("never-render"));
        assert!(view.extension_catalog_truncated);
        assert!(
            view.extension_catalog_status
                .contains("1 extensions and 0 tools")
        );

        let mut unavailable = View::default();
        assert!(matches!(
            refresh_extension_catalog(&mut client, &mut unavailable),
            Err(Error::Remote(code)) if code == "unsupported-capability"
        ));
        assert!(!unavailable.extension_catalog_capable);
        assert!(unavailable.extension_catalog_view.is_empty());
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[cfg(unix)]
    #[test]
    fn model_picker_surfaces_catalog_failure() {
        let (mut client, server, path) = mock_client(|request| match request.name.as_str() {
            "hello" => (
                MessageType::Response,
                json!({"major":1,"minor":1,"daemon_version":"test","capabilities":[]}),
            ),
            "model.list" => (MessageType::Error, json!({"code":"provider-unavailable"})),
            other => panic!("unexpected request {other}"),
        });
        let mut view = View {
            models: vec![("old-provider".into(), "old-model".into(), "old".into())],
            selected_provider: "old-provider".into(),
            selected_model: "old-model".into(),
            ..View::default()
        };
        assert!(refresh_models(&mut client, &mut view).is_err());
        assert!(view.model_status.starts_with("Models unavailable:"));
        assert!(view.models.is_empty());
        assert!(view.selected_provider.is_empty());
        assert!(view.selected_model.is_empty());
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn agent_mode_is_disabled_without_advertised_capability_and_restricted_mode_is_explicit() {
        let mut view = View::default();
        apply_capabilities(&[], &mut view);
        assert!(!view.agent_capable);
        assert!(set_agent_mode(&mut view, true).is_err());
        assert!(!view.agent_mode);
        assert!(view.agent_status.contains("did not advertise"));

        apply_capabilities(&["agent-history".into()], &mut view);
        assert!(
            !view.agent_capable,
            "audit history must not imply runnable Agent support"
        );
        assert!(
            view.agent_history_capable,
            "audit history remains independently available"
        );
        assert!(set_agent_mode(&mut view, true).is_err());

        let capabilities = ["agent-inspect-files".to_owned()];
        apply_capabilities(&capabilities, &mut view);
        assert!(view.agent_capable);
        assert!(view.agent_restricted);
        assert!(view.agent_multi_file);
        assert!(view.agent_label.contains("read-only"));
        assert!(
            view.agent_status
                .contains("up to 4 relative workspace paths")
        );
        assert_eq!(
            restricted_agent_request("docs/readme.md\nsrc/main.rs", true, false, false, false, "")
                .unwrap(),
            r#"{"operation":"inspect-files","paths":["docs/readme.md","src/main.rs"]}"#
        );
        assert!(restricted_agent_request("a\nb\nc\nd", true, false, false, false, "").is_ok());
        for invalid in ["", "a\na", "../outside", "/absolute", "a\nb\nc\nd\ne"] {
            assert!(
                restricted_agent_request(invalid, true, false, false, false, "").is_err(),
                "accepted {invalid:?}"
            );
        }
        assert!(
            restricted_agent_request(&"x".repeat(1025), true, false, false, false, "").is_err()
        );
        assert!(restricted_agent_request("a\0b", true, false, false, false, "").is_err());
        assert_eq!(
            restricted_agent_request("docs/readme.md", false, false, false, false, "").unwrap(),
            r#"{"operation":"inspect-file","path":"docs/readme.md"}"#
        );
        assert_eq!(
            restricted_agent_request(".", true, true, false, false, "").unwrap(),
            r#"{"includeHidden":false,"operation":"inspect-directory","path":"."}"#
        );
        assert_eq!(
            restricted_agent_request("src", true, true, false, true, "").unwrap(),
            r#"{"includeHidden":true,"operation":"inspect-directory","path":"src"}"#
        );
        assert!(restricted_agent_request("a\nb", true, true, false, false, "").is_err());
        apply_capabilities(&["agent-inspect-directory".to_owned()], &mut view);
        assert!(view.agent_capable);
        assert!(view.agent_directory_capable);
        assert!(!view.agent_multi_file);
        assert!(set_agent_directory_mode(&mut view, true).is_ok());
        assert!(set_agent_include_hidden(&mut view, true).is_ok());
        assert!(
            restricted_agent_request(
                ".",
                false,
                view.agent_directory_mode,
                view.agent_create_mode,
                view.agent_include_hidden,
                ""
            )
            .is_ok()
        );
        assert!(set_agent_directory_mode(&mut view, false).is_ok());
        assert!(!view.agent_include_hidden);
        apply_capabilities(&["agent-create-file".to_owned()], &mut view);
        assert!(view.agent_create_capable);
        assert!(view.approval_capable);
        assert!(set_agent_mode(&mut view, true).is_ok());
        assert!(set_agent_create_mode(&mut view, true).is_ok());
        assert_eq!(
            restricted_agent_request("notes/new.md", false, false, true, false, "safe text")
                .unwrap(),
            r#"{"content":"safe text","operation":"create-file","path":"notes/new.md"}"#
        );
        assert!(
            restricted_agent_request("notes/new.md", false, false, true, false, &"x".repeat(2049))
                .is_err()
        );
        assert!(
            restricted_agent_request("notes/new.md", false, false, true, false, "a\0b").is_err()
        );
    }

    #[test]
    fn classified_inspect_is_capability_gated_and_requires_approval_projection() {
        let mut view = View::default();
        apply_capabilities(&["agent".into()], &mut view);
        assert!(!view.agent_classify_capable);
        assert!(!view.approval_capable);
        assert!(set_agent_classify_mode(&mut view, true).is_err());

        apply_capabilities(
            &["agent".into(), "agent-classify-inspect".into()],
            &mut view,
        );
        assert!(view.agent_capable);
        assert!(view.agent_restricted);
        assert!(view.agent_classify_capable);
        assert!(view.approval_capable);
        assert!(set_agent_mode(&mut view, true).is_ok());
        assert!(set_agent_classify_mode(&mut view, true).is_ok());
        assert_eq!(
            serde_json::from_str::<Value>(
                &classified_inspect_request("Summarize the design").unwrap()
            )
            .unwrap(),
            serde_json::json!({
                "operation": "classify-inspect",
                "goal": "Summarize the design"
            })
        );
        assert!(view.agent_status.contains("coverage is not assessed"));
        for invalid in ["", " \n ", "a\0b"] {
            assert!(classified_inspect_request(invalid).is_err());
        }
        assert!(classified_inspect_request(&"x".repeat(2049)).is_err());
        assert!(classified_inspect_request(&"é".repeat(1025)).is_err());

        set_agent_classify_mode(&mut view, false).unwrap();
        assert!(set_agent_mode(&mut view, true).is_ok());
        assert!(!view.agent_classify_mode);
    }

    #[test]
    fn workspace_snapshot_is_capability_gated_bounded_and_quotes_paths() {
        let mut view = View::default();
        apply_capabilities(&[], &mut view);
        assert!(!view.workspace_files_capable);
        assert!(view.workspace_files_status.contains("not advertised"));
        apply_capabilities(&["workspace-files".into()], &mut view);
        assert!(view.workspace_files_capable);

        let (root, rendered, truncated, paths) = workspace_files_projection(&serde_json::json!({
            "root": "/tmp/authorized",
            "files": [
                "/tmp/authorized/src/main.rs",
                "/tmp/authorized/nested/line\nbreak.rs"
            ],
            "truncated": true
        }))
        .unwrap();
        assert_eq!(root, "\"/tmp/authorized\"");
        assert!(rendered.contains("\"src/main.rs\""));
        assert!(rendered.contains("line\\nbreak.rs"));
        assert!(rendered.contains("omitted paths may exist"));
        assert!(truncated);
        assert_eq!(paths, ["src/main.rs", "nested/line\nbreak.rs"]);

        assert!(
            workspace_files_projection(&serde_json::json!({
                "root": "/tmp/authorized",
                "files": ["/tmp/authorized-elsewhere/private.txt"],
                "truncated": false
            }))
            .is_err()
        );
        assert!(
            workspace_files_projection(&serde_json::json!({
                "root": "/tmp/authorized",
                "files": ["relative.txt"],
                "truncated": false
            }))
            .is_err()
        );
        let hundred_paths = (0..101)
            .map(|index| format!("/tmp/authorized/file-{index}.txt"))
            .collect::<Vec<_>>();
        let (_, rendered, truncated, _) = workspace_files_projection(&serde_json::json!({
            "root": "/tmp/authorized",
            "files": hundred_paths,
            "truncated": false
        }))
        .unwrap();
        assert!(rendered.contains("100 regular files"));
        assert!(rendered.contains("file-99.txt"));
        assert!(!rendered.contains("file-100.txt"));
        assert!(truncated);
    }

    #[test]
    fn attachment_selection_is_limited_to_snapshot_and_chat_collection() {
        let mut view = View {
            attachment_files: vec!["notes.txt".into(), "src/main.rs".into()],
            attachment_files_session_id: "session-a".into(),
            attachment_files_root: "/tmp/workspace".into(),
            attachment_files_workspace_id: "default".into(),
            ..View::default()
        };
        assert!(add_attachment(&mut view).is_ok());
        assert_eq!(view.attachments, ["notes.txt"]);
        assert!(add_attachment(&mut view).is_ok());
        assert_eq!(
            view.attachments.len(),
            1,
            "duplicate selection is idempotent"
        );
        view.selected_attachment_candidate = 1;
        add_attachment(&mut view).unwrap();
        assert_eq!(view.attachments, ["notes.txt", "src/main.rs"]);
        view.selected_attachment = 0;
        remove_attachment(&mut view).unwrap();
        assert_eq!(view.attachments, ["src/main.rs"]);

        view.attachments = vec!["one".into(), "two".into(), "three".into(), "four".into()];
        view.attachment_files = view.attachments.clone();
        view.attachment_files.push("five".into());
        view.selected_attachment_candidate = 4;
        assert!(add_attachment(&mut view).is_err());
        assert_eq!(view.attachments.len(), 4);
        clear_attachments(&mut view, "cleared");
        assert!(view.attachments.is_empty());
        assert!(view.attachment_files.is_empty());
        assert_eq!(view.attachment_status, "cleared");
    }

    #[cfg(unix)]
    #[test]
    fn chat_send_uses_only_selected_workspace_relative_attachments() {
        let (mut client, server, path) = mock_client(|request| match request.name.as_str() {
            "hello" => (
                MessageType::Response,
                json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["workspace-files"]}),
            ),
            "terminal.state" => (
                MessageType::Response,
                json!({"properties":{},"providers":[],"workspace":{"id":"default","root":"/tmp/workspace"},"workspaces":[]}),
            ),
            "chat.send" => {
                assert_eq!(request.payload["attachments"], json!(["notes.txt"]));
                assert_eq!(request.payload["session_id"], "session-a");
                (
                    MessageType::Response,
                    json!({"session_id":"session-a","run_id":"run-a"}),
                )
            }
            other => panic!("unexpected request {other}"),
        });
        let mut session = Session {
            id: "session-a".into(),
            ..Session::default()
        };
        let mut view = View {
            attachment_files: vec!["notes.txt".into()],
            attachment_files_session_id: "session-a".into(),
            attachment_files_root: "/tmp/workspace".into(),
            attachment_files_workspace_id: "default".into(),
            attachments: vec!["notes.txt".into()],
            ..View::default()
        };
        send(
            &mut client,
            &mut session,
            &mut view,
            "Summarize the note".into(),
        )
        .unwrap();
        assert_eq!(session.run, "run-a");
        assert!(
            view.attachments.is_empty(),
            "accepted attachments are not silently reused"
        );
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn workspace_changes_distinguish_unknown_baseline_and_truncation() {
        let (root, unknown, status, truncated) = workspace_changes_projection(&serde_json::json!({
            "available": false,
            "files": [],
            "truncated": false,
            "reason": "daemon-restarted"
        }))
        .unwrap();
        assert!(root.is_empty());
        assert!(unknown.contains("state is unknown"));
        assert!(unknown.contains("does not mean it is clean"));
        assert!(unknown.contains("daemon-restarted"));
        assert!(status.contains("unknown"));
        assert!(!truncated);

        let (root, changes, status, truncated) = workspace_changes_projection(&serde_json::json!({
            "available": true,
            "root": "/tmp/authorized",
            "files": [
                {"path":"/tmp/authorized/a.txt","state":"Applied","kind":"modified","diff":"@@ -1 +1 @@\n-old\n+new"},
                {"path":"/tmp/authorized/new\\nfile.txt","state":"Applied","kind":"created","diff":"+created"}
            ],
            "truncated": true,
            "reason": null
        }))
        .unwrap();
        assert_eq!(root, "\"/tmp/authorized\"");
        assert!(changes.contains("modified · \"a.txt\""));
        assert!(changes.contains("created · \"new\\\\nfile.txt\""));
        assert!(changes.contains("deletions are not inferred"));
        assert!(changes.contains("omitted changes may exist"));
        assert!(status.contains("truncated"));
        assert!(truncated);

        assert!(workspace_changes_projection(&serde_json::json!({
            "available": true,
            "root": "/tmp/authorized",
            "files": [{"path":"/tmp/elsewhere/a.txt","state":"Applied","kind":"modified","diff":"+x"}],
            "truncated": false
        }))
        .is_err());
    }

    #[cfg(unix)]
    #[test]
    fn workspace_snapshot_requests_only_selected_session_and_projects_response() {
        let (mut client, server, path) = mock_client(|request| match request.name.as_str() {
            "hello" => (
                MessageType::Response,
                json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["workspace-files"]}),
            ),
            "terminal.state" => (
                MessageType::Response,
                json!({"properties":{},"providers":[],"workspace":{"id":"default","root":"/tmp/authorized"},"workspaces":[]}),
            ),
            "workspace.files" => {
                assert_eq!(request.payload["session_id"], "session-7");
                (
                    MessageType::Response,
                    json!({
                        "root":"/tmp/authorized",
                        "files":["/tmp/authorized/readme.md","/tmp/authorized/nested/odd\nname.txt"],
                        "truncated":false
                    }),
                )
            }
            other => panic!("unexpected workspace request: {other}"),
        });
        let mut view = View::default();
        apply_capabilities(client.capabilities(), &mut view);
        let session = Session {
            id: "session-7".into(),
            ..Session::default()
        };
        refresh_workspace_files(&mut client, &session, &mut view).unwrap();
        assert_eq!(view.attachment_files, ["readme.md", "nested/odd\nname.txt"]);
        assert_eq!(view.attachment_files_session_id, "session-7");
        assert_eq!(view.attachment_files_root, "/tmp/authorized");
        assert_eq!(view.workspace_files_root, "\"/tmp/authorized\"");
        assert!(view.workspace_files_view.contains("readme.md"));
        assert!(view.workspace_files_view.contains("odd\\nname.txt"));
        assert_eq!(view.workspace_files_status, "Snapshot loaded.");
        assert!(!view.workspace_files_truncated);
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[cfg(unix)]
    #[test]
    fn workspace_changes_request_preserves_unknown_baseline_state() {
        let (mut client, server, path) = mock_client(|request| match request.name.as_str() {
            "hello" => (
                MessageType::Response,
                json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["workspace-changes"]}),
            ),
            "workspace.changes" => {
                assert_eq!(request.payload["session_id"], "session-9");
                (
                    MessageType::Response,
                    json!({
                        "available":false,
                        "files":[],
                        "truncated":false,
                        "root":null,
                        "reason":"daemon-restarted"
                    }),
                )
            }
            other => panic!("unexpected workspace changes request: {other}"),
        });
        let mut view = View::default();
        apply_capabilities(client.capabilities(), &mut view);
        assert!(view.workspace_changes_capable);
        let session = Session {
            id: "session-9".into(),
            ..Session::default()
        };
        refresh_workspace_changes(&mut client, &session, &mut view).unwrap();
        assert!(view.workspace_changes_view.contains("unknown"));
        assert!(view.workspace_changes_view.contains("daemon-restarted"));
        assert!(!view.workspace_changes_view.contains("clean workspace"));
        assert!(view.workspace_changes_status.contains("unknown"));
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[cfg(unix)]
    #[test]
    fn workspace_selection_requeries_authoritative_state_after_ack_and_refusal() {
        let current = std::sync::Arc::new(Mutex::new("workspace-a".to_owned()));
        let server_current = std::sync::Arc::clone(&current);
        let (mut client, server, path) = mock_client(move |request| {
            let current_workspace = || {
                let id = server_current.lock().unwrap().clone();
                let root = if id == "workspace-a" {
                    "/tmp/workspace-a"
                } else {
                    "/tmp/workspace-b"
                };
                json!({
                    "properties": {},
                    "providers": [],
                    "workspace":{"id":id,"name":id,"root":root,"access":"read","permission":"workspace"},
                    "workspaces":[
                        {"id":"workspace-a","name":"Alpha","root":"/tmp/workspace-a","access":"read","permission":"workspace","archived":false},
                        {"id":"workspace-b","name":"Beta","root":"/tmp/workspace-b","access":"read","permission":"workspace","archived":false}
                    ]
                })
            };
            match request.name.as_str() {
                "hello" => (
                    MessageType::Response,
                    json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["workspace-selection"]}),
                ),
                "terminal.state" => (MessageType::Response, current_workspace()),
                "workspace.select" if request.payload["workspace_id"] == "workspace-b" => {
                    *server_current.lock().unwrap() = "workspace-b".into();
                    (MessageType::Response, json!({"workspace_id":"workspace-b"}))
                }
                "workspace.select" => (MessageType::Error, json!({"code":"runtime-busy"})),
                other => panic!("unexpected workspace selection request: {other}"),
            }
        });
        let mut view = View::default();
        apply_capabilities(client.capabilities(), &mut view);
        refresh_workspace_state(&mut client, &mut view).unwrap();
        assert_eq!(view.selected_workspace_id, "workspace-a");
        view.workspace_files_view = "stale file snapshot".into();
        view.workspace_changes_view = "stale baseline comparison".into();

        select_workspace(&mut client, &Session::default(), &mut view, 1).unwrap();
        assert_eq!(view.selected_workspace_id, "workspace-b");
        assert!(view.workspace_files_view.is_empty());
        assert!(view.workspace_changes_view.is_empty());
        assert!(view.workspace_selection_status.contains("confirmed"));

        view.workspace_files_view = "stale file snapshot".into();
        view.workspace_changes_view = "stale baseline comparison".into();
        assert!(matches!(
            select_workspace(&mut client, &Session::default(), &mut view, 0),
            Err(Error::Remote(code)) if code == "runtime-busy"
        ));
        assert_eq!(view.selected_workspace_id, "workspace-b");
        assert!(view.workspace_files_view.is_empty());
        assert!(view.workspace_changes_view.is_empty());
        assert!(
            view.workspace_selection_status
                .contains("current selection")
        );
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[cfg(unix)]
    #[test]
    fn restricted_agent_rejects_parent_path_before_dispatch() {
        let (mut client, server, path) = mock_client(|request| match request.name.as_str() {
            "hello" => (
                MessageType::Response,
                json!({
                    "major":1,"minor":1,"daemon_version":"test",
                    "capabilities":["agent","agent-inspect-file"]
                }),
            ),
            other => panic!("unsupported request was dispatched: {other}"),
        });
        let mut view = View::default();
        apply_capabilities(client.capabilities(), &mut view);
        set_agent_mode(&mut view, true).unwrap();
        let mut session = Session::default();
        assert!(matches!(
            send(&mut client, &mut session, &mut view, "../outside".into()),
            Err(Error::Usage(_))
        ));
        assert!(!view.running);
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn restricted_process_ui_emits_only_fixed_argv_and_no_shell() {
        let actual: Value = serde_json::from_str(
            &restricted_process_request("/usr/bin/wc", "-c\nnotes.txt").unwrap(),
        )
        .unwrap();
        assert_eq!(
            actual,
            json!({"operation":"run-process","argv":["/usr/bin/wc","-c","notes.txt"]})
        );
        for forbidden in ["sh", "/bin/sh", "python", "/usr/bin/curl"] {
            assert!(restricted_process_request(forbidden, "-c\necho unsafe").is_err());
        }
        assert!(restricted_process_request("/usr/bin/wc", "-c\n\nnotes.txt").is_err());
        assert!(restricted_process_request("/usr/bin/wc", &"x".repeat(4000)).is_err());
        let mut view = View::default();
        apply_capabilities(
            &["agent-run-process".into(), "agent-history".into()],
            &mut view,
        );
        assert!(view.agent_process_capable);
        assert!(view.agent_history_capable);
        assert!(view.approval_capable);
        assert!(set_agent_process_mode(&mut view, true).is_ok());
        assert!(restricted_process_request("/usr/bin/grep", "pattern\nnotes.txt").is_ok());
        view.running = true;
        assert!(set_agent_process_mode(&mut view, false).is_err());
    }

    #[cfg(unix)]
    #[test]
    fn agent_audit_ui_uses_scoped_history_request_and_renders_run_detail() {
        let (mut client, server, path) = mock_client(|request| {
            let payload = match request.name.as_str() {
                "hello" => {
                    json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["agent-history"]})
                }
                "agent.history" if request.payload["run_id"] == "" => json!({"history":{
                    "runs":[{"runId":"run-2","sessionId":"session","state":"completed","goalSummary":"Inspect notes","cursorTime":"2026-11-19T10:00:00Z"}],"truncated":false
                }}),
                "agent.history" if request.payload["run_id"] == "run-2" => json!({"history":{
                    "run":{"runId":"run-2","state":"completed","finalAnswer":"Observed notes.txt"},
                    "tools":[{"toolId":"read-file","resourceSummary":"notes.txt"}],
                    "authorizations":[{"decision":"allow","scope":"one-invocation"}],
                    "evidence":[{"resourceSummary":"notes.txt","outcome":0}],"truncated":false
                }}),
                other => panic!("unexpected agent history command: {other}"),
            };
            (MessageType::Response, payload)
        });
        let mut view = View {
            agent_history_capable: true,
            ..View::default()
        };
        refresh_agent_history(&mut client, &mut view, false).unwrap();
        assert_eq!(view.agent_selected_run, "run-2");
        assert!(view.agent_history_view.contains("Observed notes.txt"));
        assert!(view.agent_history_view.contains("one-invocation"));
        assert_eq!(view.agent_runs_before_time, "2026-11-19T10:00:00Z");
        drop(client);
        server.join().unwrap();
        std::fs::remove_file(path).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn session_lifecycle_ui_renames_and_requires_exact_delete_confirmation() {
        let (mut client, server, path) = mock_client(|request| {
            let payload = match request.name.as_str() {
                "hello" => {
                    json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["session-management"]})
                }
                "desktop.action" if request.payload["action"] == "renameConversation" => {
                    assert_eq!(request.payload["session_id"], "session-1");
                    assert_eq!(
                        request.payload["arguments"],
                        json!(["session-1", "Renamed"])
                    );
                    json!({"accepted":true,"result":{"value":true}})
                }
                "desktop.action"
                    if request.payload["action"] == "requestPermanentDeleteConversation" =>
                {
                    assert_eq!(request.payload["session_id"], "session-1");
                    assert_eq!(request.payload["arguments"], json!(["session-1"]));
                    json!({"accepted":true,"result":{"value":true}})
                }
                "session.list" => json!({"sessions":[]}),
                other => panic!("unexpected session lifecycle request: {other}"),
            };
            (MessageType::Response, payload)
        });
        let mut view = View {
            session_management_capable: true,
            sessions: vec![("session-1".into(), "Original".into())],
            ..View::default()
        };
        let session = Session {
            id: "session-1".into(),
            ..Session::default()
        };
        rename_session(&mut client, &session, &mut view, "Renamed").unwrap();
        assert_eq!(view.sessions[0].1, "Renamed");
        assert!(
            delete_session(
                &mut client,
                &mut Session {
                    id: "session-1".into(),
                    ..Session::default()
                },
                &mut view,
                "delete"
            )
            .is_err()
        );
        assert_eq!(view.sessions.len(), 1);
        let mut deleted_session = Session {
            id: "session-1".into(),
            ..Session::default()
        };
        delete_session(&mut client, &mut deleted_session, &mut view, "DELETE").unwrap();
        assert!(deleted_session.id.is_empty());
        assert!(view.selected_id.is_empty());
        assert!(view.sessions.is_empty());
        drop(client);
        server.join().unwrap();
        std::fs::remove_file(path).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn latest_user_edit_uses_authoritative_id_and_retains_replacement_overlay() {
        let (mut client, server, path) = mock_client(|request| {
            let payload = match request.name.as_str() {
                "hello" => {
                    json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["chat-revisions"]})
                }
                "chat.edit" => {
                    assert_eq!(request.payload["session_id"], "session-1");
                    assert_eq!(request.payload["message_id"], 42);
                    assert_eq!(request.payload["text"], "corrected prompt");
                    json!({"session_id":"session-1","run_id":"run-edit"})
                }
                other => panic!("unexpected edit request: {other}"),
            };
            (MessageType::Response, payload)
        });
        let mut session = Session {
            id: "session-1".into(),
            ..Session::default()
        };
        let mut view = View {
            chat_revisions_capable: true,
            latest_user_id: 42,
            last_message_id: 55,
            ..View::default()
        };
        edit_latest(&mut client, &mut session, &mut view, "corrected prompt").unwrap();
        assert_eq!(session.run, "run-edit");
        assert!(view.running && view.show_overlay);
        assert_eq!(view.baseline_message_id, 55);
        assert_eq!(view.live_user, "Edited latest message: corrected prompt");
        drop(client);
        server.join().unwrap();
        std::fs::remove_file(path).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn edited_branch_attaches_to_authoritative_session() {
        let (mut client, server, path) = mock_client(|request| {
            let payload = match request.name.as_str() {
                "hello" => {
                    json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["chat-revisions"]})
                }
                "chat.edit" => json!({"session_id":"branch","run_id":"branch-run"}),
                "session.attach" => {
                    assert_eq!(request.payload["session_id"], "branch");
                    json!({"session_id":"branch","run_id":"branch-run","state":"running","output":""})
                }
                "session.messages" => json!({"session_id":"branch","messages":[]}),
                other => panic!("unexpected request {other}"),
            };
            (MessageType::Response, payload)
        });
        let mut session = Session {
            id: "original".into(),
            ..Session::default()
        };
        let mut view = View {
            chat_revisions_capable: true,
            latest_user_id: 1,
            ..View::default()
        };
        edit_latest(&mut client, &mut session, &mut view, "edited").unwrap();
        assert_eq!(session.id, "branch");
        assert_eq!(session.run, "branch-run");
        assert!(view.running);
        drop(client);
        server.join().unwrap();
        std::fs::remove_file(path).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn ollama_model_ui_requires_exact_remove_confirmation_and_uses_helper_contract() {
        let action_count = Arc::new(AtomicU64::new(0));
        let action_count_server = Arc::clone(&action_count);
        let (mut client, server, path) = mock_client(move |request| {
            let payload = match request.name.as_str() {
                "hello" => {
                    json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["ollama-model-operations"]})
                }
                "model.helper_state" => {
                    assert_eq!(request.payload["component"], "ollamaPuller");
                    json!({"component":"ollamaPuller","properties":{"pulling":false,"activeModel":"","progress":0.25,"statusText":"Ready","errorText":""}})
                }
                "model.helper_action" => {
                    assert_eq!(request.payload["component"], "ollamaPuller");
                    assert_eq!(request.payload["action"], "removeModel");
                    assert_eq!(request.payload["value"], "small-model:latest");
                    action_count_server.fetch_add(1, Ordering::Relaxed);
                    json!({"accepted":true})
                }
                other => panic!("unexpected Ollama helper request: {other}"),
            };
            (MessageType::Response, payload)
        });
        let mut view = View {
            ollama_helper_capable: true,
            ..View::default()
        };
        refresh_ollama_helper(&mut client, &mut view).unwrap();
        assert_eq!(view.ollama_helper_status, "Ready");
        assert_eq!(view.ollama_progress, 0.25);
        assert!(
            ollama_model_action(
                &mut client,
                &mut view,
                "removeModel",
                "small-model:latest",
                "wrong"
            )
            .is_err()
        );
        assert_eq!(action_count.load(Ordering::Relaxed), 0);
        view.ollama_remove_confirmation = "small-model:latest".into();
        let confirmation = view.ollama_remove_confirmation.clone();
        ollama_model_action(
            &mut client,
            &mut view,
            "removeModel",
            "small-model:latest",
            &confirmation,
        )
        .unwrap();
        assert_eq!(action_count.load(Ordering::Relaxed), 1);
        assert!(view.ollama_remove_confirmation.is_empty());
        drop(client);
        server.join().unwrap();
        std::fs::remove_file(path).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn advertised_agent_start_projects_approval_and_terminal_events() {
        let (mut client, server, path) = mock_client(|request| {
            let response = match request.name.as_str() {
                "hello" => json!({
                    "major":1,"minor":1,"daemon_version":"test",
                    "capabilities":["agent","approval","events"]
                }),
                "agent.start" => {
                    assert_eq!(request.payload["session_id"], "session");
                    assert_eq!(request.payload["text"], "Inspect the requested file");
                    json!({"session_id":"session","run_id":"agent-run"})
                }
                "approval.respond" => {
                    assert_eq!(request.payload["run_id"], "agent-run");
                    assert_eq!(request.payload["approval_id"], "approval-1");
                    assert_eq!(request.payload["allow"], true);
                    json!({"accepted":true})
                }
                other => panic!("unexpected request {other}"),
            };
            (MessageType::Response, response)
        });
        let mut session = Session {
            id: "session".into(),
            ..Session::default()
        };
        let mut view = View::default();
        apply_capabilities(client.capabilities(), &mut view);
        assert!(set_agent_mode(&mut view, true).is_ok());
        send(
            &mut client,
            &mut session,
            &mut view,
            "Inspect the requested file".into(),
        )
        .unwrap();
        assert!(view.running);
        assert_eq!(session.run, "agent-run");

        let approval = Envelope {
            version: Version {
                major: contract::MAJOR,
                minor: contract::MINOR,
            },
            kind: MessageType::Event,
            id: String::new(),
            name: "approval.requested".into(),
            payload: json!({
                "session_id":"session","run_id":"agent-run",
                "server_generation":"generation","event_sequence":1,
                "approval_id":"approval-1","tool":"read-file",
                "detail":"Read the requested workspace file","risk":0,"state":"approval",
                "resources":[{"resource":"docs/readme.md"}]
            }),
        };
        assert!(receive_event(&mut session, &mut view, approval));
        assert!(view.approval_pending);
        assert!(view.approval_detail.contains("read-file"));
        respond_approval(&mut client, &mut session, &mut view, true).unwrap();
        assert!(!view.approval_pending);
        assert!(view.approval_detail.is_empty());

        let mut terminal = event(
            "run.completed",
            "agent-run",
            "generation",
            2,
            "Read completed.",
        );
        terminal.payload["session_id"] = Value::String("session".into());
        assert!(receive_event(&mut session, &mut view, terminal));
        assert!(!view.running);
        assert_eq!(view.response, "Read completed.");
        let late_approval = Envelope {
            version: Version {
                major: contract::MAJOR,
                minor: contract::MINOR,
            },
            kind: MessageType::Event,
            id: String::new(),
            name: "approval.requested".into(),
            payload: json!({
                "session_id":"session","run_id":"agent-run",
                "server_generation":"generation","event_sequence":3,
                "approval_id":"late","tool":"read-file","detail":"late event",
                "risk":0,"resources":[]
            }),
        };
        assert!(!receive_event(&mut session, &mut view, late_approval));
        assert!(!view.approval_pending);
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[cfg(unix)]
    #[test]
    fn unacknowledged_approval_response_refreshes_state_without_replaying() {
        let approval_responses = std::sync::Arc::new(AtomicU64::new(0));
        let seen = std::sync::Arc::clone(&approval_responses);
        let (mut client, server, path) = mock_client(move |request| match request.name.as_str() {
            "hello" => (
                MessageType::Response,
                json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["approval"]}),
            ),
            "approval.respond" => {
                seen.fetch_add(1, Ordering::Relaxed);
                assert_eq!(request.payload["run_id"], "agent-run");
                assert_eq!(request.payload["approval_id"], "approval-1");
                assert_eq!(request.payload["allow"], true);
                (MessageType::Response, json!({"accepted":false}))
            }
            "session.attach" => (
                MessageType::Response,
                json!({
                    "session_id":"session","run_id":"agent-run","state":"approval",
                    "output":"","approval_id":"approval-1",
                    "approval":{"tool":"read-file","detail":"Read a file","risk":0,
                        "resources":[{"resource":"notes.txt"}]}
                }),
            ),
            other => panic!("unexpected request {other}"),
        });
        let mut session = Session {
            id: "session".into(),
            run: "agent-run".into(),
            ..Session::default()
        };
        let mut view = View {
            approval_capable: true,
            approval_pending: true,
            approval_id: "approval-1".into(),
            approval_detail: "old approval detail".into(),
            running: true,
            ..View::default()
        };
        respond_approval(&mut client, &mut session, &mut view, true).unwrap();
        assert!(view.approval_pending);
        assert_eq!(view.approval_id, "approval-1");
        assert!(view.approval_detail.contains("notes.txt"));
        assert!(view.status.contains("No response was retried"));
        assert_eq!(approval_responses.load(Ordering::Relaxed), 1);

        drop(client);
        server.join().unwrap();
        assert_eq!(approval_responses.load(Ordering::Relaxed), 1);
        let _ = std::fs::remove_file(path);
    }

    #[cfg(unix)]
    #[test]
    fn failed_model_change_reloads_authoritative_current_selection() {
        let (mut client, server, path) = mock_client(|request| match request.name.as_str() {
            "hello" => (
                MessageType::Response,
                json!({"major":1,"minor":1,"daemon_version":"test","capabilities":[]}),
            ),
            "model.list" => (
                MessageType::Response,
                json!({"models":[{"provider_id":"provider-a","model_id":"model-1"}]}),
            ),
            "model.current" => (
                MessageType::Response,
                json!({"provider_id":"provider-b","model_id":"model-2"}),
            ),
            "model.select" => (MessageType::Error, json!({"code":"model-unavailable"})),
            other => panic!("unexpected request {other}"),
        });
        let mut view = View::default();
        refresh_models(&mut client, &mut view).unwrap();
        assert!(select_model(&mut client, 0, &mut view).is_err());
        assert_eq!(view.selected_provider, "provider-b");
        assert_eq!(view.selected_model, "model-2");
        assert!(view.model_status.starts_with("Selection failed:"));
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn transcript_projects_live_turn_once_and_preserves_utf8_bounds() {
        let mut view = View {
            history: "user: earlier\n\nassistant: previous answer\n\n".into(),
            live_user: "current question".into(),
            response: "streamed answer".into(),
            show_overlay: true,
            ..View::default()
        };
        let live = transcript(&view);
        assert_eq!(live.matches("user: current question").count(), 1);
        assert_eq!(live.matches("assistant: streamed answer").count(), 1);
        assert_eq!(live.matches("assistant: previous answer").count(), 1);

        view.history = "ö\n".repeat(RESPONSE_LIMIT);
        view.live_user = "漢".repeat(8192);
        view.response = "é".repeat(20_000);
        let bounded = transcript(&view);
        assert!(bounded.len() <= RESPONSE_LIMIT);
        assert!(bounded.is_char_boundary(bounded.len()));
        assert!(bounded.contains("[Earlier history omitted]"));
        assert!(bounded.contains("[truncated]"));
    }

    #[test]
    fn cancelled_and_failed_terminal_events_keep_one_partial_answer() {
        for (name, expected_status) in [
            ("run.cancelled", "Run stopped."),
            ("run.failed", "Run failed."),
        ] {
            let mut session = Session {
                id: "session".into(),
                run: "run-1".into(),
                generation: Some("generation".into()),
                sequence: Some(1),
            };
            let mut view = View {
                history: "user: previous\n\nassistant: old answer\n\n".into(),
                live_user: "current question".into(),
                response: "partial answer".into(),
                show_overlay: true,
                running: true,
                ..View::default()
            };
            let mut terminal = event(name, "run-1", "generation", 2, "partial answer");
            terminal.payload["session_id"] = Value::String("session".into());
            assert!(receive_event(&mut session, &mut view, terminal));
            let projection = transcript(&view);
            assert_eq!(view.status, expected_status);
            assert_eq!(projection.matches("user: current question").count(), 1);
            assert_eq!(projection.matches("assistant: partial answer").count(), 1);
        }
    }

    #[cfg(unix)]
    #[test]
    fn reconnect_rehydrates_active_turn_and_terminal_refresh_collapses_overlay() {
        let history_reads = Arc::new(Mutex::new(0usize));
        let history_calls = Arc::clone(&history_reads);
        let attach_reads = Arc::new(Mutex::new(0usize));
        let attach_calls = Arc::clone(&attach_reads);
        let (path, server) = mock_server(move |request| {
            let response = match request.name.as_str() {
                "hello" => {
                    json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["approval"]})
                }
                "session.list" => json!({"sessions":[{"session_id":"session","title":"Saved"}]}),
                "model.list" => {
                    json!({"models":[{"provider_id":"persisted-provider","model_id":"persisted-model"}]})
                }
                "model.current" => {
                    json!({"provider_id":"persisted-provider","model_id":"persisted-model"})
                }
                "session.attach" => {
                    let mut reads = attach_calls.lock().unwrap();
                    *reads += 1;
                    if *reads == 1 {
                        json!({
                            "session_id":"session","state":"approval","output":"recovered partial",
                            "run_id":"run-1","server_generation":"generation","event_sequence":1,
                            "approval_id":"approval-1","approval":{"tool":"read-file","detail":"Read file",
                                "risk":0,"resources":[{"resource":"docs/readme.md"}]}
                        })
                    } else {
                        json!({
                            "session_id":"session","state":"cancelled","output":"recovered partial",
                            "run_id":"run-1","server_generation":"generation","event_sequence":2
                        })
                    }
                }
                "session.messages" => {
                    let mut reads = history_calls.lock().unwrap();
                    *reads += 1;
                    if *reads == 1 {
                        json!({"session_id":"session","messages":[
                            {"id":1,"role":"user","content":"current question","status":"sent"},
                            {"id":2,"role":"assistant","content":"","status":"streaming"}
                        ]})
                    } else {
                        json!({"session_id":"session","messages":[
                            {"id":1,"role":"user","content":"current question","status":"sent"},
                            {"id":2,"role":"assistant","content":"recovered partial","status":"cancelled"}
                        ]})
                    }
                }
                other => panic!("unexpected request {other}"),
            };
            (MessageType::Response, response)
        });

        let mut session = Session {
            id: "session".into(),
            ..Session::default()
        };
        let mut view = View::default();
        let mut client = connect(&path, &mut session, &mut view).unwrap();
        assert!(view.running);
        assert!(view.show_overlay);
        assert!(view.approval_pending);
        assert_eq!(view.approval_id, "approval-1");
        assert!(view.approval_detail.contains("docs/readme.md"));
        assert_eq!(view.selected_provider, "persisted-provider");
        assert_eq!(view.selected_model, "persisted-model");
        assert_eq!(
            view.model_status,
            "Saved selection: persisted-provider / persisted-model"
        );
        let reconnected = transcript(&view);
        assert_eq!(reconnected.matches("user: current question").count(), 1);
        assert_eq!(
            reconnected.matches("assistant: recovered partial").count(),
            1
        );

        let mut terminal = event(
            "run.cancelled",
            "run-1",
            "generation",
            2,
            "recovered partial",
        );
        terminal.payload["session_id"] = Value::String("session".into());
        assert!(receive_event(&mut session, &mut view, terminal));
        refresh_after_terminal(&mut client, &session, &mut view).unwrap();
        assert!(!view.show_overlay);
        let final_projection = transcript(&view);
        assert_eq!(
            final_projection.matches("user: current question").count(),
            1
        );
        assert_eq!(
            final_projection
                .matches("assistant: recovered partial")
                .count(),
            1
        );
        assert!(!view.history_status.contains("not confirmed"));
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[cfg(unix)]
    #[test]
    fn session_switch_and_terminal_refresh_keep_history_scoped_to_selected_id() {
        let beta_attaches = Arc::new(Mutex::new(0usize));
        let beta_histories = Arc::new(Mutex::new(0usize));
        let attach_calls = Arc::clone(&beta_attaches);
        let history_calls = Arc::clone(&beta_histories);
        let (mut client, server, path) = mock_client(move |request| {
            let success = match request.name.as_str() {
                "hello" => json!({"major":1,"minor":1,"daemon_version":"test","capabilities":[]}),
                "session.list" => json!({"sessions":[
                    {"session_id":"alpha","title":"Alpha"},
                    {"session_id":"beta","title":"Beta"}
                ]}),
                "session.attach" if request.payload["session_id"] == "beta" => {
                    let mut calls = attach_calls.lock().unwrap();
                    *calls += 1;
                    if *calls == 1 {
                        return (MessageType::Error, json!({"code":"unknown-session"}));
                    }
                    if *calls == 2 {
                        json!({"session_id":"beta","state":"completed","output":"beta output","run_id":""})
                    } else {
                        json!({"session_id":"beta","state":"completed","output":"final response","run_id":"run-1"})
                    }
                }
                "session.attach" => json!({
                    "session_id":"alpha","state":"completed",
                    "output":"alpha output","run_id":""
                }),
                "session.messages" if request.payload["session_id"] == "beta" => {
                    let mut calls = history_calls.lock().unwrap();
                    *calls += 1;
                    if *calls == 1 {
                        return (MessageType::Error, json!({"code":"unknown-session"}));
                    }
                    json!({"session_id":"beta","messages":[
                        {"id":1,"role":"user","content":"beta question","status":"sent"},
                        {"id":2,"role":"assistant","content":"final response","status":"completed"}
                    ]})
                }
                "session.messages" => json!({"session_id":"alpha","messages":[
                    {"id":1,"role":"user","content":"alpha question","status":"sent"},
                    {"id":2,"role":"assistant","content":"alpha answer","status":"completed"}
                ]}),
                other => panic!("unexpected request {other}"),
            };
            (MessageType::Response, success)
        });
        let mut view = View::default();
        refresh(&mut client, &mut view).unwrap();
        assert_eq!(
            view.sessions,
            vec![
                ("alpha".into(), "Alpha".into()),
                ("beta".into(), "Beta".into())
            ]
        );

        let mut session = Session::default();
        select_session(&mut client, "alpha", &mut session, &mut view).unwrap();
        assert_eq!(view.selected_id, "alpha");
        assert!(view.history.contains("alpha question"));

        assert!(select_session(&mut client, "beta", &mut session, &mut view).is_err());
        assert_eq!(session.id, "alpha");
        assert_eq!(view.selected_id, "alpha");
        assert!(view.history.contains("alpha question"));

        select_session(&mut client, "beta", &mut session, &mut view).unwrap();
        assert_eq!(view.selected_id, "beta");
        assert!(view.history.is_empty());
        assert!(view.history_status.starts_with("History unavailable:"));

        session.run = "run-1".into();
        session.generation = Some("generation".into());
        session.sequence = Some(0);
        view.running = true;
        view.live_user = "beta question".into();
        view.show_overlay = true;
        let mut terminal = event("run.completed", "run-1", "generation", 1, "final response");
        terminal.payload["session_id"] = Value::String("beta".into());
        assert!(receive_event(&mut session, &mut view, terminal));
        refresh_after_terminal(&mut client, &session, &mut view).unwrap();
        assert!(view.history.contains("beta question"));
        assert!(!view.history.contains("alpha question"));
        assert!(!view.show_overlay);
        assert_eq!(transcript(&view).matches("user: beta question").count(), 1);
        assert_eq!(
            transcript(&view)
                .matches("assistant: final response")
                .count(),
            1
        );
        assert_eq!(view.sessions.len(), 2);
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[cfg(unix)]
    #[test]
    fn desktop_theme_write_uses_persisted_value_after_rejection() {
        let stored = std::sync::Arc::new(Mutex::new("light".to_owned()));
        let stored_server = std::sync::Arc::clone(&stored);
        let writes = std::sync::Arc::new(AtomicU64::new(0));
        let writes_server = std::sync::Arc::clone(&writes);
        let (mut client, server, path) = mock_client(move |request| match request.name.as_str() {
            "hello" => (
                MessageType::Response,
                json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["desktop-settings"]}),
            ),
            "desktop.settings" => (
                MessageType::Response,
                json!({"values":{"themeName":stored_server.lock().unwrap().clone()}}),
            ),
            "desktop.setting" => {
                assert_eq!(request.payload["key"], "themeName");
                let attempt = writes_server.fetch_add(1, Ordering::Relaxed);
                if attempt == 0 {
                    (MessageType::Response, json!({"accepted":false}))
                } else {
                    *stored_server.lock().unwrap() =
                        request.payload["value"].as_str().unwrap().into();
                    (MessageType::Response, json!({"accepted":true}))
                }
            }
            other => panic!("unexpected request {other}"),
        });
        let mut view = View {
            desktop_settings_capable: true,
            theme_name: "system".into(),
            ..View::default()
        };
        refresh_theme(&mut client, &mut view).unwrap();
        assert_eq!(view.theme_name, "light");
        assert!(set_theme(&mut client, &mut view, 2).is_err());
        assert_eq!(view.theme_name, "light");
        assert!(view.theme_status.contains("not accepted"));
        set_theme(&mut client, &mut view, 1).unwrap();
        assert_eq!(view.theme_name, "light");
        assert!(view.theme_status.contains("Saved"));
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[cfg(unix)]
    #[test]
    fn response_profile_setting_round_trips_trimmed_chat_only_value_and_checks_utf16_limit() {
        let stored = std::sync::Arc::new(Mutex::new(String::new()));
        let stored_server = std::sync::Arc::clone(&stored);
        let (mut client, server, path) = mock_client(move |request| match request.name.as_str() {
            "hello" => (
                MessageType::Response,
                json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["desktop-settings"]}),
            ),
            "desktop.setting" => {
                assert_eq!(request.payload["key"], "responseProfileInstructions");
                *stored_server.lock().unwrap() = request.payload["value"].as_str().unwrap().into();
                (MessageType::Response, json!({"accepted":true}))
            }
            "desktop.settings" => (
                MessageType::Response,
                json!({"values":{
                    "themeName":"system",
                    "responseProfileInstructions":stored_server.lock().unwrap().clone()
                }}),
            ),
            other => panic!("unexpected request {other}"),
        });
        let mut view = View {
            desktop_settings_capable: true,
            ..View::default()
        };
        save_response_profile(&mut client, &mut view, "  Be concise.  ").unwrap();
        assert_eq!(view.response_profile_instructions, "Be concise.");
        assert!(view.response_profile_status.contains("subsequent Chat"));
        assert!(save_response_profile(&mut client, &mut view, &"😀".repeat(1001)).is_err());
        assert_eq!(view.response_profile_instructions, "Be concise.");
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[cfg(unix)]
    #[test]
    fn provider_diagnostics_are_read_only_cached_and_never_claim_connectivity() {
        let (mut client, server, path) = mock_client(|request| match request.name.as_str() {
            "hello" => (
                MessageType::Response,
                json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["provider-diagnostics"]}),
            ),
            "desktop.settings_service" => {
                assert_eq!(request.payload["action"], "providerSettingsState");
                assert_eq!(
                    request.payload["arguments"],
                    json!(["openai-compatible-local"])
                );
                (
                    MessageType::Response,
                    json!({"result":{
                        "id":"openai-compatible-local","known":true,"health":"Unknown",
                        "catalog":"Cached","local":true,"endpoint":"http://127.0.0.1:8080",
                        "discoveredModels":["fixture-model"],"contextWindow":8192,
                        "configuredModel":"fixture-model"
                    }}),
                )
            }
            other => panic!("unexpected request {other}"),
        });
        let mut view = View {
            selected_provider: "openai-compatible-local".into(),
            provider_diagnostics_capable: true,
            ..View::default()
        };
        refresh_provider_diagnostics(&mut client, &mut view).unwrap();
        assert!(
            view.provider_diagnostics
                .contains("Configured model: fixture-model")
        );
        assert!(
            view.provider_diagnostics
                .contains("Configured context window: 8192 tokens")
        );
        assert!(view.provider_diagnostics.contains("fixture-model"));
        assert!(!view.provider_diagnostics.contains("127.0.0.1"));
        assert!(
            view.provider_diagnostics_status
                .contains("Connectivity is Unknown")
        );
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[cfg(unix)]
    #[test]
    fn settings_refresh_projects_daemon_runtime_and_workspace_policy() {
        let (mut client, server, path) = mock_client(|request| match request.name.as_str() {
            "hello" => (
                MessageType::Response,
                json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["desktop-projection-partial","terminal-state"]}),
            ),
            "desktop.projection" => (
                MessageType::Response,
                json!({"page":0,"pages":28,"properties":{
                    "activeRuntimeProviderLabel":"local-provider",
                    "activeRuntimeModelLabel":"fixture-model",
                    "activeRuntimeReadinessState":"unknown",
                    "activeRuntimeReadinessSummary":"Provider connectivity has not been verified.",
                    "activeRuntimeLocalOnlySummary":"Configured for a local loopback provider."
                }}),
            ),
            "terminal.state" => (
                MessageType::Response,
                json!({"properties":{},"providers":[],"workspace":{
                    "id":"work","name":"Project","root":"/tmp/project",
                    "access":"read-only","permission":"preauthorized","archived":false
                },"workspaces":[]}),
            ),
            other => panic!("unexpected request {other}"),
        });
        let mut view = View::default();
        apply_capabilities(client.capabilities(), &mut view);
        refresh_settings_status(&mut client, &mut view).unwrap();
        assert!(view.runtime_projection_view.contains("Readiness: unknown"));
        assert!(
            view.runtime_projection_view
                .contains("has not been verified")
        );
        assert!(
            view.workspace_runtime_view
                .contains("Permission: preauthorized")
        );
        assert!(view.workspace_runtime_view.contains("Access: read-only"));
        assert!(view.workspace_runtime_status.contains("does not grant"));
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[cfg(unix)]
    #[test]
    fn latest_turn_actions_use_authoritative_message_ids() {
        let (mut client, server, path) = mock_client(|request| match request.name.as_str() {
            "hello" => (
                MessageType::Response,
                json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["chat-revisions"]}),
            ),
            "session.messages" => (
                MessageType::Response,
                json!({"session_id":"session","messages":[
                    {"id":1,"role":"user","content":"old","status":"sent"},
                    {"id":2,"role":"assistant","content":"old answer","status":"completed","reply_to":1},
                    {"id":3,"role":"user","content":"latest","status":"sent"},
                    {"id":4,"role":"assistant","content":"failed answer","status":"failed","reply_to":3}
                ]}),
            ),
            "chat.regenerate" => {
                assert_eq!(request.payload["message_id"], 3);
                (
                    MessageType::Response,
                    json!({"session_id":"session","run_id":"regen"}),
                )
            }
            "chat.retry" => {
                assert_eq!(request.payload["message_id"], 4);
                (
                    MessageType::Response,
                    json!({"session_id":"session","run_id":"retry"}),
                )
            }
            other => panic!("unexpected request {other}"),
        });
        let session = Session {
            id: "session".into(),
            ..Session::default()
        };
        let mut view = View {
            chat_revisions_capable: true,
            ..View::default()
        };
        load_history(&mut client, &session, &mut view).unwrap();
        assert_eq!(view.latest_user_id, 3);
        assert_eq!(view.latest_retry_id, 4);
        let mut session = session;
        start_existing_turn(&mut client, &mut session, &mut view, false).unwrap();
        assert_eq!(session.run, "regen");
        assert!(view.running);
        view.running = false;
        start_existing_turn(&mut client, &mut session, &mut view, true).unwrap();
        assert_eq!(session.run, "retry");
        assert!(view.running);
        drop(client);
        server.join().unwrap();
        let _ = std::fs::remove_file(path);
    }

    #[cfg(unix)]
    #[test]
    fn session_memory_ui_uses_revision_and_explicit_delete_confirmation() {
        let stored = std::sync::Arc::new(Mutex::new(None::<(String, String, String, u64)>));
        let stored_server = std::sync::Arc::clone(&stored);
        let (mut client, server, path) = mock_client(move |request| match request.name.as_str() {
            "hello" => (
                MessageType::Response,
                json!({"major":1,"minor":1,"daemon_version":"test","capabilities":["session-memory"]}),
            ),
            "desktop.action" => {
                assert_eq!(request.payload["action"], "manageMemory");
                assert_eq!(request.payload["session_id"], "session");
                let args = &request.payload["arguments"][0];
                let tag = args["requestTag"].clone();
                let session_scope = args.get("scope").and_then(Value::as_u64) == Some(1);
                let mut data = stored_server.lock().unwrap();
                let result = match args["operation"].as_str().unwrap() {
                    "list" => {
                        assert!(session_scope);
                        let records = data
                            .as_ref()
                            .map(|(id, key, value, revision)| {
                                vec![json!({
                                    "id":id,"revision":revision.to_string(),"key":key,"value":value,
                                    "valueLength":value.encode_utf16().count()
                                })]
                            })
                            .unwrap_or_default();
                        json!({"status":"Ready","requestTag":tag,"records":records,"nextCursor":""})
                    }
                    "read" => {
                        assert!(session_scope);
                        let (id, key, value, revision) = data.as_ref().unwrap();
                        json!({"status":"Ready","requestTag":tag,"record":{
                            "id":id,"revision":revision.to_string(),"key":key,"value":value,
                            "valueLength":value.encode_utf16().count(),"offset":0
                        }})
                    }
                    "create" => {
                        assert!(session_scope);
                        assert_eq!(args["session"], "session");
                        assert_eq!(args["workspace"], "");
                        *data = Some((
                            "7".into(),
                            args["key"].as_str().unwrap().into(),
                            args["value"].as_str().unwrap().into(),
                            1,
                        ));
                        json!({"status":"Ready","requestTag":tag})
                    }
                    "correct" => {
                        assert_eq!(args["session"], "session");
                        assert_eq!(args["workspace"], "");
                        let current = data.as_mut().unwrap();
                        assert_eq!(args["id"], current.0);
                        assert_eq!(args["revision"], current.3.to_string());
                        current.1 = args["key"].as_str().unwrap().into();
                        current.2 = args["value"].as_str().unwrap().into();
                        current.3 += 1;
                        json!({"status":"Ready","requestTag":tag})
                    }
                    "delete" => {
                        assert_eq!(args["session"], "session");
                        assert_eq!(args["workspace"], "");
                        assert_eq!(args["confirmed"], true);
                        assert_eq!(args["confirmation"], "DELETE");
                        assert_eq!(args["revision"], data.as_ref().unwrap().3.to_string());
                        *data = None;
                        json!({"status":"Ready","requestTag":tag})
                    }
                    other => panic!("unsupported memory operation {other}"),
                };
                (
                    MessageType::Response,
                    json!({"accepted":true,"result":{"value":result}}),
                )
            }
            other => panic!("unexpected request {other}"),
        });
        let session = Session {
            id: "session".into(),
            ..Session::default()
        };
        let mut view = View {
            memory_capable: true,
            memory_key: "topic".into(),
            memory_value: "first note".into(),
            ..View::default()
        };
        memory_save(&mut client, &session, &mut view).unwrap();
        assert!(view.memory_status.contains("created"));
        assert_eq!(view.memory_records.len(), 1);
        memory_read(&mut client, &session, &mut view, 0).unwrap();
        assert_eq!(view.memory_revision, "1");
        assert_eq!(view.memory_value, "first note");
        view.memory_value = "corrected note".into();
        memory_save(&mut client, &session, &mut view).unwrap();
        assert!(view.memory_status.contains("correction saved"));
        assert_eq!(view.memory_revision, "2");
        assert_eq!(view.memory_value, "corrected note");
        assert!(memory_delete(&mut client, &session, &mut view).is_err());
        view.memory_delete_confirmation = "DELETE".into();
        memory_delete(&mut client, &session, &mut view).unwrap();
        assert!(view.memory_id.is_empty());
        assert!(view.memory_status.contains("deleted"));
        drop(client);
        server.join().unwrap();
        assert!(stored.lock().unwrap().is_none());
        let _ = std::fs::remove_file(path);
    }
}
