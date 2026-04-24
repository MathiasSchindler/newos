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

printf 'delta\nalpha\ncharlie\n' | "$ROOT_DIR/build/sort" > "$WORK_DIR/stdin_sorted.out"
cat > "$WORK_DIR/stdin_sorted.expected" <<'EOF'
alpha
charlie
delta
EOF
assert_files_equal "$WORK_DIR/stdin_sorted.expected" "$WORK_DIR/stdin_sorted.out" "sort did not read and order standard input"

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

cat > "$WORK_DIR/key_fields.txt" <<'EOF'
zeta:2:last
alpha:10:middle
beta:1:first
EOF
"$ROOT_DIR/build/sort" -t : -k 2,2 "$WORK_DIR/key_fields.txt" > "$WORK_DIR/key_fields_lex.out"
cat > "$WORK_DIR/key_fields_lex.expected" <<'EOF'
beta:1:first
alpha:10:middle
zeta:2:last
EOF
assert_files_equal "$WORK_DIR/key_fields_lex.expected" "$WORK_DIR/key_fields_lex.out" "sort -t/-k did not order by a separated lexical field"

"$ROOT_DIR/build/sort" -n -t : -k 2,2 "$WORK_DIR/key_fields.txt" > "$WORK_DIR/key_fields_numeric.out"
cat > "$WORK_DIR/key_fields_numeric.expected" <<'EOF'
beta:1:first
zeta:2:last
alpha:10:middle
EOF
assert_files_equal "$WORK_DIR/key_fields_numeric.expected" "$WORK_DIR/key_fields_numeric.out" "sort -n -t/-k did not order by a separated numeric field"

cat > "$WORK_DIR/blank_keys.txt" <<'EOF'
row-c    3
row-a    1
row-b    2
EOF
"$ROOT_DIR/build/sort" -k 2,2 "$WORK_DIR/blank_keys.txt" > "$WORK_DIR/blank_keys.out"
cat > "$WORK_DIR/blank_keys.expected" <<'EOF'
row-a    1
row-b    2
row-c    3
EOF
assert_files_equal "$WORK_DIR/blank_keys.expected" "$WORK_DIR/blank_keys.out" "sort -k did not order by a blank-separated field"

cat > "$WORK_DIR/dictionary.txt" <<'EOF'
a-2
a 1
a_0
a10
EOF
"$ROOT_DIR/build/sort" -d "$WORK_DIR/dictionary.txt" > "$WORK_DIR/dictionary.out"
cat > "$WORK_DIR/dictionary.expected" <<'EOF'
a 1
a_0
a10
a-2
EOF
assert_files_equal "$WORK_DIR/dictionary.expected" "$WORK_DIR/dictionary.out" "sort -d did not ignore non-dictionary characters"

printf 'a\002y\na\001z\naz\n' > "$WORK_DIR/nonprinting.txt"
"$ROOT_DIR/build/sort" -i "$WORK_DIR/nonprinting.txt" > "$WORK_DIR/nonprinting.out"
printf 'a\002y\na\001z\naz\n' > "$WORK_DIR/nonprinting.expected"
assert_files_equal "$WORK_DIR/nonprinting.expected" "$WORK_DIR/nonprinting.out" "sort -i did not ignore non-printing characters during comparison"

cat > "$WORK_DIR/months.txt" <<'EOF'
Dec report
Jan report
aug report
Mar report
EOF
"$ROOT_DIR/build/sort" -M "$WORK_DIR/months.txt" > "$WORK_DIR/months.out"
cat > "$WORK_DIR/months.expected" <<'EOF'
Jan report
Mar report
aug report
Dec report
EOF
assert_files_equal "$WORK_DIR/months.expected" "$WORK_DIR/months.out" "sort -M did not order month names"

cat > "$WORK_DIR/versions.txt" <<'EOF'
file-10
file-2
file-01
file-1
EOF
"$ROOT_DIR/build/sort" -V "$WORK_DIR/versions.txt" > "$WORK_DIR/versions.out"
cat > "$WORK_DIR/versions.expected" <<'EOF'
file-1
file-01
file-2
file-10
EOF
assert_files_equal "$WORK_DIR/versions.expected" "$WORK_DIR/versions.out" "sort -V did not order version-like text naturally"

cat > "$WORK_DIR/human_sizes.txt" <<'EOF'
1K
950
2M
1.5K
EOF
"$ROOT_DIR/build/sort" --human-numeric-sort "$WORK_DIR/human_sizes.txt" > "$WORK_DIR/human_sizes.out"
cat > "$WORK_DIR/human_sizes.expected" <<'EOF'
950
1K
1.5K
2M
EOF
assert_files_equal "$WORK_DIR/human_sizes.expected" "$WORK_DIR/human_sizes.out" "sort --human-numeric-sort did not order scaled sizes"

