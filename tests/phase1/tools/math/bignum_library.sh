#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"

phase1_math_setup bignum_library

cat > "$WORK_DIR/test_bignum.c" <<'EOF'
#include "bignum.h"
#include "runtime.h"
#include "platform.h"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    Bignum a, b, result;
    Bignum remainder;
    char buffer[512];
    int status;

    bn_zero(&a);
    if (!bn_is_zero(&a)) {
        rt_write_line(2, "FAIL: bn_zero");
        return 1;
    }

    bn_from_uint(&a, 12345);
    bn_to_string(&a, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "12345") != 0) {
        rt_write_line(2, "FAIL: bn_from_uint");
        return 1;
    }

    bn_from_int(&b, -9876);
    bn_to_string(&b, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "-9876") != 0) {
        rt_write_line(2, "FAIL: bn_from_int negative");
        return 1;
    }

    bn_from_int(&b, (-9223372036854775807LL - 1LL));
    bn_to_string(&b, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "-9223372036854775808") != 0) {
        rt_write_line(2, "FAIL: bn_from_int minimum signed value");
        return 1;
    }

    status = bn_from_string(&a, "123456789012345678901234567890");
    if (status != 0) {
        rt_write_line(2, "FAIL: bn_from_string large");
        return 1;
    }
    bn_to_string(&a, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "123456789012345678901234567890") != 0) {
        rt_write_line(2, "FAIL: bn_from_string round-trip");
        return 1;
    }

    status = bn_from_string(&a, "-999888777666");
    if (status != 0 || !a.is_negative) {
        rt_write_line(2, "FAIL: bn_from_string negative");
        return 1;
    }

    bn_from_string(&a, "12345");
    bn_from_string(&b, "12345");
    if (bn_compare(&a, &b) != 0) {
        rt_write_line(2, "FAIL: bn_compare equal");
        return 1;
    }

    bn_from_string(&a, "100");
    bn_from_string(&b, "200");
    if (bn_compare(&a, &b) >= 0) {
        rt_write_line(2, "FAIL: bn_compare less than");
        return 1;
    }

    bn_from_string(&a, "5000");
    bn_from_string(&b, "4999");
    if (bn_compare(&a, &b) <= 0) {
        rt_write_line(2, "FAIL: bn_compare greater than");
        return 1;
    }

    bn_from_string(&a, "-50");
    bn_from_string(&b, "10");
    if (bn_compare(&a, &b) >= 0) {
        rt_write_line(2, "FAIL: bn_compare negative vs positive");
        return 1;
    }

    bn_from_string(&a, "123");
    bn_from_string(&b, "456");
    status = bn_add(&a, &b, &result);
    if (status != 0) {
        rt_write_line(2, "FAIL: bn_add status");
        return 1;
    }
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "579") != 0) {
        rt_write_line(2, "FAIL: bn_add simple");
        return 1;
    }

    bn_from_string(&a, "999999999999999999");
    bn_from_string(&b, "1");
    bn_add(&a, &b, &result);
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "1000000000000000000") != 0) {
        rt_write_line(2, "FAIL: bn_add carry");
        return 1;
    }

    bn_from_string(&a, "500");
    bn_from_string(&b, "200");
    bn_subtract(&a, &b, &result);
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "300") != 0) {
        rt_write_line(2, "FAIL: bn_subtract simple");
        return 1;
    }

    bn_from_string(&a, "100");
    bn_from_string(&b, "300");
    bn_subtract(&a, &b, &result);
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "-200") != 0) {
        rt_write_line(2, "FAIL: bn_subtract negative result");
        return 1;
    }

    bn_from_string(&a, "12");
    bn_from_string(&b, "34");
    bn_multiply(&a, &b, &result);
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "408") != 0) {
        rt_write_line(2, "FAIL: bn_multiply simple");
        return 1;
    }

    bn_from_string(&a, "123456789");
    bn_from_string(&b, "987654321");
    bn_multiply(&a, &b, &result);
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "121932631112635269") != 0) {
        rt_write_line(2, "FAIL: bn_multiply large");
        return 1;
    }

    bn_from_string(&a, "100");
    bn_from_string(&b, "10");
    bn_divide(&a, &b, &result, &remainder);
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "10") != 0) {
        rt_write_line(2, "FAIL: bn_divide quotient");
        return 1;
    }
    if (!bn_is_zero(&remainder)) {
        rt_write_line(2, "FAIL: bn_divide remainder should be zero");
        return 1;
    }

    bn_from_string(&a, "100");
    bn_from_string(&b, "30");
    bn_divide(&a, &b, &result, &remainder);
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "3") != 0) {
        rt_write_line(2, "FAIL: bn_divide quotient with remainder");
        return 1;
    }
    bn_to_string(&remainder, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "10") != 0) {
        rt_write_line(2, "FAIL: bn_divide remainder value");
        return 1;
    }

    bn_from_string(&a, "100");
    bn_zero(&b);
    status = bn_divide(&a, &b, &result, &remainder);
    if (status == 0) {
        rt_write_line(2, "FAIL: bn_divide by zero should fail");
        return 1;
    }

    bn_from_string(&a, "2");
    status = bn_power(&a, 10, &result);
    if (status != 0) {
        rt_write_line(2, "FAIL: bn_power status");
        return 1;
    }
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "1024") != 0) {
        rt_write_line(2, "FAIL: bn_power value");
        return 1;
    }

    bn_from_string(&a, "999");
    bn_power(&a, 0, &result);
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "1") != 0) {
        rt_write_line(2, "FAIL: bn_power zero exponent");
        return 1;
    }

    bn_from_string(&a, "123");
    bn_scale(&a, 3, &result);
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "123000") != 0) {
        rt_write_line(2, "FAIL: bn_scale up");
        return 1;
    }

    bn_from_string(&a, "123456");
    bn_scale(&a, -3, &result);
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "123") != 0) {
        rt_write_line(2, "FAIL: bn_scale down");
        return 1;
    }

    bn_from_string(&a, "42");
    bn_negate(&a);
    if (!a.is_negative) {
        rt_write_line(2, "FAIL: bn_negate flag");
        return 1;
    }
    bn_to_string(&a, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "-42") != 0) {
        rt_write_line(2, "FAIL: bn_negate value");
        return 1;
    }

    bn_from_string(&a, "99999999999999999999999999999999999999");
    bn_from_string(&b, "1");
    bn_add(&a, &b, &result);
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "100000000000000000000000000000000000000") != 0) {
        rt_write_line(2, "FAIL: very large add");
        return 1;
    }

    bn_from_string(&a, "-5");
    bn_from_string(&b, "7");
    bn_multiply(&a, &b, &result);
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "-35") != 0) {
        rt_write_line(2, "FAIL: signed multiply");
        return 1;
    }

    bn_from_string(&a, "-6");
    bn_from_string(&b, "-8");
    bn_multiply(&a, &b, &result);
    bn_to_string(&result, buffer, sizeof(buffer));
    if (rt_strcmp(buffer, "48") != 0) {
        rt_write_line(2, "FAIL: negative times negative");
        return 1;
    }

    rt_write_line(1, "BIGNUM_TESTS_OK");
    return 0;
}
EOF

