# ADR-0005 · Rust desktop and runtime migration

Status: Proposed architecture with an isolated Rust development runtime and desktop experiment; no production replacement is accepted.

## Context

The current Qt desktop, Rust CLI and Rust TUI already communicate with the C++ daemon through local IPC. The desktop loads presentation and integration code, while the daemon owns model requests, persistence and Agent execution. The legacy C++ CLI still has a separate in-process runtime path.

The migration targets low idle resource use, responsive rendering and native desktop integration on Windows, macOS and Linux. Fedora KDE/Wayland is one of the Linux acceptance environments.

## Design

- Keep one headless Rust daemon as the eventual runtime owner. The desktop, CLI and TUI use the versioned `protocol/ipc-v1.json` contract. Preserve request correlation, bounded frames and queues, reattachment and no automatic mutation replay.
- Evaluate Slint with Winit for a shared desktop, with platform adapters for global shortcuts, activation, tray and file pickers. Keep runtime state, credentials, store access and tool authority in the daemon. The experiment uses the C++ daemon while the frontend cost is measured independently.
- Preserve Unix same-user endpoints on macOS and Linux. A Windows named-pipe client implementation is present and cross-compiles; verify Qt interoperability and local-user access controls natively before claiming Windows support.
- Preserve separate persistence domains and SQLite compatibility. Establish one writer per profile across runtime implementations, including upgrade and rollback. Prove interrupted upgrades and downgrade behavior on disposable copies of supported profiles.
- Preserve the Agent gateway, frozen model binding, approval and permission decisions, audit-before-dispatch semantics, cancellation and verified completion across the port. Keep ADR-0001 active for the shipped desktop until the replacement passes its acceptance checks.

## First experiment and acceptance

The standalone Slint prototype exercises one chat session, bounded streaming preview, explicit cancellation and manual reattachment. See [how to build and run it](../../tools/slint-desktop-prototype/README.md). AC-P5-02/03/05 cover transport and recovery, AC-P6-06/07 cover the desktop and system activation, and AC-P8-03 covers profile compatibility.

Measure Release binaries with the same fixture, model event sequence and display settings. Record process-attributed idle CPU and memory with the panel hidden, cold startup, shortcut-to-focused-input p95 and frame time while scrolling a long streaming response. Use native Windows, macOS, Linux Wayland and Linux X11 runs. Require lower idle resource use and no responsiveness regression against each platform's current desktop. Set numeric budgets after recording comparable baselines.

Verify disconnected, empty, loading, error and compact states, keyboard navigation and screen-reader semantics. A provider fixture proves IPC and rendering, while native portal/activation and packaged upgrade checks require real platform runs. The prototype makes no production toolkit, performance or platform acceptance claim.

## First Rust runtime layer

The experimental `sentinel-ipc::server` layer reads bounded LF-delimited frames on Unix with an absolute per-frame deadline and dispatches generated typed `hello` and `daemon.status` requests. Its integration test owns a newly created private temporary directory and listener; it has no profile access, runtime authority, or production endpoint. Other commands fail explicitly. The portable dispatcher is shared code, while the socket reader is Unix-specific. The C++ daemon remains the production owner.

A production Rust daemon still requires compatible exclusive endpoint and profile ownership, same-user peer checks, safe stale-endpoint handling, bounded client/output queues, runtime and storage ports, and cross-platform upgrade/rollback and native security acceptance. A Windows named-pipe client implementation now cross-compiles, but native interoperability/security acceptance remains unverified; the separate isolated Windows development listener now includes a bounded local Chat worker and private profile snapshots in source. Its profile and named pipe are independent of Qt. Interrupted Windows runs are recovered without replay. Native Windows pipe, security, cancellation and interoperability testing remains outstanding. This layer does not change the provisional Slint experiment or establish toolkit, runtime, performance, or platform acceptance.

