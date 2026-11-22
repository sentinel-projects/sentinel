//! Evidence gate derived from core/agent/ObservationPolicy.cpp.
//! This development slice uses lexical workspace-relative resources; it does not
//! resolve Qt platform aliases, query a classifier, or grant tool permissions.
use serde_json::Value;
use std::path::{Component, Path};

#[allow(dead_code)] // Production domains retained for the shared evidence contract.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Domain {
    FileSystem,
    Workspace,
    Process,
    System,
    Clipboard,
    Network,
    Browser,
    Memory,
    ConversationHistory,
    ExternalService,
    Application,
    ProcessExecution,
}
#[allow(clippy::enum_variant_names)] // Names mirror the production evidence scope contract.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Scope {
    ExactResource,
    DirectoryEntries,
    SearchScope,
    Provider,
    Operation,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Purpose {
    Inspect,
    Operate,
}
#[allow(dead_code)] // The narrow executor cannot currently produce every production outcome.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outcome {
    Verified,
    Partial,
    Unavailable,
    Denied,
    Failed,
    Stale,
}
#[allow(dead_code)] // Failure statuses are retained for future gateway event integration.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExecutionStatus {
    Succeeded,
    Cancelled,
    Blocked,
    UnknownTool,
    Failed,
    InvalidArguments,
    InvalidToolContract,
    EmptyPlan,
    NotRequested,
}

