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

seq_scientific_out=$("$ROOT_DIR/build/seq" 1e3 2e3 5e3 | tr -d '\r')
printf '%s\n' "$seq_scientific_out" > "$WORK_DIR/seq_scientific.out"
cat > "$WORK_DIR/seq_scientific.expected" <<'EOF'
1000
3000
5000
EOF
assert_files_equal "$WORK_DIR/seq_scientific.expected" "$WORK_DIR/seq_scientific.out" "seq did not parse scientific notation"

seq_big_out=$("$ROOT_DIR/build/seq" 999999999999999999999999999999 1 1000000000000000000000000000001 | tr -d '\r')
printf '%s\n' "$seq_big_out" > "$WORK_DIR/seq_big.out"
cat > "$WORK_DIR/seq_big.expected" <<'EOF'
999999999999999999999999999999
1000000000000000000000000000000
1000000000000000000000000000001
EOF
assert_files_equal "$WORK_DIR/seq_big.expected" "$WORK_DIR/seq_big.out" "seq did not handle large decimal arithmetic"

seq_exp_format_out=$("$ROOT_DIR/build/seq" -f '%.2e' 1000 1000 3000 | tr -d '\r')
printf '%s\n' "$seq_exp_format_out" > "$WORK_DIR/seq_exp_format.out"
cat > "$WORK_DIR/seq_exp_format.expected" <<'EOF'
1.00e+03
2.00e+03
3.00e+03
EOF
assert_files_equal "$WORK_DIR/seq_exp_format.expected" "$WORK_DIR/seq_exp_format.out" "seq did not support exponential format output"

seq_grouped_out=$("$ROOT_DIR/build/seq" -f "%'f" 1000 1000 3000 | tr -d '\r')
printf '%s\n' "$seq_grouped_out" > "$WORK_DIR/seq_grouped.out"
cat > "$WORK_DIR/seq_grouped.expected" <<'EOF'
1,000
2,000
3,000
EOF
assert_files_equal "$WORK_DIR/seq_grouped.expected" "$WORK_DIR/seq_grouped.out" "seq did not support grouped fixed formatting"

seq_signed_padded_out=$("$ROOT_DIR/build/seq" -s ',' -f '%+08.2f' 1 1 3 | tr -d '\r\n')
assert_text_equals "$seq_signed_padded_out" '+0001.00,+0002.00,+0003.00' "seq did not support signed zero-padded formatting"

"$ROOT_DIR/build/yes" ok | "$ROOT_DIR/build/head" -n 3 > "$WORK_DIR/yes.out"
printf 'ok\nok\nok\n' > "$WORK_DIR/yes.expected"
assert_files_equal "$WORK_DIR/yes.expected" "$WORK_DIR/yes.out" "yes did not repeat the requested token"
