# Rust development daemon

This isolated Linux/macOS experiment runs one Chat at a time through an
explicit local OpenAI-compatible or Ollama text streaming endpoint. An opt-in
read-only Agent inspect-file experiment is described below. It has no credentials,
production stores, or profile migration. Sessions and committed
messages are stored in the dedicated `daemon-dev-chat.sqlite3` development file.
It never reads or writes a Qt profile; its development lock is not compatible with
Qt's runtime lock.

A separate Windows development named-pipe listener is available in source and
has not been validated on native Windows. It requires `--profile PRIVATE_TEMP_DIRECTORY`:
the directory must already be a direct child of the OS temporary directory,
owned by the current user with a protected owner-only DACL. The daemon accepts
its lock and bounded model-selection and Chat snapshot files in that profile.
It prints its private named-pipe endpoint after creation. Pass that endpoint
explicitly to the CLI `--socket` option. It accepts up to eight same-user peers
with a three-second handshake, partial-frame and reply deadline and a 30-minute
connection lifetime. Idle reads are interruptible on shutdown.

An explicitly configured `--provider ORIGIN --model ID [--provider-kind ID]`
enables literal-loopback model discovery and text Chat with streaming,
cancellation and reattachment. The daemon writes new sessions and Chat run
start/terminal snapshots under the profile lock, before acknowledgement and
terminal publication respectively. It restores interrupted runs without
replaying their request. Live deltas are kept in memory until terminal commit.
Snapshots are limited to 8 MiB, 32 sessions and 16 turns per session. The
daemon restores a saved model selection for the same startup provider and
exact loopback address. The discovery catalog remains process-local. Native
Windows pipe, storage recovery and cancellation acceptance remain open.

Before replacing a stopped Windows development profile, run
`sentinel-daemon-dev --profile PRIVATE_TEMP_DIRECTORY --check-windows-profile`.
The command requires the existing private lock and the same profile identity,
file permissions, and snapshot limits enforced at startup. It acquires the
lock exclusively and reports the snapshot version, session and run counts,
any interrupted or pending runs, and whether a saved model binding exists.
It does not start the named pipe, contact a provider, repair files or print
conversation contents. A pending staging file blocks the check until normal
daemon startup handles recovery.

To copy those validated snapshots, use
`sentinel-daemon-dev --profile PRIVATE_TEMP_DIRECTORY --export-windows-profile PRIVATE_EXPORT_DIRECTORY`.
The export directory must already exist, be a separate direct child of the
Windows temporary directory, and have a protected owner-only DACL. A successful
export publishes one owner-only `sentinel-windows-dev-profile.json` using a
same-directory rename without replacement. It contains conversation text and
the saved local model binding, when present. Keep the export private. A crashed
export may leave a private pending file in the destination. No contents are sent
to a provider or written back to the source profile. This archive uses the
Windows development format.

Use `sentinel-daemon-dev --profile NEW_PRIVATE_TEMP_DIRECTORY --restore-windows-profile ABSOLUTE_ARCHIVE_PATH`
to restore a Windows development archive into a new profile. The target must
be a direct child of the Windows temporary directory, must not exist, and its
name may contain only ASCII letters, digits, underscores and hyphens.
Both records are validated before publication. The daemon writes to a
separate owner-only staging directory, closes the files and lock, and promotes
the complete directory without replacing an existing target. A later
ordinary startup applies the existing interrupted-run handling.

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

## Persistent standalone Rust profile (Linux and macOS)

Use `sentinel-daemon-dev --persistent-profile --provider ORIGIN --model MODEL`
or pass `--persistent-profile` to the staged desktop launcher. This explicitly
selects `$HOME/.sentinel-rust` outside the temporary directory. The daemon
validates directory ownership and permissions, acquires an exclusive lock, and
serves `daemon-dev.sock` from that profile. A crash-stale private socket is
removed only under the lock after a refused connection and an identity recheck.
Use `sentinel --persistent-profile status`, `sentinel --persistent-profile tui`
or `sentinel-slint-prototype --persistent-profile` to connect. These client
options select the same endpoint as the launcher.

