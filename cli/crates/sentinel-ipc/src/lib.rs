//! Bounded local-user IPC client. Disconnect never implies cancellation.
#[rustfmt::skip]
pub mod contract;
pub mod backup;
pub mod server;
pub mod voice;
pub const APP_VERSION: &str = env!("SENTINEL_APP_VERSION");
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
#[cfg(unix)]
use std::os::unix::net::UnixStream;
#[cfg(not(windows))]
use std::path::Path;
use std::path::PathBuf;
#[cfg(unix)]
use std::{
    collections::VecDeque,
    io::{BufRead, BufReader, Write},
};

#[derive(Debug, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum MessageType {
    Request,
    Response,
    Event,
    Error,
}
#[derive(Debug, Serialize, Deserialize)]
pub struct Version {
    pub major: usize,
    pub minor: usize,
}
#[derive(Debug, Serialize, Deserialize)]
pub struct Envelope {
    pub version: Version,
    #[serde(rename = "type")]
    pub kind: MessageType,
    pub id: String,
    pub name: String,
    pub payload: Value,
}
#[derive(Debug)]
pub enum Error {
    Usage(String),
    Failed(String),
    Cancelled,
    Unavailable(String),
    Disconnected,
    Timeout,
    Protocol(String),
    Incompatible,
    Remote(String),
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Failed(detail) => write!(f, "Task failed: {detail}"),
            Self::Usage(detail) => write!(f, "{detail}"),
            Self::Cancelled => write!(f, "Run cancelled."),
            Self::Unavailable(detail) => write!(
                f,
                "Cannot connect to the Sentinel daemon: {detail}. Start sentinel-daemon externally; use --socket for a custom endpoint."
            ),
            Self::Timeout => write!(f, "Daemon response timed out."),
            Self::Disconnected => write!(
                f,
                "Daemon connection lost. Use sentinel attach <session-id> to restore the authoritative session; disconnect does not cancel a run."
            ),
            Self::Protocol(detail) => write!(f, "Invalid daemon protocol data: {detail}"),
            Self::Incompatible => write!(
                f,
                "Incompatible IPC version. Install matching Sentinel CLI and daemon versions."
            ),
            Self::Remote(code) => write!(
                f,
                "{} [{code}]",
                match code.as_str() {
                    "provider-unavailable" | "ProviderUnavailable" =>
                        "Selected provider is unavailable. Run sentinel doctor and check the provider configuration",
                    "model-unavailable" | "ModelNotFound" =>
                        "Selected model is unavailable. Use sentinel models and sentinel model select <provider> <model>",
                    "permission-denied" | "PermissionDenied" => "Permission denied by the daemon",
                    "runtime-busy" =>
                        "An active run prevents this change. Wait for completion or cancel the run",
                    "runtime-rejected" =>
                        "Runtime rejected this request. Run sentinel doctor to inspect model, workspace and tool readiness",
                    "runtime-unavailable" => "The daemon runtime is unavailable",
                    "daemon-shutting-down" =>
                        "Daemon is shutting down; new requests are not accepted",
                    "invalid-approval" =>
                        "Approval is stale or has already been answered. Reattach the session",
                    "unknown-session" =>
                        "Session does not exist. Run sentinel sessions to find its identifier",
                    "session-archived" =>
                        "This session is archived. Unarchive it before starting a new request",
                    _ => "The daemon rejected the operation",
                }
            ),
        }
    }
}
impl std::error::Error for Error {}
impl Error {
    /// Command failures that make this connection unusable. Polling Timeout is
    /// handled separately by callers and does not invalidate a healthy socket.
    pub fn requires_reconnect(&self) -> bool {
        matches!(
            self,
            Self::Disconnected
                | Self::Unavailable(_)
                | Self::Timeout
                | Self::Protocol(_)
                | Self::Incompatible
        ) || matches!(self, Self::Remote(code) if code == "daemon-shutting-down")
    }
    pub fn from_run_detail(detail: &str) -> Self {
        if [
            "ProviderUnavailable",
            "ModelNotFound",
            "AuthenticationRequired",
            "PermissionDenied",
            "provider-unavailable",
            "model-unavailable",
            "permission-denied",
            "runtime-rejected",
        ]
        .contains(&detail)
        {
            Self::Remote(detail.into())
        } else {
            Self::Failed(if detail.is_empty() {
                "Runtime did not provide a final answer".into()
            } else {
                detail.into()
            })
        }
    }
    pub fn code(&self) -> &str {
        match self {
            Self::Failed(_) => "task-failed",
            Self::Usage(_) => "cli-usage",
            Self::Cancelled => "cancelled",
            Self::Unavailable(_) => "daemon-unavailable",
            Self::Disconnected => "disconnected",
            Self::Timeout => "timeout",
            Self::Protocol(_) => "protocol-invalid",
            Self::Incompatible => "protocol-mismatch",
            Self::Remote(code) => code,
        }
    }
    pub fn exit_code(&self) -> i32 {
        match self {
            Self::Unavailable(_) | Self::Disconnected | Self::Timeout => 3,
            Self::Usage(_) => 2,
            Self::Cancelled => 130,
            Self::Incompatible | Self::Protocol(_) => 4,
            Self::Remote(code) if code == "daemon-shutting-down" => 3,
            Self::Remote(code)
                if [
                    "provider-unavailable",
                    "model-unavailable",
                    "ProviderUnavailable",
                    "ModelNotFound",
                    "AuthenticationRequired",
                ]
                .contains(&code.as_str()) =>
            {
                5
            }
            Self::Remote(code)
                if ["permission-denied", "PermissionDenied"].contains(&code.as_str()) =>
            {
                6
            }
            Self::Remote(code)
                if ["runtime-rejected", "runtime-busy", "runtime-unavailable"]
                    .contains(&code.as_str()) =>
            {
                7
            }
            _ => 1,
        }
    }
}
#[cfg(not(windows))]
pub fn default_socket_path() -> PathBuf {
    PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".sentinel/run/daemon.sock")
}
#[cfg(windows)]
pub fn default_socket_path() -> PathBuf {
    windows::default_socket_path()
}
/// Resolve the standalone Rust profile. Servers must validate ownership and lock it.
pub fn persistent_rust_profile_path() -> std::io::Result<PathBuf> {
    #[cfg(any(target_os = "linux", target_os = "macos"))]
    {
        let home = PathBuf::from(
            std::env::var_os("HOME").ok_or_else(|| std::io::Error::other("HOME is required"))?,
        );
        if !home.is_absolute() {
            return Err(std::io::Error::other("HOME must be absolute"));
        }
        Ok(home.canonicalize()?.join(".sentinel-rust"))
    }
    #[cfg(not(any(target_os = "linux", target_os = "macos")))]
    {
        Err(std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "the persistent Rust profile is available on Linux and macOS",
        ))
    }
}
pub fn decode(bytes: &[u8]) -> Result<Envelope, Error> {
    if bytes.len() > contract::MAX_FRAME_BYTES {
        return Err(Error::Protocol("oversized frame".into()));
    }
    let message: Envelope =
        serde_json::from_slice(bytes).map_err(|e| Error::Protocol(e.to_string()))?;
    if message.version.major != contract::MAJOR {
        return Err(Error::Incompatible);
    }
    if !message.payload.is_object() {
        return Err(Error::Protocol("payload must be an object".into()));
    }
    if message.kind == MessageType::Event {
        if contract::EVENTS.contains(&message.name.as_str()) {
            let _: contract::EventPayload =
                serde_json::from_value(json!({"name":message.name,"payload":message.payload}))
                    .map_err(|e| Error::Protocol(e.to_string()))?;
        } else if message.version.minor.saturating_sub(contract::MINOR) == 0 {
            return Err(Error::Protocol("unknown event".into()));
        }
    } else if message.kind == MessageType::Response {
        let _: contract::ResponsePayload =
            serde_json::from_value(json!({"name":message.name,"payload":message.payload}))
                .map_err(|e| Error::Protocol(e.to_string()))?;
    }
    Ok(message)
}
#[cfg(any(unix, windows, test))]
fn validate_request(name: &str, payload: &Value) -> Result<(), Error> {
    let _: contract::RequestPayload =
        serde_json::from_value(json!({"name":name,"payload":payload}))
            .map_err(|e| Error::Protocol(e.to_string()))?;
    Ok(())
}
#[cfg(any(unix, windows, test))]
fn encode_request(name: &str, payload: Value, id: &str) -> Result<Vec<u8>, Error> {
    let message = Envelope {
        version: Version {
            major: contract::MAJOR,
            minor: contract::MINOR,
        },
        kind: MessageType::Request,
        id: id.into(),
        name: name.into(),
        payload,
    };
    let mut frame = serde_json::to_vec(&message).map_err(|e| Error::Protocol(e.to_string()))?;
    if frame.len() + 1 > contract::MAX_FRAME_BYTES {
        return Err(Error::Protocol("oversized request".into()));
    }
    frame.push(b'\n');
    Ok(frame)
}
#[cfg(any(unix, windows, test))]
fn correlate_response(message: Envelope, id: &str, name: &str) -> Result<Value, Error> {
    if message.id != id {
        return Err(Error::Protocol("response correlation mismatch".into()));
    }
    if message.kind == MessageType::Error {
        let code = message.payload["code"].as_str().unwrap_or("unknown");
        return Err(if code == "incompatible-major" {
            Error::Incompatible
        } else {
            Error::Remote(code.into())
        });
    }
    if message.kind != MessageType::Response || message.name != name {
        return Err(Error::Protocol("invalid response".into()));
    }
    Ok(message.payload)
}

