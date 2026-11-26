#!/bin/sh
# Build a private development desktop stage. Never install or publish.
set -eu
sentinel_repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "$#" -ne 2 ]; then
    printf '%s\n' 'Usage: stage-desktop.sh NATIVE_TARGET_TRIPLE /absolute/new/stage' >&2
    exit 2
fi
sentinel_target=$1
sentinel_stage=$2
case $sentinel_target in
    x86_64-unknown-linux-gnu|aarch64-unknown-linux-gnu|x86_64-apple-darwin|aarch64-apple-darwin) ;;
    *) printf '%s\n' 'Unsupported desktop stage target.' >&2; exit 2 ;;
esac
case $sentinel_stage in /*) ;; *) printf '%s\n' 'Stage path must be absolute.' >&2; exit 2 ;; esac
if [ -e "$sentinel_stage" ] || [ -L "$sentinel_stage" ]; then
    printf '%s\n' 'Output already exists; use a new directory.' >&2; exit 2
fi
sentinel_host=$(rustc -vV | sed -n 's/^host: //p')
if [ "$sentinel_target" != "$sentinel_host" ]; then
    printf '%s\n' 'This helper stages native host builds only.' >&2; exit 2
fi
case $sentinel_target in
    *-linux-gnu) sentinel_link_inspector=readelf ;;
    *-apple-darwin) sentinel_link_inspector=otool ;;
esac
if ! command -v "$sentinel_link_inspector" >/dev/null 2>&1; then
    printf '%s\n' "$sentinel_link_inspector is required for native dependency inspection." >&2
    exit 1
fi
if command -v sha256sum >/dev/null 2>&1; then sentinel_hasher=sha256sum
elif command -v shasum >/dev/null 2>&1; then sentinel_hasher=shasum
else printf '%s\n' 'A SHA-256 tool is required.' >&2; exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    printf '%s\n' 'Python 3 is required to verify the completed stage.' >&2; exit 1
fi
hash_file() {
    if [ "$sentinel_hasher" = shasum ]; then shasum -a 256 "$1" | awk '{print $1}'
    else sha256sum "$1" | awk '{print $1}'; fi
}
check_hash() {
    case $1 in *[!0-9a-f]*|'') return 1 ;; esac
    [ "${#1}" -eq 64 ]
}
check_no_qt_links() {
    sentinel_binary_path=$1
    case $sentinel_link_inspector in
        readelf) sentinel_link_report=$(readelf -dW "$sentinel_binary_path") || return 1 ;;
        otool) sentinel_link_report=$(otool -L "$sentinel_binary_path") || return 1 ;;
    esac
    if printf '%s\n' "$sentinel_link_report" | grep -Eq '(NEEDED.*\[(.*/)?libQt[0-9]|^[[:space:]]+[^[:space:]]*(/Qt[^/[:space:]]*\.framework/|/libQt[0-9]))'; then
        printf '%s\n' "Unexpected Qt dependency in $sentinel_binary_path." >&2
        return 1
    fi
}
sentinel_version=$(sed -n 's/^set(SENTINEL_APP_VERSION "\([^"]*\)".*/\1/p' "$sentinel_repo/CMakeLists.txt")
case $sentinel_version in *[!A-Za-z0-9.+-]*|'') printf '%s\n' 'Invalid application version.' >&2; exit 1 ;; esac
sentinel_cli_lock=$(hash_file "$sentinel_repo/cli/Cargo.lock")
sentinel_gui_lock=$(hash_file "$sentinel_repo/tools/slint-desktop-prototype/Cargo.lock")
if ! check_hash "$sentinel_cli_lock" || ! check_hash "$sentinel_gui_lock"; then
    printf '%s\n' 'Lockfile SHA-256 calculation failed.' >&2; exit 1
