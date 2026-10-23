# Agent completion false negatives — 2026-10-23

## Reproduced causes

The initial checkout was main at 2618d384. No Personal-Brain executable was
available in this session. Architecture, Agent Runtime, security, build and test
instructions were read before editing.

Three false-negative regressions were reproduced against the original compiled implementation:

1. `ControlledAgentTaskService::applyRuntimeState("Completed", nonempty final)`
   assigned `Cancelled`. The new persistence round-trip test failed with enum
   value 6 instead of Completed (4). This was a projection/domain transition bug,
   independent of model quality or tool execution.
2. Both `AgentLoop::advance` and `advanceAsync` checked the productive tool budget
   before asking for the final decision. With one allowed tool step and a final
   queued next, both returned Failed (6), not Completed (4). The fixed loop allows
   final planning at the cap while refusing every additional tool dispatch.
3. `LlmAgentRuntime::decisionFromObject` rejected an empty final answer before
   the evidence-derived presentation fallback. The content-free check also ran
   before that fallback. The new Ollama, LM Studio and llama.cpp binding rows all
   returned GiveUp (2), not FinalAnswer (1), despite a structurally valid verified
   final proposal. Empty verified finals may now reach the existing evidence
   fallback and then the authoritative AgentLoop gate. A receipt-only proposal
   still cannot prove completion.

A separate live false-positive exposed another root cause. Native assistant prose
was automatically promoted to a Verified final using whichever read evidence
existed, without an explicit final decision. A real llama.cpp run said that the
file had not been modified, emitted Completed, and failed the independent AST/test
oracle. This acceptance failure is retained in the live evidence. After tool use,
native providers now must return an explicit final/giveup decision with declared
grounding and claims. Unstructured prose gets the existing two-attempt repair;
it is never upgraded to verified completion. Conversational tool-free replies
remain direct. The final answer string is still displayed as ordinary prose.

Additional boundary defects found during the trace:

- ApplicationController reconstructed runtime state without terminal reason,
  intent, evidence, grounding or claims, then used a blanket failure heading.
- A verified final after a skipped repeated mutation was rejected before the
  evidence gate. Repeated writes after an intervening independent read could also
  be replayed. The identical mutation is now skipped and one final repair can
  resolve it through the full existing gate; unverified repeats still stop.
- An accepted unable-to-verify response could mark an already mutated task
  Completed. Such a task now ends VerificationFailed; read-only factual inability
  responses retain their existing behavior.

## Contract and recovery

Execution phase and completion assessment remain separate. Existing wire phases
and persisted execution states are unchanged. Additive completion metadata derives
from authoritative phase/reason: Completed, Failed, Blocked, Verification Pending,
Verification Failed and Completion Indeterminate. Runtime error codes distinguish
verification/indeterminacy from execution failure. Controlled task JSON preserves
its completion assessment. Agent history encoding, inspector labels, daemon
terminal events and desktop presentation use the same projection.

Protocol optional metadata is defined in protocol/ipc-v1.json and regenerated for
Qt/Rust. A pending verification state does not unbind a running controlled task.
Historical run reason and evidence continue through the existing SQLite owner.
No new persistence store or heuristic command execution was introduced.

Only an accepted final marks Completed. The existing two-attempt planner repair,
claim IDs/verdicts, context/output admission, post-mutation observation, read-only
recovery limit, permissions, approvals, sandbox and persistence preflight remain
mandatory. Truncated, malformed, missing or contradictory output never creates
successful evidence. Context exhaustion and malformed final exhaustion do not
imply rollback. There is no fallback that marks arbitrary successful tools done.

## Regression coverage

The production-provider fixture substitutes inference only; tools, filesystem,
AgentRuntime, gateway, approval, daemon IPC and desktop projection remain real.
The matrix covers Ollama, LM Studio and llama.cpp identities with independent
fixture-byte checks and an unrelated-file preservation check:

