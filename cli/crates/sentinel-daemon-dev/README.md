# Rust development daemon

This isolated Linux/macOS experiment runs one Chat at a time through an
explicit local OpenAI-compatible or Ollama text streaming endpoint. An opt-in
read-only Agent inspect-file experiment is described below. It has no credentials,
production stores, or profile migration. Sessions and committed
messages are stored in the dedicated `daemon-dev-chat.sqlite3` development file.
It never reads or writes a Qt profile; its development lock is not compatible with
Qt's runtime lock.

A separate Windows development listener is cross-compiled but has not been
validated on native Windows. It accepts only `--profile PRIVATE_TEMP_DIRECTORY`:
the directory must already be a direct child of the OS temporary directory,
owned by the current user with a protected owner-only DACL, and contain only the
daemon lock if present. It prints its private named-pipe endpoint after creation;
pass that endpoint explicitly to the CLI `--socket` option. It accepts one
same-user client at a time, with a three-second I/O deadline and a 15-second
client lifetime. Only hello, status and shutdown are supported; Chat and storage
are unavailable. Its lock and endpoint are separate from Qt. Native pipe,
security, cancellation and interoperability testing remain required before use
as a supported Windows runtime.

From the `cli` directory, create an empty private directory directly under `/tmp`.
Start the daemon with a local provider and model, then use another terminal:

```sh
mkdir -m 700 /tmp/sentinel-rust-dev
cargo run -p sentinel-daemon-dev -- --profile /tmp/sentinel-rust-dev --provider http://127.0.0.1:1234 --model YOUR_MODEL
cargo run -p sentinel-cli -- --socket /tmp/sentinel-rust-dev/daemon-dev.sock chat 'Hello'
cargo run -p sentinel-cli -- --socket /tmp/sentinel-rust-dev/daemon-dev.sock shutdown
```

To enable the limited Agent experiment, add `--agent-workspace /absolute/path`
after the provider and model arguments. This explicitly authorizes reads under
that directory for this development daemon. It is named `default`. Add up to
15 more explicit roots with repeated `--workspace ID /absolute/path` options;
IDs use ASCII letters, digits, `.`, `_` or `-`. The existing `workspace.select`
command selects only these roots while no run is active, and commits the selected
ID and root to development settings. On restart the selection is restored only
when both ID and root still match the configured catalog; otherwise `default` is
selected. Changing roots clears in-memory change baselines. `workspace.root`
only confirms an exact configured root and cannot change or grant a path.
Tasks must be explicit JSON:
`{"operation":"inspect-file","path":"notes.txt"}`,
`{"operation":"inspect-files","paths":["one.txt","two.txt"]}` (at most
four unique paths), or `{"operation":"inspect-directory","path":".","includeHidden":false}`.
The separate `{"operation":"classify-inspect","goal":"..."}` task asks the
frozen provider to propose a read-only file inspection plan from natural language.
Only one to four explicit relative file reads are accepted; unsupported, empty or
ambiguous classifications fail. Each proposed file requires explicit approval even
under workspace preauthorization. A completed run reports the approved inspection
results and states that the original goal was not assessed; classification does
not establish that the plan covers the goal. The run and per-file intentions and
approval receipts remain in the development audit.

By default, the workspace flag preauthorizes these scoped reads. Add
`--agent-approval required` to require a separate per-tool approval instead;
`--agent-approval workspace` explicitly selects the default. Required approvals
are recorded before the request event, apply only to the matching run and
approval ID, and expire after five minutes. Denial, cancellation, expiry, a
failed acknowledgement or restart prevents the pending read. Approval receipts
are stored in the development Agent audit; a running client connection has a
30-minute lifetime and can reattach to a pending approval.

When workspace read preauthorization is selected, `workspace.files` lists up to
100 regular files beneath the held workspace root for an existing session. It
excludes hidden files and does not follow symbolic links. Directory, depth and
response-size limits and skipped entries are reported with `truncated: true`.
With per-tool approval required, this request returns `permission-denied`; it
does not silently bypass the approval policy. The listing is a bounded snapshot,
not a change baseline. `workspace.changes` separately compares a bounded
in-memory baseline captured before an accepted Agent run with a later observation.
Baselines are session-scoped, do not survive restart, and are unavailable if
capture or authorization fails. The comparison reports observed created or
modified text files; it never infers deletion. It can omit hidden, unreadable,
binary, oversized or otherwise unobserved files, reports truncation, and may
include concurrent external edits. It is not a rollback or Git snapshot.

