#!/usr/bin/env bash
# Build one RE2 dylib with the matching Homebrew Abseil version linked statically.
#
# Homebrew's dynamic RE2 links dozens of componentized Abseil dylibs. delocate
# correctly vendors that entire closure into a wheel. This helper keeps the RE2
# ABI and install name but collapses the Abseil closure into one dylib.

set -euo pipefail

WORK_DIR=""
OUTPUT=""
JOBS="${JOBS:-2}"
DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-15.0}"
BREW_BIN="${BREW_BIN:-}"
CMAKE_BIN="${CMAKE_BIN:-}"
ABSEIL_GIT_URL="${ABSEIL_GIT_URL:-git@github.com:abseil/abseil-cpp.git}"
ABSEIL_REF="${ABSEIL_REF:-}"

die() { echo "error: $*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage: build-macos-static-re2.sh --work-dir DIR --output FILE [options]

Options:
  --work-dir DIR             Isolated source/build/install directory
  --output FILE              Output merged RE2 dylib
  --jobs N                   Maximum build parallelism (default: 2)
  --deployment-target VER    Minimum macOS version (default: 15.0)
  --abseil-ref REF           Abseil tag; defaults to the installed brew version
  --abseil-git-url URL       Abseil repository (default: GitHub SSH URL)
  -h, --help                 Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --work-dir) WORK_DIR="${2:?--work-dir requires a value}"; shift 2 ;;
    --output) OUTPUT="${2:?--output requires a value}"; shift 2 ;;
    --jobs) JOBS="${2:?--jobs requires a value}"; shift 2 ;;
    --deployment-target) DEPLOYMENT_TARGET="${2:?--deployment-target requires a value}"; shift 2 ;;
    --abseil-ref) ABSEIL_REF="${2:?--abseil-ref requires a value}"; shift 2 ;;
    --abseil-git-url) ABSEIL_GIT_URL="${2:?--abseil-git-url requires a value}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[[ "$(uname -s)" == "Darwin" ]] || die "this helper must run on macOS"
[[ -n "$WORK_DIR" ]] || die "--work-dir is required"
[[ -n "$OUTPUT" ]] || die "--output is required"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "--jobs must be a positive integer"

for command_name in git clang++ otool; do
  command -v "$command_name" >/dev/null 2>&1 || die "missing command: $command_name"
done

if [[ -z "$CMAKE_BIN" ]]; then
  if command -v cmake >/dev/null 2>&1; then
    CMAKE_BIN="$(command -v cmake)"
  elif [[ -x /opt/homebrew/bin/cmake ]]; then
    CMAKE_BIN=/opt/homebrew/bin/cmake
  elif [[ -x /usr/local/bin/cmake ]]; then
    CMAKE_BIN=/usr/local/bin/cmake
  else
    die "missing command: cmake"
  fi
fi

if [[ -z "$BREW_BIN" ]]; then
  if command -v brew >/dev/null 2>&1; then
    BREW_BIN="$(command -v brew)"
  elif [[ -x /opt/homebrew/bin/brew ]]; then
    BREW_BIN=/opt/homebrew/bin/brew
  elif [[ -x /usr/local/bin/brew ]]; then
    BREW_BIN=/usr/local/bin/brew
  else
    die "Homebrew is required to locate the RE2 archive and matching Abseil version"
  fi
fi

re2_prefix="$($BREW_BIN --prefix re2)"
abseil_prefix="$($BREW_BIN --prefix abseil)"
re2_static="$re2_prefix/lib/libre2.a"
[[ -f "$re2_static" ]] || die "Homebrew RE2 static archive not found: $re2_static"
[[ -d "$abseil_prefix/lib" ]] || die "Homebrew Abseil is not installed: $abseil_prefix"

if [[ -z "$ABSEIL_REF" ]]; then
  active_abseil_prefix="$(cd "$abseil_prefix" && pwd -P)"
  ABSEIL_REF="$(basename "$active_abseil_prefix" | sed -E 's/_[0-9]+$//')"
fi
[[ -n "$ABSEIL_REF" ]] || die "could not determine the installed Abseil version"

