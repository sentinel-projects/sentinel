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
