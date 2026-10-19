# Local IPC v1

Sentinel's headless daemon composes the existing C++ services with `ApplicationControllerBuilder`. Rust clients never link or reimplement the runtime. Production Qt Desktop uses `DaemonClient` and `DesktopRuntimeClient` for Chat, Agent, model selection and session state. Its bootstrap does not construct an ApplicationController or a local execution fallback. Desktop may launch a detached daemon when needed (unless safe mode or `--no-daemon-start` disables it); the daemon still owns runtime execution. Rust CLI/TUI never auto-spawn it. The legacy C++ CLI uses a separate in-process path outside this contract.

## Canonical contract

[`protocol/ipc-v1.json`](../../protocol/ipc-v1.json) is the canonical versioned field/enum definition. Run `python3 tools/ipc/generate.py` after changes; `--check` fails on drift. It generates the daemon C++ request validator/command enum, the shared Qt client command/response/event field contracts, and Rust request, response and event payload enums. Generation is checked by CTest and Rust CI, and does not add a generator dependency to ordinary CMake builds. The Cargo package version is derived from `SENTINEL_APP_VERSION`; protocol version is independent: **1.1**.

Transport is local Unix-domain sockets on macOS/Linux through Qt `QLocalServer` and Rust `UnixStream`. Windows Rust transport currently reports unsupported/unavailable; no Windows runtime certification is claimed.

Each frame is one compact UTF-8 JSON object followed by LF:

```json
{"version":{"major":1,"minor":0},"type":"request","id":"request-1","name":"hello","payload":{"client_id":"example","major":1,"minor":0,"capabilities":[]}}
```

The daemon replies with `type: response`, the same `id` and `name`, and a typed payload. Errors have `type: error`, the correlated identifier when available, and `payload.code`. Events have `type: event`, `name`, and `payload.session_id`/`run_id`. Uncorrelatable malformed frames return an empty identifier. Unknown commands, enum variants, missing fields and wrong field types fail explicitly. Major version mismatch is rejected. Future minor versions may add optional fields/events; clients tolerate unknown future-minor events without treating them as success.

## Commands

| Command | Required payload | Result |
|---|---|---|
| hello | client_id, major, minor, capabilities | version, daemon_version, capabilities |
| daemon.status | none | running, uptime_ms, active_runs, sessions, daemon_version |
| daemon.shutdown | none | shutting_down |
| model.list / model.current | none | discovered models / selected provider and model |
| session.list | none | conversation-backed sessions |
| session.create | title | newly persisted conversation/session |
| session.attach | session_id | snapshot and future event subscription |
| chat.send / agent.start | session_id, text | run_id and session_id |
| run.cancel | run_id | cancellation acknowledgement |
| approval.respond | run_id, approval_id, allow | acknowledgement |

Consult the canonical file for exact response and event fields. Capabilities advertise the implemented surface. `model.select` validates provider/model identities against daemon ModelService. Selector changes affect future work; active binding is captured once and core `ModelBinding` remains authoritative.

Events are `run.started`, `output.delta`, `tool.requested`, `approval.requested`, `tool.result`, `run.completed`, `run.failed`, and `run.cancelled`. They map existing Chat/Agent events. Tool success does not produce Agent completion; only core accepted final-answer completion does. Final events include canonical terminal text/state. Clients replace incremental output with terminal text, avoiding duplicate finals.

`hello` may also return optional `server_generation`, a UUID assigned each time the daemon successfully claims/listens on its endpoint. This remains compatible with the original 1.0 surface: old clients ignore this additive field and new Qt/Rust clients accept older responses without it. The Qt client separately tracks a local connection epoch, generates fresh UUID request IDs, fails pending requests on disconnect and never automatically replays mutations. An epoch change alone does not imply a daemon restart. Major mismatch stops automatic reconnect; explicit disconnect/connect retries after repair. Unknown events are ignored only for a future compatible minor version; malformed known payloads disconnect the client.

## Sessions, approvals and lifecycle

Session IDs are existing conversation IDs. Conversation and transcript storage remain authoritative. A separate owner-only Qt SQL `ipc-sessions.sqlite3` stores the latest acknowledged IPC run identity, kind, frozen binding and terminal state per conversation; it stores neither transcripts nor permission grants. This foundation exposes one foreground active run, reflecting the existing controller, with multiple connected clients. A connection subscribes by starting a run or attaching a session. Disconnect does **not** cancel work. Reconnect and attach return a current snapshot, bounded output, frozen binding and pending approval, then future events. There is no new event log or historical replay service. After daemon restart the existing stores recover persisted records; active runs are never replayed or resumed. Persisted interrupted run projections become failed with `daemon-restarted`; pending grants are invalidated. A completed/cancelled/failed IPC run identity and binding remain available across daemon restarts. Historical model binding is empty when the existing persisted record does not contain it.

Approval events include a one-use approval ID, run ID, tool, risk, resources and runtime detail. Clients submit an explicit allow/deny decision. The daemon validates active state and IDs before calling the existing approval boundary. Replayed IDs, unknown sessions/runs and approval after cancellation fail. IPC clients cannot grant permanent permissions or bypass the tool gateway.

