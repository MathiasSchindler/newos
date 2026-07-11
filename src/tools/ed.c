#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define ED_LINE_CAPACITY 512
#define ED_INPUT_CAPACITY 1024
#define ED_PATH_CAPACITY 512

typedef struct {
    char **lines;
    size_t count;
    size_t capacity;
    size_t current;
    char path[ED_PATH_CAPACITY];
    int modified;
} EditorBuffer;

typedef struct {
    EditorBuffer buffer;
    int valid;
} UndoState;

typedef struct {
    char data[ED_INPUT_CAPACITY];
    size_t start;
    size_t end;
    int eof;
} InputReader;

static void ed_trim_newline(char *text) {
    rt_trim_newline(text);
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

static void ed_buffer_clear(EditorBuffer *buffer) {
    size_t i;
    for (i = 0U; i < buffer->count; ++i) rt_free(buffer->lines[i]);
    rt_free(buffer->lines);
    buffer->lines = 0;
    buffer->count = 0U;
    buffer->capacity = 0U;
}

static int ed_reserve_lines(EditorBuffer *buffer, size_t needed) {
    size_t capacity;
    char **lines;

    if (needed <= buffer->capacity) return 0;
    capacity = buffer->capacity != 0U ? buffer->capacity : 16U;
    while (capacity < needed) {
        if (capacity > ((size_t)-1) / 2U) {
            capacity = needed;
            break;
        }
        capacity *= 2U;
    }
    lines = (char **)rt_realloc_array(buffer->lines, capacity, sizeof(buffer->lines[0]));
    if (lines == 0) return -1;
    buffer->lines = lines;
    buffer->capacity = capacity;
    return 0;
}

static char *ed_copy_line(const char *text) {
    size_t length = rt_strlen(text);
    char *copy = (char *)rt_malloc(length + 1U);
    if (copy != 0) memcpy(copy, text, length + 1U);
    return copy;
}

static int ed_insert_line(EditorBuffer *buffer, size_t index, const char *text) {
    char *copy;

    if (index > buffer->count || ed_reserve_lines(buffer, buffer->count + 1U) != 0) return -1;
    copy = ed_copy_line(text);
    if (copy == 0) return -1;
    if (index < buffer->count) {
        memmove(buffer->lines + index + 1U, buffer->lines + index, (buffer->count - index) * sizeof(buffer->lines[0]));
    }
    buffer->lines[index] = copy;
    buffer->count += 1U;
    return 0;
}

static int ed_append_line(EditorBuffer *buffer, const char *text) {
    return ed_insert_line(buffer, buffer->count, text);
}

static int ed_copy_buffer(EditorBuffer *dst, const EditorBuffer *src) {
    size_t i;

    rt_memset(dst, 0, sizeof(*dst));
    dst->current = src->current;
    dst->modified = src->modified;
    rt_copy_string(dst->path, sizeof(dst->path), src->path);
    if (ed_reserve_lines(dst, src->count) != 0) return -1;
    for (i = 0U; i < src->count; ++i) {
        if (ed_append_line(dst, src->lines[i]) != 0) {
            ed_buffer_clear(dst);
            return -1;
        }
    }
    return 0;
}

static int ed_save_undo(UndoState *undo, const EditorBuffer *buffer) {
    if (undo->valid) ed_buffer_clear(&undo->buffer);
    undo->valid = 0;
    if (ed_copy_buffer(&undo->buffer, buffer) != 0) return -1;
    undo->valid = 1;
    return 0;
}

static int ed_restore_undo(UndoState *undo, EditorBuffer *buffer) {
    EditorBuffer current;

    if (!undo->valid) {
        return -1;
    }
    current = *buffer;
    *buffer = undo->buffer;
    undo->buffer = current;
    undo->valid = 1;
    return 0;
}

static int ed_load_file(EditorBuffer *buffer, const char *path) {
    int fd;
    ToolRecordReader reader;
    ToolByteBuffer line;
    int result = 0;

    ed_buffer_clear(buffer);
    buffer->current = 0;
    buffer->modified = 0;
    rt_copy_string(buffer->path, sizeof(buffer->path), path);

    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }

    tool_record_reader_init(&reader, fd, '\n');
    tool_byte_buffer_init(&line);
    for (;;) {
        int has_line = 0;
        if (tool_record_reader_next_buffer(&reader, &line, &has_line) != 0) {
            result = -1;
            break;
        }
        if (!has_line) break;
        if (ed_append_line(buffer, (const char *)line.data) != 0) {
            result = -1;
            break;
        }
    }
    tool_byte_buffer_free(&line);
    platform_close(fd);
    if (result != 0) return -1;
    if (buffer->count > 0) {
        buffer->current = buffer->count;
    }
    return 0;
}

