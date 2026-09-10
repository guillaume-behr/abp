#!/usr/bin/env bash
# Removes an `abp` binary installed by install.sh.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/guillaume-behr/abp/master/uninstall.sh | bash
#
# Environment variables:
#   ABP_INSTALL_DIR  Extra directory to check, in case abp was installed
#                     with a custom ABP_INSTALL_DIR at install time.
set -euo pipefail

BIN_NAME="abp"
REMOVED=0

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }

remove_if_present() {
    local path="$1"
    [ -f "$path" ] || return 0
    if rm -f "$path" 2>/dev/null; then
        info "Removed $path"
        REMOVED=1
    else
        warn "Found $path but could not remove it (try again with sudo)."
    fi
}

for dir in "${ABP_INSTALL_DIR:-}" "/usr/local/bin" "$HOME/.local/bin"; do
    [ -n "$dir" ] || continue
    remove_if_present "$dir/$BIN_NAME"
done

# Also catch whatever `abp` currently resolves to on PATH, in case it was
# installed somewhere else entirely.
if command -v "$BIN_NAME" >/dev/null 2>&1; then
    remove_if_present "$(command -v "$BIN_NAME")"
fi

if [ "$REMOVED" = "1" ]; then
    info "abp has been uninstalled."
else
    warn "No installed '$BIN_NAME' binary was found in the usual locations."
    warn "If you installed it elsewhere, remove it manually or re-run with:"
    warn "  ABP_INSTALL_DIR=/path/to/dir bash uninstall.sh"
fi
