#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir cut)

note "phase1 text: cut"

printf 'left:middle:right\n' > "$WORK_DIR/fields.txt"
"$ROOT_DIR/build/cut" -d : -f 2 "$WORK_DIR/fields.txt" > "$WORK_DIR/field.out"
assert_file_contains "$WORK_DIR/field.out" '^middle$' "cut -d/-f selected the wrong field"

printf 'ABCDE\n' > "$WORK_DIR/chars.txt"
"$ROOT_DIR/build/cut" -c 2-4 "$WORK_DIR/chars.txt" > "$WORK_DIR/chars.out"
assert_file_contains "$WORK_DIR/chars.out" '^BCD$' "cut -c selected the wrong character range"

printf 'name:role:team\nAda:Eng:Kernel\nBob:Ops:Infra\n' > "$WORK_DIR/fields_more.txt"
"$ROOT_DIR/build/cut" -d ':' -f 1,3 "$WORK_DIR/fields_more.txt" > "$WORK_DIR/fields_more.out"
printf 'name:team\nAda:Kernel\nBob:Infra\n' > "$WORK_DIR/fields_more.expected"
assert_files_equal "$WORK_DIR/fields_more.expected" "$WORK_DIR/fields_more.out" "cut -f selected the wrong fields"

"$ROOT_DIR/build/cut" --complement -d ':' -f 2 "$WORK_DIR/fields_more.txt" > "$WORK_DIR/complement.out"
assert_files_equal "$WORK_DIR/fields_more.expected" "$WORK_DIR/complement.out" "cut --complement returned the wrong columns"

printf 'ÄÖ🙂Z\n' > "$WORK_DIR/unicode.txt"
"$ROOT_DIR/build/cut" -c 2-3 "$WORK_DIR/unicode.txt" > "$WORK_DIR/unicode.out"
assert_file_contains "$WORK_DIR/unicode.out" '^Ö🙂$' "cut did not select Unicode character positions correctly"
