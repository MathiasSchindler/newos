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

"${TEST_BIN_DIR}/solve" 'x^2 - 4 > 0' > "$WORK_DIR/inequality-open.out"
assert_file_contains "$WORK_DIR/inequality-open.out" '^solution = (-inf, -2) U (2, inf)$' "solve did not report open exact inequality intervals"
"${TEST_BIN_DIR}/solve" 'x^2 - 4 >= 0' > "$WORK_DIR/inequality-closed.out"
assert_file_contains "$WORK_DIR/inequality-closed.out" '^solution = (-inf, -2\] U \[2, inf)$' "solve did not report closed exact inequality intervals"
"${TEST_BIN_DIR}/solve" 'x^2 + 1 > 0' > "$WORK_DIR/inequality-all.out"
assert_file_contains "$WORK_DIR/inequality-all.out" '^solution = all real x$' "solve did not report an always-true inequality"
inequality_empty_status=0
"${TEST_BIN_DIR}/solve" 'x^2 + 1 < 0' > "$WORK_DIR/inequality-empty.out" 2> "$WORK_DIR/inequality-empty.err" || inequality_empty_status=$?
assert_text_equals "$inequality_empty_status" '1' "solve should return 1 for an empty inequality solution set"
assert_file_contains "$WORK_DIR/inequality-empty.out" '^solution = empty set$' "solve did not report an empty inequality solution set"
"${TEST_BIN_DIR}/solve" '(x-3)^2 > 0' > "$WORK_DIR/inequality-touch-open.out"
assert_file_contains "$WORK_DIR/inequality-touch-open.out" '^solution = (-inf, 3) U (3, inf)$' "solve did not exclude a strict touching root"
"${TEST_BIN_DIR}/solve" '(x-3)^2 >= 0' > "$WORK_DIR/inequality-touch-closed.out"
assert_file_contains "$WORK_DIR/inequality-touch-closed.out" '^solution = all real x$' "solve did not include a nonnegative touching-root polynomial everywhere"
"${TEST_BIN_DIR}/solve" 'x^2 - 4 < 0' > "$WORK_DIR/inequality-inside.out"
assert_file_contains "$WORK_DIR/inequality-inside.out" '^solution = (-2, 2)$' "solve did not report the strict interior interval of a quadratic inequality"
"${TEST_BIN_DIR}/solve" '4 - x^2 > 0' > "$WORK_DIR/inequality-reversed.out"
assert_file_contains "$WORK_DIR/inequality-reversed.out" '^solution = (-2, 2)$' "solve did not handle a reversed quadratic inequality"
"${TEST_BIN_DIR}/solve" '2*x + 1 <= 5' > "$WORK_DIR/inequality-linear-right.out"
assert_file_contains "$WORK_DIR/inequality-linear-right.out" '^solution = (-inf, 2\]$' "solve did not report a half-line for a linear inequality with a right-hand side"
"${TEST_BIN_DIR}/solve" --var t 't^2 - 1 <= 0' > "$WORK_DIR/inequality-custom-var.out"
assert_file_contains "$WORK_DIR/inequality-custom-var.out" '^solution = \[-1, 1\]$' "solve did not handle inequality intervals with a custom variable"
"${TEST_BIN_DIR}/solve" --scan 0:4:80 'sin(x) > 0' > "$WORK_DIR/inequality-numeric-sine.out"
assert_file_contains "$WORK_DIR/inequality-numeric-sine.out" '^solution (within scan range) = (0, 3\.1415926537)$' "solve did not report a bounded numeric sine inequality interval"
"${TEST_BIN_DIR}/solve" --scan 0:4:80 '1/(x-2) > 0' > "$WORK_DIR/inequality-numeric-pole.out"
assert_file_contains "$WORK_DIR/inequality-numeric-pole.out" '^solution (within scan range) = (2, 4\]$' "solve did not split a numeric inequality at a pole"

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

negative_quadratic_status=0
"${TEST_BIN_DIR}/solve" --explain 'x^2 + 1 = 0' > "$WORK_DIR/negative-quadratic.out" 2> "$WORK_DIR/negative-quadratic.err" || negative_quadratic_status=$?
assert_text_equals "$negative_quadratic_status" '1' "solve should return 1 for a quadratic with no real roots"
assert_file_contains "$WORK_DIR/negative-quadratic.out" '^quadratic polynomial detected$' "solve did not detect the negative-discriminant quadratic"
assert_file_contains "$WORK_DIR/negative-quadratic.out" '^discriminant: -4$' "solve did not report the exact negative discriminant"
assert_file_contains "$WORK_DIR/negative-quadratic.out" '^discriminant < 0, so there are no real roots$' "solve did not explain the no-real-roots quadratic case"
assert_file_contains "$WORK_DIR/negative-quadratic.out" '^no real solutions$' "solve no-real-roots output mismatch"

"${TEST_BIN_DIR}/solve" --explain --all 'x^4 - 5*x^2 + 4 = 0' > "$WORK_DIR/biquadratic-factor.out"
assert_file_contains "$WORK_DIR/biquadratic-factor.out" '^rational roots: -2, -1, 1, 2$' "solve did not explain all rational roots of a biquadratic"
assert_file_contains "$WORK_DIR/biquadratic-factor.out" '^factor: (x + 2)\*(x + 1)\*(x - 1)\*(x - 2) = 0$' "solve did not print the complete biquadratic factorization"
assert_file_contains "$WORK_DIR/biquadratic-factor.out" '^x = -2$' "solve did not print the first biquadratic root in order"
assert_file_contains "$WORK_DIR/biquadratic-factor.out" '^x = 2$' "solve did not print the last biquadratic root in order"
if grep -q '^method = factoring$\|remaining quadratic' "$WORK_DIR/biquadratic-factor.out"; then
	fail "solve used inconsistent factor labels for a fully rational biquadratic"
fi

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
"${TEST_BIN_DIR}/solve" --scan -1:1:40 'sin(x)^2 = 0' > "$WORK_DIR/adaptive-touching.out"
assert_file_contains "$WORK_DIR/adaptive-touching.out" '^x = 0$' "solve adaptive scan did not refine a non-polynomial touching root"
assert_file_contains "$WORK_DIR/adaptive-touching.out" '^method = adaptive-touching-scan$' "solve adaptive scan did not label the refined touching-root method"

"${TEST_BIN_DIR}/solve" --report-y --scan -10:10 --all 'x^2 = 2*x + 3' > "$WORK_DIR/intersections.out"
assert_file_contains "$WORK_DIR/intersections.out" '^x = -1$' "solve did not find the first intersection x-value"
assert_file_contains "$WORK_DIR/intersections.out" '^y = 1$' "solve did not report the first intersection y-value"
assert_file_contains "$WORK_DIR/intersections.out" '^x = 3$' "solve did not find the second intersection x-value"
assert_file_contains "$WORK_DIR/intersections.out" '^y = 9$' "solve did not report the second intersection y-value"

"${TEST_BIN_DIR}/solve" --all 'abs(x - 3) = 2' > "$WORK_DIR/absolute-value.out"
assert_file_contains "$WORK_DIR/absolute-value.out" '^x = 1$' "solve did not find the first absolute-value root"
assert_file_contains "$WORK_DIR/absolute-value.out" '^x = 5$' "solve did not find the second absolute-value root"

solve_trig=$("${TEST_BIN_DIR}/solve" --quiet --lo 0 --hi 1 'cos(x) = x' | tr -d '\r\n')
assert_text_equals "$solve_trig" '0.7390851332' "solve trigonometric equation root mismatch"

solve_sine=$("${TEST_BIN_DIR}/solve" --quiet --lo 3 --hi 4 'sin(x) = 0' | tr -d '\r\n')
assert_text_equals "$solve_sine" '3.1415926536' "solve sine root near pi mismatch"

solve_default_sine=$("${TEST_BIN_DIR}/solve" --quiet 'sin(x) = 0' | tr -d '\r\n')
assert_text_equals "$solve_default_sine" '0' "solve default scan should prefer the root closest to zero"

