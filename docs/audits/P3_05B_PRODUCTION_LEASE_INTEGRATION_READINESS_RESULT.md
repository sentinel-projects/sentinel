# P3-05B — Production lease integration readiness and decision gate

Date: 2026-10-18. **Design only. PD-D10 remains Proposed, not Approved.**
Implementation planning is ready for maintainer review; production activation is **not authorized**.
No lease enforcement, deletion refusal, process termination, automatic unload or recovery policy
was implemented. AC-P3-02 and PD-D10-dependent P5 shutdown acceptance remain blocked.

## 1. Verified base, scope and method

- Repository: existing Sentinel sanitized checkout, verified with `git rev-parse --show-toplevel`.
  Initial working tree clean. Fetched origin; main/origin/main identical, divergence `0 0`.
- Actual task base: **`e639a0429baf35445daad84eb70a9d5e001cf082`**.
  P3-05A test commit `44da8c74` is an ancestor of main; its report and PD-D10 proposal were read.
- Created `architecture/p3-05b-lease-integration-readiness` from main without overwriting a branch.
  Previously committed QA-01 and P2-M06 work was not merged or imported. On this base the shared
  endpoint adapter is still file-local in ModelService.cpp; later-branch line locations are not used.
- Read AGENTS, architecture, Agent Runtime, security, building/testing and source contracts.
  Personal-Brain `brain` unavailable on PATH; used the explicitly supplied verified checkout.
- Host macOS ARM64, Qt 6.12.0, AppleClang 21; approximately 88 GiB initially free. One coding agent;
  existing build/tests and cli/target reused. Only this document is changed.
- No clone, worktree, repository copy, separate build tree, commit, push, merge, reset, stash,
  rebase, clean, PR or workflow dispatch. No real model process was targeted or model artifact
  deleted. Existing tests use synthetic IO/owned helper processes and disposable file fixtures.

Source anchors below are repository-relative **base-SHA anchors**, not guarantees about future
line numbers. Callable compatibility APIs and unproven composition paths are explicitly separated
from observed production entry points. No model/provider was contacted to construct this inventory.

## 2. Production resource-consumer inventory

