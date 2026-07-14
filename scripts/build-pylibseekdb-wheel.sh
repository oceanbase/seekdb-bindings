#!/usr/bin/env bash
# Build pylibseekdb wheels with cibuildwheel.
#
# This script orchestrates:
#   1. Obtaining a seekdb server binary (local path, download URL, or source build)
#   2. Stripping the binary while saving debug symbols
#   3. Configuring and building libseekdb for wheel packaging
#   4. Running cibuildwheel (manylinux_2_28 on Linux; delocate on macOS)
#
# Supported platforms:
#   - Linux x86_64 / aarch64 (manylinux_2_28 only)
#   - macOS arm64
#
# Python ABIs (see python/pyproject.toml):
#   - cp311-cp311 for Python 3.11
#   - cp312-abi3 for Python 3.12+ (covers 3.12, 3.13, 3.14)
#
# Examples:
#   # Use an existing seekdb binary
#   ./scripts/build-pylibseekdb-wheel.sh --seekdb-bin /path/to/seekdb
#
#   # Download a prebuilt binary
#   ./scripts/build-pylibseekdb-wheel.sh --seekdb-url https://example.com/seekdb
#
#   # Build seekdb from git (manylinux on Linux, native on macOS)
#   ./scripts/build-pylibseekdb-wheel.sh --build-seekdb \
#     --seekdb-git-url https://github.com/oceanbase/seekdb.git \
#     --seekdb-git-ref master
#
#   # Build seekdb with extra cmake flags (forwarded to seekdb build.sh)
#   ./scripts/build-pylibseekdb-wheel.sh --build-seekdb \
#     --seekdb-cmake-arg -DDEFAULT_LOG_LEVEL=OB_LOG_LEVEL_DBA_WARN
#
#   # Override wheel version and limit platforms
#   ./scripts/build-pylibseekdb-wheel.sh --seekdb-bin /path/to/seekdb \
#     --wheel-version 0.2.0 --platform linux --arch x86_64
#
# Environment variables mirror the CLI flags (see --help).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

SEEKDB_BIN="${SEEKDB_BIN:-}"
SEEKDB_URL="${SEEKDB_URL:-}"
SEEKDB_SHA256="${SEEKDB_SHA256:-}"
SEEKDB_GIT_URL="${SEEKDB_GIT_URL:-https://github.com/oceanbase/seekdb.git}"
SEEKDB_GIT_REF="${SEEKDB_GIT_REF:-}"
SEEKDB_REPO="${SEEKDB_REPO:-}"
BUILD_SEEKDB="${BUILD_SEEKDB:-0}"
BUILD_TYPE="${BUILD_TYPE:-release}"
WHEEL_VERSION="${WHEEL_VERSION:-}"
PLATFORM="${PLATFORM:-auto}"
ARCH="${ARCH:-}"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT/build/wheelhouse}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
STRIP_SEEKDB="${STRIP_SEEKDB:-1}"
DEBUG_DIR="${DEBUG_DIR:-$BUILD_DIR}"
CIBW_BUILD="${CIBW_BUILD:-}"
SKIP_TESTS="${SKIP_TESTS:-0}"
CMAKE_EXTRA=()
SEEKDB_CMAKE_EXTRA=()
PYTHON="${PYTHON:-}"
USE_VENV="${USE_VENV:-1}"
VENV_DIR="${VENV_DIR:-$ROOT/.venv}"
PYPROJECT="$ROOT/python/pyproject.toml"
PYPROJECT_BACKUP=""
WHEEL_SCRIPT_ARGS=()