"${TEST_BIN_DIR}/solve" --explain 'x^3 - 2 = 0' > "$WORK_DIR/cubic-irrational.out"
assert_file_contains "$WORK_DIR/cubic-irrational.out" '^x = 1\.2599210499$' "solve did not report the real cube-root approximation"
assert_file_contains "$WORK_DIR/cubic-irrational.out" '^method = bisection$' "solve did not report bisection for the cube-root approximation"
assert_file_contains "$WORK_DIR/cubic-irrational.out" '^status = approximate$' "solve did not mark a bisection-derived irrational root as approximate"
if grep -q '^residual = 0\.000000000000000$' "$WORK_DIR/cubic-irrational.out"; then
	fail "solve printed a zero residual for a displayed approximate cube-root value"
fi

"${TEST_BIN_DIR}/solve" '2^x = 10' > "$WORK_DIR/exponential-approx.out"
assert_file_contains "$WORK_DIR/exponential-approx.out" '^status = approximate$' "solve did not mark a bisection-derived exponential root as approximate"

"${TEST_BIN_DIR}/solve" 'sqrt(x) = 3' > "$WORK_DIR/sqrt-sample.out"
assert_file_contains "$WORK_DIR/sqrt-sample.out" '^x = 9$' "solve did not report the exact sampled square-root solution"
assert_file_contains "$WORK_DIR/sqrt-sample.out" '^method = exact-sample$' "solve did not label an exact sampled root honestly"
if grep -q '^status = approximate$' "$WORK_DIR/sqrt-sample.out"; then
	fail "solve marked an exact sampled root as approximate"
fi

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
assert_file_contains "$WORK_DIR/explain.out" '^worked solution$' "solve --explain did not print the student worked-solution header"
assert_file_contains "$WORK_DIR/explain.out" '^Rewrite: f(x) = (x\^2 - 2) - (0), then solve f(x) = 0\.$' "solve --explain did not print the classroom rewrite"
assert_file_contains "$WORK_DIR/explain.out" '^Exactness: approximate, because a numeric method was needed\.$' "solve --explain did not distinguish approximate numeric solving"
assert_file_contains "$WORK_DIR/explain.out" '^Check: substitute x = 1\.4142135624 into f(x); f(1\.4142135624) = ' "solve --explain did not print the substitution check"
assert_file_contains "$WORK_DIR/explain.out" '^x = 1\.4142135624$' "solve --explain did not print the final result"
"${TEST_BIN_DIR}/solve" --explain=trace --method bisection --lo 1 --hi 2 'x^2 - 2 = 0' > "$WORK_DIR/explain-trace.out"
assert_file_contains "$WORK_DIR/explain-trace.out" '^function: f(x) = ' "solve --explain=trace did not print the transformed function"
assert_file_contains "$WORK_DIR/explain-trace.out" '^step 1: ' "solve --explain=trace did not print iteration steps"
if grep -q '^worked solution$' "$WORK_DIR/explain-trace.out"; then
	fail "solve --explain=trace should not print the student worked-solution header"
fi

"${TEST_BIN_DIR}/solve" --explain 'x - 5 / 3 = 0' > "$WORK_DIR/linear-explain.out"
assert_file_contains "$WORK_DIR/linear-explain.out" '^linear equation detected$' "solve --explain did not identify a linear equation"
assert_file_contains "$WORK_DIR/linear-explain.out" '^move constant term: x = 1\.6666666667 (1 2/3)$' "solve --explain did not show the algebraic rearrangement"
assert_file_contains "$WORK_DIR/linear-explain.out" '^method = linear$' "solve --explain did not report the linear method"

"${TEST_BIN_DIR}/solve" 'x - 5 / 3 = 0' > "$WORK_DIR/fraction.out"
assert_file_contains "$WORK_DIR/fraction.out" '^x = 5/3$' "solve did not print an exact rational root as a fraction"
"${TEST_BIN_DIR}/solve" 'x - 5 = 0' > "$WORK_DIR/integer.out"
assert_file_contains "$WORK_DIR/integer.out" '^x = 5$' "solve did not print an integer root compactly"

"${TEST_BIN_DIR}/solve" --diff 'x^3-6*x^2+11*x-6' > "$WORK_DIR/diff.out"
assert_file_contains "$WORK_DIR/diff.out" '^3\*x\^2 - 12\*x + 11$' "solve --diff first derivative mismatch"
"${TEST_BIN_DIR}/solve" --diff=2 'x^3-6*x^2+11*x-6' > "$WORK_DIR/diff2.out"
assert_file_contains "$WORK_DIR/diff2.out" '^6\*x - 12$' "solve --diff=2 second derivative mismatch"
"${TEST_BIN_DIR}/solve" --diff 'x^3-6*x^2+11*x-6 = 0' > "$WORK_DIR/diff-solve.out"
assert_file_contains "$WORK_DIR/diff-solve.out" '^x = 1\.4226497308$' "solve --diff did not report the first extrema point"
assert_file_contains "$WORK_DIR/diff-solve.out" '^x = 2\.5773502692$' "solve --diff did not report the second extrema point"
assert_file_contains "$WORK_DIR/diff-solve.out" '^status = approximate$' "solve --diff did not mark irrational derivative roots as approximate"
"${TEST_BIN_DIR}/solve" --diff=0 'x^2+3*x+2' > "$WORK_DIR/diff0.out"
assert_file_contains "$WORK_DIR/diff0.out" '^x\^2 + 3\*x + 2$' "solve --diff=0 did not print the original polynomial"
"${TEST_BIN_DIR}/solve" --diff=3 'x^2+3*x+2' > "$WORK_DIR/diff3.out"
assert_file_contains "$WORK_DIR/diff3.out" '^0$' "solve did not reduce an over-differentiated polynomial to zero"
"${TEST_BIN_DIR}/solve" --diff 'sin(x)' > "$WORK_DIR/diff-nonpoly.out"
assert_file_contains "$WORK_DIR/diff-nonpoly.out" '^cos[(]x[)]$' "solve --diff should symbolically differentiate sin(x)"
"${TEST_BIN_DIR}/solve" --diff 'x^2*exp(-x)' > "$WORK_DIR/diff-product-exp.out"
assert_file_contains "$WORK_DIR/diff-product-exp.out" '^exp[(]-x[)]' "solve --diff product/chain output should contain exp"
assert_file_contains "$WORK_DIR/diff-product-exp.out" '0 - x\^2' "solve --diff should avoid ambiguous leading negative powers"
assert_file_contains "$WORK_DIR/diff-product-exp.out" '2\*x' "solve --diff product output should contain the power-rule term"
"${TEST_BIN_DIR}/solve" --all --diff 'x^2*exp(-x) = 0' > "$WORK_DIR/diff-product-exp-solve.out"
assert_file_contains "$WORK_DIR/diff-product-exp-solve.out" '^x = 0$' "solve --diff exp-polynomial should report the first exact critical root"
assert_file_contains "$WORK_DIR/diff-product-exp-solve.out" '^x = 2$' "solve --diff exp-polynomial should report the second exact critical root"
"${TEST_BIN_DIR}/solve" --param k --var x --diff 'exp(-k)*((x-k)^3 - 3*(x-k) + k^2)' > "$WORK_DIR/diff-param.out"
assert_file_contains "$WORK_DIR/diff-param.out" 'exp' "solve --diff should retain parameter factors"
assert_file_contains "$WORK_DIR/diff-param.out" 'x - k' "solve --diff should differentiate with respect to x while treating k as a parameter"
assert_file_contains "$WORK_DIR/diff-param.out" '3\*' "solve --diff should simplify constant factors in parameterized derivatives"
assert_file_contains "$WORK_DIR/diff-param.out" '^exp[(]-k[)]\*[(][(]3\*[(]x - k[)]\^2[)] - 3[)]$' "solve --diff should keep parameterized derivatives readable"
"${TEST_BIN_DIR}/solve" --param k=2 --diff 'k*x' > "$WORK_DIR/diff-param-value.out"
assert_file_contains "$WORK_DIR/diff-param-value.out" '^2$' "solve --diff should apply numeric parameter bindings before differentiating"
diff_inequality_status=0
"${TEST_BIN_DIR}/solve" --diff 'x^2 > 1' > "$WORK_DIR/diff-inequality.out" 2> "$WORK_DIR/diff-inequality.err" || diff_inequality_status=$?
assert_text_equals "$diff_inequality_status" '2' "solve --diff should reject inequality input"
assert_file_contains "$WORK_DIR/diff-inequality.err" 'derivative solving supports equations, not inequalities' "solve --diff inequality diagnostic mismatch"
diff_invalid_status=0
"${TEST_BIN_DIR}/solve" --diff=65 'x^2' > "$WORK_DIR/diff-invalid.out" 2> "$WORK_DIR/diff-invalid.err" || diff_invalid_status=$?
assert_text_equals "$diff_invalid_status" '2' "solve --diff should reject derivative orders above the limit"
assert_file_contains "$WORK_DIR/diff-invalid.err" 'invalid --diff order' "solve --diff invalid-order diagnostic mismatch"

