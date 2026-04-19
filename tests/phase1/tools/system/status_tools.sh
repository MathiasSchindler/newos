#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup status_tools

assert_command_succeeds "$ROOT_DIR/build/free" > "$WORK_DIR/free.out"
assert_file_contains "$WORK_DIR/free.out" '^Mem:' "free did not print the memory summary"

assert_command_succeeds "$ROOT_DIR/build/free" -h > "$WORK_DIR/free_h.out"
assert_file_contains "$WORK_DIR/free_h.out" '^Mem:' "free -h did not print the memory summary"

assert_command_succeeds "$ROOT_DIR/build/uptime" > "$WORK_DIR/uptime.out"
assert_file_contains "$WORK_DIR/uptime.out" 'up ' "uptime did not report the system uptime"

assert_command_succeeds "$ROOT_DIR/build/uptime" -p > "$WORK_DIR/uptime_pretty.out"
assert_file_contains "$WORK_DIR/uptime_pretty.out" '^up ' "uptime -p did not print the pretty uptime format"

assert_command_succeeds "$ROOT_DIR/build/who" -q > "$WORK_DIR/who.out"
assert_file_contains "$WORK_DIR/who.out" '# users=[0-9][0-9]*' "who -q did not report the user count"

assert_command_succeeds "$ROOT_DIR/build/users" -c > "$WORK_DIR/users_count.out"
assert_file_contains "$WORK_DIR/users_count.out" '^[0-9][0-9]*$' "users -c did not print a numeric count"
