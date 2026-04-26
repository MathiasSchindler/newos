#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir ed)

note "phase1 text: ed"

cat > "$WORK_DIR/input.txt" <<'EOF'
one
two
three
EOF

cat > "$WORK_DIR/commands.ed" <<'EOF'
1,$s/two/TWO/g
$a
four
.
1,$n
w
q
EOF

"$ROOT_DIR/build/ed" "$WORK_DIR/input.txt" < "$WORK_DIR/commands.ed" > "$WORK_DIR/ed.out"
assert_file_contains "$WORK_DIR/ed.out" '^2[[:space:]]TWO$' "ed substitution or numbered printing failed"
assert_file_contains "$WORK_DIR/ed.out" '^4[[:space:]]four$' "ed append command failed"
cat > "$WORK_DIR/expected.txt" <<'EOF'
one
TWO
three
four
EOF
assert_files_equal "$WORK_DIR/expected.txt" "$WORK_DIR/input.txt" "ed did not write the edited file"