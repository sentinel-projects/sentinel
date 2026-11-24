//! Bounded text-only PDF attachment extraction in a disposable daemon process.
use std::{
    io::{self, Read, Write},
    net::Shutdown,
    os::{fd::OwnedFd, unix::net::UnixStream},
    process::{Child, Command, Stdio},
    thread,
    time::{Duration, Instant},
};

use crate::document::{MAX_INPUT, MAX_TEXT};

const MAX_STREAM_BYTES: usize = 128 * 1024;
const MAX_PAGES: usize = 16;
const MAX_OBJECTS: usize = 2048;
const WORKER_TIMEOUT: Duration = Duration::from_millis(900);
pub const WORKER_ARG: &str = "--internal-pdf-text";

fn invalid() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "unsupported PDF attachment")
}

struct Worker(Child);
impl Drop for Worker {
    fn drop(&mut self) {
        if self.0.try_wait().ok().flatten().is_none() {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
}

/// The worker receives the authorized snapshot over a socketpair. Neither the
/// user's workspace nor the source file name is passed to the parser.
pub fn extract(bytes: &[u8]) -> io::Result<String> {
    if bytes.len() > MAX_INPUT || !bytes.starts_with(b"%PDF-") {
        return Err(invalid());
    }
    let (mut transport, worker_end) = UnixStream::pair()?;
    transport.set_nonblocking(true)?;
    let stdin = OwnedFd::from(worker_end.try_clone()?);
    let stdout = OwnedFd::from(worker_end);
    let deadline = Instant::now() + WORKER_TIMEOUT;
    let mut worker = Worker(
        Command::new(std::env::current_exe()?)
            .arg(WORKER_ARG)
            .env_clear()
            .current_dir("/")
            .stdin(Stdio::from(stdin))
            .stdout(Stdio::from(stdout))
            .stderr(Stdio::null())
            .spawn()?,
    );
    let mut pending = bytes;
    let mut closed_write = false;
    let mut eof = false;
    let mut output = Vec::new();
    let mut buffer = [0u8; 4096];
    loop {
        if Instant::now() >= deadline {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "PDF extraction deadline",
            ));
        }
        let mut progressed = false;
        if !pending.is_empty() {
            match transport.write(pending) {
                Ok(0) => return Err(invalid()),
                Ok(count) => {
                    pending = &pending[count..];
                    progressed = true;
                }
                Err(error)
                    if matches!(
                        error.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                    ) => {}
                Err(error) => return Err(error),
            }
        } else if !closed_write {
            transport.shutdown(Shutdown::Write)?;
            closed_write = true;
        }
        if !eof {
            match transport.read(&mut buffer) {
                Ok(0) => eof = true,
                Ok(count) if output.len() + count <= MAX_TEXT => {
                    output.extend_from_slice(&buffer[..count]);
                    progressed = true;
                }
                Ok(_) => return Err(invalid()),
                Err(error)
                    if matches!(
                        error.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                    ) => {}
                Err(error) => return Err(error),
            }
        }
        if eof && let Some(status) = worker.0.try_wait()? {
            if !closed_write || !status.success() {
                return Err(invalid());
            }
            let text = String::from_utf8(output).map_err(|_| invalid())?;
            return if text.trim().is_empty() || text.contains('\0') {
                Err(invalid())
            } else {
                Ok(text)
            };
        }
        if !progressed {
            thread::sleep(Duration::from_millis(5));
        }
    }
}

pub fn worker() -> io::Result<()> {
    // Limit the process before decoding untrusted data. The parent independently
    // enforces a wall-clock deadline and terminates a stuck or crashed parser.
    for (resource, ceiling) in [
        (libc::RLIMIT_AS, 384 * 1024 * 1024),
        (libc::RLIMIT_CPU, 1),
        (libc::RLIMIT_CORE, 0),
    ] {
        let mut limits = libc::rlimit {
            rlim_cur: 0,
            rlim_max: 0,
        };
        // SAFETY: getrlimit populates the initialized limits struct for this process.
        if unsafe { libc::getrlimit(resource, &mut limits) } != 0 {
            return Err(io::Error::last_os_error());
        }
        limits.rlim_cur = limits.rlim_cur.min(ceiling);
        limits.rlim_max = limits.rlim_max.min(ceiling);
        // SAFETY: setrlimit reads the supplied limits struct for this process only.
        if unsafe { libc::setrlimit(resource, &limits) } != 0 {
            return Err(io::Error::last_os_error());
        }
    }
    let mut input = Vec::new();
    io::stdin()
        .lock()
        .take((MAX_INPUT + 1) as u64)
        .read_to_end(&mut input)?;
    if input.len() > MAX_INPUT || !input.starts_with(b"%PDF-") {
        return Err(invalid());
    }

    let pdf = lopdf::Document::load_mem_with_options(
        &input,
        lopdf::LoadOptions {
            strict: true,
            max_decompressed_size: Some(MAX_STREAM_BYTES),
            ..Default::default()
        },
    )
    .map_err(|_| invalid())?;
    if pdf.is_encrypted() || pdf.was_encrypted() || pdf.objects.len() > MAX_OBJECTS {
        return Err(invalid());
    }
    let pages = pdf.page_iter().take(MAX_PAGES + 1).count();
    if pages == 0 || pages > MAX_PAGES {
        return Err(invalid());
    }

    let mut text = String::new();
    for page in 1..=pages as u32 {
        let extracted = pdf
            .extract_text_with_limit(&[page], MAX_STREAM_BYTES)
            .map_err(|_| invalid())?;
        if text.len().saturating_add(extracted.len()) > MAX_TEXT || extracted.contains('\0') {
            return Err(invalid());
        }
        text.push_str(&extracted);
    }
    if text.trim().is_empty() {
        return Err(invalid());
    }
    io::stdout().lock().write_all(text.as_bytes())
}
