#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define MAN_PATH_CAPACITY 1024
#define MAN_ENTRY_CAPACITY 256
#define MAN_LINE_CAPACITY 1024
#define MAN_SCAN_BUFFER 4096
#define MAN_ROOT_CAPACITY 16
#define MAN_RESULT_CAPACITY 512
#define MAN_RESULT_KEY_CAPACITY 320
#define MAN_TABLE_MAX_ROWS 64
#define MAN_TABLE_MAX_COLS 16
#define DEFAULT_PAGE_LINES 23
#define DEFAULT_PAGE_COLUMNS 80

typedef struct {
    char self_dir[MAN_PATH_CAPACITY];
} ManContext;

typedef struct {
    int interactive;
    int raw_mode_enabled;
    unsigned int page_lines;
    unsigned int lines_seen;
    unsigned int page_columns;
    unsigned int columns_seen;
    int color_mode;
    PlatformTerminalState saved_state;
} ManPager;

typedef struct {
    int in_code_block;
    int table_pending;
    int table_active;
    int table_has_header;
    size_t table_row_count;
    char pending_table_line[MAN_LINE_CAPACITY];
    char table_rows[MAN_TABLE_MAX_ROWS][MAN_LINE_CAPACITY];
} ManRenderState;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-k KEYWORD] [-l FILE] [--color[=WHEN]] [SECTION] TOPIC");
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

static unsigned int pager_page_columns(int *explicit_out) {
    const char *text = platform_getenv("COLUMNS");
    unsigned long long value = 0;
    unsigned int columns = 0U;

    if (explicit_out != 0) {
        *explicit_out = 0;
    }

    if (text != 0 && rt_parse_uint(text, &value) == 0 && value >= 16 && value < 2000) {
        if (explicit_out != 0) {
            *explicit_out = 1;
        }
        return (unsigned int)value;
    }

    if (platform_get_terminal_size(1, 0, &columns) == 0 && columns >= 16U && columns < 2000U) {
        return columns;
    }

    return DEFAULT_PAGE_COLUMNS;
}

static void pager_init(ManPager *pager) {
    int explicit_columns = 0;

    rt_memset(pager, 0, sizeof(*pager));
    pager->page_lines = pager_page_lines();
    pager->page_columns = pager_page_columns(&explicit_columns);
    pager->color_mode = tool_get_global_color_mode();
    pager->interactive = (platform_isatty(0) != 0 && platform_isatty(1) != 0);

    if (!pager->interactive && !explicit_columns) {
        pager->page_columns = 0U;
    }

    if (pager->interactive && platform_terminal_enable_raw_mode(0, &pager->saved_state) == 0) {
        pager->raw_mode_enabled = 1;
    }
}

static void pager_finish(ManPager *pager) {
    if (pager->raw_mode_enabled) {
        (void)platform_terminal_restore_mode(0, &pager->saved_state);
        pager->raw_mode_enabled = 0;
    }
}

static int pager_prompt(ManPager *pager) {
    char input[1];

    if (!pager->interactive || pager->page_lines == 0U) {
        return 0;
    }

    for (;;) {
        long bytes_read;

        tool_style_begin(1, pager->color_mode, TOOL_STYLE_BOLD_CYAN);
        if (rt_write_cstr(1, "--More--") != 0) {
            tool_style_end(1, pager->color_mode);
            return -1;
        }
        tool_style_end(1, pager->color_mode);

        bytes_read = platform_read(0, input, sizeof(input));
        (void)rt_write_cstr(1, "\r\033[K");

        if (bytes_read <= 0) {
            return 1;
        }

        if (input[0] == 'q' || input[0] == 'Q') {
            return 1;
        }

        if (input[0] == 'h' || input[0] == 'H' || input[0] == '?') {
            if (rt_write_cstr(1, "Keys: Enter/j line, Space/f page, q quit\n") != 0) {
                return -1;
            }
            continue;
        }

        if (input[0] == ' ' || input[0] == 'f' || input[0] == 'F') {
            pager->lines_seen = 0U;
        } else {
            pager->lines_seen = pager->page_lines > 0U ? (pager->page_lines - 1U) : 0U;
        }
        break;
    }

    return 0;
}

static int pager_write_all(ManPager *pager, const char *text, size_t length) {
    size_t index = 0U;

    while (index < length) {
        size_t next = index;
        unsigned int codepoint = 0U;
        unsigned int display_width;
        int prompt_result;

        if (pager->interactive && pager->page_lines > 0U && pager->lines_seen >= pager->page_lines) {
            prompt_result = pager_prompt(pager);
            if (prompt_result != 0) {
                return prompt_result;
            }
        }

        if (text[index] == '\033' && index + 1U < length && text[index + 1U] == '[') {
            size_t escape_end = index + 2U;

            while (escape_end < length) {
                char ch = text[escape_end++];
                if (ch >= '@' && ch <= '~') {
                    break;
                }
            }
            if (rt_write_all(1, text + index, escape_end - index) != 0) {
                return -1;
            }
            index = escape_end;
            continue;
        }

        if (rt_utf8_decode(text, length, &next, &codepoint) != 0 || next <= index) {
            next = index + 1U;
            codepoint = (unsigned char)text[index];
        }

        if (codepoint == '\n') {
            if (rt_write_all(1, text + index, next - index) != 0) {
                return -1;
            }
            pager->lines_seen += 1U;
            pager->columns_seen = 0U;
            index = next;
            continue;
        }

        display_width = (codepoint == '\t') ? (4U - (pager->columns_seen % 4U)) : rt_unicode_display_width(codepoint);

        if (pager->page_columns > 0U && pager->columns_seen > 0U && display_width > 0U &&
            pager->columns_seen + display_width > pager->page_columns) {
            if (rt_write_char(1, '\n') != 0) {
                return -1;
            }
            pager->lines_seen += 1U;
            pager->columns_seen = 0U;

            if (pager->interactive && pager->page_lines > 0U && pager->lines_seen >= pager->page_lines) {
                prompt_result = pager_prompt(pager);
                if (prompt_result != 0) {
                    return prompt_result;
                }
            }

            if (codepoint == ' ') {
                index = next;
                continue;
            }
        }

        if (rt_write_all(1, text + index, next - index) != 0) {
            return -1;
        }

        pager->columns_seen += display_width;
        index = next;
    }

    return 0;
}