- create, edit, empty verified final, initially rejected final;
- malformed final exhaustion, context exhaustion at finalization;
- classifier omits a directly requested file action; a read-only final cannot complete it;
- successful mutation with contradictory requested content;
- external resource loss between mutation and read-back;
- repeated mutation after independent read-back, without reexecution;
- runtime/persisted run/IPC/DesktopRuntimeClient completion agreement.

The original security-negative fixture rows remain in the same test target.
Sync/async budget-boundary tests ensure final planning is possible but an extra
mutation/tool is not. The reliability harness verifies fabricated claims remain
rejected and missing read-back can recover through the real gateway.

## Validation

Final tests-preset configure/build passed. The final full CTest run passed
**180/180 targets in 75.17 seconds**. The provider E2E target includes 27 new
completion rows (three provider identities by nine scenarios), plus the existing
security-negative rows. Runtime, persistence, IPC and real DesktopRuntimeClient
submission/projection are asserted together. The tests include explicit user
approval and independent bytes/unrelated-file checks.

Rust workspace: **144 passed**; all-target Clippy with warnings denied and rustfmt
passed. Changed-line C++ format, QML lint, protocol generation check, packaging
consistency, whitespace and personal-path scans passed. Desktop validation is
through real offscreen service/client/QML test flows, not manual native interaction.

The first focused run passed all five targets (controlled tasks, loop, reliability
harness, LLM planner, provider E2E); further matrix rows were then added and passed in the final full run.

## Live provider evidence and limits

LM Studio on loopback port 1234 and Ollama on 11434 were unavailable. No models
were installed, downloaded, loaded or redirected to impersonate these providers.
llama.cpp on 8080 exposed sentinel-nemotron with an 8192-token loaded context.
The isolated live helper now accepts an explicit registered local provider;
LM Studio remains its backward-compatible default.

The first llama.cpp attempt completed two read tools but exceeded the helper's
120-second acceptance deadline before terminal completion. This is a failed
acceptance attempt, not proof of a successful task or false-negative terminal.
The 300-second attempt completed the read-only inspect case. The broader edit/test
case failed before any tool because classification exceeded the eight-entry
contract; no success was claimed. A narrow edit/read-back run then exposed the
native-prose false-positive above: Completed while the trusted oracle failed.
Explicit native final format alone was insufficient: the classifier had omitted
the requested action, and a structured factual final could still pass using reads
only. A narrow explicit-imperative/concrete-file consistency floor now retains
that operation proof obligation even when classification omits it. It only adds
requirements; it never generates a tool, command, permission or filesystem access.
Bare filenames reuse an unambiguous classifier-resolved path where possible.
Negated requests and explanatory/writing-about-path cases have regressions.
Ambiguous phrasing and other languages/forms still rely on semantic classification.

With the final code the same live goal ended `run.failed` /
`Completion Indeterminate` / `ContextBudgetExhausted` **before any tool**: the
loaded 8192-token context could not admit the required action contracts (9776
required plus a 1638 output reservation). The independent test still failed,
correctly, and no mutation success is claimed. Full-tool-set mutation acceptance has not
passed on this model/configuration. No context/model settings were changed to
hide that failure. The compact [live evidence](AGENT_COMPLETION_LIVE_2026_10_23.json)
retains both pre-fix false positives and final safe refusal. Fixture data only
was supplied; no user files or credentials were used.

A final real **restricted-tool** acceptance used the existing certification probe
with write-file, read-file and stat-path only. The provider identity and loaded
model remained llama-cpp-server/sentinel-nemotron. The run executed write-file,
read-file, then an accepted final: **Completed in two tool steps**. The helper's
independent byte comparison passed (`fixture_content_verified=true`), and a
separate read confirmed exactly 16 bytes `SENTINEL_FILE_OK`. Permissions, sandbox
and final gates were unchanged; only the test's exposed tool contract was narrowed.
This proves a real file round-trip on this model, not that the full Desktop tool
catalog fits its 8192-token context.

