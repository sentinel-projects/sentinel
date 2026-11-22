#![cfg(any(target_os = "linux", target_os = "macos"))]

use rusqlite::Connection;
use sentinel_ipc::{Client, Error as IpcError};
use serde_json::{Value, json};
use std::{
    fs,
    io::{BufRead, BufReader, Read, Write},
    net::{TcpListener, TcpStream},
    os::unix::fs::{DirBuilderExt, FileTypeExt},
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    sync::atomic::{AtomicUsize, Ordering},
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

static NEXT_PROFILE: AtomicUsize = AtomicUsize::new(0);

struct Profile(PathBuf);

impl Profile {
    fn new() -> Self {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = Path::new("/tmp").join(format!(
            "sentinel-durable-test-{}-{nonce}-{}",
            std::process::id(),
            NEXT_PROFILE.fetch_add(1, Ordering::Relaxed)
        ));
        fs::DirBuilder::new().mode(0o700).create(&path).unwrap();
        Self(path)
    }
    fn socket(&self) -> PathBuf {
        self.0.join("daemon-dev.sock")
    }
    fn command(&self, provider: &Provider) -> Command {
        self.command_with_options(provider, None, None)
    }
    fn command_with_context(&self, provider: &Provider, tokens: Option<&str>) -> Command {
        self.command_with_options(provider, tokens, None)
    }
    fn command_with_options(
        &self,
        provider: &Provider,
        tokens: Option<&str>,
        kind: Option<&str>,
    ) -> Command {
        let mut command = Command::new(env!("CARGO_BIN_EXE_sentinel-daemon-dev"));
        command
            .arg("--profile")
            .arg(&self.0)
            .args(["--provider", &provider.origin, "--model", "fixture-model"])
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        if let Some(tokens) = tokens {
            command.args(["--context-window-tokens", tokens]);
        }
        if let Some(kind) = kind {
            command.args(["--provider-kind", kind]);
        }
        command
    }
    fn connect(&self) -> Client {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            if let Ok(client) = Client::connect(&self.socket(), "durable-test") {
                return client;
            }
            assert!(Instant::now() < deadline, "daemon did not start");
            thread::sleep(Duration::from_millis(10));
        }
    }
}

