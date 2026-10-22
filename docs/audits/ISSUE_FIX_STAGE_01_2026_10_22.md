# Issue fixes — stage 01

Date: 2026-10-22. Small independent fixes selected after folder discovery.
Personal-Brain CLI unavailable; existing checkout and repository instructions used.

## Implemented

- #82: SentinelSignPackages.cmake now fails the CMake process when the signer
  does not return exactly 0. Failed signing can no longer be a successful target.
  No real certificate or installer was used. The separate cross-directory signing
  configuration problem #81 remains open.
- #98: TodoService generates full UUIDs and checks the current item map before
  insertion. Removed the numeric allocator that collided with restored IDs.
  Existing restored IDs/content/status/priority are retained. This does not wire
  the currently dormant service into production (#65).

## Verification

The new tests were executed against the old implementations first:

- Package-signing regression observed helper exit 0 despite a synthetic signer
  failure and failed as intended.
- Todo regression failed all three cases: setItems then add, restoreSnapshot then
  add, and new-owner snapshot round trip. Numeric, sparse, maximum-int and custom
  restored IDs were included; original items were overwritten before the fix.

After the fixes, cmake --preset tests configured successfully and the real
sentinel_core/test_todo_service target built successfully. CCACHE_DISABLE=1 was
used to bypass the machine's compiler wrapper cache without changing launcher
settings or addressing separate issue #83.

Three scoped CTest targets passed (6.13 seconds): test_todo_service,
test_package_signing_failure and test_packaging_contract. Signing uses CMake as
an inert cross-platform fake signer; both exe/msi synthetic inputs and paths
containing spaces are checked. No native Windows signing claim is made.

clang-format --dry-run --Werror and git diff --check passed. Personal-path scan
reported zero flagged files. No full application rebuild/CTest was required for
these narrow changes; no shared runtime/authorization behavior was changed.

## Next stages

Proceed with compiler-launcher ownership (#83), installation-root handling (#87)
and local lint context (#92), each with its own scoped verification. Then address
CLI rejected-response status (#79) and evidence publication (#94) before larger
migration, queue lifecycle and runtime/security work. This ordering is a working
sequence, not a claim that P0/P1 blockers can be ignored for release.

Implementation is on the working branch; issue closure should follow integration
into the authoritative branch. No release is approved by these tests.