"${TEST_BIN_DIR}/solve" --integrate 0:1 'x^2' > "$WORK_DIR/integral-square.out"
assert_file_contains "$WORK_DIR/integral-square.out" '^integral = 1/3$' "solve did not integrate x^2 exactly"
assert_file_contains "$WORK_DIR/integral-square.out" '^method = exact-polynomial$' "solve did not label exact polynomial integration"
"${TEST_BIN_DIR}/solve" --integrate 0:1 'x^3' > "$WORK_DIR/integral-cube.out"
assert_file_contains "$WORK_DIR/integral-cube.out" '^integral = 1/4$' "solve did not integrate x^3 exactly"
"${TEST_BIN_DIR}/solve" --integrate 1:0 'x^2' > "$WORK_DIR/integral-reversed.out"
assert_file_contains "$WORK_DIR/integral-reversed.out" '^integral = -1/3$' "solve did not preserve orientation for reversed integral bounds"
"${TEST_BIN_DIR}/solve" --integrate 0.5:1.5 'x' > "$WORK_DIR/integral-decimal-bounds.out"
assert_file_contains "$WORK_DIR/integral-decimal-bounds.out" '^integral = 1$' "solve did not integrate exactly over decimal rational bounds"
"${TEST_BIN_DIR}/solve" --integrate 0:1 'x^2 = x' > "$WORK_DIR/integral-equation.out"
assert_file_contains "$WORK_DIR/integral-equation.out" '^integral = -1/6$' "solve did not integrate the transformed zero-function for an equation"
integral_quiet=$("${TEST_BIN_DIR}/solve" --quiet --integrate 0:1 'x^2' | tr -d '\r\n')
assert_text_equals "$integral_quiet" '1/3' "solve --quiet --integrate did not print only the integral value"
"${TEST_BIN_DIR}/solve" --integrate 0:pi 'sin(x)' > "$WORK_DIR/integral-sine.out"
assert_file_contains "$WORK_DIR/integral-sine.out" '^integral = 2\.0000000000$' "solve did not numerically integrate sin from 0 to pi"
assert_file_contains "$WORK_DIR/integral-sine.out" '^status = approximate$' "solve did not mark Simpson integration as approximate"
improper_integral_status=0
"${TEST_BIN_DIR}/solve" --integrate 1:3 '1/(x-2)' > "$WORK_DIR/integral-improper.out" 2> "$WORK_DIR/integral-improper.err" || improper_integral_status=$?
assert_text_equals "$improper_integral_status" '3' "solve should return 3 for an improper integral over a discontinuity"
assert_file_contains "$WORK_DIR/integral-improper.out" 'improper integral' "solve did not warn about an improper integral"
assert_file_contains "$WORK_DIR/integral-improper.out" '^classification = divergent$' "solve did not classify the rational-pole improper integral as divergent"
assert_file_contains "$WORK_DIR/integral-improper.out" '^method = rational-pole-detection$' "solve did not label rational-pole improper integral detection"
integral_invalid_status=0
"${TEST_BIN_DIR}/solve" --integrate 0,1 'x^2' > "$WORK_DIR/integral-invalid.out" 2> "$WORK_DIR/integral-invalid.err" || integral_invalid_status=$?
assert_text_equals "$integral_invalid_status" '2' "solve --integrate should reject malformed bounds"
assert_file_contains "$WORK_DIR/integral-invalid.err" 'invalid --integrate bounds' "solve --integrate malformed-bounds diagnostic mismatch"
integral_bad_bound_status=0
"${TEST_BIN_DIR}/solve" --integrate 0:y 'x^2' > "$WORK_DIR/integral-bad-bound.out" 2> "$WORK_DIR/integral-bad-bound.err" || integral_bad_bound_status=$?
assert_text_equals "$integral_bad_bound_status" '2' "solve --integrate should reject bounds with unknown identifiers"
assert_file_contains "$WORK_DIR/integral-bad-bound.err" 'invalid integration bound' "solve --integrate bad-bound diagnostic mismatch"
diff_integrate_status=0
"${TEST_BIN_DIR}/solve" --diff --integrate 0:1 'x^2' > "$WORK_DIR/diff-integrate.out" 2> "$WORK_DIR/diff-integrate.err" || diff_integrate_status=$?
assert_text_equals "$diff_integrate_status" '2' "solve should reject combining --diff and --integrate"
assert_file_contains "$WORK_DIR/diff-integrate.err" 'diff and --integrate cannot be combined' "solve diff/integrate conflict diagnostic mismatch"

"${TEST_BIN_DIR}/solve" --antiderivative 'x^2' > "$WORK_DIR/antiderivative-square.out"
assert_file_contains "$WORK_DIR/antiderivative-square.out" '^F(x) = x\^3/3 + C$' "solve did not print the exact antiderivative of x^2"
"${TEST_BIN_DIR}/solve" --antiderivative '3*x^2+2*x' > "$WORK_DIR/antiderivative-poly.out"
assert_file_contains "$WORK_DIR/antiderivative-poly.out" '^F(x) = x\^3 + x\^2 + C$' "solve did not simplify an exact polynomial antiderivative"
"${TEST_BIN_DIR}/solve" --antiderivative 'sin(x)' > "$WORK_DIR/antiderivative-sin.out"
assert_file_contains "$WORK_DIR/antiderivative-sin.out" '^F[(]x[)] = -cos[(]x[)] [+] C$' "solve did not use the symbolic table for integral sin(x)"
assert_file_contains "$WORK_DIR/antiderivative-sin.out" '^method = symbolic-table$' "solve did not label symbolic-table antiderivatives"
"${TEST_BIN_DIR}/solve" --antiderivative 'cos(2*x)' > "$WORK_DIR/antiderivative-cos-linear.out"
assert_file_contains "$WORK_DIR/antiderivative-cos-linear.out" '^F[(]x[)] = sin[(]2\*x[)]/2 [+] C$' "solve did not use the chain factor for integral cos(2*x)"
"${TEST_BIN_DIR}/solve" --antiderivative 'exp(-0.5*x)' > "$WORK_DIR/antiderivative-exp-linear.out"
assert_file_contains "$WORK_DIR/antiderivative-exp-linear.out" '^F[(]x[)] = -2\*exp[(]-0\.5\*x[)] [+] C$' "solve did not use the chain factor for integral exp(-0.5*x)"
"${TEST_BIN_DIR}/solve" --antiderivative '1/x' > "$WORK_DIR/antiderivative-one-over-x.out"
assert_file_contains "$WORK_DIR/antiderivative-one-over-x.out" '^F[(]x[)] = log[(]abs[(]x[)][)] [+] C$' "solve did not use the symbolic table for integral 1/x"

