//! Isolated Windows development pipe with opt-in local model discovery and Chat.
#[path = "windows_chat.rs"]
mod windows_chat;
use crate::windows_profile;
use sentinel_daemon_dev::provider::{self, Config};
use sentinel_ipc::{
    Envelope, MessageType,
    contract::{self, RequestPayload, ResponsePayload},
    server::Dispatcher,
};
use serde_json::json;
use std::{
    env,
    ffi::OsStr,
    io,
    os::windows::{
        ffi::OsStrExt,
        io::{AsRawHandle, FromRawHandle, OwnedHandle},
    },
    path::PathBuf,
    ptr::{null, null_mut},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
        mpsc::{SyncSender, TrySendError, sync_channel},
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::{
        ERROR_IO_INCOMPLETE, ERROR_IO_PENDING, ERROR_PIPE_CONNECTED, GetLastError,
        INVALID_HANDLE_VALUE, WAIT_FAILED, WAIT_OBJECT_0, WAIT_TIMEOUT,
    },
    Security::{EqualSid, GetTokenInformation, IsValidSid, TOKEN_QUERY, TOKEN_USER, TokenUser},
    Storage::FileSystem::{
        FILE_FLAG_FIRST_PIPE_INSTANCE, FILE_FLAG_OVERLAPPED, PIPE_ACCESS_DUPLEX, ReadFile,
        WriteFile,
    },
    System::{
        IO::{CancelIoEx, GetOverlappedResult, OVERLAPPED},
        Pipes::{
            ConnectNamedPipe, CreateNamedPipeW, DisconnectNamedPipe, GetNamedPipeClientProcessId,
            PIPE_READMODE_BYTE, PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_BYTE, PIPE_WAIT,
        },
        Threading::{
            CreateEventW, OpenProcess, OpenProcessToken, PROCESS_QUERY_LIMITED_INFORMATION,
            SetEvent, WaitForMultipleObjects, WaitForSingleObject,
        },
    },
};

