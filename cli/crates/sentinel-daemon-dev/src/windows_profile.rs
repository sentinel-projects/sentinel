//! Disposable Windows development profile ownership; no production profile access.
use std::{
    ffi::{OsStr, OsString},
    fs, io,
    os::windows::{
        ffi::OsStrExt,
        io::{AsRawHandle, FromRawHandle, OwnedHandle},
    },
    path::Path,
    ptr::{null, null_mut},
};
use windows_sys::Win32::{
    Foundation::{GENERIC_READ, GENERIC_WRITE, INVALID_HANDLE_VALUE, LocalFree},
    Security::{
        ACCESS_ALLOWED_ACE, ACL, ACL_SIZE_INFORMATION, AclSizeInformation,
        Authorization::{GetSecurityInfo, SE_FILE_OBJECT},
        DACL_SECURITY_INFORMATION, EqualSid, GetAce, GetAclInformation,
        GetSecurityDescriptorControl, GetTokenInformation, IsValidSid, OWNER_SECURITY_INFORMATION,
        SE_DACL_PROTECTED, TOKEN_QUERY, TOKEN_USER, TokenUser,
    },
    Storage::FileSystem::{
        BY_HANDLE_FILE_INFORMATION, CreateFileW, FILE_ATTRIBUTE_DIRECTORY,
        FILE_ATTRIBUTE_REPARSE_POINT, FILE_FLAG_BACKUP_SEMANTICS, FILE_FLAG_OPEN_REPARSE_POINT,
        FILE_READ_ATTRIBUTES, FILE_SHARE_READ, FILE_SHARE_WRITE, GetFileInformationByHandle,
        OPEN_ALWAYS, OPEN_EXISTING, READ_CONTROL,
    },
    System::{
        SystemServices::ACCESS_ALLOWED_ACE_TYPE,
        Threading::{GetCurrentProcess, OpenProcessToken},
    },
};

