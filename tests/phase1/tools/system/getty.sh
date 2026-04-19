#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup getty

"$ROOT_DIR/build/getty" --help > "$WORK_DIR/getty_help.out"
assert_file_contains "$WORK_DIR/getty_help.out" '^Usage: .*getty' "getty --help did not print usage"

assert_command_succeeds "$ROOT_DIR/build/getty" -n -q -i /dev/null /bin/true

getty_status=0
"$ROOT_DIR/build/getty" -n -q -i /dev/null /bin/sh -c 'exit 9' >/dev/null 2>&1 || getty_status=$?
assert_exit_code "$getty_status" '9' "getty did not propagate the child exit status"

if "$ROOT_DIR/build/getty" -n -q >/dev/null 2>&1; then
    fail "getty without a tty path should fail"
fi