impl Drop for Profile {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

struct Daemon(Child);

impl Daemon {
    fn start(profile: &Profile, provider: &Provider) -> Self {
        Self(profile.command(provider).spawn().unwrap())
    }
    fn start_with_context(profile: &Profile, provider: &Provider, tokens: &str) -> Self {
        Self(
            profile
                .command_with_context(provider, Some(tokens))
                .spawn()
                .unwrap(),
        )
    }
    fn start_with_kind(profile: &Profile, provider: &Provider, kind: &str) -> Self {
        Self(
            profile
                .command_with_options(provider, None, Some(kind))
                .spawn()
                .unwrap(),
        )
    }
    fn crash(&mut self) {
        self.0.kill().unwrap();
        assert!(!self.0.wait().unwrap().success());
    }
    fn shutdown(mut self, profile: &Profile) {
        let mut client = profile.connect();
        assert_eq!(
            client.request("daemon.shutdown", json!({})).unwrap()["shutting_down"],
            true
        );
        assert!(self.0.wait().unwrap().success());
        assert!(!profile.socket().exists());
    }
}

impl Drop for Daemon {
    fn drop(&mut self) {
        if self.0.try_wait().ok().flatten().is_none() {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
}

struct Provider {
    listener: TcpListener,
    probe: TcpListener,
    origin: String,
}

impl Provider {
    fn new() -> Self {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let probe = listener.try_clone().unwrap();
        let origin = format!("http://{}", listener.local_addr().unwrap());
        Self {
            listener,
            probe,
            origin,
        }
    }
    fn serve_once(&self, body: Vec<u8>) -> thread::JoinHandle<Vec<u8>> {
        let listener = self.listener.try_clone().unwrap();
        thread::spawn(move || {
            let (mut socket, _) = listener.accept().unwrap();
            socket
                .set_read_timeout(Some(Duration::from_secs(5)))
                .unwrap();
            socket
                .set_write_timeout(Some(Duration::from_secs(5)))
                .unwrap();
            let (head, request_body) = read_request(&socket);
            assert!(head.starts_with("POST /v1/chat/completions HTTP/1.1"));
            assert!(!head.to_ascii_lowercase().contains("authorization:"));
            let request: Value = serde_json::from_slice(&request_body).unwrap();
            assert_eq!(request["model"], "fixture-model");
            assert_eq!(request["stream"], true);
            write_response(&mut socket, &body);
            request_body
        })
    }
    fn serve_ollama_once(&self, body: Vec<u8>) -> thread::JoinHandle<Vec<u8>> {
        let listener = self.listener.try_clone().unwrap();
        thread::spawn(move || {
            let (mut socket, _) = listener.accept().unwrap();
            socket
                .set_read_timeout(Some(Duration::from_secs(5)))
                .unwrap();
            socket
                .set_write_timeout(Some(Duration::from_secs(5)))
                .unwrap();
            let (head, request_body) = read_request(&socket);
            assert!(head.starts_with("POST /api/chat HTTP/1.1"));
            assert!(
                head.to_ascii_lowercase()
                    .contains("content-type: application/json")
            );
            assert!(!head.to_ascii_lowercase().contains("authorization:"));
            let request: Value = serde_json::from_slice(&request_body).unwrap();
            assert_eq!(request["model"], "fixture-model");
            assert_eq!(request["stream"], true);
            assert_eq!(request["options"]["num_predict"], 2048);
            write_ollama_response(&mut socket, &body);
            request_body
        })
    }
    fn serve_ollama_partial_until_disconnect(
        &self,
    ) -> (thread::JoinHandle<()>, std::sync::mpsc::Receiver<()>) {
        let listener = self.listener.try_clone().unwrap();
        let (ready_tx, ready_rx) = std::sync::mpsc::channel();
        let thread = thread::spawn(move || {
            let (mut socket, _) = listener.accept().unwrap();
            socket
                .set_read_timeout(Some(Duration::from_secs(5)))
                .unwrap();
            let (head, request_body) = read_request(&socket);
            assert!(head.starts_with("POST /api/chat HTTP/1.1"));
            let request: Value = serde_json::from_slice(&request_body).unwrap();
            assert_eq!(request["model"], "fixture-model");
            let first = ollama_delta("partial Ollama", false);
            write!(
                socket,
                "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\nContent-Length: 4096\r\nConnection: close\r\n\r\n"
            )
            .unwrap();
            socket.write_all(&first).unwrap();
            ready_tx.send(()).unwrap();
            let mut byte = [0; 1];
            let _ = socket.read(&mut byte);
        });
        (thread, ready_rx)
    }
    fn serve_partial_until_disconnect(
        &self,
    ) -> (thread::JoinHandle<()>, std::sync::mpsc::Receiver<()>) {
        let listener = self.listener.try_clone().unwrap();
        let (ready_tx, ready_rx) = std::sync::mpsc::channel();
        let thread = thread::spawn(move || {
            let (mut socket, _) = listener.accept().unwrap();
            socket
                .set_read_timeout(Some(Duration::from_secs(5)))
                .unwrap();
            let (head, request_body) = read_request(&socket);
            assert!(head.starts_with("POST /v1/chat/completions HTTP/1.1"));
            let request: Value = serde_json::from_slice(&request_body).unwrap();
            assert_eq!(request["model"], "fixture-model");
            let first = sse_delta("partial", "null");
            write!(
                socket,
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: 4096\r\nConnection: close\r\n\r\n"
            )
            .unwrap();
            socket.write_all(&first).unwrap();
            ready_tx.send(()).unwrap();
            let mut byte = [0; 1];
            let _ = socket.read(&mut byte);
        });
        (thread, ready_rx)
    }
    fn serve_gated_complete(
        &self,
        answer: &str,
    ) -> (
        thread::JoinHandle<()>,
        std::sync::mpsc::Receiver<()>,
        std::sync::mpsc::Sender<()>,
    ) {
        let listener = self.listener.try_clone().unwrap();
        let answer = answer.to_owned();
        let (ready_tx, ready_rx) = std::sync::mpsc::channel();
        let (release_tx, release_rx) = std::sync::mpsc::channel();
        let thread = thread::spawn(move || {
            let (mut socket, _) = listener.accept().unwrap();
            socket
                .set_read_timeout(Some(Duration::from_secs(5)))
                .unwrap();
            let (head, request_body) = read_request(&socket);
            assert!(head.starts_with("POST /v1/chat/completions HTTP/1.1"));
            let request: Value = serde_json::from_slice(&request_body).unwrap();
            assert_eq!(request["model"], "fixture-model");
            let first = sse_delta(&answer, "null");
            let mut rest = sse_delta("", "\"stop\"");
            rest.extend_from_slice(b"data: [DONE]\n\n");
            write!(
                socket,
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
                first.len() + rest.len()
            )
            .unwrap();
            socket.write_all(&first).unwrap();
            ready_tx.send(()).unwrap();
            release_rx.recv_timeout(Duration::from_secs(5)).unwrap();
            socket.write_all(&rest).unwrap();
        });
        (thread, ready_rx, release_tx)
    }
    fn assert_no_pending_request(&self) {
        self.probe.set_nonblocking(true).unwrap();
        match self.probe.accept() {
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {}
            Ok((socket, _)) => {
                let _ = socket.shutdown(std::net::Shutdown::Both);
                panic!("provider received an unexpected request")
            }
            Err(error) => panic!("provider accept probe failed: {error}"),
        }
        self.probe.set_nonblocking(false).unwrap();
    }
}

fn read_request(socket: &TcpStream) -> (String, Vec<u8>) {
    let mut reader = BufReader::new(socket.try_clone().unwrap());
    let mut head = Vec::new();
    let mut content_length = 0usize;
    loop {
        let mut line = Vec::new();
        reader.read_until(b'\n', &mut line).unwrap();
        if line == b"\r\n" {
            break;
        }
        if let Ok(line_text) = std::str::from_utf8(&line)
            && let Some((name, value)) = line_text.split_once(':')
            && name.eq_ignore_ascii_case("content-length")
        {
            content_length = value.trim().parse().unwrap();
        }
        head.extend_from_slice(&line);
    }
    let head = String::from_utf8(head).unwrap();
    let mut body = vec![0; content_length];
    reader.read_exact(&mut body).unwrap();
    (head, body)
}

fn write_response(socket: &mut TcpStream, body: &[u8]) {
    write!(
        socket,
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    )
    .unwrap();
    socket.write_all(body).unwrap();
}

fn write_ollama_response(socket: &mut TcpStream, body: &[u8]) {
    write!(
        socket,
        "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
        body.len()
    )
    .unwrap();
    socket.write_all(body).unwrap();
}

fn sse_delta(text: &str, finish_reason: &str) -> Vec<u8> {
    format!(
        "data: {{\"choices\":[{{\"delta\":{{\"content\":{}}},\"finish_reason\":{finish_reason}}}]}}\n\n",
        serde_json::to_string(text).unwrap()
    )
    .into_bytes()
}

fn completed_stream(answer: &str) -> Vec<u8> {
    let mut body = sse_delta(answer, "null");
    body.extend_from_slice(&sse_delta("", "\"stop\""));
    body.extend_from_slice(b"data: [DONE]\n\n");
    body
}

fn ollama_delta(text: &str, done: bool) -> Vec<u8> {
    let value =
        json!({"model":"fixture-model","message":{"role":"assistant","content":text},"done":done});
    let mut line = serde_json::to_vec(&value).unwrap();
    line.push(b'\n');
    line
}

fn ollama_completed_stream(answer: &str) -> Vec<u8> {
    let mut body = ollama_delta(answer, false);
    let mut final_chunk = serde_json::to_vec(&json!({
        "model":"fixture-model",
        "message":{"role":"assistant","content":""},
        "done":true,
        "done_reason":"stop"
    }))
    .unwrap();
    final_chunk.push(b'\n');
    body.extend_from_slice(&final_chunk);
    body
}

fn create_session(client: &mut Client) -> String {
    client
        .request("session.create", json!({"title":"durable"}))
        .unwrap()["session_id"]
        .as_str()
        .unwrap()
        .to_owned()
}

fn wait_for_terminal(client: &mut Client, run_id: &str) -> sentinel_ipc::Envelope {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        assert!(
            Instant::now() < deadline,
            "run did not reach a terminal event"
        );
        let event = client.next_event().unwrap();
        if event.payload["run_id"] == run_id
            && ["run.completed", "run.failed", "run.cancelled"].contains(&event.name.as_str())
        {
            return event;
        }
    }
}

fn message_rows(client: &mut Client, session_id: &str) -> Vec<Value> {
    client
        .request("session.messages", json!({"session_id":session_id}))
        .unwrap()["messages"]
        .as_array()
        .unwrap()
        .clone()
}

fn send_and_finish(client: &mut Client, session_id: &str, text: &str) -> String {
    let run = client
        .request("chat.send", json!({"session_id":session_id,"text":text}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(wait_for_terminal(client, &run).name, "run.completed");
    run
}

fn request_messages(request: &[u8]) -> Vec<Value> {
    serde_json::from_slice::<Value>(request).unwrap()["messages"]
        .as_array()
        .unwrap()
        .clone()
}

#[test]
fn terminal_attach_uses_the_bounded_session_snapshot() {
    let profile = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon(profile.command(&provider).spawn().unwrap());
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let terminal = client
        .request("terminal.attach", json!({"session_id":session}))
        .unwrap();
    let desktop = client
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(terminal["session_id"], session);
    assert_eq!(terminal["state"], desktop["state"]);
    assert_eq!(terminal["output"], desktop["output"]);
    assert_eq!(terminal["run_id"], desktop["run_id"]);
    assert_eq!(terminal["run_type"], desktop["run_type"]);
    assert_eq!(terminal["title"], desktop["title"]);
    daemon.shutdown(&profile);
}

#[test]
fn voice_state_reports_unavailable_without_a_speech_runtime() {
    let profile = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon(profile.command(&provider).spawn().unwrap());
    let mut client = profile.connect();
    let state = client.request("voice.state", json!({})).unwrap();
    assert_eq!(state["state"], "Unavailable");
    assert_eq!(state["available"], false);
    assert_eq!(state["owned"], false);
    assert_eq!(state["failure"], "RuntimeUnavailable");
    assert_eq!(state["transcript"], "");
    daemon.shutdown(&profile);
}

#[test]
fn desktop_projection_reports_partial_runtime_without_health_claims() {
    let profile = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon(profile.command(&provider).spawn().unwrap());
    let mut client = profile.connect();
    let result = client
        .request("desktop.projection", json!({"page":0}))
        .unwrap();
    assert_eq!(result["page"], 0);
    assert_eq!(result["pages"], 28);
    assert_eq!(
        result["properties"]["activeRuntimeModelLabel"],
        "fixture-model"
    );
    assert_eq!(
        result["properties"]["activeRuntimeReadinessState"],
        "unknown"
    );
    assert!(
        client
            .request("desktop.projection", json!({"page":1}))
            .is_err()
    );
    daemon.shutdown(&profile);
}

#[test]
fn provider_settings_diagnostics_do_not_claim_connectivity() {
    let profile = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon(profile.command(&provider).spawn().unwrap());
    let mut client = profile.connect();
    let query = |client: &mut Client, id: &str| {
        client
            .request(
                "desktop.settings_service",
                json!({"action":"providerSettingsState","arguments":[id]}),
            )
            .unwrap()["result"]
            .clone()
    };
    let known = query(&mut client, "openai-compatible-local");
    assert_eq!(known["known"], true);
    assert_eq!(known["health"], "Unknown");
    assert_eq!(known["catalog"], "Unknown");
    assert_eq!(known["configuredModel"], "fixture-model");
    assert_eq!(query(&mut client, "ollama")["known"], false);
    daemon.shutdown(&profile);
}

#[test]
fn response_profile_is_durable_and_included_in_chat_request() {
    let profile = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon(profile.command(&provider).spawn().unwrap());
    let mut client = profile.connect();
    let session = create_session(&mut client);
    assert_eq!(
        client
            .request(
                "desktop.setting",
                json!({"key":"responseProfileInstructions","value":"  concise \"quoted\"  "})
            )
            .unwrap()["accepted"],
        true
    );
    assert!(
        client
            .request(
                "desktop.setting",
                json!({"key":"responseProfileInstructions","value":"x".repeat(2001)})
            )
            .is_err()
    );
    daemon.shutdown(&profile);
    let daemon = Daemon(profile.command(&provider).spawn().unwrap());
    let mut client = profile.connect();
    assert_eq!(
        client.request("desktop.settings", json!({})).unwrap()["values"]["responseProfileInstructions"],
        "concise \"quoted\""
    );
    let fixture = provider.serve_once(completed_stream("answer"));
    send_and_finish(&mut client, &session, "question");
    let messages = request_messages(&fixture.join().unwrap());
    assert_eq!(messages[0]["role"], "system");
    assert!(
        messages[0]["content"]
            .as_str()
            .unwrap()
            .contains("concise \\\"quoted\\\"")
    );
    assert!(
        messages[0]["content"]
            .as_str()
            .unwrap()
            .contains("grants no tool or data access")
    );
    assert_eq!(messages[1]["content"], "question");
    daemon.shutdown(&profile);
}

#[test]
fn plugin_catalog_reads_explicit_root_without_launching() {
    let profile = Profile::new();
    let plugins = Profile::new();
    fs::create_dir(plugins.0.join("demo")).unwrap();
    fs::write(plugins.0.join("demo/plugin.json"), json!({"id":"demo","name":"Demo","version":"1","entry_point":"missing.so","host_capabilities":["FilesystemRead"]}).to_string()).unwrap();
    let provider = Provider::new();
    let mut command = profile.command(&provider);
    command.arg("--plugin-root").arg(&plugins.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    let result = client
        .request(
            "desktop.settings_service",
            json!({"action":"extensionsHubState","arguments":[]}),
        )
        .unwrap();
    assert_eq!(result["result"]["entries"][0]["id"], "plugin:demo");
    assert_eq!(result["result"]["entries"][0]["hostRunning"], false);
    assert_eq!(result["result"]["entries"][0]["actions"], json!([]));
    daemon.shutdown(&profile);
}

#[test]
fn text_attachment_is_persisted_and_reused_after_restart() {
    let profile = Profile::new();
    let workspace = Profile::new();
    fs::write(workspace.0.join("note.txt"), "attachment fixture").unwrap();
    let provider = Provider::new();
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let fixture = provider.serve_once(completed_stream("answer"));
    let run = client
        .request(
            "chat.send",
            json!({"session_id":session,"text":"question","attachments":["note.txt"]}),
        )
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(wait_for_terminal(&mut client, &run).name, "run.completed");
    let request = fixture.join().unwrap();
    assert!(
        request_messages(&request)[0]["content"]
            .as_str()
            .unwrap()
            .contains("attachment fixture")
    );
    daemon.shutdown(&profile);
    fs::write(workspace.0.join("note.txt"), "changed later").unwrap();
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    let fixture = provider.serve_once(completed_stream("follow-up"));
    send_and_finish(&mut client, &session, "next");
    let messages = request_messages(&fixture.join().unwrap());
    assert!(
        messages[0]["content"]
            .as_str()
            .unwrap()
            .contains("attachment fixture")
    );
    assert!(
        !messages[0]["content"]
            .as_str()
            .unwrap()
            .contains("changed later")
    );
    daemon.shutdown(&profile);
}

#[test]
fn agent_inspect_file_records_evidence_and_rejects_arbitrary_goal() {
    let profile = Profile::new();
    let workspace = Profile::new();
    fs::write(workspace.0.join("note.txt"), "fixture evidence").unwrap();
    let provider = Provider::new();
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    let session = create_session(&mut client);
    assert!(
        client
            .request(
                "agent.start",
                json!({"session_id":session,"text":"arbitrary goal"})
            )
            .is_err()
    );
    provider.assert_no_pending_request();

    let decision = json!({"action":"tool","tool":"read-file","argsJson":"{\"path\":\"note.txt\"}"})
        .to_string();
    let first = provider.serve_once(completed_stream(&decision));
    let task = json!({"operation":"inspect-file","path":"note.txt"}).to_string();
    let response = client
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap();
    let run = response["run_id"].as_str().unwrap().to_owned();
    first.join().unwrap();
    let final_decision = json!({"action":"final","grounding":"verified","claims":[{"id":"read-1","assertion":true}]}).to_string();
    let second = provider.serve_once(completed_stream(&final_decision));
    let terminal = wait_for_terminal(&mut client, &run);
    assert_eq!(terminal.name, "run.completed");
    assert!(
        terminal.payload["text"]
            .as_str()
            .unwrap()
            .contains("fixture evidence")
    );
    let request = second.join().unwrap();
    assert!(
        String::from_utf8(request)
            .unwrap()
            .contains("fixture evidence")
    );
    let audit = Connection::open(profile.0.join("daemon-dev-agent.sqlite3")).unwrap();
    let (state, confirmed): (String, bool) = audit
        .query_row(
            "SELECT state,confirmed FROM runs WHERE id=?1",
            [&run],
            |r| Ok((r.get(0)?, r.get(1)?)),
        )
        .unwrap();
    assert_eq!(state, "completed");
    assert!(confirmed);
    let (path, content, complete): (String, String, bool) = audit
        .query_row(
            "SELECT path,content,complete FROM observations WHERE run_id=?1",
            [&run],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)),
        )
        .unwrap();
    assert_eq!(
        (path.as_str(), content.as_str(), complete),
        ("note.txt", "fixture evidence", true)
    );
    let (arguments, descriptor_version): (String, String) = audit
        .query_row(
            "SELECT arguments,descriptor_version FROM observations WHERE run_id=?1",
            [&run],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .unwrap();
    assert_eq!(
        serde_json::from_str::<Value>(&arguments).unwrap(),
        json!({"path":"note.txt","offset":1,"limit":2000})
    );
    assert_eq!(descriptor_version, "dev-1");
    drop(audit);
    let history = client
        .request(
            "agent.history",
            json!({"run_id":run,"before_time":"","before_id":""}),
        )
        .unwrap();
    assert_eq!(history["history"]["run"]["runId"], run);
    assert_eq!(history["history"]["run"]["modelId"], "fixture-model");
    let mut unrelated = profile.connect();
    assert!(
        unrelated
            .request(
                "agent.history",
                json!({"run_id":run,"before_time":"","before_id":""})
            )
            .is_err()
    );
    daemon.shutdown(&profile);
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut resumed = profile.connect();
    resumed
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    let history = resumed
        .request(
            "agent.history",
            json!({"run_id":run,"before_time":"","before_id":""}),
        )
        .unwrap();
    assert_eq!(history["history"]["run"]["state"], "completed");
    assert_eq!(history["history"]["tools"][0]["descriptorVersion"], "dev-1");
    let arguments = history["history"]["tools"][0]["arguments"]
        .as_str()
        .unwrap();
    assert_eq!(
        serde_json::from_str::<Value>(arguments).unwrap(),
        json!({"path":"note.txt","offset":1,"limit":2000})
    );
    assert_eq!(
        history["history"]["run"]["providerId"],
        "openai-compatible-local"
    );
    daemon.shutdown(&profile);
}

#[test]
fn agent_multifile_and_directory_preserve_explicit_scope() {
    let profile = Profile::new();
    let workspace = Profile::new();
    fs::write(workspace.0.join("a"), "alpha").unwrap();
    fs::write(workspace.0.join("b"), "beta").unwrap();
    fs::write(workspace.0.join(".hidden"), "secret").unwrap();
    fs::write(workspace.0.join("line\nspoof"), "x").unwrap();
    let provider = Provider::new();
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let tool = |name: &str, args: Value| {
        json!({"action":"tool","tool":name,"argsJson":args.to_string()}).to_string()
    };
    let first = provider.serve_once(completed_stream(&tool("read-file", json!({"path":"b"}))));
    let task = json!({"operation":"inspect-files","paths":["a","b"]}).to_string();
    let run = client
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    first.join().unwrap();
    let second = provider.serve_once(completed_stream(&tool("read-file", json!({"path":"a"}))));
    second.join().unwrap();
    let final_decision = json!({"action":"final","grounding":"verified","claims":[{"id":"read-2","assertion":true},{"id":"read-1","assertion":true}]}).to_string();
    let final_response = provider.serve_once(completed_stream(&final_decision));
    let terminal = wait_for_terminal(&mut client, &run);
    final_response.join().unwrap();
    assert_eq!(terminal.name, "run.completed");
    assert_eq!(terminal.payload["text"], "File: a\nalpha\n\nFile: b\nbeta");
    let audit = Connection::open(profile.0.join("daemon-dev-agent.sqlite3")).unwrap();
    let rows: Vec<(i64, String, String, bool)> = audit
        .prepare(
            "SELECT step,path,content,complete FROM observations WHERE run_id=?1 ORDER BY step",
        )
        .unwrap()
        .query_map([&run], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)))
        .unwrap()
        .map(Result::unwrap)
        .collect();
    assert_eq!(
        rows,
        vec![
            (1, "a".into(), "alpha".into(), true),
            (2, "b".into(), "beta".into(), true)
        ]
    );
    drop(audit);

    let listing = provider.serve_once(completed_stream(&tool(
        "list-directory",
        json!({"path":".","includeHidden":false}),
    )));
    let task =
        json!({"operation":"inspect-directory","path":".","includeHidden":false}).to_string();
    let run = client
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    listing.join().unwrap();
    let final_decision = json!({"action":"final","grounding":"verified","claims":[{"id":"read-1","assertion":true}]}).to_string();
    let final_response = provider.serve_once(completed_stream(&final_decision));
    let terminal = wait_for_terminal(&mut client, &run);
    final_response.join().unwrap();
    assert_eq!(terminal.name, "run.completed");
    let output = terminal.payload["text"].as_str().unwrap();
    assert!(output.contains("Hidden entries excluded"));
    assert!(output.contains("a") && output.contains("b"));
    assert!(!output.contains(".hidden"));
    assert!(output.contains("line\\nspoof"));

    let first = provider.serve_once(completed_stream(&tool("read-file", json!({"path":"a"}))));
    let task = json!({"operation":"inspect-files","paths":["a","b"]}).to_string();
    let run = client
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    first.join().unwrap();
    let early = json!({"action":"final","grounding":"verified","claims":[{"id":"read-1","assertion":true},{"id":"read-2","assertion":true}]}).to_string();
    let response = provider.serve_once(completed_stream(&early));
    response.join().unwrap();
    let second_rejection = provider.serve_once(completed_stream(&early));
    let terminal = wait_for_terminal(&mut client, &run);
    let feedback = String::from_utf8(second_rejection.join().unwrap()).unwrap();
    assert!(feedback.contains("Final rejected"));
    assert_eq!(terminal.name, "run.failed");
    daemon.shutdown(&profile);
}

#[test]
fn completed_messages_survive_orderly_restart_without_provider_replay() {
    let profile = Profile::new();
    let provider = Provider::new();
    let fixture = provider.serve_once(completed_stream("persisted answer"));
    let daemon = Daemon::start(&profile, &provider);
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let run = client
        .request(
            "chat.send",
            json!({"session_id":session,"text":"persist this"}),
        )
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(wait_for_terminal(&mut client, &run).name, "run.completed");
    let messages = message_rows(&mut client, &session);
    assert_eq!(messages.len(), 2);
    assert_eq!(messages[0]["role"], "user");
    assert_eq!(messages[0]["content"], "persist this");
    assert_eq!(messages[0]["status"], "sent");
    assert_eq!(messages[1]["role"], "assistant");
    assert_eq!(messages[1]["content"], "persisted answer");
    assert_eq!(messages[1]["status"], "completed");
    fixture.join().unwrap();
    drop(client);
    daemon.shutdown(&profile);

    let daemon = Daemon::start(&profile, &provider);
    let mut client = profile.connect();
    let listed = client.request("session.list", json!({})).unwrap();
    assert_eq!(listed["sessions"].as_array().unwrap().len(), 1);
    let attached = client
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(attached["state"], "completed");
    assert_eq!(attached["output"], "persisted answer");
    assert_eq!(attached["run_id"], run);
    assert_eq!(message_rows(&mut client, &session), messages);
    thread::sleep(Duration::from_millis(250));
    provider.assert_no_pending_request();
    drop(client);
    daemon.shutdown(&profile);
}

#[test]
fn interrupted_run_is_failed_on_restart_and_never_replayed() {
    let profile = Profile::new();
    let provider = Provider::new();
    let (fixture, accepted) = provider.serve_partial_until_disconnect();
    let mut daemon = Daemon::start(&profile, &provider);
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let run = client
        .request(
            "chat.send",
            json!({"session_id":session,"text":"interrupt this"}),
        )
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let mut saw_delta = false;
    while !saw_delta {
        let event = client.next_event().unwrap();
        if event.name == "output.delta" {
            assert_eq!(event.payload["text"], "partial");
            saw_delta = true;
        }
    }
    accepted.recv_timeout(Duration::from_secs(3)).unwrap();
    daemon.crash();
    fixture.join().unwrap();
    drop(client);

    // The dev server intentionally refuses stale endpoints; this test removes
    // only the socket it just observed in its private temporary profile.
    let socket_meta = fs::symlink_metadata(profile.socket()).unwrap();
    assert!(socket_meta.file_type().is_socket());
    fs::remove_file(profile.socket()).unwrap();

    let daemon = Daemon::start(&profile, &provider);
    let mut client = profile.connect();
    let attached = client
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(attached["state"], "failed");
    assert_eq!(attached["detail"], "daemon-restarted");
    assert_eq!(attached["output"], "");
    assert_eq!(attached["run_id"], run);
    let messages = message_rows(&mut client, &session);
    assert_eq!(messages.len(), 2);
    assert_eq!(messages[0]["role"], "user");
    assert_eq!(messages[0]["content"], "interrupt this");
    assert_eq!(messages[1]["role"], "assistant");
    assert_eq!(messages[1]["content"], "");
    assert_eq!(messages[1]["status"], "interrupted");
    assert_eq!(messages[1]["partial"], true);
    thread::sleep(Duration::from_millis(250));
    provider.assert_no_pending_request();
    drop(client);
    daemon.shutdown(&profile);
}

#[test]
fn failed_run_write_is_not_acknowledged_or_dispatched() {
    let profile = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon::start(&profile, &provider);
    let mut client = profile.connect();
    let session = create_session(&mut client);
    // DELETE-journal mode needs this sibling path to be a regular journal file.
    // A directory makes the begin transaction fail deterministically.
    fs::create_dir(profile.0.join("daemon-dev-chat.sqlite3-journal")).unwrap();
    assert!(matches!(
        client.request(
            "chat.send",
            json!({"session_id":session,"text":"must not dispatch"})
        ),
        Err(IpcError::Remote(code)) if code == "storage-failure"
    ));
    thread::sleep(Duration::from_millis(100));
    provider.assert_no_pending_request();
    fs::remove_dir(profile.0.join("daemon-dev-chat.sqlite3-journal")).unwrap();
    drop(client);
    daemon.shutdown(&profile);

    // The failed storage boundary is sticky for this daemon process. Restart
    // after removing the injected fault and verify the transaction rolled back.
    let daemon = Daemon::start(&profile, &provider);
    let mut client = profile.connect();
    let attached = client
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(attached["state"], "idle");
    assert_eq!(attached["run_id"], "");
    assert!(message_rows(&mut client, &session).is_empty());
    provider.assert_no_pending_request();
    drop(client);
    daemon.shutdown(&profile);
}

#[test]
fn failed_terminal_transaction_reports_storage_failure_without_assistant_row() {
    let profile = Profile::new();
    let provider = Provider::new();
    let (fixture, accepted, release) = provider.serve_gated_complete("terminal text");
    let daemon = Daemon::start(&profile, &provider);
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let run = client
        .request(
            "chat.send",
            json!({"session_id":session,"text":"terminal transaction"}),
        )
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let mut saw_delta = false;
    while !saw_delta {
        let event = client.next_event().unwrap();
        if event.name == "output.delta" {
            assert_eq!(event.payload["text"], "terminal text");
            saw_delta = true;
        }
    }
    accepted.recv_timeout(Duration::from_secs(3)).unwrap();
    let lock = Connection::open(profile.0.join("daemon-dev-chat.sqlite3")).unwrap();
    lock.execute_batch("BEGIN EXCLUSIVE;").unwrap();
    release.send(()).unwrap();
    let terminal = wait_for_terminal(&mut client, &run);
    assert_eq!(terminal.name, "run.failed");
    assert_eq!(terminal.payload["terminal_reason"], "storage-failure");
    lock.execute_batch("ROLLBACK;").unwrap();
    assert!(
        matches!(client.request("session.attach", json!({"session_id":session})), Err(IpcError::Remote(code)) if code == "storage-failure")
    );
    assert!(
        matches!(client.request("session.messages", json!({"session_id":session})), Err(IpcError::Remote(code)) if code == "storage-failure")
    );
    fixture.join().unwrap();
    let assistant_count: i64 = lock
        .query_row(
            "SELECT COUNT(*) FROM messages WHERE session_id=?1 AND role='assistant'",
            [&session],
            |row| row.get(0),
        )
        .unwrap();
    assert_eq!(assistant_count, 0);
    drop(lock);
    drop(client);
    daemon.shutdown(&profile);
    let daemon = Daemon::start(&profile, &provider);
    let mut client = profile.connect();
    let attached = client
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(attached["state"], "failed");
    assert_eq!(attached["detail"], "daemon-restarted");
    let messages = message_rows(&mut client, &session);
    assert_eq!(messages.len(), 2);
    assert_eq!(messages[0]["role"], "user");
    assert_eq!(messages[0]["content"], "terminal transaction");
    assert_eq!(messages[1]["role"], "assistant");
    assert_eq!(messages[1]["status"], "interrupted");
    assert_eq!(messages[1]["content"], "");
    assert_eq!(messages[1]["partial"], true);
    provider.assert_no_pending_request();
    drop(client);
    daemon.shutdown(&profile);
}

#[test]
fn provider_context_restores_after_restart_and_stays_session_scoped() {
    let profile = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon::start(&profile, &provider);
    let mut client = profile.connect();
    let session_a = create_session(&mut client);
    let first_request = provider.serve_once(completed_stream("answer one"));
    send_and_finish(&mut client, &session_a, "question one");
    let first_body = first_request.join().unwrap();
    assert_eq!(
        request_messages(&first_body),
        vec![json!({"role":"user","content":"question one"})]
    );
    drop(client);
    daemon.shutdown(&profile);

    let daemon = Daemon::start(&profile, &provider);
    let mut client = profile.connect();
    let session_b = create_session(&mut client);
    let follow_up = provider.serve_once(completed_stream("answer two"));
    send_and_finish(&mut client, &session_a, "question two");
    assert_eq!(
        request_messages(&follow_up.join().unwrap()),
        vec![
            json!({"role":"user","content":"question one"}),
            json!({"role":"assistant","content":"answer one"}),
            json!({"role":"user","content":"question two"})
        ]
    );

    let isolated = provider.serve_once(completed_stream("isolated answer"));
    send_and_finish(&mut client, &session_b, "other session question");
    assert_eq!(
        request_messages(&isolated.join().unwrap()),
        vec![json!({"role":"user","content":"other session question"})]
    );
    drop(client);
    daemon.shutdown(&profile);
}

#[test]
fn provider_context_excludes_failed_and_cancelled_turns() {
    let profile = Profile::new();
    let provider = Provider::new();
    let mut daemon = Daemon::start(&profile, &provider);
    let mut client = profile.connect();
    let session = create_session(&mut client);

    let first = provider.serve_once(completed_stream("eligible answer"));
    send_and_finish(&mut client, &session, "eligible question");
    first.join().unwrap();

    let failed_body = sse_delta("failed partial", "null");
    let failed_fixture = provider.serve_once(failed_body);
    let failed_run = client
        .request(
            "chat.send",
            json!({"session_id":session,"text":"failed question"}),
        )
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(
        wait_for_terminal(&mut client, &failed_run).name,
        "run.failed"
    );
    failed_fixture.join().unwrap();

    let (cancel_fixture, accepted) = provider.serve_partial_until_disconnect();
    let cancelled_run = client
        .request(
            "chat.send",
            json!({"session_id":session,"text":"cancelled question"}),
        )
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    accepted.recv_timeout(Duration::from_secs(3)).unwrap();
    assert_eq!(
        client
            .request("run.cancel", json!({"run_id":cancelled_run}))
            .unwrap()["accepted"],
        true
    );
    assert_eq!(
        wait_for_terminal(&mut client, &cancelled_run).name,
        "run.cancelled"
    );
    cancel_fixture.join().unwrap();

    let (interrupted_fixture, accepted) = provider.serve_partial_until_disconnect();
    let interrupted_run = client
        .request(
            "chat.send",
            json!({"session_id":session,"text":"interrupted question"}),
        )
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    accepted.recv_timeout(Duration::from_secs(3)).unwrap();
    daemon.crash();
    interrupted_fixture.join().unwrap();
    drop(client);
    let socket_meta = fs::symlink_metadata(profile.socket()).unwrap();
    assert!(socket_meta.file_type().is_socket());
    fs::remove_file(profile.socket()).unwrap();

    let daemon = Daemon::start(&profile, &provider);
    let mut client = profile.connect();
    let recovered = client
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(recovered["run_id"], interrupted_run);
    assert_eq!(recovered["state"], "failed");
    assert_eq!(recovered["detail"], "daemon-restarted");

    let final_request = provider.serve_once(completed_stream("final answer"));
    send_and_finish(&mut client, &session, "final question");
    let messages = request_messages(&final_request.join().unwrap());
    assert_eq!(
        messages,
        vec![
            json!({"role":"user","content":"eligible question"}),
            json!({"role":"assistant","content":"eligible answer"}),
            json!({"role":"user","content":"final question"})
        ]
    );
    let rows = message_rows(&mut client, &session);
    assert_eq!(rows[3]["status"], "failed");
    assert_eq!(rows[3]["partial"], true);
    assert_eq!(rows[5]["status"], "cancelled");
    assert_eq!(rows[5]["partial"], true);
    assert_eq!(rows[7]["status"], "interrupted");
    assert_eq!(rows[7]["partial"], true);
    drop(client);
    daemon.shutdown(&profile);
}

#[test]
fn provider_context_drops_old_turns_whole_and_oversized_prompt_is_not_committed() {
    let profile = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon::start_with_context(&profile, &provider, "1000000");
    let mut client = profile.connect();
    let session = create_session(&mut client);

    let prompts = [
        format!("oldest {}", "a".repeat(45_000)),
        format!("middle {}", "b".repeat(45_000)),
        format!("newest {}", "c".repeat(45_000)),
    ];
    for (index, prompt) in prompts.iter().enumerate() {
        let answer = format!("answer-{} {}", index, "r".repeat(10_000));
        let fixture = provider.serve_once(completed_stream(&answer));
        send_and_finish(&mut client, &session, prompt);
        fixture.join().unwrap();
    }

    let current = format!("current {}", "d".repeat(45_000));
    let fixture = provider.serve_once(completed_stream("bounded answer"));
    send_and_finish(&mut client, &session, &current);
    let sent = request_messages(&fixture.join().unwrap());
    assert_eq!(
        sent,
        vec![
            json!({"role":"user","content":prompts[2]}),
            json!({"role":"assistant","content":format!("answer-2 {}", "r".repeat(10_000))}),
            json!({"role":"user","content":current})
        ]
    );

    let isolated = create_session(&mut client);
    let oversized = "\0".repeat(22_000);
    assert!(matches!(
        client.request(
            "chat.send",
            json!({"session_id":isolated,"text":oversized})
        ),
        Err(IpcError::Remote(code)) if code == "message-too-large"
    ));
    provider.assert_no_pending_request();
    let attached = client
        .request("session.attach", json!({"session_id":isolated}))
        .unwrap();
    assert_eq!(attached["state"], "idle");
    assert!(message_rows(&mut client, &isolated).is_empty());
    drop(client);
    daemon.shutdown(&profile);
}

#[test]
fn configured_token_window_keeps_only_recent_complete_turns() {
    let profile = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon::start_with_context(&profile, &provider, "4000");
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let prompts =
        ["oldest", "middle", "newest"].map(|label| format!("{label} {}", "q".repeat(1_400)));
    for (index, prompt) in prompts.iter().enumerate() {
        let answer = format!("answer-{index} {}", "r".repeat(350));
        let fixture = provider.serve_once(completed_stream(&answer));
        send_and_finish(&mut client, &session, prompt);
        fixture.join().unwrap();
    }
    let current = format!("current {}", "c".repeat(1_400));
    let fixture = provider.serve_once(completed_stream("bounded answer"));
    send_and_finish(&mut client, &session, &current);
    let body = fixture.join().unwrap();
    assert!(body.len() < 128 * 1024);
    assert_eq!(
        request_messages(&body),
        vec![
            json!({"role":"user","content":prompts[2]}),
            json!({"role":"assistant","content":format!("answer-2 {}", "r".repeat(350))}),
            json!({"role":"user","content":current})
        ]
    );
    drop(client);
    daemon.shutdown(&profile);
}

#[test]
fn fallback_context_budget_rejects_escaped_mandatory_prompt_without_mutation() {
    let profile = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon::start(&profile, &provider); // omitted flag: conservative 8192-token fallback
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let escaped = "\0".repeat(20_000); // JSON body stays below 128 KiB; token estimate exceeds fallback.
    assert!(matches!(
        client.request(
            "chat.send",
            json!({"session_id":session,"text":escaped})
        ),
        Err(IpcError::Remote(code)) if code == "context-limit-exceeded"
    ));
    provider.assert_no_pending_request();
    let attached = client
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(attached["state"], "idle");
    assert_eq!(attached["run_id"], "");
    assert!(message_rows(&mut client, &session).is_empty());
    drop(client);
    daemon.shutdown(&profile);
}

#[test]
fn invalid_context_window_flags_fail_before_profile_mutation() {
    let profile = Profile::new();
    let provider = Provider::new();
    let status = profile
        .command_with_context(&provider, Some("0"))
        .status()
        .unwrap();
    assert!(!status.success());
    assert!(!profile.socket().exists());
    assert!(!profile.0.join("daemon-dev-chat.sqlite3").exists());
    assert!(!profile.0.join("daemon-dev.lock").exists());

    let mut duplicate = profile.command_with_context(&provider, Some("8192"));
    duplicate.args(["--context-window-tokens", "8192"]);
    let status = duplicate.status().unwrap();
    assert!(!status.success());
    assert!(!profile.socket().exists());
    assert!(!profile.0.join("daemon-dev-chat.sqlite3").exists());
    assert!(!profile.0.join("daemon-dev.lock").exists());

    let mut unknown = profile.command_with_options(&provider, None, Some("openai"));
    let status = unknown.status().unwrap();
    assert!(!status.success());
    assert!(!profile.socket().exists());
    assert!(!profile.0.join("daemon-dev-chat.sqlite3").exists());
    assert!(!profile.0.join("daemon-dev.lock").exists());

    let mut duplicate_kind = profile.command_with_options(&provider, None, Some("lm-studio"));
    duplicate_kind.args(["--provider-kind", "llama-cpp-server"]);
    let status = duplicate_kind.status().unwrap();
    assert!(!status.success());
    assert!(!profile.socket().exists());
    assert!(!profile.0.join("daemon-dev-chat.sqlite3").exists());
    assert!(!profile.0.join("daemon-dev.lock").exists());
}

#[test]
fn declared_provider_kind_survives_restarts_until_a_new_run_binds() {
    let profile = Profile::new();
    let provider_lm = Provider::new();
    let provider_llama = Provider::new();
    let provider_compat = Provider::new();
    let daemon = Daemon::start_with_kind(&profile, &provider_lm, "lm-studio");
    let mut client = profile.connect();
    let session = create_session(&mut client);

    let first_fixture = provider_lm.serve_once(completed_stream("answer from lm"));
    send_and_finish(&mut client, &session, "question for lm");
    first_fixture.join().unwrap();
    let first_rows = message_rows(&mut client, &session);
    assert_eq!(first_rows[1]["id"], 2);
    assert_eq!(first_rows[1]["provider_id"], "lm-studio");
    drop(client);
    daemon.shutdown(&profile);

    let daemon = Daemon::start_with_kind(&profile, &provider_llama, "llama-cpp-server");
    let mut client = profile.connect();
    let attached = client
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(attached["provider_id"], "lm-studio");
    assert_eq!(attached["model_id"], "fixture-model");
    let second_fixture = provider_llama.serve_once(completed_stream("answer from llama"));
    send_and_finish(&mut client, &session, "question for llama");
    second_fixture.join().unwrap();
    let second_rows = message_rows(&mut client, &session);
    assert_eq!(second_rows[1]["id"], 2);
    assert_eq!(second_rows[1]["provider_id"], "lm-studio");
    assert_eq!(second_rows[3]["id"], 4);
    assert_eq!(second_rows[3]["provider_id"], "llama-cpp-server");
    assert_eq!(
        client
            .request("session.attach", json!({"session_id":session}))
            .unwrap()["provider_id"],
        "llama-cpp-server"
    );
    drop(client);
    daemon.shutdown(&profile);

    let daemon = Daemon::start_with_kind(&profile, &provider_compat, "openai-compatible-local");
    let mut client = profile.connect();
    let attached = client
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(attached["provider_id"], "llama-cpp-server");
    let third_fixture = provider_compat.serve_once(completed_stream("answer from compatible"));
    send_and_finish(&mut client, &session, "question for compatible");
    third_fixture.join().unwrap();
    let rows = message_rows(&mut client, &session);
    assert_eq!(rows[1]["id"], 2);
    assert_eq!(rows[1]["provider_id"], "lm-studio");
    assert_eq!(rows[3]["id"], 4);
    assert_eq!(rows[3]["provider_id"], "llama-cpp-server");
    assert_eq!(rows[5]["id"], 6);
    assert_eq!(rows[5]["provider_id"], "openai-compatible-local");
    assert_eq!(
        client
            .request("session.attach", json!({"session_id":session}))
            .unwrap()["provider_id"],
        "openai-compatible-local"
    );
    drop(client);
    daemon.shutdown(&profile);
}

#[test]
fn ollama_ndjson_history_failure_cancel_and_persistence() {
    let profile = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon::start_with_kind(&profile, &provider, "ollama");
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let first = provider.serve_ollama_once(ollama_completed_stream("Ollama answer one"));
    send_and_finish(&mut client, &session, "Ollama question one");
    assert_eq!(
        request_messages(&first.join().unwrap()),
        vec![json!({"role":"user","content":"Ollama question one"})]
    );
    let first_rows = message_rows(&mut client, &session);
    assert_eq!(first_rows[1]["provider_id"], "ollama");
    assert_eq!(first_rows[1]["content"], "Ollama answer one");
    drop(client);
    daemon.shutdown(&profile);

    let daemon = Daemon::start_with_kind(&profile, &provider, "ollama");
    let mut client = profile.connect();
    let attached = client
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(attached["provider_id"], "ollama");
    assert_eq!(attached["model_id"], "fixture-model");

    let mut length_failure = ollama_delta("truncated Ollama answer", false);
    let mut terminal = serde_json::to_vec(&json!({
        "model":"fixture-model",
        "message":{"role":"assistant","content":""},
        "done":true,
        "done_reason":"length"
    }))
    .unwrap();
    terminal.push(b'\n');
    length_failure.extend_from_slice(&terminal);
    let failed_fixture = provider.serve_ollama_once(length_failure);
    let failed_run = client
        .request(
            "chat.send",
            json!({"session_id":session,"text":"Ollama failed question"}),
        )
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(
        wait_for_terminal(&mut client, &failed_run).name,
        "run.failed"
    );
    failed_fixture.join().unwrap();

    let (cancel_fixture, accepted) = provider.serve_ollama_partial_until_disconnect();
    let cancelled_run = client
        .request(
            "chat.send",
            json!({"session_id":session,"text":"Ollama cancelled question"}),
        )
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    accepted.recv_timeout(Duration::from_secs(3)).unwrap();
    assert_eq!(
        client
            .request("run.cancel", json!({"run_id":cancelled_run}))
            .unwrap()["accepted"],
        true
    );
    assert_eq!(
        wait_for_terminal(&mut client, &cancelled_run).name,
        "run.cancelled"
    );
    cancel_fixture.join().unwrap();

    let final_fixture = provider.serve_ollama_once(ollama_completed_stream("Ollama answer two"));
    send_and_finish(&mut client, &session, "Ollama question two");
    assert_eq!(
        request_messages(&final_fixture.join().unwrap()),
        vec![
            json!({"role":"user","content":"Ollama question one"}),
            json!({"role":"assistant","content":"Ollama answer one"}),
            json!({"role":"user","content":"Ollama question two"})
        ]
    );
    let rows = message_rows(&mut client, &session);
    assert_eq!(rows[1]["provider_id"], "ollama");
    assert_eq!(rows[3]["status"], "failed");
    assert_eq!(rows[3]["provider_id"], "ollama");
    assert_eq!(rows[5]["status"], "cancelled");
    assert_eq!(rows[5]["provider_id"], "ollama");
    assert_eq!(rows[7]["provider_id"], "ollama");
    drop(client);
    daemon.shutdown(&profile);
}

#[test]
fn context_read_busy_is_retryable_without_dispatch_or_write() {
    let profile = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon::start(&profile, &provider);
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let lock = Connection::open(profile.0.join("daemon-dev-chat.sqlite3")).unwrap();
    lock.execute_batch("BEGIN EXCLUSIVE;").unwrap();

    assert!(matches!(
        client.request(
            "chat.send",
            json!({"session_id":session,"text":"retry after lock"})
        ),
        Err(IpcError::Remote(code)) if code == "storage-failure"
    ));
    provider.assert_no_pending_request();
    lock.execute_batch("ROLLBACK;").unwrap();

    let fixture = provider.serve_once(completed_stream("retry answer"));
    send_and_finish(&mut client, &session, "retry after lock");
    assert_eq!(
        request_messages(&fixture.join().unwrap()),
        vec![json!({"role":"user","content":"retry after lock"})]
    );
    let rows = message_rows(&mut client, &session);
    assert_eq!(rows.len(), 2);
    assert_eq!(rows[0]["content"], "retry after lock");
    drop(lock);
    drop(client);
    daemon.shutdown(&profile);
}

#[test]
fn required_agent_approval_is_durable_scoped_and_cancellable() {
    let profile = Profile::new();
    let workspace = Profile::new();
    fs::write(workspace.0.join("note.txt"), "approval evidence").unwrap();
    let provider = Provider::new();
    let mut command = profile.command(&provider);
    command
        .args(["--agent-approval", "required", "--agent-workspace"])
        .arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut owner = profile.connect();
    let session = create_session(&mut owner);
    let task = json!({"operation":"inspect-file","path":"note.txt"}).to_string();
    let tool = json!({"action":"tool","tool":"read-file","argsJson":"{\"path\":\"note.txt\"}"})
        .to_string();
    let first = provider.serve_once(completed_stream(&tool));
    let run = owner
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    first.join().unwrap();
    let approval = loop {
        let event = owner.next_event().unwrap();
        if event.name == "approval.requested" {
            break event;
        }
    };
    assert_eq!(approval.payload["run_id"], run);
    assert_eq!(approval.payload["tool"], "read-file");
    let id = approval.payload["approval_id"].as_str().unwrap().to_owned();
    let audit = Connection::open(profile.0.join("daemon-dev-agent.sqlite3")).unwrap();
    let decision: String = audit
        .query_row("SELECT decision FROM approvals WHERE id=?1", [&id], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(decision, "pending");
    let content: String = audit
        .query_row(
            "SELECT content FROM observations WHERE run_id=?1",
            [&run],
            |r| r.get(0),
        )
        .unwrap();
    assert!(content.is_empty());
    assert!(
        owner
            .request(
                "approval.respond",
                json!({"run_id":run,"approval_id":"wrong","allow":true})
            )
            .is_err()
    );
    provider.assert_no_pending_request();
    let mut second = profile.connect();
    let snapshot = second
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(snapshot["state"], "approval");
    let replay = loop {
        let event = second.next_event().unwrap();
        if event.name == "approval.requested" {
            break event;
        }
    };
    assert_eq!(replay.payload["approval_id"], id);
    let final_decision=json!({"action":"final","grounding":"verified","claims":[{"id":"read-1","assertion":true}]}).to_string();
    let second_provider = provider.serve_once(completed_stream(&final_decision));
    assert_eq!(
        second
            .request(
                "approval.respond",
                json!({"run_id":run,"approval_id":id,"allow":true})
            )
            .unwrap()["accepted"],
        true
    );
    assert!(
        second
            .request(
                "approval.respond",
                json!({"run_id":run,"approval_id":id,"allow":true})
            )
            .is_err()
    );
    assert_eq!(wait_for_terminal(&mut owner, &run).name, "run.completed");
    second_provider.join().unwrap();
    let decision: String = audit
        .query_row("SELECT decision FROM approvals WHERE id=?1", [&id], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(decision, "allow");
    let next = provider.serve_once(completed_stream(&tool));
    let task = json!({"operation":"inspect-file","path":"note.txt"}).to_string();
    let cancelled = owner
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    next.join().unwrap();
    let approval = loop {
        let event = owner.next_event().unwrap();
        if event.name == "approval.requested" && event.payload["run_id"] == cancelled {
            break event;
        }
    };
    let cancelled_id = approval.payload["approval_id"].as_str().unwrap().to_owned();
    assert_eq!(
        second
            .request("run.cancel", json!({"run_id":cancelled}))
            .unwrap()["accepted"],
        true
    );
    assert_eq!(
        wait_for_terminal(&mut owner, &cancelled).name,
        "run.cancelled"
    );
    assert!(
        second
            .request(
                "approval.respond",
                json!({"run_id":cancelled,"approval_id":cancelled_id,"allow":true})
            )
            .is_err()
    );
    let decision: String = audit
        .query_row(
            "SELECT decision FROM approvals WHERE id=?1",
            [&cancelled_id],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(decision, "cancelled");
    provider.assert_no_pending_request();
    let denied_provider = provider.serve_once(completed_stream(&tool));
    let task = json!({"operation":"inspect-file","path":"note.txt"}).to_string();
    let denied = owner
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    denied_provider.join().unwrap();
    let approval = loop {
        let event = owner.next_event().unwrap();
        if event.name == "approval.requested" && event.payload["run_id"] == denied {
            break event;
        }
    };
    let denied_id = approval.payload["approval_id"].as_str().unwrap().to_owned();
    let denied_followup = provider.serve_once(completed_stream(
        &json!({"action":"giveup","reason":"approval denied"}).to_string(),
    ));
    assert_eq!(
        second
            .request(
                "approval.respond",
                json!({"run_id":denied,"approval_id":denied_id,"allow":false})
            )
            .unwrap()["accepted"],
        true
    );
    assert_eq!(wait_for_terminal(&mut owner, &denied).name, "run.failed");
    assert!(
        String::from_utf8(denied_followup.join().unwrap())
            .unwrap()
            .contains("user denied")
    );
    let decision: String = audit
        .query_row(
            "SELECT decision FROM approvals WHERE id=?1",
            [&denied_id],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(decision, "deny");
    provider.assert_no_pending_request();
    drop(audit);
    daemon.shutdown(&profile);
}

#[test]
fn create_file_agent_requires_approval_and_never_overwrites() {
    let profile = Profile::new();
    let workspace = Profile::new();
    let provider = Provider::new();
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let task =
        json!({"operation":"create-file","path":"new.txt","content":"approved text"}).to_string();
    let decision=json!({"action":"tool","tool":"write-file","argsJson":json!({"path":"new.txt","content":"approved text"}).to_string()}).to_string();
    let first = provider.serve_once(completed_stream(&decision));
    let run = client
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    first.join().unwrap();
    let approval = loop {
        let event = client.next_event().unwrap();
        if event.name == "approval.requested" && event.payload["run_id"] == run {
            break event;
        }
    };
    assert!(!workspace.0.join("new.txt").exists());
    assert_eq!(approval.payload["tool"], "write-file");
    assert_eq!(approval.payload["risk"], 2);
    assert!(
        approval.payload["detail"]
            .as_str()
            .unwrap()
            .contains("approved text")
    );
    let id = approval.payload["approval_id"].as_str().unwrap();
    let final_decision=json!({"action":"final","grounding":"verified","claims":[{"id":"read-2","assertion":true}]}).to_string();
    let next = provider.serve_once(completed_stream(&final_decision));
    assert_eq!(
        client
            .request(
                "approval.respond",
                json!({"run_id":run,"approval_id":id,"allow":true})
            )
            .unwrap()["accepted"],
        true
    );
    let terminal = wait_for_terminal(&mut client, &run);
    next.join().unwrap();
    assert_eq!(terminal.name, "run.completed");
    assert_eq!(
        fs::read_to_string(workspace.0.join("new.txt")).unwrap(),
        "approved text"
    );
    let audit = Connection::open(profile.0.join("daemon-dev-agent.sqlite3")).unwrap();
    let tools: Vec<String> = audit
        .prepare("SELECT tool FROM observations WHERE run_id=?1 ORDER BY step")
        .unwrap()
        .query_map([&run], |r| r.get(0))
        .unwrap()
        .map(Result::unwrap)
        .collect();
    assert_eq!(tools, vec!["write-file", "read-file"]);
    let existing = provider.serve_once(completed_stream(&decision));
    let task =
        json!({"operation":"create-file","path":"new.txt","content":"approved text"}).to_string();
    let run = client
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    existing.join().unwrap();
    let approval = loop {
        let event = client.next_event().unwrap();
        if event.name == "approval.requested" && event.payload["run_id"] == run {
            break event;
        }
    };
    client
        .request(
            "approval.respond",
            json!({"run_id":run,"approval_id":approval.payload["approval_id"],"allow":true}),
        )
        .unwrap();
    assert_eq!(wait_for_terminal(&mut client, &run).name, "run.failed");
    assert_eq!(
        fs::read_to_string(workspace.0.join("new.txt")).unwrap(),
        "approved text"
    );
    drop(audit);
    daemon.shutdown(&profile);
}

#[test]
fn session_memory_action_is_scoped_and_durable() {
    let profile = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon(profile.command(&provider).spawn().unwrap());
    let mut client = profile.connect();
    let a = create_session(&mut client);
    let b = create_session(&mut client);
    let call = |client: &mut Client, sid: &str, request: Value| {
        client
            .request(
                "desktop.action",
                json!({"action":"manageMemory","session_id":sid,"arguments":[request]}),
            )
            .unwrap()["result"]["value"]
            .clone()
    };
    assert_eq!(
        call(
            &mut client,
            &a,
            json!({"operation":"create","scope":1,"session":a,"workspace":"","key":"name","value":"kept"})
        )["status"],
        "Ready"
    );
    let rows = call(&mut client, &a, json!({"operation":"list","scope":1}));
    let id = rows["records"][0]["id"].as_str().unwrap().to_owned();
    assert_eq!(
        call(
            &mut client,
            &b,
            json!({"operation":"read","scope":1,"id":id})
        )["status"],
        "NotFound"
    );
    assert_eq!(
        call(
            &mut client,
            &a,
            json!({"operation":"correct","scope":1,"session":a,"workspace":"","id":id,"revision":"99","key":"name","value":"wrong"})
        )["status"],
        "Conflict"
    );
    daemon.shutdown(&profile);
    let daemon = Daemon(profile.command(&provider).spawn().unwrap());
    let mut client = profile.connect();
    assert_eq!(
        call(
            &mut client,
            &a,
            json!({"operation":"read","scope":1,"id":id})
        )["record"]["value"],
        "kept"
    );
    assert_eq!(
        call(
            &mut client,
            &a,
            json!({"operation":"delete","scope":1,"session":a,"workspace":"","id":id,"revision":"1","confirmed":true,"confirmation":"DELETE"})
        )["status"],
        "Ready"
    );
    daemon.shutdown(&profile);
}

#[test]
fn regenerate_and_retry_preserve_rows_and_replace_context() {
    let profile = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon(profile.command(&provider).spawn().unwrap());
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let first = provider.serve_once(completed_stream("original answer"));
    send_and_finish(&mut client, &session, "question one");
    first.join().unwrap();
    let rows = message_rows(&mut client, &session);
    let user = rows[0]["id"].as_u64().unwrap();
    let original = rows[1]["id"].as_u64().unwrap();
    let regenerate = provider.serve_once(completed_stream("new answer"));
    let run = client
        .request(
            "chat.regenerate",
            json!({"session_id":session,"message_id":user}),
        )
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(wait_for_terminal(&mut client, &run).name, "run.completed");
    regenerate.join().unwrap();
    let rows = message_rows(&mut client, &session);
    assert_eq!(rows.len(), 3);
    assert_eq!(rows[1]["content"], "original answer");
    assert_eq!(rows[2]["reply_to"], user);
    assert_eq!(rows[2]["replaces"], original);
    assert_eq!(rows[2]["content"], "new answer");
    let failed = provider.serve_once(b"data: [DONE]\n\n".to_vec());
    let run = client
        .request(
            "chat.send",
            json!({"session_id":session,"text":"question two"}),
        )
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(wait_for_terminal(&mut client, &run).name, "run.failed");
    let sent = request_messages(&failed.join().unwrap());
    assert!(sent.iter().any(|m| m["content"] == "new answer"));
    assert!(!sent.iter().any(|m| m["content"] == "original answer"));
    let rows = message_rows(&mut client, &session);
    let user2 = rows[3]["id"].as_u64().unwrap();
    let failed_id = rows[4]["id"].as_u64().unwrap();
    let retry = provider.serve_once(completed_stream("retry answer"));
    let run = client
        .request(
            "chat.retry",
            json!({"session_id":session,"message_id":failed_id}),
        )
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(wait_for_terminal(&mut client, &run).name, "run.completed");
    retry.join().unwrap();
    let rows = message_rows(&mut client, &session);
    assert_eq!(rows.len(), 6);
    assert_eq!(rows[5]["reply_to"], user2);
    assert_eq!(rows[5]["replaces"], failed_id);
    assert_eq!(rows[5]["content"], "retry answer");
    let edit = provider.serve_once(completed_stream("edited answer"));
    let response = client
        .request(
            "chat.edit",
            json!({"session_id":session,"message_id":user2,"text":"revised question"}),
        )
        .unwrap();
    let run = response["run_id"].as_str().unwrap().to_owned();
    let branch = response["session_id"].as_str().unwrap().to_owned();
    assert_ne!(branch, session);
    assert_eq!(wait_for_terminal(&mut client, &run).name, "run.completed");
    let sent = request_messages(&edit.join().unwrap());
    assert!(sent.iter().any(|m| m["content"] == "new answer"));
    assert!(
        !sent
            .iter()
            .any(|m| m["content"] == "question two" || m["content"] == "retry answer")
    );
    let original_rows = message_rows(&mut client, &session);
    assert_eq!(original_rows.len(), 6);
    assert_eq!(original_rows[3]["content"], "question two");
    let branch_rows = message_rows(&mut client, &branch);
    assert_eq!(branch_rows.len(), 5);
    assert_eq!(branch_rows[3]["id"], user2);
    assert_eq!(branch_rows[3]["content"], "revised question");
    assert_eq!(branch_rows[4]["reply_to"], user2);
    let next = provider.serve_once(completed_stream("subsequent"));
    send_and_finish(&mut client, &branch, "question three");
    let sent = request_messages(&next.join().unwrap());
    assert!(sent.iter().any(|m| m["content"] == "revised question"));
    assert!(sent.iter().any(|m| m["content"] == "edited answer"));
    assert!(
        !sent
            .iter()
            .any(|m| m["content"] == "question two" || m["content"] == "retry answer")
    );
    let historical = provider.serve_once(completed_stream("historical branch"));
    let response = client
        .request(
            "chat.regenerate",
            json!({"session_id":session,"message_id":user}),
        )
        .unwrap();
    let historical_branch = response["session_id"].as_str().unwrap().to_owned();
    assert_ne!(historical_branch, session);
    let historical_run = response["run_id"].as_str().unwrap().to_owned();
    assert_eq!(
        wait_for_terminal(&mut client, &historical_run).name,
        "run.completed"
    );
    let sent = request_messages(&historical.join().unwrap());
    assert_eq!(sent, vec![json!({"role":"user","content":"question one"})]);
    let history = message_rows(&mut client, &historical_branch);
    assert_eq!(history.len(), 2);
    assert_eq!(history[0]["id"], user);
    assert_eq!(history[1]["content"], "historical branch");
    assert_eq!(message_rows(&mut client, &session).len(), 6);
    let later = provider.serve_once(completed_stream("later source answer"));
    send_and_finish(&mut client, &session, "later source question");
    later.join().unwrap();
    let retry_historical = provider.serve_once(completed_stream("historical retry answer"));
    let response = client
        .request(
            "chat.retry",
            json!({"session_id":session,"message_id":failed_id}),
        )
        .unwrap();
    let retry_branch = response["session_id"].as_str().unwrap().to_owned();
    assert_ne!(retry_branch, session);
    let retry_run = response["run_id"].as_str().unwrap().to_owned();
    assert_eq!(
        wait_for_terminal(&mut client, &retry_run).name,
        "run.completed"
    );
    let sent = request_messages(&retry_historical.join().unwrap());
    assert!(sent.iter().any(|m| m["content"] == "question two"));
    assert!(
        !sent
            .iter()
            .any(|m| m["content"] == "later source question"
                || m["content"] == "later source answer")
    );
    assert_eq!(
        message_rows(&mut client, &retry_branch).last().unwrap()["content"],
        "historical retry answer"
    );
    assert_eq!(message_rows(&mut client, &session).len(), 8);
    drop(client);
    daemon.shutdown(&profile);
    let daemon = Daemon(profile.command(&provider).spawn().unwrap());
    let mut client = profile.connect();
    client
        .request("session.attach", json!({"session_id":retry_branch}))
        .unwrap();
    assert_eq!(
        message_rows(&mut client, &retry_branch).last().unwrap()["content"],
        "historical retry answer"
    );
    assert_eq!(message_rows(&mut client, &session).len(), 8);
    drop(client);
    daemon.shutdown(&profile);
}

#[cfg(target_os = "macos")]
#[test]
fn agent_process_requires_exact_argv_approval_and_reports_exit() {
    let profile = Profile::new();
    let workspace = Profile::new();
    let provider = Provider::new();
    fs::write(workspace.0.join("input"), "abc").unwrap();
    let mut command = profile.command(&provider);
    command
        .arg("--agent-workspace")
        .arg(&workspace.0)
        .arg("--agent-process");
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let argv = json!(["/usr/bin/wc", "-c", "input"]);
    let task = json!({"operation":"run-process","argv":argv}).to_string();
    let decision =
        json!({"action":"tool","tool":"process-exec","argsJson":json!({"argv":argv}).to_string()})
            .to_string();
    let first = provider.serve_once(completed_stream(&decision));
    let run = client
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    first.join().unwrap();
    let approval = loop {
        let event = client.next_event().unwrap();
        if event.name == "approval.requested" && event.payload["run_id"] == run {
            break event;
        }
    };
    assert_eq!(approval.payload["tool"], "process-exec");
    assert_eq!(approval.payload["risk"], 2);
    assert_eq!(approval.payload["resources"][0]["argv"], argv);
    let final_decision=json!({"action":"final","grounding":"verified","claims":[{"id":"process-1","assertion":true}]}).to_string();
    let next = provider.serve_once(completed_stream(&final_decision));
    assert_eq!(
        client
            .request(
                "approval.respond",
                json!({"run_id":run,"approval_id":approval.payload["approval_id"],"allow":true})
            )
            .unwrap()["accepted"],
        true
    );
    let terminal = wait_for_terminal(&mut client, &run);
    next.join().unwrap();
    assert_eq!(terminal.name, "run.completed");
    assert!(
        terminal.payload["text"]
            .as_str()
            .unwrap()
            .contains("Process exit code: 0")
    );
    assert!(terminal.payload["text"].as_str().unwrap().contains('3'));
    daemon.shutdown(&profile);
}

#[test]
fn session_delete_removes_all_three_stores_atomically() {
    let profile = Profile::new();
    let workspace = Profile::new();
    let provider = Provider::new();
    fs::write(workspace.0.join("note.txt"), "evidence").unwrap();
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let other = create_session(&mut client);
    let action = |client: &mut Client, action: &str, sid: &str, arguments: Value| {
        client.request(
            "desktop.action",
            json!({"action":action,"session_id":sid,"arguments":arguments}),
        )
    };
    assert_eq!(
        action(
            &mut client,
            "renameConversation",
            &session,
            json!([session, "Renamed"])
        )
        .unwrap()["accepted"],
        true
    );
    assert!(
        client.request("session.list", json!({})).unwrap()["sessions"]
            .as_array()
            .unwrap()
            .iter()
            .any(|s| s["session_id"] == session && s["title"] == "Renamed")
    );
    let memory = json!({"operation":"create","scope":1,"session":session,"workspace":"","key":"k","value":"v"});
    assert_eq!(
        action(&mut client, "manageMemory", &session, json!([memory])).unwrap()["result"]["value"]
            ["status"],
        "Ready"
    );
    let tool = json!({"action":"tool","tool":"read-file","argsJson":"{\"path\":\"note.txt\"}"})
        .to_string();
    let first = provider.serve_once(completed_stream(&tool));
    let task = json!({"operation":"inspect-file","path":"note.txt"}).to_string();
    let run = client
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    first.join().unwrap();
    let final_decision=json!({"action":"final","grounding":"verified","claims":[{"id":"read-1","assertion":true}]}).to_string();
    let next = provider.serve_once(completed_stream(&final_decision));
    assert_eq!(wait_for_terminal(&mut client, &run).name, "run.completed");
    next.join().unwrap();
    // Abort inside the second attached store after the Agent deletes have executed.
    let injector = Connection::open(profile.0.join("daemon-dev-memory.sqlite3")).unwrap();
    injector.execute_batch("CREATE TRIGGER fail_delete BEFORE DELETE ON entries BEGIN SELECT RAISE(ABORT,'injected'); END;").unwrap();
    assert!(
        action(
            &mut client,
            "requestPermanentDeleteConversation",
            &session,
            json!([session])
        )
        .is_err()
    );
    for (file, sql) in [
        (
            "daemon-dev-chat.sqlite3",
            "SELECT count(*) FROM sessions WHERE id=?1",
        ),
        (
            "daemon-dev-agent.sqlite3",
            "SELECT count(*) FROM runs WHERE session_id=?1",
        ),
        (
            "daemon-dev-memory.sqlite3",
            "SELECT count(*) FROM entries WHERE session=?1",
        ),
    ] {
        let db = Connection::open(profile.0.join(file)).unwrap();
        let count: i64 = db.query_row(sql, [&session], |r| r.get(0)).unwrap();
        assert!(count > 0, "rollback failed in {file}");
    }
    injector.execute_batch("DROP TRIGGER fail_delete").unwrap();
    drop(injector);
    daemon.shutdown(&profile);
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    assert_eq!(
        action(
            &mut client,
            "requestPermanentDeleteConversation",
            &session,
            json!([session])
        )
        .unwrap()["accepted"],
        true
    );
    for (file, sql) in [
        (
            "daemon-dev-chat.sqlite3",
            "SELECT count(*) FROM sessions WHERE id=?1",
        ),
        (
            "daemon-dev-agent.sqlite3",
            "SELECT count(*) FROM runs WHERE session_id=?1",
        ),
        (
            "daemon-dev-memory.sqlite3",
            "SELECT count(*) FROM entries WHERE session=?1",
        ),
    ] {
        let db = Connection::open(profile.0.join(file)).unwrap();
        let count: i64 = db.query_row(sql, [&session], |r| r.get(0)).unwrap();
        assert_eq!(count, 0, "{file}");
    }
    assert!(
        client
            .request("session.attach", json!({"session_id":session}))
            .is_err()
    );
    assert!(
        client
            .request("session.attach", json!({"session_id":other}))
            .is_ok()
    );
    daemon.shutdown(&profile);
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    assert!(
        client
            .request("session.attach", json!({"session_id":session}))
            .is_err()
    );
    assert!(
        client
            .request("session.attach", json!({"session_id":other}))
            .is_ok()
    );
    daemon.shutdown(&profile);
}

#[test]
fn qt_memory_copy_import_cli_is_one_shot_and_preserves_source() {
    use std::os::unix::fs::PermissionsExt;
    let profile = Profile::new();
    let source = Profile::new();
    let provider = Provider::new();
    let daemon = Daemon(profile.command(&provider).spawn().unwrap());
    let mut client = profile.connect();
    let session = create_session(&mut client);
    daemon.shutdown(&profile);
    let path = source.0.join("copy.sqlite3");
    let db = Connection::open(&path).unwrap();
    fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
    db.execute_batch("CREATE TABLE memory_schema_metadata(key TEXT PRIMARY KEY,value INTEGER NOT NULL); INSERT INTO memory_schema_metadata VALUES('schema_version',7); CREATE TABLE memory_entries(id INTEGER PRIMARY KEY,key TEXT,value TEXT); CREATE TABLE scoped_memory_entries(id TEXT PRIMARY KEY,scope INTEGER,owner_id TEXT,workspace_id TEXT,key TEXT,value TEXT,provenance INTEGER,source_ref TEXT,created_at TEXT,updated_at TEXT,revision TEXT,corrections TEXT);").unwrap();
    let id = "8fb66a15-7cb8-4779-892e-d46d7ff026ec";
    db.execute("INSERT INTO scoped_memory_entries VALUES(?1,1,?2,'','key','value',2,'source','2026-01-01T12:00:00.000Z','2026-01-01T12:00:01.000Z','revision','[]')",rusqlite::params![id,session]).unwrap();
    drop(db);
    let before = fs::read(&path).unwrap();
    let run = || {
        Command::new(env!("CARGO_BIN_EXE_sentinel-daemon-dev"))
            .arg("--profile")
            .arg(&profile.0)
            .arg("--import-qt-memory-copy")
            .arg(&path)
            .output()
            .unwrap()
    };
    assert!(run().status.success());
    assert!(!run().status.success());
    assert_eq!(fs::read(&path).unwrap(), before);
    let daemon = Daemon(profile.command(&provider).spawn().unwrap());
    let mut client = profile.connect();
    let result=client.request("desktop.action",json!({"action":"manageMemory","session_id":session,"arguments":[{"operation":"read","scope":1,"id":id}]})).unwrap();
    assert_eq!(result["result"]["value"]["record"]["value"], "value");
    assert_eq!(result["result"]["value"]["record"]["revision"], "revision");
    assert_eq!(client.request("desktop.action",json!({"action":"requestPermanentDeleteConversation","session_id":session,"arguments":[session]})).unwrap()["accepted"],true);
    let db = Connection::open(profile.0.join("daemon-dev-memory.sqlite3")).unwrap();
    assert_eq!(
        db.query_row("SELECT count(*) FROM qt_imports", [], |r| r
            .get::<_, i64>(0))
            .unwrap(),
        0
    );
    drop(db);
    daemon.shutdown(&profile);
}

#[test]
fn classified_inspection_requires_approval_and_labels_scope() {
    let profile = Profile::new();
    let workspace = Profile::new();
    fs::write(workspace.0.join("note.txt"), "scoped evidence").unwrap();
    let provider = Provider::new();
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut owner = profile.connect();
    let session = create_session(&mut owner);
    let classified = json!({"requirements":[{"domain":"filesystem","resource":"note.txt","purpose":"inspect","operation":"none","query":"","recursive":false,"tool":"read-file"}]}).to_string();
    let classifier = provider.serve_once(completed_stream(&classified));
    let task = json!({"operation":"classify-inspect","goal":"Inspect note.txt"}).to_string();
    let run = owner
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    classifier.join().unwrap();
    let decision = json!({"action":"tool","tool":"read-file","argsJson":"{\"path\":\"note.txt\"}"})
        .to_string();
    let planner = provider.serve_once(completed_stream(&decision));
    planner.join().unwrap();
    let approval = loop {
        let event = owner.next_event().unwrap();
        if event.name == "approval.requested" && event.payload["run_id"] == run {
            break event;
        }
    };
    assert_eq!(approval.payload["tool"], "read-file");
    let final_decision = json!({"action":"final","grounding":"verified","claims":[{"id":"read-1","assertion":true}]}).to_string();
    let final_provider = provider.serve_once(completed_stream(&final_decision));
    let mut responder = profile.connect();
    responder
        .request(
            "approval.respond",
            json!({"run_id":run,"approval_id":approval.payload["approval_id"],"allow":true}),
        )
        .unwrap();
    let terminal = wait_for_terminal(&mut owner, &run);
    assert_eq!(terminal.name, "run.completed");
    assert!(
        terminal.payload["text"]
            .as_str()
            .unwrap()
            .contains("The original natural-language goal was not assessed")
    );
    assert!(
        terminal.payload["text"]
            .as_str()
            .unwrap()
            .contains("scoped evidence")
    );
    final_provider.join().unwrap();
    drop(owner);
    drop(responder);
    daemon.shutdown(&profile);
}

#[test]
fn rejected_agent_final_can_recover_without_replaying_tool() {
    let profile = Profile::new();
    let workspace = Profile::new();
    fs::write(workspace.0.join("note.txt"), "one observation").unwrap();
    let provider = Provider::new();
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut owner = profile.connect();
    let session = create_session(&mut owner);
    let tool = json!({"action":"tool","tool":"read-file","argsJson":"{\"path\":\"note.txt\"}"})
        .to_string();
    let first = provider.serve_once(completed_stream(&tool));
    let task = json!({"operation":"inspect-file","path":"note.txt"}).to_string();
    let run = owner
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    first.join().unwrap();
    let bad =
        json!({"action":"final","grounding":"verified","claims":[{"id":"wrong","assertion":true}]})
            .to_string();
    let second = provider.serve_once(completed_stream(&bad));
    second.join().unwrap();
    let good = json!({"action":"final","grounding":"verified","claims":[{"id":"read-1","assertion":true}]}).to_string();
    let third = provider.serve_once(completed_stream(&good));
    assert_eq!(wait_for_terminal(&mut owner, &run).name, "run.completed");
    let request = third.join().unwrap();
    assert!(
        String::from_utf8(request)
            .unwrap()
            .contains("Final rejected")
    );
    let audit = Connection::open(profile.0.join("daemon-dev-agent.sqlite3")).unwrap();
    let count: i64 = audit
        .query_row(
            "SELECT COUNT(*) FROM observations WHERE run_id=?1",
            [&run],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 1);
    drop(audit);
    drop(owner);
    daemon.shutdown(&profile);
}

#[test]
fn agent_sequential_batch_respects_scope_and_dependencies() {
    let profile = Profile::new();
    let workspace = Profile::new();
    fs::write(workspace.0.join("a.txt"), "first").unwrap();
    fs::write(workspace.0.join("b.txt"), "second").unwrap();
    let provider = Provider::new();
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut owner = profile.connect();
    let session = create_session(&mut owner);
    let batch = json!({"action":"tool_batch","calls":[{"tool":"read-file","args":{"path":"a.txt"}},{"tool":"read-file","args":{"path":"b.txt"},"dependsOn":[0]}]}).to_string();
    let first = provider.serve_once(completed_stream(&batch));
    let task = json!({"operation":"inspect-files","paths":["a.txt","b.txt"]}).to_string();
    let run = owner
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    first.join().unwrap();
    let final_decision = json!({"action":"final","grounding":"verified","claims":[{"id":"read-1","assertion":true},{"id":"read-2","assertion":true}]}).to_string();
    let last = provider.serve_once(completed_stream(&final_decision));
    let terminal = wait_for_terminal(&mut owner, &run);
    assert_eq!(terminal.name, "run.completed");
    let output = terminal.payload["text"].as_str().unwrap();
    assert!(output.contains("first") && output.contains("second"));
    last.join().unwrap();
    let audit = Connection::open(profile.0.join("daemon-dev-agent.sqlite3")).unwrap();
    let count: i64 = audit
        .query_row(
            "SELECT COUNT(*) FROM observations WHERE run_id=?1 AND complete=1",
            [&run],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 2);
    drop(audit);
    drop(owner);
    daemon.shutdown(&profile);
}

#[test]
fn agent_batch_denial_blocks_dependents_but_allows_independent_reads() {
    let profile = Profile::new();
    let workspace = Profile::new();
    for name in ["a", "b", "c"] {
        fs::write(workspace.0.join(name), name).unwrap();
    }
    let provider = Provider::new();
    let mut command = profile.command(&provider);
    command
        .args(["--agent-approval", "required", "--agent-workspace"])
        .arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut owner = profile.connect();
    let session = create_session(&mut owner);
    let batch = json!({"action":"tool_batch","calls":[{"tool":"read-file","args":{"path":"a"}},{"tool":"read-file","args":{"path":"b"},"dependsOn":[0]},{"tool":"read-file","args":{"path":"c"}}]}).to_string();
    let first = provider.serve_once(completed_stream(&batch));
    let task = json!({"operation":"inspect-files","paths":["a","b","c"]}).to_string();
    let run = owner
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    first.join().unwrap();
    let mut responder = profile.connect();
    let first_approval = loop {
        let event = owner.next_event().unwrap();
        if event.name == "approval.requested" && event.payload["run_id"] == run {
            break event;
        }
    };
    assert_eq!(first_approval.payload["resources"][0]["path"], "a");
    responder
        .request(
            "approval.respond",
            json!({"run_id":run,"approval_id":first_approval.payload["approval_id"],"allow":false}),
        )
        .unwrap();
    let second_approval = loop {
        let event = owner.next_event().unwrap();
        if event.name == "approval.requested" && event.payload["run_id"] == run {
            break event;
        }
    };
    assert_eq!(second_approval.payload["resources"][0]["path"], "c");
    let final_provider = provider.serve_once(completed_stream(
        &json!({"action":"giveup","reason":"approval denied"}).to_string(),
    ));
    responder
        .request(
            "approval.respond",
            json!({"run_id":run,"approval_id":second_approval.payload["approval_id"],"allow":true}),
        )
        .unwrap();
    assert_eq!(wait_for_terminal(&mut owner, &run).name, "run.failed");
    assert!(
        String::from_utf8(final_provider.join().unwrap())
            .unwrap()
            .contains("user denied")
    );
    let audit = Connection::open(profile.0.join("daemon-dev-agent.sqlite3")).unwrap();
    let rows: Vec<i64> = audit
        .prepare("SELECT step FROM observations WHERE run_id=?1 ORDER BY step")
        .unwrap()
        .query_map([&run], |r| r.get(0))
        .unwrap()
        .collect::<Result<_, _>>()
        .unwrap();
    assert_eq!(rows, vec![1, 3]);
    drop(audit);
    drop(owner);
    drop(responder);
    daemon.shutdown(&profile);
}

#[test]
fn blocked_out_of_scope_tool_can_recover_without_dispatch() {
    let profile = Profile::new();
    let workspace = Profile::new();
    fs::write(workspace.0.join("a"), "evidence").unwrap();
    fs::write(workspace.0.join("other"), "not authorized").unwrap();
    let provider = Provider::new();
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut owner = profile.connect();
    let session = create_session(&mut owner);
    let bad = json!({"action":"tool","tool":"read-file","args":{"path":"other"}}).to_string();
    let first = provider.serve_once(completed_stream(&bad));
    let task = json!({"operation":"inspect-file","path":"a"}).to_string();
    let run = owner
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    first.join().unwrap();
    let good = json!({"action":"tool","tool":"read-file","args":{"path":"a"}}).to_string();
    let second = provider.serve_once(completed_stream(&good));
    assert!(
        String::from_utf8(second.join().unwrap())
            .unwrap()
            .contains("Tool request blocked")
    );
    let final_decision=json!({"action":"final","grounding":"verified","claims":[{"id":"read-1","assertion":true}]}).to_string();
    let third = provider.serve_once(completed_stream(&final_decision));
    assert_eq!(wait_for_terminal(&mut owner, &run).name, "run.completed");
    third.join().unwrap();
    let audit = Connection::open(profile.0.join("daemon-dev-agent.sqlite3")).unwrap();
    let rows: Vec<String> = audit
        .prepare("SELECT path FROM observations WHERE run_id=?1")
        .unwrap()
        .query_map([&run], |r| r.get(0))
        .unwrap()
        .collect::<Result<_, _>>()
        .unwrap();
    assert_eq!(rows, vec!["a"]);
    drop(audit);
    drop(owner);
    daemon.shutdown(&profile);
}

#[test]
fn workspace_files_is_scoped_bounded_and_requires_preauthorization() {
    use std::os::unix::fs::symlink;
    let profile = Profile::new();
    let workspace = Profile::new();
    let outside = Profile::new();
    fs::create_dir(workspace.0.join("sub")).unwrap();
    fs::write(workspace.0.join("one.txt"), "one").unwrap();
    fs::write(workspace.0.join("sub/two.txt"), "two").unwrap();
    fs::write(workspace.0.join(".hidden"), "hidden").unwrap();
    fs::write(outside.0.join("secret"), "secret").unwrap();
    symlink(&outside.0, workspace.0.join("escape")).unwrap();
    let provider = Provider::new();
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    let session = create_session(&mut client);
    assert!(
        client
            .request("workspace.files", json!({"session_id":"missing"}))
            .is_err()
    );
    let response = client
        .request("workspace.files", json!({"session_id":session}))
        .unwrap();
    let root = response["root"].as_str().unwrap();
    let files = response["files"].as_array().unwrap();
    assert!(
        files
            .iter()
            .any(|p| p.as_str().unwrap().ends_with("/one.txt"))
    );
    assert!(
        files
            .iter()
            .any(|p| p.as_str().unwrap().ends_with("/sub/two.txt"))
    );
    assert!(
        !files
            .iter()
            .any(|p| p.as_str().unwrap().contains(".hidden")
                || p.as_str().unwrap().contains("secret"))
    );
    assert!(files.iter().all(|p| p.as_str().unwrap().starts_with(root)));
    // A skipped symlink makes this observation conservatively incomplete.
    assert_eq!(response["truncated"], true);
    drop(client);
    daemon.shutdown(&profile);
    let mut command = profile.command(&provider);
    command
        .args(["--agent-approval", "required", "--agent-workspace"])
        .arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    assert!(
        matches!(client.request("workspace.files",json!({"session_id":session})), Err(IpcError::Remote(code)) if code=="permission-denied")
    );
    drop(client);
    daemon.shutdown(&profile);
}

#[test]
fn workspace_changes_uses_run_baseline_and_never_infers_deletion() {
    let profile = Profile::new();
    let workspace = Profile::new();
    fs::write(workspace.0.join("a.txt"), "before\n").unwrap();
    fs::write(workspace.0.join("deleted.txt"), "old\n").unwrap();
    let provider = Provider::new();
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let other = create_session(&mut client);
    assert_eq!(
        client
            .request("workspace.changes", json!({"session_id":session}))
            .unwrap()["available"],
        false
    );
    let tool =
        json!({"action":"tool","tool":"read-file","argsJson":"{\"path\":\"a.txt\"}"}).to_string();
    let first = provider.serve_once(completed_stream(&tool));
    let task = json!({"operation":"inspect-file","path":"a.txt"}).to_string();
    let run = client
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    first.join().unwrap();
    let final_decision=json!({"action":"final","grounding":"verified","claims":[{"id":"read-1","assertion":true}]}).to_string();
    let last = provider.serve_once(completed_stream(&final_decision));
    assert_eq!(wait_for_terminal(&mut client, &run).name, "run.completed");
    last.join().unwrap();
    fs::write(workspace.0.join("a.txt"), "after\n").unwrap();
    fs::write(workspace.0.join("new.txt"), "new\n").unwrap();
    fs::remove_file(workspace.0.join("deleted.txt")).unwrap();
    let result = client
        .request("workspace.changes", json!({"session_id":session}))
        .unwrap();
    assert_eq!(result["available"], true);
    assert_eq!(result["truncated"], true);
    let files = result["files"].as_array().unwrap();
    assert!(
        files
            .iter()
            .any(|file| file["path"] == "a.txt" && file["kind"] == "modified")
    );
    assert!(
        files
            .iter()
            .any(|file| file["path"] == "new.txt" && file["kind"] == "created")
    );
    assert!(!files.iter().any(|file| file["path"] == "deleted.txt"));
    assert_eq!(
        client
            .request("workspace.changes", json!({"session_id":other}))
            .unwrap()["available"],
        false
    );
    drop(client);
    daemon.shutdown(&profile);
    let mut command = profile.command(&provider);
    command.arg("--agent-workspace").arg(&workspace.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    assert_eq!(
        client
            .request("workspace.changes", json!({"session_id":session}))
            .unwrap()["available"],
        false
    );
    drop(client);
    daemon.shutdown(&profile);
}

#[test]
fn configured_workspace_selection_is_bound_persistent_and_invalidates_baseline() {
    let profile = Profile::new();
    let first_root = Profile::new();
    let second_root = Profile::new();
    let other_root = Profile::new();
    fs::write(first_root.0.join("first.txt"), "first").unwrap();
    fs::write(second_root.0.join("second.txt"), "second").unwrap();
    let provider = Provider::new();
    let mut command = profile.command(&provider);
    command
        .arg("--agent-workspace")
        .arg(&first_root.0)
        .arg("--workspace")
        .arg("second")
        .arg(&second_root.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    let session = create_session(&mut client);
    let state = client.request("terminal.state", json!({})).unwrap();
    assert_eq!(state["workspace"]["id"], "default");
    assert_eq!(state["workspaces"].as_array().unwrap().len(), 2);
    let configured_root = state["workspaces"][1]["root"].as_str().unwrap();
    assert_eq!(
        client
            .request(
                "workspace.root",
                json!({"workspace_id":"second","path":configured_root})
            )
            .unwrap()["root"],
        configured_root
    );
    assert!(
        matches!(client.request("workspace.select",json!({"workspace_id":"missing"})), Err(IpcError::Remote(code)) if code=="unknown-workspace")
    );
    assert!(
        client
            .request(
                "workspace.root",
                json!({"workspace_id":"second","path":other_root.0.to_str().unwrap()})
            )
            .is_err()
    );
    let tool = json!({"action":"tool","tool":"read-file","argsJson":"{\"path\":\"first.txt\"}"})
        .to_string();
    let initial = provider.serve_once(completed_stream(&tool));
    let task = json!({"operation":"inspect-file","path":"first.txt"}).to_string();
    let run = client
        .request("agent.start", json!({"session_id":session,"text":task}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    initial.join().unwrap();
    assert!(
        matches!(client.request("workspace.select",json!({"workspace_id":"second"})), Err(IpcError::Remote(code)) if code=="runtime-busy")
    );
    let done=json!({"action":"final","grounding":"verified","claims":[{"id":"read-1","assertion":true}]}).to_string();
    let final_provider = provider.serve_once(completed_stream(&done));
    assert_eq!(wait_for_terminal(&mut client, &run).name, "run.completed");
    final_provider.join().unwrap();
    assert_eq!(
        client
            .request("workspace.changes", json!({"session_id":session}))
            .unwrap()["available"],
        true
    );
    let lock = Connection::open(profile.0.join("daemon-dev-settings.sqlite3")).unwrap();
    lock.execute_batch("BEGIN EXCLUSIVE;").unwrap();
    assert!(
        matches!(client.request("workspace.select",json!({"workspace_id":"second"})), Err(IpcError::Remote(code)) if code=="storage-failure")
    );
    assert_eq!(
        client.request("terminal.state", json!({})).unwrap()["workspace"]["id"],
        "default"
    );
    lock.execute_batch("ROLLBACK;").unwrap();
    drop(lock);
    assert_eq!(
        client
            .request("workspace.select", json!({"workspace_id":"second"}))
            .unwrap()["workspace_id"],
        "second"
    );
    assert_eq!(
        client
            .request("workspace.changes", json!({"session_id":session}))
            .unwrap()["available"],
        false
    );
    let files = client
        .request("workspace.files", json!({"session_id":session}))
        .unwrap();
    assert!(
        files["files"]
            .as_array()
            .unwrap()
            .iter()
            .any(|path| path.as_str().unwrap().ends_with("/second.txt"))
    );
    assert!(
        !files["files"]
            .as_array()
            .unwrap()
            .iter()
            .any(|path| path.as_str().unwrap().ends_with("/first.txt"))
    );
    drop(client);
    daemon.shutdown(&profile);
    let mut command = profile.command(&provider);
    command
        .arg("--agent-workspace")
        .arg(&first_root.0)
        .arg("--workspace")
        .arg("second")
        .arg(&second_root.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    assert_eq!(
        client.request("terminal.state", json!({})).unwrap()["workspace"]["id"],
        "second"
    );
    drop(client);
    daemon.shutdown(&profile);
    let mut command = profile.command(&provider);
    command
        .arg("--agent-workspace")
        .arg(&first_root.0)
        .arg("--workspace")
        .arg("second")
        .arg(&other_root.0);
    let daemon = Daemon(command.spawn().unwrap());
    let mut client = profile.connect();
    assert_eq!(
        client.request("terminal.state", json!({})).unwrap()["workspace"]["id"],
        "default"
    );
    client
        .request("workspace.select", json!({"workspace_id":"second"}))
        .unwrap();
    fs::rename(&other_root.0, profile.0.join("moved-workspace")).unwrap();
    assert!(
        matches!(client.request("workspace.files",json!({"session_id":session})), Err(IpcError::Remote(code)) if code == "workspace-root-rejected")
    );
    assert!(
        matches!(client.request("agent.start",json!({"session_id":session,"text":task})), Err(IpcError::Remote(code)) if code == "workspace-root-rejected")
    );
    drop(client);
    daemon.shutdown(&profile);
}
