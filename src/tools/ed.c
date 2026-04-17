#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define ED_MAX_LINES 4096
#define ED_LINE_CAPACITY 512
#define ED_INPUT_CAPACITY 1024
#define ED_PATH_CAPACITY 512

typedef struct {
    char lines[ED_MAX_LINES][ED_LINE_CAPACITY];
    size_t count;
    size_t current;
    char path[ED_PATH_CAPACITY];
    int modified;
} EditorBuffer;

typedef struct {
    char data[ED_INPUT_CAPACITY];
    size_t start;
    size_t end;
    int eof;
} InputReader;

static void ed_trim_newline(char *text) {
    rt_trim_newline(text);
}

static int ed_starts_with(const char *text, const char *prefix) {
    size_t i = 0;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1;
    }

    return 1;
}

static int ed_read_line(InputReader *reader, char *line, size_t line_size) {
    size_t used = 0;

    if (line_size == 0) {
        return -1;
    }

    for (;;) {
        if (reader->start == reader->end) {
            long bytes_read;

            if (reader->eof) {
                if (used == 0) {
                    return 0;
                }
                line[used] = '\0';
                return 1;
            }

            bytes_read = platform_read(0, reader->data, sizeof(reader->data));
            if (bytes_read < 0) {
                return -1;
            }
            if (bytes_read == 0) {
                reader->eof = 1;
                continue;
            }

            reader->start = 0;
            reader->end = (size_t)bytes_read;
        }

        while (reader->start < reader->end) {
            char ch = reader->data[reader->start++];

            if (ch == '\n') {
                line[used] = '\0';
                return 1;
            }

            if (used + 1 >= line_size) {
                return -1;
            }

            line[used++] = ch;
        }
    }
}

static int ed_append_line(char lines[ED_MAX_LINES][ED_LINE_CAPACITY], size_t *count, const char *text) {
    size_t len = rt_strlen(text);

    if (*count >= ED_MAX_LINES) {
        return -1;
    }

    if (len >= ED_LINE_CAPACITY) {
        len = ED_LINE_CAPACITY - 1U;
    }

    memcpy(lines[*count], text, len);
    lines[*count][len] = '\0';
    *count += 1U;
    return 0;
}

static int ed_load_file(EditorBuffer *buffer, const char *path) {
    int fd;
    char chunk[512];
    char line[ED_LINE_CAPACITY];
    size_t line_len = 0;
    long bytes_read;

    buffer->count = 0;
    buffer->current = 0;
    buffer->modified = 0;
    rt_copy_string(buffer->path, sizeof(buffer->path), path);

    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                line[line_len] = '\0';
                if (ed_append_line(buffer->lines, &buffer->count, line) != 0) {
                    platform_close(fd);
                    return -1;
                }
                line_len = 0;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        platform_close(fd);
        return -1;
    }

    if (line_len > 0 || buffer->count == 0) {
        line[line_len] = '\0';
        if (!(buffer->count == 0 && line_len == 0)) {
            if (ed_append_line(buffer->lines, &buffer->count, line) != 0) {
                platform_close(fd);
                return -1;
            }
        }
    }

    platform_close(fd);
    if (buffer->count > 0) {
        buffer->current = buffer->count;
    }
    return 0;
}

static int ed_write_file(const EditorBuffer *buffer, const char *path) {
    int fd;
    size_t i;
    unsigned long long bytes_written = 0;

    fd = platform_open_write(path, 0644U);
    if (fd < 0) {
        return -1;
    }

    for (i = 0; i < buffer->count; ++i) {
        size_t len = rt_strlen(buffer->lines[i]);
        if (rt_write_all(fd, buffer->lines[i], len) != 0 || rt_write_char(fd, '\n') != 0) {
            platform_close(fd);
            return -1;
        }
        bytes_written += (unsigned long long)len + 1ULL;
    }

    platform_close(fd);
    rt_write_uint(1, bytes_written);
    rt_write_char(1, '\n');
    return 0;
}

static void ed_print_range(const EditorBuffer *buffer, size_t start, size_t end, int numbered) {
    size_t i;

    for (i = start; i <= end && i < buffer->count; ++i) {
        if (numbered) {
            rt_write_uint(1, (unsigned long long)(i + 1U));
            rt_write_char(1, '\t');
        }
        rt_write_line(1, buffer->lines[i]);
    }
}

static int ed_delete_range(EditorBuffer *buffer, size_t start, size_t end) {
    size_t remove_count;

    if (start > end || end >= buffer->count) {
        return -1;
    }

    remove_count = end - start + 1U;
    if (end + 1U < buffer->count) {
        memmove(buffer->lines[start], buffer->lines[end + 1U], (buffer->count - end - 1U) * sizeof(buffer->lines[0]));
    }

    buffer->count -= remove_count;
    buffer->modified = 1;

    if (buffer->count == 0) {
        buffer->current = 0;
    } else if (start >= buffer->count) {
        buffer->current = buffer->count;
    } else {
        buffer->current = start + 1U;
    }

    return 0;
}

