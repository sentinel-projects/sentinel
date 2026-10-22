#!/usr/bin/env bash
# Compare the complete PR or push range, including non-ancestor force pushes.
set -euo pipefail
base_ref=${BASE_SHA:-${BEFORE_SHA:-}}
if [[ -n "$base_ref" ]]; then
    if [[ ! "$base_ref" =~ ^[[:xdigit:]]{40}$ && ! "$base_ref" =~ ^[[:xdigit:]]{64}$ ]]; then
        printf '%s\n' 'Expected a hexadecimal event base SHA.' >&2
        exit 2
    fi
    if [[ "$base_ref" =~ ^0+$ ]]; then
        base_ref=$(git hash-object -w -t tree --stdin </dev/null)
    elif ! git cat-file -e "${base_ref}^{commit}" 2>/dev/null; then
        # A shallow checkout may not contain the PR base or pre-force-push tip.
        # If the remote cannot supply it, fail instead of checking a smaller range.
        git fetch --no-tags --depth=1 origin "$base_ref"
    fi
else
    base_ref=HEAD^
fi
format_diff=$(git diff -U0 "$base_ref" HEAD -- apps core integrations plugins tests |
    clang-format-diff.py -p1)
if [[ -n "$format_diff" ]]; then
    printf '%s\n' "$format_diff"
    exit 1
fi
