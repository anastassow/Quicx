set -e

# release binaries live in their own repo, not in the source repo
REPO="Quicx-Engine/Quicx-Releases"
INSTALL_DIR="/usr/local/bin"

# bump on every release — what "latest" resolves to
LATEST_VERSION="v1.0.3"

VERSION="${QUICX_VERSION:-latest}"

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

# -------------------------
# Detect platform
# -------------------------
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
    x86_64) PLATFORM="darwin-x86_64" ;;
    arm64)  PLATFORM="darwin-arm64" ;;
    *) error "Unsupported architecture: $ARCH" ;;
  esac
  ;;
*)
  error "Unsupported OS: $OS"
  ;;
esac
}

# -------------------------
# Download binary
# -------------------------
download_binary() {
info "Resolving version: $VERSION"

# decide version folder — accepts "1.0.3" and "v1.0.3" alike
if [ "$VERSION" = "latest" ]; then
  VERSION_DIR="$LATEST_VERSION"
else
  case "$VERSION" in
    v*) VERSION_DIR="$VERSION" ;;
    *)  VERSION_DIR="v$VERSION" ;;
  esac
fi

BASE_URL="https://raw.githubusercontent.com/${REPO}/main/releases/${VERSION_DIR}"

BINARY="quicx-${PLATFORM}"
URL="${BASE_URL}/${BINARY}"

info "Downloading ${BINARY}..."

if command -v curl >/dev/null 2>&1; then
  curl -fsSL --progress-bar "$URL" -o "$DEST" || error "Download failed: $URL"
elif command -v wget >/dev/null 2>&1; then
  wget -q --show-progress "$URL" -O "$DEST" || error "Download failed: $URL"
else
  error "curl or wget is required"
fi

chmod +x "$DEST"
}

# -------------------------
# Install binary
# -------------------------
install_binary() {
info "Installing to ${INSTALL_DIR}/quicx..."

if [ -w "$INSTALL_DIR" ]; then
  mv "$DEST" "${INSTALL_DIR}/quicx"
else
  if command -v sudo >/dev/null 2>&1; then
    sudo mv "$DEST" "${INSTALL_DIR}/quicx"
  else
    error "No write permission to ${INSTALL_DIR} and sudo not available"
  fi
fi
}

# -------------------------
# Verify
# -------------------------
verify_install() {
if command -v quicx >/dev/null 2>&1; then
  success "quicx installed — run ${BOLD}quicx --help${RESET}"
else
  printf "\n${CYAN}Note:${RESET} not in PATH. Add:\n\n"
  printf "  export PATH=\"\$PATH:${INSTALL_DIR}\"\n\n"
fi
}

# -------------------------
# Cleanup
# -------------------------
cleanup() {
rm -rf "$TMP_DIR"
}

trap cleanup EXIT

# run
detect_platform
download_binary
install_binary
verify_install