| Consumer / precise source seam | Actual admission/dispatch | Authoritative local completion and proposed retention boundary |
| --- | --- | --- |
| Interactive Agent | `core/src/app/ApplicationController.cpp:9142` startAgentLoopRun resolves workspace selection; bindAgentPlannerToResolvedModel at 9240 captures binding/provider. `core/src/agent/AgentRuntime.cpp:613` launch admits the session/turn; `LlmAgentRuntime.cpp:343` sendRequest or 349 bounded streaming performs inference | AgentRuntime commitResult at 1153 and finishTurn at 397 establish semantic terminal/approval state. They are not by themselves a worker join: the async finish closure at 1211 subsequently schedules context deletion and quits the worker. Retain through accepted final/failure/cancel **and** all outstanding request/child work reaching a local quiescence barrier |
| Agent planner retries, native tool continuation and observation classification | `LlmAgentRuntime.cpp:309` two-attempt planning loop; native history/options carried into each call. `core/src/agent/ObservationPolicy.cpp:230` ObservationIntentPolicy::classify makes its own bounded provider sendRequest at 298 | Return of each synchronous provider operation is the local request boundary. Reuse the enclosing run's retention lineage; classify/repair calls are real consumers, not free metadata. Neither a delta nor tool-result completion releases the root run retention |
| Controlled tasks | `core/src/app/ControlledTaskService.cpp:137` startTask creates/configures an Agent session and calls runtime.start at 228; cancellation delegates runtime.cancel at 243; onAgentEvent at 283 projects state | Same AgentRuntime lifecycle, not a second provider executor. Associate task ID with the existing run owner; do not count both the task projection and the same physical request as two independent dispatches. AwaitingApproval is nonterminal |
| Child/subagent | `AgentRuntime.cpp:894` forks the planner, 924 begins child turn, 1000 finishes it; `LlmAgentRuntime.cpp:131` forkForSubagent shares a concurrency-supported provider, constructs one from the same binding or serializes a shared provider | Child completion plus actual child request/worker quiescence, independently of parent UI state. Parent cancellation does not prove every child stopped. Provider mutex serialization is not process/artifact protection. Child owner includes parent lineage and its own execution incarnation |
| Chat | `core/src/chat/ChatModeService.cpp:218` resolves selection after policy checks; QThread dispatch at 312/317 calls streaming/sendRequest | acceptResult projects the message, while QThread::finished at 328 and workers_ bookkeeping/tryShutdown at 30 provide worker-lifetime seams. **stop at 418 marks the UI Cancelled and clears activeMessageId_ before the worker necessarily exits**. busy() is therefore not physical-use accounting; keep every old worker's retention after a new turn begins |
| Background conversation title | `apps/sentinel-daemon/service/DaemonIpcServer.cpp:1783` resolves provider/model; QThread::create at 1799 sends request at 1802 | Provider return + title worker completion at 1827; metadata callback suppression is not release. requestShutdown at 466 signals cancellation and polls wait(0) at 482; destructor also waits. Title work is absent from Chat busy and needs its own owner/retention |
| Controller local inference compatibility path | `ApplicationController.cpp:8337` runLocalInference and 8596 runLocalInferenceStream perform their own gates; dispatch through ILocalInferenceWorker, streaming at 8767 | finishLocalInferenceRequest at 8811 / finishLocalInferenceStreamRequest at 8885 are projection callbacks; acquire before accepted worker dispatch, rollback a rejected start, and release only after worker work is done. cancelLocalInference at 8552 is cancellation intent, not a physical release |
| Conversation summary | `ApplicationController.cpp:7202` startConversationSummaryInference constructs an Ollama request and calls localInferenceWorker_->startInference at 7261 | Same worker completion/quiescence rule as local inference, with a separate summary request ID. Summary completion/persistence is not permission to delete a serving artifact |
| Generic compaction API — callable, live composition not found | `core/src/session/CompactionService.cpp:184` generateSummary calls its borrowed IChatProvider::sendMessage; compactAsync at 133 invokes compact rather than establishing a separate physical supervisor | Provider return is a candidate local boundary. No production instantiation/wiring was found by source search, so do not claim this is an active daemon path. Before any wiring, replace the borrowed-pointer ambiguity with an owned execution context and retain its provider/resources for the full call |
| Real Ollama embeddings, including activation probe | `core/src/memory/SemanticRetrieval.cpp:657` embedText sends `/api/embed`; embed at 734 loops documents. `ApplicationController.cpp:2253` setSemanticProvider performs a **real embedding request** when enabled, despite being a settings/activation path | HTTP reply/error handling and QNetworkAccessManager/reply cleanup, not just loop quit/timeout, must complete before local quiescence. Actual activation probe is a consumer; merely changing a selection without that call is not. Endpoint/model are independent from the selected chat model |
| Isolated embedding API | `SemanticRetrieval.cpp:2998` generateIsolatedEmbeddings calls supplied provider.embed at 3089 after local policy checks | Retain around the full batch and transport completion for a real provider. FakeEmbeddingProvider performs no real model use and must stay labeled test-only. No assumed embedding-server ownership |
| Shared speech STT/TTS | `core/src/voice/UnifiedAudioService.cpp:292` WhisperSttRuntime::transcribeFile, 367 PiperTtsRuntime::synthesize, 507 KokoroTtsRuntime::synthesize call executeSpeechProcess at 196; VoiceSessionService uses QtConcurrent/watchers at 1142/1163 and synthesis at 1384 | Acquire before speech model/voice/config files can be opened by the owned child; retain until its executor has observed exit and local worker cleanup is complete. Future result/cancellation generation fences suppress stale UI updates but are not physical acknowledgements. Playback completion is a different resource from model synthesis completion |
| Direct speech consumers | `ApplicationController.cpp:3602`, `core/src/runtime/RealToolExecutor.cpp:317` and 2724 call shared TTS; authorized speech tools and VoiceSessionService transcription also use the shared runtime | Put the common retention seam inside the shared authorized runtime plus caller context, not only in Desktop. Existing executeSpeechProcess cancellation may kill its **own speech helper**; that existing behavior is not new model-server termination authority and was not exercised against a real speech model here |
| Retired speech compatibility clients | `core/src/voice/WhisperTranscription.cpp:363` LocalWhisperTranscriptionClient and `PiperTts.cpp:404` LocalPiperSynthesisClient explicitly refuse retired direct execution | Metadata/refusal paths do not acquire physical leases or pretend to run. Do not resurrect these clients as an integration shortcut |
| Generic background jobs | `core/src/job/BackgroundJobService.cpp:26` submitJob accepts a function; executeJob at 258 executes it | No model call is inherent to a generic job. A future model-using job must enter the same admission seam when dispatching. cancelJob at 47 can mark Cancelled while its function still runs, so persisted job terminal metadata is not quiescence |
| Daemon-owned serving process itself | `apps/sentinel-daemon/service/LocalModelRuntime.cpp:336` launches selected GGUF via private ProcessExecutor | **An idle but running llama-server still retains/reads its artifact.** Proposed M6 must cover a startup/residency artifact-reader pin from actual launch until verified process exit, independent of Agent/Chat request counts. Selection alone gets no pin. With no new stop policy, deletion may remain refused until the existing runtime actually closes |

