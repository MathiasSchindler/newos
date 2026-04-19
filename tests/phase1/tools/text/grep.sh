#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir grep)

note "phase1 text: grep"

cat > "$WORK_DIR/input.txt" <<'EOF'
alpha
skip
alpha beta
EOF

"$ROOT_DIR/build/grep" -n 'alpha' "$WORK_DIR/input.txt" > "$WORK_DIR/out.txt"
assert_file_contains "$WORK_DIR/out.txt" '^1:alpha$' "grep -n missed the first matching line"
assert_file_contains "$WORK_DIR/out.txt" '^3:alpha beta$' "grep -n missed the later matching line"

"$ROOT_DIR/build/grep" -v 'skip' "$WORK_DIR/input.txt" > "$WORK_DIR/invert.out"
printf 'alpha\nalpha beta\n' > "$WORK_DIR/invert.expected"
assert_files_equal "$WORK_DIR/invert.expected" "$WORK_DIR/invert.out" "grep -v did not filter the excluded line"
