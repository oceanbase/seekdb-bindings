#!/usr/bin/env bash
set -euo pipefail

if ! docker info >/dev/null 2>&1; then
  exec sg docker -c "$0 ${*:-}"
fi

REPO="${SEEKDB_REPO:-${HOME}/seekdb}"
if [ ! -d "$REPO" ]; then
  echo "error: seekdb repo not found at $REPO" >&2
  echo "       set SEEKDB_REPO=/path/to/seekdb to override" >&2
  exit 1
fi

IMAGE="${SEEKDB_EL8_IMAGE:-quay.io/pypa/manylinux_2_28:2026.03.20-1}"
BUILD_TYPE="${1:-release}"

echo "=== building seekdb ($BUILD_TYPE) in $IMAGE ==="

docker run --rm \
  -v "$REPO":/seekdb -w /seekdb \
  -e BUILD_TYPE="$BUILD_TYPE" \
  -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
  "$IMAGE" bash -c '
set -euo pipefail
yum install -y wget cpio
rm -rf "build_$BUILD_TYPE"
./build.sh "$BUILD_TYPE" --init --make -j"$(nproc)"
chown -R "$HOST_UID:$HOST_GID" "build_$BUILD_TYPE" deps/3rd 2>/dev/null || true
bin=$(find "build_$BUILD_TYPE" -type f -name seekdb -perm -u+x | head -1)
echo "=== seekdb binary: $bin"
echo "=== max GLIBC symbols (want <= 2.28):"
objdump -T "$bin" | grep -oE "GLIBC_[0-9]+\.[0-9]+" | sort -V | uniq | tail -3
'