For a **new, absent** persistent directory, use either:

```sh
sentinel-daemon-dev --persistent-profile --restore-dev-set /tmp/PRIVATE_COPY/snapshot
sentinel-daemon-dev --persistent-profile --import-qt-preview-subset \
  /tmp/PRIVATE_COPY/conversations.sqlite3 /tmp/PRIVATE_COPY/settings.json \
  /tmp/PRIVATE_COPY/memory.sqlite3 --acknowledge-preview-subset
```

Both commands stage under the destination parent and publish with an exclusive
rename. A concurrent creation wins without overwriting the directory. The Qt
command requires offline copies and keeps the existing subset restrictions.
It does not convert the active Qt profile or import credentials, Agent runs or
permissions. Leave the Qt profile intact for access to the original data.

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
deletion flags, summaries and unsupported attachment snapshots. Known Qt provider
error categories on historical assistant failures are preserved in the
`session.messages` projection. Explicitly renamed titles and archive/pin flags are retained. Each imported session
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
Agent activity events cover model decisions and committed plan-step transitions.
Tool lifecycle and activity events contain generic descriptions. Reattaching
returns the latest bounded activity in `properties.agent_activity` and any
pending approval in the attach snapshot without consuming an event sequence.
Agent history continues to expose durable step records.
Each Agent execution has a shared five-minute wall-clock deadline and at most
24 model requests across classification, retries and delegated plan steps.
The deadline requests cooperative cancellation. It preserves an earlier
explicit cancellation and reports failed timeout or decision-limit outcomes
through the existing run terminal event. For a plan, the runtime prepares and
freezes every initial step request before any model or tool dispatch. A failed
preflight ends the run with no executed steps. Dependency checks, authorization
and budgets still apply at dispatch.


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

For local Rust-only development, build `sentinel-dev-launch`, the Rust daemon,
and the Slint prototype, then run:

```sh
sentinel-dev-launch --daemon /absolute/path/sentinel-daemon-dev --desktop /absolute/path/sentinel-slint-prototype --provider http://127.0.0.1:1234 --model MODEL
```

The launcher creates and retains a private `/tmp` development profile and prints
its path. To resume that profile, pass `--profile /tmp/PROFILE`; an existing
socket is refused and never removed automatically. `--provider-kind` explicitly
selects the supported local adapter; `--panel` and optional `--global-shortcut`
select the compact macOS UI. On desktop exit or interrupt the launcher requests
shutdown from its own verified child daemon, then terminates the child if it
does not exit. This launcher supports Linux and macOS development only; it does
not start, migrate, or replace the production Qt application. Same-user socket
replacement while the desktop is running remains outside its ownership guarantee.

To enable the scoped Agent and workspace attachment features through the
launcher, pass `--agent-workspace /absolute/workspace` and explicitly choose
`--agent-approval required` or `--agent-approval workspace`. Additional
configured roots use repeatable `--workspace ID /absolute/path` arguments.
The workspace policy is not selected automatically.
The launcher also accepts explicit `--context-window-tokens N`,
`--plugin-root /absolute/path`, and, on macOS with an Agent workspace,
`--agent-process`. Process execution remains opt-in and subject to the daemon's
sandbox and per-call approval. If forced daemon termination occurs during a
process invocation, the launcher cannot guarantee cleanup of that invocation's
separate process group; the normal shutdown path requests cancellation first.

## Import a Qt preview subset from disposable copies

