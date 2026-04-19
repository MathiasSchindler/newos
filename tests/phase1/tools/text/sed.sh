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

"$ROOT_DIR/build/sed" -f "$WORK_DIR/script.sed" "$WORK_DIR/input.txt" > "$WORK_DIR/out.txt"
printf 'red\ngreen\n' > "$WORK_DIR/expected.txt"
assert_files_equal "$WORK_DIR/expected.txt" "$WORK_DIR/out.txt" "sed script application regressed"
