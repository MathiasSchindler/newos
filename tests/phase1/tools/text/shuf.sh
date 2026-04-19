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

"$ROOT_DIR/build/shuf" "$WORK_DIR/input.txt" > "$WORK_DIR/out.txt"
"$ROOT_DIR/build/sort" "$WORK_DIR/input.txt" > "$WORK_DIR/input.sorted"
"$ROOT_DIR/build/sort" "$WORK_DIR/out.txt" > "$WORK_DIR/out.sorted"
assert_files_equal "$WORK_DIR/input.sorted" "$WORK_DIR/out.sorted" "shuf lost or duplicated lines from the input set"