This Unix-only, development-only command imports a deliberately narrow preview
into a **new** private Rust profile. It never discovers, opens, or changes a Qt
profile. First stop the Qt writer and prepare disposable copies of these three
inputs: the conversation database, `settings.json`, and the session-memory
database. Do not copy a live SQLite main file by itself: committed WAL content
may be omitted. Use a SQLite backup operation on the disposable inputs, put
SQLite copies in rollback/DELETE journal mode, close all SQLite handles, and
confirm no `-wal`, `-shm`, or `-journal` sidecars remain. Put all copies in one
owner-only directory directly under `/tmp` (mode `0700`); each input must be a
regular owner-only file (mode `0400` or `0600`) with one hard link. The three
input paths must be distinct and share the same private directory. The importer
checks each copy’s SHA-256, file identity and metadata before and after the staged
import. These checks detect changes during import. The copies must still be made at
a consistent point in time before import. Keep the Qt writer stopped until the disposable copies are complete.

Choose a destination path that does not exist and is a direct child of `/tmp`,
separate from the source directory. The importer builds all stores in a locked
private sibling staging directory, validates the inputs and imported records,
then publishes the completed profile with a no-replace directory rename. A
failure before publication removes the staging directory. If parent-directory
sync fails after rename, the destination may already be visible; inspect it
before retrying.

Run a non-publishing rehearsal with the same input copies and a new destination:

```sh
sentinel-daemon-dev --profile /tmp/NEW_PRIVATE_PROFILE \
  --check-qt-preview-subset \
  /tmp/PRIVATE_SOURCE/conversations.sqlite3 \
  /tmp/PRIVATE_SOURCE/settings.json \
  /tmp/PRIVATE_SOURCE/memory.sqlite3
```

The rehearsal writes and validates the same temporary stores, reports import
counts and discards the staging directory without publishing the destination.
It also supports `--persistent-profile` when the persistent destination is absent.
Use the same quiescent copies for a later import. To publish the supported
subset, use the explicit acknowledgement flag:

```sh
sentinel-daemon-dev --profile /tmp/NEW_PRIVATE_PROFILE \
  --import-qt-preview-subset \
  /tmp/PRIVATE_SOURCE/conversations.sqlite3 \
  /tmp/PRIVATE_SOURCE/settings.json \
  /tmp/PRIVATE_SOURCE/memory.sqlite3 \
  --acknowledge-preview-subset
```

The accepted conversation input is Qt conversation schema v6. It preserves
supported conversation/message IDs, order, text, timestamps, status and
provider/model/reply metadata, including explicitly renamed titles. It rejects
summaries, deletion flags, unsupported attachment snapshots or unknown or
inconsistent error categories, malformed rows and limits
above 64 conversations, 2,048 total messages or 32 messages per conversation.
Archive and pin flags carry into the Rust session list. Active conversations come first, with pinned conversations first within each group. Archived history stays readable, and new requests require unarchiving the conversation in Slint. Deleted source conversations cause staged import to fail before publication.

Inline user attachment snapshots are retained when they fit Rust Chat limits.
This includes text previously extracted by Qt from PDF or DOCX and validated
Qt PNG snapshots. Migration reads the stored bytes only. Unsupported or
oversized content fails the staged import before publication.
The memory input is Qt schema v7 session-scoped memory only, associated with
the imported sessions. The settings input contributes
`responseProfileInstructions` (up to 2,000 UTF-16 units), a supported
`themeName`, and one explicit local model selection when its provider and
origin are supported. The importer reports unsupported themes and model
selections without substituting a value. Other Qt settings and credentials,
Agent runs, permissions, the separate
`chat_history.sqlite3`/FTS search history, IPC projections, workspace/global
memory and other stores are excluded. This does not migrate a complete profile
or establish production Qt/Rust lock compatibility.

To return to Qt, keep both profiles. Export the Rust data with
`sentinel --persistent-profile backup export profile /absolute/new/backup.snt`
while the Rust daemon is still running, shut down Rust, then open Qt with its
original untouched profile. For a temporary Rust profile, pass its socket with
`--socket` in place of `--persistent-profile`. The Rust archive is restorable
into Rust. New or changed Rust records do not appear in Qt. Interrupted,
non-published imports may leave a private sibling staging directory, which is
separate from both published profiles.

