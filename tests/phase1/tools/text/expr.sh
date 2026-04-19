#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir expr)

note "phase1 text: expr"

expr_sum=$("$ROOT_DIR/build/expr" 2 + 3 | tr -d '\r\n')
assert_text_equals "$expr_sum" '5' "expr basic arithmetic regressed"

expr_big_add=$("$ROOT_DIR/build/expr" 9223372036854775807 + 1 | tr -d '\r\n')
assert_text_equals "$expr_big_add" '9223372036854775808' "expr arbitrary-precision addition regressed"

expr_big_mul=$("$ROOT_DIR/build/expr" 999999999999999999999999 '*' 9 | tr -d '\r\n')
assert_text_equals "$expr_big_mul" '8999999999999999999999991' "expr arbitrary-precision multiplication regressed"
