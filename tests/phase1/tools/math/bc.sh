#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

phase1_math_setup bc

bc_large_add=$("$ROOT_DIR/build/bc" '12345678901234567890 + 98765432109876543210' | tr -d '\r\n')
assert_text_equals "$bc_large_add" '111111111011111111100' "bc large addition failed"

bc_pow_64=$("$ROOT_DIR/build/bc" '2^64' | tr -d '\r\n')
assert_text_equals "$bc_pow_64" '18446744073709551616' "bc 2^64 failed"

bc_pow_100=$("$ROOT_DIR/build/bc" '2^100' | tr -d '\r\n')
assert_text_equals "$bc_pow_100" '1267650600228229401496703205376' "bc 2^100 failed"

bc_large_mul=$("$ROOT_DIR/build/bc" '999999999999 * 888888888888' | tr -d '\r\n')
assert_text_equals "$bc_large_mul" '888888888887111111111112' "bc large multiplication failed"

bc_abs=$("$ROOT_DIR/build/bc" 'abs(-42.5)' | tr -d '\r\n')
assert_text_equals "$bc_abs" '42.5' "bc abs() failed"

bc_high_scale=$("$ROOT_DIR/build/bc" 'scale=30; 1/7' | tr -d '\r\n')
assert_text_equals "$bc_high_scale" '0.142857142857142857142857142857' "bc high-scale division failed"

bc_decimal_mod=$("$ROOT_DIR/build/bc" '5.5 % 2' | tr -d '\r\n')
assert_text_equals "$bc_decimal_mod" '1.5' "bc decimal remainder failed"

bc_loop_control=$("$ROOT_DIR/build/bc" 'sum=0; for(i=0;i<10;i=i+1){ if(i==3) continue; if(i==6) break; sum=sum+i }; sum' | tr -d '\r\n')
assert_text_equals "$bc_loop_control" '12' "bc break/continue flow control failed"

bc_math_mode=$("$ROOT_DIR/build/bc" -l 'pi > 3 && e > 2; scale' | tr -d '\r')
cat > "$WORK_DIR/bc_math_mode.expected" <<'EOF'
1
32
EOF
printf '%s\n' "$bc_math_mode" > "$WORK_DIR/bc_math_mode.out"
assert_files_equal "$WORK_DIR/bc_math_mode.expected" "$WORK_DIR/bc_math_mode.out" "bc -l constants or default scale failed"

bc_minmax=$("$ROOT_DIR/build/bc" 'min(-3, 8); max(1.25, 1.5)' | tr -d '\r')
cat > "$WORK_DIR/bc_minmax.expected" <<'EOF'
-3
1.5
EOF
printf '%s\n' "$bc_minmax" > "$WORK_DIR/bc_minmax.out"
assert_files_equal "$WORK_DIR/bc_minmax.expected" "$WORK_DIR/bc_minmax.out" "bc min()/max() failed"

bc_comments=$("$ROOT_DIR/build/bc" '1 + 2 # comment
/* skip */
3 + 4' | tr -d '\r')
cat > "$WORK_DIR/bc_comments.expected" <<'EOF'
3
7
EOF
printf '%s\n' "$bc_comments" > "$WORK_DIR/bc_comments.out"
assert_files_equal "$WORK_DIR/bc_comments.expected" "$WORK_DIR/bc_comments.out" "bc comment handling failed"

if "$ROOT_DIR/build/bc" 'while(1){}' > "$WORK_DIR/loop.out" 2> "$WORK_DIR/loop.err"; then
    fail "bc did not reject a runaway loop"
fi
assert_file_contains "$WORK_DIR/loop.err" 'loop iteration limit exceeded' "bc did not report the loop guard"