The development-only `sentinel-daemon-dev --profile /tmp/PRIVATE_DIRECTORY` binary now exercises the same Rust dispatcher through a real Unix listener on Linux and macOS. It requires an existing owner-only direct child of `/tmp` containing only its own lock, socket and development SQLite artifacts, holds its own profile lock for its lifetime, checks peer UID, limits concurrent same-user clients with per-frame and per-client deadlines, and refuses all pre-existing endpoints rather than deleting stale artifacts. A successful `daemon.shutdown` removes the socket it created; an interrupted process may require manual inspection and removal of its stale socket. It also supports development-only durable sessions and one streaming Chat with an explicitly configured literal-loopback provider, cancellation and reattachment. Agent, tools, production profiles and runtime stores remain unavailable. Its lock is deliberately not a Qt `QLockFile` interoperability or production single-writer solution. Production endpoints, full runtime and store ownership, Windows, crash recovery and cross-version upgrades remain gated.

The development Chat freezes its provider and model for a run and only completes after a valid terminal provider stream. Committed session and message state is stored in a dedicated development SQLite file; disconnect does not cancel. Failed and cancelled runs retain partial output in their attach snapshots, while their terminal events explicitly report failure or cancellation. Bounded client queues disconnect slow peers rather than replay or silently drop events. The [development daemon guide](../../cli/crates/sentinel-daemon-dev/README.md) records its limits and startup contract.

The development store uses a distinct application ID and version, owner-private artifacts, a process-lifetime profile lock, bundled SQLite, DELETE journaling and full synchronous commits. It records the user message and running identity transactionally before dispatch, and commits terminal state and assistant output before terminal publication. Startup turns unfinished runs into `daemon-restarted` failures without replay; in-flight uncommitted deltas are lost. A storage error prevents new mutations until restart, except a busy/locked refusal before a write transaction begins, which can be retried manually. It does not open production Qt profiles or prove cross-runtime locking. An experimental importer can read a narrowly validated, quiescent Qt conversation database copy into an empty development store; that transcript import is not a profile migration. `session.messages` projects committed rows and refuses responses exceeding the IPC frame limit. AC-P8-03 upgrade/rollback and real profile ownership remain open.

### Development storage evidence and remaining gates

The development store has one private owner and a bounded 16-turn history per
session; its schema remains distinct from Qt. Acceptance oracles for this slice are:

- An acknowledged run has a committed user row before the provider sees it;
  a failed write dispatches nothing. A busy lock before transaction entry admits
  a later manual retry, without automatic replay.
- A terminal write commits the run state and assistant row together; a failed
  terminal transaction publishes `run.failed` with `storage-failure`, blocks new
  mutations and does not publish completion.
- After interruption, reopening the same development profile marks a running
  record failed with `daemon-restarted`, retains the user row, records an
  interrupted assistant row and never contacts the provider. Completed and
  cancelled terminal rows and stable session identities survive orderly restart.
- Unknown/newer schemas, corrupt/empty databases and unsafe linked artifacts are
  refused rather than migrated or overwritten; over-limit history is rejected
  on the wire without silent truncation.

The scoped source and fixture checks exercise those boundaries on macOS; the
production C++ daemon, Qt stores, provider semantics and GUI were not migrated
or audited by these tests. Linux-native storage/peer behavior remains to be run.
The P5 matrix's cross-process admission and recovery follow-up (AC-P5-01–05)
requires native tests with the eventual shared profile lock. The P8 matrix's
profile compatibility and rollback follow-up (AC-P8-02/03) requires an explicit
cross-runtime import/rollback design and interruption tests on disposable copies
of all production stores. Until then, neither phase is accepted by this slice.


### Scoped Qt transcript copy under AC-P8-03

The development daemon has an experimental, one-way import path for a finalized, quiescent disposable copy of Qt `conversations.sqlite3` schema version 6. It validates a bounded subset and imports into an empty development database in one transaction. Unsupported schema, conversation metadata, summary rows, message states, error categories and attachments are rejected rather than silently discarded. Source quiescence is a caller precondition; Qt uses WAL, so an arbitrary copy of the database file alone is not a reliable snapshot. The source is opened read-only and the importer is restricted to separate private temporary directories. This path is under review and does not establish an accepted migration or tested compatibility claim.

