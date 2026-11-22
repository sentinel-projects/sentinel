//! Experimental request framing and dispatch. This module does not secure or own an endpoint.
use crate::{APP_VERSION, Envelope, MessageType, Version, contract};
use serde_json::{Value, json};
#[cfg(unix)]
use std::io::{self, BufRead, BufReader};
#[cfg(unix)]
use std::os::unix::net::UnixStream;
#[cfg(unix)]
use std::time::Duration;
use std::time::Instant;

/// Reads one bounded frame under one absolute deadline, including fragmented input.
/// A framing error, timeout or EOF makes the connection unusable.
#[cfg(unix)]
pub fn read_frame(reader: &mut BufReader<UnixStream>, deadline: Instant) -> io::Result<Vec<u8>> {
    let mut frame = Vec::new();
    loop {
        let remaining = deadline
            .checked_duration_since(Instant::now())
            .filter(|d| !d.is_zero())
            .ok_or_else(|| io::Error::new(io::ErrorKind::TimedOut, "frame deadline"))?;
        let mut drain_only = false;
        if reader.buffer().is_empty()
            && reader
                .get_ref()
                .set_read_timeout(Some(remaining.max(Duration::from_micros(1))))
                .is_err()
        {
            // A closed macOS peer can leave readable bytes while rejecting timeout changes.
            // Drain only immediately available bytes so the deadline cannot be extended.
            reader.get_ref().set_nonblocking(true)?;
            drain_only = true;
        }
        let reset = if drain_only {
            Some(reader.get_ref().try_clone()?)
        } else {
            None
        };
        let result = reader.fill_buf();
        if let Some(socket) = reset {
            socket.set_nonblocking(false)?;
        }
        let available = result.map_err(|e| {
            if e.kind() == io::ErrorKind::WouldBlock {
                io::Error::new(io::ErrorKind::TimedOut, e)
            } else {
                e
            }
        })?;
        if Instant::now() >= deadline {
            return Err(io::Error::new(io::ErrorKind::TimedOut, "frame deadline"));
        }
        if available.is_empty() {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "incomplete frame",
            ));
        }
        let end = available.iter().position(|b| *b == b'\n');
        let take = end.map_or(available.len(), |i| i + 1);
        let content = end.unwrap_or(take);
        if frame.len() + take > contract::MAX_FRAME_BYTES {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "frame too large",
            ));
        }
        frame.extend_from_slice(&available[..content]);
        reader.consume(take);
        if end.is_some() {
            return Ok(frame);
        }
    }
}