Desktop/Rust CLI/TUI are IPC surfaces, not extra model engines. Legacy C++ CLI enters the existing
controller path (`apps/sentinel-cli/commands/ChatCommand.cpp:129`). IChatProvider adapters,
LocalInference clients and ProviderRequestRuntime are shared execution layers, not additional
logical consumer owners: do not double-count every delegation. Discovery, telemetry, model listing
and capability metadata are not model use unless they actually dispatch an inference/load.

### Critical completion distinctions

1. **Semantic terminal:** accepted AgentLoop final, failed/cancelled turn or Chat UI result.
2. **Local execution quiescence:** every associated worker/provider call and borrowed callback has
   stopped accessing the resource; joins/explicit completion proofs, not a UI flag, establish it.
3. **Serving-process/artifact quiescence:** process exit or an independently supported residency
   acknowledgement. Client transport abort/return cannot prove server compute/residency ended.

Request release requires semantic terminal plus local quiescence/child drain. A cancelled HTTP
request with unconfirmed server cancellation must not manufacture an idle-process/unloaded state.
Keep process-reconciliation posture Unknown where needed; M3/M4 remain gated. An owned serving
process's independent artifact-reader pin protects its mapped file even when no Agent turn runs.
This is a **Proposed refinement** of PD-D10/P3-05A, not adoption of a new production rule.

## 3. Existing lifecycle and destructive seams

| Source anchor | Existing action | Future serialization requirement / gap |
| --- | --- | --- |
| LocalModelRuntime.cpp:225 / 254 / 336 / 114 / destructor | ensureRunning early-returns for another provider; model-change guard terminates owned handle when !busy; start callback clears handle on failure/exit; beginShutdown stops probes, destructor shuts down private executor | Retain old identity across config change and physical teardown. Existing selectionRevision_ fences probes only, not all consumers. busy excludes title/embedding/old cancelled workers. Do not alter D4/D5b or add zero-count termination under this milestone |
| ModelService.cpp:1291 resolve; providerConfig and constructProvider | Captures provider/model/config/credential snapshot, constructs health-wrapped provider | Resolve is preparatory, not acquisition. Add a future resource-association snapshot without changing selection/factory override, readiness, auth or capability provenance. Shared_ptr protects C++ objects, not model files or server ownership |
| ModelOperationService.cpp:581 / 597 / 874 | RemoveRegistration, managed artifact removal, provider `/api/delete` | Reserve delete intent before dispatch, revalidate authorization and exact incarnation; serialize all aliases with admissions. Removal is not unload |
| ModelOperationService.cpp:789 / 809 / 821 / 250 / 254 | QSaveFile publication, sidecar-failure cleanup, failed-registration cleanup, journal/orphan cleanup | Publication/rollback must use a private provisional artifact incarnation until registration is committed. Cleanup of an old attempt must never remove a replacement or an already-published reader's artifact |
| ModelStorageManager.cpp:110 / 133 / 181 / 199 | Known-root/canonical sidecar ownership checks, file then sidecar removal, orphan-sidecar cleanup | Existing ownership proof remains mandatory. Boolean removal can fail **after file unlink**; future typed mutation result must distinguish unchanged, removed-with-sidecar-repair, and uncertain outcomes rather than reopening a stale identity |
| ModelLibrary.cpp:265 | Removes persisted GGUF registration, not necessarily the file | Pin registration revision during association; deduplicate aliases of a physical file. Decide metadata-only removal separately from physical deletion; recommend in-use refusal where removing the registration would invalidate protected association |
| runtime/OllamaRuntime.cpp:653 | OllamaModelPuller::removeModel independently exposes `/api/delete` | Callable API bypasses a ModelOperationService-only fence; current production UI wiring was not inferred. Future provider inventory mutation needs its own explicit namespace/authority and cannot prove absence of other applications' consumers |
| runtime/QtFileSystemService.cpp:348 / 400 / move implementation; RealToolExecutor.cpp:1816 | Authorized writes, deletes and moves through IFileSystemService, existing securefs final checks | Fence overwrite/rename/delete as mutations of protected artifacts, not merely a UI Remove button. Keep ToolExecutionGateway and AuthorizedPath validation authoritative; lease possession cannot satisfy them |
| patch/PatchParser.cpp:88 and authorized patch/file-editor paths | Generic patch path may remove/replace files | Inventory composition reachability before claiming coverage. Managed-artifact mutation must converge on a common guarded filesystem boundary or be excluded/refused; filename matching in a UI is insufficient |
| Authorized process/plugin/external-editor tools | Approved processes may mutate files without calling ModelStorageManager | A library counter cannot stop arbitrary subprocess writes. M1/M6/M8 must explicitly scope sandbox writable roots / broker mutations or exclude such access from the claimed managed-artifact guarantee. No new permissions or process killing may be inferred |
| DaemonService.cpp:19 / 34 and DaemonIpcServer.cpp:466 | Composition, cooperative shutdown and ordered teardown; title wait is separate from controller Chat/Agent drain | One daemon-owned ledger must outlive all borrowers. Extend observability/completion accounting only after approval; do not duplicate shutdown lifecycle or infer finite hard-stop |
| DaemonIpcServer.cpp:127 / 460 | Interrupted running/approval sessions become failed/daemon-restarted; new server_generation UUID | Projection reconciliation is not proof of child death or file quiescence. Never replay work, restore grants, adopt old handles or zero protection merely because generation changed |

