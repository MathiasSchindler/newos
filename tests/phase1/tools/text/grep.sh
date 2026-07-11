#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir grep)

note "phase1 text: grep"

cat > "$WORK_DIR/input.txt" <<'EOF'
alpha
skip
alpha beta
EOF

"${TEST_BIN_DIR}/grep" -n 'alpha' "$WORK_DIR/input.txt" > "$WORK_DIR/out.txt"
assert_file_contains "$WORK_DIR/out.txt" '^1:alpha$' "grep -n missed the first matching line"
assert_file_contains "$WORK_DIR/out.txt" '^3:alpha beta$' "grep -n missed the later matching line"

"${TEST_BIN_DIR}/grep" -v 'skip' "$WORK_DIR/input.txt" > "$WORK_DIR/invert.out"
printf 'alpha\nalpha beta\n' > "$WORK_DIR/invert.expected"
assert_files_equal "$WORK_DIR/invert.expected" "$WORK_DIR/invert.out" "grep -v did not filter the excluded line"

mkdir -p "$WORK_DIR/tree/nested"
printf 'Hello\nworld\nHELLO\n' > "$WORK_DIR/tree/a.txt"
printf 'hello nested\nskip me\n' > "$WORK_DIR/tree/nested/b.txt"
"${TEST_BIN_DIR}/grep" -ir hello "$WORK_DIR/tree" > "$WORK_DIR/recursive.out"
assert_file_contains "$WORK_DIR/recursive.out" 'a.txt:Hello' "grep -ir missed the top-level match"
assert_file_contains "$WORK_DIR/recursive.out" 'b.txt:hello nested' "grep -ir missed the nested match"

printf 'Äpfel\nÖl\n' > "$WORK_DIR/unicode.txt"
"${TEST_BIN_DIR}/grep" -i -F 'äpfel' "$WORK_DIR/unicode.txt" > "$WORK_DIR/unicode_fixed.out"
assert_file_contains "$WORK_DIR/unicode_fixed.out" '^Äpfel$' "grep -i -F did not match Unicode text"
"${TEST_BIN_DIR}/grep" -iw 'öl' "$WORK_DIR/unicode.txt" > "$WORK_DIR/unicode_word.out"
assert_file_contains "$WORK_DIR/unicode_word.out" '^Öl$' "grep -iw did not match a Unicode whole word"

printf 'مرحبا،عالم\ncafé noir\ncan\047t stop\n' > "$WORK_DIR/unicode_boundaries.txt"
"${TEST_BIN_DIR}/grep" -w 'مرحبا' "$WORK_DIR/unicode_boundaries.txt" > "$WORK_DIR/arabic_word.out"
assert_file_contains "$WORK_DIR/arabic_word.out" '^مرحبا،عالم$' "grep -w did not recognize Unicode punctuation as a word boundary"
if "${TEST_BIN_DIR}/grep" -w 'cafe' "$WORK_DIR/unicode_boundaries.txt" > /dev/null; then
    fail "grep -w split a base letter from its combining mark"
fi
if "${TEST_BIN_DIR}/grep" -w 'can' "$WORK_DIR/unicode_boundaries.txt" > /dev/null; then
    fail "grep -w split a word at an internal apostrophe"
fi

printf 'cat\nscatter\ncat_\n' > "$WORK_DIR/ascii_word.txt"
"${TEST_BIN_DIR}/grep" -Fw 'cat' "$WORK_DIR/ascii_word.txt" > "$WORK_DIR/ascii_word.out"
printf 'cat\n' > "$WORK_DIR/ascii_word.expected"
assert_files_equal "$WORK_DIR/ascii_word.expected" "$WORK_DIR/ascii_word.out" "grep -Fw did not respect ASCII word boundaries"

printf 'Hello\nNope\nHello\n' > "$WORK_DIR/count.txt"
grep_count=$("${TEST_BIN_DIR}/grep" -c Hello "$WORK_DIR/count.txt" | tr -d '\r\n')
assert_text_equals "$grep_count" '2' "grep -c reported the wrong match count"
assert_command_succeeds "${TEST_BIN_DIR}/grep" -q Hello "$WORK_DIR/count.txt"

printf 'X("src/shared/runtime/memory.c")\nX("src/shared/tool_io.c")\nX("src/shared/archive_util.c")\n' > "$WORK_DIR/manifest.txt"
"${TEST_BIN_DIR}/grep" -oE '"src/shared/(runtime/[^"]+|tool_[^"]+|archive_util|bignum)\.c"' "$WORK_DIR/manifest.txt" > "$WORK_DIR/manifest.out"
assert_file_contains "$WORK_DIR/manifest.out" '^"src/shared/runtime/memory.c"$' "grep -oE did not extract the runtime manifest entry"
assert_file_contains "$WORK_DIR/manifest.out" '^"src/shared/tool_io.c"$' "grep -oE did not extract the tool manifest entry"
assert_file_contains "$WORK_DIR/manifest.out" '^"src/shared/archive_util.c"$' "grep -oE did not extract the archive manifest entry"

printf 'abc123\nplain\n' > "$WORK_DIR/posix-ere.txt"
"${TEST_BIN_DIR}/grep" -E '^[[:alpha:]]+[[:digit:]]+$' "$WORK_DIR/posix-ere.txt" > "$WORK_DIR/posix-ere.out"
assert_file_contains "$WORK_DIR/posix-ere.out" '^abc123$' "grep -E did not support POSIX named classes"
"${TEST_BIN_DIR}/grep" '^abc(123)$' "$WORK_DIR/posix-ere.txt" > "$WORK_DIR/bre-literal.out"
[ ! -s "$WORK_DIR/bre-literal.out" ] || fail "basic grep treated unescaped parentheses as a capture group"
"${TEST_BIN_DIR}/grep" '^abc\(123\)$' "$WORK_DIR/posix-ere.txt" > "$WORK_DIR/bre-group.out"
assert_file_contains "$WORK_DIR/bre-group.out" '^abc123$' "basic grep did not support escaped capture groups"

awk 'BEGIN { for (i = 0; i < 7000; ++i) printf "A"; printf "MARKER\n"; }' > "$WORK_DIR/long.txt"
"${TEST_BIN_DIR}/grep" 'MARKER' "$WORK_DIR/long.txt" > "$WORK_DIR/long.out"
assert_file_contains "$WORK_DIR/long.out" 'MARKER' "grep failed to match text at the end of a long input line"

"${TEST_BIN_DIR}/grep" --color=always 'alpha' "$WORK_DIR/input.txt" > "$WORK_DIR/color.out"
if ! LC_ALL=C grep -q "$(printf '\033')\\[" "$WORK_DIR/color.out"; then
    fail "grep --color=always did not emit ANSI color sequences"
fi

"${TEST_BIN_DIR}/grep" --color=never 'alpha' "$WORK_DIR/input.txt" > "$WORK_DIR/plain.out"
if LC_ALL=C grep -q "$(printf '\033')\\[" "$WORK_DIR/plain.out"; then
    fail "grep --color=never unexpectedly emitted ANSI color sequences"
fi
