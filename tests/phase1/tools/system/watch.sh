#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup watch

"${TEST_BIN_DIR}/watch" -n 0.01 -c 2 -t "${TEST_BIN_DIR}/echo" watched > "$WORK_DIR/watch.out"
watch_runs=$(grep -c '^watched$' "$WORK_DIR/watch.out" | tr -d '\r\n')
assert_text_equals "$watch_runs" '2' "watch did not rerun the command the requested number of times"
