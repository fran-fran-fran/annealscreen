#!/bin/sh
# AnnealScreen Installer
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage:
#   curl -sSL https://raw.githubusercontent.com/fran-fran-fran/annealscreen/main/scripts/install.sh | sh
#
# Or download and run:
#   wget https://raw.githubusercontent.com/fran-fran-fran/annealscreen/main/scripts/install.sh
#   chmod +x install.sh
#   ./install.sh
#
# Options:
#   --update       Update existing installation (preserves config)
#   --uninstall    Remove AnnealScreen
#   --local FILE   Install from local .zip (skip download)
#   --help         Show help
#
# For development (builds from source):
#   ./scripts/install.sh --build
#
# AnnealScreen requires: Klipper + Moonraker + annealr plugin

set -eu

# ── Configuration ────────────────────────────────────────────────────────

GITHUB_REPO="fran-fran-fran/annealscreen"
SERVICE_NAME="annealscreen"
INSTALL_DIR="${HOME}/annealscreen"
TMP_DIR="/tmp/annealscreen-install"

# ── Colors ───────────────────────────────────────────────────────────────

if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
    CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; CYAN=''; BOLD=''; NC=''
fi

log_info()    { printf '%b\n' "${CYAN}[INFO]${NC}  $1" >&2; }
log_ok()      { printf '%b\n' "${GREEN}[OK]${NC}    $1" >&2; }
log_warn()    { printf '%b\n' "${YELLOW}[WARN]${NC}  $1" >&2; }
log_error()   { printf '%b\n' "${RED}[ERROR]${NC} $1" >&2; }

# ── Error handler ────────────────────────────────────────────────────────

cleanup() {
    [ -d "$TMP_DIR" ] && rm -rf "$TMP_DIR"
}
trap cleanup EXIT

# ── Platform detection ───────────────────────────────────────────────────

detect_platform() {
    local arch
    arch=$(uname -m)

    case "$arch" in
        aarch64)
            # Check for Debian-family (Pi, BTT, MKS, etc.)
            if [ -f /etc/os-release ] && \
               grep -qi "debian\|raspbian\|ubuntu\|armbian" /etc/os-release 2>/dev/null; then
                echo "pi"
                return
            fi
            if command -v dpkg >/dev/null 2>&1; then
                echo "pi"
                return
            fi
            if [ -d /home/pi ] || [ -d /home/mks ] || [ -d /home/biqu ]; then
                echo "pi"
                return
            fi
            ;;
        armv7l)
            # 32-bit Pi
            if [ -f /etc/os-release ] && \
               grep -qi "debian\|raspbian" /etc/os-release 2>/dev/null; then
                echo "pi32"
                return
            fi
            ;;
        x86_64)
            echo "x86"
            return
            ;;
    esac

    echo "unsupported"
}

# ── Sudo helper ──────────────────────────────────────────────────────────

SUDO=""
check_permissions() {
    if [ "$(id -u)" != "0" ]; then
        if command -v sudo >/dev/null 2>&1; then
            SUDO="sudo"
        else
            log_error "Not root and sudo not available."
            exit 1
        fi
    fi
}

# ── Klipper user detection ──────────────────────────────────────────────

KLIPPER_USER=""
KLIPPER_HOME=""
detect_klipper_user() {
    # systemd service owner
    if command -v systemctl >/dev/null 2>&1; then
        local svc_user
        svc_user=$(systemctl show -p User --value klipper.service 2>/dev/null) || true
        if [ -n "$svc_user" ] && [ "$svc_user" != "root" ] && id "$svc_user" >/dev/null 2>&1; then
            KLIPPER_USER="$svc_user"
            KLIPPER_HOME=$(eval echo "~$svc_user")
            return 0
        fi
    fi

    # Well-known users
    for user in pi biqu mks; do
        if id "$user" >/dev/null 2>&1; then
            KLIPPER_USER="$user"
            KLIPPER_HOME="/home/$user"
            return 0
        fi
    done

    KLIPPER_USER=$(whoami)
    KLIPPER_HOME="$HOME"
}

# ── Download helpers ─────────────────────────────────────────────────────

fetch_url() {
    local url=$1
    if command -v curl >/dev/null 2>&1; then
        curl -sSL --connect-timeout 10 "$url" 2>/dev/null
    elif command -v wget >/dev/null 2>&1; then
        wget -qO- --timeout=10 "$url" 2>/dev/null
    else
        return 1
    fi
}

