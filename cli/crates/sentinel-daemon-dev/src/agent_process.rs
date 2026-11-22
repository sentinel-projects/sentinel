//! Restricted macOS process slice. No shell, network, writes or child processes.
use crate::agent_gateway::{PermissionDecision, Workspace};
use crate::tool_registry::{self, Tool};
use serde_json::{Value, json};
use std::{io, sync::atomic::AtomicBool};

pub fn descriptor() -> Value {
    tool_registry::descriptor(Tool::ProcessExec).schema()
}

pub fn available() -> bool {
    cfg!(target_os = "macos")
        && [
            "/usr/bin/sandbox-exec",
            "/usr/bin/wc",
            "/usr/bin/head",
            "/usr/bin/tail",
            "/usr/bin/grep",
        ]
        .iter()
        .all(|path| std::path::Path::new(path).is_file())
}

pub fn execute(
    workspace: &Workspace,
    args: &Value,
    decision: PermissionDecision,
    cancel: &AtomicBool,
) -> io::Result<Value> {
    let _ = (workspace, args, decision, cancel);
    #[cfg(target_os = "macos")]
    {
        execute_macos(workspace, args, decision, cancel)
    }
    #[cfg(not(target_os = "macos"))]
    Err(io::Error::new(
        io::ErrorKind::Unsupported,
        "process sandbox unavailable",
    ))
}

fn checked_argv(args: &Value) -> io::Result<Vec<&str>> {
    let invalid = || io::Error::new(io::ErrorKind::InvalidInput, "invalid process arguments");
    let invocation = tool_registry::plan(Tool::ProcessExec, args)?;
    if !invocation.is_current() {
        return Err(invalid());
    }
    let object = args.as_object().ok_or_else(invalid)?;
    let argv = object
        .get("argv")
        .and_then(Value::as_array)
        .ok_or_else(invalid)?;
    let argv: Vec<&str> = argv
        .iter()
        .map(|v| v.as_str().ok_or_else(invalid))
        .collect::<io::Result<_>>()?;
    let program = argv[0];
    if ![
        "/usr/bin/wc",
        "/usr/bin/head",
        "/usr/bin/tail",
        "/usr/bin/grep",
    ]
    .contains(&program)
    {
        return Err(invalid());
    }
    Ok(argv)
}

pub fn validate(args: &Value) -> io::Result<()> {
    checked_argv(args).map(|_| ())
}