"${TEST_BIN_DIR}/solve" --monotonicity 'x^2' > "$WORK_DIR/monotonicity-square.out"
assert_file_contains "$WORK_DIR/monotonicity-square.out" '^decreasing = (-inf, 0)$' "solve did not report the decreasing interval of x^2"
assert_file_contains "$WORK_DIR/monotonicity-square.out" '^increasing = (0, inf)$' "solve did not report the increasing interval of x^2"
"${TEST_BIN_DIR}/solve" --curvature 'x^3' > "$WORK_DIR/curvature-cubic.out"
assert_file_contains "$WORK_DIR/curvature-cubic.out" '^right-curved = (-inf, 0)$' "solve did not report negative curvature interval of x^3"
assert_file_contains "$WORK_DIR/curvature-cubic.out" '^left-curved = (0, inf)$' "solve did not report positive curvature interval of x^3"
"${TEST_BIN_DIR}/solve" --monotonicity 'x^2*exp(-x)' > "$WORK_DIR/monotonicity-exp-poly.out"
assert_file_contains "$WORK_DIR/monotonicity-exp-poly.out" '^decreasing = (-inf, 0)$' "solve exp-polynomial monotonicity should use the exact derivative factor before x=0"
assert_file_contains "$WORK_DIR/monotonicity-exp-poly.out" '^increasing = (0, 2)$' "solve exp-polynomial monotonicity should use the exact derivative factor between critical points"
assert_file_contains "$WORK_DIR/monotonicity-exp-poly.out" '^method = exact-exp-polynomial-sign$' "solve exp-polynomial monotonicity should label the exact sign-factor method"
"${TEST_BIN_DIR}/solve" --curvature 'x^2*exp(-x)' > "$WORK_DIR/curvature-exp-poly.out"
assert_file_contains "$WORK_DIR/curvature-exp-poly.out" '^left-curved = (-inf, 0\.5857864376)$' "solve exp-polynomial curvature should report the first irrational factor root"
assert_file_contains "$WORK_DIR/curvature-exp-poly.out" '^right-curved = (0\.5857864376, 3\.4142135624)$' "solve exp-polynomial curvature should report the middle curvature interval"
assert_file_contains "$WORK_DIR/curvature-exp-poly.out" '^left-curved = (3\.4142135624, inf)$' "solve exp-polynomial curvature should report the final curvature interval"

"${TEST_BIN_DIR}/solve" --tangent 2 'x^2' > "$WORK_DIR/tangent-square.out"
assert_file_contains "$WORK_DIR/tangent-square.out" '^tangent: y = 4\*x - 4$' "solve did not print the exact tangent line at x=2 for x^2"
"${TEST_BIN_DIR}/solve" --explain --tangent 2 'x^2*exp(-x)' > "$WORK_DIR/tangent-exp-poly.out"
assert_file_contains "$WORK_DIR/tangent-exp-poly.out" '^method = exact-exp-polynomial-derivative-factor$' "solve tangent should reuse the exact exp-polynomial derivative factor"
assert_file_contains "$WORK_DIR/tangent-exp-poly.out" '^tangent approximate: y = 0\*x [+] 0\.5413411329$' "solve exp-polynomial tangent output mismatch"
"${TEST_BIN_DIR}/solve" --normal 0 'x^2' > "$WORK_DIR/normal-vertical.out"
assert_file_contains "$WORK_DIR/normal-vertical.out" '^normal: x = 0$' "solve did not report a vertical normal at a horizontal tangent"

"${TEST_BIN_DIR}/solve" --end-behavior 'x^3' > "$WORK_DIR/end-behavior-cubic.out"
assert_file_contains "$WORK_DIR/end-behavior-cubic.out" '^limit x->-inf: -inf$' "solve cubic left end behavior mismatch"
assert_file_contains "$WORK_DIR/end-behavior-cubic.out" '^limit x->inf: +inf$' "solve cubic right end behavior mismatch"

"${TEST_BIN_DIR}/solve" --discuss 'x^3' > "$WORK_DIR/discuss-cubic.out"
assert_file_contains "$WORK_DIR/discuss-cubic.out" '^saddle: (0, 0)$' "solve must classify x^3 at x=0 as a saddle"
if grep -q '^maximum: (0, 0)$\|^minimum: (0, 0)$' "$WORK_DIR/discuss-cubic.out"; then
	fail "solve classified the x^3 terrace point as an extremum"
fi
"${TEST_BIN_DIR}/solve" --discuss 'x^3 - 3*x' > "$WORK_DIR/discuss-cubic-shift.out"
assert_file_contains "$WORK_DIR/discuss-cubic-shift.out" '^maximum: (-1, 2)$' "solve did not classify the exact local maximum of x^3 - 3*x"
assert_file_contains "$WORK_DIR/discuss-cubic-shift.out" '^minimum: (1, -2)$' "solve did not classify the exact local minimum of x^3 - 3*x"
assert_file_contains "$WORK_DIR/discuss-cubic-shift.out" '^inflection: (0, 0)$' "solve did not report the exact inflection point of x^3 - 3*x"
"${TEST_BIN_DIR}/solve" 'x^3 - 3*x' > "$WORK_DIR/overview-default.out"
assert_file_contains "$WORK_DIR/overview-default.out" '^overview$' "solve bare expression should default to overview mode"
assert_file_contains "$WORK_DIR/overview-default.out" '^zeros: -1\.7320508076, 0, 1\.7320508076$' "solve default overview should include zeros"
assert_file_contains "$WORK_DIR/overview-default.out" '^maximum: (-1, 2)$' "solve default overview should include extrema"
"${TEST_BIN_DIR}/solve" --explain 'x^3 - 3*x' > "$WORK_DIR/overview-explain-default.out"
assert_file_contains "$WORK_DIR/overview-explain-default.out" '^overview$' "solve --explain bare expression should default to overview mode"
assert_file_contains "$WORK_DIR/overview-explain-default.out" '^explain: curve discussion$' "solve --explain default overview should explain curve discussion"
"${TEST_BIN_DIR}/solve" --discuss 'x*exp(x)' > "$WORK_DIR/discuss-xexp.out"
assert_file_contains "$WORK_DIR/discuss-xexp.out" '^domain: all real x$' "solve exp-polynomial discussion should report the full domain"
assert_file_contains "$WORK_DIR/discuss-xexp.out" '^structure: exp[(]linear[)]\*polynomial$' "solve exp-polynomial discussion should label the recognized structure"
assert_file_contains "$WORK_DIR/discuss-xexp.out" '^zeros: 0$' "solve did not report the exact zero factor of x*exp(x)"
assert_file_contains "$WORK_DIR/discuss-xexp.out" '^minimum approximate: (-1, -0\.3678794412)$' "solve did not classify the x*exp(x) minimum from exact derivative factors"
assert_file_contains "$WORK_DIR/discuss-xexp.out" '^inflection approximate: (-2, -0\.2706705665)$' "solve did not report the x*exp(x) inflection point from exact derivative factors"
assert_file_contains "$WORK_DIR/discuss-xexp.out" '^decreasing = (-inf, -1)$' "solve did not report the full decreasing interval of x*exp(x)"
assert_file_contains "$WORK_DIR/discuss-xexp.out" '^increasing = (-1, inf)$' "solve did not report the full increasing interval of x*exp(x)"
assert_file_contains "$WORK_DIR/discuss-xexp.out" '^right-curved = (-inf, -2)$' "solve did not report the full right-curved interval of x*exp(x)"
assert_file_contains "$WORK_DIR/discuss-xexp.out" '^left-curved = (-2, inf)$' "solve did not report the full left-curved interval of x*exp(x)"
assert_file_contains "$WORK_DIR/discuss-xexp.out" '^limit x->-inf approximate: 0 from below$' "solve did not report the left end behavior of x*exp(x)"
assert_file_contains "$WORK_DIR/discuss-xexp.out" '^horizontal asymptote approximate: y = 0$' "solve did not report the numeric horizontal asymptote of x*exp(x)"
assert_file_contains "$WORK_DIR/discuss-xexp.out" '^limit x->inf approximate: +inf$' "solve did not report the right end behavior of x*exp(x)"
assert_file_contains "$WORK_DIR/discuss-xexp.out" '^method = exact-exp-polynomial-critical-points$' "solve did not label exp-polynomial discussion method"
assert_file_contains "$WORK_DIR/discuss-xexp.out" '^status = approximate-values$' "solve did not label exp-polynomial values as approximate"

