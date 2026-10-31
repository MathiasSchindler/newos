#!/bin/sh
set -eu

. "$(dirname "$0")/common.inc"
phase1_setup tool_cli_object_helpers

cat > "$WORK_DIR/helpers_test.c" <<'C'
#include "runtime.h"
#include "tool_util.h"

#include <limits.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        rt_write_cstr(2, "shared helper check failed at line "); \
        rt_write_uint(2, __LINE__); \
        rt_write_cstr(2, ": "); \
        rt_write_line(2, #condition); \
        return 1; \
    } \
} while (0)

int main(void) {
    static const char *const names[] = {
        "UNSIGNED", "SUBTRACTOR", "BRANCH26", "PAGE21", "PAGEOFF12",
        "GOT_LOAD_PAGE21", "GOT_LOAD_PAGEOFF12", "POINTER_TO_GOT",
        "TLVP_LOAD_PAGE21", "TLVP_LOAD_PAGEOFF12", "ADDEND"
    };
    const char short_arg[] = "-w80";
    const char long_arg[] = "--width=80";
    const char empty_arg[] = "--width=";
    size_t i;

    for (i = 0U; i < sizeof(names) / sizeof(names[0]); ++i) {
        CHECK(rt_strcmp(tool_macho_arm64_relocation_name((unsigned int)i), names[i]) == 0);
    }
    CHECK(rt_strcmp(tool_macho_arm64_relocation_name(11U), "UNKNOWN") == 0);
    CHECK(rt_strcmp(tool_macho_arm64_relocation_name(15U), "UNKNOWN") == 0);
    CHECK(rt_strcmp(tool_macho_arm64_relocation_name(UINT_MAX), "UNKNOWN") == 0);

    CHECK(tool_option_attached_value(short_arg, "-w", "--width") == short_arg + 2U);
    CHECK(tool_option_attached_value(long_arg, "-w", "--width") == long_arg + 8U);
    CHECK(tool_option_attached_value(empty_arg, "-w", "--width") == empty_arg + 8U);
    CHECK(*tool_option_attached_value(empty_arg, "-w", "--width") == '\0');
    CHECK(rt_strcmp(tool_option_attached_value("-w=80", "-w", "--width"), "=80") == 0);
    CHECK(rt_strcmp(tool_option_attached_value("--width==80", "-w", "--width"), "=80") == 0);
    CHECK(rt_strcmp(tool_option_attached_value("-C20", "-C", "--line-bytes"), "20") == 0);
    CHECK(tool_option_attached_value("-c20", "-C", "--line-bytes") == 0);
    CHECK(tool_option_attached_value("-w", "-w", "--width") == 0);
    CHECK(tool_option_attached_value("--width", "-w", "--width") == 0);
    CHECK(tool_option_attached_value("--widthmore=80", "-w", "--width") == 0);
    CHECK(tool_option_attached_value("-x80", "-w", "--width") == 0);
    CHECK(tool_option_attached_value("file", "-w", "--width") == 0);
    CHECK(tool_option_attached_value("", "-w", "--width") == 0);
    return 0;
}
C

make --no-print-directory -s -C "$ROOT_DIR" -f - HELPER_TEST_DIR="$WORK_DIR" "$WORK_DIR/helpers_test" <<'MAKE'
include Makefile

$(HELPER_TEST_DIR)/helpers_test: $(HELPER_TEST_DIR)/helpers_test.c $(SHARED_DEPS) $(HOST_PLATFORM_SOURCES)
	$(CC) $(HOST_CFLAGS) $< $(SHARED_SOURCES) $(HOST_PLATFORM_SOURCES) -o $@
MAKE
assert_command_succeeds "$WORK_DIR/helpers_test"
