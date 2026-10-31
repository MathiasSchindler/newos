#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup runtime_io

cat > "$WORK_DIR/runtime_io_test.c" <<'EOF'
#include "platform.h"
#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "runtime I/O check failed at line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static const char payload[] = "abcdef";
static long replies[3];
static size_t calls;
static size_t accepted;

size_t rt_strlen(const char *text) {
    return strlen(text);
}

long platform_write(int fd, const void *data, size_t count) {
    long result;

    CHECK(fd == 17);
    CHECK(calls < sizeof(replies) / sizeof(replies[0]));
    CHECK(count == sizeof(payload) - 1U - accepted);
    CHECK(memcmp(data, payload + accepted, count) == 0);
    result = replies[calls++];
    CHECK(result <= (long)count);
    if (result > 0) accepted += (size_t)result;
    return result;
}

static void reset(long first, long second, long third) {
    replies[0] = first;
    replies[1] = second;
    replies[2] = third;
    calls = 0U;
    accepted = 0U;
}

int main(void) {
    reset(6, 0, 0);
    CHECK(rt_write_all(17, payload, 6U) == 0);
    CHECK(calls == 1U && accepted == 6U);

    reset(2, 1, 3);
    CHECK(rt_write_all(17, payload, 6U) == 0);
    CHECK(calls == 3U && accepted == 6U);

    reset(0, 0, 0);
    CHECK(rt_write_all(-1, 0, 0U) == 0);
    CHECK(calls == 0U);
    CHECK(rt_write_all(17, payload, 6U) == -1);
    CHECK(calls == 1U && accepted == 0U);

    reset(-1, 0, 0);
    CHECK(rt_write_all(17, payload, 6U) == -1);
    CHECK(calls == 1U && accepted == 0U);

    reset(2, 0, 0);
    CHECK(rt_write_all(17, payload, 6U) == -1);
    CHECK(calls == 2U && accepted == 2U);

    reset(2, -1, 0);
    CHECK(rt_write_all(17, payload, 6U) == -1);
    CHECK(calls == 2U && accepted == 2U);
    return 0;
}
EOF

"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$ROOT_DIR/src/shared" \
    "$WORK_DIR/runtime_io_test.c" "$ROOT_DIR/src/shared/runtime/io.c" \
    -o "$WORK_DIR/runtime_io_test"
assert_command_succeeds "$WORK_DIR/runtime_io_test"