re2_dynamic="$(find "$re2_prefix/lib" -maxdepth 1 -type f -name 'libre2.*.dylib' -print | sort | head -1)"
[[ -n "$re2_dynamic" ]] || die "versioned Homebrew RE2 dylib not found under $re2_prefix/lib"
re2_name="$(basename "$re2_dynamic")"
version_line="$(otool -L "$re2_dynamic" | sed -n '2s/.*compatibility version \([^,]*\), current version \([^)]*\).*/\1 \2/p')"
compatibility_version="${version_line%% *}"
current_version="${version_line##* }"
[[ -n "$compatibility_version" && -n "$current_version" ]] \
  || die "could not read RE2 compatibility versions from $re2_dynamic"

source_dir="$WORK_DIR/abseil-src"
build_dir="$WORK_DIR/abseil-build"
install_dir="$WORK_DIR/abseil-install"
mkdir -p "$WORK_DIR" "$(dirname "$OUTPUT")"

if [[ ! -d "$source_dir/.git" ]]; then
  # A prior interrupted clone can leave a partial path in this isolated work dir.
  rm -rf "$source_dir"
  echo "==> cloning Abseil $ABSEIL_REF through GitHub SSH" >&2
  if ! env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY \
      -u all_proxy -u ALL_PROXY \
    git clone --depth 1 --branch "$ABSEIL_REF" "$ABSEIL_GIT_URL" "$source_dir"; then
    rm -rf "$source_dir"
    if [[ "$ABSEIL_GIT_URL" == git@github.com:* ]]; then
      github_path="${ABSEIL_GIT_URL#git@github.com:}"
      fallback_url="ssh://git@ssh.github.com:443/$github_path"
      echo "==> GitHub SSH port 22 failed; retrying through port 443" >&2
      env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY \
          -u all_proxy -u ALL_PROXY \
        git clone --depth 1 --branch "$ABSEIL_REF" "$fallback_url" "$source_dir"
    else
      die "failed to clone Abseil from $ABSEIL_GIT_URL"
    fi
  fi
else
  expected_commit="$(git -C "$source_dir" rev-list -n 1 "$ABSEIL_REF" 2>/dev/null || true)"
  actual_commit="$(git -C "$source_dir" rev-parse HEAD)"
  [[ -n "$expected_commit" && "$actual_commit" == "$expected_commit" ]] \
    || die "existing Abseil checkout does not match $ABSEIL_REF: $source_dir"
fi

# These paths are owned by this isolated helper work directory.
rm -rf "$build_dir" "$install_dir"

echo "==> building static Abseil $ABSEIL_REF (-j$JOBS)" >&2
"$CMAKE_BIN" -S "$source_dir" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$install_dir" \
  -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DABSL_BUILD_TESTING=OFF \
  -DABSL_PROPAGATE_CXX_STD=ON \
  -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET"
"$CMAKE_BIN" --build "$build_dir" --target install --parallel "$JOBS"

abseil_archives=("$install_dir"/lib/libabsl_*.a)
[[ -f "${abseil_archives[0]}" ]] || die "static Abseil archives were not installed"

temporary_output="${OUTPUT}.tmp.$$"
trap 'rm -f "$temporary_output"' EXIT

echo "==> linking $re2_name with static Abseil" >&2
clang++ -dynamiclib -std=c++17 \
  "-mmacosx-version-min=$DEPLOYMENT_TARGET" \
  -Wl,-all_load \
  "$re2_static" "${abseil_archives[@]}" \
  -framework CoreFoundation \
  "-Wl,-install_name,@loader_path/$re2_name" \
  "-Wl,-compatibility_version,$compatibility_version" \
  "-Wl,-current_version,$current_version" \
  -o "$temporary_output"

if otool -L "$temporary_output" | grep -q 'libabsl_'; then
  die "merged RE2 still has dynamic Abseil dependencies"
fi
if otool -L "$temporary_output" | grep -Eq '/opt/homebrew|/usr/local'; then
  die "merged RE2 retains an absolute Homebrew dependency"
fi

mv -f "$temporary_output" "$OUTPUT"
trap - EXIT

echo "==> merged RE2 ready: $OUTPUT" >&2
ls -lh "$OUTPUT" >&2
otool -L "$OUTPUT" >&2