die() { echo "error: $*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage: build-pylibseekdb-wheel.sh [options]

Obtain seekdb binary (pick one):
  --seekdb-bin PATH         Use a prebuilt seekdb binary
  --seekdb-url URL          Download a prebuilt seekdb binary
  --seekdb-sha256 HEX       Verify downloaded seekdb binary SHA-256
  --build-seekdb            Build seekdb from source before packaging
  --seekdb-git-url URL      seekdb git remote (default: oceanbase/seekdb)
  --seekdb-git-ref REF      Branch, tag, or commit when building from source
  --seekdb-repo PATH        Local seekdb checkout (Linux manylinux build)

Wheel / cibuildwheel:
  --wheel-version VER       Override wheel version (temporarily patches pyproject.toml)
  --platform PLATFORM       auto | linux | macos (default: auto)
  --arch ARCH               x86_64 | aarch64 | arm64 (default: all for platform)
  --output-dir DIR          Wheel output directory (default: build/wheelhouse)
  --build-selector SPEC     cibuildwheel build selector (e.g. 'cp311-* cp312-*')
  --skip-tests              Disable cibuildwheel wheel tests

Binary preparation:
  --no-strip                Do not strip the seekdb binary
  --debug-dir DIR           Where to store seekdb.debug (default: build/)

Other:
  --build-type TYPE         seekdb build type: release | debug (default: release)
  --cmake-arg ARG           Extra cmake argument for libseekdb bindings (repeatable)
  --seekdb-cmake-arg ARG    Extra cmake argument for seekdb build.sh (e.g. -DDEFAULT_LOG_LEVEL=OB_LOG_LEVEL_DBA_WARN)
  --venv-dir DIR            Virtualenv directory (default: .venv)
  --no-venv                 Use system PYTHON instead of creating a virtualenv
  -h, --help                Show this help

Environment variables: SEEKDB_BIN, SEEKDB_URL, SEEKDB_SHA256, SEEKDB_GIT_URL, SEEKDB_GIT_REF,
SEEKDB_REPO, BUILD_SEEKDB, WHEEL_VERSION, PLATFORM, ARCH, OUTPUT_DIR, STRIP_SEEKDB,
DEBUG_DIR, CIBW_BUILD, SKIP_TESTS, BUILD_TYPE, PYTHON, CONTAINER_RUNTIME, USE_VENV, VENV_DIR,
SEEKDB_CMAKE_ARGS
EOF
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

init_container_runtime() {
  # shellcheck source=lib/container-runtime.sh
  source "$ROOT/scripts/lib/container-runtime.sh"
  detect_container_runtime "${WHEEL_SCRIPT_ARGS[@]}"
}

load_seekdb_cmake_extra_from_env() {
  [[ -n "${SEEKDB_CMAKE_ARGS:-}" ]] || return 0
  local arg
  read -r -a _seekdb_cmake_from_env <<< "${SEEKDB_CMAKE_ARGS}"
  for arg in "${_seekdb_cmake_from_env[@]}"; do
    [[ -n "$arg" ]] && SEEKDB_CMAKE_EXTRA+=("$arg")
  done
}

canon_path() {
  "$PYTHON" -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$1"
}

verify_seekdb_sha256() {
  local file="$1"
  [[ -n "$SEEKDB_SHA256" ]] || return 0
  local actual
  if command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "$file" | awk '{print $1}')"
  elif command -v shasum >/dev/null 2>&1; then
    actual="$(shasum -a 256 "$file" | awk '{print $1}')"
  else
    die "SEEKDB_SHA256 is set but neither sha256sum nor shasum is available"
  fi
  if [[ "$actual" != "$SEEKDB_SHA256" ]]; then
    die "seekdb sha256 mismatch: expected $SEEKDB_SHA256, got $actual"
  fi
  echo "==> seekdb sha256 verified" >&2
}

