# Sentinel CLI

The canonical executable is `sentinel`. Rust is a thin client over versioned local IPC; the C++ daemon owns providers, model bindings, workspaces, conversations, permissions, tools and AgentRuntime. No daemon, provider or model is started or selected as a fallback.

## Build and install

From a source checkout with the repository's supported Rust toolchain:

```sh
cargo build --release --workspace --manifest-path cli/Cargo.toml
./tools/terminal/install-dev.sh
# Binary-only Cargo alternative:
cargo install --path cli/crates/sentinel-cli --locked
command -v sentinel
sentinel --version
sentinel --help
```

For a custom destination, use `./tools/terminal/install-dev.sh --root /path/to/root`.
The helper selects `--root`, then `CARGO_INSTALL_ROOT`, then `CARGO_HOME`, then
`$HOME/.cargo`. It passes that root explicitly to Cargo, so Cargo's configured
`install.root` does not override it. Assets are linked into
`ROOT/share/sentinel/core-warden`; keep the checkout available for that development
link. Existing asset directories are preserved. Add `ROOT/bin` to PATH manually.

Cargo installs `sentinel` under its install root's `bin` directory, normally `$HOME/.cargo/bin`. Put that directory on PATH manually if necessary. No shell configuration is edited. Reinstall after changing the checkout. Uninstall with `cargo uninstall sentinel-cli` (use the same `--root` as installation, if customized).

Release source installations use the same Cargo path command on the chosen release checkout. There is currently no claim that `cargo install sentinel-cli` from crates.io or a prebuilt release package is available. The IPC build script obtains the version from the source checkout's canonical CMake version. Packaging/updating remains outside this phase.

Build the C++ daemon as described in [Building](../development/BUILDING.md). Start `sentinel-daemon` externally. After updating this checkout, rebuild and restart the daemon before using new terminal operations; an already-running older daemon is not replaced automatically. Coordinate the socket and profile; do not start multiple daemons against one profile. Clients support `--socket PATH`; the Unix default is `~/.sentinel/run/daemon.sock`. Windows named-pipe transport is not implemented in the Rust client.

## Commands

All major commands have offline `--help`. Omitting a command retains the historical `status` behavior.

| Command | Behavior |
| --- | --- |
| `status` | Connection, daemon version, IPC version, uptime, endpoint, generation and active run/session counts |
| `models` | ModelService's discovered model catalog; an empty catalog is legitimate |
| `model current` | Current global selection; workspace overrides are shown in doctor |
| `model select PROVIDER MODEL` | Explicit discovered-model selection; rejected during active work |
| `provider list` | Provider readiness, selected workspace and safe service diagnostics |
| `provider select PROVIDER` | Explicit provider selection; remembered selection, if any, is returned by daemon |
| `provider refresh` | Refresh discovery for the selected provider; discovery is asynchronous |
| `provider endpoint ollama\|lm-studio\|llama-cpp-server URL` | Configure a local provider endpoint through daemon settings |
| `workspace list` | Workspace catalog and current selection |
| `workspace create NAME --template Coding` | Create a custom workspace through WorkspaceService |
| `workspace root ID PATH` | Attach an existing readable directory to a custom workspace; built-in roots are immutable |
| `workspace select ID` | Select a known non-archived workspace; rejected during active work |
| `sessions` | Daemon-owned session titles, identifiers, mode, state and binding metadata |
| `chat [TEXT…]` | Chat Mode only |
| `agent [TEXT…]`, `run [TEXT…]` | AgentRuntime execution only |
| `chat\|agent\|run --session ID [TEXT…]` | Continue an existing conversation |
| `attach ID`, `tui --session ID`, `tui` | Interactive terminal interface |
| `changes ID` | Bounded, read-only change review for a session's most recent in-memory Agent baseline |
| `doctor` | Connection/protocol, installation PATH, model/provider, workspace, registry, MCP, permissions and network diagnostics |
| `completions zsh\|bash\|fish` | Write shell completion source to stdout |
| `shutdown` | Request daemon shutdown explicitly |

For a new local provider, configure its endpoint, explicitly select the provider, refresh discovery, inspect `models`, then explicitly select the model. Cloud credential configuration remains in the existing Sentinel settings surface. Secrets are not accepted or printed by terminal diagnostics.

## Output and pipelines

