#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir sed)

note "phase1 text: sed"

cat > "$WORK_DIR/input.txt" <<'EOF'
red
blue
red
EOF

cat > "$WORK_DIR/script.sed" <<'EOF'
2s/blue/green/
3d
EOF

"${TEST_BIN_DIR}/sed" -f "$WORK_DIR/script.sed" "$WORK_DIR/input.txt" > "$WORK_DIR/out.txt"
printf 'red\ngreen\n' > "$WORK_DIR/expected.txt"
assert_files_equal "$WORK_DIR/expected.txt" "$WORK_DIR/out.txt" "sed script application regressed"

printf 'Über\n' > "$WORK_DIR/unicode.txt"
"${TEST_BIN_DIR}/sed" 's/Über/Ueber/' "$WORK_DIR/unicode.txt" > "$WORK_DIR/unicode.out"
assert_file_contains "$WORK_DIR/unicode.out" '^Ueber$' "sed did not substitute a Unicode pattern correctly"

printf 'keep\ndrop\nstop\nlater\n' > "$WORK_DIR/delete.txt"
"${TEST_BIN_DIR}/sed" '/drop/d' "$WORK_DIR/delete.txt" > "$WORK_DIR/delete.out"
printf 'keep\nstop\nlater\n' > "$WORK_DIR/delete.expected"
assert_files_equal "$WORK_DIR/delete.expected" "$WORK_DIR/delete.out" "sed delete command failed"

printf 'red\nblue\ngreen\n' > "$WORK_DIR/ere.txt"
"${TEST_BIN_DIR}/sed" -E 's/(red|blue)/color/' "$WORK_DIR/ere.txt" > "$WORK_DIR/ere.out"
printf 'color\ncolor\ngreen\n' > "$WORK_DIR/ere.expected"
assert_files_equal "$WORK_DIR/ere.expected" "$WORK_DIR/ere.out" "sed -E did not enable extended regex syntax"

printf 'red\0blue\0' | "${TEST_BIN_DIR}/sed" -z 's/blue/green/' | tr '\0' '\n' > "$WORK_DIR/zero.out"
printf 'red\ngreen\n' > "$WORK_DIR/zero.expected"
assert_files_equal "$WORK_DIR/zero.expected" "$WORK_DIR/zero.out" "sed -z did not process NUL-delimited records"

printf 'red\nblue\n' > "$WORK_DIR/inplace.txt"
assert_command_succeeds "${TEST_BIN_DIR}/sed" -i.bak '2c azure' "$WORK_DIR/inplace.txt"
printf 'red\nazure\n' > "$WORK_DIR/inplace.expected"
assert_files_equal "$WORK_DIR/inplace.expected" "$WORK_DIR/inplace.txt" "sed -i did not rewrite the file in place"
assert_file_contains "$WORK_DIR/inplace.txt.bak" '^blue$' "sed -i backup suffix did not preserve the original content"