static int ed_read_file_after(EditorBuffer *buffer, size_t index, const char *path, unsigned long long *bytes_read_out) {
    int fd;
    ToolRecordReader reader;
    ToolByteBuffer line;
    size_t insert_at = index;
    unsigned long long total_bytes = 0ULL;
    int result = 0;

    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }

    tool_record_reader_init(&reader, fd, '\n');
    tool_byte_buffer_init(&line);
    for (;;) {
        int has_line = 0;
        if (tool_record_reader_next_buffer(&reader, &line, &has_line) != 0) {
            result = -1;
            break;
        }
        if (!has_line) break;
        total_bytes += (unsigned long long)line.size + 1ULL;
        if (ed_insert_line(buffer, insert_at, (const char *)line.data) != 0) {
            result = -1;
            break;
        }
        buffer->current = insert_at + 1U;
        insert_at += 1U;
    }

    tool_byte_buffer_free(&line);
    platform_close(fd);
    if (result != 0) return -1;
    if (bytes_read_out != 0) {
        *bytes_read_out = total_bytes;
    }
    buffer->modified = 1;
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
    {
        size_t i;
        for (i = start; i <= end; ++i) rt_free(buffer->lines[i]);
    }
    if (end + 1U < buffer->count) {
        memmove(buffer->lines + start, buffer->lines + end + 1U, (buffer->count - end - 1U) * sizeof(buffer->lines[0]));
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

        if (ed_insert_line(buffer, insert_at, line) != 0) return -1;
        buffer->current = insert_at + 1U;
        insert_at += 1U;
        buffer->modified = 1;
    }

    return 0;
}

