# Adaptive operation timeouts — proposed architecture

2026-10-30, Prompt 93. **Design proposal, not implemented policy.** This document inventories inspected source and proposes bounded changes; it does not change deadlines, security limits, roadmap adoption or live acceptance. [Engineering standard](ENGINEERING_STANDARD.md), [Agent Runtime](../concepts/AGENT_RUNTIME.md), [P4 policy design](../audits/P4_03_ADAPTIVE_RESOURCE_POLICY_DESIGN.md) and [P93 diagnostic result](../audits/P1_P2_TRANSPORT_DIAGNOSTICS_RESULT.md) supply context.

P94 implementation update: bounded classifier/planner parent budgets, changed-reserve re-admission and native/streaming result diagnostic parity are now implemented and fixture-verified; see the [result](../audits/P1_P2_AGENT_DEADLINE_AND_PROVIDER_PARITY_RESULT.md). Initial-response/useful-progress/idle adaptive defaults below remain a proposal, with local long-wait compatibility preserved.

## Inspected current behavior (source-derived)

| Owner/path | Current contract and limitation |
|---|---|
| `ObservationPolicy.cpp` / `LlmAgentRuntime.cpp` | `agentInferenceDeadlineMs` selects cloud/unknown classification 60 s and planning 120 s. Registered local providers add no Agent wall-clock deadline; cancellation remains. Typed classifier truncation permits one changed-budget retry; it is distinct from transport retry. |
| `LocalInference.h`, `LocalInference.cpp`, `ModelService.cpp` | Local inference/config default timeout 0; explicit configured value can apply. LM Studio/llama.cpp compatible and Ollama generation use common request options/transport but different serializers. A zero wait can be indefinite. No current split initial/idle/total policy. |
| `ProviderRequestRuntime.cpp` | One event-loop deadline, cancellation polled every 25 ms, abort locally. Deadline-bound compatible JSON sends once; unbounded generation can take up to 3 transient attempts with bounded 250/500 ms backoff. No retry after emitted stream output, timeout or cancellation; remote cancellation is not confirmed. |
| `OllamaRuntime.h/.cpp` | Health/discovery config defaults 750/1500 ms, constructor can replace both with 750 ms; cloud catalog helper default 4000 ms. Several acquisition/download paths also set 12000 ms transfer timeout. These are distinct probe/transfer operations, not inference progress clocks. |
| `ModelOperationService.cpp` | Refresh/delete operations use a 15000 ms operation timer; pull disables that timer and consumes streamed pull lines. File download uses 30000 ms transfer timeout; a separate catalog request uses 5000 ms. Transfer activity does not certify completed model acquisition. |
| `ModelLifecycleService.cpp` | Static model metadata registry, not a physical load/unload controller. Do not attribute readiness timeouts to this class. P3 ownership/lease and P4 shadow-policy evidence remain separate from full production resource arbitration. |
| `ProcessExecutor.h/.cpp`, `RealToolExecutor.cpp` | Generic process default 30000 ms; individual tool paths use different budgets, including 300000 ms and additional 120000 ms. Process deadline/termination containment differs from LLM inference; never replace them with an inference heuristic. |
| `AgentRuntime.h/.cpp` | Approval timeout 300000 ms by default; zero disables it. Expiry denies rather than grants permission. `tryShutdown` has caller-supplied drain bounds; blocking shutdown can wait forever on a non-cooperative owner. Daemon drain polls every 25 ms until owners stop. |
| Desktop `DaemonClient` | Caller-supplied positive RPC/connection timeout, per-request monotonic pending deadlines, timeout polling at min(100 ms, request timeout). RPC acceptance timeout is not an inference deadline. Mutations are not replayed after uncertain acknowledgement. |

**Scope:** Detailed source inspection covers the above seams. It is not a complete audit of all plugin/model-loader/OS waits. Source constants are not calibrated hardware budgets. Hard security/protocol limits (frame sizes, tool/context permission boundaries, validation ceilings) remain fixed unless separately reviewed.

## Proposed policy and ownership

Keep policy in existing runtime/provider operation owners, with one resolved immutable effective budget per operation. Add typed options only when consumers and serialization need them; do not introduce another execution manager. Separate:

1. **Initial-response timeout:** start at admitted dispatch; wait for a documented observable response milestone. Qt request queueing/connect/TLS/request-sent observations are local phases, not proof of provider acceptance. An initial HTTP header can end the network-response wait but cannot satisfy semantic generation progress.
2. **No-useful-progress timeout:** after initial response, reset only on validated operation-specific progress: parsed nonempty visible generation output, a valid provider tool-call delta under bounded assembly, validated increasing download bytes, or an authoritative loader readiness milestone. Header churn, TCP keepalive, arbitrary SSE comments, CPU/GPU use and repeated identical heartbeats do not reset it. Count bytes/chunks separately for diagnosis, with parser/size limits.
3. **Total deadline:** monotonic parent operation deadline that no progress, retry, repair, redirect, cold-start event or nested child can extend. Compute child remaining time from that deadline. Keep retry count/output/memory/concurrency limits independently bounded.
4. **Cancellation:** explicit user token outranks timeout classification at the race boundary; bound polling/abort/drain and fence late generations. Cancellation is local unless a documented provider acknowledgement exists. A stopped UI is not a joined worker or proof of cancelled billing.
5. **Provider limits:** documented server TTL/quota/Retry-After and capabilities constrain policy; do not manufacture them from model names. Missing usage/capacity/progress is unknown. Fallback policy is conservative and inspectable.
6. **Cold start / slow hardware:** distinguish an owned model-start operation from inference. Permit an explicit longer bounded startup profile with measured memory/disk/ownership and cancellable readiness checks. Do not infer useful progress from utilization or extend a cloud request because the local GPU is busy.