External user tools/OS writes are outside a Sentinel-only lease guarantee. Native same-user file
mutation races require explicit ownership/security assumptions and independent testing; no portable
global lock is assumed. Catalog-cache eviction and generated audio cleanup are not serving-model
deletion, though publication/cleanup identity must still prevent cross-resource mistakes.

## 4. Proposed production contracts — specification, not compiled APIs

Names below are interface vocabulary for implementation review. **No headers or production types
were added.** Each result is typed and bounded; identifiers carry no credentials/prompts/raw paths
into public projections. There is no terminate, unload, permission-grant or arbitrary-token IPC API.

| ID / proposed interface | Inputs and result | Ownership, lifetime and failure contract |
| --- | --- | --- |
| L01 OwnedProcessIdentityResolver::snapshot | Private ExecutorInstance + opaque owned handle + launch incarnation + daemon epoch + config revision -> OwnedProcessIdentity or NotOwned/NotRunning/IdentityUnverified | Only original executor attests its own record. Include executor nonce/start incarnation to fence handle/PID reuse; PID/endpoint/name alone never suffice. Identity observation grants no destructive authority |
| L02 ManagedArtifactIdentityResolver::snapshot | Verified storage domain/root generation + registration/content metadata + opened-file identity/incarnation -> ManagedArtifactIdentity or Unmanaged/Stale/Ambiguous/Unavailable | Separate physical file identity from provider/model labels. POSIX device/inode and Windows volume/file identity require incarnation fencing; path or content hash alone is insufficient. Cross-provider registrations/hardlinks to the same file share accounting or fail closed when ambiguous |
| L03 LeaseOwnerFactory::newOwner | DaemonEpoch + consumer kind + run/turn/request/subrequest incarnation + optional parent -> internal OwnerIdentity | A reusable UI session ID is insufficient. Controlled task projection reuses its run lineage; each actual child/background request has an explicit owner. Never accept an owner ID as an authorization grant |
| L04 LeaseAdmission::reserveComposite | OwnerIdentity + immutable resource-association snapshot + existing authorization/readiness result -> PendingReservation or Cancelled/Closing/ResourceInUse/Stale/Quarantined/Unavailable | All required process/artifact references succeed atomically or none do. Dedupe resources within one reservation. No network/filesystem/process start under ledger lock; reservation acquires before actual dispatch, not selection |
| L05 PendingReservation::commit / abort | Owner/config/resource revisions, dispatch outcome -> ActiveRetention or RolledBack/Conflict | Revalidate generations and cancellation before commit. Failure to start/prepare releases provisional references exactly once. If a child actually started or file access escaped, transfer to a tracked startup/residency owner, not unconditional rollback |
| L06 LeaseCompletion::semanticTerminal / localQuiescent | Owner incarnation + typed terminal reason, internal worker/transport completion proof, child-drain acknowledgement -> Pending or ReleasedLocal | Release only when all required latches hold; duplicate/late events are idempotent. Guard destruction while active cannot silently release: unresolved work stays retained/diagnosable. Only internal owners supply completion proofs; UI/IPC terminal messages are not proofs |
| L07 ResourceUsageSnapshot | Resource incarnation -> retained owners, active local requests, startup/residency pins, unknown activity, delete/quarantine state | Counts cannot underflow/overflow or be mutated by UI. Distinguish retained readers from CPU-active inference. Zero local counts alone never proves a shared server idle or authorizes termination |
| L08 ArtifactMutationFence::begin / complete / abort | Existing mutation authority + exact managed identity + expected revision -> DeleteIntent or RefusedModelInUse/RecoveryUnverified/Stale/NotOwned; typed filesystem result -> Removed/RepairPending/RevalidatedOpen/Quarantined | Intent closes new admission for that incarnation. Unlink/replace still uses existing filesystem security checks. Partial failure cannot reopen an old removed file identity. No deferred delete queue in the refusal-only milestone |
| L09 DaemonEpochFence::beginEpoch / reconcileMetadata | Exclusive daemon startup claim + new epoch + reviewed prior clean/unclean shutdown metadata -> Ready or RecoveryUnverified quarantine | Old tickets cannot affect new counts. Do not import old lease counts as live grants, assume old children died or auto-reap them. Pending M7 forbids orphan adoption/kill; uncertainty blocks destructive admission, not fabricated recovery |
| L10 RetentionScope / ResourceAssociationResolver | Captured ModelBinding/config revision or speech/embedding model + verified owned-process/artifact association -> execution context with optional owned resources and explicit shared observations | Common orchestration seam covers every actual dispatch, including lower-level worker paths. Unknown external mappings stay observations, not invented owned artifacts. Run retention and per-request child tickets share explicit lineage without counting adapter delegation repeatedly |

