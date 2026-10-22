#![cfg(unix)]
use serde_json::{Value, json};
use std::{
    io::{BufRead, BufReader, Write},
    os::unix::net::UnixListener,
    process::{Command, Stdio},
    sync::atomic::{AtomicU64, Ordering},
};
static NEXT: AtomicU64 = AtomicU64::new(0);
fn cli(args: &[&str], stdin: Option<&str>, terminal: &str) -> std::process::Output {
    cli_events(args, stdin, terminal, false)
}
fn cli_events(
    args: &[&str],
    stdin: Option<&str>,
    terminal: &str,
    stale: bool,
) -> std::process::Output {
    let root = std::env::temp_dir().join(format!(
        "sentinel-cli-test-{}-{}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    std::fs::create_dir_all(&root).unwrap();
    let path = root.join("daemon.sock");
    let listener = UnixListener::bind(&path).unwrap();
    let terminal = terminal.to_owned();
    let server = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut reader = BufReader::new(stream.try_clone().unwrap());
        for _ in 0..3 {
            let mut line = String::new();
            reader.read_line(&mut line).unwrap();
            let request: Value = serde_json::from_str(&line).unwrap();
            let payload = match request["name"].as_str().unwrap() {
                "hello" => json!({"major":1,"minor":0,"daemon_version":"test","capabilities":[]}),
                "session.create" => {
                    json!({"session_id":"session","type":"conversation","created_at":"test"})
                }
                "chat.send" | "agent.start" => {
                    assert!(
                        request["payload"]["text"]
                            .as_str()
                            .is_some_and(|s| !s.is_empty())
                    );
                    json!({"run_id":"run","session_id":"session"})
                }
                _ => panic!("unexpected command"),
            };
            if stale && ["chat.send", "agent.start"].contains(&request["name"].as_str().unwrap()) {
                // Real request() queues these before the new start acknowledgement.
                for (sid, run) in [("session", "old-run"), ("other-session", "run")] {
                    writeln!(stream,"{}",json!({"version":{"major":1,"minor":0},"type":"event","id":"","name":"run.failed","payload":{"text":"STALE","run_id":run,"session_id":sid,"state":"failed"}})).unwrap();
                }
            }
            writeln!(stream,"{}",json!({"version":{"major":1,"minor":0},"type":"response","id":request["id"],"name":request["name"],"payload":payload})).unwrap();
        }
        writeln!(stream,"{}",json!({"version":{"major":1,"minor":0},"type":"event","id":"","name":"output.delta","payload":{"text":"MAVI","run_id":"run","session_id":"session"}})).unwrap();
        writeln!(stream,"{}",json!({"version":{"major":1,"minor":0},"type":"event","id":"","name":terminal,"payload":{"text":"MAVI","run_id":"run","session_id":"session","state":terminal.trim_start_matches("run.")}})).unwrap();
        // The real daemon keeps the transport alive after a terminal event.
        // Wait for the client's close, rather than racing socket option restoration.
        stream
            .set_read_timeout(Some(std::time::Duration::from_secs(5)))
            .unwrap();
        let mut extra = String::new();
        assert_eq!(reader.read_line(&mut extra).unwrap(), 0);
    });
    let mut child = Command::new(env!("CARGO_BIN_EXE_sentinel"))
        .arg("--socket")
        .arg(&path)
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    if let Some(text) = stdin {
        child
            .stdin
            .take()
            .unwrap()
            .write_all(text.as_bytes())
            .unwrap();
    }
    let result = child.wait_with_output().unwrap();
    server.join().unwrap();
    std::fs::remove_dir_all(root).unwrap();
    result
}
#[test]
fn text_final_not_duplicated() {
    let r = cli(&["chat", "hello"], None, "run.completed");
    assert!(r.status.success());
    assert_eq!(r.stdout, b"MAVI\n");
}
#[test]
fn json_final() {
    let r = cli(
        &["--output", "json", "agent", "task"],
        None,
        "run.completed",
    );
    assert!(r.status.success());
    let v: Value = serde_json::from_slice(&r.stdout).unwrap();
    assert_eq!(v["text"], "MAVI");
    assert_eq!(v["state"], "completed");
}
#[test]
fn stream_json() {
    let r = cli(
        &["--output", "stream-json", "chat", "hello"],
        None,
        "run.completed",
    );
    assert!(r.status.success());
    let rows: Vec<Value> = String::from_utf8(r.stdout)
        .unwrap()
        .lines()
        .map(|s| serde_json::from_str(s).unwrap())
        .collect();
    assert_eq!(rows.len(), 2);
    assert_eq!(rows[0]["name"], "output.delta");
    assert_eq!(rows[1]["name"], "run.completed");
}
#[test]
fn stdin_task() {
    let r = cli(&["run"], Some("pipeline task"), "run.completed");
    assert!(r.status.success());
}
#[test]
fn cancelled_exit_is_distinct() {
    let r = cli(&["run", "task"], None, "run.cancelled");
    assert_eq!(
        r.status.code(),
        Some(130),
        "{}",
        String::from_utf8_lossy(&r.stderr)
    );
}
#[test]
fn unavailable_exit_is_distinct() {
    let r = Command::new(env!("CARGO_BIN_EXE_sentinel"))
        .args(["--socket", "/nonexistent/sentinel.sock", "status"])
        .output()
        .unwrap();
    assert_eq!(r.status.code(), Some(3));
    assert!(r.stdout.is_empty());
}