#[cfg(unix)]
fn remaining_budget(deadline: std::time::Instant) -> Result<std::time::Duration, Error> {
    deadline
        .checked_duration_since(std::time::Instant::now())
        .filter(|value| !value.is_zero())
        .ok_or(Error::Timeout)
}
#[cfg(unix)]
fn transport_error(error: std::io::Error) -> Error {
    if matches!(
        error.kind(),
        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
    ) {
        Error::Timeout
    } else {
        Error::Disconnected
    }
}
#[cfg(unix)]
pub struct Client {
    reader: BufReader<UnixStream>,
    writer: UnixStream,
    incoming: Vec<u8>,
    sequence: u64,
    events: VecDeque<Envelope>,
    capabilities: Vec<String>,
}
#[cfg(unix)]
impl Client {
    pub fn connect(path: &Path, identity: &str) -> Result<Self, Error> {
        let stream = UnixStream::connect(path).map_err(|e| Error::Unavailable(e.to_string()))?;
        Self::connect_stream(stream, identity)
    }
    /// Complete the canonical handshake on a caller-verified connected Unix stream.
    pub fn connect_stream(stream: UnixStream, identity: &str) -> Result<Self, Error> {
        stream
            .set_read_timeout(Some(std::time::Duration::from_secs(10)))
            .map_err(|e| Error::Unavailable(e.to_string()))?;
        let writer = stream
            .try_clone()
            .map_err(|e| Error::Unavailable(e.to_string()))?;
        let mut client = Self {
            reader: BufReader::new(stream),
            writer,
            incoming: Vec::new(),
            sequence: 0,
            events: VecDeque::new(),
            capabilities: Vec::new(),
        };
        let hello = client.request(
            "hello",
            json!({"client_id":identity,"major":contract::MAJOR,"minor":contract::MINOR,"capabilities":contract::CAPABILITIES}),
        )?;
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
    pub fn set_read_timeout(&self, timeout: Option<std::time::Duration>) -> Result<(), Error> {
        self.reader
            .get_ref()
            .set_read_timeout(timeout)
            .map_err(|e| Error::Protocol(e.to_string()))
    }
    pub fn request(&mut self, name: &str, payload: Value) -> Result<Value, Error> {
        let polling_timeout = self
            .reader
            .get_ref()
            .read_timeout()
            .map_err(|_| Error::Disconnected)?;
        let write_timeout = self
            .writer
            .write_timeout()
            .map_err(|_| Error::Disconnected)?;
        // Blocking event polling must not make command acknowledgements unbounded.
        let deadline = std::time::Instant::now()
            + polling_timeout.unwrap_or(std::time::Duration::from_secs(10));
        let result = self.request_until(name, payload, deadline);
        let restored_read = self.reader.get_ref().set_read_timeout(polling_timeout);
        let restored_write = self.writer.set_write_timeout(write_timeout);
        if matches!(result, Err(Error::Timeout))
            || restored_read.is_err()
            || restored_write.is_err()
        {
            // Late replies cannot poison the next command. Disconnect does not cancel a run.
            let _ = self.writer.shutdown(std::net::Shutdown::Both);
        }
        // macOS can reject setsockopt after the peer closes. Preserve an already
        // validated acknowledgement; shutdown above makes further I/O fail closed.
        result
    }
    fn request_until(
        &mut self,
        name: &str,
        payload: Value,
        deadline: std::time::Instant,
    ) -> Result<Value, Error> {
        // Validate before consuming a request ID, as on the existing Unix path.
        validate_request(name, &payload)?;
        self.sequence += 1;
        let id = self.sequence.to_string();
        let frame = encode_request(name, payload, &id)?;
        let mut remaining = frame.as_slice();
        while !remaining.is_empty() {
            self.writer
                .set_write_timeout(Some(remaining_budget(deadline)?))
                .map_err(|_| Error::Disconnected)?;
            let count = match self.writer.write(remaining) {
                Err(error) if error.kind() == std::io::ErrorKind::Interrupted => continue,
                result => result.map_err(transport_error)?,
            };
            if count == 0 {
                return Err(Error::Disconnected);
            }
            remaining = &remaining[count..];
        }
        loop {
            let message = self.read_until(Some(deadline))?;
            if message.kind == MessageType::Event {
                if self.events.len() >= 256 {
                    return Err(Error::Protocol("event backlog exceeded".into()));
                }
                self.events.push_back(message);
                continue;
            }
            return correlate_response(message, &id, name);
        }
    }
    fn read(&mut self) -> Result<Envelope, Error> {
        self.read_until(None)
    }
    fn read_until(&mut self, deadline: Option<std::time::Instant>) -> Result<Envelope, Error> {
        loop {
            let mut drain_only = false;
            if let Some(deadline) = deadline {
                let budget = remaining_budget(deadline)?;
                if self.reader.buffer().is_empty()
                    && self
                        .reader
                        .get_ref()
                        .set_read_timeout(Some(budget))
                        .is_err()
                {
                    // macOS may reject setsockopt after peer closure even when
                    // complete frames remain queued. Drain without blocking;
                    // never extend the deadline or wait after a setter failure.
                    self.reader
                        .get_ref()
                        .set_nonblocking(true)
                        .map_err(|_| Error::Disconnected)?;
                    drain_only = true;
                }
            }
            let available = self.reader.fill_buf();
            if drain_only {
                self.writer
                    .set_nonblocking(false)
                    .map_err(|_| Error::Disconnected)?;
            }
            let available = available.map_err(|error| {
                if drain_only {
                    Error::Disconnected
                } else {
                    transport_error(error)
                }
            })?;
            if available.is_empty() {
                return Err(Error::Disconnected);
            }
            let end = available.iter().position(|b| *b == b'\n');
            let count = end.map_or(available.len(), |n| n + 1);
            if self.incoming.len() + count > contract::MAX_FRAME_BYTES {
                return Err(Error::Protocol("oversized frame".into()));
            }
            self.incoming.extend_from_slice(&available[..count]);
            self.reader.consume(count);
            if end.is_some() {
                return decode(&std::mem::take(&mut self.incoming));
            }
        }
    }
    pub fn next_event(&mut self) -> Result<Envelope, Error> {
        if let Some(event) = self.events.pop_front() {
            return Ok(event);
        }
        let event = self.read()?;
        if event.kind == MessageType::Error {
            return Err(Error::Remote(
                event.payload["code"]
                    .as_str()
                    .unwrap_or("protocol error")
                    .into(),
            ));
        }
        if event.kind != MessageType::Event {
            return Err(Error::Protocol("expected event".into()));
        }
        Ok(event)
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn shared_request_encoding_and_correlation() {
        validate_request("daemon.status", &json!({})).unwrap();
        assert!(
            validate_request(
                "approval.respond",
                &json!({"run_id":"r","approval_id":"a","allow":"yes"})
            )
            .is_err()
        );
        let frame = encode_request("daemon.status", json!({}), "42").unwrap();
        assert_eq!(frame.last(), Some(&b'\n'));
        let request: Envelope = serde_json::from_slice(&frame).unwrap();
        assert_eq!(request.id, "42");
        assert_eq!(request.name, "daemon.status");
        let response = || Envelope {
            version: Version {
                major: contract::MAJOR,
                minor: contract::MINOR,
            },
            kind: MessageType::Response,
            id: "42".into(),
            name: "daemon.status".into(),
            payload: json!({"running":true}),
        };
        assert_eq!(
            correlate_response(response(), "42", "daemon.status").unwrap()["running"],
            true
        );
        assert!(matches!(
            correlate_response(response(), "old", "daemon.status"),
            Err(Error::Protocol(_))
        ));
        assert!(matches!(
            correlate_response(response(), "42", "hello"),
            Err(Error::Protocol(_))
        ));
        let error = Envelope {
            kind: MessageType::Error,
            payload: json!({"code":"incompatible-major"}),
            ..response()
        };
        assert!(matches!(
            correlate_response(error, "42", "daemon.status"),
            Err(Error::Incompatible)
        ));
    }

    #[test]
    fn shared_request_enforces_wire_size_including_newline() {
        let payload = json!({"session_id":"s", "text":"\0".repeat(50000)});
        assert!(validate_request("chat.send", &payload).is_ok());
        assert!(matches!(
            encode_request("chat.send", payload, "1"),
            Err(Error::Protocol(_))
        ));
    }

    #[test]
    fn command_reconnection_policy_preserves_business_errors() {
        for error in [
            Error::Timeout,
            Error::Disconnected,
            Error::Unavailable("test".into()),
            Error::Protocol("bad frame".into()),
            Error::Incompatible,
            Error::Remote("daemon-shutting-down".into()),
        ] {
            assert!(error.requires_reconnect());
        }
        for code in [
            "permission-denied",
            "provider-unavailable",
            "runtime-busy",
            "unknown-session",
        ] {
            assert!(!Error::Remote(code.into()).requires_reconnect());
        }
        assert!(!Error::Cancelled.requires_reconnect());
    }
    #[test]
    fn rejects_malformed() {
        assert!(decode(b"invalid").is_err());
    }
    #[test]
    fn rejects_oversized() {
        assert!(decode(&vec![0; contract::MAX_FRAME_BYTES + 1]).is_err());
    }
    #[test]
    fn event_decoding() {
        let e=decode(br#"{"version":{"major":1,"minor":0},"type":"event","id":"","name":"run.cancelled","payload":{"run_id":"r","session_id":"s","text":"","state":"cancelled"}}"#).unwrap();
        assert_eq!(e.kind, MessageType::Event);
    }
    #[test]
    fn incompatible() {
        assert!(matches!(decode(br#"{"version":{"major":2,"minor":0},"type":"error","id":"1","name":"ProtocolError","payload":{}}"#),Err(Error::Incompatible)));
    }
    #[test]
    fn exit_codes() {
        assert_eq!(Error::Disconnected.exit_code(), 3);
        assert_eq!(Error::Incompatible.exit_code(), 4);
    }
    #[cfg(unix)]
    #[test]
    fn partial_frame_survives_poll_timeout() {
        let (stream, mut peer) = UnixStream::pair().unwrap();
        stream
            .set_read_timeout(Some(std::time::Duration::from_millis(20)))
            .unwrap();
        let mut client = Client {
            writer: stream.try_clone().unwrap(),
            reader: BufReader::new(stream),
            incoming: Vec::new(),
            sequence: 0,
            events: VecDeque::new(),
            capabilities: Vec::new(),
        };
        let frame = br#"{"version":{"major":1,"minor":1},"type":"event","id":"","name":"run.cancelled","payload":{"run_id":"r","session_id":"s","text":"","state":"cancelled"}}"#;
        peer.write_all(&frame[..30]).unwrap();
        assert!(matches!(client.read(), Err(Error::Timeout)));
        let partial = client.incoming.clone();
        assert!(!partial.is_empty());
        assert!(matches!(
            client.read_until(Some(std::time::Instant::now())),
            Err(Error::Timeout)
        ));
        assert_eq!(client.incoming, partial);
        peer.write_all(&frame[30..]).unwrap();
        peer.write_all(b"\n").unwrap();
        assert_eq!(client.read().unwrap().name, "run.cancelled");
    }
    #[cfg(unix)]
    #[test]
    fn expired_deadline_rejects_buffered_frame_without_consuming_it() {
        let (stream, mut peer) = UnixStream::pair().unwrap();
        let mut client = Client {
            writer: stream.try_clone().unwrap(),
            reader: BufReader::new(stream),
            incoming: Vec::new(),
            sequence: 0,
            events: VecDeque::new(),
            capabilities: Vec::new(),
        };
        writeln!(peer, "{}", json!({"version":{"major":1,"minor":1},"type":"event","id":"","name":"run.cancelled","payload":{"run_id":"r","session_id":"s","text":"","state":"cancelled"}})).unwrap();
        assert!(matches!(
            client.read_until(Some(std::time::Instant::now())),
            Err(Error::Timeout)
        ));
        assert!(client.incoming.is_empty());
        assert_eq!(client.read().unwrap().name, "run.cancelled");
    }
    #[cfg(unix)]
    #[test]
    fn request_timeout_closes_transport() {
        let (stream, _peer) = UnixStream::pair().unwrap();
        let timeout = Some(std::time::Duration::from_millis(20));
        stream.set_read_timeout(timeout).unwrap();
        let mut client = Client {
            writer: stream.try_clone().unwrap(),
            reader: BufReader::new(stream),
            incoming: Vec::new(),
            sequence: 0,
            events: VecDeque::new(),
            capabilities: Vec::new(),
        };
        assert!(matches!(
            client.request("daemon.status", json!({})),
            Err(Error::Timeout)
        ));
        assert!(matches!(
            client.request("daemon.status", json!({})),
            Err(Error::Disconnected)
        ));
    }
    #[cfg(unix)]
    #[test]
    fn successful_request_restores_polling_timeout() {
        let (stream, mut peer) = UnixStream::pair().unwrap();
        let timeout = Some(std::time::Duration::from_millis(20));
        stream.set_read_timeout(timeout).unwrap();
        let mut client = Client {
            writer: stream.try_clone().unwrap(),
            reader: BufReader::new(stream),
            incoming: Vec::new(),
            sequence: 0,
            events: VecDeque::new(),
            capabilities: Vec::new(),
        };
        writeln!(peer, "{}", json!({"version":{"major":1,"minor":1},"type":"response","id":"1","name":"daemon.status","payload":{"running":true,"daemon_version":APP_VERSION,"uptime_ms":0,"active_runs":0,"sessions":0}})).unwrap();
        assert_eq!(
            client.request("daemon.status", json!({})).unwrap()["running"],
            true
        );
        assert_eq!(client.reader.get_ref().read_timeout().unwrap(), timeout);
        assert_eq!(client.writer.write_timeout().unwrap(), None);
    }
    #[test]
    fn stable_error_categories() {
        for (error, code) in [
            (Error::Usage("bad".into()), 2),
            (Error::Remote("model-unavailable".into()), 5),
            (Error::Remote("permission-denied".into()), 6),
            (Error::Remote("runtime-rejected".into()), 7),
            (Error::Remote("daemon-shutting-down".into()), 3),
            (Error::Cancelled, 130),
            (Error::Failed("arbitrary detail".into()), 1),
        ] {
            assert_eq!(error.exit_code(), code);
        }
        assert_eq!(
            Error::from_run_detail("arbitrary planner error").code(),
            "task-failed"
        );
    }
    #[cfg(unix)]
    #[test]
    fn missing_daemon() {
        assert!(matches!(
            Client::connect(Path::new("/nonexistent/sentinel.sock"), "test"),
            Err(Error::Unavailable(_))
        ));
    }
}

#[cfg(windows)]
mod windows;
#[cfg(windows)]
pub use windows::Client;
#[cfg(not(any(unix, windows)))]
pub struct Client;
#[cfg(not(any(unix, windows)))]
impl Client {
    pub fn capabilities(&self) -> &[String] {
        &[]
    }
    pub fn set_read_timeout(&self, _: Option<std::time::Duration>) -> Result<(), Error> {
        Ok(())
    }
    pub fn connect(_: &Path, _: &str) -> Result<Self, Error> {
        Err(Error::Unavailable(
            "Windows named-pipe transport is not implemented yet".into(),
        ))
    }
    pub fn request(&mut self, _: &str, _: Value) -> Result<Value, Error> {
        Err(Error::Disconnected)
    }
    pub fn next_event(&mut self) -> Result<Envelope, Error> {
        Err(Error::Disconnected)
    }
}

#[cfg(all(test, unix))]
mod transport_tests {
    use super::*;
    use std::os::unix::net::UnixListener;
    #[test]
    fn handshake_correlation_and_event_queue() {
        let directory =
            std::env::temp_dir().join(format!("sentinel-rust-ipc-{}", std::process::id()));
        std::fs::create_dir_all(&directory).unwrap();
        let path = directory.join("mock.sock");
        let listener = UnixListener::bind(&path).unwrap();
        let handle = std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            let mut reader = BufReader::new(stream.try_clone().unwrap());
            for _ in 0..2 {
                let mut line = String::new();
                reader.read_line(&mut line).unwrap();
                let request: Envelope = serde_json::from_str(&line).unwrap();
                if request.name == "daemon.status" {
                    writeln!(stream,"{}",json!({"version":{"major":1,"minor":0},"type":"event","id":"","name":"output.delta","payload":{"text":"test","run_id":"r","session_id":"s"}})).unwrap();
                }
                writeln!(stream,"{}",json!({"version":{"major":1,"minor":0},"type":"response","id":request.id,"name":request.name,"payload":if request.name == "hello" { json!({"major":1,"minor":0,"daemon_version":APP_VERSION,"capabilities":["agent","events"]}) } else { json!({"running":true,"daemon_version":APP_VERSION,"uptime_ms":0,"active_runs":0,"sessions":0}) }})).unwrap();
            }
        });
        let mut client = Client::connect(&path, "rust-test").unwrap();
        assert_eq!(
            client.capabilities(),
            &[String::from("agent"), String::from("events")]
        );
        assert_eq!(
            client.request("daemon.status", json!({})).unwrap()["running"],
            true
        );
        assert_eq!(client.next_event().unwrap().payload["text"], "test");
        assert!(matches!(client.next_event(), Err(Error::Disconnected)));
        handle.join().unwrap();
        std::fs::remove_dir_all(directory).unwrap();
    }
    #[test]
    fn typed_request_invalid_enum_or_fields() {
        assert!(
            serde_json::from_value::<contract::RequestPayload>(
                json!({"name":"invented","payload":{}})
            )
            .is_err()
        );
        assert!(serde_json::from_value::<contract::RequestPayload>(json!({"name":"approval.respond","payload":{"run_id":"x","approval_id":"y","allow":"yes"}})).is_err());
    }
}

#[cfg(test)]
mod generation_compatibility_tests {
    use super::contract::ResponsePayload;
    use serde_json::json;

    #[test]
    fn hello_generation_is_additive() {
        for generation in [None, Some("daemon-generation")] {
            let mut value = json!({
                "name": "hello",
                "payload": {"major": 1, "minor": 0, "daemon_version": "fixture", "capabilities": []}
            });
            if let Some(generation) = generation {
                value["payload"]["server_generation"] = json!(generation);
            }
            match serde_json::from_value::<ResponsePayload>(value).unwrap() {
                ResponsePayload::Hello {
                    server_generation, ..
                } => {
                    assert_eq!(server_generation.as_deref(), generation);
                }
                _ => panic!("Expected typed hello response"),
            }
        }
    }
}

#[cfg(all(test, unix))]
mod close_regression_tests {
    use super::*;
    use std::io::Read;
    use std::net::Shutdown;
    use std::sync::mpsc;
    use std::time::{Duration, Instant};

    fn client(stream: UnixStream, capacity: usize) -> Client {
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        Client {
            writer: stream.try_clone().unwrap(),
            reader: BufReader::with_capacity(capacity, stream),
            incoming: Vec::new(),
            sequence: 0,
            events: VecDeque::new(),
            capabilities: Vec::new(),
        }
    }
    fn response(id: &str, name: &str) -> Value {
        let payload = if name == "hello" {
            json!({"major":1,"minor":0,"daemon_version":APP_VERSION,"capabilities":[]})
        } else {
            json!({"running":true,"daemon_version":APP_VERSION,"uptime_ms":0,"active_runs":0,"sessions":0})
        };
        json!({"version":{"major":1,"minor":0},"type":"response","id":id,"name":name,"payload":payload})
    }
    fn event() -> Value {
        json!({"version":{"major":1,"minor":0},"type":"event","id":"","name":"output.delta","payload":{"text":"test","run_id":"r","session_id":"s"}})
    }
    fn read_request(peer: &mut BufReader<UnixStream>) -> Envelope {
        let mut line = String::new();
        peer.read_line(&mut line).unwrap();
        serde_json::from_str(&line).unwrap()
    }
    #[test]
    fn buffered_response_after_confirmed_peer_close() {
        let (stream, mut peer) = UnixStream::pair().unwrap();
        let mut client = client(stream, 8192);
        let frame = format!("{}\n", response("1", "hello"));
        peer.write_all(frame.as_bytes()).unwrap();
        // Prefetch the complete frame, then synchronously close both peer handles.
        assert_eq!(client.reader.fill_buf().unwrap(), frame.as_bytes());
        peer.shutdown(Shutdown::Both).unwrap();
        drop(peer);
        let setting = client
            .reader
            .get_ref()
            .set_read_timeout(Some(Duration::from_secs(1)));
        eprintln!(
            "closed peer timeout setter: {setting:?}; buffered bytes={}",
            client.reader.buffer().len()
        );
        let result = client.read_until(Some(Instant::now() + Duration::from_secs(1)));
        assert_eq!(result.unwrap().name, "hello");
        assert!(matches!(client.read(), Err(Error::Disconnected)));
    }
    #[test]
    fn unbuffered_complete_response_after_confirmed_peer_close() {
        let (stream, mut peer) = UnixStream::pair().unwrap();
        let mut client = client(stream, 16);
        writeln!(peer, "{}", response("1", "hello")).unwrap();
        peer.shutdown(Shutdown::Both).unwrap();
        drop(peer);
        assert!(client.reader.buffer().is_empty());
        assert_eq!(
            client
                .read_until(Some(Instant::now() + Duration::from_secs(1)))
                .unwrap()
                .name,
            "hello"
        );
        assert!(matches!(client.read(), Err(Error::Disconnected)));
    }
    #[test]
    fn incomplete_response_at_eof_is_not_success() {
        let (stream, mut peer) = UnixStream::pair().unwrap();
        let mut client = client(stream, 16);
        let frame = response("1", "hello").to_string();
        peer.write_all(&frame.as_bytes()[..30]).unwrap();
        peer.shutdown(Shutdown::Both).unwrap();
        drop(peer);
        assert!(matches!(
            client.read_until(Some(Instant::now() + Duration::from_secs(1))),
            Err(Error::Disconnected)
        ));
        assert_eq!(client.incoming.len(), 30);
    }
    #[test]
    fn handshake_event_order_and_immediate_close() {
        for before in [None, Some(true), Some(false)] {
            let (stream, peer) = UnixStream::pair().unwrap();
            let server = std::thread::spawn(move || {
                peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
                let mut reader = BufReader::new(peer.try_clone().unwrap());
                let request = read_request(&mut reader);
                assert_eq!(request.name, "hello");
                let reply = response(&request.id, "hello");
                let frames = match before {
                    Some(true) => format!("{}\n{}\n", event(), reply),
                    Some(false) => format!("{}\n{}\n", reply, event()),
                    None => format!("{reply}\n"),
                };
                let mut peer = peer;
                peer.write_all(frames.as_bytes()).unwrap();
                peer.shutdown(Shutdown::Both).unwrap();
            });
            let mut client = client(stream, 16);
            assert_eq!(
                client
                    .request(
                        "hello",
                        json!({"client_id":"fixture","major":1,"minor":0,"capabilities":[]})
                    )
                    .unwrap()["major"],
                1
            );
            if before.is_some() {
                assert_eq!(client.next_event().unwrap().payload["text"], "test");
            }
            assert!(matches!(client.next_event(), Err(Error::Disconnected)));
            server.join().unwrap();
        }
    }
    #[test]
    fn fragmented_frame_writes_and_large_request() {
        let (stream, mut peer) = UnixStream::pair().unwrap();
        let text = "x".repeat(20000);
        let expected = text.clone();
        let server = std::thread::spawn(move || {
            peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
            let mut reader = BufReader::with_capacity(16, peer.try_clone().unwrap());
            let request = read_request(&mut reader);
            assert_eq!(request.payload["text"], expected);
            let reply = json!({"version":{"major":1,"minor":0},"type":"response","id":request.id,"name":"chat.send","payload":{"run_id":"r","session_id":"s"}});
            for chunk in format!("{reply}\n").as_bytes().chunks(3) {
                peer.write_all(chunk).unwrap();
            }
            peer.shutdown(Shutdown::Both).unwrap();
        });
        let mut client = client(stream, 16);
        assert_eq!(
            client
                .request("chat.send", json!({"session_id":"s","text":text}))
                .unwrap()["run_id"],
            "r"
        );
        server.join().unwrap();
    }
    #[test]
    fn expired_write_deadline_sends_no_frame() {
        let (stream, mut peer) = UnixStream::pair().unwrap();
        let mut client = client(stream, 16);
        assert!(matches!(
            client.request_until("daemon.status", json!({}), Instant::now()),
            Err(Error::Timeout)
        ));
        peer.set_nonblocking(true).unwrap();
        assert_eq!(
            peer.read(&mut [0; 1]).unwrap_err().kind(),
            std::io::ErrorKind::WouldBlock
        );
    }
    #[test]
    fn sequential_requests_restore_both_timeouts() {
        let (stream, mut peer) = UnixStream::pair().unwrap();
        let (done, wait) = mpsc::channel();
        let server = std::thread::spawn(move || {
            peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
            let mut reader = BufReader::new(peer.try_clone().unwrap());
            for id in ["1", "2"] {
                let request = read_request(&mut reader);
                assert_eq!(request.id, id);
                writeln!(peer, "{}", response(id, "daemon.status")).unwrap();
            }
            wait.recv_timeout(Duration::from_secs(2)).unwrap();
        });
        let mut client = client(stream, 16);
        let read = Some(Duration::from_secs(1));
        let write = Some(Duration::from_millis(750));
        client.set_read_timeout(read).unwrap();
        client.writer.set_write_timeout(write).unwrap();
        for _ in 0..2 {
            assert_eq!(
                client.request("daemon.status", json!({})).unwrap()["running"],
                true
            );
            assert_eq!(client.reader.get_ref().read_timeout().unwrap(), read);
            assert_eq!(client.writer.write_timeout().unwrap(), write);
        }
        done.send(()).unwrap();
        server.join().unwrap();
    }
    #[test]
    fn stale_id_and_wrong_name_are_rejected() {
        for (id, name) in [("old", "daemon.status"), ("1", "hello")] {
            let (stream, mut peer) = UnixStream::pair().unwrap();
            let server = std::thread::spawn(move || {
                peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
                let mut reader = BufReader::new(peer.try_clone().unwrap());
                read_request(&mut reader);
                writeln!(peer, "{}", response(id, name)).unwrap();
                peer.shutdown(Shutdown::Both).unwrap();
            });
            let mut client = client(stream, 16);
            assert!(matches!(
                client.request("daemon.status", json!({})),
                Err(Error::Protocol(_))
            ));
            server.join().unwrap();
        }
    }
    #[test]
    fn blocked_writer_deadline_closes_transport() {
        let (stream, _peer) = UnixStream::pair().unwrap();
        let mut client = client(stream, 16);
        // Fill the actual send queue to WouldBlock; no scheduling sleep is needed.
        client.writer.set_nonblocking(true).unwrap();
        loop {
            match client.writer.write(&[0; 8192]) {
                Ok(0) => panic!("unexpected zero write"),
                Ok(_) => {}
                Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => break,
                Err(error) => panic!("unexpected queue-fill error: {error}"),
            }
        }
        client.writer.set_nonblocking(false).unwrap();
        client
            .set_read_timeout(Some(Duration::from_millis(20)))
            .unwrap();
        assert!(matches!(
            client.request("daemon.status", json!({})),
            Err(Error::Timeout)
        ));
        assert!(matches!(
            client.request("daemon.status", json!({})),
            Err(Error::Disconnected)
        ));
    }
    #[test]
    fn peer_closes_without_response_is_disconnected() {
        let (stream, peer) = UnixStream::pair().unwrap();
        let server = std::thread::spawn(move || {
            peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
            let mut reader = BufReader::new(peer.try_clone().unwrap());
            read_request(&mut reader);
            peer.shutdown(Shutdown::Both).unwrap();
        });
        let mut client = client(stream, 16);
        assert!(matches!(
            client.request("daemon.status", json!({})),
            Err(Error::Disconnected)
        ));
        server.join().unwrap();
    }
    #[test]
    fn client_drop_closes_all_socket_handles() {
        let (stream, mut peer) = UnixStream::pair().unwrap();
        let client = client(stream, 16);
        peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
        drop(client);
        assert_eq!(peer.read(&mut [0; 1]).unwrap(), 0);
    }
    #[test]
    fn event_backlog_saturation_triggers_protocol_error() {
        let (stream, peer) = UnixStream::pair().unwrap();
        let server = std::thread::spawn(move || {
            peer.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
            let mut reader = BufReader::new(peer.try_clone().unwrap());
            let request = read_request(&mut reader);
            assert_eq!(request.name, "hello");
            let event_frame = format!("{}\n", event());
            let mut peer = peer;
            for _ in 0..257 {
                peer.write_all(event_frame.as_bytes()).unwrap();
            }
            writeln!(peer, "{}", response(&request.id, "hello")).unwrap();
        });
        let mut client = client(stream, 16);
        let err = client
            .request(
                "hello",
                json!({"client_id":"fixture","major":1,"minor":0,"capabilities":[]}),
            )
            .unwrap_err();
        assert!(
            matches!(err, Error::Protocol(ref s) if s.contains("event backlog exceeded")),
            "unexpected error: {err:?}"
        );
        server.join().unwrap();
    }
}
