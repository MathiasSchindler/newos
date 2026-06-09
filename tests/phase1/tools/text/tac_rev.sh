#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir tac_rev)

note "phase1 text: tac/rev"

cat > "$WORK_DIR/lines.txt" <<'EOF'
top
middle
bottom
EOF

"${TEST_BIN_DIR}/tac" "$WORK_DIR/lines.txt" > "$WORK_DIR/tac.out"
cat > "$WORK_DIR/tac.expected" <<'EOF'
bottom
middle
top
EOF
assert_files_equal "$WORK_DIR/tac.expected" "$WORK_DIR/tac.out" "tac did not reverse the line order"

printf 'stressed\n' > "$WORK_DIR/rev.txt"
"${TEST_BIN_DIR}/rev" "$WORK_DIR/rev.txt" > "$WORK_DIR/rev.out"
assert_file_contains "$WORK_DIR/rev.out" '^desserts$' "rev did not reverse the characters in the line"

printf 'äö🙂\n' > "$WORK_DIR/unicode_rev.txt"
"${TEST_BIN_DIR}/rev" "$WORK_DIR/unicode_rev.txt" > "$WORK_DIR/unicode_rev.out"
assert_file_contains "$WORK_DIR/unicode_rev.out" '^🙂öä$' "rev did not preserve UTF-8 characters while reversing"

printf 'a\314\210b\n' > "$WORK_DIR/combining_rev.txt"
"${TEST_BIN_DIR}/rev" "$WORK_DIR/combining_rev.txt" > "$WORK_DIR/combining_rev.out"
printf 'ba\314\210\n' > "$WORK_DIR/combining_rev.expected"
assert_files_equal "$WORK_DIR/combining_rev.expected" "$WORK_DIR/combining_rev.out" "rev did not keep combining marks with their base character"

printf 'ab\033[31m\n' > "$WORK_DIR/escape_rev.txt"
"${TEST_BIN_DIR}/rev" "$WORK_DIR/escape_rev.txt" > "$WORK_DIR/escape_rev.out"
printf '\033[31mba\n' > "$WORK_DIR/escape_rev.expected"
assert_files_equal "$WORK_DIR/escape_rev.expected" "$WORK_DIR/escape_rev.out" "rev split or byte-reversed an ANSI escape sequence"

printf 'ab\0cd\0' > "$WORK_DIR/nul_rev.txt"
"${TEST_BIN_DIR}/rev" -0 "$WORK_DIR/nul_rev.txt" > "$WORK_DIR/nul_rev.out"
printf 'ba\0dc\0' > "$WORK_DIR/nul_rev.expected"
assert_files_equal "$WORK_DIR/nul_rev.expected" "$WORK_DIR/nul_rev.out" "rev -0 did not reverse NUL-delimited records"

awk 'BEGIN { for (i = 0; i < 5000; ++i) printf "a"; printf "Z\n"; }' > "$WORK_DIR/long_rev.txt"
"${TEST_BIN_DIR}/rev" "$WORK_DIR/long_rev.txt" > "$WORK_DIR/long_rev.out"
assert_file_contains "$WORK_DIR/long_rev.out" '^Zaaaaaaaaaa' "rev did not handle records longer than the old 4096-byte buffer"

printf 'red::green::blue' | "${TEST_BIN_DIR}/tac" -s '::' > "$WORK_DIR/tac_sep.out"
tac_sep_out=$(tr -d '\r\n' < "$WORK_DIR/tac_sep.out")
assert_text_equals "$tac_sep_out" 'blue::green::red' "tac -s did not reverse custom-delimited records"
