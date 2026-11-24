//! Disposable Windows development profile ownership; no production profile access.
use sentinel_daemon_dev::provider::Config;
use serde::{Deserialize, Serialize};
use std::{
    ffi::{OsStr, OsString},
    fs,
    io::{self, Read, Write},
    os::windows::{
        ffi::OsStrExt,
        io::{AsRawHandle, FromRawHandle, OwnedHandle},
    },
    path::{Path, PathBuf},
    ptr::{null, null_mut},
};
use windows_sys::Win32::{
    Foundation::{GENERIC_READ, GENERIC_WRITE, INVALID_HANDLE_VALUE, LocalFree},
    Security::{
        ACCESS_ALLOWED_ACE, ACL, ACL_REVISION, ACL_SIZE_INFORMATION, AclSizeInformation,
        AddAccessAllowedAce,
        Authorization::{GetSecurityInfo, SE_FILE_OBJECT},
        DACL_SECURITY_INFORMATION, EqualSid, GetAce, GetAclInformation, GetLengthSid,
        GetSecurityDescriptorControl, GetTokenInformation, InitializeAcl,
        InitializeSecurityDescriptor, IsValidSid, OWNER_SECURITY_INFORMATION, SE_DACL_PROTECTED,
        SECURITY_ATTRIBUTES, SECURITY_DESCRIPTOR, SetSecurityDescriptorControl,
        SetSecurityDescriptorDacl, SetSecurityDescriptorOwner, TOKEN_QUERY, TOKEN_USER, TokenUser,
    },
    Storage::FileSystem::{
        BY_HANDLE_FILE_INFORMATION, CREATE_NEW, CreateDirectoryW, CreateFileW, FILE_ALL_ACCESS,
        FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_REPARSE_POINT, FILE_FLAG_BACKUP_SEMANTICS,
        FILE_FLAG_OPEN_REPARSE_POINT, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, FILE_SHARE_WRITE,
        FILE_TYPE_DISK, GetFileInformationByHandle, GetFileType, MOVEFILE_REPLACE_EXISTING,
        MOVEFILE_WRITE_THROUGH, MoveFileExW, OPEN_ALWAYS, OPEN_EXISTING, READ_CONTROL,
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

pub fn with_owner_only_security<T>(
    current: &[usize],
    operation: impl FnOnce(&SECURITY_ATTRIBUTES) -> io::Result<T>,
) -> io::Result<T> {
    // Keep the owner SID and ACL buffers live until the object is created.
    let sid = unsafe { (*(current.as_ptr().cast::<TOKEN_USER>())).User.Sid };
    if unsafe { IsValidSid(sid) } == 0 {
        return Err(invalid("invalid owner SID"));
    }
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
        return Err(io::Error::last_os_error());
    }
    let attributes = SECURITY_ATTRIBUTES {
        nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
        lpSecurityDescriptor: sd,
        bInheritHandle: 0,
    };
    operation(&attributes)
}

const MODEL_FILE: &str = "daemon-dev-model.json";
const MODEL_PENDING: &str = "daemon-dev-model.pending";
const MAX_MODEL_FILE_BYTES: u64 = 4096;
const CHAT_FILE: &str = "daemon-dev-chat.json";
const CHAT_PENDING: &str = "daemon-dev-chat.pending";
pub(crate) const MAX_CHAT_FILE_BYTES: u64 = 8 * 1024 * 1024;
const MAX_EXPORT_FILE_BYTES: u64 = MAX_CHAT_FILE_BYTES + MAX_MODEL_FILE_BYTES + 1024;
const EXPORT_FILE: &str = "sentinel-windows-dev-profile.json";
const EXPORT_PENDING: &str = ".sentinel-windows-dev-profile.pending";

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SavedModel {
    version: u32,
    pub endpoint: String,
    pub provider_id: String,
    pub model: String,
}

fn validate_private_file(handle: &OwnedHandle, user: &[usize]) -> io::Result<()> {
    let metadata = info(handle)?;
    if metadata.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || metadata.nNumberOfLinks != 1
        || unsafe { GetFileType(raw(handle)) } != FILE_TYPE_DISK
    {
        return Err(invalid("unsafe Windows profile file"));
    }
    check_security(handle, user, true)
}

fn open_private_file(path: &Path, user: &[usize]) -> io::Result<Option<OwnedHandle>> {
    let name = wide(path)?;
    let handle = match owned(unsafe {
        CreateFileW(
            name.as_ptr(),
            GENERIC_READ | READ_CONTROL,
            0,
            null(),
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,
            null_mut(),
        )
    }) {
        Ok(handle) => handle,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(error),
    };
    validate_private_file(&handle, user)?;
    Ok(Some(handle))
}

fn valid_model(record: &SavedModel) -> bool {
    record.version == 1
        && Config::valid_persisted_selection(&record.endpoint, &record.provider_id, &record.model)
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
    canonical: PathBuf,
    user: Vec<usize>,
}
impl ProfileGuard {
    pub fn endpoint(&self) -> &OsStr {
        &self.endpoint
    }

    pub fn load_model(&self) -> io::Result<Option<SavedModel>> {
        let Some(bytes) = self.load_private(MODEL_FILE, MAX_MODEL_FILE_BYTES)? else {
            return Ok(None);
        };
        let record = serde_json::from_slice::<SavedModel>(&bytes)?;
        if !valid_model(&record) {
            return Err(invalid("invalid Windows model selection binding"));
        }
        Ok(Some(record))
    }

    /// Update only the profile-owned selection file, under the daemon's profile lock.
    pub fn save_model(&self, config: &Config, model: &str) -> io::Result<()> {
        self.save_model_record(&SavedModel {
            version: 1,
            endpoint: config.endpoint.clone(),
            provider_id: config.provider_id.clone(),
            model: model.to_owned(),
        })
    }

    fn save_model_record(&self, record: &SavedModel) -> io::Result<()> {
        let bytes = serde_json::to_vec(record)?;
        if bytes.len() as u64 > MAX_MODEL_FILE_BYTES || !valid_model(record) {
            return Err(invalid("invalid Windows model selection binding"));
        }
        self.save_private(MODEL_FILE, MODEL_PENDING, &bytes, MAX_MODEL_FILE_BYTES)
    }

    pub fn load_chat(&self) -> io::Result<Option<Vec<u8>>> {
        self.load_private(CHAT_FILE, MAX_CHAT_FILE_BYTES)
    }

    pub fn save_chat(&self, bytes: &[u8]) -> io::Result<()> {
        self.save_private(CHAT_FILE, CHAT_PENDING, bytes, MAX_CHAT_FILE_BYTES)
    }

    fn load_private(&self, name: &str, limit: u64) -> io::Result<Option<Vec<u8>>> {
        read_private_file(&self.canonical.join(name), &self.user, limit)
    }

    fn save_private(&self, name: &str, staged: &str, bytes: &[u8], limit: u64) -> io::Result<()> {
        let destination = self.canonical.join(name);
        drop(open_private_file(&destination, &self.user)?);
        publish_private(
            &destination,
            &self.canonical.join(staged),
            &self.user,
            bytes,
            limit,
            true,
        )
    }

    pub fn export_to(&self, destination: &Path, bytes: &[u8]) -> io::Result<PathBuf> {
        let (directory, canonical) = open_private_temp_directory(destination, &self.user)?;
        if identity_equal(&info(&self._directory)?, &info(&directory)?) {
            return Err(invalid(
                "export directory must be separate from the profile",
            ));
        }
        let output = canonical.join(EXPORT_FILE);
        publish_private(
            &output,
            &canonical.join(EXPORT_PENDING),
            &self.user,
            bytes,
            MAX_EXPORT_FILE_BYTES,
            false,
        )?;
        Ok(output)
    }
}

fn read_private_file(path: &Path, user: &[usize], limit: u64) -> io::Result<Option<Vec<u8>>> {
    let Some(handle) = open_private_file(path, user)? else {
        return Ok(None);
    };
    let mut bytes = Vec::new();
    fs::File::from(handle)
        .take(limit + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() as u64 > limit {
        return Err(invalid("Windows profile file exceeds byte limit"));
    }
    Ok(Some(bytes))
}

pub fn load_export(source: &Path) -> io::Result<Vec<u8>> {
    let user = current_user_sid()?;
    let filename = source
        .file_name()
        .ok_or_else(|| invalid("missing profile archive filename"))?;
    if !source.is_absolute() || filename != OsStr::new(EXPORT_FILE) {
        return Err(invalid(
            "expected an absolute Windows development archive path",
        ));
    }
    let parent = source
        .parent()
        .ok_or_else(|| invalid("missing profile archive directory"))?;
    let (_directory, canonical) = open_private_temp_directory(parent, &user)?;
    read_private_file(&canonical.join(filename), &user, MAX_EXPORT_FILE_BYTES)?.ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::NotFound,
            "Windows development archive missing",
        )
    })
}

