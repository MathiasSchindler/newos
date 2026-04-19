#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir wc)

note "phase1 text: wc"

printf 'one two\nthree\n' > "$WORK_DIR/input.txt"

set -- $("$ROOT_DIR/build/wc" -lwc "$WORK_DIR/input.txt")
assert_text_equals "$1" '2' "wc -l reported the wrong number of lines"
assert_text_equals "$2" '3' "wc -w reported the wrong number of words"
assert_text_equals "$3" '14' "wc -c reported the wrong number of bytes"

wc_selected=$(printf 'one two\nthree\n' | "$ROOT_DIR/build/wc" -lw | tr -d '\r\n')
assert_text_equals "$wc_selected" '2 3' "wc -lw returned the wrong selective counts"

wc_chars=$(printf 'caf\303\251\n' | "$ROOT_DIR/build/wc" -m | tr -d ' \r\n')
assert_text_equals "$wc_chars" '5' "wc -m should count characters rather than bytes"

wc_width=$(printf 'ab\tc\nwide\n' | "$ROOT_DIR/build/wc" -L | tr -d ' \r\n')
assert_text_equals "$wc_width" '9' "wc -L should report the maximum display width"

printf '界界\n' > "$WORK_DIR/unicode_width.txt"
"$ROOT_DIR/build/wc" -L "$WORK_DIR/unicode_width.txt" > "$WORK_DIR/unicode_width.out"
unicode_width=$(awk '{print $1}' "$WORK_DIR/unicode_width.out" | tr -d '\r\n ')
assert_text_equals "$unicode_width" '4' "wc did not account for wide Unicode characters in line width"