For settings-only development import, first initialize its isolated profile
and run:

```sh
sentinel-daemon-dev --profile /tmp/PRIVATE_PROFILE --import-qt-settings-copy /tmp/PRIVATE_COPY/settings.json
```

The copy must be an owner-only regular file in a separate private direct `/tmp`
child. The caller must prepare the copy after stopping the Qt writer; the
importer reads it without modifying it and does not access a Qt profile. It
recognizes Qt's optional schema version 1. It transfers present `themeName`
(`system`, `light`, or `dark`), `responseProfileInstructions` (at most 2,000
UTF-16 units), and one explicitly selected Ollama, LM Studio, or llama.cpp
model with a valid literal-loopback origin. The daemon restores the model
only when the configured provider and origin match. Importing the selection
does not configure or contact an endpoint. Other keys, including credentials,
remain excluded. Existing destination values for any imported setting refuse
the entire transaction. Unsupported preference values, duplicate selected
keys, and unknown schema versions are refused. This is not
a complete settings migration or a Qt backup import.

The daemon can inspect explicitly selected local speech artifacts at startup with `--speech-binary PATH --speech-binary-sha256 HEX --speech-model PATH --speech-model-sha256 HEX` after `--provider ORIGIN --model ID`. All four options are required together. It reads only regular single-link files owned by the current user or root, rejects group/other-writable inputs and symlink leaves, bounds the binary to 256 MiB and model to 4 GiB, checks both supplied SHA-256 digests and the Whisper GGML magic, then exposes the startup result through the read-only `speechSettingsState` query and `speech-inspection` capability. Inspection alone is not publisher authentication, full model validation, or runtime readiness. Hashing a large model can delay startup.

On macOS only, explicitly add `--speech-execution` with those four inspection
options to enable the `voice` capability. The worker rechecks the pinned hashes
before execution, writes only the supplied PCM to a private temporary WAV, and
runs the selected Whisper binary with a deny-by-default `sandbox-exec` profile:
no network or file writes, bounded output, and a 120-second deadline. It does
not capture a microphone, send the transcript to Chat, or claim voice output.
`voice.state` remains unavailable unless execution is explicitly enabled and
the local sandbox executable is present. Native Whisper/model compatibility
and successful macOS sandbox execution have not been verified; forced-kill
cleanup is best effort. This remains a development-only opt-in, not production
speech support.

Example (with hashes computed independently for the local artifacts):

```sh
sentinel-daemon-dev --profile /tmp/PRIVATE_PROFILE \
  --provider http://127.0.0.1:1234 --model MODEL \
  --speech-execution \
  --speech-binary /absolute/path/whisper-cli \
  --speech-binary-sha256 BINARY_SHA256 \
  --speech-model /absolute/path/ggml-model.bin \
  --speech-model-sha256 MODEL_SHA256
```

The provider/model pair is required by the daemon's current option parser even
when the requested operation is speech transcription. The CLI client reads an
explicitly named `.wav` or raw PCM file and supplies its decoded PCM; it does not
invoke the microphone capture path.

Flat OpenDocument Text attachments are a restricted development feature advertised as `chat-attachments-fodt` when the configured workspace attachment path is available; they use the existing `chat.send` attachment paths and require no additional command-line flag. Inputs must be explicitly selected workspace-relative `.fodt` files. The parser accepts UTF-8 flat XML up to 1 MiB and extracts at most 8 KiB from simple text paragraphs, headings, lists, spans, links, tabs, and spaces; DTDs, external entities, unsupported content, excessive nesting, and oversized output are rejected. Formatting and link targets are not preserved. Zipped ODT, scanned documents and OCR are not supported. The extracted text is treated as untrusted user-supplied attachment data.

