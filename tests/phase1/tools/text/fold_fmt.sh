#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir fold_fmt)

note "phase1 text: fold/fmt"

printf 'abcd1234\n' > "$WORK_DIR/fold.txt"
"${TEST_BIN_DIR}/fold" -w 4 "$WORK_DIR/fold.txt" > "$WORK_DIR/fold.out"
printf 'abcd\n1234\n' > "$WORK_DIR/fold.expected"
assert_files_equal "$WORK_DIR/fold.expected" "$WORK_DIR/fold.out" "fold did not wrap at the requested width"

printf 'foo bar baz\n' > "$WORK_DIR/fmt.txt"
"${TEST_BIN_DIR}/fmt" -w 6 "$WORK_DIR/fmt.txt" > "$WORK_DIR/fmt.out"
printf 'foo\nbar\nbaz\n' > "$WORK_DIR/fmt.expected"
assert_files_equal "$WORK_DIR/fmt.expected" "$WORK_DIR/fmt.out" "fmt did not reflow the line into the requested width"

printf '界界界a\n' > "$WORK_DIR/fold_unicode.txt"
"${TEST_BIN_DIR}/fold" -w 6 "$WORK_DIR/fold_unicode.txt" > "$WORK_DIR/fold_unicode.out"
assert_file_contains "$WORK_DIR/fold_unicode.out" '^界界界$' "fold did not wrap on Unicode display-width boundaries"
assert_file_contains "$WORK_DIR/fold_unicode.out" '^a$' "fold did not preserve the trailing character after Unicode wrapping"

printf '🥹a\n' > "$WORK_DIR/fold_emoji.txt"
"${TEST_BIN_DIR}/fold" -w 2 "$WORK_DIR/fold_emoji.txt" > "$WORK_DIR/fold_emoji.out"
assert_file_contains "$WORK_DIR/fold_emoji.out" '^🥹$' "fold did not treat newer emoji as wide"
assert_file_contains "$WORK_DIR/fold_emoji.out" '^a$' "fold did not preserve text after newer emoji wrapping"

printf '👍🏽a\n' > "$WORK_DIR/fold_emoji_modifier.txt"
"${TEST_BIN_DIR}/fold" -w 2 "$WORK_DIR/fold_emoji_modifier.txt" > "$WORK_DIR/fold_emoji_modifier.out"
assert_file_contains "$WORK_DIR/fold_emoji_modifier.out" '^👍🏽$' "fold counted an emoji skin-tone modifier as a separate display cell"
assert_file_contains "$WORK_DIR/fold_emoji_modifier.out" '^a$' "fold did not preserve text after emoji skin-tone wrapping"

printf '\033[31mred\033[0mblue\n' > "$WORK_DIR/fold_ansi.txt"
"${TEST_BIN_DIR}/fold" -w 3 "$WORK_DIR/fold_ansi.txt" > "$WORK_DIR/fold_ansi.out"
printf '\033[31mred\033[0m\nblu\ne\n' > "$WORK_DIR/fold_ansi.expected"
assert_files_equal "$WORK_DIR/fold_ansi.expected" "$WORK_DIR/fold_ansi.out" "fold counted ANSI escape sequences toward display width"

printf 'aa　bb\n' > "$WORK_DIR/fold_unicode_space.txt"
"${TEST_BIN_DIR}/fold" -s -w 4 "$WORK_DIR/fold_unicode_space.txt" > "$WORK_DIR/fold_unicode_space.out"
printf 'aa　\nbb\n' > "$WORK_DIR/fold_unicode_space.expected"
assert_files_equal "$WORK_DIR/fold_unicode_space.expected" "$WORK_DIR/fold_unicode_space.out" "fold -s did not break on Unicode whitespace"

printf 'éé\n' > "$WORK_DIR/fold_byte.txt"
"${TEST_BIN_DIR}/fold" -b -w 3 "$WORK_DIR/fold_byte.txt" > "$WORK_DIR/fold_byte.out"
byte_lines=$("${TEST_BIN_DIR}/wc" -l "$WORK_DIR/fold_byte.out" | "${TEST_BIN_DIR}/awk" '{print $1}')
assert_text_equals "$byte_lines" '2' "fold -b did not count raw bytes"

printf 'äö aa\n' > "$WORK_DIR/fmt_unicode.txt"
"${TEST_BIN_DIR}/fmt" -w 5 "$WORK_DIR/fmt_unicode.txt" > "$WORK_DIR/fmt_unicode.out"
assert_file_contains "$WORK_DIR/fmt_unicode.out" '^äö aa$' "fmt did not keep a Unicode-width-fitting line intact"
