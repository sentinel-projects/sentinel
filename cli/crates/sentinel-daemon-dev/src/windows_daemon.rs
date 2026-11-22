//! Isolated, non-production Windows development pipe with no storage or Chat.
use crate::windows_profile;
use sentinel_ipc::{contract, server::Dispatcher};
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
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::{
        ERROR_IO_INCOMPLETE, ERROR_IO_PENDING, ERROR_PIPE_CONNECTED, GetLastError,
        INVALID_HANDLE_VALUE, WAIT_FAILED, WAIT_OBJECT_0, WAIT_TIMEOUT,
    },
    Security::{
        ACL, ACL_REVISION, AddAccessAllowedAce, EqualSid, GetLengthSid, GetTokenInformation,
        InitializeAcl, InitializeSecurityDescriptor, IsValidSid, SE_DACL_PROTECTED,
        SECURITY_ATTRIBUTES, SECURITY_DESCRIPTOR, SetSecurityDescriptorControl,
        SetSecurityDescriptorDacl, SetSecurityDescriptorOwner, TOKEN_QUERY, TOKEN_USER, TokenUser,
    },
    Storage::FileSystem::{
        FILE_ALL_ACCESS, FILE_FLAG_FIRST_PIPE_INSTANCE, FILE_FLAG_OVERLAPPED, PIPE_ACCESS_DUPLEX,
        ReadFile, WriteFile,
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
const CLIENT_LIFETIME: Duration = Duration::from_secs(15);
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
    // The current SID and ACL buffers remain alive until CreateNamedPipe returns.
    let sid = unsafe { (*(current.as_ptr().cast::<TOKEN_USER>())).User.Sid };
    if unsafe { IsValidSid(sid) } == 0 {
        return Err(invalid("invalid owner SID"));
    }
    let acl_len = (std::mem::size_of::<ACL>() + 64 + unsafe { GetLengthSid(sid) } as usize)
        .next_multiple_of(std::mem::size_of::<usize>());
    let mut acl = vec![0usize; acl_len / std::mem::size_of::<usize>()];
    let acl_ptr = acl.as_mut_ptr().cast::<ACL>();
    let mut descriptor = SECURITY_DESCRIPTOR::default();
    let sd = (&mut descriptor as *mut SECURITY_DESCRIPTOR).cast();
    if unsafe { InitializeSecurityDescriptor(sd, 1) } == 0
        || unsafe { InitializeAcl(acl_ptr, acl_len as u32, ACL_REVISION) } == 0
        || unsafe { AddAccessAllowedAce(acl_ptr, ACL_REVISION, FILE_ALL_ACCESS, sid) } == 0
        || unsafe { SetSecurityDescriptorOwner(sd, sid, 0) } == 0
        || unsafe { SetSecurityDescriptorDacl(sd, 1, acl_ptr, 0) } == 0
        || unsafe { SetSecurityDescriptorControl(sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED) } == 0
    {
        return Err(io::Error::last_os_error());
    }
    let attributes = SECURITY_ATTRIBUTES {
        nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
        lpSecurityDescriptor: sd,
        bInheritHandle: 0,
    };
    let mut open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if first_instance {
        open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    }
    owned(unsafe {
        CreateNamedPipeW(
            wide.as_ptr(),
            open_mode,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            MAX_PIPE_INSTANCES,
            65536,
            65536,
            0,
            &attributes,
        )
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
    fn transfer(mut self, write: bool, deadline: Instant) -> io::Result<Vec<u8>> {
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
        self.finish(result != 0, pending, deadline)
    }
}
fn serve(pipe: Arc<OwnedHandle>, started: Instant, shutdown: &OwnedHandle) -> io::Result<bool> {
    let mut dispatcher = Dispatcher::with_started(started);
    dispatcher.allow_shutdown();
    let deadline = Instant::now() + CLIENT_LIFETIME;
    let mut incoming = Vec::new();
    loop {
        while let Some(end) = incoming.iter().position(|b| *b == b'\n') {
            if end + 1 > contract::MAX_FRAME_BYTES {
                return Err(invalid("frame too large"));
            }
            let frame: Vec<u8> = incoming.drain(..=end).collect();
            let reply = dispatcher.dispatch(&frame);
            let shutdown_requested = dispatcher.shutdown_requested();
            let write_result = (|| -> io::Result<()> {
                let mut bytes = serde_json::to_vec(&reply)?;
                if bytes.len() + 1 > contract::MAX_FRAME_BYTES {
                    return Err(invalid("reply too large"));
                }
                bytes.push(b'\n');
                let write_deadline = deadline.min(Instant::now() + IO_WAIT);
                let mut offset = 0;
                while offset < bytes.len() {
                    let written = Op::new(pipe.clone(), bytes[offset..].to_vec())?
                        .transfer(true, write_deadline)?
                        .len();
                    if written == 0 {
                        return Err(io::Error::new(io::ErrorKind::WriteZero, "pipe closed"));
                    }
                    offset += written;
                }
                Ok(())
            })();
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
        let chunk = Op::new(pipe.clone(), vec![0; 4096])?
            .transfer(false, deadline.min(Instant::now() + IO_WAIT))?;
        if chunk.is_empty() {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "pipe closed"));
        }
        incoming.extend_from_slice(&chunk);
    }
}
pub fn run() -> io::Result<()> {
    let mut args = env::args_os();
    let _ = args.next();
    if args.next().as_deref() != Some(OsStr::new("--profile")) {
        return Err(invalid(
            "usage: sentinel-daemon-dev --profile PRIVATE_TEMP_DIRECTORY",
        ));
    }
    let profile = PathBuf::from(args.next().ok_or_else(|| invalid("missing profile"))?);
    if args.next().is_some() {
        return Err(invalid("unexpected arguments; Windows Chat is unavailable"));
    }
    let guard = windows_profile::acquire(&profile)?;
    let current = windows_profile::current_user_sid()?;
    let started = Instant::now();
    let shutdown = Arc::new(owned(unsafe { CreateEventW(null(), 1, 0, null()) })?);
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
                    let peer = connected.clone();
                    let peer_shutdown = shutdown.clone();
                    match thread::Builder::new()
                        .name("sentinel-dev-pipe-peer".into())
                        .spawn(move || {
                            let result = serve(peer.clone(), started, &peer_shutdown);
                            if matches!(&result, Ok(true)) || QUARANTINED.load(Ordering::SeqCst) {
                                unsafe {
                                    SetEvent(raw(&peer_shutdown));
                                }
                            }
                            unsafe {
                                DisconnectNamedPipe(raw(&peer));
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
    unsafe {
        SetEvent(raw(&shutdown));
    }
    let mut worker_panicked = false;
    for worker in workers {
        worker_panicked |= worker.join().is_err();
    }
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
