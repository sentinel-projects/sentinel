# Prompt 93 — Transport diagnostics and engineering governance result

Date: 2026-10-30, Europe/Istanbul. **Observed** = test/runtime/repository evidence; **Source-derived** = inspected implementation/contracts; **Hypothesized/Proposed** = unproven remote explanation or future design. Historical Prompt 89–92 reports remain unchanged.

## Result and scope

**Observed:** Non-streaming provider JSON inference used by Agent observation classification now carries bounded, privacy-safe per-HTTP-attempt lifecycle frames into the existing schema-8 classification JSON. Twelve injected response scenarios, three retry-policy rows, network-policy refusal and bounded redaction/retention pass. Eight focused Release CTest targets pass. No new live/billable Agent request or gateway probe was submitted. P92 D failed twice before tools; D/E acceptance remains incomplete.

**Observed:** Permanent engineering policy, adaptive timeout proposal and a P0–P12 quality matrix were added and linked from AGENTS/CONTRIBUTING/PR guidance. They distinguish inspected seams from future audits and implemented behavior from proposals. No unrelated subsystem or roadmap decision was rewritten.

## Preflight, ownership and preservation

**Observed:** Continued on existing diagnostic-owner branch `fix/p1-cloud-agent-gateway-reliability`, base `87d762c4a053e7211a9f019b6591fa78edeb8631`. Entry manifest `/tmp/sentinel-p93-entry-manifest.json` captured 16 pre-existing dirty/untracked files. All six prior untracked audit reports retained identical hashes. Prompt 91 code remains in place and was extended at the same diagnostic seams, not reset/replaced. `brain` was unavailable; the explicitly supplied checkout was used. AGENTS was read first; architecture/runtime/security/build/test guidance, ADRs, Prompt 89–92 evidence, roadmap, acceptance/backlog/decision contracts and CI/contributor guidance were reviewed.

**Observed:** Read-only GitHub queries identified `sentinel-projects/sentinel` and retrieved at most 100 open issues; the relevant referenced numbers were present. Snapshot: `/tmp/sentinel-p93-open-issues.txt`. This is issue tracking evidence, not source revalidation of every issue description. No issue, PR, label, milestone or Project was created/edited. No commit, push, merge, reset, stash, clean, rebase, worktree or fresh build directory.

## Retained failure boundary and gateway correlation

**Observed, P92:** Provider/model `cloud-api / deepseek-v4.1-flash`; requested classifier deadline 60000 ms. Both runs invoked one provider attempt, observed no HTTP status/usage, reported no truncation, never planned/dispatched a tool and ended Failed/ObservationIndeterminate:

| UTC window, 2026-10-30 | Core run | Local classifier elapsed | Local request UUID |
|---|---|---:|---|
| 07:13:44–07:14:45 | `634c03a8-888f-4672-bdc7-a8f75b9e82ed` | 59904 ms | `0b9d3f87-df5f-4bdb-a293-9630e2e09a6f` |
| 07:16:45–07:17:46 | `f22ac375-0b79-468a-bbce-bbf24e7d88a3` | 60289 ms | `a4c4b663-3b68-4108-9adf-1ca356c8275d` |

**Observed:** Existing authorized origin remains `https://evren-llmapi.ssyz.org.tr`. Prior reports/configuration supply a generation endpoint, not an administrative diagnostic interface. Available connected tools expose no gateway-receipt interface. No legitimately accessible correlated acceptance/queue/upstream/cancellation/usage receipt was found in the available evidence. No guessed admin URL, credential extraction or server access was attempted. Old run rows were read only; new fields were not backfilled or fabricated.

**Hypothesized:** Queueing, upstream inference/reasoning, network/TLS delay and local scheduling remain unresolved alternatives. Local provider invocation and request UUID do not prove gateway acceptance. P93 improves future local evidence; it cannot reconstruct phases from P92, establish remaining quota/cost or prove a remote root cause.

## Actual lifecycle observations and semantics

**Source-derived:** `postJsonOnce` creates a logical request trace before NetworkPolicyService admission, posts through the existing injected-or-real QNetworkAccessManager and passes the trace to ProviderRequestRuntime. A stack-owned QObject context disconnects observers at request end. Wait uses its existing deadline/cancel mechanism; callbacks collect timestamps only, never read/copy payloads. JsonReply aggregates at most three transport frames across the existing retry limit. LocalInference response → SelectedEndpointChatProvider reply → ObservationPolicy sanitizer → existing runtime event/store preserves them.

