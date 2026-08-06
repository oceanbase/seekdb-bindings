#!/usr/bin/env bash
# Build a self-contained libseekdb C SDK archive for the native host platform.
#
# Linux builds run in a manylinux container to give the archive an explicit
# glibc baseline. macOS builds run natively because Apple binaries cannot be
# built or repaired on Linux. Run this script once on each release builder.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

VERSION="${LIBSEEKDB_VERSION:-}"
REVISION="${LIBSEEKDB_REVISION:-1}"
SEEKDB_URL="${SEEKDB_GIT_URL:-https://github.com/oceanbase/seekdb.git}"
SEEKDB_REF="${SEEKDB_GIT_REF:-master}"
BINDINGS_URL="${BINDINGS_GIT_URL:-https://github.com/oceanbase/seekdb-bindings.git}"
BINDINGS_REF="${BINDINGS_GIT_REF:-main}"
TARGET_PLATFORM="${PLATFORM:-auto}"
TARGET_ARCH="${ARCH:-auto}"
GLIBC_VERSION="${GLIBC_VERSION:-2.28}"
MACOS_VERSION="${MACOS_VERSION:-auto}"
LINUX_IMAGE="${LIBSEEKDB_LINUX_IMAGE:-quay.io/pypa/manylinux_2_28:2026.03.20-1}"
OUTPUT_DIR="${OUTPUT_DIR:-$ROOT/build/libseekdb-packages}"
WORK_PARENT="${WORK_DIR:-$ROOT/build/libseekdb-package-work}"
DEP_CACHE_DIR="${SEEKDB_DEP_CACHE_DIR:-}"
BUILD_TYPE="${BUILD_TYPE:-release}"
RUST_TOOLCHAIN="${RUST_TOOLCHAIN:-system}"
RUST_MIRROR="${RUST_MIRROR:-ustc}"
STRIP_BINARIES="${STRIP_BINARIES:-1}"
KEEP_WORK="${KEEP_WORK:-0}"
FORCE="${FORCE:-0}"
SEEKDB_CMAKE_EXTRA=()
BINDINGS_CMAKE_EXTRA=()
ORIGINAL_ARGS=("$@")

die() {
  echo "error: $*" >&2
  exit 1
}

usage() {
  cat <<'EOF'
Usage: build-libseekdb-package.sh --version VERSION [options]

Release identity:
  --version VERSION          Product version, for example 1.3.0 (required)
  --revision REVISION        Packaging correction number (default: 1)

Source revisions:
  --seekdb-ref REF           oceanbase/seekdb branch, tag, or commit (default: master)
  --seekdb-url URL           oceanbase/seekdb git URL
  --bindings-ref REF         seekdb-bindings branch, tag, or commit (default: main)
  --bindings-url URL         seekdb-bindings git URL

Target platform (the target must match the native builder):
  --platform PLATFORM       auto | linux | macos (default: auto)
  --arch ARCH               auto | x86_64 | aarch64 | arm64 (default: auto)
  --glibc-version VERSION   Linux ABI baseline recorded in the name (default: 2.28)
  --linux-image IMAGE       Linux build image (default: manylinux_2_28)
  --macos-version VERSION   Require and record macOS major.minor, e.g. 15.6

Build/package options:
  --output-dir DIR          Archive output directory
  --work-dir DIR            Parent directory for temporary work
  --dep-cache-dir DIR       Persistent seekdb dependency cache
  --build-type TYPE         seekdb build type: release | debug (default: release)
  --rust-toolchain NAME     Rust toolchain for seekdb (default: system)
  --rust-mirror MIRROR      ustc | none (default: ustc)
  --seekdb-cmake-arg ARG    Extra seekdb build.sh CMake argument (repeatable)
  --bindings-cmake-arg ARG  Extra seekdb-bindings CMake argument (repeatable)
  --container-runtime CMD   docker | podman (Linux only; auto-detected)
  --no-strip                Keep debug symbols in shipped binaries
  --keep-work               Keep the assembled package directory after completion
  --force                   Replace an existing archive with the same identity
  -h, --help                Show this help

Outputs:
  libseekdb-VERSION-rREVISION-PLATFORM-sdbSHA-bndSHA.tar.gz
  libseekdb-VERSION-rREVISION-PLATFORM-sdbSHA-bndSHA.tar.gz.sha256

Linux platform tags are linux-x86_64-glibc2.28 or
linux-aarch64-glibc2.28. macOS tags include the build OS, for example
macos15.6-arm64.
EOF
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --version) VERSION="${2:?--version requires a value}"; shift 2 ;;
      --revision) REVISION="${2:?--revision requires a value}"; shift 2 ;;
      --seekdb-ref) SEEKDB_REF="${2:?--seekdb-ref requires a value}"; shift 2 ;;
      --seekdb-url) SEEKDB_URL="${2:?--seekdb-url requires a value}"; shift 2 ;;
      --bindings-ref) BINDINGS_REF="${2:?--bindings-ref requires a value}"; shift 2 ;;
      --bindings-url) BINDINGS_URL="${2:?--bindings-url requires a value}"; shift 2 ;;
      --platform) TARGET_PLATFORM="${2:?--platform requires a value}"; shift 2 ;;
      --arch) TARGET_ARCH="${2:?--arch requires a value}"; shift 2 ;;
      --glibc-version) GLIBC_VERSION="${2:?--glibc-version requires a value}"; shift 2 ;;
      --linux-image) LINUX_IMAGE="${2:?--linux-image requires a value}"; shift 2 ;;
      --macos-version) MACOS_VERSION="${2:?--macos-version requires a value}"; shift 2 ;;
      --output-dir) OUTPUT_DIR="${2:?--output-dir requires a value}"; shift 2 ;;
      --work-dir) WORK_PARENT="${2:?--work-dir requires a value}"; shift 2 ;;
      --dep-cache-dir) DEP_CACHE_DIR="${2:?--dep-cache-dir requires a value}"; shift 2 ;;
      --build-type) BUILD_TYPE="${2:?--build-type requires a value}"; shift 2 ;;
      --rust-toolchain) RUST_TOOLCHAIN="${2:?--rust-toolchain requires a value}"; shift 2 ;;
      --rust-mirror) RUST_MIRROR="${2:?--rust-mirror requires a value}"; shift 2 ;;
      --seekdb-cmake-arg)
        SEEKDB_CMAKE_EXTRA+=("${2:?--seekdb-cmake-arg requires a value}")
        shift 2
        ;;
      --bindings-cmake-arg)
        BINDINGS_CMAKE_EXTRA+=("${2:?--bindings-cmake-arg requires a value}")
        shift 2
        ;;
      --container-runtime)
        CONTAINER_RUNTIME="${2:?--container-runtime requires a value}"
        export CONTAINER_RUNTIME
        shift 2
        ;;
      --no-strip) STRIP_BINARIES=0; shift ;;
      --keep-work) KEEP_WORK=1; shift ;;
      --force) FORCE=1; shift ;;
      -h|--help) usage; exit 0 ;;
      *) die "unknown argument: $1 (try --help)" ;;
    esac
  done
}

