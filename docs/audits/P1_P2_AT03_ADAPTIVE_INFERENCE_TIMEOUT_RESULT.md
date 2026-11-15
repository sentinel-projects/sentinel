# P95 — AT-03 adaptive inference timeout first slice

Date: 2026-10-30. Scope: P1/P2 compatible visible-text streaming; P3/P4/P5 compatibility review. Observed means executed evidence; source-derived means inspected contracts; proposed means not enabled. This is not calibrated hardware policy, full-provider parity, native Linux/Windows acceptance or Agent D/E acceptance.

## Baseline and ownership

Entry branch main, clean tree, source base `f6edb704224b28c0a7b14b83634d435a1d1c5f78`. Personal-Brain executable was unavailable; the user supplied this checkout. AGENTS, engineering standard, architecture/runtime/security/build/test documents, P91–94 results, adaptive design and affected module matrix were inspected. No other source edits were present. No reset, clean, stash, rebase, commit, push, merge, new worktree or new build directory was used. All changes remain uncommitted.

Production ownership remains ModelService/IModelRouter frozen binding → SelectedEndpointChatProvider/IChatProvider → existing LocalInference protocol/parser → ProviderRequestRuntime/Qt event loop. AgentRuntime/AgentLoop, ContextBudgetManager and ToolExecutionGateway retain their authority. Native structured classification/planning total budgets remain 60000/120000 ms for cloud/unknown providers. Registered local provider zero/default and explicit configured total timeouts are preserved. AT-02 shared parents, re-admission and late-result fences are unchanged.

The read-only process inventory found no matching Sentinel Desktop/daemon process at preflight; no live loaded-runtime hash was therefore asserted. Disk Release daemon SHA-256 before rebuilding: `9ca00a31296657386e501ab37376479799c385ec88567d6fcd927b4d38239d10`; Desktop: `1db285f07cf04ab9d49a6d54a69c85b682119b1362e49f9c49f8a258e5843f12`. Earlier P94 reports describe historical loaded binaries, not a new observation. No user daemon was started, stopped or activated; no user credentials, production history or profile were read/written and no paid inference occurred.

## Implemented contract

`InferenceTimeout.h` is the single typed contract: optional initial/idle millisecond options, supported progress kind/location/operation, effective configured total, cancellation/initial/idle/total stop reasons and an injected-clock monotonic watch. Options flow through ChatRequestOptions and LocalInferenceOptions; this adds no settings store or migration. The existing provider total and shared parent are independent absolute bounds; useful progress only resets idle. Exact-boundary progress loses; cancellation overrides a simultaneous timeout. A latched timeout cannot later reopen.

Initial response starts immediately before POST, after network/security admission and serialization. HTTP metadata or first valid visible text ends initial wait. Headers start idle once and never count as generation. If initial is disabled and idle enabled, idle starts at dispatch. Zero new fields preserve legacy behavior. An initial-only new request without finite total/parent or idle is rejected so headers cannot create a new indefinitely hanging profile. Invalid negative options and unsupported explicit policies fail before POST.

Only a fully parsed, non-whitespace visible `choices[0].delta.content` is useful progress. Transport bytes, comments/heartbeats, headers, usage, roles/status, hidden reasoning and incomplete/malformed JSON are not progress. Structured classification and native tool-call streams remain unsupported in this slice; fragments cannot become accepted tool proposals. Existing usage normalization remains optional and strict. Missing or empty usage is unknown, never fabricated zero/cost. Completion still requires an actual complete nonempty, nontruncated response; transport completion alone is insufficient.

The existing request event loop aborts on observed expiry/cancel, with precise total timer and 25 ms initial/idle polling plus checks before consuming progress and at return. This is not hard real-time scheduling or a physical worker-join guarantee. A noncooperative event-loop blocker cannot be forcibly stopped; remote abort acknowledgement/billing remains unknown. Supported stream transport performs one POST with no automatic retry, including partial output. Existing transient retry gates and Agent duplicate-dispatch/final-admission gates remain in place.

## Backend and slow-model behavior

| Path | New initial/idle policy | Existing total/cancel behavior |
|---|---|---|
| OpenAI-compatible cloud visible text | Implemented through the selected endpoint adapter and parser; injected transport tested | Existing configured/Agent parent bounds cannot grow |
| LM Studio and llama.cpp compatible visible text | Same parser and typed options; injected transport tested | Local zero remains zero, explicit total respected; Stop remains available |
| Ollama generation/stream | Explicit requested adaptive policy rejected | Existing NDJSON/total/cancel behavior retained and regression-tested |
| Claude/Gemini native stream | Explicit requested adaptive policy rejected | Existing parser, optional usage, truncation and total/cancel behavior retained |
| Native structured classifier/planner | No fabricated streaming progress; new policy rejected | Existing admitted bounded JSON operation and parent/retry contract retained |