The imported data is a transcript subset. It does not import Qt's separate session-run projection, chat-history index, settings, credentials, memory, agent runs or permissions, and its development profile lock does not coordinate with Qt. Imported session attachment is initially idle with empty output and no imported run identity; `session.messages` exposes the preserved transcript. The importer rejects transcripts that would exceed the unpaged response bound. Later native turns receive logical IDs above the imported maximum. AC-P8-03 remains open: cross-runtime exclusive ownership, a consistent multi-store snapshot, interrupted upgrade and downgrade recovery, Qt-to-Rust and Rust-to-Qt behavior, and rollback must be designed and tested on disposable copies of every affected store before any production-profile migration is claimed.

The development-only v1-to-v2 database migration adds per-session logical message IDs, reply/replacement references and conversation timestamps transactionally; version-1 data keeps its prior message IDs. The one-shot importer requires an already initialized current-version development store, preserves supported v6 Qt transcript rows, and holds a read transaction on a finalized rollback-mode copy while committing the destination. Source quiescence is a caller precondition, not a claim of coordination with Qt. Unknown, incomplete or unrepresentable source data is refused before a successful import. Older development binaries refuse newer development schemas.

On macOS, a disposable fixture produced by the actual Qt/C++ daemon contained two conversations and four messages, including system messages. After import and Rust daemon restart, Rust IPC returned the fixture's IDs, timestamps, roles, content, statuses, provider/model values and reply references unchanged; the source database SHA-256 remained unchanged. The Rust CLI then completed a new local Chat in the imported conversation, emitting message IDs 4 and 5 after imported IDs 1, 2 and 3. The same scoped verification rejected an unknown schema, a nonempty destination and a missing destination. A later oversized conversation was rejected with zero target rows and both source and destination file hashes unchanged. This is evidence for the tested transcript subset only; it does not establish preservation of Qt's runtime projection or multi-store profile compatibility.


Chat store v6 retains supported Qt provider failure categories on historical
assistant messages and exposes them through `session.messages`. The category
is retained when a conversation branches and is deleted with its message.
The v6 upgrade keeps existing history and session flags. Coordinated restores
continue accepting exact v4 and v5 Chat schemas and upgrade them before
publication. The new metadata is descriptive and grants no runtime authority.

### Development Chat context admission

The initial development Chat context slice sent the current prompt and eligible complete prior turns from the same session. That byte-only admission has since been extended by the context estimate below. It admits the mandatory current prompt first, then takes newest whole user/assistant pairs until the next pair would exceed 131,072 bytes in the serialized JSON request body, including the selected model and framing; selected pairs are restored to chronological user order. A mandatory body that does not fit returns `message-too-large` before persistence and provider dispatch. Eligible user rows are nonempty, nonpartial and sent, received or completed; eligible assistant rows are nonempty, nonpartial and received or completed. Native pairs must belong to the same completed durable run. Imported Qt answers must explicitly reference an earlier user row in the same imported conversation and be completed or received and nonpartial; the latest valid answer is selected, while failed or partial replacements do not suppress an earlier successful answer. Imported system rows are omitted, not promoted to instructions. These rules protect the development path from silently elevating untrusted imported roles; they do not implement all production Qt context, scoped memory, attachment or policy semantics.

A macOS Release check imported a previously generated disposable Qt/C++ daemon transcript, then sent a new request to a deterministic loopback fixture. The captured request contained exactly the earlier Qt user prompt and successful assistant response followed by the new user prompt; its system row was absent. The Rust daemon completed the fixture response, the transcript retained IDs 1–5 and the source copy's SHA-256 was unchanged. This is a local fixture observation, not a live-model or full cross-runtime acceptance result. AC-P8-01 remains open for token-aware context limits, complete trust and omission policy, and explicit rejection/reduction under supported model capabilities. AC-P2-03 remains open for native, authorized real-provider/model parity; a byte cap is not a tokenizer or model-window guarantee.

