#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup sleep

assert_command_succeeds "$ROOT_DIR/build/sleep" 0 0.01s 0.02

if "$ROOT_DIR/build/sleep" nope >/dev/null 2>&1; then
    fail "sleep should reject an invalid duration"
fi

interrupt_status=0
"$ROOT_DIR/build/timeout" --preserve-status -s INT 0.05 "$ROOT_DIR/build/sleep" 5 >/dev/null 2>&1 || interrupt_status=$?
assert_exit_code "$interrupt_status" '130' "sleep should terminate with SIGINT when timeout sends INT"
