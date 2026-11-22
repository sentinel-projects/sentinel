//! Read-only metadata from an explicitly supplied plugin directory.
use crate::agent_gateway::{PermissionDecision, Workspace};
use serde_json::{Value, json};
use std::{collections::HashSet, io, path::Path};

fn identifier(s: &str, limit: usize) -> bool {
    !s.is_empty()
        && s.len() <= limit
        && s != "."
        && s != ".."
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-'))
}
fn manifest(value: &Value) -> Option<Value> {
    let object = value.as_object()?;
    let id = object.get("id")?.as_str()?;
    let name = object.get("name")?.as_str()?;
    let version = object.get("version")?.as_str()?;
    let entry = object.get("entry_point")?.as_str()?;
    if !identifier(id, 80)
        || id != id.to_ascii_lowercase()
        || !identifier(entry, 128)
        || name.trim().is_empty()
        || name.len() > 160
        || version.trim().is_empty()
        || version.len() > 80
        || name.chars().any(char::is_control)
        || version.chars().any(char::is_control)
    {
        return None;
    }
    // The read-only slice deliberately declines declarations it does not fully validate.
    if object.contains_key("credentials") || object.contains_key("credential_schema_version") {
        return None;
    }
    if object
        .get("permissions")
        .is_some_and(|v| v.as_array().is_none_or(|a| !a.is_empty()))
        || object
            .get("dependencies")
            .is_some_and(|v| v.as_object().is_none_or(|a| !a.is_empty()))
    {
        return None;
    }
    let capabilities = object
        .get("host_capabilities")
        .map_or(Some(&[][..]), |v| v.as_array().map(Vec::as_slice))?;
    if capabilities.len() > 4 {
        return None;
    }
    let mut seen = HashSet::new();
    for value in capabilities {
        let capability = value.as_str()?;
        if ![
            "FilesystemRead",
            "FilesystemWrite",
            "NetworkRequest",
            "ProcessExecute",
        ]
        .contains(&capability)
            || !seen.insert(capability)
        {
            return None;
        }
    }
    Some(
        json!({"id":format!("plugin:{id}"),"type":"plugins","name":name,"version":version,
        "enabledKnown":false,"available":false,"health":"Disconnected","maturity":"Unavailable",
        "hostRunning":false,"capabilities":capabilities,"permissions":[],"actions":[],
        "connection":"Not loaded","sandbox":"Not started","failure":"ReadOnlyCatalog"}),
    )
}

pub fn scan(root: &Workspace) -> io::Result<Value> {
    let listing = root.inventory(PermissionDecision::Allow)?;
    let root_path = listing["root"]
        .as_str()
        .ok_or_else(|| io::Error::other("invalid root"))?;
    let paths = listing["files"]
        .as_array()
        .ok_or_else(|| io::Error::other("invalid inventory"))?;
    let mut truncated = listing["truncated"] == true;
    let mut entries = Vec::new();
    let mut ids = HashSet::new();
    for path in paths {
        let Some(relative) = path
            .as_str()
            .and_then(|s| Path::new(s).strip_prefix(root_path).ok())
            .and_then(Path::to_str)
        else {
            truncated = true;
            continue;
        };
        if Path::new(relative)
            .file_name()
            .is_none_or(|name| name != "plugin.json")
        {
            continue;
        }
        if entries.len() == 64 {
            truncated = true;
            break;
        }
        let value = root
            .read_file(&json!({"path":relative}), PermissionDecision::Allow)
            .ok()
            .filter(|v| v["complete"] == true)
            .and_then(|v| {
                v["content"]
                    .as_str()
                    .and_then(|s| serde_json::from_str::<Value>(s).ok())
            })
            .and_then(|v| manifest(&v));
        let Some(row) = value else {
            truncated = true;
            continue;
        };
        if !ids.insert(row["id"].as_str().unwrap_or_default().to_owned()) {
            truncated = true;
            entries.retain(|e: &Value| e["id"] != row["id"]);
            continue;
        }
        entries.push(row);
    }
    if root.inventory_root()? != Path::new(root_path) {
        return Err(io::Error::other("plugin root changed"));
    }
    Ok(
        json!({"extensionCount":entries.len(),"entries":entries,"toolCount":0,"toolEntries":[],"truncated":truncated}),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn accepts_only_valid_supported_manifest() {
        let valid = json!({"id":"demo","name":"Demo","version":"1","entry_point":"demo.so","host_capabilities":["FilesystemRead"]});
        assert_eq!(manifest(&valid).unwrap()["id"], "plugin:demo");
        let mut bad = valid.clone();
        bad["id"] = json!("../bad");
        assert!(manifest(&bad).is_none());
        let mut bad = valid.clone();
        bad["credentials"] = json!([]);
        assert!(manifest(&bad).is_none());
        let mut bad = valid;
        bad["host_capabilities"] = json!(["Unrecognized"]);
        assert!(manifest(&bad).is_none());
    }
}
