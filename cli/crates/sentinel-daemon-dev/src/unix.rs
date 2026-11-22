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
            fs::{FileTypeExt, MetadataExt, OpenOptionsExt, PermissionsExt},
            net::{UnixListener, UnixStream},
        },
    },
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering},
        mpsc::sync_channel,
    },
    thread,
    time::{Duration, Instant},
};

const CLIENT_LIFETIME: Duration = Duration::from_secs(1800);
const MAX_CLIENTS: usize = 8;
const FRAME_LIFETIME: Duration = Duration::from_secs(3);
const WRITE_TIMEOUT: Duration = Duration::from_secs(3);

fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}
fn private_directory(path: &Path) -> io::Result<()> {
    let metadata = fs::symlink_metadata(path)?;
    // A development profile is a direct child of /tmp, never a real Sentinel profile.
    let parent = path
        .parent()
        .ok_or_else(|| invalid("profile needs a parent"))?
        .canonicalize()?;
    if parent != Path::new("/tmp").canonicalize()?
        || !metadata.file_type().is_dir()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.mode() & 0o7777 != 0o700
    {
        return Err(invalid(
            "profile must be an existing private owner-owned directory directly under /tmp",
        ));
    }
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
        ];
        if state
            .lock()
            .map_err(|_| io::Error::other("state lock"))?
            .has_provider()
        {
            capabilities.extend([
                "chat".into(),
                "events".into(),
                "model-selection".into(),
                "chat-revisions".into(),
            ]);
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
            .has_workspace_files()
        {
            capabilities.push("workspace-files".into());
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
            .has_ollama_operations()
        {
            capabilities.push("ollama-model-operations".into());
        }
        dispatcher.set_capabilities(capabilities);
        let deadline = Instant::now() + CLIENT_LIFETIME;
        loop {
            wait_input(&reader, deadline)?;
            let frame = read_frame(&mut reader, deadline.min(Instant::now() + FRAME_LIFETIME))?;
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
                    let models = crate::provider::discover_models(&config, &AtomicBool::new(false))
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
                peer.send_confirm(bytes)?;
                stopping.store(true, Ordering::SeqCst);
                wake.shutdown(std::net::Shutdown::Write)?;
                return Ok(());
            }
            let delivery = if matches!(reply.name.as_str(), "run.cancel" | "approval.respond")
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
            if let Some(job) = job {
                state_guard.start(&job);
                let state = state.clone();
                thread::spawn(move || chat::worker(state, job));
            }
            delivery?;
        }
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
    if args.next().as_deref() != Some(std::ffi::OsStr::new("--profile")) {
        return Err(invalid(
            "usage: sentinel-daemon-dev --profile /tmp/PRIVATE_DIRECTORY [--provider ORIGIN --model ID [--context-window-tokens N] [--provider-kind ID] [--agent-workspace PATH [--workspace ID PATH]... [--agent-approval required|workspace] [--agent-process]] | --restore-dev-set /tmp/SOURCE/SET | --restore-dev-settings-copy /tmp/SOURCE/COPY.sqlite3 | --import-qt-copy /tmp/SOURCE/COPY.sqlite3 | --import-qt-memory-copy /tmp/SOURCE/COPY.sqlite3]",
        ));
    }
    let profile = PathBuf::from(args.next().ok_or_else(|| invalid("missing profile"))?);
    if !profile.is_absolute() {
        return Err(invalid("profile must be an absolute path"));
    }
    let next = args.next();
    if next.as_deref() == Some(std::ffi::OsStr::new("--restore-dev-set")) {
        let source = PathBuf::from(args.next().ok_or_else(|| invalid("missing snapshot set"))?);
        if args.next().is_some() {
            return Err(invalid("unexpected argument"));
        }
        crate::set_restore::restore(&profile, &source).map_err(|error| {
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
    let mut extra_workspaces: Vec<(String, PathBuf)> = Vec::new();
    let mut process_enabled = false;
    let mut approval_required = false;
    let mut seen_approval = false;
    let mut provider = if let Some(flag) = next {
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
        Some(config)
    } else {
        None
    };
    if args.next().is_some() {
        return Err(invalid("unexpected argument"));
    }
    if (seen_approval || process_enabled || !extra_workspaces.is_empty())
        && agent_workspace.is_none()
    {
        return Err(invalid("Agent approval mode requires workspace"));
    }
    private_directory(&profile)?;
    let profile = profile.canonicalize()?;
    let _lock = lock_profile(&profile)?;
    // bind never unlinks a pre-existing file or socket, including crash-stale sockets.
    let path = profile.join("daemon-dev.sock");
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
    if let Some(config) = &mut provider
        && let Some(model) = settings
            .restore(config)
            .map_err(|_| invalid("development settings unavailable"))?
    {
        config.model = model;
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
    let mut agent_store = crate::agent_store::AgentStore::open(&profile)
        .map_err(|_| invalid("development Agent audit unavailable"))?;
    let mut store = crate::store::Store::open_deferred(&profile)
        .map_err(|_| invalid("development store unavailable"))?;
    let pending = agent_store
        .pending()
        .map_err(|_| invalid("development Agent audit unavailable"))?;
    for session in store
        .load()
        .map_err(|_| invalid("development store unavailable"))?
    {
        if let Some(owner) = agent_store
            .owner(&session.run_id)
            .map_err(|_| invalid("development Agent audit unavailable"))?
            && owner != session.id
        {
            return Err(invalid("Agent projection identity mismatch"));
        }
    }
    for id in &pending {
        let owner = agent_store
            .owner(id)
            .map_err(|_| invalid("development Agent recovery failed"))?
            .ok_or_else(|| invalid("missing Agent recovery owner"))?;
        store
            .validate_agent_owner(id, &owner)
            .map_err(|_| invalid("Agent recovery identity mismatch"))?;
    }
    store
        .recover_interrupted()
        .map_err(|_| invalid("development Chat recovery failed"))?;
    for id in pending {
        let owner = agent_store
            .owner(&id)
            .map_err(|_| invalid("development Agent recovery failed"))?
            .ok_or_else(|| invalid("missing Agent recovery owner"))?;
        store
            .validate_agent_owner(&id, &owner)
            .map_err(|_| invalid("Agent recovery identity mismatch"))?;
        let possible_write = agent_store
            .may_have_written(&id)
            .map_err(|_| invalid("development Agent recovery failed"))?;
        store
            .invalidate_agent(&id, possible_write)
            .map_err(|_| invalid("development Agent recovery failed"))?;
        agent_store
            .recover(&id)
            .map_err(|_| invalid("development Agent recovery failed"))?;
    }
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
    if let Some(root) = plugin_root {
        daemon_state.set_plugin_root(Arc::new(crate::agent_gateway::Workspace::open(&root)?));
    }
    let state = Arc::new(Mutex::new(daemon_state));
    listener.set_nonblocking(true)?;
    let started = Instant::now();
    let stopping = Arc::new(AtomicBool::new(false));
    let clients = Arc::new(AtomicUsize::new(0));
    let (wake_read, wake_write) = UnixStream::pair()?;
    let wake_write = Arc::new(wake_write);
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
                thread::spawn(move || {
                    let _ = serve(stream, started, state, stopping, wake);
                    clients.fetch_sub(1, Ordering::SeqCst);
                });
            }
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => continue,
            Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
            Err(error) => return Err(error),
        }
    }
    Ok(())
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
