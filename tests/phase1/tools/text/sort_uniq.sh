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

"$ROOT_DIR/build/uniq" -c "$WORK_DIR/sorted.out" > "$WORK_DIR/uniq.out"
assert_file_contains "$WORK_DIR/uniq.out" '^2 apple$' "uniq -c did not count duplicate lines correctly"
assert_file_contains "$WORK_DIR/uniq.out" '^1 banana$' "uniq -c lost a unique line"
assert_file_contains "$WORK_DIR/uniq.out" '^1 pear$' "uniq -c lost the final unique line"