The probe now accepts `SENTINEL_CERTIFICATION_LOCAL_ENDPOINT` for an explicit
clean loopback llama.cpp endpoint (default remains port 8081). It rejects remote,
credential-bearing, query/fragment endpoints. The test used port 8080. The normal
live helper also supports `--provider` and `--completion-only`, preserving its
independent trusted AST/test oracle and isolated profile.

Deterministic binding rows are not certification of every model or live transport.
Arbitrary natural-language goal satisfaction still depends on the classified
requirements and model decision; the fix does not pretend a read-back proves all
possible semantic goals. Cross-platform CI/native Fedora and Windows acceptance
remain separate from local macOS results.

## Changed files

- `apps/sentinel-daemon/service/DaemonIpcServer.cpp`
- `apps/sentinel-desktop/src/DesktopRuntimeClient.cpp`
- `apps/sentinel-desktop/src/viewmodels/AgentInspectorViewModel.cpp`
- `cli/crates/sentinel-ipc/src/contract.rs`
- `core/include/sentinel/core/agent/AgentEvent.h`
- `core/include/sentinel/core/agent/AgentHistoryCodec.h`
- `core/include/sentinel/core/agent/AgentLoopState.h`
- `core/include/sentinel/core/agent/ControlledAgentTasks.h`
- `core/include/sentinel/core/agent/IAgentRuntime.h`
- `core/include/sentinel/core/agent/IAgentStepPlanner.h`
- `core/src/agent/AgentLoop.cpp`
- `core/src/agent/AgentRuntime.cpp`
- `core/src/agent/ControlledAgentTasks.cpp`
- `core/src/agent/LlmAgentRuntime.cpp`
- `core/src/agent/ObservationPolicy.cpp`
- `core/src/app/ApplicationController.cpp`
- `core/src/app/ControlledTaskService.cpp`
- `docs/audits/AGENT_COMPLETION_FALSE_NEGATIVES_2026_10_23.md`
- `docs/audits/AGENT_COMPLETION_LIVE_2026_10_23.json`
- `docs/concepts/AGENT_RUNTIME.md`
- `protocol/QtIpcContract.generated.h`
- `protocol/ipc-v1.json`
- `tests/CMakeLists.txt`
- `tests/core/test_agent_loop.cpp`
- `tests/core/test_agent_reliability_harness.cpp`
- `tests/core/test_controlled_agent_tasks.cpp`
- `tests/core/test_llm_agent_runtime.cpp`
- `tests/core/test_observation_policy.cpp`
- `tests/desktop/test_provider_agent_e2e.cpp`
- `tests/helpers/provider_certification_probe.cpp`
- `tools/agent/README.md`
- `tools/agent/live_coding.py`
- `ui/qml/pages/AgentInspectorPage.qml`


## LM Studio follow-up acceptance — 2026-10-23

LM Studio became available at loopback port 1234. Its native model catalog
confirmed `qwen/qwen3.5-9b` was already loaded with a 262144-token context.
No model was loaded/downloaded and no user profile or server setting was changed.
Ollama is explicitly deferred.

Two isolated full-daemon `--completion-only` runs used the real LM Studio
transport, native tools, permission approval, AgentLoop and IPC. Both changed
`calc.py` from subtraction to addition and successfully read it back. The trusted
AST verifier and arithmetic tests passed; unrelated source and tests were preserved.
The first run reached the final planner repair but exceeded its 300-second harness
limit without a terminal event. Its independent verifier was run separately after
the harness stopped; this is not an accepted final or live acceptance pass.