cat > "$WORK_DIR/key_human.txt" <<'EOF'
pkg-a 2M
pkg-b 512K
pkg-c 1G
EOF
"$ROOT_DIR/build/sort" --human-numeric-sort -k 2,2 "$WORK_DIR/key_human.txt" > "$WORK_DIR/key_human.out"
cat > "$WORK_DIR/key_human.expected" <<'EOF'
pkg-b 512K
pkg-a 2M
pkg-c 1G
EOF
assert_files_equal "$WORK_DIR/key_human.expected" "$WORK_DIR/key_human.out" "sort --human-numeric-sort did not apply to selected keys"

printf 'banana\nApple\ncarrot\n' > "$WORK_DIR/check_unsorted.txt"
if "$ROOT_DIR/build/sort" -c "$WORK_DIR/check_unsorted.txt" > /dev/null 2> "$WORK_DIR/check_unsorted.err"; then
    fail "sort -c accepted unsorted input"
fi
assert_file_contains "$WORK_DIR/check_unsorted.err" '^sort: disorder: Apple$' "sort -c did not report the first disorder"

if "$ROOT_DIR/build/sort" -C "$WORK_DIR/check_unsorted.txt" > "$WORK_DIR/check_quiet.out" 2> "$WORK_DIR/check_quiet.err"; then
    fail "sort -C accepted unsorted input"
fi
[ ! -s "$WORK_DIR/check_quiet.out" ] || fail "sort -C wrote output for an unsorted input"
[ ! -s "$WORK_DIR/check_quiet.err" ] || fail "sort -C reported disorder despite quiet mode"

printf 'Apple\nbanana\ncarrot\n' > "$WORK_DIR/check_sorted.txt"
"$ROOT_DIR/build/sort" -c "$WORK_DIR/check_sorted.txt" > /dev/null 2> "$WORK_DIR/check_sorted.err" || \
    fail "sort -c rejected already sorted input"

printf 'alpha\nbeta\n' > "$WORK_DIR/merge_a.txt"
printf 'beta\ngamma\n' > "$WORK_DIR/merge_b.txt"
"$ROOT_DIR/build/sort" -m -u "$WORK_DIR/merge_a.txt" "$WORK_DIR/merge_b.txt" > "$WORK_DIR/merge.out"
printf 'alpha\nbeta\ngamma\n' > "$WORK_DIR/merge.expected"
assert_files_equal "$WORK_DIR/merge.expected" "$WORK_DIR/merge.out" "sort -m -u did not merge sorted inputs correctly"

merge_args=""
value=1
while [ "$value" -le 8 ]; do
    printf '%03d\n' "$value" > "$WORK_DIR/merge_ok_$value.txt"
    merge_args="$merge_args $WORK_DIR/merge_ok_$value.txt"
    value=$((value + 1))
done
"$ROOT_DIR/build/sort" -m $merge_args > "$WORK_DIR/merge_limit_ok.out"
cat > "$WORK_DIR/merge_limit_ok.expected" <<'EOF'
001
002
003
004
005
006
007
008
EOF
assert_files_equal "$WORK_DIR/merge_limit_ok.expected" "$WORK_DIR/merge_limit_ok.out" "sort -m rejected or misordered the maximum supported input count"

printf 'pear\napple\nbanana\n' > "$WORK_DIR/output_file.txt"
"$ROOT_DIR/build/sort" -o "$WORK_DIR/output_file.out" "$WORK_DIR/output_file.txt"
cat > "$WORK_DIR/output_file.expected" <<'EOF'
apple
banana
pear
EOF
assert_files_equal "$WORK_DIR/output_file.expected" "$WORK_DIR/output_file.out" "sort -o did not write sorted output to the requested file"

cp "$WORK_DIR/output_file.txt" "$WORK_DIR/output_in_place.txt"
"$ROOT_DIR/build/sort" -o "$WORK_DIR/output_in_place.txt" "$WORK_DIR/output_in_place.txt"
assert_files_equal "$WORK_DIR/output_file.expected" "$WORK_DIR/output_in_place.txt" "sort -o did not safely rewrite an input file after buffering it"

if "$ROOT_DIR/build/sort" -k 184467440737095516151 "$WORK_DIR/input.txt" > /dev/null 2> "$WORK_DIR/sort_key.err"
then
    fail "sort accepted an overflowing key specification"
