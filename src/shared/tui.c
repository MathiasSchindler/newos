#include "tui.h"

#include "runtime.h"

static int tui_text_contains(const char *text, const char *needle) {
    size_t needle_length;
    size_t i;

    if (text == 0 || needle == 0) return 0;
    needle_length = rt_strlen(needle);
    if (needle_length == 0U) return 1;
    for (i = 0U; text[i] != '\0'; ++i) {
        if (rt_strncmp(text + i, needle, needle_length) == 0) return 1;
    }
    return 0;
}

void tui_capabilities_detect(TuiCapabilities *capabilities) {
    const char *term;
    const char *colorterm;

    if (capabilities == 0) return;
    rt_memset(capabilities, 0, sizeof(*capabilities));
    term = platform_getenv("TERM");
    colorterm = platform_getenv("COLORTERM");
    if (term != 0 && rt_strcmp(term, "dumb") == 0) return;
    capabilities->ansi = 1;
    capabilities->alternate_screen = 1;
    capabilities->bracketed_paste = 1;
    capabilities->color_count = term != 0 && tui_text_contains(term, "256color") ? 256 : 8;
    capabilities->truecolor = colorterm != 0 && (tui_text_contains(colorterm, "truecolor") || tui_text_contains(colorterm, "24bit"));
}

void tui_width_policy_default(TuiWidthPolicy *policy) {
    if (policy == 0) return;
    policy->tab_width = 8U;
    policy->ambiguous_width = 1U;
}

unsigned long long tui_text_width(const TuiWidthPolicy *policy, const char *text, size_t length, unsigned long long initial_width) {
    unsigned int tab_width = policy != 0 && policy->tab_width != 0U ? policy->tab_width : 8U;
    return rt_text_display_width_n_tabstop(text, length, initial_width, tab_width);
}

static int tui_write_uint(TuiTerminal *terminal, unsigned int value) {
    char buffer[32];
    rt_unsigned_to_string((unsigned long long)value, buffer, sizeof(buffer));
    return tui_write_cstr(terminal, buffer);
}

int tui_write(TuiTerminal *terminal, const char *text, size_t length) {
    if (terminal == 0 || text == 0) return -1;
    return rt_write_all(terminal->output_fd, text, length);
}

int tui_write_cstr(TuiTerminal *terminal, const char *text) {
    if (text == 0) return -1;
    return tui_write(terminal, text, rt_strlen(text));
}

int tui_terminal_refresh_size(TuiTerminal *terminal) {
    unsigned int rows = 0U;
    unsigned int columns = 0U;

    if (terminal == 0) return -1;
    if (platform_get_terminal_size(terminal->output_fd, &rows, &columns) != 0) {
        rows = 24U;
        columns = 80U;
    }
    terminal->rows = rows == 0U ? 24U : rows;
    terminal->columns = columns == 0U ? 80U : columns;
    return 0;
}

int tui_terminal_check_resize(TuiTerminal *terminal) {
    unsigned int old_rows;
    unsigned int old_columns;

    if (terminal == 0) return -1;
    old_rows = terminal->rows;
    old_columns = terminal->columns;
    if (tui_terminal_refresh_size(terminal) != 0) return -1;
    return terminal->rows != old_rows || terminal->columns != old_columns;
}

int tui_enter_alternate_screen(TuiTerminal *terminal) {
    if (terminal == 0 || !terminal->capabilities.alternate_screen) return 0;
    if (tui_write_cstr(terminal, "\033[?1049h") != 0) return -1;
    terminal->alternate_screen = 1;
    return 0;
}

int tui_leave_alternate_screen(TuiTerminal *terminal) {
    if (terminal == 0 || !terminal->alternate_screen) return 0;
    terminal->alternate_screen = 0;
    return tui_write_cstr(terminal, "\033[?1049l");
}

int tui_hide_cursor(TuiTerminal *terminal) {
    if (terminal == 0 || !terminal->capabilities.ansi) return 0;
    if (tui_write_cstr(terminal, "\033[?25l") != 0) return -1;
    terminal->cursor_hidden = 1;
    return 0;
}

int tui_show_cursor(TuiTerminal *terminal) {
    if (terminal == 0 || !terminal->cursor_hidden) return 0;
    terminal->cursor_hidden = 0;
    return tui_write_cstr(terminal, "\033[?25h");
}

