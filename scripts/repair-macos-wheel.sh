#!/usr/bin/env bash
# Run delocate, then collapse Homebrew RE2's dynamic Abseil dependency fanout.

set -euo pipefail

[[ $# -eq 3 ]] || {
  echo "usage: repair-macos-wheel.sh WHEEL DEST_DIR DELOCATE_ARCHS" >&2
  exit 2
}

wheel="$1"
dest_dir="$2"
delocate_archs="$3"
script_dir="$(cd "$(dirname "$0")" && pwd)"
merged_re2="${SEEKDB_RE2_BUNDLE_DYLIB:-}"
python_bin="${SEEKDB_WHEEL_REWRITE_PYTHON:-python3}"
max_bundled_dylibs="${MACOS_MAX_BUNDLED_DYLIBS:-20}"

[[ -f "$wheel" ]] || { echo "error: wheel not found: $wheel" >&2; exit 1; }
[[ -f "$merged_re2" ]] || {
  echo "error: SEEKDB_RE2_BUNDLE_DYLIB is missing or not a file: $merged_re2" >&2
  exit 1
}
command -v delocate-wheel >/dev/null 2>&1 || { echo "error: delocate-wheel not found" >&2; exit 1; }
command -v "$python_bin" >/dev/null 2>&1 || { echo "error: Python not found: $python_bin" >&2; exit 1; }

repair_dir="$(mktemp -d "${TMPDIR:-/tmp}/pylibseekdb-delocate.XXXXXX")"
cleanup() { rm -rf "$repair_dir"; }
trap cleanup EXIT

delocate-wheel --require-archs "$delocate_archs" -w "$repair_dir" -v "$wheel"

repaired_wheels=("$repair_dir"/*.whl)
[[ ${#repaired_wheels[@]} -eq 1 && -f "${repaired_wheels[0]}" ]] || {
  echo "error: delocate produced ${#repaired_wheels[@]} wheels, expected one" >&2
  exit 1
}
repaired_wheel="${repaired_wheels[0]}"

"$python_bin" "$script_dir/rewrite-macos-wheel.py" \
  --max-bundled-dylibs "$max_bundled_dylibs" \
  "$repaired_wheel" "$merged_re2"

if command -v delocate-listdeps >/dev/null 2>&1; then
  dependency_report="$(delocate-listdeps "$repaired_wheel")"
  if printf '%s\n' "$dependency_report" | grep -Eq '/opt/homebrew|/usr/local|libabsl_'; then
    echo "error: repaired wheel retains forbidden dynamic dependencies" >&2
    printf '%s\n' "$dependency_report" >&2
    exit 1
  fi
fi

mkdir -p "$dest_dir"
mv -f "$repaired_wheel" "$dest_dir/"
echo "==> repaired wheel: $dest_dir/$(basename "$repaired_wheel")" >&2