CPU-only, limited RAM, cold loading, long prefill, slow generation and temporary pressure are not mapped to invented budgets. No new timers are enabled by default and no telemetry is interpreted as headroom. An explicitly chosen initial timer bounds first response, not physical model loading; separate startup ownership does not reset inference or its parent. Positive text progress can exceed a chosen idle duration many times, but never the finite total or output/resource bounds. Model/hardware calibration is explicitly unavailable and unused. No model download, residency or ownership changes occurred.

## Safety, resources, privacy and lifecycle

A discovered adjacent risk was unbounded compatible-parser pending bytes/chunk retention. The scoped parser now reads in 64 KiB slices with 1 MiB frame/pending, 8 MiB total received, 4 MiB visible UTF-8 text and 4096 retained chunks. These are independent memory/protocol safety ceilings, not token estimates, hardware calibration or user-granted authority. They also apply to the shared parser's native dialects. Crossing a ceiling aborts/fails, never reports completion or starts a retry. Server max-token/context admission still applies. Long-output compatibility beyond these ceilings requires separately measured acceptance.

Diagnostics contain fixed policy numbers/support/location/origin, parent bounded/remainder, response/first-progress time, progress count, stop reason and partial/resource flags. No prompts, output, reasoning, tool arguments, URLs, headers or credentials enter new diagnostics. They use existing per-result diagnostic objects; complete durable Chat/stream history persistence is not introduced. Schema stays 8 and no IPC schema/generated field changes occur.

UI/IPC ownership is unchanged: the existing Chat cancel clears active message identity, stale worker callbacks cannot finish a newer request; daemon acknowledgement timeout is separate from inference. Disconnect does not prove worker cancellation. Existing client reconciliation must not replay mutations. `LocalInferenceWorker` still joins a running thread at destruction and may wait on noncooperative code; this is a retained AT-04/#54 follow-up, not falsely fixed. No new execution thread/process, background lease, permission, resource controller or shutdown authority was added. No QML/UI change or new user flow was exposed.

## Findings and independent closure oracles

| Condition → consequence | Evidence → mitigation | Oracle / mapping |
|---|---|---|
| Header/heartbeat without generation → indefinitely stalled new profile | Source had one total wait; parsed-text watch and initial/idle separation added | Headers/comments/tool fragments expire without semantic progress; initial-only unbounded profile refused. AT-03, P2 |
| Valid progress at idle boundary / endless genuine output → timer renewal bypasses bound | Fake monotonic boundary and finite-parent tests | Exact boundary rejects; 12 ms parent expires despite 4/8 ms progress; cancellation wins. AT-02/03, P1/P2 |
| Large/incomplete SSE frame → unbounded memory | Source pending/chunks had no parser ceilings | Oversized injected frame aborts with resource-limit flag, one POST, no accepted completion. P2/P4 |
| Partial output then timeout → retry or false complete/tool effects | Existing streaming single-attempt path plus new adapter/worker fixtures | One POST, failed reply without tools/complete message, one terminal sync/async; Agent no-duplicate regressions. P1/P2, #68 historical linkage |
| Unknown hardware / slow cold prefill → unjustified cloud-oriented timeout | Local AppSettings default 0; model metadata is not readiness ownership | Legacy zero/configured total tests; no telemetry-derived budget; startup/inference lifetime separation. P3/P4, AT-03/04 |
| Unsupported dialect or structured output → silently ignored requested policy | Existing Ollama/native formats differ | Explicit policy refused with CapabilityUnsupported and no extra POST; legacy parity regressions remain. P2, #42 historical linkage |
| Disconnected client / noncooperative worker → uncertain completion or indefinite join | Existing DaemonClient, Chat cancel identity and worker destructor | No new replay mechanism; retain independent native reconnect/drain tests before settings rollout. P5, AT-04/05 |

## Deterministic validation and artifact evidence

Validation uses fake clocks and in-memory injected HTTP managers, not live inference. Initial expanded focused run passed 48 QtTest cases including init/cleanup: 42 local/cloud/llama stream rows, one clock-boundary test, one real-adapter contract and two worker sync/async cases. Final build/test receipts below supersede this intermediate run after the silent-stall fixtures were strengthened.

Covered: silent dispatch, headers without generation, useful text before idle, exact idle boundary, comments/metadata/hidden reasoning, partial outer JSON/tool arguments, parent expiry despite text, cancellation at idle boundary, late final response, separate cold-start lifetime, local zero/explicit total, cloud parent preservation, truncation, empty/missing usage, no partial-output retry and exactly one terminal. Existing AgentLoop/runtime/ObservationPolicy/context/IPC regressions supply no-dispatch/duplicate-tool and asynchronous terminal fences; no new test bypasses the actual provider adapter.

