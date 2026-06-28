#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

phase1_math_setup solve

solve_sqrt=$("${TEST_BIN_DIR}/solve" --quiet --lo 1 --hi 2 'x^2 - 2 = 0' | tr -d '\r\n')
assert_text_equals "$solve_sqrt" '1.4142135624' "solve bisection sqrt(2) root mismatch"

"${TEST_BIN_DIR}/solve" --scan -10:10:200 --all 'x^2 - 5*x + 6 = 0' > "$WORK_DIR/quadratic.out"
assert_file_contains "$WORK_DIR/quadratic.out" '^x = 2$' "solve did not find the first quadratic root"
assert_file_contains "$WORK_DIR/quadratic.out" '^x = 3$' "solve did not find the second quadratic root"

"${TEST_BIN_DIR}/solve" --explain --all 'x^2 - 5*x + 6 = 0' > "$WORK_DIR/factor.out"
assert_file_contains "$WORK_DIR/factor.out" '^quadratic polynomial detected$' "solve did not detect a quadratic polynomial"
assert_file_contains "$WORK_DIR/factor.out" '^factor: (x - 2).* = 0$' "solve did not show the factored quadratic"
assert_file_contains "$WORK_DIR/factor.out" '^method = factoring$' "solve did not report the factoring method"

"${TEST_BIN_DIR}/solve" --explain --all 'x^3 - 6*x^2 + 11*x - 6 = 0' > "$WORK_DIR/cubic-factor.out"
assert_file_contains "$WORK_DIR/cubic-factor.out" '^polynomial factoring detected$' "solve did not detect a higher-degree polynomial factorization"
assert_file_contains "$WORK_DIR/cubic-factor.out" '^degree: 3$' "solve did not report the cubic degree"
assert_file_contains "$WORK_DIR/cubic-factor.out" '^x = 1$' "solve did not report the first cubic root"
assert_file_contains "$WORK_DIR/cubic-factor.out" '^x = 2$' "solve did not report the second cubic root"
assert_file_contains "$WORK_DIR/cubic-factor.out" '^x = 3$' "solve did not report the third cubic root"

"${TEST_BIN_DIR}/solve" --explain --all '(x - 1)^3 = 0' > "$WORK_DIR/repeated-cubic.out"
assert_file_contains "$WORK_DIR/repeated-cubic.out" '^polynomial factoring detected$' "solve did not factor a repeated cubic root"
assert_file_contains "$WORK_DIR/repeated-cubic.out" '^x = 1$' "solve did not report the repeated cubic root"

"${TEST_BIN_DIR}/solve" --explain --all '(x - 1)*(x - 2)*(x - 3)*(x - 4) = 0' > "$WORK_DIR/quartic-factor.out"
assert_file_contains "$WORK_DIR/quartic-factor.out" '^degree: 4$' "solve did not report the quartic degree"
assert_file_contains "$WORK_DIR/quartic-factor.out" '^x = 4$' "solve did not report the fourth quartic root"

"${TEST_BIN_DIR}/solve" --explain --all '(x - 1)*(x - 2)*(x - 3)*(x - 4)*(x - 5) = 0' > "$WORK_DIR/quintic-factor.out"
assert_file_contains "$WORK_DIR/quintic-factor.out" '^degree: 5$' "solve did not report the quintic degree"
assert_file_contains "$WORK_DIR/quintic-factor.out" '^x = 1$' "solve did not report the first quintic root"
assert_file_contains "$WORK_DIR/quintic-factor.out" '^x = 5$' "solve did not report the fifth quintic root"

"${TEST_BIN_DIR}/solve" --quiet --all '(x - 1)*(x - 2)*(x - 3)*(x - 4)*(x - 5)*(x - 6)*(x - 7)*(x - 8)*(x - 9) = 0' > "$WORK_DIR/degree9-factor.out"
assert_file_contains "$WORK_DIR/degree9-factor.out" '^1$' "solve did not report the first degree-9 root"
assert_file_contains "$WORK_DIR/degree9-factor.out" '^9$' "solve did not report the ninth degree-9 root"

