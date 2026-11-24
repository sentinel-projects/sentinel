//! Narrow Unix read-only gateway. The caller owns workspace and permission policy.
use crate::tool_registry::{self, Tool};
use serde_json::{Value, json};
use std::{
    ffi::CString,
    fs::File,
    io::{self, Read, Seek, SeekFrom, Write},
    os::{
        fd::{AsRawFd, FromRawFd, IntoRawFd},
        unix::ffi::OsStrExt,
    },
    path::{Component, Path},
    sync::atomic::{AtomicU64, Ordering},
};

const MAX_READ: usize = 64 * 1024;
pub fn failure_category(error: &io::Error) -> &'static str {
    match error.kind() {
        io::ErrorKind::NotFound => "not-found",
        io::ErrorKind::PermissionDenied => "permission-denied",
        io::ErrorKind::InvalidInput => "invalid-path",
        _ => "unavailable",
    }
}
static NEXT_TEMP: AtomicU64 = AtomicU64::new(0);

fn glob_basename(pattern: &str, name: &str) -> bool {
    let characters: Vec<char> = name.chars().collect();
    let mut previous = vec![false; characters.len() + 1];
    previous[0] = true;
    for token in pattern.chars() {
        let mut next = vec![false; characters.len() + 1];
        if token == '*' {
            next[0] = previous[0];
        }
        for index in 1..=characters.len() {
            next[index] = if token == '*' {
                previous[index] || next[index - 1]
            } else {
                previous[index - 1] && (token == '?' || token == characters[index - 1])
            };
        }
        previous = next;
    }
    previous[characters.len()]
}

#[derive(Debug)]
pub enum MutationError {
    NotCommitted(io::Error),
    CommitUncertain(io::Error),
}