### Thread safety, lifetime and failure semantics

- **Proposed default:** one daemon-composed serialized ledger/actor, typed queued requests from
  worker threads and same-thread internal transactions. All count/intent/generation transitions
  linearize there. No callbacks, joins, provider calls or disk IO while holding ledger state locks.
- Prefer asynchronous preflight before worker start. Existing synchronous embedding/controller
  paths need an explicit asynchronous admission boundary; never block the owner thread waiting
  for its own queued operation or add a nested event loop as a shortcut.
- Lock ordering must be specified against AgentRuntime workerMutex_/mutex_, eventMutex_, provider
  serialization, filesystem authorization and operation queues. No synchronous cross-lock callback;
  completion messages carry immutable values, not borrowed controller/provider pointers.
- A move-only provisional reservation rolls back only while no work escaped. A committed lease
  belongs to an owned work envelope retained through actual worker completion. Root run retention
  survives tool/approval waits; child/request tickets prevent parent terminal from dropping live
  dependencies. Destroying a UI/session wrapper does not destroy physical protection.
- Ledger lifetime extends beyond Chat workers_, Agent children, title workers, speech futures and
  legacy inference workers. Existing bounded shutdown false/Stopping retains the composition;
  no destructor may free a ledger still borrowed by a non-cooperative provider.
- Storage/start/readiness/queue failures return typed failures and roll back all provisional
  state. Uncertain process start, incomplete file mutation or missing quiescence proof enters
  RecoveryUnverified/QuiescenceUnconfirmed rather than success, negative counts or silent cleanup.
- Exact-once is per epoch/owner/resource token, not merely per session or callback type. Keep a
  bounded tombstone/incarnation strategy only after liveness rules are specified; never reclaim
  a possibly active owner by elapsed time alone. No new timeout or performance budget is adopted.

## 5. Race protocols and restart safety

### Admission versus mutation — Proposed linearization

1. Resolve stable resource association under the original binding/configuration and existing
   policy. UI selection changes neither that snapshot nor authority.
2. On the serialized ledger, reserve the entire resource set iff owner epoch/incarnation is live,
   no matching mutation intent exists, identities match and recovery posture permits it. Otherwise
   return a typed refusal; no partial acquisitions or implicit model/provider fallback.
3. Mutation uses the same serialization domain: obtain existing authorization, begin intent only
   when all relevant readers/pins are absent and posture is known. Mark Closing/Deleting before
   dispatching filesystem IO. Revalidate final authorization and file incarnation at the secure
   mutation boundary. The ledger is not an alternative authorization service.