## Security and limits

Default endpoint is `~/.sentinel/run/daemon.sock`. The runtime directory is owner-only (0700), its owner and non-symlink status are checked, and the socket is restricted to its owner. Custom endpoints require an existing private owner-owned directory. macOS `getpeereid` / Linux `SO_PEERCRED` enforce same UID. No TCP listener is opened.

A `QLockFile` claims the endpoint before stores open. Existing regular files and foreign/live sockets are never blindly removed. Same-owner socket artifacts are probed and recovered only after refused/not-found connection; crash-stale locks use Qt's process identity checks. Normal shutdown removes the owned endpoint and lock.

Frames are limited to 262144 bytes; request text/string fields to 65536 characters; identifiers to 128 characters; connected clients to 32. Server pending output is limited to 1048576 bytes per client. A slow/oversized-output client's connection is closed without cancelling or changing the core run state. Rust event buffering is bounded (256 pending events, 256 TUI channel entries). Attach output retains at most 65536 UTF-16 characters with `output_truncated`; full terminal output is subject to frame limits. Large results can therefore disconnect a client truthfully rather than allocate unbounded queues.

Logs contain lifecycle and error codes, not credentials or full tool/request payloads. Same-user clients are trusted to act as the user's UI, including shutdown/cancel decisions; this is not a remote security boundary.

## Additive Desktop surface

The original additive Desktop surface used protocol 1.0; the terminal extensions use 1.1. Canonical optional session snapshots declare generation, sequence,
run kind, binding, approval context and conversation metadata. `agent.activity` projects
existing runtime activity without a client-side Agent state machine. Every run event
carries daemon generation and monotonic sequence; clients discard foreign session/run,
old generation, duplicate sequence and closed-run events.

`session.messages` returns canonical persisted/live message rows; `chat.retry`,
`chat.regenerate` and `chat.edit` enter the same core Chat path. `desktop.projection`
returns paged, QML-safe primary runtime properties. Non-primary diagnostic fields in
its allowlist are reserved and are not advertised as available runtime features.
`desktop.action` uses a generated finite action/argument/scope allowlist; it never
invokes arbitrary QObject methods. Runtime-busy mutation restrictions preserve frozen
workspace and binding. Generate this allowlist with `tools/ipc/generate_desktop.py`.

`desktop.settings`, `desktop.setting` and `desktop.settings_service` forward runtime
configuration, product settings and maintenance to the daemon. Visual preferences
persist separately in Desktop's `.desktop` store. Secret setters operate only through
the daemon credential boundary; secrets are excluded from Desktop settings projection.
Settings mutation submission returns Pending and a request UUID until daemon reply;
submission is not represented as completed success. `model.helper_state/action`
projects daemon-owned model acquisition helpers without Desktop network clients.

The current v1 bounded-history response can reject oversized transcripts rather than
truncate them silently. Historical event replay and transcript pagination are outside
this version. Unsupported legacy service-pointer operations return unavailable; they
never fall back to local execution. The local-controller Shell constructor is retained
for existing unit tests only.

## Terminal surface (1.1)

`terminal.state` exposes safe cached provider/model, workspace and service diagnostics.
`terminal.attach` returns a bounded session snapshot without the full Desktop projection.
Provider and workspace selection/configuration remain daemon-owned. `workspace.files`
uses authorized registered read-only tools; `workspace.changes` returns bounded Applied
diffs from an in-memory Agent baseline, without rollback or durable Git attribution.
`tool.running`, tool timing metadata, approval decisions and subagent state project
existing runtime events. See the [CLI](../user/CLI.md) and [TUI](../user/TUI.md) guides.

## Quick Panel dictation and readiness (2026-10-08)

`voice.state` projects runtime readiness, state, owner-only completed transcript, failure, input-device identity and VAD setting. `voice.action` supports start/cancel. Platform microphone permission and capture stay in the GUI using the existing AudioDeviceService; the daemon reserves ownership and accepts normalized mono 16 kHz signed little-endian 16-bit PCM through `voice.audio {pcm, final, speech}`. The GUI sends acknowledged 49,152-byte chunks (at most 65,536 base64 characters), stopping at 59 seconds; daemon total bound is 1,920,000 bytes. Disconnect and cancellation release capture/STT; changed STT revision rejects the lease. Dictation is reviewed in the composer, never automatically executed. Unconfigured STT remains unavailable.

Desktop Connected is transport status only. Ready requires complete required eager/global projection, settings, session list and successful attachment. Optional/session-only fields are not eagerly enumerated. Disconnect clears stale projections and reconnect follows the same path. Model-library presentation uses ModelService's credential-free cached status; actual provider binding still authorizes configuration normally.

## Rust request deadlines