"${TEST_BIN_DIR}/solve" --explain --all '(x - 2)^5 = 0' > "$WORK_DIR/repeated-quintic.out"
assert_file_contains "$WORK_DIR/repeated-quintic.out" '^degree: 5$' "solve did not factor a repeated quintic root"
assert_file_contains "$WORK_DIR/repeated-quintic.out" '^x = 2$' "solve did not report the repeated quintic root"

"${TEST_BIN_DIR}/solve" --explain --all 'x^2 - 2 = 0' > "$WORK_DIR/quadratic-formula.out"
assert_file_contains "$WORK_DIR/quadratic-formula.out" '^quadratic formula: x = ' "solve did not show the quadratic formula"
assert_file_contains "$WORK_DIR/quadratic-formula.out" '^method = quadratic-formula$' "solve did not report the quadratic formula method"
assert_file_contains "$WORK_DIR/quadratic-formula.out" '^status = approximate$' "solve did not mark irrational quadratic roots as approximate"

"${TEST_BIN_DIR}/solve" --explain 'x^2 - x - 1 = 0' > "$WORK_DIR/golden-quadratic.out"
assert_file_contains "$WORK_DIR/golden-quadratic.out" '^x = -0\.6180339887$' "solve did not report the negative golden-ratio quadratic root"
assert_file_contains "$WORK_DIR/golden-quadratic.out" '^x = 1\.6180339887$' "solve did not report the positive golden-ratio quadratic root without --all"
assert_file_contains "$WORK_DIR/golden-quadratic.out" '^status = approximate$' "solve did not mark golden-ratio roots as approximate"
if grep -q '^x = .*(.*)' "$WORK_DIR/golden-quadratic.out"; then
	fail "solve printed a false rational hint for irrational quadratic roots"
fi

"${TEST_BIN_DIR}/solve" --explain '(x + 1)^2 = x^2 + 2*x + 1' > "$WORK_DIR/identity.out"
assert_file_contains "$WORK_DIR/identity.out" '^polynomial identity detected$' "solve did not detect a polynomial identity"
assert_file_contains "$WORK_DIR/identity.out" '^identity = true$' "solve did not report the identity result"

"${TEST_BIN_DIR}/solve" --explain '(x^2 - 1)^4 = (x - 1)^4*(x + 1)^4' > "$WORK_DIR/degree8-identity.out"
assert_file_contains "$WORK_DIR/degree8-identity.out" '^polynomial identity detected$' "solve did not detect a degree-8 polynomial identity"

"${TEST_BIN_DIR}/solve" --quiet '(x^2 - 1)^8 = (x - 1)^8*(x + 1)^8' > "$WORK_DIR/degree16-identity.out"
assert_file_contains "$WORK_DIR/degree16-identity.out" '^all real values$' "solve did not detect a degree-16 polynomial identity"

"${TEST_BIN_DIR}/solve" --explain '(x + 0.1)^2 = x^2 + 0.2*x + 0.01' > "$WORK_DIR/decimal-identity.out"
assert_file_contains "$WORK_DIR/decimal-identity.out" '^polynomial identity detected$' "solve did not prove a decimal-rational identity exactly"
assert_file_contains "$WORK_DIR/decimal-identity.out" '^identity = true$' "solve did not report exact decimal-rational identity status"

decimal_nonidentity_status=0
"${TEST_BIN_DIR}/solve" --explain '(x + 0.1)^2 = x^2 + 0.2*x + 0.0100000001' > "$WORK_DIR/decimal-nonidentity.out" 2> "$WORK_DIR/decimal-nonidentity.err" || decimal_nonidentity_status=$?
assert_text_equals "$decimal_nonidentity_status" '1' "solve should not report a mismatched decimal-rational identity"
assert_file_contains "$WORK_DIR/decimal-nonidentity.out" 'no solution found' "solve decimal-rational non-identity output mismatch"
if grep -q 'identity =' "$WORK_DIR/decimal-nonidentity.out"; then
	fail "solve reported an identity for a mismatched decimal-rational equation"
fi

