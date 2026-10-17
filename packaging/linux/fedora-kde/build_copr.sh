#!/usr/bin/env bash
# ==============================================================================
# Sentinel Fedora COPR Packaging & Deployment Automation
# Target OS: Fedora Linux (KDE Plasma 6)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build-copr"
SPEC_FILE="${REPO_ROOT}/packaging/linux/rpm/sentinel-desktop.spec"
COPR_PROJECT="${COPR_PROJECT:-sentinel/sentinel-desktop}"

echo "==> Initializing Fedora COPR package build for Sentinel..."
echo "    Repo Root : ${REPO_ROOT}"
echo "    Spec File : ${SPEC_FILE}"
echo "    Target Repo: ${COPR_PROJECT}"

for dependency in git python3 rpmbuild; do
    command -v "$dependency" >/dev/null || { echo "Missing dependency: $dependency" >&2; exit 1; }
done
if [[ -n "$(git -C "$REPO_ROOT" status --porcelain --untracked-files=no)" ]]; then
    echo "Source RPM preparation requires committed source and spec changes." >&2
    exit 1
fi
if [[ -e "$BUILD_DIR" ]]; then
    echo "Output already exists: $BUILD_DIR; preserve or move it before retrying." >&2
    exit 1
fi
mkdir -p "${BUILD_DIR}/SOURCES" "${BUILD_DIR}/SPECS" "${BUILD_DIR}/SRPMS"
python3 "$REPO_ROOT/tools/packaging/create_source_archive.py" --repo "$REPO_ROOT" \
    --output "$BUILD_DIR/SOURCES"
mv "$BUILD_DIR/SOURCES/sentinel-desktop.spec" "$BUILD_DIR/SPECS/"

# Validate Spec file using rpmlint if available
if command -v rpmlint &> /dev/null; then
    echo "==> Linting RPM Spec file with rpmlint..."
    rpmlint "${BUILD_DIR}/SPECS/sentinel-desktop.spec"
else
    echo "    [NOTE] rpmlint not found; skipping lint step."
fi

# Build Source RPM (SRPM)
echo "==> Building Source RPM (SRPM)..."
rpmbuild -bs --define "_topdir ${BUILD_DIR}" "$BUILD_DIR/SPECS/sentinel-desktop.spec"
echo "==> SRPM created successfully in ${BUILD_DIR}/SRPMS/"

# Submit build to Fedora COPR if copr-cli is installed and COPR_SUBMIT is enabled
if [[ "${1:-}" == "--submit" ]]; then
    if command -v copr-cli &> /dev/null; then
        SRPM_FILE=$(find "${BUILD_DIR}/SRPMS" -name "*.src.rpm" | head -n 1)
        if [[ -n "${SRPM_FILE}" ]]; then
            echo "==> Submitting SRPM to Fedora COPR (${COPR_PROJECT})..."
            copr-cli build "${COPR_PROJECT}" "${SRPM_FILE}"
        else
            echo "No source RPM was produced; refusing remote fallback." >&2
            exit 1
        fi
    else
        echo "[ERROR] copr-cli is not installed. Please install 'copr-cli' to submit builds to Fedora COPR."
        exit 1
    fi
else
    echo "==> COPR build preparation complete!"
    echo "    To submit to Fedora COPR, run: ${0} --submit"
fi
