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

i=0
: > "$WORK_DIR/large.in"
while [ "$i" -lt 4200 ]; do
	printf 'item-%s\n' "$i" >> "$WORK_DIR/large.in"
	i=$((i + 1))
done
"${TEST_BIN_DIR}/shuf" --random-source="$WORK_DIR/random.bin" "$WORK_DIR/large.in" > "$WORK_DIR/large.out"
[ "$(wc -l < "$WORK_DIR/large.out")" -eq 4200 ] || fail "shuf retained the old fixed item limit"

awk 'BEGIN { for (i = 0; i < 1500; ++i) printf "x"; printf "\n"; }' > "$WORK_DIR/long.in"
"${TEST_BIN_DIR}/shuf" --random-source="$WORK_DIR/random.bin" "$WORK_DIR/long.in" > "$WORK_DIR/long.out"
[ "$(wc -c < "$WORK_DIR/long.out")" -eq 1501 ] || fail "shuf truncated records at the old fixed line length"
