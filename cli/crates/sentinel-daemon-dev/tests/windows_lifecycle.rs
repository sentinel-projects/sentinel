#![cfg(windows)]

use sentinel_ipc::{Client, contract};
use std::{
    io::{BufRead, BufReader},
    os::windows::{
        ffi::OsStrExt,
        io::{AsRawHandle, FromRawHandle, OwnedHandle},
    },
    path::Path,
    process::{Child, Command, Stdio},
    ptr::null_mut,
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
use windows_sys::Win32::{
    Foundation::INVALID_HANDLE_VALUE,
    Security::{
        ACL, ACL_REVISION, AddAccessAllowedAce, GetLengthSid, GetTokenInformation, InitializeAcl,
        InitializeSecurityDescriptor, SE_DACL_PROTECTED, SECURITY_ATTRIBUTES, SECURITY_DESCRIPTOR,
        SetSecurityDescriptorControl, SetSecurityDescriptorDacl, SetSecurityDescriptorOwner,
        TOKEN_QUERY, TOKEN_USER, TokenUser,
    },
    Storage::FileSystem::{CreateDirectoryW, FILE_ALL_ACCESS},
    System::Threading::{GetCurrentProcess, OpenProcessToken},
};

fn owner_only_temp_directory() -> std::io::Result<std::path::PathBuf> {
    let base = std::env::temp_dir().canonicalize()?;
    let path = base.join(format!(
        "sentinel-win-lifecycle-{}-{}",
        std::process::id(),
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    let mut token: windows_sys::Win32::Foundation::HANDLE = null_mut();
    if unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) } == 0 {
        return Err(std::io::Error::last_os_error());
    }
    if token.is_null() || token == INVALID_HANDLE_VALUE {
        return Err(std::io::Error::last_os_error());
    }
    let token = unsafe { OwnedHandle::from_raw_handle(token) };
    let mut sid_size = 0;
    unsafe {
        GetTokenInformation(
            token.as_raw_handle() as _,
            TokenUser,
            null_mut(),
            0,
            &mut sid_size,
        )
    };
    if sid_size == 0 || sid_size > 65536 {
        return Err(std::io::Error::other("invalid current-user token"));
    }
    let mut token_user = vec![0usize; (sid_size as usize).div_ceil(std::mem::size_of::<usize>())];
    if unsafe {
        GetTokenInformation(
            token.as_raw_handle() as _,
            TokenUser,
            token_user.as_mut_ptr().cast(),
            sid_size,
            &mut sid_size,
        )
    } == 0
    {
        return Err(std::io::Error::last_os_error());
    }
    let sid = unsafe { (*(token_user.as_ptr().cast::<TOKEN_USER>())).User.Sid };
    let acl_len = (std::mem::size_of::<ACL>() + 64 + unsafe { GetLengthSid(sid) } as usize)
        .next_multiple_of(std::mem::size_of::<usize>());
    let mut acl = vec![0usize; acl_len / std::mem::size_of::<usize>()];
    let acl_ptr = acl.as_mut_ptr().cast::<ACL>();
    let mut descriptor = SECURITY_DESCRIPTOR::default();
    let sd = (&mut descriptor as *mut SECURITY_DESCRIPTOR).cast();
    if unsafe { InitializeSecurityDescriptor(sd, 1) } == 0
        || unsafe { InitializeAcl(acl_ptr, acl_len as u32, ACL_REVISION) } == 0
        || unsafe { AddAccessAllowedAce(acl_ptr, ACL_REVISION, FILE_ALL_ACCESS, sid) } == 0
        || unsafe { SetSecurityDescriptorOwner(sd, sid, 0) } == 0
        || unsafe { SetSecurityDescriptorDacl(sd, 1, acl_ptr, 0) } == 0
        || unsafe { SetSecurityDescriptorControl(sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED) } == 0
    {
        return Err(std::io::Error::last_os_error());
    }
    let attributes = SECURITY_ATTRIBUTES {
        nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
        lpSecurityDescriptor: sd,
        bInheritHandle: 0,
    };
    let mut wide: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
    if unsafe { CreateDirectoryW(wide.as_mut_ptr(), &attributes) } == 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(path)
}

struct ChildGuard(Child);
impl Drop for ChildGuard {
    fn drop(&mut self) {
        if self.0.try_wait().ok().flatten().is_none() {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
}

struct ProfileCleanup(std::path::PathBuf);
impl Drop for ProfileCleanup {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(self.0.join("daemon-dev.lock"));
        let _ = std::fs::remove_dir(&self.0);
    }
}

fn start_daemon(
    profile: &Path,
) -> std::io::Result<(ChildGuard, BufReader<std::process::ChildStdout>, String)> {
    let child = Command::new(env!("CARGO_BIN_EXE_sentinel-daemon-dev"))
        .arg("--profile")
        .arg(profile)
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()?;
    let mut child = ChildGuard(child);
    let stdout = child
        .0
        .stdout
        .take()
        .ok_or_else(|| std::io::Error::other("missing stdout"))?;
    let mut reader = BufReader::new(stdout);
    let mut line = String::new();
    reader.read_line(&mut line)?;
    let endpoint = line
        .strip_prefix("Development pipe: ")
        .map(str::trim)
        .filter(|value| !value.is_empty())
        .ok_or_else(|| std::io::Error::other("daemon did not publish endpoint"))?
        .to_owned();
    Ok((child, reader, endpoint))
}

#[test]
fn windows_hello_status_shutdown_and_profile_exclusion() -> Result<(), Box<dyn std::error::Error>> {
    let profile =
        ProfileCleanup(owner_only_temp_directory().expect("create restricted temporary profile"));
    let (mut daemon, _stdout, endpoint) = start_daemon(&profile.0).expect("start dev daemon");

    // The profile lock must reject a second lifecycle owner before another pipe is created.
    let second = Command::new(env!("CARGO_BIN_EXE_sentinel-daemon-dev"))
        .arg("--profile")
        .arg(&profile.0)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .expect("spawn competing daemon");
    assert!(
        !second.success(),
        "second daemon unexpectedly opened profile"
    );

    let mut client = Client::connect(Path::new(&endpoint), "windows-lifecycle-test")
        .expect("complete named-pipe hello");
    let status = client
        .request("daemon.status", serde_json::json!({}))
        .expect("read daemon status");
    assert_eq!(status["running"], true);
    assert_eq!(status["protocol_major"], contract::MAJOR);
    assert_eq!(status["protocol_minor"], contract::MINOR);
    assert_eq!(status["active_runs"], 0);
    assert_eq!(status["sessions"], 0);

    let shutdown = client
        .request("daemon.shutdown", serde_json::json!({}))
        .expect("receive shutdown acknowledgement");
    assert_eq!(shutdown["shutting_down"], true);
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        if let Some(status) = daemon.0.try_wait()? {
            assert!(status.success(), "daemon shutdown failed: {status}");
            break;
        }
        if Instant::now() >= deadline {
            panic!("daemon did not exit after shutdown acknowledgement");
        }
        thread::sleep(Duration::from_millis(10));
    }
    drop(client);
    drop(daemon);
    Ok(())
}
