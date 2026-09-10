#!/usr/bin/env bash
# abp installer — clones, builds, and installs the `abp` CLI.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/guillaume-behr/abp/master/install.sh | bash
#
# Environment variables:
#   ABP_VERSION      Git ref (branch/tag) to install. Default: master
#   ABP_INSTALL_DIR  Directory to install the 'abp' binary into.
#                    Default: /usr/local/bin if writable, else ~/.local/bin
set -euo pipefail

REPO_URL="https://github.com/guillaume-behr/abp.git"
REF="${ABP_VERSION:-master}"
BIN_NAME="abp"

info()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
error() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

if [ "$(uname -s)" != "Linux" ]; then
    warn "abp targets Linux; other platforms are untested."
fi

command -v git   >/dev/null 2>&1 || error "git is required but was not found. Install it and re-run."
command -v cmake >/dev/null 2>&1 || error "cmake is required but was not found. Install it and re-run."
command -v make  >/dev/null 2>&1 || error "make is required but was not found. Install it and re-run."
if ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
    error "a C++17 compiler (g++ or clang++) is required but was not found."
fi

if [ -n "${ABP_INSTALL_DIR:-}" ]; then
    INSTALL_DIR="$ABP_INSTALL_DIR"
elif [ "$(id -u)" = "0" ] || [ -w "/usr/local/bin" ]; then
    INSTALL_DIR="/usr/local/bin"
else
    INSTALL_DIR="$HOME/.local/bin"
fi
mkdir -p "$INSTALL_DIR"

WORK_DIR="$(mktemp -d -t abp-install-XXXXXX)"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

info "Cloning abp (${REF})..."
git clone --depth 1 --branch "$REF" "$REPO_URL" "$WORK_DIR/src" >/dev/null 2>&1 \
    || error "failed to clone $REPO_URL @ $REF"

info "Building abp (this can take a minute)..."
JOBS="$(command -v nproc >/dev/null 2>&1 && nproc || echo 2)"
cmake -S "$WORK_DIR/src" -B "$WORK_DIR/build" -DCMAKE_BUILD_TYPE=Release -DABP_BUILD_TESTS=OFF >/dev/null \
    || error "cmake configure failed"
cmake --build "$WORK_DIR/build" -j"$JOBS" >/dev/null \
    || error "build failed"

BIN_PATH="$WORK_DIR/build/src/$BIN_NAME"
[ -x "$BIN_PATH" ] || error "build succeeded but '$BIN_NAME' was not found at the expected path"

install -m 755 "$BIN_PATH" "$INSTALL_DIR/$BIN_NAME"
info "Installed $BIN_NAME to $INSTALL_DIR/$BIN_NAME"

case ":$PATH:" in
    *":$INSTALL_DIR:"*) ;;
    *)
        warn "$INSTALL_DIR is not on your PATH. Add this to your shell profile (~/.bashrc, ~/.zshrc, ...):"
        printf '\n    export PATH="%s:$PATH"\n\n' "$INSTALL_DIR"
        ;;
esac

if ! command -v adb >/dev/null 2>&1; then
    warn "adb was not found on your PATH. Install Android platform-tools to use abp."
fi

info "Done! Run '$BIN_NAME --help' to get started."
