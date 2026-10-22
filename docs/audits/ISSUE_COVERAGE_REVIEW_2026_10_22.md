# Issue coverage review — initial bounded pass

Date: 2026-10-22. Candidate: `2cc7a301a991e031ded9c7b8f4a7cd6e21e6e2b1`, branch `fix/26-release-manifests`.

## Scope and limits

This is a discovery pass, not a full application audit or issue-closure review.
Read the local agent instructions, architecture, Agent Runtime, security model,
building and testing guidance. Personal-Brain's `brain` executable was unavailable.
Retrieved all 53 open GitHub entries: 52 individual issues and tracker #71.
Compared the following findings with their titles and bodies. Existing issue
claims have not been independently accepted merely because they are listed.
No product source changed; no GitHub issues were created, edited or closed.
No full build, CTest, live provider, native desktop or package acceptance ran.

Inspected production web-fetch response handling, Agent output truncation and
its call sites, MCP response buffering, and selected auth/export helper code.

## Findings

### DISC-01 — Full tool output files overwrite one another (new issue candidate)

Suggested priority: P1. `core/src/runtime/ToolOutputTruncator.cpp:74–82`
uses only tool name and wall-clock milliseconds as file identity, then opens
with `WriteOnly`. Two outputs for the same tool in the same millisecond share
a path and the later write replaces the earlier output. Multiple AgentLoop
instances also use the same default output directory.

Production reachability: `AgentLoop::truncatedObservation` at
`core/src/agent/AgentLoop.cpp:133` calls this owner and advertises the saved path
as the complete observation. This can make an earlier evidence reference resolve
to a later operation's content. Actual concurrent Agent runs were not exercised.

Reproduced by invoking the production truncator twice per iteration, up to 100
iterations, with `maxBytes=4`, the same `run-command` tool name, a private
QTemporaryDir, and distinct `FIRST-PAYLOAD` / `SECOND-PAYLOAD` bytes. A collision
occurred; reading the first returned path yielded the second payload.

Acceptance: exclusive unique file creation; run/session isolation where needed;
earlier content remains unchanged under repeated/concurrent same-tool writes.
No open issue body explicitly covers this collision.

### DISC-02 — Unwritten full-output paths are advertised (new issue candidate)

Suggested priority: P1; may share a persistence-reliability issue with DISC-01.
`core/src/runtime/ToolOutputTruncator.cpp:74–85` ignores directory creation,
open and write failures and returns a nonempty fullOutputPath regardless.
`AgentLoop::truncatedObservation` then tells the model to retrieve that file.
The omitted output can therefore be unavailable without a truthful diagnostic.

Reproduced with a regular file occupying the configured output-directory path.
The production truncator returned a nonempty output path that did not exist.
This verifies an open-failure case; disk-full/short-write cases remain untested.

Acceptance: publish a path only after successful complete persistence; expose
failure without claiming that omitted bytes are retrievable; cover creation,
open and partial-write failures. No open issue body explicitly covers this path.

### DISC-03 — Web fetch treats failed HTTP replies as success (extend #35)

Suggested priority: P1. `core/src/runtime/tools/WebFetchTool.cpp:159–175`
records the HTTP status but never checks it or QNetworkReply::error before
setting success=true. Redirect following is disabled. Both HTTP 404 with
ContentNotFoundError and HTTP 302 without a transport error returned success.

Production reachability: `RealToolExecutor::executeWebFetch` at
`core/src/runtime/RealToolExecutor.cpp:2658` trusts response.success. The sync
path checks isFinished, which does not establish HTTP success. The async path
also calls the same parser directly.

Validation used a synthetic QNetworkReply subclass and the production response
parser (test-only private access); no real HTTP transport or full gateway run.
This expands existing #35's false-success theme rather than proving a separate
unrelated issue. Its current body does not mention web-fetch.

Acceptance: HTTP/network failure maps to typed failed tool execution; explicit
redirect behavior; deterministic tests for 404, 500, connection error, abort,
redirect and valid success in both entry paths.

### DISC-04 — Web response size limit runs after buffering (source finding)

Suggested priority: P1 candidate, requires bounded transport reproduction.
Both fetch paths wait for completion without a readyRead byte cap; the 5 MiB
check is performed only inside processResponse after reception. This bounds
accepted content, not peak transport buffering. A large response may consume
substantially more than the advertised limit before rejection. This is distinct
from missing model digests (#22) and updater integrity (#51).

Acceptance: stop reception at a bounded limit, including unknown-length and
compressed responses; test with a local disposable server and measure received
bytes/memory. No large response or memory-exhaustion experiment ran in this pass.

## Validation record

A standalone scratch Qt Core/Network probe compiled the production
WebFetchTool, its generated moc, HtmlToMarkdown and ToolOutputTruncator sources
with clang++ C++20. Temporary probe: `/private/tmp/sentinel-gap-probe/probe.cpp`;
all writable test data lived in QTemporaryDir. No model, network endpoint,
credential, user profile or application settings were used.

Observed output:

```text
404 success: true
302 success: true
collision overwrote first: true
collision observed: true
unwritten path advertised: true
```

This establishes class-level defects, not end-to-end acceptance. The scratch
probe compiled with one ignored-nodiscard warning in its fixture file creation;
production files were unmodified.

## Avoided duplicate or inflated findings

McpOAuthService fabricates token-shaped UUID values, and SessionExportService
import validates metadata without importing messages. No production call sites
were found in this pass; they are dormant/helper-contract candidates, not
established active authentication or import vulnerabilities. Review under #65
before opening independent user-impact issues.

MCP remote response paths inspected here have readyRead abort checks in addition
to final parsing checks. A post-read limit alone was not reported as an MCP bug.
This does not certify the whole MCP implementation.

## Remaining discovery sequence

Complete discovery before switching to issue-status triage or fixes. Each pass
should cover one real flow and report new findings, extensions, deliberate
unsupported behavior, and unverified scenarios separately.

1. Tool execution and external data: remaining HTTP/search, process output,
   filesystem boundaries, cancellation, deadlines and temporary evidence.
2. MCP/plugins/skills: connection, discovery, execution, credentials, reconnect,
   cancellation, server exit and trust boundaries.
3. Persistence: migrations, profile/workspace/session switching, retention,
   exports, malformed input, disk errors and interrupted writes.
4. Chat/Agent/provider lifecycle: stream interruption, late callbacks, model
   changes, concurrent clients and outcome truthfulness.
5. Desktop user journeys: startup/onboarding, model setup, chat, approvals,
   attachments and settings; loading/empty/error, focus and responsive states.
6. Native/distribution: daemon lifecycle, install/upgrade/uninstall, offline
   behavior, platform integration and recovery; record unavailable OS cells.

For every new candidate record source and candidate SHA, reachability, expected
versus observed behavior, reproduction level, impact, existing-issue overlap and
acceptance criteria. A finite issue list cannot establish whole-product coverage.