static int ed_insert_after(EditorBuffer *buffer, size_t index, InputReader *reader) {
    char line[ED_LINE_CAPACITY];
    size_t insert_at = index;

    for (;;) {
        int status = ed_read_line(reader, line, sizeof(line));

        if (status <= 0) {
            return status < 0 ? -1 : 0;
        }

        if (rt_strcmp(line, ".") == 0) {
            break;
        }

        if (buffer->count >= ED_MAX_LINES || insert_at > buffer->count) {
            return -1;
        }

        if (insert_at < buffer->count) {
            memmove(buffer->lines[insert_at + 1U], buffer->lines[insert_at], (buffer->count - insert_at) * sizeof(buffer->lines[0]));
        }

        rt_copy_string(buffer->lines[insert_at], sizeof(buffer->lines[insert_at]), line);
        buffer->count += 1U;
        buffer->current = insert_at + 1U;
        insert_at += 1U;
        buffer->modified = 1;
    }

    return 0;
}

static int ed_replace_text(const char *line, const char *old_text, const char *new_text, int global, char *out, size_t out_size, int *changed_out) {
    size_t old_len = rt_strlen(old_text);
    size_t new_len = rt_strlen(new_text);
    size_t in_pos = 0;
    size_t out_pos = 0;
    int changed = 0;

    if (old_len == 0) {
        return -1;
    }

    while (line[in_pos] != '\0') {
        size_t i;
        int match = 1;

        for (i = 0; i < old_len; ++i) {
            if (line[in_pos + i] != old_text[i]) {
                match = 0;
                break;
            }
        }

        if (match) {
            if (out_pos + new_len + 1 > out_size) {
                return -1;
            }
            memcpy(out + out_pos, new_text, new_len);
            out_pos += new_len;
            in_pos += old_len;
            changed = 1;
            if (!global) {
                while (line[in_pos] != '\0') {
                    if (out_pos + 2 > out_size) {
                        return -1;
                    }
                    out[out_pos++] = line[in_pos++];
                }
                break;
            }
        } else {
            if (out_pos + 2 > out_size) {
                return -1;
            }
            out[out_pos++] = line[in_pos++];
        }
    }

    out[out_pos] = '\0';
    *changed_out = changed;
    return 0;
}

static int ed_parse_single_address(const char *text, size_t *pos, const EditorBuffer *buffer, size_t *value_out, int *has_value_out) {
    size_t value = 0;

    *has_value_out = 0;

    if (text[*pos] >= '1' && text[*pos] <= '9') {
        while (text[*pos] >= '0' && text[*pos] <= '9') {
            value = (value * 10U) + (size_t)(text[*pos] - '0');
            *pos += 1U;
        }
        *value_out = value;
        *has_value_out = 1;
        return 0;
    }

    if (text[*pos] == '.') {
        *value_out = buffer->current;
        *has_value_out = 1;
        *pos += 1U;
        return 0;
    }

    if (text[*pos] == '$') {
        *value_out = buffer->count;
        *has_value_out = 1;
        *pos += 1U;
        return 0;
    }

    return 0;
}

static int ed_parse_range(const char *text, const EditorBuffer *buffer, size_t *pos, size_t *start_out, size_t *end_out, int *has_range_out) {
    size_t first = 0;
    size_t second = 0;
    int has_first = 0;
    int has_second = 0;

    *has_range_out = 0;

    while (text[*pos] == ' ' || text[*pos] == '\t') {
        *pos += 1U;
    }

    if (text[*pos] == ',') {
        has_first = 1;
        first = 1;
    } else {
        if (ed_parse_single_address(text, pos, buffer, &first, &has_first) != 0) {
            return -1;
        }
    }

    if (text[*pos] == ',') {
        *pos += 1U;
        if (text[*pos] == '\0') {
            return -1;
        }
        if (!has_first) {
            first = 1;
            has_first = 1;
        }
        if (ed_parse_single_address(text, pos, buffer, &second, &has_second) != 0) {
            return -1;
        }
        if (!has_second) {
            second = buffer->count;
        }
        *start_out = first;
        *end_out = second;
        *has_range_out = 1;
        return 0;
    }

    if (has_first) {
        *start_out = first;
        *end_out = first;
        *has_range_out = 1;
    }

    return 0;
}

