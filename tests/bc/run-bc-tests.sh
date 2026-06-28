#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
. "$ROOT_DIR/tests/lib/assert.sh"
. "$ROOT_DIR/tests/lib/build.sh"

newos_configure_test_tools

BC=${NEWOS_BC:-$TEST_BIN_DIR/bc}
CANONICAL_BC=${CANONICAL_BC:-}
WORK_DIR="$ROOT_DIR/tests/tmp/bc"

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"

note "bc canonical compatibility and correctness"

if [ ! -x "$BC" ]; then
    fail "bc binary not found or not executable: $BC"
fi

if [ -z "$CANONICAL_BC" ]; then
    if [ -x /usr/bin/bc ]; then
        CANONICAL_BC=/usr/bin/bc
    elif command -v bc >/dev/null 2>&1; then
        CANONICAL_BC=$(command -v bc)
    fi
fi

run_newos_bc() {
    name=$1
    input_file=$2
    shift 2
    output_file="$WORK_DIR/$name.newos.out"
    error_file="$WORK_DIR/$name.newos.err"

    if ! "$BC" "$@" < "$input_file" > "$output_file" 2> "$error_file"; then
        cat "$error_file" >&2
        fail "newos bc failed for $name"
    fi
}

run_canonical_bc() {
    name=$1
    input_file=$2
    shift 2
    output_file="$WORK_DIR/$name.canonical.out"
    error_file="$WORK_DIR/$name.canonical.err"

    if [ -z "$CANONICAL_BC" ] || [ ! -x "$CANONICAL_BC" ]; then
        return 1
    fi
    if ! BC_LINE_LENGTH=0 "$CANONICAL_BC" "$@" < "$input_file" > "$output_file" 2> "$error_file"; then
        cat "$error_file" >&2
        fail "canonical bc failed for $name"
    fi
    return 0
}

compare_with_canonical() {
    name=$1
    program=$2
    input_file="$WORK_DIR/$name.bc"

    printf '%s\n' "$program" > "$input_file"
    run_newos_bc "$name" "$input_file"
    if run_canonical_bc "$name" "$input_file"; then
        assert_files_equal "$WORK_DIR/$name.canonical.out" "$WORK_DIR/$name.newos.out" "bc output differs from canonical bc for $name"
    else
        echo "SKIP canonical comparison for $name: no executable canonical bc found" >&2
    fi
}

compare_programs_with_canonical() {
    name=$1
    newos_program=$2
    canonical_program=$3
    newos_input_file="$WORK_DIR/$name.newos.bc"
    canonical_input_file="$WORK_DIR/$name.canonical.bc"

    printf '%s\n' "$newos_program" > "$newos_input_file"
    printf '%s\n' "$canonical_program" > "$canonical_input_file"
    run_newos_bc "$name" "$newos_input_file"
    if run_canonical_bc "$name" "$canonical_input_file"; then
        assert_files_equal "$WORK_DIR/$name.canonical.out" "$WORK_DIR/$name.newos.out" "bc output differs from canonical bc for $name"
    else
        echo "SKIP canonical comparison for $name: no executable canonical bc found" >&2
    fi
}

compare_math_with_canonical() {
    name=$1
    program=$2
    tolerance=$3
    shift 3
    input_file="$WORK_DIR/$name.bc"

    printf '%s\n' "$program" > "$input_file"
    run_newos_bc "$name" "$input_file" "$@"
    if run_canonical_bc "$name" "$input_file" "$@"; then
        awk -v tol="$tolerance" -v name="$name" '
            BEGIN { ok = 1 }
            FNR == NR { expected[++expected_count] = $0; next }
            {
                actual_count++
                if (actual_count > expected_count) {
                    printf "FAIL: bc emitted extra math result for %s: %s\n", name, $0 > "/dev/stderr"
                    ok = 0
                    next
                }
                delta = ($0 + 0) - (expected[actual_count] + 0)
                if (delta < 0) {
                    delta = -delta
                }
                if (delta > tol) {
                    printf "FAIL: bc math result differs from canonical bc for %s line %d: expected %s, got %s, delta %.12g\n", name, actual_count, expected[actual_count], $0, delta > "/dev/stderr"
                    ok = 0
                }
            }
            END {
                if (actual_count != expected_count) {
                    printf "FAIL: bc math result count differs from canonical bc for %s: expected %d, got %d\n", name, expected_count, actual_count > "/dev/stderr"
                    ok = 0
                }
                exit ok ? 0 : 1
            }
        ' "$WORK_DIR/$name.canonical.out" "$WORK_DIR/$name.newos.out" || exit 1
    else
        echo "SKIP canonical math comparison for $name: no executable canonical bc found" >&2
    fi
}

expect_newos_output() {
    name=$1
    program=$2
    expected=$3
    input_file="$WORK_DIR/$name.bc"
    expected_file="$WORK_DIR/$name.expected.out"

    printf '%s\n' "$program" > "$input_file"
    printf '%s\n' "$expected" > "$expected_file"
    run_newos_bc "$name" "$input_file"
    assert_files_equal "$expected_file" "$WORK_DIR/$name.newos.out" "bc output mismatch for $name"
}

expect_newos_output_with_args() {
    name=$1
    program=$2
    expected=$3
    shift 3
    input_file="$WORK_DIR/$name.bc"
    expected_file="$WORK_DIR/$name.expected.out"

    printf '%s\n' "$program" > "$input_file"
    printf '%s\n' "$expected" > "$expected_file"
    run_newos_bc "$name" "$input_file" "$@"
    assert_files_equal "$expected_file" "$WORK_DIR/$name.newos.out" "bc output mismatch for $name"
}