/// Per-connection handshake state. This limited dispatcher has no runtime authority.
pub struct Dispatcher {
    hello: bool,
    started: Instant,
    shutdown_allowed: bool,
    shutdown_requested: bool,
    capabilities: Vec<Value>,
    counts: (u64, u64),
}
impl Default for Dispatcher {
    fn default() -> Self {
        Self {
            hello: false,
            started: Instant::now(),
            shutdown_allowed: false,
            shutdown_requested: false,
            capabilities: Vec::new(),
            counts: (0, 0),
        }
    }
}
impl Dispatcher {
    /// Set the lifetime origin shared by all connections to a daemon.
    pub fn with_started(started: Instant) -> Self {
        Self {
            started,
            ..Self::default()
        }
    }
    pub fn set_capabilities(&mut self, capabilities: Vec<Value>) {
        self.capabilities = capabilities;
    }
    pub fn set_counts(&mut self, active_runs: u64, sessions: u64) {
        self.counts = (active_runs, sessions);
    }
    /// Opt in only when the caller owns the daemon lifecycle.
    pub fn allow_shutdown(&mut self) {
        self.shutdown_allowed = true;
    }
    pub fn shutdown_requested(&self) -> bool {
        self.shutdown_requested
    }
    pub fn dispatch(&mut self, frame: &[u8]) -> Envelope {
        self.dispatch_with(frame, |_| Err("runtime-unavailable"))
    }
    /// Dispatch validated, post-handshake commands to the runtime owner.
    pub fn dispatch_with(
        &mut self,
        frame: &[u8],
        handler: impl FnOnce(
            contract::RequestPayload,
        ) -> Result<contract::ResponsePayload, &'static str>,
    ) -> Envelope {
        let value: Value = match serde_json::from_slice(frame) {
            Ok(value) => value,
            Err(_) => return error("", "malformed-json"),
        };
        if !value.is_object() {
            return error("", "malformed-json");
        }
        let id_owned = value
            .get("id")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_owned();
        let id = id_owned.as_str();
        let valid_id = !id.is_empty() && id.encode_utf16().count() <= 128;
        let request: Envelope = match serde_json::from_value(value) {
            Ok(request) => request,
            Err(_) => return error(if valid_id { id } else { "" }, "invalid-envelope"),
        };
        if !valid_id || request.kind != MessageType::Request || !request.payload.is_object() {
            return error(if valid_id { id } else { "" }, "invalid-envelope");
        }
        if request.version.major != contract::MAJOR {
            return error(id, "incompatible-major");
        }
        let Some((_, fields)) = contract::COMMANDS
            .iter()
            .find(|(name, _)| *name == request.name)
        else {
            return error(id, "unknown-command");
        };
        for (key, kind) in fields.iter() {
            let field = &request.payload[*key];
            let valid = match *kind {
                "string" => field
                    .as_str()
                    .is_some_and(|s| s.encode_utf16().count() <= 65536),
                "integer" => field.as_u64().is_some_and(|n| n <= i32::MAX as u64),
                "boolean" => field.is_boolean(),
                "array" => field.is_array(),
                "object" => field.is_object(),
                _ => false,
            };
            if !valid {
                return error(id, "invalid-payload");
            }
        }
        let typed: contract::RequestPayload =
            match serde_json::from_value(json!({"name":request.name,"payload":request.payload})) {
                Ok(typed) => typed,
                Err(_) => return error(id, "invalid-payload"),
            };
        let response = match typed {
            contract::RequestPayload::Hello {
                client_id, major, ..
            } => {
                if self.hello || client_id.is_empty() {
                    return error(id, "invalid-state");
                }
                if major != contract::MAJOR as u64 {
                    return error(id, "incompatible-major");
                }
                self.hello = true;
                contract::ResponsePayload::Hello {
                    major: contract::MAJOR as u64,
                    minor: contract::MINOR as u64,
                    daemon_version: APP_VERSION.into(),
                    capabilities: self.capabilities.clone(),
                    server_generation: None,
                }
            }
            _ if !self.hello => return error(id, "handshake-required"),
            contract::RequestPayload::DaemonStatus {} => contract::ResponsePayload::DaemonStatus {
                running: true,
                daemon_version: APP_VERSION.into(),
                uptime_ms: self.started.elapsed().as_millis().min(u64::MAX as u128) as u64,
                active_runs: self.counts.0,
                sessions: self.counts.1,
                protocol_major: Some(contract::MAJOR as u64),
                protocol_minor: Some(contract::MINOR as u64),
                endpoint: None,
                server_generation: None,
            },
            contract::RequestPayload::DaemonShutdown {} if self.shutdown_allowed => {
                self.shutdown_requested = true;
                contract::ResponsePayload::DaemonShutdown {
                    shutting_down: true,
                }
            }
            other => match handler(other) {
                Ok(response) => response,
                Err(code) => return error(id, code),
            },
        };
        let mut response = serde_json::to_value(response).expect("typed response serializes");
        if let Some(payload) = response["payload"].as_object_mut() {
            payload.retain(|_, value| !value.is_null());
        }
        Envelope {
            version: Version {
                major: contract::MAJOR,
                minor: contract::MINOR,
            },
            kind: MessageType::Response,
            id: id.to_owned(),
            name: request.name,
            payload: response["payload"].clone(),
        }
    }
}
fn error(id: &str, code: &str) -> Envelope {
    Envelope {
        version: Version {
            major: contract::MAJOR,
            minor: contract::MINOR,
        },
        kind: MessageType::Error,
        id: id.into(),
        name: "ProtocolError".into(),
        payload: json!({"code": code}),
    }
}
