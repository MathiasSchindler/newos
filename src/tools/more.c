#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PAGER_LINE_CAPACITY 4096
#define DEFAULT_PAGE_LINES 23
#define PAGER_SEARCH_CAPACITY 256

typedef enum {
    PAGER_ACT_PAGE = 0,
    PAGER_ACT_LINE = 1,
    PAGER_ACT_START = 2,
    PAGER_ACT_END = 3,
    PAGER_ACT_NEXT_MATCH = 4,
    PAGER_ACT_QUIT = 5
} PagerAction;

typedef struct {
    int fd;
    int interactive;
    int raw_mode_enabled;
    int show_numbers;
    int seekable;
    int color_mode;
    unsigned int page_lines;
    unsigned long long line_number;
    char search_pattern[PAGER_SEARCH_CAPACITY];
    PlatformTerminalState saved_state;
} PagerState;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-N] [-p PATTERN] [--color[=WHEN]] [+/PATTERN] [file ...]");
}

static unsigned int pager_page_lines(void) {
    const char *text = platform_getenv("LINES");
    unsigned long long value = 0;
    unsigned int rows = 0U;

    if (text != 0 && rt_parse_uint(text, &value) == 0 && value > 1 && value < 1000) {
        return (unsigned int)(value - 1);
    }

    if (platform_get_terminal_size(1, &rows, 0) == 0 && rows > 1U && rows < 1000U) {
        return rows - 1U;
    }

    return DEFAULT_PAGE_LINES;
}

static int contains_case_insensitive(const char *text, const char *needle) {
    size_t text_len = rt_strlen(text);
    size_t needle_len = rt_strlen(needle);
    size_t pos = 0U;

    if (needle_len == 0U) {
        return 1;
    }

    while (pos < text_len) {
        size_t ti = pos;
        size_t ni = 0U;
        int matched = 1;

        while (ni < needle_len) {
            unsigned int lhs = 0U;
            unsigned int rhs = 0U;

            if (ti >= text_len || rt_utf8_decode(text, text_len, &ti, &lhs) != 0 ||
                rt_utf8_decode(needle, needle_len, &ni, &rhs) != 0) {
                matched = 0;
                break;
            }
            if (rt_unicode_simple_fold(lhs) != rt_unicode_simple_fold(rhs)) {
                matched = 0;
                break;
            }
        }

        if (matched) {
            return 1;
        }

        {
            unsigned int ignored = 0U;
            if (rt_utf8_decode(text, text_len, &pos, &ignored) != 0) {
                pos += 1U;
            }
        }
    }

    return 0;
}

static int find_case_insensitive_match(const char *text, const char *needle, size_t *start_out, size_t *end_out) {
    size_t text_len = rt_strlen(text);
    size_t needle_len = rt_strlen(needle);
    size_t pos = 0U;

    if (needle_len == 0U) {
        *start_out = 0U;
        *end_out = 0U;
        return 0;
    }

    while (pos < text_len) {
        size_t start = pos;
        size_t ti = pos;
        size_t ni = 0U;
        int matched = 1;

        while (ni < needle_len) {
            unsigned int lhs = 0U;
            unsigned int rhs = 0U;

            if (ti >= text_len || rt_utf8_decode(text, text_len, &ti, &lhs) != 0 ||
                rt_utf8_decode(needle, needle_len, &ni, &rhs) != 0) {
                matched = 0;
                break;
            }
            if (rt_unicode_simple_fold(lhs) != rt_unicode_simple_fold(rhs)) {
                matched = 0;
                break;
            }
        }

        if (matched) {
            *start_out = start;
            *end_out = ti;
            return 0;
        }

        {
            unsigned int ignored = 0U;
            if (rt_utf8_decode(text, text_len, &pos, &ignored) != 0) {
                pos += 1U;
            }
        }
    }

    return -1;
}

