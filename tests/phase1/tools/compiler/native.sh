#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

phase1_compiler_setup native

cat > "$WORK_DIR/sample.c" <<'EOF'
int main(void) {
    return 42;
}
EOF

compile_and_check_native "$WORK_DIR/sample.c" "$WORK_DIR/sample_native_bin" "42" "compiler linker did not produce a runnable executable"

if [ "$RUN_TARGET" = "linux-x86_64" ]; then
    cat > "$WORK_DIR/tiny_leaf_return.c" <<'EOF'
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 1;
}
EOF

    "${TEST_BIN_DIR}/ncc" --target "$RUN_TARGET" -S -std=c11 -O2 -ffreestanding -ffunction-sections -fdata-sections "$WORK_DIR/tiny_leaf_return.c" -o "$WORK_DIR/tiny_leaf_return.s"
    if grep -q 'pushq %rbp\|subq.*%rsp\|movq %rdi\|movq %rsi\|leave' "$WORK_DIR/tiny_leaf_return.s"; then
        fail "compiler emitted a stack frame or unused parameter spills for a tiny leaf return"
    fi
    assert_file_contains "$WORK_DIR/tiny_leaf_return.s" 'pushq \$1' "compiler did not use compact small-immediate return code"
    assert_file_contains "$WORK_DIR/tiny_leaf_return.s" 'popq %rax' "compiler did not complete compact small-immediate return code"
fi

cat > "$WORK_DIR/flow.c" <<'EOF'
int main(void) {
    int value = 3;
    if (value < 5) {
        value = value + 1;
    } else {
        value = value - 1;
    }
    return value;
}
EOF

compile_and_check_native "$WORK_DIR/flow.c" "$WORK_DIR/flow_native_bin" "4" "compiler linker did not preserve control-flow semantics"