"${TEST_BIN_DIR}/solve" --json '(x + 1)^2 = x^2 + 2*x + 1' > "$WORK_DIR/identity.jsonl"
assert_file_contains "$WORK_DIR/identity.jsonl" '"event":"solve_identity"' "solve --json did not emit a solve_identity event"
assert_file_contains "$WORK_DIR/identity.jsonl" '"exact":true' "solve --json did not mark integer polynomial identity as exact"

"${TEST_BIN_DIR}/solve" --json '(x + 0.1)^2 = x^2 + 0.2*x + 0.01' > "$WORK_DIR/decimal-identity.jsonl"
assert_file_contains "$WORK_DIR/decimal-identity.jsonl" '"event":"solve_identity"' "solve --json did not emit decimal solve_identity event"
assert_file_contains "$WORK_DIR/decimal-identity.jsonl" '"exact":true' "solve --json did not mark decimal-rational identity as exact"

"${TEST_BIN_DIR}/solve" --var t --all '(t - 3)*(t + 2) = 0' > "$WORK_DIR/custom-var.out"
assert_file_contains "$WORK_DIR/custom-var.out" '^t = -2$' "solve did not report the first custom-variable root"
assert_file_contains "$WORK_DIR/custom-var.out" '^t = 3$' "solve did not report the second custom-variable root"

"${TEST_BIN_DIR}/solve" --scan 0:6:120 'x^2 - 6*x + 9 = 0' > "$WORK_DIR/touching.out"
assert_file_contains "$WORK_DIR/touching.out" '^x = 3$' "solve did not report a repeated/touching root"
assert_file_contains "$WORK_DIR/touching.out" '^residual = 0\.0000000000$' "solve repeated root residual mismatch"

"${TEST_BIN_DIR}/solve" --report-y --scan -10:10 --all 'x^2 = 2*x + 3' > "$WORK_DIR/intersections.out"
assert_file_contains "$WORK_DIR/intersections.out" '^x = -1$' "solve did not find the first intersection x-value"
assert_file_contains "$WORK_DIR/intersections.out" '^y = 1$' "solve did not report the first intersection y-value"
assert_file_contains "$WORK_DIR/intersections.out" '^x = 3$' "solve did not find the second intersection x-value"
assert_file_contains "$WORK_DIR/intersections.out" '^y = 9$' "solve did not report the second intersection y-value"

solve_trig=$("${TEST_BIN_DIR}/solve" --quiet --lo 0 --hi 1 'cos(x) = x' | tr -d '\r\n')
assert_text_equals "$solve_trig" '0.7390851332' "solve trigonometric equation root mismatch"

solve_sine=$("${TEST_BIN_DIR}/solve" --quiet --lo 3 --hi 4 'sin(x) = 0' | tr -d '\r\n')
assert_text_equals "$solve_sine" '3.1415926536' "solve sine root near pi mismatch"

solve_default_sine=$("${TEST_BIN_DIR}/solve" --quiet 'sin(x) = 0' | tr -d '\r\n')
assert_text_equals "$solve_default_sine" '0' "solve default scan should prefer the root closest to zero"

solve_scaled=$("${TEST_BIN_DIR}/solve" --quiet --scale 4 --lo 1 --hi 2 'x^2 - 2 = 0' | tr -d '\r\n')
assert_text_equals "$solve_scaled" '1.4142' "solve --scale did not control quiet precision"

scale_status=0
"${TEST_BIN_DIR}/solve" --scale 16 'x = 1' > "$WORK_DIR/scale16.out" 2> "$WORK_DIR/scale16.err" || scale_status=$?
assert_text_equals "$scale_status" '2' "solve should reject display scale beyond double precision"

"${TEST_BIN_DIR}/solve" --quiet --lo 0 --hi 1 'x^2 - 0.5624999 = 0' > "$WORK_DIR/near-rational.out"
if grep -q '(' "$WORK_DIR/near-rational.out"; then
	fail "solve printed a rational hint for a nearby but non-rational root"
fi