static int pager_read_line(int fd, char *buffer, size_t buffer_size, int *eof_out) {
    size_t length = 0U;

    if (buffer_size == 0U) {
        return -1;
    }

    *eof_out = 0;
    while (length + 1U < buffer_size) {
        char ch = '\0';
        long bytes_read = platform_read(fd, &ch, 1);

        if (bytes_read < 0) {
            return -1;
        }
        if (bytes_read == 0) {
            *eof_out = 1;
            break;
        }

        buffer[length++] = ch;
        if (ch == '\n') {
            break;
        }
    }

    if (length + 1U >= buffer_size && length > 0U && buffer[length - 1U] != '\n') {
        for (;;) {
            char ch = '\0';
            long bytes_read = platform_read(fd, &ch, 1);
            if (bytes_read < 0) {
                return -1;
            }
            if (bytes_read == 0) {
                *eof_out = 1;
                break;
            }
            if (ch == '\n') {
                break;
            }
        }
    }

    buffer[length] = '\0';
    return 0;
}

static long long pager_tell(int fd) {
    return platform_seek(fd, 0, PLATFORM_SEEK_CUR);
}

static void pager_init(PagerState *state, int fd, int interactive, int show_numbers, int color_mode, const char *pattern) {
    rt_memset(state, 0, sizeof(*state));
    state->fd = fd;
    state->interactive = interactive;
    state->show_numbers = show_numbers;
    state->color_mode = color_mode;
    state->page_lines = pager_page_lines();
    state->line_number = 1ULL;
    state->seekable = pager_tell(fd) >= 0;

    if (pattern != 0) {
        rt_copy_string(state->search_pattern, sizeof(state->search_pattern), pattern);
    }

    if (state->interactive && platform_terminal_enable_raw_mode(0, &state->saved_state) == 0) {
        state->raw_mode_enabled = 1;
    }
}

static void pager_finish(PagerState *state) {
    if (state->raw_mode_enabled) {
        (void)platform_terminal_restore_mode(0, &state->saved_state);
        state->raw_mode_enabled = 0;
    }
}

static void pager_write_text(int fd, int color_mode, int style, const char *text) {
    if (style != TOOL_STYLE_PLAIN) {
        tool_style_begin(fd, color_mode, style);
    }
    rt_write_cstr(fd, text);
    if (style != TOOL_STYLE_PLAIN) {
        tool_style_end(fd, color_mode);
    }
}

static int pager_write_highlighted_line(const PagerState *state, const char *line) {
    size_t start = 0U;
    size_t end = 0U;
    int use_color = tool_should_use_color_fd(1, state->color_mode);

    if (state->show_numbers) {
        if (use_color) {
            tool_style_begin(1, state->color_mode, TOOL_STYLE_CYAN);
        }
        if (rt_write_uint(1, state->line_number) != 0 || rt_write_cstr(1, "\t") != 0) {
            if (use_color) {
                tool_style_end(1, state->color_mode);
            }
            return -1;
        }
        if (use_color) {
            tool_style_end(1, state->color_mode);
        }
    }

    if (state->search_pattern[0] != '\0' && use_color &&
        find_case_insensitive_match(line, state->search_pattern, &start, &end) == 0 &&
        end > start) {
        if (start > 0U && rt_write_all(1, line, start) != 0) {
            return -1;
        }
        tool_style_begin(1, state->color_mode, TOOL_STYLE_BOLD_YELLOW);
        if (rt_write_all(1, line + start, end - start) != 0) {
            tool_style_end(1, state->color_mode);
            return -1;
        }
        tool_style_end(1, state->color_mode);
        return rt_write_cstr(1, line + end);
    }

    if (use_color) {
        tool_style_begin(1, state->color_mode, TOOL_STYLE_CYAN);
    }
    if (rt_write_cstr(1, line) != 0) {
        if (use_color) {
            tool_style_end(1, state->color_mode);
        }
        return -1;
    }
    if (use_color) {
        tool_style_end(1, state->color_mode);
    }
    return 0;
}

