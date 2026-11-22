#![cfg(any(target_os = "linux", target_os = "macos"))]
use rusqlite::{Connection, params};
use sentinel_ipc::Client;
use serde_json::json;
use std::{
    fs,
    io::{BufRead, BufReader, Read, Write},
    net::TcpListener,
    os::unix::fs::{DirBuilderExt, OpenOptionsExt},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::atomic::{AtomicUsize, Ordering},
    thread,
    time::{Duration, Instant},
};
static NEXT: AtomicUsize = AtomicUsize::new(0);
const S1: &str = "11111111-1111-4111-8111-111111111111";
const S2: &str = "22222222-2222-4222-8222-222222222222";
const TIME: &str = "2026-11-17T09:48:25.575Z";
struct Temp(PathBuf);
impl Temp {
    fn new() -> Self {
        let path = Path::new("/tmp").join(format!(
            "sentinel-import-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::DirBuilder::new().mode(0o700).create(&path).unwrap();
        Self(path)
    }
    fn command(&self) -> Command {
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_sentinel-daemon-dev"));
        cmd.arg("--profile")
            .arg(&self.0)
            .stdout(Stdio::null())
            .stderr(Stdio::null());
        cmd
    }
    fn start(&self) -> std::process::Child {
        self.command().spawn().unwrap()
    }
    fn connect(&self) -> Client {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            if let Ok(client) = Client::connect(&self.0.join("daemon-dev.sock"), "import-test") {
                return client;
            }
            assert!(Instant::now() < deadline, "daemon startup");
            thread::sleep(Duration::from_millis(10));
        }
    }
    fn initialize(&self) {
        let mut daemon = Running(self.start());
        let mut client = self.connect();
        client.request("daemon.shutdown", json!({})).unwrap();
        assert!(daemon.0.wait().unwrap().success());
    }
}
struct Running(std::process::Child);
impl Drop for Running {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}
impl Drop for Temp {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
fn make_source(dir: &Temp, oversized: bool) -> PathBuf {
    let path = dir.0.join("copy.sqlite3");
    fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&path)
        .unwrap();
    let db = Connection::open(&path).unwrap();
    db.execute_batch("PRAGMA journal_mode=DELETE;
        CREATE TABLE conversation_schema_metadata(key TEXT PRIMARY KEY NOT NULL,value INTEGER NOT NULL);
        INSERT INTO conversation_schema_metadata VALUES('schema_version',6);
        CREATE TABLE conversations(id TEXT PRIMARY KEY NOT NULL,title TEXT NOT NULL,created_at TEXT NOT NULL,updated_at TEXT NOT NULL,archived INTEGER NOT NULL DEFAULT 0,pinned INTEGER NOT NULL DEFAULT 0,deleted INTEGER NOT NULL DEFAULT 0,user_renamed INTEGER NOT NULL DEFAULT 0);
        CREATE TABLE conversation_messages(conversation_id TEXT NOT NULL,message_id INTEGER NOT NULL,role TEXT NOT NULL,content TEXT NOT NULL,timestamp TEXT NOT NULL,status TEXT NOT NULL,provider_id TEXT NOT NULL DEFAULT '',model_id TEXT NOT NULL DEFAULT '',reply_to_id INTEGER NOT NULL DEFAULT 0,replaces_id INTEGER NOT NULL DEFAULT 0,partial INTEGER NOT NULL DEFAULT 0,error_category TEXT NOT NULL DEFAULT 'None',attachments_json TEXT NOT NULL DEFAULT '[]',PRIMARY KEY(conversation_id,message_id));
        CREATE TABLE conversation_summary_metadata(conversation_id TEXT PRIMARY KEY NOT NULL,summary_timestamp TEXT NOT NULL,covered_first_message_id INTEGER NOT NULL DEFAULT 0,covered_last_message_id INTEGER NOT NULL DEFAULT 0,estimated_reduction_percent INTEGER NOT NULL DEFAULT 0,readiness_state TEXT NOT NULL,summary_text TEXT NOT NULL DEFAULT '',summary TEXT NOT NULL);").unwrap();
    for id in [S1, S2] {
        db.execute(
            "INSERT INTO conversations(id,title,created_at,updated_at) VALUES(?1,'title',?2,?2)",
            params![id, TIME],
        )
        .unwrap();
        let content = if oversized && id == S2 {
            "\u{0}".repeat(50000)
        } else {
            format!("system {id}")
        };
        db.execute("INSERT INTO conversation_messages(conversation_id,message_id,role,content,timestamp,status) VALUES(?1,1,'system',?2,?3,'received')",params![id,content,TIME]).unwrap();
    }
    drop(db);
    path
}

fn make_context_source(dir: &Temp) -> PathBuf {
    let path = make_source(dir, false);
    let db = Connection::open(&path).unwrap();
    db.execute("INSERT INTO conversation_messages(conversation_id,message_id,role,content,timestamp,status) VALUES(?1,2,'user','question one',?2,'completed')",params![S1,TIME]).unwrap();
    db.execute("INSERT INTO conversation_messages(conversation_id,message_id,role,content,timestamp,status) VALUES(?1,3,'user','question two',?2,'completed')",params![S1,TIME]).unwrap();
    db.execute("INSERT INTO conversation_messages(conversation_id,message_id,role,content,timestamp,status,provider_id,model_id,reply_to_id) VALUES(?1,4,'assistant','answer two',?2,'completed','lm-studio','fixture-model',3)",params![S1,TIME]).unwrap();
    db.execute("INSERT INTO conversation_messages(conversation_id,message_id,role,content,timestamp,status,provider_id,model_id,reply_to_id) VALUES(?1,5,'assistant','old answer one',?2,'completed','lm-studio','fixture-model',2)",params![S1,TIME]).unwrap();
    db.execute("INSERT INTO conversation_messages(conversation_id,message_id,role,content,timestamp,status,provider_id,model_id,reply_to_id) VALUES(?1,6,'assistant','replacement answer one',?2,'completed','lm-studio','fixture-model',2)",params![S1,TIME]).unwrap();
    db.execute("INSERT INTO conversation_messages(conversation_id,message_id,role,content,timestamp,status,provider_id,model_id,reply_to_id,partial) VALUES(?1,7,'assistant','failed replacement',?2,'failed','lm-studio','fixture-model',2,1)",params![S1,TIME]).unwrap();
    db.execute("INSERT INTO conversation_messages(conversation_id,message_id,role,content,timestamp,status,provider_id,model_id,reply_to_id) VALUES(?1,8,'assistant','not a user reply',?2,'completed','lm-studio','fixture-model',1)",params![S1,TIME]).unwrap();
    drop(db);
    path
}

struct ProviderFixture {
    listener: TcpListener,
    origin: String,
}
impl ProviderFixture {
    fn new() -> Self {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let origin = format!("http://{}", listener.local_addr().unwrap());
        Self { listener, origin }
    }
    fn serve_once(&self) -> thread::JoinHandle<Vec<u8>> {
        let listener = self.listener.try_clone().unwrap();
        thread::spawn(move || {
            let (mut socket, _) = listener.accept().unwrap();
            let mut reader = BufReader::new(socket.try_clone().unwrap());
            let mut head = Vec::new();
            let mut length = 0usize;
            loop {
                let mut line = Vec::new();
                reader.read_until(b'\n', &mut line).unwrap();
                if line == b"\r\n" {
                    break;
                }
                if let Ok(text) = std::str::from_utf8(&line)
                    && let Some((name, value)) = text.split_once(':')
                    && name.eq_ignore_ascii_case("content-length")
                {
                    length = value.trim().parse().unwrap();
                }
                head.extend_from_slice(&line);
            }
            assert!(
                String::from_utf8(head)
                    .unwrap()
                    .starts_with("POST /v1/chat/completions HTTP/1.1")
            );
            let mut request = vec![0; length];
            reader.read_exact(&mut request).unwrap();
            let answer = serde_json::to_string("import context answer").unwrap();
            let body = format!("data: {{\"choices\":[{{\"delta\":{{\"content\":{answer}}},\"finish_reason\":null}}]}}\n\ndata: {{\"choices\":[{{\"delta\":{{}},\"finish_reason\":\"stop\"}}]}}\n\ndata: [DONE]\n\n").into_bytes();
            write!(socket,"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",body.len()).unwrap();
            socket.write_all(&body).unwrap();
            request
        })
    }
}

fn wait_completed(client: &mut Client, run_id: &str) {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        assert!(Instant::now() < deadline, "run did not complete");
        let event = client.next_event().unwrap();
        if event.payload["run_id"] == run_id
            && ["run.completed", "run.failed", "run.cancelled"].contains(&event.name.as_str())
        {
            assert_eq!(event.name, "run.completed");
            return;
        }
    }
}
#[test]
fn preserves_per_session_ids_and_source_bytes() {
    let source = Temp::new();
    let dest = Temp::new();
    let path = make_source(&source, false);
    let before = fs::read(&path).unwrap();
    dest.initialize();
    assert!(
        dest.command()
            .arg("--import-qt-copy")
            .arg(&path)
            .status()
            .unwrap()
            .success()
    );
    assert_eq!(fs::read(&path).unwrap(), before);
    let mut daemon = Running(dest.start());
    let mut client = dest.connect();
    for sid in [S1, S2] {
        let rows = client
            .request("session.messages", json!({"session_id":sid}))
            .unwrap();
        assert_eq!(rows["messages"][0]["id"], 1);
        assert_eq!(rows["messages"][0]["role"], "system");
        assert_eq!(rows["messages"][0]["timestamp"], TIME);
        assert_eq!(rows["messages"][0]["content"], format!("system {sid}"));
        let attached = client
            .request("session.attach", json!({"session_id":sid}))
            .unwrap();
        assert_eq!(attached["state"], "idle");
        assert_eq!(attached["output"], "");
    }
    client.request("daemon.shutdown", json!({})).unwrap();
    assert!(daemon.0.wait().unwrap().success());
    assert!(
        !dest
            .command()
            .arg("--import-qt-copy")
            .arg(&path)
            .status()
            .unwrap()
            .success()
    );
}

#[test]
fn memory_copy_import_preserves_metadata_through_ipc() {
    let conversations = Temp::new();
    let memory_source = Temp::new();
    let dest = Temp::new();
    let conversation_path = make_source(&conversations, false);
    dest.initialize();
    assert!(
        dest.command()
            .arg("--import-qt-copy")
            .arg(conversation_path)
            .status()
            .unwrap()
            .success()
    );
    let path = memory_source.0.join("memory.sqlite3");
    fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&path)
        .unwrap();
    let db = Connection::open(&path).unwrap();
    db.execute_batch("CREATE TABLE memory_schema_metadata(key TEXT PRIMARY KEY,value INTEGER NOT NULL); INSERT INTO memory_schema_metadata VALUES('schema_version',7); CREATE TABLE memory_entries(id INTEGER PRIMARY KEY,key TEXT,value TEXT); CREATE TABLE scoped_memory_entries(id TEXT PRIMARY KEY,scope INTEGER,owner_id TEXT,workspace_id TEXT,key TEXT,value TEXT,provenance INTEGER,source_ref TEXT,created_at TEXT,updated_at TEXT,revision TEXT,corrections TEXT);").unwrap();
    let id = "8fb66a15-7cb8-4779-892e-d46d7ff026ec";
    db.execute("INSERT INTO scoped_memory_entries VALUES(?1,1,?2,'','key','value',2,'source',?3,?3,'revision','[]')",params![id,S1,TIME]).unwrap();
    drop(db);
    let before = fs::read(&path).unwrap();
    assert!(
        dest.command()
            .arg("--import-qt-memory-copy")
            .arg(&path)
            .status()
            .unwrap()
            .success()
    );
    assert_eq!(fs::read(&path).unwrap(), before);
    assert!(
        !dest
            .command()
            .arg("--import-qt-memory-copy")
            .arg(&path)
            .status()
            .unwrap()
            .success()
    );
    let mut daemon = Running(dest.start());
    let mut client = dest.connect();
    client
        .request("session.attach", json!({"session_id":S1}))
        .unwrap();
    let response=client.request("desktop.action",json!({"session_id":S1,"action":"manageMemory","arguments":[{"operation":"read","scope":1,"id":id}]})).unwrap();
    let record = &response["result"]["value"]["record"];
    assert_eq!(record["id"], id);
    assert_eq!(record["revision"], "revision");
    assert_eq!(record["provenance"], 2);
    assert_eq!(record["source"], "source");
    assert_eq!(record["value"], "value");
    client.request("daemon.shutdown", json!({})).unwrap();
    assert!(daemon.0.wait().unwrap().success());
}
#[test]
fn oversized_second_conversation_rolls_back_everything() {
    let source = Temp::new();
    let dest = Temp::new();
    let path = make_source(&source, true);
    let before = fs::read(&path).unwrap();
    dest.initialize();
    assert!(
        !dest
            .command()
            .arg("--import-qt-copy")
            .arg(&path)
            .status()
            .unwrap()
            .success()
    );
    assert_eq!(fs::read(&path).unwrap(), before);
    let db = Connection::open_with_flags(
        dest.0.join("daemon-dev-chat.sqlite3"),
        rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY,
    )
    .unwrap();
    let count:i64=db.query_row("SELECT (SELECT count(*) FROM sessions)+(SELECT count(*) FROM runs)+(SELECT count(*) FROM messages)",[],|r|r.get(0)).unwrap();
    assert_eq!(count, 0);
}

