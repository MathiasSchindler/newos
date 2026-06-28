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

"${TEST_BIN_DIR}/solve" --explain --all 'x^2 - 2 = 0' > "$WORK_DIR/quadratic-formula.out"
assert_file_contains "$WORK_DIR/quadratic-formula.out" '^quadratic formula: x = ' "solve did not show the quadratic formula"
assert_file_contains "$WORK_DIR/quadratic-formula.out" '^method = quadratic-formula$' "solve did not report the quadratic formula method"

"${TEST_BIN_DIR}/solve" --explain '(x + 1)^2 = x^2 + 2*x + 1' > "$WORK_DIR/identity.out"
assert_file_contains "$WORK_DIR/identity.out" '^polynomial identity detected$' "solve did not detect a polynomial identity"
assert_file_contains "$WORK_DIR/identity.out" '^identity = true$' "solve did not report the identity result"

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

"${TEST_BIN_DIR}/solve" --explain --method bisection --lo 1 --hi 2 'x^2 - 2 = 0' > "$WORK_DIR/explain.out"
assert_file_contains "$WORK_DIR/explain.out" '^function: f(x) = ' "solve --explain did not print the transformed function"
assert_file_contains "$WORK_DIR/explain.out" '^step 1: ' "solve --explain did not print iteration steps"
assert_file_contains "$WORK_DIR/explain.out" '^x = 1\.4142135624$' "solve --explain did not print the final result"

"${TEST_BIN_DIR}/solve" --explain 'x - 5 / 3 = 0' > "$WORK_DIR/linear-explain.out"
assert_file_contains "$WORK_DIR/linear-explain.out" '^linear equation detected$' "solve --explain did not identify a linear equation"
assert_file_contains "$WORK_DIR/linear-explain.out" '^move constant term: x = 1\.6666666667 (1 2/3)$' "solve --explain did not show the algebraic rearrangement"
assert_file_contains "$WORK_DIR/linear-explain.out" '^method = linear$' "solve --explain did not report the linear method"

"${TEST_BIN_DIR}/solve" 'x - 5 / 3 = 0' > "$WORK_DIR/fraction.out"
assert_file_contains "$WORK_DIR/fraction.out" '^x = 1\.6666666667 (1 2/3)$' "solve did not print a simple mixed-number hint"
"${TEST_BIN_DIR}/solve" 'x - 5 = 0' > "$WORK_DIR/integer.out"
assert_file_contains "$WORK_DIR/integer.out" '^x = 5$' "solve did not print an integer root compactly"

"${TEST_BIN_DIR}/solve" --json --lo 1 --hi 2 'x^2 - 2 = 0' > "$WORK_DIR/solve.jsonl"
assert_file_contains "$WORK_DIR/solve.jsonl" '"event":"solve_result"' "solve --json did not emit a solve_result event"
assert_file_contains "$WORK_DIR/solve.jsonl" '"root":"1\.4142135624"' "solve --json root value mismatch"
assert_file_contains "$WORK_DIR/solve.jsonl" '"event":"solve_summary"' "solve --json did not emit a solve_summary event"

no_root_status=0
"${TEST_BIN_DIR}/solve" --lo 1 --hi 2 'x^2 + 1 = 0' > "$WORK_DIR/no_root.out" 2> "$WORK_DIR/no_root.err" || no_root_status=$?
assert_text_equals "$no_root_status" '1' "solve should return 1 when no solution is found"
assert_file_contains "$WORK_DIR/no_root.out" 'no solution found' "solve no-root output mismatch"

unknown_status=0
"${TEST_BIN_DIR}/solve" 'x + y = 3' > "$WORK_DIR/unknown.out" 2> "$WORK_DIR/unknown.err" || unknown_status=$?
assert_text_equals "$unknown_status" '2' "solve should return 2 for an unknown identifier"
assert_file_contains "$WORK_DIR/unknown.err" 'unknown identifier' "solve unknown identifier diagnostic mismatch"

conflict_status=0
"${TEST_BIN_DIR}/solve" --scan 0:1 --lo 0 --hi 1 'x = 0' > "$WORK_DIR/conflict.out" 2> "$WORK_DIR/conflict.err" || conflict_status=$?
assert_text_equals "$conflict_status" '2' "solve should reject --scan combined with --lo/--hi"
assert_file_contains "$WORK_DIR/conflict.err" 'cannot be combined' "solve interval conflict diagnostic mismatch"