pub fn restore_export(
    target: &Path,
    model: Option<&SavedModel>,
    chat: Option<&[u8]>,
) -> io::Result<PathBuf> {
    if !target.is_absolute() {
        return Err(invalid("restore profile must be absolute"));
    }
    let name = target
        .file_name()
        .and_then(OsStr::to_str)
        .filter(|name| {
            !name.is_empty()
                && name.len() <= 64
                && name
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'-' | b'_'))
        })
        .ok_or_else(|| {
            invalid("restore profile name must use ASCII letters, digits, hyphens or underscores")
        })?;
    let parent = target
        .parent()
        .ok_or_else(|| invalid("missing restore profile parent"))?;
    let temp = fs::canonicalize(std::env::temp_dir())?;
    if fs::canonicalize(parent)? != temp {
        return Err(invalid(
            "restore profile must be a direct temporary-directory child",
        ));
    }
    let destination = temp.join(name);
    match fs::symlink_metadata(&destination) {
        Ok(_) => {
            return Err(io::Error::new(
                io::ErrorKind::AlreadyExists,
                "restore profile already exists",
            ));
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => return Err(error),
    }
    if model.is_some_and(|record| !valid_model(record))
        || chat.is_some_and(|bytes| bytes.len() as u64 > MAX_CHAT_FILE_BYTES)
    {
        return Err(invalid("invalid Windows development archive"));
    }
    let user = current_user_sid()?;
    let mut stage = None;
    for index in 0..64 {
        let candidate = temp.join(format!(
            ".sentinel-windows-restore-{}-{index}",
            std::process::id()
        ));
        let name = wide(&candidate)?;
        match with_owner_only_security(&user, |attributes| {
            // The security descriptor and path remain live for the synchronous call.
            if unsafe { CreateDirectoryW(name.as_ptr(), attributes) } == 0 {
                Err(io::Error::last_os_error())
            } else {
                Ok(())
            }
        }) {
            Ok(()) => {
                stage = Some(candidate);
                break;
            }
            Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
            Err(error) => return Err(error),
        }
    }
    let stage = stage.ok_or_else(|| invalid("cannot reserve private restore staging directory"))?;
    let mut stage_identity = None;
    let result = (|| {
        let staged = acquire(&stage)?;
        stage_identity = Some(info(&staged._directory)?);
        if let Some(model) = model {
            staged.save_model_record(model)?;
        }
        if let Some(chat) = chat {
            staged.save_chat(chat)?;
        }
        // Release directory and lock handles before the same-volume rename.
        drop(staged);
        let stage_name = wide(&stage)?;
        let destination_name = wide(&destination)?;
        if unsafe {
            MoveFileExW(
                stage_name.as_ptr(),
                destination_name.as_ptr(),
                MOVEFILE_WRITE_THROUGH,
            )
        } == 0
        {
            return Err(io::Error::last_os_error());
        }
        Ok(destination)
    })();
    if result.is_err()
        && let Some(identity) = stage_identity
        && let Ok((directory, canonical)) = open_private_temp_directory(&stage, &user)
        && info(&directory).is_ok_and(|current| identity_equal(&identity, &current))
    {
        for name in [
            "daemon-dev.lock",
            MODEL_FILE,
            MODEL_PENDING,
            CHAT_FILE,
            CHAT_PENDING,
        ] {
            let _ = fs::remove_file(canonical.join(name));
        }
        drop(directory);
        let _ = fs::remove_dir(&canonical);
    }
    result
}