#[cfg(target_os = "macos")]
fn execute_macos(
    workspace: &Workspace,
    args: &Value,
    decision: PermissionDecision,
    cancel: &AtomicBool,
) -> io::Result<Value> {
    use std::{
        io::Read,
        os::{fd::AsRawFd, unix::process::CommandExt},
        process::{Child, Command, Stdio},
        sync::atomic::Ordering,
        thread,
        time::{Duration, Instant},
    };
    let argv = checked_argv(args)?;
    let program = argv[0];
    if decision != PermissionDecision::Allow {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "process approval required",
        ));
    }
    if cancel.load(Ordering::SeqCst) {
        return Err(io::Error::new(io::ErrorKind::Interrupted, "cancelled"));
    }
    let (root, cwd) = workspace.process_root()?;
    use std::os::unix::ffi::OsStrExt;
    let mut record = std::mem::MaybeUninit::<libc::passwd>::uninit();
    let mut buffer = vec![0u8; 16384];
    let mut found = std::ptr::null_mut();
    let code = unsafe {
        libc::getpwuid_r(
            libc::geteuid(),
            record.as_mut_ptr(),
            buffer.as_mut_ptr().cast(),
            buffer.len(),
            &mut found,
        )
    };
    if code != 0 || found.is_null() {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "home directory unavailable",
        ));
    }
    let home_ptr = unsafe { (*found).pw_dir };
    if home_ptr.is_null() {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "home directory unavailable",
        ));
    }
    let home =
        std::ffi::OsStr::from_bytes(unsafe { std::ffi::CStr::from_ptr(home_ptr) }.to_bytes());
    let home = std::fs::canonicalize(home)?;
    let root_path = std::path::Path::new(&root);
    if root_path.parent().is_none() || home.starts_with(root_path) {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "workspace too broad",
        ));
    }
    let literal = |s: &str| format!("\"{}\"", s.replace('\\', "\\\\").replace('"', "\\\""));
    let profile = format!(
        "(version 1)(deny default)(import \"system.sb\")(allow process-exec (literal {}))(deny process-fork)(deny network*)(deny file-write*)(allow signal (target self))(allow file-read* (subpath \"/System\") (subpath \"/usr/lib\") (literal {}) (literal \"/dev/null\") (subpath {}))",
        literal(program),
        literal(program),
        literal(&root)
    );
    let mut command = Command::new("/usr/bin/sandbox-exec");
    command
        .args(["-p", &profile, program])
        .args(&argv[1..])
        .env_clear()
        .env("LANG", "C")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .process_group(0);
    // SAFETY: fchdir is async-signal-safe; cwd is a held directory descriptor that
    // remains alive until spawn returns. No allocation or lock occurs in the child.
    unsafe {
        command.pre_exec(move || {
            if libc::fchdir(cwd) == 0 {
                Ok(())
            } else {
                Err(io::Error::last_os_error())
            }
        });
    }
    struct ChildGuard(Child);
    impl Drop for ChildGuard {
        fn drop(&mut self) {
            if self.0.try_wait().ok().flatten().is_none() {
                unsafe {
                    libc::kill(-(self.0.id() as i32), libc::SIGKILL);
                }
                let _ = self.0.wait();
            }
        }
    }
    let mut child = ChildGuard(command.spawn()?);
    let mut stdout = child
        .0
        .stdout
        .take()
        .ok_or_else(|| io::Error::other("stdout unavailable"))?;
    let mut stderr = child
        .0
        .stderr
        .take()
        .ok_or_else(|| io::Error::other("stderr unavailable"))?;
    for fd in [stdout.as_raw_fd(), stderr.as_raw_fd()] {
        let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
        if flags < 0 || unsafe { libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK) } < 0 {
            return Err(io::Error::last_os_error());
        }
    }
    fn drain(reader: &mut impl Read, buffer: &mut Vec<u8>) -> io::Result<bool> {
        let mut chunk = [0u8; 4096];
        loop {
            match reader.read(&mut chunk) {
                Ok(0) => return Ok(true),
                Ok(n) => {
                    if buffer.len().saturating_add(n) > 6 * 1024 {
                        return Err(io::Error::other("process output exceeded limit"));
                    }
                    buffer.extend_from_slice(&chunk[..n]);
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => return Ok(false),
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(e) => return Err(e),
            }
        }
    }
    let deadline = Instant::now() + Duration::from_secs(10);
    let mut out = Vec::new();
    let mut err = Vec::new();
    let mut status = None;
    loop {
        let stdout_done = drain(&mut stdout, &mut out)?;
        let stderr_done = drain(&mut stderr, &mut err)?;
        if status.is_none() {
            status = child.0.try_wait()?;
        }
        if status.is_some() && stdout_done && stderr_done {
            break;
        }
        if cancel.load(Ordering::SeqCst) || Instant::now() >= deadline {
            return Err(io::Error::new(
                if cancel.load(Ordering::SeqCst) {
                    io::ErrorKind::Interrupted
                } else {
                    io::ErrorKind::TimedOut
                },
                "process stopped",
            ));
        }
        thread::sleep(Duration::from_millis(10));
    }
    let status = status.ok_or_else(|| io::Error::other("missing process status"))?;
    // A nonzero sandbox-exec status may be a launcher failure, not a child exit.
    // Without a trusted launch handshake, fail closed instead of attributing it.
    if !status.success() {
        return Err(io::Error::other(
            "sandboxed process did not complete successfully",
        ));
    }
    let stdout = String::from_utf8(out)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "non-UTF-8 process output"))?;
    let stderr = String::from_utf8(err)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "non-UTF-8 process output"))?;
    Ok(
        json!({"argv":argv,"exitCode":status.code(),"stdout":stdout,"stderr":stderr,"complete":true}),
    )
}

#[cfg(all(test, target_os = "macos"))]
mod tests {
    use super::*;
    use std::fs;
    #[test]
    fn process_is_allowlisted_and_uses_sandboxed_workspace() {
        let root =
            std::env::temp_dir().join(format!("sentinel-process-test-{}", std::process::id()));
        fs::create_dir(&root).unwrap();
        fs::write(root.join("input"), "abc").unwrap();
        let outside = root.with_extension("outside");
        fs::write(&outside, "private-fixture-marker").unwrap();
        let workspace = Workspace::open(&root).unwrap();
        let cancel = AtomicBool::new(false);
        let args = json!({"argv":["/usr/bin/wc","-c","input"]});
        assert!(execute(&workspace, &args, PermissionDecision::Deny, &cancel).is_err());
        assert!(
            execute(
                &workspace,
                &json!({"argv":["/bin/sh","-c","echo test"]}),
                PermissionDecision::Allow,
                &cancel
            )
            .is_err()
        );
        let result = execute(&workspace, &args, PermissionDecision::Allow, &cancel).unwrap();
        assert_eq!(result["exitCode"], 0, "{result}");
        assert!(result["stdout"].as_str().unwrap().contains('3'));
        let outside_args =
            json!({"argv":["/usr/bin/grep","private-fixture-marker",outside.to_str().unwrap()]});
        let denied = execute(
            &workspace,
            &outside_args,
            PermissionDecision::Allow,
            &cancel,
        );
        assert!(denied.is_err());
        fs::remove_file(outside).unwrap();
        fs::remove_dir_all(root).unwrap();
    }
}
