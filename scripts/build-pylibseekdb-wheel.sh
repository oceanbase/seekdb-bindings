#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "warning: build-pylibseekdb-wheel.sh was renamed to build-seekdb-wheel.sh" >&2
exec "$SCRIPT_DIR/build-seekdb-wheel.sh" "$@"