fn invalid(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::PermissionDenied, message)
}
fn raw(h: &OwnedHandle) -> windows_sys::Win32::Foundation::HANDLE {
    h.as_raw_handle() as _
}
fn owned(handle: windows_sys::Win32::Foundation::HANDLE) -> io::Result<OwnedHandle> {
    if handle.is_null() || handle == INVALID_HANDLE_VALUE {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: CreateFileW/OpenProcessToken returned a new owned handle.
    Ok(unsafe { OwnedHandle::from_raw_handle(handle) })
}
fn wide(path: &Path) -> io::Result<Vec<u16>> {
    let mut name: Vec<u16> = path.as_os_str().encode_wide().collect();
    if name.contains(&0) {
        return Err(invalid("profile path contains NUL"));
    }
    name.push(0);
    Ok(name)
}
/// The buffer owns the TOKEN_USER and pointed-to SID; keep it alive while using the SID.
pub fn current_user_sid() -> io::Result<Vec<usize>> {
    let mut token = null_mut();
    if unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) } == 0 {
        return Err(io::Error::last_os_error());
    }
    let token = owned(token)?;
    let mut size = 0;
    unsafe {
        GetTokenInformation(raw(&token), TokenUser, null_mut(), 0, &mut size);
    }
    if size == 0 || size > 65536 {
        return Err(invalid("invalid user token"));
    }
    let mut bytes = vec![0usize; (size as usize).div_ceil(std::mem::size_of::<usize>())];
    if unsafe {
        GetTokenInformation(
            raw(&token),
            TokenUser,
            bytes.as_mut_ptr().cast(),
            size,
            &mut size,
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    let sid = unsafe { (*(bytes.as_ptr().cast::<TOKEN_USER>())).User.Sid };
    if sid.is_null() || unsafe { IsValidSid(sid) } == 0 {
        return Err(invalid("invalid user SID"));
    }
    Ok(bytes)
}
fn info(handle: &OwnedHandle) -> io::Result<BY_HANDLE_FILE_INFORMATION> {
    let mut info = BY_HANDLE_FILE_INFORMATION::default();
    if unsafe { GetFileInformationByHandle(raw(handle), &mut info) } == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(info)
}
fn identity_equal(a: &BY_HANDLE_FILE_INFORMATION, b: &BY_HANDLE_FILE_INFORMATION) -> bool {
    a.dwVolumeSerialNumber == b.dwVolumeSerialNumber
        && a.nFileIndexHigh == b.nFileIndexHigh
        && a.nFileIndexLow == b.nFileIndexLow
}
fn check_security(handle: &OwnedHandle, user: &[usize], protected: bool) -> io::Result<()> {
    let sid = unsafe { (*(user.as_ptr().cast::<TOKEN_USER>())).User.Sid };
    let mut owner = null_mut();
    let mut acl: *mut ACL = null_mut();
    let mut descriptor = null_mut();
    let result = unsafe {
        GetSecurityInfo(
            raw(handle),
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &mut owner,
            null_mut(),
            &mut acl,
            null_mut(),
            &mut descriptor,
        )
    };
    if result != 0 || descriptor.is_null() {
        return Err(io::Error::from_raw_os_error(result as i32));
    }
    struct Descriptor(*mut core::ffi::c_void);
    impl Drop for Descriptor {
        fn drop(&mut self) {
            unsafe {
                LocalFree(self.0);
            }
        }
    }
    let _descriptor = Descriptor(descriptor);
    if owner.is_null() || acl.is_null() || unsafe { EqualSid(owner, sid) } == 0 {
        return Err(invalid("profile owner or ACL not private"));
    }
    let mut control = 0u16;
    let mut revision = 0u32;
    if unsafe { GetSecurityDescriptorControl(descriptor, &mut control, &mut revision) } == 0
        || (protected && control & SE_DACL_PROTECTED == 0)
    {
        return Err(invalid("profile DACL is not protected"));
    }
    let mut size = ACL_SIZE_INFORMATION::default();
    if unsafe {
        GetAclInformation(
            acl,
            (&mut size as *mut ACL_SIZE_INFORMATION).cast(),
            std::mem::size_of::<ACL_SIZE_INFORMATION>() as u32,
            AclSizeInformation,
        )
    } == 0
        || size.AceCount == 0
    {
        return Err(invalid("profile ACL unavailable"));
    }
    for index in 0..size.AceCount {
        let mut entry = null_mut();
        if unsafe { GetAce(acl, index, &mut entry) } == 0 || entry.is_null() {
            return Err(invalid("invalid profile ACE"));
        }
        let ace = unsafe { &*(entry.cast::<ACCESS_ALLOWED_ACE>()) };
        if ace.Header.AceType as u32 != ACCESS_ALLOWED_ACE_TYPE
            || (ace.Header.AceSize as usize) < std::mem::size_of::<ACCESS_ALLOWED_ACE>()
        {
            return Err(invalid("unsupported profile ACE"));
        }
        let ace_sid = (&ace.SidStart as *const u32).cast_mut().cast();
        if unsafe { IsValidSid(ace_sid) } == 0 || unsafe { EqualSid(ace_sid, sid) } == 0 {
            return Err(invalid("profile ACL grants another principal"));
        }
    }
    Ok(())
}

fn endpoint_for(info: &BY_HANDLE_FILE_INFORMATION) -> io::Result<OsString> {
    let index = (u64::from(info.nFileIndexHigh) << 32) | u64::from(info.nFileIndexLow);
    if index == 0 {
        return Err(invalid("profile has no stable file identity"));
    }
    Ok(OsString::from(format!(
        r"\\.\pipe\sentinel-dev-{:08x}-{index:016x}",
        info.dwVolumeSerialNumber
    )))
}

pub struct ProfileGuard {
    _directory: OwnedHandle,
    _lock: OwnedHandle,
    endpoint: OsString,
}
impl ProfileGuard {
    pub fn endpoint(&self) -> &OsStr {
        &self.endpoint
    }
}

pub fn acquire(path: &Path) -> io::Result<ProfileGuard> {
    if !path.is_absolute() {
        return Err(invalid("profile must be absolute"));
    }
    let user = current_user_sid()?;
    let name = wide(path)?;
    let directory = owned(unsafe {
        CreateFileW(
            name.as_ptr(),
            READ_CONTROL | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            null(),
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            null_mut(),
        )
    })?;
    let d = info(&directory)?;
    if d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY == 0
        || d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT != 0
    {
        return Err(invalid("profile must be a real directory"));
    }
    check_security(&directory, &user, true)?;
    let canonical = fs::canonicalize(path)?;
    let temp = fs::canonicalize(std::env::temp_dir())?;
    if canonical.parent() != Some(temp.as_path()) {
        return Err(invalid(
            "profile must be a direct temporary-directory child",
        ));
    }
    let canonical_name = wide(&canonical)?;
    let verify = owned(unsafe {
        CreateFileW(
            canonical_name.as_ptr(),
            READ_CONTROL | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            null(),
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            null_mut(),
        )
    })?;
    if !identity_equal(&d, &info(&verify)?) {
        return Err(invalid("profile identity changed"));
    }
    for entry in fs::read_dir(&canonical)? {
        if entry?.file_name() != OsStr::new("daemon-dev.lock") {
            return Err(invalid("profile contains unexpected files"));
        }
    }
    let lock_path = canonical.join("daemon-dev.lock");
    let lock_name = wide(&lock_path)?;
    let lock = owned(unsafe {
        CreateFileW(
            lock_name.as_ptr(),
            GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
            0,
            null(),
            OPEN_ALWAYS,
            FILE_FLAG_OPEN_REPARSE_POINT,
            null_mut(),
        )
    })?;
    let l = info(&lock)?;
    if l.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || l.nNumberOfLinks != 1
    {
        return Err(invalid("unsafe profile lock"));
    }
    check_security(&lock, &user, false)?;
    let endpoint = endpoint_for(&d)?;
    Ok(ProfileGuard {
        _directory: directory,
        _lock: lock,
        endpoint,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identity_and_endpoint_use_volume_and_file_index() {
        let mut first = BY_HANDLE_FILE_INFORMATION::default();
        first.dwVolumeSerialNumber = 7;
        first.nFileIndexHigh = 1;
        first.nFileIndexLow = 9;
        let mut other = first;
        assert!(identity_equal(&first, &other));
        assert_eq!(
            endpoint_for(&first).unwrap(),
            OsStr::new(r"\\.\pipe\sentinel-dev-00000007-0000000100000009")
        );
        other.nFileIndexLow += 1;
        assert!(!identity_equal(&first, &other));
        assert_ne!(endpoint_for(&first).unwrap(), endpoint_for(&other).unwrap());
        first.nFileIndexHigh = 0;
        first.nFileIndexLow = 0;
        assert!(endpoint_for(&first).is_err());
    }
}
