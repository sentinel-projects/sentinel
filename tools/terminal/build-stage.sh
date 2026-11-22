#!/bin/sh
# Build and stage isolated development artifacts; never install or publish.
set -eu
sentinel_repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "$#" -ne 2 ]; then
    printf '%s\n' 'Usage: build-stage.sh TARGET_TRIPLE /absolute/new/stage' >&2
    exit 2
fi
sentinel_target=$1
sentinel_stage=$2
case $sentinel_target in
    x86_64-unknown-linux-gnu|aarch64-unknown-linux-gnu|x86_64-apple-darwin|aarch64-apple-darwin|x86_64-pc-windows-gnu|x86_64-pc-windows-msvc|aarch64-pc-windows-msvc) ;;
    *) printf '%s\n' 'Unsupported target; see the development staging matrix.' >&2; exit 2 ;;
esac
case $sentinel_stage in /*) ;; *) printf '%s\n' 'Stage path must be absolute.' >&2; exit 2 ;; esac
if [ -e "$sentinel_stage" ] || [ -L "$sentinel_stage" ]; then
    printf '%s\n' 'Output already exists; use a new directory.' >&2
    exit 2
fi
case $sentinel_target in *-windows-*) sentinel_suffix=.exe ;; *) sentinel_suffix= ;; esac
cd "$sentinel_repo"
sentinel_version=$(sed -n 's/^set(SENTINEL_APP_VERSION "\([^"]*\)".*/\1/p' "$sentinel_repo/CMakeLists.txt")
case $sentinel_version in *[!A-Za-z0-9.+-]*|'') printf '%s\n' 'Invalid canonical application version.' >&2; exit 1 ;; esac
export SENTINEL_APP_VERSION="$sentinel_version"
sentinel_target_dir=${CARGO_TARGET_DIR:-"$sentinel_repo/cli/target"}
case $sentinel_target_dir in /*) ;; *) sentinel_target_dir="$sentinel_repo/$sentinel_target_dir" ;; esac
export CARGO_TARGET_DIR="$sentinel_target_dir"
if command -v sha256sum >/dev/null 2>&1; then
    sentinel_hasher=sha256sum
elif command -v shasum >/dev/null 2>&1; then
    sentinel_hasher=shasum
else
    printf '%s\n' 'A SHA-256 tool (sha256sum or shasum) is required.' >&2
    exit 1
fi
hash_file() {
    if [ "$sentinel_hasher" = shasum ]; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        sha256sum "$1" | awk '{print $1}'
    fi
}
check_hash() {
    case $1 in *[!0-9a-f]*|'') return 1 ;; esac
    [ "${#1}" -eq 64 ]
}
sentinel_lock_before=$(hash_file "$sentinel_repo/cli/Cargo.lock")
if ! check_hash "$sentinel_lock_before"; then
    printf '%s\n' 'SHA-256 calculation failed.' >&2
    exit 1
fi
cargo build --manifest-path "$sentinel_repo/cli/Cargo.toml" --release --locked --target "$sentinel_target" -p sentinel-cli -p sentinel-daemon-dev
sentinel_source="$sentinel_target_dir/$sentinel_target/release"
for sentinel_name in "sentinel$sentinel_suffix" "sentinel-daemon-dev$sentinel_suffix"; do
    if [ ! -f "$sentinel_source/$sentinel_name" ]; then
        printf '%s\n' "Missing build artifact: $sentinel_name" >&2
        exit 1
    fi
done
umask 077
mkdir "$sentinel_stage"
mkdir -p "$sentinel_stage/bin" "$sentinel_stage/share/sentinel/core-warden" "$sentinel_stage/share/licenses/sentinel"
cp "$sentinel_source/sentinel$sentinel_suffix" "$sentinel_stage/bin/sentinel$sentinel_suffix"
cp "$sentinel_source/sentinel-daemon-dev$sentinel_suffix" "$sentinel_stage/bin/sentinel-daemon-dev$sentinel_suffix"
cp "$sentinel_repo/assets/core-warden/state-contract.json" "$sentinel_stage/share/sentinel/core-warden/"
cp -R "$sentinel_repo/assets/core-warden/animations" "$sentinel_stage/share/sentinel/core-warden/"
cp "$sentinel_repo/LICENSE" "$sentinel_stage/share/licenses/sentinel/LICENSE"
cp "$sentinel_repo/THIRD_PARTY_NOTICES.md" "$sentinel_stage/share/licenses/sentinel/THIRD_PARTY_NOTICES.md"
printf '%s\n' 'Development evaluation only. No signing, installer, production daemon, Qt profile migration, model or native platform certification is included. Asset integrity is not covered by manifest.json.' > "$sentinel_stage/README.txt"
sentinel_cli_hash=$(hash_file "$sentinel_stage/bin/sentinel$sentinel_suffix")
sentinel_daemon_hash=$(hash_file "$sentinel_stage/bin/sentinel-daemon-dev$sentinel_suffix")
sentinel_lock_hash=$(hash_file "$sentinel_repo/cli/Cargo.lock")
if [ "$sentinel_lock_hash" != "$sentinel_lock_before" ]; then
    printf '%s\n' 'Cargo.lock changed during staging; discard the incomplete output.' >&2
    exit 1
fi
if ! check_hash "$sentinel_cli_hash" || ! check_hash "$sentinel_daemon_hash" || ! check_hash "$sentinel_lock_hash"; then
    printf '%s\n' 'SHA-256 calculation failed.' >&2
    exit 1
fi
printf '{"schema_version":1,"app_version":"%s","target":"%s","stage":"development-evaluation","artifacts":[{"path":"bin/sentinel%s","sha256":"%s"},{"path":"bin/sentinel-daemon-dev%s","sha256":"%s"}],"lockfile_sha256":"%s","assets_integrity":"not-verified"}\n' "$sentinel_version" "$sentinel_target" "$sentinel_suffix" "$sentinel_cli_hash" "$sentinel_suffix" "$sentinel_daemon_hash" "$sentinel_lock_hash" > "$sentinel_stage/manifest.json"
printf '%s\n' "$sentinel_stage"
