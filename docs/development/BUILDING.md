# Building

Sentinel requires CMake 3.27+ (presets), Ninja, a C++20 compiler, and Qt 6.5+ with Core, Gui, Quick, Qml, Sql, Test, Network, Widgets, LinguistTools, Multimedia, and Concurrent. CI currently installs Qt 6.11 on Linux, macOS, and Windows.

Linux Desktop also requires Qt DBus. Global Quick Panel shortcuts require an available
`org.freedesktop.portal.GlobalShortcuts` session-bus backend (version 1 or newer) and a
portal-confirmed binding. The portal owns consent and shortcut assignment. Missing runtime
support is reported as unavailable. Portal contract tests start an isolated `dbus-daemon`;
they do not connect to the user's desktop portal. On macOS these tests are enabled when
the optional Qt DBus module is installed. See the [P6-04 result](../audits/P6_04_KDE_WAYLAND_NATIVE_INTEGRATION_RESULT.md)
for native verification limits.

```bash
cmake --preset debug
cmake --build --preset debug
./build/debug/apps/sentinel-desktop/sentinel-desktop
```

On macOS, launch the bundle with `open build/debug/apps/sentinel-desktop/sentinel-desktop.app`.

If your shell places ccache compiler wrappers on `PATH`, `SENTINEL_USE_CCACHE=OFF`
only disables Sentinel's automatic compiler launcher. To bypass the shell wrappers
as well, use `CCACHE_DISABLE=1 cmake --fresh --preset debug -DSENTINEL_USE_CCACHE=OFF`
and `CCACHE_DISABLE=1 cmake --build --preset debug -j6`, or use the `no-ccache`
preset for both commands.

Explicit `CMAKE_C_COMPILER_LAUNCHER` and `CMAKE_CXX_COMPILER_LAUNCHER` values
(including an empty value) take precedence over automatic detection. Sentinel
records the launchers it assigns, updates them when switching cache tools, and
removes only those assignments when disabled or unavailable. Build trees from
before this ownership tracking cannot distinguish an old automatic assignment
from a user override. To discard those legacy cached assignments explicitly,
configure once with `-UCMAKE_C_COMPILER_LAUNCHER -UCMAKE_CXX_COMPILER_LAUNCHER`,
adding `-DSENTINEL_USE_CCACHE=OFF` if automatic detection should stay disabled.

Linguist's `unfinished` and `untranslated` counts describe incomplete translation
catalogs; missing translations fall back to source text. They are not compiler
errors. Unsigned/ad-hoc signing messages are expected for local macOS builds
without a configured signing identity.

New profiles use llama.cpp by default. Models → Runtime setup can install `llama-server` after an explicit user action: Homebrew on supported macOS setups, otherwise a source build requiring Git, CMake, and a C++ compiler. Source builds stay under the application's data directory. Existing provider preferences are preserved.

The Models view pages the currently loaded, filtered model families in slices of 40 cards. Hugging Face discovery uses separate, explicit previous/next catalog batches; fetching a remote batch is different from changing the local card page. Counts and sorting describe the loaded catalog, not the entire upstream corpus. Repository details, `config.json`, and model cards supply artifact sizes and published metadata. Fields the publisher does not supply can remain unavailable. GGUF artifacts can be registered with llama.cpp; downloading a safetensors, PyTorch, or ONNX file does not make that model runnable by llama.cpp or assemble a complete multi-file repository. Gated repositories require an authorized Hugging Face token.

Chat attachments accept up to four files, each up to 4 MB. Qt PDF is optional; install its development module for PDF text extraction. DOCX text extraction requires `unzip` on PATH. Scanned PDFs require OCR before attaching. Images require a vision-capable model.

Voice & Audio settings select Local Whisper or system dictation. Local Whisper uses the daemon STT runtime and desktop microphone capture. Native system dictation is available on macOS when on-device recognition supports the selected language and microphone/speech permissions are granted. Other platforms can use their desktop dictation input or Local Whisper.

Use `cmake --preset tests`, `release`, `relwithdebinfo`, `package-ready`, `asan`, or `coverage` as needed. The `windows-arm64` preset is experimental. Fedora KDE Plasma is the primary desktop target; do not put Linux-only assumptions in core code.

The CMake tree builds `sentinel_core`, plugin SDK/samples, desktop, CLI, daemon, plugin host, and tests when `SENTINEL_BUILD_TESTS` is enabled. Packaging configuration is in `cmake/SentinelCPack.cmake`.

## macOS scoped clang-tidy without production PCH

Apple compiler PCH cannot be consumed by a differently versioned Homebrew clang-tidy. Keep the production compiler/PCH unchanged and generate an analysis-only database:

```bash
python3 tools/analysis/compile_database.py build/tests/compile_commands.json /tmp/sentinel-cleanup-analysis
clang-tidy -p /tmp/sentinel-cleanup-analysis apps/sentinel-desktop/src/DesktopRuntimeClient.cpp
```

The helper strips only CMake PCH flags, preserves other compile arguments and resolves macOS SDK/libc++ paths through xcrun. See the deferred cleanup report for the actually completed five-source scope and narrow QObject ownership analyzer exception. This supersedes the previous PCH blocker for that scope, not whole-repository analysis certification.