"${TEST_BIN_DIR}/solve" --explain --method bisection --lo 1 --hi 2 'x^2 - 2 = 0' > "$WORK_DIR/explain.out"
assert_file_contains "$WORK_DIR/explain.out" '^function: f(x) = ' "solve --explain did not print the transformed function"
assert_file_contains "$WORK_DIR/explain.out" '^step 1: ' "solve --explain did not print iteration steps"
assert_file_contains "$WORK_DIR/explain.out" '^x = 1\.4142135624$' "solve --explain did not print the final result"

"${TEST_BIN_DIR}/solve" --explain 'x - 5 / 3 = 0' > "$WORK_DIR/linear-explain.out"
assert_file_contains "$WORK_DIR/linear-explain.out" '^linear equation detected$' "solve --explain did not identify a linear equation"
assert_file_contains "$WORK_DIR/linear-explain.out" '^move constant term: x = 1\.6666666667 (1 2/3)$' "solve --explain did not show the algebraic rearrangement"
assert_file_contains "$WORK_DIR/linear-explain.out" '^method = linear$' "solve --explain did not report the linear method"

"${TEST_BIN_DIR}/solve" 'x - 5 / 3 = 0' > "$WORK_DIR/fraction.out"
assert_file_contains "$WORK_DIR/fraction.out" '^x = 5/3$' "solve did not print an exact rational root as a fraction"
"${TEST_BIN_DIR}/solve" 'x - 5 = 0' > "$WORK_DIR/integer.out"
assert_file_contains "$WORK_DIR/integer.out" '^x = 5$' "solve did not print an integer root compactly"

"${TEST_BIN_DIR}/solve" --quiet '(10000000000*x + 1)^8 = (10000000000*x + 1)^8' > "$WORK_DIR/rational-overflow-fallback.out"
assert_file_contains "$WORK_DIR/rational-overflow-fallback.out" '^all real values' "solve did not fall back after exact rational coefficient overflow"

"${TEST_BIN_DIR}/solve" --json --lo 1 --hi 2 'x^2 - 2 = 0' > "$WORK_DIR/solve.jsonl"
assert_file_contains "$WORK_DIR/solve.jsonl" '"event":"solve_result"' "solve --json did not emit a solve_result event"
assert_file_contains "$WORK_DIR/solve.jsonl" '"root":"1\.4142135624"' "solve --json root value mismatch"
assert_file_contains "$WORK_DIR/solve.jsonl" '"event":"solve_summary"' "solve --json did not emit a solve_summary event"

no_root_status=0
"${TEST_BIN_DIR}/solve" --lo 1 --hi 2 'x^2 + 1 = 0' > "$WORK_DIR/no_root.out" 2> "$WORK_DIR/no_root.err" || no_root_status=$?
assert_text_equals "$no_root_status" '1' "solve should return 1 when no solution is found"
assert_file_contains "$WORK_DIR/no_root.out" 'no solution found' "solve no-root output mismatch"

discontinuity_status=0
"${TEST_BIN_DIR}/solve" --explain --lo 1 --hi 3 '1/(x - 2) = 0' > "$WORK_DIR/discontinuity.out" 2> "$WORK_DIR/discontinuity.err" || discontinuity_status=$?
assert_text_equals "$discontinuity_status" '3' "solve should return 3 for a suspected discontinuity"
assert_file_contains "$WORK_DIR/discontinuity.out" 'suspected discontinuity' "solve --explain did not report the suspected discontinuity"

unknown_status=0
"${TEST_BIN_DIR}/solve" 'x + y = 3' > "$WORK_DIR/unknown.out" 2> "$WORK_DIR/unknown.err" || unknown_status=$?
assert_text_equals "$unknown_status" '2' "solve should return 2 for an unknown identifier"
assert_file_contains "$WORK_DIR/unknown.err" 'unknown identifier' "solve unknown identifier diagnostic mismatch"

conflict_status=0
"${TEST_BIN_DIR}/solve" --scan 0:1 --lo 0 --hi 1 'x = 0' > "$WORK_DIR/conflict.out" 2> "$WORK_DIR/conflict.err" || conflict_status=$?
assert_text_equals "$conflict_status" '2' "solve should reject --scan combined with --lo/--hi"
assert_file_contains "$WORK_DIR/conflict.err" 'cannot be combined' "solve interval conflict diagnostic mismatch"
