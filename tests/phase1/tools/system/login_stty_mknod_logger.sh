#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup login_stty_mknod_logger

"$ROOT_DIR/build/login" --help > "$WORK_DIR/login_help.out" 2>&1
assert_file_contains "$WORK_DIR/login_help.out" '^Usage: .*login' "login --help did not print usage"

current_user=$("$ROOT_DIR/build/whoami" | tr -d '\r\n')
assert_nonempty_text "$current_user" "whoami did not provide a current user"
"$ROOT_DIR/build/login" "$current_user" "$ROOT_DIR/build/printf" 'login-ok\n' > "$WORK_DIR/login.out"
assert_file_contains "$WORK_DIR/login.out" '^login-ok$' "login did not execute a command for the current user"

"$ROOT_DIR/build/stty" --help > "$WORK_DIR/stty_help.out" 2>&1
assert_file_contains "$WORK_DIR/stty_help.out" '^Usage: .*stty' "stty --help did not print usage"
stty_status=0
"$ROOT_DIR/build/stty" -a </dev/null >"$WORK_DIR/stty.out" 2>"$WORK_DIR/stty.err" || stty_status=$?
assert_exit_code "$stty_status" '1' "stty should fail when standard input is not a terminal"
assert_file_contains "$WORK_DIR/stty.err" 'not a terminal' "stty did not explain non-terminal input"

"$ROOT_DIR/build/mknod" --help > "$WORK_DIR/mknod_help.out" 2>&1
assert_file_contains "$WORK_DIR/mknod_help.out" '^Usage: .*mknod' "mknod --help did not print usage"
assert_command_succeeds "$ROOT_DIR/build/mknod" -m 600 "$WORK_DIR/fifo" p
[ -p "$WORK_DIR/fifo" ] || fail "mknod did not create a FIFO"
"$ROOT_DIR/build/stat" "$WORK_DIR/fifo" > "$WORK_DIR/fifo.stat"
assert_file_contains "$WORK_DIR/fifo.stat" 'Mode: prw-------' "mknod did not apply FIFO permissions"

"$ROOT_DIR/build/logger" --help > "$WORK_DIR/logger_help.out" 2>&1
assert_file_contains "$WORK_DIR/logger_help.out" '^Usage: .*logger' "logger --help did not print usage"
assert_command_succeeds "$ROOT_DIR/build/logger" -f "$WORK_DIR/logger.log" -t phase1 -p local0.notice hello logger
assert_file_contains "$WORK_DIR/logger.log" '^<5>phase1\[[0-9][0-9]*\]: hello logger$' "logger did not append the expected file record"

"$ROOT_DIR/build/logger" -s -t phase1 -p err mirrored message >"$WORK_DIR/logger_s.out" 2>"$WORK_DIR/logger_s.err"
assert_file_contains "$WORK_DIR/logger_s.err" '^phase1\[[0-9][0-9]*\]: mirrored message$' "logger -s did not mirror to stderr"