All phase values are milliseconds from that attempt's monotonic QElapsedTimer origin. **Null means unobserved/unavailable; numeric zero is a real measured zero.** Missing transport frames in old rows are compatible, not success. Wait-only callers that did not originate the request here do not invent `request_created_ms`.

| Milestone/field | Direct evidence and limit |
|---|---|
| `request_created_ms` | Logical JSON attempt entry, not upstream acceptance or socket creation |
| `policy_allowed`, `policy_admitted_ms` | Actual network-policy decision; rejection creates no manager POST/reply and leaves response/network fields unknown |
| `reply_created_ms` | Local reply is available and observer attaches immediately after POST returns; constructor-time signals may already have occurred |
| `socket_connect_started_ms` | First observed Qt socketStartedConnecting signal; not DNS start or successful connection |
| `dns_started_ms`, `tcp_connected_ms` | Explicit null: this QNetworkReply contract does not provide separate reliable milestones here |
| `tls_encrypted_ms` | First observed Qt encrypted signal; absent on some reused connections; not gateway acceptance |
| `request_sent_ms` | First observed Qt requestSent signal, not remote receipt or a separate verified body acknowledgement |
| `metadata_received_ms`, `metadata_signal_observed` | First observed metaDataChanged signal; status attribute independently sampled when valid. Headers are never copied |
| `http_status` | Valid observed 100–599 attribute only; null when unavailable. Existing parent diagnostic `http_status=0` remains compatible and means no observed status |
| `first_body_ms` | First readyRead with positive available bytes, before consumer parsing; no byte content copied. Empty/constructor-time delivery can leave it null |
| `qt_finished_ms` | Observed Qt finished signal, including error/abort; never alone treated as success |
| `response_completed`, `partial_body` | Reply finished without network error/local stop, versus observed body without that completion. This is HTTP transport completion, not JSON/model/Agent success; malformed/truncated semantics remain separate |
| `deadline_expired_ms`, `cancelled_ms` | First local deadline/cancel observation before abort when callback occurs, with return-path fallback. Parent typed category/lifecycle distinguishes outcomes; remote cancellation unconfirmed |
| `error_ms`, `qt_network_error` | First observed error signal time and numeric Qt NetworkError enum, or final enum if signal unavailable. No errorString/peer/certificate/URL copied |
| `observation_finished_ms` | Local wait/observation end (or policy refusal); not remote inference end |
| `connection_reuse` | Always `unknown`: absence of connect/encrypted signals cannot reliably prove reuse |