download_file() {
    local url=$1 dest=$2
    log_info "Downloading: $url"
    if command -v curl >/dev/null 2>&1; then
        curl -SL --connect-timeout 30 --progress-bar -o "$dest" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget --timeout=300 -O "$dest" "$url"
    else
        log_error "Neither curl nor wget available"
        return 1
    fi
    [ -f "$dest" ] && [ -s "$dest" ]
}

# ── Get latest version ───────────────────────────────────────────────────

get_latest_version() {
    log_info "Checking latest version..."
    local version
    version=$(fetch_url "https://api.github.com/repos/${GITHUB_REPO}/releases/latest" | \
              grep '"tag_name"' | sed 's/.*"\([^"]*\)".*/\1/')
    if [ -z "$version" ]; then
        log_error "Failed to fetch latest version from GitHub"
        exit 1
    fi
    echo "$version"
}

# ── Download release ─────────────────────────────────────────────────────

download_release() {
    local version=$1 platform=$2
    local filename="annealscreen-${platform}.zip"
    local url="https://github.com/${GITHUB_REPO}/releases/download/${version}/${filename}"
    local dest="${TMP_DIR}/${filename}"

    mkdir -p "$TMP_DIR"
    if ! download_file "$url" "$dest"; then
        log_error "Failed to download ${filename}"
        log_error "URL: $url"
        log_error ""
        log_error "Either the release doesn't exist or network is unreachable."
        log_error "For local install, build first then: ./scripts/install.sh --local path/to/zip"
        exit 1
    fi

    if ! unzip -tqq "$dest" >/dev/null 2>&1; then
        log_error "Downloaded file is not a valid zip archive"
        exit 1
    fi

    log_ok "Downloaded ${filename}"
}

# ── Extract and install ──────────────────────────────────────────────────

extract_release() {
    local platform=$1
    local archive
    archive=$(ls "${TMP_DIR}"/annealscreen-*.zip 2>/dev/null | head -1)
    if [ -z "$archive" ]; then
        log_error "No archive found in ${TMP_DIR}"
        exit 1
    fi

    local extract_dir="${TMP_DIR}/extract"
    mkdir -p "$extract_dir"
    unzip -q -o "$archive" -d "$extract_dir"

    # Backup existing settings
    local backup_settings=""
    if [ -f "${INSTALL_DIR}/config/settings.json" ]; then
        backup_settings="${TMP_DIR}/settings.json.bak"
        cp "${INSTALL_DIR}/config/settings.json" "$backup_settings"
        log_info "Backed up existing settings.json"
    fi

    # Install
    mkdir -p "${INSTALL_DIR}"
    cp -r "${extract_dir}/"* "${INSTALL_DIR}/"

    # Restore settings
    if [ -n "$backup_settings" ] && [ -f "$backup_settings" ]; then
        cp "$backup_settings" "${INSTALL_DIR}/config/settings.json"
        log_info "Restored settings.json"
    fi

    # Make binary executable
    chmod +x "${INSTALL_DIR}/bin/annealscreen"

    log_ok "Installed to ${INSTALL_DIR}"
}

# ── Competing UIs ────────────────────────────────────────────────────────

stop_competing_uis() {
    for svc in KlipperScreen klipperscreen helixscreen guppyscreen; do
        if systemctl is-active --quiet "$svc" 2>/dev/null; then
            log_warn "Stopping competing UI: $svc"
            $SUDO systemctl stop "$svc" 2>/dev/null || true
            $SUDO systemctl disable "$svc" 2>/dev/null || true
        fi
    done
}

# ── Systemd service ──────────────────────────────────────────────────────

install_service() {
    log_info "Installing systemd service..."

    $SUDO tee "/etc/systemd/system/${SERVICE_NAME}.service" > /dev/null << EOF
[Unit]
Description=AnnealScreen - Touchscreen UI for annealr
After=network-online.target klipper.service moonraker.service
After=systemd-udev-settle.service
Wants=network-online.target

[Service]
Type=simple
User=${KLIPPER_USER}

WorkingDirectory=${INSTALL_DIR}

# Unbind framebuffer console before starting (runs as root via + prefix).
# Prevents kernel messages and login prompt from painting over the UI.
# This is the primary mechanism — matches HelixScreen's approach.
ExecStartPre=+/bin/sh -c 'for f in /sys/class/vtconsole/vtcon*/bind; do echo 0 > "\$\$f" 2>/dev/null || true; done'

ExecStart=${INSTALL_DIR}/bin/annealscreen -v

Restart=on-failure
RestartSec=5

Environment=HOME=${KLIPPER_HOME}

# Device access groups
SupplementaryGroups=tty video input

# CAP_SYS_TTY_CONFIG: allows KDSETMODE KD_GRAPHICS as backup console suppression
AmbientCapabilities=CAP_SYS_TTY_CONFIG

# Logging
StandardOutput=journal
StandardError=journal
SyslogIdentifier=annealscreen

[Install]
WantedBy=multi-user.target
EOF

    $SUDO systemctl daemon-reload
    $SUDO systemctl enable "$SERVICE_NAME"
    log_ok "Systemd service installed"
}