#[test]
fn failed_exit_is_nonzero() {
    let r = cli(&["run", "task"], None, "run.failed");
    assert_eq!(
        r.status.code(),
        Some(1),
        "{}",
        String::from_utf8_lossy(&r.stderr)
    );
}

#[test]
fn incompatible_exit_is_distinct() {
    let root = std::env::temp_dir().join(format!(
        "sentinel-major-test-{}-{}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    std::fs::create_dir_all(&root).unwrap();
    let path = root.join("daemon.sock");
    let listener = UnixListener::bind(&path).unwrap();
    let server = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        let mut reader = BufReader::new(stream.try_clone().unwrap());
        let mut line = String::new();
        reader.read_line(&mut line).unwrap();
        let request: Value = serde_json::from_str(&line).unwrap();
        writeln!(stream,"{}",json!({"version":{"major":2,"minor":0},"type":"response","id":request["id"],"name":"hello","payload":{}})).unwrap();
    });
    let result = Command::new(env!("CARGO_BIN_EXE_sentinel"))
        .arg("--socket")
        .arg(&path)
        .arg("status")
        .output()
        .unwrap();
    server.join().unwrap();
    std::fs::remove_dir_all(root).unwrap();
    assert_eq!(result.status.code(), Some(4));
    assert!(result.stdout.is_empty());
}

#[test]
fn help_is_offline_and_successful() {
    for command in [
        "",
        "status",
        "models",
        "sessions",
        "chat",
        "agent",
        "run",
        "attach",
        "tui",
        "doctor",
        "model",
        "completions",
        "shutdown",
    ] {
        let mut invocation = Command::new(env!("CARGO_BIN_EXE_sentinel"));
        if !command.is_empty() {
            invocation.arg(command);
        }
        let result = invocation.arg("--help").output().unwrap();
        assert!(result.status.success(), "{command}: {:?}", result.stderr);
        let text = String::from_utf8(result.stdout).unwrap();
        assert!(text.contains("Usage:"));
        assert!(!text.contains("Protocol("));
    }
}
#[test]
fn usage_validation_precedes_daemon_connection() {
    for args in [
        vec!["invented"],
        vec!["--output", "wrong", "status"],
        vec!["attach"],
        vec!["model", "select", "ollama"],
    ] {
        let result = Command::new(env!("CARGO_BIN_EXE_sentinel"))
            .args(args)
            .output()
            .unwrap();
        assert_eq!(result.status.code(), Some(2));
    }
}
#[test]
fn missing_task_is_usage_error() {
    let result = Command::new(env!("CARGO_BIN_EXE_sentinel"))
        .args(["run", "--output", "json"])
        .stdin(Stdio::null())
        .output()
        .unwrap();
    assert_eq!(result.status.code(), Some(2));
    let json: Value = serde_json::from_slice(&result.stdout).unwrap();
    assert_eq!(json["error"]["code"], "cli-usage");
}
#[test]
fn machine_error_is_typed() {
    let result = Command::new(env!("CARGO_BIN_EXE_sentinel"))
        .args([
            "status",
            "--socket",
            "/nonexistent/sentinel.sock",
            "--output",
            "json",
        ])
        .output()
        .unwrap();
    assert_eq!(result.status.code(), Some(3));
    let json: Value = serde_json::from_slice(&result.stdout).unwrap();
    assert_eq!(json["error"]["code"], "daemon-unavailable");
    assert!(!result.stderr.is_empty());
}
#[test]
fn completions_are_offline() {
    for shell in ["bash", "zsh", "fish"] {
        let result = Command::new(env!("CARGO_BIN_EXE_sentinel"))
            .args(["completions", shell])
            .output()
            .unwrap();
        assert!(result.status.success());
        assert!(!result.stdout.is_empty());
    }
}

#[test]
fn global_output_after_task_is_parsed() {
    let r = cli(
        &["chat", "hello", "--output", "json"],
        None,
        "run.completed",
    );
    assert!(r.status.success());
    let value: Value = serde_json::from_slice(&r.stdout).unwrap();
    assert_eq!(value["text"], "MAVI");
}