Focused development-daemon process tests passed for same-session completed-turn reuse after restart, session isolation, exclusion of failed, cancelled and interrupted turns, whole-pair reduction, and rejection of an oversized mandatory request without a persisted row or provider dispatch. Import tests covered nonadjacent reply links, successful versus failed partial replacements, and exclusion of system and unmatched rows. A busy context read was followed by a successful manual retry. These are deterministic scoped tests, not model-window or live-provider acceptance.

### Development context estimate and local provider identities

Development Chat additionally applies an estimated token-capacity policy to the
same admitted messages. The default unknown-capacity ceiling is 8192 tokens; an
explicit positive `--context-window-tokens` value may set another ceiling. The
policy estimates ceil(serialized messages-array UTF-8 bytes / 3), adds a rounded-up
25% uncertainty allowance, 256 framing tokens and the 2048-token output reserve.
The current prompt is mandatory and returns `context-limit-exceeded` before storage
or dispatch if the estimate does not fit; the existing exact 128 KiB body bound
remains separate and takes precedence when both fail. Optional history is admitted
as newest whole pairs under both limits. The estimate does not measure provider
tokenization or prove model context-window support; AC-P8-01 and AC-P2-03 remain open.

A development run may explicitly identify its loopback OpenAI-compatible endpoint
as `openai-compatible-local` (default), `lm-studio` or `llama-cpp-server`. These
identities all use the existing fixed SSE protocol and do not implement provider
discovery, lifecycle ownership, native dialect parity or production-profile access.
Development store v3 adds a provider identity to durable run bindings; the chosen
identity and model are frozen per run and retained for terminal and restart
projection. Existing development v1/v2 stores migrate forward transactionally;
older binaries refuse the new version. No Qt database is migrated by this upgrade.

### Development Ollama chat adapter

An explicitly selected `ollama` development provider uses the same literal-loopback
endpoint policy and frozen model/binding as the other development providers. It sends
structured admitted user/assistant messages to `/api/chat` with `stream:true` and
`options.num_predict=2048`; it does not set `num_ctx`, discover models or retry.
The bounded NDJSON reader requires nonempty visible text and an explicit successful
terminal, rejects truncation, provider errors and unsupported reasoning/tool output,
and checks any reported model against the selected binding with only `:latest`
normalization. The existing byte/token admission and cancellation limits apply.
This experimental path differs from the C++ Ollama adapter's `/api/generate` and
claims neither native model parity nor production-runtime acceptance. AC-P2-03
requires real native-provider tests and separate dialect/continuation evaluation.

#### Isolated native Ollama smoke (macOS, 2026-11-17)

On Apple M2/macOS arm64, an isolated Ollama 0.40.16 server bound to loopback
with cloud disabled and a temporary model store. The Rust Release development
daemon and CLI used a separate temporary development profile. No user Ollama
service, model store or Qt profile was used. The tested daemon SHA-256 was
`62c4eeb2d663434707d9d30d24359c70a192c153c7ea4b8c621951ec4f70be83`;
the CLI SHA-256 was
`1f844584ac525e9edcfabfc37f474909ead98e7e89179aafb52b7ee786b591cf`.

A 19 MB public tiny story GGUF (SHA-256
`6151b1929d7f5aa3385d9ddef3393e55587c0a55de661562322bc51dfda93a04`)
produced 1,279 deltas followed by `run.failed` on the first Rust run. A separate
comparable direct Ollama request ended with `done_reason:length` after 1,280
model tokens; the Rust error did not expose a specific terminal reason, so this
is supporting evidence rather than proof of that exact run's failure cause. With
an explicit `.` stop sequence in a disposable Ollama Modelfile, the same model
completed through Rust with ten deltas and `run.completed`.

