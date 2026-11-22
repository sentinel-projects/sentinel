#![cfg(unix)]
use sentinel_ipc::{
    Client, contract,
    server::{Dispatcher, read_frame},
};
use serde_json::{Value, json};
use std::{
    fs,
    io::{BufReader, Write},
    os::unix::{
        fs::DirBuilderExt,
        net::{UnixListener, UnixStream},
    },
    time::{Duration, Instant},
};

fn request(id: &str, name: &str, payload: Value) -> Value {
    json!({"version":{"major":1,"minor":1},"type":"request","id":id,"name":name,"payload":payload})
}
fn hello() -> Value {
    json!({"client_id":"fixture","major":1,"minor":1,"capabilities":[]})
}
fn dispatch(d: &mut Dispatcher, value: Value) -> sentinel_ipc::Envelope {
    d.dispatch(&serde_json::to_vec(&value).unwrap())
}

#[test]
fn typed_dispatch_and_rejection() {
    let mut d = Dispatcher::default();
    assert_eq!(
        dispatch(&mut d, request("a", "daemon.status", json!({}))).payload["code"],
        "handshake-required"
    );
    let mut bad = request("b", "hello", hello());
    bad["type"] = json!("event");
    assert_eq!(dispatch(&mut d, bad).payload["code"], "invalid-envelope");
    let mut bad = request("c", "hello", hello());
    bad["version"]["major"] = json!(2);
    assert_eq!(dispatch(&mut d, bad).payload["code"], "incompatible-major");
    assert_eq!(
        dispatch(&mut d, request("d", "unknown", json!({}))).payload["code"],
        "unknown-command"
    );
    assert_eq!(
        dispatch(&mut d, request("e", "hello", json!({"client_id":3}))).payload["code"],
        "invalid-payload"
    );
    let reply = dispatch(&mut d, request("f", "hello", hello()));
    assert_eq!((reply.id.as_str(), reply.name.as_str()), ("f", "hello"));
    assert_eq!(
        dispatch(&mut d, request("g", "hello", hello())).payload["code"],
        "invalid-state"
    );
    assert_eq!(
        dispatch(&mut d, request("h", "daemon.shutdown", json!({}))).payload["code"],
        "runtime-unavailable"
    );
    assert_eq!(d.dispatch(b"{").payload["code"], "malformed-json");
    assert_eq!(d.dispatch(b"[]").payload["code"], "malformed-json");
}

#[test]
fn actual_listener_interoperates_with_client() {
    // create_dir is exclusive: never unlink or bind an existing endpoint.
    let dir = std::env::temp_dir().join(format!(
        "sentinel-ipc-fixture-{}-{:?}",
        std::process::id(),
        std::thread::current().id()
    ));
    fs::DirBuilder::new().mode(0o700).create(&dir).unwrap();
    struct Cleanup(std::path::PathBuf);
    impl Drop for Cleanup {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    let _cleanup = Cleanup(dir.clone());
    let path = dir.join("fixture.sock");
    let listener = UnixListener::bind(&path).unwrap();
    let worker = std::thread::spawn(move || {
        let (stream, _) = listener.accept().unwrap();
        let mut writer = stream.try_clone().unwrap();
        let mut reader = BufReader::new(stream);
        let mut dispatcher = Dispatcher::default();
        for _ in 0..2 {
            let frame = read_frame(&mut reader, Instant::now() + Duration::from_secs(2)).unwrap();
            let response = dispatcher.dispatch(&frame);
            writeln!(writer, "{}", serde_json::to_string(&response).unwrap()).unwrap();
        }
    });
    let mut client = Client::connect(&path, "integration").unwrap();
    assert_eq!(
        client.request("daemon.status", json!({})).unwrap()["running"],
        true
    );
    worker.join().unwrap();
}

#[test]
fn frames_are_bounded_fragmented_and_deadlined() {
    let (stream, mut peer) = UnixStream::pair().unwrap();
    let mut reader = BufReader::new(stream);
    peer.write_all(b"one\ntwo\n").unwrap();
    assert_eq!(
        read_frame(&mut reader, Instant::now() + Duration::from_secs(1)).unwrap(),
        b"one"
    );
    assert_eq!(
        read_frame(&mut reader, Instant::now() + Duration::from_secs(1)).unwrap(),
        b"two"
    );
    peer.write_all(b"part").unwrap();
    assert_eq!(
        read_frame(&mut reader, Instant::now() + Duration::from_millis(30))
            .unwrap_err()
            .kind(),
        std::io::ErrorKind::TimedOut
    );
    let (stream, mut peer) = UnixStream::pair().unwrap();
    let worker = std::thread::spawn(move || {
        peer.write_all(&vec![b'x'; contract::MAX_FRAME_BYTES - 1])
            .unwrap();
        peer.write_all(b"\n").unwrap();
    });
    let mut reader = BufReader::new(stream);
    assert_eq!(
        read_frame(&mut reader, Instant::now() + Duration::from_secs(2))
            .unwrap()
            .len(),
        contract::MAX_FRAME_BYTES - 1
    );
    worker.join().unwrap();
    let (stream, mut peer) = UnixStream::pair().unwrap();
    let worker = std::thread::spawn(move || {
        let _ = peer.write_all(&vec![b'x'; contract::MAX_FRAME_BYTES + 1]);
    });
    let mut reader = BufReader::new(stream);
    assert_eq!(
        read_frame(&mut reader, Instant::now() + Duration::from_secs(2))
            .unwrap_err()
            .kind(),
        std::io::ErrorKind::InvalidData
    );
    worker.join().unwrap();
}

#[test]
fn buffered_frame_survives_peer_close() {
    use std::io::BufRead;
    let (stream, mut peer) = UnixStream::pair().unwrap();
    let mut reader = BufReader::new(stream);
    peer.write_all(b"{}\n").unwrap();
    assert_eq!(reader.fill_buf().unwrap(), b"{}\n");
    drop(peer);
    assert_eq!(
        read_frame(&mut reader, Instant::now() + Duration::from_secs(1)).unwrap(),
        b"{}"
    );
}