#[test]
fn imported_context_uses_latest_successful_reply_links_and_omits_system_rows() {
    let source = Temp::new();
    let dest = Temp::new();
    let path = make_context_source(&source);
    let before = fs::read(&path).unwrap();
    dest.initialize();
    assert!(
        dest.command()
            .arg("--import-qt-copy")
            .arg(&path)
            .status()
            .unwrap()
            .success()
    );

    let provider = ProviderFixture::new();
    let mut command = dest.command();
    command.args(["--provider", &provider.origin, "--model", "fixture-model"]);
    let mut daemon = Running(command.spawn().unwrap());
    let mut client = dest.connect();
    let fixture = provider.serve_once();
    let run = client
        .request(
            "chat.send",
            json!({"session_id":S1,"text":"current question"}),
        )
        .unwrap()["run_id"]
        .as_str()
        .unwrap()
        .to_owned();
    wait_completed(&mut client, &run);
    let request: serde_json::Value = serde_json::from_slice(&fixture.join().unwrap()).unwrap();
    assert_eq!(
        request["messages"],
        json!([
            {"role":"user","content":"question one"},
            {"role":"assistant","content":"replacement answer one"},
            {"role":"user","content":"question two"},
            {"role":"assistant","content":"answer two"},
            {"role":"user","content":"current question"}
        ])
    );
    assert_eq!(fs::read(&path).unwrap(), before);
    client.request("daemon.shutdown", json!({})).unwrap();
    assert!(daemon.0.wait().unwrap().success());
}
