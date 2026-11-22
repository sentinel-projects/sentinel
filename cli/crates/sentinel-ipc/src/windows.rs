//! Windows local named-pipe client. Native Windows security and Qt compatibility
//! require platform acceptance; failures are closed, with no mutation replay.
use super::{
    Envelope, Error, MessageType, contract, correlate_response, decode, encode_request,
    validate_request,
};
use serde_json::{Value, json};
use std::{
    cell::Cell,
    collections::VecDeque,
    ffi::{OsStr, OsString},
    os::windows::{
        ffi::{OsStrExt, OsStringExt},
        io::{AsRawHandle, FromRawHandle, OwnedHandle},
    },
    path::{Path, PathBuf},
    ptr::{null, null_mut},
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicUsize, Ordering},
    },
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::{
        ERROR_IO_INCOMPLETE, ERROR_IO_PENDING, ERROR_PIPE_BUSY, GENERIC_READ, GENERIC_WRITE,
        GetLastError, INVALID_HANDLE_VALUE, WAIT_OBJECT_0,
    },
    Security::{EqualSid, GetTokenInformation, TOKEN_QUERY, TOKEN_USER, TokenUser},
    Storage::FileSystem::{
        CreateFileW, FILE_FLAG_OVERLAPPED, GetLongPathNameW, GetTempPathW, OPEN_EXISTING, ReadFile,
        WriteFile,
    },
    System::{
        IO::{CancelIoEx, GetOverlappedResult, OVERLAPPED},
        LibraryLoader::{GetModuleHandleW, GetProcAddress},
        Pipes::{GetNamedPipeServerProcessId, WaitNamedPipeW},
        Threading::{
            CreateEventW, GetCurrentProcess, OpenProcess, OpenProcessToken,
            PROCESS_QUERY_LIMITED_INFORMATION, WaitForSingleObject,
        },
    },
};

const COMMAND_TIMEOUT: Duration = Duration::from_secs(10);
// `None` must not make Windows reads unbounded even when the Unix caller uses it.
const MAX_EVENT_WAIT: Duration = Duration::from_secs(180);
const CANCEL_GRACE_MS: u32 = 250;
const MAX_OPERATIONS: usize = 32;
static IN_FLIGHT: AtomicUsize = AtomicUsize::new(0);
static QUARANTINED: AtomicBool = AtomicBool::new(false);

