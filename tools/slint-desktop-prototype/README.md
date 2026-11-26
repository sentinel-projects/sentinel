# Sentinel Slint desktop prototype

This developer experiment compares a Slint/Winit desktop client with the current Qt desktop. It uses the existing daemon and generated IPC contract. See [ADR-0005](../../docs/decisions/ADR-0005-rust-desktop-runtime-migration.md) for the migration plan.


On Windows, pass `--socket` with the development daemon's named pipe and
`--panel --global-shortcut` to register Ctrl+Alt+S. Add `--start-hidden` to
show the panel on the first shortcut. Closing the panel hides it in shortcut
mode. Ctrl+Shift+Q exits and releases the shortcut. The panel requests foreground activation and flashes the taskbar when Windows declines. The notification-area icon toggles the window and offers a Quit action.
The app registers the icon again after Explorer restarts. Native Windows
acceptance is pending.

On Linux X11, `--panel --global-shortcut` registers Ctrl+Alt+S with the
X server. `--start-hidden` is supported. The panel requests activation
through the window manager. A conflicting grab or missing keyboard mapping
reports an error. Wayland uses the desktop GlobalShortcuts portal instead. The portal must
confirm the Sentinel binding. Its configured shortcut toggles the panel.
If the portal includes an XDG activation token, Slint submits it for the panel
surface. The compositor decides whether to raise the window. A desktop with a
KDE-compatible StatusNotifier watcher can also show and hide the panel using
the tray icon. Tray menus and native acceptance remain open.

F5 reconnects when disconnected. F6 focuses the composer on Chat, Agent or
Quick Panel.

## Run on macOS or Linux

Use Rust 1.100, the toolchain used to lock this standalone workspace. Slint 1.19 requires at least Rust 1.95. This experiment leaves the CLI and TUI workspace on its current toolchain policy. Start a development C++ daemon with a disposable profile and a socket in an owner-only directory. Configure a model or local streaming fixture in that profile, then run:

```sh
cargo run --manifest-path tools/slint-desktop-prototype/Cargo.toml --locked --release -- --socket /path/to/isolated/daemon.sock
```

For an opt-in macOS Quick Panel, run the built prototype with `--panel
--global-shortcut`. It registers Ctrl+Option+Space exclusively and creates a
status-menu item with Show / Hide and Quit. A shortcut conflict fails startup;
the prototype does not replace another application's binding. Closing the
window hides it while the shortcut and status menu remain active. Select
Quit in the status menu to exit. Add `--start-hidden` to start in the background.
The shortcut opens Quick Panel even when the full desktop was previously active.
On Linux, the staged Rust launcher also accepts `--panel --global-shortcut` and optional `--start-hidden`. On Windows, pass those flags directly to the Slint executable.

For disposable LaunchServices testing, build the desired profile first and wrap
it in a new local unsigned bundle:

```sh
tools/slint-desktop-prototype/stage-macos-app.sh debug /tmp/SentinelQuickPanel.app
open -n /tmp/SentinelQuickPanel.app --args --socket /path/to/isolated/daemon.sock --panel --global-shortcut
```

The bundle helper does not install, sign or publish the application; the socket
must still be supplied explicitly. Native shortcut dispatch, menu interaction,
focus and accessibility require separate GUI validation.

The default window is the full desktop shell with Chat, Models, Agent, Memory, Settings, and, when advertised, Extensions pages. `--panel` starts with the compact Quick Panel layout. The Quick Panel and Desktop buttons switch within one window, retaining the same session, draft, daemon connection and in-flight run. Each layout remembers its window size for that process. Enter sends a Chat message; the first send creates a session before requesting a reply. The Chat page can rename a session, permanently delete it after typing `DELETE`, edit or regenerate the latest user turn, and retry a failed latest answer when the daemon advertises those operations. Stop requests cancellation, and the daemon's terminal event determines when the run has stopped. Saved sessions can be selected and resumed; Refresh reloads the list and New starts a fresh session. The panel displays the first 128 sessions and a transcript capped at 64 KiB of UTF-8; the daemon owns persisted history. Live text remains visible until the terminal snapshot and saved message are confirmed; if confirmation fails, the panel reports it. Switching sessions is disabled while a run is active.

