<!-- Generated QA-01 snapshot, not an execution receipt. -->

Regenerate from `verification/registry.json` with `python3 tools/verification/validate.py`.
This snapshot lists source-inspected scope and gaps; it does not certify any feature.
See [contract and proposed QA-02 architecture](../../verification/README.md).

# Verification coverage summary

Registry valid: True
Release certified: False (registry validation is not product acceptance)
Candidate SHA: e639a0429baf35445daad84eb70a9d5e001cf082
Dirty candidate: True

Verification states: {"blocked": 8, "unsupported": 1, "unverified": 34}
- FSV-BASELINE: partial; mapped; unverified; pending
- FSV-DESKTOP: partial; mapped; unverified; pending
- FSV-QUICK-PANEL: partial; mapped; unverified; pending
- FSV-TRAY: partial; mapped; unverified; pending
- FSV-NATIVE: partial; mapped; unverified; pending
- FSV-CLI: partial; mapped; unverified; pending
- FSV-TUI: partial; mapped; unverified; pending
- FSV-LEGACY-CLI: partial; mapped; unverified; pending
- FSV-IPC: partial; mapped; unverified; pending
- FSV-CHAT: implemented; mapped; unverified; pending
- FSV-AGENT: partial; mapped; unverified; pending
- FSV-TOOLS: implemented; mapped; unverified; pending
- FSV-APPROVAL: implemented; mapped; unverified; pending
- FSV-LOCAL-PROVIDERS: partial; mapped; unverified; pending
- FSV-CLOUD-PROVIDERS: partial; mapped; unverified; pending
- FSV-DISCOVERY: partial; mapped; unverified; pending
- FSV-MODEL-STORAGE: partial; mapped; blocked; blocked
- FSV-RESOURCES: partial; mapped; blocked; blocked
- FSV-SESSIONS: implemented; mapped; unverified; pending
- FSV-MEMORY: partial; mapped; unverified; pending
- FSV-BACKGROUND: partial; mapped; blocked; blocked
- FSV-SPEECH: partial; mapped; unverified; pending
- FSV-MULTIMODAL: partial; mapped; unverified; pending
- FSV-EXTENSIONS: partial; mapped; unverified; pending
- FSV-DIAGNOSTICS: partial; mapped; unverified; pending
- FSV-SECURITY: partial; mapped; unverified; pending
- FSV-PERFORMANCE: partial; mapped; unverified; pending
- FSV-PLATFORMS: partial; mapped; unverified; pending
- FSV-FEDORA: partial; mapped; blocked; blocked
- FSV-INSTALL: partial; mapped; unverified; pending
- FSV-RELEASE: planned; mapped; blocked; blocked
- FSV-AUTOMATION-FUTURE: not_implemented; mapped; blocked; blocked
- FSV-LEASE-FUTURE: not_implemented; mapped; blocked; blocked
- FSV-WEBUI-ACP: stub; mapped; blocked; blocked
- FSV-WINDOWS-IPC: unsupported; mapped; unsupported; blocked
- FSV-QA: implemented; mapped; unverified; pending
- FSV-SETTINGS: partial; mapped; unverified; pending
- FSV-WORKSPACES: partial; mapped; unverified; pending
- FSV-PLUGIN: partial; mapped; unverified; pending
- FSV-SKILLS: partial; mapped; unverified; pending
- FSV-RETRIEVAL: partial; mapped; unverified; pending
- FSV-MODES: partial; mapped; unverified; pending
- FSV-NOTIFICATIONS: partial; unmapped; unverified; pending
- Warning: missing external component evidence: roasd.packaging

## QA-02A fixture boundary mappings

The eight E2E IDs map to inspected QtTest slots in the registry. Five bounded scopes are executable:
streaming Chat plus durable daemon projection (01), rejected approval with zero workspace mutation
(03), cancellation before approval with rejected replay (04), reconnect with authoritative snapshot
(06), and client/daemon terminal-state consistency (08). The provider-driven Agent chain (02), full
process restart with generation fencing (05), and conversation-store cross-session isolation (07)
remain Blocked; their supporting component/IPC probes do not close those scopes. Existing generation
and duplicate-event tests remain available in `test_agent_reliability_ipc`; they are not a daemon
process crash test. No AC status or feature verification level is promoted. All 43 features and
65 historical criteria remain intact. See the QA-02A audit for actual execution outcomes and gaps.

## QA-02B provider-driven fixture chain

E2E-02 now maps to `test_provider_agent_e2e::providerDrivenChain`: scripted IChatProvider,
production LLM planner, AgentLoop, gateway, real filesystem executor, and separate SQLite
conversation/run/projection stores. Seven rows cover authorized write/read/final, denial,
cancellation awaiting approval, out-of-workspace resource, malformed arguments, rejected final
after a successful write, and old-generation approval replay. The collector requires every
row's independently checked SHA-256 manifest and persisted counters. This closes the bounded
fixture chain gap only; E2E-05 and E2E-07 remain Blocked. Historical feature and acceptance
statuses are unchanged. See the QA-02B audit for exact execution evidence and QA-02C scope.
