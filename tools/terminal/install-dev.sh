#!/bin/sh
# Install the canonical thin terminal client without changing shell configuration.
set -eu
sentinel_repo_root=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
install_root=${CARGO_INSTALL_ROOT:-${CARGO_HOME:-$HOME/.cargo}}
explicit_root=false
expect_root=false
for argument do
    if [ "$expect_root" = true ]; then
        install_root=$argument
        expect_root=false
    else
        case $argument in
            --root|--root=*)
                if [ "$explicit_root" = true ]; then
                    printf '%s\n' 'Specify --root only once.' >&2
                    exit 2
                fi
                explicit_root=true
                case $argument in
                    --root) expect_root=true ;;
                    --root=*) install_root=${argument#--root=} ;;
                esac ;;
            --)
                printf '%s\n' 'The install helper does not support an argument separator (--).' >&2
                exit 2 ;;
        esac
    fi
done
if [ "$expect_root" = true ] || [ -z "$install_root" ]; then
    printf '%s\n' 'An install root must be provided.' >&2
    exit 2
fi
case $install_root in
    /*) ;;
    *) install_root=$PWD/$install_root ;;
esac
if [ "$explicit_root" = false ]; then
    # An explicit root keeps Cargo configuration and asset placement in agreement.
    set -- "$@" --root "$install_root"
fi
printf '%s\n' "Installing Sentinel in: $install_root"
cargo install --path "$sentinel_repo_root/cli/crates/sentinel-cli" --locked "$@"
asset_link=$install_root/share/sentinel/core-warden
mkdir -p "$(dirname "$asset_link")"
if [ -d "$asset_link" ]; then
    : # Preserve existing assets and valid directory links.
elif [ -e "$asset_link" ] || [ -L "$asset_link" ]; then
    printf '%s\n' "Asset location is not a directory: $asset_link" >&2
    exit 1
else
    ln -s "$sentinel_repo_root/assets/core-warden" "$asset_link"
fi
installed_binary=$install_root/bin/sentinel
if [ ! -x "$installed_binary" ] && [ -x "$installed_binary.exe" ]; then
    installed_binary=$installed_binary.exe
fi
printf '%s\n' "$installed_binary"
"$installed_binary" --version
