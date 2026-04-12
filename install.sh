#!/usr/bin/env sh
set -e
REPO="anastassow/Quicx"
INSTALL_DIR="/usr/local/bin"
TMP_DIR="$(mktemp -d)"
DEST="${TMP_DIR}/quicx"
BOLD='\033[1m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
RED='\033[0;31m'
RESET='\033[0m'
info()    { printf "${CYAN}  →${RESET} %s\n" "$1"; }
success() { printf "${GREEN}  ✓${RESET} %s\n" "$1"; }
error()   { printf "${RED}  ✗${RESET} %s\n" "$1" >&2; exit 1; }
printf "\n${BOLD}  quicx installer${RESET}\n\n"
detect_platform() {
OS="$(uname -s)"
ARCH="$(uname -m)"
case "$OS" in
Linux)
case "$ARCH" in
x86_64)  PLATFORM="linux-x86_64" ;;
aarch64) PLATFORM="linux-aarch64" ;;
*) error "Unsupported architecture: $ARCH" ;;
esac
      ;;
Darwin)
case "$ARCH" in
x86_64)  PLATFORM="darwin-x86_64" ;;
arm64)   PLATFORM="darwin-arm64" ;;
*) error "Unsupported architecture: $ARCH" ;;
esac
      ;;
*) error "Unsupported OS: $OS" ;;
esac
}
download_binary() {
URL="https://raw.githubusercontent.com/${REPO}/main/quicxd-${PLATFORM}"
info "Downloading quicx for ${PLATFORM}..."
if command -v curl >/dev/null 2>&1; then
curl -fsSL --progress-bar "$URL" -o "$DEST" || error "Download failed. Check https://github.com/${REPO}"
elif command -v wget >/dev/null 2>&1; then
wget -q --show-progress "$URL" -O "$DEST" || error "Download failed. Check https://github.com/${REPO}"
else
error "curl or wget is required"
fi
chmod +x "$DEST"
}
install_binary() {
info "Installing to ${INSTALL_DIR}/quicx..."
if [ -w "$INSTALL_DIR" ]; then
mv "$DEST" "${INSTALL_DIR}/quicx"
else
if command -v sudo >/dev/null 2>&1; then
sudo mv "$DEST" "${INSTALL_DIR}/quicx"
else
error "No write permission to ${INSTALL_DIR} and sudo not available. Try running as root."
fi
fi
}
verify_install() {
if command -v quicx >/dev/null 2>&1; then
success "quicx installed — run ${BOLD}quicx --help${RESET} to get started"
printf "\n"
else
printf "\n  ${CYAN}Note:${RESET} quicx was installed to ${INSTALL_DIR} but is not in your PATH.\n"
printf "  Add this to your shell config:\n\n"
printf "    export PATH=\"\$PATH:${INSTALL_DIR}\"\n\n"
fi
}
cleanup() {
rm -rf "$TMP_DIR"
}
trap cleanup EXIT
detect_platform
download_binary
install_binary
verify_install