checkout_seekdb_git_ref() {
  local repo="$1"
  [[ -n "$SEEKDB_GIT_REF" ]] || return 0
  if git -C "$repo" rev-parse --verify -q "$SEEKDB_GIT_REF^{commit}" >/dev/null 2>&1; then
    git -C "$repo" checkout -q "$SEEKDB_GIT_REF"
    return
  fi
  git -C "$repo" fetch --depth 1 "$SEEKDB_GIT_URL" "$SEEKDB_GIT_REF"
  git -C "$repo" checkout -q FETCH_HEAD
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --seekdb-bin) SEEKDB_BIN="${2:?--seekdb-bin requires a value}"; shift 2 ;;
      --seekdb-url) SEEKDB_URL="${2:?--seekdb-url requires a value}"; shift 2 ;;
      --seekdb-sha256) SEEKDB_SHA256="${2:?--seekdb-sha256 requires a value}"; shift 2 ;;
      --build-seekdb) BUILD_SEEKDB=1; shift ;;
      --seekdb-git-url) SEEKDB_GIT_URL="${2:?--seekdb-git-url requires a value}"; shift 2 ;;
      --seekdb-git-ref) SEEKDB_GIT_REF="${2:?--seekdb-git-ref requires a value}"; shift 2 ;;
      --seekdb-repo) SEEKDB_REPO="${2:?--seekdb-repo requires a value}"; shift 2 ;;
      --wheel-version) WHEEL_VERSION="${2:?--wheel-version requires a value}"; shift 2 ;;
      --platform) PLATFORM="${2:?--platform requires a value}"; shift 2 ;;
      --arch) ARCH="${2:?--arch requires a value}"; shift 2 ;;
      --output-dir) OUTPUT_DIR="${2:?--output-dir requires a value}"; shift 2 ;;
      --build-selector) CIBW_BUILD="${2:?--build-selector requires a value}"; shift 2 ;;
      --skip-tests) SKIP_TESTS=1; shift ;;
      --no-strip) STRIP_SEEKDB=0; shift ;;
      --debug-dir) DEBUG_DIR="${2:?--debug-dir requires a value}"; shift 2 ;;
      --build-type) BUILD_TYPE="${2:?--build-type requires a value}"; shift 2 ;;
      --cmake-arg) CMAKE_EXTRA+=("${2:?--cmake-arg requires a value}"); shift 2 ;;
      --seekdb-cmake-arg) SEEKDB_CMAKE_EXTRA+=("${2:?--seekdb-cmake-arg requires a value}"); shift 2 ;;
      --venv-dir) VENV_DIR="${2:?--venv-dir requires a value}"; USE_VENV=1; shift 2 ;;
      --no-venv) USE_VENV=0; shift ;;
      -h|--help) usage; exit 0 ;;
      *) die "unknown argument: $1 (try --help)" ;;
    esac
  done
}

detect_host_platform() {
  case "$(uname -s)" in
    Linux) echo "linux" ;;
    Darwin) echo "macos" ;;
    *) die "unsupported host OS for wheel builds: $(uname -s)" ;;
  esac
}

download_seekdb() {
  local url="$1"
  local dest="$2"
  need_cmd curl
  mkdir -p "$(dirname "$dest")"
  echo "==> downloading seekdb from $url" >&2
  curl -fsSL "$url" -o "$dest"
  verify_seekdb_sha256 "$dest"
  chmod +x "$dest"
}

