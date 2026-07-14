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
#   # pass seekdb cmake flags, e.g. -DDEFAULT_LOG_LEVEL=OB_LOG_LEVEL_DBA_WARN
#   ./scripts/build-seekdb-glibc228.sh release \
#     --seekdb-cmake-arg -DDEFAULT_LOG_LEVEL=OB_LOG_LEVEL_DBA_WARN
#
# Environment:
#   SEEKDB_REPO       Local seekdb checkout to mount (optional if SEEKDB_GIT_URL is set)
#   SEEKDB_GIT_URL    Remote git URL to clone when SEEKDB_REPO is unset
#   SEEKDB_GIT_REF    Branch, tag, or commit to build (optional)
#   SEEKDB_OUT_BIN    Copy the built binary to this path (optional)
#   SEEKDB_EL8_IMAGE  manylinux_2_28 image (default: quay.io/pypa/manylinux_2_28:2026.03.20-1)
#   CONTAINER_RUNTIME docker | podman (auto-detected; podman used when docker is unavailable)
#   SEEKDB_CMAKE_ARGS space-separated cmake -D flags for seekdb build.sh
#                       (e.g. -DDEFAULT_LOG_LEVEL=OB_LOG_LEVEL_DBA_WARN)
#
# Prints the built binary path and max GLIBC symbol versions on stdout.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib/container-runtime.sh
source "$SCRIPT_DIR/lib/container-runtime.sh"
detect_container_runtime "$@"

BUILD_TYPE="release"
SEEKDB_CMAKE_EXTRA=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --seekdb-cmake-arg)
      [[ $# -ge 2 ]] || { echo "error: --seekdb-cmake-arg requires a value" >&2; exit 1; }
      SEEKDB_CMAKE_EXTRA+=("$2")
      shift 2
      ;;
    -D*)
      SEEKDB_CMAKE_EXTRA+=("$1")
      shift
      ;;
    release|debug|errsim)
      BUILD_TYPE="$1"
      shift
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      exit 1
      ;;
  esac
done

if [[ -n "${SEEKDB_CMAKE_ARGS:-}" ]]; then
  read -r -a _seekdb_cmake_from_env <<< "${SEEKDB_CMAKE_ARGS}"
  for arg in "${_seekdb_cmake_from_env[@]}"; do
    [[ -n "$arg" ]] && SEEKDB_CMAKE_EXTRA+=("$arg")
  done
fi

REPO="${SEEKDB_REPO:-${HOME}/seekdb}"
GIT_URL="${SEEKDB_GIT_URL:-https://github.com/oceanbase/seekdb.git}"
GIT_REF="${SEEKDB_GIT_REF:-}"
OUT_BIN="${SEEKDB_OUT_BIN:-}"
IMAGE="${SEEKDB_EL8_IMAGE:-quay.io/pypa/manylinux_2_28:2026.03.20-1}"

use_local_repo=0
if [[ -e "$REPO/.git" ]]; then
  use_local_repo=1
elif [[ -z "$GIT_URL" ]]; then
  echo "error: seekdb repo not found at $REPO and SEEKDB_GIT_URL is unset" >&2
  exit 1
fi

if [[ "$use_local_repo" == "0" && -z "$OUT_BIN" ]]; then
  OUT_BIN="${PWD}/build/seekdb-glibc228"
  echo "note: cloning in container; exporting binary to $OUT_BIN (set SEEKDB_OUT_BIN to override)" >&2
fi

echo "=== building seekdb ($BUILD_TYPE) in $IMAGE ($CONTAINER_RUNTIME) ==="

container_args=(
  --rm
  -e BUILD_TYPE="$BUILD_TYPE"
  -e HOST_UID="$(id -u)"
  -e HOST_GID="$(id -g)"
  -e GIT_URL="$GIT_URL"
  -e GIT_REF="$GIT_REF"
  -e USE_LOCAL_REPO="$use_local_repo"
)

if [[ "$use_local_repo" == "1" ]]; then
  container_args+=(-v "$REPO":/seekdb -w /seekdb)
else
  container_args+=(-w /tmp)
fi

if [[ -n "$OUT_BIN" ]]; then
  out_dir="$(dirname "$OUT_BIN")"
  mkdir -p "$out_dir"
  container_args+=(-v "$out_dir":/out)
  container_args+=(-e OUT_BIN="/out/$(basename "$OUT_BIN")")
fi

if [[ ${#SEEKDB_CMAKE_EXTRA[@]} -gt 0 ]]; then
  container_args+=(-e "SEEKDB_CMAKE_ARGS=${SEEKDB_CMAKE_EXTRA[*]}")
fi

"$CONTAINER_RUNTIME" run "${container_args[@]}" "$IMAGE" bash -c '
set -euo pipefail
yum install -y wget cpio git

checkout_git_ref() {
  if git rev-parse --verify -q "$GIT_REF^{commit}" >/dev/null 2>&1; then
    git checkout -q "$GIT_REF"
    return
  fi
  git fetch --depth 1 "$GIT_URL" "$GIT_REF"
  git checkout -q FETCH_HEAD
}

if [[ "$USE_LOCAL_REPO" == "1" ]]; then
  cd /seekdb
  if [[ -n "$GIT_REF" ]]; then
    original_ref="$(git symbolic-ref -q HEAD || echo "DETACHED:$(git rev-parse HEAD)")"
    checkout_git_ref
    trap '"'"'if [[ "$original_ref" == DETACHED:* ]]; then git checkout -q "${original_ref#DETACHED:}" 2>/dev/null || true; else git checkout -q "$original_ref" 2>/dev/null || true; fi'"'"' EXIT
  fi
else
  rm -rf /seekdb
  git clone --depth 1 "$GIT_URL" /seekdb
  cd /seekdb
  if [[ -n "$GIT_REF" ]]; then
    checkout_git_ref
  fi
fi

rm -rf "build_$BUILD_TYPE"
cmake_extra=()
if [[ -n "${SEEKDB_CMAKE_ARGS:-}" ]]; then
  read -r -a cmake_extra <<< "${SEEKDB_CMAKE_ARGS}"
fi
./build.sh "$BUILD_TYPE" --init "${cmake_extra[@]}" --make -j"$(nproc)"
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
  chown "$HOST_UID:$HOST_GID" "$OUT_BIN" 2>/dev/null || true
  bin="$OUT_BIN"
fi

echo "=== seekdb binary: $bin"
echo "=== max GLIBC symbols (want <= 2.28):"
objdump -T "$bin" | grep -oE "GLIBC_[0-9]+\.[0-9]+" | sort -V | uniq | tail -3
'