int tui_terminal_open(TuiTerminal *terminal, int input_fd, int output_fd, int use_alternate_screen) {
    if (terminal == 0) return -1;

    rt_memset(terminal, 0, sizeof(*terminal));
    terminal->input_fd = input_fd;
    terminal->output_fd = output_fd;
    tui_capabilities_detect(&terminal->capabilities);
    tui_width_policy_default(&terminal->width_policy);

    if (!platform_isatty(input_fd) || !platform_isatty(output_fd)) return -1;
    if (platform_terminal_enable_raw_mode(input_fd, &terminal->saved_state) != 0) return -1;
    terminal->raw_enabled = 1;
    (void)tui_terminal_refresh_size(terminal);
    if (use_alternate_screen && tui_enter_alternate_screen(terminal) != 0) {
        tui_terminal_close(terminal);
        return -1;
    }
    if (tui_enable_bracketed_paste(terminal) != 0) {
        tui_terminal_close(terminal);
        return -1;
    }
    (void)tui_hide_cursor(terminal);
    return 0;
}

void tui_terminal_close(TuiTerminal *terminal) {
    if (terminal == 0) return;
    (void)tui_show_cursor(terminal);
    (void)tui_set_inverse(terminal, 0);
    (void)tui_disable_bracketed_paste(terminal);
    (void)tui_leave_alternate_screen(terminal);
    if (terminal->raw_enabled) {
        (void)platform_terminal_restore_mode(terminal->input_fd, &terminal->saved_state);
        terminal->raw_enabled = 0;
    }
}

int tui_clear_screen(TuiTerminal *terminal) {
    if (terminal == 0 || !terminal->capabilities.ansi) return 0;
    return tui_write_cstr(terminal, "\033[2J");
}

int tui_clear_line(TuiTerminal *terminal) {
    if (terminal == 0 || !terminal->capabilities.ansi) return 0;
    return tui_write_cstr(terminal, "\033[K");
}

int tui_move_cursor(TuiTerminal *terminal, unsigned int row, unsigned int column) {
    if (terminal == 0 || !terminal->capabilities.ansi) return 0;
    if (row == 0U) row = 1U;
    if (column == 0U) column = 1U;
    if (tui_write_cstr(terminal, "\033[") != 0) return -1;
    if (tui_write_uint(terminal, row) != 0) return -1;
    if (tui_write_cstr(terminal, ";") != 0) return -1;
    if (tui_write_uint(terminal, column) != 0) return -1;
    return tui_write_cstr(terminal, "H");
}

int tui_set_inverse(TuiTerminal *terminal, int enabled) {
    if (terminal == 0 || !terminal->capabilities.ansi) return 0;
    return tui_write_cstr(terminal, enabled ? "\033[7m" : "\033[0m");
}

int tui_set_style(TuiTerminal *terminal, int style) {
    if (terminal == 0 || !terminal->capabilities.ansi) return 0;
    switch (style) {
        case TUI_STYLE_INVERSE: return tui_write_cstr(terminal, "\033[7m");
        case TUI_STYLE_COMMENT: return tui_write_cstr(terminal, "\033[32m");
        case TUI_STYLE_STRING: return tui_write_cstr(terminal, "\033[33m");
        case TUI_STYLE_KEYWORD: return tui_write_cstr(terminal, "\033[36m");
        case TUI_STYLE_NUMBER: return tui_write_cstr(terminal, "\033[35m");
        case TUI_STYLE_MATCH: return tui_write_cstr(terminal, "\033[30;47m");
        default: return tui_write_cstr(terminal, "\033[0m");
    }
}

int tui_enable_bracketed_paste(TuiTerminal *terminal) {
    if (terminal == 0 || terminal->bracketed_paste || !terminal->capabilities.bracketed_paste) return 0;
    if (tui_write_cstr(terminal, "\033[?2004h") != 0) return -1;
    terminal->bracketed_paste = 1;
    return 0;
}

int tui_disable_bracketed_paste(TuiTerminal *terminal) {
    if (terminal == 0 || !terminal->bracketed_paste) return 0;
    terminal->bracketed_paste = 0;
    return tui_write_cstr(terminal, "\033[?2004l");
}

static int tui_read_byte(TuiTerminal *terminal, unsigned char *byte_out) {
    long bytes;

    if (terminal == 0 || byte_out == 0) return -1;
    bytes = platform_read(terminal->input_fd, byte_out, 1U);
    return bytes == 1 ? 0 : -1;
}

