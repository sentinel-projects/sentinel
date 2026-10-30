# Sentinel agent instructions

Sentinel is a C++20, Qt 6/QML modular monolith. Fedora KDE Plasma is the primary target; preserve portability across Linux, macOS, and Windows.

Before architecture-sensitive work, read [architecture](docs/development/ARCHITECTURE.md), [Agent Runtime](docs/concepts/AGENT_RUNTIME.md), [security model](docs/security/SECURITY_MODEL.md), [building](docs/development/BUILDING.md), and [testing](docs/development/TESTING.md).

Follow the [engineering standard](docs/development/ENGINEERING_STANDARD.md) for substantive work. Inspect related production owners/contracts, discover hidden operating conditions and failure scenarios, and record evidence-based findings with concrete acceptance oracles. Review the affected phase in the [module quality matrix](docs/roadmaps/MODULE_QUALITY_MATRIX.md); use the standard's lightweight path for trivial edits.

- Check platform/hardware/provider differences, resource constants, failure/cancellation, security/privacy, durability/migration, UX/accessibility, observability and upgrade/rollback where relevant. Explain unaffected dimensions; do not claim a project-wide audit from a scoped review.
- Make the smallest maintainable change; cover relevant positive, negative, boundary and regression cases and verify real production behavior when appropriate and authorized. Report implemented work separately from recommendations, including failures/skips and new risks.
- Block affected unsafe delivery for confirmed security/data-integrity violations. Map other material follow-ups to roadmap/acceptance IDs or verified existing issues with evidence and closure tests; do not expand scope or publish issues without authorization.

- Keep QML presentation-only and expose QML-safe view models.
- Agent Mode and controlled tasks execute through `AgentRuntime`; only an accepted `AgentLoop` final answer completes a run.
- Resolve active runs through `IModelRouter` and one `ModelBinding`; keep provider behavior behind `IChatProvider`.
- `ToolDescriptor`/registry contracts and `ToolExecutionGateway` are authoritative. Filesystem tools use `IFileSystemService`; processes use `ProcessExecutor`.
- Keep settings, memory, chat history, conversations, agent runs, and permission grants separate. SQLite uses Qt SQL.
- Preserve explicit permission, approval, sandbox, and platform-service boundaries. Natural-language input must never become a shell command through heuristic planning.
- Do not introduce platform-only core logic, Electron, a Python backend, or unrelated dependencies.

Validate scoped changes with the relevant tests; broad shared changes require:

```bash
cmake --preset tests
cmake --build --preset tests
ctest --preset tests --output-on-failure
```