**Source-derived / primary API verification:** Qt documents that encrypted may be absent for reused connections, socketStartedConnecting and requestSent can occur more than once, and finished also applies to errors. Only the first observed milestone is retained. See [Qt QNetworkReply](https://doc.qt.io/qt-6/qnetworkreply.html). No undocumented socket introspection, TLS bypass or gateway-specific parameter was added.

**Source-derived limitation:** Full persistence in this patch targets non-streaming infer/classification. Native completion-specific structs and streaming projection do not automatically retain these nested frames end-to-end; discovery wait can observe only its own wait-local portion and has no new persistent store contract. TD-02 tracks that parity review. No claim of complete tracing across every provider path.

## Privacy, storage and compatibility

**Source-derived:** New `sanitizedTransportDiagnostics` accepts fixed UUID, finite nonnegative bounded integer/boolean/null fields only. HTTP/Qt error values have additional numeric bounds; unknown fields, arbitrary strings, headers, URLs, source and body contents are discarded. Connection reuse is normalized to `unknown`. Classification's established sanitizer re-applies this allowlist before persistence/logging.

**Observed:** Synthetic PRIVATE key/prompt/context/error/partial-body and malicious nested timing/header/body/reuse strings do not survive diagnostic serialization. UUIDs, actual status and safe timings survive. Negative and string timings become null. Malicious fourth transport frame is dropped; a third classifier frame is dropped. Worst tested nested retention packet remains below **8192 bytes**. Missing usage stays absent. Transport capture adds no usage estimate presented as actual usage.

**Source-derived:** Schema remains **8**, unchanged nullable JSON column and store APIs. At most **2 classifier attempts × 3 HTTP frames**, with fixed bounded fields; deadline-bound classification currently allows only one transport send per classifier attempt. Existing 30-day/newest-256 finished-run diagnostic retention and history controls remain authoritative. Older schema-8 readers tolerate the optional JSON fields (an older sanitizer can discard them on rewrite); schema-7 readers still reject schema 8. No new migration/downgrade process, profile write or global payload logging was introduced.

**Observed:** The schema-7→8 migration and typed HTTP-status-on-abort corrections from P91/P92 remain intact. P93 does not relax ContextBudgetManager, ToolExecutionGateway, TLS, credentials, sandbox or native approval boundaries. It does not alter timeout values/retry decisions. Instrumentation distinguishes transport completion from partial body and accepted classification; it cannot authorize tools.

## Deterministic regression and build results

**Observed:** Existing `build/release` reused; incremental build of Desktop, daemon and the scoped tests succeeded with `CCACHE_DISABLE=1`, `-j6`. Final focused command:

```sh
ctest --test-dir build/release --output-on-failure -R '^(test_desktop_daemon_e2e|test_desktop_shell_launch|test_provider_contracts|test_native_provider_contract|test_local_inference|test_agent_loop|test_agent_runtime|test_observation_policy)$'
```

**8/8 targets passed**, **21.37 seconds**. No unrelated full suite or live opt-in model test was run.

| Target | QtTest passed | Existing skips |
|---|---:|---:|
| provider_contracts | 205 | 0 |
| native_provider_contract | 78 | 0 |
| local_inference | 53 | 0 |
| agent_loop | 80 | 0 |
| agent_runtime | 29 | 4 macOS strict shell/fork fixtures |
| observation_policy | 83 | 1 explicit opt-in live classifier probe |
| desktop_daemon_e2e / desktop_shell_launch | Both CTest passed | — |

**Observed:** New/extended deterministic coverage:

- Connected/TLS/request-sent but no response starts: timeout with metadata/body/status unknown, never success.
- Metadata received/status 200 but no body: timeout retains status and metadata timing.
- Partial body then timeout: first-body present, partial=true, response_completed=false; private partial bytes excluded.
- TLS handshake failure and connection refusal: typed numeric Qt error; no fabricated HTTP status.
- User cancellation and deadline expiry: distinct local fields/category; cancellation fixture under 1000 ms, with 25 ms injected deadline rather than a real 60-second test.
- Normal and already-finished immediate success; immediately missed signals remain null while known finished state is represented.
- Reused-connection fixture without fresh connect/TLS signals: request/metadata/finish still visible, reuse remains unknown.
- Allowed transient retries: exactly 3 requests/frames with unique per-attempt UUIDs; bounded-deadline and authentication requests do not retry.
- Network policy refusal: no POST/reply, admission=false, status/network observation unknown.
- Monotonic observed bounds, missing usage, nested redaction and two-by-three diagnostic frame cap, 30-day/256-row retention and schema migration remain tested.
- Existing sync/async AgentLoop failure and production runtime/store tests retain Failed/ObservationIndeterminate and no planner/tool dispatch. No false final acceptance is introduced.

**Observed:** The first expanded data-test run exposed an old fixture assertion that treated all new failure rows as invalid JSON; it was corrected to expect typed ProviderFailure for the new transport failures. This was a test expectation correction, not a product timeout fix. The four initial failed rows (connection-no-response, metadata-stall, partial-body, refused) expected InvalidJson but observed ProviderFailure; that first result remains in the recorded tool output and this report. The temporary focused log was replaced by the passing rerun; final CTest evidence is `/tmp/sentinel-p93-final-tests.log`. Build `/tmp/sentinel-p93-final-build.log`; retention regression `/tmp/sentinel-p93-retention-test.log`. Formatting used installed clang-format 23.1.4 on changed lines; CI-pinned 18 was not installed. IPC/Desktop generation checks, personal-path scan and git diff whitespace checks passed. Documentation relative links/phase coverage were checked separately.

## Release and running-profile impact

**Observed:** Version remains **1.0.0-rc.8**, embedded base commit above; this is an **uncommitted-source candidate**, not a clean release SHA. Current disk artifacts after the final incremental build:

| Artifact | SHA-256 |
|---|---|
| Release daemon | `002ad3b7b0e804c800b34b64197c0b418802a691757b945481e772e81f05492f` |
| Release Desktop | `e8d9b98e700d608d3cb96df5eb69c2f679627842237831e5d1308acbeec38d53` |
| Sorted changed C++ path→SHA manifest digest | `4a7fc13f330775fbcde41ed55c3d064d32d11ddfbe24c02effe4636e447d1a19` |

Manifest `/tmp/sentinel-p93-source-manifest.json` identifies individual source/test files and artifact paths; audit/governance documents are excluded from that C++ digest.

**Observed:** User Desktop PID **22048** and daemon PID **22051**, server generation `0cbf1c45-a7f5-40cf-bd15-b4f91766b463`, remain running the already loaded **P91/P92** code. Profile remains schema **8**, **82** runs, active_runs **0**, same model/binding. Retained P92 rows still lack transport frames as expected. No restart, shutdown, new competing daemon, production migration or old-reader launch. New disk artifacts are **not yet activated** in the user daemon. Isolated test processes validate the candidate; they do not upgrade the live process. Future activation should reuse P92's idle/backup/lifecycle/provenance discipline when required, without an automatic extra billable run.

## Governance deliverables and files changed in P93

Production/test changes relative to P93 entry:

- `core/include/sentinel/core/runtime/TransportDiagnostics.h` (new strict bounded transport sanitizer)
- `core/include/sentinel/core/runtime/ProviderRequestRuntime.h` and `core/src/runtime/ProviderRequestRuntime.cpp` (stack-owned phase observer/result metadata)
- `core/src/runtime/LocalInference.cpp` (per-attempt tracing/aggregation and response metadata)
- `core/include/sentinel/core/agent/ClassificationDiagnostics.h` and `core/src/agent/ObservationPolicy.cpp` (nested sanitized contract propagation)
- `tests/support/ScriptedProviderTransport.h`, `tests/core/test_observation_policy.cpp`, `tests/core/test_agent_runtime.cpp` (deterministic signal/retry/privacy/retention cases)

Repository policy/documents:

- [Engineering standard](../development/ENGINEERING_STANDARD.md): architecture → hidden requirements/risk discovery → minimal design/implementation → verification → production acceptance → issue/roadmap reconciliation; actionable evidence/oracles, lightweight trivial path, affected-action security/data gates.
- [Adaptive timeout design](../development/ADAPTIVE_TIMEOUT_DESIGN.md): inspected hardcoded/default waits and prioritized initial-response/semantic-idle/total/cancellation/provider/cold-start/low-hardware design, explicit acceptance criteria; no broad timeout redesign implemented.
- [P0–P12 module matrix](../roadmaps/MODULE_QUALITY_MATRIX.md): each phase's boundaries/conditions/evidence/assumptions/failures/missing tests/risks/priority; S/C/F inspection levels and trackable local follow-up keys. No claim of full source audit or phase completion.
- `AGENTS.md`, `CONTRIBUTING.md`, `.github/pull_request_template.md`: concise links/instructions preserving existing architecture, validation and security controls. No CI workflow or permissions changed.
- This transport diagnostic result report.

## Verified findings, hypotheses and next priority

**Verified local diagnostic gap:** P92 could not distinguish reply lifecycle stages. The implemented recorder now distinguishes observed connect/TLS/request-sent/metadata/body/finished/stop/error stages through the exact classification path, with unknowns explicit and deterministic regressions. No demonstrated transport-policy defect requiring larger deadlines or a speculative provider workaround was found.

**Source-derived follow-ups:** Native/streaming persistence parity (TD-02); changed-output classifier retry should receive a separate fresh context-admission boundary regression (AT-02); local zero-timeout/default waits, probe constants, model physical ownership and non-cooperative drain need the prioritized design slices, not a global timeout replacement. Matrix mappings use existing #29/#42/#43/#54/#55/#61/#67/#68 where relevant and local keys, not new remote issues.

**Next development priority:** A bounded AT-02 parent/child-budget + classifier retry admission design/regression, and TD-02 transport projection parity as a separate scoped patch. For actual gateway diagnosis, seek authorized sanitized receipts for the two retained UTC windows; unavailable receipts do not block delivery of this local instrumentation. Only after controlled activation and a materially discriminating plan should live D be reconsidered under appropriate scope.

**Acceptance:** P93 local transport diagnostics/regressions and repository governance deliverables **Passed for the inspected scope**. New diagnostics are built/tested but not activated in the preserved user daemon. Remote cause **unresolved**; gateway acceptance, queue time, upstream inference, billing and remote cancellation **unknown**. A/B/C retain historical passes; D failed twice in P92 and E remains blocked. No current live tool execution or full Agent E2E success is claimed.
