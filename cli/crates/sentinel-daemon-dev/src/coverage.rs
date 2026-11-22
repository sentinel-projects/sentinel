//! Port of `FileSystemCoverage` in C++ ClaimGroundingResolver.cpp.
use serde_json::Value;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[allow(dead_code)] // Other production observation kinds are retained for the same coverage rules.
pub enum ObservationKind {
    DirectoryListing,
    PathMatches,
    TextSearch,
    FileContent,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HiddenEntriesPolicy {
    Unknown,
    Included,
    Excluded,
}
#[derive(Debug)]
pub struct FileSystemCoverage {
    pub scope: String,
    pub hidden: HiddenEntriesPolicy,
    pub complete: bool,
    pub truncated: bool,
    pub cancelled: bool,
    pub permission_limited: bool,
    pub recursive: bool,
    #[allow(dead_code)]
    // Preserved for production coverage consumers; absence gate uses policy-known.
    pub follow_symlinks: bool,
    pub symlink_policy_known: bool,
    pub skipped_symlinks: i32,
    pub max_depth: i32,
    pub result_limit: i32,
    pub enumeration: bool,
}
impl FileSystemCoverage {
    pub fn from_observation(kind: ObservationKind, data: &Value) -> Self {
        let string = |key: &str| data[key].as_str().unwrap_or_default();
        let boolean = |key: &str, default| data[key].as_bool().unwrap_or(default);
        let integer = |key: &str| {
            data[key]
                .as_i64()
                .and_then(|v| i32::try_from(v).ok())
                .unwrap_or(0)
        };
        let scope = ["scope", "root", "path"]
            .into_iter()
            .map(string)
            .find(|s| !s.is_empty())
            .unwrap_or_default()
            .to_owned();
        let hidden = match string("hiddenEntriesPolicy") {
            "included" => HiddenEntriesPolicy::Included,
            "excluded" => HiddenEntriesPolicy::Excluded,
            _ => HiddenEntriesPolicy::Unknown,
        };
        let skipped = data["skippedSymlinks"]
            .as_i64()
            .and_then(|v| i32::try_from(v).ok());
        Self {
            scope,
            hidden,
            complete: boolean("complete", false),
            truncated: boolean("truncated", true),
            cancelled: boolean("cancelled", true),
            permission_limited: boolean("permissionLimited", true)
                || data["issues"].as_array().is_none_or(|a| !a.is_empty()),
            recursive: boolean("recursive", false),
            follow_symlinks: boolean("followSymlinks", false),
            symlink_policy_known: data["followSymlinks"].is_boolean()
                && skipped.is_some_and(|v| v >= 0),
            skipped_symlinks: skipped.unwrap_or(0),
            max_depth: integer("maxDepth"),
            result_limit: integer("resultLimit"),
            enumeration: matches!(
                kind,
                ObservationKind::DirectoryListing
                    | ObservationKind::PathMatches
                    | ObservationKind::TextSearch
            ),
        }
    }
    pub fn supports_absence(&self, include_hidden: bool, require_recursive: bool) -> bool {
        self.complete
            && !self.truncated
            && !self.cancelled
            && !self.permission_limited
            && (!self.enumeration || (self.symlink_policy_known && self.skipped_symlinks == 0))
            && (!self.recursive || (self.symlink_policy_known && self.skipped_symlinks == 0))
            && (!include_hidden
                || (self.hidden == HiddenEntriesPolicy::Included && self.result_limit > 0))
            && (!require_recursive
                || (self.recursive
                    && self.max_depth > 0
                    && self.result_limit > 0
                    && self.symlink_policy_known
                    && self.skipped_symlinks == 0))
            && !self.scope.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn coverage_defaults_fail_closed_and_requires_exact_scope() {
        let incomplete = FileSystemCoverage::from_observation(
            ObservationKind::DirectoryListing,
            &json!({"path":".","complete":true}),
        );
        assert!(!incomplete.supports_absence(false, false));
        let mut data = json!({"path":".","complete":true,"truncated":false,"cancelled":false,"permissionLimited":false,"issues":[],"hiddenEntriesPolicy":"excluded","recursive":false,"followSymlinks":false,"skippedSymlinks":0,"resultLimit":500});
        let coverage =
            FileSystemCoverage::from_observation(ObservationKind::DirectoryListing, &data);
        assert!(coverage.supports_absence(false, false));
        assert!(!coverage.supports_absence(true, false));
        assert!(!coverage.supports_absence(false, true));
        data["hiddenEntriesPolicy"] = json!("included");
        data["skippedSymlinks"] = json!(1);
        assert!(
            !FileSystemCoverage::from_observation(ObservationKind::DirectoryListing, &data)
                .supports_absence(true, false)
        );
    }
}