Global `--output text|json|stream-json` works before or after the subcommand and task. Text is the default. Human diagnostics go to stderr. `NO_COLOR`/non-terminal compatibility is respected by parser output; command text uses no ANSI palette.

```sh
sentinel status --output json
sentinel models --output json
printf '%s\n' 'inspect this workspace' | sentinel run --output stream-json
cat task.txt | sentinel agent --output json
sentinel chat --session SESSION_ID 'continue the conversation'
```

Positional text takes precedence over stdin; the two are never concatenated. If neither is supplied, usage fails before connecting. Input must contain 1–65536 UTF-8 bytes. Use `--` before task text that begins with option syntax. TUI/attach require text output.

JSON query success is one IPC payload document. JSON run success is one terminal payload document, including `run_id`, `session_id`, `state` and `text`. Errors are one object with `schema_version:1`, `type:error`, `error.code`, `error.message`, and `exit_code`. Parser errors in an explicitly requested machine format use the same error document. JSON schemas preserve the existing successful payload shapes; additive protocol metadata is versioned in `protocol/ipc-v1.json`.

Stream JSON is newline-delimited. Run events use the canonical IPC envelope (`version`, `type`, `id`, `name`, `payload`), with run/session identity and sequence/generation where supplied. Query results use `{schema_version:1,type:result,name,payload}`. A failed/cancelled run may emit a terminal event followed by a typed error object. No prose is mixed into stdout. Event sequences and types are deterministic contracts; UUIDs/timestamps are not fixed across invocations.

CLI approvals are interactive on terminal stdin only: `y` allows once, everything else denies. Piped/non-interactive invocations deny automatically, without a grant. Ctrl+C requests authoritative cancellation and waits for a terminal event. Losing the connection does not cancel execution; use `sessions` and `attach` to recover.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Success / help / version |
| 1 | Task failed or other internal/remote failure |
| 2 | CLI usage/input error |
| 3 | Daemon unavailable, disconnected or response timeout |
| 4 | Protocol data/version mismatch |
| 5 | Provider/model/authentication unavailable |
| 6 | Permission denied |
| 7 | Runtime rejected, unavailable or busy |
| 130 | Authoritatively cancelled |

Cancellation previously returned 2; it now returns 130 so scripts can distinguish cancellation from usage errors. `run` remains an alias for `agent`; positional argument words remain supported. No natural-language-to-shell dispatch is added. TUI migration: Enter now sends and Ctrl+O inserts a newline; the old `/agent task` composer syntax is rejected. Select Agent mode explicitly, then send the task.

## Completions

```sh
sentinel completions zsh > _sentinel
sentinel completions bash > sentinel.bash
sentinel completions fish > sentinel.fish
```

For zsh, place `_sentinel` in a directory on `fpath` before `compinit`. For bash, source `sentinel.bash`. For fish, place `sentinel.fish` in the user's fish completions directory. Install/configure these files manually.

Change review observes authorized text state since a run began, including possible concurrent external edits. It labels changes **Applied**, does not attribute every edit to Agent, does not offer accept/revert, and explicitly reports truncation. Baselines are bounded (8 sessions, 32 files, 8192 characters per file, 32768 characters per baseline), exclude hidden/denied/binary/oversized resources, and do not survive daemon restart. Deleted/unreadable resources are omitted rather than inferred. Diffs use a valid whole-file replacement hunk, not a minimal Git diff.

## TUI V3 workflow consistency

`sentinel tui` and existing attach/model/workspace/session CLI commands retain their flags and script behavior. TUI slash commands reuse existing IPC semantics; they are not new shell subcommands. `/status` uses daemon status, `/doctor` inspects safe terminal diagnostics, `/model` selects an accepted daemon binding, and `/resume` attaches an existing conversation. See [TUI commands](TUI.md) and the [complete registry](../development/TUI_V3_COMMAND_SPEC.md). Unsupported Plan/rollback/compaction commands do not imply corresponding CLI functionality.

V3.1 adds the TUI-only `/theme` command; it does not add a CLI subcommand or change existing flags. `SENTINEL_TUI_THEME` accepts terminal, obsidian, glacier and porcelain (dark/light aliases retained); `NO_COLOR` overrides color. See [live acceptance](../development/TUI_V3_1_LIVE_ACCEPTANCE.md).
