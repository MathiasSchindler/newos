#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir tac_rev)

note "phase1 text: tac/rev"

cat > "$WORK_DIR/lines.txt" <<'EOF'
top
middle
bottom
EOF

"$ROOT_DIR/build/tac" "$WORK_DIR/lines.txt" > "$WORK_DIR/tac.out"
cat > "$WORK_DIR/tac.expected" <<'EOF'
bottom
middle
top
EOF
assert_files_equal "$WORK_DIR/tac.expected" "$WORK_DIR/tac.out" "tac did not reverse the line order"

printf 'stressed\n' > "$WORK_DIR/rev.txt"
"$ROOT_DIR/build/rev" "$WORK_DIR/rev.txt" > "$WORK_DIR/rev.out"
assert_file_contains "$WORK_DIR/rev.out" '^desserts$' "rev did not reverse the characters in the line"
