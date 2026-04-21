#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup logic

assert_command_succeeds "$ROOT_DIR/build/test" 1 = 1

test_status=0
"$ROOT_DIR/build/test" 1 = 2 >/dev/null 2>&1 || test_status=$?
assert_exit_code "$test_status" '1' "test should report false expressions with exit status 1"

assert_command_succeeds "$ROOT_DIR/build/[" -f "$ROOT_DIR/Makefile" "]"

true_status=0
"$ROOT_DIR/build/true" >/dev/null 2>&1 || true_status=$?
assert_exit_code "$true_status" '0' "true should exit successfully"

false_status=0
"$ROOT_DIR/build/false" >/dev/null 2>&1 || false_status=$?
assert_exit_code "$false_status" '1' "false should exit with status 1"

expr_sum=$("$ROOT_DIR/build/expr" 2 + 3 | tr -d '\r\n')
assert_text_equals "$expr_sum" '5' "expr arithmetic failed"

expr_len=$("$ROOT_DIR/build/expr" length system | tr -d '\r\n')
assert_text_equals "$expr_len" '6' "expr length failed"

expr_invalid_status=0
"$ROOT_DIR/build/expr" substr system 999999999999999999999999 1 >/dev/null 2>"$WORK_DIR/expr_invalid.err" || expr_invalid_status=$?
assert_exit_code "$expr_invalid_status" '2' "expr should reject out-of-range numeric inputs"
assert_file_contains "$WORK_DIR/expr_invalid.err" '^expr: syntax error$' "expr did not report the invalid numeric input cleanly"
