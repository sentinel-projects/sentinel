# Engineering standard — all Sentinel work, P0–P12

Repository policy introduced by Prompt 93, 2026-10-30. This standard applies to features, defect fixes, refactors, migrations and releases. It supplements [AGENTS](../../AGENTS.md), [architecture](ARCHITECTURE.md), the accepted [ADRs](../decisions/README.md), [security](../security/SECURITY_MODEL.md), [testing](TESTING.md) and the existing [acceptance matrix](../roadmaps/ACCEPTANCE_MATRIX.md). It does not adopt pending roadmap parameters, close acceptance rows or grant publication/runtime authority.

## Choose the appropriate path

**Trivial:** wording, links, comments or a reversible change with no behavior/contract/storage/permission effect. Record scope, inspect surrounding context, check links/format and applicable generation/build checks. A sentence in the change description is sufficient; do not add implementation-mirroring tests or require an unrelated live/model run.

**Substantive:** changes to behavior, public/domain contracts, resource policy, persistent state, platform integration, security or distribution. Follow the evidence workflow below. Scale the record to the risk: a small bug can use a few paragraphs; a cross-owner migration needs a design and failure/recovery matrix. Reviewing all dimensions does not mean executing every environment on every change. Explicitly state why a dimension is unaffected; do not copy blanket “all checked” claims.

**Urgent defect:** preserve the failing evidence and apply a scoped containment/correction with deterministic regression. Record deferred broader review and its owner/phase. Urgency never permits credential exposure, permission bypass or unreviewed destructive migration.

## Evidence workflow

1. **Resolve and inspect.** Use Personal-Brain project resolution when available; otherwise record its unavailability and the user's explicit checkout. Read local instructions, the changed owner's architecture/ADRs, callers, production composition and related tests. Capture branch/base, dirty work ownership, generated contracts, relevant source/binary provenance. Preserve other work; resolve unexpected drift before edits.
2. **Discover requirements and risks.** State the user-visible trigger and desired behavior, current authority/data owner and relevant hidden operating conditions. Use the dimensions below to find concrete failure scenarios, including absent/unknown information. Search related issues and prior failed/skipped receipts. An open issue title is tracking metadata, not proof the current source still has the defect.
3. **Design the smallest correction.** State alternatives only where a real tradeoff exists; select one owner for each contract/type. Describe budgets, cancellation, errors, unknown states, persistence and upgrade/rollback before implementation. Durable changes to accepted architecture need an ADR; ordinary implementation within an ADR does not need a new abstraction/ADR.
4. **Implement within existing boundaries.** AgentRuntime/AgentLoop, IModelRouter/ModelBinding, IChatProvider, ToolDescriptor/registry and ToolExecutionGateway remain authoritative. System effects use IFileSystemService/ProcessExecutor. QML presents QML-safe state. Settings, memory, history, conversations, runs and grants keep separate owners and Qt SQL stores. No parallel engine or platform-only core shortcut.
5. **Verify the risk, not just the happy path.** Provide positive, negative, boundary and regression cases relevant to the change. For each material scenario specify an independent oracle and concrete pass criterion before running it. Include failure/cancel/late-event behavior for asynchronous work. Fake tests validate contracts; actual product acceptance additionally uses the real production composition where applicable and authorized.
6. **Accept with honest evidence.** Run the required scoped lint/generation/type/build/tests and broader checks for shared behavior according to AGENTS/TESTING. Retain first failures, skips and reruns, exact commands and environment. Check actual artifact/effects and terminal state, not just success text. Native platform, hardware and live-provider cells remain unverified when unavailable. Do not add billable/live requests merely for display evidence.
7. **Reconcile follow-ups.** Report implemented behavior separately from proposals. Material unresolved findings receive a concrete local tracking key or an existing verified issue, roadmap/acceptance ID, suggested priority, next action, closure test and dependency. Create/change remote issues only when authorized; a local follow-up register is valid pending publication. Keep unrelated improvements out of the current patch.

## Required risk dimensions and example questions

For a substantive change, record affected dimensions as findings with evidence. These examples are prompts for investigation, not a requirement to invent a problem in every row.

