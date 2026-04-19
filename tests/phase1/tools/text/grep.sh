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

"$ROOT_DIR/build/grep" -n 'alpha' "$WORK_DIR/input.txt" > "$WORK_DIR/out.txt"
assert_file_contains "$WORK_DIR/out.txt" '^1:alpha$' "grep -n missed the first matching line"
assert_file_contains "$WORK_DIR/out.txt" '^3:alpha beta$' "grep -n missed the later matching line"

"$ROOT_DIR/build/grep" -v 'skip' "$WORK_DIR/input.txt" > "$WORK_DIR/invert.out"
printf 'alpha\nalpha beta\n' > "$WORK_DIR/invert.expected"
assert_files_equal "$WORK_DIR/invert.expected" "$WORK_DIR/invert.out" "grep -v did not filter the excluded line"

mkdir -p "$WORK_DIR/tree/nested"
printf 'Hello\nworld\nHELLO\n' > "$WORK_DIR/tree/a.txt"
printf 'hello nested\nskip me\n' > "$WORK_DIR/tree/nested/b.txt"
"$ROOT_DIR/build/grep" -ir hello "$WORK_DIR/tree" > "$WORK_DIR/recursive.out"
assert_file_contains "$WORK_DIR/recursive.out" 'a.txt:Hello' "grep -ir missed the top-level match"
assert_file_contains "$WORK_DIR/recursive.out" 'b.txt:hello nested' "grep -ir missed the nested match"

printf 'Äpfel\nÖl\n' > "$WORK_DIR/unicode.txt"
"$ROOT_DIR/build/grep" -i -F 'äpfel' "$WORK_DIR/unicode.txt" > "$WORK_DIR/unicode_fixed.out"
assert_file_contains "$WORK_DIR/unicode_fixed.out" '^Äpfel$' "grep -i -F did not match Unicode text"
"$ROOT_DIR/build/grep" -iw 'öl' "$WORK_DIR/unicode.txt" > "$WORK_DIR/unicode_word.out"
assert_file_contains "$WORK_DIR/unicode_word.out" '^Öl$' "grep -iw did not match a Unicode whole word"

printf 'Hello\nNope\nHello\n' > "$WORK_DIR/count.txt"
grep_count=$("$ROOT_DIR/build/grep" -c Hello "$WORK_DIR/count.txt" | tr -d '\r\n')
assert_text_equals "$grep_count" '2' "grep -c reported the wrong match count"
assert_command_succeeds "$ROOT_DIR/build/grep" -q Hello "$WORK_DIR/count.txt"

printf 'X("src/shared/runtime/memory.c")\nX("src/shared/tool_io.c")\nX("src/shared/archive_util.c")\n' > "$WORK_DIR/manifest.txt"
"$ROOT_DIR/build/grep" -oE '"src/shared/(runtime/[^"]+|tool_[^"]+|archive_util|bignum)\.c"' "$WORK_DIR/manifest.txt" > "$WORK_DIR/manifest.out"
assert_file_contains "$WORK_DIR/manifest.out" '^"src/shared/runtime/memory.c"$' "grep -oE did not extract the runtime manifest entry"
assert_file_contains "$WORK_DIR/manifest.out" '^"src/shared/tool_io.c"$' "grep -oE did not extract the tool manifest entry"
assert_file_contains "$WORK_DIR/manifest.out" '^"src/shared/archive_util.c"$' "grep -oE did not extract the archive manifest entry"

awk 'BEGIN { for (i = 0; i < 7000; ++i) printf "A"; printf "MARKER\n"; }' > "$WORK_DIR/long.txt"
"$ROOT_DIR/build/grep" 'MARKER' "$WORK_DIR/long.txt" > "$WORK_DIR/long.out"
assert_file_contains "$WORK_DIR/long.out" 'MARKER' "grep failed to match text at the end of a long input line"

"$ROOT_DIR/build/grep" --color=always 'alpha' "$WORK_DIR/input.txt" > "$WORK_DIR/color.out"
if ! LC_ALL=C grep -q "$(printf '\033')\\[" "$WORK_DIR/color.out"; then
    fail "grep --color=always did not emit ANSI color sequences"
fi

"$ROOT_DIR/build/grep" --color=never 'alpha' "$WORK_DIR/input.txt" > "$WORK_DIR/plain.out"
if LC_ALL=C grep -q "$(printf '\033')\\[" "$WORK_DIR/plain.out"; then
    fail "grep --color=never unexpectedly emitted ANSI color sequences"
fi