static unsigned int tui_csi_modifiers(unsigned int value) {
    unsigned int modifiers = 0U;

    if (value < 2U) {
        return 0U;
    }
    value -= 1U;
    if ((value & 1U) != 0U) modifiers |= TUI_MOD_SHIFT;
    if ((value & 2U) != 0U) modifiers |= TUI_MOD_ALT;
    if ((value & 4U) != 0U) modifiers |= TUI_MOD_CTRL;
    return modifiers;
}

static void tui_event_special_mod(TuiKeyEvent *event, unsigned int codepoint, unsigned int modifiers) {
    rt_memset(event, 0, sizeof(*event));
    event->type = TUI_KEY_SPECIAL;
    event->codepoint = codepoint;
    event->modifiers = modifiers;
}

static void tui_event_special(TuiKeyEvent *event, unsigned int codepoint) {
    tui_event_special_mod(event, codepoint, 0U);
}

static void tui_event_ctrl(TuiKeyEvent *event, unsigned int codepoint) {
    rt_memset(event, 0, sizeof(*event));
    event->type = TUI_KEY_CTRL;
    event->codepoint = codepoint;
}

static void tui_event_character(TuiKeyEvent *event, const char *text, size_t length, unsigned int codepoint) {
    rt_memset(event, 0, sizeof(*event));
    event->type = TUI_KEY_CHARACTER;
    event->codepoint = codepoint;
    if (length > sizeof(event->text)) length = sizeof(event->text);
    memcpy(event->text, text, length);
    event->text_length = length;
}

static void tui_decode_csi_event(unsigned char final, const unsigned int *params, size_t param_count, TuiKeyEvent *event_out) {
    unsigned int key = param_count > 0U ? params[0] : 0U;
    unsigned int modifiers = param_count > 1U ? tui_csi_modifiers(params[1]) : 0U;
    unsigned int special = 0U;

    if (final == 'A') special = TUI_KEY_ARROW_UP;
    else if (final == 'B') special = TUI_KEY_ARROW_DOWN;
    else if (final == 'C') special = TUI_KEY_ARROW_RIGHT;
    else if (final == 'D') special = TUI_KEY_ARROW_LEFT;
    else if (final == 'H') special = TUI_KEY_HOME;
    else if (final == 'F') special = TUI_KEY_END;
    else if (final == '~') {
        switch (key) {
            case 1U: case 7U: special = TUI_KEY_HOME; break;
            case 2U: special = TUI_KEY_INSERT; break;
            case 3U: special = TUI_KEY_DELETE; break;
            case 4U: case 8U: special = TUI_KEY_END; break;
            case 5U: special = TUI_KEY_PAGE_UP; break;
            case 6U: special = TUI_KEY_PAGE_DOWN; break;
            case 11U: special = TUI_KEY_FUNCTION_1; break;
            case 12U: special = TUI_KEY_FUNCTION_2; break;
            case 13U: special = TUI_KEY_FUNCTION_3; break;
            case 14U: special = TUI_KEY_FUNCTION_4; break;
            case 15U: special = TUI_KEY_FUNCTION_5; break;
            case 17U: special = TUI_KEY_FUNCTION_6; break;
            case 18U: special = TUI_KEY_FUNCTION_7; break;
            case 19U: special = TUI_KEY_FUNCTION_8; break;
            case 20U: special = TUI_KEY_FUNCTION_9; break;
            case 21U: special = TUI_KEY_FUNCTION_10; break;
            case 23U: special = TUI_KEY_FUNCTION_11; break;
            case 24U: special = TUI_KEY_FUNCTION_12; break;
            case 200U: special = TUI_KEY_PASTE_BEGIN; break;
            case 201U: special = TUI_KEY_PASTE_END; break;
            default: break;
        }
    }
    tui_event_special_mod(event_out, special != 0U ? special : TUI_KEY_ESCAPE, modifiers);
}

