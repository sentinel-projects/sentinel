# CLI and terminal UI

The canonical Rust executable, `sentinel`, connects to `sentinel-daemon` over [typed local IPC](IPC.md). It never links the C++ core or starts a hidden daemon. Rust **1.94** is the declared minimum; the Cargo workspace is `cli/`.

For installation, commands, stable output/exit contracts, provider/workspace selection and automation, see the [CLI user guide](../user/CLI.md). For composer keys, pickers, approvals, cancellation, reconnect, file references and Applied change review, see the [TUI user guide](../user/TUI.md).

```sh
./tools/terminal/install-dev.sh
sentinel --help
sentinel status --output json
sentinel tui
```

CLI and TUI retain daemon-owned conversations, immutable model bindings, permissions and AgentRuntime execution. Chat and Agent are explicit modes. Enter sends; Ctrl+O inserts a composer newline. Ctrl+C requests cancellation during active work and preserves an idle draft, exiting only when empty. Esc preserves a pending approval. Unsupported compaction/revert operations are not simulated.

Build the normal Rust CLI without building C++/Qt:

```sh
cargo build --manifest-path cli/Cargo.toml --locked --release -p sentinel-cli
./cli/target/release/sentinel --help
```

The CLI remains a client of an external daemon. The production daemon is still
the C++/Qt `sentinel-daemon`; the Rust `sentinel-daemon-dev` is an isolated
development runtime with distinct profile ownership and SQLite schemas. Do not
point it at a Qt profile. Standalone Slint and local Rust development commands
are in the [build guide](../development/BUILDING.md).

On Linux and macOS, `sentinel --persistent-profile status` selects the
standalone Rust daemon's socket under `$HOME/.sentinel-rust`. Start the daemon
or the staged launcher with `--persistent-profile` first. The client accepts
either that option or an explicit `--socket PATH`.


`sentinel chat --attach PATH 'prompt'` attaches a file from the authorized
workspace. Repeat `--attach` for multiple paths. Paths are relative to the
daemon-selected workspace. The CLI sends the path strings, and the daemon
validates and reads the files before handing their content to the selected
provider. This option requires the `chat-workspace-attachments` capability.

`sentinel voice state` queries daemon speech readiness. `sentinel voice transcribe
FILE [--format auto|wav|pcm]` reads only the explicitly named local file and
sends bounded audio through the daemon's existing voice IPC. Auto is the
default: a RIFF prefix or case-insensitive `.wav` extension selects strict
RIFF/WAVE parsing; otherwise legacy raw PCM behavior is preserved. WAV accepts
only integer PCM, 16 kHz mono, 16-bit little-endian, no more than 60 seconds of
samples and 64 KiB of framing/metadata. Malformed/truncated RIFF, duplicate
format/data chunks, and unsupported formats fail before daemon connection.
`--format pcm` explicitly selects nonempty, even-length raw PCM up to 60 seconds.
No conversion, resampling, microphone capture or VAD is performed. The daemon
must support `voice.action` and `voice.audio` and report
`voice.state.available: true`. The Rust development daemon reports voice
unavailable by default. Its opt-in macOS `--speech-execution` path accepts
bounded supplied PCM only with explicitly inspected Whisper binary/model
artifacts; inspection by itself is not runtime readiness. This development
path has not received native model-compatibility acceptance. The command does
not install or launch a speech runtime.

The terminal uses additive IPC 1.1 operations. `terminal.attach` returns the bounded safe session snapshot, avoiding Desktop's full presentation projection; `session.attach` retains its existing Desktop behavior. `terminal.state` uses cached ModelService readiness without inspecting unobserved cloud credentials. Discovery and runtime preflight remain explicit existing authoritative operations.

## Legacy C++ CLI

`sentinel-cli` remains the existing in-process diagnostic/configuration/model interface for Desktop compatibility. Its commands are documented in the [legacy CLI reference](../reference/CLI_REFERENCE.md). It is separate from the daemon-connected Rust binary. Chat timeout and model-pull failure can still return exit code 0; do not use that code as proof of completion.

Core Warden is a presentation component with terminal graphics/text fallback, not provider health or inference authority. The Rust IPC client has Windows named-pipe code and cross-compiles, but native CLI/TUI connection to the Qt daemon and Windows runtime behavior remain unverified; Windows CLI/TUI support is not certified. The TUI explicitly leaves `/plan`, `/compact`, `/undo`, `/redo`, `/skills`, `/init`, `/grill-me` and `/editor` unavailable; inspect the user guides for supported commands.

## Local release-candidate evaluation

For a Rust development-evaluation stage, build for an explicit target and a new
absolute destination with `tools/terminal/build-stage.sh TARGET /absolute/new/stage`
(PowerShell: `tools/terminal/build-stage.ps1`). Then run
`sentinel update verify-stage /absolute/stage --lockfile cli/Cargo.lock` from a
CLI built for that same target/version. Verification checks the schema, app version,
target triple, exact CLI/daemon artifact set, and their SHA-256 bytes; `--lockfile`
also compares the declared lockfile digest. Its output labels these hashes as byte
integrity only: the manifest has no publisher signature, does not authenticate
provenance, and does not establish asset integrity. The command is offline and never
installs or executes staged files.

Build with `cargo build --release --locked --manifest-path cli/Cargo.toml`, then run
`./tools/terminal/stage-release.sh /absolute/new/stage-directory`. The script only
stages the existing executable, canonical Core Warden runtime animations/contract
and project license; it never installs globally or publishes. Run the staged
`bin/sentinel` from any working directory. Keep `bin/` beside `share/` when moving
it. The daemon remains a separate prerequisite. This is an unsigned local RC
stage, not a macOS installer, notarized package or supported Windows distribution.
The staging copier does not certify binary/asset compatibility. The evaluation
packager refuses the known old V4-B executable with the newer CW03B catalog;
build and validate matching inputs first. See the [SEC-P1 audit](../development/SEC_P1_PUBLIC_REPOSITORY_AUDIT.md).

The TUI resolves `SENTINEL_CORE_WARDEN_ASSETS` first, then executable-relative
`../share/sentinel/core-warden`, then the standard checkout `cli/target/debug|release`
layout. It never uses a compiled developer home path. An invalid explicit override
stays authoritative and selects text fallback. Copying only the binary is valid
for text operation; graphical assets require the adjacent shared directory or an
explicit asset root. Plain `cargo install` installs only the binary; the development
`install-dev.sh` helper additionally links checkout assets beside the selected
installation root (see the CLI user guide). Use an explicit asset root or the staged
layout for an independent graphical installation. No shell configuration
is changed automatically. Use the absolute executable path and `--version` when
checking for older PATH-shadowing copies.


`sentinel agent-history SESSION_ID` reads durable Agent history after attaching
to the specified session. Pass `--run RUN_ID` for one run's step and decision
receipts. To retrieve older runs, pass the prior page's final `cursorTime`
and `runId` as `--before-time` and `--before-id`.


- `sentinel agent-inspect 'inspect notes.txt'` asks the local Agent to propose bounded file reads and request approval for each.
- `sentinel agent-delegate < plan.json` runs an explicit `delegate-read` plan through the same permission gateway and shared run budget.
- `sentinel agent-analyze < notes.txt` submits the supplied text to the Windows text-analysis Agent. The daemon does not receive the file path and cannot read additional files.
