//! Structured claim resolution derived from C++ ClaimGroundingResolver.
//! Unsupported glob patterns remain unknown; they must never prove absence.
use crate::coverage::{FileSystemCoverage, HiddenEntriesPolicy, ObservationKind};
use crate::observation::{
    ClaimType, Domain, Evidence, FileFailure, Kind, Observation, Outcome, Requirement,
    normalized_resource,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Verdict {
    Supported,
    Contradicted,
    Unknown,
}

fn path(s: &str) -> String {
    normalized_resource(s, Domain::FileSystem)
}
fn parent_path(value: &str) -> Option<String> {
    let parent = std::path::Path::new(value).parent()?.to_str()?;
    Some(if parent.is_empty() {
        ".".into()
    } else {
        parent.into()
    })
}
fn coverage(obs: &Observation) -> FileSystemCoverage {
    let kind = match obs.kind {
        Kind::DirectoryListing => ObservationKind::DirectoryListing,
        Kind::PathMatches => ObservationKind::PathMatches,
        Kind::TextSearch => ObservationKind::TextSearch,
        _ => ObservationKind::FileContent,
    };
    FileSystemCoverage::from_observation(kind, &obs.data)
}
fn field<'a>(obs: &'a Observation, name: &str) -> &'a str {
    obs.data[name].as_str().unwrap_or_default()
}
fn observed(req: &Requirement, obs: &Observation) -> Option<bool> {
    let target = path(&req.resource);
    // Failed lexical normalization must never make unrelated invalid paths equal.
    if target.is_empty() {
        return None;
    }
    let c = coverage(obs);
    let complete = c.supports_absence(false, false);
    if req.claim_type == ClaimType::HiddenEntriesExist {
        if target.is_empty() || path(&c.scope) != target {
            return None;
        }
        if let Some(entries) = obs.data["hiddenEntries"].as_array() {
            for entry in entries {
                let item = path(entry.as_str().unwrap_or_default());
                if (req.recursive && item.starts_with(&(target.clone() + "/")))
                    || (!req.recursive && parent_path(&item).is_some_and(|p| p == target))
                {
                    return Some(true);
                }
            }
        }
        if obs.kind == Kind::DirectoryListing {
            if obs.data["entries"].as_array().is_some_and(|entries| {
                entries.iter().any(|entry| {
                    entry["name"]
                        .as_str()
                        .is_some_and(|name| name.starts_with('.'))
                })
            }) {
                return Some(true);
            }
        } else if obs.kind != Kind::PathMatches {
            return None;
        }
        if !obs.data["hiddenEntries"].is_array() {
            return None;
        }
        return c.supports_absence(true, req.recursive).then_some(false);
    }
    if req.claim_type == ClaimType::PathPatternExists {
        if target.is_empty() || path(&c.scope) != target {
            return None;
        }
        if obs.kind == Kind::PathMatches && field(obs, "pattern") == req.claim_query {
            if obs.data["matches"]
                .as_array()
                .is_some_and(|items| !items.is_empty())
            {
                return Some(true);
            }
            return c.supports_absence(true, req.recursive).then_some(false);
        }
        // Qt QDir::match supports more pattern syntax than this dev runtime. Do
        // not infer either truth value from an unsupported pattern matcher.
        return None;
    }
    let existence = matches!(
        req.claim_type,
        ClaimType::PathExists | ClaimType::FileExists | ClaimType::DirectoryExists
    );
    if existence {
        if obs.kind == Kind::FileSystemFact && path(field(obs, "path")) == target {
            let key = match req.claim_type {
                ClaimType::PathExists => "exists",
                ClaimType::FileExists => "regularFile",
                _ => "directory",
            };
            return obs.data[key].as_bool();
        }
        if obs.kind == Kind::FileSystemFailure && path(&obs.failure_resource) == target {
            return match obs.failure {
                FileFailure::NotFound => Some(false),
                FileFailure::NotFile if req.claim_type == ClaimType::PathExists => Some(true),
                FileFailure::NotFile if req.claim_type == ClaimType::FileExists => Some(false),
                FileFailure::NotDirectory if req.claim_type == ClaimType::PathExists => Some(true),
                FileFailure::NotDirectory if req.claim_type == ClaimType::DirectoryExists => {
                    Some(false)
                }
                FileFailure::AlreadyExists if req.claim_type == ClaimType::PathExists => Some(true),
                _ => None,
            };
        }
        if obs.kind == Kind::DirectoryListing {
            let listed = path(field(obs, "path"));
            if listed == target {
                return Some(req.claim_type != ClaimType::FileExists);
            }
            let target_path = std::path::Path::new(&target);
            let name = target_path.file_name()?.to_str()?;
            if parent_path(&target)? != listed {
                return None;
            }
            if let Some(entries) = obs.data["entries"].as_array() {
                for entry in entries {
                    if entry["name"].as_str() != Some(name) {
                        continue;
                    }
                    if req.claim_type == ClaimType::PathExists {
                        return Some(true);
                    }
                    return Some(
                        entry["type"].as_str()
                            == Some(if req.claim_type == ClaimType::DirectoryExists {
                                "directory"
                            } else {
                                "file"
                            }),
                    );
                }
            }
            if name.starts_with('.') && c.hidden != HiddenEntriesPolicy::Included {
                return None;
            }
            return complete.then_some(false);
        }
        if obs.kind == Kind::FileContent && path(field(obs, "path")) == target {
            return Some(req.claim_type != ClaimType::DirectoryExists);
        }
        if obs.kind == Kind::PathMatches && req.claim_type != ClaimType::DirectoryExists {
            let target_path = std::path::Path::new(&target);
            let name = target_path.file_name()?.to_str()?;
            if field(obs, "pattern") != name || path(field(obs, "root")) != parent_path(&target)? {
                return None;
            }
            if obs.data["matches"].as_array().is_some_and(|items| {
                items
                    .iter()
                    .any(|item| path(item.as_str().unwrap_or_default()) == target)
            }) {
                return Some(true);
            }
            if name.starts_with('.') && c.hidden != HiddenEntriesPolicy::Included {
                return None;
            }
            return complete.then_some(false);
        }
    }
    if req.claim_type == ClaimType::TextContains
        && obs.kind == Kind::FileContent
        && path(field(obs, "path")) == target
    {
        if field(obs, "content").contains(&req.claim_query) {
            return Some(true);
        }
        return complete.then_some(false);
    }
    if req.claim_type == ClaimType::SearchHasMatches
        && obs.kind == Kind::TextSearch
        && path(field(obs, "scope")) == target
        && field(obs, "query") == req.claim_query
    {
        if obs.data["matchCount"].as_i64().unwrap_or(0) > 0 {
            return Some(true);
        }
        return c
            .supports_absence(
                obs.data["recursive"].as_bool().unwrap_or(false),
                req.recursive,
            )
            .then_some(false);
    }
    None
}