# ── Moonraker update_manager ────────────────────────────────────────────

configure_moonraker() {
    local conf=""
    for candidate in \
        "${KLIPPER_HOME}/printer_data/config/moonraker.conf" \
        "${HOME}/printer_data/config/moonraker.conf" \
        "${HOME}/moonraker.conf" \
        "${HOME}/klipper_config/moonraker.conf"; do
        if [ -f "$candidate" ]; then
            conf="$candidate"
            break
        fi
    done

    if [ -z "$conf" ]; then
        log_info "moonraker.conf not found, skipping update_manager config"
        return 0
    fi

    if grep -q '\[update_manager annealscreen\]' "$conf" 2>/dev/null; then
        log_info "Moonraker update_manager already configured"
        return 0
    fi

    log_info "Adding update_manager section to $conf"
    cat >> "$conf" << MOONRAKER

# AnnealScreen Update Manager
# Enables one-click updates from Mainsail/Fluidd
[update_manager annealscreen]
type: web
channel: stable
repo: ${GITHUB_REPO}
path: ${INSTALL_DIR}
MOONRAKER

    # Restart Moonraker to pick up changes
    if systemctl is-active --quiet moonraker 2>/dev/null; then
        $SUDO systemctl restart moonraker 2>/dev/null || true
        log_info "Restarted Moonraker"
    fi

    log_ok "Moonraker update_manager configured"
}

# ── Start service ────────────────────────────────────────────────────────

start_service() {
    log_info "Starting AnnealScreen..."
    $SUDO systemctl start "$SERVICE_NAME"

    local i
    for i in 1 2 3 4 5; do
        sleep 1
        if systemctl is-active --quiet "$SERVICE_NAME"; then
            log_ok "AnnealScreen is running!"
            return
        fi
    done
    log_warn "Service may still be starting..."
    log_warn "Check: systemctl status $SERVICE_NAME"
}

# ── Uninstall ────────────────────────────────────────────────────────────

