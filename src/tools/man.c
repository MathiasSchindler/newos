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

typedef struct {
    char self_dir[MAN_PATH_CAPACITY];
} ManContext;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-k KEYWORD] [-l FILE] [SECTION] TOPIC");
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

static int line_contains_char(const char *text, char target) {
    size_t i = 0;
    while (text[i] != '\0') {
        if (text[i] == target) {
            return 1;
        }
        i += 1U;
    }
    return 0;
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
            i += 1U;
            while (text[i] != '\0' && text[i] != ']') {
                if (buffer_append_char(buffer, buffer_size, &out, text[i]) != 0) {
                    return -1;
                }
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

static int render_table_row(const char *text) {
    const char *cursor = text;
    int first_cell = 1;

    while (*cursor != '\0') {
        const char *start;
        const char *end;
        char cell[MAN_LINE_CAPACITY];
        char rendered[MAN_LINE_CAPACITY];
        size_t length = 0;

        if (*cursor == '|') {
            cursor += 1;
        }
        start = cursor;
        while (*cursor != '\0' && *cursor != '|') {
            cursor += 1;
        }
        end = cursor;
        trim_range(&start, &end);
        while (start < end && length + 1U < sizeof(cell)) {
            cell[length++] = *start++;
        }
        cell[length] = '\0';

        if (cell[0] == '\0') {
            continue;
        }

        if (format_inline_markdown(cell, rendered, sizeof(rendered)) != 0) {
            return -1;
        }

        if (!first_cell && rt_write_cstr(1, "    ") != 0) {
            return -1;
        }
        if (rt_write_cstr(1, rendered) != 0) {
            return -1;
        }
        first_cell = 0;
    }

    return rt_write_char(1, '\n');
}

static int flush_rendered_line(const char *line, int *in_code_block) {
    const char *text = line;
    char rendered[MAN_LINE_CAPACITY];

    if (text_starts_with(text, "```")) {
        *in_code_block = !*in_code_block;
        return 0;
    }

    if (!*in_code_block) {
        while (*text == '#') {
            text += 1;
        }
        while (*text == ' ' || *text == '\t') {
            text += 1;
        }

        if (is_table_separator_line(text)) {
            return 0;
        }
        if (line_contains_char(text, '|')) {
            return render_table_row(text);
        }

        if (text_starts_with(text, "- ") || text_starts_with(text, "* ")) {
            if (rt_write_cstr(1, "  * ") != 0) {
                return -1;
            }
            text += 2;
        }
    }

    if (!*in_code_block && format_inline_markdown(text, rendered, sizeof(rendered)) == 0) {
        text = rendered;
    }

    if (rt_write_cstr(1, text) != 0 || rt_write_char(1, '\n') != 0) {
        return -1;
    }

    return 0;
}

static int render_markdown_file(const char *path) {
    char buffer[MAN_SCAN_BUFFER];
    char line[MAN_LINE_CAPACITY];
    size_t line_length = 0;
    int in_code_block = 0;
    int fd;
    long bytes_read;

    fd = platform_open_read(path);
    if (fd < 0) {
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
                line[line_length] = '\0';
                if (flush_rendered_line(line, &in_code_block) != 0) {
                    platform_close(fd);
                    return -1;
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
        platform_close(fd);
        return -1;
    }

    if (line_length > 0U) {
        line[line_length] = '\0';
        if (flush_rendered_line(line, &in_code_block) != 0) {
            platform_close(fd);
            return -1;
        }
    }

    platform_close(fd);
    return 0;
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
