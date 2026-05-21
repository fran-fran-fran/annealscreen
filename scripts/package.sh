#!/usr/bin/env bash
# Package AnnealScreen for release
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Creates annealscreen-<platform>.zip ready for GitHub Releases.
# The installer downloads this zip and extracts it to ~/annealscreen/.
#
# Usage:
#   ./scripts/package.sh              # Package for current platform
#   ./scripts/package.sh pi           # Package for Pi (must have binary)
#   ./scripts/package.sh --all        # Package for all built platforms

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"
PKG_DIR="${BUILD_DIR}/pkg"
RELEASE_DIR="${BUILD_DIR}/release"

detect_platform() {
    local arch
    arch=$(uname -m)
    case "$arch" in
        aarch64) echo "pi" ;;
        armv7l)  echo "pi32" ;;
        x86_64)  echo "x86" ;;
        *)       echo "unknown" ;;
    esac
}

package_platform() {
    local platform=$1
    local binary="${BUILD_DIR}/bin/annealscreen"

    if [ ! -f "$binary" ]; then
        echo "ERROR: Binary not found at ${binary}"
        echo "Build first with: make -j"
        exit 1
    fi

    echo "Packaging annealscreen-${platform}.zip..."

    local pkg="${PKG_DIR}/${platform}"
    rm -rf "$pkg"
    mkdir -p "$pkg"/{bin,config/themes,ui_xml}

    # Binary
    cp "$binary" "$pkg/bin/"
    chmod +x "$pkg/bin/annealscreen"

    # XML layouts
    cp -r "${REPO_DIR}/ui_xml/"* "$pkg/ui_xml/"

    # Config
    cp -r "${REPO_DIR}/config/themes/"*.json "$pkg/config/themes/"
    cp "${REPO_DIR}/config/settings.json.template" "$pkg/config/settings.json.template"

    # Docs
    cp "${REPO_DIR}/README.md" "$pkg/" 2>/dev/null || true

    # Version info (for Moonraker update_manager)
    local version
    version=$("$binary" --version 2>/dev/null | head -1 || echo "dev")
    cat > "$pkg/release_info.json" << EOF
{"project_name":"annealscreen","project_owner":"YOUR_USERNAME","version":"${version}"}
EOF

    # Create zip (flat layout — installer extracts directly to INSTALL_DIR)
    mkdir -p "$RELEASE_DIR"
    local zipfile="${RELEASE_DIR}/annealscreen-${platform}.zip"
    rm -f "$zipfile"
    (cd "$pkg" && zip -rq "$zipfile" .)

    local size
    size=$(ls -lh "$zipfile" | awk '{print $5}')
    echo "Created: ${zipfile} (${size})"
}

# ── Main ─────────────────────────────────────────────────────────────────

if [ "${1:-}" = "--all" ]; then
    for p in pi pi32 x86; do
        if [ -f "${BUILD_DIR}/bin/annealscreen" ]; then
            package_platform "$p"
        fi
    done
elif [ -n "${1:-}" ]; then
    package_platform "$1"
else
    platform=$(detect_platform)
    package_platform "$platform"
fi

echo ""
echo "Release artifacts in: ${RELEASE_DIR}/"
ls -la "${RELEASE_DIR}/"