const ACCEPT_WAIT: Duration = Duration::from_secs(300);
const CLIENT_LIFETIME: Duration = Duration::from_secs(1800);
const IO_WAIT: Duration = Duration::from_secs(3);
const CANCEL_GRACE_MS: u32 = 250;
const MAX_CLIENTS: usize = 8;
const MAX_PIPE_INSTANCES: u32 = (MAX_CLIENTS + 2) as u32;
static QUARANTINED: AtomicBool = AtomicBool::new(false);
fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message)
}
fn raw(h: &OwnedHandle) -> windows_sys::Win32::Foundation::HANDLE {
    h.as_raw_handle() as _
}
fn owned(h: windows_sys::Win32::Foundation::HANDLE) -> io::Result<OwnedHandle> {
    if h.is_null() || h == INVALID_HANDLE_VALUE {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: a newly created handle is transferred exactly once.
    Ok(unsafe { OwnedHandle::from_raw_handle(h) })
}
fn wait_ms(deadline: Instant) -> io::Result<u32> {
    let duration = deadline
        .checked_duration_since(Instant::now())
        .filter(|d| !d.is_zero())
        .ok_or_else(|| io::Error::new(io::ErrorKind::TimedOut, "deadline"))?;
    Ok(duration
        .as_millis()
        .saturating_add(1)
        .min(u32::MAX as u128 - 1) as u32)
}
fn token_user(process: windows_sys::Win32::Foundation::HANDLE) -> io::Result<Vec<usize>> {
    let mut token = null_mut();
    if unsafe { OpenProcessToken(process, TOKEN_QUERY, &mut token) } == 0 {
        return Err(io::Error::last_os_error());
    }
    let token = owned(token)?;
    let mut size = 0;
    unsafe {
        GetTokenInformation(raw(&token), TokenUser, null_mut(), 0, &mut size);
    }
    if size == 0 || size > 65536 {
        return Err(invalid("invalid client token"));
    }
    let mut data = vec![0usize; (size as usize).div_ceil(std::mem::size_of::<usize>())];
    if unsafe {
        GetTokenInformation(
            raw(&token),
            TokenUser,
            data.as_mut_ptr().cast(),
            size,
            &mut size,
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    Ok(data)
}
fn verify_client(pipe: &OwnedHandle, current: &[usize]) -> io::Result<()> {
    let mut pid = 0;
    if unsafe { GetNamedPipeClientProcessId(raw(pipe), &mut pid) } == 0 || pid == 0 {
        return Err(invalid("unverified pipe client"));
    }
    let process = owned(unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) })?;
    let user = token_user(raw(&process))?;
    // SAFETY: aligned buffers hold valid TOKEN_USER records for this comparison.
    let a = unsafe { (*(current.as_ptr().cast::<TOKEN_USER>())).User.Sid };
    let b = unsafe { (*(user.as_ptr().cast::<TOKEN_USER>())).User.Sid };
    if unsafe { EqualSid(a, b) } == 0 {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "foreign pipe client",
        ));
    }
    let mut confirmed = 0;
    if unsafe { GetNamedPipeClientProcessId(raw(pipe), &mut confirmed) } == 0 || confirmed != pid {
        return Err(invalid("pipe client identity changed"));
    }
    Ok(())
}
fn create_pipe(name: &OsStr, current: &[usize], first_instance: bool) -> io::Result<OwnedHandle> {
    let mut wide: Vec<u16> = name.encode_wide().collect();
    if wide.is_empty() || wide.contains(&0) {
        return Err(invalid("invalid pipe name"));
    }
    wide.push(0);
    let mut open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if first_instance {
        open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    }
    windows_profile::with_owner_only_security(current, |attributes| {
        owned(unsafe {
            CreateNamedPipeW(
                wide.as_ptr(),
                open_mode,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                MAX_PIPE_INSTANCES,
                65536,
                65536,
                0,
                attributes,
            )
        })
    })
}
struct Op {
    pipe: Arc<OwnedHandle>,
    event: OwnedHandle,
    overlap: Box<OVERLAPPED>,
    data: Vec<u8>,
}
impl Op {
    fn new(pipe: Arc<OwnedHandle>, data: Vec<u8>) -> io::Result<Self> {
        if QUARANTINED.load(Ordering::SeqCst) {
            return Err(io::Error::other("pipe I/O quarantined"));
        }
        let event = owned(unsafe { CreateEventW(null(), 1, 0, null()) })?;
        let mut overlap = Box::<OVERLAPPED>::default();
        overlap.hEvent = raw(&event);
        Ok(Self {
            pipe,
            event,
            overlap,
            data,
        })
    }
    fn finish(mut self, success: bool, pending: bool, deadline: Instant) -> io::Result<Vec<u8>> {
        let mut count = 0;
        if !success {
            if !pending {
                return Err(io::Error::last_os_error());
            }
            let timeout = wait_ms(deadline).unwrap_or(0);
            if unsafe { WaitForSingleObject(raw(&self.event), timeout) } != WAIT_OBJECT_0 {
                unsafe {
                    CancelIoEx(raw(&self.pipe), &*self.overlap);
                }
                if unsafe { WaitForSingleObject(raw(&self.event), CANCEL_GRACE_MS) }
                    != WAIT_OBJECT_0
                {
                    QUARANTINED.store(true, Ordering::SeqCst);
                    std::mem::forget(self);
                    return Err(io::Error::new(io::ErrorKind::TimedOut, "pipe I/O deadline"));
                }
                if unsafe { GetOverlappedResult(raw(&self.pipe), &*self.overlap, &mut count, 0) }
                    == 0
                    && unsafe { GetLastError() } == ERROR_IO_INCOMPLETE
                {
                    QUARANTINED.store(true, Ordering::SeqCst);
                    std::mem::forget(self);
                }
                return Err(io::Error::new(io::ErrorKind::TimedOut, "pipe I/O deadline"));
            }
        }
        if unsafe { GetOverlappedResult(raw(&self.pipe), &*self.overlap, &mut count, 0) } == 0 {
            if unsafe { GetLastError() } == ERROR_IO_INCOMPLETE {
                QUARANTINED.store(true, Ordering::SeqCst);
                std::mem::forget(self);
            }
            return Err(io::Error::other("pipe I/O failed"));
        }
        if Instant::now() >= deadline {
            return Err(io::Error::new(io::ErrorKind::TimedOut, "pipe I/O deadline"));
        }
        self.data.truncate(count as usize);
        Ok(self.data)
    }
    fn cancel_and_drain(self) -> io::Result<bool> {
        unsafe {
            CancelIoEx(raw(&self.pipe), &*self.overlap);
        }
        if unsafe { WaitForSingleObject(raw(&self.event), CANCEL_GRACE_MS) } != WAIT_OBJECT_0 {
            QUARANTINED.store(true, Ordering::SeqCst);
            std::mem::forget(self);
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "pipe cancellation did not drain",
            ));
        }
        let mut count = 0;
        if unsafe { GetOverlappedResult(raw(&self.pipe), &*self.overlap, &mut count, 0) } != 0 {
            return Ok(true);
        }
        if unsafe { GetLastError() } == ERROR_IO_INCOMPLETE {
            QUARANTINED.store(true, Ordering::SeqCst);
            std::mem::forget(self);
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "pipe operation remained pending after cancellation",
            ));
        }
        Ok(false)
    }
    fn connect(mut self, deadline: Instant, shutdown: &OwnedHandle) -> io::Result<bool> {
        wait_ms(deadline)?;
        let result = unsafe { ConnectNamedPipe(raw(&self.pipe), &mut *self.overlap) };
        if result != 0 {
            self.finish(true, false, deadline)?;
            return Ok(true);
        }
        let code = unsafe { GetLastError() };
        if code == ERROR_PIPE_CONNECTED {
            return Ok(true);
        }
        if code != ERROR_IO_PENDING {
            return Err(io::Error::last_os_error());
        }
        let handles = [raw(shutdown), raw(&self.event)];
        // ConnectNamedPipe is pending now. Never use `?` on deadline calculation
        // here: returning would drop the OVERLAPPED storage before cancellation.
        let timeout = wait_ms(deadline).unwrap_or(0);
        let wait =
            unsafe { WaitForMultipleObjects(handles.len() as u32, handles.as_ptr(), 0, timeout) };
        if wait == WAIT_OBJECT_0 {
            let _ = self.cancel_and_drain()?;
            return Ok(false);
        }
        if wait == WAIT_OBJECT_0 + 1 {
            self.finish(true, false, deadline)?;
            return Ok(true);
        }
        if wait == WAIT_TIMEOUT {
            return if self.cancel_and_drain()? {
                Ok(true)
            } else {
                Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "pipe accept deadline",
                ))
            };
        }
        let error = if wait == WAIT_FAILED {
            io::Error::last_os_error()
        } else {
            io::Error::other("unexpected named-pipe wait result")
        };
        let _ = self.cancel_and_drain();
        Err(error)
    }
    fn transfer(
        mut self,
        write: bool,
        deadline: Instant,
        shutdown: Option<&OwnedHandle>,
    ) -> io::Result<Vec<u8>> {
        wait_ms(deadline)?;
        let result = unsafe {
            if write {
                WriteFile(
                    raw(&self.pipe),
                    self.data.as_ptr(),
                    self.data.len() as u32,
                    null_mut(),
                    &mut *self.overlap,
                )
            } else {
                ReadFile(
                    raw(&self.pipe),
                    self.data.as_mut_ptr(),
                    self.data.len() as u32,
                    null_mut(),
                    &mut *self.overlap,
                )
            }
        };
        let pending = result == 0 && unsafe { GetLastError() } == ERROR_IO_PENDING;
        if pending && let Some(shutdown) = shutdown {
            let handles = [raw(shutdown), raw(&self.event)];
            // A pending read must be cancelled and drained before its buffers can drop.
            let timeout = wait_ms(deadline).unwrap_or(0);
            let wait = unsafe {
                WaitForMultipleObjects(handles.len() as u32, handles.as_ptr(), 0, timeout)
            };
            if wait == WAIT_OBJECT_0 + 1 {
                return self.finish(true, false, deadline);
            }
            let error = match wait {
                WAIT_OBJECT_0 => io::Error::new(io::ErrorKind::Interrupted, "daemon shutdown"),
                WAIT_TIMEOUT => io::Error::new(io::ErrorKind::TimedOut, "pipe read deadline"),
                WAIT_FAILED => io::Error::last_os_error(),
                _ => io::Error::other("unexpected named-pipe read wait result"),
            };
            let _ = self.cancel_and_drain()?;
            return Err(error);
        }
        self.finish(result != 0, pending, deadline)
    }
}