Models shows the daemon's current binding and available catalog. For a configured Ollama provider, it also exposes pull, status, cancel-local-wait, and model removal; removal requires typing the exact model ID. Cancelling only stops Sentinel's wait and may not stop Ollama, so refresh the state and catalog before retrying. Settings loads and saves the profile theme. Memory exposes manual session-scoped list/read/create/correct/delete with revision checks; notes are never inserted into model context automatically. The Extensions page appears only when `plugin-catalog` is advertised and shows bounded metadata from an explicitly configured plugin root. It is read-only: the page cannot load, enable, or start extensions or tools. Agent controls appear only for advertised capabilities. The process task selector is limited to the daemon's `wc`, `head`, `tail`, and `grep` allowlist, passes explicit argv without a shell, and always requires per-invocation approval. Agent history shows bounded persisted run, tool, authorization, and evidence details only for sessions attached by this client.

No production automation or routine configuration API is currently available, so the prototype does not expose routine controls. This Slint slice exposes neither microphone capture nor WAV-file transcription controls. The CLI can send an explicitly named audio file through the daemon's bounded PCM IPC when a speech runtime is actually available.

The prototype uses the existing Unix IPC client. Windows named-pipe transport, global shortcut and tray integration, and native visual/accessibility verification remain open acceptance work. Audit and session lists are bounded and paginated where the daemon supports cursors. Idle transport errors are surfaced at the next operation or explicit reconnect. This lets the worker wait without polling between its own runs.

## Checks

```sh
cargo fmt --manifest-path tools/slint-desktop-prototype/Cargo.toml --check
cargo clippy --manifest-path tools/slint-desktop-prototype/Cargo.toml --locked --all-targets -- -D warnings
cargo test --manifest-path tools/slint-desktop-prototype/Cargo.toml --locked
```

For comparable performance data, use Release builds, the same daemon and recorded stream, and collect native window/CPU/memory measurements on each supported system. Compare both `SLINT_BACKEND=winit-femtovg` and `SLINT_BACKEND=winit-software` where supported before choosing a renderer.

## Initial evidence · 2026-11-16

Base revision: `b66f3431`. The Brain CLI was unavailable, so the experiment used the explicitly supplied Sentinel checkout.

On macOS arm64, the Release prototype used the production C++ daemon with a temporary profile and a deterministic loopback streaming provider. A native window opened with composer focus. Enter completed a chat, Stop cancelled a second run during visible streaming, and Reconnect recovered that terminal state. Text selection and a narrower window worked, and a third chat completed. Read-only inspection of the temporary SQLite profile found two completed assistant messages and one cancelled assistant message. The platform accessibility tree exposed the response, composer, status and controls. A screen reader and global shortcut were outside this run.

One five-second idle sample after streaming, with the panel visible, measured 52.9 MiB median physical footprint and 0.67% of one CPU core for the prototype process. The C++ daemon measured 9.9 MiB and 0.25% of one core in the same interval. This exploratory sample has no equivalent Qt desktop measurement. Hidden-panel consumption, activation latency and frame times remain open.

The two prototype regression tests, Clippy, 27 shared Rust IPC tests and both IPC generator checks passed. The Qt daemon IPC, Desktop IPC and generated-contract CTest targets passed. The existing Qt Desktop end-to-end test timed out at its cancellation setup in the first run and passed when run alone. Windows named-pipe and Linux Wayland/X11 native verification remain open.


## Exploratory macOS initial-window comparison

`benchmark-macos.py` runs three alternating initial-window trials per client on a single disposable C++ daemon with a loopback fixture configured. It records process counters via the existing Darwin sampler and checks on-screen window bounds with CoreGraphics. It does not send a chat. Five seconds after launch it collects 21 samples at 250 ms intervals. Renderer choice is explicit. The actual user home must be sandbox-denied: Qt on macOS does not reliably use the `HOME` environment variable for standard paths, and its startup migration precedes the preferences override. The script refuses to launch unless the real Library, `.sentinel`, and Documents paths are denied. The sandbox below denies the entire home, with read-only access to this checkout and a write exception for its evidence directory. Each client has its own temporary directory and preferences; the Qt process is passed `--no-daemon-start` and the explicit fixture socket. Run only after building both Release clients and the daemon, and with an otherwise idle machine:

