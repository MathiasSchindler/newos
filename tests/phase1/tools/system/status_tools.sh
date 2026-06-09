#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup status_tools

free_status=0
"${TEST_BIN_DIR}/free" > "$WORK_DIR/free.out" 2> "$WORK_DIR/free.err" || free_status=$?
if [ "$free_status" -ne 0 ]; then
	assert_file_contains "$WORK_DIR/free.err" 'memory information unavailable' "free should report unavailable memory information clearly"
	note "free memory information not available on this platform; skipping free output checks"
else
	assert_file_contains "$WORK_DIR/free.out" '^Mem:' "free did not print the memory summary"

	assert_command_succeeds "${TEST_BIN_DIR}/free" -h > "$WORK_DIR/free_h.out"
	assert_file_contains "$WORK_DIR/free_h.out" '^Mem:' "free -h did not print the memory summary"
fi

assert_command_succeeds "${TEST_BIN_DIR}/uptime" > "$WORK_DIR/uptime.out"
assert_file_contains "$WORK_DIR/uptime.out" 'up ' "uptime did not report the system uptime"

assert_command_succeeds "${TEST_BIN_DIR}/uptime" -p > "$WORK_DIR/uptime_pretty.out"
assert_file_contains "$WORK_DIR/uptime_pretty.out" '^up ' "uptime -p did not print the pretty uptime format"

assert_command_succeeds "${TEST_BIN_DIR}/who" -q > "$WORK_DIR/who.out"
assert_file_contains "$WORK_DIR/who.out" '# users=[0-9][0-9]*' "who -q did not report the user count"

assert_command_succeeds "${TEST_BIN_DIR}/users" -c > "$WORK_DIR/users_count.out"
assert_file_contains "$WORK_DIR/users_count.out" '^[0-9][0-9]*$' "users -c did not print a numeric count"

assert_command_succeeds "${TEST_BIN_DIR}/users" -l > "$WORK_DIR/users_long.out"
assert_command_succeeds "${TEST_BIN_DIR}/users" --since 9999999999 -c > "$WORK_DIR/users_since_future.out"
assert_file_contains "$WORK_DIR/users_since_future.out" '^0$' "users --since future filter did not exclude current sessions"
