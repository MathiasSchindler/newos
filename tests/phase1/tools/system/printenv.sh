#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup printenv

value_out=$(FOO=bar "$ROOT_DIR/build/printenv" FOO | tr -d '\r\n')
assert_text_equals "$value_out" 'bar' "printenv NAME did not return the variable value"

name_out=$(FOO=bar "$ROOT_DIR/build/printenv" -n FOO | tr -d '\r\n')
assert_text_equals "$name_out" 'FOO' "printenv -n did not print only the variable name"

zero_out=$(FOO=bar "$ROOT_DIR/build/printenv" -0 FOO | tr '\0\r' '\n' | tr -d '\n')
assert_text_equals "$zero_out" 'bar' "printenv -0 did not use NUL-delimited output"

missing_status=0
FOO=bar "$ROOT_DIR/build/printenv" DOES_NOT_EXIST >/dev/null 2>&1 || missing_status=$?
assert_exit_code "$missing_status" '1' "printenv should fail for a missing variable"