uninstall() {
    log_info "Uninstalling AnnealScreen..."

    if systemctl is-active --quiet "$SERVICE_NAME" 2>/dev/null; then
        $SUDO systemctl stop "$SERVICE_NAME" || true
    fi
    if systemctl is-enabled --quiet "$SERVICE_NAME" 2>/dev/null; then
        $SUDO systemctl disable "$SERVICE_NAME" || true
    fi
    $SUDO rm -f "/etc/systemd/system/${SERVICE_NAME}.service"
    $SUDO systemctl daemon-reload 2>/dev/null || true

    # Remove Moonraker section
    for conf in \
        "${KLIPPER_HOME}/printer_data/config/moonraker.conf" \
        "${HOME}/printer_data/config/moonraker.conf"; do
        if [ -f "$conf" ] && grep -q '\[update_manager annealscreen\]' "$conf" 2>/dev/null; then
            awk '
                /^\[update_manager annealscreen\]/ { skip=1; next }
                /^\[/ { skip=0 }
                !skip { print }
            ' "$conf" > "${conf}.tmp" && mv "${conf}.tmp" "$conf"
            # Remove comment
            sed -i '/# AnnealScreen Update Manager/d' "$conf" 2>/dev/null || true
            sed -i '/# Enables one-click updates/d' "$conf" 2>/dev/null || true
            log_info "Removed Moonraker update_manager section"
        fi
    done

    if [ -d "$INSTALL_DIR" ]; then
        rm -rf "$INSTALL_DIR"
        log_ok "Removed ${INSTALL_DIR}"
    fi

    log_ok "AnnealScreen uninstalled"
}

# ── Build from source ────────────────────────────────────────────────────

build_from_source() {
    log_info "Building from source..."

    # Must be run from the repo root
    if [ ! -f "Makefile" ]; then
        log_error "Makefile not found. Run from the annealscreen repo root."
        exit 1
    fi

    # Init submodules if needed
    if [ ! -f "lib/lvgl/lvgl.h" ]; then
        log_info "Initializing git submodules..."
        git submodule update --init --recursive
    fi

    # Build
    make -j"$(nproc)"

    # Create a local zip for install
    mkdir -p "${TMP_DIR}/pkg/bin" "${TMP_DIR}/pkg/config/themes" "${TMP_DIR}/pkg/ui_xml"
    cp build/bin/annealscreen "${TMP_DIR}/pkg/bin/"
    cp -r ui_xml/* "${TMP_DIR}/pkg/ui_xml/"
    cp -r config/* "${TMP_DIR}/pkg/config/"
    cp README.md "${TMP_DIR}/pkg/" 2>/dev/null || true

    local platform
    platform=$(detect_platform)
    (cd "${TMP_DIR}/pkg" && zip -rq "${TMP_DIR}/annealscreen-${platform}.zip" .)
    log_ok "Built and packaged"
}

# ── Main ─────────────────────────────────────────────────────────────────

main() {
    local update_mode=false
    local uninstall_mode=false
    local build_mode=false
    local local_archive=""
    local version=""

    while [ $# -gt 0 ]; do
        case $1 in
            --update)    update_mode=true; shift ;;
            --uninstall) uninstall_mode=true; shift ;;
            --build)     build_mode=true; shift ;;
            --local)     local_archive="$2"; shift 2 ;;
            --version)   version="$2"; shift 2 ;;
            --help|-h)
                echo "AnnealScreen Installer"
                echo ""
                echo "Usage: $0 [options]"
                echo ""
                echo "  --update       Update existing installation"
                echo "  --uninstall    Remove AnnealScreen"
                echo "  --build        Build from source, then install"
                echo "  --local FILE   Install from local .zip"
                echo "  --version VER  Install specific version"
                echo "  --help         Show this help"
                exit 0
                ;;
            *) log_error "Unknown option: $1"; exit 1 ;;
        esac
    done

    printf '\n'
    printf '%b\n' "${BOLD}========================================${NC}"
    printf '%b\n' "${BOLD}     AnnealScreen Installer${NC}"
    printf '%b\n' "${BOLD}========================================${NC}"
    printf '\n'

    # Detect platform
    local platform
    platform=$(detect_platform)
    log_info "Platform: ${BOLD}${platform}${NC}"

    if [ "$platform" = "unsupported" ]; then
        log_error "Unsupported platform: $(uname -m)"
        log_error "AnnealScreen supports: Raspberry Pi (aarch64/armv7l), x86_64"
        exit 1
    fi

    check_permissions
    detect_klipper_user
    log_info "Klipper user: ${KLIPPER_USER} (${KLIPPER_HOME})"
    log_info "Install directory: ${INSTALL_DIR}"

    # Handle uninstall
    if [ "$uninstall_mode" = true ]; then
        uninstall
        exit 0
    fi

    # Handle build mode
    if [ "$build_mode" = true ]; then
        build_from_source
    fi

    # Download or use local archive
    if [ -n "$local_archive" ]; then
        mkdir -p "$TMP_DIR"
        cp "$local_archive" "${TMP_DIR}/"
        log_ok "Using local archive: $local_archive"
    elif [ "$build_mode" = false ]; then
        if [ -z "$version" ]; then
            version=$(get_latest_version)
        fi
        log_info "Version: ${BOLD}${version}${NC}"
        download_release "$version" "$platform"
    fi

    # Stop service if updating
    if [ "$update_mode" = true ] || systemctl is-active --quiet "$SERVICE_NAME" 2>/dev/null; then
        log_info "Stopping existing service..."
        $SUDO systemctl stop "$SERVICE_NAME" 2>/dev/null || true
    fi

    stop_competing_uis
    extract_release "$platform"
    install_service
    configure_moonraker
    start_service

    printf '\n'
    printf '%b\n' "${GREEN}${BOLD}========================================${NC}"
    printf '%b\n' "${GREEN}${BOLD}    Installation Complete!${NC}"
    printf '%b\n' "${GREEN}${BOLD}========================================${NC}"
    printf '\n'
    echo "AnnealScreen installed to ${INSTALL_DIR}"
    echo ""
    echo "Useful commands:"
    echo "  systemctl status ${SERVICE_NAME}    # Check status"
    echo "  journalctl -u ${SERVICE_NAME} -f    # View logs"
    echo "  systemctl restart ${SERVICE_NAME}   # Restart"
    echo ""
    echo "Edit UI:     ${INSTALL_DIR}/ui_xml/"
    echo "Settings:    ${INSTALL_DIR}/config/settings.json"
    echo "Themes:      ${INSTALL_DIR}/config/themes/"
    echo ""
}

main "$@"