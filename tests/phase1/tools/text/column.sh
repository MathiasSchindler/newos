#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir column)

note "phase1 text: column"

printf 'a 1\nlong 22\n' > "$WORK_DIR/input.txt"
"$ROOT_DIR/build/column" -t "$WORK_DIR/input.txt" > "$WORK_DIR/out.txt"
printf 'a     1\nlong  22\n' > "$WORK_DIR/expected.txt"
assert_files_equal "$WORK_DIR/expected.txt" "$WORK_DIR/out.txt" "column -t did not align the table cells cleanly"

printf '界 a\nxx bb\n' > "$WORK_DIR/unicode.txt"
"$ROOT_DIR/build/column" -t "$WORK_DIR/unicode.txt" > "$WORK_DIR/unicode.out"
assert_file_contains "$WORK_DIR/unicode.out" '^界  a$' "column did not align wide Unicode cells correctly"
assert_file_contains "$WORK_DIR/unicode.out" '^xx  bb$' "column did not preserve aligned output for ASCII rows"

cat > "$WORK_DIR/table.txt" <<'EOF'
name:role:team
Ada:Eng:Kernel
Bob:Ops:Infra
EOF
"$ROOT_DIR/build/column" -t -s ':' -o ' | ' "$WORK_DIR/table.txt" > "$WORK_DIR/table.out"
assert_file_contains "$WORK_DIR/table.out" '^name \| role \| team$' "column did not honor the requested separators"
