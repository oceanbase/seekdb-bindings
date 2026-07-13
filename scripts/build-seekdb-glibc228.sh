#!/usr/bin/env bash
# Build the seekdb server binary inside a manylinux_2_28 container so it is
# compatible with pylibseekdb manylinux wheels.
#
# Usage:
#   # local checkout (default: $HOME/seekdb)
#   SEEKDB_REPO=~/seekdb ./scripts/build-seekdb-glibc228.sh [release|debug]
#
#   # clone a specific tag inside the container
#   SEEKDB_GIT_URL=https://github.com/oceanbase/seekdb.git \
#   SEEKDB_GIT_REF=v1.0.0 \
#   ./scripts/build-seekdb-glibc228.sh
#
# Environment:
#   SEEKDB_REPO       Local seekdb checkout to mount (optional if SEEKDB_GIT_URL is set)
#   SEEKDB_GIT_URL    Remote git URL to clone when SEEKDB_REPO is unset
#   SEEKDB_GIT_REF    Branch, tag, or commit to build (optional)
#   SEEKDB_OUT_BIN    Copy the built binary to this path (optional)
#   SEEKDB_EL8_IMAGE  manylinux_2_28 image (default: quay.io/pypa/manylinux_2_28:2026.03.20-1)
#
# Prints the built binary path and max GLIBC symbol versions on stdout.

set -euo pipefail

if ! docker info >/dev/null 2>&1; then
  exec sg docker -c "$0 ${*:-}"
fi

REPO="${SEEKDB_REPO:-${HOME}/seekdb}"
GIT_URL="${SEEKDB_GIT_URL:-https://github.com/oceanbase/seekdb.git}"
GIT_REF="${SEEKDB_GIT_REF:-}"
OUT_BIN="${SEEKDB_OUT_BIN:-}"
IMAGE="${SEEKDB_EL8_IMAGE:-quay.io/pypa/manylinux_2_28:2026.03.20-1}"
BUILD_TYPE="${1:-release}"

use_local_repo=0
if [[ -d "$REPO/.git" ]]; then
  use_local_repo=1
elif [[ -z "$GIT_URL" ]]; then
  echo "error: seekdb repo not found at $REPO and SEEKDB_GIT_URL is unset" >&2
  exit 1
fi

echo "=== building seekdb ($BUILD_TYPE) in $IMAGE ==="

docker_args=(
  --rm
  -e BUILD_TYPE="$BUILD_TYPE"
  -e HOST_UID="$(id -u)"
  -e HOST_GID="$(id -g)"
  -e GIT_URL="$GIT_URL"
  -e GIT_REF="$GIT_REF"
  -e USE_LOCAL_REPO="$use_local_repo"
)

if [[ "$use_local_repo" == "1" ]]; then
  docker_args+=(-v "$REPO":/seekdb -w /seekdb)
else
  docker_args+=(-w /tmp)
fi

if [[ -n "$OUT_BIN" ]]; then
  out_dir="$(dirname "$OUT_BIN")"
  mkdir -p "$out_dir"
  docker_args+=(-v "$out_dir":/out)
  docker_args+=(-e OUT_BIN="/out/$(basename "$OUT_BIN")")
fi

docker run "${docker_args[@]}" "$IMAGE" bash -c '
set -euo pipefail
yum install -y wget cpio git

if [[ "$USE_LOCAL_REPO" == "1" ]]; then
  cd /seekdb
  if [[ -n "$GIT_REF" ]]; then
    git fetch --depth 1 origin "$GIT_REF"
    git checkout -q FETCH_HEAD
  fi
else
  rm -rf /seekdb
  if [[ -n "$GIT_REF" ]]; then
    git clone --depth 1 --branch "$GIT_REF" "$GIT_URL" /seekdb
  else
    git clone --depth 1 "$GIT_URL" /seekdb
  fi
  cd /seekdb
fi

rm -rf "build_$BUILD_TYPE"
./build.sh "$BUILD_TYPE" --init --make -j"$(nproc)"
if [[ "$USE_LOCAL_REPO" == "1" ]]; then
  chown -R "$HOST_UID:$HOST_GID" "build_$BUILD_TYPE" deps/3rd 2>/dev/null || true
fi

bin=$(find "build_$BUILD_TYPE" -type f -name seekdb -perm -u+x | head -1)
if [[ -z "$bin" ]]; then
  echo "error: seekdb binary not found under build_$BUILD_TYPE" >&2
  exit 1
fi

if [[ -n "${OUT_BIN:-}" ]]; then
  cp -f "$bin" "$OUT_BIN"
  chmod +x "$OUT_BIN"
  bin="$OUT_BIN"
fi

echo "=== seekdb binary: $bin"
echo "=== max GLIBC symbols (want <= 2.28):"
objdump -T "$bin" | grep -oE "GLIBC_[0-9]+\.[0-9]+" | sort -V | uniq | tail -3
'