The official Ollama `smollm2:135m-instruct-q4_0` model was then downloaded into
the temporary model store (model layer SHA-256
`f68203bfb98b1b1e8c64fac75fab10c4e36acac081609573b4df0fcc19c90dd9`). One short
arithmetic prompt through the Rust CLI produced 76 deltas and `run.completed`;
the persisted run and assistant message both recorded provider `ollama` and the
selected model. The answer contained the correct digit but did not follow the
requested one-digit-only format. These are single native transport smoke results,
not a model-reliability, context-window, load, cancellation, multi-platform or
production-parity acceptance. The Rust endpoint uses `/api/chat`; the production
C++ adapter uses `/api/generate`. No native LM Studio model was available for a
matching live smoke. AC-P2-03 and cross-provider/native platform parity remain open.

### Development IPC and desktop coverage ledger · 2026-11-20

This ledger is a source-level implementation count, not an acceptance score. The
versioned contract currently defines 37 request commands and 11 event names in
`protocol/ipc-v1.json`. The portable Rust `Dispatcher` validates every known
command's envelope and typed payload, but its built-in behavior is only
`hello`, `daemon.status` and `daemon.shutdown` (3/37). It forwards other known
requests to the runtime-owner callback; validation is not implementation of
those runtime features.

The macOS/Linux `sentinel-daemon-dev` runtime implements 30 typed request
variants in its runtime handler, in addition to those 3 dispatcher methods
(33/37 total): `model.list`, `model.current`, `model.select`, `provider.select`,
`session.list`, `session.create`, `session.attach`, `session.messages`,
`chat.send`, `chat.edit`, `chat.regenerate`, `chat.retry`, `run.cancel`,
`agent.start`, `approval.respond`, `agent.history`, `desktop.projection`,
`desktop.action`, `desktop.settings`, `desktop.setting`,
`desktop.settings_service`, `terminal.state`, `terminal.attach`,
`workspace.files`, `workspace.select`, `workspace.root`, `workspace.changes`,
`voice.state`, `model.helper_state`, and `model.helper_action`. This count
describes recognized request variants, not unconditional availability.
Provider/model and Agent methods may reject with `provider-unavailable` or
`unsupported-capability`; `desktop.projection` currently serves only page 0;
`desktop.action` is limited to session-scoped memory and the explicitly named
rename/permanent-delete actions; `desktop.settings_service` implements a subset
of actions; `voice.state` truthfully reports an unavailable speech runtime;
model helper operations exist only for configured Ollama. The remaining four
contract commands are forwarded and rejected by this runtime:
`workspace.create`, `voice.action`, `voice.audio`, and `backup.transfer` (their
names are part of the contract but have no development-daemon implementation).

The IPC schema defines 11 events. The current development daemon emits six
run events: `run.started`, `output.delta`, `run.completed`, `run.failed`,
`run.cancelled`, and `approval.requested`. It does not currently stream
`agent.activity`, `tool.requested`, `tool.running`, `tool.result`, or
`subagent.activity`; persisted Agent audit details are fetched separately by
`agent.history`. The Slint client recognizes all 11 event names, but recognition
does not make the missing live activity streams available.