int tui_decode_input(const unsigned char *data, size_t length, TuiKeyEvent *event_out, size_t *consumed_out) {
    unsigned char ch;

    if (data == 0 || event_out == 0 || consumed_out == 0) return -1;
    *consumed_out = 0U;
    if (length == 0U) return 0;
    ch = data[0];

    if (ch == 27U) {
        if (length < 2U) return 0;
        if (data[1] == '[') {
            unsigned int params[4];
            size_t param_count = 0U;
            unsigned int number = 0U;
            int has_number = 0;
            size_t i = 2U;

            rt_memset(params, 0, sizeof(params));
            if (i < length && data[i] == '?') i += 1U;
            while (i < length) {
                unsigned char current = data[i];
                if (current >= '0' && current <= '9') {
                    has_number = 1;
                    number = number * 10U + (unsigned int)(current - '0');
                } else if (current == ';') {
                    if (param_count < sizeof(params) / sizeof(params[0])) params[param_count++] = has_number ? number : 0U;
                    number = 0U;
                    has_number = 0;
                } else if (current >= 0x40U && current <= 0x7eU) {
                    if (has_number && param_count < sizeof(params) / sizeof(params[0])) params[param_count++] = number;
                    tui_decode_csi_event(current, params, param_count, event_out);
                    *consumed_out = i + 1U;
                    return 1;
                }
                i += 1U;
            }
            return 0;
        }
        if (data[1] == 'O') {
            unsigned int special = 0U;
            if (length < 3U) return 0;
            if (data[2] == 'P') special = TUI_KEY_FUNCTION_1;
            else if (data[2] == 'Q') special = TUI_KEY_FUNCTION_2;
            else if (data[2] == 'R') special = TUI_KEY_FUNCTION_3;
            else if (data[2] == 'S') special = TUI_KEY_FUNCTION_4;
            else if (data[2] == 'H') special = TUI_KEY_HOME;
            else if (data[2] == 'F') special = TUI_KEY_END;
            tui_event_special(event_out, special != 0U ? special : TUI_KEY_ESCAPE);
            *consumed_out = 3U;
            return 1;
        }
        if (data[1] >= 32U && data[1] < 127U) {
            char text = (char)data[1];
            tui_event_character(event_out, &text, 1U, data[1]);
            event_out->modifiers = TUI_MOD_ALT;
            *consumed_out = 2U;
            return 1;
        }
        tui_event_special(event_out, TUI_KEY_ESCAPE);
        *consumed_out = 1U;
        return 1;
    }
    if (ch == '\r' || ch == '\n') tui_event_special(event_out, TUI_KEY_ENTER);
    else if (ch == 127U || ch == 8U) tui_event_special(event_out, TUI_KEY_BACKSPACE);
    else if (ch > 0U && ch < 27U) tui_event_ctrl(event_out, (unsigned int)('A' + ch - 1U));
    else if (ch < 128U) {
        char text = (char)ch;
        tui_event_character(event_out, &text, 1U, ch);
    } else {
        size_t expected;
        size_t decode_index = 0U;
        unsigned int codepoint = 0U;

        if ((ch & 0xe0U) == 0xc0U) expected = 2U;
        else if ((ch & 0xf0U) == 0xe0U) expected = 3U;
        else if ((ch & 0xf8U) == 0xf0U) expected = 4U;
        else expected = 1U;
        if (length < expected) return 0;
        if (rt_utf8_decode((const char *)data, expected, &decode_index, &codepoint) != 0 || decode_index != expected) {
            tui_event_character(event_out, "?", 1U, '?');
            expected = 1U;
        } else {
            tui_event_character(event_out, (const char *)data, expected, codepoint);
        }
        *consumed_out = expected;
        return 1;
    }
    *consumed_out = 1U;
    return 1;
}

int tui_read_key(TuiTerminal *terminal, TuiKeyEvent *event_out) {
    if (terminal == 0 || event_out == 0) return -1;
    for (;;) {
        size_t consumed = 0U;
        int decoded = tui_decode_input(terminal->input_buffer, terminal->input_length, event_out, &consumed);

        if (decoded < 0) return -1;
        if (decoded > 0) {
            if (consumed < terminal->input_length) {
                memmove(terminal->input_buffer, terminal->input_buffer + consumed, terminal->input_length - consumed);
            }
            terminal->input_length -= consumed;
            return 0;
        }
        if (terminal->input_length == sizeof(terminal->input_buffer)) {
            tui_event_special(event_out, TUI_KEY_ESCAPE);
            terminal->input_length = 0U;
            return 0;
        }
        if (tui_read_byte(terminal, &terminal->input_buffer[terminal->input_length]) != 0) return -1;
        terminal->input_length += 1U;
    }
}