File paths must be relative; symlinks and hard links are refused. Directory
results cover immediate entries only and say whether hidden entries were included.
The Agent can only read the requested files or list the requested directory; each
observation must be complete and each evidence ID affirmed before it can finish.
The displayed answer is derived from observed content. Other tasks, tools,
partial observations and unverified finals fail. The scoped planner accepts
bounded sequential tool batches: every invocation is checked against the task
scope before dispatch, dependencies must succeed, and each action retains its
own audit and approval. A denied action blocks its dependents; unrelated actions
may continue. Repeated executed or denied actions are not dispatched again.
Blocked requests and rejected finals receive bounded recovery feedback. This is
not full production AgentLoop or parallel scheduler parity. The separate
`daemon-dev-agent.sqlite3` audit records run state, tool intent and observation.
Version 4 stores normalized invocation arguments and descriptor version before
dispatch, separately from the resulting resource and output; older records have
unknown invocation fields. Session-scoped Agent history exposes bounded previews.
Interrupted or unconfirmed cross-store transitions recover as failed without
replaying reads. This is a narrow development path, not full Agent parity or a
replacement for production approval and sandbox policy. The additional
`{"operation":"create-file","path":"new.txt","content":"..."}` task
creates only a new file and always requires per-call approval, even when reads
are workspace-preauthorized. It never overwrites an existing path; a separate
readback must match before completion. If publication or later verification is
uncertain, inspect the workspace before retrying. Workspace ownership assumes
cooperating same-user processes; it does not prevent another same-user process
from racing directory or temporary-file changes.
The `agent-glob` task accepts `{ "operation":"glob", "path":".", "pattern":"*.rs" }` and always requires per-call approval. It searches regular files recursively in the authorized workspace, excludes hidden entries and symlinks, and supports case-sensitive ASCII basename patterns with `*` and `?` only. Coverage is bounded; incomplete results cannot satisfy the final evidence gate. This is a development subset of the production glob tool.
The `agent-edit-file` task accepts `{ "operation":"edit-file", "path":"relative/file", "oldString":"...", "newString":"..." }`. It requires a nonempty exact whole-file preimage and explicit approval for each write, then verifies the replacement with a separate read. Each string is limited to 2,048 UTF-8 bytes and the normalized invocation to 3,072 serialized bytes. A conflicting preimage is refused; an uncertain outcome requires inspection before retry. Atomic replacement relies on the same cooperative workspace assumption and is not a compare-and-swap against an uncooperative concurrent writer.

Chat regeneration accepts a user message ID; retry accepts a failed, interrupted,
or error assistant message. For the latest user, these append a linked assistant
response to the existing session. An edit, or regeneration/retry of an earlier
turn, atomically creates a new session containing the transcript through the
target user and starts the response there; the source session is unchanged. The
acknowledgement and run events carry the branch session ID. Branching is subject
to the development transcript and session limits; it does not migrate Qt profiles.

The development daemon also exposes the existing desktop settings IPC for
`themeName` (`system`, `light`, or `dark`) and explicit session-scoped memory
management through `desktop.action`. Memory is stored in a separate private
SQLite file, is not automatically included in model context, and deletion
requires explicit confirmation. Workspace/global memory and semantic recall are
not implemented.

On macOS only, `--agent-process` additionally enables the experimental typed
`{"operation":"run-process","argv":["/usr/bin/wc","-c","notes.txt"]}`
task. It accepts only `/usr/bin/wc`, `head`, `tail`, or `grep`, requires a
separate approval for the exact argv, and uses Seatbelt with no shell, network,
workspace writes, or child processes. The command has a ten-second deadline and
bounded output. Its final describes the observed exit and output, not an inferred
task outcome. The backend fails closed if the sandbox or allowed executable is
unavailable; Linux and Windows process execution are not implemented here.

The existing model-selection IPC lists models on demand from the configured
loopback provider (`/v1/models`, or `/api/tags` for Ollama). A selection must
be in the last successfully discovered catalog, and changes are refused while a
run is active. Only the configured provider can be selected. A committed selection is stored
in a separate development settings SQLite file and restored on restart only for
the same provider kind and loopback origin. Discovery does not load a model.