"${TEST_BIN_DIR}/solve" --area -1:1 'x^3 - x' '0' > "$WORK_DIR/area-odd.out"
assert_file_contains "$WORK_DIR/area-odd.out" '^area = 1/2$' "solve area must sum absolute lobes instead of returning signed zero"
"${TEST_BIN_DIR}/solve" --area 0:2 '2*x' 'x^2' > "$WORK_DIR/area-parabola-line.out"
assert_file_contains "$WORK_DIR/area-parabola-line.out" '^area = 4/3$' "solve exact area between 2*x and x^2 mismatch"
"${TEST_BIN_DIR}/solve" --area '2*x' 'x^2' > "$WORK_DIR/area-auto-bounds.out"
assert_file_contains "$WORK_DIR/area-auto-bounds.out" '^area = 4/3$' "solve did not infer area bounds from exact intersections"
"${TEST_BIN_DIR}/solve" --scan 0:4:400 --area 'sin(x)' '0' > "$WORK_DIR/area-numeric-auto-bounds.out"
assert_file_contains "$WORK_DIR/area-numeric-auto-bounds.out" '^area = 2\.0000000000$' "solve did not infer numeric omitted area bounds from scan roots"
assert_file_contains "$WORK_DIR/area-numeric-auto-bounds.out" '^method = simpson$' "solve numeric omitted-bound area should use Simpson integration"
area_numeric_no_scan_status=0
"${TEST_BIN_DIR}/solve" --area 'sin(x)' '0' > "$WORK_DIR/area-numeric-no-scan.out" 2> "$WORK_DIR/area-numeric-no-scan.err" || area_numeric_no_scan_status=$?
assert_text_equals "$area_numeric_no_scan_status" '2' "solve numeric omitted-bound area should require an explicit scan range"
assert_file_contains "$WORK_DIR/area-numeric-no-scan.err" 'numeric omitted area bounds require --scan' "solve numeric omitted-bound area diagnostic mismatch"
"${TEST_BIN_DIR}/solve" --area-quadrant II 'x^3 - 3*x' '0' > "$WORK_DIR/area-quadrant.out"
assert_file_contains "$WORK_DIR/area-quadrant.out" '^area = 2\.2500000000$' "solve quadrant area mismatch"
assert_file_contains "$WORK_DIR/area-quadrant.out" '^rational area hint = 9/4$' "solve quadrant area should offer a rational hint for the irrational-endpoint polynomial lobe"
assert_file_contains "$WORK_DIR/area-quadrant.out" '^method = simpson$' "solve quadrant area with irrational endpoints should use numeric fallback"

"${TEST_BIN_DIR}/solve" --eval --at x=2 'x^2*exp(-x)' > "$WORK_DIR/eval-at.out"
assert_file_contains "$WORK_DIR/eval-at.out" '^value = 0\.5413411329$' "solve --eval --at output mismatch"
"${TEST_BIN_DIR}/solve" --param k=2 --eval --at x=3 'k*x + 1' > "$WORK_DIR/eval-param-value.out"
assert_file_contains "$WORK_DIR/eval-param-value.out" '^value = 7\.0000000000$' "solve --param NAME=VALUE should bind numeric parameters during evaluation"
assert_file_contains "$WORK_DIR/eval-param-value.out" '^method = direct-evaluation$' "solve numeric parameter evaluation should remain direct evaluation"
"${TEST_BIN_DIR}/solve" --subst x=k 'exp(-k)*((x-k)^3 - 3*(x-k) + k^2)' > "$WORK_DIR/subst.out"
assert_file_contains "$WORK_DIR/subst.out" '^exp(-k)' "solve --subst output missing leading factor"
assert_file_contains "$WORK_DIR/subst.out" 'k\^2' "solve --subst should simplify substituted zero differences"
if grep -q 'k-k' "$WORK_DIR/subst.out"; then
	fail "solve --subst left an unsimplified k-k term"
fi
"${TEST_BIN_DIR}/solve" --subst x=2 'x^2 + 3*x' > "$WORK_DIR/subst-folded.out"
assert_file_contains "$WORK_DIR/subst-folded.out" '^10$' "solve --subst should fold numeric constants after substitution"
"${TEST_BIN_DIR}/solve" --eval --at x=k 'exp(-k)*((x-k)^3 - 3*(x-k) + k^2)' > "$WORK_DIR/eval-symbolic-at.out"
assert_file_contains "$WORK_DIR/eval-symbolic-at.out" '^value expression = exp[(]-k[)]\*k\^2$' "solve --eval --at should fall back to symbolic substitution for parameter values"
assert_file_contains "$WORK_DIR/eval-symbolic-at.out" '^method = symbolic-substitution$' "solve symbolic --eval --at should label the fallback method"
"${TEST_BIN_DIR}/solve" --explain --subst x=k 'exp(-k)*((x-k)^3 - 3*(x-k) + k^2)' > "$WORK_DIR/explain-subst.out"
assert_file_contains "$WORK_DIR/explain-subst.out" '^after replacement: exp[(]-k[)]\*[(][(]k-k[)]\^3 - 3\*[(]k-k[)] [+] k\^2[)]$' "solve --explain --subst should show the expression before simplification"
assert_file_contains "$WORK_DIR/explain-subst.out" '^- identical terms cancel: k - k = 0$' "solve --explain --subst should show the k-k cancellation step"
assert_file_contains "$WORK_DIR/explain-subst.out" '^- positive powers of zero collapse: 0\^n = 0$' "solve --explain --subst should show the zero-power simplification rule"
assert_file_contains "$WORK_DIR/explain-subst.out" '^simplified expression: exp[(]-k[)]\*k\^2$' "solve --explain --subst should show the simplified expression"
"${TEST_BIN_DIR}/solve" --explain --eval --at x=k 'exp(-k)*((x-k)^3 - 3*(x-k) + k^2)' > "$WORK_DIR/explain-eval-symbolic-at.out"
assert_file_contains "$WORK_DIR/explain-eval-symbolic-at.out" '^explain: symbolic evaluation$' "solve --explain symbolic eval should identify symbolic evaluation mode"
assert_file_contains "$WORK_DIR/explain-eval-symbolic-at.out" '^simplified expression: exp[(]-k[)]\*k\^2$' "solve --explain symbolic eval should show the simplified value expression"
"${TEST_BIN_DIR}/solve" --average-rate 11:21 '18 + 36*exp(-0.033*x)' > "$WORK_DIR/average-rate.out"
assert_file_contains "$WORK_DIR/average-rate.out" '^average rate approximate = -0\.7038462161$' "solve average rate mismatch"
"${TEST_BIN_DIR}/solve" --max --range -0.5:inf 'x^2*exp(-x)' > "$WORK_DIR/max-exp.out"
assert_file_contains "$WORK_DIR/max-exp.out" '^maximum: (2, 0\.5413411329)$' "solve exact-critical maximum mismatch"
assert_file_contains "$WORK_DIR/max-exp.out" '^method = exact-exp-polynomial-critical-points$' "solve should use exact exp-polynomial critical points"
"${TEST_BIN_DIR}/solve" --integrate -1.7320508075688772:0 'x^3 - 3*x' > "$WORK_DIR/integral-overflow-fallback.out"
assert_file_contains "$WORK_DIR/integral-overflow-fallback.out" '^method = simpson$' "solve exact integration overflow should fall back to Simpson"
"${TEST_BIN_DIR}/solve" --param b=2 --integrate 0:b 'x' > "$WORK_DIR/integral-param-bound.out"
assert_file_contains "$WORK_DIR/integral-param-bound.out" '^integral = 2\.0000000000$' "solve --integrate should accept numeric parameter bounds"
assert_file_contains "$WORK_DIR/integral-param-bound.out" '^method = simpson$' "solve numeric parameter bounds should use numeric integration when exact bounds are not literal rationals"
"${TEST_BIN_DIR}/solve" --fit-exp-asymptote 18 --points '1:71,10:51' > "$WORK_DIR/fit-exp.out"
assert_file_contains "$WORK_DIR/fit-exp.out" '^b = 55\.8648074527$' "solve exponential-asymptote fit b mismatch"
assert_file_contains "$WORK_DIR/fit-exp.out" '^c = 0\.0526427058$' "solve exponential-asymptote fit c mismatch"