validate_safe_component() {
  local name="$1"
  local value="$2"
  [[ -n "$value" ]] || die "$name must not be empty"
  case "$value" in
    *[!A-Za-z0-9._+~-]*) die "$name contains unsupported characters: $value" ;;
  esac
}

validate_single_line() {
  local name="$1"
  local value="$2"
  case "$value" in
    *$'\n'*|*$'\r'*) die "$name must be a single line" ;;
  esac
}

detect_host_platform() {
  case "$(uname -s)" in
    Linux) echo linux ;;
    Darwin) echo macos ;;
    *) die "unsupported host OS: $(uname -s)" ;;
  esac
}

normalize_arch() {
  local platform="$1"
  local arch="$2"
  case "$platform:$arch" in
    linux:x86_64|linux:amd64) echo x86_64 ;;
    linux:aarch64|linux:arm64) echo aarch64 ;;
    macos:arm64|macos:aarch64) echo arm64 ;;
    macos:x86_64|macos:amd64) echo x86_64 ;;
    *) die "unsupported architecture for $platform: $arch" ;;
  esac
}

hash_file() {
  local file="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$file" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$file" | awk '{print $1}'
  else
    die "neither sha256sum nor shasum is available"
  fi
}

checkout_ref() {
  local url="$1"
  local ref="$2"
  local dest="$3"
  local recursive="$4"

  mkdir -p "$dest"
  git -C "$dest" init -q
  git -C "$dest" remote add origin "$url"
  echo "==> fetching $url @ $ref" >&2
  local attempt fetched=0
  for attempt in 1 2 3; do
    if git -C "$dest" fetch --depth 1 origin "$ref"; then
      fetched=1
      break
    fi
    echo "warning: git fetch failed (attempt $attempt/3); retrying" >&2
    sleep $((attempt * 2))
  done
  [[ "$fetched" == "1" ]] || die "failed to fetch $url @ $ref after 3 attempts"
  git -C "$dest" checkout -q --detach FETCH_HEAD
  if [[ "$recursive" == "1" ]]; then
    # The C library only needs MariaDB Connector/C. googletest is also
    # initialized so older bindings refs that configure it unconditionally
    # remain buildable. Do not recursively clone the unused OpenSSL tree.
    git -C "$dest" submodule update --init --depth 1 \
      deps/mariadb-connector-c deps/googletest \
      || git -C "$dest" submodule update --init \
        deps/mariadb-connector-c deps/googletest
  fi
}

copy_source_licenses() {
  local source_dir="$1"
  local destination="$2"
  local prefix="$3"
  local file
  mkdir -p "$destination"
  for file in "$source_dir"/LICENSE* "$source_dir"/NOTICE* "$source_dir"/COPYING*; do
    [[ -f "$file" ]] || continue
    cp "$file" "$destination/${prefix}-$(basename "$file")"
  done
}

write_arg_files() {
  : > "$WORK_DIR/seekdb-cmake-args"
  : > "$WORK_DIR/bindings-cmake-args"
  local arg
  for arg in ${SEEKDB_CMAKE_EXTRA[@]+"${SEEKDB_CMAKE_EXTRA[@]}"}; do
    validate_single_line "seekdb CMake argument" "$arg"
    printf '%s\n' "$arg" >> "$WORK_DIR/seekdb-cmake-args"
  done
  for arg in ${BINDINGS_CMAKE_EXTRA[@]+"${BINDINGS_CMAKE_EXTRA[@]}"}; do
    validate_single_line "bindings CMake argument" "$arg"
    printf '%s\n' "$arg" >> "$WORK_DIR/bindings-cmake-args"
  done
}

build_linux() {
  # shellcheck source=lib/container-runtime.sh
  source "$ROOT/scripts/lib/container-runtime.sh"
  detect_container_runtime ${ORIGINAL_ARGS[@]+"${ORIGINAL_ARGS[@]}"}

  echo "==> Linux build: image=$LINUX_IMAGE runtime=$CONTAINER_RUNTIME arch=$TARGET_ARCH" >&2
  # -i is required because the container build program is supplied on stdin.
  "$CONTAINER_RUNTIME" run --rm -i \
    -e "HOST_UID=$(id -u)" \
    -e "HOST_GID=$(id -g)" \
    -e "TARGET_ARCH=$TARGET_ARCH" \
    -e "EXPECTED_GLIBC=$GLIBC_VERSION" \
    -e "SEEKDB_URL=$SEEKDB_URL" \
    -e "SEEKDB_REF=$SEEKDB_REF" \
    -e "BINDINGS_URL=$BINDINGS_URL" \
    -e "BINDINGS_REF=$BINDINGS_REF" \
    -e "BUILD_TYPE=$BUILD_TYPE" \
    -e "RUST_TOOLCHAIN=$RUST_TOOLCHAIN" \
    -e "RUST_MIRROR=$RUST_MIRROR" \
    -e "STRIP_BINARIES=$STRIP_BINARIES" \
    -v "$WORK_DIR:/out" \
    -v "$DEP_CACHE_DIR:/dep-cache" \
    "$LINUX_IMAGE" bash -s <<'LINUX_BUILD'
set -euo pipefail

fail() {
  echo "error: $*" >&2
  exit 1
}

export DEP_CACHE_DIR=/dep-cache
trap 'chown -R "$HOST_UID:$HOST_GID" /out /dep-cache 2>/dev/null || true' EXIT

command -v yum >/dev/null 2>&1 || fail "the Linux image must provide yum"
if [[ ! -f /opt/libseekdb-builder-ready ]]; then
  yum install -y cargo cpio curl file git libaio libaio-devel openssl-devel \
    patchelf rust wget >/dev/null
fi
command -v cmake >/dev/null 2>&1 || fail "cmake is missing from the Linux image"
for required_command in cargo cpio curl file git patchelf rustc wget; do
  command -v "$required_command" >/dev/null 2>&1 \
    || fail "missing command in Linux builder: $required_command"
done
[[ -f /usr/include/openssl/ssl.h ]] \
  || fail "OpenSSL development headers are missing from the Linux builder"

case "$(uname -m)" in
  x86_64|amd64) actual_arch=x86_64 ;;
  aarch64|arm64) actual_arch=aarch64 ;;
  *) fail "unsupported container architecture: $(uname -m)" ;;