The second run, bounded to 600 seconds with diagnostic logging, reached a terminal
result: `run.failed`, `completion_status=Completion Indeterminate`,
`terminal_reason=CompletionIndeterminate`. After read/edit/read the model requested
`todo-read`, then repeatedly requested the already observed `read-file` instead of
an explicit accepted final. The single repeated-read repair was exhausted. No
additional mutation was dispatched and no rollback was reported. There was no
context-budget error. This demonstrates a remaining model/finalization reliability
failure, not an execution failure, successful acceptance, or UI projection mismatch.
The existing acceptance gate correctly refused to turn successful tool execution
into Completed. It does not establish that an accepted final is possible for this
model on this flow; further finalization recovery work remains.

Compact sanitized evidence is in
[LM Studio acceptance evidence](AGENT_COMPLETION_LM_STUDIO_2026_10_23.json).
These follow-up runs do not modify production code or replace the earlier
180-target deterministic regression result.


## User screenshot follow-up — 2026-10-23

Read-only inspection of the four matching persisted daemon runs separates the
reported symptoms. The command attempts returned `SecurityDenied`, with sandbox
category `WorkingDirectoryTooBroad`; approvals did not make execution succeed.
The root-level `/main.cpp` writes failed. Another run exhausted native output
(`finish_reason=length`); its incomplete response was correctly rejected. The
classification HTTP 400 at 15:39:01 local time corresponds to the LM Studio server
log rejecting the Qwen model load due to insufficient system resources. This is
not evidence of a malformed Sentinel classification request. No model loading
protection or sandbox policy was changed.

A contributing tool-discovery defect was fixed: focused local filesystem tool
selection hid the read-only `system-info` tool even though the planner instructs
models to use its OS-resolved known directories. It is now visible for inspection
and mutation tasks, still subject to enabled/exposure/platform/profile filters.
The existing profile-filter assertion continues to exclude it when not allowed.

The sync and async repeated-call stop paths now retain `SecurityDenied` and the
sandbox failure category, producing the existing `Blocked` completion projection
instead of replacing the actual blocker with generic `ToolFailure`. The repeated
call remains undispatched, and no accepted final or successful execution is
invented. New sync/async regression rows verify one executor invocation, empty
final, preserved `WorkingDirectoryTooBroad`, and `Blocked` projection.

This improves discovery and diagnosis; it does not claim to fix all Qwen final
repetition, truncated output, root-path planning, or resource exhaustion. Existing
persisted runs are not rewritten, and the running release binary must be rebuilt
to contain these source changes.


Follow-up validation: tests preset configure and full build passed, and the release
preset build passed. The full CTest run passed 179/180 targets in 308.86 seconds;
`test_desktop_ipc` failed only because its cloud fixture socket path was 124 bytes
against the macOS 103-byte limit. Re-running that target with `TMPDIR=/tmp` passed
in 32.38 seconds. Thus every target passed, with the documented environment retry.
The new security-denial rows and focused tool/profile regression passed. Changed
C++ files passed clang-format verification and the diff passed whitespace checks.
The release executable was rebuilt without terminating the user's running process;
a restart is required to use the changes. No live LM Studio acceptance pass is
claimed for these follow-up source fixes.


### Post-restart workspace denial and malformed blocker response

The user run shown in the 16:27 screenshot used `system-info` successfully, but its
`run-command` result was `Blocked: workdir is outside the approved workspace`.
The workspace was the Sentinel repository while the requested destination was
Desktop. The LM Studio server log confirms the model then explained that scope
mismatch in prose. Such prose is not an accepted final/giveup decision after tools.
The native repair prompt nevertheless said to "give a specific final answer",
contradicting the required explicit protocol. It now supplies final/giveup JSON
examples, requires exact evidence claims, and explicitly forbids claiming completion
after denial. The two-attempt limit, gateway and acceptance gate are unchanged.
Exhausted response repair now identifies the missing accepted decision and refers
to recorded tool denials/workspace scope when present, without inventing success
or implying that mutations occurred. All 70 LlmAgentRuntime test cases passed;
release daemon and desktop rebuilt. No real model acceptance pass is inferred.
