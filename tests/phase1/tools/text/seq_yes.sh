#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

WORK_DIR=$(phase1_text_workdir seq_yes)

note "phase1 text: seq/yes"

"$ROOT_DIR/build/seq" 2 2 6 > "$WORK_DIR/seq.out"
printf '2\n4\n6\n' > "$WORK_DIR/seq.expected"
assert_files_equal "$WORK_DIR/seq.expected" "$WORK_DIR/seq.out" "seq did not generate the expected arithmetic progression"

seq_decimal_out=$("$ROOT_DIR/build/seq" -w 0.5 0.25 1.0 | tr -d '\r')
printf '%s\n' "$seq_decimal_out" > "$WORK_DIR/seq_decimal.out"
cat > "$WORK_DIR/seq_decimal.expected" <<'EOF'
0.50
0.75
1.00
EOF
assert_files_equal "$WORK_DIR/seq_decimal.expected" "$WORK_DIR/seq_decimal.out" "seq decimal stepping or -w formatting failed"

seq_format_out=$("$ROOT_DIR/build/seq" -s ',' -f 'item%.1f' 1 0.5 2 | tr -d '\r\n')
assert_text_equals "$seq_format_out" 'item1.0,item1.5,item2.0' "seq -f or separator handling failed"

"$ROOT_DIR/build/yes" ok | "$ROOT_DIR/build/head" -n 3 > "$WORK_DIR/yes.out"
printf 'ok\nok\nok\n' > "$WORK_DIR/yes.expected"
assert_files_equal "$WORK_DIR/yes.expected" "$WORK_DIR/yes.out" "yes did not repeat the requested token"