| Surface | Present in the development Rust/Slint experiment | Primary parity blockers |
| --- | --- | --- |
| Chat | Plain-text streaming, cancellation/reattach, session list/create/attach, bounded transcript, branching edits and historical retry/regeneration, rename/delete, durable history, and bounded workspace UTF-8, FODT, DOCX, text PDF and opt-in PNG/JPEG/static WebP attachments via Slint and CLI | Production runtime ownership, scanned PDF/OCR, broader media and document support, voice, citations and full message presentation |
| Models/providers | Persisted selection and loopback endpoint editing for supported configured local providers, selected-endpoint discovery, opt-in OpenAI, Groq, Mistral, Claude, Gemini and DeepSeek HTTPS for text Chat, OpenAI-compatible SSE and Ollama Chat adapters, optional Ollama pull/status/cancel-local-wait/remove helper | Provider/model discovery and lifecycle parity, remaining provider dialects and continuation behavior, native live-provider parity, and explicit outcome reconciliation after a cancelled Ollama wait |
| Settings | Persisted theme, Chat response instructions, model selection and configured workspace selection; read-only runtime and provider diagnostics | Remaining Qt settings, credentials and helper workflows |
| Memory | Explicit session-only create/read/correct/delete, revision checks and bounded paging; never injected automatically | Production memory domains, search/indexing/retrieval, retention and migration are separate and absent |
| Agent | Typed inspect-file(s), directory, create-file and macOS allowlisted process tasks; per-call approval where required; verified evidence-derived terminal answer; separate audit store and inspector | No generic production AgentRuntime, full ToolExecutionGateway/permission policy, subagent execution; narrow development workspace and provider prerequisites |
| Desktop shell | Chat, Models, Agent, Memory, Workspace, Settings and read-only Extensions; compact Quick Panel; opt-in macOS shortcut and menu-bar actions | Native macOS shortcut/focus/menu verification, Linux and Windows activation and tray, accessibility, and remaining Qt surfaces |
| Platform/runtime | Unix development daemon on macOS/Linux; Windows IPC client and listener source with bounded durable Chat and loopback discovery | No native Windows pipe interoperability/security result; Linux Wayland/X11 and cross-runtime profile-lock/upgrade acceptance remain open |

The development daemon commits Agent admission, initial plan rows and the
user transcript together before it enqueues work. A rejected admission
creates neither a Chat turn nor an Agent run. The development daemon commits
the terminal Agent audit, plan closure,
session state and transcript in one SQLite rollback-mode transaction before
publishing the terminal event. If a commit cannot be confirmed, the daemon
freezes writes and leaves reconciliation to startup. Startup reconciles the
Agent audit, pending approvals, plan steps and Chat projection in a single
rollback-mode transaction before recovering ordinary Chat. Cross-store owner
or confirmed-state mismatches block startup. Agent runs interrupted before
admission retain an audit receipt without a fabricated Chat message. A run
with an authorized write retains an explicit unknown-outcome receipt. The regression fixture
forces the session update to fail after the audit update and expects both
stores to retain their running state until a successful terminal commit.
This code change leaves native crash and power-loss acceptance open.

The Agent source's current completion gate accepts only a bounded, typed task.
Every completed observation must have exactly one affirming claim with the
expected ID, structured evidence must pass `observation::evaluate`, and the
terminal text is generated from observations rather than the model's proposed
answer. File creation performs readback verification. The process task is
available only where the fixed macOS `sandbox-exec` implementation exists,
with a fixed executable allowlist, bounded argv/output/deadline and approval.
These source checks support the restricted development-agent description; they
do not prove equivalence with the production C++ Agent runtime or live-model
reliability.

The Rust daemon requires the exact model ID as an explicit IPC confirmation
before starting Ollama model removal. Slint forwards the typed ID. See the
[Rust migration tracker](../roadmaps/RUST_MIGRATION_TRACKER.md) for the current
implementation and acceptance backlog.

Remaining full-app gates are AC-P2-03 (provider/model parity and real-model
reliability), AC-P5-01–05 (shared endpoint/profile ownership and recovery),
AC-P6-06/07 (native desktop and activation integration), and AC-P8-01–03
(context/policy completeness, production profile compatibility, migration and
rollback). The feature ledger above does not close these criteria or change
ADR-0001's production status.

The Rust CLI and isolated development daemon now have separate local build and
staging helpers described in [Building](../development/BUILDING.md). Their
manifest records the build target, canonical app version, lockfile hash and
binary checksums; it is unsigned and does not cover staged assets. The helpers
do not install or replace production components. Target names are build inputs,
not native-platform certification; the existing AC-P5, AC-P6 and AC-P8 gates
remain open.

### Standalone Rust build and production cutover boundary

The standalone Rust binaries build independently of CMake and Qt:

