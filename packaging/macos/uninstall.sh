#!/usr/bin/env bash
set -euo pipefail

# Sentinel macOS Silent Uninstaller & Cleanup Script

echo "=== Sentinel macOS Uninstaller ==="

# 1. Terminate running process
echo "Stopping Sentinel process..."
pkill -x "sentinel-desktop" || true
pkill -x "sentinel-daemon" || true

# 2. Remove App Bundle (current and pre-rename bundle names)
for bundle in "/Applications/Sentinel.app" "/Applications/Sentinel Desktop.app" \
              "${HOME}/Applications/Sentinel.app" "${HOME}/Applications/Sentinel Desktop.app"; do
    if [ -d "${bundle}" ]; then
        echo "Removing ${bundle}..."
        rm -rf "${bundle}"
    fi
done

# 3. Clean Preferences & Application Support
echo "Cleaning application preferences and support files..."
rm -rf "${HOME}/Library/Application Support/Sopwit/Sentinel"
rm -rf "${HOME}/Library/Application Support/Sopwit/Sentinel Desktop"
rm -rf "${HOME}/Library/Logs/Sentinel"
rm -rf "${HOME}/Library/Caches/dev.sentinel.Sentinel"
rm -f "${HOME}/Library/Preferences/dev.sentinel.Sentinel.plist"

# 4. Remove Keychain item (optional cleanup)
security delete-generic-password -s "dev.sentinel.Sentinel" -a "master_encryption_key" || true

echo "✅ Sentinel has been completely uninstalled."
