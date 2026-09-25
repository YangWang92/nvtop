#!/usr/bin/env bash
# Build and install nvtop with Moore Threads support.
set -euo pipefail

SOURCE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-/usr/local}"
BUILD_DIR="${BUILD_DIR:-${SOURCE_DIR}/build-install}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')}"
INSTALL_DEPS=0
CMAKE_ARGS=()

usage() {
  cat <<'HELP'
Usage: ./install.sh [--deps] [--prefix PATH] [--jobs N] [-- CMAKE_OPTIONS...]

Build Release nvtop with only Moore Threads support and install it.
Enable additional GPU backends via CMAKE_OPTIONS if needed.
  --deps         Install build dependencies (apt-get or dnf; requires root/sudo)
  --prefix PATH  Installation prefix (default: /usr/local)
  --jobs N       Parallel build jobs (default: number of online CPUs)
  -h, --help     Show this help

Environment overrides: PREFIX, BUILD_DIR, JOBS.
Examples:
  ./install.sh --deps
  ./install.sh --prefix "$HOME/.local"
  ./install.sh -- -DBUILD_TESTING=ON

The Moore Threads driver and libmtml must already be installed to monitor GPUs.
HELP
}

fail() { printf 'Error: %s\n' "$*" >&2; exit 1; }
as_root() {
  if (( EUID == 0 )); then
    "$@"
  elif command -v sudo >/dev/null 2>&1; then
    sudo -- "$@"
  else
    fail "Root privileges required. Install sudo or use --prefix \"\$HOME/.local\"."
  fi
}

while (( $# )); do
  case "$1" in
    --deps) INSTALL_DEPS=1; shift ;;
    --prefix|--jobs)
      (( $# >= 2 )) && [[ -n "$2" ]] || fail "$1 requires a value"
      if [[ "$1" == --prefix ]]; then PREFIX="$2"; else JOBS="$2"; fi
      shift 2 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; CMAKE_ARGS=("$@"); break ;;
    *) fail "Unknown argument: $1 (see --help)" ;;
  esac
done
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "--jobs must be a positive integer"
[[ "$PREFIX" == /* ]] || fail "Installation prefix must be an absolute path"
[[ "$BUILD_DIR" == /* ]] || BUILD_DIR="$PWD/$BUILD_DIR"

if (( INSTALL_DEPS )); then
  if command -v apt-get >/dev/null 2>&1; then
    as_root apt-get update
    as_root apt-get install -y build-essential cmake pkg-config libncurses-dev libdrm-dev libudev-dev
  elif command -v dnf >/dev/null 2>&1; then
    as_root dnf install -y gcc gcc-c++ make cmake pkgconf-pkg-config ncurses-devel libdrm-devel systemd-devel
  else
    fail "Automatic dependencies require apt-get or dnf. Install C/C++ compilers, make, cmake, pkg-config, ncurses, libdrm and libudev development packages manually."
  fi
fi
command -v cmake >/dev/null 2>&1 || fail "cmake not found; rerun with --deps"

# This branch targets Moore Threads; enable other vendors explicitly after --.
BACKEND_ARGS=()
for backend in NVIDIA AMDGPU RADEON INTEL MSM APPLE PANFROST PANTHOR ASCEND V3D TPU ROCKCHIP METAX ENFLAME TENSTORRENT IXML; do
  BACKEND_ARGS+=("-D${backend}_SUPPORT=OFF")
done
cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" \
  "${BACKEND_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DMOORETHREADS_SUPPORT=ON \
  "${CMAKE_ARGS[@]}"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

# Check the nearest existing ancestor to support new user-owned prefixes.
ancestor="$PREFIX"
while [[ ! -e "$ancestor" ]]; do ancestor="$(dirname -- "$ancestor")"; done
if [[ -w "$ancestor" ]]; then
  cmake --install "$BUILD_DIR" --prefix "$PREFIX"
else
  as_root "$(command -v cmake)" --install "$BUILD_DIR" --prefix "$PREFIX"
fi
printf '\nInstalled nvtop. Run: %s/bin/nvtop\n' "${PREFIX%/}"
