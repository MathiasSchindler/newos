#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir awk)

note "phase1 text: awk"

printf 'alpha one\nbeta two three\nalpha four\n' > "$WORK_DIR/input.txt"
"$ROOT_DIR/build/awk" 'BEGIN { print "begin" } /alpha/ { print NR, NF, $2 } END { print "end", NR }' "$WORK_DIR/input.txt" > "$WORK_DIR/out.txt"
cat > "$WORK_DIR/expected.txt" <<'INNER'
begin
1 2 one
3 2 four
end 3
INNER
assert_files_equal "$WORK_DIR/expected.txt" "$WORK_DIR/out.txt" "awk BEGIN/END, NR, or NF handling regressed"

printf 'eins\302\240zwei\n' > "$WORK_DIR/unicode_space.txt"
"$ROOT_DIR/build/awk" '{ print NF }' "$WORK_DIR/unicode_space.txt" > "$WORK_DIR/unicode_nf.out"
assert_file_contains "$WORK_DIR/unicode_nf.out" '^2$' "awk did not split fields on Unicode whitespace"