pub fn create_descriptor() -> Value {
    tool_registry::descriptor(Tool::WriteFile).schema()
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[allow(dead_code)] // Ask and deny remain explicit fail-closed policy outcomes.
pub enum PermissionDecision {
    Allow,
    Ask,
    Deny,
}

pub fn descriptor() -> Value {
    tool_registry::descriptor(Tool::ReadFile).schema()
}

pub fn directory_descriptor() -> Value {
    tool_registry::descriptor(Tool::ListDirectory).schema()
}

pub struct Workspace(File);

/// A bounded observation, not an atomic filesystem snapshot or a rollback record.
#[derive(Clone)]
pub struct WorkspaceSnapshot {
    pub root: String,
    pub listed: std::collections::BTreeSet<String>,
    pub contents: std::collections::BTreeMap<String, String>,
    pub complete: bool,
    pub truncated: bool,
    identity: (u64, u64),
}

impl Workspace {
    /// Bounded binary input for an explicitly selected attachment under the held root.
    pub fn read_attachment_bytes(
        &self,
        path: &str,
        max_bytes: usize,
        decision: PermissionDecision,
    ) -> io::Result<Vec<u8>> {
        use std::os::unix::fs::MetadataExt;
        if decision != PermissionDecision::Allow {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "attachment requires authorization",
            ));
        }
        if path.is_empty() || path.len() > 4096 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid attachment path",
            ));
        }
        let parts: Vec<_> = Path::new(path).components().collect();
        if parts.is_empty()
            || parts
                .iter()
                .any(|part| !matches!(part, Component::Normal(_)))
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid attachment path",
            ));
        }
        let mut file = self.0.try_clone()?;
        let mut parent = None;
        for (index, part) in parts.iter().enumerate() {
            let Component::Normal(name) = part else {
                unreachable!()
            };
            let name = CString::new(name.as_bytes())
                .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "invalid path"))?;
            let flags = libc::O_RDONLY
                | libc::O_NONBLOCK
                | libc::O_NOFOLLOW
                | libc::O_CLOEXEC
                | if index + 1 == parts.len() {
                    0
                } else {
                    libc::O_DIRECTORY
                };
            if index + 1 == parts.len() {
                parent = Some((file.try_clone()?, name.clone()));
            }
            let fd = unsafe { libc::openat(file.as_raw_fd(), name.as_ptr(), flags) };
            if fd < 0 {
                return Err(io::Error::last_os_error());
            }
            file = unsafe { File::from_raw_fd(fd) };
        }
        let before = file.metadata()?;
        if !before.is_file() || before.nlink() != 1 || before.len() > max_bytes as u64 {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "unsafe attachment",
            ));
        }
        let mut bytes = Vec::new();
        Read::by_ref(&mut file)
            .take((max_bytes + 1) as u64)
            .read_to_end(&mut bytes)?;
        let after = file.metadata()?;
        let (parent, leaf) = parent.ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidInput, "invalid attachment path")
        })?;
        let mut current = std::mem::MaybeUninit::<libc::stat>::uninit();
        if unsafe {
            libc::fstatat(
                parent.as_raw_fd(),
                leaf.as_ptr(),
                current.as_mut_ptr(),
                libc::AT_SYMLINK_NOFOLLOW,
            )
        } != 0
        {
            return Err(io::Error::last_os_error());
        }
        let current = unsafe { current.assume_init() };
        if bytes.len() > max_bytes
            || bytes.len() as u64 != before.len()
            || before.dev() != after.dev()
            || before.ino() != after.ino()
            || before.len() != after.len()
            || before.mtime() != after.mtime()
            || before.mtime_nsec() != after.mtime_nsec()
            || before.ctime() != after.ctime()
            || before.ctime_nsec() != after.ctime_nsec()
            || after.nlink() != 1
            || current.st_mode & libc::S_IFMT != libc::S_IFREG
            || current.st_nlink != 1
            || current.st_dev as u64 != before.dev()
            || current.st_ino as u64 != before.ino()
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "attachment changed during read",
            ));
        }
        Ok(bytes)
    }
    /// Inspect a single existing file or directory without following links.
    pub fn stat_path(&self, args: &Value, decision: PermissionDecision) -> io::Result<Value> {
        use std::os::unix::fs::MetadataExt;
        let invocation = tool_registry::plan(Tool::StatPath, args)?;
        if decision != PermissionDecision::Allow || !invocation.is_current() {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "stat requires authorization",
            ));
        }
        let path = invocation.arguments()["path"]
            .as_str()
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "invalid path"))?;
        let parts: Vec<_> = if path == "." {
            Vec::new()
        } else {
            Path::new(path).components().collect()
        };
        if path != "."
            && (parts.is_empty() || parts.iter().any(|p| !matches!(p, Component::Normal(_))))
        {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "invalid path"));
        }
        let mut parent = self.0.try_clone()?;
        for part in parts.iter().take(parts.len().saturating_sub(1)) {
            let Component::Normal(name) = part else {
                unreachable!()
            };
            let name = CString::new(name.as_bytes())
                .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "invalid path"))?;
            let fd = unsafe {
                libc::openat(
                    parent.as_raw_fd(),
                    name.as_ptr(),
                    libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
                )
            };
            if fd < 0 {
                return Err(io::Error::last_os_error());
            }
            parent = unsafe { File::from_raw_fd(fd) };
        }
        if parts.is_empty() {
            let metadata = parent.metadata()?;
            return Ok(
                json!({"path":path,"exists":true,"directory":true,"regularFile":false,"size":metadata.size(),"complete":true}),
            );
        }
        let Component::Normal(name) = parts[parts.len() - 1] else {
            unreachable!()
        };
        let name = CString::new(name.as_bytes())
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "invalid path"))?;
        let mut info = std::mem::MaybeUninit::<libc::stat>::uninit();
        if unsafe {
            libc::fstatat(
                parent.as_raw_fd(),
                name.as_ptr(),
                info.as_mut_ptr(),
                libc::AT_SYMLINK_NOFOLLOW,
            )
        } != 0
        {
            return Err(io::Error::last_os_error());
        }
        let info = unsafe { info.assume_init() };
        let mode = info.st_mode & libc::S_IFMT;
        if (mode != libc::S_IFREG && mode != libc::S_IFDIR)
            || (mode == libc::S_IFREG && info.st_nlink != 1)
        {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "unsupported path type",
            ));
        }
        let size = u64::try_from(info.st_size)
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid size"))?;
        Ok(
            json!({"path":path,"exists":true,"directory":mode == libc::S_IFDIR,"regularFile":mode == libc::S_IFREG,"size":size,"complete":true}),
        )
    }
    /// Bounded basename glob subset; the caller authorizes the exact invocation.
    pub fn glob(&self, args: &Value, decision: PermissionDecision) -> io::Result<Value> {
        let invocation = tool_registry::plan(Tool::Glob, args)?;
        if !invocation.is_current() || decision != PermissionDecision::Allow {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "glob requires authorization",
            ));
        }
        let args = invocation.arguments();
        let path = args["path"]
            .as_str()
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "invalid scope"))?;
        if path != "."
            && Path::new(path).components().any(|part| match part {
                Component::Normal(name) => name.as_bytes().starts_with(b"."),
                _ => true,
            })
        {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, "invalid scope"));
        }
        let pattern = args["pattern"]
            .as_str()
            .filter(|p| {
                !p.trim().is_empty()
                    && p.bytes()
                        .all(|b| (0x20..=0x7e).contains(&b) && !b"/\\[]{}".contains(&b))
            })
            .ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidInput, "unsupported glob pattern")
            })?;
        if args["includeHidden"] != false {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "hidden glob unsupported",
            ));
        }
        self.list_directory(&json!({"path":path,"includeHidden":false}), decision)?;
        let inventory = self.inventory(decision)?;
        let root = inventory["root"]
            .as_str()
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid root"))?;
        let mut complete = inventory["truncated"] == false;
        let files = inventory["files"]
            .as_array()
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid inventory"))?;
        let mut matches = Vec::new();
        for file in files {
            let relative = file
                .as_str()
                .and_then(|v| Path::new(v).strip_prefix(root).ok())
                .and_then(Path::to_str)
                .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid file path"))?;
            if path != "." && !Path::new(relative).starts_with(path) {
                continue;
            }
            let name = Path::new(relative)
                .file_name()
                .and_then(|v| v.to_str())
                .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid filename"))?;
            if !glob_basename(pattern, name) {
                continue;
            }
            matches.push(relative.to_owned());
            if matches.len() >= 100 || serde_json::to_vec(&matches)?.len() > 12_000 {
                if serde_json::to_vec(&matches)?.len() > 12_000 {
                    matches.pop();
                }
                complete = false;
                break;
            }
        }
        Ok(
            json!({"scope":path,"root":path,"pattern":pattern,"matches":matches,"recursive":true,"includeHidden":false,"followSymlinks":false,"complete":complete,"truncated":!complete,"cancelled":false,"issues":[]}),
        )
    }
    /// Bounded literal-only subset of grep; caller owns scope and permission policy.
    pub fn grep(&self, args: &Value, decision: PermissionDecision) -> io::Result<Value> {
        let invocation = tool_registry::plan(Tool::Grep, args)?;
        if !invocation.is_current() || decision != PermissionDecision::Allow {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "search requires authorization",
            ));
        }
        let args = invocation.arguments();
        let path = args["path"]
            .as_str()
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "invalid search scope"))?;
        if path != "."
            && Path::new(path).components().any(|part| match part {
                Component::Normal(name) => name.as_bytes().starts_with(b"."),
                _ => true,
            })
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid search scope",
            ));
        }
        let pattern = args["pattern"]
            .as_str()
            .filter(|p| {
                !p.trim().is_empty()
                    && !p
                        .chars()
                        .any(|c| c.is_control() || ".*+?[]{}()|^$\\".contains(c))
            })
            .ok_or_else(|| {
                io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "only literal search patterns are supported",
                )
            })?;
        if args["includeHidden"] != false {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "hidden search is unsupported",
            ));
        }
        // Validate the requested scope through the no-follow traversal before listing.
        self.list_directory(&json!({"path":path,"includeHidden":false}), decision)?;
        let inventory = self.inventory(decision)?;
        let root = inventory["root"]
            .as_str()
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid root"))?;
        let mut complete = inventory["truncated"] == false;
        let mut matches = Vec::new();
        let files = inventory["files"]
            .as_array()
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid inventory"))?;
        for file in files {
            let relative = file
                .as_str()
                .and_then(|v| Path::new(v).strip_prefix(root).ok())
                .and_then(Path::to_str)
                .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid file path"))?;
            if path != "." && !Path::new(relative).starts_with(path) {
                continue;
            }
            let value = match self.read_file(&json!({"path":relative}), decision) {
                Ok(value) if value["complete"] == true => value,
                _ => {
                    complete = false;
                    continue;
                }
            };
            let content = value["content"].as_str().ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidData, "invalid file content")
            })?;
            for (index, line) in content.split('\n').enumerate() {
                if !line.contains(pattern) {
                    continue;
                }
                let preview: String = line.chars().take(512).collect();
                if preview.len() < line.len() {
                    complete = false;
                }
                let item = json!({"path":relative,"line":index + 1,"text":preview,"lineTruncated":preview.len() < line.len()});
                matches.push(item);
                if matches.len() >= 100 || serde_json::to_vec(&matches)?.len() > 16_000 {
                    if serde_json::to_vec(&matches)?.len() > 16_000 {
                        matches.pop();
                    }
                    complete = false;
                    break;
                }
            }
            if matches.len() >= 100 || serde_json::to_vec(&matches)?.len() >= 16_000 {
                break;
            }
        }
        Ok(
            json!({"scope":path,"query":pattern,"matches":matches,"matchCount":matches.len(),"recursive":true,"includeHidden":false,"followSymlinks":false,"complete":complete,"truncated":!complete,"cancelled":false,"issues":[]}),
        )
    }
    /// Compare complete text observations; unreadable or deleted paths are not inferred.
    pub fn review_changes(
        &self,
        before: &WorkspaceSnapshot,
        decision: PermissionDecision,
    ) -> io::Result<Value> {
        let after = self.snapshot(decision)?;
        if before.root != after.root || before.identity != after.identity {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "workspace identity changed",
            ));
        }
        let mut truncated = before.truncated || after.truncated;
        let mut files = Vec::new();
        for (path, content) in &after.contents {
            let old = before.contents.get(path);
            if old.is_none() && (before.listed.contains(path) || !before.complete) {
                truncated = true;
                continue;
            }
            if old.is_some_and(|text| text == content) {
                continue;
            }
            let previous = old.map(String::as_str).unwrap_or("");
            let lines = |text: &str| {
                let mut result: Vec<_> = text.split('\n').map(str::to_owned).collect();
                if text.is_empty() || text.ends_with('\n') {
                    result.pop();
                }
                result
            };
            let old_lines = lines(previous);
            let new_lines = lines(content);
            let mut diff = format!(
                "--- {}\n+++ b/{}\n@@ -{},{} +{},{} @@\n",
                if old.is_some() {
                    format!("a/{path}")
                } else {
                    "/dev/null".to_owned()
                },
                path,
                usize::from(!old_lines.is_empty()),
                old_lines.len(),
                usize::from(!new_lines.is_empty()),
                new_lines.len()
            );
            for line in old_lines {
                diff.push('-');
                diff.push_str(&line);
                diff.push('\n');
            }
            if !previous.is_empty() && !previous.ends_with('\n') {
                diff.push_str("\\ No newline at end of file\n");
            }
            for line in new_lines {
                diff.push('+');
                diff.push_str(&line);
                diff.push('\n');
            }
            if !content.is_empty() && !content.ends_with('\n') {
                diff.push_str("\\ No newline at end of file\n");
            }
            let item = json!({"path":path,"state":"Applied","kind":if old.is_some() {"modified"} else {"created"},"diff":diff});
            files.push(item);
            if serde_json::to_vec(&files)?.len() > 100_000 {
                files.pop();
                truncated = true;
                break;
            }
        }
        // A missing baseline file may be deleted or unreadable; neither is a confirmed deletion.
        if before
            .contents
            .keys()
            .any(|path| !after.contents.contains_key(path))
        {
            truncated = true;
        }
        Ok(
            json!({"available":true,"files":files,"truncated":truncated,"root":before.root,"reason":"Observed changes since this run began; concurrent external edits may be included. Deleted, binary, hidden, oversized and unreadable files may be omitted."}),
        )
    }
    pub fn snapshot(&self, decision: PermissionDecision) -> io::Result<WorkspaceSnapshot> {
        let inventory = self.inventory(decision)?;
        let root = inventory["root"]
            .as_str()
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid inventory root"))?
            .to_owned();
        let complete = inventory["truncated"] == false;
        use std::os::unix::fs::MetadataExt;
        let metadata = self.0.metadata()?;
        let mut snapshot = WorkspaceSnapshot {
            root: root.clone(),
            listed: Default::default(),
            contents: Default::default(),
            complete,
            truncated: !complete,
            identity: (metadata.dev(), metadata.ino()),
        };
        let files = inventory["files"]
            .as_array()
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid inventory"))?;
        for file in files {
            let path = file
                .as_str()
                .and_then(|s| Path::new(s).strip_prefix(&root).ok())
                .and_then(Path::to_str)
                .ok_or_else(|| {
                    io::Error::new(io::ErrorKind::InvalidData, "invalid inventory path")
                })?;
            snapshot.listed.insert(path.to_owned());
        }
        let mut budget = 32_768usize;
        for path in &snapshot.listed {
            if snapshot.contents.len() >= 32 || budget == 0 {
                snapshot.truncated = true;
                break;
            }
            let read = match self.read_file(&json!({"path":path}), decision) {
                Ok(value) => value,
                Err(_) => {
                    snapshot.truncated = true;
                    continue;
                }
            };
            let Some(content) = read["content"].as_str() else {
                snapshot.truncated = true;
                continue;
            };
            if read["complete"] != true || content.len() > 8192 || content.len() > budget {
                snapshot.truncated = true;
                continue;
            }
            budget -= content.len();
            snapshot.contents.insert(path.clone(), content.to_owned());
        }
        if self.inventory_root()? != Path::new(&root) {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "workspace identity changed",
            ));
        }
        Ok(snapshot)
    }
    pub(crate) fn inventory_root(&self) -> io::Result<std::path::PathBuf> {
        use std::os::unix::fs::MetadataExt;
        #[cfg(target_os = "macos")]
        let path = {
            let mut buffer = [0i8; 1024];
            if unsafe { libc::fcntl(self.0.as_raw_fd(), libc::F_GETPATH, buffer.as_mut_ptr()) } < 0
            {
                return Err(io::Error::last_os_error());
            }
            let bytes = unsafe { std::ffi::CStr::from_ptr(buffer.as_ptr()) }.to_bytes();
            std::path::PathBuf::from(std::ffi::OsStr::from_bytes(bytes))
        };
        #[cfg(target_os = "linux")]
        let path = std::fs::read_link(format!("/proc/self/fd/{}", self.0.as_raw_fd()))?;
        let expected = self.0.metadata()?;
        let actual = std::fs::metadata(&path)?;
        if !path.is_absolute()
            || path.to_str().is_none()
            || !actual.is_dir()
            || expected.dev() != actual.dev()
            || expected.ino() != actual.ino()
        {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "workspace identity changed",
            ));
        }
        Ok(path)
    }

    /// Enumerate regular files beneath the authorized held root, without following links.
    pub fn inventory(&self, decision: PermissionDecision) -> io::Result<Value> {
        if decision != PermissionDecision::Allow {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "workspace inventory requires authorization",
            ));
        }
        let root = self.inventory_root()?;
        let mut pending = vec![(String::from("."), 0usize)];
        let mut visited = 0usize;
        let mut files = Vec::new();
        let mut bytes = 0usize;
        let mut truncated = false;
        while let Some((dir, depth)) = pending.pop() {
            visited += 1;
            let listing =
                match self.list_directory(&json!({"path":dir,"includeHidden":false}), decision) {
                    Ok(value) => value,
                    Err(error) if visited == 1 => return Err(error),
                    Err(_) => {
                        truncated = true;
                        continue;
                    }
                };
            if listing["complete"] != true {
                truncated = true;
            }
            let mut entries = listing["entries"]
                .as_array()
                .ok_or_else(|| {
                    io::Error::new(io::ErrorKind::InvalidData, "invalid directory listing")
                })?
                .clone();
            entries.sort_by(|a, b| a["name"].as_str().cmp(&b["name"].as_str()));
            for entry in entries {
                let name = entry["name"].as_str().ok_or_else(|| {
                    io::Error::new(io::ErrorKind::InvalidData, "invalid directory entry")
                })?;
                let relative = if dir == "." {
                    name.to_owned()
                } else {
                    format!("{dir}/{name}")
                };
                if relative.len() > 4096 {
                    truncated = true;
                    continue;
                }
                match entry["type"].as_str() {
                    Some("file") => {
                        let absolute = root.join(&relative);
                        let text = absolute.to_str().ok_or_else(|| {
                            io::Error::new(io::ErrorKind::InvalidData, "non-UTF-8 workspace path")
                        })?;
                        if files.len() >= 100 || bytes.saturating_add(text.len()) > 24_000 {
                            truncated = true;
                            continue;
                        }
                        bytes += text.len();
                        files.push(text.to_owned());
                    }
                    Some("directory") if depth < 32 && visited + pending.len() < 128 => {
                        pending.push((relative, depth + 1))
                    }
                    _ => truncated = true,
                }
            }
        }
        if self.inventory_root()? != root {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "workspace identity changed",
            ));
        }
        Ok(json!({
            "files": files,
            "truncated": truncated,
            "root": root.to_str().ok_or_else(|| io::Error::new(
                io::ErrorKind::InvalidData,
                "non-UTF-8 workspace",
            ))?
        }))
    }
    #[cfg(target_os = "macos")]
    pub fn process_root(&self) -> io::Result<(String, std::os::fd::RawFd)> {
        use std::os::unix::fs::MetadataExt;
        let mut buffer = [0i8; 1024];
        if unsafe { libc::fcntl(self.0.as_raw_fd(), libc::F_GETPATH, buffer.as_mut_ptr()) } < 0 {
            return Err(io::Error::last_os_error());
        }
        let path = unsafe { std::ffi::CStr::from_ptr(buffer.as_ptr()) }
            .to_str()
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "non-UTF-8 workspace"))?;
        if path.chars().any(char::is_control) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid workspace path",
            ));
        }
        let expected = self.0.metadata()?;
        let actual = std::fs::metadata(path)?;
        if expected.dev() != actual.dev() || expected.ino() != actual.ino() || !actual.is_dir() {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "workspace identity changed",
            ));
        }
        Ok((path.to_owned(), self.0.as_raw_fd()))
    }
    /// The caller must have independently authorized this workspace root.
    pub fn open(root: &Path) -> io::Result<Self> {
        let name = CString::new(root.as_os_str().as_bytes())
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "invalid root"))?;
        let fd = unsafe {
            libc::open(
                name.as_ptr(),
                libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
            )
        };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(Self(unsafe { File::from_raw_fd(fd) }))
    }

    /// Create one new file only. The caller must audit and approve this exact invocation.
    /// Replace one existing file only when its full preimage still matches. Cooperating
    /// same-user processes must not race the final check and rename.
    pub fn edit_file(
        &self,
        args: &Value,
        decision: PermissionDecision,
    ) -> Result<Value, MutationError> {
        use std::os::unix::fs::MetadataExt;
        let fail = MutationError::NotCommitted;
        let invalid = || io::Error::new(io::ErrorKind::InvalidInput, "invalid edit-file arguments");
        let invocation = tool_registry::plan(Tool::EditFile, args).map_err(fail)?;
        if !invocation.is_current() || decision != PermissionDecision::Allow {
            return Err(fail(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "edit requires approval",
            )));
        }
        let values = invocation.arguments();
        let path = values["path"].as_str().ok_or_else(|| fail(invalid()))?;
        let old = values["oldString"]
            .as_str()
            .ok_or_else(|| fail(invalid()))?;
        let new = values["newString"]
            .as_str()
            .ok_or_else(|| fail(invalid()))?;
        if values["replaceAll"] != false || old == new {
            return Err(fail(invalid()));
        }
        let parts: Vec<_> = Path::new(path).components().collect();
        if parts.is_empty() || parts.iter().any(|p| !matches!(p, Component::Normal(_))) {
            return Err(fail(invalid()));
        }
        let mut parent = self.0.try_clone().map_err(fail)?;
        for part in &parts[..parts.len() - 1] {
            let Component::Normal(name) = part else {
                unreachable!()
            };
            let name = CString::new(name.as_bytes()).map_err(|_| fail(invalid()))?;
            let fd = unsafe {
                libc::openat(
                    parent.as_raw_fd(),
                    name.as_ptr(),
                    libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
                )
            };
            if fd < 0 {
                return Err(fail(io::Error::last_os_error()));
            }
            parent = unsafe { File::from_raw_fd(fd) };
        }
        let Component::Normal(leaf) = parts[parts.len() - 1] else {
            unreachable!()
        };
        let leaf = CString::new(leaf.as_bytes()).map_err(|_| fail(invalid()))?;
        let fd = unsafe {
            libc::openat(
                parent.as_raw_fd(),
                leaf.as_ptr(),
                libc::O_RDONLY | libc::O_NONBLOCK | libc::O_NOFOLLOW | libc::O_CLOEXEC,
            )
        };
        if fd < 0 {
            return Err(fail(io::Error::last_os_error()));
        }
        let mut original = unsafe { File::from_raw_fd(fd) };
        let meta = original.metadata().map_err(fail)?;
        if !meta.is_file()
            || meta.nlink() != 1
            || meta.uid() != unsafe { libc::geteuid() }
            || meta.len() > MAX_READ as u64
            || meta.mode() & 0o6000 != 0
        {
            return Err(fail(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "unsafe edit target",
            )));
        }
        let mut bytes = Vec::new();
        Read::by_ref(&mut original)
            .take((MAX_READ + 1) as u64)
            .read_to_end(&mut bytes)
            .map_err(fail)?;
        if bytes != old.as_bytes() {
            return Err(fail(io::Error::new(
                io::ErrorKind::InvalidData,
                "file preimage changed",
            )));
        }
        let mut staged = None;
        for _ in 0..32 {
            let name = CString::new(format!(
                ".sentinel-{}-{}",
                std::process::id(),
                NEXT_TEMP.fetch_add(1, Ordering::Relaxed)
            ))
            .expect("generated name");
            let fd = unsafe {
                libc::openat(
                    parent.as_raw_fd(),
                    name.as_ptr(),
                    libc::O_WRONLY
                        | libc::O_CREAT
                        | libc::O_EXCL
                        | libc::O_NOFOLLOW
                        | libc::O_CLOEXEC,
                    0o600,
                )
            };
            if fd >= 0 {
                staged = Some((name, unsafe { File::from_raw_fd(fd) }));
                break;
            }
            if io::Error::last_os_error().kind() != io::ErrorKind::AlreadyExists {
                return Err(fail(io::Error::last_os_error()));
            }
        }
        let (temp, mut file) = staged.ok_or_else(|| {
            fail(io::Error::new(
                io::ErrorKind::AlreadyExists,
                "temporary names exhausted",
            ))
        })?;
        let prepare = file.write_all(new.as_bytes()).and_then(|_| {
            if unsafe { libc::fchmod(file.as_raw_fd(), (meta.mode() & 0o777) as libc::mode_t) } != 0
            {
                Err(io::Error::last_os_error())
            } else {
                file.sync_all()
            }
        });
        let recheck = prepare.and_then(|_| {
            original.seek(SeekFrom::Start(0))?;
            let mut current = Vec::new();
            Read::by_ref(&mut original)
                .take((MAX_READ + 1) as u64)
                .read_to_end(&mut current)?;
            let mut stat = std::mem::MaybeUninit::<libc::stat>::uninit();
            if unsafe {
                libc::fstatat(
                    parent.as_raw_fd(),
                    leaf.as_ptr(),
                    stat.as_mut_ptr(),
                    libc::AT_SYMLINK_NOFOLLOW,
                )
            } != 0
            {
                return Err(io::Error::last_os_error());
            }
            let stat = unsafe { stat.assume_init() };
            if current != old.as_bytes()
                || stat.st_dev as u64 != meta.dev()
                || stat.st_ino as u64 != meta.ino()
                || stat.st_nlink != 1
                || stat.st_uid != meta.uid()
                || (stat.st_mode as u32 & 0o7777) != (meta.mode() & 0o7777)
                || stat.st_mode & libc::S_IFMT != libc::S_IFREG
            {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "file preimage changed",
                ));
            }
            Ok(())
        });
        if let Err(error) = recheck {
            if unsafe { libc::unlinkat(parent.as_raw_fd(), temp.as_ptr(), 0) } != 0 {
                return Err(MutationError::CommitUncertain(io::Error::last_os_error()));
            }
            return Err(fail(error));
        }
        if unsafe {
            libc::renameat(
                parent.as_raw_fd(),
                temp.as_ptr(),
                parent.as_raw_fd(),
                leaf.as_ptr(),
            )
        } != 0
        {
            let error = io::Error::last_os_error();
            if unsafe { libc::unlinkat(parent.as_raw_fd(), temp.as_ptr(), 0) } != 0 {
                return Err(MutationError::CommitUncertain(io::Error::last_os_error()));
            }
            return Err(fail(error));
        }
        parent.sync_all().map_err(MutationError::CommitUncertain)?;
        Ok(
            json!({"path":path,"content":"File modified after exact preimage verification.","complete":true}),
        )
    }

    /// Create one new file only. The caller must audit and approve this exact invocation.
    pub fn create_file(
        &self,
        args: &Value,
        decision: PermissionDecision,
    ) -> Result<Value, MutationError> {
        let fail = |e| MutationError::NotCommitted(e);
        let invalid =
            || io::Error::new(io::ErrorKind::InvalidInput, "invalid create-file arguments");
        let invocation = tool_registry::plan(Tool::WriteFile, args).map_err(fail)?;
        if !invocation.is_current() {
            return Err(fail(invalid()));
        }
        let object = invocation
            .arguments()
            .as_object()
            .ok_or_else(|| fail(invalid()))?;
        let path = object
            .get("path")
            .and_then(Value::as_str)
            .filter(|s| !s.is_empty())
            .ok_or_else(|| fail(invalid()))?;
        let content = object
            .get("content")
            .and_then(Value::as_str)
            .filter(|s| s.len() <= MAX_READ)
            .ok_or_else(|| fail(invalid()))?;
        if decision != PermissionDecision::Allow {
            return Err(fail(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "write-file requires approval",
            )));
        }
        let parts: Vec<_> = Path::new(path).components().collect();
        if parts.is_empty() || parts.iter().any(|p| !matches!(p, Component::Normal(_))) {
            return Err(fail(invalid()));
        }
        let mut parent = self.0.try_clone().map_err(fail)?;
        for part in &parts[..parts.len() - 1] {
            let Component::Normal(part) = part else {
                unreachable!()
            };
            let name = CString::new(part.as_bytes()).map_err(|_| fail(invalid()))?;
            let fd = unsafe {
                libc::openat(
                    parent.as_raw_fd(),
                    name.as_ptr(),
                    libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
                )
            };
            if fd < 0 {
                return Err(fail(io::Error::last_os_error()));
            }
            parent = unsafe { File::from_raw_fd(fd) };
        }
        let Component::Normal(leaf) = parts[parts.len() - 1] else {
            unreachable!()
        };
        let leaf = CString::new(leaf.as_bytes()).map_err(|_| fail(invalid()))?;
        let mut created = None;
        for _ in 0..32 {
            let temp = CString::new(format!(
                ".sentinel-{}-{}",
                std::process::id(),
                NEXT_TEMP.fetch_add(1, Ordering::Relaxed)
            ))
            .expect("generated name");
            let fd = unsafe {
                libc::openat(
                    parent.as_raw_fd(),
                    temp.as_ptr(),
                    libc::O_WRONLY
                        | libc::O_CREAT
                        | libc::O_EXCL
                        | libc::O_NOFOLLOW
                        | libc::O_CLOEXEC,
                    0o600,
                )
            };
            if fd >= 0 {
                created = Some((temp, unsafe { File::from_raw_fd(fd) }));
                break;
            }
            let error = io::Error::last_os_error();
            if error.kind() != io::ErrorKind::AlreadyExists {
                return Err(fail(error));
            }
        }
        let (temp, mut file) = created.ok_or_else(|| {
            fail(io::Error::new(
                io::ErrorKind::AlreadyExists,
                "temporary names exhausted",
            ))
        })?;
        let prepared = file
            .write_all(content.as_bytes())
            .and_then(|_| file.sync_all());
        if let Err(error) = prepared {
            if unsafe { libc::unlinkat(parent.as_raw_fd(), temp.as_ptr(), 0) } != 0 {
                return Err(MutationError::CommitUncertain(io::Error::last_os_error()));
            }
            return Err(fail(error));
        }
        if unsafe {
            libc::linkat(
                parent.as_raw_fd(),
                temp.as_ptr(),
                parent.as_raw_fd(),
                leaf.as_ptr(),
                0,
            )
        } != 0
        {
            let error = io::Error::last_os_error();
            if unsafe { libc::unlinkat(parent.as_raw_fd(), temp.as_ptr(), 0) } != 0 {
                return Err(MutationError::CommitUncertain(io::Error::last_os_error()));
            }
            return Err(fail(error));
        }
        if unsafe { libc::unlinkat(parent.as_raw_fd(), temp.as_ptr(), 0) } != 0 {
            return Err(MutationError::CommitUncertain(io::Error::last_os_error()));
        }
        parent.sync_all().map_err(MutationError::CommitUncertain)?;
        Ok(json!({"path":path,"created":true,"bytes":content.len()}))
    }

    pub fn list_directory(&self, args: &Value, decision: PermissionDecision) -> io::Result<Value> {
        let invalid = || {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid list-directory arguments",
            )
        };
        let invocation = tool_registry::plan(Tool::ListDirectory, args)?;
        if !invocation.is_current() {
            return Err(invalid());
        }
        let object = invocation.arguments().as_object().ok_or_else(invalid)?;
        let path = object
            .get("path")
            .and_then(Value::as_str)
            .filter(|s| !s.is_empty())
            .ok_or_else(invalid)?;
        let hidden = match object.get("includeHidden") {
            None => false,
            Some(value) => value.as_bool().ok_or_else(invalid)?,
        };
        if decision != PermissionDecision::Allow {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "list-directory requires authorization",
            ));
        }
        let parts: Vec<_> = if path == "." {
            Vec::new()
        } else {
            Path::new(path).components().collect()
        };
        if path != "."
            && (parts.is_empty() || parts.iter().any(|p| !matches!(p, Component::Normal(_))))
        {
            return Err(invalid());
        }
        // Opening "." gives this enumeration its own directory offset.
        let dot = c".";
        let fd = unsafe {
            libc::openat(
                self.0.as_raw_fd(),
                dot.as_ptr(),
                libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
            )
        };
        if fd < 0 {
            return Err(io::Error::last_os_error());
        }
        let mut dir = unsafe { File::from_raw_fd(fd) };
        for part in parts {
            let Component::Normal(part) = part else {
                unreachable!()
            };
            let name = CString::new(part.as_bytes()).map_err(|_| invalid())?;
            let fd = unsafe {
                libc::openat(
                    dir.as_raw_fd(),
                    name.as_ptr(),
                    libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
                )
            };
            if fd < 0 {
                return Err(io::Error::last_os_error());
            }
            dir = unsafe { File::from_raw_fd(fd) };
        }
        let fd = dir.into_raw_fd();
        let stream = unsafe { libc::fdopendir(fd) };
        if stream.is_null() {
            let error = io::Error::last_os_error();
            unsafe {
                libc::close(fd);
            }
            return Err(error);
        }
        struct Directory(*mut libc::DIR);
        impl Drop for Directory {
            fn drop(&mut self) {
                unsafe {
                    libc::closedir(self.0);
                }
            }
        }
        let stream = Directory(stream);
        let mut entries = Vec::new();
        let mut complete = true;
        let mut scanned = 0usize;
        loop {
            #[cfg(target_os = "macos")]
            unsafe {
                *libc::__error() = 0;
            }
            #[cfg(target_os = "linux")]
            unsafe {
                *libc::__errno_location() = 0;
            }
            let entry = unsafe { libc::readdir(stream.0) };
            if entry.is_null() {
                let error = io::Error::last_os_error();
                if error.raw_os_error() != Some(0) {
                    return Err(error);
                }
                break;
            }
            scanned += 1;
            if scanned > 4096 {
                complete = false;
                break;
            }
            let name = unsafe { std::ffi::CStr::from_ptr((*entry).d_name.as_ptr()) };
            let bytes = name.to_bytes();
            if bytes == b"." || bytes == b".." || (!hidden && bytes.starts_with(b".")) {
                continue;
            }
            if entries.len() == 500 {
                complete = false;
                break;
            }
            let name_text = name.to_str().map_err(|_| {
                io::Error::new(io::ErrorKind::InvalidData, "non-UTF-8 directory entry")
            })?;
            let mut info = std::mem::MaybeUninit::<libc::stat>::uninit();
            if unsafe {
                libc::fstatat(
                    libc::dirfd(stream.0),
                    name.as_ptr(),
                    info.as_mut_ptr(),
                    libc::AT_SYMLINK_NOFOLLOW,
                )
            } != 0
            {
                return Err(io::Error::last_os_error());
            }
            let mode = unsafe { info.assume_init() }.st_mode & libc::S_IFMT;
            let kind = if mode == libc::S_IFDIR {
                "directory"
            } else if mode == libc::S_IFREG {
                "file"
            } else {
                "other"
            };
            entries.push(json!({"name":name_text,"type":kind}));
        }
        Ok(
            json!({"path":path,"scope":path,"entries":entries,"complete":complete,
            "truncated":!complete,"cancelled":false,"permissionLimited":false,"issues":[],
            "hiddenEntriesPolicy":if hidden {"included"} else {"excluded"},
            "recursive":false,"followSymlinks":false,"skippedSymlinks":0,"maxDepth":1,"resultLimit":500}),
        )
    }

    pub fn read_file(&self, args: &Value, decision: PermissionDecision) -> io::Result<Value> {
        let invalid = || io::Error::new(io::ErrorKind::InvalidInput, "invalid read-file arguments");
        let invocation = tool_registry::plan(Tool::ReadFile, args)?;
        if !invocation.is_current() {
            return Err(invalid());
        }
        let object = invocation.arguments().as_object().ok_or_else(invalid)?;
        let path = object
            .get("path")
            .and_then(Value::as_str)
            .filter(|s| !s.is_empty())
            .ok_or_else(invalid)?;
        let positive = |key: &str, default: u64| -> io::Result<usize> {
            let value = match object.get(key) {
                Some(value) => value.as_u64().filter(|n| *n > 0).ok_or_else(invalid)?,
                None => default,
            };
            usize::try_from(value).map_err(|_| invalid())
        };
        let offset = positive("offset", 1)?;
        let limit = positive("limit", 2000)?;
        if decision != PermissionDecision::Allow {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "read-file requires authorization",
            ));
        }
        let parts: Vec<_> = Path::new(path).components().collect();
        if parts.is_empty() || parts.iter().any(|p| !matches!(p, Component::Normal(_))) {
            return Err(invalid());
        }
        let mut dir = self.0.try_clone()?;
        for (index, part) in parts.iter().enumerate() {
            let Component::Normal(part) = part else {
                unreachable!()
            };
            let name = CString::new(part.as_bytes()).map_err(|_| invalid())?;
            let last = index + 1 == parts.len();
            let flags = libc::O_RDONLY
                | libc::O_CLOEXEC
                | libc::O_NOFOLLOW
                | libc::O_NONBLOCK
                | if last { 0 } else { libc::O_DIRECTORY };
            let fd = unsafe { libc::openat(dir.as_raw_fd(), name.as_ptr(), flags) };
            if fd < 0 {
                return Err(io::Error::last_os_error());
            }
            dir = unsafe { File::from_raw_fd(fd) };
        }
        use std::os::unix::fs::MetadataExt;
        let metadata = dir.metadata()?;
        if !metadata.is_file() || metadata.nlink() != 1 {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "unsafe file",
            ));
        }
        let mut bytes = Vec::new();
        dir.take((MAX_READ + 1) as u64).read_to_end(&mut bytes)?;
        let complete_read = bytes.len() <= MAX_READ;
        bytes.truncate(MAX_READ);
        if bytes.contains(&0) {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "binary file"));
        }
        if let Err(error) = std::str::from_utf8(&bytes) {
            if !complete_read && error.error_len().is_none() {
                bytes.truncate(error.valid_up_to());
            } else {
                return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid UTF-8"));
            }
        }
        let text = std::str::from_utf8(&bytes)
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid UTF-8"))?;
        let lines: Vec<_> = text.split('\n').collect();
        let start = offset.saturating_sub(1).min(lines.len());
        let end = start.saturating_add(limit).min(lines.len());
        let mut shortened = false;
        let page: Vec<String> = lines[start..end]
            .iter()
            .map(|line| {
                let s: String = line.chars().take(2000).collect();
                shortened |= s.len() < line.len();
                s
            })
            .collect();
        Ok(
            json!({"path":path,"content":page.join("\n"),"offset":offset,"returnedLines":page.len(),"complete":complete_read && offset == 1 && end == lines.len() && !shortened}),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs, os::unix::fs::symlink};
    #[test]
    fn snapshot_review_reports_only_observed_changes() {
        let root = std::env::temp_dir().join(format!("sentinel-changes-{}", std::process::id()));
        fs::create_dir(&root).unwrap();
        fs::write(root.join("old"), "before\n").unwrap();
        fs::write(root.join("removed"), "old").unwrap();
        fs::write(root.join("newlines"), "line\r\n").unwrap();
        let workspace = Workspace::open(&root).unwrap();
        let baseline = workspace.snapshot(PermissionDecision::Allow).unwrap();
        fs::write(root.join("old"), "after\n").unwrap();
        fs::write(root.join("new"), "new\n").unwrap();
        fs::write(root.join("newlines"), "line\n").unwrap();
        fs::remove_file(root.join("removed")).unwrap();
        let result = workspace
            .review_changes(&baseline, PermissionDecision::Allow)
            .unwrap();
        assert_eq!(result["truncated"], true);
        let files = result["files"].as_array().unwrap();
        assert_eq!(files.len(), 3);
        let newline_change = files.iter().find(|v| v["path"] == "newlines").unwrap();
        assert!(
            newline_change["diff"]
                .as_str()
                .unwrap()
                .contains("-line\r\n+line\n")
        );
        assert!(
            files
                .iter()
                .any(|v| v["path"] == "old" && v["kind"] == "modified")
        );
        assert!(
            files
                .iter()
                .any(|v| v["path"] == "new" && v["kind"] == "created")
        );
        assert!(
            workspace
                .review_changes(&baseline, PermissionDecision::Deny)
                .is_err()
        );
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn snapshot_diff_preserves_crlf() {
        let root = std::env::temp_dir().join(format!("sentinel-crlf-{}", std::process::id()));
        fs::create_dir(&root).unwrap();
        fs::write(root.join("file"), "line\r\n").unwrap();
        let workspace = Workspace::open(&root).unwrap();
        let baseline = workspace.snapshot(PermissionDecision::Allow).unwrap();
        fs::write(root.join("file"), "line\n").unwrap();
        let result = workspace
            .review_changes(&baseline, PermissionDecision::Allow)
            .unwrap();
        let diff = result["files"][0]["diff"].as_str().unwrap();
        assert!(diff.contains("-line\r\n+line\n"));
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn inventory_is_scoped_and_reports_limits() {
        let root = std::env::temp_dir().join(format!("sentinel-inventory-{}", std::process::id()));
        fs::create_dir(&root).unwrap();
        fs::create_dir(root.join("nested")).unwrap();
        fs::write(root.join("nested/file"), "x").unwrap();
        fs::write(root.join(".hidden"), "x").unwrap();
        symlink("/tmp", root.join("link")).unwrap();
        let workspace = Workspace::open(&root).unwrap();
        assert!(workspace.inventory(PermissionDecision::Deny).is_err());
        let value = workspace.inventory(PermissionDecision::Allow).unwrap();
        assert_eq!(value["truncated"], true);
        let files = value["files"].as_array().unwrap();
        assert_eq!(files.len(), 1);
        assert!(files[0].as_str().unwrap().ends_with("/nested/file"));
        for index in 0..101 {
            fs::write(root.join(format!("file-{index}")), "x").unwrap();
        }
        let value = workspace.inventory(PermissionDecision::Allow).unwrap();
        assert_eq!(value["truncated"], true);
        assert_eq!(value["files"].as_array().unwrap().len(), 100);
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn create_only_requires_approval_and_never_replaces() {
        let root =
            std::env::temp_dir().join(format!("sentinel-create-gateway-{}", std::process::id()));
        fs::create_dir(&root).unwrap();
        fs::write(root.join("existing"), "original").unwrap();
        symlink("existing", root.join("link")).unwrap();
        let workspace = Workspace::open(&root).unwrap();
        let args = json!({"path":"new","content":"proof"});
        assert!(matches!(
            workspace.create_file(&args, PermissionDecision::Ask),
            Err(MutationError::NotCommitted(_))
        ));
        assert!(!root.join("new").exists());
        workspace
            .create_file(&args, PermissionDecision::Allow)
            .unwrap();
        assert_eq!(fs::read_to_string(root.join("new")).unwrap(), "proof");
        for path in ["new", "existing", "link", "../escape", "/tmp/escape"] {
            assert!(matches!(
                workspace.create_file(
                    &json!({"path":path,"content":"overwrite"}),
                    PermissionDecision::Allow
                ),
                Err(MutationError::NotCommitted(_))
            ));
        }
        assert_eq!(
            fs::read_to_string(root.join("existing")).unwrap(),
            "original"
        );
        assert_eq!(fs::read_to_string(root.join("new")).unwrap(), "proof");
        assert!(fs::read_dir(&root).unwrap().all(|entry| {
            !entry
                .unwrap()
                .file_name()
                .to_string_lossy()
                .starts_with(".sentinel-")
        }));
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn list_is_scoped_bounded_and_requires_permission() {
        let root =
            std::env::temp_dir().join(format!("sentinel-list-gateway-{}", std::process::id()));
        fs::create_dir(&root).unwrap();
        fs::write(root.join("visible"), "x").unwrap();
        fs::write(root.join(".hidden"), "x").unwrap();
        symlink("/tmp", root.join("outside")).unwrap();
        let workspace = Workspace::open(&root).unwrap();
        let args = json!({"path":"."});
        assert!(
            workspace
                .list_directory(&args, PermissionDecision::Deny)
                .is_err()
        );
        let result = workspace
            .list_directory(&args, PermissionDecision::Allow)
            .unwrap();
        assert_eq!(result["complete"], true);
        assert!(
            result["entries"]
                .as_array()
                .unwrap()
                .iter()
                .any(|e| e["name"] == "visible")
        );
        assert!(
            !result["entries"]
                .as_array()
                .unwrap()
                .iter()
                .any(|e| e["name"] == ".hidden")
        );
        let hidden = workspace
            .list_directory(
                &json!({"path":".","includeHidden":true}),
                PermissionDecision::Allow,
            )
            .unwrap();
        assert!(
            hidden["entries"]
                .as_array()
                .unwrap()
                .iter()
                .any(|e| e["name"] == ".hidden")
        );
        for path in ["outside", "../", "/tmp"] {
            assert!(
                workspace
                    .list_directory(&json!({"path":path}), PermissionDecision::Allow)
                    .is_err()
            );
        }
        for i in 0..501 {
            fs::write(root.join(format!("entry-{i:03}")), "").unwrap();
        }
        let capped = workspace
            .list_directory(&args, PermissionDecision::Allow)
            .unwrap();
        assert_eq!(capped["entries"].as_array().unwrap().len(), 500);
        assert_eq!(capped["complete"], false);
        assert!(
            workspace
                .list_directory(&json!({"path":".","extra":true}), PermissionDecision::Allow)
                .is_err()
        );
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn read_is_bounded_scoped_and_requires_permission() {
        let root = std::env::temp_dir().join(format!("sentinel-gateway-{}", std::process::id()));
        fs::create_dir(&root).unwrap();
        fs::write(root.join("file"), "one\ntwo\nthree").unwrap();
        symlink("file", root.join("link")).unwrap();
        fs::hard_link(root.join("file"), root.join("hard")).unwrap();
        let workspace = Workspace::open(&root).unwrap();
        let args = json!({"path":"file","offset":2,"limit":1});
        assert!(workspace.read_file(&args, PermissionDecision::Ask).is_err());
        assert!(
            workspace
                .read_file(&args, PermissionDecision::Allow)
                .is_err()
        );
        fs::remove_file(root.join("hard")).unwrap();
        assert_eq!(
            workspace
                .read_file(&args, PermissionDecision::Allow)
                .unwrap()["content"],
            "two"
        );
        for args in [
            json!({"path":"link"}),
            json!({"path":"../file"}),
            json!({"path":"file","extra":true}),
            json!({"path":"file","offset":0}),
        ] {
            assert!(
                workspace
                    .read_file(&args, PermissionDecision::Allow)
                    .is_err()
            );
        }
        assert!(
            workspace
                .read_file(&json!({"path":"file"}), PermissionDecision::Deny)
                .is_err()
        );
        fs::remove_dir_all(root).unwrap();
    }
}