/// Classifies a tool observation without turning failed execution into evidence.
pub fn record_outcome(
    status: ExecutionStatus,
    observation: Option<&Observation>,
) -> Option<Outcome> {
    match status {
        ExecutionStatus::InvalidArguments
        | ExecutionStatus::InvalidToolContract
        | ExecutionStatus::EmptyPlan
        | ExecutionStatus::NotRequested => None,
        ExecutionStatus::Succeeded => {
            let partial = observation.is_some_and(|o| {
                o.data["partialEvidence"] == true
                    || (o.data.get("complete").is_some()
                        && (o.data["complete"] != true
                            || o.data["truncated"] == true
                            || o.data["cancelled"] == true))
            });
            Some(if partial {
                Outcome::Partial
            } else {
                Outcome::Verified
            })
        }
        ExecutionStatus::Cancelled => Some(Outcome::Partial),
        ExecutionStatus::Blocked => Some(Outcome::Denied),
        ExecutionStatus::UnknownTool => Some(Outcome::Unavailable),
        ExecutionStatus::Failed => Some(Outcome::Failed),
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClaimType {
    None,
    PathExists,
    FileExists,
    DirectoryExists,
    TextContains,
    SearchHasMatches,
    HiddenEntriesExist,
    PathPatternExists,
}
#[allow(dead_code)] // Other production observation kinds remain part of the contract.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    None,
    DirectoryListing,
    PathMatches,
    FileContent,
    TextSearch,
    CodeDefinitions,
    FileSystemFailure,
    FileSystemFact,
    PatchResult,
    Generic,
}
#[allow(dead_code)] // The gateway does not yet emit every production filesystem failure.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FileFailure {
    None,
    NotFound,
    PermissionDenied,
    NotFile,
    NotDirectory,
    InvalidPath,
    AlreadyExists,
    ReadFailed,
    WriteFailed,
    IoError,
    Unavailable,
    ResourceChanged,
    SymlinkEscape,
    UnsafeParent,
    SecurityBoundaryViolation,
}
#[derive(Clone, Debug)]
pub struct Observation {
    pub kind: Kind,
    pub data: Value,
    pub failure: FileFailure,
    pub failure_resource: String,
}
#[derive(Clone, Debug)]
pub struct Requirement {
    pub domain: Domain,
    pub resource: String,
    pub purpose: Purpose,
    pub tool_id: String,
    pub claim_type: ClaimType,
    #[allow(dead_code)] // Claim identifiers are consumed by structured final-claim validation.
    pub claim_id: String,
    pub claim_query: String,
    pub recursive: bool,
}
#[derive(Clone, Debug)]
pub struct Evidence {
    pub tool_id: String,
    pub call_id: String,
    pub step: usize,
    pub domain: Domain,
    pub resource: String,
    pub scope: Scope,
    pub qualifier: String,
    pub outcome: Outcome,
    pub observation: Option<Observation>,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Grounding {
    Context,
    Verified,
    UnableToVerify,
}
#[derive(Debug)]
pub struct Gate {
    pub accepted: bool,
    pub evidence_ids: Vec<String>,
    #[allow(dead_code)] // Repair text is retained for an eventual multi-attempt planner.
    pub repair: Option<&'static str>,
    pub answer_override: Option<String>,
}
fn rejected(reason: &'static str) -> Gate {
    Gate {
        accepted: false,
        evidence_ids: Vec::new(),
        repair: Some(reason),
        answer_override: None,
    }
}

pub fn normalized_resource(resource: &str, domain: Domain) -> String {
    let trimmed = resource.trim();
    if matches!(domain, Domain::FileSystem | Domain::Workspace) {
        if trimmed == "." {
            return ".".into();
        }
        let mut out = std::path::PathBuf::new();
        for component in Path::new(trimmed).components() {
            match component {
                Component::CurDir => {}
                Component::ParentDir => {
                    if !out.pop() {
                        return String::new();
                    }
                }
                other => out.push(other.as_os_str()),
            }
        }
        out.to_string_lossy().into_owned()
    } else if matches!(
        domain,
        Domain::ProcessExecution | Domain::Network | Domain::Browser
    ) {
        trimmed.to_owned()
    } else {
        trimmed.to_lowercase()
    }
}
fn matches(requirement: &Requirement, evidence: &Evidence) -> bool {
    if (!requirement.tool_id.is_empty() && requirement.tool_id != evidence.tool_id)
        || !(requirement.domain == evidence.domain
            || (requirement.domain == Domain::Workspace && evidence.domain == Domain::FileSystem))
        || (requirement.purpose == Purpose::Inspect && evidence.scope == Scope::Operation)
        || (requirement.purpose == Purpose::Operate && evidence.scope != Scope::Operation)
    {
        return false;
    }
    if matches!(
        requirement.domain,
        Domain::Process | Domain::System | Domain::Clipboard
    ) {
        return true;
    }
    let target = normalized_resource(&requirement.resource, requirement.domain);
    if target.is_empty() {
        return requirement.resource.trim().is_empty();
    }
    let observed = normalized_resource(&evidence.resource, requirement.domain);
    if observed.is_empty() {
        return false;
    }
    if target == observed {
        return true;
    }
    if evidence.scope == Scope::Provider && observed.ends_with(&format!(":{target}")) {
        return true;
    }
    if matches!(requirement.domain, Domain::FileSystem | Domain::Workspace) {
        if evidence.scope == Scope::DirectoryEntries {
            return Path::new(&target)
                .parent()
                .is_some_and(|p| p == Path::new(&observed));
        }
        if evidence.scope == Scope::SearchScope && target.starts_with(&(observed.clone() + "/")) {
            return Path::new(&target)
                .file_name()
                .and_then(|v| v.to_str())
                .is_some_and(|name| {
                    !name.is_empty()
                        && evidence
                            .qualifier
                            .to_lowercase()
                            .contains(&name.to_lowercase())
                });
        }
    }
    false
}
pub fn evaluate(
    requirements: &[Requirement],
    indeterminate: bool,
    evidence: &[Evidence],
    mode: Grounding,
    declared: bool,
) -> Gate {
    if indeterminate {
        return rejected("observation need is unclear");
    }
    let mut ids = Vec::new();
    if requirements.is_empty() {
        if mode == Grounding::UnableToVerify {
            return rejected("no failed observation supports inability to verify");
        }
        if mode == Grounding::Verified {
            for item in evidence.iter().filter(|e| e.outcome == Outcome::Verified) {
                if !ids.contains(&item.call_id) {
                    ids.push(item.call_id.clone());
                }
            }
            if ids.is_empty() {
                return rejected("verified grounding requires a current-run observation");
            }
        }
        return Gate {
            accepted: true,
            evidence_ids: ids,
            repair: None,
            answer_override: None,
        };
    }
    if !declared || mode == Grounding::Context {
        return rejected("fresh observation grounding required");
    }
    let mut failures = Vec::new();
    for requirement in requirements {
        let latest = evidence
            .iter()
            .filter(|item| matches(requirement, item))
            .max_by_key(|item| item.step);
        let partial_content = latest
            .and_then(|e| e.observation.as_ref())
            .is_some_and(|o| {
                requirement.claim_type == ClaimType::None
                    && match o.kind {
                        Kind::DirectoryListing => {
                            o.data["entries"].as_array().is_some_and(|v| !v.is_empty())
                        }
                        Kind::PathMatches => {
                            o.data["matches"].as_array().is_some_and(|v| !v.is_empty())
                        }
                        Kind::FileContent => {
                            o.data["content"].as_str().is_some_and(|v| !v.is_empty())
                        }
                        _ => false,
                    }
            });
        let partial_claim = latest.is_some_and(|e| e.outcome == Outcome::Partial)
            && requirement.claim_type != ClaimType::None
            && crate::claim_grounding::resolve(requirement, evidence, true)
                == crate::claim_grounding::Verdict::Supported;
        if let Some(item) = latest
            && (item.outcome == Outcome::Verified
                || (item.outcome == Outcome::Partial && (partial_content || partial_claim)))
        {
            if !ids.contains(&item.call_id) {
                ids.push(item.call_id.clone());
            }
            continue;
        }
        if mode == Grounding::Verified {
            return rejected("verified answer lacks matching fresh evidence");
        }
        let Some(item) = latest else {
            return rejected("no observation attempt exists");
        };
        if !ids.contains(&item.call_id) {
            ids.push(item.call_id.clone());
        }
        failures.push(if requirement.resource.is_empty() {
            format!("{:?}", requirement.domain)
        } else {
            requirement.resource.clone()
        });
    }
    if mode == Grounding::UnableToVerify {
        if failures.is_empty() {
            let unknown = requirements.iter().any(|r| {
                r.claim_type != ClaimType::None
                    && crate::claim_grounding::resolve(r, evidence, true)
                        == crate::claim_grounding::Verdict::Unknown
            });
            if !unknown {
                return rejected("all required observations succeeded");
            }
            return Gate {
                accepted: true,
                evidence_ids: ids,
                repair: None,
                answer_override: Some(
                    "I could not verify the requested fact from the available observation.".into(),
                ),
            };
        }
        return Gate {
            accepted: true,
            evidence_ids: ids,
            repair: None,
            answer_override: Some(format!(
                "I could not verify the current state of {} because the observation did not succeed.",
                failures.join(", ")
            )),
        };
    }
    Gate {
        accepted: true,
        evidence_ids: ids,
        repair: None,
        answer_override: None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    fn requirement() -> Requirement {
        Requirement {
            domain: Domain::FileSystem,
            resource: "file".into(),
            purpose: Purpose::Inspect,
            tool_id: "read-file".into(),
            claim_type: ClaimType::None,
            claim_id: String::new(),
            claim_query: String::new(),
            recursive: false,
        }
    }
    fn evidence(step: usize, outcome: Outcome, content: &str) -> Evidence {
        Evidence {
            tool_id: "read-file".into(),
            call_id: format!("call-{step}"),
            step,
            domain: Domain::FileSystem,
            resource: "file".into(),
            scope: Scope::ExactResource,
            qualifier: String::new(),
            outcome,
            observation: Some(Observation {
                kind: Kind::FileContent,
                data: json!({"content":content}),
                failure: FileFailure::None,
                failure_resource: String::new(),
            }),
        }
    }
    #[test]
    fn latest_matching_failure_overrides_older_success_and_partial_content_is_usable() {
        let required = [requirement()];
        let observations = [
            evidence(1, Outcome::Verified, "old"),
            evidence(2, Outcome::Failed, ""),
        ];
        assert!(!evaluate(&required, false, &observations, Grounding::Verified, true).accepted);
        let unable = evaluate(
            &required,
            false,
            &observations,
            Grounding::UnableToVerify,
            true,
        );
        assert!(unable.accepted);
        assert_eq!(unable.evidence_ids, ["call-2"]);
        assert!(unable.answer_override.is_some());
        let partial = [evidence(3, Outcome::Partial, "observed")];
        assert!(evaluate(&required, false, &partial, Grounding::Verified, true).accepted);
        assert!(!evaluate(&required, false, &[], Grounding::UnableToVerify, true).accepted);
    }
    #[test]
    fn purpose_and_workspace_root_remain_distinct() {
        let mut r = requirement();
        r.resource = ".".into();
        r.tool_id = "list-directory".into();
        let mut e = evidence(1, Outcome::Verified, "");
        e.resource = ".".into();
        e.tool_id = "list-directory".into();
        e.scope = Scope::DirectoryEntries;
        assert!(evaluate(&[r.clone()], false, &[e.clone()], Grounding::Verified, true).accepted);
        r.purpose = Purpose::Operate;
        assert!(!evaluate(&[r], false, &[e], Grounding::Verified, true).accepted);
        assert_eq!(normalized_resource("../escape", Domain::FileSystem), "");
    }
    #[test]
    fn result_classification_never_upgrades_failure_or_partial() {
        let obs = Observation {
            kind: Kind::FileContent,
            data: json!({"complete":false,"content":"prefix"}),
            failure: FileFailure::None,
            failure_resource: String::new(),
        };
        assert_eq!(
            record_outcome(ExecutionStatus::Succeeded, Some(&obs)),
            Some(Outcome::Partial)
        );
        assert_eq!(
            record_outcome(ExecutionStatus::Blocked, Some(&obs)),
            Some(Outcome::Denied)
        );
        assert_eq!(
            record_outcome(ExecutionStatus::Cancelled, None),
            Some(Outcome::Partial)
        );
        assert_eq!(
            record_outcome(ExecutionStatus::InvalidArguments, None),
            None
        );
    }
}