static int pager_write_cstr(ManPager *pager, const char *text) {
    return pager_write_all(pager, text, rt_strlen(text));
}

static int pager_write_char(ManPager *pager, char ch) {
    return pager_write_all(pager, &ch, 1U);
}

static int pager_write_styled_cstr(ManPager *pager, int style, const char *text) {
    int use_style;
    int result;

    if (text == 0) {
        return 0;
    }
    use_style = (style != TOOL_STYLE_PLAIN) && tool_should_use_color_fd(1, pager->color_mode);
    if (use_style) {
        tool_style_begin(1, pager->color_mode, style);
    }
    result = pager_write_cstr(pager, text);
    if (use_style) {
        tool_style_end(1, pager->color_mode);
    }
    return result;
}

static int pager_write_styled_char(ManPager *pager, int style, char ch) {
    int use_style = (style != TOOL_STYLE_PLAIN) && tool_should_use_color_fd(1, pager->color_mode);
    int result;

    if (use_style) {
        tool_style_begin(1, pager->color_mode, style);
    }
    result = pager_write_char(pager, ch);
    if (use_style) {
        tool_style_end(1, pager->color_mode);
    }
    return result;
}

static int pager_write_repeated_char(ManPager *pager, int style, char ch, unsigned int count) {
    unsigned int i;

    for (i = 0U; i < count; ++i) {
        int result = pager_write_styled_char(pager, style, ch);
        if (result != 0) {
            return result;
        }
    }
    return 0;
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

static int text_ends_with(const char *text, const char *suffix) {
    size_t text_len = rt_strlen(text);
    size_t suffix_len = rt_strlen(suffix);
    size_t i;

    if (suffix_len > text_len) {
        return 0;
    }

    for (i = 0; i < suffix_len; ++i) {
        if (text[text_len - suffix_len + i] != suffix[i]) {
            return 0;
        }
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
            unsigned int lhs = 0;
            unsigned int rhs = 0;

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
            unsigned int ignored = 0;
            if (rt_utf8_decode(text, text_len, &pos, &ignored) != 0) {
                pos += 1U;
            }
        }
    }

    return 0;
}

static int is_section_name(const char *text) {
    size_t i = 0;

    if (text == 0 || text[0] == '\0') {
        return 0;
    }

    while (text[i] != '\0') {
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        i += 1U;
    }

    return 1;
}

