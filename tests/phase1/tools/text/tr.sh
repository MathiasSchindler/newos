#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir tr)

note "phase1 text: tr"

printf 'abba cab\n' > "$WORK_DIR/input.txt"
"$ROOT_DIR/build/tr" 'ab' 'AB' < "$WORK_DIR/input.txt" > "$WORK_DIR/out.txt"
printf 'ABBA cAB\n' > "$WORK_DIR/expected.txt"
assert_files_equal "$WORK_DIR/expected.txt" "$WORK_DIR/out.txt" "tr did not translate characters one-for-one"

printf 'éø\n' > "$WORK_DIR/unicode.txt"
"$ROOT_DIR/build/tr" 'éø' 'ab' < "$WORK_DIR/unicode.txt" > "$WORK_DIR/unicode.out"
assert_file_contains "$WORK_DIR/unicode.out" '^ab$' "tr did not translate Unicode characters as whole code points"

tr_delete=$(printf 'a1b22c333\n' | "$ROOT_DIR/build/tr" -d '0-9' | tr -d '\r\n')
assert_text_equals "$tr_delete" 'abc' "tr -d did not delete the requested range"

tr_squeeze=$(printf 'aa   bb    cc\n' | "$ROOT_DIR/build/tr" -s ' ' | tr -d '\r\n')
assert_text_equals "$tr_squeeze" 'aa bb cc' "tr -s did not squeeze repeated characters"

tr_class_upper=$(printf 'mixed Case 42\n' | "$ROOT_DIR/build/tr" '[:lower:]' '[:upper:]' | tr -d '\r\n')
assert_text_equals "$tr_class_upper" 'MIXED CASE 42' "tr class-based translation failed"
