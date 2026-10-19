# Memory

Sentinel keeps settings, memory, chat history, conversations, and semantic retrieval data in separate stores. `IMemoryStore` is for key-value memory; it is not a chat-message store. SQLite-backed stores use Qt SQL, while settings use the configured settings-store boundary.

`SQLiteMemoryStore` owns persistent memory. `SQLiteChatHistoryStore` and `SQLiteConversationStore` own their respective transcript/conversation domains. `LocalRagStore` owns local retrieval data. Backup/export is user initiated and excludes credentials and model binaries.

These records can contain sensitive local information. See [Data locations](../reference/DATA_LOCATIONS.md), [Secrets](../security/SECRETS.md), and the Desktop’s privacy and retention controls.

## Scoped management (P8-03)

The Memory page uses `ApplicationController::manageMemory` through the typed
`desktop.action` bridge. Core resolves the active persisted conversation and the
verified workspace; paths and serialized owner fields cannot establish authority.
Session and Project operations are limited to that context. Personal management
requires an explicit per-operation profile opt-in; it never enables Personal recall.
Legacy management requires explicit selection and never grants automatic recall.

Records have UUIDs, scope/owner/workspace, provenance/source, timestamps, a revision
token and correction receipts. Corrections preserve original source evidence and
creation time. Scope migration is a separate source-and-destination-authorized
operation. Mutations use optimistic revisions; SQLite batches roll back entirely
on conflict/error. Deletion changes subsequent retrieval and invalidates projected
recall state. It does not retract immutable in-flight prompts, provider copies or
historical backups, and does not promise physical erasure. Legacy historical creation
time remains unknown until a separately attributed explicit reclassification.

Schema 5 migrates existing scoped records without changing recall authority and adds
UUID/revision/receipt metadata to legacy records without changing their native IDs,
keys or values. Older schema-aware binaries must refuse this future schema. No
conversation, credential, permission or agent-run database is part of the migration.

The explicit scope archive is JSON `sentinel-scoped-memory`, version 1, with `records`
and SHA-256 over compact canonical records JSON. It preserves UUID, owner/scope,
provenance/source, timestamps, values and correction receipts. Hash verification
checks integrity, not authorship: imports append an actual import receipt and prompt
assembly marks imported history `importedUnverified`. Serialized identifiers must
match the authorized target context; they are never remapped to another workspace.
No archive data is executed or used as a filesystem destination.

The v1 path remains bounded to 500 records / 96 KiB and 64 receipts. Prefer the
P8-05 native file archive below for larger collections and consistent snapshots.
Both paths reject duplicate UUID conflicts by default; explicit skip-identical never
replaces a differing record. Imports regenerate concurrency revisions.

Generic legacy JSON memory backup/replace stays blocked whenever managed records
(including Legacy records with identities) exist. Other portable domains remain supported individually; multi-domain legacy
export/import is refused until owner quiescence and durable recovery are implemented. The scope archive is a memory-only export, not a full application backup.
Old key/value backups can be imported into an empty store as unclassified Legacy
data; they cannot overwrite managed records. There is no automatic durable
promotion, embedding/indexing, or Personal sharing change. P8-02's fail-closed image
token accounting limitation remains unchanged.

## Ranked scoped retrieval and index lifecycle (P8-04)

Schema 6 adds an optional FTS5 projection `scoped_memory_search` in the existing
memory database. Three SQLite triggers update only the affected projection row in
the same transaction as a scoped insert/update/delete. This includes correction,
scope moves (delete/insert), imports and clears. Legacy FTS remains a separate
compatibility index and never supplies automatic Chat/Agent memory.

Scoped SQL authority is applied before candidate delivery or lexical scoring. A
trusted `retrievalScope` can only restrict an existing MemoryAccessContext; it cannot
add authority. Candidate discovery considers at most six terms and returns at most
200 source records. It combines literal substring matches with optional FTS token
matches, always joining/filtering the authoritative scoped source. FTS bm25 is not
used: global corpus statistics must not become a cross-scope scoring signal. SQL
prioritizes literal exact keys and phrases before recency when preparing this bounded
pool. This is bounded relevance selection, not an exhaustive global top-k guarantee.

Within that pool, shared deterministic integer **lexical** scoring uses case-folded,
whitespace-simplified text: exact key +10000; key phrase +1200; value phrase +600;
per-term key hit +80 and value hit +20; full term coverage +200. Equal scores resolve
by Session/Project/explicit Personal scope, modification time descending, then UUID
ascending. Recency/provenance never grant authority or turn generated data into user
statements. SQLite LIKE/lower fallback retains SQLite's ASCII case behavior; Qt
case-folding is used after candidate discovery, and ready FTS5 additionally supplies
Unicode token matches. No stemming, semantic equivalence or complete Unicode recall
is promised by the fallback. Empty queries return bounded recent scoped candidates.

Chat preserves this ranking as optional-memory priority inside ContextBudgetManager.
ContextEngine keeps ranked order instead of reordering every Session record above
an exact Project hit. Existing complete-item omission, output reserve, ModelBinding,
request identity, image refusal, mandatory security/evidence admission and AgentLoop
completion acceptance remain unchanged. No embeddings, provider calls, compaction
or durable summary promotion are activated. The older SemanticRetrieval component
contains explicit fake/test and metadata overlap simulations and an opt-in provider
adapter; none of those scores are used as production scoped semantic measurements.
Semantic similarity remains explicitly unavailable/null in this path.