pub fn resolve(req: &Requirement, evidence: &[Evidence], assertion: bool) -> Verdict {
    resolve_assertion(req, evidence, &req.claim_id, assertion)
}

/// Resolve only the assertion bound to this requirement, as in AgentLoop.
pub fn resolve_assertion(
    req: &Requirement,
    evidence: &[Evidence],
    assertion_id: &str,
    assertion: bool,
) -> Verdict {
    if req.claim_type == ClaimType::None || assertion_id != req.claim_id {
        return Verdict::Unknown;
    }
    let mut latest = None;
    let mut value = None;
    for item in evidence {
        let Some(obs) = &item.observation else {
            continue;
        };
        if !(matches!(item.outcome, Outcome::Verified | Outcome::Partial)
            || item.outcome == Outcome::Failed && obs.kind == Kind::FileSystemFailure)
        {
            continue;
        }
        if latest.is_some_and(|step| item.step < step) {
            continue;
        }
        if let Some(found) = observed(req, obs) {
            latest = Some(item.step);
            value = Some(found);
        } else if matches!(
            req.claim_type,
            ClaimType::HiddenEntriesExist | ClaimType::PathPatternExists
        ) && path(&coverage(obs).scope) == path(&req.resource)
        {
            latest = Some(item.step);
            value = None;
        }
    }
    match value {
        Some(v) if v == assertion => Verdict::Supported,
        Some(_) => Verdict::Contradicted,
        None => Verdict::Unknown,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::observation::{Purpose, Scope};
    use serde_json::json;
    fn requirement(claim_type: ClaimType) -> Requirement {
        Requirement {
            domain: Domain::FileSystem,
            resource: "workspace/a".into(),
            purpose: Purpose::Inspect,
            tool_id: String::new(),
            claim_type,
            claim_id: "c".into(),
            claim_query: "needle".into(),
            recursive: false,
        }
    }
    fn evidence(data: serde_json::Value, kind: Kind, step: usize) -> Evidence {
        Evidence {
            tool_id: "read-file".into(),
            call_id: format!("call-{step}"),
            step,
            domain: Domain::FileSystem,
            resource: "workspace/a".into(),
            scope: Scope::ExactResource,
            qualifier: String::new(),
            outcome: Outcome::Verified,
            observation: Some(Observation {
                kind,
                data,
                failure: FileFailure::None,
                failure_resource: String::new(),
            }),
        }
    }
    #[test]
    fn positive_text_and_incomplete_absence() {
        let req = requirement(ClaimType::TextContains);
        let item = evidence(
            json!({"path":"workspace/a","content":"needle"}),
            Kind::FileContent,
            1,
        );
        assert_eq!(
            resolve(&req, std::slice::from_ref(&item), true),
            Verdict::Supported
        );
        let missing = evidence(
            json!({"path":"workspace/a","content":"other"}),
            Kind::FileContent,
            2,
        );
        assert_eq!(resolve(&req, &[missing], false), Verdict::Unknown);
    }
    #[test]
    fn later_incomplete_enumeration_invalidates_prior_hidden_claim() {
        let mut req = requirement(ClaimType::HiddenEntriesExist);
        req.resource = "workspace".into();
        let complete = json!({"path":"workspace","scope":"workspace","hiddenEntriesPolicy":"included","complete":true,"truncated":false,"cancelled":false,"permissionLimited":false,"issues":[],"recursive":false,"followSymlinks":false,"skippedSymlinks":0,"resultLimit":500,"hiddenEntries":[],"entries":[]});
        let first = evidence(complete, Kind::DirectoryListing, 1);
        let later = evidence(
            json!({"path":"workspace","scope":"workspace","entries":[]}),
            Kind::DirectoryListing,
            2,
        );
        assert_eq!(
            resolve(&req, std::slice::from_ref(&first), false),
            Verdict::Supported
        );
        assert_eq!(resolve(&req, &[first, later], false), Verdict::Unknown);
    }
    #[test]
    fn workspace_root_listing_resolves_direct_child() {
        let mut req = requirement(ClaimType::FileExists);
        req.resource = "a".into();
        let listing = evidence(
            json!({"path":".", "entries":[{"name":"a", "type":"file"}]}),
            Kind::DirectoryListing,
            1,
        );
        assert_eq!(resolve(&req, &[listing], true), Verdict::Supported);
    }
    #[test]
    fn foreign_assertion_id_cannot_use_matching_evidence() {
        let req = requirement(ClaimType::TextContains);
        let item = evidence(
            json!({"path":"workspace/a","content":"needle"}),
            Kind::FileContent,
            1,
        );
        assert_eq!(
            resolve_assertion(&req, &[item], "other", true),
            Verdict::Unknown
        );
    }
    #[test]
    fn failed_stat_is_only_proof_for_known_failures() {
        let req = requirement(ClaimType::FileExists);
        let mut item = evidence(json!({}), Kind::FileSystemFailure, 1);
        item.outcome = Outcome::Failed;
        let obs = item.observation.as_mut().unwrap();
        obs.failure_resource = "workspace/a".into();
        obs.failure = FileFailure::NotFound;
        assert_eq!(
            resolve(&req, std::slice::from_ref(&item), false),
            Verdict::Supported
        );
        item.observation.as_mut().unwrap().failure = FileFailure::PermissionDenied;
        assert_eq!(resolve(&req, &[item], false), Verdict::Unknown);
    }
}
