#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir column)

note "phase1 text: column"

printf 'a 1\nlong 22\n' > "$WORK_DIR/input.txt"
"${TEST_BIN_DIR}/column" -t "$WORK_DIR/input.txt" > "$WORK_DIR/out.txt"
printf 'a     1\nlong  22\n' > "$WORK_DIR/expected.txt"
assert_files_equal "$WORK_DIR/expected.txt" "$WORK_DIR/out.txt" "column -t did not align the table cells cleanly"

printf '界 a\nxx bb\n' > "$WORK_DIR/unicode.txt"
"${TEST_BIN_DIR}/column" -t "$WORK_DIR/unicode.txt" > "$WORK_DIR/unicode.out"
assert_file_contains "$WORK_DIR/unicode.out" '^界  a$' "column did not align wide Unicode cells correctly"
assert_file_contains "$WORK_DIR/unicode.out" '^xx  bb$' "column did not preserve aligned output for ASCII rows"

printf '· a\nxx b\n' > "$WORK_DIR/ambiguous.txt"
NEWOS_AMBIGUOUS_WIDTH=2 "${TEST_BIN_DIR}/column" -t "$WORK_DIR/ambiguous.txt" > "$WORK_DIR/ambiguous.out"
assert_file_contains "$WORK_DIR/ambiguous.out" '^·  a$' "column did not honor wide East Asian Ambiguous characters"
assert_file_contains "$WORK_DIR/ambiguous.out" '^xx  b$' "column wide ambiguous mode misaligned ASCII cells"

printf 'é a\nxx bb\n' > "$WORK_DIR/combining.txt"
"${TEST_BIN_DIR}/column" -t "$WORK_DIR/combining.txt" > "$WORK_DIR/combining.out"
assert_file_contains "$WORK_DIR/combining.out" '^é   a$' "column counted a combining mark as visible width"
assert_file_contains "$WORK_DIR/combining.out" '^xx  bb$' "column did not preserve ASCII alignment with combining input"

printf '\033[31mred\033[0m a\nplain bb\n' > "$WORK_DIR/ansi.txt"
"${TEST_BIN_DIR}/column" -t "$WORK_DIR/ansi.txt" > "$WORK_DIR/ansi.out"
printf '\033[31mred\033[0m    a\nplain  bb\n' > "$WORK_DIR/ansi.expected"
assert_files_equal "$WORK_DIR/ansi.expected" "$WORK_DIR/ansi.out" "column counted ANSI escapes as visible table width"

cat > "$WORK_DIR/table.txt" <<'EOF'
name:role:team
Ada:Eng:Kernel
Bob:Ops:Infra
EOF
"${TEST_BIN_DIR}/column" -t -s ':' -o ' | ' "$WORK_DIR/table.txt" > "$WORK_DIR/table.out"
assert_file_contains "$WORK_DIR/table.out" '^name \| role \| team$' "column did not honor the requested separators"

i=0
: > "$WORK_DIR/large.txt"
while [ "$i" -lt 600 ]; do
	printf 'row-%s value-%s\n' "$i" "$i" >> "$WORK_DIR/large.txt"
	i=$((i + 1))
done
"${TEST_BIN_DIR}/column" -t "$WORK_DIR/large.txt" > "$WORK_DIR/large.out"
[ "$(wc -l < "$WORK_DIR/large.out")" -eq 600 ] || fail "column retained the old fixed row limit"

awk 'BEGIN { for (i = 0; i < 40; ++i) printf "%sfield%d", i ? ":" : "", i; printf "\n"; }' > "$WORK_DIR/wide.txt"
"${TEST_BIN_DIR}/column" -t -s ':' "$WORK_DIR/wide.txt" > "$WORK_DIR/wide.out"
assert_file_contains "$WORK_DIR/wide.out" 'field39' "column retained the old fixed field limit"
