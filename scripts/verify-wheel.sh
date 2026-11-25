#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEST_PY="${SEEKDB_TEST_PY:-$REPO_ROOT/python/tests/connection_options_test.py}"
MIGRATION_TEST_PY="$REPO_ROOT/python/tests/migration_test.py"
MIGRATION_INTEGRATION_TEST_PY="$REPO_ROOT/python/tests/migration_integration_test.py"

if [ ! -f "$TEST_PY" ]; then
  echo "error: wheel test script not found: $TEST_PY" >&2
  exit 1
fi
if [ ! -f "$MIGRATION_TEST_PY" ]; then
  echo "error: migration test script not found: $MIGRATION_TEST_PY" >&2
  exit 1
fi
if [ ! -f "$MIGRATION_INTEGRATION_TEST_PY" ]; then
  echo "error: migration integration test script not found: $MIGRATION_INTEGRATION_TEST_PY" >&2
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
import hashlib
import pathlib
import sys
import zipfile

wheel = pathlib.Path(sys.argv[1])
prefix = "pylibseekdb/licenses/bindings/"
with zipfile.ZipFile(wheel) as archive:
    names = archive.namelist()
    required = [prefix + name for name in (
        "LICENSE", "NOTICE", "THIRD_PARTY_COMPONENTS.json", "REDISTRIBUTION.md", "MANIFEST.sha256")]
    missing = [name for name in required if name not in names]
    if missing:
        raise SystemExit(f"wheel is missing bindings license files: {missing}")
    manifest = archive.read(prefix + "MANIFEST.sha256").decode().splitlines()
    broken = []
    for line in manifest:
        checksum, _, relative = line.partition("  ")
        member = prefix + relative
        if member not in names or hashlib.sha256(archive.read(member)).hexdigest() != checksum:
            broken.append(relative)
    if broken:
        raise SystemExit(f"bindings license manifest mismatch: {broken[:5]}")
    if "pylibseekdb/licenses/THIRD_PARTY_COMPONENTS.json" not in names:
        print(f"warning: wheel has no engine license inventory: {wheel}")
PY

  local tmp
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT

  "$py" -m venv "$tmp/venv"
  # shellcheck disable=SC1091
  source "$tmp/venv/bin/activate"
  python -m pip install -q --upgrade pip
  python -m pip install -q "$wheel" PyMySQL aiomysql

  mkdir -p "$tmp/work"
  (cd "$tmp/work" && \
    python "$MIGRATION_TEST_PY" && \
    python "$MIGRATION_INTEGRATION_TEST_PY" && \
    python "$TEST_PY")
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
