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

cat > "$WORK_DIR/script.awk" <<'INNER'
FNR == 1 { print FILENAME }
{ print prefix, FNR, $1 }
END { print "records", NR }
INNER
printf 'left:1\nleft:2\n' > "$WORK_DIR/left.txt"
printf 'right:3\n' > "$WORK_DIR/right.txt"
"$ROOT_DIR/build/awk" -F: -v prefix=tag -v OFS='|' -f "$WORK_DIR/script.awk" \
    "$WORK_DIR/left.txt" "$WORK_DIR/right.txt" > "$WORK_DIR/script.out"
cat > "$WORK_DIR/script.expected" <<EOF
$WORK_DIR/left.txt
tag|1|left
tag|2|left
$WORK_DIR/right.txt
tag|1|right
records|3
EOF
assert_files_equal "$WORK_DIR/script.expected" "$WORK_DIR/script.out" "awk -F/-v/-f or FNR/FILENAME handling regressed"

printf 'alpha--beta--gamma' > "$WORK_DIR/records.txt"
"$ROOT_DIR/build/awk" -v RS='--' -v ORS='|' '{ print $0 }' "$WORK_DIR/records.txt" > "$WORK_DIR/records.out"
records_out=$(cat "$WORK_DIR/records.out")
assert_text_equals "$records_out" 'alpha|beta|gamma|' "awk RS/ORS handling failed"