Word document attachments are advertised separately as `chat-attachments-docx`.
The Rust ZIP reader opens only the main `word/document.xml` part from an
authorized workspace-relative `.docx` snapshot. Files are limited to 1 MiB,
256 archive entries, 256 KiB of inflated XML and 8 KiB of extracted text.
Simple paragraphs, tables and hyperlinks are supported. Hyperlink targets,
external relationships, embedded resources and formatting are excluded.
The extractor rejects unsupported body markup and never writes ZIP entries
to disk.

Text PDF attachments are advertised as `chat-attachments-pdf-text`. The
daemon passes the held workspace snapshot through a socketpair to a separate
process running the same Rust binary. The worker limits input to 1 MiB,
the document to 16 pages and 2,048 objects, stream decompression to 128 KiB
and extracted text to 8 KiB. A process memory/CPU limit and the parent's
900 ms deadline bound parser failures. Encrypted, malformed, scanned and
textless documents are refused. Links and embedded files are not opened.




### Local provider catalog and image attachments

Repeat `--provider-config KIND ORIGIN MODEL` after the primary
`--provider ORIGIN --model MODEL` to configure up to four distinct local
provider kinds. For example:

```sh
sentinel-daemon-dev --profile /tmp/PRIVATE_DIRECTORY \
  --provider http://127.0.0.1:1234 --model LM_STUDIO_MODEL --provider-kind lm-studio \
  --provider-config ollama http://127.0.0.1:11434 OLLAMA_MODEL
sentinel --socket /tmp/PRIVATE_DIRECTORY/daemon-dev.sock provider list
sentinel --socket /tmp/PRIVATE_DIRECTORY/daemon-dev.sock provider select ollama
sentinel --socket /tmp/PRIVATE_DIRECTORY/daemon-dev.sock models
```

Slint's Models page shows the configured provider picker. Provider selection
persists with the model and exact origin, and discovery queries the selected
endpoint. Reselecting a different provider uses that provider's startup model.
All endpoints use literal loopback HTTP. Configure the provider kinds at daemon
startup. For a configured Ollama, LM Studio or llama.cpp provider, set a new
origin with `sentinel provider endpoint KIND http://127.0.0.1:PORT` or the
Desktop settings equivalent. Changes are rejected while a run or a local
Ollama operation is active. Changing the active origin resets discovery and
selects the provider's startup model. Refresh discovery to choose another
model. An override is restored only when its provider and exact startup
origin still match. A different origin uses default context policy until the
runtime is explicitly restarted with policy for that origin. Qt settings
imports cannot grant endpoint overrides.

Images require the primary binding, an authorized workspace,
`--context-window-tokens N` and `--image-token-upper-bound N`. Set both limits
using the actual model and server configuration. The image limit must bound
the provider's token cost for every supported image. The daemon refuses an
image on any other origin, adapter, or model. A Chat message can attach one
PNG, JPEG, or static WebP of up to 32 KiB, with dimensions up to 2048 × 2048, within the four-file
limit. The daemon retains validated attachment snapshots for history, retry,
and branching. Session-message IPC omits image bytes.

Use the Slint workspace picker or `sentinel chat --attach relative/path.png 'prompt'`.
Use the same entry points for UTF-8 text or supported FODT documents. Both
OpenAI-compatible and Ollama Chat request paths send images in their respective wire formats. Cloud
providers, credential management, PDF and DOCX extraction remain outside
this implementation.

### Explicit cloud Chat

Linux and macOS support text Chat with fixed OpenAI, Groq, Mistral, Claude,
Gemini and DeepSeek HTTPS endpoints. DeepSeek uses its non-thinking
Chat Completions mode.
Choose an explicit model and credential source:

```sh
sentinel-daemon-dev --persistent-profile --cloud-provider openai MODEL /absolute/private/api-key
sentinel-daemon-dev --persistent-profile --cloud-keychain mistral MODEL ACCOUNT
```

