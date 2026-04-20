#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup clear

"$ROOT_DIR/build/clear" > "$WORK_DIR/clear.out"
clear_hex=$(od -An -tx1 "$WORK_DIR/clear.out" | tr -d ' 
')
assert_text_equals "$clear_hex" '1b5b481b5b324a1b5b334a' "clear did not emit the expected ANSI sequence"

"$ROOT_DIR/build/clear" --help > "$WORK_DIR/help.out"
assert_file_contains "$WORK_DIR/help.out" '^Usage: ' "clear --help did not print usage"

clear_status=0
"$ROOT_DIR/build/clear" unexpected >/dev/null 2>"$WORK_DIR/error.out" || clear_status=$?
assert_exit_code "$clear_status" '1' "clear should reject unexpected arguments"
assert_file_contains "$WORK_DIR/error.out" '^Usage: ' "clear error output did not print usage"