fn discover_models(
    chat: &Mutex<windows_chat::State>,
    cancel: &AtomicBool,
) -> Result<ResponsePayload, &'static str> {
    let (config, epoch) = chat
        .lock()
        .map_err(|_| "runtime-unavailable")?
        .start_discovery()?;
    let models = provider::discover_models(&config, cancel).map_err(|_| "provider-unavailable")?;
    chat.lock()
        .map_err(|_| "runtime-unavailable")?
        .install_catalog(models, epoch)
}

struct Peer {
    pipe: Arc<OwnedHandle>,
    writer: Mutex<()>,
    alive: AtomicBool,
    deadline: Instant,
}
impl Peer {
    fn write(&self, envelope: &Envelope) -> io::Result<()> {
        let _write = self
            .writer
            .lock()
            .map_err(|_| invalid("pipe writer unavailable"))?;
        self.write_locked(envelope, Duration::from_millis(350))
    }

    fn write_locked(&self, envelope: &Envelope, timeout: Duration) -> io::Result<()> {
        if !self.alive.load(Ordering::Relaxed) {
            return Err(io::Error::new(io::ErrorKind::BrokenPipe, "pipe closed"));
        }
        let mut bytes = serde_json::to_vec(envelope)?;
        if bytes.len() + 1 > contract::MAX_FRAME_BYTES {
            return Err(invalid("reply too large"));
        }
        bytes.push(b'\n');
        let write_deadline = self.deadline.min(Instant::now() + timeout);
        let mut offset = 0;
        while offset < bytes.len() {
            if let Ok(written) = Op::new(self.pipe.clone(), bytes[offset..].to_vec())
                .and_then(|operation| operation.transfer(true, write_deadline, None))
                && !written.is_empty()
            {
                offset += written.len();
            } else {
                // A partial frame may already have reached the peer.
                self.alive.store(false, Ordering::Relaxed);
                unsafe {
                    DisconnectNamedPipe(raw(&self.pipe));
                }
                return Err(io::Error::new(
                    io::ErrorKind::BrokenPipe,
                    "pipe write failed",
                ));
            }
        }
        Ok(())
    }
}

