import argparse
import collections
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

LEVELS = {
    "unverified": 0,
    "deterministic": 1,
    "integration": 2,
    "end_to_end": 3,
    "native": 4,
}
SCHEMA_KEYS = {
    "$schema",
    "$id",
    "$ref",
    "$defs",
    "title",
    "description",
    "type",
    "enum",
    "properties",
    "required",
    "additionalProperties",
    "items",
    "pattern",
    "minLength",
    "maxLength",
    "minItems",
    "maxItems",
    "minimum",
}


def schema_errors(value, schema, document=None, location="registry"):
    document = schema if document is None else document
    errors = []
    if set(schema) - SCHEMA_KEYS:
        return [f"{location}: unsupported schema keyword"]
    if "$ref" in schema:
        reference = schema["$ref"]
        if not reference.startswith("#/$defs/"):
            return [f"{location}: nonlocal schema reference"]
        target = document.get("$defs", {}).get(reference.split("/")[-1])
        return (
            schema_errors(value, target, document, location)
            if target
            else [f"{location}: missing schema definition"]
        )
    types = schema.get("type", [])
    types = [types] if isinstance(types, str) else types
    predicates = {
        "object": lambda: isinstance(value, dict),
        "array": lambda: isinstance(value, list),
        "string": lambda: isinstance(value, str),
        "boolean": lambda: type(value) is bool,
        "integer": lambda: type(value) is int,
        "null": lambda: value is None,
    }
    if types and not any(predicates.get(kind, lambda: False)() for kind in types):
        return [f"{location}: incorrect type"]
    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{location}: invalid enum")
    if isinstance(value, dict):
        properties = schema.get("properties", {})
        if set(schema.get("required", [])) - set(value):
            errors.append(f"{location}: missing required fields")
        if schema.get("additionalProperties") is False and set(value) - set(properties):
            errors.append(f"{location}: unknown fields")
        for key in properties.keys() & value.keys():
            errors.extend(
                schema_errors(
                    value[key], properties[key], document, f"{location}.{key}"
                )
            )
        if isinstance(schema.get("additionalProperties"), dict):
            for key in value.keys() - properties.keys():
                errors.extend(
                    schema_errors(
                        value[key],
                        schema["additionalProperties"],
                        document,
                        location + ".entry",
                    )
                )
    if isinstance(value, list):
        if len(value) < schema.get("minItems", 0):
            errors.append(f"{location}: too few items")
        if len(value) > schema.get("maxItems", 1000):
            return [f"{location}: too many items"]
        for index, item in enumerate(value):
            errors.extend(
                schema_errors(
                    item, schema.get("items", {}), document, f"{location}[{index}]"
                )
            )
    if isinstance(value, str):
        if not schema.get("minLength", 0) <= len(value) <= schema.get(
            "maxLength", 2048
        ) or ("pattern" in schema and not re.fullmatch(schema["pattern"], value)):
            errors.append(f"{location}: invalid string")
    if type(value) is int and value < schema.get("minimum", value):
        errors.append(f"{location}: below minimum")
    return errors


def safe_file(root, reference):
    path = Path(reference)
    if path.is_absolute() or ".." in path.parts or "\\" in reference:
        return None
    resolved = (root / path).resolve()
    return (
        resolved
        if resolved.is_relative_to(root.resolve()) and resolved.is_file()
        else None
    )


def unique_index(records, label, errors):
    result = {}
    for record in records:
        identity = record["id"]
        if identity in result:
            errors.append(f"duplicate {label} ID: {identity}")
        result[identity] = record
    return result


def classify_evidence(
    record,
    candidate_sha,
    candidate_dirty,
    candidate_digest,
    now,
    max_age_days,
    candidate_branch=None,
):
    reasons = []
    try:
        timestamp = dt.datetime.fromisoformat(
            record["timestamp"].replace("Z", "+00:00")
        )
        if timestamp.tzinfo is None:
            reasons.append("timestamp_unqualified")
        elif timestamp > now:
            reasons.append("future_timestamp")
        elif (now - timestamp).total_seconds() > max_age_days * 86400:
            reasons.append("stale_time")
    except ValueError:
        reasons.append("invalid_timestamp")
    if record["source_sha"] != candidate_sha:
        reasons.append("sha_mismatch")
    if not candidate_branch or record["branch"] != candidate_branch:
        reasons.append("branch_mismatch_or_unqualified")
    if record["dirty"] != candidate_dirty:
        reasons.append("tree_mismatch")
    if record["dirty"] and (
        not candidate_digest or record["dirty_digest"] != candidate_digest
    ):
        reasons.append("dirty_digest_mismatch")
    return {
        "id": record["id"],
        "outcome": record["outcome"],
        "reasons": reasons,
        "eligible": record["outcome"] == "pass" and not reasons,
        "platform": record["platform"],
        "level": record["level"],
        "branch": record["branch"],
    }


