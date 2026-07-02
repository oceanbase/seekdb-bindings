#!/usr/bin/env bash
# Run TwoClientsOpen.TwoConcurrentClients in a loop until it fails.
#
# Env:
#   SEEKDB_BIN   path to the seekdb binary (required — passed through to the test)
#   TEST_BIN     path to the test executable (default: ../build/test_two_clients_threads
#                relative to this script)

set -u

: "${SEEKDB_BIN:?set SEEKDB_BIN to the seekdb binary}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST_BIN="${TEST_BIN:-$SCRIPT_DIR/../build/test_two_clients_threads}"
FILTER="TwoClientsOpen.TwoConcurrentClients"

if [[ ! -x "$TEST_BIN" ]]; then
    echo "test binary not found or not executable: $TEST_BIN" >&2
    exit 2
fi

i=1
while true; do
    echo "=== iteration $i ==="
    if ! "$TEST_BIN" --gtest_filter="$FILTER"; then
        echo "FAILED on iteration $i" >&2
        exit 1
    fi
    i=$((i + 1))
done