esac
[[ "$actual_arch" == "$TARGET_ARCH" ]] \
  || fail "container architecture $actual_arch does not match requested $TARGET_ARCH"

export CARGO_HOME=/opt/cargo
mkdir -p "$CARGO_HOME"
case "$RUST_MIRROR" in
  ustc)
    cat > "$CARGO_HOME/config.toml" <<'EOF'
[source.crates-io]
replace-with = "ustc"

[source.ustc]
registry = "sparse+https://mirrors.ustc.edu.cn/crates.io-index/"

[registries.ustc]
index = "sparse+https://mirrors.ustc.edu.cn/crates.io-index/"

[net]
git-fetch-with-cli = true
EOF
    export RUSTUP_DIST_SERVER=https://mirrors.ustc.edu.cn/rust-static
    export RUSTUP_UPDATE_ROOT=https://mirrors.ustc.edu.cn/rust-static/rustup
    ;;
  none)
    export RUSTUP_DIST_SERVER=https://static.rust-lang.org
    export RUSTUP_UPDATE_ROOT=https://static.rust-lang.org/rustup
    ;;
  *) fail "unsupported Rust mirror: $RUST_MIRROR" ;;
esac

if [[ "$RUST_TOOLCHAIN" != "system" ]]; then
  export RUSTUP_HOME=/opt/rustup
  export PATH="$CARGO_HOME/bin:$PATH"
  echo "==> installing Rust toolchain $RUST_TOOLCHAIN" >&2
  case "$actual_arch" in
    x86_64) rustup_target=x86_64-unknown-linux-gnu ;;
    aarch64) rustup_target=aarch64-unknown-linux-gnu ;;
  esac
  curl --proto '=https' --tlsv1.2 -fsSL \
    "$RUSTUP_UPDATE_ROOT/dist/$rustup_target/rustup-init" \
    -o /tmp/rustup-init
  chmod +x /tmp/rustup-init
  /tmp/rustup-init -y --profile minimal --default-toolchain "$RUST_TOOLCHAIN"
fi
rustc_version="$(rustc --version)"

actual_glibc="$(getconf GNU_LIBC_VERSION | awk '{print $2}')"
[[ "$actual_glibc" == "$EXPECTED_GLIBC" ]] \
  || fail "container glibc $actual_glibc does not match requested $EXPECTED_GLIBC"

checkout_ref() {
  local url="$1"
  local ref="$2"
  local dest="$3"
  local recursive="$4"
  git init -q "$dest"
  git -C "$dest" remote add origin "$url"
  echo "==> fetching $url @ $ref" >&2
  local attempt fetched=0
  for attempt in 1 2 3; do
    if git -C "$dest" fetch --depth 1 origin "$ref"; then
      fetched=1
      break
    fi
    echo "warning: git fetch failed (attempt $attempt/3); retrying" >&2
    sleep $((attempt * 2))
  done
  [[ "$fetched" == "1" ]] || fail "failed to fetch $url @ $ref after 3 attempts"
  git -C "$dest" checkout -q --detach FETCH_HEAD
  if [[ "$recursive" == "1" ]]; then
    git -C "$dest" submodule update --init --depth 1 \
      deps/mariadb-connector-c deps/googletest \
      || git -C "$dest" submodule update --init \
        deps/mariadb-connector-c deps/googletest
  fi
}

checkout_ref "$SEEKDB_URL" "$SEEKDB_REF" /tmp/seekdb-src 0
checkout_ref "$BINDINGS_URL" "$BINDINGS_REF" /tmp/bindings-src 1
seekdb_commit="$(git -C /tmp/seekdb-src rev-parse HEAD)"
bindings_commit="$(git -C /tmp/bindings-src rev-parse HEAD)"
bindings_build_overlay=none

# Older bindings revisions configured the Python extension unconditionally.
# Adapt only the temporary checkout so historical refs with the current source
# layout can still be used for a C-only release. The resolved source SHA remains
# unchanged and the overlay is recorded in BUILD-INFO.txt.
if ! grep -q 'SEEKDB_BUILD_PYTHON' /tmp/bindings-src/CMakeLists.txt; then
  python_subdir_count="$(grep -Ec '^[[:space:]]*add_subdirectory\(python\)[[:space:]]*$' \
    /tmp/bindings-src/CMakeLists.txt || true)"
  [[ "$python_subdir_count" == "1" ]] \
    || fail "selected bindings ref cannot be adapted for a C-only build"
  awk '
    /^[[:space:]]*add_subdirectory\(python\)[[:space:]]*$/ {
      print "option(SEEKDB_BUILD_PYTHON \"Build the pylibseekdb Python extension\" ON)"
      print "if(SEEKDB_BUILD_PYTHON)"
      print "    add_subdirectory(python)"
      print "endif()"
      next
    }
    { print }
  ' /tmp/bindings-src/CMakeLists.txt > /tmp/bindings-src/CMakeLists.txt.package
  mv /tmp/bindings-src/CMakeLists.txt.package /tmp/bindings-src/CMakeLists.txt
  bindings_build_overlay=c-only-python-guard-v1
fi

seekdb_args=()
while IFS= read -r arg || [[ -n "$arg" ]]; do
  [[ -n "$arg" ]] && seekdb_args+=("$arg")
done < /out/seekdb-cmake-args

bindings_args=()
while IFS= read -r arg || [[ -n "$arg" ]]; do
  [[ -n "$arg" ]] && bindings_args+=("$arg")
