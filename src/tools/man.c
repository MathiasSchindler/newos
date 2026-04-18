#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define MAN_PATH_CAPACITY 1024
#define MAN_ENTRY_CAPACITY 256
#define MAN_LINE_CAPACITY 1024
#define MAN_SCAN_BUFFER 4096

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

static char to_lower_ascii(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int contains_case_insensitive(const char *text, const char *needle) {
    size_t i;
    size_t needle_len = rt_strlen(needle);

    if (needle_len == 0U) {
        return 1;
    }

    for (i = 0; text[i] != '\0'; ++i) {
        size_t j = 0;
        while (needle[j] != '\0' && text[i + j] != '\0' &&
               to_lower_ascii(text[i + j]) == to_lower_ascii(needle[j])) {
            j += 1U;
        }
        if (j == needle_len) {
            return 1;
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

static int find_man_root(const ManContext *context, char *buffer, size_t buffer_size) {
    int is_dir = 0;
    char candidate[MAN_PATH_CAPACITY];

    rt_copy_string(candidate, sizeof(candidate), "man");
    if (platform_path_is_directory(candidate, &is_dir) == 0 && is_dir) {
        rt_copy_string(buffer, buffer_size, candidate);
        return 0;
    }

    if (tool_join_path(context->self_dir, "../man", candidate, sizeof(candidate)) == 0 &&
        platform_path_is_directory(candidate, &is_dir) == 0 && is_dir) {
        rt_copy_string(buffer, buffer_size, candidate);
        return 0;
    }

    if (tool_join_path(context->self_dir, "man", candidate, sizeof(candidate)) == 0 &&
        platform_path_is_directory(candidate, &is_dir) == 0 && is_dir) {
        rt_copy_string(buffer, buffer_size, candidate);
        return 0;
    }

    return -1;
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

static int flush_rendered_line(const char *line, int *in_code_block) {
    const char *text = line;

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

        if (text_starts_with(text, "- ") || text_starts_with(text, "* ")) {
            if (rt_write_cstr(1, "  * ") != 0) {
                return -1;
            }
            text += 2;
        }
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
    char buffer[MAN_SCAN_BUFFER + 1];
    int fd = platform_open_read(path);
    long bytes_read;

    if (fd < 0) {
        return 0;
    }

    bytes_read = platform_read(fd, buffer, MAN_SCAN_BUFFER);
    platform_close(fd);
    if (bytes_read <= 0) {
        return 0;
    }

    buffer[(size_t)bytes_read] = '\0';
    return contains_case_insensitive(buffer, keyword);
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
    static const char *sections[] = {"1", "5", "7"};
    char man_root[MAN_PATH_CAPACITY];
    int found = 0;
    size_t section_index;

    if (find_man_root(context, man_root, sizeof(man_root)) != 0) {
        tool_write_error("man", "cannot find manual directory", 0);
        return 1;
    }

    for (section_index = 0; section_index < sizeof(sections) / sizeof(sections[0]); ++section_index) {
        PlatformDirEntry entries[MAN_ENTRY_CAPACITY];
        size_t count = 0;
        int path_is_directory = 0;
        char section_dir[MAN_PATH_CAPACITY];
        size_t i;

        if (tool_join_path(man_root, sections[section_index], section_dir, sizeof(section_dir)) != 0) {
            continue;
        }
        if (platform_collect_entries(section_dir, 0, entries, MAN_ENTRY_CAPACITY, &count, &path_is_directory) != 0 || !path_is_directory) {
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
            if (write_search_result(sections[section_index], entries[i].name) != 0) {
                return 1;
            }
            found = 1;
        }
    }

    if (!found) {
        tool_write_error("man", "nothing appropriate for ", keyword);
        return 1;
    }

    return 0;
}

static int open_named_page(const ManContext *context, const char *section, const char *topic) {
    static const char *sections[] = {"1", "5", "7"};
    char man_root[MAN_PATH_CAPACITY];
    char page_path[MAN_PATH_CAPACITY];
    size_t i;

    if (find_man_root(context, man_root, sizeof(man_root)) != 0) {
        tool_write_error("man", "cannot find manual directory", 0);
        return 1;
    }

    if (section != 0) {
        if (find_page_in_section(man_root, section, topic, page_path, sizeof(page_path)) == 0) {
            return render_markdown_file(page_path) == 0 ? 0 : 1;
        }
    } else {
        for (i = 0; i < sizeof(sections) / sizeof(sections[0]); ++i) {
            if (find_page_in_section(man_root, sections[i], topic, page_path, sizeof(page_path)) == 0) {
                return render_markdown_file(page_path) == 0 ? 0 : 1;
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