fn last_error() -> String {
    std::io::Error::last_os_error().to_string()
}
fn owned(raw: windows_sys::Win32::Foundation::HANDLE) -> Result<OwnedHandle, Error> {
    if raw.is_null() || raw == INVALID_HANDLE_VALUE {
        return Err(Error::Unavailable(last_error()));
    }
    // SAFETY: caller transfers a fresh valid Win32 handle exactly once.
    Ok(unsafe { OwnedHandle::from_raw_handle(raw) })
}
fn raw(h: &OwnedHandle) -> windows_sys::Win32::Foundation::HANDLE {
    h.as_raw_handle() as _
}
fn timeout_ms(deadline: Instant) -> Result<u32, Error> {
    let remaining = deadline
        .checked_duration_since(Instant::now())
        .filter(|d| !d.is_zero())
        .ok_or(Error::Timeout)?;
    Ok(remaining
        .as_millis()
        .saturating_add(1)
        .min(u32::MAX as u128 - 1) as u32)
}
fn qt_temp_path() -> OsString {
    // Qt resolves GetTempPath2W dynamically, falls back to GetTempPathW,
    // expands a short path if possible, strips slashes and uppercases drive.
    let mut buf = [0u16; 260];
    let kernel: Vec<u16> = OsStr::new("kernel32.dll")
        .encode_wide()
        .chain(Some(0))
        .collect();
    let name = b"GetTempPath2W\0";
    let module = unsafe { GetModuleHandleW(kernel.as_ptr()) };
    let function = if module.is_null() {
        None
    } else {
        unsafe { GetProcAddress(module, name.as_ptr()) }
    };
    let length = if let Some(function) = function {
        // SAFETY: GetTempPath2W has the documented GetTempPathW signature.
        let f: unsafe extern "system" fn(u32, *mut u16) -> u32 =
            unsafe { std::mem::transmute(function) };
        unsafe { f(buf.len() as u32, buf.as_mut_ptr()) }
    } else {
        unsafe { GetTempPathW(buf.len() as u32, buf.as_mut_ptr()) }
    };
    if length == 0 || length as usize >= buf.len() {
        return OsString::from("C:/tmp");
    }
    let mut input = buf[..length as usize].to_vec();
    input.push(0);
    let mut expanded = [0u16; 260];
    let len =
        unsafe { GetLongPathNameW(input.as_ptr(), expanded.as_mut_ptr(), expanded.len() as u32) };
    let mut result = if len > 0 && (len as usize) < expanded.len() {
        expanded[..len as usize].to_vec()
    } else {
        input[..input.len() - 1].to_vec()
    };
    while result.last() == Some(&(b'\\' as u16)) {
        result.pop();
    }
    for c in &mut result {
        if *c == b'\\' as u16 {
            *c = b'/' as u16;
        }
    }
    if result.len() >= 2
        && result[1] == b':' as u16
        && (b'a' as u16..=b'z' as u16).contains(&result[0])
    {
        result[0] -= 32;
    }
    OsString::from_wide(&result)
}
pub fn default_socket_path() -> PathBuf {
    let mut path = qt_temp_path();
    path.push("/sentinel-daemon-");
    path.push(std::env::var_os("USERNAME").unwrap_or_default());
    PathBuf::from(path)
}
fn pipe_name(path: &Path) -> Result<Vec<u16>, Error> {
    let mut name: Vec<u16> = if path.is_absolute() {
        path.as_os_str().encode_wide().collect()
    } else {
        let mut prefix: Vec<u16> = qt_temp_path().encode_wide().collect();
        prefix.push(b'/' as u16);
        prefix.extend(path.as_os_str().encode_wide());
        prefix
    };
    if name.is_empty() || name.contains(&0) {
        return Err(Error::Usage("invalid Windows pipe endpoint".into()));
    }
    let prefix: Vec<u16> = OsStr::new(r"\\.\pipe\").encode_wide().collect();
    if !name.starts_with(&prefix) {
        let mut full = prefix;
        full.extend(name);
        name = full;
    }
    name.push(0);
    Ok(name)
}
fn token_user(process: windows_sys::Win32::Foundation::HANDLE) -> Result<Vec<usize>, Error> {
    let mut token = null_mut();
    if unsafe { OpenProcessToken(process, TOKEN_QUERY, &mut token) } == 0 {
        return Err(Error::Unavailable("cannot verify server identity".into()));
    }
    let token = owned(token)?;
    let mut size = 0u32;
    unsafe {
        GetTokenInformation(raw(&token), TokenUser, null_mut(), 0, &mut size);
    }
    if size == 0 || size > 65536 {
        return Err(Error::Unavailable("cannot verify server identity".into()));
    }
    let mut buffer = vec![0usize; (size as usize).div_ceil(std::mem::size_of::<usize>())];
    if unsafe {
        GetTokenInformation(
            raw(&token),
            TokenUser,
            buffer.as_mut_ptr().cast(),
            size,
            &mut size,
        )
    } == 0
    {
        return Err(Error::Unavailable("cannot verify server identity".into()));
    }
    Ok(buffer)
}
fn verify_server(pipe: &OwnedHandle) -> Result<(), Error> {
    let mut pid = 0;
    if unsafe { GetNamedPipeServerProcessId(raw(pipe), &mut pid) } == 0 || pid == 0 {
        return Err(Error::Unavailable("cannot verify server identity".into()));
    }
    let process = owned(unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) })?;
    let current = token_user(unsafe { GetCurrentProcess() })?;
    let remote = token_user(raw(&process))?;
    // SAFETY: buffers are aligned, populated TOKEN_USER values held alive here.
    let a = unsafe { (*(current.as_ptr().cast::<TOKEN_USER>())).User.Sid };
    let b = unsafe { (*(remote.as_ptr().cast::<TOKEN_USER>())).User.Sid };
    if unsafe { EqualSid(a, b) } == 0 {
        return Err(Error::Unavailable(
            "named-pipe server is not the current user".into(),
        ));
    }
    let mut confirmed = 0;
    if unsafe { GetNamedPipeServerProcessId(raw(pipe), &mut confirmed) } == 0 || confirmed != pid {
        return Err(Error::Unavailable(
            "named-pipe server identity changed".into(),
        ));
    }
    Ok(())
}
struct Permit;
impl Permit {
    fn acquire() -> Result<Self, Error> {
        if QUARANTINED.load(Ordering::SeqCst) {
            return Err(Error::Unavailable("Windows pipe I/O quarantined".into()));
        }
        IN_FLIGHT
            .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |n| {
                (n < MAX_OPERATIONS).then_some(n + 1)
            })
            .map_err(|_| Error::Unavailable("Windows pipe operation limit".into()))?;
        Ok(Self)
    }
}
impl Drop for Permit {
    fn drop(&mut self) {
        IN_FLIGHT.fetch_sub(1, Ordering::SeqCst);
    }
}
struct Operation {
    pipe: Arc<OwnedHandle>,
    event: OwnedHandle,
    overlap: Box<OVERLAPPED>,
    data: Vec<u8>,
    _permit: Permit,
}
impl Operation {
    fn new(pipe: Arc<OwnedHandle>, data: Vec<u8>) -> Result<Self, Error> {
        let permit = Permit::acquire()?;
        let event = owned(unsafe { CreateEventW(null(), 1, 0, null()) })?;
        let mut overlap = Box::<OVERLAPPED>::default();
        overlap.hEvent = raw(&event);
        Ok(Self {
            pipe,
            event,
            overlap,
            data,
            _permit: permit,
        })
    }
    fn perform(mut self, write: bool, deadline: Instant) -> Result<Vec<u8>, Error> {
        timeout_ms(deadline)?;
        let mut count = 0u32;
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
        if result == 0 {
            if unsafe { GetLastError() } != ERROR_IO_PENDING {
                return Err(Error::Disconnected);
            }
            let wait_ms = timeout_ms(deadline).unwrap_or(0);
            let wait = unsafe { WaitForSingleObject(raw(&self.event), wait_ms) };
            if wait != WAIT_OBJECT_0 {
                unsafe {
                    CancelIoEx(raw(&self.pipe), &*self.overlap);
                }
                // Windows may complete cancellation asynchronously. Never release
                // kernel-owned buffers while pending; quarantine instead of waiting forever.
                if unsafe { WaitForSingleObject(raw(&self.event), CANCEL_GRACE_MS) }
                    != WAIT_OBJECT_0
                {
                    QUARANTINED.store(true, Ordering::SeqCst);
                    std::mem::forget(self);
                    return Err(Error::Timeout);
                }
                // A read can win the cancellation race. Preserve those bytes so
                // polling timeouts cannot silently consume a late terminal frame.
                let completed =
                    unsafe { GetOverlappedResult(raw(&self.pipe), &*self.overlap, &mut count, 0) };
                if completed == 0 && unsafe { GetLastError() } == ERROR_IO_INCOMPLETE {
                    QUARANTINED.store(true, Ordering::SeqCst);
                    std::mem::forget(self);
                    return Err(Error::Timeout);
                }
                if !write && completed != 0 && count > 0 {
                    self.data.truncate(count as usize);
                    return Ok(self.data);
                }
                return Err(Error::Timeout);
            }
        }
        if unsafe { GetOverlappedResult(raw(&self.pipe), &*self.overlap, &mut count, 0) } == 0 {
            if unsafe { GetLastError() } == ERROR_IO_INCOMPLETE {
                QUARANTINED.store(true, Ordering::SeqCst);
                std::mem::forget(self);
            }
            return Err(Error::Disconnected);
        }
        if write && Instant::now() >= deadline {
            return Err(Error::Timeout);
        }
        if count == 0 {
            return Err(Error::Disconnected);
        }
        self.data.truncate(count as usize);
        Ok(self.data)
    }
}
pub struct Client {
    pipe: Option<Arc<OwnedHandle>>,
    incoming: Vec<u8>,
    events: VecDeque<Envelope>,
    sequence: u64,
    read_timeout: Cell<Option<Duration>>,
    capabilities: Vec<String>,
}
impl Client {
    pub fn connect(path: &Path, identity: &str) -> Result<Self, Error> {
        if QUARANTINED.load(Ordering::SeqCst) {
            return Err(Error::Unavailable("Windows pipe I/O quarantined".into()));
        }
        let name = pipe_name(path)?;
        let deadline = Instant::now() + COMMAND_TIMEOUT;
        let handle = loop {
            timeout_ms(deadline)?;
            let h = unsafe {
                CreateFileW(
                    name.as_ptr(),
                    GENERIC_READ | GENERIC_WRITE,
                    0,
                    null(),
                    OPEN_EXISTING,
                    FILE_FLAG_OVERLAPPED,
                    null_mut(),
                )
            };
            if h != INVALID_HANDLE_VALUE {
                break owned(h)?;
            }
            if unsafe { GetLastError() } != ERROR_PIPE_BUSY {
                return Err(Error::Unavailable(last_error()));
            }
            if unsafe { WaitNamedPipeW(name.as_ptr(), timeout_ms(deadline)?) } == 0 {
                return Err(Error::Unavailable(last_error()));
            }
        };
        verify_server(&handle)?;
        timeout_ms(deadline)?;
        let mut client = Self {
            pipe: Some(Arc::new(handle)),
            incoming: Vec::new(),
            events: VecDeque::new(),
            sequence: 0,
            read_timeout: Cell::new(Some(COMMAND_TIMEOUT)),
            capabilities: Vec::new(),
        };
        let hello = client.request_until("hello", json!({"client_id":identity,"major":contract::MAJOR,"minor":contract::MINOR,"capabilities":contract::CAPABILITIES}), deadline)?;
        client.capabilities = hello["capabilities"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(Value::as_str)
            .map(str::to_owned)
            .collect();
        Ok(client)
    }
    /// Capabilities advertised by the daemon during the initial hello exchange.
    pub fn capabilities(&self) -> &[String] {
        &self.capabilities
    }
    pub fn set_read_timeout(&self, timeout: Option<Duration>) -> Result<(), Error> {
        if timeout.is_some_and(|v| v.is_zero()) {
            return Err(Error::Protocol("zero timeout".into()));
        }
        self.read_timeout.set(timeout);
        Ok(())
    }
    fn pipe(&self) -> Result<Arc<OwnedHandle>, Error> {
        self.pipe.clone().ok_or(Error::Disconnected)
    }
    pub fn request(&mut self, name: &str, payload: Value) -> Result<Value, Error> {
        let deadline = Instant::now()
            .checked_add(self.read_timeout.get().unwrap_or(COMMAND_TIMEOUT))
            .ok_or_else(|| Error::Protocol("timeout out of range".into()))?;
        let result = self.request_until(name, payload, deadline);
        if matches!(
            result,
            Err(Error::Timeout)
                | Err(Error::Unavailable(_))
                | Err(Error::Disconnected)
                | Err(Error::Protocol(_))
                | Err(Error::Incompatible)
        ) {
            self.pipe.take();
        }
        result
    }
    fn request_until(
        &mut self,
        name: &str,
        payload: Value,
        deadline: Instant,
    ) -> Result<Value, Error> {
        validate_request(name, &payload)?;
        self.sequence += 1;
        let id = self.sequence.to_string();
        let frame = encode_request(name, payload, &id)?;
        let mut cursor = 0;
        while cursor < frame.len() {
            let written = Operation::new(self.pipe()?, frame[cursor..].to_vec())?
                .perform(true, deadline)?
                .len();
            cursor += written;
        }
        loop {
            let msg = self.read_until(deadline)?;
            if msg.kind == MessageType::Event {
                if self.events.len() >= 256 {
                    return Err(Error::Protocol("event backlog exceeded".into()));
                }
                self.events.push_back(msg);
                continue;
            }
            return correlate_response(msg, &id, name);
        }
    }
    fn read_until(&mut self, deadline: Instant) -> Result<Envelope, Error> {
        loop {
            timeout_ms(deadline)?;
            if let Some(end) = self.incoming.iter().position(|b| *b == b'\n') {
                if end + 1 > contract::MAX_FRAME_BYTES {
                    return Err(Error::Protocol("oversized frame".into()));
                }
                let frame: Vec<u8> = self.incoming.drain(..=end).collect();
                return decode(&frame);
            }
            if self.incoming.len() >= contract::MAX_FRAME_BYTES {
                return Err(Error::Protocol("oversized frame".into()));
            }
            let bytes = Operation::new(self.pipe()?, vec![0; 4096])?.perform(false, deadline)?;
            self.incoming.extend_from_slice(&bytes);
        }
    }
    pub fn next_event(&mut self) -> Result<Envelope, Error> {
        if let Some(event) = self.events.pop_front() {
            return Ok(event);
        }
        let deadline = Instant::now()
            .checked_add(self.read_timeout.get().unwrap_or(MAX_EVENT_WAIT))
            .ok_or_else(|| Error::Protocol("timeout out of range".into()))?;
        let result = self.read_until(deadline);
        match result {
            Ok(event) if event.kind == MessageType::Event => Ok(event),
            Ok(event) if event.kind == MessageType::Error => Err(Error::Remote(
                event.payload["code"]
                    .as_str()
                    .unwrap_or("protocol error")
                    .into(),
            )),
            Ok(_) => Err(Error::Protocol("expected event".into())),
            Err(error) => Err(error),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use windows_sys::Win32::Security::{
        CreateWellKnownSid, SECURITY_MAX_SID_SIZE, WinBuiltinUsersSid, WinWorldSid,
    };
    #[test]
    fn full_qt_endpoint_keeps_entire_resolved_path() {
        let name = pipe_name(Path::new(
            "C:/Example/Temp/sentinel-daemon-Example",
        ))
        .unwrap();
        let expected: Vec<u16> =
            OsStr::new(r"\\.\pipe\C:/Example/Temp/sentinel-daemon-Example")
                .encode_wide()
                .chain(Some(0))
                .collect();
        assert_eq!(name, expected);
        assert!(pipe_name(Path::new("C:/bad\0name")).is_err());
    }
    #[test]
    fn distinct_well_known_sids_do_not_compare_equal() {
        let mut a = [0usize; 16];
        let mut b = [0usize; 16];
        let mut a_len = SECURITY_MAX_SID_SIZE;
        let mut b_len = SECURITY_MAX_SID_SIZE;
        assert_ne!(
            unsafe {
                CreateWellKnownSid(WinWorldSid, null_mut(), a.as_mut_ptr().cast(), &mut a_len)
            },
            0
        );
        assert_ne!(
            unsafe {
                CreateWellKnownSid(
                    WinBuiltinUsersSid,
                    null_mut(),
                    b.as_mut_ptr().cast(),
                    &mut b_len,
                )
            },
            0
        );
        assert_eq!(
            unsafe { EqualSid(a.as_mut_ptr().cast(), b.as_mut_ptr().cast()) },
            0
        );
        assert_ne!(
            unsafe { EqualSid(a.as_mut_ptr().cast(), a.as_mut_ptr().cast()) },
            0
        );
    }
}
