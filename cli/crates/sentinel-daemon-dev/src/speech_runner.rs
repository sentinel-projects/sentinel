//! Explicit local Whisper execution; unsupported without the macOS sandbox.
use crate::speech_inspection::Inspection;
use std::{io, sync::atomic::AtomicBool};

pub fn available() -> bool {
    #[cfg(target_os = "macos")]
    {
        use std::os::unix::fs::MetadataExt;
        return std::fs::symlink_metadata("/usr/bin/sandbox-exec").is_ok_and(|metadata| {
            metadata.is_file() && metadata.uid() == 0 && metadata.mode() & 0o022 == 0
        });
    }
    #[cfg(not(target_os = "macos"))]
    false
}

pub fn transcribe(
    inspection: &Inspection,
    _profile: &std::path::Path,
    pcm: &[u8],
    cancel: &AtomicBool,
) -> io::Result<String> {
    #[cfg(target_os = "macos")]
    {
        run_macos(inspection, pcm, cancel)
    }
    #[cfg(not(target_os = "macos"))]
    {
        let _ = (inspection, pcm, cancel);
        Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "speech sandbox unavailable",
        ))
    }
}

#[cfg(target_os = "macos")]
fn run_macos(inspection: &Inspection, pcm: &[u8], cancel: &AtomicBool) -> io::Result<String> {
    use std::{
        fs::{self, OpenOptions},
        io::{Read, Write},
        os::{
            fd::AsRawFd,
            unix::{
                fs::{DirBuilderExt, OpenOptionsExt},
                process::CommandExt,
            },
        },
        path::PathBuf,
        process::{Child, Command, Stdio},
        sync::atomic::{AtomicU64, Ordering},
        thread,
        time::{Duration, Instant},
    };
    const AUDIO_LIMIT: usize = 16000 * 2 * 60;
    const OUTPUT_LIMIT: usize = 16 * 1024;
    static NEXT: AtomicU64 = AtomicU64::new(0);
    if pcm.is_empty() || pcm.len() > AUDIO_LIMIT || pcm.len() % 2 != 0 {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "invalid PCM"));
    }
    if !available() {
        return Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "speech sandbox unavailable",
        ));
    }
    let deadline = Instant::now() + Duration::from_secs(120);
    inspection.revalidate(cancel, deadline)?;
    let root = std::fs::canonicalize("/tmp")?;
    let mut created = None;
    for _ in 0..128 {
        let dir = root.join(format!(
            "sentinel-speech-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        match fs::DirBuilder::new().mode(0o700).create(&dir) {
            Ok(()) => {
                created = Some(dir);
                break;
            }
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(e),
        }
    }
    struct Temp(Option<PathBuf>);
    impl Drop for Temp {
        fn drop(&mut self) {
            if let Some(dir) = self.0.take() {
                let _ = fs::remove_dir_all(dir);
            }
        }
    }
    let mut temp =
        Temp(Some(created.ok_or_else(|| {
            io::Error::other("speech temp unavailable")
        })?));
    let result = (|| -> io::Result<String> {
        let dir = temp
            .0
            .as_ref()
            .ok_or_else(|| io::Error::other("speech temp unavailable"))?;
        let wav = dir.join("input.wav");
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(&wav)?;
        let size = pcm.len() as u32;
        file.write_all(b"RIFF")?;
        file.write_all(&(36 + size).to_le_bytes())?;
        file.write_all(b"WAVEfmt ")?;
        file.write_all(&16u32.to_le_bytes())?;
        file.write_all(&1u16.to_le_bytes())?;
        file.write_all(&1u16.to_le_bytes())?;
        file.write_all(&16000u32.to_le_bytes())?;
        file.write_all(&32000u32.to_le_bytes())?;
        file.write_all(&2u16.to_le_bytes())?;
        file.write_all(&16u16.to_le_bytes())?;
        file.write_all(b"data")?;
        file.write_all(&size.to_le_bytes())?;
        file.write_all(pcm)?;
        file.sync_all()?;
        drop(file);
        fn path_text(path: &std::path::Path) -> io::Result<&str> {
            let value = path
                .to_str()
                .ok_or_else(|| io::Error::other("non-UTF-8 speech path"))?;
            if value.chars().any(char::is_control) {
                return Err(io::Error::other("invalid speech path"));
            }
            Ok(value)
        }
        let binary = path_text(&inspection.binary)?;
        let model = path_text(&inspection.model)?;
        let audio = path_text(&wav)?;
        let quote = |s: &str| format!("\"{}\"", s.replace('\\', "\\\\").replace('"', "\\\""));
        let sandbox = format!(
            "(version 1)(deny default)(import \"system.sb\")(allow process-exec (literal {}))(deny process-fork)(deny network*)(deny file-write*)(allow signal (target self))(allow file-read* (subpath \"/System\") (subpath \"/usr/lib\") (literal \"/dev/null\") (literal {}) (literal {}) (literal {}))",
            quote(binary),
            quote(binary),
            quote(model),
            quote(audio)
        );
        if cancel.load(Ordering::SeqCst) || Instant::now() >= deadline {
            return Err(io::Error::new(io::ErrorKind::Interrupted, "speech stopped"));
        }
        let mut command = Command::new("/usr/bin/sandbox-exec");
        command
            .args([
                "-p",
                &sandbox,
                binary,
                "--model",
                model,
                "--file",
                audio,
                "--no-prints",
                "--no-timestamps",
                "--no-gpu",
            ])
            .env_clear()
            .env("LANG", "C")
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .process_group(0);
        struct Guard(Child);
        impl Drop for Guard {
            fn drop(&mut self) {
                if self.0.try_wait().ok().flatten().is_none() {
                    unsafe {
                        libc::kill(-(self.0.id() as i32), libc::SIGKILL);
                    }
                    let _ = self.0.wait();
                }
            }
        }
        let mut child = Guard(command.spawn()?);
        let mut out = child
            .0
            .stdout
            .take()
            .ok_or_else(|| io::Error::other("speech output unavailable"))?;
        let mut err = child
            .0
            .stderr
            .take()
            .ok_or_else(|| io::Error::other("speech output unavailable"))?;
        for fd in [out.as_raw_fd(), err.as_raw_fd()] {
            let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
            if flags < 0 || unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) } < 0
            {
                return Err(io::Error::last_os_error());
            }
        }
        fn drain(reader: &mut impl Read, output: &mut Vec<u8>, limit: usize) -> io::Result<bool> {
            let mut buf = [0u8; 4096];
            for _ in 0..8 {
                match reader.read(&mut buf) {
                    Ok(0) => return Ok(true),
                    Ok(n) => {
                        if output.len() + n > limit {
                            return Err(io::Error::other("speech output limit"));
                        }
                        output.extend_from_slice(&buf[..n]);
                    }
                    Err(e) if e.kind() == io::ErrorKind::WouldBlock => return Ok(false),
                    Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                    Err(e) => return Err(e),
                }
            }
            Ok(false)
        }
        let mut stdout = Vec::new();
        let mut stderr = Vec::new();
        let mut status = None;
        loop {
            if cancel.load(Ordering::SeqCst) || Instant::now() >= deadline {
                return Err(io::Error::new(io::ErrorKind::Interrupted, "speech stopped"));
            }
            let done_out = drain(&mut out, &mut stdout, OUTPUT_LIMIT)?;
            let done_err = drain(&mut err, &mut stderr, OUTPUT_LIMIT)?;
            if status.is_none() {
                status = child.0.try_wait()?;
            }
            if status.is_some() && done_out && done_err {
                break;
            }
            thread::sleep(Duration::from_millis(10));
        }
        if !status.is_some_and(|value| value.success()) {
            return Err(io::Error::other("sandboxed speech process failed"));
        }
        let text = String::from_utf8(stdout)
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid speech output"))?;
        let text = text.trim().to_owned();
        if text.is_empty() || text.contains('\0') {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "empty speech output",
            ));
        }
        Ok(text)
    })();
    let path = temp
        .0
        .as_ref()
        .ok_or_else(|| io::Error::other("speech temp unavailable"))?;
    if let Err(error) = fs::remove_dir_all(path) {
        return Err(io::Error::new(
            error.kind(),
            "speech temporary audio cleanup failed",
        ));
    }
    temp.0 = None;
    result
}