"${TEST_BIN_DIR}/solve" --volume 0:1 'x' > "$WORK_DIR/volume-line.out"
assert_file_contains "$WORK_DIR/volume-line.out" '^volume = pi\*(1/3)$' "solve exact rotation volume mismatch"
"${TEST_BIN_DIR}/solve" --mean 0:2 '2*x' > "$WORK_DIR/mean-line.out"
assert_file_contains "$WORK_DIR/mean-line.out" '^mean = 2$' "solve exact mean value mismatch"
"${TEST_BIN_DIR}/solve" --volume 0:pi 'sin(x)' > "$WORK_DIR/volume-sine.out"
assert_file_contains "$WORK_DIR/volume-sine.out" '^volume approximate = pi\*(1\.5707963268)$' "solve numeric rotation volume mismatch"
assert_file_contains "$WORK_DIR/volume-sine.out" '^status = approximate$' "solve did not label numeric volume as approximate"
"${TEST_BIN_DIR}/solve" --mean 0:pi 'sin(x)' > "$WORK_DIR/mean-sine.out"
assert_file_contains "$WORK_DIR/mean-sine.out" '^mean approximate = 0\.6366197724$' "solve numeric mean value mismatch"
assert_file_contains "$WORK_DIR/mean-sine.out" '^status = approximate$' "solve did not label numeric mean as approximate"

"${TEST_BIN_DIR}/solve" --limit 'x->2' '(x^2 - 4)/(x - 2)' > "$WORK_DIR/limit-removable.out"
assert_file_contains "$WORK_DIR/limit-removable.out" '^limit = 4$' "solve did not report the removable 0/0 limit"
limit_pole_status=0
"${TEST_BIN_DIR}/solve" --limit 'x->0' '1/x' > "$WORK_DIR/limit-pole.out" 2> "$WORK_DIR/limit-pole.err" || limit_pole_status=$?
assert_text_equals "$limit_pole_status" '1' "solve should return 1 for a missing two-sided pole limit"
assert_file_contains "$WORK_DIR/limit-pole.out" '^limit: no two-sided limit (pole)$' "solve printed a finite or non-pole limit for 1/x at 0"
limit_jump_status=0
"${TEST_BIN_DIR}/solve" --limit 'x->0' 'abs(x)/x' > "$WORK_DIR/limit-jump.out" 2> "$WORK_DIR/limit-jump.err" || limit_jump_status=$?
assert_text_equals "$limit_jump_status" '1' "solve should return 1 for a finite jump with no two-sided limit"
assert_file_contains "$WORK_DIR/limit-jump.out" '^limit: no two-sided limit$' "solve did not reject the finite jump limit of abs(x)/x"
assert_file_contains "$WORK_DIR/limit-jump.out" '^left = -1$' "solve finite jump left-hand limit mismatch"
assert_file_contains "$WORK_DIR/limit-jump.out" '^right = 1$' "solve finite jump right-hand limit mismatch"

"${TEST_BIN_DIR}/solve" --asymptotes '(x^2 + 1)/(x - 1)' > "$WORK_DIR/asymptotes-rational.out"
assert_file_contains "$WORK_DIR/asymptotes-rational.out" '^vertical: x = 1$' "solve did not report the vertical asymptote"
assert_file_contains "$WORK_DIR/asymptotes-rational.out" '^oblique: y = x + 1$' "solve did not report the oblique asymptote"
"${TEST_BIN_DIR}/solve" --asymptotes '1 + 2/(x-3)' > "$WORK_DIR/asymptotes-sum-rational.out"
assert_file_contains "$WORK_DIR/asymptotes-sum-rational.out" '^vertical: x = 3$' "solve did not normalize quotient-plus-polynomial asymptote input"
assert_file_contains "$WORK_DIR/asymptotes-sum-rational.out" '^horizontal: y = 1$' "solve did not report the horizontal asymptote of quotient-plus-polynomial input"