4. Execute outside ledger locks while intent prevents new in-process admission. On success retire
   the incarnation. On failure re-probe the precise file/root identity before reopening; file
   removed + sidecar remaining is RepairPending, not an unchanged artifact.
5. Publish a replacement/download only with a new incarnation after validated commit. A stale
   request, cancelled download cleanup, old delete completion or stale callback cannot affect it.

Every mutation alias (registration, canonical path, provider label, hardlink) must resolve to the
same protected physical identity or be rejected as ambiguous. Root moves, atomic replacement,
same-path redownload and config A→B→A all require monotonically fenced incarnations. OS identity
reuse is not solved by a path string or PID check. Where the current filesystem layer cannot
provide a sufficient primitive/security assumption, enforcement readiness is blocked pending review.

### Terminal/cancel races

Cancel-before-reservation closes that owner incarnation; a delayed admission cannot resurrect it.
Cancel-after-reservation signals existing transport/worker cancellation but retains resources until
the completion latches hold. Semantic terminal before a worker reply, late reply after a newer turn,
duplicate terminal, nested child survival and release racing deletion are independent deterministic
cases. A callback carrying a stale request ID must not decrement the new turn's count.

### Startup, residency and daemon interruption

Acquire a provisional artifact reader before a serving/speech child can open model files. Failed
startup without any live child rolls back; ambiguous partial startup stays protected. On actual
owned serving start, retain its independent artifact-reader pin until verified exit. Consequently,
a refusal-only first milestone may truthfully refuse deletion of an **idle loaded** GGUF until
the existing process closes; it does not add an unload/stop action to satisfy the delete request.

An unclean daemon restart may leave a child or external server using an artifact even though the
new in-memory ledger has zero owners. Proposed default is fail-closed destructive quarantine for
affected managed identities (or the managed domain if affected identities cannot be established),
with explicit RecoveryUnverified projection. A minimal crash marker/intent checkpoint, separate
from settings, permissions and run stores, needs Qt SQL/storage/security design review before
implementation. It must contain no credentials and confer no recovery authority. No resurrection
of grants/mutations and no automatic PID/port/name orphan reaping. Clearing uncertain quarantine
requires a separately approved proof/review path; M7 recovery is not smuggled into this milestone.

## 6. Actionable maintainer approval record — all Proposed / unanswered

This is a review form, **not** a change to ACCEPTANCE_DECISIONS.md. Named reviewer, disposition,
scope amendments, date and evidence links remain blank until an explicit human decision.

| Decision | Proposed recommendation / options | First-milestone gate |
| --- | --- | --- |
| **M1** identity/ownership | Ratify separate owned-process attestation and physical artifact incarnation; shared HTTP remains external. Amend if different managed-root/alias scope is intended | **Required:** approve exact managed-file scope, alias proof and Unknown/RecoveryUnverified behavior; endpoint/PID never grants ownership |
| **M2** refuse/drain | Refuse conflicting destructive admission; no queued drain/deferred operation. Non-destructive future selection may proceed with frozen old binding | **Required:** authorize refusal behavior and typed compatibility errors only; no automatic process reconciliation |
| M3 idle child on switch | Remains separate: immediate stop / keep warm / until shutdown. No choice adopted | **Not enabled:** D4/D5b stay open. Zero count is not a stop instruction |
| M4 graceful/forced shutdown | No new escalation or deadline; existing cooperative Stopping remains truthful | **Not enabled:** requires separate PD-D04 calibration, owned-process and native supervision review |
| M5 supervised worker | Defer isolated-worker project; no hard-stop promise | **Not enabled:** new protocol/credential/security boundary requires separate approval |
| **M6** consumer coverage | All actual managed-artifact readers, including Agent/Chat/controlled/children/title/summary/classification/legacy inference, real embedding/speech as applicable, and actual owned-process residency pins | **Required:** approve the complete dispatch + mutation matrix and conservative idle-loaded refusal. A limited subset cannot claim global D6 protection |
| M7 orphan recovery | No adoption/reaping; fail-closed uncertainty/quarantine first. Eventual authenticated persisted-identity recovery is separate | **Not enabled:** exact recovery-clearance procedure/native proof must be reviewed; no process-name/port shortcuts |
| **M8** delete in use | Refuse, no deferred delete; managed physical mutation and registration semantics explicitly scoped. Shared-provider inventory administration remains a distinct approval scope | **Required:** approve in-use/uncertain/stale error semantics, partial-delete repair and filesystem/tool/process mutation coverage or exclusions |
| M9 no hard-stop claim | Keep AC-P5-04 non-cooperative branch blocked; no implied release acceptance | **Separate release disposition:** the first milestone may report truthful Stopping but cannot close shutdown acceptance |
| **M10** accountability | Named models/runtime, IPC, filesystem/security reviewers plus independent maintainer signoff | **Required:** actual people and dates, independent review and native evidence responsibilities; roles listed here are not assignments |