static int ed_replace_text(const char *line, const char *old_text, const char *new_text, int global, char *out, size_t out_size, int *changed_out) {
    if (old_text[0] == '\0') {
        return -1;
    }
    return tool_regex_replace(old_text, new_text, line, 0, global, out, out_size, changed_out);
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

static int ed_parse_step_address(const char *text, size_t *pos, const EditorBuffer *buffer, size_t start, size_t *value_out) {
    unsigned long long step = 0ULL;

    if (text[*pos] != '~') {
        return 0;
    }
    *pos += 1U;
    while (text[*pos] >= '0' && text[*pos] <= '9') {
        step = step * 10ULL + (unsigned long long)(text[*pos] - '0');
        *pos += 1U;
    }
    if (step == 0ULL) {
        return -1;
    }
    *value_out = start;
    while (*value_out < buffer->count && (*value_out % (size_t)step) != 0U) {
        *value_out += 1U;
    }
    if (*value_out > buffer->count) {
        *value_out = buffer->count;
    }
    return 1;
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
        if (text[*pos] == '~') {
            int step_status = ed_parse_step_address(text, pos, buffer, first, &second);
            if (step_status < 0) {
                return -1;
            }
            has_second = step_status > 0;
        } else if (ed_parse_single_address(text, pos, buffer, &second, &has_second) != 0) {
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

static int ed_parse_delimited(const char *text, size_t *pos, char delimiter, char *out, size_t out_size) {
    size_t length = 0U;

    while (text[*pos] != '\0' && text[*pos] != delimiter) {
        if (text[*pos] == '\\' && text[*pos + 1U] != '\0') {
            if (text[*pos + 1U] != delimiter) {
                if (length + 1U >= out_size) {
                    return -1;
                }
                out[length++] = text[*pos];
            }
            if (length + 1U >= out_size) {
                return -1;
            }
            out[length++] = text[*pos + 1U];
            *pos += 2U;
            continue;
        }
        if (length + 1U >= out_size) {
            return -1;
        }
        out[length++] = text[*pos];
        *pos += 1U;
    }
    if (text[*pos] != delimiter) {
        return -1;
    }
    out[length] = '\0';
    *pos += 1U;
    return 0;
}

static int ed_apply_substitute(EditorBuffer *buffer, size_t start, size_t end, const char *expr) {
    char delimiter;
    char old_text[ED_LINE_CAPACITY];
    char new_text[ED_LINE_CAPACITY];
    size_t pos = 1;
    int global = 0;
    size_t i;

    if (expr[0] != 's' || expr[1] == '\0') {
        return -1;
    }

    delimiter = expr[pos++];
    if (ed_parse_delimited(expr, &pos, delimiter, old_text, sizeof(old_text)) != 0) {
        return -1;
    }
    if (ed_parse_delimited(expr, &pos, delimiter, new_text, sizeof(new_text)) != 0) {
        return -1;
    }

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
        size_t capacity = rt_strlen(buffer->lines[i]) + rt_strlen(new_text) + 64U;
        char *replaced = 0;
        int replace_result;

        if (capacity < 128U) capacity = 128U;
        for (;;) {
            replaced = (char *)rt_realloc(replaced, capacity);
            if (replaced == 0) return -1;
            replace_result = ed_replace_text(buffer->lines[i], old_text, new_text, global, replaced, capacity, &changed);
            if (replace_result == 0) break;
            if (capacity > ((size_t)-1) / 2U) {
                rt_free(replaced);
                return -1;
            }
            capacity *= 2U;
        }
        if (changed) {
            rt_free(buffer->lines[i]);
            buffer->lines[i] = replaced;
            buffer->current = i + 1U;
            buffer->modified = 1;
        } else {
            rt_free(replaced);
        }
    }

    return 0;
}

static int ed_apply_global(EditorBuffer *buffer, size_t start, size_t end, const char *expr) {
    char delimiter;
    char pattern[ED_LINE_CAPACITY];
    size_t pos = 1U;
    char command[ED_INPUT_CAPACITY];
    size_t command_len = 0U;
    size_t i;

    if (expr[0] != 'g' || expr[1] == '\0') {
        return -1;
    }
    delimiter = expr[pos++];
    if (ed_parse_delimited(expr, &pos, delimiter, pattern, sizeof(pattern)) != 0) {
        return -1;
    }
    while (expr[pos] == ' ' || expr[pos] == '\t') {
        pos += 1U;
    }
    if (expr[pos] == '\0') {
        command[command_len++] = 'p';
    } else {
        while (expr[pos] != '\0' && command_len + 1U < sizeof(command)) {
            command[command_len++] = expr[pos++];
        }
    }
    command[command_len] = '\0';

    i = start;
    while (i <= end && i <= buffer->count) {
        size_t match_start = 0U;
        size_t match_end = 0U;

        if (tool_regex_search(pattern, buffer->lines[i - 1U], 0, 0U, &match_start, &match_end)) {
            if (command[0] == 'p' && command[1] == '\0') {
                ed_print_range(buffer, i - 1U, i - 1U, 0);
                buffer->current = i;
            } else if (command[0] == 'n' && command[1] == '\0') {
                ed_print_range(buffer, i - 1U, i - 1U, 1);
                buffer->current = i;
            } else if (command[0] == 'd' && command[1] == '\0') {
                if (ed_delete_range(buffer, i - 1U, i - 1U) != 0) {
                    return -1;
                }
                if (end > 0U) {
                    end -= 1U;
                }
                continue;
            } else if (command[0] == 's') {
                if (ed_apply_substitute(buffer, i, i, command) != 0) {
                    return -1;
                }
            } else {
                return -1;
            }
        }
        i += 1U;
    }
    return 0;
}

static void ed_print_error(void) {
    rt_write_line(2, "?");
}

int main(int argc, char **argv) {
    EditorBuffer buffer;
    UndoState undo;
    InputReader reader;
    char command[ED_LINE_CAPACITY];

    rt_memset(&buffer, 0, sizeof(buffer));
    rt_memset(&undo, 0, sizeof(undo));
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

        if ((op == 'p' || op == 'n' || op == 'd' || op == 'c' || op == 's' || op == 'g') && !has_range) {
            if (buffer.current == 0 || buffer.current > buffer.count) {
                if (op == 'g') {
                    if (buffer.count == 0U) {
                        ed_print_error();
                        continue;
                    }
                    start = 1U;
                    end = buffer.count;
                    has_range = buffer.count > 0U;
                } else {
                    ed_print_error();
                    continue;
                }
            } else if (op == 'g') {
                start = 1U;
                end = buffer.count;
                has_range = buffer.count > 0U;
            } else {
                start = buffer.current;
                end = buffer.current;
                has_range = 1;
            }
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
            (void)ed_save_undo(&undo, &buffer);
            if (ed_delete_range(&buffer, start - 1U, end - 1U) != 0) {
                ed_print_error();
            }
        } else if (op == 'a') {
            size_t insert_after = has_range ? end : buffer.current;
            if (insert_after > buffer.count) {
                ed_print_error();
                continue;
            }
            (void)ed_save_undo(&undo, &buffer);
            if (ed_insert_after(&buffer, insert_after, &reader) != 0) {
                ed_print_error();
            }
        } else if (op == 'i') {
            size_t insert_before = has_range ? start - 1U : (buffer.current == 0 ? 0 : buffer.current - 1U);
            (void)ed_save_undo(&undo, &buffer);
            if (ed_insert_after(&buffer, insert_before, &reader) != 0) {
                ed_print_error();
            }
        } else if (op == 'c') {
            size_t insert_at = start - 1U;
            (void)ed_save_undo(&undo, &buffer);
            if (ed_delete_range(&buffer, start - 1U, end - 1U) != 0 || ed_insert_after(&buffer, insert_at, &reader) != 0) {
                ed_print_error();
            }
        } else if (op == 'r') {
            const char *path = buffer.path;
            unsigned long long bytes_read = 0ULL;
            size_t insert_after = has_range ? end : buffer.current;

            while (command[pos] != '\0' && command[pos] != ' ' && command[pos] != '\t') {
                pos += 1U;
            }
            while (command[pos] == ' ' || command[pos] == '\t') {
                pos += 1U;
            }
            if (command[pos] != '\0') {
                path = command + pos;
            }
            if (insert_after > buffer.count || path[0] == '\0') {
                ed_print_error();
                continue;
            }
            (void)ed_save_undo(&undo, &buffer);
            if (ed_read_file_after(&buffer, insert_after, path, &bytes_read) != 0) {
                ed_print_error();
            } else {
                rt_write_uint(1, bytes_read);
                rt_write_char(1, '\n');
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
            (void)ed_save_undo(&undo, &buffer);
            if (ed_apply_substitute(&buffer, start, end, command + pos) != 0) {
                ed_print_error();
            }
        } else if (op == 'g') {
            ed_save_undo(&undo, &buffer);
            if (ed_apply_global(&buffer, start, end, command + pos) != 0) {
                ed_print_error();
            }
        } else if (op == 'u') {
            if (ed_restore_undo(&undo, &buffer) != 0) {
                ed_print_error();
            }
        } else if (tool_starts_with(command + pos, "H")) {
            rt_write_line(1, "p n a i c d s g r u w q");
        } else {
            ed_print_error();
        }
    }

    return 0;
}
