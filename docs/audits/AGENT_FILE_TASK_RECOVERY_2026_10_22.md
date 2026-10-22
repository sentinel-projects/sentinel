# Agent file task recovery — 2026-10-22

## Observed failures

A real Evren `cloud-api` / `auto` Agent run requested a Desktop file but attempted `/Desktop/deneme.txt`. The write was rejected with SecurityDenied / ResourceChanged. The subsequent HTTP 200 provider response had no visible assistant text, and the run stopped with MalformedResponse.

Live reproductions exposed two more boundaries: a created/read-back file could still be rejected because the model invented final claim IDs, and a localized Desktop request could produce evidence requirements for a different path than the tool actually observed. Production also uses bounded planner streaming; its empty-text failure needed the same explicit recovery marker as non-streaming.

## Changes

- `standardUserDirectories()` provides Qt-resolved home/Desktop/Documents/Downloads metadata to both observation classification and the `system-info` tool. Classification reserves room for these trusted paths while retaining the workspace suffix and treats negated actions as prohibitions, not requested operations. Paths are metadata, never grants; no approval or filesystem checks are bypassed.
- `stat-path` emits `regularFile`, while claim resolution previously read nonexistent `file` and defaulted it to false. Resolution now consumes the actual boolean contract; absent or non-boolean existence metadata remains Unknown. A real executor-to-resolver regression verifies a regular file is supported and missing metadata cannot prove absence.
- Planner instructions resolve named folders through that metadata, inspect changed/missing parents, and use only supplied claim IDs. The example no longer encourages an invented `claim-1`, and the required assertion shape explicitly uses the parser/schema field `assertion`. Grounding validation remains authoritative.
- Completed HTTP 200 replies with empty visible text are marked explicitly. Before emitting any planner output, one repair may use the existing finite two-attempt planner budget, with the same output limit, cancellation and context admission. A failed or empty provider reply never becomes a successful tool observation.
- OpenAI-compatible cloud non-streaming and streaming `finish_reason:length` responses fail as output-limit rejection. Partial or private reasoning text is never accepted as a final answer. Safe non-streaming diagnostics contain finish reason/token counts, not response bodies or reasoning.
- A real read-only Desktop run exposed a 2048-token planner truncation after approval. Explicit output-limit failures now permit one fresh decision with up to 4096 output tokens, admitted again by ContextBudgetManager and capped by the bound model. No tool executes from discarded partial JSON; a second truncation still fails.
- The opt-in provider certification helper accepts an explicit clean HTTPS Cloud endpoint and a canonical disposable workspace. `file-roundtrip` refuses to overwrite an existing fixture and restricts filesystem path schemas to one fixed file. Production permissions are unchanged.

- A later application run crashed during the TLS handshake in `libcrypto.4.dylib` (`OSSL_DECODER_up_ref`, called by Qt's OpenSSL backend). macOS daemon bootstrap now selects Qt's available native `securetransport` backend before starting workers, retaining certificate/hostname checks. The underlying library defect is not established; this avoids that observed crash path. Linux/Windows backend selection is unchanged. See [Qt TLS backend selection](https://doc.qt.io/qt-6.12/qsslsocket.html#setActiveBackend).

## Evidence

Regressions cover empty and truncated cloud responses, bounded streaming recovery, the one-repair limit, standard-directory metadata and localized Desktop classification. Existing grounding, filesystem and cancellation tests remain in the full suite.

A live Evren disposable run created `agent-roundtrip.txt` containing exactly `SENTINEL_FILE_OK`, read it back, and reached AgentLoop Completed in two tool steps. The helper's independent byte comparison passed. This also passed with the production bounded-streaming planner path enabled. The final-code probe completed in four tool steps: write succeeded, one read was blocked, stat succeeded, then a fresh read succeeded. AgentLoop Completed and the independent byte comparison both passed; the blocked read was not counted as successful evidence. An earlier live run that wrote/read the file but failed unknown-claim validation was retained as a failure, not counted as success.

The real Desktop retry created `deneme.txt` at the actual Desktop location. Final read-only application run `ad089e16-e132-401a-866a-188d9ad97d95` reached AgentLoop Completed with verified grounding, three recorded steps and no failure. The UI reported the observed file content in Turkish. Existing user files were not overwritten or deleted during validation.

Release daemon and full tests-preset builds passed. The existing Release desktop application exercised the updated remote daemon. The final runtime test target passed 63 cases; observation-policy and real-executor contract tests passed. The final full suite passed all 180 CTest targets in 290.51 seconds (`TMPDIR=/tmp QT_QPA_PLATFORM=offscreen ctest --preset tests --output-on-failure`). Diff formatting and the personal-path scan passed.

## Scope

Evidence covers Evren's `auto` alias on this host. The backend behind that alias may change. This does not certify all Evren models, native tool calls, vision, or all providers. The change is stacked on the custom Cloud gateway correction in PR #103.
