#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup process_tools

assert_command_succeeds "${TEST_BIN_DIR}/ps" -p $$ > "$WORK_DIR/ps.out"
assert_file_contains "$WORK_DIR/ps.out" '^PID' "ps did not print the process table header"
assert_file_contains "$WORK_DIR/ps.out" "^$$[[:space:]]" "ps did not include the current shell pid"

current_user=$(id -un)
assert_command_succeeds "${TEST_BIN_DIR}/ps" aux > "$WORK_DIR/ps_aux.out"
assert_file_contains "$WORK_DIR/ps_aux.out" "^$$[[:space:]]" "ps aux compatibility did not include the current shell pid"

current_state=$(awk -v pid="$$" '$1 == pid { print $4; exit }' "$WORK_DIR/ps.out")
assert_nonempty_text "$current_state" "ps did not expose the current shell state"
assert_command_succeeds "${TEST_BIN_DIR}/ps" -u "$current_user" -s "$current_state" -o pid=,uid=,command= > "$WORK_DIR/ps_filtered.out"
assert_file_contains "$WORK_DIR/ps_filtered.out" "^$$[[:space:]][[:digit:]][[:digit:]]*[[:space:]]" "ps filtering or custom -o output did not include the current shell"
if grep -q '^PID' "$WORK_DIR/ps_filtered.out"; then
    fail "ps -o field= should suppress the default header labels"
fi

assert_command_succeeds "${TEST_BIN_DIR}/pstree" -A -u -p $$ > "$WORK_DIR/pstree.out"
assert_file_contains "$WORK_DIR/pstree.out" "\\($$\\)" "pstree -p did not show the current shell pid"
assert_file_contains "$WORK_DIR/pstree.out" '{' "pstree -u did not annotate user ownership"
if ! grep -Eq '(^|[[:space:]])(\|- |`- )' "$WORK_DIR/pstree.out"; then
    fail "pstree -A did not use ASCII branch markers"
fi

assert_command_succeeds "${TEST_BIN_DIR}/top" -b -n 5 -p $$ > "$WORK_DIR/top.out"
assert_file_contains "$WORK_DIR/top.out" '^top - ' "top did not print the expected summary banner"
assert_file_contains "$WORK_DIR/top.out" '^PID[[:space:]][[:space:]]*USER[[:space:]][[:space:]]*STATE[[:space:]][[:space:]]*RSS' "top did not print the process table header"
assert_file_contains "$WORK_DIR/top.out" "^$$[[:space:]]" "top did not include the current shell pid"

assert_command_succeeds "${TEST_BIN_DIR}/lsof" --help > "$WORK_DIR/lsof_help.out" 2>&1
assert_file_contains "$WORK_DIR/lsof_help.out" 'list open files' "lsof --help did not print the tool summary"
if [ -d "/proc/$$/fd" ]; then
    assert_command_succeeds "${TEST_BIN_DIR}/lsof" -p $$ > "$WORK_DIR/lsof.out"
    assert_file_contains "$WORK_DIR/lsof.out" '^COMMAND PID USER FD NAME$' "lsof did not print the expected header"
    assert_file_contains "$WORK_DIR/lsof.out" "^[^[:space:]][^[:space:]]*[[:space:]]$$[[:space:]]" "lsof did not include the current shell pid"
fi

kill_term=$("${TEST_BIN_DIR}/kill" -l TERM | tr -d '\r\n')
assert_text_equals "$kill_term" '15' "kill -l TERM did not resolve to SIGTERM"

kill_name=$("${TEST_BIN_DIR}/kill" -l 15 | tr -d '\r\n')
assert_text_equals "$kill_name" 'TERM' "kill -l 15 did not resolve to the TERM name"

"${TEST_BIN_DIR}/sleep" 30 &
sleep_pid=$!
trap 'kill "$sleep_pid" 2>/dev/null || true' EXIT HUP INT TERM
assert_command_succeeds "${TEST_BIN_DIR}/pgrep" -P $$ -x sleep > "$WORK_DIR/pgrep.out"
assert_file_contains "$WORK_DIR/pgrep.out" "^$sleep_pid$" "pgrep did not find the child sleep process"

assert_command_succeeds "${TEST_BIN_DIR}/pgrep" -l -P $$ -x sleep > "$WORK_DIR/pgrep_list.out"
assert_file_contains "$WORK_DIR/pgrep_list.out" "^$sleep_pid[[:space:]].*sleep" "pgrep -l did not print the process name"

pgrep_count=$("${TEST_BIN_DIR}/pgrep" -c -P $$ -x sleep | tr -d '\r\n')
case "$pgrep_count" in
    ''|*[!0-9]*) fail "pgrep -c did not print a numeric count" ;;
    *) [ "$pgrep_count" -ge 1 ] || fail "pgrep -c did not count the child sleep process" ;;
esac

assert_command_succeeds "${TEST_BIN_DIR}/pkill" -0 -P $$ -x sleep
kill "$sleep_pid"
wait "$sleep_pid" 2>/dev/null || true
trap - EXIT HUP INT TERM

pgrep_none_status=0
"${TEST_BIN_DIR}/pgrep" -P $$ -x sleep > /dev/null 2>&1 || pgrep_none_status=$?
assert_exit_code "$pgrep_none_status" '1' "pgrep should return 1 when no process matches"

pgrep_regex_none_status=0
"${TEST_BIN_DIR}/pgrep" definitely_no_such_process_name > "$WORK_DIR/pgrep_regex_none.out" 2>&1 || pgrep_regex_none_status=$?
assert_exit_code "$pgrep_regex_none_status" '1' "pgrep non-exact matching should return 1 when no process matches"
[ ! -s "$WORK_DIR/pgrep_regex_none.out" ] || fail "pgrep non-exact no-match should not print unrelated processes"

pkill_regex_none_status=0
"${TEST_BIN_DIR}/pkill" -0 definitely_no_such_process_name > "$WORK_DIR/pkill_regex_none.out" 2>&1 || pkill_regex_none_status=$?
assert_exit_code "$pkill_regex_none_status" '1' "pkill non-exact matching should return 1 when no process matches"
[ ! -s "$WORK_DIR/pkill_regex_none.out" ] || fail "pkill non-exact no-match should not signal unrelated processes"

assert_command_succeeds "${TEST_BIN_DIR}/time" "${TEST_BIN_DIR}/true" > "$WORK_DIR/time.out" 2> "$WORK_DIR/time.err"
assert_file_contains "$WORK_DIR/time.err" '^real[[:space:]][[:digit:]]' "time did not report real elapsed time"

time_false_status=0
"${TEST_BIN_DIR}/time" "${TEST_BIN_DIR}/false" > /dev/null 2> "$WORK_DIR/time_false.err" || time_false_status=$?
assert_exit_code "$time_false_status" '1' "time should preserve the wrapped command status"
assert_file_contains "$WORK_DIR/time_false.err" '^real[[:space:]][[:digit:]]' "time did not report timing for a failing command"

assert_command_succeeds "${TEST_BIN_DIR}/timeout" 1 "${TEST_BIN_DIR}/true"

timeout_status=0
"${TEST_BIN_DIR}/timeout" 1 "${TEST_BIN_DIR}/sleep" 2 > /dev/null 2>&1 || timeout_status=$?
assert_exit_code "$timeout_status" '124' "timeout should return 124 when the command exceeds the limit"

preserve_status=0
"${TEST_BIN_DIR}/timeout" --preserve-status 1 "${TEST_BIN_DIR}/false" > /dev/null 2>&1 || preserve_status=$?
assert_exit_code "$preserve_status" '1' "timeout --preserve-status should keep the wrapped command status"