def validate(
    registry,
    schema,
    root,
    candidate_sha,
    candidate_dirty=False,
    candidate_digest=None,
    now=None,
    candidate_branch=None,
):
    errors = sorted(schema_errors(registry, schema))
    summary = {
        "valid": False,
        "release_certified": False,
        "errors": errors,
        "warnings": [],
        "features": [],
        "evidence": [],
        "candidate_sha": candidate_sha,
        "candidate_dirty": candidate_dirty,
        "candidate_digest": candidate_digest,
    }
    if errors:
        return summary
    now = now or dt.datetime.now(dt.timezone.utc)
    features = unique_index(registry["features"], "feature", errors)
    tests = unique_index(registry["tests"], "test", errors)
    components = unique_index(registry["components"], "component", errors)
    criteria = unique_index(registry["criteria"], "criterion", errors)
    evidence = unique_index(registry["evidence"], "evidence", errors)
    for criterion in criteria.values():
        source = safe_file(root, criterion["source"])
        rows = (
            []
            if not source
            else [
                line
                for line in source.read_text(encoding="utf-8").splitlines()
                if line.startswith("| " + criterion["id"] + " |")
            ]
        )
        fields = (
            [field.strip() for field in rows[0].split("|")[1:-1]]
            if len(rows) == 1
            else []
        )
        if (
            len(fields) < 3
            or fields[1] != criterion["phase"]
            or fields[-2] != criterion["status"]
        ):
            errors.append(f"criterion source/status mismatch: {criterion['id']}")
    for test in tests.values():
        source = safe_file(root, test["source"])
        if not source or test["scenario"] not in source.read_text(encoding="utf-8"):
            errors.append(f"test source/scenario missing: {test['id']}")
    for component in components.values():
        if component["required"] and component["source_sha"] is None:
            summary["warnings"].append(
                f"missing external component evidence: {component['id']}"
            )
    for record in evidence.values():
        if record["component"] not in components or record["scenario"] not in tests:
            errors.append(f"unknown evidence component/scenario: {record['id']}")
        if record["dirty"] != (record["dirty_digest"] is not None):
            errors.append(f"inconsistent dirty provenance: {record['id']}")
        if not record["limitations"] or not record["scope"] or not record["artifacts"]:
            errors.append(f"incomplete evidence scope/artifacts: {record['id']}")
        if set(record["scope"]) - features.keys():
            errors.append(f"unknown evidence scope: {record['id']}")
        for artifact in record["artifacts"]:
            source = safe_file(root, artifact["path"])
            if (
                not source
                or hashlib.sha256(source.read_bytes()).hexdigest() != artifact["sha256"]
            ):
                errors.append(f"missing or changed evidence artifact: {record['id']}")
        component = components.get(record["component"], {})
        local = component.get("repository") == registry["application_repository"]
        classified = classify_evidence(
            record,
            candidate_sha if local else component.get("source_sha"),
            candidate_dirty if local else component.get("dirty", False),
            candidate_digest if local else component.get("dirty_digest"),
            now,
            registry["max_age_days"],
            candidate_branch if local else record["branch"],
        )
        for component_id, sha in record["component_versions"].items():
            if component_id not in components or not re.fullmatch(r"[0-9a-f]{40}", sha):
                errors.append(f"invalid component provenance: {record['id']}")
            elif components[component_id]["source_sha"] != sha:
                classified["reasons"].append("component_sha_mismatch")
        if (
            record["component_versions"].get(record["component"])
            != record["source_sha"]
        ):
            classified["reasons"].append("component_identity_mismatch")
        for feature_id in record["scope"]:
            feature = features.get(feature_id, {})
            if any(
                platform["id"] == record["platform"]
                and platform["support"] == "unsupported"
                for platform in feature.get("platforms", [])
            ):
                classified["reasons"].append("unsupported_platform")
            for component_id in feature.get("components", []):
                component = components.get(component_id)
                if (
                    not component
                    or not component["source_sha"]
                    or record["component_versions"].get(component_id)
                    != component["source_sha"]
                ):
                    classified["reasons"].append("missing_component_evidence")
        if record["level"] == "native" and record["runtime_environment"] != "native":
            errors.append(f"native claim without native environment: {record['id']}")
        classified["eligible"] = classified["eligible"] and not classified["reasons"]
        summary["evidence"].append(classified)
    classified_by_id = {item["id"]: item for item in summary["evidence"]}
    for feature in features.values():
        identity = feature["id"]
        for field, index in [
            ("test_ids", tests),
            ("evidence_ids", evidence),
            ("dependencies", features),
            ("components", components),
            ("criteria", criteria),
        ]:
            if set(feature[field]) - index.keys():
                errors.append(f"unknown {field}: {identity}")
        for reference in feature["source_refs"] + feature["context_refs"]:
            if not safe_file(root, reference):
                errors.append(f"broken source/context reference: {identity}")
        if feature["coverage"] == "mapped" and not feature["test_ids"]:
            errors.append(f"mapped coverage without tests: {identity}")
        if feature["coverage"] == "unmapped" and feature["test_ids"]:
            errors.append(f"unmapped coverage with tests: {identity}")
        platforms = [item["id"] for item in feature["platforms"]]
        if len(platforms) != len(set(platforms)):
            errors.append(f"duplicate platform: {identity}")
        claimed = LEVELS.get(feature["verification"], 0)
        if feature["verification"] == "blocked" and not feature["blockers"]:
            errors.append(f"blocked without reason: {identity}")
        if claimed and (
            feature["implementation"] not in {"implemented", "partial"}
            or feature["blockers"]
        ):
            errors.append(f"contradictory implementation/blockers: {identity}")
        supported = [
            item["id"]
            for item in feature["platforms"]
            if item["support"] != "unsupported"
        ]
        if feature["verification"] == "unsupported" and supported:
            errors.append(f"unsupported claim on qualified platform: {identity}")
        if not supported and feature["verification"] != "unsupported":
            errors.append(f"missing supported platform qualification: {identity}")
        eligible_platforms = set()
        adverse_current = False
        outcomes = collections.Counter()
        for evidence_id in feature["evidence_ids"]:
            if evidence_id not in evidence:
                continue
            record = evidence[evidence_id]
            classified = classified_by_id[evidence_id]
            outcomes[classified["outcome"]] += 1
            if not classified["reasons"] and classified["outcome"] != "pass":
                adverse_current = True
            if identity not in record["scope"] or record["platform"] not in platforms:
                errors.append(f"evidence scope/platform mismatch: {identity}")
            if classified["eligible"] and LEVELS[record["level"]] >= claimed:
                eligible_platforms.add(record["platform"])
        if claimed and (set(supported) - eligible_platforms):
            errors.append(
                f"verification claim lacks current platform evidence: {identity}"
            )
        if claimed and adverse_current:
            errors.append(
                f"verification claim includes non-passing current evidence: {identity}"
            )
        if feature["acceptance"] == "accepted" and (
            candidate_dirty
            or feature["verification"] != "native"
            or feature["blockers"]
            or not feature["reviewer"]
            or not feature["criteria"]
            or any(
                criteria[item]["status"] != "Passed"
                for item in feature["criteria"]
                if item in criteria
            )
        ):
            errors.append(f"contradictory acceptance: {identity}")
        summary["features"].append(
            {
                "id": identity,
                "implementation": feature["implementation"],
                "coverage": feature["coverage"],
                "verification": feature["verification"],
                "acceptance": feature["acceptance"],
                "outcomes": dict(outcomes),
                "current_evidence_platforms": sorted(eligible_platforms),
            }
        )
    errors.sort()
    summary["valid"] = not errors
    return summary


