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
