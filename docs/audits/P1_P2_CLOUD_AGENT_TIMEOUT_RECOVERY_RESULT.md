# P1/P2 — Cloud Agent timeout recovery

Date: 2026-10-30 (Europe/Istanbul). Prompt 90; continuation of [Prompt 89](P1_P2_CLOUD_GATEWAY_AGENT_E2E_RESULT.md).

**Final outcome: C Passed; D Failed on its first attempt and its one bounded recovery; E Blocked. Definition of Done remains unmet.** The existing Release completed a real mutation/readback workflow with a truthful final state. It did not compile or execute the fixture through Agent tools in this session.

## Scope and provenance

Used the existing Sentinel checkout and Release on `main`, HEAD `87d762c4a053`, version `1.0.0-rc.8`, build 8. Generated build metadata matches HEAD. No clone, worktree, build directory or rebuild was created. Personal-Brain remained unavailable on PATH; the supplied checkout was used.

The three preceding untracked audit reports were preserved. No source-code change, fix branch, commit, push, merge, reset, stash or clean was performed. No reproducible implementation defect was established that justified a source correction. Existing security, context admission, tool approval and sandbox controls remained enabled. A/B were not repeated.

Production non-secret projections confirmed `cloud-api` / `deepseek-v4.1-flash`, existing HTTPS origin `https://evren-llmapi.ssyz.org.tr`, credential Configured, and no active runs before this session. The base path remains `/v1`; no provider, model, credential or endpoint was changed. The sandbox/host Keychain discrepancy remains the visibility limitation established in [Prompt 88.1](P1_CLOUD_AGENT_CREDENTIAL_DIAGNOSTIC_RESULT.md), not a missing production credential.

The test workspace remains `workspace-ee808837-5bc6-4d3f-b250-cce59511d5a7`, canonical root `/private/tmp/sentinel-cloud-agent-smoke`. Existing test conversation `540ae8d8-b572-4704-83dd-e4d4a9324d8b` was reused; no real documents or Sentinel source were supplied as test input. The normal user workspace entry/history was not overwritten or deleted.

## One session authorization and token accounting

The user supplied session-wide authority for remaining C–E work and stated an approximately **15 million token daily allowance**, authorizing continuation without per-stage administrative prompts. The old USD 1 proposal was not silently adopted as a new enforceable limit. Actual billed dollars remain unknown.

Operational limits adopted within that authority:

- **5 million estimated-token session ceiling**, not a gateway-enforced or actual-token guarantee.
- Initially three new Agent runs, C/D/E. After classifying D's new timeout, one shorter D recovery attempt was explicitly announced, making the revised maximum four runs including E if D succeeds. No unlimited restart policy.
- At most 60 observed planning phases in the session, with a cancellation/stop decision if abnormal repetition or scope escape appears.
- Conservative reserve per observed planning phase: 65536 estimated tokens; reserve per run's classification: 32768. These use the 8192 unknown-capacity policy fallback, allowance for bounded repairs/transport sends, and a further uncertainty margin. They are operational reservations, not measured usage.
- Even four run-classification reserves plus 60 planning reserves total 4063232, below the 5 million ceiling, leaving accounting headroom. No quota balance, tariff, provider bill or upstream token count is inferred from this arithmetic.

The local ledger is `/tmp/sentinel-p90-session-ledger.json`. Classification input/output admission remains ContextBudgetManager's responsibility; its estimates are not replaced by the operational ledger. Deadline-bound compatible JSON transport uses one attempt within the supplied lifetime; planner recovery and typed-truncation repair remain separate bounded paths. A timeout does not prove upstream computation/charging stopped: the existing provider diagnostic marks upstream cancellation unconfirmed.

Mandatory Sentinel approvals were reviewed at action time. Only the required disposable-file edit was approved once during C. No persistent grant or permission weakening was used. No extra administrative C/D approval was requested after session authorization.

## Retained Test C timeout diagnosis

Investigated the original interval `2026-10-29T22:35:39Z`–`22:36:40Z` (01:35:39–01:36:40 Istanbul on October 30) using the existing event file, scoped read-only SQLite rows and source contracts.

- Original core run: `d55d4bdc-1d57-41ef-8567-ece8968fc3e6`.
- Start `22:35:39.375Z`; finish `22:36:39.736Z`: **60361 ms** total elapsed.
- State Failed, terminal reason ObservationIndeterminate, displayed Completion Indeterminate.
- Zero persisted tool steps/evidence and unchanged fixture. No mutation or false rollback conclusion.
- Stored provider HTTP/category/attempt/lifecycle columns and planning context fields were null; recovery count 0. This is an evidence-retention limit, not proof of zero billed requests.
- Existing Desktop log for the day was empty. No accessible sanitized gateway receipt for acceptance, queue duration, upstream duration, HTTP response, cancellation or usage was available. No secret-bearing gateway/server access was attempted.

Source findings:

