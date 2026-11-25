use crate::{
    chat::{self, Peer},
    provider::Config,
};
use sentinel_ipc::server::{Dispatcher, read_frame};
use std::{
    env,
    fs::{self, File, OpenOptions},
    io::{self, BufReader, Write},
    os::{
        fd::AsRawFd,
        unix::{
            fs::{DirBuilderExt, FileTypeExt, MetadataExt, OpenOptionsExt, PermissionsExt},
            net::{SocketAddr, UnixListener, UnixStream},
        },
    },
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering},
        mpsc::{SyncSender, TrySendError, sync_channel},
    },
    thread,
    time::{Duration, Instant},
};

const CLIENT_LIFETIME: Duration = Duration::from_secs(1800);
const MAX_CLIENTS: usize = 8;
const SHUTDOWN_DRAIN: Duration = Duration::from_secs(5);
const FRAME_LIFETIME: Duration = Duration::from_secs(3);
const WRITE_TIMEOUT: Duration = Duration::from_secs(3);

fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}
fn checked_profile_directory(path: &Path) -> io::Result<()> {
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.file_type().is_dir()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.mode() & 0o7777 != 0o700
    {
        return Err(invalid("profile must be an owner-only directory"));
    }
    Ok(())
}
fn private_directory(path: &Path) -> io::Result<()> {
    let parent = path
        .parent()
        .ok_or_else(|| invalid("profile needs a parent"))?
        .canonicalize()?;
    if parent != Path::new("/tmp").canonicalize()? {
        return Err(invalid("development profile must be directly under /tmp"));
    }
    checked_profile_directory(path)?;
    profile_inventory(path)
}
fn persistent_profile_path() -> io::Result<PathBuf> {
    let path = sentinel_ipc::persistent_rust_profile_path()?;
    let home = path
        .parent()
        .ok_or_else(|| invalid("missing profile parent"))?;
    let uid = unsafe { libc::geteuid() };
    if fs::metadata(home)?.uid() != uid {
        return Err(invalid("HOME must be owned by the current user"));
    }
    for directory in home.ancestors() {
        let metadata = fs::symlink_metadata(directory)?;
        if !metadata.is_dir()
            || (metadata.uid() != uid && metadata.uid() != 0)
            || metadata.mode() & 0o022 != 0
        {
            return Err(invalid("unsafe persistent profile parent"));
        }
    }
    SocketAddr::from_pathname(path.join("daemon-dev.sock"))?;
    Ok(path)
}
fn persistent_profile() -> io::Result<PathBuf> {
    let path = persistent_profile_path()?;
    match fs::DirBuilder::new().mode(0o700).create(&path) {
        Ok(()) => File::open(
            path.parent()
                .ok_or_else(|| invalid("missing profile parent"))?,
        )?
        .sync_all()?,
        Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {}
        Err(error) => return Err(error),
    }
    checked_profile_directory(&path)?;
    profile_inventory(&path)?;
    Ok(path)
}
fn profile_inventory(path: &Path) -> io::Result<()> {
    for entry in fs::read_dir(path)? {
        let entry = entry?;
        let name = entry.file_name();
        // SQLite uses this narrowly shaped super-journal for an atomic multi-store commit.
        if let Some(suffix) = name
            .to_str()
            .and_then(|n| n.strip_prefix("daemon-dev-chat.sqlite3-mj"))
            && suffix.len() == 9
            && suffix.as_bytes()[6] == b'9'
            && suffix
                .bytes()
                .all(|b| b.is_ascii_hexdigit() && !b.is_ascii_lowercase())
        {
            let meta = fs::symlink_metadata(entry.path())?;
            if !meta.is_file()
                || meta.uid() != unsafe { libc::geteuid() }
                || meta.mode() & 0o7777 != 0o600
                || meta.nlink() != 1
            {
                return Err(invalid("unsafe SQLite super-journal"));
            }
            continue;
        }
        if name != "daemon-dev.lock"
            && name != "daemon-dev.sock"
            && name != crate::store::DATABASE
            && name != crate::store::JOURNAL
            && name != crate::settings::DATABASE
            && name != crate::settings::JOURNAL
            && name != crate::agent_store::DATABASE
            && name != crate::agent_store::JOURNAL
            && name != crate::memory::DATABASE
            && name != crate::memory::JOURNAL
        {
            return Err(invalid("profile contains non-development files"));
        }
    }
    Ok(())
}
pub(crate) fn lock_profile(profile: &Path) -> io::Result<File> {
    let lock = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK)
        .open(profile.join("daemon-dev.lock"))?;
    let m = lock.metadata()?;
    if !m.is_file()
        || m.uid() != unsafe { libc::geteuid() }
        || m.mode() & 0o7777 != 0o600
        || m.nlink() != 1
    {
        return Err(invalid("unsafe profile lock"));
    }
    if unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(lock)
}
/// Run under the profile lock. Only a refused connection to the same private
/// socket can authorize removal of a crash residue.
fn remove_stale_persistent_socket(path: &Path) -> io::Result<()> {
    let before = match fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error),
    };
    if !before.file_type().is_socket()
        || before.uid() != unsafe { libc::geteuid() }
        || before.mode() & 0o7777 != 0o600
        || before.nlink() != 1
    {
        return Err(invalid("unsafe persistent profile endpoint"));
    }
    match UnixStream::connect(path) {
        Err(error) if error.kind() == io::ErrorKind::ConnectionRefused => {}
        _ => {
            return Err(io::Error::new(
                io::ErrorKind::AddrInUse,
                "persistent endpoint may be live",
            ));
        }
    }
    let after = fs::symlink_metadata(path)?;
    if !after.file_type().is_socket()
        || before.dev() != after.dev()
        || before.ino() != after.ino()
        || before.uid() != after.uid()
        || before.mode() != after.mode()
        || before.nlink() != after.nlink()
    {
        return Err(invalid("persistent endpoint changed before cleanup"));
    }
    fs::remove_file(path)
}
struct Endpoint {
    path: PathBuf,
    dev: u64,
    ino: u64,
}
impl Drop for Endpoint {
    fn drop(&mut self) {
        if let Ok(meta) = fs::symlink_metadata(&self.path)
            && meta.file_type().is_socket()
            && meta.dev() == self.dev
            && meta.ino() == self.ino
        {
            let _ = fs::remove_file(&self.path);
        }
    }
}
fn peer_uid(stream: &UnixStream) -> io::Result<u32> {
    #[cfg(target_os = "macos")]
    {
        let mut uid = 0;
        let mut gid = 0;
        if unsafe { libc::getpeereid(stream.as_raw_fd(), &mut uid, &mut gid) } != 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(uid)
    }
    #[cfg(target_os = "linux")]
    {
        let mut cred: libc::ucred = unsafe { std::mem::zeroed() };
        let mut len = std::mem::size_of::<libc::ucred>() as libc::socklen_t;
        if unsafe {
            libc::getsockopt(
                stream.as_raw_fd(),
                libc::SOL_SOCKET,
                libc::SO_PEERCRED,
                (&mut cred as *mut libc::ucred).cast(),
                &mut len,
            )
        } != 0
            || len as usize != std::mem::size_of::<libc::ucred>()
        {
            return Err(io::Error::last_os_error());
        }
        Ok(cred.uid)
    }
}
fn write_frame(stream: &mut UnixStream, bytes: &[u8]) -> io::Result<()> {
    let deadline = Instant::now() + WRITE_TIMEOUT;
    let mut remaining_bytes = bytes;
    while !remaining_bytes.is_empty() {
        let remaining = deadline
            .checked_duration_since(Instant::now())
            .filter(|duration| !duration.is_zero())
            .ok_or_else(|| io::Error::new(io::ErrorKind::TimedOut, "write deadline"))?;
        stream.set_write_timeout(Some(remaining))?;
        match stream.write(remaining_bytes) {
            Ok(0) => return Err(io::Error::new(io::ErrorKind::WriteZero, "closed peer")),
            Ok(count) => remaining_bytes = &remaining_bytes[count..],
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(error),
        }
    }
    if Instant::now() >= deadline {
        return Err(io::Error::new(io::ErrorKind::TimedOut, "write deadline"));
    }
    Ok(())
}
fn wait_input(reader: &BufReader<UnixStream>, deadline: Instant) -> io::Result<()> {
    if !reader.buffer().is_empty() {
        return Ok(());
    }
    loop {
        let remaining = deadline
            .checked_duration_since(Instant::now())
            .ok_or_else(|| io::Error::new(io::ErrorKind::TimedOut, "client deadline"))?;
        let millis = remaining.as_millis().clamp(1, i32::MAX as u128) as i32;
        let mut fd = libc::pollfd {
            fd: reader.get_ref().as_raw_fd(),
            events: libc::POLLIN,
            revents: 0,
        };
        let result = unsafe { libc::poll(&mut fd, 1, millis) };
        if result > 0 {
            return Ok(());
        }
        if result < 0 {
            let error = io::Error::last_os_error();
            if error.kind() != io::ErrorKind::Interrupted {
                return Err(error);
            }
        }
    }
}
fn serve(
    stream: UnixStream,
    started: Instant,
    state: Arc<Mutex<chat::State>>,
    stopping: Arc<AtomicBool>,
    wake: Arc<UnixStream>,
    jobs: SyncSender<Option<chat::Job>>,
) -> io::Result<()> {
    if peer_uid(&stream)? != unsafe { libc::geteuid() } {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "foreign peer",
        ));
    }
    stream.set_nonblocking(false)?;
    let mut writer = stream.try_clone()?;
    let (sender, receiver) = sync_channel::<chat::Outbound>(16);
    let close_stream = stream.try_clone()?;
    let peer = Arc::new(Peer {
        sender,
        close: Box::new(move || {
            let _ = close_stream.shutdown(std::net::Shutdown::Both);
        }),
    });
    let writer_thread = thread::spawn(move || {
        while let Ok(item) = receiver.recv() {
            let ok = write_frame(&mut writer, &item.bytes).is_ok();
            if let Some(confirmation) = item.confirmation {
                let _ = confirmation.send(ok);
            }
            if !ok {
                let _ = writer.shutdown(std::net::Shutdown::Both);
                break;
            }
        }
    });
    let result = (|| -> io::Result<()> {
        let mut reader = BufReader::new(stream);
        let mut dispatcher = Dispatcher::with_started(started);
        dispatcher.allow_shutdown();
        let mut capabilities = vec![
            "sessions".into(),
            "session-messages".into(),
            "desktop-settings".into(),
            "provider-diagnostics".into(),
            "desktop-projection-partial".into(),
            "terminal-state".into(),
            "session-memory".into(),
            "agent-history".into(),
            "session-management".into(),
            "dev-backup-export".into(),
            "dev-backup-set".into(),
            "session-flags".into(),
        ];
        {
            let state = state.lock().map_err(|_| io::Error::other("state lock"))?;
            if state.has_provider() {
                capabilities.extend(["chat".into(), "events".into(), "model-selection".into()]);
                if state.has_cloud_provider() {
                    capabilities.push("dev-cloud-chat".into());
                    if state.has_workspace_files() {
                        capabilities.push("dev-cloud-document-attachments".into());
                        if state.has_image_attachments() {
                            capabilities.push("dev-cloud-image-attachments".into());
                        }
                    }
                    if state.has_cloud_history() {
                        capabilities.push("dev-cloud-history".into());
                    }
                } else {
                    capabilities.extend([
                        "dev-local-provider-catalog".into(),
                        "dev-local-provider-endpoints".into(),
                        "chat-revisions".into(),
                    ]);
                }
            }
        }
        if state
            .lock()
            .map_err(|_| io::Error::other("state lock"))?
            .has_workspace_selection()
        {
            capabilities.push("workspace-selection".into());
            capabilities.push("dev-workspace-revocation".into());
        }
        if state
            .lock()
            .map_err(|_| io::Error::other("state lock"))?
            .plugin_root()
            .is_some()
        {
            capabilities.push("plugin-catalog".into());
        }
        if state
            .lock()
            .map_err(|_| io::Error::other("state lock"))?
            .has_voice()
        {
            capabilities.push("voice".into());
        }
        if state
            .lock()
            .map_err(|_| io::Error::other("state lock"))?
            .has_speech_inspection()
        {
            capabilities.push("speech-inspection".into());
        }
        if state
            .lock()
            .map_err(|_| io::Error::other("state lock"))?
            .has_workspace_files()
        {
            capabilities.push("workspace-files".into());
            capabilities.push("chat-workspace-attachments".into());
            capabilities.push("chat-attachments-fodt".into());
            capabilities.push("chat-attachments-docx".into());
            capabilities.push("chat-attachments-pdf-text".into());
            capabilities.push("workspace-changes".into());
        }
        if state
            .lock()
            .map_err(|_| io::Error::other("state lock"))?
            .has_agent()
        {
            capabilities.push("agent".into());
            capabilities.push("agent-inspect-file".into());
            capabilities.push("agent-inspect-files".into());
            capabilities.push("agent-classify-inspect".into());
            capabilities.push("agent-delegate-read".into());
            capabilities.push("agent-inspect-directory".into());
            capabilities.push("agent-search-literal".into());
            capabilities.push("agent-glob".into());
            capabilities.push("agent-stat-path".into());
            capabilities.push("agent-read-plan".into());
            capabilities.push("agent-work-plan".into());
            capabilities.push("agent-create-file".into());
            capabilities.push("agent-edit-file".into());
            if state
                .lock()
                .map_err(|_| io::Error::other("state lock"))?
                .has_process()
            {
                capabilities.push("agent-run-process".into());
            }
            if state
                .lock()
                .map_err(|_| io::Error::other("state lock"))?
                .requires_approval()
            {
                capabilities.push("approval".into());
            }
        }
        if state
            .lock()
            .map_err(|_| io::Error::other("state lock"))?
            .has_configured_ollama()
        {
            capabilities.push("ollama-model-operations".into());
        }
        if state
            .lock()
            .map_err(|_| io::Error::other("state lock"))?
            .has_image_attachments()
        {
            capabilities.push("chat-image-png".into());
            capabilities.push("chat-image-jpeg-webp".into());
        }
        dispatcher.set_capabilities(capabilities);
        let deadline = Instant::now() + CLIENT_LIFETIME;
        let hello_deadline = deadline.min(Instant::now() + FRAME_LIFETIME);
        let mut hello_received = false;
        while !stopping.load(Ordering::SeqCst) {
            let input_deadline = if hello_received {
                deadline
            } else {
                hello_deadline
            };
            wait_input(&reader, input_deadline)?;
            let frame = read_frame(
                &mut reader,
                input_deadline.min(Instant::now() + FRAME_LIFETIME),
            )?;
            let mut state_guard = Some(state.lock().map_err(|_| io::Error::other("state lock"))?);
            let mut job = None;
            let (active, sessions) = state_guard.as_ref().unwrap().counts();
            dispatcher.set_counts(active, sessions);
            let reply = dispatcher.dispatch_with(&frame, |request| {
                if let sentinel_ipc::contract::RequestPayload::DesktopSettingsService { ref action, ref arguments } = request
                    && action == "extensionsHubState" && arguments.is_empty()
                {
                    let root = state_guard.as_ref().unwrap().plugin_root().ok_or("runtime-unavailable")?;
                    drop(state_guard.take());
                    let result = crate::plugin_catalog::scan(&root).map_err(|_| "runtime-unavailable")?;
                    serde_json::from_value::<sentinel_ipc::contract::ResponsePayload>(serde_json::json!({"name":"desktop.settings_service","payload":{"result":result}})).map_err(|_| "runtime-unavailable")
                } else if matches!(
                    request,
                    sentinel_ipc::contract::RequestPayload::ModelList {}
                ) {
                    let (config, generation) = state_guard.as_ref().unwrap().discovery_config()?;
                    drop(state_guard.take());
                    let models = crate::provider::discover_models(&config, &stopping)
                        .map_err(|_| "provider-unavailable")?;
                    state
                        .lock()
                        .map_err(|_| "runtime-unavailable")?
                        .install_catalog(&config, generation, models)
                } else if let sentinel_ipc::contract::RequestPayload::AgentStart { ref session_id, .. } = request {
                    let session_id = session_id.clone();
                    let workspace = state_guard.as_ref().unwrap().baseline_candidate(&session_id);
                    let baseline = if let Some((workspace, token)) = workspace {
                        drop(state_guard.take());
                        let snapshot = workspace.snapshot(crate::agent_gateway::PermissionDecision::Allow).ok();
                        state_guard = Some(state.lock().map_err(|_| "runtime-unavailable")?);
                        if state_guard.as_ref().unwrap().baseline_candidate_is_current(token) { snapshot } else { None }
                    } else { None };
                    let state_guard = state_guard.as_mut().unwrap();
                    let result = state_guard.handle(request, &peer, &mut job);
                    if let Ok(sentinel_ipc::contract::ResponsePayload::AgentStart { run_id, .. }) = &result {
                        state_guard.install_baseline(&session_id, run_id, baseline);
                    }
                    result
                } else if let sentinel_ipc::contract::RequestPayload::WorkspaceChanges { session_id } = request {
                    let access = state_guard.as_ref().unwrap().changes_access(&session_id)?;
                    let result = if let Some((workspace, run_id, baseline)) = access {
                        drop(state_guard.take());
                        let reviewed = workspace.review_changes(&baseline, crate::agent_gateway::PermissionDecision::Allow).ok();
                        let current = state.lock().map_err(|_| "runtime-unavailable")?.baseline_is_current(&session_id, &run_id)?;
                        if current { reviewed } else { None }
                    } else { None };
                    let result = result.unwrap_or_else(|| serde_json::json!({"available":false,"files":[],"truncated":false,"reason":"No current authorized in-memory baseline is available."}));
                    serde_json::from_value::<sentinel_ipc::contract::ResponsePayload>(
                        serde_json::json!({"name":"workspace.changes","payload":result})
                    ).map_err(|_| "runtime-unavailable")
                } else if let sentinel_ipc::contract::RequestPayload::WorkspaceFiles {
                    session_id,
                } = request
                {
                    let workspace = state_guard
                        .as_ref()
                        .unwrap()
                        .workspace_files_access(&session_id)?;
                    drop(state_guard.take());
                    let result = workspace
                        .inventory(crate::agent_gateway::PermissionDecision::Allow)
                        .map_err(|_| "permission-denied")?;
                    // Revalidate session ownership after the unlocked filesystem read.
                    let current_workspace = state
                        .lock()
                        .map_err(|_| "runtime-unavailable")?
                        .workspace_files_access(&session_id)?;
                    if !Arc::ptr_eq(&workspace, &current_workspace) { return Err("runtime-busy"); }
                    serde_json::from_value::<sentinel_ipc::contract::ResponsePayload>(
                        serde_json::json!({"name":"workspace.files","payload":result}),
                    )
                    .map_err(|_| "runtime-unavailable")
                } else {
                    state_guard
                        .as_mut()
                        .unwrap()
                        .handle(request, &peer, &mut job)
                }
            });
            hello_received |=
                reply.name == "hello" && reply.kind == sentinel_ipc::MessageType::Response;
            let mut state_guard = match state_guard {
                Some(guard) => guard,
                None => state.lock().map_err(|_| io::Error::other("state lock"))?,
            };
            let mut bytes = serde_json::to_vec(&reply)?;
            if bytes.len() + 1 > sentinel_ipc::contract::MAX_FRAME_BYTES {
                return Err(invalid("reply too large"));
            }
            bytes.push(b'\n');
            if dispatcher.shutdown_requested() {
                state_guard.stop();
                stopping.store(true, Ordering::SeqCst);
                drop(state_guard);
                let delivery = peer.send_confirm(bytes);
                let notified = wake.shutdown(std::net::Shutdown::Write);
                delivery?;
                return notified;
            }
            let agent_admission =
                reply.name == "agent.start" && reply.kind == sentinel_ipc::MessageType::Response;
            let delivery = if (agent_admission
                || matches!(reply.name.as_str(), "run.cancel" | "approval.respond"))
                && reply.kind == sentinel_ipc::MessageType::Response
            {
                peer.send_confirm(bytes)
            } else {
                peer.send(bytes)
            };
            if delivery.is_ok() {
                state_guard.deliver_approval();
            } else {
                state_guard.discard_approval();
            }
            if let Some(voice_job) = state_guard.take_voice_job() {
                let state = state.clone();
                thread::spawn(move || chat::voice_worker(state, voice_job));
            }
            if let Some(job) = job {
                if agent_admission && delivery.is_err() {
                    job.cancel_before_dispatch();
                } else {
                    state_guard.start(&job);
                }
                if let Err(TrySendError::Full(Some(job)) | TrySendError::Disconnected(Some(job))) =
                    jobs.try_send(Some(job))
                {
                    job.cancel_before_dispatch();
                    drop(state_guard);
                    chat::worker(state.clone(), job);
                    return Err(io::Error::other("runtime worker unavailable"));
                }
            }
            delivery?;
        }
        Ok(())
    })();
    if let Ok(mut guard) = state.lock() {
        guard.drop_peer(&peer);
    }
    (peer.close)();
    drop(peer);
    let _ = writer_thread.join();
    result
}
fn try_acquire_client(clients: &AtomicUsize) -> bool {
    let mut current = clients.load(Ordering::SeqCst);
    loop {
        if current >= MAX_CLIENTS {
            return false;
        }
        match clients.compare_exchange(current, current + 1, Ordering::SeqCst, Ordering::SeqCst) {
            Ok(_) => return true,
            Err(actual) => current = actual,
        }
    }
}
pub fn run() -> io::Result<()> {
    let mut args = env::args_os();
    let _ = args.next();
    let profile_option = args.next();
    let persistent =
        profile_option.as_deref() == Some(std::ffi::OsStr::new("--persistent-profile"));
    if !persistent && profile_option.as_deref() != Some(std::ffi::OsStr::new("--profile")) {
        return Err(invalid(
            "usage: sentinel-daemon-dev (--profile /tmp/PRIVATE_DIRECTORY | --persistent-profile) [--provider ORIGIN --model ID [--context-window-tokens N [--image-token-upper-bound N]] [--provider-kind ID] [--provider-config KIND ORIGIN MODEL]... [--agent-workspace PATH [--workspace ID PATH]... [--agent-approval required|workspace] [--agent-process]] [--speech-binary PATH --speech-binary-sha256 HEX --speech-model PATH --speech-model-sha256 HEX [--speech-execution]] | --cloud-provider KIND MODEL PRIVATE_KEY_FILE [--cloud-history] [--cloud-attachment-root PATH [--cloud-vision CONTEXT_TOKENS IMAGE_TOKENS]] | --cloud-keychain KIND MODEL ACCOUNT [--cloud-history] [--cloud-attachment-root PATH [--cloud-vision CONTEXT_TOKENS IMAGE_TOKENS]] | --import-qt-preview-subset CONVERSATIONS SETTINGS MEMORY --acknowledge-preview-subset | --check-qt-preview-subset CONVERSATIONS SETTINGS MEMORY | --check-qt-cutover QT_DIRECTORY | --cutover-qt-profile QT_DIRECTORY --expect-source-id ID | --abort-qt-cutover QT_DIRECTORY --expect-source-id ID | --check-cloud-keychain KIND ACCOUNT | --check-qt-permission-copy /tmp/SOURCE/COPY.sqlite3 | --export-qt-conversations /tmp/NEW_DIRECTORY [--include-settings] [--include-memory] | --import-qt-settings-copy /tmp/SOURCE/settings.json | --restore-dev-set /tmp/SOURCE/SET | --restore-dev-settings-copy /tmp/SOURCE/COPY.sqlite3 | --import-qt-copy /tmp/SOURCE/COPY.sqlite3 | --import-qt-memory-copy /tmp/SOURCE/COPY.sqlite3]",
        ));
    }
    let profile = if persistent {
        PathBuf::new()
    } else {
        PathBuf::from(args.next().ok_or_else(|| invalid("missing profile"))?)
    };
    if !persistent && !profile.is_absolute() {
        return Err(invalid("profile must be an absolute path"));
    }
    let next = args.next();
    if persistent
        && next.as_ref().is_some_and(|arg| {
            arg != "--provider"
                && arg != "--cloud-openai"
                && arg != "--cloud-openai-keychain"
                && arg != "--cloud-provider"
                && arg != "--cloud-keychain"
                && arg != "--restore-dev-set"
                && arg != "--import-qt-preview-subset"
                && arg != "--check-qt-preview-subset"
                && arg != "--export-qt-conversations"
                && arg != "--check-qt-permission-copy"
                && arg != "--check-qt-cutover"
                && arg != "--cutover-qt-profile"
                && arg != "--abort-qt-cutover"
                && arg != "--check-cloud-keychain"
        })
    {
        return Err(invalid("option unavailable for persistent profiles"));
    }
    let cutover_check = next.as_deref() == Some(std::ffi::OsStr::new("--check-qt-cutover"));
    let cutover_abort = next.as_deref() == Some(std::ffi::OsStr::new("--abort-qt-cutover"));
    if cutover_check
        || cutover_abort
        || next.as_deref() == Some(std::ffi::OsStr::new("--cutover-qt-profile"))
    {
        let source = PathBuf::from(
            args.next()
                .ok_or_else(|| invalid("missing Qt profile directory"))?,
        );
        let expected_id = if cutover_check {
            None
        } else {
            if args.next().as_deref() != Some(std::ffi::OsStr::new("--expect-source-id")) {
                return Err(invalid(
                    "cutover requires --expect-source-id from the rehearsal",
                ));
            }
            Some(
                args.next()
                    .and_then(|value| value.into_string().ok())
                    .filter(|value| {
                        value.len() == 64
                            && value
                                .bytes()
                                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
                    })
                    .ok_or_else(|| invalid("invalid cutover source ID"))?,
            )
        };
        if args.next().is_some() {
            return Err(invalid("unexpected Qt cutover argument"));
        }
        if cutover_abort {
            let id = expected_id
                .as_deref()
                .ok_or_else(|| invalid("missing reservation source ID"))?;
            let report = crate::qt_cutover::abort_unpublished(&source, id)
                .map_err(|error| io::Error::other(format!("Qt cutover abort failed: {error}")))?;
            println!("{report}");
            return Ok(());
        }
        let destination = if persistent {
            persistent_profile_path()?
        } else {
            profile
        };
        let report =
            crate::qt_cutover::run(&source, &destination, persistent, expected_id.as_deref())
                .map_err(|error| io::Error::other(format!("Qt cutover failed: {error}")))?;
        println!("{report}");
        return Ok(());
    }
    if next.as_deref() == Some(std::ffi::OsStr::new("--check-cloud-keychain")) {
        #[cfg(target_os = "macos")]
        {
            let kind = args
                .next()
                .and_then(|value| value.into_string().ok())
                .ok_or_else(|| invalid("missing cloud provider"))?;
            let account = args
                .next()
                .and_then(|value| value.into_string().ok())
                .ok_or_else(|| invalid("missing Keychain account"))?;
            if args.next().is_some() {
                return Err(invalid("unexpected Keychain argument"));
            }
            Config::check_cloud_keychain(&kind, &account)
                .map_err(|error| io::Error::other(error.to_string()))?;
            println!(
                "{}",
                serde_json::json!({"provider":kind,"credential_available":true})
            );
            return Ok(());
        }
        #[cfg(not(target_os = "macos"))]
        return Err(invalid("cloud Keychain credentials require macOS"));
    }
    if next.as_deref() == Some(std::ffi::OsStr::new("--check-qt-permission-copy")) {
        let source = PathBuf::from(
            args.next()
                .ok_or_else(|| invalid("missing Qt permission copy"))?,
        );
        if args.next().is_some() {
            return Err(invalid("unexpected permission inspection argument"));
        }
        let report = crate::qt_permission_inspection::inspect(&source).map_err(|error| {
            io::Error::other(format!("Qt permission inspection failed: {error}"))
        })?;
        println!("{report}");
        return Ok(());
    }
    if next.as_deref() == Some(std::ffi::OsStr::new("--export-qt-conversations")) {
        let destination = PathBuf::from(
            args.next()
                .ok_or_else(|| invalid("missing Qt export directory"))?,
        );
        let mut include_settings = false;
        let mut include_memory = false;
        for option in args.by_ref() {
            if option == "--include-settings" && !include_settings {
                include_settings = true;
            } else if option == "--include-memory" && !include_memory {
                include_memory = true;
            } else {
                return Err(invalid("unexpected export argument"));
            }
        }
        let source = if persistent {
            let path = persistent_profile_path()?;
            checked_profile_directory(&path)?;
            profile_inventory(&path)?;
            path
        } else {
            private_directory(&profile)?;
            profile
        };
        if !fs::symlink_metadata(source.join("daemon-dev.lock"))?.is_file() {
            return Err(invalid("missing regular Rust profile lock"));
        }
        let _lock = lock_profile(&source)?;
        let report = crate::qt_conversation_export::export(
            &source,
            &destination,
            include_settings,
            include_memory,
        )
        .map_err(|error| io::Error::other(format!("Qt conversation export failed: {error}")))?;
        println!("{report}");
        return Ok(());
    }
    let check_preview = next.as_deref() == Some(std::ffi::OsStr::new("--check-qt-preview-subset"));
    if check_preview || next.as_deref() == Some(std::ffi::OsStr::new("--import-qt-preview-subset"))
    {
        let conversations = PathBuf::from(
            args.next()
                .ok_or_else(|| invalid("missing conversations copy"))?,
        );
        let settings = PathBuf::from(
            args.next()
                .ok_or_else(|| invalid("missing settings copy"))?,
        );
        let memory = PathBuf::from(args.next().ok_or_else(|| invalid("missing memory copy"))?);
        if !check_preview
            && args.next().as_deref() != Some(std::ffi::OsStr::new("--acknowledge-preview-subset"))
        {
            return Err(invalid(
                "preview import requires explicit subset acknowledgement",
            ));
        }
        let mut json_report = false;
        let mut normalize_wal = false;
        let mut allow_excluded_memory = false;
        let mut expected_source_id = None;
        while let Some(option) = args.next() {
            if option == "--report-json" && !json_report {
                json_report = true;
            } else if option == "--normalize-wal-copies" && !normalize_wal {
                normalize_wal = true;
            } else if option == "--allow-excluded-memory" && !allow_excluded_memory {
                allow_excluded_memory = true;
            } else if option == "--expect-source-id"
                && !check_preview
                && expected_source_id.is_none()
            {
                let id = args
                    .next()
                    .and_then(|value| value.into_string().ok())
                    .filter(|value| {
                        value.len() == 64
                            && value
                                .bytes()
                                .all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
                    })
                    .ok_or_else(|| invalid("expected a lowercase 64-digit source ID"))?;
                expected_source_id = Some(id);
            } else {
                return Err(invalid("unexpected preview argument"));
            }
        }
        if allow_excluded_memory
            && (!json_report || (!check_preview && expected_source_id.is_none()))
        {
            return Err(invalid(
                "excluded memory requires a JSON report and a reviewed source ID for publication",
            ));
        }
        let destination = if persistent {
            persistent_profile_path()?
        } else {
            profile
        };
        let report = crate::qt_preview_import::import(
            &destination,
            &conversations,
            &settings,
            &memory,
            persistent,
            if check_preview {
                crate::qt_preview_import::Mode::Check
            } else {
                crate::qt_preview_import::Mode::Publish
            },
            normalize_wal,
            expected_source_id.as_deref(),
            allow_excluded_memory,
        )
        .map_err(|error| io::Error::other(format!("Qt preview import failed: {error}")))?;
        if json_report {
            let model_binding = match report.settings.binding {
                crate::qt_settings_import::BindingImport::Absent => "absent",
                crate::qt_settings_import::BindingImport::Imported => "included",
                crate::qt_settings_import::BindingImport::Skipped => "skipped",
            };
            println!(
                "{}",
                serde_json::json!({
                    "schema_version": 1,
                    "outcome": if check_preview { "validated" } else { "published" },
                    "source_id": report.source_id,
                    "normalized_wal_copies": report.normalized_wal_copies,
                    "included": {
                        "conversations": report.conversations,
                        "archived": report.archived,
                        "pinned": report.pinned,
                        "messages": report.messages,
                        "attachments": report.attachments,
                        "reply_references": report.reply_references,
                        "replacement_references": report.replacement_references,
                        "session_memory": report.memory,
                        "preferences": report.settings.preferences,
                        "local_model_binding": model_binding,
                    },
                    "excluded": {
                        "ignored_settings_fields": report.settings.ignored_fields,
                        "unsupported_theme": report.settings.skipped_theme,
                        "qt_memory": report.excluded_memory,
                        "domains_not_inspected": ["credentials", "agent", "permissions", "chat_history",
                            "ipc_projections", "other_memory_stores", "other_search_indexes"],
                    },
                })
            );
            return Ok(());
        }
        if report.normalized_wal_copies > 0 {
            println!(
                "Normalized {} detached SQLite WAL copies.",
                report.normalized_wal_copies
            );
        }
        if report.settings.skipped_theme {
            println!("Unsupported Qt theme was skipped; no theme approximation was applied.");
        }
        let outcome = if check_preview {
            "Validated preview subset without publishing a profile"
        } else {
            "Imported preview subset"
        };
        println!(
            "{outcome}: {} conversations ({} archived, {} pinned), {} messages, {} session memories, {} preferences. {} Excluded: other Qt settings, credentials, Agent runs, permissions, chat history, IPC projections, workspace/global memory and other stores.",
            report.conversations,
            report.archived,
            report.pinned,
            report.messages,
            report.memory,
            report.settings.preferences,
            report.settings.binding.summary(),
        );
        return Ok(());
    }
    if next.as_deref() == Some(std::ffi::OsStr::new("--import-qt-settings-copy")) {
        let source = PathBuf::from(
            args.next()
                .ok_or_else(|| invalid("missing Qt settings copy"))?,
        );
        if args.next().is_some() {
            return Err(invalid("unexpected argument"));
        }
        private_directory(&profile)?;
        let profile = profile.canonicalize()?;
        let _lock = lock_profile(&profile)?;
        match fs::symlink_metadata(profile.join("daemon-dev.sock")) {
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            _ => return Err(invalid("destination endpoint exists")),
        }
        let report = crate::qt_settings_import::import(&profile, &source)
            .map_err(|_| invalid("unsupported Qt settings copy or existing destination setting"))?;
        println!(
            "Imported {} supported preferences. {} Other Qt settings and credentials were excluded.",
            report.preferences,
            report.binding.summary(),
        );
        return Ok(());
    }
    if next.as_deref() == Some(std::ffi::OsStr::new("--restore-dev-set")) {
        let source = PathBuf::from(args.next().ok_or_else(|| invalid("missing snapshot set"))?);
        if args.next().is_some() {
            return Err(invalid("unexpected argument"));
        }
        let destination = if persistent {
            persistent_profile_path()?
        } else {
            profile
        };
        crate::set_restore::restore(&destination, &source, persistent).map_err(|error| {
            io::Error::other(format!("development snapshot restore failed: {error}"))
        })?;
        println!("Restored development profile snapshot set");
        return Ok(());
    }
    if next.as_deref() == Some(std::ffi::OsStr::new("--restore-dev-settings-copy")) {
        let source = PathBuf::from(
            args.next()
                .ok_or_else(|| invalid("missing development settings snapshot"))?,
        );
        if args.next().is_some() {
            return Err(invalid("unexpected argument"));
        }
        private_directory(&profile)?;
        let profile = profile.canonicalize()?;
        let _lock = lock_profile(&profile)?;
        match fs::symlink_metadata(profile.join("daemon-dev.sock")) {
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            _ => return Err(invalid("destination endpoint exists")),
        }
        for (name, app, version, tables) in [
            (
                crate::store::DATABASE,
                crate::store::APP_ID,
                crate::store::VERSION,
                &["sessions", "runs", "messages"][..],
            ),
            (
                crate::agent_store::DATABASE,
                crate::agent_store::APP_ID,
                crate::agent_store::VERSION,
                &["runs", "observations", "approvals", "plan_steps"][..],
            ),
            (
                crate::memory::DATABASE,
                crate::memory::APP_ID,
                crate::memory::VERSION,
                &["entries", "qt_imports"][..],
            ),
        ] {
            let (db, _, _) = crate::store::open_database(&profile, name, app, version, true)
                .map_err(|_| invalid("initialized destination required"))?;
            for table in tables {
                let count: i64 = db
                    .query_row(&format!("SELECT count(*) FROM {table}"), [], |r| r.get(0))
                    .map_err(|_| invalid("invalid destination store"))?;
                if count != 0 {
                    return Err(invalid("destination profile is not empty"));
                }
            }
        }
        crate::settings::Settings::restore_snapshot(&profile, &source)
            .map_err(|_| invalid("invalid settings snapshot or nonempty destination"))?;
        println!("Restored development settings");
        return Ok(());
    }
    if next.as_deref() == Some(std::ffi::OsStr::new("--import-qt-copy")) {
        let source = PathBuf::from(args.next().ok_or_else(|| invalid("missing Qt snapshot"))?);
        if args.next().is_some() {
            return Err(invalid("unexpected argument"));
        }
        private_directory(&profile)?;
        let profile = profile.canonicalize()?;
        let _lock = lock_profile(&profile)?;
        match fs::symlink_metadata(profile.join("daemon-dev.sock")) {
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            _ => return Err(invalid("destination endpoint exists")),
        }
        let snapshot = crate::import::read(&source, &profile)
            .map_err(|_| invalid("unsupported or unsafe Qt snapshot"))?;
        let mut store = crate::store::Store::open_for_import(&profile)
            .map_err(|_| invalid("development destination unavailable"))?;
        store
            .import_snapshot(&snapshot)
            .map_err(|_| invalid("import failed or destination not empty"))?;
        let messages: usize = snapshot
            .conversations
            .iter()
            .map(|c| c.messages.len())
            .sum();
        println!(
            "Imported {} conversations and {} messages",
            snapshot.conversations.len(),
            messages
        );
        return Ok(());
    }
    if next.as_deref() == Some(std::ffi::OsStr::new("--import-qt-memory-copy")) {
        let source = PathBuf::from(
            args.next()
                .ok_or_else(|| invalid("missing Qt memory snapshot"))?,
        );
        if args.next().is_some() {
            return Err(invalid("unexpected argument"));
        }
        private_directory(&profile)?;
        let profile = profile.canonicalize()?;
        let _lock = lock_profile(&profile)?;
        match fs::symlink_metadata(profile.join("daemon-dev.sock")) {
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            _ => return Err(invalid("destination endpoint exists")),
        }
        let chat = crate::store::Store::open_for_import(&profile)
            .map_err(|_| invalid("development destination unavailable"))?;
        let sessions = chat
            .load()
            .map_err(|_| invalid("development destination unavailable"))?
            .into_iter()
            .map(|s| s.id)
            .collect::<Vec<_>>();
        let snapshot = crate::memory_import::read(&source, &profile, &sessions)
            .map_err(|_| invalid("unsupported or unsafe Qt memory snapshot"))?;
        let mut memory = crate::memory::MemoryStore::open_for_import(&profile)
            .map_err(|_| invalid("development memory destination unavailable"))?;
        memory
            .import_snapshot(&snapshot)
            .map_err(|_| invalid("memory import failed or destination not empty"))?;
        println!("Imported {} session memory entries", snapshot.rows.len());
        return Ok(());
    }
    let mut agent_workspace = None;
    let mut plugin_root = None;
    let mut speech_binary = None;
    let mut speech_binary_hash = None;
    let mut speech_model = None;
    let mut speech_model_hash = None;
    let mut speech_execution = false;
    let mut extra_workspaces: Vec<(String, PathBuf)> = Vec::new();
    let mut process_enabled = false;
    let mut approval_required = false;
    let mut seen_approval = false;
    let mut other_providers = Vec::new();
    let mut cloud_key_path = None;
    let cloud_flag = next.as_deref().and_then(std::ffi::OsStr::to_str);
    let mut provider = if matches!(
        cloud_flag,
        Some(
            "--cloud-openai" | "--cloud-openai-keychain" | "--cloud-provider" | "--cloud-keychain"
        )
    ) {
        let mut string_arg = || {
            args.next()
                .and_then(|value| value.into_string().ok())
                .ok_or_else(|| invalid("missing cloud provider argument"))
        };
        let kind = if matches!(cloud_flag, Some("--cloud-provider" | "--cloud-keychain")) {
            string_arg()?
        } else {
            "openai".into()
        };
        let model = string_arg()?;
        if matches!(
            cloud_flag,
            Some("--cloud-openai-keychain" | "--cloud-keychain")
        ) {
            #[cfg(not(target_os = "macos"))]
            return Err(invalid("cloud Keychain credentials require macOS"));
            #[cfg(target_os = "macos")]
            {
                let account = string_arg()?;
                Some(
                    Config::cloud_keychain(&kind, &model, &account)
                        .map_err(|error| invalid(&error.to_string()))?,
                )
            }
        } else {
            let key_file = PathBuf::from(
                args.next()
                    .ok_or_else(|| invalid("missing cloud key file"))?,
            );
            cloud_key_path = Some(key_file.canonicalize()?);
            Some(
                Config::cloud(&kind, &model, &key_file)
                    .map_err(|error| invalid(&error.to_string()))?,
            )
        }
    } else if let Some(flag) = next {
        if flag != "--provider" {
            return Err(invalid("expected --provider"));
        }
        let endpoint = args
            .next()
            .and_then(|v| v.into_string().ok())
            .ok_or_else(|| invalid("missing provider origin"))?;
        if args.next().as_deref() != Some(std::ffi::OsStr::new("--model")) {
            return Err(invalid("expected --model"));
        }
        let model = args
            .next()
            .and_then(|v| v.into_string().ok())
            .ok_or_else(|| invalid("missing model"))?;
        let mut config = Config::parse(&endpoint, &model)
            .map_err(|_| invalid("invalid provider configuration"))?;
        let mut vision_tokens = None;
        let mut seen_context = false;
        let mut seen_kind = false;
        while let Some(flag) = args.next() {
            if flag == "--context-window-tokens" && !seen_context {
                seen_context = true;
                let raw = args
                    .next()
                    .and_then(|value| value.into_string().ok())
                    .ok_or_else(|| invalid("missing context window"))?;
                if raw.is_empty() || !raw.bytes().all(|b| b.is_ascii_digit()) {
                    return Err(invalid("invalid context window"));
                }
                let tokens = raw
                    .parse::<usize>()
                    .map_err(|_| invalid("invalid context window"))?;
                config = config
                    .with_context_window(tokens)
                    .map_err(|_| invalid("invalid context window"))?;
            } else if flag == "--image-token-upper-bound" && vision_tokens.is_none() {
                vision_tokens = Some(
                    args.next()
                        .and_then(|v| v.to_str()?.parse::<usize>().ok())
                        .ok_or_else(|| invalid("invalid image token upper bound"))?,
                );
            } else if flag == "--provider-config"
                && other_providers.len() < crate::provider_catalog::MAX_CONFIGURED_PROVIDERS - 1
            {
                let mut value = || {
                    args.next()
                        .and_then(|v| v.into_string().ok())
                        .ok_or_else(|| invalid("expected --provider-config KIND ORIGIN MODEL"))
                };
                let kind = value()?;
                let endpoint = value()?;
                let model = value()?;
                other_providers.push(
                    Config::parse(&endpoint, &model)
                        .and_then(|config| config.with_provider_kind(&kind))
                        .map_err(|_| invalid("invalid additional provider configuration"))?,
                );
            } else if flag == "--provider-kind" && !seen_kind {
                seen_kind = true;
                let kind = args
                    .next()
                    .and_then(|value| value.into_string().ok())
                    .ok_or_else(|| invalid("missing provider kind"))?;
                config = config
                    .with_provider_kind(&kind)
                    .map_err(|_| invalid("invalid provider kind"))?;
            } else if flag == "--agent-approval" && !seen_approval {
                seen_approval = true;
                approval_required = match args.next().and_then(|v| v.into_string().ok()).as_deref()
                {
                    Some("required") => true,
                    Some("workspace") => false,
                    _ => return Err(invalid("invalid Agent approval mode")),
                };
            } else if flag == "--speech-execution" && !speech_execution {
                speech_execution = true;
            } else if flag == "--speech-binary" && speech_binary.is_none() {
                speech_binary = Some(PathBuf::from(
                    args.next()
                        .ok_or_else(|| invalid("missing speech binary"))?,
                ));
            } else if flag == "--speech-binary-sha256" && speech_binary_hash.is_none() {
                speech_binary_hash = Some(
                    args.next()
                        .and_then(|v| v.into_string().ok())
                        .ok_or_else(|| invalid("missing speech binary hash"))?,
                );
            } else if flag == "--speech-model" && speech_model.is_none() {
                speech_model = Some(PathBuf::from(
                    args.next().ok_or_else(|| invalid("missing speech model"))?,
                ));
            } else if flag == "--speech-model-sha256" && speech_model_hash.is_none() {
                speech_model_hash = Some(
                    args.next()
                        .and_then(|v| v.into_string().ok())
                        .ok_or_else(|| invalid("missing speech model hash"))?,
                );
            } else if flag == "--plugin-root" && plugin_root.is_none() {
                let path =
                    PathBuf::from(args.next().ok_or_else(|| invalid("missing plugin root"))?);
                if !path.is_absolute() {
                    return Err(invalid("plugin root must be absolute"));
                }
                plugin_root = Some(path);
            } else if flag == "--agent-process" && !process_enabled {
                process_enabled = true;
            } else if flag == "--workspace" {
                let id = args
                    .next()
                    .and_then(|v| v.into_string().ok())
                    .ok_or_else(|| invalid("missing workspace ID"))?;
                let path = PathBuf::from(
                    args.next()
                        .ok_or_else(|| invalid("missing workspace path"))?,
                );
                let path_text = path
                    .to_str()
                    .ok_or_else(|| invalid("invalid workspace path"))?;
                if id == "default"
                    || !crate::settings::valid_workspace_binding(&id, path_text)
                    || extra_workspaces.len() >= 15
                    || extra_workspaces.iter().any(|(existing, _)| existing == &id)
                {
                    return Err(invalid("invalid or duplicate workspace"));
                }
                extra_workspaces.push((id, path));
            } else if flag == "--agent-workspace" && agent_workspace.is_none() {
                let path = PathBuf::from(
                    args.next()
                        .ok_or_else(|| invalid("missing Agent workspace"))?,
                );
                if !path.is_absolute() {
                    return Err(invalid("Agent workspace must be absolute"));
                }
                agent_workspace = Some(path);
            } else {
                return Err(invalid("unknown or duplicate option"));
            }
        }
        if let Some(tokens) = vision_tokens {
            if !seen_context {
                return Err(invalid(
                    "image token upper bound requires explicit context window",
                ));
            }
            config = config
                .with_vision_budget(tokens)
                .map_err(|_| invalid("invalid vision configuration"))?;
        }
        Some(config)
    } else {
        None
    };
    let mut cloud_vision = false;
    while let Some(flag) = args.next() {
        let config = provider
            .as_mut()
            .filter(|config| config.is_cloud())
            .ok_or_else(|| invalid("unexpected argument"))?;
        if flag == "--cloud-history" && !config.cloud_history {
            config.cloud_history = true;
        } else if flag == "--cloud-attachment-root" && agent_workspace.is_none() {
            let root = PathBuf::from(
                args.next()
                    .ok_or_else(|| invalid("missing cloud attachment root"))?,
            );
            if !root.is_absolute() {
                return Err(invalid("cloud attachment root must be absolute"));
            }
            agent_workspace = Some(root);
        } else if flag == "--cloud-vision" && !cloud_vision && config.supports_cloud_vision() {
            let mut tokens = || {
                args.next()
                    .and_then(|value| value.to_str()?.parse::<usize>().ok())
                    .ok_or_else(|| invalid("cloud vision requires context and image token limits"))
            };
            let context = tokens()?;
            let image = tokens()?;
            *config = config
                .clone()
                .with_context_window(context)
                .and_then(|config| config.with_vision_budget(image))
                .map_err(|_| invalid("invalid cloud vision token limits"))?;
            cloud_vision = true;
        } else {
            return Err(invalid("unknown, unsupported or duplicate cloud option"));
        }
    }
    if cloud_vision && agent_workspace.is_none() {
        return Err(invalid(
            "cloud vision requires an explicit cloud attachment root",
        ));
    }
    if (seen_approval || process_enabled || !extra_workspaces.is_empty())
        && agent_workspace.is_none()
    {
        return Err(invalid("Agent approval mode requires workspace"));
    }
    let speech_inspection = match (
        speech_binary,
        speech_binary_hash,
        speech_model,
        speech_model_hash,
    ) {
        (None, None, None, None) => None,
        (Some(binary), Some(binary_hash), Some(model), Some(model_hash)) => Some(
            crate::speech_inspection::inspect_pair(&binary, &binary_hash, &model, &model_hash)
                .map_err(|_| invalid("speech artifact inspection failed"))?,
        ),
        _ => {
            return Err(invalid(
                "speech inspection requires both paths and both SHA-256 hashes",
            ));
        }
    };
    if speech_execution && (speech_inspection.is_none() || !crate::speech_runner::available()) {
        return Err(invalid(
            "speech execution requires inspected artifacts and macOS",
        ));
    }
    let profile = if persistent {
        persistent_profile()?
    } else {
        private_directory(&profile)?;
        profile.canonicalize()?
    };
    let _lock = lock_profile(&profile)?;
    let path = profile.join("daemon-dev.sock");
    if persistent {
        profile_inventory(&profile)?;
        remove_stale_persistent_socket(&path)?;
    }
    let listener = UnixListener::bind(&path)?;
    let meta = fs::symlink_metadata(&path)?;
    let _endpoint = Endpoint {
        path: path.clone(),
        dev: meta.dev(),
        ino: meta.ino(),
    };
    fs::set_permissions(&path, fs::Permissions::from_mode(0o600))?;
    let settings = crate::settings::Settings::open(&profile)
        .map_err(|_| invalid("development settings unavailable"))?;
    let startup_providers =
        crate::provider_catalog::validate(provider.into_iter().chain(other_providers).collect())
            .map_err(invalid)?;
    let mut providers = startup_providers.clone();
    let preferences = settings
        .desktop_values()
        .map_err(|_| invalid("development settings unavailable"))?;
    for (key, provider_id) in crate::provider_catalog::ENDPOINT_KEYS {
        if let Some((startup, endpoint)) = preferences[key]
            .as_str()
            .and_then(crate::provider_catalog::endpoint_override)
            && let Some(config) = providers
                .iter_mut()
                .find(|config| config.provider_id == provider_id && config.endpoint == startup)
        {
            *config = crate::provider_catalog::rebind(config, &endpoint)
                .map_err(|_| invalid("development provider settings unavailable"))?;
        }
    }
    let mut provider = providers.first().cloned();
    for config in &providers {
        if let Some(model) = settings
            .restore(config)
            .map_err(|_| invalid("development settings unavailable"))?
        {
            let mut selected = config.clone();
            selected.model = model;
            provider = Some(selected);
            break;
        }
    }
    let mut workspaces = Vec::new();
    if let Some(path) = agent_workspace {
        let mut configured = vec![("default".to_owned(), path)];
        configured.extend(extra_workspaces);
        for (id, path) in configured {
            let handle = Arc::new(crate::agent_gateway::Workspace::open(&path)?);
            let root = handle
                .inventory_root()?
                .into_os_string()
                .into_string()
                .map_err(|_| invalid("workspace path is not UTF-8"))?;
            if provider.as_ref().is_some_and(Config::is_cloud)
                && (profile.starts_with(&root)
                    || cloud_key_path
                        .as_ref()
                        .is_some_and(|path| path.starts_with(&root)))
            {
                return Err(invalid(
                    "cloud attachment root contains profile data or the API key file",
                ));
            }
            if !crate::settings::valid_workspace_binding(&id, &root)
                || workspaces
                    .iter()
                    .any(|workspace: &chat::ConfiguredWorkspace| workspace.root == root)
            {
                return Err(invalid("invalid or duplicate workspace root"));
            }
            workspaces.push(chat::ConfiguredWorkspace { id, root, handle });
        }
    }
    let persisted_workspace = settings
        .workspace_selection()
        .map_err(|_| invalid("development settings unavailable"))?;
    let selected_workspace = persisted_workspace
        .and_then(|(id, root)| {
            workspaces
                .iter()
                .find(|w| w.id == id && w.root == root)
                .map(|w| w.id.clone())
        })
        .or_else(|| workspaces.first().map(|w| w.id.clone()));
    let memory = crate::memory::MemoryStore::open(&profile)
        .map_err(|_| invalid("development memory unavailable"))?;
    let agent_store = crate::agent_store::AgentStore::open(&profile)
        .map_err(|_| invalid("development Agent audit unavailable"))?;
    let mut store = crate::store::Store::open_deferred(&profile)
        .map_err(|_| invalid("development store unavailable"))?;
    crate::store::Store::recover_agent_runs(&profile)
        .map_err(|_| invalid("development Agent recovery failed"))?;
    store
        .recover_interrupted()
        .map_err(|_| invalid("development Chat recovery failed"))?;
    let mut daemon_state = chat::State::new(
        provider,
        store,
        settings,
        agent_store,
        memory,
        chat::AgentOptions {
            workspaces,
            selected_workspace,
            approval_required,
            process_enabled,
        },
        profile.clone(),
    )
    .map_err(|_| invalid("development store unavailable"))?;
    daemon_state.set_configured_providers(startup_providers, providers);
    if let Some(root) = plugin_root {
        daemon_state.set_plugin_root(Arc::new(crate::agent_gateway::Workspace::open(&root)?));
    }
    if let Some(inspection) = speech_inspection {
        daemon_state.set_speech_inspection(inspection);
    }
    if speech_execution {
        daemon_state.enable_speech_execution();
    }
    let state = Arc::new(Mutex::new(daemon_state));
    listener.set_nonblocking(true)?;
    let (jobs, pending_jobs) = sync_channel::<Option<chat::Job>>(1);
    let worker_state = state.clone();
    let run_worker = thread::Builder::new()
        .name("sentinel-run-worker".into())
        .spawn(move || {
            while let Ok(Some(job)) = pending_jobs.recv() {
                chat::worker(worker_state.clone(), job);
            }
        })?;
    let started = Instant::now();
    let stopping = Arc::new(AtomicBool::new(false));
    let clients = Arc::new(AtomicUsize::new(0));
    let (wake_read, wake_write) = UnixStream::pair()?;
    let wake_write = Arc::new(wake_write);
    let mut signals = signal_hook::iterator::Signals::new([libc::SIGINT, libc::SIGTERM])?;
    let signal_handle = signals.handle();
    let signal_worker = {
        let state = state.clone();
        let stopping = stopping.clone();
        let wake = wake_write.clone();
        thread::Builder::new()
            .name("sentinel-shutdown-signal".into())
            .spawn(move || {
                if signals.forever().next().is_some() {
                    if let Ok(mut state) = state.lock() {
                        state.stop();
                    }
                    stopping.store(true, Ordering::SeqCst);
                    let _ = wake.shutdown(std::net::Shutdown::Write);
                }
            })?
    };
    let accept_result = (|| -> io::Result<()> {
        while !stopping.load(Ordering::SeqCst) {
            let mut fds = [
                libc::pollfd {
                    fd: listener.as_raw_fd(),
                    events: libc::POLLIN,
                    revents: 0,
                },
                libc::pollfd {
                    fd: wake_read.as_raw_fd(),
                    events: libc::POLLIN,
                    revents: 0,
                },
            ];
            let ready = unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::nfds_t, -1) };
            if ready < 0 {
                let error = io::Error::last_os_error();
                if error.kind() == io::ErrorKind::Interrupted {
                    continue;
                }
                return Err(error);
            }
            if stopping.load(Ordering::SeqCst) {
                break;
            }
            if fds[1].revents != 0 {
                return Err(io::Error::other("shutdown notification failed"));
            }
            if fds[0].revents & libc::POLLIN == 0 {
                return Err(io::Error::other("listener poll failed"));
            }
            match listener.accept() {
                Ok((stream, _)) => {
                    if !try_acquire_client(&clients) {
                        continue;
                    }
                    let state = state.clone();
                    let stopping = stopping.clone();
                    let clients = clients.clone();
                    let wake = wake_write.clone();
                    let jobs = jobs.clone();
                    thread::spawn(move || {
                        let _ = serve(stream, started, state, stopping, wake, jobs);
                        clients.fetch_sub(1, Ordering::SeqCst);
                    });
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => continue,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) => return Err(error),
            }
        }
        Ok(())
    })();
    signal_handle.close();
    let _ = signal_worker.join();
    if accept_result.is_err() {
        state
            .lock()
            .map_err(|_| io::Error::other("state lock"))?
            .stop();
        stopping.store(true, Ordering::SeqCst);
    }
    // Keep the profile lock until workers have committed their terminal state,
    // or return a failure so restart recovery can reconcile interrupted runs.
    let deadline = Instant::now() + SHUTDOWN_DRAIN;
    while state
        .lock()
        .map_err(|_| io::Error::other("state lock"))?
        .shutdown_pending()?
    {
        if Instant::now() >= deadline {
            return Err(io::Error::other(
                "runtime workers did not stop before shutdown",
            ));
        }
        thread::sleep(Duration::from_millis(20));
    }
    jobs.try_send(None)
        .map_err(|_| io::Error::other("runtime worker did not accept shutdown"))?;
    run_worker
        .join()
        .map_err(|_| io::Error::other("runtime worker panicked"))?;
    accept_result
}

#[cfg(test)]
mod tests {
    use super::{AtomicUsize, MAX_CLIENTS, Ordering, try_acquire_client};

    #[test]
    fn client_admission_stops_at_the_limit() {
        let clients = AtomicUsize::new(0);
        for expected in 1..=MAX_CLIENTS {
            assert!(try_acquire_client(&clients));
            assert_eq!(clients.load(Ordering::SeqCst), expected);
        }
        assert!(!try_acquire_client(&clients));
        assert_eq!(clients.load(Ordering::SeqCst), MAX_CLIENTS);
    }
}