cc -std=c11 -Wall -Wextra -Wpedantic -O2 \
    -Isrc/shared -Isrc/compiler -Isrc/platform/posix -Isrc/platform/linux -Isrc/platform/common \
    -o "$WORK_DIR/test_bignum" "$WORK_DIR/test_bignum.c" \
    src/shared/runtime/memory.c \
    src/shared/runtime/string.c \
    src/shared/runtime/parse.c \
    src/shared/runtime/io.c \
    src/shared/runtime/unicode.c \
    src/shared/tool_io.c \
    src/shared/tool_cli.c \
    src/shared/tool_regex.c \
    src/shared/tool_path.c \
    src/shared/tool_fs.c \
    src/shared/archive_util.c \
    src/shared/bignum.c \
    src/platform/posix/fs.c \
    src/platform/posix/process.c \
    src/platform/posix/identity.c \
    src/platform/posix/net.c \
    src/platform/posix/time.c || fail "bignum test compilation failed"

"$WORK_DIR/test_bignum" > "$WORK_DIR/test_output.txt" 2>&1 || {
    cat "$WORK_DIR/test_output.txt"
    fail "bignum test execution failed"
}

assert_file_contains "$WORK_DIR/test_output.txt" 'BIGNUM_TESTS_OK' "bignum library tests failed"
