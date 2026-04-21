#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir sort_uniq)

note "phase1 text: sort/uniq"

cat > "$WORK_DIR/input.txt" <<'EOF'
pear
apple
banana
apple
EOF

"$ROOT_DIR/build/sort" "$WORK_DIR/input.txt" > "$WORK_DIR/sorted.out"
cat > "$WORK_DIR/sorted.expected" <<'EOF'
apple
apple
banana
pear
EOF
assert_files_equal "$WORK_DIR/sorted.expected" "$WORK_DIR/sorted.out" "sort output was not lexicographically ordered"

printf '10\n2\n2\n1\n' > "$WORK_DIR/numeric.txt"
"$ROOT_DIR/build/sort" -nru "$WORK_DIR/numeric.txt" > "$WORK_DIR/numeric.out"
printf '10\n2\n1\n' > "$WORK_DIR/numeric.expected"
assert_files_equal "$WORK_DIR/numeric.expected" "$WORK_DIR/numeric.out" "sort -nru did not preserve numeric reverse unique ordering"

"$ROOT_DIR/build/uniq" -c "$WORK_DIR/sorted.out" > "$WORK_DIR/uniq.out"
assert_file_contains "$WORK_DIR/uniq.out" '^2 apple$' "uniq -c did not count duplicate lines correctly"
assert_file_contains "$WORK_DIR/uniq.out" '^1 banana$' "uniq -c lost a unique line"
assert_file_contains "$WORK_DIR/uniq.out" '^1 pear$' "uniq -c lost the final unique line"

printf 'tag Alpha\nTAG alpha\nkeep beta\nKEEP beta\n' > "$WORK_DIR/uniq_controls.txt"
"$ROOT_DIR/build/uniq" -i -f 1 -w 5 "$WORK_DIR/uniq_controls.txt" > "$WORK_DIR/uniq_controls.out"
printf 'tag Alpha\nkeep beta\n' > "$WORK_DIR/uniq_controls.expected"
assert_files_equal "$WORK_DIR/uniq_controls.expected" "$WORK_DIR/uniq_controls.out" "uniq comparison controls failed"

: > "$WORK_DIR/large_numbers.txt"
value=1500
while [ "$value" -ge 1 ]; do
    printf '%s\n' "$value" >> "$WORK_DIR/large_numbers.txt"
    value=$((value - 1))
done
"$ROOT_DIR/build/sort" -n "$WORK_DIR/large_numbers.txt" > "$WORK_DIR/large_sorted.out"
first_sorted=$(head -n 1 "$WORK_DIR/large_sorted.out" | tr -d '\r\n')
last_sorted=$(tail -n 1 "$WORK_DIR/large_sorted.out" | tr -d '\r\n')
assert_text_equals "$first_sorted" '1' "sort -n did not place the lowest value first on a large input"
assert_text_equals "$last_sorted" '1500' "sort -n did not place the highest value last on a large input"

: > "$WORK_DIR/huge_numbers.txt"
value=3005
while [ "$value" -ge 1 ]; do
    printf '%s\n' "$value" >> "$WORK_DIR/huge_numbers.txt"
    value=$((value - 1))
done
"$ROOT_DIR/build/sort" -n "$WORK_DIR/huge_numbers.txt" > "$WORK_DIR/huge_sorted.out"
huge_first=$(head -n 1 "$WORK_DIR/huge_sorted.out" | tr -d '\r\n')
huge_last=$(tail -n 1 "$WORK_DIR/huge_sorted.out" | tr -d '\r\n')
huge_count=$(wc -l < "$WORK_DIR/huge_sorted.out" | tr -d ' \r\n')
assert_text_equals "$huge_first" '1' "sort lost numeric ordering on >2048 lines"
assert_text_equals "$huge_last" '3005' "sort lost the high end of a >2048 line input"
assert_text_equals "$huge_count" '3005' "sort dropped lines on a >2048 line input"

awk 'BEGIN { printf "b"; for (i = 0; i < 700; ++i) printf "x"; printf "\n"; printf "a"; for (i = 0; i < 700; ++i) printf "y"; printf "\n"; }' > "$WORK_DIR/sort_long_lines.txt"
"$ROOT_DIR/build/sort" "$WORK_DIR/sort_long_lines.txt" > "$WORK_DIR/sort_long_lines.out"
long_first=$(head -n 1 "$WORK_DIR/sort_long_lines.out" | cut -c 1 | tr -d '\r\n')
long_first_len=$(awk 'NR == 1 { print length($0) }' "$WORK_DIR/sort_long_lines.out" | tr -d ' \r\n')
assert_text_equals "$long_first" 'a' "sort failed lexical ordering on long lines"
assert_text_equals "$long_first_len" '701' "sort truncated a long input line"

printf 'banana\nApple\ncarrot\n' > "$WORK_DIR/casefold.txt"
"$ROOT_DIR/build/sort" -f "$WORK_DIR/casefold.txt" > "$WORK_DIR/casefold.out"
printf 'Apple\nbanana\ncarrot\n' > "$WORK_DIR/casefold.expected"
assert_files_equal "$WORK_DIR/casefold.expected" "$WORK_DIR/casefold.out" "sort -f did not ignore ASCII case"

printf 'banana\nApple\ncarrot\n' > "$WORK_DIR/check_unsorted.txt"
if "$ROOT_DIR/build/sort" -c "$WORK_DIR/check_unsorted.txt" > /dev/null 2> "$WORK_DIR/check_unsorted.err"; then
    fail "sort -c accepted unsorted input"
fi
assert_file_contains "$WORK_DIR/check_unsorted.err" '^sort: disorder: Apple$' "sort -c did not report the first disorder"

printf 'Apple\nbanana\ncarrot\n' > "$WORK_DIR/check_sorted.txt"
"$ROOT_DIR/build/sort" -c "$WORK_DIR/check_sorted.txt" > /dev/null 2> "$WORK_DIR/check_sorted.err" || \
    fail "sort -c rejected already sorted input"

printf 'alpha\nbeta\n' > "$WORK_DIR/merge_a.txt"
printf 'beta\ngamma\n' > "$WORK_DIR/merge_b.txt"
"$ROOT_DIR/build/sort" -m -u "$WORK_DIR/merge_a.txt" "$WORK_DIR/merge_b.txt" > "$WORK_DIR/merge.out"
printf 'alpha\nbeta\ngamma\n' > "$WORK_DIR/merge.expected"
assert_files_equal "$WORK_DIR/merge.expected" "$WORK_DIR/merge.out" "sort -m -u did not merge sorted inputs correctly"

if "$ROOT_DIR/build/sort" -k 184467440737095516151 "$WORK_DIR/input.txt" > /dev/null 2> "$WORK_DIR/sort_key.err"
then
    fail "sort accepted an overflowing key specification"
fi
assert_file_contains "$WORK_DIR/sort_key.err" '^Usage: sort ' "sort did not reject an overflowing key specification"