### Proposed configuration contract

A future settings change exposes standard and extended operation profiles with separate finite initial/idle/total values, units, sensible validation and a visible effective operation summary. No universal numeric defaults are adopted here: calibrate against the existing ENV/BE/BUD matrix and real local/cloud profiles before replacing defaults. Fixed shipped defaults must remain finite; optional user-initiated long operations require an explicit finite extension, a maximum bound, resource admission and working Stop. An unavailable semantic-progress signal falls back to a finite total deadline rather than disabling waits.

Migration must preserve existing explicit local timeout preferences until a reviewed conversion is selected; the current zero default cannot silently become an arbitrary short cap. Conversely, zero cannot be carried forward as an unlimited default under a new policy without explicit compatibility treatment and user communication. Preserve current settings until that design is implemented. Settings/UI/API types have one owner; no per-widget timeout tables.

RPC acknowledgement, approval expiry, process timeout, model lease lifetime and inference lifetime stay independent typed concepts. A timed-out acknowledgement requires status reconciliation through existing run/session IDs, never blind mutation replay. Starting model unload/delete requires real ownership/quiescence; an Agent terminal state or idle Chat UI alone is insufficient.

## Prioritized implementation slices and closure tests

| Local design key / priority | Small implementation slice | Explicit acceptance / independent oracle |
|---|---|---|
| AT-01 / High, P1/P2 | Use Prompt 93 phase evidence and optional authorized gateway correlation before policy tuning. Extend native/streaming persistence only through their existing result contracts when needed. | Fake connection/metadata/partial-body cases remain distinguishable; unknown connect/reuse remains unknown; actual gateway evidence is separately labelled. No extra D loop. |
| AT-02 / High, P1/P2 | Introduce a monotonic parent-budget/child-remaining-time contract inside existing request ownership, preserving cancellation and retry semantics. | Fake-clock boundary just before/at/after expiry, deadline across multiple retries and backoff, cancellation race, no new dispatch after expiry, exact one terminal and no tool on indeterminate classification. Classifier changed-output retry gets fresh ContextBudgetManager admission; test boundary-sized model context. |
| AT-03 / High, P2/P3/P4 | Separate initial/idle/total clocks for one compatible path, then Ollama and supported native/streaming dialects; calibrate low-end/cold/warm profile cells. | Metadata-only and heartbeat-only replies cannot live forever; parsed useful progress resets idle only, total still expires; stalled partial tool calls cannot dispatch; cancellation bounded and next task works. Measure actual cold/warm hardware; missing telemetry never implies large capacity. |
| AT-04 / High, P3/P5 | Model start/readiness/unload/lease and daemon drain policy based on real worker/process quiescence. | Hung loader, cancelled download, endpoint replacement, old Chat worker, active Agent child and delete/unload races retain ownership until completion/drain; no competing daemon or model deletion under active use. Non-cooperative worker has truthful blocked/timeout state, not false shutdown success. |
| AT-05 / Normal, P5/P6 | Expose effective budget/extended mode and safe RPC reconciliation through existing settings/view models. | Invalid/overflow/zero options rejected per reviewed migration; keyboard-focus/loading/error/cancel flow works; late ack cannot duplicate execution, timed-out client can reconnect and inspect same run. |
| AT-06 / High before P12 | Exact candidate native/provider/hardware regression and rollout/downgrade policy. | Adopt concrete budgets before measurement; all first failures/skips retained. Config rollback never opens schema-8 profile with a rejecting old executable; binaries, effective config and model/profile identities recorded. |

These are local follow-up identifiers, not newly created GitHub issues. Read-only GitHub snapshot on 2026-10-30 confirms related open tracking: #29 live corpus, #42 provider parity, #43 model ownership, #54 non-cooperative shutdown, #55 IPC capacity, #68 negative-path coverage. They do not prove every old issue description still matches current code. Map new child work to the relevant existing issue after scope verification; no remote edits in this task.

## Decisions intentionally deferred

No larger global timeout, speculative reasoning parameter, classification bypass, adaptive permission relaxation or automatic retry expansion. No hardware utilization signal is promoted to semantic progress. The two retained Prompt 92 no-status timeouts alone cannot select a better budget or prove a remote root cause. Agent D/E remains incomplete until actual production tools execute and independent artifacts support accepted final claims.
