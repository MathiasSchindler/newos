#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir echo_printf)

note "phase1 text: echo/printf"

echo_out=$("${TEST_BIN_DIR}/echo" -n 'phase1 ready')
assert_text_equals "$echo_out" 'phase1 ready' "echo -n did not preserve the text exactly"

"${TEST_BIN_DIR}/echo" -e 'line1\nline2\tend' > "$WORK_DIR/echo_escape.out"
printf 'line1\nline2\tend\n' > "$WORK_DIR/echo_escape.expected"
assert_files_equal "$WORK_DIR/echo_escape.expected" "$WORK_DIR/echo_escape.out" "echo -e did not decode common escapes"

echo_literal_out=$("${TEST_BIN_DIR}/echo" -E 'left\nright' | tr -d '\r\n')
assert_text_equals "$echo_literal_out" 'left\nright' "echo -E should leave backslash sequences untouched"

"${TEST_BIN_DIR}/printf" 'value:%04d:%s\n' 7 ok > "$WORK_DIR/printf.out"
printf 'value:0007:ok\n' > "$WORK_DIR/printf.expected"
assert_files_equal "$WORK_DIR/printf.expected" "$WORK_DIR/printf.out" "printf formatting output changed unexpectedly"

printf_cycle=$("${TEST_BIN_DIR}/printf" '%s:' A B C | tr -d '\r\n')
assert_text_equals "$printf_cycle" 'A:B:C:' "printf format cycling failed"

"${TEST_BIN_DIR}/printf" '%b' 'one\ntwo\n' > "$WORK_DIR/printf_b.out"
printf 'one\ntwo\n' > "$WORK_DIR/printf_b.expected"
assert_files_equal "$WORK_DIR/printf_b.expected" "$WORK_DIR/printf_b.out" "printf %b escape decoding failed"

printf_stop=$("${TEST_BIN_DIR}/printf" '%b%s' 'stop\c' tail | tr -d '\r\n')
assert_text_equals "$printf_stop" 'stop' "printf %b \\c did not stop output"

printf_q=$("${TEST_BIN_DIR}/printf" '%q' "can't stop" | tr -d '\r\n')
assert_text_equals "$printf_q" "'can'\''t stop'" "printf %q shell quoting failed"

printf_float=$("${TEST_BIN_DIR}/printf" '%.3f|%.2e|%.4g' 12.5 12.5 12.5 | tr -d '\r\n')
assert_text_equals "$printf_float" '12.500|1.25e+01|12.5' "printf floating formatting changed unexpectedly"

printf_special=$("${TEST_BIN_DIR}/printf" '%.1f|%e|%G|%f' -0 inf nan -infinity | tr -d '\r\n')
assert_text_equals "$printf_special" '-0.0|inf|NAN|-inf' "printf special floating formatting mismatch"