```sh
RUSTUP_TOOLCHAIN=1.94.0 cargo build --manifest-path cli/Cargo.toml --locked --release -p sentinel-cli -p sentinel-daemon-dev
RUSTUP_TOOLCHAIN=1.100.0 cargo build --manifest-path tools/slint-desktop-prototype/Cargo.toml --locked --release
```

The first command builds the daemon-connected `sentinel` client and the isolated
`sentinel-daemon-dev`; the second builds the Slint evaluation shell. This proves
that these Rust artifacts can be built without the C++/Qt product toolchain. It
does not provide a standalone production runtime: the development daemon uses its own private profile and schema. Unix development-daemon builds require a C compiler for bundled SQLite. The staged launcher starts the Rust daemon and Slint client together with an explicit local provider and the selected isolated Rust profile.

The remaining C++/Qt dependencies for a production cutover are concrete:

| Product surface | Current authority and dependencies | Cutover work still required |
| --- | --- | --- |
| Production daemon/runtime | `apps/sentinel-daemon` composes `sentinel_core`, Qt services and Qt SQL owners; it owns model routing, production provider adapters, AgentRuntime/AgentLoop, ToolExecutionGateway, permissions, filesystem/process services, plugin/MCP paths and production IPC | Port or replace every required runtime owner with equivalent contracts, limits, authority checks, cancellation, audit-before-dispatch and failure semantics; validate the production IPC contract and native process/peer security |
| Production desktop | `apps/sentinel-desktop` is the shipped Qt/QML application with C++ view models, bootstrap, platform integration, Qt file/media flows and existing daemon lifecycle | Complete and accept the replacement UI and platform adapters, then package it with truthful capability gating, accessibility, activation, tray/shortcut, file picker, voice and upgrade behavior |
| Production data | Separate Qt SQL stores own settings, conversations/history, memory, Agent runs and permission grants; Qt migration and platform profile paths remain in the C++ application | Establish one cross-runtime writer/lock, schema and identity compatibility, coordinated snapshot/restore, crash-safe upgrade/downgrade and rollback. The Rust development transcript importer covers only a narrow conversation subset; the dev profile set archive/restore is not a Qt-profile migration |
| Legacy CLI and release | The C++ `sentinel-cli` retains a separate in-process composition path, and CMake/CPack still builds and packages the Qt product | Decide and verify the supported executable set, replace or retire the legacy path, then prove native installers, upgrade/uninstall and rollback without competing daemon/profile ownership |

The portable Rust IPC dispatcher and dev daemon do not close these dependencies.
The dev daemon's durable Chat, typed Agent tasks, memory, workspace and
development-only backup/archive operations address a separate profile; Windows
still lacks production profile compatibility, and cross-runtime Qt/Rust locking is
not established. Profile archive export plus restore has only compile evidence
in this slice, not a runtime restore receipt. These boundaries keep the
standalone build useful for evaluation while AC-P5-01–05, AC-P6-06/07 and
AC-P8-02/03 remain production cutover gates.

The standalone launcher and Unix daemon also support an opt-in persistent
`$HOME/.sentinel-rust` profile. The same namespace accepts a verified
development archive or the acknowledged Qt preview subset into a new
directory, using staged publication without replacement. Eligible inline
conversation attachments and explicitly bound local model selection are
included in that subset. The Qt profile
remains independently owned. A manual Rust candidate workflow builds and
verifies native Linux x86_64 and macOS arm64 packages without publishing them
as production releases.

### Next implementation sequence

1. Close the desktop gaps under AC-P6-06/07. Exercise the packaged macOS Quick Panel through hide, reopen, focus and quit, then implement and validate Linux and Windows activation and tray behavior.
2. Extend the remaining daemon operations without conflating dispatch coverage and runtime support. Prioritize coordinated backup/restore and Windows durable Chat, then workspace creation and voice capture/transcription with their existing authority boundaries.
3. Gate production promotion on profile transfer and rollback under AC-P8-03, followed by native packages and authenticated updates. Retain the macOS local stage as development evidence.
