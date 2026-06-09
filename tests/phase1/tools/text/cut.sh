#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir cut)

note "phase1 text: cut"

printf 'left:middle:right\n' > "$WORK_DIR/fields.txt"
"${TEST_BIN_DIR}/cut" -d : -f 2 "$WORK_DIR/fields.txt" > "$WORK_DIR/field.out"
assert_file_contains "$WORK_DIR/field.out" '^middle$' "cut -d/-f selected the wrong field"

printf 'ABCDE\n' > "$WORK_DIR/chars.txt"
"${TEST_BIN_DIR}/cut" -c 2-4 "$WORK_DIR/chars.txt" > "$WORK_DIR/chars.out"
assert_file_contains "$WORK_DIR/chars.out" '^BCD$' "cut -c selected the wrong character range"

printf 'name:role:team\nAda:Eng:Kernel\nBob:Ops:Infra\n' > "$WORK_DIR/fields_more.txt"
"${TEST_BIN_DIR}/cut" -d ':' -f 1,3 "$WORK_DIR/fields_more.txt" > "$WORK_DIR/fields_more.out"
printf 'name:team\nAda:Kernel\nBob:Infra\n' > "$WORK_DIR/fields_more.expected"
assert_files_equal "$WORK_DIR/fields_more.expected" "$WORK_DIR/fields_more.out" "cut -f selected the wrong fields"

"${TEST_BIN_DIR}/cut" --complement -d ':' -f 2 "$WORK_DIR/fields_more.txt" > "$WORK_DIR/complement.out"
assert_files_equal "$WORK_DIR/fields_more.expected" "$WORK_DIR/complement.out" "cut --complement returned the wrong columns"

printf 'ÄÖ🙂Z\n' > "$WORK_DIR/unicode.txt"
"${TEST_BIN_DIR}/cut" -c 2-3 "$WORK_DIR/unicode.txt" > "$WORK_DIR/unicode.out"
assert_file_contains "$WORK_DIR/unicode.out" '^Ö🙂$' "cut did not select Unicode character positions correctly"

many_ranges=''
i=1
while [ "$i" -le 40 ]; do
	if [ -n "$many_ranges" ]; then
		many_ranges="$many_ranges,"
	fi
	many_ranges="$many_ranges$i"
	i=$((i + 1))
done
printf 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN\n' > "$WORK_DIR/many_ranges.txt"
"${TEST_BIN_DIR}/cut" -c "$many_ranges" "$WORK_DIR/many_ranges.txt" > "$WORK_DIR/many_ranges.out"
assert_file_contains "$WORK_DIR/many_ranges.out" '^abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN$' "cut should accept more than 32 ranges"

awk 'BEGIN { for (i = 0; i < 9000; ++i) printf "x"; printf "Z\n"; }' > "$WORK_DIR/long_line.txt"
"${TEST_BIN_DIR}/cut" -c 9001 "$WORK_DIR/long_line.txt" > "$WORK_DIR/long_line.out"
assert_file_contains "$WORK_DIR/long_line.out" '^Z$' "cut should handle records longer than 8192 bytes"

printf 'one:two\0alpha:beta\0' > "$WORK_DIR/nul_fields.txt"
"${TEST_BIN_DIR}/cut" -z -d ':' -f 2 "$WORK_DIR/nul_fields.txt" > "$WORK_DIR/nul_fields.out"
printf 'two\0beta\0' > "$WORK_DIR/nul_fields.expected"
assert_files_equal "$WORK_DIR/nul_fields.expected" "$WORK_DIR/nul_fields.out" "cut -z did not use NUL-terminated records"

printf 'ÄÖ\n' > "$WORK_DIR/byte_mode.txt"
"${TEST_BIN_DIR}/cut" -b 1 "$WORK_DIR/byte_mode.txt" > "$WORK_DIR/byte_mode.out"
od -An -tx1 "$WORK_DIR/byte_mode.out" | tr -d ' \n' > "$WORK_DIR/byte_mode.hex"
assert_text_equals "$(cat "$WORK_DIR/byte_mode.hex")" 'c30a' "cut -b should operate on bytes, not UTF-8 characters"