Minimum authorization is **M1 + M2 + M6 + M8 + M10**, with explicit scope and security review.
Unanswered decisions mean **do not activate**. M3/M4/M5/M7/M9 remain separately gated; accepting
the minimum packet neither approves PD-D10 globally nor closes a release/shutdown criterion.

## 7. Test-to-interface implementation matrix — future tests, not fixes

| Interface | Positive / failure / concurrency / cancellation / restart cases | Platform semantics and required native evidence |
| --- | --- | --- |
| L01 owned identity | Genuine executor attestation; unknown/exited handle; reused handle/PID; config ABA; cancellation during start; old epoch rejected | Fedora owned llama child vs external Ollama/LM Studio/llama, macOS launch lifecycle, Windows process identity. No external process termination test against a user server |
| L02 artifact identity | Valid managed file/aliases; unmanaged/root escape/sidecar mismatch; hardlinks and replacement; cancelled import; root/device and epoch change | Disposable POSIX open/unlink, symlink/hardlink/inode reuse; Windows open-file sharing/volume identity; case and canonicalization per OS |
| L03 owner | Distinct turns/subagents; reused UI session rejected; concurrent admission; cancellation before owner publication; stale epoch | IPC generation/reconnect on Fedora/macOS/Windows; no cross-session or persistent grant resurrection |
| L04 composite reserve | Process+artifact all-or-nothing; second resource refusal; delete/admit barrier; cancel before/after transaction; startup quarantine | Native filesystem/serving association for managed GGUF and separately versioned speech assets; fake transport is not residency proof |
| L05 rollback/commit | Commit once; readiness/start/queue/storage failure; abort vs late dispatch; child partially started; old commit after restart | Failed executable/permissions and loader failure in disposable environments; prove no leaked child/file access, without stopping real user models |
| L06 terminal/quiescence | Accepted final + worker drain; cancelled UI while worker lives; duplicate/reordered callbacks; parent/child races; old release after restart | Real cancellable and non-cooperative provider fixtures under supported native sandbox; HTTP abort cannot claim remote residency ended |
| L07 accounting | Two runs/one artifact; root plus child/residency pins; no negative/overflow; read snapshot during mutations; cancel priority; old-generation isolation | Native workload pressure and retained-reader explainability; no performance thresholds invented; low/high/shared GPU profiles separately calibrated |
| L08 mutation intent | Idle scoped delete; active/unknown refusal; intent/acquire/rename/cleanup races; cancellation of request does not drop intent early; partial unlink/sidecar failure and restart | Disposable installed file tree on Fedora/macOS/Windows; open handle behavior, authorized tool write/replace/delete and sandbox/broker bypass negatives |
| L09 epoch/quarantine | Clean metadata startup; interrupted run + live old child; ticket serial reuse; stale completion; metadata IO failure; restart during delete intent | Disposable daemon crash and native supervisor scenarios; no auto-kill/orphan adoption. Recovery uncertainty remains visible until separately approved proof |
| L10 execution context | Every inventoried dispatch, retry, child and shared observation; unmapped managed association; duplicate wrapper accounting; old cancelled worker/new turn; daemon teardown ordering | Real target backends/embedding/speech only after authorized assets and native environments exist; no cloud cost or process ownership inferred from mocked success |

Existing P3-05A reference fixtures supply design witnesses, **not** drop-in production code.
Production tests must call the actual new interfaces through the above source seams after approval.
Preserve authorization, redaction, capabilities, frozen binding and all existing negative tests.
D4/D5b stay XFAIL and D6 stays SKIP until underlying production behavior and native evidence are
actually fixed; a managed-file-only subset must not silently close the broader D6/AC-P3-02 scope.

## 8. Exact smallest milestone after explicit approval

1. Record the minimum five decisions with dates/reviewers and a narrow **managed-artifact refusal**
   scope, including residency readers, cross-alias identity, generic mutation boundaries and unclean
   restart behavior. No global shared-provider idle or hard-stop guarantee.