```sh
mkdir -p /tmp/sentinel-bench-swift-cache build/slint-phase1
swiftc -module-cache-path /tmp/sentinel-bench-swift-cache tools/slint-desktop-prototype/macos-window-inventory.swift -o /tmp/sentinel-bench-windows
sandbox-exec -p "(version 1) (allow default) (deny file-read* file-write* (subpath \"$HOME\")) (allow file-read* (subpath \"$PWD\")) (allow file-write* (subpath \"$PWD/build/slint-phase1\"))" python3 tools/slint-desktop-prototype/benchmark-macos.py /tmp/sentinel-bench-windows
```

The receipt is written to a new file in ignored `build/slint-phase1`. Add `--smoke` for one short trial per mode to verify the harness; smoke receipts are not performance evidence. Sandbox execution may require system authorization. The helper only confirms on-screen geometry; it does not identify the screen contents, input focus, or actual rendering. An accessible on-screen window alone does not establish chat readiness. No model event sequence, activation timing, hidden-panel measurement, scrolling frame time, energy, or GPU measurement is included. Failure or a missing native platform leaves that cell unverified.

On macOS arm64, 2026-11-16, this initial-window experiment observed one window per trial. Median physical footprint across the three trial medians was 287.58 MiB for Qt (1320 × 892), 55.50 MiB for Slint femtovg and 35.13 MiB for Slint software (both 580 × 442). The interval CPU percentages of one logical core were 13.51–14.73%, 0.81–0.89%, and 2.91–3.28% respectively. Qt screen contents and state were not independently observed; the window dimensions and feature sets differ, and no chat was exercised. These are exploratory process observations, not a matched full-desktop workload, toolkit attribution, responsiveness comparison, or ADR acceptance. Raw samples and binary hashes are in `build/slint-phase1/macos-comparison.json` on the measurement machine. That original run used a sandbox denying the real Library and an isolated temporary HOME; the saved harness subsequently changed to deny the whole home without changing HOME. Its separate short smoke run is not a replacement for the original receipt.


### Connected Quick Panel observation

Add `--panel` to the command above to run an opt-in Release `settings_ui_certification_probe --quick-panel-observation` against the same disposable daemon as both Slint renderers. Build that target first. The fixture loads the production QML Main and Quick Panel, hides Main, sizes the panel, and waits for the Qt runtime projection and daemon connection. Slint's opt-in `SENTINEL_BENCHMARK_READY` diagnostic marks its connected view. Both then remain idle without sending a prompt. CoreGraphics must observe a 580 × 442 window in each case. This matches outer window bounds, not content area: Qt is frameless and Slint includes a title bar. No stream, answer, session completion, rendering equivalence, activation latency or frame time is established.

The Qt fixture includes a hidden Main and an in-process test controller and services; it does not measure the production Qt desktop executable. It also requests a full desktop runtime projection, while Slint connects without creating a session. Features, work performed, window layers, and rendering composition differ. These observations cannot attribute the difference to the UI toolkit or satisfy ADR-0005's matched-workload acceptance.

On the same macOS arm64 machine, three alternating connected-idle trials on 2026-11-16 recorded median-of-trial-median physical footprints of 147.80 MiB for the Qt fixture, 55.06 MiB for Slint femtovg and 34.63 MiB for Slint software. Whole-interval CPU ranged from 11.36–12.62%, 0.69–0.91% and 2.37–2.83% of one core, respectively. The original samples, windows and binary hashes are in ignored `build/slint-phase1/macos-panel-bfm9pzzc.json` on the measurement machine. Each trial waited for its connected marker, then warmed up for five seconds and sampled over five seconds. Linux Wayland/X11 and Windows runs are not available on this host; the current harness and sampler support macOS only. The cited benchmark used the macOS transport.

### Isolated desktop evaluation stage