#[test]
fn queued_old_and_cross_client_terminals_cannot_finish_new_command() {
    for terminal in ["run.completed", "run.cancelled", "run.failed"] {
        let result = cli_events(
            &["--output", "stream-json", "run", "task"],
            None,
            terminal,
            true,
        );
        assert_eq!(
            result.status.code(),
            Some(match terminal {
                "run.completed" => 0,
                "run.cancelled" => 130,
                _ => 1,
            })
        );
        let rows: Vec<Value> = String::from_utf8(result.stdout)
            .unwrap()
            .lines()
            .map(|line| serde_json::from_str(line).unwrap())
            .collect();
        assert_eq!(rows.len(), if terminal == "run.completed" { 2 } else { 3 });
        if terminal != "run.completed" {
            assert_eq!(
                rows[2]["error"]["code"],
                if terminal == "run.cancelled" {
                    "cancelled"
                } else {
                    "task-failed"
                }
            );
        }
        let rows = &rows[..2];
        assert_eq!(rows[1]["name"], terminal);
        assert!(
            rows.iter().all(|row| row["payload"]["run_id"] == "run"
                && row["payload"]["session_id"] == "session")
        );
    }
}

fn acknowledgement(args: &[&str], name: &str, payload: Value) -> std::process::Output {
    let root = std::path::Path::new("/tmp").join(format!(
        "s-ack-{}-{}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    std::fs::create_dir(&root).unwrap();
    let path = root.join("daemon.sock");
    let listener = UnixListener::bind(&path).unwrap();
    let name = name.to_owned();
    let server = std::thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        stream
            .set_read_timeout(Some(std::time::Duration::from_secs(5)))
            .unwrap();
        let mut reader = BufReader::new(stream.try_clone().unwrap());
        for operation in ["hello", name.as_str()] {
            let mut line = String::new();
            assert!(reader.read_line(&mut line).unwrap() > 0);
            let request: Value = serde_json::from_str(&line).unwrap();
            assert_eq!(request["name"], operation);
            let response = if operation == "hello" {
                json!({"major":1,"minor":0,"daemon_version":"fixture","capabilities":[]})
            } else {
                payload.clone()
            };
            writeln!(
                stream,
                "{}",
                json!({"version":{"major":1,"minor":0},"type":"response",
                "id":request["id"],"name":operation,"payload":response})
            )
            .unwrap();
        }
        let mut extra = String::new();
        assert_eq!(reader.read_line(&mut extra).unwrap(), 0);
    });
    let result = Command::new(env!("CARGO_BIN_EXE_sentinel"))
        .arg("--socket")
        .arg(&path)
        .args(args)
        .output()
        .unwrap();
    server.join().unwrap();
    std::fs::remove_dir_all(root).unwrap();
    result
}

#[test]
fn mutation_acknowledgements_are_truthful_in_every_output_mode() {
    for output in ["text", "json", "stream-json"] {
        for (command, name) in [
            (
                vec!["provider", "endpoint", "ollama", "http://localhost:11434"],
                "desktop.setting",
            ),
            (vec!["provider", "refresh"], "desktop.action"),
        ] {
            for accepted in [true, false] {
                let mut args = vec!["--output", output];
                args.extend(command.clone());
                let payload = if name == "desktop.action" {
                    json!({"accepted":accepted,"result":null})
                } else {
                    json!({"accepted":accepted})
                };
                let result = acknowledgement(&args, name, payload.clone());
                assert_eq!(result.status.code(), Some(if accepted { 0 } else { 1 }));
                if accepted {
                    assert!(result.stderr.is_empty());
                    if output == "json" {
                        assert_eq!(
                            serde_json::from_slice::<Value>(&result.stdout).unwrap(),
                            payload
                        );
                    } else if output == "stream-json" {
                        let row: Value = serde_json::from_slice(&result.stdout).unwrap();
                        assert_eq!(row["type"], "result");
                        assert_eq!(row["payload"], payload);
                    }
                } else {
                    assert!(String::from_utf8_lossy(&result.stderr).contains("operation-rejected"));
                    if output == "text" {
                        assert!(result.stdout.is_empty());
                    } else {
                        let row: Value = serde_json::from_slice(&result.stdout).unwrap();
                        assert_eq!(row["type"], "error");
                        assert_eq!(row["error"]["code"], "operation-rejected");
                        assert_eq!(row["exit_code"], 1);
                    }
                }
            }
        }
    }
}

#[test]
fn id_based_mutations_and_queries_do_not_require_accepted() {
    for (command, name, payload) in [
        (vec!["models"], "model.list", json!({"models":[]})),
        (
            vec!["workspace", "select", "coding"],
            "workspace.select",
            json!({"workspace_id":"coding"}),
        ),
    ] {
        let mut args = vec!["--output", "json"];
        args.extend(command);
        let result = acknowledgement(&args, name, payload.clone());
        assert!(result.status.success());
        assert_eq!(
            serde_json::from_slice::<Value>(&result.stdout).unwrap(),
            payload
        );
    }
}
