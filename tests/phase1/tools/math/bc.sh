#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

phase1_math_setup bc

bc_large_add=$("${TEST_BIN_DIR}/bc" '12345678901234567890 + 98765432109876543210' | tr -d '\r\n')
assert_text_equals "$bc_large_add" '111111111011111111100' "bc large addition failed"

"${TEST_BIN_DIR}/bc" --json '1+2; scale=4; 1/2' > "$WORK_DIR/bc_json.out"
assert_file_contains "$WORK_DIR/bc_json.out" '"schema":"newos.tool.v1"' "bc --json did not use the shared JSON envelope"
assert_file_contains "$WORK_DIR/bc_json.out" '"event":"bc_result"' "bc --json did not emit bc_result events"
assert_file_contains "$WORK_DIR/bc_json.out" '"text":"3"' "bc --json did not report the first result"
assert_file_contains "$WORK_DIR/bc_json.out" '"text":"0.5000"' "bc --json did not preserve formatted scale output"
assert_file_contains "$WORK_DIR/bc_json.out" '"obase":10' "bc --json did not report the output base"

bc_pow_64=$("${TEST_BIN_DIR}/bc" '2^64' | tr -d '\r\n')
assert_text_equals "$bc_pow_64" '18446744073709551616' "bc 2^64 failed"

bc_pow_100=$("${TEST_BIN_DIR}/bc" '2^100' | tr -d '\r\n')
assert_text_equals "$bc_pow_100" '1267650600228229401496703205376' "bc 2^100 failed"

bc_pow_tower_size=$("${TEST_BIN_DIR}/bc" '2^2^2^2^2' | wc -c | tr -d '[:space:]')
assert_text_equals "$bc_pow_tower_size" '19730' "bc larger power tower exceeded the bignum range"

bc_large_mul=$("${TEST_BIN_DIR}/bc" '999999999999 * 888888888888' | tr -d '\r\n')
assert_text_equals "$bc_large_mul" '888888888887111111111112' "bc large multiplication failed"

bc_abs=$("${TEST_BIN_DIR}/bc" 'abs(-42.5)' | tr -d '\r\n')
assert_text_equals "$bc_abs" '42.5' "bc abs() failed"

bc_integer_helpers=$("${TEST_BIN_DIR}/bc" 'gcd(48, 18); gcd(-48, 18); lcm(21, 6); lcm(0, 99); fact(10); factorial(0)' | tr -d '\r')
cat > "$WORK_DIR/bc_integer_helpers.expected" <<'EOF'
6
6
42
0
3628800
1
EOF
printf '%s\n' "$bc_integer_helpers" > "$WORK_DIR/bc_integer_helpers.out"
assert_files_equal "$WORK_DIR/bc_integer_helpers.expected" "$WORK_DIR/bc_integer_helpers.out" "bc integer helper functions failed"

bc_large_length=$("${TEST_BIN_DIR}/bc" 'length(12345678901234567890123456789012345678901234567890123456789012345678901234567890)' | tr -d '\r\n')
assert_text_equals "$bc_large_length" '80' "bc large length() failed"

bc_high_scale=$("${TEST_BIN_DIR}/bc" 'scale=30; 1/7' | tr -d '\r\n')
assert_text_equals "$bc_high_scale" '0.142857142857142857142857142857' "bc high-scale division failed"

bc_extended_scale_size=$("${TEST_BIN_DIR}/bc" 'scale=128; 1/3' | wc -c | tr -d '[:space:]')
assert_text_equals "$bc_extended_scale_size" '131' "bc extended scale division failed"

bc_decimal_mod=$("${TEST_BIN_DIR}/bc" '5.5 % 2' | tr -d '\r\n')
assert_text_equals "$bc_decimal_mod" '1.5' "bc decimal remainder failed"

bc_loop_control=$("${TEST_BIN_DIR}/bc" 'sum=0; for(i=0;i<10;i=i+1){ if(i==3) continue; if(i==6) break; sum=sum+i }; sum' | tr -d '\r\n')
assert_text_equals "$bc_loop_control" '12' "bc break/continue flow control failed"

bc_math_mode=$("${TEST_BIN_DIR}/bc" -l 'pi > 3 && e > 2; scale' | tr -d '\r')
cat > "$WORK_DIR/bc_math_mode.expected" <<'EOF'
1
32
EOF
printf '%s\n' "$bc_math_mode" > "$WORK_DIR/bc_math_mode.out"
assert_files_equal "$WORK_DIR/bc_math_mode.expected" "$WORK_DIR/bc_math_mode.out" "bc -l constants or default scale failed"

bc_math_library=$("${TEST_BIN_DIR}/bc" -l 'scale=10; e(1); l(1); l(10); a(1); s(pi/2); c(pi); j(0,0); j(1,0); exp(0); sin(pi)' | tr -d '\r')
cat > "$WORK_DIR/bc_math_library.expected" <<'EOF'
2.7182818284
0.0000000000
2.3025850929
0.7853981633
1.0000000000
-1.0000000000
1.0000000000
0.0000000000
1.0000000000
0.0000000000
EOF
printf '%s\n' "$bc_math_library" > "$WORK_DIR/bc_math_library.out"
assert_files_equal "$WORK_DIR/bc_math_library.expected" "$WORK_DIR/bc_math_library.out" "bc math library functions failed"

bc_minmax=$("${TEST_BIN_DIR}/bc" 'min(-3, 8); max(1.25, 1.5)' | tr -d '\r')
cat > "$WORK_DIR/bc_minmax.expected" <<'EOF'
-3
1.5
EOF
printf '%s\n' "$bc_minmax" > "$WORK_DIR/bc_minmax.out"
assert_files_equal "$WORK_DIR/bc_minmax.expected" "$WORK_DIR/bc_minmax.out" "bc min()/max() failed"

bc_comments=$("${TEST_BIN_DIR}/bc" '1 + 2 # comment
/* skip */
3 + 4' | tr -d '\r')
cat > "$WORK_DIR/bc_comments.expected" <<'EOF'
3
7
EOF
printf '%s\n' "$bc_comments" > "$WORK_DIR/bc_comments.out"
assert_files_equal "$WORK_DIR/bc_comments.expected" "$WORK_DIR/bc_comments.out" "bc comment handling failed"

if "${TEST_BIN_DIR}/bc" 'while(1){}' > "$WORK_DIR/loop.out" 2> "$WORK_DIR/loop.err"; then
    fail "bc did not reject a runaway loop"
fi
assert_file_contains "$WORK_DIR/loop.err" 'loop iteration limit exceeded' "bc did not report the loop guard"
