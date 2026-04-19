#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup init

assert_command_succeeds "$ROOT_DIR/build/init" -n -q /bin/true

init_status=0
"$ROOT_DIR/build/init" -n -q -c 'exit 7' >/dev/null 2>&1 || init_status=$?
assert_exit_code "$init_status" '7' "init did not propagate the child exit status in no-respawn mode"