"${TEST_BIN_DIR}/solve" --explain --antiderivative 'x^2' > "$WORK_DIR/explain-antiderivative.out"
assert_file_contains "$WORK_DIR/explain-antiderivative.out" '^explain: antiderivative$' "solve --explain did not identify antiderivative mode"
assert_file_contains "$WORK_DIR/explain-antiderivative.out" '^rule: integral a\*x\^n dx = a\*x\^(n+1)/(n+1)$' "solve --explain antiderivative missing power-rule certificate"
"${TEST_BIN_DIR}/solve" --explain --diff 'x^3' > "$WORK_DIR/explain-diff.out"
assert_file_contains "$WORK_DIR/explain-diff.out" '^explain: derivative$' "solve --explain did not identify derivative mode"
assert_file_contains "$WORK_DIR/explain-diff.out" '^derivative: 3\*x\^2$' "solve --explain derivative missing derivative polynomial"
"${TEST_BIN_DIR}/solve" --explain --integrate 0:1 'x^2' > "$WORK_DIR/explain-integral-exact.out"
assert_file_contains "$WORK_DIR/explain-integral-exact.out" '^explain: definite integral$' "solve --explain exact integral missing mode header"
assert_file_contains "$WORK_DIR/explain-integral-exact.out" '^rule: integral from a to b = F(b) - F(a)$' "solve --explain exact integral missing fundamental theorem rule"
"${TEST_BIN_DIR}/solve" --explain --integrate 0:pi 'sin(x)' > "$WORK_DIR/explain-integral-numeric.out"
assert_file_contains "$WORK_DIR/explain-integral-numeric.out" '^method detail: composite Simpson rule with 1000 and 2000 subintervals$' "solve --explain numeric integral missing Simpson detail"
"${TEST_BIN_DIR}/solve" --explain 'x^2 - 4 > 0' > "$WORK_DIR/explain-inequality.out"
assert_file_contains "$WORK_DIR/explain-inequality.out" '^explain: inequality$' "solve --explain inequality missing mode header"
assert_file_contains "$WORK_DIR/explain-inequality.out" '^target sign: f(x) > 0$' "solve --explain inequality missing target sign"
assert_file_contains "$WORK_DIR/explain-inequality.out" '^boundary roots: -2, 2$' "solve --explain inequality missing exact boundary roots"
"${TEST_BIN_DIR}/solve" --explain --monotonicity 'x^2' > "$WORK_DIR/explain-monotonicity.out"
assert_file_contains "$WORK_DIR/explain-monotonicity.out" '^first derivative: 2\*x$' "solve --explain monotonicity missing first derivative"
assert_file_contains "$WORK_DIR/explain-monotonicity.out" '^rule: f'"'"' > 0 means increasing; f'"'"' < 0 means decreasing$' "solve --explain monotonicity missing sign rule"
"${TEST_BIN_DIR}/solve" --explain --curvature 'x^3' > "$WORK_DIR/explain-curvature.out"
assert_file_contains "$WORK_DIR/explain-curvature.out" '^second derivative: 6\*x$' "solve --explain curvature missing second derivative"
assert_file_contains "$WORK_DIR/explain-curvature.out" '^rule: f'"'"''"'"' > 0 means left-curved; f'"'"''"'"' < 0 means right-curved$' "solve --explain curvature missing sign rule"
"${TEST_BIN_DIR}/solve" --explain --tangent 2 'x^2' > "$WORK_DIR/explain-tangent.out"
assert_file_contains "$WORK_DIR/explain-tangent.out" '^point a = 2$' "solve --explain tangent missing point"
assert_file_contains "$WORK_DIR/explain-tangent.out" '^intercept f(a) - m\*a = -4$' "solve --explain tangent missing line intercept construction"
"${TEST_BIN_DIR}/solve" --explain --normal 0 'x^2' > "$WORK_DIR/explain-normal.out"
assert_file_contains "$WORK_DIR/explain-normal.out" '^rule: tangent slope is 0, so the normal line is vertical$' "solve --explain normal missing vertical-normal reason"
"${TEST_BIN_DIR}/solve" --explain --end-behavior 'x^3' > "$WORK_DIR/explain-end-behavior.out"
assert_file_contains "$WORK_DIR/explain-end-behavior.out" '^rule: the leading nonzero term controls behavior as x approaches +/-inf$' "solve --explain end behavior missing leading-term rule"
"${TEST_BIN_DIR}/solve" --explain --discuss 'x^3 - 3*x' > "$WORK_DIR/explain-discuss-poly.out"
assert_file_contains "$WORK_DIR/explain-discuss-poly.out" '^extremum rule: f'"'"' sign change + to - gives maximum; - to + gives minimum; no sign change gives saddle$' "solve --explain polynomial discussion missing extremum rule"
assert_file_contains "$WORK_DIR/explain-discuss-poly.out" '^inflection rule: roots of f'"'"''"'"' where curvature changes sign are inflection points$' "solve --explain polynomial discussion missing inflection rule"
"${TEST_BIN_DIR}/solve" --explain --discuss 'x*exp(x)' > "$WORK_DIR/explain-discuss-numeric.out"
assert_file_contains "$WORK_DIR/explain-discuss-numeric.out" '^zero factor: x$' "solve --explain exp-polynomial discussion missing zero factor"
assert_file_contains "$WORK_DIR/explain-discuss-numeric.out" '^first-derivative factor: x [+] 1$' "solve --explain exp-polynomial discussion missing first derivative factor"
assert_file_contains "$WORK_DIR/explain-discuss-numeric.out" '^rule: the exponential factor is positive, so exact polynomial factors decide zeros, critical points, and curvature changes$' "solve --explain exp-polynomial discussion missing sign-factor rule"
"${TEST_BIN_DIR}/solve" --explain --area -1:1 'x^3 - x' '0' > "$WORK_DIR/explain-area.out"
assert_file_contains "$WORK_DIR/explain-area.out" '^rule: area is integral |h(x)| dx, so sign changes split the interval into absolute pieces$' "solve --explain area missing absolute-area rule"
assert_file_contains "$WORK_DIR/explain-area.out" '^area cut roots: -1, 0, 1$' "solve --explain area missing lobe split roots"
"${TEST_BIN_DIR}/solve" --explain --volume 0:pi 'sin(x)' > "$WORK_DIR/explain-volume.out"
assert_file_contains "$WORK_DIR/explain-volume.out" '^formula: volume = pi \* integral f(x)\^2 dx$' "solve --explain volume missing rotation formula"
assert_file_contains "$WORK_DIR/explain-volume.out" '^method detail: composite Simpson rule integrates f(x)\^2 with 1000 and 2000 subintervals$' "solve --explain numeric volume missing Simpson detail"
"${TEST_BIN_DIR}/solve" --explain --mean 0:pi 'sin(x)' > "$WORK_DIR/explain-mean.out"
assert_file_contains "$WORK_DIR/explain-mean.out" '^formula: mean = (1/(b-a)) \* integral f(x) dx$' "solve --explain mean missing mean-value formula"
assert_file_contains "$WORK_DIR/explain-mean.out" '^method detail: composite Simpson rule integrates f(x), then divides by b-a$' "solve --explain numeric mean missing Simpson detail"
explain_limit_jump_status=0
"${TEST_BIN_DIR}/solve" --explain --limit 'x->0' 'abs(x)/x' > "$WORK_DIR/explain-limit-jump.out" 2> "$WORK_DIR/explain-limit-jump.err" || explain_limit_jump_status=$?
assert_text_equals "$explain_limit_jump_status" '1' "solve --explain finite jump limit should preserve exit status"
assert_file_contains "$WORK_DIR/explain-limit-jump.out" '^rule: matching left and right samples indicate a two-sided limit; divergent magnitude indicates a pole; finite mismatch indicates a jump$' "solve --explain limit missing limit classification rule"
"${TEST_BIN_DIR}/solve" --explain --asymptotes '(x^2 + 1)/(x - 1)' > "$WORK_DIR/explain-asymptotes.out"
assert_file_contains "$WORK_DIR/explain-asymptotes.out" '^vertical rule: denominator root with nonzero numerator gives a vertical asymptote$' "solve --explain asymptotes missing vertical rule"
assert_file_contains "$WORK_DIR/explain-asymptotes.out" '^division form: numerator/denominator = quotient + remainder/denominator$' "solve --explain asymptotes missing division form"

"${TEST_BIN_DIR}/solve" --quiet '(10000000000*x + 1)^8 = (10000000000*x + 1)^8' > "$WORK_DIR/rational-overflow-fallback.out"
assert_file_contains "$WORK_DIR/rational-overflow-fallback.out" '^all real values' "solve did not fall back after exact rational coefficient overflow"

"${TEST_BIN_DIR}/solve" --json --lo 1 --hi 2 'x^2 - 2 = 0' > "$WORK_DIR/solve.jsonl"
assert_file_contains "$WORK_DIR/solve.jsonl" '"event":"solve_result"' "solve --json did not emit a solve_result event"
assert_file_contains "$WORK_DIR/solve.jsonl" '"root":"1\.4142135624"' "solve --json root value mismatch"
assert_file_contains "$WORK_DIR/solve.jsonl" '"event":"solve_summary"' "solve --json did not emit a solve_summary event"

# --json must be universal: every mode emits only JSON objects, never plain text
for json_mode in '--eval --at x=3 x^2' '--diff x^3' '--integrate 0:1 x^2' '--discuss x^3-3*x' '--monotonicity x^2' '--tangent 2 x^2' '--end-behavior x^3' '--limit x->2 (x^2-4)/(x-2)' '--volume 0:1 x' '--mean 0:2 2*x' '--average-rate 0:2 x^2' 'x^2-4>0'; do
	"${TEST_BIN_DIR}/solve" --json $json_mode > "$WORK_DIR/json-universal.out" 2>/dev/null || true
	while IFS= read -r json_line; do
		[ -z "$json_line" ] && continue
		case "$json_line" in
			'{"schema":"newos.tool.v1"'*) ;;
			*) fail "solve --json emitted non-JSON line for [$json_mode]: $json_line" ;;
		esac
	done < "$WORK_DIR/json-universal.out"
done
"${TEST_BIN_DIR}/solve" --json --eval --at x=3 'x^2' > "$WORK_DIR/json-eval.out"
assert_file_contains "$WORK_DIR/json-eval.out" '"event":"solve_value"' "solve --json --eval did not emit a solve_value event"
assert_file_contains "$WORK_DIR/json-eval.out" '"key":"value","value":"9\.0000000000"' "solve --json --eval value field mismatch"
"${TEST_BIN_DIR}/solve" --json --discuss 'x^3-3*x' > "$WORK_DIR/json-discuss.out"
assert_file_contains "$WORK_DIR/json-discuss.out" '"event":"solve_output"' "solve --json --discuss did not emit solve_output events"
assert_file_contains "$WORK_DIR/json-discuss.out" '"text":"maximum: (-1, 2)"' "solve --json --discuss point text mismatch"
"${TEST_BIN_DIR}/solve" --json 'x^3-3*x' > "$WORK_DIR/json-overview-default.out"
assert_file_contains "$WORK_DIR/json-overview-default.out" '"event":"solve_output"' "solve --json bare expression should emit solve_output events"
assert_file_contains "$WORK_DIR/json-overview-default.out" '"text":"overview"' "solve --json bare expression should emit the overview marker"
assert_file_contains "$WORK_DIR/json-overview-default.out" '"text":"maximum: (-1, 2)"' "solve --json default overview point text mismatch"