static int pager_seek_to(PagerState *state, long long offset, unsigned long long line_number) {
    if (!state->seekable || offset < 0) {
        return -1;
    }
    if (platform_seek(state->fd, offset, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }
    state->line_number = line_number;
    return 0;
}

static int pager_seek_to_next_match(PagerState *state) {
    char line[PAGER_LINE_CAPACITY];
    long long saved_offset;
    unsigned long long current_line;
    unsigned long long original_line;

    if (!state->seekable || state->search_pattern[0] == '\0') {
        return 0;
    }

    saved_offset = pager_tell(state->fd);
    original_line = state->line_number;
    current_line = state->line_number;

    for (;;) {
        int eof = 0;
        long long line_offset = pager_tell(state->fd);
        unsigned long long line_number = current_line;

        if (pager_read_line(state->fd, line, sizeof(line), &eof) != 0) {
            return -1;
        }
        if (eof && line[0] == '\0') {
            break;
        }
        if (contains_case_insensitive(line, state->search_pattern)) {
            return pager_seek_to(state, line_offset, line_number);
        }
        current_line += 1ULL;
        if (eof) {
            break;
        }
    }

    (void)pager_seek_to(state, saved_offset, original_line);
    return 0;
}

static int pager_seek_to_last_page(PagerState *state) {
    char line[PAGER_LINE_CAPACITY];
    long long candidate_offset = 0;
    unsigned long long candidate_line = 1ULL;
    unsigned long long current_line = 1ULL;
    unsigned int lines_in_page = 0U;

    if (!state->seekable) {
        return -1;
    }

    if (platform_seek(state->fd, 0, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }

    for (;;) {
        int eof = 0;
        long long line_offset = pager_tell(state->fd);

        if (pager_read_line(state->fd, line, sizeof(line), &eof) != 0) {
            return -1;
        }
        if (eof && line[0] == '\0') {
            break;
        }
        if (lines_in_page == 0U) {
            candidate_offset = line_offset;
            candidate_line = current_line;
        }
        lines_in_page += 1U;
        current_line += 1ULL;
        if (lines_in_page >= state->page_lines) {
            lines_in_page = 0U;
        }
        if (eof) {
            break;
        }
    }

    return pager_seek_to(state, candidate_offset, candidate_line);
}

static void pager_write_help(const PagerState *state) {
    rt_write_char(1, '\n');
    pager_write_text(1, state->color_mode, TOOL_STYLE_BOLD_CYAN, "Keys:");
    rt_write_cstr(1, " Enter/j line, Space/f page, g top, G end");
    if (state->search_pattern[0] != '\0') {
        rt_write_cstr(1, ", n next match");
    }
    rt_write_cstr(1, ", q quit");
    rt_write_char(1, '\n');
}

static PagerAction pager_prompt(PagerState *state) {
    char input = '\0';

    for (;;) {
        long long current = -1;
        long long end = -1;

        pager_write_text(1, state->color_mode, TOOL_STYLE_BOLD_CYAN, "--More--");
        if (state->seekable) {
            current = pager_tell(state->fd);
            if (current >= 0) {
                end = platform_seek(state->fd, 0, PLATFORM_SEEK_END);
                (void)platform_seek(state->fd, current, PLATFORM_SEEK_SET);
            }
        }
        if (state->search_pattern[0] != '\0') {
            rt_write_cstr(1, " /");
            rt_write_cstr(1, state->search_pattern);
        }
        if (current >= 0 && end > 0) {
            rt_write_cstr(1, " (");
            rt_write_uint(1, (unsigned long long)((current * 100LL) / end));
            rt_write_cstr(1, "%)");
        }

        if (platform_read(0, &input, 1) <= 0) {
            rt_write_cstr(1, "\r\033[K");
            return PAGER_ACT_QUIT;
        }
        rt_write_cstr(1, "\r\033[K");

        if (input == 'q' || input == 'Q') {
            return PAGER_ACT_QUIT;
        }
        if (input == ' ' || input == 'f' || input == 'F') {
            return PAGER_ACT_PAGE;
        }
        if (input == '\n' || input == '\r' || input == 'j' || input == 'J') {
            return PAGER_ACT_LINE;
        }
        if (input == 'g') {
            return PAGER_ACT_START;
        }
        if (input == 'G') {
            return PAGER_ACT_END;
        }
        if ((input == 'n' || input == 'N') && state->search_pattern[0] != '\0') {
            return PAGER_ACT_NEXT_MATCH;
        }
        if (input == 'h' || input == 'H' || input == '?') {
            pager_write_help(state);
            continue;
        }
        rt_write_char(1, '\a');
    }
}

static int page_stream(int fd, int interactive, int show_numbers, int color_mode, const char *search_pattern) {
    PagerState state;
    unsigned int page_target;
    int exit_code = 0;

    pager_init(&state, fd, interactive, show_numbers, color_mode, search_pattern);

    if (state.search_pattern[0] != '\0' && state.seekable) {
        if (platform_seek(fd, 0, PLATFORM_SEEK_SET) >= 0) {
            state.line_number = 1ULL;
            if (pager_seek_to_next_match(&state) != 0) {
                pager_finish(&state);
                return 1;
            }
        }
    }

    page_target = interactive ? state.page_lines : 0U;

    for (;;) {
        unsigned int lines_shown = 0U;

        for (;;) {
            char line[PAGER_LINE_CAPACITY];
            int eof = 0;

            if (interactive && page_target > 0U && lines_shown >= page_target) {
                break;
            }

            if (pager_read_line(fd, line, sizeof(line), &eof) != 0) {
                exit_code = 1;
                goto done;
            }
            if (eof && line[0] == '\0') {
                goto done;
            }

            if (pager_write_highlighted_line(&state, line) != 0) {
                exit_code = 1;
                goto done;
            }
            state.line_number += 1ULL;
            lines_shown += 1U;

            if (eof) {
                goto done;
            }
        }

        if (!interactive) {
            break;
        }

        for (;;) {
            PagerAction action = pager_prompt(&state);

            if (action == PAGER_ACT_QUIT) {
                goto done;
            }
            if (action == PAGER_ACT_PAGE) {
                page_target = state.page_lines;
                break;
            }
            if (action == PAGER_ACT_LINE) {
                page_target = 1U;
                break;
            }
            if (action == PAGER_ACT_START) {
                if (pager_seek_to(&state, 0, 1ULL) != 0) {
                    exit_code = 1;
                    goto done;
                }
                page_target = state.page_lines;
                break;
            }
            if (action == PAGER_ACT_END) {
                if (pager_seek_to_last_page(&state) != 0) {
                    rt_write_char(1, '\a');
                    continue;
                }
                page_target = state.page_lines;
                break;
            }
            if (action == PAGER_ACT_NEXT_MATCH) {
                if (pager_seek_to_next_match(&state) != 0) {
                    exit_code = 1;
                    goto done;
                }
                page_target = state.page_lines;
                break;
            }
        }
    }

done:
    pager_finish(&state);
    return exit_code;
}

int main(int argc, char **argv) {
    int arg_index = 1;
    int show_numbers = 0;
    int color_mode = TOOL_COLOR_AUTO;
    const char *search_pattern = 0;
    int exit_code = 0;
    int i;

    tool_set_global_color_mode(TOOL_COLOR_AUTO);

    while (arg_index < argc) {
        if (rt_strcmp(argv[arg_index], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (rt_strcmp(argv[arg_index], "-N") == 0) {
            show_numbers = 1;
            arg_index += 1;
            continue;
        }
        if (rt_strcmp(argv[arg_index], "-p") == 0) {
            if (arg_index + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            search_pattern = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        if (rt_strcmp(argv[arg_index], "--color") == 0) {
            color_mode = TOOL_COLOR_ALWAYS;
            arg_index += 1;
            continue;
        }
        if (tool_starts_with(argv[arg_index], "--color=")) {
            if (tool_parse_color_mode(argv[arg_index] + 8, &color_mode) != 0) {
                tool_write_error("more", "invalid color mode: ", argv[arg_index] + 8);
                return 1;
            }
            arg_index += 1;
            continue;
        }
        if (tool_starts_with(argv[arg_index], "+/")) {
            search_pattern = argv[arg_index] + 2;
            arg_index += 1;
            continue;
        }
        break;
    }

    tool_set_global_color_mode(color_mode);

    if (arg_index == argc) {
        int interactive = (platform_isatty(0) != 0 && platform_isatty(1) != 0);
        return page_stream(0, interactive, show_numbers, color_mode, search_pattern) == 0 ? 0 : 1;
    }

    for (i = arg_index; i < argc; ++i) {
        int fd;
        int should_close;
        int interactive;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            tool_write_error("more", "cannot open ", argv[i]);
            exit_code = 1;
            continue;
        }

        interactive = (platform_isatty(0) != 0 && platform_isatty(1) != 0);

        if (argc - arg_index > 1) {
            if (i > arg_index) {
                rt_write_char(1, '\n');
            }
            pager_write_text(1, color_mode, TOOL_STYLE_BOLD_CYAN, "==> ");
            rt_write_cstr(1, argv[i]);
            pager_write_text(1, color_mode, TOOL_STYLE_BOLD_CYAN, " <==\n");
        }

        if (page_stream(fd, interactive, show_numbers, color_mode, search_pattern) != 0) {
            tool_write_error("more", "read error on ", argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
