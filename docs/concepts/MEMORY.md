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

Exports/imports are bounded to 500 records and 96 KiB and refuse partial results.
The default duplicate policy rejects the entire import; the explicit alternative
skips logically identical records and rejects differing UUID conflicts. Imports are
transactional. Internal concurrency revisions are regenerated to avoid stale-token
reuse. Correction receipts are bounded to 64; export refuses records at that bound
because a restore must retain room for its import receipt. A larger or long-lived
archive/history policy is deferred. Export pagination is serialized within a daemon
request, but is not a cross-process database snapshot; coordinate external writers
when taking an archive.

Generic legacy JSON memory backup/replace stays blocked whenever managed records
(including Legacy records with identities) exist. Other backup domains remain
supported. The scope archive is a memory-only export, not a full application backup.
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
migration. The 500-record/96-KiB archive, 64-receipt limit, combined-backup block and
cross-process archive snapshot limitation remain for P8-05; import trigger maintenance
is atomic with existing scope-preserving transactional restore.
