#!/bin/sh
set -eu
. "$(dirname "$0")/common.inc"
phase1_setup runtime_task_claims

if [ ! -x "${TEST_BIN_DIR}/taskclaimtest" ]; then
    note "taskclaimtest unavailable on this platform; skipping worker claim fixture"
    exit 0
fi

output=$("${TEST_BIN_DIR}/timeout" 30 "${TEST_BIN_DIR}/taskclaimtest")
assert_text_equals "$(printf '%s\n' "$output" | tail -n 1)" 'status: ok' "worker generation or transition fixture failed"