The provider accepts only a literal loopback HTTP origin with explicit port. The
OpenAI-compatible kinds post text to `/v1/chat/completions` with streaming enabled; there is no automatic
provider selection, authentication or retry. `--provider-kind` optionally records
`openai-compatible-local` (the default), `lm-studio`, `llama-cpp-server`, or
`ollama` as the explicit run identity. The first three use the same local
OpenAI-compatible protocol;
this does not implement discovery, lifecycle management or native backend parity.
The explicit `ollama` kind posts structured roles to `/api/chat`, accepts bounded
NDJSON text, and sets `options.num_predict` to the 2048-token output reserve without
setting `num_ctx`. It requires a complete `done:true` / `done_reason:stop` terminal;
truncation, errors, reasoning and tool output are refused. If a response reports a
model name, it must match the selected name (with only `:latest` normalization).
There is no model discovery, implicit fallback or retry. This `/api/chat` path is
not protocol parity with the production C++ Ollama adapter, which uses `/api/generate`.
`--provider-kind` and `--context-window-tokens` may follow `--model` in either order;
unknown and duplicate flags are refused before the profile is opened. Without both provider arguments,
status, sessions and shutdown remain available, while Chat reports unavailable.
A disconnected client does not cancel a run; another can attach using the session
ID. Cancellation uses `run.cancel`, including the existing CLI's Ctrl-C control
connection. Failed or cancelled runs retain any received partial output in the
attach snapshot; their terminal event reports failure/cancellation, not success.
Only a valid complete provider stream and a committed terminal record can finish
successfully. `session.messages` returns committed user/assistant rows in order;
streaming partial output is visible through events and attach. For each request, the daemon includes eligible earlier complete turns from the same
session. It first admits the current prompt, then selects the newest whole eligible
user/assistant pairs until the next pair would exceed either the 128 KiB serialized
JSON request-body limit or the estimated context-window limit; selected pairs are sent in chronological user order with the
current prompt last. The limit includes model and request framing. If the current
prompt alone exceeds the byte limit, the daemon returns `message-too-large` before
persistence or provider dispatch. If it fits the bytes but exceeds the estimated
context limit, it returns `context-limit-exceeded` at the same boundary. If both
would fail, the byte error takes precedence.

Native history contributes only complete successful pairs from the same durable run.
For imported Qt transcripts, a nonpartial completed or received assistant message
must explicitly reference an earlier user message in the same imported conversation;
the latest valid answer is selected. Eligible users are nonempty, nonpartial and
`sent`, `received` or `completed`; eligible assistants are nonempty, nonpartial and
`received` or `completed`. Failed or partial replacements do not suppress
an earlier successful answer. System rows and unattached or unsuccessful assistant
messages are not sent as provider instructions or complete turns. Imported content
remains historical data; its recorded role does not grant authority. The context
window defaults to an unknown-capacity policy ceiling of 8192 tokens; specify a
positive `--context-window-tokens N` to set another ceiling. The value must fit the
shortest valid prompt plus the policy reserves or startup is refused. The estimate
uses the escaped serialized messages-array UTF-8 byte count: ceil(bytes/3), plus
ceil(25% of that base), 256 framing tokens and the existing 2048-token output reserve.
This estimate is not a tokenizer, a measured provider capacity or a guarantee that
the actual request fits a model window.

At most eight local same-user clients and one run are active, with 16 pending
frames per client, 64 sessions, 16 turns per session, 64 KiB UTF-8 input and
32 KiB output per run, a
3-second frame/write bound, an 1800-second client lifetime, and provider deadlines
of 180 seconds total and 60 seconds idle. An overflowing or slow peer disconnects
without cancelling the run. A daemon shutdown requests worker cancellation. The
user message and running record are committed before dispatch; output and terminal
state commit together before the terminal event. A storage failure blocks further
mutations until restart, except a busy/locked refusal before transaction entry,
which can be retried manually; a terminal commit failure emits failed, never completed.
On restart an unfinished run becomes failed with `daemon-restarted` and is not
replayed; uncommitted streaming output is lost. A transcript that exceeds the IPC
frame limit returns `message-too-large` rather than silently truncating history.

Orderly shutdown removes its own socket. After a crash it refuses a leftover
socket; inspect it and remove it manually before restarting. It never removes
an endpoint found at startup. The directory, database and lock file remain for reuse.
An interrupted first database initialization can leave an empty or invalid database;
startup refuses it. Inspect and remove it only if the disposable profile is known
to contain no data to retain. Unknown database application IDs or versions are
refused. SQLite is compiled from the rusqlite bundled source and does not depend
on the host SQLite library.

## Import a disposable Qt transcript copy