fi
assert_file_contains "$WORK_DIR/sort_key.err" '^Usage: sort ' "sort did not reject an overflowing key specification"

: > "$WORK_DIR/max_lines.txt"
value=8192
while [ "$value" -ge 1 ]; do
    printf '%04d\n' "$value" >> "$WORK_DIR/max_lines.txt"
    value=$((value - 1))
done
"$ROOT_DIR/build/sort" "$WORK_DIR/max_lines.txt" > "$WORK_DIR/max_lines.out"
max_lines_count=$(wc -l < "$WORK_DIR/max_lines.out" | tr -d ' \r\n')
max_lines_first=$(head -n 1 "$WORK_DIR/max_lines.out" | tr -d '\r\n')
max_lines_last=$(tail -n 1 "$WORK_DIR/max_lines.out" | tr -d '\r\n')
assert_text_equals "$max_lines_count" '8192' "sort dropped lines at its maximum buffered line count"
assert_text_equals "$max_lines_first" '0001' "sort did not order the first line at its maximum buffered line count"
assert_text_equals "$max_lines_last" '8192' "sort did not order the final line at its maximum buffered line count"

: > "$WORK_DIR/external_lines.txt"
value=20000
while [ "$value" -ge 1 ]; do
    printf '%05d\n' "$value" >> "$WORK_DIR/external_lines.txt"
    value=$((value - 1))
done
"$ROOT_DIR/build/sort" "$WORK_DIR/external_lines.txt" > "$WORK_DIR/external_lines.out"
external_lines_count=$(wc -l < "$WORK_DIR/external_lines.out" | tr -d ' \r\n')
external_lines_first=$(head -n 1 "$WORK_DIR/external_lines.out" | tr -d '\r\n')
external_lines_last=$(tail -n 1 "$WORK_DIR/external_lines.out" | tr -d '\r\n')
assert_text_equals "$external_lines_count" '20000' "sort dropped lines while using external temporary runs"
assert_text_equals "$external_lines_first" '00001' "sort did not order the first external-sort line"
assert_text_equals "$external_lines_last" '20000' "sort did not order the final external-sort line"

: > "$WORK_DIR/external_duplicates.txt"
value=12000
while [ "$value" -ge 1 ]; do
    printf '%04d\n' $((value % 300)) >> "$WORK_DIR/external_duplicates.txt"
    value=$((value - 1))
done
"$ROOT_DIR/build/sort" -nu "$WORK_DIR/external_duplicates.txt" > "$WORK_DIR/external_duplicates.out"
external_unique_count=$(wc -l < "$WORK_DIR/external_duplicates.out" | tr -d ' \r\n')
external_unique_first=$(head -n 1 "$WORK_DIR/external_duplicates.out" | tr -d '\r\n')
external_unique_last=$(tail -n 1 "$WORK_DIR/external_duplicates.out" | tr -d '\r\n')
assert_text_equals "$external_unique_count" '300' "sort -u did not deduplicate across external temporary runs"
assert_text_equals "$external_unique_first" '0000' "sort -nu lost the lowest external duplicate key"
assert_text_equals "$external_unique_last" '0299' "sort -nu lost the highest external duplicate key"

cp "$WORK_DIR/external_lines.txt" "$WORK_DIR/external_in_place.txt"
"$ROOT_DIR/build/sort" -o "$WORK_DIR/external_in_place.txt" "$WORK_DIR/external_in_place.txt"
external_in_place_first=$(head -n 1 "$WORK_DIR/external_in_place.txt" | tr -d '\r\n')
external_in_place_last=$(tail -n 1 "$WORK_DIR/external_in_place.txt" | tr -d '\r\n')
assert_text_equals "$external_in_place_first" '00001' "sort -o did not safely rewrite a large input file"
assert_text_equals "$external_in_place_last" '20000' "sort -o truncated a large input file before external sorting"

merge_args=""
value=1
while [ "$value" -le 9 ]; do
    printf '%s\n' "$value" > "$WORK_DIR/merge_limit_$value.txt"
    merge_args="$merge_args $WORK_DIR/merge_limit_$value.txt"
    value=$((value + 1))
done
if "$ROOT_DIR/build/sort" -m $merge_args > "$WORK_DIR/merge_limit.out" 2> "$WORK_DIR/merge_limit.err"
then
    fail "sort -m accepted more inputs than its bounded merge state allows"
fi
assert_file_contains "$WORK_DIR/merge_limit.err" '^sort: too many inputs for merge mode$' "sort -m did not report bounded merge input exhaustion"
