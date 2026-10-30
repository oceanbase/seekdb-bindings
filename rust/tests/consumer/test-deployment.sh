#!/usr/bin/env bash
# Compatibility entry point. Platform logic lives in the shared Python runner.
set -euo pipefail
python3 "$(dirname -- "${BASH_SOURCE[0]}")/test_deployment.py" "$@"
