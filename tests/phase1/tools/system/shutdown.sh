#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup shutdown

"$ROOT_DIR/build/shutdown" --help > "$WORK_DIR/help.out" 2>&1
assert_file_contains "$WORK_DIR/help.out" '^Usage: ' "shutdown --help did not print usage"

shutdown_status=0
"$ROOT_DIR/build/shutdown" --no-such-option > "$WORK_DIR/invalid.out" 2>&1 || shutdown_status=$?
[ "$shutdown_status" -ne 0 ] || fail "shutdown should reject unknown options"
assert_file_contains "$WORK_DIR/invalid.out" '^Usage: ' "shutdown invalid-option output did not include usage"