Final Release: full build in the existing Release tree passed; focused CTest **10/10 targets passed**, 67.98 seconds. ProviderContractsTest recorded **300 passed, zero failed/skipped**, including all 46 new adaptive cases (42 stream rows, clock boundary, adapter contract, sync/async workers). Within the other passing CTest targets, **six existing QtTest cases skipped**: one blocked native incremental-tool-assembly fixture, four macOS strict-no-fork shell/Docker/shutdown fixtures, and one opt-in live classification probe. These are exclusions, not passes; no skip was added here. Log: `/tmp/sentinel-p95-release-ctest.log`; detailed QtTest output copied to the P95 Release receipt.

Candidate Release SHA-256:

- daemon: `ac08f88356d4c2030ab87c59c391fdc3fcb7f548db3e0a63709e0c36a4984ee2`
- Desktop executable: `6666b80d51a3bd7ab589f8cccb50223db364e47e0d1f6cb3cbb927192334ae5d`
- provider contract test: `dc1b034d3ab51cac19ebb2ef07413f483431b742869559c99f25743e20ab6846`

Source identity is base commit plus uncommitted patch and SHA-256 manifest `/tmp/sentinel-p95-source-manifest.json`; it is not a clean release tag. Rebuilding disk artifacts is not runtime activation or a distributable release approval.

Final full Debug suite: configured and built the existing `build/tests` tree, then `QT_QPA_PLATFORM=offscreen ctest --preset tests --output-on-failure`: **180/180 CTest targets passed**, 474.40 seconds. Nine existing internal QtTest cases skipped: the six exclusions above, Linux Secret Service helper on macOS, missing-node branch when npx is installed, and unimplemented model-residency/removal lease acceptance. These are not native acceptance or passing cases. No executed test failed in this slice, so there is no failing-test receipt to suppress or rerun as a pass. Existing build warnings include unrelated enum-switch coverage; they were not hidden or changed. Full receipt: `/tmp/sentinel-p95-full-ctest.log`, detailed output `/tmp/sentinel-p95-debug-qt-test.log`.

Commands used existing trees only: `CCACHE_DISABLE=1 cmake --preset tests` with this host's existing QtPdf package path; `CCACHE_DISABLE=1 cmake --build --preset tests -j6`; `CCACHE_DISABLE=1 cmake --build build/release -j4`; focused Release CTest regex covered provider contracts, local inference, AgentLoop/runtime/reliability, ObservationPolicy, context budget, Chat, Desktop IPC and reliability IPC. Host access was granted for local socket/process fixtures; application security restrictions stayed enabled.

Both generated IPC contracts (`generate.py --check`, `generate_desktop.py --check`), clang-format 18 changed-line check, personal-path scan (zero flagged) and whitespace check passed. No IPC/type generation change or QML lint is required by a QML-unmodified patch.

Evidence receipts are local `/tmp/sentinel-p95-*`: initial/focused build logs, adaptive-first/expanded logs, Debug configure/build-final, full-ctest, Release build-final/ctest, source manifests. Build/test host is macOS arm64 with Qt 6.12; no native Linux/Windows acceptance is claimed. Generated IPC and format/privacy/whitespace checks are recorded at finalization. No test was intentionally skipped or changed to conceal a platform/security failure.

## Remaining work / exact next slice

- **AT-03-CAL (P2/P3/P4, High):** measure low-end CPU-only, constrained-memory, cold/warm prefill and long-output profiles on each backend. Oracle: independent per-phase timings, cancellation under pressure, legitimate output within resource ceilings; never treat missing telemetry as performance. No adaptive default is calibrated by this work.
- **AT-05 (P5/P6, Normal):** implement the concrete versioned existing-settings proposal in ADAPTIVE_TIMEOUT_DESIGN, including effective-policy projection, opt-in migration preserving current local values, finite extended totals and reconnect/no-replay/cancel UI acceptance. This is the next implementation slice; do not add a second configuration file.
- **AT-03-DIALECT (P2, High before parity claim):** add separately validated Ollama and native semantic event mappings, limits and boundary/cancel tests; until then reject explicit adaptive requirements on those paths.
- **AT-04 (P3/P5, High):** authoritative startup/readiness/lease and bounded worker drain. Oracle: noncooperative owner, cancel/unload/delete race and disconnect cannot destroy in-use resources or report false shutdown. Historical #43/#54 linkage, not freshly closed/modified issues.
- **AT-06 (P12, High before release):** run exact candidate on native Fedora KDE/Linux and Windows, including Qt timers/event-loop pressure, shutdown, socket/profile generation, explicit totals, disconnect/reconnect and extended-policy UI. Oracle: same terminal/effect assertions with native receipts, not macOS extrapolation. Historical #42/#68 and AC-P2-03/04, AC-P4-01–03, AC-P5-01–05 remain relevant; no remote issue or roadmap was rewritten.

Agent D/E remains unaccepted; this deterministic slice makes no production-tool-execution or gateway-connectivity claim.