| Dimension | Evidence to seek / acceptance implication |
|---|---|
| Architectural consistency and extensibility | Real production caller and owner, accepted ADR, one authoritative type/contract; test the actual composition, not an unused helper. Can another backend implement the contract without vendor assumptions? |
| Cross-platform behavior | Linux/KDE/Wayland primary, macOS/Windows ports, headless/GUI separation, path/case/permissions, process/thread/socket semantics; declare supported and unavailable cells. |
| Hardware scalability | Cold model, slow CPU/disk, memory pressure, unknown telemetry, battery/thermal constraints; measured profile and bounded resources, not universal RAM thresholds. |
| Local/cloud/provider differences | Frozen binding, discovered vs default capabilities, optional usage, remote cancellation unknown, reasoning/native/streaming dialects; no undocumented parameters or fallback provider. |
| Performance, resources and constants | Identify changed/consumed deadlines, queues, byte/output/context caps and concurrency; classify each as security bound, policy default, protocol limit or measured budget. Explain units/owner/configuration and test at the boundary. Never adapt away security/protocol caps. |
| Failure and cancellation | Not-started, partial output/side effects, hung dependency, race at deadline, late callback, non-cooperative worker, retry after possible dispatch; bounded stop with no false success or duplicate mutation. |
| Security and privacy | Untrusted input/response, descriptor validation, authority/grants/sandbox, secrets and logs, least scope, retention; negative tests prove denial and no sensitive projection. |
| Durability and migration | Transaction/admission, audit failure before dispatch, disk full, interrupted upgrade, read-only state, schema compatibility, backup integrity; no unsupported multi-owner rollback claim. |
| UX and accessibility | Real flow, loading/empty/error, focus/keyboard/screen reader, responsive sizes and readable recovery; projections truthfully separate pending, failed and complete. |
| Observability and support | Stable local correlation, monotonic stages, unknown vs zero, bounded safe diagnostic storage; operator can distinguish observed facts from inference without payload logging. |
| Upgrade and rollback | Exact artifact/profile/schema identities, ownership/quiescence, compatible reader or explicit downgrade refusal and tested recovery. Rebuilding disk files does not activate a running runtime. |
| Testability | Injected clocks/transports/failures, sync/async parity and independent oracles; account for skipped native or paid tests without calling them passed. |

## Minimum substantive change record

Use an existing issue/PR, audit report or a short design section; no duplicate document is required. Include:

- Problem/trigger and expected outcome; production owner/call chain and source/configuration baseline.
- Material findings: **operating condition → failure consequence → source/runtime evidence → chosen mitigation or follow-up**. Give each a priority and closure condition. Identify unknowns and inspected scope.
- Acceptance cases: **scenario + oracle + budget/limit + expected terminal/effect + evidence location**. Include the relevant positive, negative, boundary and regression cases; explain omissions rather than filling checkboxes.
- Implementation and compatibility impact; source files and contracts changed; data/security/platform consequences, migration and rollback path if affected.
- Commands/results, failures/skips, actual production/native acceptance when appropriate, candidate provenance and residual risk. Distinguish implemented, source-derived, observed and proposed claims.
- Follow-ups with phase/acceptance/verified issue mappings. Link the initial [module risk matrix](../roadmaps/MODULE_QUALITY_MATRIX.md) and update only the affected row when new material evidence changes it; historical audits stay immutable.

For example, “cloud timeout fixed” is insufficient. A useful finding is: “Classification reached its local deadline without observed response metadata; preserve safe per-attempt phase timings, test partial-body abort, retain Failed/ObservationIndeterminate with zero dispatch, and keep gateway queueing unproven until correlated receipts exist.”

## Gates and escalation

Block the **affected unsafe delivery/action** for a reproduced security boundary violation, unprotected secret exposure, irreversible data-loss/migration risk, duplicate side-effect dispatch, false accepted completion or unverifiable artifact/profile ownership. Preserve evidence and propose safe containment. Do not turn a speculative unrelated risk into a blanket stop on all work.

Lower-priority maintainability/default/performance concerns are scoped follow-ups with evidence and closure tests. A missing environment blocks that acceptance claim, not independent deterministic work. Existing unresolved project issues do not automatically prohibit a safe scoped diagnostic patch. Publication, legal/security disclosures and credential handling retain their separate rules.

## Repeatability and CI

Use existing CI: C++/Qt CMake/CTest, formatting, generation checks, QML lint where affected, Rust fmt/clippy/test where Rust/contracts are affected, platform/package checks where applicable. Preserve pinned actions and read-only PR jobs. A schema/IPC contract change requires migration/generation/consumer tests; documentation-only changes use link/structure/privacy/whitespace checks.

CI verifies executable checks; it cannot automatically certify architectural reasoning, native behavior, current model quality or a paid gateway. The contributor/PR change record must supply that evidence and label missing cells. New CI gates require their own scope and regression; this policy does not silently alter workflow permissions or create a generic checklist bot.

For release acceptance, use the existing EV-01 exact-candidate evidence contract and [release process](../release/RELEASE_PROCESS.md). Never relabel an excluded/blocked phase as complete. Review the related module risk row when starting substantive work and at acceptance; reconcile recurring failures into a scoped regression and trackable follow-up rather than relying on the maintainer to rediscover them.
