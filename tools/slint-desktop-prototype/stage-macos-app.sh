#!/bin/sh
# Wrap an existing local prototype build for disposable LaunchServices testing.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
if [ "$(uname -s)" != Darwin ] || [ "$#" -ne 2 ]; then
    printf '%s\n' 'Usage (macOS): stage-macos-app.sh debug|release /absolute/new/QuickPanel.app' >&2
    exit 2
fi
profile=$1
app=$2
case $profile in debug|release) ;; *) printf '%s\n' 'Choose debug or release.' >&2; exit 2 ;; esac
case $app in /*.app) ;; *) printf '%s\n' 'Destination must be an absolute new .app path.' >&2; exit 2 ;; esac
if [ -e "$app" ] || [ -L "$app" ]; then
    printf '%s\n' 'Destination exists; choose a new path.' >&2
    exit 2
fi
binary="$root/target/$profile/sentinel-slint-prototype"
if [ ! -f "$binary" ] || [ ! -x "$binary" ]; then
    printf '%s\n' 'Build the selected prototype profile first.' >&2
    exit 1
fi
umask 077
mkdir "$app"
mkdir -p "$app/Contents/MacOS"
cp "$binary" "$app/Contents/MacOS/sentinel-slint-prototype"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>org.sentinel.development.quickpanel</string>
<key>CFBundleName</key><string>Sentinel Quick Panel Prototype</string>
<key>CFBundleExecutable</key><string>sentinel-slint-prototype</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleVersion</key><string>1</string>
<key>CFBundleShortVersionString</key><string>0.1.0</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
printf '%s\n' "$app"