done < /out/bindings-cmake-args

echo "==> building seekdb ($BUILD_TYPE)" >&2
(
  cd /tmp/seekdb-src
  ./build.sh "$BUILD_TYPE" --init -DBUILD_EMBED_MODE=ON \
    ${seekdb_args[@]+"${seekdb_args[@]}"} --make
)

seekdb_count="$(find "/tmp/seekdb-src/build_$BUILD_TYPE" -type f -name seekdb -perm -u+x | wc -l | tr -d ' ')"
[[ "$seekdb_count" == "1" ]] \
  || fail "expected one seekdb binary, found $seekdb_count"
seekdb_bin="$(find "/tmp/seekdb-src/build_$BUILD_TYPE" -type f -name seekdb -perm -u+x | head -n 1)"

echo "==> building libseekdb" >&2
cmake -S /tmp/bindings-src -B /tmp/bindings-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DSEEKDB_BUILD_PYTHON=OFF \
  -DSEEKDB_BIN="$seekdb_bin" \
  ${bindings_args[@]+"${bindings_args[@]}"}
cmake --build /tmp/bindings-build --target seekdb --parallel
[[ -f /tmp/bindings-build/libseekdb.so ]] \
  || fail "libseekdb.so was not produced"

stage=/out/stage
mkdir -p "$stage/bin" "$stage/include" "$stage/lib" "$stage/licenses"
cp "$seekdb_bin" "$stage/lib/seekdb"
cp /tmp/bindings-build/libseekdb.so "$stage/lib/libseekdb.so"
cp /tmp/bindings-src/lib/include/seekdb.h "$stage/include/seekdb.h"
ln -s libseekdb.so "$stage/lib/libseekdb_driver.so"
ln -s ../lib/seekdb "$stage/bin/seekdb"

copy_licenses() {
  local source_dir="$1"
  local prefix="$2"
  local file
  for file in "$source_dir"/LICENSE* "$source_dir"/NOTICE* "$source_dir"/COPYING*; do
    [[ -f "$file" ]] || continue
    cp "$file" "$stage/licenses/${prefix}-$(basename "$file")"
  done
}
copy_licenses /tmp/seekdb-src seekdb
copy_licenses /tmp/bindings-src seekdb-bindings

if [[ "$STRIP_BINARIES" == "1" ]]; then
  strip --strip-debug --strip-unneeded "$stage/lib/seekdb" "$stage/lib/libseekdb.so"
fi

is_glibc_runtime() {
  case "$1" in
    linux-vdso.so*|ld-linux*.so*|ld64.so*|libc.so*|libm.so*|libdl.so*|\
    libpthread.so*|librt.so*|libresolv.so*|libutil.so*|libanl.so*|\
    libBrokenLocale.so*|libnss_*.so*|libthread_db.so*) return 0 ;;
    *) return 1 ;;
  esac
}