static void set_self_dir(const char *argv0, char *buffer, size_t buffer_size) {
    size_t len;
    size_t i;

    if (argv0 == 0 || argv0[0] == '\0') {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    len = rt_strlen(argv0);
    if (len + 1U > buffer_size) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    memcpy(buffer, argv0, len + 1U);
    for (i = len; i > 0; --i) {
        if (buffer[i - 1U] == '/') {
            if (i == 1U) {
                buffer[1] = '\0';
            } else {
                buffer[i - 1U] = '\0';
            }
            return;
        }
    }

    rt_copy_string(buffer, buffer_size, ".");
}

static void add_root_if_unique(const char *path,
                               char roots[MAN_ROOT_CAPACITY][MAN_PATH_CAPACITY],
                               size_t *count_io) {
    char normalized[MAN_PATH_CAPACITY];
    const char *candidate = path;
    int is_dir = 0;
    size_t i;

    if (path == 0 || path[0] == '\0' || *count_io >= MAN_ROOT_CAPACITY) {
        return;
    }

    if (tool_canonicalize_path(path, 0, 0, normalized, sizeof(normalized)) == 0) {
        candidate = normalized;
    }

    if (platform_path_is_directory(candidate, &is_dir) != 0 || !is_dir) {
        return;
    }

    for (i = 0; i < *count_io; ++i) {
        if (rt_strcmp(roots[i], candidate) == 0) {
            return;
        }
    }

    rt_copy_string(roots[*count_io], MAN_PATH_CAPACITY, candidate);
    *count_io += 1U;
}

static void parse_root_list(const char *text,
                            char roots[MAN_ROOT_CAPACITY][MAN_PATH_CAPACITY],
                            size_t *count_io) {
    size_t i = 0;

    while (text != 0 && text[i] != '\0' && *count_io < MAN_ROOT_CAPACITY) {
        char part[MAN_PATH_CAPACITY];
        size_t length = 0;

        while (text[i] != '\0' && text[i] != ':') {
            if (length + 1U < sizeof(part)) {
                part[length++] = text[i];
            }
            i += 1U;
        }
        part[length] = '\0';
        if (text[i] == ':') {
            i += 1U;
        }

        if (part[0] == '\0') {
            continue;
        }

        add_root_if_unique(part, roots, count_io);
    }
}

static int should_emit_search_result(const char *section,
                                     const char *name,
                                     char seen[MAN_RESULT_CAPACITY][MAN_RESULT_KEY_CAPACITY],
                                     size_t *seen_count_io) {
    char key[MAN_RESULT_KEY_CAPACITY];
    size_t i;

    if (section == 0 || name == 0 || seen_count_io == 0) {
        return 1;
    }

    rt_copy_string(key, sizeof(key), section);
    rt_copy_string(key + rt_strlen(key), sizeof(key) - rt_strlen(key), "/");
    rt_copy_string(key + rt_strlen(key), sizeof(key) - rt_strlen(key), name);

    for (i = 0; i < *seen_count_io; ++i) {
        if (rt_strcmp(seen[i], key) == 0) {
            return 0;
        }
    }

    if (*seen_count_io < MAN_RESULT_CAPACITY) {
        rt_copy_string(seen[*seen_count_io], MAN_RESULT_KEY_CAPACITY, key);
        *seen_count_io += 1U;
    }

    return 1;
}

static int collect_man_roots(const ManContext *context,
                             char roots[MAN_ROOT_CAPACITY][MAN_PATH_CAPACITY],
                             size_t *count_out) {
    char candidate[MAN_PATH_CAPACITY];

    *count_out = 0U;
    parse_root_list(platform_getenv("MANPATH"), roots, count_out);

    rt_copy_string(candidate, sizeof(candidate), "man");
    parse_root_list(candidate, roots, count_out);

    if (tool_join_path(context->self_dir, "../man", candidate, sizeof(candidate)) == 0) {
        parse_root_list(candidate, roots, count_out);
    }

    if (tool_join_path(context->self_dir, "man", candidate, sizeof(candidate)) == 0) {
        parse_root_list(candidate, roots, count_out);
    }

    return *count_out > 0U ? 0 : -1;
}

static void build_page_filename(const char *topic, char *buffer, size_t buffer_size) {
    size_t len = 0;
    size_t i = 0;

    while (topic[i] != '\0' && len + 1U < buffer_size) {
        buffer[len++] = topic[i++];
    }
    buffer[len] = '\0';

    if (!text_ends_with(buffer, ".md") && len + 4U < buffer_size) {
        buffer[len++] = '.';
        buffer[len++] = 'm';
        buffer[len++] = 'd';
        buffer[len] = '\0';
    }
}

static int find_page_in_section(const char *man_root, const char *section, const char *topic, char *path_out, size_t path_size) {
    char section_dir[MAN_PATH_CAPACITY];
    char file_name[MAN_PATH_CAPACITY];

    if (tool_join_path(man_root, section, section_dir, sizeof(section_dir)) != 0) {
        return -1;
    }

    build_page_filename(topic, file_name, sizeof(file_name));
    if (tool_join_path(section_dir, file_name, path_out, path_size) != 0) {
        return -1;
    }

    return tool_path_exists(path_out) ? 0 : -1;
}

static int buffer_append_char(char *buffer, size_t buffer_size, size_t *length_io, char ch) {
    if (*length_io + 1U >= buffer_size) {
        return -1;
    }
    buffer[*length_io] = ch;
    *length_io += 1U;
    buffer[*length_io] = '\0';
    return 0;
}

static void trim_range(const char **start_io, const char **end_io) {
    while (*start_io < *end_io && ((**start_io == ' ') || (**start_io == '\t'))) {
        *start_io += 1;
    }
    while (*end_io > *start_io && (((*end_io)[-1] == ' ') || ((*end_io)[-1] == '\t'))) {
        *end_io -= 1;
    }
}

static unsigned int display_text_width(const char *text) {
    size_t text_length = rt_strlen(text);
    size_t index = 0U;
    unsigned int width = 0U;

    while (index < text_length) {
        size_t next = index;
        unsigned int codepoint = 0U;

        if (rt_utf8_decode(text, text_length, &next, &codepoint) != 0 || next <= index) {
            next = index + 1U;
            codepoint = (unsigned char)text[index];
        }
        if (codepoint == '\t') {
            width += 4U - (width % 4U);
        } else {
            width += rt_unicode_display_width(codepoint);
        }
        index = next;
    }

    return width;
}

static int is_table_separator_line(const char *text) {
    int saw_rule = 0;

    while (*text != '\0') {
        if (*text == '-' || *text == ':' || *text == '|' || *text == ' ' || *text == '\t') {
            if (*text == '-' || *text == ':') {
                saw_rule = 1;
            }
            text += 1;
            continue;
        }
        return 0;
    }

    return saw_rule;
}

static int is_table_candidate_line(const char *text) {
    int pipe_count = 0;
    int saw_nonspace = 0;
    size_t i = 0U;

    while (text[i] != '\0') {
        if (text[i] == '|') {
            pipe_count += 1;
        } else if (text[i] != ' ' && text[i] != '\t') {
            saw_nonspace = 1;
        }
        i += 1U;
    }

    return saw_nonspace && pipe_count >= 2;
}

static int format_inline_markdown(const char *text, char *buffer, size_t buffer_size) {
    size_t i = 0;
    size_t out = 0;

    buffer[0] = '\0';
    while (text[i] != '\0') {
        if ((text[i] == '*' && text[i + 1U] == '*') || (text[i] == '_' && text[i + 1U] == '_')) {
            i += 2U;
            continue;
        }
        if (text[i] == '`') {
            i += 1U;
            continue;
        }
        if (text[i] == '[') {
            size_t lookahead = i + 1U;

            while (text[lookahead] != '\0' && text[lookahead] != ']') {
                lookahead += 1U;
            }
            if (text[lookahead] == ']' && text[lookahead + 1U] == '(') {
                i += 1U;
                while (text[i] != '\0' && text[i] != ']') {
                    if (buffer_append_char(buffer, buffer_size, &out, text[i]) != 0) {
                        return -1;
                    }
                    i += 1U;
                }
                if (text[i] == ')') {
                    i += 1U;
                }
                if (text[i] == ']') {
                    i += 1U;
                }
                if (text[i] == '(') {
                    while (text[i] != '\0' && text[i] != ')') {
                        i += 1U;
                    }
                    if (text[i] == ')') {
                        i += 1U;
                    }
                }
                continue;
            }
        }
        if (text[i] == '*' || text[i] == '_') {
            i += 1U;
            continue;
        }
        if (buffer_append_char(buffer, buffer_size, &out, text[i]) != 0) {
            return -1;
        }
        i += 1U;
    }

    return 0;
}

static int parse_table_row_cells(const char *text,
                                 char cells[MAN_TABLE_MAX_COLS][MAN_LINE_CAPACITY],
                                 size_t *count_out) {
    const char *cursor = text;
    int at_row_start = 1;
    size_t count = 0U;

    while (1) {
        const char *start = cursor;
        const char *end;
        char raw[MAN_LINE_CAPACITY];
        size_t raw_length = 0U;
        int is_last_segment;

        while (*cursor != '\0' && *cursor != '|') {
            cursor += 1;
        }
        end = cursor;
        trim_range(&start, &end);
        is_last_segment = (*cursor == '\0');

        if (!(start == end && (at_row_start || is_last_segment))) {
            while (start < end && raw_length + 1U < sizeof(raw)) {
                raw[raw_length++] = *start++;
            }
            raw[raw_length] = '\0';

            if (count < MAN_TABLE_MAX_COLS) {
                if (format_inline_markdown(raw, cells[count], MAN_LINE_CAPACITY) != 0) {
                    return -1;
                }
                count += 1U;
            }
        }

        at_row_start = 0;
        if (*cursor == '\0') {
            break;
        }
        cursor += 1;
    }

    *count_out = count;
    return 0;
}

static void clear_table_state(ManRenderState *state) {
    state->table_pending = 0;
    state->table_active = 0;
    state->table_has_header = 0;
    state->table_row_count = 0U;
    state->pending_table_line[0] = '\0';
}

static int add_table_row(ManRenderState *state, const char *line) {
    if (state->table_row_count >= MAN_TABLE_MAX_ROWS) {
        return -1;
    }
    rt_copy_string(state->table_rows[state->table_row_count], MAN_LINE_CAPACITY, line);
    state->table_row_count += 1U;
    return 0;
}

static int pager_write_repeated_cstr(ManPager *pager, int style, const char *text, unsigned int count) {
    unsigned int i;

    for (i = 0U; i < count; ++i) {
        int result = pager_write_styled_cstr(pager, style, text);
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

static size_t skip_cell_leading_space(const char *text, size_t index) {
    size_t length = rt_strlen(text);

    while (index < length) {
        size_t next = index;
        unsigned int codepoint = 0U;

        if (rt_utf8_decode(text, length, &next, &codepoint) != 0 || next <= index) {
            next = index + 1U;
            codepoint = (unsigned char)text[index];
        }
        if (codepoint != ' ' && codepoint != '\t') {
            break;
        }
        index = next;
    }

    return index;
}

static int cell_text_next_segment(const char *text,
                                  size_t start_index,
                                  unsigned int max_width,
                                  char *buffer,
                                  size_t buffer_size,
                                  size_t *next_index_out,
                                  unsigned int *width_out) {
    size_t length = rt_strlen(text);
    size_t index = skip_cell_leading_space(text, start_index);
    size_t out = 0U;
    unsigned int width = 0U;
    int wrote_any = 0;

    if (buffer == 0 || buffer_size == 0U || next_index_out == 0 || width_out == 0) {
        return -1;
    }

    buffer[0] = '\0';
    *next_index_out = index;
    *width_out = 0U;

    if (max_width == 0U || index >= length) {
        return 0;
    }

    while (index < length) {
        size_t word_start = index;
        size_t word_end = index;
        unsigned int word_width = 0U;

        while (word_end < length) {
            size_t next = word_end;
            unsigned int codepoint = 0U;
            unsigned int glyph_width;

            if (rt_utf8_decode(text, length, &next, &codepoint) != 0 || next <= word_end) {
                next = word_end + 1U;
                codepoint = (unsigned char)text[word_end];
            }
            if (codepoint == ' ' || codepoint == '\t') {
                break;
            }
            glyph_width = rt_unicode_display_width(codepoint);
            word_width += glyph_width > 0U ? glyph_width : 1U;
            word_end = next;
        }

        if (!wrote_any) {
            if (word_width <= max_width) {
                size_t word_length = word_end - word_start;
                if (out + word_length + 1U >= buffer_size) {
                    return -1;
                }
                memcpy(buffer + out, text + word_start, word_length);
                out += word_length;
                buffer[out] = '\0';
                width = word_width;
                wrote_any = 1;
                index = word_end;
            } else {
                while (index < word_end) {
                    size_t next = index;
                    unsigned int codepoint = 0U;
                    unsigned int glyph_width;
                    size_t glyph_length;

                    if (rt_utf8_decode(text, length, &next, &codepoint) != 0 || next <= index) {
                        next = index + 1U;
                        codepoint = (unsigned char)text[index];
                    }
                    glyph_width = rt_unicode_display_width(codepoint);
                    if (glyph_width == 0U) {
                        glyph_width = 1U;
                    }
                    if (width > 0U && width + glyph_width > max_width) {
                        break;
                    }
                    glyph_length = next - index;
                    if (out + glyph_length + 1U >= buffer_size) {
                        return -1;
                    }
                    memcpy(buffer + out, text + index, glyph_length);
                    out += glyph_length;
                    buffer[out] = '\0';
                    width += glyph_width;
                    wrote_any = 1;
                    index = next;
                    if (width >= max_width) {
                        break;
                    }
                }
                *next_index_out = index;
                *width_out = width;
                return 0;
            }
        } else {
            if (width + 1U + word_width > max_width) {
                break;
            }
            if (out + 1U >= buffer_size) {
                return -1;
            }
            buffer[out++] = ' ';
            buffer[out] = '\0';
            width += 1U;

            {
                size_t word_length = word_end - word_start;
                if (out + word_length + 1U >= buffer_size) {
                    return -1;
                }
                memcpy(buffer + out, text + word_start, word_length);
                out += word_length;
                buffer[out] = '\0';
                width += word_width;
            }
            index = word_end;
        }

        index = skip_cell_leading_space(text, index);
    }

    *next_index_out = index;
    *width_out = width;
    return 0;
}

static void constrain_table_widths(unsigned int *widths, size_t column_count, unsigned int page_columns) {
    unsigned int total_width = 0U;
    unsigned int min_widths[MAN_TABLE_MAX_COLS];
    unsigned int available_content;
    size_t column;

    if (column_count == 0U || page_columns == 0U) {
        return;
    }

    if (page_columns <= (unsigned int)(column_count * 3U + 1U)) {
        for (column = 0U; column < column_count; ++column) {
            widths[column] = 1U;
        }
        return;
    }

    available_content = page_columns - (unsigned int)(column_count * 3U + 1U);
    for (column = 0U; column < column_count; ++column) {
        if (widths[column] == 0U) {
            widths[column] = 1U;
        }
        min_widths[column] = available_content >= column_count * 8U ? 4U : 1U;
        if (min_widths[column] > widths[column]) {
            min_widths[column] = widths[column];
        }
        if (min_widths[column] == 0U) {
            min_widths[column] = 1U;
        }
        total_width += widths[column];
    }

    while (total_width > available_content) {
        size_t widest_column = (size_t)-1;

        for (column = 0U; column < column_count; ++column) {
            if (widths[column] > min_widths[column] &&
                (widest_column == (size_t)-1 || widths[column] > widths[widest_column])) {
                widest_column = column;
            }
        }
        if (widest_column == (size_t)-1) {
            break;
        }
        widths[widest_column] -= 1U;
        total_width -= 1U;
    }
}

static int render_table_border(ManPager *pager,
                               const unsigned int *widths,
                               size_t column_count,
                               const char *left,
                               const char *middle,
                               const char *right) {
    size_t column;
    int result;

    result = pager_write_styled_cstr(pager, TOOL_STYLE_CYAN, left);
    if (result != 0) {
        return result;
    }
    for (column = 0U; column < column_count; ++column) {
        result = pager_write_repeated_cstr(pager, TOOL_STYLE_CYAN, "─", widths[column] + 2U);
        if (result != 0) {
            return result;
        }
        result = pager_write_styled_cstr(pager, TOOL_STYLE_CYAN, (column + 1U < column_count) ? middle : right);
        if (result != 0) {
            return result;
        }
    }
    return pager_write_char(pager, '\n');
}

static int render_table_row_framed(ManPager *pager,
                                   const char *line,
                                   const unsigned int *widths,
                                   size_t column_count,
                                   int is_header) {
    char cells[MAN_TABLE_MAX_COLS][MAN_LINE_CAPACITY];
    size_t positions[MAN_TABLE_MAX_COLS];
    size_t count = 0U;
    size_t column;

    if (parse_table_row_cells(line, cells, &count) != 0) {
        return -1;
    }

    for (column = 0U; column < MAN_TABLE_MAX_COLS; ++column) {
        positions[column] = 0U;
    }

    for (;;) {
        int result;
        int row_has_more = 0;

        result = pager_write_styled_cstr(pager, TOOL_STYLE_CYAN, "│");
        if (result != 0) {
            return result;
        }

        for (column = 0U; column < column_count; ++column) {
            const char *cell_text = column < count ? cells[column] : "";
            char segment[MAN_LINE_CAPACITY];
            unsigned int segment_width = 0U;
            segment[0] = '\0';
            unsigned int padding;
            unsigned int left_padding = 0U;
            size_t next_index = positions[column];

            if (cell_text[0] != '\0' &&
                cell_text_next_segment(cell_text,
                                       positions[column],
                                       widths[column],
                                       segment,
                                       sizeof(segment),
                                       &next_index,
                                       &segment_width) != 0) {
                return -1;
            }
            positions[column] = next_index;

            result = pager_write_char(pager, ' ');
            if (result != 0) {
                return result;
            }

            padding = widths[column] > segment_width ? (widths[column] - segment_width) : 0U;
            if (is_header && padding > 0U) {
                left_padding = padding / 2U;
            }
            if (left_padding > 0U) {
                result = pager_write_repeated_char(pager, TOOL_STYLE_PLAIN, ' ', left_padding);
                if (result != 0) {
                    return result;
                }
            }
            result = pager_write_styled_cstr(pager, is_header ? TOOL_STYLE_BOLD_CYAN : TOOL_STYLE_PLAIN, segment);
            if (result != 0) {
                return result;
            }
            if (padding > left_padding) {
                result = pager_write_repeated_char(pager, TOOL_STYLE_PLAIN, ' ', padding - left_padding);
                if (result != 0) {
                    return result;
                }
            }
            result = pager_write_char(pager, ' ');
            if (result != 0) {
                return result;
            }
            result = pager_write_styled_cstr(pager, TOOL_STYLE_CYAN, "│");
            if (result != 0) {
                return result;
            }

            if (skip_cell_leading_space(cell_text, positions[column]) < rt_strlen(cell_text)) {
                row_has_more = 1;
            }
        }

        result = pager_write_char(pager, '\n');
        if (result != 0) {
            return result;
        }
        if (!row_has_more) {
            break;
        }
    }

    return 0;
}

static int render_table_block(ManRenderState *state, ManPager *pager) {
    unsigned int widths[MAN_TABLE_MAX_COLS];
    size_t column_count = 0U;
    size_t row;
    int result;

    rt_memset(widths, 0, sizeof(widths));

    for (row = 0U; row < state->table_row_count; ++row) {
        char cells[MAN_TABLE_MAX_COLS][MAN_LINE_CAPACITY];
        size_t count = 0U;
        size_t column;

        if (parse_table_row_cells(state->table_rows[row], cells, &count) != 0) {
            return -1;
        }
        if (count > column_count) {
            column_count = count;
        }
        for (column = 0U; column < count; ++column) {
            unsigned int width = display_text_width(cells[column]);
            if (width > widths[column]) {
                widths[column] = width;
            }
        }
    }

    if (column_count == 0U) {
        clear_table_state(state);
        return 0;
    }

    constrain_table_widths(widths, column_count, pager->page_columns);

    result = render_table_border(pager, widths, column_count, "┌", "┬", "┐");
    if (result != 0) {
        return result;
    }

    for (row = 0U; row < state->table_row_count; ++row) {
        result = render_table_row_framed(pager, state->table_rows[row], widths, column_count, state->table_has_header && row == 0U);
        if (result != 0) {
            return result;
        }
        if (state->table_has_header && row == 0U) {
            result = render_table_border(pager, widths, column_count, "├", "┼", "┤");
            if (result != 0) {
                return result;
            }
        }
    }

    result = render_table_border(pager, widths, column_count, "└", "┴", "┘");
    clear_table_state(state);
    return result;
}

static int render_plain_line(const char *line, ManRenderState *state, ManPager *pager) {
    const char *text = line;
    char rendered[MAN_LINE_CAPACITY];
    int result;

    if (text_starts_with(text, "```")) {
        state->in_code_block = !state->in_code_block;
        return 0;
    }

    if (state->in_code_block) {
        result = pager_write_styled_cstr(pager, TOOL_STYLE_GREEN, "    ");
        if (result != 0) {
            return result;
        }
        result = pager_write_styled_cstr(pager, TOOL_STYLE_GREEN, text);
        if (result != 0) {
            return result;
        }
        return pager_write_char(pager, '\n');
    }

    while (*text == ' ' || *text == '\t') {
        text += 1;
    }

    if (*text == '#') {
        size_t level = 0U;
        size_t i;
        char underline_char;

        while (text[level] == '#') {
            level += 1U;
        }
        text += level;
        while (*text == ' ' || *text == '\t') {
            text += 1;
        }
        if (format_inline_markdown(text, rendered, sizeof(rendered)) != 0) {
            return -1;
        }
        underline_char = level <= 1U ? '=' : '-';
        result = pager_write_styled_cstr(pager, TOOL_STYLE_BOLD_CYAN, rendered);
        if (result != 0) {
            return result;
        }
        result = pager_write_char(pager, '\n');
        if (result != 0) {
            return result;
        }
        for (i = 0; rendered[i] != '\0'; ++i) {
            result = pager_write_styled_char(pager, TOOL_STYLE_CYAN, underline_char);
            if (result != 0) {
                return result;
            }
        }
        return pager_write_char(pager, '\n');
    }
    if (text_starts_with(text, "> ") || *text == '>') {
        if (*text == '>') {
            text += 1;
            if (*text == ' ') {
                text += 1;
            }
        }
        if (format_inline_markdown(text, rendered, sizeof(rendered)) != 0) {
            return -1;
        }
        result = pager_write_styled_cstr(pager, TOOL_STYLE_YELLOW, "  | ");
        if (result != 0) {
            return result;
        }
        result = pager_write_cstr(pager, rendered);
        if (result != 0) {
            return result;
        }
        return pager_write_char(pager, '\n');
    }

    if (text_starts_with(text, "- ") || text_starts_with(text, "* ")) {
        result = pager_write_styled_cstr(pager, TOOL_STYLE_BOLD_YELLOW, "  * ");
        if (result != 0) {
            return result;
        }
        text += 2;
    } else if (text[0] >= '0' && text[0] <= '9') {
        size_t i = 0U;
        while (text[i] >= '0' && text[i] <= '9') {
            i += 1U;
        }
        if ((text[i] == '.' || text[i] == ')') && text[i + 1U] == ' ') {
            result = pager_write_styled_cstr(pager, TOOL_STYLE_BOLD_YELLOW, "  ");
            if (result != 0) {
                return result;
            }
        }
    }

    if (format_inline_markdown(text, rendered, sizeof(rendered)) == 0) {
        text = rendered;
    }

    result = pager_write_cstr(pager, text);
    if (result != 0) {
        return result;
    }

    return pager_write_char(pager, '\n');
}

static int flush_rendered_line(const char *line, ManRenderState *state, ManPager *pager) {
    const char *text = line;

    while (*text == ' ' || *text == '\t') {
        text += 1;
    }

    if (!state->in_code_block && is_table_separator_line(text)) {
        if (state->table_pending) {
            state->table_active = 1;
            state->table_has_header = 1;
            if (add_table_row(state, state->pending_table_line) != 0) {
                return -1;
            }
            state->table_pending = 0;
            state->pending_table_line[0] = '\0';
        }
        return 0;
    }

    if (!state->in_code_block && is_table_candidate_line(text)) {
        if (state->table_active) {
            return add_table_row(state, text);
        }
        if (state->table_pending) {
            state->table_active = 1;
            if (add_table_row(state, state->pending_table_line) != 0 || add_table_row(state, text) != 0) {
                return -1;
            }
            state->table_pending = 0;
            state->pending_table_line[0] = '\0';
            return 0;
        }

        rt_copy_string(state->pending_table_line, sizeof(state->pending_table_line), text);
        state->table_pending = 1;
        return 0;
    }

    if (state->table_active) {
        int table_result = render_table_block(state, pager);
        if (table_result != 0) {
            return table_result;
        }
    } else if (state->table_pending) {
        int pending_result;
        state->table_pending = 0;
        pending_result = render_plain_line(state->pending_table_line, state, pager);
        state->pending_table_line[0] = '\0';
        if (pending_result != 0) {
            return pending_result;
        }
    }

    return render_plain_line(text, state, pager);
}

static int render_markdown_file(const char *path) {
    char buffer[MAN_SCAN_BUFFER];
    char line[MAN_LINE_CAPACITY];
    size_t line_length = 0;
    int fd;
    long bytes_read;
    int result = 0;
    ManPager pager;
    ManRenderState state;

    pager_init(&pager);
    rt_memset(&state, 0, sizeof(state));

    fd = platform_open_read(path);
    if (fd < 0) {
        pager_finish(&pager);
        return -1;
    }

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];

            if (ch == '\r') {
                continue;
            }

            if (ch == '\n') {
                int flush_result;
                line[line_length] = '\0';
                flush_result = flush_rendered_line(line, &state, &pager);
                if (flush_result > 0) {
                    platform_close(fd);
                    pager_finish(&pager);
                    return 0;
                }
                if (flush_result != 0) {
                    result = -1;
                    platform_close(fd);
                    pager_finish(&pager);
                    return result;
                }
                line_length = 0U;
                continue;
            }

            if (line_length + 1U < sizeof(line)) {
                line[line_length++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        result = -1;
    } else if (line_length > 0U) {
        int flush_result;
        line[line_length] = '\0';
        flush_result = flush_rendered_line(line, &state, &pager);
        if (flush_result < 0) {
            result = -1;
        }
    }

    if (result == 0 && state.table_active) {
        if (render_table_block(&state, &pager) != 0) {
            result = -1;
        }
    } else if (result == 0 && state.table_pending) {
        if (render_plain_line(state.pending_table_line, &state, &pager) != 0) {
            result = -1;
        }
    }

    platform_close(fd);
    pager_finish(&pager);
    return result;
}

static int file_contains_keyword(const char *path, const char *keyword) {
    char buffer[(MAN_SCAN_BUFFER * 2) + 1];
    size_t carry = 0;
    size_t keyword_len = rt_strlen(keyword);
    int fd = platform_open_read(path);
    long bytes_read;

    if (fd < 0) {
        return 0;
    }
    if (keyword_len == 0U) {
        platform_close(fd);
        return 1;
    }
    if (keyword_len > MAN_SCAN_BUFFER) {
        keyword_len = MAN_SCAN_BUFFER;
    }

    while ((bytes_read = platform_read(fd, buffer + carry, MAN_SCAN_BUFFER)) > 0) {
        size_t total = carry + (size_t)bytes_read;
        buffer[total] = '\0';

        if (contains_case_insensitive(buffer, keyword)) {
            platform_close(fd);
            return 1;
        }

        carry = keyword_len > 0U ? (keyword_len - 1U) : 0U;
        if (carry > total) {
            carry = total;
        }
        if (carry > 0U) {
            memmove(buffer, buffer + total - carry, carry);
        }
    }

    platform_close(fd);
    return 0;
}

static int write_search_result(const char *section, const char *name) {
    size_t length = rt_strlen(name);

    if (text_ends_with(name, ".md")) {
        length -= 3U;
    }

    if (rt_write_all(1, name, length) != 0 ||
        rt_write_cstr(1, " (") != 0 ||
        rt_write_cstr(1, section) != 0 ||
        rt_write_line(1, ")") != 0) {
        return -1;
    }

    return 0;
}

static int search_keyword(const ManContext *context, const char *keyword) {
    char roots[MAN_ROOT_CAPACITY][MAN_PATH_CAPACITY];
    char seen_results[MAN_RESULT_CAPACITY][MAN_RESULT_KEY_CAPACITY];
    size_t root_count = 0;
    size_t seen_count = 0;
    int found = 0;
    size_t root_index;

    if (collect_man_roots(context, roots, &root_count) != 0) {
        tool_write_error("man", "cannot find manual directory", 0);
        return 1;
    }

    for (root_index = 0; root_index < root_count; ++root_index) {
        PlatformDirEntry sections[MAN_ENTRY_CAPACITY];
        size_t section_count = 0;
        int path_is_directory = 0;
        size_t section_index;

        if (platform_collect_entries(roots[root_index], 0, sections, MAN_ENTRY_CAPACITY,
                                     &section_count, &path_is_directory) != 0 || !path_is_directory) {
            continue;
        }

        for (section_index = 0; section_index < section_count; ++section_index) {
            PlatformDirEntry entries[MAN_ENTRY_CAPACITY];
            size_t count = 0;
            int section_is_directory = 0;
            char section_dir[MAN_PATH_CAPACITY];
            size_t i;

            if (!sections[section_index].is_dir || sections[section_index].is_hidden) {
                continue;
            }
            if (tool_join_path(roots[root_index], sections[section_index].name, section_dir, sizeof(section_dir)) != 0) {
                continue;
            }
            if (platform_collect_entries(section_dir, 0, entries, MAN_ENTRY_CAPACITY, &count, &section_is_directory) != 0 || !section_is_directory) {
                continue;
            }

            for (i = 0; i < count; ++i) {
                char page_path[MAN_PATH_CAPACITY];

                if (entries[i].is_dir || !text_ends_with(entries[i].name, ".md")) {
                    continue;
                }
                if (tool_join_path(section_dir, entries[i].name, page_path, sizeof(page_path)) != 0) {
                    continue;
                }
                if (!contains_case_insensitive(entries[i].name, keyword) && !file_contains_keyword(page_path, keyword)) {
                    continue;
                }
                if (!should_emit_search_result(sections[section_index].name, entries[i].name, seen_results, &seen_count)) {
                    found = 1;
                    continue;
                }
                if (write_search_result(sections[section_index].name, entries[i].name) != 0) {
                    return 1;
                }
                found = 1;
            }
        }
    }

    if (!found) {
        tool_write_error("man", "nothing appropriate for ", keyword);
        return 1;
    }

    return 0;
}

static int open_named_page(const ManContext *context, const char *section, const char *topic) {
    char roots[MAN_ROOT_CAPACITY][MAN_PATH_CAPACITY];
    char page_path[MAN_PATH_CAPACITY];
    size_t root_count = 0;
    size_t root_index;

    if (collect_man_roots(context, roots, &root_count) != 0) {
        tool_write_error("man", "cannot find manual directory", 0);
        return 1;
    }

    for (root_index = 0; root_index < root_count; ++root_index) {
        if (section != 0) {
            if (find_page_in_section(roots[root_index], section, topic, page_path, sizeof(page_path)) == 0) {
                return render_markdown_file(page_path) == 0 ? 0 : 1;
            }
        } else {
            PlatformDirEntry sections[MAN_ENTRY_CAPACITY];
            size_t section_count = 0;
            int path_is_directory = 0;
            size_t i;

            if (platform_collect_entries(roots[root_index], 0, sections, MAN_ENTRY_CAPACITY,
                                         &section_count, &path_is_directory) != 0 || !path_is_directory) {
                continue;
            }

            for (i = 0; i < section_count; ++i) {
                if (!sections[i].is_dir || sections[i].is_hidden) {
                    continue;
                }
                if (find_page_in_section(roots[root_index], sections[i].name, topic, page_path, sizeof(page_path)) == 0) {
                    return render_markdown_file(page_path) == 0 ? 0 : 1;
                }
            }
        }
    }

    tool_write_error("man", "no manual entry for ", topic);
    return 1;
}

int main(int argc, char **argv) {
    ManContext context;
    const char *section = 0;
    const char *topic = 0;
    const char *literal_path = 0;
    const char *keyword = 0;
    int argi = 1;

    set_self_dir(argv[0], context.self_dir, sizeof(context.self_dir));
    tool_set_global_color_mode(TOOL_COLOR_AUTO);

    if (argc > 1 && rt_strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-k") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            keyword = argv[argi + 1];
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-l") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            literal_path = argv[argi + 1];
            argi += 2;
        } else if (rt_strcmp(argv[argi], "--color") == 0) {
            tool_set_global_color_mode(TOOL_COLOR_ALWAYS);
            argi += 1;
        } else if (text_starts_with(argv[argi], "--color=")) {
            int color_mode = TOOL_COLOR_AUTO;
            if (tool_parse_color_mode(argv[argi] + 8, &color_mode) != 0) {
                tool_write_error("man", "invalid color mode: ", argv[argi] + 8);
                return 1;
            }
            tool_set_global_color_mode(color_mode);
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (literal_path != 0) {
        if (render_markdown_file(literal_path) != 0) {
            tool_write_error("man", "cannot open ", literal_path);
            return 1;
        }
        return 0;
    }

    if (keyword != 0) {
        return search_keyword(&context, keyword);
    }

    if (argi < argc && is_section_name(argv[argi])) {
        section = argv[argi++];
    }
    if (argi < argc) {
        topic = argv[argi++];
    }

    if (topic == 0 || argi != argc) {
        print_usage(argv[0]);
        return 1;
    }

    return open_named_page(&context, section, topic);
}
