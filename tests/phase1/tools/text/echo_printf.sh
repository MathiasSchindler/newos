#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir echo_printf)

note "phase1 text: echo/printf"

echo_out=$("$ROOT_DIR/build/echo" -n 'phase1 ready')
assert_text_equals "$echo_out" 'phase1 ready' "echo -n did not preserve the text exactly"

"$ROOT_DIR/build/echo" -e 'line1\nline2\tend' > "$WORK_DIR/echo_escape.out"
printf 'line1\nline2\tend\n' > "$WORK_DIR/echo_escape.expected"
assert_files_equal "$WORK_DIR/echo_escape.expected" "$WORK_DIR/echo_escape.out" "echo -e did not decode common escapes"

echo_literal_out=$("$ROOT_DIR/build/echo" -E 'left\nright' | tr -d '\r\n')
assert_text_equals "$echo_literal_out" 'left\nright' "echo -E should leave backslash sequences untouched"

"$ROOT_DIR/build/printf" 'value:%04d:%s\n' 7 ok > "$WORK_DIR/printf.out"
printf 'value:0007:ok\n' > "$WORK_DIR/printf.expected"
assert_files_equal "$WORK_DIR/printf.expected" "$WORK_DIR/printf.out" "printf formatting output changed unexpectedly"

printf_cycle=$("$ROOT_DIR/build/printf" '%s:' A B C | tr -d '\r\n')
assert_text_equals "$printf_cycle" 'A:B:C:' "printf format cycling failed"