no_root_status=0
"${TEST_BIN_DIR}/solve" --lo 1 --hi 2 'x^2 + 1 = 0' > "$WORK_DIR/no_root.out" 2> "$WORK_DIR/no_root.err" || no_root_status=$?
assert_text_equals "$no_root_status" '1' "solve should return 1 when no solution is found"
assert_file_contains "$WORK_DIR/no_root.out" 'no solution found' "solve no-root output mismatch"

discontinuity_status=0
"${TEST_BIN_DIR}/solve" --explain=trace --lo 1 --hi 3 '1/(x - 2) = 0' > "$WORK_DIR/discontinuity.out" 2> "$WORK_DIR/discontinuity.err" || discontinuity_status=$?
assert_text_equals "$discontinuity_status" '3' "solve should return 3 for a suspected discontinuity"
assert_file_contains "$WORK_DIR/discontinuity.out" 'suspected discontinuity' "solve --explain=trace did not report the suspected discontinuity"

unknown_status=0
"${TEST_BIN_DIR}/solve" 'x + y = 3' > "$WORK_DIR/unknown.out" 2> "$WORK_DIR/unknown.err" || unknown_status=$?
assert_text_equals "$unknown_status" '2' "solve should return 2 for an unknown identifier"
assert_file_contains "$WORK_DIR/unknown.err" 'unknown identifier' "solve unknown identifier diagnostic mismatch"

conflict_status=0
"${TEST_BIN_DIR}/solve" --scan 0:1 --lo 0 --hi 1 'x = 0' > "$WORK_DIR/conflict.out" 2> "$WORK_DIR/conflict.err" || conflict_status=$?
assert_text_equals "$conflict_status" '2' "solve should reject --scan combined with --lo/--hi"
assert_file_contains "$WORK_DIR/conflict.err" 'cannot be combined' "solve interval conflict diagnostic mismatch"

# Regression: a cubic with one real root plus a complex pair must report the real root
"${TEST_BIN_DIR}/solve" 'x^3 - 8 = 0' > "$WORK_DIR/cubic-single-real.out"
assert_file_contains "$WORK_DIR/cubic-single-real.out" '^x = 2$' "solve must report the lone real root of x^3 - 8"
if grep -q '^no real solutions$' "$WORK_DIR/cubic-single-real.out"; then
	fail "solve must not discard a real cubic root when the leftover factor is complex"
fi
"${TEST_BIN_DIR}/solve" 'x^3 = 27' > "$WORK_DIR/cubic-perfect.out"
assert_file_contains "$WORK_DIR/cubic-perfect.out" '^x = 3$' "solve must report the cube root of 27"

# Regression: exact polynomial roots must report even outside the default scan window
"${TEST_BIN_DIR}/solve" --all 'x^2 = 40000' > "$WORK_DIR/quad-wide.out"
assert_file_contains "$WORK_DIR/quad-wide.out" '^x = -200$' "solve must report exact root -200 outside default scan window"
assert_file_contains "$WORK_DIR/quad-wide.out" '^x = 200$' "solve must report exact root 200 outside default scan window"
"${TEST_BIN_DIR}/solve" --all 'x^2 = 1000000000000' > "$WORK_DIR/quad-million.out"
assert_file_contains "$WORK_DIR/quad-million.out" '^x = 1000000$' "solve must report large exact quadratic root"
# An explicit scan window still clips reported roots
quad_clip_status=0
"${TEST_BIN_DIR}/solve" --scan -100:100 'x^2 = 40000' > "$WORK_DIR/quad-clip.out" 2> "$WORK_DIR/quad-clip.err" || quad_clip_status=$?
assert_text_equals "$quad_clip_status" '1' "solve should report no roots inside an explicit window that excludes them"
assert_file_contains "$WORK_DIR/quad-clip.out" 'no solution found in requested range' "solve did not clip exact roots to an explicit scan window"

# Extended built-in functions
tan_root=$("${TEST_BIN_DIR}/solve" --quiet --lo 0 --hi 1 'tan(x) = 1' | tr -d '\r\n')
assert_text_equals "$tan_root" '0.7853981634' "solve tan(x)=1 root mismatch"
asin_root=$("${TEST_BIN_DIR}/solve" --quiet --lo 0 --hi 1 'asin(x) = 0.5' | tr -d '\r\n')
assert_text_equals "$asin_root" '0.4794255386' "solve asin(x)=0.5 root mismatch"
acos_root=$("${TEST_BIN_DIR}/solve" --quiet --lo 0 --hi 1 'acos(x) = 1' | tr -d '\r\n')
assert_text_equals "$acos_root" '0.5403023059' "solve acos(x)=1 root mismatch"
tanh_root=$("${TEST_BIN_DIR}/solve" --quiet --lo 0 --hi 2 'tanh(x) = 0.5' | tr -d '\r\n')
assert_text_equals "$tanh_root" '0.5493061442' "solve tanh(x)=0.5 root mismatch"
"${TEST_BIN_DIR}/solve" --all 'cosh(x) = 2' > "$WORK_DIR/cosh.out"
assert_file_contains "$WORK_DIR/cosh.out" '^x = -1\.3169578969$' "solve did not report the negative cosh root"
assert_file_contains "$WORK_DIR/cosh.out" '^x = 1\.3169578969$' "solve did not report the positive cosh root"
sinh_root=$("${TEST_BIN_DIR}/solve" --quiet --lo 0 --hi 2 'sinh(x) = 1' | tr -d '\r\n')
assert_text_equals "$sinh_root" '0.8813735871' "solve sinh(x)=1 root mismatch"
"${TEST_BIN_DIR}/solve" 'floor(x) = 3' > "$WORK_DIR/floor.out"
assert_file_contains "$WORK_DIR/floor.out" '^x = 3$' "solve did not solve floor(x)=3"
"${TEST_BIN_DIR}/solve" 'ceil(x) = 3' > "$WORK_DIR/ceil.out"
assert_file_contains "$WORK_DIR/ceil.out" '^x = 2\.5000000000' "solve did not solve ceil(x)=3"
"${TEST_BIN_DIR}/solve" 'round(x) = 2' > "$WORK_DIR/round.out"
assert_file_contains "$WORK_DIR/round.out" '^x = 1\.5000000000' "solve did not solve round(x)=2"

# Symbolic derivatives of extended functions
"${TEST_BIN_DIR}/solve" --diff 'tan(x)' > "$WORK_DIR/diff-tan.out"
assert_file_contains "$WORK_DIR/diff-tan.out" '^1/cos[(]x[)]\^2$' "solve --diff tan mismatch"
"${TEST_BIN_DIR}/solve" --diff 'sinh(x)' > "$WORK_DIR/diff-sinh.out"
assert_file_contains "$WORK_DIR/diff-sinh.out" '^cosh[(]x[)]$' "solve --diff sinh mismatch"
"${TEST_BIN_DIR}/solve" --diff 'cosh(x)' > "$WORK_DIR/diff-cosh.out"
assert_file_contains "$WORK_DIR/diff-cosh.out" '^sinh[(]x[)]$' "solve --diff cosh mismatch"
"${TEST_BIN_DIR}/solve" --diff 'tanh(x)' > "$WORK_DIR/diff-tanh.out"
assert_file_contains "$WORK_DIR/diff-tanh.out" '^1/cosh[(]x[)]\^2$' "solve --diff tanh mismatch"
"${TEST_BIN_DIR}/solve" --diff 'asin(x)' > "$WORK_DIR/diff-asin.out"
assert_file_contains "$WORK_DIR/diff-asin.out" 'sqrt' "solve --diff asin should contain sqrt(1 - x^2)"
diff_floor_status=0
"${TEST_BIN_DIR}/solve" --diff 'floor(x)' > "$WORK_DIR/diff-floor.out" 2> "$WORK_DIR/diff-floor.err" || diff_floor_status=$?
assert_text_equals "$diff_floor_status" '2' "solve --diff should reject floor (no symbolic derivative)"
assert_file_contains "$WORK_DIR/diff-floor.err" 'symbolic derivative unsupported' "solve --diff floor diagnostic mismatch"