The development-only importer accepts a finalized, quiescent **copy** of the Qt
conversation database at schema version 6. Prepare it from a stopped disposable
Qt profile or a consistent SQLite backup, normalized to rollback/DELETE journal
mode; Qt normally uses WAL, so copying its main database alone can omit committed
content. Only on the disposable backup, use SQLite to set `PRAGMA journal_mode=DELETE`,
close that SQLite connection and confirm its sidecars are gone. The importer neither
checkpoints nor changes its source. All source connections must remain closed during import; the importer holds a read transaction
but cannot prove that another same-user process will not modify the copy.

The source must be a regular owner-only file (0400 or 0600, one hard link) in a
separate owner-only 0700 directory directly under `/tmp`. It must have no `-wal`,
`-shm`, or `-journal` sidecars. The destination must be an already initialized,
stopped and empty development profile in another private `/tmp` directory. Initialize
it by starting this daemon once without a provider and shutting it down, then run:

```sh
cargo run -p sentinel-daemon-dev -- --profile /tmp/sentinel-rust-dev --import-qt-copy /tmp/qt-copy-dir/conversations-copy.sqlite3
```

Import validates the entire source and inserts all accepted rows in one transaction.
It preserves session UUIDs, titles, creation/update timestamps, per-session message
IDs and order, system/user/assistant content, message timestamps/status, partial
flags, provider/model and valid earlier reply/replacement links. It rejects unknown
schema, malformed or oversized rows, live message states, nondefault conversation
flags, summaries, attachments and nondefault error categories. Each imported session
is limited to 32 messages and must fit the unpaged `session.messages` IPC response.
A nonempty destination, rerun, unknown schema or unsafe source fails without a
partial import. Existing native history can later exceed the IPC limit and then
`session.messages` explicitly returns `message-too-large`.

This is a transcript import only: `session.attach` initially reports idle with empty
output and no imported run identity; `session.messages` is the authoritative
imported transcript. It does not copy Qt's separate run projections, chat history
index, settings, credentials, memory, permissions or Agent records. Subsequent
native messages use IDs above the imported maximum. Development schemas version 1 and 2 are upgraded transactionally to version 3
when the daemon starts; version 3 records the selected provider identity per run; back up any data
needed before upgrading. Older development binaries refuse version 3, and no
production Qt-profile compatibility, downgrade or migration is claimed.

## Session management and memory copy import

The development daemon supports renaming and confirmed permanent deletion of a
selected session. Deletion commits removal from the Chat, Agent audit and memory
SQLite stores together under the profile lock. It is logical SQLite deletion;
it is not a secure-erase guarantee. The client must obtain explicit confirmation
before sending the permanent-delete action. A failed or uncertain storage commit
blocks further session operations until restart. SQLite super-journals are retained
when needed for recovery; do not manually remove them from a profile with data.

A finalized, quiescent **copy** of a Qt memory v7 database may be imported into
an already initialized, stopped development profile with an empty memory store:

```sh
cargo run -p sentinel-daemon-dev -- --profile /tmp/sentinel-rust-dev --import-qt-memory-copy /tmp/qt-memory-copy/memory.sqlite3
```

The source must be a separate private `/tmp` copy in rollback/DELETE mode with
no SQLite sidecars. Only supported session-scoped rows whose owner sessions
already exist in the destination are accepted; unsupported scopes or metadata
cause the whole import to fail. IDs, revisions, provenance and correction receipts
are retained. Imported corrections cannot yet be edited, and memory is not
injected into model context. This does not migrate or open a live Qt profile or
establish an atomic import across the separate stores.

The development daemon advertises `dev-backup-export` for the existing
`backup.transfer` IPC command. An explicit `export` selects one of `chat`,
`settings`, `memory`, or `agent`; `read` retrieves its peer-owned bytes in
48 KiB base64 chunks, and `cancel` releases them. Each consistent, single-store
SQLite snapshot is limited to 16 MiB; at most four peer transfers are retained.
A transfer expires after ten minutes of inactivity and is removed on the next
backup request, peer disconnect, or daemon shutdown. Snapshot generation uses
the already-open store and does not write a backup file. Its
`sentinel-dev-sqlite-v1` format is specific to this isolated daemon, is not a Qt
portable backup, and has no restore operation here. It is not encrypted and may
contain private conversation, settings, memory, or audit data; recipients must
protect any bytes they save. Separate exports are not a cross-store transaction.