expect_newos_failure() {
    name=$1
    program=$2
    expected_error=$3
    input_file="$WORK_DIR/$name.bc"
    output_file="$WORK_DIR/$name.failure.out"
    error_file="$WORK_DIR/$name.failure.err"
    status=0

    printf '%s\n' "$program" > "$input_file"
    "$BC" < "$input_file" > "$output_file" 2> "$error_file" || status=$?
    if [ "$status" -eq 0 ]; then
        fail "newos bc unexpectedly succeeded for $name"
    fi
    assert_file_contains "$error_file" "$expected_error" "bc failure message mismatch for $name"
}

compare_with_canonical integer_arithmetic '2 + 3
17 - 29
12 * 34
99 / 8
99 % 8
2 ^ 10
-(7 + 5)'
compare_with_canonical decimal_scale 'scale=4
22 / 7
3.125 + 4.5
3.125 * 4
10.000 - 0.125'
compare_with_canonical variables_and_last 'x=17
y=x*3
y
last + 4
scale=3
last / 5'
compare_with_canonical comparisons_and_logic '3 < 4
3 <= 3
5 == 5
5 != 5
0 && 1
1 || 0
!0
!7'
compare_with_canonical input_base 'ibase=16
FF
10'
compare_with_canonical output_base 'obase=16
255
4095'
compare_with_canonical control_flow 'sum=0
for (i=0; i<5; i=i+1) sum=sum+i
sum
i=0
while (i<3) { i=i+1 }
i'
compare_with_canonical large_integer_arithmetic '999999999999999999999999999999 + 1
12345678901234567890 * 98765432109876543210
2 ^ 80
123456789012345678901234567890 / 97
123456789012345678901234567890 % 97'
compare_with_canonical huge_integer_arithmetic 'a=12345678901234567890123456789012345678901234567890123456789012345678901234567890
b=98765432109876543210987654321098765432109876543210987654321098765432109876543210
a + b
a * b
2 ^ 256
(10^80 + 12345)^2
(a * b + 123456789) / 97
(a * b + 123456789) % 97'
compare_with_canonical complex_decimal_expression 'scale=12
(22 / 7) * (355 / 113)
((12345.6789 + 0.0001) * 1000) / 7
sum=0
for (i=1; i<=20; i=i+1) sum=sum+i*i
sum
last / 13'
compare_math_with_canonical grouped_math_library 'scale=8
p=4*a(1)
s(p/6) + c(p/3)
(s(p/4)^2) + (c(p/4)^2)
a(1) * 4
e(l(5))
sqrt(2)^2
(s(p/8) * c(p/8)) * 2
j(0,0) + j(1,0)' 0.00000002 -l

expect_newos_output builtin_integer_functions 'gcd(48, 18)
lcm(21, 6)
fact(10)
length(12345)
scale(12.3400)' '6
42
3628800
5
4'
expect_newos_output large_factorial 'fact(50)' '30414093201713378043612608166064768844377641568960512000000000000'
compare_programs_with_canonical large_builtin_oracles 'fact(100)
a=12345678901234567890123456789012345678901234567890
b=98765432109876543210987654321098765432109876543210
gcd(a,b)
lcm(a,b)' 'f=1
for (i=1; i<=100; i=i+1) f=f*i
f
a=12345678901234567890123456789012345678901234567890
b=98765432109876543210987654321098765432109876543210
x=a
y=b
while (y != 0) { t=x%y; x=y; y=t }
x
(a / x) * b'
expect_newos_output min_max_abs_sqrt 'sqrt(81)
abs(-12.5)
min(3, 7)
max(3, 7)' '9
12.5
3
7'
expect_newos_output comments_and_blocks 'sum=0 /* block comment */
for (i=0; i<10; i=i+1) { if (i==3) continue; if (i==6) break; sum=sum+i } # line comment
sum
// trailing comment
last' '12
12'
expect_newos_output_with_args math_mode_constants 'scale=10
s(pi/2)
c(pi)
a(1)
e(1)
l(1)
j(0,0)' '1.0000000000
-1.0000000000
0.7853981633
2.7182818284
0.0000000000
1.0000000000' -l

json_out="$WORK_DIR/json.out"
json_err="$WORK_DIR/json.err"
if ! "$BC" --json 'scale=4; 22/7; obase=16; 255' > "$json_out" 2> "$json_err"; then
    cat "$json_err" >&2
    fail "bc --json failed"
fi
assert_file_contains "$json_out" '"event":"bc_result"' "bc --json did not emit result events"
assert_file_contains "$json_out" '"text":"3.1428"' "bc --json did not emit decimal result text"
assert_file_contains "$json_out" '"obase":16' "bc --json did not record obase"
assert_file_contains "$json_out" '"text":"FF"' "bc --json did not emit based result text"

expect_newos_failure divide_by_zero '1 / 0' 'division by zero'
expect_newos_failure logical_and_divide_by_zero '0 && (1 / 0)' 'division by zero'
expect_newos_failure logical_or_divide_by_zero '1 || (1 / 0)' 'division by zero'
expect_newos_failure bad_scale 'scale=257' 'scale out of range'
expect_newos_failure non_integer_factorial 'fact(2.5)' 'non-integer argument'

note "bc tests passed"
