#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir wc)

note "phase1 text: wc"

printf 'one two\nthree\n' > "$WORK_DIR/input.txt"

set -- $("${TEST_BIN_DIR}/wc" -lwc "$WORK_DIR/input.txt")
assert_text_equals "$1" '2' "wc -l reported the wrong number of lines"
assert_text_equals "$2" '3' "wc -w reported the wrong number of words"
assert_text_equals "$3" '14' "wc -c reported the wrong number of bytes"

wc_selected=$(printf 'one two\nthree\n' | "${TEST_BIN_DIR}/wc" -lw | tr -d '\r\n')
assert_text_equals "$wc_selected" '2 3' "wc -lw returned the wrong selective counts"

wc_chars=$(printf 'caf\303\251\n' | "${TEST_BIN_DIR}/wc" -m | tr -d ' \r\n')
assert_text_equals "$wc_chars" '5' "wc -m should count characters rather than bytes"

wc_width=$(printf 'ab\tc\nwide\n' | "${TEST_BIN_DIR}/wc" -L | tr -d ' \r\n')
assert_text_equals "$wc_width" '9' "wc -L should report the maximum display width"

wc_ansi_width=$(printf '\033[31mred\033[0m\n' | "${TEST_BIN_DIR}/wc" -L | tr -d ' \r\n')
assert_text_equals "$wc_ansi_width" '3' "wc -L should ignore ANSI escape sequences when measuring display width"

printf '界界\n' > "$WORK_DIR/unicode_width.txt"
"${TEST_BIN_DIR}/wc" -L "$WORK_DIR/unicode_width.txt" > "$WORK_DIR/unicode_width.out"
unicode_width=$(awk '{print $1}' "$WORK_DIR/unicode_width.out" | tr -d '\r\n ')
assert_text_equals "$unicode_width" '4' "wc did not account for wide Unicode characters in line width"

printf 'éx\n' > "$WORK_DIR/combining_width.txt"
"${TEST_BIN_DIR}/wc" -L "$WORK_DIR/combining_width.txt" > "$WORK_DIR/combining_width.out"
combining_width=$(awk '{print $1}' "$WORK_DIR/combining_width.out" | tr -d '\r\n ')
assert_text_equals "$combining_width" '2' "wc counted a combining mark as an extra display column"

printf '👍🏽x\n' > "$WORK_DIR/emoji_modifier_width.txt"
"${TEST_BIN_DIR}/wc" -L "$WORK_DIR/emoji_modifier_width.txt" > "$WORK_DIR/emoji_modifier_width.out"
emoji_modifier_width=$(awk '{print $1}' "$WORK_DIR/emoji_modifier_width.out" | tr -d '\r\n ')
assert_text_equals "$emoji_modifier_width" '3' "wc counted an emoji skin-tone modifier as a separate display cell"

printf '👩‍💻x\n' > "$WORK_DIR/zwj_width.txt"
"${TEST_BIN_DIR}/wc" -L "$WORK_DIR/zwj_width.txt" > "$WORK_DIR/zwj_width.out"
zwj_width=$(awk '{print $1}' "$WORK_DIR/zwj_width.out" | tr -d '\r\n ')
assert_text_equals "$zwj_width" '3' "wc counted members of a ZWJ grapheme as separate display cells"

printf '·x\n' > "$WORK_DIR/ambiguous_width.txt"
NEWOS_AMBIGUOUS_WIDTH=2 "${TEST_BIN_DIR}/wc" -L "$WORK_DIR/ambiguous_width.txt" > "$WORK_DIR/ambiguous_width.out"
ambiguous_width=$(awk '{print $1}' "$WORK_DIR/ambiguous_width.out" | tr -d '\r\n ')
assert_text_equals "$ambiguous_width" '3' "wc did not honor wide East Asian Ambiguous characters"
