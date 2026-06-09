#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup sleep

assert_command_succeeds "${TEST_BIN_DIR}/sleep" 0 0.01s 0.02

if "${TEST_BIN_DIR}/sleep" nope >/dev/null 2>&1; then
    fail "sleep should reject an invalid duration"
fi

interrupt_status=0
"${TEST_BIN_DIR}/timeout" --preserve-status -s INT 0.05 "${TEST_BIN_DIR}/sleep" 5 >/dev/null 2>&1 || interrupt_status=$?
assert_exit_code "$interrupt_status" '130' "sleep should terminate with SIGINT when timeout sends INT"