static int ed_apply_substitute(EditorBuffer *buffer, size_t start, size_t end, const char *expr) {
    char delimiter;
    char old_text[ED_LINE_CAPACITY];
    char new_text[ED_LINE_CAPACITY];
    char replaced[ED_LINE_CAPACITY];
    size_t pos = 1;
    size_t old_len = 0;
    size_t new_len = 0;
    int global = 0;
    size_t i;

    if (expr[0] != 's' || expr[1] == '\0') {
        return -1;
    }

    delimiter = expr[pos++];
    while (expr[pos] != '\0' && expr[pos] != delimiter && old_len + 1U < sizeof(old_text)) {
        old_text[old_len++] = expr[pos++];
    }
    if (expr[pos] != delimiter) {
        return -1;
    }
    old_text[old_len] = '\0';
    pos += 1U;

    while (expr[pos] != '\0' && expr[pos] != delimiter && new_len + 1U < sizeof(new_text)) {
        new_text[new_len++] = expr[pos++];
    }
    if (expr[pos] != delimiter) {
        return -1;
    }
    new_text[new_len] = '\0';
    pos += 1U;

    if (expr[pos] == 'g') {
        global = 1;
        pos += 1U;
    }

    if (expr[pos] != '\0') {
        return -1;
    }

    if (start == 0 || end == 0 || start > end || end > buffer->count) {
        return -1;
    }

    for (i = start - 1U; i < end; ++i) {
        int changed = 0;
        if (ed_replace_text(buffer->lines[i], old_text, new_text, global, replaced, sizeof(replaced), &changed) != 0) {
            return -1;
        }
        if (changed) {
            rt_copy_string(buffer->lines[i], sizeof(buffer->lines[i]), replaced);
            buffer->current = i + 1U;
            buffer->modified = 1;
        }
    }

    return 0;
}

static void ed_print_error(void) {
    rt_write_line(2, "?");
}

int main(int argc, char **argv) {
    EditorBuffer buffer;
    InputReader reader;
    char command[ED_LINE_CAPACITY];

    rt_memset(&buffer, 0, sizeof(buffer));
    rt_memset(&reader, 0, sizeof(reader));

    if (argc > 2) {
        tool_write_usage(tool_base_name(argv[0]), "[file]");
        return 1;
    }

    if (argc == 2) {
        if (ed_load_file(&buffer, argv[1]) != 0) {
            tool_write_error("ed", "cannot open ", argv[1]);
            return 1;
        }
    }

    while (ed_read_line(&reader, command, sizeof(command)) > 0) {
        size_t pos = 0;
        size_t start = 0;
        size_t end = 0;
        int has_range = 0;
        char op;

        ed_trim_newline(command);

        if (ed_parse_range(command, &buffer, &pos, &start, &end, &has_range) != 0) {
            ed_print_error();
            continue;
        }

        while (command[pos] == ' ' || command[pos] == '\t') {
            pos += 1U;
        }

        op = command[pos];
        if (op == '\0') {
            if (!has_range || end == 0 || end > buffer.count) {
                ed_print_error();
                continue;
            }
            buffer.current = end;
            rt_write_line(1, buffer.lines[end - 1U]);
            continue;
        }

        if ((op == 'p' || op == 'n' || op == 'd' || op == 'c' || op == 's') && !has_range) {
            if (buffer.current == 0 || buffer.current > buffer.count) {
                ed_print_error();
                continue;
            }
            start = buffer.current;
            end = buffer.current;
            has_range = 1;
        }

        if (has_range && (start == 0 || end == 0 || start > end || end > buffer.count)) {
            ed_print_error();
            continue;
        }

        if (op == 'p') {
            ed_print_range(&buffer, start - 1U, end - 1U, 0);
            buffer.current = end;
        } else if (op == 'n') {
            ed_print_range(&buffer, start - 1U, end - 1U, 1);
            buffer.current = end;
        } else if (op == 'd') {
            if (ed_delete_range(&buffer, start - 1U, end - 1U) != 0) {
                ed_print_error();
            }
        } else if (op == 'a') {
            size_t insert_after = has_range ? end : buffer.current;
            if (insert_after > buffer.count) {
                ed_print_error();
                continue;
            }
            if (ed_insert_after(&buffer, insert_after, &reader) != 0) {
                ed_print_error();
            }
        } else if (op == 'i') {
            size_t insert_before = has_range ? start - 1U : (buffer.current == 0 ? 0 : buffer.current - 1U);
            if (ed_insert_after(&buffer, insert_before, &reader) != 0) {
                ed_print_error();
            }
        } else if (op == 'c') {
            size_t insert_at = start - 1U;
            if (ed_delete_range(&buffer, start - 1U, end - 1U) != 0 || ed_insert_after(&buffer, insert_at, &reader) != 0) {
                ed_print_error();
            }
        } else if (op == 'w') {
            const char *path = buffer.path;
            while (command[pos] != '\0' && command[pos] != ' ' && command[pos] != '\t') {
                pos += 1U;
            }
            while (command[pos] == ' ' || command[pos] == '\t') {
                pos += 1U;
            }
            if (command[pos] != '\0') {
                path = command + pos;
                rt_copy_string(buffer.path, sizeof(buffer.path), path);
            }
            if (path[0] == '\0' || ed_write_file(&buffer, path) != 0) {
                ed_print_error();
            } else {
                buffer.modified = 0;
            }
        } else if (op == 'q' || op == 'Q') {
            return 0;
        } else if (op == 's') {
            if (ed_apply_substitute(&buffer, start, end, command + pos) != 0) {
                ed_print_error();
            }
        } else if (ed_starts_with(command + pos, "H")) {
            rt_write_line(1, "p n a i c d s w q");
        } else {
            ed_print_error();
        }
    }

    return 0;
}
