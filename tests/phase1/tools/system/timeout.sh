#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup timeout

assert_command_succeeds "${TEST_BIN_DIR}/timeout" 1 "${TEST_BIN_DIR}/true"

timeout_status=0
"${TEST_BIN_DIR}/timeout" 50ms "${TEST_BIN_DIR}/sleep" 2 > /dev/null 2>&1 || timeout_status=$?
assert_exit_code "$timeout_status" '124' "timeout should return 124 when the command exceeds the limit"

preserve_status=0
"${TEST_BIN_DIR}/timeout" --preserve-status 1 "${TEST_BIN_DIR}/false" > /dev/null 2>&1 || preserve_status=$?
assert_exit_code "$preserve_status" '1' "timeout --preserve-status should keep the wrapped command status"

signal_status=0
"${TEST_BIN_DIR}/timeout" --preserve-status --signal=INT 50ms "${TEST_BIN_DIR}/sleep" 2 > /dev/null 2>&1 || signal_status=$?
assert_exit_code "$signal_status" '130' "timeout --signal=INT should send SIGINT and preserve signal status"

kill_after_status=0
"${TEST_BIN_DIR}/timeout" -k 50ms 50ms "${TEST_BIN_DIR}/sleep" 2 > /dev/null 2>&1 || kill_after_status=$?
assert_exit_code "$kill_after_status" '124' "timeout -k should still report timeout status"

invalid_timeout_status=0
"${TEST_BIN_DIR}/timeout" nope "${TEST_BIN_DIR}/true" > /dev/null 2>&1 || invalid_timeout_status=$?
assert_exit_code "$invalid_timeout_status" '125' "timeout should reject invalid durations"
