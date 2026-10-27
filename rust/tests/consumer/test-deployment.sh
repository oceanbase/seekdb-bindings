#!/usr/bin/env bash
# Linux deployment checks shared by local development and CI.
set -euo pipefail

check_noexec=false
if [[ $# -gt 0 ]]; then
  if [[ $# -ne 1 || "$1" != "--check-noexec" ]]; then
    echo "Usage: bash $0 [--check-noexec]" >&2
    exit 2
  fi
  check_noexec=true
fi

consumer_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
test_dir="$(mktemp -d)"
test_dir="$(cd -- "$test_dir" && pwd)"
noexec_dir=""

cleanup() {
  local status=$?
  if [[ -n "$noexec_dir" ]] && mountpoint -q "$noexec_dir"; then
    if ! sudo umount "$noexec_dir"; then
      echo "Cannot unmount $noexec_dir; preserving $test_dir" >&2
      return 1
    fi
  fi
  rm -rf -- "$test_dir"
  return "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Own the build directory so hiding it cannot disrupt another Cargo build.
export CARGO_TARGET_DIR="$test_dir/target"
echo "Building application and dynamic-library consumers"
cargo install --path "$consumer_dir/app" --root "$test_dir/installed" --debug --locked
cargo build --manifest-path "$consumer_dir/Cargo.toml" --package seekdb-consumer-ffi --locked

app_dir="$test_dir/deployed"
mkdir "$app_dir"
cp "$CARGO_TARGET_DIR/debug/libseekdb_consumer_ffi.so" "$app_dir/"
mv "$CARGO_TARGET_DIR" "$test_dir/hidden-target"
cd "$app_dir"
app="$test_dir/installed/bin/seekdb-consumer-app"
unset LD_LIBRARY_PATH DYLD_LIBRARY_PATH

echo "Running the installed application with a fresh cache"
SEEKDB_CACHE_DIR="$app_dir/cache" "$app" "$app_dir/database-1"
rm -rf -- "$app_dir/cache"
echo "Running the installed application after cache deletion"
SEEKDB_CACHE_DIR="$app_dir/cache" "$app" "$app_dir/database-2"

echo "Loading the deployed Rust cdylib with a fresh cache"
SEEKDB_CACHE_DIR="$app_dir/ffi-cache" python3 "$consumer_dir/probe_dynamic_library.py" \
  "$app_dir/libseekdb_consumer_ffi.so" "$app_dir/ffi-database"

if "$check_noexec"; then
  echo "Checking the error reported for a noexec cache filesystem"
  noexec_dir="$test_dir/noexec"
  mkdir "$noexec_dir"
  sudo mount -t tmpfs -o noexec,nosuid,nodev,size=1m tmpfs "$noexec_dir"
  sudo chown "$(id -u):$(id -g)" "$noexec_dir"
  chmod 700 "$noexec_dir"
  if SEEKDB_CACHE_DIR="$noexec_dir" "$app" "$app_dir/noexec-database" > "$app_dir/noexec.log" 2>&1; then
    echo "Expected a noexec error" >&2
    exit 1
  fi
  grep -F 'is on a filesystem mounted with noexec' "$app_dir/noexec.log"
  grep -Fq 'SEEKDB_CACHE_DIR' "$app_dir/noexec.log"
fi

echo "Deployment checks passed"
