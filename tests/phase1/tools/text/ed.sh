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

cat > "$WORK_DIR/advanced.txt" <<'EOF'
alpha
beta
alps
foo42
EOF

cat > "$WORK_DIR/read.txt" <<'EOF'
tail
EOF

cat > "$WORK_DIR/advanced.ed" <<EOF
g/^al/s/a/A/
4s/([a-z]+)([0-9]+)/\\2-\\1/
1,~2p
2d
u
\$r $WORK_DIR/read.txt
1,\$n
w
q
EOF

"$ROOT_DIR/build/ed" "$WORK_DIR/advanced.txt" < "$WORK_DIR/advanced.ed" > "$WORK_DIR/advanced.out"
assert_file_contains "$WORK_DIR/advanced.out" '^Alpha$' "ed global command or stepped range did not print the first expected line"
assert_file_contains "$WORK_DIR/advanced.out" '^beta$' "ed stepped range did not include the expected boundary line"
assert_file_contains "$WORK_DIR/advanced.out" '^4[[:space:]]42-foo$' "ed regex class/back-reference substitution failed"
assert_file_contains "$WORK_DIR/advanced.out" '^5[[:space:]]tail$' "ed read command did not insert additional file content"
cat > "$WORK_DIR/advanced.expected" <<'EOF'
Alpha
beta
Alps
42-foo
tail
EOF
assert_files_equal "$WORK_DIR/advanced.expected" "$WORK_DIR/advanced.txt" "ed advanced editing commands did not write expected content"