Index status is Ready, Stale, Unavailable, or Direct (the locked InMemoryStore has no
persistent index). Status verifies the optional FTS module with a read-only query,
trigger definitions, authorized source/projection UUID/revision/content/ownership
consistency and authorized orphan entries. It exposes no foreign counts, query text,
values or similarity diagnostics. A stale/missing/failed index uses authoritative
lexical fallback and reports its state, rather than returning unrestricted memory.
An unusable source record is an explicit InvalidInput result.

The Memory page offers ranked recall preview with exact/phrase/term explanations and
index status. Explicit confirmed REFRESH repairs at most 100 authorized source rows
and 100 authorized orphan projection rows per transaction. It does not edit memory
facts or receipts. A missing FTS table/triggers can be recreated, while incompatible
FTS/module failures stay unavailable. No full index rebuild runs after every mutation;
there is no polling, background worker, embedding job or automatic retention deletion.
Missing revisions determine the next repair batch, so a restart resumes deterministically
without a volatile cursor. Migration adds the projection without an unbounded startup
backfill; existing scoped data uses fallback until explicit bounded refresh completes.

Health and candidate SQL may scan authorized rows to validate consistency; the
candidate/result and repair row counts are bounded, not a hard CPU/deadline guarantee.
The read-only FTS probe is not a comprehensive physical database integrity certification.
Severe SQLite/FTS corruption can remain unavailable for a separately reviewed recovery
migration. P8-05 adds the consistent large archive below. The combined-backup block remains;
import trigger maintenance is atomic with scope-preserving transactional restore.


## Large memory-only archives and recovery (P8-05)

The native Memory page file workflow writes version 2 NDJSON. Its header fixes
format `sentinel-scoped-memory`, version 2, schema 7 and one scope. Each chunk
contains ordered records plus its canonical-records SHA-256. A final manifest binds
the exact header/chunk bytes, UUID sequence, chunk/record counts and versions.
Each record preserves its metadata and receipts; `exportRevision` is snapshot
metadata, never an accepted optimistic-concurrency token on import. These hashes
provide integrity, without encryption or authenticity.

Defaults are 64 MiB and 10,000 records; explicit bounds allow 96 KiB–256 MiB and
1–100,000 records. Desktop uses whole MiB controls. Chunks hold at most 100 records
and 1 MiB, with a single serialized record limited to 1 MiB minus 1024 bytes.
Oversize records/collections fail explicitly. No full archive is loaded into RAM.
A forward-only authorized UUID cursor runs in one dedicated read transaction on a
worker-owned SQLite connection. Schema 7 adds an ordered scope/owner/workspace/UUID
index; metadata and FTS behavior remain intact. Concurrent writers cannot mix
snapshot states. The cooperative 30-second worker deadline and 1-second SQL busy
wait bound ordinary jobs; filesystem stalls are not a hard real-time guarantee.
Snapshots can retain WAL pages until their transaction ends.

Native selected files never cross IPC as paths. Downloads/uploads use 32 KiB
chunks and opaque context-bound jobs. Private staging files have owner-only
permissions and an exclusive directory lock; at most two jobs coexist. Inactive
jobs expire lazily after ten minutes on a later begin operation. Restart removes
only marked, generated staging files while holding the lock. Export publishes a
new filename after complete verification and never overwrites an existing file.

Preflight checks structure, integrity, scope and owners without changing records.
Confirmed restore validates again inside one `BEGIN IMMEDIATE` transaction with
`FULL` synchronous mode. Duplicate policy is reject or explicit skip-identical;
replacement is unsupported. Any parse/authority/conflict/write/index failure or
precommit cancellation rolls back records and trigger-maintained FTS together.
A missing/stale scoped index blocks completion until explicit bounded repair.
SQLite crash recovery retains the pre-restore state. No unfinished export is
published and no interrupted job is automatically resumed or committed.
Cancellation after the commit boundary reports the actual committed outcome.
Transport/context loss can make the client outcome unknown; inspect the authorized
scope before retrying. Jobs remain bound to the original context and cannot be
redirected by changing the selected conversation/workspace.

Correction receipts are retained without pruning, capped at 256; reaching the cap
blocks further changes needing a receipt. v2 export/preflight also refuses records
at capacity, reserving room for the required import receipt instead of silently
producing an unrestorable archive. v1 keeps its stricter 64-receipt guard.
History is read in pages of 16. Receipts preceding the latest import are labelled
unverified imported history; the locally recorded import event and subsequent
local corrections are distinguished. Neither receipts nor hashes authenticate
historical source evidence. No retention duration or automatic compaction is set.

The memory-only archive is independent of combined application backup. Settings,
conversation history and memory have different storage owners with no coordinated
snapshot/rollback contract. Combined managed-memory backup therefore stays blocked.
No credentials, grants or unrelated databases are included. Personal inclusion,
Legacy recall exclusion and P8-02 vision-token restrictions are unchanged.
