//! Explicit local Ollama pull/remove operations; no automatic downloads.
use crate::provider::{self, Config};
use serde_json::{Value, json};
use std::{
    io,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    thread,
};

#[derive(Default)]
struct State {
    active: bool,
    model: String,
    progress: f64,
    status: String,
    error: String,
    cancel: Option<Arc<AtomicBool>>,
}
#[derive(Default)]
pub struct Operations {
    inner: Arc<Mutex<State>>,
}
impl Operations {
    pub fn is_active(&self) -> io::Result<bool> {
        Ok(self
            .inner
            .lock()
            .map_err(|_| io::Error::other("model operation state unavailable"))?
            .active)
    }
    pub fn state(&self) -> io::Result<Value> {
        let s = self
            .inner
            .lock()
            .map_err(|_| io::Error::other("model operation state unavailable"))?;
        Ok(
            json!({"pulling":s.active,"activeModel":s.model,"progress":s.progress,"statusText":s.status,"errorText":s.error}),
        )
    }
    pub fn start(&self, config: Config, model: &str, remove: bool) -> io::Result<()> {
        if config.provider_id != "ollama"
            || !config.address.ip().is_loopback()
            || config.address.port() == 0
            || model.is_empty()
            || model.len() > 256
            || model.trim() != model
            || model.chars().any(char::is_control)
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid local model operation",
            ));
        }
        let mut state = self
            .inner
            .lock()
            .map_err(|_| io::Error::other("model operation state unavailable"))?;
        if state.active {
            return Err(io::Error::new(
                io::ErrorKind::WouldBlock,
                "model operation busy",
            ));
        }
        let token = Arc::new(AtomicBool::new(false));
        state.active = true;
        state.model = model.into();
        state.progress = 0.0;
        state.status = if remove {
            "Removing model…"
        } else {
            "Starting model pull…"
        }
        .into();
        state.error.clear();
        state.cancel = Some(token.clone());
        let shared = self.inner.clone();
        let model = model.to_owned();
        let spawn=thread::Builder::new().name("sentinel-model-operation".into()).spawn(move || {
            let progress_state=shared.clone();
            let result=std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| provider::ollama_model_operation(&config,&model,remove,&token,|status,counts| {
                if let Ok(mut state)=progress_state.lock() {
                    state.status=status.into();
                    if let Some((completed,total))=counts { state.progress=completed as f64 / total as f64; }
                }
            })));
            if let Ok(mut state)=shared.lock() {
                state.active=false; state.cancel=None;
                match result {
                    Ok(Ok(())) => { state.progress=1.0; state.status=if remove {"Model removed."} else {"Model pull complete."}.into(); }
                    Ok(Err(provider::ProviderError::Cancelled)) => { state.status="Stopped waiting; provider may continue. Refresh model list to check.".into(); state.error="Outcome may have changed on the provider.".into(); }
                    _ => { state.status="Model operation failed or its outcome is unknown.".into(); state.error="Refresh model list before retrying.".into(); }
                }
            }
        });
        if let Err(error) = spawn {
            state.active = false;
            state.cancel = None;
            state.status = "Model operation did not start.".into();
            return Err(error);
        }
        Ok(())
    }
    pub fn cancel(&self) -> io::Result<bool> {
        let state = self
            .inner
            .lock()
            .map_err(|_| io::Error::other("model operation state unavailable"))?;
        if let Some(cancel) = &state.cancel {
            cancel.store(true, Ordering::SeqCst);
            Ok(true)
        } else {
            Ok(false)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        io::{Read, Write},
        net::TcpListener,
        time::{Duration, Instant},
    };
    #[test]
    fn concurrent_operation_is_rejected_and_cancel_is_uncertain() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let config = Config::parse(
            &format!("http://{}", listener.local_addr().unwrap()),
            "fixture",
        )
        .unwrap()
        .with_provider_kind("ollama")
        .unwrap();
        let (accepted_tx, accepted_rx) = std::sync::mpsc::channel();
        let server = thread::spawn(move || {
            let (mut socket, _) = listener.accept().unwrap();
            let mut buf = [0u8; 4096];
            let _ = socket.read(&mut buf).unwrap();
            socket.write_all(b"HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\nContent-Length: 100\r\n\r\n").unwrap();
            accepted_tx.send(()).unwrap();
            let _ = socket.read(&mut buf);
        });
        let ops = Operations::default();
        ops.start(config.clone(), "fixture", false).unwrap();
        assert!(ops.start(config, "other", false).is_err());
        accepted_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        assert!(ops.cancel().unwrap());
        let deadline = Instant::now() + Duration::from_secs(3);
        while ops.is_active().unwrap() {
            assert!(Instant::now() < deadline);
            thread::sleep(Duration::from_millis(10));
        }
        assert!(
            ops.state().unwrap()["statusText"]
                .as_str()
                .unwrap()
                .contains("provider may continue")
        );
        server.join().unwrap();
    }
}