fi
sentinel_target_dir=${CARGO_TARGET_DIR:-"$sentinel_repo/cli/target"}
case $sentinel_target_dir in /*) ;; *) sentinel_target_dir="$sentinel_repo/$sentinel_target_dir" ;; esac
export CARGO_TARGET_DIR="$sentinel_target_dir"
export SENTINEL_APP_VERSION="$sentinel_version"
cargo build --manifest-path "$sentinel_repo/cli/Cargo.toml" --release --locked --target "$sentinel_target" -p sentinel-cli --bin sentinel-dev-launch
cargo build --manifest-path "$sentinel_repo/tools/slint-desktop-prototype/Cargo.toml" --release --locked --target "$sentinel_target"
sentinel_build="$sentinel_target_dir/$sentinel_target/release"
for sentinel_binary in sentinel-dev-launch sentinel-slint-prototype; do
    if [ ! -f "$sentinel_build/$sentinel_binary" ] || [ -L "$sentinel_build/$sentinel_binary" ]; then
        printf '%s\n' "Missing regular artifact: $sentinel_binary" >&2; exit 1
    fi
done
if [ "$sentinel_cli_lock" != "$(hash_file "$sentinel_repo/cli/Cargo.lock")" ] || [ "$sentinel_gui_lock" != "$(hash_file "$sentinel_repo/tools/slint-desktop-prototype/Cargo.lock")" ]; then
    printf '%s\n' 'A lockfile changed during build; no stage created.' >&2; exit 1
fi
umask 077
mkdir "$sentinel_stage"
# Preserve the established terminal manifest and its existing verifier contract.
"$sentinel_repo/tools/terminal/build-stage.sh" "$sentinel_target" "$sentinel_stage/terminal" >/dev/null
mkdir "$sentinel_stage/bin"
cp "$sentinel_build/sentinel-dev-launch" "$sentinel_stage/bin/"
cp "$sentinel_build/sentinel-slint-prototype" "$sentinel_stage/bin/"
for sentinel_artifact in bin/sentinel-dev-launch bin/sentinel-slint-prototype terminal/bin/sentinel terminal/bin/sentinel-daemon-dev; do
    if ! check_no_qt_links "$sentinel_stage/$sentinel_artifact"; then
        printf '%s\n' 'Native dependency inspection failed. Discard the incomplete stage.' >&2
        exit 1
    fi
done
if [ "$sentinel_cli_lock" != "$(hash_file "$sentinel_repo/cli/Cargo.lock")" ] || [ "$sentinel_gui_lock" != "$(hash_file "$sentinel_repo/tools/slint-desktop-prototype/Cargo.lock")" ]; then
    printf '%s\n' 'A lockfile changed while staging; discard the incomplete output.' >&2; exit 1
fi
sentinel_launcher_hash=$(hash_file "$sentinel_stage/bin/sentinel-dev-launch")
sentinel_gui_hash=$(hash_file "$sentinel_stage/bin/sentinel-slint-prototype")
sentinel_terminal_manifest_hash=$(hash_file "$sentinel_stage/terminal/manifest.json")
if ! check_hash "$sentinel_launcher_hash" || ! check_hash "$sentinel_gui_hash" || ! check_hash "$sentinel_terminal_manifest_hash"; then
    printf '%s\n' 'Artifact SHA-256 calculation failed; discard the incomplete output.' >&2; exit 1
fi
printf '{"schema_version":2,"stage":"development-desktop-evaluation","app_version":"%s","target":"%s","artifacts":[{"path":"bin/sentinel-dev-launch","sha256":"%s"},{"path":"bin/sentinel-slint-prototype","sha256":"%s"}],"cli_lockfile_sha256":"%s","desktop_lockfile_sha256":"%s","terminal_stage":"terminal","terminal_manifest_sha256":"%s","assets_integrity":"not-verified"}\n' "$sentinel_version" "$sentinel_target" "$sentinel_launcher_hash" "$sentinel_gui_hash" "$sentinel_cli_lock" "$sentinel_gui_lock" "$sentinel_terminal_manifest_hash" > "$sentinel_stage/desktop-manifest.json"
printf '%s\n' 'Development evaluation only. Unsigned; not an installer or production package. No model is bundled. Run only with explicit local daemon/provider configuration. Terminal stage integrity is checked separately; asset provenance is not verified.' > "$sentinel_stage/README.txt"
python3 "$sentinel_repo/tools/slint-desktop-prototype/verify-desktop-stage.py" "$sentinel_stage" --expected-version "$sentinel_version" --cli-lockfile "$sentinel_repo/cli/Cargo.lock" --desktop-lockfile "$sentinel_repo/tools/slint-desktop-prototype/Cargo.lock" --runtime-assets-source "$sentinel_repo/assets/core-warden" >&2
printf '%s\n' "$sentinel_stage"
