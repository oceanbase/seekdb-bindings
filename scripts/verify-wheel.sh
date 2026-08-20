#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEST_PY="${SEEKDB_TEST_PY:-$REPO_ROOT/python/tests/connection_options_test.py}"

if [ ! -f "$TEST_PY" ]; then
  echo "error: wheel test script not found: $TEST_PY" >&2
  exit 1
fi

pick_python() {
  local wheel="$1"
  if [[ "$wheel" == *cp311-* ]]; then
    if command -v python3.11 >/dev/null 2>&1; then
      command -v python3.11
      return
    fi
  fi
  if [[ "$wheel" == *abi3* ]] || [[ "$wheel" == *cp312-* ]]; then
    for py in python3.14 python3.13 python3.12 python3; do
      if command -v "$py" >/dev/null 2>&1; then
        echo "$py"
        return
      fi
    done
  fi
  command -v python3
}

verify_one_wheel() (
  local wheel="$1"
  local py
  py="$(pick_python "$wheel")"

  echo "=== verifying wheel: $wheel (python: $py) ==="

  "$py" - "$wheel" <<'PY'
import pathlib
import sys
import zipfile

wheel = pathlib.Path(sys.argv[1])
with zipfile.ZipFile(wheel) as archive:
    names = archive.namelist()
    metadata_name = next(
        (name for name in names if name.endswith(".dist-info/METADATA")),
        None,
    )
    if metadata_name is None:
        raise SystemExit(f"wheel has no METADATA: {wheel}")
    metadata = archive.read(metadata_name).decode()

if "Name: seekdb\n" not in metadata:
    raise SystemExit(f"wheel distribution is not seekdb: {wheel}")
if "seekdb/__init__.py" not in names:
    raise SystemExit(f"wheel has no seekdb package: {wheel}")
if not any(name.startswith("seekdb/seekdb_binding") for name in names):
    raise SystemExit(f"wheel has no seekdb.seekdb_binding extension: {wheel}")
if any(name.startswith("pylibseekdb/") for name in names):
    raise SystemExit(f"preview wheel unexpectedly contains pylibseekdb: {wheel}")
PY

  local tmp
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT

  "$py" -m venv "$tmp/venv"
  # shellcheck disable=SC1091
  source "$tmp/venv/bin/activate"
  python -m pip install -q --upgrade pip
  python -m pip install -q "$wheel" PyMySQL aiomysql
  python -m pip check
  python - <<'PY'
import importlib.metadata
import seekdb

assert seekdb.__version__ == importlib.metadata.version("seekdb")
PY

  mkdir -p "$tmp/work"
  (
    cd "$tmp/work"
    python "$REPO_ROOT/python/tests/compat_package_test.py"
    python "$TEST_PY"
  )
)

if [ "$#" -eq 0 ]; then
  shopt -s nullglob
  set -- "$REPO_ROOT"/build/wheelhouse/*.whl
  shopt -u nullglob
  if [ "$#" -eq 0 ]; then
    echo "error: no wheels given and none found in build/wheelhouse/" >&2
    exit 1
  fi
fi

for wheel in "$@"; do
  if [ ! -f "$wheel" ]; then
    echo "error: wheel not found: $wheel" >&2
    exit 1
  fi
  verify_one_wheel "$wheel"
done

echo "=== all wheel verification passed ==="
