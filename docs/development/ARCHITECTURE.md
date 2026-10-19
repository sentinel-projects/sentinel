# Architecture

Sentinel is a C++20/Qt 6 modular monolith. Desktop, CLI, daemon, and plugin host are executable surfaces over the `sentinel_core` static library.

```mermaid
flowchart TD
  D[Desktop Qt/QML and view models] --> IPC[Typed local IPC v1]
  A[ApplicationController and core services]
  C[Legacy C++ CLI] --> LC[Separate in-process core composition]
  RC[Rust CLI] --> IPC
  RT[Rust TUI] --> IPC
  IPC --> N
  N[Daemon] --> A
  A --> R[Agent and chat runtime]
  R --> T[Tool registry and gateway]
  R --> P[Provider/model routing]
  A --> S[Settings, sessions, memory, stores]
  T --> X[Plugins, MCP, filesystem, processes]
  P --> X
  S --> O[Qt SQL and platform paths]
```

`apps/sentinel-desktop` owns the Qt/QML shell and bootstrap. QML is presentation; `DesktopShellViewModel` and related view models expose safe state and actions. `apps/sentinel-cli` is a partial, small command dispatcher. `apps/sentinel-daemon` is the headless production runtime authority for Desktop and Rust CLI/TUI clients: it composes existing core services through the same builder and exposes versioned local IPC for sessions, Chat, Agent, approvals and cancellation. Production Desktop is a daemon-backed IPC client with separate local presentation preferences. Its Quick Panel shares the existing DesktopRuntimeClient; NativeCompanionAdapter owns platform activation, tray, shortcuts and lifecycle. Desktop can launch a detached daemon through `ApplicationBootstrapper::ensureBackgroundDaemon` when needed; safe mode and `--no-daemon-start` suppress this attempt. Process launching does not transfer execution ownership to Desktop: it constructs no in-process ApplicationController fallback. Rust clients require an externally started daemon. The legacy C++ CLI retains a separate in-process composition path. See [daemon development](DAEMON.md) and [IPC](../interfaces/IPC.md). `apps/sentinel-plugin-host` supports the experimental plugin-host protocol.

`core/` owns the shared application controller and domain services. Agent work flows through `AgentRuntime`/`AgentLoop`, with `LlmAgentRuntime` supplying LLM step decisions and `ControlledTaskService` projecting controlled tasks through the same runtime; provider behavior through `IChatProvider` and `IModelRouter`; tools through `IToolRegistry`, `ToolDescriptor`, and `ToolExecutionGateway`. `IFileSystemService` and `ProcessExecutor` isolate system-facing tools. Platform functionality belongs behind interfaces such as `IPathProvider`, `IPlatformService`, and `ISystemIntegrationService`.

Persistence remains split: settings use `ISettingsStore`; memory uses `IMemoryStore`; history and conversations use their own stores; permission grants and agent runs have separate SQLite stores. Qt SQL is the SQLite path. Core should not depend on QML presentation or platform-only behavior.

Scoped memory uses the existing `IMemoryStore` owner and memory database. Schema v4 adds
`scoped_memory_entries` for session, project and explicitly included personal records; the old
`memory_entries` table remains unclassified Legacy data with its original keys/values/IDs.
Legacy data is excluded from automatic scoped retrieval. Session records also bind their workspace
so selecting another workspace cannot expose the same conversation's previous project memory.
ApplicationController resolves scope from the active conversation and selected existing workspace;
ChatModeService snapshots scoped data before inference. AgentLoop/ContextEngine and memory-search
use runtime-owned session/workspace identity, including the gateway's sealed invocation session.
Stable UUIDs, UTC metadata, typed provenance and bounded source references survive retrieval;
generated/inferred/unknown content remains untrusted data. Personal inclusion is off in production
composition until an explicit sharing policy is supplied. Existing JSON memory backup is refused
when scoped rows exist, avoiding lossy export or rollback; scoped management/export/deletion UI
remains later P8 work. See [P8-01](../audits/P8_01_SCOPED_MEMORY_IMPLEMENTATION_RESULT.md).


The concurrency model uses Qt signals/events for UI and provider integration; agent execution scheduling and loop events keep active work observable and cancellable. See [Agent Runtime](../concepts/AGENT_RUNTIME.md) for run semantics. These components are implemented; reliable real-model task completion remains unaccepted.

Built-in provider identity and static registration metadata live in the immutable `ProviderRegistry`
descriptor table. ModelService composes its existing runtime factories from those descriptors and
continues to own selection, binding, endpoint/credential resolution, health and capability merging.
Discovery dispatch consults the same descriptors; transport, catalog parsing and cancellation remain
in their existing owners. Provider defaults carry `ProviderDefault` provenance and are not live
model verification. See the [P2-M05 audit](../audits/P2_M05_PROVIDER_REGISTRY_DISCOVERY_RESULT.md)
for facade compatibility and discovery limitations. The shared non-Ollama endpoint adapter now
lives in `core/provider/SelectedEndpointChatProvider`, composed through a narrow factory returning
`IChatProvider`. It owns request execution and protocol-specific continuation/response handling;
ModelService retains selection, binding, configuration, health and capability orchestration.
It reuses the existing LocalInference transport and protocol modules rather than creating vendor
HTTP engines. OpenAI-compatible native request/continuation serialization, shared function-name/tool
schema conversion, first-choice/usage normalization and object-only JSON decoding live in the
existing OpenAiCompatibleProtocol module. The adapter retains capability/registration policy,
request budgets, transport execution, lifecycle/error mapping and native Claude/Gemini orchestration. See the [P2-M06 audit](../audits/P2_M06_SHARED_ENDPOINT_ADAPTER_EXTRACTION_RESULT.md).

Hardware capability facts and model-fit estimates exist, but a continuous CPU/RAM/GPU pressure-driven adaptive resource policy is not implemented. Linux global Quick Panel shortcuts use the session-bus GlobalShortcuts portal when the backend confirms a binding; unavailable portals and refused bindings remain unavailable. Native KDE/Wayland registration and focus acceptance remain unverified (see the [P6-04 report](../audits/P6_04_KDE_WAYLAND_NATIVE_INTEGRATION_RESULT.md)). Daemon clipboard tools require a GUI and are unavailable in the headless process; Linux application launching is explicitly unsupported. Native platform behavior remains subject to acceptance, including Rust Windows named-pipe transport, which is not implemented. Public distribution blockers remain in the [SEC-P1 register](SEC_P1_4_GITHUB_HARDENING_REPORT.md#5-consolidated-blocker-register).