The manually dispatched [Qt-free Rust desktop release candidate](../../.github/workflows/rust-release-candidate.yml) workflow creates Linux x86_64 and macOS arm64 stages as unsigned CI artifacts. The production release workflow remains separate.

`tools/slint-desktop-prototype/stage-desktop.sh NATIVE_TARGET_TRIPLE /absolute/new/stage` builds a private local desktop evaluation stage for a supported native macOS or Linux host. It places the launcher and Slint binary under `bin/` with their own `desktop-manifest.json`, and keeps the existing CLI/daemon stage and its unchanged verifier contract under `terminal/`. Run `python3 tools/slint-desktop-prototype/verify-desktop-stage.py /absolute/new/stage` to check the four executable hashes, expected application version, required files, bounded link-free asset tree, and consistent manifest identities without running an artifact. Pass `--cli-lockfile cli/Cargo.lock --desktop-lockfile tools/slint-desktop-prototype/Cargo.lock` to also compare lockfile hashes; `--expected-version` overrides this checkout’s canonical version. The terminal CLI can separately verify `terminal/` with `sentinel update verify-stage` on a matching target.

On a native Windows MSVC host, run
`tools/slint-desktop-prototype/stage-desktop.ps1 x86_64-pc-windows-msvc C:\path\to\new-stage`.
The Windows stage contains the Slint executable and nested CLI/daemon stage.
Start the isolated daemon separately, then connect Slint to its named pipe
with `--socket`. The same Python verifier checks PE architecture, regular
and delay-load Qt imports, and manifest hashes. The manual candidate workflow
includes Windows x64 alongside Linux and macOS.

Staging inspects all four executables with `readelf` on Linux and `otool` on macOS. It fails before writing the desktop manifest if native inspection fails or an executable declares a direct Qt library or framework dependency. This is a static check of declared native imports. Native operation, dynamically loaded libraries, and transitive dependencies require platform acceptance.

These are local unsigned development artifacts. Hashes do not establish source provenance or publisher authenticity; copied assets are not verified, no model is bundled, and no installer, production service, profile migration, or platform acceptance is implied. The launcher requires explicit local configuration; by default it creates and retains a private temporary profile, or it can use an explicitly supplied isolated profile. Pass `--persistent-profile` to retain data under `$HOME/.sentinel-rust` on Linux/macOS. Consult its `--help` before use. The staging helper refuses existing output and non-native target triples; a failed build or copy can leave an incomplete private output that must be discarded.

Invoke the staged launcher with an existing local loopback provider and model. It locates both executable siblings relative to the stage, regardless of the working directory. To override those locations, provide both absolute `--daemon` and `--desktop` paths:

```sh
STAGE=/absolute/path/to/stage
"$STAGE/bin/sentinel-dev-launch" --persistent-profile --provider http://127.0.0.1:PORT --model MODEL_ID
```

Replace the placeholders with your explicitly configured local service and installed model. The launcher reports the retained profile path; it does not download or start a model. For an already-running persistent Rust daemon, connect directly with `sentinel-slint-prototype --persistent-profile`.

For a stage with a separately supplied detached Ed25519 signature, verify the v2 desktop manifest and all four executable hashes with:

```sh
python3 tools/slint-desktop-prototype/verify-desktop-stage.py /absolute/path/to/stage \
  --trusted-key /absolute/path/to/pinned-public-key.der \
  --signature /absolute/path/to/desktop-manifest.sig \
  --openssl /absolute/path/to/trusted/openssl
```

The pinned key must be an Ed25519 DER SubjectPublicKeyInfo (44 bytes), and the signature must be the 64-byte detached signature over the exact `desktop-manifest.json` bytes. The v2 manifest binds the nested terminal manifest, which in turn binds its two executables; the signed desktop manifest also binds the launcher and Slint executable. OpenSSL 3.x or later is required. Supply the public key and OpenSSL executable through a trusted channel: this command does not establish their trust or generate keys, sign, download, install, or publish anything. Unsigned verification remains available and explicitly reports integrity only. Asset contents, provenance beyond the supplied key, dependency licensing, and native platform acceptance remain outside this signature's coverage.
