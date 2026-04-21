#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir split_csplit)

note "phase1 text: split/csplit"

cat > "$WORK_DIR/split.txt" <<'EOF'
one
two
three
four
EOF

"$ROOT_DIR/build/split" -l 2 "$WORK_DIR/split.txt" "$WORK_DIR/part-"
printf 'one\ntwo\n' > "$WORK_DIR/part-aa.expected"
printf 'three\nfour\n' > "$WORK_DIR/part-ab.expected"
assert_files_equal "$WORK_DIR/part-aa.expected" "$WORK_DIR/part-aa" "split did not create the first chunk correctly"
assert_files_equal "$WORK_DIR/part-ab.expected" "$WORK_DIR/part-ab" "split did not create the second chunk correctly"

cat > "$WORK_DIR/csplit.txt" <<'EOF'
alpha
beta
gamma
EOF

"$ROOT_DIR/build/csplit" -f "$WORK_DIR/chunk" "$WORK_DIR/csplit.txt" '/beta/' > "$WORK_DIR/csplit.out"
printf 'alpha\n' > "$WORK_DIR/chunk00.expected"
printf 'beta\ngamma\n' > "$WORK_DIR/chunk01.expected"
assert_files_equal "$WORK_DIR/chunk00.expected" "$WORK_DIR/chunk00" "csplit did not create the leading section correctly"
assert_files_equal "$WORK_DIR/chunk01.expected" "$WORK_DIR/chunk01" "csplit did not create the trailing section correctly"

printf 'abcdef\n' > "$WORK_DIR/split_bytes.txt"
(
    cd "$WORK_DIR"
    "$ROOT_DIR/build/split" -b 2 -a 3 -d split_bytes.txt part
)
assert_file_contains "$WORK_DIR/part000" '^ab$' "split -b/-d did not create the first numeric chunk"
assert_file_contains "$WORK_DIR/part001" '^cd$' "split -b/-d did not create the second numeric chunk"
assert_file_contains "$WORK_DIR/part002" '^ef$' "split -b/-d did not create the third numeric chunk"

if "$ROOT_DIR/build/split" -b 18446744073709551615g "$WORK_DIR/split.txt" "$WORK_DIR/overflow-" > /dev/null 2> "$WORK_DIR/split_overflow.err"
then
    fail "split accepted a byte size that overflowed its internal arithmetic"
fi
assert_file_contains "$WORK_DIR/split_overflow.err" '^Usage:' "split did not reject an overflowing -b size"

if "$ROOT_DIR/build/split" -a 18446744073709551615 "$WORK_DIR/split.txt" "$WORK_DIR/overflow-" > /dev/null 2> "$WORK_DIR/split_suffix.err"
then
    fail "split accepted an absurdly large suffix length"
fi
assert_file_contains "$WORK_DIR/split_suffix.err" 'split: too many output files for prefix ' "split did not fail safely on a huge suffix length"

if "$ROOT_DIR/build/csplit" -f "$WORK_DIR/repeat" "$WORK_DIR/csplit.txt" '/beta/{184467440737095516151}' > /dev/null 2> "$WORK_DIR/csplit_repeat.err"
then
    fail "csplit accepted a repeat count that overflowed"
fi
assert_file_contains "$WORK_DIR/csplit_repeat.err" 'csplit: invalid pattern ' "csplit did not reject an overflowing repeat count"