queue=/tmp/dependency-queue
seen=/tmp/dependency-seen
: > "$queue"
: > "$seen"
printf 'library\tbuild_source\tsystem_package\n' > "$stage/DEPENDENCIES.txt"
printf '%s\n' "$stage/lib/seekdb" "$stage/lib/libseekdb.so" >> "$queue"
queue_index=1
while :; do
  current="$(sed -n "${queue_index}p" "$queue")"
  [[ -n "$current" ]] || break
  queue_index=$((queue_index + 1))

  ldd_output="$(ldd "$current" 2>&1)" || fail "ldd failed for $current: $ldd_output"
  if printf '%s\n' "$ldd_output" | grep -q 'not found'; then
    fail "unresolved dependency for $current: $ldd_output"
  fi

  printf '%s\n' "$ldd_output" | awk '
    /=>/ {
      for (i = 1; i <= NF; i++) if ($i ~ /^\//) { print $i; break }
      next
    }
    /^[[:space:]]*\// { print $1 }
  ' | while IFS= read -r dep; do
    [[ -n "$dep" ]] || continue
    base="$(basename "$dep")"
    is_glibc_runtime "$base" && continue

    if grep -Fqx "$base" "$seen"; then
      [[ -f "$stage/lib/$base" ]] || fail "dependency tracking lost $base"
      cmp -s "$dep" "$stage/lib/$base" \
        || fail "different libraries share dependency name $base"
      continue
    fi

    cp -L "$dep" "$stage/lib/$base"
    if ! owner="$(rpm -qf "$dep" 2>/dev/null)"; then
      owner=""
    fi
    if [[ -z "$owner" ]] && command -v ldconfig >/dev/null 2>&1; then
      system_dep="$(ldconfig -p 2>/dev/null \
        | awk -v name="$base" '$1 == name { print $NF; exit }')"
      if [[ -n "$system_dep" && -f "$system_dep" ]]; then
        if ! owner="$(rpm -qf "$system_dep" 2>/dev/null)"; then
          owner=""
        fi
      fi
    fi
    [[ -n "$owner" ]] || owner=unmanaged

    if [[ "$owner" != "unmanaged" ]]; then
      safe_owner="$(printf '%s' "$owner" | tr -c 'A-Za-z0-9._+-' '_')"
      while IFS= read -r license_file; do
        [[ -f "$license_file" ]] || continue
        cp -L "$license_file" \
          "$stage/licenses/runtime-${safe_owner}-$(basename "$license_file")"
      done < <(rpm -ql "$owner" 2>/dev/null \
        | grep -Ei '^/usr/share/licenses/|^/usr/share/doc/.*/(LICENSE|COPYING|NOTICE|COPYRIGHT)(\..*)?$' \
        || true)
    fi

    printf '%s\n' "$base" >> "$seen"
    printf '%s\t%s\t%s\n' "$base" "$dep" "$owner" >> "$stage/DEPENDENCIES.txt"
    printf '%s\n' "$stage/lib/$base" >> "$queue"
  done
done

while IFS= read -r binary; do
  file "$binary" | grep -q ELF || continue
  patchelf --set-rpath '$ORIGIN' "$binary"
done < <(find "$stage/lib" -maxdepth 1 -type f | sort)

max_glibc=""
while IFS= read -r binary; do
  file "$binary" | grep -q ELF || continue
  objdump -T "$binary" 2>/dev/null \
    | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' \
    | sort -V \
    | tail -n 1 >> /tmp/glibc-versions || true
done < <(find "$stage/lib" -maxdepth 1 -type f | sort)
if [[ -s /tmp/glibc-versions ]]; then
  max_glibc="$(sort -V /tmp/glibc-versions | tail -n 1)"
  highest="$(printf '%s\n' "$max_glibc" "GLIBC_$EXPECTED_GLIBC" | sort -V | tail -n 1)"
  [[ "$highest" == "GLIBC_$EXPECTED_GLIBC" ]] \
    || fail "archive requires $max_glibc, above glibc $EXPECTED_GLIBC"
fi

while IFS= read -r binary; do
  file "$binary" | grep -q ELF || continue
  verify_ldd="$(LD_LIBRARY_PATH="$stage/lib" ldd "$binary" 2>&1)" \
    || fail "packaged ldd failed for $binary: $verify_ldd"
  printf '%s\n' "$verify_ldd" | grep -q 'not found' \
    && fail "packaged dependency unresolved for $binary: $verify_ldd"
  printf '%s\n' "$verify_ldd" | awk '
    /=>/ {
      name=$1
      for (i = 1; i <= NF; i++) if ($i ~ /^\//) { print name "\t" $i; break }
    }
  ' | while IFS=$'\t' read -r name path; do
    is_glibc_runtime "$name" && continue
    case "$path" in
      "$stage/lib"/*) ;;
      *) fail "$name resolves outside the package: $path" ;;
    esac
  done
done < <(find "$stage/lib" -maxdepth 1 -type f | sort)

LD_LIBRARY_PATH="$stage/lib" "$stage/lib/seekdb" -V > "$stage/seekdb-version.txt" 2>&1 \
  || fail "packaged seekdb -V smoke test failed"

cat > /out/build.env <<EOF
SEEKDB_COMMIT=$seekdb_commit
BINDINGS_COMMIT=$bindings_commit
BINDINGS_BUILD_OVERLAY=$bindings_build_overlay
PLATFORM=linux
ARCH=$actual_arch
ABI=glibc$actual_glibc
MAX_GLIBC=$max_glibc
RUSTC_VERSION=$rustc_version
EOF

chown -R "$HOST_UID:$HOST_GID" "$stage" /out/build.env
LINUX_BUILD
}

macos_resolve_dependency() {
  local origin="$1"
  local dependency="$2"
  local executable_origin="$3"
  local suffix candidate rpath

  case "$dependency" in
    /*)
      [[ -f "$dependency" ]] && { echo "$dependency"; return 0; }
      ;;
    @loader_path/*)
      suffix="${dependency#@loader_path/}"
      candidate="$(dirname "$origin")/$suffix"
      [[ -f "$candidate" ]] && { echo "$candidate"; return 0; }
      ;;
    @executable_path/*)
      suffix="${dependency#@executable_path/}"
      candidate="$(dirname "$executable_origin")/$suffix"
      [[ -f "$candidate" ]] && { echo "$candidate"; return 0; }
      ;;
    @rpath/*)
      suffix="${dependency#@rpath/}"
      while IFS= read -r rpath; do
        case "$rpath" in
          @loader_path*) rpath="$(dirname "$origin")${rpath#@loader_path}" ;;
          @executable_path*) rpath="$(dirname "$executable_origin")${rpath#@executable_path}" ;;
        esac
        candidate="$rpath/$suffix"
        [[ -f "$candidate" ]] && { echo "$candidate"; return 0; }
      done < <(otool -l "$origin" | awk '
        $1 == "cmd" && $2 == "LC_RPATH" { found=1; next }
        found && $1 == "path" { print $2; found=0 }
      ')
      ;;
  esac

  suffix="$(basename "$dependency")"
  for candidate in \
    "$(dirname "$origin")/$suffix" \
    /opt/homebrew/lib/"$suffix" \
    /usr/local/lib/"$suffix"; do
    [[ -f "$candidate" ]] && { echo "$candidate"; return 0; }
  done
  return 1
}

is_macos_system_library() {
  case "$1" in
    /usr/lib/*|/System/Library/*) return 0 ;;
    *) return 1 ;;
  esac
}

macos_add_rpath() {
  local file="$1"
  local rpath="$2"
  if ! otool -l "$file" | awk '
      $1 == "cmd" && $2 == "LC_RPATH" { found=1; next }
      found && $1 == "path" { print $2; found=0 }
    ' | grep -Fqx "$rpath"; then
    install_name_tool -add_rpath "$rpath" "$file"
  fi
}

build_macos() {
  need_cmd git
  need_cmd cmake
  need_cmd otool
  need_cmd install_name_tool
  need_cmd codesign
  need_cmd lipo
  need_cmd cargo
  need_cmd rustc
  export CARGO_HOME="$WORK_DIR/cargo-home"
  mkdir -p "$CARGO_HOME"
  case "$RUST_MIRROR" in
    ustc)
      cat > "$CARGO_HOME/config.toml" <<'EOF'
[source.crates-io]
replace-with = "ustc"

[source.ustc]
registry = "sparse+https://mirrors.ustc.edu.cn/crates.io-index/"

[registries.ustc]
index = "sparse+https://mirrors.ustc.edu.cn/crates.io-index/"

[net]
git-fetch-with-cli = true
EOF
      export RUSTUP_DIST_SERVER=https://mirrors.ustc.edu.cn/rust-static
      export RUSTUP_UPDATE_ROOT=https://mirrors.ustc.edu.cn/rust-static/rustup
      ;;
    none)
      export RUSTUP_DIST_SERVER=https://static.rust-lang.org
      export RUSTUP_UPDATE_ROOT=https://static.rust-lang.org/rustup
      ;;
    *) die "unsupported Rust mirror: $RUST_MIRROR" ;;
  esac
  if [[ "$RUST_TOOLCHAIN" != "system" ]]; then
    need_cmd rustup
    rustup toolchain install "$RUST_TOOLCHAIN" --profile minimal
    export RUSTUP_TOOLCHAIN="$RUST_TOOLCHAIN"
  fi

  local actual_version
  actual_version="$(sw_vers -productVersion | awk -F. '{print $1 "." $2}')"
  if [[ "$MACOS_VERSION" != "auto" && "$MACOS_VERSION" != "$actual_version" ]]; then
    die "macOS builder is $actual_version, requested $MACOS_VERSION"
  fi
  MACOS_VERSION="$actual_version"

  local seekdb_src="$WORK_DIR/seekdb-src"
  local bindings_src="$WORK_DIR/bindings-src"
  checkout_ref "$SEEKDB_URL" "$SEEKDB_REF" "$seekdb_src" 0
  checkout_ref "$BINDINGS_URL" "$BINDINGS_REF" "$bindings_src" 1
  SEEKDB_COMMIT="$(git -C "$seekdb_src" rev-parse HEAD)"
  BINDINGS_COMMIT="$(git -C "$bindings_src" rev-parse HEAD)"
  BINDINGS_BUILD_OVERLAY=none

  if ! grep -q 'SEEKDB_BUILD_PYTHON' "$bindings_src/CMakeLists.txt"; then
    local python_subdir_count
    python_subdir_count="$(grep -Ec '^[[:space:]]*add_subdirectory\(python\)[[:space:]]*$' \
      "$bindings_src/CMakeLists.txt" || true)"
    [[ "$python_subdir_count" == "1" ]] \
      || die "selected bindings ref cannot be adapted for a C-only build"
    awk '
      /^[[:space:]]*add_subdirectory\(python\)[[:space:]]*$/ {
        print "option(SEEKDB_BUILD_PYTHON \"Build the pylibseekdb Python extension\" ON)"
        print "if(SEEKDB_BUILD_PYTHON)"
        print "    add_subdirectory(python)"
        print "endif()"
        next
      }
      { print }
    ' "$bindings_src/CMakeLists.txt" > "$bindings_src/CMakeLists.txt.package"
    mv "$bindings_src/CMakeLists.txt.package" "$bindings_src/CMakeLists.txt"
    BINDINGS_BUILD_OVERLAY=c-only-python-guard-v1
  fi

  echo "==> building seekdb ($BUILD_TYPE) on macOS $MACOS_VERSION" >&2
  (
    cd "$seekdb_src"
    export DEP_CACHE_DIR="$DEP_CACHE_DIR"
    ./build.sh "$BUILD_TYPE" --init -DBUILD_EMBED_MODE=ON \
      ${SEEKDB_CMAKE_EXTRA[@]+"${SEEKDB_CMAKE_EXTRA[@]}"} --make
  )

  local seekdb_count seekdb_bin
  seekdb_count="$(find "$seekdb_src/build_$BUILD_TYPE" -type f -name seekdb -perm -u+x | wc -l | tr -d ' ')"
  [[ "$seekdb_count" == "1" ]] \
    || die "expected one seekdb binary, found $seekdb_count"
  seekdb_bin="$(find "$seekdb_src/build_$BUILD_TYPE" -type f -name seekdb -perm -u+x | head -n 1)"

  echo "==> building libseekdb" >&2
  local -a macos_cmake_args=()
  if command -v brew >/dev/null 2>&1; then
    local formula openssl_prefix
    for formula in openssl@3 openssl@1.1 openssl; do
      openssl_prefix="$(brew --prefix "$formula" 2>/dev/null || true)"
      if [[ -n "$openssl_prefix" && -f "$openssl_prefix/include/openssl/ssl.h" ]]; then
        macos_cmake_args+=("-DOPENSSL_ROOT_DIR=$openssl_prefix")
        break
      fi
    done
  fi
  cmake -S "$bindings_src" -B "$WORK_DIR/bindings-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DSEEKDB_BUILD_PYTHON=OFF \
    -DSEEKDB_BIN="$seekdb_bin" \
    -DWITH_EXTERNAL_ZLIB=YES \
    ${macos_cmake_args[@]+"${macos_cmake_args[@]}"} \
    ${BINDINGS_CMAKE_EXTRA[@]+"${BINDINGS_CMAKE_EXTRA[@]}"}
  cmake --build "$WORK_DIR/bindings-build" --target seekdb --parallel
  [[ -f "$WORK_DIR/bindings-build/libseekdb.dylib" ]] \
    || die "libseekdb.dylib was not produced"

  STAGE_DIR="$WORK_DIR/stage"
  mkdir -p "$STAGE_DIR/bin" "$STAGE_DIR/include" "$STAGE_DIR/lib" "$STAGE_DIR/licenses"
  cp "$seekdb_bin" "$STAGE_DIR/lib/seekdb"
  cp "$WORK_DIR/bindings-build/libseekdb.dylib" "$STAGE_DIR/lib/libseekdb.dylib"
  cp "$bindings_src/lib/include/seekdb.h" "$STAGE_DIR/include/seekdb.h"
  ln -s libseekdb.dylib "$STAGE_DIR/lib/libseekdb_driver.dylib"
  ln -s ../lib/seekdb "$STAGE_DIR/bin/seekdb"
  copy_source_licenses "$seekdb_src" "$STAGE_DIR/licenses" seekdb
  copy_source_licenses "$bindings_src" "$STAGE_DIR/licenses" seekdb-bindings

  if [[ "$STRIP_BINARIES" == "1" ]]; then
    strip -S -x "$STAGE_DIR/lib/seekdb" "$STAGE_DIR/lib/libseekdb.dylib"
  fi

  local queue="$WORK_DIR/macos-dependency-queue"
  local seen="$WORK_DIR/macos-dependency-seen"
  local inventory="$STAGE_DIR/DEPENDENCIES.txt"
  : > "$queue"
  : > "$seen"
  : > "$inventory"
  printf '%s|%s|%s\n' \
    "$STAGE_DIR/lib/seekdb" "$seekdb_bin" "$seekdb_bin" \
    "$STAGE_DIR/lib/libseekdb.dylib" "$WORK_DIR/bindings-build/libseekdb.dylib" "$seekdb_bin" \
    >> "$queue"

  local queue_index=1 record destination origin executable_origin dependency resolved base
  while :; do
    record="$(sed -n "${queue_index}p" "$queue")"
    [[ -n "$record" ]] || break
    queue_index=$((queue_index + 1))
    destination="${record%%|*}"
    record="${record#*|}"
    origin="${record%%|*}"
    executable_origin="${record#*|}"

    while IFS= read -r dependency; do
      [[ -n "$dependency" ]] || continue
      is_macos_system_library "$dependency" && continue
      [[ "$(basename "$dependency")" == "$(basename "$origin")" ]] && continue
      resolved="$(macos_resolve_dependency "$origin" "$dependency" "$executable_origin")" \
        || die "cannot resolve $dependency required by $origin"
      base="$(basename "$resolved")"

      if grep -Fqx "$base" "$seen"; then
        cmp -s "$resolved" "$STAGE_DIR/lib/$base" \
          || die "different libraries share dependency name $base"
        continue
      fi

      cp -L "$resolved" "$STAGE_DIR/lib/$base"
      printf '%s\n' "$base" >> "$seen"
      printf '%s\t%s\n' "$base" "$resolved" >> "$inventory"
      printf '%s|%s|%s\n' \
        "$STAGE_DIR/lib/$base" "$resolved" "$executable_origin" >> "$queue"
    done < <(otool -L "$origin" | sed -n '2,$p' | awk '{print $1}')
  done

  local file new_dependency
  while IFS= read -r file; do
    while IFS= read -r dependency; do
      [[ -n "$dependency" ]] || continue
      is_macos_system_library "$dependency" && continue
      [[ "$(basename "$dependency")" == "$(basename "$file")" ]] && continue
      base="$(basename "$dependency")"
      [[ -f "$STAGE_DIR/lib/$base" ]] \
        || die "packaged dependency is missing: $base (from $file)"
      new_dependency="@rpath/$base"
      install_name_tool -change "$dependency" "$new_dependency" "$file"
    done < <(otool -L "$file" | sed -n '2,$p' | awk '{print $1}')

    case "$file" in
      *.dylib) install_name_tool -id "@rpath/$(basename "$file")" "$file" ;;
    esac
    if [[ "$file" == "$STAGE_DIR/lib/seekdb" ]]; then
      macos_add_rpath "$file" '@executable_path'
      macos_add_rpath "$file" '@executable_path/../lib'
    else
      macos_add_rpath "$file" '@loader_path'
    fi
  done < <(find "$STAGE_DIR/lib" -maxdepth 1 -type f | sort)

  while IFS= read -r file; do
    lipo -archs "$file" | tr ' ' '\n' | grep -Fqx "$TARGET_ARCH" \
      || die "$file does not contain requested architecture $TARGET_ARCH"
    codesign --force --sign - --timestamp=none "$file" >/dev/null
  done < <(find "$STAGE_DIR/lib" -maxdepth 1 -type f | sort)

  while IFS= read -r file; do
    while IFS= read -r dependency; do
      [[ -n "$dependency" ]] || continue
      is_macos_system_library "$dependency" && continue
      case "$dependency" in
        @rpath/*)
          [[ -f "$STAGE_DIR/lib/${dependency#@rpath/}" ]] \
            || die "missing packaged dependency $dependency for $file"
          ;;
        *) die "dependency still points outside the package: $dependency ($file)" ;;
      esac
    done < <(otool -L "$file" | sed -n '2,$p' | awk '{print $1}')
  done < <(find "$STAGE_DIR/lib" -maxdepth 1 -type f | sort)

  DYLD_LIBRARY_PATH="$STAGE_DIR/lib" "$STAGE_DIR/lib/seekdb" -V \
    > "$STAGE_DIR/seekdb-version.txt" 2>&1 \
    || die "packaged seekdb -V smoke test failed"

  PLATFORM_TAG="macos${MACOS_VERSION}-${TARGET_ARCH}"
  ABI="macos${MACOS_VERSION}"
  MAX_GLIBC="n/a"
  RUSTC_VERSION="$(rustc --version)"
}

metadata_value() {
  local key="$1"
  sed -n "s/^${key}=//p" "$WORK_DIR/build.env" | head -n 1
}

write_package_metadata() {
  local package_root="$1"
  local built_at
  built_at="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

  cat > "$package_root/BUILD-INFO.txt" <<EOF
package=$PACKAGE_NAME
version=$VERSION
revision=$REVISION
platform=$TARGET_PLATFORM
architecture=$TARGET_ARCH
abi=$ABI
built_at_utc=$built_at
seekdb_repository=$SEEKDB_URL
seekdb_ref=$SEEKDB_REF
seekdb_commit=$SEEKDB_COMMIT
seekdb_bindings_repository=$BINDINGS_URL
seekdb_bindings_ref=$BINDINGS_REF
seekdb_bindings_commit=$BINDINGS_COMMIT
seekdb_bindings_build_overlay=$BINDINGS_BUILD_OVERLAY
build_type=$BUILD_TYPE
rust_toolchain=$RUST_TOOLCHAIN
rust_mirror=$RUST_MIRROR
rustc_version=$RUSTC_VERSION
stripped=$STRIP_BINARIES
max_glibc_symbol=$MAX_GLIBC
EOF

  cat > "$package_root/README.txt" <<EOF
libseekdb C SDK $VERSION-r$REVISION

Contents:
  include/seekdb.h            Public C API
  lib/libseekdb.*             C shared library
  lib/libseekdb_driver.*      Compatibility symlink
  lib/seekdb                  Embedded seekdb server (kept next to libseekdb)
  bin/seekdb                  Convenience symlink to ../lib/seekdb
  lib/<dependencies>          Bundled non-system runtime dependencies
  DEPENDENCIES.txt            Bundled dependency provenance
  BUILD-INFO.txt              Exact source commits and build platform
  SHA256SUMS                  Per-file checksums

Keep lib/seekdb next to lib/libseekdb: the C library discovers the server in
its own directory. Linux glibc and the dynamic loader are intentionally not
bundled; the platform tag is the minimum glibc ABI for this archive.
EOF
}

write_inner_checksums() {
  local package_root="$1"
  local relative hash
  : > "$package_root/SHA256SUMS"
  (
    cd "$package_root"
    find . -type f ! -name SHA256SUMS | LC_ALL=C sort
  ) | while IFS= read -r relative; do
    relative="${relative#./}"
    hash="$(hash_file "$package_root/$relative")"
    printf '%s  %s\n' "$hash" "$relative" >> "$package_root/SHA256SUMS"
  done
}

normalize_package_permissions() {
  local package_root="$1"
  find "$package_root" -type d -exec chmod 0755 {} +
  find "$package_root" -type f -exec chmod 0644 {} +
  find "$package_root/lib" -maxdepth 1 -type f -exec chmod 0755 {} +
}

create_archive() {
  local package_root="$1"
  local archive="$OUTPUT_DIR/$PACKAGE_NAME.tar.gz"
  local checksum="$archive.sha256"
  local archive_tmp="$WORK_DIR/$PACKAGE_NAME.tar.gz.tmp"

  mkdir -p "$OUTPUT_DIR"
  if [[ -e "$archive" || -e "$checksum" ]]; then
    [[ "$FORCE" == "1" ]] \
      || die "output already exists (use --force): $archive"
  fi

  COPYFILE_DISABLE=1 tar -C "$(dirname "$package_root")" \
    -czf "$archive_tmp" "$(basename "$package_root")"
  mv -f "$archive_tmp" "$archive"
  printf '%s  %s\n' "$(hash_file "$archive")" "$(basename "$archive")" > "$checksum"
  chmod 0644 "$archive" "$checksum"

  echo "==> archive: $archive"
  echo "==> checksum: $checksum"
}

main() {
  parse_args "$@"
  [[ -n "$VERSION" ]] || die "--version is required"
  validate_safe_component version "$VERSION"
  validate_safe_component revision "$REVISION"
  validate_safe_component glibc-version "$GLIBC_VERSION"
  [[ "$GLIBC_VERSION" =~ ^[0-9]+\.[0-9]+$ ]] \
    || die "glibc version must be major.minor (for example 2.28)"
  if [[ "$MACOS_VERSION" != "auto" ]]; then
    [[ "$MACOS_VERSION" =~ ^[0-9]+\.[0-9]+$ ]] \
      || die "macOS version must be major.minor (for example 15.6)"
  fi
  validate_safe_component rust-toolchain "$RUST_TOOLCHAIN"
  [[ "$RUST_MIRROR" == "ustc" || "$RUST_MIRROR" == "none" ]] \
    || die "unsupported Rust mirror: $RUST_MIRROR"
  [[ "$BUILD_TYPE" == "release" || "$BUILD_TYPE" == "debug" ]] \
    || die "unsupported build type: $BUILD_TYPE"
  [[ "$STRIP_BINARIES" == "0" || "$STRIP_BINARIES" == "1" ]] \
    || die "STRIP_BINARIES must be 0 or 1"
  validate_single_line seekdb-ref "$SEEKDB_REF"
  validate_single_line bindings-ref "$BINDINGS_REF"
  validate_single_line seekdb-url "$SEEKDB_URL"
  validate_single_line bindings-url "$BINDINGS_URL"

  local host_platform host_arch
  host_platform="$(detect_host_platform)"
  if [[ "$TARGET_PLATFORM" == "auto" ]]; then
    TARGET_PLATFORM="$host_platform"
  fi
  [[ "$TARGET_PLATFORM" == "linux" || "$TARGET_PLATFORM" == "macos" ]] \
    || die "unsupported platform: $TARGET_PLATFORM"
  [[ "$TARGET_PLATFORM" == "$host_platform" ]] \
    || die "$TARGET_PLATFORM packages must be built on $TARGET_PLATFORM (host=$host_platform)"

  host_arch="$(normalize_arch "$TARGET_PLATFORM" "$(uname -m)")"
  if [[ "$TARGET_ARCH" == "auto" ]]; then
    TARGET_ARCH="$host_arch"
  else
    TARGET_ARCH="$(normalize_arch "$TARGET_PLATFORM" "$TARGET_ARCH")"
  fi
  [[ "$TARGET_ARCH" == "$host_arch" ]] \
    || die "native builder architecture is $host_arch, requested $TARGET_ARCH"

  if [[ -z "$DEP_CACHE_DIR" ]]; then
    DEP_CACHE_DIR="$ROOT/build/libseekdb-dep-cache/${TARGET_PLATFORM}-${TARGET_ARCH}"
  fi

  mkdir -p "$WORK_PARENT" "$DEP_CACHE_DIR"
  WORK_DIR="$(mktemp -d "$WORK_PARENT/work.XXXXXX")"
  export WORK_DIR
  trap 'rc=$?; trap - EXIT; if [[ "$KEEP_WORK" == "1" ]]; then echo "==> kept work directory: $WORK_DIR" >&2; else rm -rf -- "$WORK_DIR"; fi; exit "$rc"' EXIT
  write_arg_files

  echo "==> libseekdb package build"
  echo "    seekdb:  $SEEKDB_URL @ $SEEKDB_REF"
  echo "    bindings: $BINDINGS_URL @ $BINDINGS_REF"
  echo "    target:   $TARGET_PLATFORM/$TARGET_ARCH"

  if [[ "$TARGET_PLATFORM" == "linux" ]]; then
    build_linux
    [[ -f "$WORK_DIR/build.env" ]] \
      || die "Linux container did not produce build metadata"
    SEEKDB_COMMIT="$(metadata_value SEEKDB_COMMIT)"
    BINDINGS_COMMIT="$(metadata_value BINDINGS_COMMIT)"
    BINDINGS_BUILD_OVERLAY="$(metadata_value BINDINGS_BUILD_OVERLAY)"
    ABI="$(metadata_value ABI)"
    MAX_GLIBC="$(metadata_value MAX_GLIBC)"
    RUSTC_VERSION="$(metadata_value RUSTC_VERSION)"
    PLATFORM_TAG="linux-${TARGET_ARCH}-${ABI}"
    STAGE_DIR="$WORK_DIR/stage"
  else
    build_macos
  fi

  [[ "$SEEKDB_COMMIT" =~ ^[0-9a-f]{40}$ ]] || die "invalid resolved seekdb commit"
  [[ "$BINDINGS_COMMIT" =~ ^[0-9a-f]{40}$ ]] || die "invalid resolved bindings commit"
  PACKAGE_NAME="libseekdb-${VERSION}-r${REVISION}-${PLATFORM_TAG}-sdb${SEEKDB_COMMIT:0:12}-bnd${BINDINGS_COMMIT:0:12}"
  local package_root="$WORK_DIR/$PACKAGE_NAME"
  mv "$STAGE_DIR" "$package_root"

  write_package_metadata "$package_root"
  write_inner_checksums "$package_root"
  normalize_package_permissions "$package_root"
  create_archive "$package_root"
}

main "$@"
