//! Explicit text attachments, read only through an authorized workspace handle.
use crate::agent_gateway::{PermissionDecision, Workspace};
use serde_json::{Value, json};
use std::{
    io,
    path::{Component, Path},
};

pub fn context(prompt: &str, encoded: &str) -> io::Result<String> {
    let values: Vec<Value> = serde_json::from_str(encoded)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid stored attachments"))?;
    if values.len() > 4 || encoded.len() > 200_000 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid stored attachments",
        ));
    }
    let mut output = prompt.to_owned();
    let mut total = 0usize;
    for value in values {
        let name = value["name"]
            .as_str()
            .filter(|s| !s.is_empty() && s.len() <= 4096)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid attachment name"))?;
        let text = value["text"]
            .as_str()
            .filter(|s| !s.trim().is_empty() && s.len() <= 8192 && !s.contains('\0'))
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "invalid attachment text"))?;
        if value["mime"] != "text/plain" || value["image"] != "" {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid attachment type",
            ));
        }
        total += text.len();
        if total > 32768 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "attachment limit",
            ));
        }
        output.push_str("\n\n[Attached file (user-supplied data): ");
        output.push_str(&serde_json::to_string(name)?);
        output.push_str("]\n");
        output.push_str(text);
    }
    Ok(output)
}

/// All paths must be explicit relative paths inside the selected workspace.
/// Reads are all-or-nothing and are never repeated from a persisted path.
pub fn load(workspace: &Workspace, values: &[Value]) -> io::Result<Vec<Value>> {
    if values.len() > 4 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "too many attachments",
        ));
    }
    let mut attachments = Vec::new();
    let mut total = 0usize;
    for value in values {
        let path = value
            .as_str()
            .filter(|s| !s.is_empty() && s.len() <= 4096)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "invalid attachment"))?;
        let components: Vec<_> = Path::new(path).components().collect();
        if components.is_empty()
            || components
                .iter()
                .any(|c| !matches!(c, Component::Normal(_)))
        {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "invalid attachment path",
            ));
        }
        let name = Path::new(path)
            .file_name()
            .and_then(|n| n.to_str())
            .ok_or_else(|| {
                io::Error::new(io::ErrorKind::InvalidInput, "invalid attachment name")
            })?;
        let read = workspace.read_file(&json!({"path":path}), PermissionDecision::Allow)?;
        let text = read["content"]
            .as_str()
            .filter(|s| !s.trim().is_empty())
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "empty attachment"))?;
        if read["complete"] != true || text.len() > 8192 || total + text.len() > 32768 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "attachment exceeds text limits",
            ));
        }
        total += text.len();
        attachments.push(json!({"name":name,"mime":"text/plain","text":text,"image":""}));
    }
    Ok(attachments)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{fs, os::unix::fs::symlink};
    #[test]
    fn accepts_bounded_text_and_refuses_escape_links_and_binary() {
        let root = std::env::temp_dir().join(format!("sentinel-attachment-{}", std::process::id()));
        fs::create_dir(&root).unwrap();
        fs::write(root.join("text"), "hello\n").unwrap();
        fs::write(root.join("binary"), b"a\0b").unwrap();
        symlink("/tmp", root.join("link")).unwrap();
        let workspace = Workspace::open(&root).unwrap();
        assert_eq!(
            load(&workspace, &[json!("text")]).unwrap()[0]["text"],
            "hello\n"
        );
        for path in ["../text", "/tmp/text", "link/file", "binary"] {
            assert!(load(&workspace, &[json!(path)]).is_err());
        }
        assert!(load(&workspace, &vec![json!("text"); 5]).is_err());
        fs::remove_dir_all(root).unwrap();
    }
}