fn publish_private(
    destination: &Path,
    pending: &Path,
    user: &[usize],
    bytes: &[u8],
    limit: u64,
    replace: bool,
) -> io::Result<()> {
    if bytes.len() as u64 > limit {
        return Err(invalid("Windows profile file exceeds byte limit"));
    }
    let pending_name = wide(pending)?;
    let destination_name = wide(destination)?;
    let handle = with_owner_only_security(user, |attributes| {
        owned(unsafe {
            CreateFileW(
                pending_name.as_ptr(),
                GENERIC_WRITE | READ_CONTROL,
                0,
                attributes,
                CREATE_NEW,
                FILE_FLAG_OPEN_REPARSE_POINT,
                null_mut(),
            )
        })
    })?;
    let result = (|| {
        validate_private_file(&handle, user)?;
        let mut file = fs::File::from(handle);
        file.write_all(bytes)?;
        file.sync_all()?;
        drop(file);
        let flags = if replace {
            MOVEFILE_WRITE_THROUGH | MOVEFILE_REPLACE_EXISTING
        } else {
            MOVEFILE_WRITE_THROUGH
        };
        if unsafe { MoveFileExW(pending_name.as_ptr(), destination_name.as_ptr(), flags) } == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(pending);
    }
    result
}

pub fn acquire(path: &Path) -> io::Result<ProfileGuard> {
    acquire_profile(path, true)
}

pub fn acquire_for_inspection(path: &Path) -> io::Result<ProfileGuard> {
    acquire_profile(path, false)
}

fn open_private_temp_directory(path: &Path, user: &[usize]) -> io::Result<(OwnedHandle, PathBuf)> {
    if !path.is_absolute() {
        return Err(invalid("profile must be absolute"));
    }
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
    check_security(&directory, user, true)?;
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
    Ok((directory, canonical))
}

fn acquire_profile(path: &Path, recover: bool) -> io::Result<ProfileGuard> {
    let user = current_user_sid()?;
    let (directory, canonical) = open_private_temp_directory(path, &user)?;
    let d = info(&directory)?;
    for entry in fs::read_dir(&canonical)? {
        let name = entry?.file_name();
        if name != OsStr::new("daemon-dev.lock")
            && name != OsStr::new(MODEL_FILE)
            && name != OsStr::new(MODEL_PENDING)
            && name != OsStr::new(CHAT_FILE)
            && name != OsStr::new(CHAT_PENDING)
        {
            return Err(invalid("profile contains unexpected files"));
        }
    }
    let lock_path = canonical.join("daemon-dev.lock");
    let lock_name = wide(&lock_path)?;
    let (access, disposition) = if recover {
        (GENERIC_READ | GENERIC_WRITE | READ_CONTROL, OPEN_ALWAYS)
    } else {
        (GENERIC_READ | READ_CONTROL, OPEN_EXISTING)
    };
    let open_lock = |attributes| {
        owned(unsafe {
            CreateFileW(
                lock_name.as_ptr(),
                access,
                0,
                attributes,
                disposition,
                FILE_FLAG_OPEN_REPARSE_POINT,
                null_mut(),
            )
        })
    };
    let lock = if recover {
        with_owner_only_security(&user, |attributes| open_lock(attributes))?
    } else {
        open_lock(null())?
    };
    let l = info(&lock)?;
    if l.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || l.nNumberOfLinks != 1
    {
        return Err(invalid("unsafe profile lock"));
    }
    check_security(&lock, &user, false)?;
    for staged in [MODEL_PENDING, CHAT_PENDING] {
        let pending = canonical.join(staged);
        if let Some(handle) = open_private_file(&pending, &user)? {
            if !recover {
                return Err(invalid(
                    "profile contains a pending snapshot; run normal startup to recover it",
                ));
            }
            drop(handle);
            fs::remove_file(&pending)?;
        }
    }
    let endpoint = endpoint_for(&d)?;
    Ok(ProfileGuard {
        _directory: directory,
        _lock: lock,
        endpoint,
        canonical,
        user,
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