An offline, settings-only development restore is available as
`sentinel-daemon-dev --profile /tmp/PRIVATE_PROFILE --restore-dev-settings-copy /tmp/PRIVATE_SOURCE/settings.sqlite3`.
Stop the daemon first. Both directories must be separate owner-only `/tmp`
directories, the profile must already have all current-version dev stores and
contain no chat, agent, memory, or settings records, and the source must be a
finalized private copy of a `sentinel-dev-sqlite-v1` settings export. The
operation validates the SQLite version, structure, and supported values before
copying settings in one transaction. Stored workspace and provider selections
still require matching explicit configuration on startup. Chat, agent and memory
restores are not supported by this command; their cross-store references require
a coordinated restore design. This is not a Qt profile or portable backup restore.

The `agent-read-plan` capability accepts a user-authored `read-plan` with one to
four existing read-only typed tasks, each paired with `dependsOn` containing
zero-based indexes of earlier steps. For example:

```json
{"operation":"read-plan","steps":[{"task":{"operation":"inspect-file","path":"notes.txt"},"dependsOn":[]},{"task":{"operation":"glob","path":".","pattern":"*.rs"},"dependsOn":[0]}]}
```

Each task retains its existing authorization and evidence gate. A failed step
blocks its dependents; independent steps may continue, but the overall plan
succeeds only when every step completes with verified evidence. Writes, process
execution, and free-form goals are not accepted in this plan mode. It is a
development subset, not general Agent parity.

When an explicit workspace is configured, `dev-workspace-revocation` exposes
`devWorkspaceAccess` as `true` or `false` through the desktop settings IPC. A
disable persists the revocation, cancels the active run, denies pending approval,
and invalidates workspace change baselines; later workspace operations are denied
until an explicit idle re-enable. Revocation is not a quiescence barrier: I/O
already past its final cancellation check may finish, and a write can have an
uncertain outcome. This development control is separate from Qt's privacy policy.
Agent activity and tool lifecycle IPC events contain generic descriptions, not
file contents or tool arguments; session attach and Agent history remain the
reconnection sources of record.

The `agent-stat-path` task accepts `{ "operation":"stat-path", "path":"." }`
for an existing workspace-relative file or directory. It requires approval and
returns bounded file type and size metadata; missing paths and unsafe file types
are errors, not evidence of absence. Read plans also accept this task. A read
plan has a 300-second cooperative deadline from worker start; bounded in-flight
filesystem operations may finish after cancellation. Session-authorized Agent
history records generic succeeded, failed, and dependency-blocked step statuses,
while the run remains failed if any step is incomplete.

`agent-work-plan` is a separate explicit plan mode with the same four-step and
dependency limits. It additionally accepts the scoped `create-file` and
`edit-file` tasks described above; each mutation requires its own approval and
readback before dependent steps run. A write with an uncertain outcome stops the
plan and must be inspected before retry. Processes and general free-form tasks
remain outside this plan mode. The plan deadline and cooperative workspace race
limitations still apply.

Agent store schema v7 records each explicit plan's bounded operation names and
dependency indexes with pending steps before dispatch. A step is marked started
before execution. On normal termination or restart, unfinished started steps
are recorded as interrupted and pending steps as skipped; completed, failed,
and dependency-blocked outcomes are retained. The session-scoped Agent history
exposes these records. Interrupted work is never automatically replayed; an
approved file mutation may have an uncertain outcome and requires inspection.
Legacy plan rows retain unknown metadata rather than inferred values.

`dev-backup-set` exports a coordinated, idle development profile snapshot via
`backup.transfer` using domains `["chat","settings","memory","agent"]` in
that order. The bounded `sentinel-dev-sqlite-set-v1` archive contains four raw
SQLite snapshots and per-store digests. The daemon state lock excludes its own
writes during capture; unrelated same-user writers are not coordinated. Treat
the unencrypted archive as private: digests detect corruption, not forgery.

To restore, stop the daemon and run
`sentinel-daemon-dev --profile /tmp/NEW_PROFILE --restore-dev-set /tmp/PRIVATE_SOURCE/SET`.
The source must be a private regular file in an owner-only direct `/tmp` child,
and the destination must not exist. Restore validates the current exact schema,
bounded supported row values, and cross-store ownership before publishing a
locked private stage with an atomic no-replace rename. A directory-sync error
after publication reports that the destination may already be visible. This is
a conservative development format; older or otherwise incompatible schemas are
refused, and Qt profiles and independently captured per-domain exports cannot be
restored by this command. It is not a power-loss or authenticity guarantee.