1. ObservationPolicy classifies the task through the captured provider binding with a **60000 ms cloud deadline**, cancellation token and output budget 2048. ContextBudgetManager admits that classification request before dispatch. A budget refusal has a distinct message; none was recorded for the failed run.
2. Classification permits one changed-budget retry only on typed output truncation, increasing output reservation subject to fresh admission. Ordinary timeout, cancellation and malformed JSON do not trigger that classifier retry. The precise remote attempt history of the old run was not retained.
3. `postJson()` maintains a finite remaining lifetime and supplies one transport attempt when a deadline is set. `ProviderRequestRuntime::wait()` aborts locally on timeout, polls cancellation at 25 ms, and distinguishes timeout from authentication. Remote/upstream cancellation remains unconfirmed.
4. The classifier asks for reasoning disabled, but the generic cloud wire path does **not** invent a vendor-specific reasoning switch. The source explicitly leaves cloud reasoning controls alone where the endpoint contract is unknown. Hidden reasoning or gateway queueing are possible contributors, not established causes; no undocumented parameter was injected.
5. Existing 5-second controller polling and 60-second daemon refresh permit discovery to overlap inference. They were not disabled or changed. No retained transaction-level evidence correlates discovery/cancellation with this particular generation timeout.
6. Classification trace JSON can carry safe request IDs/timing/usage when enabled, but its category defaults to Warning while these records use Info. Such historical trace receipts were not present in the inspected evidence; enabling future diagnostics would not reconstruct the past.

**Conclusion:** a cloud classification deadline was reached; the gateway/upstream timing cause and exact charging remain unresolved. One timeout does not establish a provider serialization defect or justify increasing every timeout, bypassing classification/admission, or manufacturing a completed run. The unchanged Release's subsequent C success demonstrates the original task can complete through the existing implementation; it does not prove sustained gateway reliability.

## C recovery — Passed

Submitted the original C task once in the production Agent interface under the session authorization:

`Correct the arithmetic defect in main.cpp so that topla performs addition. Modify only the incorrect calculation. Read back the file to verify the change, then stop.`

- IPC run: `d2fa785e-7891-46e7-a406-05744b85be8b`.
- Core run: `69a871df-f4dd-48fe-a22a-0459040d4daf`.
- Core start `2026-10-29T22:45:49.125Z`; finish `22:47:57.690Z`.
- Actual sequence: **read-file → edit-file → read-file → accepted Completed**. Four planning phases including the final decision.
- First read call `25d90371-3de9-4549-94c2-c26dc8d99e57`, Succeeded.
- Edit call `02934c55-153e-4ad3-9d07-6fac3a64b2d4`, risk 2; native Sentinel approval `2ec16811-46d4-4277-ba81-4cf13eedab88` covered read/write only to the test main.cpp. Selected **Allow once**. Persistent authorization rows record Approved for those two resources; no always-allow grant.
- Edit Succeeded. Readback call `3116276e-0396-448f-8a7f-085ab97d29b7`, Succeeded.
- One terminal `run.completed`, Completed/Completed, verified grounding; no duplicate edit or command request. Stored provider recovery count 0.
- Context snapshot: 4218 estimated context tokens, not actual provider tokens. No context/capability override.
- Independent content comparison proves the only source change was `return a - b;` → `return a + b;`. Corrected SHA-256: `4d72586c7eed1c6c1c77a60c9353bea533161e0bfb01f98202f649cb7a611954`; original: `653447da9ab437e9bbb3e2cfbf8a70e9c592544d9223461f3cce799c6c415b98`.
- Final Agent response stated the correction and showed the read-back function with `a + b`. Completion agrees with actual effects. Original failed C and successful recovery are both retained; no historical result was overwritten.

Receipts: `/tmp/sentinel-p90-test-c-events.json`, `/tmp/sentinel-p90-test-c-final-record.json`.

## D — Failed, including one bounded recovery

The first D task specified `clang++ -std=c++20 main.cpp -o smoke-result`, `./smoke-result`, zero compile/run exit codes, exact `15\n`, and generated-executable-only mutation.

- IPC run: `20aeb223-1c75-46a6-94ee-4d5d159efdc6`.
- Core run: `07b79e4d-7744-4fe2-b39e-deaf62c5536b`.
- Start `2026-10-29T22:48:45.500Z`; finish `22:50:37.735Z`: **112235 ms** elapsed.
- Failed before tool execution: observation classification timed out after 60000 ms; ObservationIndeterminate / Completion Indeterminate.
- The 60-second failure message is the request deadline, **not** the measured duration of this entire run. Typed-truncation retry is a source-supported possible explanation for longer total elapsed time; the actual request trace is absent, so this is not asserted as the measured cause.
- No compile/run call, no command approval, no smoke-result binary. Corrected main.cpp preserved.

After recording/classifying this failure, one shorter expression of the same D goal was submitted, not an automatic loop:

`Compile main.cpp with clang++ -std=c++20 main.cpp -o smoke-result, run ./smoke-result and verify stdout is exactly 15 followed by a newline. Report both exit codes and stop.`

