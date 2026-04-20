#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PAGER_BUFFER_SIZE 4096
#define LESS_BUFFER_CAPACITY 262144
#define LESS_MAX_LINES 8192
#define DEFAULT_PAGE_LINES 23
#define PAGER_SEARCH_CAPACITY 256

typedef struct {
    int interactive;
    int raw_mode_enabled;
    unsigned int page_lines;
    int color_mode;
    PlatformTerminalState saved_state;
} LessPager;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-N] [-p PATTERN] [--color[=WHEN]] [+/PATTERN] [file ...]");
}

static unsigned int pager_page_lines(void) {
    const char *text = platform_getenv("LINES");
    unsigned long long value = 0;

    if (text != 0 && rt_parse_uint(text, &value) == 0 && value > 1 && value < 1000) {
        return (unsigned int)(value - 1);
    }

    return DEFAULT_PAGE_LINES;
}

static int text_starts_with(const char *text, const char *prefix) {
    while (*prefix != '\0') {
        if (*text != *prefix) {
            return 0;
        }
        text += 1;
        prefix += 1;
    }
    return 1;
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

static void pager_init(LessPager *pager, int color_mode) {
    rt_memset(pager, 0, sizeof(*pager));
    pager->page_lines = pager_page_lines();
    pager->interactive = (platform_isatty(0) != 0 && platform_isatty(1) != 0);
    pager->color_mode = color_mode;
    if (pager->interactive && platform_terminal_enable_raw_mode(0, &pager->saved_state) == 0) {
        pager->raw_mode_enabled = 1;
    }
}

static void pager_finish(LessPager *pager) {
    if (pager->raw_mode_enabled) {
        (void)platform_terminal_restore_mode(0, &pager->saved_state);
        pager->raw_mode_enabled = 0;
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

static int pager_write_line_with_number(const char *line, size_t length, unsigned long long line_number, int show_numbers) {
    if (show_numbers) {
        if (rt_write_uint(1, line_number) != 0 || rt_write_cstr(1, "\t") != 0) {
            return -1;
        }
    }
    if (length > 0U && rt_write_all(1, line, length) != 0) {
        return -1;
    }
    return rt_write_char(1, '\n');
}

static int page_stream(int fd, int interactive, int show_numbers) {
    char buffer[PAGER_BUFFER_SIZE];
    long bytes_read;
    unsigned int page_lines = pager_page_lines();
    unsigned int lines_seen = 0;
    unsigned long long line_number = 1;
    int at_line_start = 1;

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            if (show_numbers && at_line_start) {
                if (rt_write_uint(1, line_number) != 0 || rt_write_cstr(1, "\t") != 0) {
                    return -1;
                }
                at_line_start = 0;
            }

            if (rt_write_char(1, buffer[i]) != 0) {
                return -1;
            }

            if (buffer[i] == '\n') {
                at_line_start = 1;
                line_number += 1;
                if (interactive) {
                    char input[16];
                    lines_seen += 1U;
                    if (lines_seen >= page_lines) {
                        if (rt_write_cstr(1, "--More--") != 0) {
                            return -1;
                        }
                        bytes_read = platform_read(0, input, sizeof(input));
                        (void)rt_write_cstr(1, "\r        \r");
                        if (bytes_read <= 0 || input[0] == 'q' || input[0] == 'Q') {
                            return 0;
                        }
                        if (input[0] == ' ') {
                            lines_seen = 0U;
                        } else {
                            lines_seen = page_lines > 0U ? (page_lines - 1U) : 0U;
                        }
                    }
                }
            }
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int load_buffered_input(int fd, char *buffer, size_t buffer_size, size_t *length_out) {
    size_t used = 0U;
    long bytes_read;

    while ((bytes_read = platform_read(fd, buffer + used, buffer_size - used - 1U)) > 0) {
        used += (size_t)bytes_read;
        if (used + 1U >= buffer_size) {
            return -1;
        }
    }
    if (bytes_read < 0) {
        return -1;
    }
    buffer[used] = '\0';
    *length_out = used;
    return 0;
}

static size_t build_line_index(char *buffer, size_t length, size_t *line_offsets, size_t capacity) {
    size_t count = 0U;
    size_t i;

    if (capacity == 0U) {
        return 0U;
    }
    line_offsets[count++] = 0U;
    for (i = 0U; i < length && count < capacity; ++i) {
        if (buffer[i] == '\n' && i + 1U < length) {
            line_offsets[count++] = i + 1U;
        }
    }
    return count;
}

static size_t line_end_offset(const char *buffer, size_t length, size_t start) {
    size_t end = start;
    while (end < length && buffer[end] != '\n') {
        end += 1U;
    }
    return end;
}

static int search_forward(const char *buffer,
                          size_t length,
                          const size_t *line_offsets,
                          size_t line_count,
                          size_t start_line,
                          const char *pattern,
                          size_t *match_line_out) {
    size_t i;

    if (pattern == 0 || pattern[0] == '\0') {
        return -1;
    }

    for (i = start_line; i < line_count; ++i) {
        size_t end = line_end_offset(buffer, length, line_offsets[i]);
        char line[1024];
        size_t copy_len = end - line_offsets[i];
        if (copy_len >= sizeof(line)) {
            copy_len = sizeof(line) - 1U;
        }
        memcpy(line, buffer + line_offsets[i], copy_len);
        line[copy_len] = '\0';
        if (contains_case_insensitive(line, pattern)) {
            *match_line_out = i;
            return 0;
        }
    }

    return -1;
}

static int read_search_pattern(char *buffer, size_t buffer_size) {
    size_t length = 0U;
    char ch = '\0';

    if (rt_write_char(1, '/') != 0) {
        return -1;
    }

    while (platform_read(0, &ch, 1U) == 1) {
        if (ch == '\r' || ch == '\n') {
            break;
        }
        if ((ch == 127 || ch == '\b') && length > 0U) {
            length -= 1U;
            buffer[length] = '\0';
            (void)rt_write_cstr(1, "\b \b");
            continue;
        }
        if (ch == 27) {
            buffer[0] = '\0';
            return 1;
        }
        if (length + 1U < buffer_size) {
            buffer[length++] = ch;
            buffer[length] = '\0';
            (void)rt_write_char(1, ch);
        }
    }

    return 0;
}

static int render_page(const char *buffer,
                       size_t length,
                       const size_t *line_offsets,
                       size_t line_count,
                       size_t top_line,
                       unsigned int page_lines,
                       int show_numbers,
                       int color_mode,
                       const char *status_text) {
    size_t i;
    unsigned int shown = 0U;

    if (rt_write_cstr(1, "\033[H\033[J") != 0) {
        return -1;
    }

    for (i = top_line; i < line_count && shown < page_lines; ++i, ++shown) {
        size_t start = line_offsets[i];
        size_t end = line_end_offset(buffer, length, start);
        if (pager_write_line_with_number(buffer + start, end - start, (unsigned long long)(i + 1U), show_numbers) != 0) {
            return -1;
        }
    }

    if (status_text != 0 && status_text[0] != '\0') {
        if (rt_write_cstr(1, status_text) != 0) {
            return -1;
        }
    } else {
        pager_write_text(1, color_mode, TOOL_STYLE_BOLD_CYAN, "--Less--");
        if (rt_write_cstr(1, " (Space:fwd b:back j/k:line /:search n:next q:quit)") != 0) {
            return -1;
        }
    }

    return 0;
}

static int page_buffered(int fd, int show_numbers, const char *search_pattern, int color_mode) {
    static char buffer[LESS_BUFFER_CAPACITY];
    static size_t line_offsets[LESS_MAX_LINES];
    LessPager pager;
    size_t length = 0U;
    size_t line_count;
    size_t top_line = 0U;
    char last_search[PAGER_SEARCH_CAPACITY];
    char status[192];

    if (load_buffered_input(fd, buffer, sizeof(buffer), &length) != 0) {
        return -1;
    }

    line_count = build_line_index(buffer, length, line_offsets, LESS_MAX_LINES);
    if (line_count == 0U) {
        return 0;
    }

    last_search[0] = '\0';
    status[0] = '\0';
    pager_init(&pager, color_mode);

    if (search_pattern != 0 && search_pattern[0] != '\0') {
        size_t match_line = 0U;
        rt_copy_string(last_search, sizeof(last_search), search_pattern);
        if (search_forward(buffer, length, line_offsets, line_count, 0U, search_pattern, &match_line) == 0) {
            top_line = match_line;
        } else {
            rt_copy_string(status, sizeof(status), "\rPattern not found");
        }
    }

    if (!pager.interactive) {
        size_t i;
        for (i = top_line; i < line_count; ++i) {
            size_t start = line_offsets[i];
            size_t end = line_end_offset(buffer, length, start);
            if (pager_write_line_with_number(buffer + start, end - start, (unsigned long long)(i + 1U), show_numbers) != 0) {
                pager_finish(&pager);
                return -1;
            }
        }
        pager_finish(&pager);
        return 0;
    }

    for (;;) {
        char ch = '\0';

        if (render_page(buffer, length, line_offsets, line_count, top_line, pager.page_lines, show_numbers, pager.color_mode, status) != 0) {
            pager_finish(&pager);
            return -1;
        }
        status[0] = '\0';

        if (platform_read(0, &ch, 1U) <= 0) {
            break;
        }
        if (ch == 'q' || ch == 'Q') {
            break;
        }
        if (ch == ' ' || ch == 'f') {
            if (top_line + pager.page_lines < line_count) {
                top_line += pager.page_lines;
            }
            continue;
        }
        if (ch == 'b') {
            if (top_line > (size_t)pager.page_lines) {
                top_line -= pager.page_lines;
            } else {
                top_line = 0U;
            }
            continue;
        }
        if (ch == '\r' || ch == '\n' || ch == 'j') {
            if (top_line + 1U < line_count) {
                top_line += 1U;
            }
            continue;
        }
        if (ch == 'k') {
            if (top_line > 0U) {
                top_line -= 1U;
            }
            continue;
        }
        if (ch == '/') {
            char pattern[128];
            size_t match_line = 0U;
            pattern[0] = '\0';
            if (render_page(buffer, length, line_offsets, line_count, top_line, pager.page_lines, show_numbers, pager.color_mode, "") != 0) {
                pager_finish(&pager);
                return -1;
            }
            if (read_search_pattern(pattern, sizeof(pattern)) == 0 && pattern[0] != '\0') {
                rt_copy_string(last_search, sizeof(last_search), pattern);
                if (search_forward(buffer, length, line_offsets, line_count, top_line + 1U, pattern, &match_line) == 0 ||
                    search_forward(buffer, length, line_offsets, line_count, 0U, pattern, &match_line) == 0) {
                    top_line = match_line;
                } else {
                    rt_copy_string(status, sizeof(status), "\rPattern not found");
                }
            }
            continue;
        }
        if (ch == 'n' && last_search[0] != '\0') {
            size_t match_line = 0U;
            if (search_forward(buffer, length, line_offsets, line_count, top_line + 1U, last_search, &match_line) == 0 ||
                search_forward(buffer, length, line_offsets, line_count, 0U, last_search, &match_line) == 0) {
                top_line = match_line;
            } else {
                rt_copy_string(status, sizeof(status), "\rPattern not found");
            }
            continue;
        }
    }

    pager_finish(&pager);
    (void)rt_write_char(1, '\n');
    return 0;
}

int main(int argc, char **argv) {
    int arg_index = 1;
    int show_numbers = 0;
    int color_mode = TOOL_COLOR_AUTO;
    const char *search_pattern = 0;
    int path_count;
    int i;
    int exit_code = 0;

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
        if (text_starts_with(argv[arg_index], "--color=")) {
            if (tool_parse_color_mode(argv[arg_index] + 8, &color_mode) != 0) {
                tool_write_error("less", "invalid color mode: ", argv[arg_index] + 8);
                return 1;
            }
            arg_index += 1;
            continue;
        }
        if (text_starts_with(argv[arg_index], "+/")) {
            search_pattern = argv[arg_index] + 2;
            arg_index += 1;
            continue;
        }
        break;
    }

    tool_set_global_color_mode(color_mode);
    path_count = argc - arg_index;
    if (path_count <= 0) {
        return page_buffered(0, show_numbers, search_pattern, color_mode) == 0 ? 0 : 1;
    }

    for (i = arg_index; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            tool_write_error("less", "cannot open ", argv[i]);
            exit_code = 1;
            continue;
        }

        if (path_count > 1) {
            if (i > arg_index) {
                rt_write_char(1, '\n');
            }
            pager_write_text(1, color_mode, TOOL_STYLE_BOLD_CYAN, "==> ");
            rt_write_cstr(1, argv[i]);
            pager_write_text(1, color_mode, TOOL_STYLE_BOLD_CYAN, " <==\n");
        }

        if (page_buffered(fd, show_numbers, search_pattern, color_mode) != 0) {
            if (page_stream(fd, platform_isatty(0) != 0 && platform_isatty(1) != 0, show_numbers) != 0) {
                tool_write_error("less", "read error on ", argv[i]);
                exit_code = 1;
            }
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