fn serve(
    peer: Arc<Peer>,
    started: Instant,
    shutdown: &OwnedHandle,
    cancel: &AtomicBool,
    chat: &Mutex<windows_chat::State>,
    jobs: &SyncSender<windows_chat::Job>,
) -> io::Result<bool> {
    let mut dispatcher = Dispatcher::with_started(started);
    dispatcher.allow_shutdown();
    let mut capabilities = vec![json!("terminal-state")];
    if chat
        .lock()
        .map_err(|_| invalid("Chat state unavailable"))?
        .has_provider()
    {
        capabilities.extend([
            json!("model-discovery"),
            json!("model-selection"),
            json!("chat"),
            json!("events"),
        ]);
    }
    capabilities.extend([
        json!("sessions"),
        json!("session-messages"),
        json!("session-management"),
    ]);
    dispatcher.set_capabilities(capabilities);
    let deadline = peer.deadline;
    let hello_deadline = deadline.min(Instant::now() + IO_WAIT);
    let mut frame_deadline = Some(hello_deadline);
    let mut handshaken = false;
    let mut incoming = Vec::new();
    loop {
        while let Some(end) = incoming.iter().position(|b| *b == b'\n') {
            if unsafe { WaitForSingleObject(raw(shutdown), 0) } == WAIT_OBJECT_0 {
                return Ok(false);
            }
            wait_ms(frame_deadline.unwrap_or(deadline))?;
            if end + 1 > contract::MAX_FRAME_BYTES {
                return Err(invalid("frame too large"));
            }
            let frame: Vec<u8> = incoming.drain(..=end).collect();
            let mut job = None;
            let (write_result, shutdown_requested) = {
                // Serialize the ACK with events. An attaching client sees either the
                // last event in its snapshot or the event after the ACK.
                let _write = peer
                    .writer
                    .lock()
                    .map_err(|_| invalid("pipe writer unavailable"))?;
                let (active, sessions) = chat
                    .lock()
                    .map_err(|_| invalid("Chat state unavailable"))?
                    .counts();
                dispatcher.set_counts(active, sessions);
                let reply = dispatcher.dispatch_with(&frame, |request| match request {
                    RequestPayload::ModelList {} => discover_models(chat, cancel),
                    _ => chat
                        .lock()
                        .map_err(|_| "runtime-unavailable")?
                        .dispatch(request, &peer, &mut job),
                });
                handshaken |= reply.kind == MessageType::Response && reply.name == "hello";
                (
                    peer.write_locked(&reply, IO_WAIT),
                    dispatcher.shutdown_requested(),
                )
            };
            if let Some(job) = job {
                if write_result.is_ok() && !cancel.load(Ordering::Relaxed) {
                    if let Err(TrySendError::Full(job) | TrySendError::Disconnected(job)) =
                        jobs.try_send(job)
                    {
                        windows_chat::reject_unacknowledged(chat, &job);
                    }
                } else {
                    windows_chat::reject_unacknowledged(chat, &job);
                }
            }
            // Once shutdown was accepted, wake the accept loop after the bounded ACK
            // attempt even when delivery is ambiguous or the peer closes early.
            if shutdown_requested {
                unsafe {
                    SetEvent(raw(shutdown));
                }
            }
            write_result?;
            if shutdown_requested {
                return Ok(true);
            }
        }
        if incoming.len() >= contract::MAX_FRAME_BYTES {
            return Err(invalid("frame too large"));
        }
        if incoming.is_empty() {
            frame_deadline = if handshaken {
                None
            } else {
                Some(hello_deadline)
            };
        }
        let chunk = Op::new(peer.pipe.clone(), vec![0; 4096])?.transfer(
            false,
            frame_deadline.unwrap_or(deadline),
            Some(shutdown),
        )?;
        if chunk.is_empty() {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "pipe closed"));
        }
        frame_deadline.get_or_insert_with(|| deadline.min(Instant::now() + IO_WAIT));
        incoming.extend_from_slice(&chunk);
    }
}
pub fn run() -> io::Result<()> {
    let mut args = env::args_os();
    let _ = args.next();
    if args.next().as_deref() != Some(OsStr::new("--profile")) {
        return Err(invalid(
            "usage: sentinel-daemon-dev --profile PRIVATE_TEMP_DIRECTORY [--provider ORIGIN --model ID [--provider-kind ID]]",
        ));
    }
    let profile = PathBuf::from(args.next().ok_or_else(|| invalid("missing profile"))?);
    let mut config = if let Some(flag) = args.next() {
        if flag != "--provider" {
            return Err(invalid("expected --provider"));
        }
        let origin = args
            .next()
            .and_then(|value| value.into_string().ok())
            .ok_or_else(|| invalid("missing provider origin"))?;
        if args.next().as_deref() != Some(OsStr::new("--model")) {
            return Err(invalid("expected --model"));
        }
        let model = args
            .next()
            .and_then(|value| value.into_string().ok())
            .ok_or_else(|| invalid("missing model"))?;
        let mut config = Config::parse(&origin, &model)
            .map_err(|_| invalid("invalid provider configuration"))?;
        if let Some(flag) = args.next() {
            if flag != "--provider-kind" {
                return Err(invalid("expected --provider-kind"));
            }
            let kind = args
                .next()
                .and_then(|value| value.into_string().ok())
                .ok_or_else(|| invalid("missing provider kind"))?;
            config = config
                .with_provider_kind(&kind)
                .map_err(|_| invalid("invalid provider kind"))?;
        }
        Some(config)
    } else {
        None
    };
    if args.next().is_some() {
        return Err(invalid("unexpected argument"));
    }
    let guard = Arc::new(windows_profile::acquire(&profile)?);
    if let Some(saved) = guard.load_model()?
        && let Some(config) = config.as_mut()
        && saved.endpoint == config.endpoint
        && saved.provider_id == config.provider_id
    {
        config.model = saved.model;
    }
    let current = windows_profile::current_user_sid()?;
    let started = Instant::now();
    let shutdown = Arc::new(owned(unsafe { CreateEventW(null(), 1, 0, null()) })?);
    let cancel_discovery = Arc::new(AtomicBool::new(false));
    let chat = Arc::new(Mutex::new(windows_chat::State::load(
        config,
        Arc::clone(&guard),
    )?));
    let (jobs, pending_jobs) = sync_channel(1);
    let worker_chat = Arc::clone(&chat);
    let chat_worker = thread::Builder::new()
        .name("sentinel-windows-chat".into())
        .spawn(move || windows_chat::worker(worker_chat, pending_jobs))?;
    let mut workers: Vec<JoinHandle<io::Result<bool>>> = Vec::new();
    let result = (|| -> io::Result<()> {
        let mut first_instance = true;
        // Keep a connectable instance alive even while connected peers are served.
        let mut anchor = Arc::new(create_pipe(guard.endpoint(), &current, first_instance)?);
        first_instance = false;
        println!("Development pipe: {}", guard.endpoint().to_string_lossy());
        loop {
            reap_finished(&mut workers);
            if unsafe { WaitForSingleObject(raw(&shutdown), 0) } == WAIT_OBJECT_0 {
                break;
            }
            match Op::new(anchor.clone(), vec![])?.connect(Instant::now() + ACCEPT_WAIT, &shutdown)
            {
                Ok(false) => break,
                Err(error)
                    if error.kind() == io::ErrorKind::TimedOut
                        && !QUARANTINED.load(Ordering::SeqCst) =>
                {
                    // The cancelled ConnectNamedPipe operation drained; reuse the same
                    // instance so the endpoint never loses its first-instance claim.
                    continue;
                }
                Err(error) => return Err(error),
                Ok(true) => {
                    let connected = anchor.clone();
                    // Replace the accepting instance before auth checks or scheduling.
                    let replacement = match create_pipe(guard.endpoint(), &current, first_instance)
                    {
                        Ok(pipe) => Arc::new(pipe),
                        Err(error) => {
                            unsafe {
                                DisconnectNamedPipe(raw(&connected));
                            }
                            return Err(error);
                        }
                    };
                    anchor = replacement;
                    if unsafe { WaitForSingleObject(raw(&shutdown), 0) } == WAIT_OBJECT_0 {
                        unsafe {
                            DisconnectNamedPipe(raw(&connected));
                        }
                        break;
                    }
                    if verify_client(&connected, &current).is_err() {
                        unsafe {
                            DisconnectNamedPipe(raw(&connected));
                        }
                        continue;
                    }
                    reap_finished(&mut workers);
                    if workers.len() >= MAX_CLIENTS {
                        // The temporary connected instance is rejected while the
                        // replacement anchor continues accepting peers.
                        unsafe {
                            DisconnectNamedPipe(raw(&connected));
                        }
                        continue;
                    }
                    let peer = Arc::new(Peer {
                        pipe: connected.clone(),
                        writer: Mutex::new(()),
                        alive: AtomicBool::new(true),
                        deadline: Instant::now() + CLIENT_LIFETIME,
                    });
                    let peer_shutdown = shutdown.clone();
                    let peer_cancel = cancel_discovery.clone();
                    let peer_chat = chat.clone();
                    let peer_jobs = jobs.clone();
                    match thread::Builder::new()
                        .name("sentinel-dev-pipe-peer".into())
                        .spawn(move || {
                            let result = serve(
                                peer.clone(),
                                started,
                                &peer_shutdown,
                                &peer_cancel,
                                &peer_chat,
                                &peer_jobs,
                            );
                            peer.alive.store(false, Ordering::Relaxed);
                            if matches!(&result, Ok(true)) || QUARANTINED.load(Ordering::SeqCst) {
                                unsafe {
                                    SetEvent(raw(&peer_shutdown));
                                }
                            }
                            unsafe {
                                DisconnectNamedPipe(raw(&peer.pipe));
                            }
                            result
                        }) {
                        Ok(worker) => workers.push(worker),
                        Err(error) => {
                            unsafe {
                                DisconnectNamedPipe(raw(&connected));
                            }
                            return Err(error);
                        }
                    }
                }
            }
        }
        Ok(())
    })();
    // Wake the accept anchor on every exit path, then join every peer before releasing
    // the profile guard and its named-pipe identity.
    cancel_discovery.store(true, Ordering::Relaxed);
    unsafe {
        SetEvent(raw(&shutdown));
    }
    if let Ok(state) = chat.lock() {
        state.cancel_active();
    }
    drop(jobs);
    let mut worker_panicked = false;
    for worker in workers {
        worker_panicked |= worker.join().is_err();
    }
    worker_panicked |= chat_worker.join().is_err();
    if worker_panicked && result.is_ok() {
        Err(io::Error::other("named-pipe peer worker panicked"))
    } else {
        result
    }
}

fn reap_finished(workers: &mut Vec<JoinHandle<io::Result<bool>>>) {
    let mut index = 0;
    while index < workers.len() {
        if workers[index].is_finished() {
            let worker = workers.swap_remove(index);
            let _ = worker.join();
        } else {
            index += 1;
        }
    }
}