The key file must be a regular, single-link, owner-only file in an owner-only
directory. The Keychain option is macOS-only and reads an existing generic
password for service `com.sentinel.rust.PROVIDER` and the given account.
Provider IDs are `openai`, `groq`, `mistral`, `claude`, `gemini` and `deepseek`.
For Gemini, enter the short model ID, without the `models/` prefix. The staged launcher accepts
`--cloud-provider PROVIDER --model MODEL --cloud-key-file PATH` or the same
provider and model options with `--cloud-keychain ACCOUNT`. Credentials are read at startup.
Restart the daemon after replacing or rotating the credential.

Cloud mode sends only the current prompt by default. Add `--cloud-history`
to the daemon or staged launcher to include completed text turns in the same
conversation from the exact same provider and model. Imported and local turns
are excluded. This option can include earlier turns sent with another account
at that provider. Use `--cloud-attachment-root /absolute/path` on the daemon
or launcher to allow explicit UTF-8, FODT, DOCX and text PDF attachments from
one workspace. The provider receives extracted text for the current request.
The root cannot contain the active profile or credential file. For an
explicitly selected OpenAI vision model, add `--cloud-vision CONTEXT_TOKENS
IMAGE_TOKENS` to enable one bounded current-turn image from that root. The
configured upper bound must cover the selected model's image cost. The
vision budget is tied to the exact startup model. Cloud history continues
to exclude prior attachments. Response-profile instructions, retry/edit
replay and Agent tooling remain unavailable in cloud mode. HTTPS uses native
certificate validation, a fixed host, bounded responses, and direct connections.
Model discovery and selection use the existing model IPC. Persisted selections
can identify the provider and model, while credentials stay out of IPC,
profiles, backups and Qt migration. Claude and Gemini use their native text
streaming protocols. A model catalog that exceeds the bounded page is refused
rather than presented as complete.


### Unix shutdown

On Linux and macOS, `daemon.shutdown`, SIGINT and SIGTERM stop new run
admission and signal cancellation to active Chat, Agent, speech and model
operations. The daemon retains the profile lock while waiting up to five
seconds for workers to finish. A drain timeout exits with failure and
leaves incomplete runs for restart recovery. Cancelling a local model wait
does not establish the remote Ollama operation's outcome. Unauthenticated
IPC clients must complete their handshake within three seconds.


### Agent authorization and capability inspection

Tool descriptors supply the argument schema, access type, risk and baseline
approval requirement. `sentinel doctor` includes the enabled descriptors
under `terminal.properties.agent_tools`. A task may require approval beyond
this baseline. The daemon accepts approval responses only from a peer
attached to that Agent session, and rechecks workspace access before treating
an approval as executable. Tools still execute through the bounded workspace
gateway and platform policy.


Typed Agent execution shares a five-minute wall-clock window and a 24-request
model budget across classification, retries and plan phases. The daemon
reports explicit cancellation, elapsed deadline and exhausted decision
budget separately. The existing per-call authorization policy still applies.


An Agent request is admitted durably before the start reply. The Unix daemon
waits for the reply write before releasing the Agent worker to execute. Failed
delivery cancels the admitted run before a provider or tool call.

Linux and macOS reuse one Chat/Agent worker behind a bounded queue. An explicit
Agent plan prepares every phase's initial provider request before dispatching
its first phase. An invalid later phase therefore fails before an earlier
phase can make workspace changes.


### Delegated reads and Windows text analysis

On Linux and macOS, `agent-delegate` accepts a user-defined JSON graph with up
to four child tasks. Each task is read-only and requires its own approval.
Dependencies refer to zero-based indexes of earlier children. The tasks
share the parent binding, deadline, decision budget and cancellation.
Child IDs and outcomes are recorded in the parent audit.

```json
{"operation":"delegate-read","steps":[{"task":{"operation":"inspect-file","path":"notes.txt"},"dependsOn":[]}]}
```

On Windows, `agent-analyze` submits supplied text to the local provider. The
daemon checks quoted evidence against the input before accepting the analysis.
This mode does not grant filesystem or process access.