The shared Rust CLI/TUI client applies one absolute deadline to a command's write,
interleaved events, partial frames and acknowledgement. It uses the configured
read timeout, or ten seconds when event polling is unbounded. Event polling keeps
its own existing timeout and partial-frame recovery behavior. Successful requests
restore the polling/write timeouts. A command timeout closes the connection so a
late reply cannot be consumed by the next command; it does not cancel the daemon
run or replay the command. Reconnect and attach to inspect authoritative state.

## Terminal client recovery and event identity

Rust command transport failures (including invalid protocol/version and
`daemon-shutting-down`) invalidate the TUI connection. Provider/permission/business
errors do not request a transport reconnect. Idle polling timeouts remain normal.
Both TUI reconnect paths must obtain a matching `terminal.attach` snapshot before
sending session-dependent commands. Failed restoration closes that socket and
fails the queued command once; it never replays a sent command. Explicit new/session
selection can replace an unavailable old session. Snapshot state remains daemon
owned; disconnect/shutdown rejection is not a fabricated run completion.

The noninteractive Rust CLI validates the acknowledged session/run identity and
filters queued/streamed events by both before displaying output, answering approvals
or selecting a terminal exit code. Previous-run and other-session events cannot
finish the current command. In the TUI, the first terminal outcome closes the run;
duplicate terminals and late approvals/activity cannot overwrite it. Only an
authoritative new-run snapshot reopens execution. The protocol schema and Core
permission decisions are unchanged. P3 development remains based on the unmerged PR #17 branch.

## Scoped memory management

P8-03 adds the canonical Desktop action `manageMemory(const QVariantMap &request)`
returning a `QVariantMap`; it uses the existing `desktop.action` session ownership
and busy-run gate. The generated bridge/daemon descriptors are derived from
`protocol/ipc-v1.json` and `protocol/DesktopBridgeMethods.json`.

`operation` is `list`, `read`, `create`, `correct`, `delete`, `deleteSelected`,
`clear`, `move`, `export` or `import`. Scope numbers are Legacy=0, Session=1,
Project=2, Personal=3. `personal` and `legacy` are explicit management opt-ins only.
Core resolves identity; mutation `session`/`workspace` fields must match the current
context and never grant authority. Unknown fields/types fail validation.

Listing uses `search` (256 characters max), `cursor` (UUID keyset), ten records/page
and returns metadata/value previews plus `nextCursor`, actual `session`/`workspace`.
Exact `read` returns `record` with a 4096-character value chunk from `offset`,
`valueLength` and revision; Desktop checks revisions across chunks. Create/correct
use `key`/`value`; corrections/deletions/moves use `id` and `revision`. Selected
batch deletion uses `selected:[{id,revision}]`. Deletion/clear requires
`confirmed:true, confirmation:"DELETE"`; move additionally uses `destination` and
`confirmation:"MOVE"`. Import requires `confirmed:true`, `archive` and explicit
`conflict:"reject"` or `"skipIdentical"`. Export/import use the scope-only memory
archive documented in [Memory](../concepts/MEMORY.md).

Replies echo `requestTag` and report `Ready`, `Unavailable`, `Unauthorized`,
`InvalidInput`, `Conflict`, `NotFound` or `LimitExceeded`. Empty authorized lists
are `Ready` with no records; foreign record IDs are hidden as `NotFound`.
Transport failures produce an unavailable completion; the UI ignores stale tags
and clears its state when conversation/workspace changes. Memory replies refresh
projections without reattaching or redirecting the current session. Archive and
value bounds preserve the existing IPC frame limit; no streamed model/provider
request is introduced.

### P8-04 retrieval and index operations

The existing typed `manageMemory` action additionally accepts `operation:"search"`,
`"indexStatus"` and `"refreshIndex"`; no new wire command or method signature was
introduced. Existing canonical IPC/bridge generators remain authoritative and pass
`--check`. These operations keep the same active session/workspace and explicit
Personal management rules; Legacy ranked recall/index operations are refused.

`search` accepts `scope` and `search` (at most 256 characters), returning up to ten
ranked record previews. Each result's `relevance` contains integer `lexicalScore`,
`exactKey`, `phrase`, `matchedTerms`, `totalTerms` and null `semanticSimilarity`.
The reply also contains `candidatesExamined` (at most 200), `candidateLimitReached`,
`index`, `semantic:"Unavailable"` and actual session/workspace identity. Scores only
explain authorized lexical candidates; serialized fields do not grant access.
There is no ranked cursor: the page shows the top bounded recall preview; management
inventory keeps the existing UUID keyset pagination.

`indexStatus` returns the authorized index state without content/counts from foreign
scopes. Ordinary scoped inventory also returns the selected scope's index state.
`refreshIndex` requires current mutation `session`/`workspace`, `confirmed:true` and
`confirmation:"REFRESH"`, and returns `index` and `refreshed`. One batch repairs at
most 100 source and 100 orphan projection rows in a transaction. Stale means more
work remains or hooks are inconsistent; unavailable means unsupported/unusable index
or storage. No background refresh or job polling is implied. Existing Cancel-first
DELETE/MOVE confirmations are preserved; index refresh also defaults to Cancel.