def git_identity(root):
    def git(*args):
        return subprocess.check_output(
            ["git", "-C", str(root), *args], stderr=subprocess.DEVNULL
        )

    sha = git("rev-parse", "HEAD").decode().strip()
    dirty = bool(git("status", "--porcelain", "--untracked-files=all"))
    digest = hashlib.sha256(git("diff", "HEAD", "--binary"))
    for name in sorted(
        git("ls-files", "--others", "--exclude-standard", "-z").split(b"\0")
    ):
        if name:
            source = safe_file(root, name.decode())
            if source:
                digest.update(name + b"\0" + source.read_bytes())
    branch = git("branch", "--show-current").decode().strip() or "detached"
    return sha, dirty, digest.hexdigest() if dirty else None, branch


def human_report(summary):
    counts = collections.Counter(item["verification"] for item in summary["features"])
    lines = [
        "# Verification coverage summary",
        "",
        "Registry valid: " + str(summary["valid"]),
        "Release certified: False (registry validation is not product acceptance)",
        "Candidate SHA: " + summary["candidate_sha"],
        "Dirty candidate: " + str(summary["candidate_dirty"]),
        "",
        "Verification states: "
        + json.dumps(dict(sorted(counts.items())), sort_keys=True),
    ]
    for item in summary["features"]:
        lines.append(
            f"- {item['id']}: {item['implementation']}; {item['coverage']}; {item['verification']}; {item['acceptance']}"
        )
    for item in summary["evidence"]:
        lines.append(
            f"- Evidence {item['id']}: {item['outcome']}; eligible={item['eligible']}; "
            + ",".join(item["reasons"])
        )
    lines.extend("- Warning: " + value for value in summary["warnings"][:100])
    lines.extend("- Error: " + value for value in summary["errors"][:100])
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(
        description="Offline, read-only verification registry validation"
    )
    parser.add_argument("--registry", default="verification/registry.json")
    parser.add_argument("--schema", default="verification/schema.json")
    parser.add_argument("--format", choices=["json", "markdown"], default="markdown")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    try:
        registry = json.loads((root / args.registry).read_text(encoding="utf-8"))
        schema = json.loads((root / args.schema).read_text(encoding="utf-8"))
        sha, dirty, digest, branch = git_identity(root)
        summary = validate(
            registry, schema, root, sha, dirty, digest, candidate_branch=branch
        )
    except (OSError, ValueError, TypeError, KeyError, subprocess.CalledProcessError):
        print("Verification input could not be validated.", file=sys.stderr)
        return 2
    print(
        (
            json.dumps(summary, indent=2, sort_keys=True)
            if args.format == "json"
            else human_report(summary)
        ),
        end="\n",
    )
    return 0 if summary["valid"] else 1


if __name__ == "__main__":
    sys.exit(main())
