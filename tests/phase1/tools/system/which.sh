#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup which

which_out=$(PATH="$ROOT_DIR/build:/bin:/usr/bin" "$ROOT_DIR/build/which" sh | tr -d '\r\n')
assert_text_equals "$which_out" "$ROOT_DIR/build/sh" "which did not resolve the in-repo binary first"

PATH="$ROOT_DIR/build:/bin:/usr/bin" "$ROOT_DIR/build/which" -a sh > "$WORK_DIR/which_all.out"
assert_file_contains "$WORK_DIR/which_all.out" "^$ROOT_DIR/build/sh$" "which -a did not include the in-repo binary"

missing_status=0
PATH="$ROOT_DIR/build:/bin:/usr/bin" "$ROOT_DIR/build/which" definitely_missing_command >/dev/null 2>&1 || missing_status=$?
assert_exit_code "$missing_status" '1' "which should fail for an unknown command"
