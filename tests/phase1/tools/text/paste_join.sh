#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir paste_join)

note "phase1 text: paste/join"

printf 'red\nblue\n' > "$WORK_DIR/colors.txt"
printf '1\n2\n' > "$WORK_DIR/numbers.txt"
"${TEST_BIN_DIR}/paste" -d ':' "$WORK_DIR/colors.txt" "$WORK_DIR/numbers.txt" > "$WORK_DIR/paste.out"
printf 'red:1\nblue:2\n' > "$WORK_DIR/paste.expected"
assert_files_equal "$WORK_DIR/paste.expected" "$WORK_DIR/paste.out" "paste did not merge columns with the requested delimiter"

printf 'k1 left\nk2 right\n' > "$WORK_DIR/join_left.txt"
printf 'k1 one\nk2 two\n' > "$WORK_DIR/join_right.txt"
"${TEST_BIN_DIR}/join" "$WORK_DIR/join_left.txt" "$WORK_DIR/join_right.txt" > "$WORK_DIR/join.out"
printf 'k1 left one\nk2 right two\n' > "$WORK_DIR/join.expected"
assert_files_equal "$WORK_DIR/join.expected" "$WORK_DIR/join.out" "join did not combine matching sorted keys"

printf 'Äpfel 1\n' > "$WORK_DIR/join_unicode_left.txt"
printf 'äpfel 2\n' > "$WORK_DIR/join_unicode_right.txt"
"${TEST_BIN_DIR}/join" -i "$WORK_DIR/join_unicode_left.txt" "$WORK_DIR/join_unicode_right.txt" > "$WORK_DIR/join_unicode.out"
assert_file_contains "$WORK_DIR/join_unicode.out" '^Äpfel 1 2$' "join -i did not match Unicode keys ignore-case"

printf 'k1:left\nk2:solo\n' > "$WORK_DIR/join_fmt_left.txt"
printf 'k1:right\nk3:extra\n' > "$WORK_DIR/join_fmt_right.txt"
"${TEST_BIN_DIR}/join" -t ':' -o 0,1.2,2.2 -e NONE "$WORK_DIR/join_fmt_left.txt" "$WORK_DIR/join_fmt_right.txt" > "$WORK_DIR/join_fmt.out"
printf 'k1:left:right\n' > "$WORK_DIR/join_fmt.expected"
assert_files_equal "$WORK_DIR/join_fmt.expected" "$WORK_DIR/join_fmt.out" "join -o output selection failed"

printf 'apple\nbanana\ncarrot\n' > "$WORK_DIR/comm_left.txt"
printf 'banana\ncarrot\ndate\n' > "$WORK_DIR/comm_right.txt"
"${TEST_BIN_DIR}/comm" "$WORK_DIR/comm_left.txt" "$WORK_DIR/comm_right.txt" > "$WORK_DIR/comm.out"
printf 'apple\n\t\tbanana\n\t\tcarrot\n\tdate\n' > "$WORK_DIR/comm.expected"
assert_files_equal "$WORK_DIR/comm.expected" "$WORK_DIR/comm.out" "comm did not compare sorted files into three columns"

"${TEST_BIN_DIR}/comm" -12 "$WORK_DIR/comm_left.txt" "$WORK_DIR/comm_right.txt" > "$WORK_DIR/comm_common.out"
printf 'banana\ncarrot\n' > "$WORK_DIR/comm_common.expected"
assert_files_equal "$WORK_DIR/comm_common.expected" "$WORK_DIR/comm_common.out" "comm -12 did not print only common lines"
