#![cfg(any(target_os = "linux", target_os = "macos"))]
use sentinel_ipc::Client;
use serde_json::json;
use std::{
    fs,
    os::unix::{
        fs::{DirBuilderExt, PermissionsExt, symlink},
        net::UnixListener,
    },
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    sync::atomic::{AtomicUsize, Ordering},
    thread,
    time::{Duration, Instant},
};
static NEXT: AtomicUsize = AtomicUsize::new(0);
struct Profile(PathBuf);
impl Profile {
    fn new() -> Self {
        let path = Path::new("/tmp").join(format!(
            "sentinel-dev-test-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::DirBuilder::new().mode(0o700).create(&path).unwrap();
        Self(path)
    }
    fn socket(&self) -> PathBuf {
        self.0.join("daemon-dev.sock")
    }
    fn command(&self) -> Command {
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_sentinel-daemon-dev"));
        cmd.arg("--profile")
            .arg(&self.0)
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        cmd
    }
    fn connect(&self) -> Client {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            if let Ok(client) = Client::connect(&self.socket(), "process-test") {
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
struct Running(Child);
impl Drop for Running {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}
fn shutdown(profile: &Profile, mut process: Running) {
    let mut client = profile.connect();
    assert_eq!(
        client.request("daemon.shutdown", json!({})).unwrap()["shutting_down"],
        true
    );
    assert!(process.0.wait().unwrap().success());
    assert!(!profile.socket().exists());
}
#[test]
fn real_client_exclusive_profile_and_orderly_restart() {
    let profile = Profile::new();
    let first = Running(profile.command().spawn().unwrap());
    let mut client = profile.connect();
    let status = client.request("daemon.status", json!({})).unwrap();
    assert_eq!(status["running"], true);
    assert_eq!(status["protocol_major"], sentinel_ipc::contract::MAJOR);
    assert_eq!(status["protocol_minor"], sentinel_ipc::contract::MINOR);
    assert!(!profile.command().status().unwrap().success());
    let first_uptime = client.request("daemon.status", json!({})).unwrap()["uptime_ms"]
        .as_u64()
        .unwrap();
    assert!(
        client
            .request("chat.send", json!({"session_id":"s", "text":"test"}))
            .is_err()
    );
    drop(client);
    thread::sleep(Duration::from_millis(30));
    let second_uptime = profile
        .connect()
        .request("daemon.status", json!({}))
        .unwrap()["uptime_ms"]
        .as_u64()
        .unwrap();
    assert!(second_uptime > first_uptime);
    shutdown(&profile, first);
    let second = Running(profile.command().spawn().unwrap());
    assert_eq!(
        profile
            .connect()
            .request("daemon.status", json!({}))
            .unwrap()["running"],
        true
    );
    shutdown(&profile, second);
}
#[test]
fn refuses_existing_files_and_stale_sockets_without_removal() {
    let profile = Profile::new();
    fs::write(profile.socket(), b"preserve").unwrap();
    assert!(!profile.command().status().unwrap().success());
    assert_eq!(fs::read(profile.socket()).unwrap(), b"preserve");
    fs::remove_file(profile.socket()).unwrap();
    let socket = UnixListener::bind(profile.socket()).unwrap();
    drop(socket);
    assert!(!profile.command().status().unwrap().success());
    assert!(fs::symlink_metadata(profile.socket()).is_ok());
}
#[test]
fn rejects_non_private_or_symlink_profile_and_lock() {
    let profile = Profile::new();
    fs::set_permissions(&profile.0, fs::Permissions::from_mode(0o755)).unwrap();
    assert!(!profile.command().status().unwrap().success());
    fs::set_permissions(&profile.0, fs::Permissions::from_mode(0o700)).unwrap();
    let target = profile.0.join("target");
    fs::write(&target, b"unchanged").unwrap();
    symlink(&target, profile.0.join("daemon-dev.lock")).unwrap();
    assert!(!profile.command().status().unwrap().success());
    assert_eq!(fs::read(target).unwrap(), b"unchanged");
    let link = profile.0.with_extension("link");
    symlink(&profile.0, &link).unwrap();
    let status = Command::new(env!("CARGO_BIN_EXE_sentinel-daemon-dev"))
        .arg("--profile")
        .arg(&link)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .unwrap();
    assert!(!status.success());
    fs::remove_file(link).unwrap();
}

#[test]
fn raw_malformed_peer_does_not_stop_daemon() {
    use std::{
        io::{BufRead, BufReader, Write},
        os::unix::net::UnixStream,
    };
    let profile = Profile::new();
    let process = Running(profile.command().spawn().unwrap());
    drop(profile.connect());
    let mut stream = UnixStream::connect(profile.socket()).unwrap();
    stream
        .set_read_timeout(Some(Duration::from_secs(2)))
        .unwrap();
    stream.write_all(b"{\n").unwrap();
    let mut reader = BufReader::new(stream);
    let mut line = String::new();
    reader.read_line(&mut line).unwrap();
    let reply: serde_json::Value = serde_json::from_str(&line).unwrap();
    assert_eq!(reply["payload"]["code"], "malformed-json");
    drop(reader);
    assert_eq!(
        profile
            .connect()
            .request("daemon.status", json!({}))
            .unwrap()["running"],
        true
    );
    shutdown(&profile, process);
}

#[test]
fn streaming_disconnect_reattach_cancel_and_next_run() {
    use std::{
        io::{BufRead, BufReader, Read, Write},
        net::TcpListener,
        sync::mpsc,
    };
    let profile = Profile::new();
    let provider = TcpListener::bind("127.0.0.1:0").unwrap();
    let origin = format!("http://{}", provider.local_addr().unwrap());
    let (ready_tx, ready_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let fixture = thread::spawn(move || {
        for index in 0..3 {
            let (mut stream, _) = provider.accept().unwrap();
            stream
                .set_read_timeout(Some(Duration::from_secs(5)))
                .unwrap();
            stream
                .set_write_timeout(Some(Duration::from_secs(5)))
                .unwrap();
            let mut reader = BufReader::new(stream.try_clone().unwrap());
            let mut first = String::new();
            reader.read_line(&mut first).unwrap();
            assert_eq!(first.trim(), "POST /v1/chat/completions HTTP/1.1");
            let mut content_length = 0;
            loop {
                let mut line = String::new();
                reader.read_line(&mut line).unwrap();
                if line == "\r\n" {
                    break;
                }
                if let Some(value) = line.to_ascii_lowercase().strip_prefix("content-length:") {
                    content_length = value.trim().parse::<usize>().unwrap();
                }
            }
            let mut body = vec![0; content_length];
            reader.read_exact(&mut body).unwrap();
            let request: serde_json::Value = serde_json::from_slice(&body).unwrap();
            assert_eq!(request["model"], "fixture-model");
            assert_eq!(request["stream"], true);
            let delta = match index {
                0 => "first",
                1 => "second",
                _ => "partial",
            };
            let first_event = format!(
                "data: {{\"choices\":[{{\"delta\":{{\"content\":\"{delta}\"}},\"finish_reason\":null}}]}}\n\n"
            );
            let rest = if index == 2 {
                "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\ndata: [DONE]\n\n"
            } else {
                "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n"
            };
            write!(
                stream,
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: {}\r\n\r\n",
                first_event.len() + rest.len()
            )
            .unwrap();
            stream.write_all(first_event.as_bytes()).unwrap();
            if index == 0 {
                ready_tx.send(()).unwrap();
                release_rx.recv_timeout(Duration::from_secs(5)).unwrap();
            }
            let _ = stream.write_all(rest.as_bytes());
        }
    });
    let mut command = profile.command();
    command.args(["--provider", &origin, "--model", "fixture-model"]);
    let process = Running(command.spawn().unwrap());
    let mut owner = profile.connect();
    let session = owner
        .request("session.create", json!({"title":"chat"}))
        .unwrap()["session_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let run = owner
        .request("chat.send", json!({"session_id":session,"text":"hello"}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(owner.next_event().unwrap().name, "run.started");
    let delta = owner.next_event().unwrap();
    assert_eq!(
        (delta.name.as_str(), delta.payload["text"].as_str()),
        ("output.delta", Some("first"))
    );
    ready_rx.recv_timeout(Duration::from_secs(2)).unwrap();
    drop(owner);
    let mut attached = profile.connect();
    let snapshot = attached
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(snapshot["run_id"], run);
    assert_eq!(snapshot["state"], "running");
    assert_eq!(snapshot["output"], "first");
    assert_eq!(snapshot["model_id"], "fixture-model");
    assert_eq!(snapshot["provider_id"], "openai-compatible-local");
    let mut control = profile.connect();
    assert!(
        control
            .request("run.cancel", json!({"run_id":"foreign"}))
            .is_err()
    );
    assert_eq!(
        control
            .request("run.cancel", json!({"run_id":run}))
            .unwrap()["accepted"],
        true
    );
    let terminal = attached.next_event().unwrap();
    assert_eq!(terminal.name, "run.cancelled");
    assert_eq!(terminal.payload["run_id"], run);
    assert_eq!(terminal.payload["text"], "first");
    let restored = control
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(restored["state"], "cancelled");
    assert_eq!(restored["output"], terminal.payload["text"]);
    assert!(
        control
            .request("run.cancel", json!({"run_id":run}))
            .is_err()
    );
    release_tx.send(()).unwrap();
    let next = attached
        .request("chat.send", json!({"session_id":session,"text":"again"}))
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_ne!(next, run);
    assert_eq!(attached.next_event().unwrap().name, "run.started");
    let delta = attached.next_event().unwrap();
    assert_eq!(delta.payload["text"], "second");
    let terminal = attached.next_event().unwrap();
    assert_eq!(terminal.name, "run.completed");
    assert_eq!(terminal.payload["run_id"], next);
    assert_eq!(terminal.payload["text"], "second");
    let failed = attached
        .request(
            "chat.send",
            json!({"session_id":session,"text":"truncated"}),
        )
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    assert_eq!(attached.next_event().unwrap().name, "run.started");
    assert_eq!(attached.next_event().unwrap().payload["text"], "partial");
    let terminal = attached.next_event().unwrap();
    assert_eq!(terminal.name, "run.failed");
    assert_eq!(terminal.payload["run_id"], failed);
    assert_eq!(terminal.payload["terminal_reason"], "provider-failed");
    let restored = control
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(restored["output"], terminal.payload["text"]);
    fixture.join().unwrap();
    drop(attached);
    drop(control);
    shutdown(&profile, process);
}

#[test]
fn run_survives_requester_disconnect_before_ack() {
    use std::{
        io::{BufRead, BufReader, Read, Write},
        net::TcpListener,
        os::unix::net::UnixStream,
        sync::mpsc,
    };
    let profile = Profile::new();
    let provider = TcpListener::bind("127.0.0.1:0").unwrap();
    let origin = format!("http://{}", provider.local_addr().unwrap());
    let (accepted_tx, accepted_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let fixture = thread::spawn(move || {
        let (mut stream, _) = provider.accept().unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(5)))
            .unwrap();
        let mut reader = BufReader::new(stream.try_clone().unwrap());
        let mut content_length = 0;
        loop {
            let mut line = String::new();
            reader.read_line(&mut line).unwrap();
            if line == "\r\n" {
                break;
            }
            if let Some(value) = line.to_ascii_lowercase().strip_prefix("content-length:") {
                content_length = value.trim().parse::<usize>().unwrap();
            }
        }
        let mut body = vec![0; content_length];
        reader.read_exact(&mut body).unwrap();
        accepted_tx.send(()).unwrap();
        release_rx.recv_timeout(Duration::from_secs(5)).unwrap();
        let body = "data: {\"choices\":[{\"delta\":{\"content\":\"survived\"},\"finish_reason\":null}]}\n\ndata: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
        write!(stream, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: {}\r\n\r\n{body}", body.len()).unwrap();
    });
    let mut command = profile.command();
    command.args(["--provider", &origin, "--model", "fixture-model"]);
    let process = Running(command.spawn().unwrap());
    let mut control = profile.connect();
    let session = control
        .request("session.create", json!({"title":"detached"}))
        .unwrap()["session_id"]
        .as_str()
        .unwrap()
        .to_owned();
    let mut raw = UnixStream::connect(profile.socket()).unwrap();
    raw.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
    writeln!(raw, "{}", json!({"version":{"major":1,"minor":1},"type":"request","id":"h","name":"hello","payload":{"client_id":"drop-test","major":1,"minor":1,"capabilities":[]}})).unwrap();
    let mut reader = BufReader::new(raw);
    let mut response = String::new();
    reader.read_line(&mut response).unwrap();
    assert_eq!(
        serde_json::from_str::<serde_json::Value>(&response).unwrap()["type"],
        "response"
    );
    reader.get_ref().shutdown(std::net::Shutdown::Read).unwrap();
    writeln!(reader.get_mut(), "{}", json!({"version":{"major":1,"minor":1},"type":"request","id":"r","name":"chat.send","payload":{"session_id":session,"text":"continue"}})).unwrap();
    drop(reader);
    accepted_rx.recv_timeout(Duration::from_secs(5)).unwrap();
    let snapshot = control
        .request("session.attach", json!({"session_id":session}))
        .unwrap();
    assert_eq!(snapshot["state"], "running");
    let run = snapshot["run_id"].as_str().unwrap().to_owned();
    release_tx.send(()).unwrap();
    assert_eq!(control.next_event().unwrap().name, "run.started");
    let delta = control.next_event().unwrap();
    assert_eq!(delta.name, "output.delta");
    assert_eq!(delta.payload["run_id"], run);
    assert_eq!(delta.payload["text"], "survived");
    let terminal = control.next_event().unwrap();
    assert_eq!(terminal.name, "run.completed");
    assert_eq!(terminal.payload["run_id"], run);
    fixture.join().unwrap();
    drop(control);
    shutdown(&profile, process);
}

#[test]
fn terminal_storage_failure_never_reports_completion() {
    use std::{
        io::{BufRead, BufReader, Read, Write},
        net::TcpListener,
        sync::mpsc,
    };
    let profile = Profile::new();
    let provider = TcpListener::bind("127.0.0.1:0").unwrap();
    let origin = format!("http://{}", provider.local_addr().unwrap());
    let (release_tx, release_rx) = mpsc::channel();
    let fixture = thread::spawn(move || {
        let (mut stream, _) = provider.accept().unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(5)))
            .unwrap();
        let mut reader = BufReader::new(stream.try_clone().unwrap());
        let mut content_length = 0;
        loop {
            let mut line = String::new();
            reader.read_line(&mut line).unwrap();
            if line == "\r\n" {
                break;
            }
            if let Some(value) = line.to_ascii_lowercase().strip_prefix("content-length:") {
                content_length = value.trim().parse::<usize>().unwrap();
            }
        }
        let mut body = vec![0; content_length];
        reader.read_exact(&mut body).unwrap();
        let delta = "data: {\"choices\":[{\"delta\":{\"content\":\"partial\"},\"finish_reason\":null}]}\n\n";
        let end =
            "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
        write!(stream, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: {}\r\n\r\n{delta}", delta.len() + end.len()).unwrap();
        release_rx.recv_timeout(Duration::from_secs(5)).unwrap();
        stream.write_all(end.as_bytes()).unwrap();
    });
    let mut command = profile.command();
    command.args(["--provider", &origin, "--model", "fixture-model"]);
    let process = Running(command.spawn().unwrap());
    let mut client = profile.connect();
    let session = client
        .request("session.create", json!({"title":"failure"}))
        .unwrap()["session_id"]
        .as_str()
        .unwrap()
        .to_owned();
    client
        .request("chat.send", json!({"session_id":session,"text":"hello"}))
        .unwrap();
    assert_eq!(client.next_event().unwrap().name, "run.started");
    assert_eq!(client.next_event().unwrap().payload["text"], "partial");
    let journal = profile.0.join("daemon-dev-chat.sqlite3-journal");
    fs::create_dir(&journal).unwrap();
    release_tx.send(()).unwrap();
    let terminal = client.next_event().unwrap();
    assert_eq!(terminal.name, "run.failed");
    assert_eq!(terminal.payload["terminal_reason"], "storage-failure");
    assert!(
        client
            .request("chat.send", json!({"session_id":session,"text":"again"}))
            .is_err()
    );
    fs::remove_dir(journal).unwrap();
    fixture.join().unwrap();
    drop(client);
    shutdown(&profile, process);
}
