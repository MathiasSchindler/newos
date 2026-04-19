#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup process_tools

assert_command_succeeds "$ROOT_DIR/build/ps" -p $$ > "$WORK_DIR/ps.out"
assert_file_contains "$WORK_DIR/ps.out" '^PID' "ps did not print the process table header"
assert_file_contains "$WORK_DIR/ps.out" "^$$[[:space:]]" "ps did not include the current shell pid"

kill_term=$("$ROOT_DIR/build/kill" -l TERM | tr -d '\r\n')
assert_text_equals "$kill_term" '15' "kill -l TERM did not resolve to SIGTERM"

kill_name=$("$ROOT_DIR/build/kill" -l 15 | tr -d '\r\n')
assert_text_equals "$kill_name" 'TERM' "kill -l 15 did not resolve to the TERM name"

assert_command_succeeds "$ROOT_DIR/build/timeout" 1 "$ROOT_DIR/build/true"

timeout_status=0
"$ROOT_DIR/build/timeout" 1 "$ROOT_DIR/build/sleep" 2 > /dev/null 2>&1 || timeout_status=$?
assert_exit_code "$timeout_status" '124' "timeout should return 124 when the command exceeds the limit"

preserve_status=0
"$ROOT_DIR/build/timeout" --preserve-status 1 "$ROOT_DIR/build/false" > /dev/null 2>&1 || preserve_status=$?
assert_exit_code "$preserve_status" '1' "timeout --preserve-status should keep the wrapped command status"