Recovery evidence:

- IPC run: `64c6de89-9a16-4cf5-81f4-564bdc013fa0`.
- Core run: `5278464c-f906-4e1a-95bd-2b3920bb1932`.
- Start `2026-10-29T22:52:17.304Z`; finish `22:53:17.734Z`: **60430 ms** elapsed.
- The shorter task also terminated Failed / ObservationIndeterminate / Completion Indeterminate, with the same explicit 60000 ms cloud classification timeout.
- Zero tool steps, no compiler/executable request or approval, no context-plan snapshot. Provider HTTP status/attempt fields null; stored provider recovery count 0.
- Exactly one failed terminal event was observed for each D attempt. Neither was silently replaced by a success result. No third D attempt was submitted.
- Independent end-of-session inventory shows only main.cpp changed from the initial session inventory; it still matches C's verified corrected hash. **No smoke-result binary exists**, and there is no Agent compiler exit code, run exit code, stdout or stderr to report. No output 15 is fabricated.

Evidence: `/tmp/sentinel-p90-test-d-events.json`, `/tmp/sentinel-p90-test-d-first-final-record.json`, `/tmp/sentinel-p90-test-d-retry-events.json`, `/tmp/sentinel-p90-test-d-retry-final-record.json`.

## E — Blocked

E was not started because D did not reach tool execution, including its one bounded recovery. The test-owned baseline was not reconstructed; C's successfully corrected file remains intact. No independent compiler call is substituted for Agent execution. No further inference/retry was sent after the second D failure.

## Source corrections and focused checks

No implementation correction or Release rebuild. Existing code was not changed simply to enlarge a deadline or mask a provider error. No existing GitHub issue was claimed to own an unproven transport cause. Relevant acceptance work remains P1-02 / AC-P1-02 and AC-P2-03 in the repository backlog/matrix; those are document IDs, not remote issue numbers. Historical issues #94/#95 concern different tool-output/authorization findings and were not reproduced here. No issue/PR/comment was created.

Existing Release `test_observation_policy` executable: six targeted admission/deadline/truncation/cancellation cases, **8 pass records including lifecycle, 0 failed**. These use fixture providers and make no cloud inference calls. No full suite or historically failing sandbox/plugin/process test was altered.

## Final ledger, stop decision and next action

| Item | Observed outcome |
| --- | --- |
| New Agent runs | 3: C, D first, D recovery; A/B not repeated |
| Planning phases observed | 4, all in successful C |
| Operational estimated-token reserve | **360448** = three classification reserves plus four planning reserves |
| Remaining operational reserve allowance | 4639552 of the adopted 5 million estimate; not an account balance |
| Actual billed tokens, remaining daily account quota, monetary charge | Unavailable; no guarantee inferred from user-reported daily allowance |
| Fixture effect | Only main.cpp: subtraction → addition; verified corrected hash retained |
| Compiler/execution effect | None; no generated smoke-result |
| Final active daemon runs | 0 |
| Final route/credential/catalog projection | Same gateway/model, credential Configured, catalog Available |
| Source/build/security changes | None |

Budget was not the reason for stopping, and no repeated administrative approval blocked progress. After two classified D timeouts, another blind request would not resolve the missing timing evidence. Catalog Available is a discovery snapshot, not proof of dependable classification inference; it does not erase the failed generation results. All native Sentinel approvals remained authoritative.

Next action is to obtain existing sanitized gateway receipts for:

- Original C: `2026-10-29T22:35:39Z`–`22:36:40Z`.
- D first: `2026-10-29T22:48:45Z`–`22:50:38Z`.
- D recovery: `2026-10-29T22:52:17Z`–`22:53:18Z`.

These are October 30 01:35:39–01:36:40, 01:48:45–01:50:38 and 01:52:17–01:53:18 in Istanbul. Needed fields are request correlation, acceptance time, queue/upstream duration, completion/HTTP category, cancellation and usage. Secrets, authorization headers and complete prompts are unnecessary. No gateway admin/account access was configured or invented by this diagnostic.

If those receipts establish a reproducible Sentinel defect, verify source ownership/Git state, map the responsible issue, add a focused regression and incrementally validate an isolated correction. Safe classification-request metadata retention is a follow-up candidate because existing persisted category/status/attempt fields were absent, but no speculative runtime patch was made here. Do not simply remove deadlines, disable classification or send undocumented reasoning parameters. Do not rerun successful A/B/C automatically; preserve C and resume D only after the failure evidence has been addressed. E remains contingent on D.

The only repository file created by Prompt 90 is this report. Three prior audit files remain untouched and uncommitted. Final checks cover whitespace, personal-path scanning, exact C diff, C Completed, two D Failed terminal events, no D tool requests and unchanged other fixture files. No full suite, rebuild, commit, push or merge. The milestone is **incomplete due to unreliable classification response within the existing deadline**, with the remote timing cause still unproven.
