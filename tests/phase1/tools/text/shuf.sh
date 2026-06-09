#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir shuf)

note "phase1 text: shuf"

cat > "$WORK_DIR/input.txt" <<'EOF'
red
blue
green
EOF

"${TEST_BIN_DIR}/shuf" "$WORK_DIR/input.txt" > "$WORK_DIR/out.txt"
"${TEST_BIN_DIR}/sort" "$WORK_DIR/input.txt" > "$WORK_DIR/input.sorted"
"${TEST_BIN_DIR}/sort" "$WORK_DIR/out.txt" > "$WORK_DIR/out.sorted"
assert_files_equal "$WORK_DIR/input.sorted" "$WORK_DIR/out.sorted" "shuf lost or duplicated lines from the input set"

printf 'red\0blue sky\0green\0' > "$WORK_DIR/zero.in"
printf 'abcdefgh' > "$WORK_DIR/random.bin"
assert_command_succeeds "${TEST_BIN_DIR}/shuf" -z -n 2 --random-source="$WORK_DIR/random.bin" -o "$WORK_DIR/zero.out" "$WORK_DIR/zero.in"
assert_command_succeeds "${TEST_BIN_DIR}/shuf" -z -n 2 --random-source="$WORK_DIR/random.bin" -o "$WORK_DIR/zero_again.out" "$WORK_DIR/zero.in"
assert_files_equal "$WORK_DIR/zero.out" "$WORK_DIR/zero_again.out" "shuf --random-source should be deterministic for the same byte stream"
