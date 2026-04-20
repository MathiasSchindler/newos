#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup process_tools

assert_command_succeeds "$ROOT_DIR/build/ps" -p $$ > "$WORK_DIR/ps.out"
assert_file_contains "$WORK_DIR/ps.out" '^PID' "ps did not print the process table header"
assert_file_contains "$WORK_DIR/ps.out" "^$$[[:space:]]" "ps did not include the current shell pid"

current_user=$(id -un)
assert_command_succeeds "$ROOT_DIR/build/ps" aux > "$WORK_DIR/ps_aux.out"
assert_file_contains "$WORK_DIR/ps_aux.out" "^$$[[:space:]]" "ps aux compatibility did not include the current shell pid"

assert_command_succeeds "$ROOT_DIR/build/ps" -u "$current_user" -s S -o pid=,uid=,command= > "$WORK_DIR/ps_filtered.out"
assert_file_contains "$WORK_DIR/ps_filtered.out" "^$$[[:space:]][[:digit:]][[:digit:]]*[[:space:]]" "ps filtering or custom -o output did not include the current shell"
if grep -q '^PID' "$WORK_DIR/ps_filtered.out"; then
    fail "ps -o field= should suppress the default header labels"
fi

assert_command_succeeds "$ROOT_DIR/build/pstree" -A -u -p $$ > "$WORK_DIR/pstree.out"
assert_file_contains "$WORK_DIR/pstree.out" "\\($$\\)" "pstree -p did not show the current shell pid"
assert_file_contains "$WORK_DIR/pstree.out" '{' "pstree -u did not annotate user ownership"
if ! grep -Eq '(^|[[:space:]])(\|- |`- )' "$WORK_DIR/pstree.out"; then
    fail "pstree -A did not use ASCII branch markers"
fi

assert_command_succeeds "$ROOT_DIR/build/top" -b -n 5 -p $$ > "$WORK_DIR/top.out"
assert_file_contains "$WORK_DIR/top.out" '^top - ' "top did not print the expected summary banner"
assert_file_contains "$WORK_DIR/top.out" '^PID[[:space:]][[:space:]]*USER[[:space:]][[:space:]]*STATE[[:space:]][[:space:]]*RSS' "top did not print the process table header"
assert_file_contains "$WORK_DIR/top.out" "^$$[[:space:]]" "top did not include the current shell pid"

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