2. Implement inert internal L01–L10 identity/accounting interfaces and a daemon-composed ledger,
   separate from security grants. Add actual production-interface unit/race tests and a disabled
   integration path; prove no behavior change before activation. Do not copy the reference model.
3. Wire every in-scope dispatch and authoritative quiescence latch. Add startup/residency pins,
   rollback and restart uncertainty before allowing a zero-reader destructive decision. If a
   consumer/mutation path is unmapped, refuse the affected destructive admission or keep that
   scope disabled; do not announce comprehensive in-use protection.
4. After reviewers verify the coverage matrix, activate **refusal-only managed mutation fencing**
   at operation + common filesystem boundaries, retaining all existing authorization checks.
   Cover library metadata removal, download publication/cleanup and aliases; explicitly constrain
   subprocess/broker writes or keep the guarantee unaccepted. Shared provider `/api/delete` and
   external mutations need their own approved scope; they are not hidden behind a managed-file claim.
5. Validate deterministic production paths and disposable native GGUF/speech/open-file cases.
   Only then consider replacing a matching D6 subset witness. Do not close AC-P3-02 globally.
   Provider/endpoint process reconciliation, unload, forced shutdown, orphan reaping and keep-warm
   remain future M3/M4/M5/M7/M9 work; this milestone adds **no process stop action**.

## 9. Validation results

This document adds no tests or production code. Existing suites characterize the unchanged
application at the verified base; passing them does not establish production lease safety.

| Validation | Actual result |
|---|---|
| `CCACHE_DISABLE=1 cmake --preset tests` | Passed; reused the existing tests build tree |
| `CCACHE_DISABLE=1 cmake --build --preset tests -j4` | Passed; no compiler warning/error diagnostics in the captured build log |
| Focused lifecycle/ownership/runtime/IPC/chat/process/context/RAG/voice/persistence CTest selection, offscreen, verbose | 11/11 passed, 9.75 seconds; 225 QtTest passes and one preserved skip |
| Full `QT_QPA_PLATFORM=offscreen ctest --preset tests --output-on-failure -j1 -V` | 156/156 passed, 175.14 seconds; 141 QtTest suites report 2,848 passes, zero failures, nine skips, two expected failures and 72 warning messages |
| `cargo +1.94.0 fmt --manifest-path cli/Cargo.toml --all -- --check` | Passed |
| `cargo +1.94.0 clippy --manifest-path cli/Cargo.toml --workspace --all-targets --locked --offline -- -D warnings` | Passed |
| `cargo +1.94.0 test --manifest-path cli/Cargo.toml --workspace --locked --offline -- --test-threads=1` | Passed: 142 tests, zero failures/ignored; eight result groups including empty targets/doc-tests |
| `python3 tools/ipc/generate.py --check` and `python3 tools/ipc/generate_desktop.py --check` | Both passed; no schema/generated-contract change |
| `python3 tools/analysis/check_personal_paths.py --repo .` | Passed: zero files flagged |
| High-confidence credential-pattern scan of the new audit | Zero findings; Markdown fenced blocks balanced |
| `git diff --check` | Passed; final status contains only this untracked audit on the requested branch |
| C++ formatting/static analysis and QML lint | Not applicable to this documentation-only diff; no C++, QML or build configuration changed. Rust formatting and Clippy still ran as requested |

D4 `providerSwitchRetainsOwnedChild` and D5b
`endpointSwitchDoesNotReconcileOwnedChild` remain XFAIL. D6
`removalDuringActualUseRemainsBlocked` remains SKIP. The other eight full-suite skips
cover the Linux Secret Service helper on macOS, missing native incremental tool assembly,
four strict macOS child-process shell fixtures, opt-in live classification, and the
missing-node branch when `npx` is installed. Warning messages are not native acceptance
evidence or repaired defects.

Validation ran on macOS ARM64, not a Fedora KDE session. No native Fedora/Windows
identity, restart, filesystem-sharing or model-residency acceptance is claimed. Tests use
existing isolated fixtures; no real user model process was terminated and no user model
artifact was deleted. Build/test logs remain local rather than publishing runner paths.
The Git base, main and origin/main still match the SHA in section 1; no production
enforcement, approved decision or acceptance status changed.

## 10. Exact changed files

- `docs/audits/P3_05B_PRODUCTION_LEASE_INTEGRATION_READINESS_RESULT.md` only.

No approved decision/acceptance record, test expectation, schema, runtime or provider source changed.
All changes remain uncommitted for maintainer review.
