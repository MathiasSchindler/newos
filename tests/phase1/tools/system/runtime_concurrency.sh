#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup runtime_concurrency

if [ ! -x "${TEST_BIN_DIR}/concurrencytest" ]; then
    note "concurrencytest unavailable on this platform; skipping runtime concurrency fixture"
    exit 0
fi

output=$("${TEST_BIN_DIR}/concurrencytest")
assert_text_equals "$output" 'status: ok' "arena reuse or all-ready polling fixture failed"