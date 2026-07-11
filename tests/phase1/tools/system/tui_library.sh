#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
. "$ROOT_DIR/tests/phase1/lib/test_common.sh"

phase1_prepare_workdir "system/tui_library"
note "phase1 system: tui library"

cat > "$WORK_DIR/tui_test.c" <<'EOF'
#include "tui.h"
#include "runtime.h"

#include <stdlib.h>
#include <string.h>

static const char *test_term = "xterm-256color";

const char *platform_getenv(const char *name) {
    if (strcmp(name, "TERM") == 0) return test_term;
    if (strcmp(name, "COLORTERM") == 0) return "truecolor";
    return 0;
}

int platform_get_terminal_size(int fd, unsigned int *rows, unsigned int *columns) { (void)fd; *rows = 24U; *columns = 80U; return 0; }
int platform_isatty(int fd) { (void)fd; return 1; }
int platform_terminal_enable_raw_mode(int fd, PlatformTerminalState *state) { (void)fd; (void)state; return 0; }
int platform_terminal_restore_mode(int fd, const PlatformTerminalState *state) { (void)fd; (void)state; return 0; }
long platform_read(int fd, void *buffer, size_t count) { (void)fd; (void)buffer; (void)count; return -1; }

void rt_memset(void *buffer, int value, size_t count) { (void)memset(buffer, value, count); }
size_t rt_strlen(const char *text) { return strlen(text); }
int rt_strcmp(const char *left, const char *right) { return strcmp(left, right); }
int rt_strncmp(const char *left, const char *right, size_t count) { return strncmp(left, right, count); }
void rt_unsigned_to_string(unsigned long long value, char *buffer, size_t size) {
    char reversed[32];
    size_t count = 0U;
    size_t index;
    if (size == 0U) return;
    do { reversed[count++] = (char)('0' + value % 10ULL); value /= 10ULL; } while (value != 0ULL && count < sizeof(reversed));
    index = count < size - 1U ? count : size - 1U;
    buffer[index] = '\0';
    while (index > 0U) { index -= 1U; buffer[index] = reversed[count - index - 1U]; }
}
int rt_write_all(int fd, const void *data, size_t count) { (void)fd; (void)data; (void)count; return 0; }
unsigned long long rt_text_display_width_n_tabstop(const char *text, size_t length, unsigned long long initial, unsigned int tab_width) {
    (void)text;
    return initial + length + tab_width;
}
int rt_utf8_decode(const char *text, size_t length, size_t *index, unsigned int *codepoint) {
    unsigned char first;
    if (*index >= length) return -1;
    first = (unsigned char)text[(*index)++];
    if (first < 0x80U) { *codepoint = first; return 0; }
    if ((first & 0xe0U) == 0xc0U && *index < length) {
        *codepoint = ((unsigned int)(first & 0x1fU) << 6U) | ((unsigned int)text[(*index)++] & 0x3fU);
        return 0;
    }
    return -1;
}

static int decode(const unsigned char *bytes, size_t length, unsigned int key, unsigned int modifiers) {
    TuiKeyEvent event;
    size_t consumed = 0U;
    int result = tui_decode_input(bytes, length, &event, &consumed);
    return result == 1 && consumed == length && event.codepoint == key && event.modifiers == modifiers;
}

int main(void) {
    TuiKeyEvent event;
    TuiCapabilities capabilities;
    TuiWidthPolicy policy;
    size_t consumed = 99U;
    static const unsigned char partial[] = {27U, '['};
    static const unsigned char modified_right[] = {27U, '[', '1', ';', '6', 'C'};
    static const unsigned char paste_begin[] = {27U, '[', '2', '0', '0', '~'};
    static const unsigned char paste_end[] = {27U, '[', '2', '0', '1', '~'};
    static const unsigned char function_f5[] = {27U, '[', '1', '5', '~'};
    static const unsigned char utf8[] = {0xc3U, 0xa4U};

    if (tui_decode_input(partial, sizeof(partial), &event, &consumed) != 0 || consumed != 0U) return 1;
    if (!decode(modified_right, sizeof(modified_right), TUI_KEY_ARROW_RIGHT, TUI_MOD_SHIFT | TUI_MOD_CTRL)) return 2;
    if (!decode(paste_begin, sizeof(paste_begin), TUI_KEY_PASTE_BEGIN, 0U)) return 3;
    if (!decode(paste_end, sizeof(paste_end), TUI_KEY_PASTE_END, 0U)) return 4;
    if (!decode(function_f5, sizeof(function_f5), TUI_KEY_FUNCTION_5, 0U)) return 5;
    if (!decode(utf8, sizeof(utf8), 0xe4U, 0U)) return 6;

    tui_capabilities_detect(&capabilities);
    if (!capabilities.ansi || !capabilities.bracketed_paste || capabilities.color_count != 256 || !capabilities.truecolor) return 7;
    test_term = "dumb";
    tui_capabilities_detect(&capabilities);
    if (capabilities.ansi || capabilities.bracketed_paste) return 8;

    tui_width_policy_default(&policy);
    if (policy.tab_width != 8U || policy.ambiguous_width != 1U) return 9;
    if (tui_text_width(&policy, "ab", 2U, 3U) != 13U) return 10;
    return 0;
}
EOF

cc -std=c11 -Wall -Wextra -Wpedantic -Isrc/shared \
    "$WORK_DIR/tui_test.c" src/shared/tui.c \
    -o "$WORK_DIR/tui_test"
assert_command_succeeds "$WORK_DIR/tui_test"