build_seekdb_linux() {
  local out_bin="$BUILD_DIR/seekdb-from-source"
  mkdir -p "$BUILD_DIR"

  local -a env_args=(
    BUILD_TYPE="$BUILD_TYPE"
    SEEKDB_GIT_URL="$SEEKDB_GIT_URL"
    SEEKDB_GIT_REF="$SEEKDB_GIT_REF"
    SEEKDB_OUT_BIN="$out_bin"
  )
  if [[ -n "$SEEKDB_REPO" ]]; then
    env_args+=(SEEKDB_REPO="$SEEKDB_REPO")
  fi
  if [[ ${#SEEKDB_CMAKE_EXTRA[@]} -gt 0 ]]; then
    env_args+=(SEEKDB_CMAKE_ARGS="${SEEKDB_CMAKE_EXTRA[*]}")
  fi

  init_container_runtime
  env_args+=(CONTAINER_RUNTIME="$CONTAINER_RUNTIME")

  echo "==> building seekdb inside manylinux_2_28 ($CONTAINER_RUNTIME)" >&2
  env "${env_args[@]}" "$ROOT/scripts/build-seekdb-glibc228.sh" "$BUILD_TYPE" >/dev/null
  [[ -f "$out_bin" ]] || die "seekdb build did not produce $out_bin"
  echo "$out_bin"
}

build_seekdb_macos() {
  local src_dir="$BUILD_DIR/seekdb-src"
  need_cmd git
  mkdir -p "$BUILD_DIR"

  if [[ ! -e "$src_dir/.git" ]]; then
    echo "==> cloning seekdb from $SEEKDB_GIT_URL" >&2
    git clone --depth 1 "$SEEKDB_GIT_URL" "$src_dir"
    checkout_seekdb_git_ref "$src_dir"
  elif [[ -n "$SEEKDB_GIT_REF" ]]; then
    echo "==> checking out $SEEKDB_GIT_REF in $src_dir" >&2
    checkout_seekdb_git_ref "$src_dir"
  fi

  echo "==> building seekdb natively on macOS" >&2
  (
    cd "$src_dir"
    rm -rf "build_$BUILD_TYPE"
    ./build.sh "$BUILD_TYPE" --init "${SEEKDB_CMAKE_EXTRA[@]}" --make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  )

  local bin
  bin="$(find "$src_dir/build_$BUILD_TYPE" -type f -name seekdb -perm -u+x | head -1)"
  [[ -n "$bin" ]] || die "seekdb binary not found after native build"
  echo "$bin"
}

resolve_seekdb_bin() {
  if [[ -n "$SEEKDB_BIN" ]]; then
    [[ -f "$SEEKDB_BIN" ]] || die "seekdb binary not found: $SEEKDB_BIN"
    echo "$SEEKDB_BIN"
    return
  fi

  if [[ -n "$SEEKDB_URL" ]]; then
    local dest="$BUILD_DIR/seekdb-downloaded"
    download_seekdb "$SEEKDB_URL" "$dest"
    echo "$dest"
    return
  fi

  if [[ "$BUILD_SEEKDB" == "1" ]]; then
    case "$(detect_host_platform)" in
      linux) build_seekdb_linux ;;
      macos) build_seekdb_macos ;;
    esac
    return
  fi

  die "no seekdb binary source specified; use --seekdb-bin, --seekdb-url, or --build-seekdb"
}

prepare_seekdb_binary() {
  local src="$1"
  local staged="$BUILD_DIR/seekdb"
  local debug_file="$DEBUG_DIR/seekdb.debug"
  local src_canon staged_canon

  mkdir -p "$BUILD_DIR" "$DEBUG_DIR"

  src_canon="$(canon_path "$src")"
  if [[ -f "$staged" ]]; then
    staged_canon="$(canon_path "$staged")"
    if [[ "$src_canon" == "$staged_canon" ]]; then
      echo "==> seekdb source is already staged at $staged (identical file)" >&2
    elif cmp -s "$src" "$staged"; then
      echo "==> seekdb at $staged is byte-identical to $src" >&2
    else
      echo "==> replacing staged seekdb at $staged with $src" >&2
      cp -f "$src" "$staged"
    fi
  else
    cp -f "$src" "$staged"
  fi
  chmod +x "$staged"

  if [[ "$STRIP_SEEKDB" == "1" ]]; then
    need_cmd strip
    echo "==> stripping seekdb (debug symbols -> $debug_file)" >&2
    if command -v objcopy >/dev/null 2>&1; then
      objcopy --only-keep-debug "$staged" "$debug_file"
      chmod -x "$debug_file" 2>/dev/null || true
      strip --strip-debug --strip-unneeded "$staged"
      objcopy --add-gnu-debuglink="$debug_file" "$staged" 2>/dev/null || true
    else
      cp -f "$staged" "$debug_file"
      strip "$staged"
    fi
    ls -lh "$staged" "$debug_file" >&2
    "$staged" -V 2>&1 | head -2 >&2 || die "seekdb -V failed after strip"
  else
    echo "==> skipping strip (--no-strip / STRIP_SEEKDB=0)" >&2
  fi

  echo "$staged"
}

configure_bindings() {
  local seekdb_bin="$1"
  local -a cmake_args=(
    -S "$ROOT"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE=Release
    -DSEEKDB_BIN="$seekdb_bin"
    -DPython3_EXECUTABLE="$PYTHON"
    -DPython_EXECUTABLE="$PYTHON"
  )

  if [[ "$(uname -s)" == "Darwin" ]]; then
    cmake_args+=(-DWITH_EXTERNAL_ZLIB=YES)
  fi
  cmake_args+=("${CMAKE_EXTRA[@]}")

  echo "==> cmake configure"
  cmake "${cmake_args[@]}"
}

build_seekdb_library() {
  echo "==> building libseekdb"
  cmake --build "$BUILD_DIR" --target seekdb -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  [[ -f "$BUILD_DIR/seekdb" ]] || die "expected $BUILD_DIR/seekdb after build"
  [[ -f "$BUILD_DIR/libseekdb.so" || -f "$BUILD_DIR/libseekdb.dylib" ]] \
    || die "libseekdb not found under $BUILD_DIR"
}

apply_wheel_version() {
  [[ -n "$WHEEL_VERSION" ]] || return 0
  PYPROJECT_BACKUP="$(mktemp)"
  cp "$PYPROJECT" "$PYPROJECT_BACKUP"
  sed -i.bak "s/^version = .*/version = \"${WHEEL_VERSION}\"/" "$PYPROJECT"
  rm -f "${PYPROJECT}.bak"
  echo "==> wheel version override: $WHEEL_VERSION" >&2
}

restore_wheel_version() {
  [[ -n "$PYPROJECT_BACKUP" && -f "$PYPROJECT_BACKUP" ]] || return 0
  cp -f "$PYPROJECT_BACKUP" "$PYPROJECT"
  rm -f "$PYPROJECT_BACKUP"
}

run_cibuildwheel() {
  local platform="$1"
  mkdir -p "$OUTPUT_DIR"

  local -a env_args=(
    SEEKDB_BIN="$BUILD_DIR/seekdb"
  )

  if [[ -n "$CIBW_BUILD" ]]; then
    env_args+=(CIBW_BUILD="$CIBW_BUILD")
  fi
  if [[ "$SKIP_TESTS" == "1" ]]; then
    env_args+=(CIBW_TEST_COMMAND=)
  fi

  case "$platform" in
    linux)
      init_container_runtime
      echo "==> container runtime: $CONTAINER_RUNTIME" >&2
      if [[ "$CONTAINER_RUNTIME" == "podman" ]]; then
        env_args+=(CIBW_CONTAINER_ENGINE=podman)
      fi
      if [[ -n "$ARCH" ]]; then
        env_args+=(CIBW_ARCHS_LINUX="$ARCH")
      fi
      ;;
    macos)
      if [[ -n "$ARCH" ]]; then
        env_args+=(CIBW_ARCHS_MACOS="$ARCH")
      fi
      ;;
    auto)
      ;;
    *) die "unsupported platform: $platform" ;;
  esac

  echo "==> running cibuildwheel (platform=$platform, output=$OUTPUT_DIR)"
  (
    cd "$ROOT"
    env "${env_args[@]}" "$PYTHON" -m cibuildwheel \
      --platform "$platform" \
      --output-dir "$OUTPUT_DIR" \
      "$ROOT/python"
  )

  echo ""
  echo "==> wheels written to $OUTPUT_DIR"
  ls -lh "$OUTPUT_DIR"/*.whl 2>/dev/null || die "no wheels produced in $OUTPUT_DIR"
}

pick_python() {
  if [[ -n "${PYTHON:-}" ]]; then
    echo "$PYTHON"
    return
  fi
  local candidate
  for candidate in python3.11 python3.12 python3.13 python3.14 python3 python; do
    if command -v "$candidate" >/dev/null 2>&1; then
      echo "$candidate"
      return
    fi
  done
  die "python >= 3.11 not found; set PYTHON=/path/to/python"
}

setup_python_env() {
  local requirements="$ROOT/scripts/requirements-wheel-build.txt"
  [[ -f "$requirements" ]] || die "missing requirements file: $requirements"

  if [[ -n "${PYTHON:-}" ]]; then
    need_cmd "$PYTHON"
    echo "==> using explicit PYTHON=$PYTHON" >&2
    "$PYTHON" -m pip install -q --upgrade pip
    "$PYTHON" -m pip install -q -r "$requirements"
    return
  fi

  if [[ "$USE_VENV" != "1" ]]; then
    PYTHON="$(pick_python)"
    need_cmd "$PYTHON"
    echo "==> using system python: $PYTHON (--no-venv)" >&2
    "$PYTHON" -m pip install -q --upgrade pip
    "$PYTHON" -m pip install -q -r "$requirements"
    return
  fi

  local base_python
  base_python="$(pick_python)"
  need_cmd "$base_python"

  if [[ ! -x "$VENV_DIR/bin/python" ]]; then
    echo "==> creating virtual environment at $VENV_DIR" >&2
    "$base_python" -m venv "$VENV_DIR"
  fi

  PYTHON="$VENV_DIR/bin/python"
  echo "==> using virtualenv: $PYTHON" >&2
  "$PYTHON" -m pip install -q --upgrade pip
  echo "==> installing build dependencies" >&2
  "$PYTHON" -m pip install -q -r "$requirements"
}

main() {
  WHEEL_SCRIPT_ARGS=("$@")
  parse_args "$@"
  load_seekdb_cmake_extra_from_env

  local host_platform
  host_platform="$(detect_host_platform)"
  local target_platform="$PLATFORM"
  if [[ "$target_platform" == "auto" ]]; then
    target_platform="$host_platform"
  fi

  if [[ "$target_platform" == "macos" && "$host_platform" != "macos" ]]; then
    die "macOS wheels must be built on macOS (got host=$host_platform)"
  fi

  need_cmd cmake
  setup_python_env
  need_cmd "$PYTHON"

  if [[ "$BUILD_DIR" != "$ROOT/build" ]]; then
    echo "warning: BUILD_DIR must be under the project root for cibuildwheel; using $ROOT/build" >&2
    if [[ "$DEBUG_DIR" == "$BUILD_DIR" ]]; then
      DEBUG_DIR="$ROOT/build"
    fi
    BUILD_DIR="$ROOT/build"
  fi

  echo "==> pylibseekdb wheel build"
  echo "    ROOT=$ROOT"
  echo "    PYTHON=$PYTHON"
  if [[ "$PYTHON" == "$VENV_DIR/bin/python" ]]; then
    echo "    venv=$VENV_DIR"
  else
    echo "    venv=disabled"
  fi
  echo "    platform=$target_platform arch=${ARCH:-all}"
  echo "    output=$OUTPUT_DIR"

  trap restore_wheel_version EXIT
  apply_wheel_version

  local raw_bin prepared_bin
  raw_bin="$(resolve_seekdb_bin)"
  prepared_bin="$(prepare_seekdb_binary "$raw_bin")"

  configure_bindings "$prepared_bin"
  build_seekdb_library
  run_cibuildwheel "$target_platform"

  echo ""
  echo "done."
  if [[ "$STRIP_SEEKDB" == "1" ]]; then
    echo "  debug symbols: $DEBUG_DIR/seekdb.debug"
  fi
  echo "  verify: $ROOT/scripts/verify-wheel.sh $OUTPUT_DIR/*.whl"
}

main "$@"