cat > "$WORK_DIR/double_local_arithmetic.c" <<'EOF'
int main(void) {
    double value = 1.5;
    value = value + 2.25;
    return value > 3.74 && value < 3.76 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/double_local_arithmetic.c" "$WORK_DIR/double_local_arithmetic_bin" "0" "compiler did not preserve local binary64 literal, addition, and comparison semantics"

cat > "$WORK_DIR/double_operator_semantics.c" <<'EOF'
int main(void) {
    double product = 6.0 * 1.5;
    double quotient = product / 4.0;
    double negative = -quotient;
    double negative_zero = -0.0;
    return quotient > 2.24 && quotient < 2.26 &&
           negative < -2.24 && negative > -2.26 &&
           negative == -2.25 && negative != -2.0 &&
           -0.0 == 0.0 && 1.0 / negative_zero < 0.0 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/double_operator_semantics.c" "$WORK_DIR/double_operator_semantics_bin" "0" "compiler did not preserve binary64 multiply, divide, unary sign, and equality semantics"

cat > "$WORK_DIR/double_branch_semantics.c" <<'EOF'
static int classify(double value) {
    if (value < 0.0) return 1;
    if (value >= 0.0) return 2;
    return 3;
}

int main(void) {
    double negative_zero = -0.0;
    double nan = 0.0 / 0.0;
    if (negative_zero) return 1;
    return classify(negative_zero) == 2 && classify(nan) == 3 ? 0 : 2;
}
EOF

compile_and_check_native "$WORK_DIR/double_branch_semantics.c" "$WORK_DIR/double_branch_semantics_bin" "0" "compiler did not preserve binary64 branch truthiness and ordered comparison semantics"

cat > "$WORK_DIR/double_scalar_casts.c" <<'EOF'
int main(void) {
    int positive = 7;
    long long negative = -3;
    double positive_value = (double)positive;
    double negative_value = (double)negative;
    int truncated_positive = (int)3.75;
    long long truncated_negative = (long long)-8.5;
    return positive_value == 7.0 && negative_value == -3.0 &&
           truncated_positive == 3 && truncated_negative == -8 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/double_scalar_casts.c" "$WORK_DIR/double_scalar_casts_bin" "0" "compiler did not preserve signed integer and binary64 cast semantics"

cat > "$WORK_DIR/double_call_abi.c" <<'EOF'
static double affine(double value, int scale, double offset) {
    return value * (double)scale + offset;
}

static double twice(double value) {
    return value + value;
}

int main(void) {
    double result = twice(affine(1.25, 2, 0.5));
    return result == 6.0 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/double_call_abi.c" "$WORK_DIR/double_call_abi_bin" "0" "compiler did not preserve mixed SysV binary64 parameters and return values"

if [ "$RUN_TARGET" = "linux-x86_64" ]; then
    cat > "$WORK_DIR/double_abi_external_caller.c" <<'EOF'
extern double abi_affine(double value, int scale, double offset);

int main(void) {
    double result = abi_affine(1.25, 2, 0.5);
    return result == 3.0 ? 0 : 1;
}
EOF

    cat > "$WORK_DIR/double_abi_external_callee.s" <<'EOF'
.text
.globl abi_affine
abi_affine:
    cvtsi2sdl %edi, %xmm2
    mulsd %xmm2, %xmm0
    addsd %xmm1, %xmm0
    ret
EOF

    cc -c "$WORK_DIR/double_abi_external_callee.s" -o "$WORK_DIR/double_abi_external_callee.o"
    "${TEST_BIN_DIR}/ncc" --target "$RUN_TARGET" "$WORK_DIR/double_abi_external_caller.c" "$WORK_DIR/double_abi_external_callee.o" -o "$WORK_DIR/double_abi_external_caller_bin"
    actual_status=0
    "$WORK_DIR/double_abi_external_caller_bin" || actual_status=$?
    assert_text_equals "$actual_status" "0" "compiler caller did not use the SysV binary64 argument and return ABI"

    cat > "$WORK_DIR/double_abi_ncc_callee.c" <<'EOF'
double ncc_affine(double value, int scale, double offset) {
    return value * (double)scale + offset;
}
EOF

    cat > "$WORK_DIR/double_abi_host_caller.c" <<'EOF'
extern double ncc_affine(double value, int scale, double offset);

int main(void) {
    double result = ncc_affine(1.25, 2, 0.5);
    return result == 3.0 ? 0 : 1;
}
EOF

    "${TEST_BIN_DIR}/ncc" --target "$RUN_TARGET" -c "$WORK_DIR/double_abi_ncc_callee.c" -o "$WORK_DIR/double_abi_ncc_callee.o"
    cc "$WORK_DIR/double_abi_host_caller.c" "$WORK_DIR/double_abi_ncc_callee.o" -o "$WORK_DIR/double_abi_host_caller_bin"
    actual_status=0
    "$WORK_DIR/double_abi_host_caller_bin" || actual_status=$?
    assert_text_equals "$actual_status" "0" "compiler callee did not use the SysV binary64 argument and return ABI"
fi

cat > "$WORK_DIR/double_shared_math.c" <<'EOF'
#include "math.h"

int main(void) {
    double root = math_sqrt(9.0);
    double power = math_pow(2.0, 5.0);
    double sine = math_sin(MATH_PI / 2.0);
    if (!(root > 2.999 && root < 3.001)) return 1;
    if (!(power > 31.999 && power < 32.001)) return 2;
    if (!(sine > 0.999 && sine < 1.001)) return 3;
    return 0;
}
EOF

"${TEST_BIN_DIR}/ncc" --target "$RUN_TARGET" -Isrc/shared \
    "$WORK_DIR/double_shared_math.c" src/shared/math.c -o "$WORK_DIR/double_shared_math_bin"
actual_status=0
"$WORK_DIR/double_shared_math_bin" || actual_status=$?
assert_text_equals "$actual_status" "0" "compiler did not execute the shared binary64 math implementation correctly"

cat > "$WORK_DIR/backend_expr.c" <<'EOF'
int main(void) {
    char buffer[16];
    buffer[0] = "ok"[0];
    buffer[1] = "ok"[1];
    buffer[2] = '\0';
    return buffer[1] == 'k' ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/backend_expr.c" "$WORK_DIR/backend_expr_bin" "0" "compiler backend did not preserve string/index expression semantics"

cat > "$WORK_DIR/call_result_member.c" <<'EOF'
typedef struct {
    char *first;
    char *second;
} Pair;

Pair *select_pair(Pair *pair) {
    return pair;
}

int main(void) {
    Pair pair;
    char first = 'a';
    char second = 'b';
    pair.first = &first;
    pair.second = &second;
    return select_pair(&pair)->second == &second ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/call_result_member.c" "$WORK_DIR/call_result_member_bin" "0" "compiler lost the return type before member access on a function-call result"

cat > "$WORK_DIR/call_result_deref.c" <<'EOF'
char *skip_one(char *text) {
    return text + 1;
}

int main(void) {
    char text[] = " [";
    return *skip_one(text) == '[' ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/call_result_deref.c" "$WORK_DIR/call_result_deref_bin" "0" "compiler lost the return type before dereferencing a function-call result"

cat > "$WORK_DIR/prefix_pointer_deref_width.c" <<'EOF'
int main(void) {
    unsigned char source[16];
    unsigned char destination[16];
    unsigned char *source_cursor = source + 8;
    unsigned char *destination_cursor = destination + 8;
    int i;

    for (i = 0; i < 16; i += 1) {
        source[i] = (unsigned char)(i + 1);
        destination[i] = 0xa5U;
    }
    *--destination_cursor = *--source_cursor;
    return destination[7] == 8U && destination[8] == 0xa5U && destination[14] == 0xa5U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/prefix_pointer_deref_width.c" "$WORK_DIR/prefix_pointer_deref_width_bin" "0" "compiler used the wrong width when dereferencing a prefix-decremented byte pointer"

cat > "$WORK_DIR/add_sub_chain.c" <<'EOF'
static int base64_like(int ch) {
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    return -1;
}

int main(void) {
    return base64_like('a') == 26 && base64_like('z') == 51 && base64_like('0') == 52 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/add_sub_chain.c" "$WORK_DIR/add_sub_chain_bin" "0" "compiler IR optimizer changed left-associative add/sub semantics"

cat > "$WORK_DIR/long_expr.c" <<'EOF'
int main(void) {
    return 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1 + 1;
}
EOF

compile_and_check_native "$WORK_DIR/long_expr.c" "$WORK_DIR/long_expr_bin" "64" "compiler failed on a repository-scale long expression"

cat > "$WORK_DIR/inline_shifted_ranges.c" <<'EOF'
typedef struct Node {
    struct Node *next;
    int value;
} Node;

static int adjust(int value) {
    int result = value + 3;
    if (result > 5) result -= 1;
    return result;
}

static int sum_nodes(Node *node) {
    int total = 0;
    while (node != 0) {
        total += node->value;
        node = node->next;
    }
    return total;
}

int main(void) {
    Node tail = {0, 4};
    Node head = {&tail, 2};
    int adjusted = adjust(4);
    int total = sum_nodes(&head);
    return adjusted == 6 && total == 6 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/inline_shifted_ranges.c" "$WORK_DIR/inline_shifted_ranges_bin" "0" "compiler used stale function-body ranges after an exact-call inline expansion"

cat > "$WORK_DIR/inline_local_keyword.c" <<'EOF'
static int copy_first(const char *text) {
    char local[4];
    char *dynamic = 0;
    local[0] = text[0];
    local[1] = '\0';
    if (dynamic != 0) return 2;
    return local[0] == 'x' ? 1 : 0;
}

int main(void) {
    int result = copy_first("x");
    return result == 1 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/inline_local_keyword.c" "$WORK_DIR/inline_local_keyword_bin" "0" "compiler rewrote the local keyword in an inlined IR declaration"

cat > "$WORK_DIR/inline_member_name.c" <<'EOF'
struct Signature {
    unsigned int target_tag;
    unsigned int target_index;
};

static int matches(const struct Signature *signature, unsigned int target_tag, unsigned int target_index) {
    if (signature->target_tag != target_tag || signature->target_index != target_index) return 0;
    return 1;
}

int main(void) {
    struct Signature signature;
    int result;
    signature.target_tag = 14U;
    signature.target_index = 3U;
    result = matches(&signature, 14U, 3U);
    return result == 1 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/inline_member_name.c" "$WORK_DIR/inline_member_name_bin" "0" "compiler rewrote a member name that matched an inline parameter"

cat > "$WORK_DIR/inline_postfix_parameter.c" <<'EOF'
static unsigned int hash_text(const char *text) {
    unsigned int hash = 2166136261U;
    while (text != 0 && *text != '\0') {
        hash ^= (unsigned int)(unsigned char)*text++;
        hash *= 16777619U;
    }
    return hash;
}

static int caller_pointer_is_preserved(const char *name) {
    unsigned int hash = hash_text(name);
    return hash != 0U && name[0] == 'm';
}

int main(void) {
    return caller_pointer_is_preserved("main") ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/inline_postfix_parameter.c" "$WORK_DIR/inline_postfix_parameter_bin" "0" "compiler let an inlined postfix increment mutate the caller argument"

cat > "$WORK_DIR/pointer_to_array_parameter.c" <<'EOF'
static void set_row(char (*rows)[32], int index, char value) {
    rows[index][0] = value;
    rows[index][31] = (char)(value + 1);
}

int main(void) {
    char rows[2][32];
    rows[0][0] = 0;
    set_row(rows, 1, 41);
    return rows[0][0] == 0 && rows[1][0] == 41 && rows[1][31] == 42 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/pointer_to_array_parameter.c" "$WORK_DIR/pointer_to_array_parameter_bin" "0" "compiler lost pointer-to-array parameter stride metadata"

cat > "$WORK_DIR/inline_long_argument.c" <<'EOF'
static unsigned int rotate_left(unsigned int value, unsigned int count) {
    return (value << count) | (value >> (32U - count));
}

int main(void) {
    unsigned int words[20] = {0U};
    unsigned int round = 16U;
    words[round - 3U] = 1U;
    words[round - 8U] = 2U;
    words[round - 14U] = 4U;
    words[round - 16U] = 8U;
    words[round] = rotate_left(words[round - 3U] ^ words[round - 8U] ^
                               words[round - 14U] ^ words[round - 16U], 1U);
    return words[round] == 30U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/inline_long_argument.c" "$WORK_DIR/inline_long_argument_bin" "0" "compiler truncated a long argument during expression-only inlining"

cat > "$WORK_DIR/grouped_rotate_xor.c" <<'EOF'
#define ROTR32(value, count) (((value) >> (count)) | ((value) << (32U - (count))))

int main(void) {
    unsigned int value = 0x12345678U;
    unsigned int result = value ^ ROTR32(value, 6U);
    return result == 0xf27c8721U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/grouped_rotate_xor.c" "$WORK_DIR/grouped_rotate_xor_bin" "0" "compiler consumed the caller group delimiter while recognizing a rotate"

cat > "$WORK_DIR/large_relational_fallback.c" <<'EOF'
static int parse(const char *text, unsigned long long *value_out) {
    *value_out = text[0] == 'x' ? 0x100000000ULL : 7ULL;
    return 0;
}

int main(void) {
    unsigned long long value = 0ULL;
    int argi = 1;
    int argc = 5;

    if (argi + 4 != argc || parse("x", &value) != 0 || value > 0xffffffffULL) return 0;
    return 1;
}
EOF

compile_and_check_native "$WORK_DIR/large_relational_fallback.c" "$WORK_DIR/large_relational_fallback_bin" "0" "compiler advanced past a large relational immediate before register fallback"

cat > "$WORK_DIR/ternary_array_bound.c" <<'EOF'
#define LEFT_SIZE (16U * 8U)
#define RIGHT_SIZE (32U * 4U)

int main(void) {
    char buffer[LEFT_SIZE < RIGHT_SIZE ? LEFT_SIZE : RIGHT_SIZE];
    buffer[127] = 9;
    return sizeof(buffer) == 128U && buffer[127] == 9 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/ternary_array_bound.c" "$WORK_DIR/ternary_array_bound_bin" "0" "compiler misparsed relational or ternary operators in an array bound"

{
    symbol_index=0
    while [ "$symbol_index" -lt 1100 ]; do
        printf 'static int generated_symbol_%s;\n' "$symbol_index"
        symbol_index=$((symbol_index + 1))
    done
    printf 'int main(void) { return generated_symbol_1099; }\n'
} > "$WORK_DIR/large_symbol_table.c"

compile_and_check_native "$WORK_DIR/large_symbol_table.c" "$WORK_DIR/large_symbol_table_bin" "0" "compiler exhausted its semantic table on a large translation unit"

cat > "$WORK_DIR/recursive_inline_candidate.c" <<'EOF'
static unsigned int recursive_sum(unsigned int value) {
    unsigned int tail;
    if (value == 0U) return 0U;
    tail = recursive_sum(value - 1U);
    return value + tail;
}

static unsigned int is_odd(unsigned int value);

static unsigned int is_even(unsigned int value) {
    unsigned int result;
    if (value == 0U) return 1U;
    result = is_odd(value - 1U);
    return result;
}

static unsigned int is_odd(unsigned int value) {
    unsigned int result;
    if (value == 0U) return 0U;
    result = is_even(value - 1U);
    return result;
}

int main(void) {
    return recursive_sum(10U) == 55U && is_even(10U) && is_odd(9U) ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/recursive_inline_candidate.c" "$WORK_DIR/recursive_inline_candidate_bin" "0" "compiler repeatedly expanded a recursive static exact-call inline candidate"

cat > "$WORK_DIR/static_local_string_array.c" <<'EOF'
int main(void) {
    static const char punctuation[] = "{}[]()#";
    return punctuation[2] == '[' ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/static_local_string_array.c" "$WORK_DIR/static_local_string_array_bin" "0" "compiler failed on a function-local static string array initializer"

cat > "$WORK_DIR/local_string_row_array.c" <<'EOF'
static int same(const char *left, const char *right) {
    unsigned int i = 0;
    while (left[i] != '\0' || right[i] != '\0') {
        if (left[i] != right[i]) return 0;
        i += 1U;
    }
    return 1;
}

static int match(const char *text, const char rows[][4], unsigned long count) {
    unsigned long i;

    for (i = 0; i < count; ++i) {
        if (same(text, rows[i])) return 1;
    }
    return 0;
}

int main(void) {
    const char rows[][4] = {"==", "!="};

    if (!match("==", rows, sizeof(rows) / sizeof(rows[0]))) return 1;
    if (!match("!=", rows, sizeof(rows) / sizeof(rows[0]))) return 2;
    return match("+", rows, sizeof(rows) / sizeof(rows[0])) ? 3 : 0;
}
EOF

compile_and_check_native "$WORK_DIR/local_string_row_array.c" "$WORK_DIR/local_string_row_array_bin" "0" "compiler stored string pointers instead of inline rows for a local char[][N] initializer"

cat > "$WORK_DIR/nested_array_row_decay.c" <<'EOF'
static unsigned char pick(const unsigned char table[2][4][4], int outer, int row, int column) {
    const unsigned char *entry = table[outer][row];
    return entry[column];
}

int main(void) {
    static const unsigned char table[2][4][4] = {
        {
            { 1U, 2U, 3U, 4U },
            { 5U, 6U, 7U, 8U },
            { 9U, 10U, 11U, 12U },
            { 13U, 14U, 15U, 16U }
        },
        {
            { 17U, 18U, 19U, 20U },
            { 21U, 22U, 23U, 24U },
            { 25U, 26U, 27U, 28U },
            { 29U, 30U, 31U, 32U }
        }
    };

    return pick(table, 1, 2, 3) == 28U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/nested_array_row_decay.c" "$WORK_DIR/nested_array_row_decay_bin" "0" "compiler loaded nested array rows instead of preserving row address decay"

cat > "$WORK_DIR/sizeof_string_array_bound.c" <<'EOF'
int main(void) {
    static const char context[] = "TLS 1.3, server CertificateVerify";
    unsigned char storage[64U + sizeof(context) + 32U];
    unsigned char guard = 7;

    storage[64U + sizeof(context) + 31U] = 9U;
    return guard == 7U && storage[64U + sizeof(context) + 31U] == 9U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/sizeof_string_array_bound.c" "$WORK_DIR/sizeof_string_array_bound_bin" "0" "compiler dropped sizeof(string array) from an array bound"

cat > "$WORK_DIR/u64_constant_compare.c" <<'EOF'
static const unsigned long long expected = 0x1122334455667788ULL;

int main(void) {
    return expected == 0x1122334455667788ULL ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/u64_constant_compare.c" "$WORK_DIR/u64_constant_compare_bin" "0" "compiler miscompiled a 64-bit immediate constant on x86_64"

cat > "$WORK_DIR/u64_global_array.c" <<'EOF'
static const unsigned long long values[2] = {
    0x1122334455667788ULL,
    0x99aabbccddeeff00ULL
};

int main(void) {
    return (values[0] == 0x1122334455667788ULL &&
            values[1] == 0x99aabbccddeeff00ULL) ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/u64_global_array.c" "$WORK_DIR/u64_global_array_bin" "0" "compiler mis-sized a global 64-bit array on x86_64"

cat > "$WORK_DIR/u64_unsigned_shift.c" <<'EOF'
int main(void) {
    volatile unsigned long long x = 0x8000000000000000ULL;
    volatile unsigned long long y = x >> 8;
    return y == 0x0080000000000000ULL ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/u64_unsigned_shift.c" "$WORK_DIR/u64_unsigned_shift_bin" "0" "compiler miscompiled an unsigned 64-bit right shift on x86_64"

cat > "$WORK_DIR/constant_shift_immediate.c" <<'EOF'
unsigned long rotate_like(unsigned long value) {
    return (value << 7U) | (value >> (64U - 7U));
}

int main(void) {
    return rotate_like(1UL) == 128UL ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/constant_shift_immediate.c" "$WORK_DIR/constant_shift_immediate_bin" "0" "compiler miscompiled constant shift counts"
if [ "$RUN_TARGET" = "linux-x86_64" ]; then
    "${TEST_BIN_DIR}/ncc" --target "$RUN_TARGET" -S -std=c11 -O2 -ffreestanding -ffunction-sections -fdata-sections "$WORK_DIR/constant_shift_immediate.c" -o "$WORK_DIR/constant_shift_immediate.s"
    assert_file_contains "$WORK_DIR/constant_shift_immediate.s" 'salq \$7' "compiler did not lower a constant left shift to an immediate shift"
    assert_file_contains "$WORK_DIR/constant_shift_immediate.s" 'shrq \$57' "compiler did not fold a constant expression right-shift count"
    if grep -q '%cl' "$WORK_DIR/constant_shift_immediate.s"; then
        fail "compiler used a variable shift count for constant shifts"
    fi
fi

cat > "$WORK_DIR/cached_parenthesized_assignment.c" <<'EOF'
static unsigned long mix(unsigned long seed, unsigned long *out) {
    unsigned long a = seed;
    unsigned long b = 3UL;
    unsigned long c = 5UL;
    unsigned long d = 7UL;
    unsigned long i;

    for (i = 0; i < 12UL; ++i) {
        (a) += ((b) & (c)) | ((~(b)) & (d));
        (a) = b + ((a << 7U) | (a >> (64U - 7U)));
        (b) += a ^ c ^ d;
        (c) += b | d;
        (d) += c & a;
    }
    *out = a + b + c + d;
    return *out;
}

int main(void) {
    unsigned long value = 0UL;

    return mix(11UL, &value) == value && value != 0UL ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/cached_parenthesized_assignment.c" "$WORK_DIR/cached_parenthesized_assignment_bin" "0" "compiler mishandled parenthesized assignments to cached locals"

cat > "$WORK_DIR/u64_unsigned_division_guard.c" <<'EOF'
static unsigned long long parse_guard_limit(unsigned long long digit) {
    return (18446744073709551615ULL - digit) / 10ULL;
}

int main(void) {
    return parse_guard_limit(1ULL) == 1844674407370955161ULL ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/u64_unsigned_division_guard.c" "$WORK_DIR/u64_unsigned_division_guard_bin" "0" "compiler emitted signed division for an unsigned overflow guard"

cat > "$WORK_DIR/sizeof_scalar_specifiers.c" <<'EOF'
int main(void) {
    return sizeof(unsigned short) == 2U &&
           sizeof(unsigned char) == 1U &&
           sizeof(unsigned int) == 4U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/sizeof_scalar_specifiers.c" "$WORK_DIR/sizeof_scalar_specifiers_bin" "0" "compiler mis-sized multi-token scalar sizeof type names"

cat > "$WORK_DIR/unsigned_char_cast_load.c" <<'EOF'
int main(void) {
    const char png_sig[] = "\211PNG";

    return (unsigned char)png_sig[0] == 0x89U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/unsigned_char_cast_load.c" "$WORK_DIR/unsigned_char_cast_load_bin" "0" "compiler failed to zero-extend an explicit unsigned char cast from memory"

cat > "$WORK_DIR/static_unsigned_char_string_array.c" <<'EOF'
int main(void) {
    static const unsigned char text[] = "urn:c2pa:newos-dev";

    return sizeof(text) == 19U && text[18] == 0U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/static_unsigned_char_string_array.c" "$WORK_DIR/static_unsigned_char_string_array_bin" "0" "compiler mis-sized a local static unsigned char string array initializer"

cat > "$WORK_DIR/sizeof_unsized_braced_array.c" <<'EOF'
static const unsigned char data[] = {1U, 2U, 3U, 4U, 5U};

int main(void) {
    unsigned char copy[sizeof(data)];

    copy[4] = data[4];
    return sizeof(data) == 5U && sizeof(copy) == 5U && copy[4] == 5U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/sizeof_unsized_braced_array.c" "$WORK_DIR/sizeof_unsized_braced_array_bin" "0" "compiler mis-sized sizeof of an unsized braced global array in a later local array bound"

cat > "$WORK_DIR/struct_pointer_to_array_field.c" <<'EOF'
struct List {
    unsigned char (*oids)[20];
    unsigned long count;
};

int main(void) {
    unsigned char data[2][20];
    struct List list;

    data[1][0] = 42U;
    list.oids = data;
    list.count = 2U;
    return sizeof(struct List) == 16U && list.count == 2U && list.oids[1][0] == 42U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/struct_pointer_to_array_field.c" "$WORK_DIR/struct_pointer_to_array_field_bin" "0" "compiler mishandled a struct field declared as pointer to array"

cat > "$WORK_DIR/compound_literal_struct_argument.c" <<'EOF'
typedef struct {
    const unsigned char *text;
    unsigned long size;
} Ref;

typedef struct {
    unsigned int seen;
} State;

static void visit(Ref parent, State *state) {
    if (parent.text == 0 && parent.size == 0U) {
        state->seen += 1U;
    }
}

static void wrapper(State *state) {
    visit((Ref){0, 0U}, state);
}

int main(void) {
    State state;

    state.seen = 0U;
    wrapper(&state);
    return state.seen == 1U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/compound_literal_struct_argument.c" "$WORK_DIR/compound_literal_struct_argument_bin" "0" "compiler failed to allocate and initialize a struct compound literal argument"

cat > "$WORK_DIR/shadowed_cached_local.c" <<'EOF'
static unsigned int count_kept(int use_inner) {
    unsigned int kept_count = 0U;
    unsigned int index;

    if (use_inner) {
        unsigned int kept_count = 0U;

        for (index = 0U; index < 3U; ++index) {
            kept_count += 10U;
        }
        return kept_count;
    }
    for (index = 0U; index < 5U; ++index) {
        if (index != 2U) {
            kept_count += 1U;
        }
    }
    return kept_count;
}

int main(void) {
    return count_kept(0) == 4U && count_kept(1) == 30U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/shadowed_cached_local.c" "$WORK_DIR/shadowed_cached_local_bin" "0" "compiler cached a shadowed local across scopes"

cat > "$WORK_DIR/shadowed_cached_parameter.c" <<'EOF'
static unsigned int pick_token(unsigned int match_index, unsigned int use_inner) {
    unsigned int score = match_index + 1U;
    unsigned int token = score + match_index;

    if (use_inner) {
        unsigned int match_index = 7U;

        token += match_index;
    }
    return token + score + match_index;
}

int main(void) {
    return pick_token(3U, 0U) == 14U && pick_token(3U, 1U) == 21U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/shadowed_cached_parameter.c" "$WORK_DIR/shadowed_cached_parameter_bin" "0" "compiler cached a parameter shadowed by a local"

cat > "$WORK_DIR/u32_mul_shift_wrap.c" <<'EOF'
static unsigned int hash32(unsigned int value) {
    return (value * 2246822519U) >> 16U;
}

int main(void) {
    return hash32(0x123456U) == 0x1eb4U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/u32_mul_shift_wrap.c" "$WORK_DIR/u32_mul_shift_wrap_bin" "0" "compiler failed to wrap unsigned int multiplication before a shift"

cat > "$WORK_DIR/signed_int_return_compare.c" <<'EOF'
static int cmp_path(const char *left, const char *right) {
    while (*left != '\0' && *left == *right) {
        left += 1;
        right += 1;
    }
    return (unsigned char)*left - (unsigned char)*right;
}

static int is_less(int (*compare)(const char *, const char *), const char *left, const char *right) {
    return compare(left, right) < 0;
}

int main(void) {
    return is_less(cmp_path, "delete.txt", "keep.txt") ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/signed_int_return_compare.c" "$WORK_DIR/signed_int_return_compare_bin" "0" "compiler failed to sign-extend a negative int function return"

cat > "$WORK_DIR/struct_member_function_pointer_call.c" <<'EOF'
struct Reader {
    int (*read_fn)(void *context, unsigned char *buffer, unsigned long capacity, unsigned long *size_out);
    void *context;
};

static int fill(void *context, unsigned char *buffer, unsigned long capacity, unsigned long *size_out) {
    (void)context;
    if (capacity < 1U) return -1;
    buffer[0] = 66U;
    *size_out = 1U;
    return 0;
}

int main(void) {
    struct Reader reader;
    unsigned char buffer[4];
    unsigned long size = 0U;

    reader.read_fn = fill;
    reader.context = 0;
    return reader.read_fn(reader.context, buffer, sizeof(buffer), &size) == 0 &&
           size == 1U && buffer[0] == 66U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/struct_member_function_pointer_call.c" "$WORK_DIR/struct_member_function_pointer_call_bin" "0" "compiler failed to emit an indirect call through a struct member function pointer"

cat > "$WORK_DIR/shifted_array_bound.c" <<'EOF'
#define FAST_BITS 12U
#define FAST_SIZE (1U << FAST_BITS)

struct Table {
    unsigned short fast_symbol[FAST_SIZE];
    unsigned char fast_bits[FAST_SIZE];
};

int main(void) {
    return sizeof(struct Table) == (4096U * 2U + 4096U) ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/shifted_array_bound.c" "$WORK_DIR/shifted_array_bound_bin" "0" "compiler mis-sized array bounds containing shifts"

cat > "$WORK_DIR/compound_shift_assignment.c" <<'EOF'
typedef struct {
    unsigned int bit_buffer;
} Writer;

int main(void) {
    Writer writer;
    writer.bit_buffer = 0x8000U;
    writer.bit_buffer >>= 8U;
    return writer.bit_buffer == 0x80U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/compound_shift_assignment.c" "$WORK_DIR/compound_shift_assignment_bin" "0" "compiler IR optimizer split >>= into relational tokens"

cat > "$WORK_DIR/function_sections_gc.c" <<'EOF'
extern int missing_external(void);

static int unused_function(void) {
    return missing_external();
}

int main(void) {
    return 0;
}
EOF

if [ -n "$RUN_TARGET" ]; then
    "${TEST_BIN_DIR}/ncc" --target "$RUN_TARGET" -ffunction-sections -fdata-sections -Wl,--gc-sections "$WORK_DIR/function_sections_gc.c" -o "$WORK_DIR/function_sections_gc_bin"
    "$WORK_DIR/function_sections_gc_bin"
fi

cat > "$WORK_DIR/long_logic_label.c" <<'EOF'
static int compression_zstd_parse_compressed_literals_header(int first, int second) {
    return first ? (second && first ? 7 : 9) : 11;
}

int main(void) {
    return compression_zstd_parse_compressed_literals_header(1, 1) == 7 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/long_logic_label.c" "$WORK_DIR/long_logic_label_bin" "0" "compiler truncated long generated logic labels"

cat > "$WORK_DIR/global_struct_copy_from_pointer.c" <<'EOF'
typedef struct {
    int entering;
    long number;
    long args[6];
    long result;
} PlatformSyscallEvent;

static PlatformSyscallEvent pending_event;

static void remember_event(const PlatformSyscallEvent *event) {
    pending_event = *event;
}

int main(void) {
    PlatformSyscallEvent event;

    event.entering = 1;
    event.number = 42;
    event.args[0] = 7;
    event.result = -3;
    remember_event(&event);
    return pending_event.number == 42 && pending_event.args[0] == 7 && pending_event.result == -3 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/global_struct_copy_from_pointer.c" "$WORK_DIR/global_struct_copy_from_pointer_bin" "0" "compiler failed to copy into a global aggregate assignment target"

cat > "$WORK_DIR/large_struct_argument.c" <<'EOF'
typedef struct {
    unsigned int digits[8];
    unsigned int length;
    int sign;
} Big;

typedef struct {
    Big mantissa;
    int scale;
} Value;

static int check_value(Value value) {
    if (value.mantissa.digits[0] != 1U) return 1;
    if (value.mantissa.digits[1] != 2U) return 2;
    if (value.mantissa.length != 2U) return 3;
    if (value.scale != 3) return 4;
    return 0;
}

int main(void) {
    Value value;

    value.mantissa.digits[0] = 1U;
    value.mantissa.digits[1] = 2U;
    value.mantissa.length = 2U;
    value.mantissa.sign = 0;
    value.scale = 3;
    return check_value(value);
}
EOF

compile_and_check_native "$WORK_DIR/large_struct_argument.c" "$WORK_DIR/large_struct_argument_bin" "0" "compiler failed to pass a large aggregate argument by value"

cat > "$WORK_DIR/large_object_return_argument.c" <<'EOF'
typedef struct {
    unsigned int digits[2048];
    unsigned int length;
    int sign;
} Large;

static Large make_large(unsigned int first) {
    Large value;

    value.digits[0] = first;
    value.digits[2047] = 99U;
    value.length = 2048U;
    value.sign = 0;
    return value;
}

static int check_large(Large value) {
    if (value.digits[0] != 7U) return 1;
    if (value.digits[2047] != 99U) return 2;
    if (value.length != 2048U) return 3;
    return value.sign == 0 ? 0 : 4;
}

int main(void) {
    return check_large(make_large(7U));
}
EOF

compile_and_check_native "$WORK_DIR/large_object_return_argument.c" "$WORK_DIR/large_object_return_argument_bin" "0" "compiler used an undersized object-return temporary for a large aggregate argument"

cat > "$WORK_DIR/object_return_to_nested_member.c" <<'EOF'
typedef struct {
    unsigned int digits[2048];
    unsigned int length;
    int sign;
} Big;

typedef struct {
    Big mantissa;
    int scale;
} Value;

typedef struct {
    Value number;
    int type;
} Token;

typedef struct {
    Token token;
    int base;
} Parser;

static Value make_int(unsigned int digit) {
    Value result;

    result.mantissa.length = 0U;
    result.mantissa.sign = 0;
    result.scale = 0;
    if (digit != 0U) {
        result.mantissa.digits[0] = digit;
        result.mantissa.length = 1U;
    }
    return result;
}

static int parse_like(Parser *parser) {
    parser->token.number = make_int(0U);
    if (parser->token.number.mantissa.length != 0U) return 1;
    parser->token.number = make_int(10U);
    if (parser->token.number.mantissa.length != 1U) return 2;
    return parser->token.number.mantissa.digits[0] == 10U ? 0 : 3;
}

int main(void) {
    Parser parser;
    return parse_like(&parser);
}
EOF

compile_and_check_native "$WORK_DIR/object_return_to_nested_member.c" "$WORK_DIR/object_return_to_nested_member_bin" "0" "compiler copied an object-return function symbol instead of assigning into a nested aggregate member"

cat > "$WORK_DIR/object_conditional_return.c" <<'EOF'
typedef struct {
    unsigned int digits[2048];
    unsigned int length;
    int sign;
} Big;

typedef struct {
    Big mantissa;
    int scale;
} Value;

static Value choose(int pick_left, Value left, Value right) {
    return pick_left ? left : right;
}

int main(void) {
    Value left;
    Value right;
    Value result;

    left.mantissa.digits[0] = 3U;
    left.mantissa.length = 1U;
    left.mantissa.sign = 0;
    left.scale = 0;
    right.mantissa.digits[0] = 8U;
    right.mantissa.length = 1U;
    right.mantissa.sign = 0;
    right.scale = 0;
    result = choose(0, left, right);
    if (result.mantissa.digits[0] != 8U) return 1;
    result = choose(1, left, right);
    return result.mantissa.digits[0] == 3U ? 0 : 2;
}
EOF

compile_and_check_native "$WORK_DIR/object_conditional_return.c" "$WORK_DIR/object_conditional_return_bin" "0" "compiler loaded the first aggregate word instead of selecting an object-valued conditional branch"

cat > "$WORK_DIR/block_shadowing.c" <<'EOF'
static int marker;

static int shadowed_pointer(void) {
    int outer_value = 3;
    int *value = &outer_value;

    if (outer_value == 3) {
        int inner_value = 7;
        int *value = &inner_value;
        marker = *value;
    }

    *value = *value + marker;
    return outer_value;
}

int main(void) {
    return shadowed_pointer() == 10 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/block_shadowing.c" "$WORK_DIR/block_shadowing_bin" "0" "compiler did not restore shadowed local bindings after a block scope"

cat > "$WORK_DIR/array_parameter_reassignment.c" <<'EOF'
typedef unsigned char u8;

static int pick_key(const u8 seed[32], const u8 public_key[32]) {
    u8 derived_public_key[32];

    derived_public_key[0] = seed[0] + 1U;
    if (public_key == (const u8 *)0) {
        public_key = derived_public_key;
    }
    return public_key[0] == 8U ? 0 : 1;
}

int main(void) {
    u8 seed[32];

    seed[0] = 7U;
    return pick_key(seed, (const u8 *)0);
}
EOF

compile_and_check_native "$WORK_DIR/array_parameter_reassignment.c" "$WORK_DIR/array_parameter_reassignment_bin" "0" "compiler treated array-parameter pointer reassignment as object copy"

cat > "$WORK_DIR/pointer_to_array_sizeof.c" <<'EOF'
int main(void) {
    char storage[2][16];
    char (*rows)[16];
    char *pointers[16];

    rows = (char (*)[16])storage;
    pointers[0] = storage[0];
    rows[1][0] = 'X';

    if (sizeof(rows[0]) != 16) return 1;
    if (sizeof(pointers[0]) != 8) return 2;
    if (rows[1][0] != 'X') return 3;
    return pointers[0][0] == storage[0][0] ? 0 : 4;
}
EOF

compile_and_check_native "$WORK_DIR/pointer_to_array_sizeof.c" "$WORK_DIR/pointer_to_array_sizeof_bin" "0" "compiler miscompiled sizeof or indexing for a pointer-to-array"

cat > "$WORK_DIR/signed_indexed_array_compare.c" <<'EOF'
int main(void) {
    int values[2];
    unsigned long index = 0U;

    values[0] = -1;
    values[1] = 7;
    return values[index] < 0 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/signed_indexed_array_compare.c" "$WORK_DIR/signed_indexed_array_compare_bin" "0" "compiler treated an indexed signed int array expression as unsigned"

cat > "$WORK_DIR/shared_crypto_api.c" <<'EOF'
#include "crypto/aes128_gcm.h"
#include "crypto/hmac_sha256.h"
#include "crypto/hkdf_sha256.h"
#include "crypto/rsa.h"

int main(void) {
    unsigned char digest[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned char okm[CRYPTO_SHA256_DIGEST_SIZE];
    CryptoAes256GcmContext gcm;
    CryptoRsaPrivateKey rsa_key;

    (void)digest;
    (void)okm;
    return sizeof(gcm) > 0U && sizeof(rsa_key) > 0U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/shared_crypto_api.c" "$WORK_DIR/shared_crypto_api_bin" "0" "shared crypto headers or types were unavailable"

cat > "$WORK_DIR/local_struct_init.c" <<'EOF'
typedef struct {
    int first;
    int second;
} Pair;

int main(void) {
    Pair pair = { 3, 4 };
    return pair.first == 3 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/local_struct_init.c" "$WORK_DIR/local_struct_init_bin" "0" "compiler failed on a function-local aggregate initializer"

cat > "$WORK_DIR/posix_addrinfo_fields.c" <<'EOF'
#include <netdb.h>
#include <sys/socket.h>

int main(void) {
    struct addrinfo hints;

    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    return (hints.ai_flags == AI_PASSIVE &&
            hints.ai_family == AF_INET &&
            hints.ai_socktype == SOCK_STREAM &&
            hints.ai_protocol == IPPROTO_TCP) ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/posix_addrinfo_fields.c" "$WORK_DIR/posix_addrinfo_fields_bin" "0" "compiler mis-lowered host addrinfo field stores"

cat > "$WORK_DIR/null_static_pointer.c" <<'EOF'
static const char *items[] = { NULL, "ok" };

int main(void) {
    return (items[0] == 0 && items[1][0] == 'o') ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/null_static_pointer.c" "$WORK_DIR/null_static_pointer_bin" "0" "compiler failed to lower a NULL pointer global initializer"

cat > "$WORK_DIR/escaped_char_literal.c" <<'EOF'
int main(void) {
    char quote = '\'';
    return quote == '\'' ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/escaped_char_literal.c" "$WORK_DIR/escaped_char_literal_bin" "0" "compiler failed on an escaped single-quote character literal"

cat > "$WORK_DIR/escaped_control_literals.c" <<'EOF'
int main(void) {
    return ('\v' == 11 && '\f' == 12) ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/escaped_control_literals.c" "$WORK_DIR/escaped_control_literals_bin" "0" "compiler miscompiled escaped vertical-tab or form-feed character literals"

cat > "$WORK_DIR/adjacent_strings.c" <<'EOF'
int main(void) {
    const char *text = "hello, " "world";
    return text[7] == 'w' ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/adjacent_strings.c" "$WORK_DIR/adjacent_strings_bin" "0" "compiler failed on adjacent string literal concatenation"

cat > "$WORK_DIR/binary_string_literal.c" <<'EOF'
static const unsigned char *table_ptr(void) {
    return (const unsigned char *)"\x63\x00\xff\x16";
}

static const char table_array[] = "\x05\x00\x07";

int main(void) {
    const unsigned char *table = table_ptr();
    return table[0] == 0x63U && table[1] == 0U && table[2] == 0xffU && table[3] == 0x16U &&
           (unsigned char)table_array[0] == 5U && (unsigned char)table_array[1] == 0U && (unsigned char)table_array[2] == 7U &&
           sizeof(table_array) == 4U ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/binary_string_literal.c" "$WORK_DIR/binary_string_literal_bin" "0" "compiler truncated binary string literals at embedded NUL bytes"

cat > "$WORK_DIR/comment_macro.c" <<'EOF'
#define VALUE 0 /* inline expansion payload */
/*
 * VALUE should remain ordinary comment text here.
 */
int main(void) {
    return VALUE;
}
EOF

compile_and_check_native "$WORK_DIR/comment_macro.c" "$WORK_DIR/comment_macro_bin" "0" "preprocessor expanded a macro inside a block comment"

cat > "$WORK_DIR/multi_arg_call.c" <<'EOF'
int check_args(int number, const char *text) {
    return number == 7 && text[0] == 'o' ? 0 : 1;
}

int main(void) {
    return check_args(7, "ok");
}
EOF

compile_and_check_native "$WORK_DIR/multi_arg_call.c" "$WORK_DIR/multi_arg_call_bin" "0" "compiler failed to pass multiple call arguments correctly"

cat > "$WORK_DIR/logical_or_side_effect.c" <<'EOF'
typedef struct {
    char **argv;
    int index;
} Parser;

static int parse_depth(char *text, long long *out, const char *tool, const char *name) {
    (void)text;
    (void)tool;
    (void)name;
    *out = 3;
    return 0;
}

static int parse_parser(Parser *parser) {
    long long depth = 0;
    if (parse_depth(parser->argv[parser->index + 1], &depth, "find", "mindepth") != 0 || depth < 0) {
        return 1;
    }
    return depth == 3 ? 0 : 2;
}

int main(int argc, char **argv) {
    Parser parser;
    (void)argc;
    parser.argv = argv;
    parser.index = 0;
    return parse_parser(&parser);
}
EOF

compile_and_check_native "$WORK_DIR/logical_or_side_effect.c" "$WORK_DIR/logical_or_side_effect_bin" "0" "compiler IR optimization corrupted a logical-or expression after a pointer-mutating call"

cat > "$WORK_DIR/constant_short_circuit.c" <<'EOF'
static int bump(int *slot) {
    *slot += 1;
    return *slot;
}

int main(void) {
    int value = 4;
    if (((2 * 3) - 6) && bump(&value)) {
        return 1;
    }
    return value == 4 ? 0 : 2;
}
EOF

compile_and_check_native "$WORK_DIR/constant_short_circuit.c" "$WORK_DIR/constant_short_circuit_bin" "0" "compiler constant-folding broke short-circuit behavior for a side-effecting call"


cat > "$WORK_DIR/logical_short_circuit_fold.c" <<'EOF'
static int bump(int *slot) {
    *slot += 1;
    return *slot;
}

int main(void) {
    int value = 4;
    if (0 && bump(&value)) {
        return 1;
    }
    if (1 || bump(&value)) {
        return value == 4 ? 0 : 2;
    }
    return 3;
}
EOF

compile_and_check_native "$WORK_DIR/logical_short_circuit_fold.c" "$WORK_DIR/logical_short_circuit_fold_bin" "0" "compiler short-circuit optimization changed side-effect semantics"

cat > "$WORK_DIR/multi_file_helper.c" <<'EOF'
int helper_value(void) {
    return 41;
}
EOF

cat > "$WORK_DIR/multi_file_main.c" <<'EOF'
int helper_value(void);

int main(void) {
    return helper_value() == 41 ? 0 : 1;
}
EOF

if [ -n "$RUN_TARGET" ]; then
    assert_command_succeeds "${TEST_BIN_DIR}/ncc" --target "$RUN_TARGET" "$WORK_DIR/multi_file_main.c" "$WORK_DIR/multi_file_helper.c" -o "$WORK_DIR/multi_file_bin"
    if "$WORK_DIR/multi_file_bin"; then
        actual_status=0
    else
        actual_status=$?
    fi
    assert_text_equals "$actual_status" "0" "compiler multi-file linker flow did not produce a runnable executable"
fi

cat > "$WORK_DIR/lto_helper.c" <<'EOF'
int lto_helper_value(void) {
    return 17;
}
EOF

cat > "$WORK_DIR/lto_main.c" <<'EOF'
int lto_helper_value(void);

int main(void) {
    int value = lto_helper_value();
    return value == 17 ? 0 : 1;
}
EOF

cat > "$WORK_DIR/lto_static_a.c" <<'EOF'
static int same_name(void) {
    return 11;
}

int lto_left(void) {
    return same_name();
}
EOF

cat > "$WORK_DIR/lto_static_b.c" <<'EOF'
static int same_name(void) {
    return 31;
}

int lto_right(void) {
    return same_name();
}
EOF

cat > "$WORK_DIR/lto_static_main.c" <<'EOF'
int lto_left(void);
int lto_right(void);

int main(void) {
    return lto_left() + lto_right() == 42 ? 0 : 1;
}
EOF

cat > "$WORK_DIR/lto_shadow_static.c" <<'EOF'
static int shadowed = 40;

int lto_shadow_local(void) {
    int shadowed = 2;
    return shadowed;
}

int lto_shadow_file(void) {
    return shadowed;
}
EOF

cat > "$WORK_DIR/lto_shadow_main.c" <<'EOF'
int lto_shadow_local(void);
int lto_shadow_file(void);

int main(void) {
    return lto_shadow_local() + lto_shadow_file() == 42 ? 0 : 1;
}
EOF

if [ -n "$RUN_TARGET" ]; then
    assert_command_succeeds "${TEST_BIN_DIR}/ncc" --target "$RUN_TARGET" -flto "$WORK_DIR/lto_main.c" "$WORK_DIR/lto_helper.c" -o "$WORK_DIR/lto_multi_file_bin"
    if "$WORK_DIR/lto_multi_file_bin"; then
        actual_status=0
    else
        actual_status=$?
    fi
    assert_text_equals "$actual_status" "0" "compiler -flto multi-file flow did not produce a runnable executable"

    assert_command_succeeds "${TEST_BIN_DIR}/ncc" --target "$RUN_TARGET" -flto "$WORK_DIR/lto_static_main.c" "$WORK_DIR/lto_static_a.c" "$WORK_DIR/lto_static_b.c" -o "$WORK_DIR/lto_static_renamed_bin"
    if "$WORK_DIR/lto_static_renamed_bin"; then
        actual_status=0
    else
        actual_status=$?
    fi
    assert_text_equals "$actual_status" "0" "compiler -flto static renaming did not preserve duplicate static function names"

    native_nm="${TEST_BIN_DIR}/nm"
    if [ ! -x "$native_nm" ] && command -v nm >/dev/null 2>&1; then
        native_nm=nm
    fi
    if [ -x "$native_nm" ] || command -v "$native_nm" >/dev/null 2>&1; then
        "$native_nm" "$WORK_DIR/lto_static_renamed_bin" > "$WORK_DIR/lto_static_renamed_nm.out"
        assert_file_contains "$WORK_DIR/lto_static_renamed_nm.out" '__ncc_lto_' "compiler -flto duplicate static helpers fell back instead of merging with renamed internals"
    fi

    assert_command_succeeds "${TEST_BIN_DIR}/ncc" --target "$RUN_TARGET" -flto "$WORK_DIR/lto_shadow_main.c" "$WORK_DIR/lto_shadow_static.c" -o "$WORK_DIR/lto_shadow_bin"
    if "$WORK_DIR/lto_shadow_bin"; then
        actual_status=0
    else
        actual_status=$?
    fi
    assert_text_equals "$actual_status" "0" "compiler -flto did not preserve local shadowing of a static symbol"
fi

cat > "$WORK_DIR/many_arg_call.c" <<'EOF'
int check_many(int a, int b, int c, int d, int e, int f, int g, int h, int i) {
    return a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6 && g == 7 && h == 8 && i == 9 ? 0 : 1;
}

int main(void) {
    return check_many(1, 2, 3, 4, 5, 6, 7, 8, 9);
}
EOF

compile_and_check_native "$WORK_DIR/many_arg_call.c" "$WORK_DIR/many_arg_call_bin" "0" "compiler failed to preserve arguments beyond the register-only calling convention"

cat > "$WORK_DIR/branch_separator_string.c" <<'EOF'
int check_text(int code, const char *text) {
    return code == 1 && text[1] == '-' && text[2] == '>' ? 0 : 1;
}

int main(void) {
    if (check_text(1, " ->") != 0) {
        return 1;
    }
    return 0;
}
EOF

compile_and_check_native "$WORK_DIR/branch_separator_string.c" "$WORK_DIR/branch_separator_string_bin" "0" "compiler confused a quoted ' ->' string with an IR branch separator"

cat > "$WORK_DIR/casted_member_lvalue.c" <<'EOF'
typedef struct {
    unsigned char code;
} Box;

int main(void) {
    static unsigned char storage[16];
    ((Box *)storage)->code = 7;
    return ((Box *)storage)->code == 7 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/casted_member_lvalue.c" "$WORK_DIR/casted_member_lvalue_bin" "0" "compiler failed on a casted pointer member assignment lvalue"

cat > "$WORK_DIR/struct_array_member_decay.c" <<'EOF'
typedef struct {
    unsigned char bytes[8];
} State;

static void fill(unsigned char *dst) {
    dst[0] = 'o';
    dst[1] = 'k';
    dst[2] = '\0';
}

int main(void) {
    State state;
    fill(state.bytes);
    return state.bytes[1] == 'k' ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/struct_array_member_decay.c" "$WORK_DIR/struct_array_member_decay_bin" "0" "compiler failed to decay a struct byte-array member to its address"

cat > "$WORK_DIR/typedef_struct_local_storage.c" <<'EOF'
typedef struct {
    unsigned char bytes[8192];
} State;

static void fill(State *state) {
    int i;
    for (i = 0; i < 8192; i += 1) {
        state->bytes[i] = (unsigned char)i;
    }
}

int main(void) {
    State state;
    fill(&state);
    return state.bytes[8191] == 255 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/typedef_struct_local_storage.c" "$WORK_DIR/typedef_struct_local_storage_bin" "0" "compiler under-allocated stack storage for a typedef-backed local struct"

cat > "$WORK_DIR/global_multidim_array.c" <<'EOF'
static unsigned char grid[256][256];

int main(void) {
    int i;
    for (i = 0; i < 256; i += 1) {
        grid[i][0] = (unsigned char)'A';
    }
    return sizeof(grid) == 65536U && grid[255][0] == (unsigned char)'A' ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/global_multidim_array.c" "$WORK_DIR/global_multidim_array_bin" "0" "compiler under-allocated a multidimensional global array"

cat > "$WORK_DIR/typedef_struct_copy_assignment.c" <<'EOF'
typedef struct {
    unsigned char bytes[128];
} State;

static void fill(State *state) {
    int i;
    for (i = 0; i < 128; i += 1) {
        state->bytes[i] = (unsigned char)i;
    }
}

static int check(const State *state) {
    State copy;
    copy = *state;
    return copy.bytes[64] == 64 && copy.bytes[127] == 127;
}

int main(void) {
    State state;
    fill(&state);
    return check(&state) ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/typedef_struct_copy_assignment.c" "$WORK_DIR/typedef_struct_copy_assignment_bin" "0" "compiler failed to copy a typedef-backed struct by value"

cat > "$WORK_DIR/struct_copy_tail.c" <<'EOF'
typedef struct {
    unsigned int word[8];
} WideInt;

typedef struct {
    WideInt x;
    WideInt y;
    WideInt z;
    int infinity;
} Jacobian;

static void fill_jacobian(Jacobian *point, unsigned int seed) {
    int i;
    for (i = 0; i < 8; i += 1) {
        point->x.word[i] = seed + (unsigned int)i;
        point->y.word[i] = seed + 20U + (unsigned int)i;
        point->z.word[i] = seed + 40U + (unsigned int)i;
    }
    point->infinity = (int)(seed + 60U);
}

static int check_jacobian(const Jacobian *point, unsigned int seed) {
    return point->x.word[7] == seed + 7U &&
           point->y.word[7] == seed + 27U &&
           point->z.word[7] == seed + 47U &&
           point->infinity == (int)(seed + 60U);
}

int main(void) {
    Jacobian result;
    Jacobian next;
    int guard = 77;

    fill_jacobian(&result, 1U);
    fill_jacobian(&next, 9U);
    result = next;
    return check_jacobian(&result, 9U) && guard == 77 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/struct_copy_tail.c" "$WORK_DIR/struct_copy_tail_bin" "0" "compiler aggregate assignment overcopied a non-8-byte-sized struct"

cat > "$WORK_DIR/loop_continue.c" <<'EOF'
int main(void) {
    int total = 0;
    int i;

    for (i = 0; i < 4; i += 1) {
        if (i == 1 || i == 2) {
            continue;
        }
        total += i;
    }

    return total == 3 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/loop_continue.c" "$WORK_DIR/loop_continue_bin" "0" "compiler miscompiled continue control flow in a for-loop"

cat > "$WORK_DIR/prefix_increment_width.c" <<'EOF'
static void keep_on_stack(unsigned int *value) {
    if (*value == 123U) {
        *value = 456U;
    }
}

unsigned long long poison_stack(void) {
    unsigned long long words[4];

    words[0] = 0xffffffffffffffffULL;
    words[1] = 0xffffffffffffffffULL;
    words[2] = 0xffffffffffffffffULL;
    words[3] = 0xffffffffffffffffULL;
    return words[0];
}

static int increment_local(void) {
    unsigned int value = 0U;

    keep_on_stack(&value);
    return ++value == 1U;
}

int main(void) {
    unsigned long long poison = poison_stack();
    return poison != 0ULL && increment_local() ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/prefix_increment_width.c" "$WORK_DIR/prefix_increment_width_bin" "0" "compiler used the wrong storage width for prefix increment on an unsigned int local"

cat > "$WORK_DIR/second_pipeline_command.c" <<'EOF'
#include <string.h>
#include <stddef.h>

typedef struct {
    char *argv[64 + 1];
    int argc;
    char *input_path;
    char *output_path;
    int output_append;
    int no_expand[64];
} ShCommand;

typedef struct {
    ShCommand commands[8];
    size_t count;
} ShPipeline;

int main(void) {
    ShPipeline pipeline;
    ShCommand *current;

    memset(&pipeline, 0, sizeof(pipeline));
    pipeline.count = 1;
    current = &pipeline.commands[pipeline.count++];
    current->argv[0] = "cat";
    current->argc = 1;

    return (pipeline.count == 2 &&
            pipeline.commands[1].argc == 1 &&
            strcmp(pipeline.commands[1].argv[0], "cat") == 0) ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/second_pipeline_command.c" "$WORK_DIR/second_pipeline_command_bin" "0" "compiler miscompiled second-element access in a struct array"

cat > "$WORK_DIR/typedef_struct_return_assignment.c" <<'EOF'
typedef struct {
    unsigned char bytes[2];
} Buffer;

static Buffer make_buffer(void) {
    Buffer buffer = { { 3, 4 } };
    return buffer;
}

int main(void) {
    Buffer value;
    value = make_buffer();
    return value.bytes[0] == 3 && value.bytes[1] == 4 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/typedef_struct_return_assignment.c" "$WORK_DIR/typedef_struct_return_assignment_bin" "0" "compiler failed to assign a typedef-backed struct returned from a function"

cat > "$WORK_DIR/char_pointer_deref.c" <<'EOF'
int main(void) {
    const char *text = "-c";
    return (*text == '-' && *(text + 1) == 'c') ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/char_pointer_deref.c" "$WORK_DIR/char_pointer_deref_bin" "0" "compiler loaded a full word instead of a byte for char-pointer dereference"

cat > "$WORK_DIR/char_double_pointer_deref.c" <<'EOF'
static int first_is_a(char **cursor) {
    return **cursor == 'a';
}

int main(void) {
    char text[] = "abc";
    char *cursor = text;
    return first_is_a(&cursor) ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/char_double_pointer_deref.c" "$WORK_DIR/char_double_pointer_deref_bin" "0" "compiler loaded a full word instead of a byte through a char double-pointer dereference"

cat > "$WORK_DIR/shadowed_local_name.c" <<'EOF'
static int path_is_nonempty(char **argv) {
    return argv[0][0] != '\0';
}

int main(int argc, char **argv) {
    int guard = 0;
    if (argc > 100) {
        int j = 1;
        guard = j;
    }
    if (guard == 0) {
        int j = 0;
        if (path_is_nonempty(argv)) {
            return j;
        }
        return 2;
    }
    return 1;
}
EOF

compile_and_check_native "$WORK_DIR/shadowed_local_name.c" "$WORK_DIR/shadowed_local_name_bin" "0" "compiler corrupted a shadowed block-local with the same name in a later scope"

cat > "$WORK_DIR/implicit_fallthrough_return.c" <<'EOF'
static void copy_text(char *dst, int limit, const char *src) {
    int i = 0;
    if (limit == 0) {
        return;
    }
    while (src[i] != '\0' && i + 1 < limit) {
        dst[i] = src[i];
        i = i + 1;
    }
    dst[i] = '\0';
}

int main(int argc, char **argv) {
    char buf[32];
    copy_text(buf, 32, argc > 0 ? argv[0] : "copy");
    return buf[0] != '\0' ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/implicit_fallthrough_return.c" "$WORK_DIR/implicit_fallthrough_return_bin" "0" "compiler omitted the function epilogue after an early return in a void helper"

cat > "$WORK_DIR/int128_cast.c" <<'EOF'
int main(void) {
    unsigned __int128 root = 42;
    long long value = (__int128)root;
    return value == 42 ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/int128_cast.c" "$WORK_DIR/int128_cast_bin" "0" "compiler failed on __int128 cast expressions"

cat > "$WORK_DIR/int128_predefine.c" <<'EOF'
#ifndef __SIZEOF_INT128__
int main(void) { return 1; }
#else
int main(void) {
    return __SIZEOF_INT128__ == 16 ? 0 : 2;
}
#endif
EOF

compile_and_check_native "$WORK_DIR/int128_predefine.c" "$WORK_DIR/int128_predefine_bin" "0" "compiler did not advertise x86_64 __int128 support to the preprocessor"

cat > "$WORK_DIR/int128_runtime_arithmetic.c" <<'EOF'
static unsigned long long high_product(unsigned long long value) {
    unsigned __int128 wide = value;
    unsigned __int128 product = wide;
    product *= wide;
    unsigned long long high = (unsigned long long)(product >> 51);
    product &= ((unsigned long long)((1ULL << 51) - 1ULL));
    if ((unsigned long long)product != 1ULL) return 0ULL;
    return high;
}

static int scalar_cast_products(void) {
    struct Fraction { long long num; long long den; } values[1];
    unsigned long long aa = 84ULL;
    unsigned long long gcd = 7ULL;
    unsigned long long bb = 9ULL;
    long long signed_aa = -84LL;
    __int128 positive = (__int128)(aa / gcd) * bb;
    __int128 negative = (__int128)(signed_aa / (long long)gcd) * (long long)bb;
    long long lcm = 45LL;
    __int128 member_product;
    values[0].num = 7LL;
    values[0].den = 5LL;
    member_product = (__int128)values[0].num * (lcm / values[0].den);
    return positive == 108 && negative == -108 && member_product == 63;
}

int main(void) {
    return high_product(2251799813685247ULL) == 2251799813685246ULL && scalar_cast_products() ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/int128_runtime_arithmetic.c" "$WORK_DIR/int128_runtime_arithmetic_bin" "0" "compiler failed on runtime unsigned __int128 arithmetic"

cat > "$WORK_DIR/u64_typedef_mask_width.c" <<'EOF'
typedef unsigned long long u64;
#define MASK51 ((u64)((1ULL << 51) - 1ULL))

int main(void) {
    u64 value = 0x6666666666666658ULL;
    u64 masked = value & MASK51;
    return masked == 1801439850948184ULL ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/u64_typedef_mask_width.c" "$WORK_DIR/u64_typedef_mask_width_bin" "0" "compiler truncated ULL/u64 mask expressions to 32 bits"

cat > "$WORK_DIR/u32_shift_widens_after_wrap.c" <<'EOF'
static unsigned long long widen_shift(unsigned int word) {
    unsigned long long widened = (word << 26) | 7u;
    return widened;
}

int main(void) {
    return widen_shift(0x04000000u) == 7ULL ? 0 : 1;
}
EOF

compile_and_check_native "$WORK_DIR/u32_shift_widens_after_wrap.c" "$WORK_DIR/u32_shift_widens_after_wrap_bin" "0" "compiler widened unsigned 32-bit shift results before wrapping"

cat > "$WORK_DIR/atomic_builtins.c" <<'EOF'
int main(void) {
    int value = 1;
    int expected;

    if (__atomic_exchange_n(&value, 3, __ATOMIC_ACQ_REL) != 1) return 1;
    __atomic_store_n(&value, 4, __ATOMIC_RELEASE);
    if (__atomic_load_n(&value, __ATOMIC_ACQUIRE) != 4) return 2;
    if (__atomic_fetch_add(&value, 2, __ATOMIC_ACQ_REL) != 4) return 3;
    if (__atomic_fetch_sub(&value, 1, __ATOMIC_ACQ_REL) != 6) return 4;
    expected = 5;
    if (!__atomic_compare_exchange_n(&value, &expected, 9, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return 5;
    expected = 5;
    if (__atomic_compare_exchange_n(&value, &expected, 10, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return 6;
    if (expected != 9) return 7;
    __sync_synchronize();
    return value == 9 ? 0 : 8;
}
EOF

compile_and_check_native "$WORK_DIR/atomic_builtins.c" "$WORK_DIR/atomic_builtins_bin" "0" "compiler atomic builtin lowering failed"

cat > "$WORK_DIR/atomic_member_width.c" <<'EOF'
struct State {
    unsigned int claimed;
    unsigned int finished;
};

int main(void) {
    struct State state;
    unsigned int expected = 1U;

    state.claimed = 1U;
    state.finished = 123U;
    if (!__atomic_compare_exchange_n(&state.claimed, &expected, 2U, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return 1;
    if (state.claimed != 2U) return 2;
    if (state.finished != 123U) return 3;
    return 0;
}
EOF

compile_and_check_native "$WORK_DIR/atomic_member_width.c" "$WORK_DIR/atomic_member_width_bin" "0" "compiler widened a 32-bit member compare-exchange"

cat > "$WORK_DIR/char_escape_folding.c" <<'EOF'
int main(void) {
    if ('\b' != 8) return 1;
    if ('\a' != 7) return 2;
    if ('\x41' != 'A') return 3;
    if ('\101' != 'A') return 4;
    return 0;
}
EOF

compile_and_check_native "$WORK_DIR/char_escape_folding.c" "$WORK_DIR/char_escape_folding_bin" "0" "compiler constant-folded escaped character literals incorrectly"

if [ "$RUN_TARGET" = "linux-x86_64" ] && command -v cc >/dev/null 2>&1; then
    native_ar="${TEST_BIN_DIR}/ar"
    if [ ! -x "$native_ar" ] && command -v ar >/dev/null 2>&1; then
        native_ar=ar
    fi

    cat > "$WORK_DIR/native_archive_start.s" <<'EOF'
.globl _start
_start:
    call helper
    mov %rax, %rdi
    mov $60, %rax
    syscall
EOF
    cat > "$WORK_DIR/native_archive_helper.s" <<'EOF'
.globl helper
helper:
    mov $42, %rax
    ret
EOF
    cc -x assembler -c "$WORK_DIR/native_archive_start.s" -o "$WORK_DIR/native_archive_start.o"
    cc -x assembler -c "$WORK_DIR/native_archive_helper.s" -o "$WORK_DIR/native_archive_helper.o"
    "$native_ar" rc "$WORK_DIR/libnative_helper.a" "$WORK_DIR/native_archive_helper.o"
    "${TEST_BIN_DIR}/ncc" --target linux-x86_64 -nostdlib -static \
        "$WORK_DIR/native_archive_start.o" "$WORK_DIR/libnative_helper.a" \
        -o "$WORK_DIR/native_archive_bin"
    if "$WORK_DIR/native_archive_bin"; then
        native_archive_status=0
    else
        native_archive_status=$?
    fi
    assert_text_equals "$native_archive_status" "42" "native linker did not pull a referenced object from an ar archive"

    cat > "$WORK_DIR/native_gc_sections.s" <<'EOF'
.globl _start
_start:
    call used
    mov %rax, %rdi
    mov $60, %rax
    syscall
.section .text.used,"ax",@progbits
used:
    xor %rax, %rax
    ret
.section .rodata.unused,"a",@progbits
unused_marker:
    .asciz "NATIVE_GC_DROP_ME"
EOF
    cc -x assembler -c "$WORK_DIR/native_gc_sections.s" -o "$WORK_DIR/native_gc_sections.o"
    "${TEST_BIN_DIR}/ncc" --target linux-x86_64 -nostdlib -static -Wl,--gc-sections \
        "$WORK_DIR/native_gc_sections.o" -o "$WORK_DIR/native_gc_sections_bin"
    "$WORK_DIR/native_gc_sections_bin"
    if grep -aq 'NATIVE_GC_DROP_ME' "$WORK_DIR/native_gc_sections_bin"; then
        fail "ncc native linker path did not pass --gc-sections through to the in-project linker"
    fi

    cat > "$WORK_DIR/native_lto_start.s" <<'EOF'
.globl _start
_start:
    call native_lto_entry
    mov %rax, %rdi
    mov $60, %rax
    syscall
EOF
    cat > "$WORK_DIR/native_lto_value.c" <<'EOF'
int native_lto_value(void) {
    return 23;
}
EOF
    cat > "$WORK_DIR/native_lto_entry.c" <<'EOF'
int native_lto_value(void);

int native_lto_entry(void) {
    return native_lto_value();
}
EOF
    cc -x assembler -c "$WORK_DIR/native_lto_start.s" -o "$WORK_DIR/native_lto_start.o"
    "${TEST_BIN_DIR}/ncc" --target linux-x86_64 -nostdlib -static -flto \
        "$WORK_DIR/native_lto_start.o" "$WORK_DIR/native_lto_entry.c" "$WORK_DIR/native_lto_value.c" \
        -o "$WORK_DIR/native_lto_bin"
    if "$WORK_DIR/native_lto_bin"; then
        native_lto_status=0
    else
        native_lto_status=$?
    fi
    assert_text_equals "$native_lto_status" "23" "native linker -flto path did not link a combined ncc object"

    if command -v readelf >/dev/null 2>&1; then
        readelf -l "$WORK_DIR/native_archive_bin" > "$WORK_DIR/native_archive_readelf.out"
        assert_file_contains "$WORK_DIR/native_archive_readelf.out" 'There is 1 program header' "native linker did not keep a pure-text archive link compact"
    fi

    cat > "$WORK_DIR/native_data_segment.s" <<'EOF'
.globl _start
_start:
    mov value(%rip), %rdi
    mov $60, %rax
    syscall
.data
.globl value
value:
    .quad 7
EOF
    cc -x assembler -c "$WORK_DIR/native_data_segment.s" -o "$WORK_DIR/native_data_segment.o"
    "${TEST_BIN_DIR}/ncc" --target linux-x86_64 -nostdlib -static \
        "$WORK_DIR/native_data_segment.o" -o "$WORK_DIR/native_data_segment_bin"
    if "$WORK_DIR/native_data_segment_bin"; then
        native_data_status=0
    else
        native_data_status=$?
    fi
    assert_text_equals "$native_data_status" "7" "native linker did not preserve a data-segment relocation"

    if command -v readelf >/dev/null 2>&1; then
        readelf -l "$WORK_DIR/native_data_segment_bin" > "$WORK_DIR/native_data_segment_readelf.out"
        assert_file_contains "$WORK_DIR/native_data_segment_readelf.out" 'R E' "native linker text segment is not executable/read-only"
        assert_file_contains "$WORK_DIR/native_data_segment_readelf.out" 'RW' "native linker did not emit a writable data segment"
    fi

    cat > "$WORK_DIR/native_merge_strings.s" <<'EOF'
.globl _start
_start:
    lea whole(%rip), %rsi
    cmpb $'s', 0(%rsi)
    jne fail
    cmpb $'l', 10(%rsi)
    jne fail
    lea duplicate(%rip), %rsi
    cmpb $'h', 1(%rsi)
    jne fail
    lea suffix(%rip), %rsi
    cmpb $'t', 0(%rsi)
    jne fail
    cmpb $0, 4(%rsi)
    jne fail
    xor %rdi, %rdi
    jmp done
fail:
    mov $17, %rdi
done:
    mov $60, %rax
    syscall
.section .rodata.str1.1,"aMS",@progbits,1
whole:
    .asciz "shared-tail"
duplicate:
    .asciz "shared-tail"
suffix:
    .asciz "tail"
EOF
    cc -x assembler -c "$WORK_DIR/native_merge_strings.s" -o "$WORK_DIR/native_merge_strings.o"
    "${TEST_BIN_DIR}/ncc" --target linux-x86_64 -nostdlib -static \
        "$WORK_DIR/native_merge_strings.o" -o "$WORK_DIR/native_merge_strings_bin"
    "$WORK_DIR/native_merge_strings_bin"
    merge_string_count=$(grep -ao 'shared-tail' "$WORK_DIR/native_merge_strings_bin" | wc -l | tr -d ' ')
    assert_text_equals "$merge_string_count" "1" "native linker did not merge duplicate SHF_MERGE strings"
fi

"${TEST_BIN_DIR}/ncc" -c "$WORK_DIR/sample.c" -o "$WORK_DIR/default_host.o"
if command -v od >/dev/null 2>&1; then
    od -An -tx1 -N4 "$WORK_DIR/default_host.o" > "$WORK_DIR/default_host_hex.out"
else
    native_hexdump="${TEST_BIN_DIR}/hexdump"
    if [ ! -x "$native_hexdump" ] && command -v hexdump >/dev/null 2>&1; then
        native_hexdump=hexdump
    fi
    "$native_hexdump" "$WORK_DIR/default_host.o" > "$WORK_DIR/default_host_hex.out"
fi
if ! grep -q '7f[[:space:]][[:space:]]*45[[:space:]][[:space:]]*4c[[:space:]][[:space:]]*46' "$WORK_DIR/default_host_hex.out" &&
   ! grep -q 'cf[[:space:]][[:space:]]*fa[[:space:]][[:space:]]*ed[[:space:]][[:space:]]*fe' "$WORK_DIR/default_host_hex.out"; then
    fail "compiler default target did not emit a supported object format"
fi
"${TEST_BIN_DIR}/ncc" "$WORK_DIR/sample.c" -o "$WORK_DIR/default_host_bin"
if "$WORK_DIR/default_host_bin"; then
    default_link_status=0
else
    default_link_status=$?
fi
assert_text_equals "$default_link_status" "42" "compiler default target did not link a runnable executable"
