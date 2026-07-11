#include "concurrency.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#include <limits.h>

#if defined(NEWOS_TOOL_DEFAULT_COLOR_NEVER) && NEWOS_TOOL_DEFAULT_COLOR_NEVER
static int tool_global_color_mode = TOOL_COLOR_NEVER;
#else
static int tool_global_color_mode = TOOL_COLOR_AUTO;
#endif

static int env_value_is_nonzero(const char *value) {
    return value != 0 && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
}

static int tool_write_style_code(int fd, int style) {
    volatile int selected = style;
    if (selected == TOOL_STYLE_BOLD) return rt_write_cstr(fd, "1") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_RED) return rt_write_cstr(fd, "31") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_GREEN) return rt_write_cstr(fd, "32") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_YELLOW) return rt_write_cstr(fd, "33") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BLUE) return rt_write_cstr(fd, "34") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_MAGENTA) return rt_write_cstr(fd, "35") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_CYAN) return rt_write_cstr(fd, "36") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BOLD_RED) return rt_write_cstr(fd, "1;31") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BOLD_GREEN) return rt_write_cstr(fd, "1;32") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BOLD_YELLOW) return rt_write_cstr(fd, "1;33") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BOLD_BLUE) return rt_write_cstr(fd, "1;34") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BOLD_MAGENTA) return rt_write_cstr(fd, "1;35") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BOLD_CYAN) return rt_write_cstr(fd, "1;36") == 0 ? 1 : -1;
    if (selected == TOOL_STYLE_BOLD_WHITE) return rt_write_cstr(fd, "1;97") == 0 ? 1 : -1;
    return 0;
}

int tool_parse_color_mode(const char *text, int *mode_out) {
    if (text == 0 || mode_out == 0) {
        return -1;
    }

    if (rt_strcmp(text, "always") == 0 || rt_strcmp(text, "yes") == 0 || rt_strcmp(text, "force") == 0) {
        *mode_out = TOOL_COLOR_ALWAYS;
        return 0;
    }
    if (rt_strcmp(text, "auto") == 0) {
        *mode_out = TOOL_COLOR_AUTO;
        return 0;
    }
    if (rt_strcmp(text, "never") == 0 || rt_strcmp(text, "none") == 0 || rt_strcmp(text, "no") == 0) {
        *mode_out = TOOL_COLOR_NEVER;
        return 0;
    }

    return -1;
}

void tool_set_global_color_mode(int mode) {
    tool_global_color_mode = mode;
}

int tool_get_global_color_mode(void) {
    return tool_global_color_mode;
}

int tool_should_use_color_fd(int fd, int mode) {
    const char *term;
    const char *no_color;
    const char *clicolor;
    const char *clicolor_force;

    if (mode == TOOL_COLOR_ALWAYS) {
        return 1;
    }
    if (mode == TOOL_COLOR_NEVER) {
        return 0;
    }

    clicolor_force = platform_getenv("CLICOLOR_FORCE");
    if (env_value_is_nonzero(clicolor_force)) {
        return 1;
    }

    no_color = platform_getenv("NO_COLOR");
    if (no_color != 0) {
        return 0;
    }

    clicolor = platform_getenv("CLICOLOR");
    if (clicolor != 0 && clicolor[0] == '0' && clicolor[1] == '\0') {
        return 0;
    }

    term = platform_getenv("TERM");
    if (term != 0 && rt_strcmp(term, "dumb") == 0) {
        return 0;
    }

    return platform_isatty(fd) != 0;
}

void tool_style_begin(int fd, int mode, int style) {
    int wrote_code;

    if (!tool_should_use_color_fd(fd, mode) || style == TOOL_STYLE_PLAIN) {
        return;
    }

    if (rt_write_cstr(fd, "\033[") != 0) {
        return;
    }
    wrote_code = tool_write_style_code(fd, style);
    if (wrote_code == 1) {
        rt_write_char(fd, 'm');
    }
}

void tool_style_end(int fd, int mode) {
    if (tool_should_use_color_fd(fd, mode)) {
        rt_write_cstr(fd, "\033[0m");
    }
}

void tool_write_styled(int fd, int mode, int style, const char *text) {
    int use_style;

    if (text == 0) {
        return;
    }
    use_style = tool_should_use_color_fd(fd, mode) && style != TOOL_STYLE_PLAIN;
    if (use_style) {
        tool_style_begin(fd, mode, style);
    }
    rt_write_cstr(fd, text);
    if (use_style) {
        tool_style_end(fd, mode);
    }
}

void tool_byte_buffer_init(ToolByteBuffer *buffer) {
    if (buffer != 0) rt_memset(buffer, 0, sizeof(*buffer));
}

void tool_byte_buffer_free(ToolByteBuffer *buffer) {
    if (buffer == 0) return;
    rt_free(buffer->data);
    tool_byte_buffer_init(buffer);
}

int tool_byte_buffer_reserve(ToolByteBuffer *buffer, size_t needed) {
    size_t next_capacity;
    unsigned char *next;

    if (buffer == 0) return -1;
    if (needed <= buffer->capacity) return 0;
    next_capacity = buffer->capacity != 0U ? buffer->capacity : 128U;
    while (next_capacity < needed) {
        if (next_capacity > ((size_t)-1) / 2U) {
            next_capacity = needed;
            break;
        }
        next_capacity *= 2U;
    }
    next = (unsigned char *)rt_realloc(buffer->data, next_capacity);
    if (next == 0) return -1;
    buffer->data = next;
    buffer->capacity = next_capacity;
    return 0;
}

int tool_byte_buffer_reserve_extra(ToolByteBuffer *buffer, size_t extra) {
    if (buffer == 0 || extra > ((size_t)-1) - buffer->size) return -1;
    return tool_byte_buffer_reserve(buffer, buffer->size + extra);
}

int tool_byte_buffer_append(ToolByteBuffer *buffer, const void *data, size_t size) {
    if (size == 0U) return 0;
    if (data == 0 || tool_byte_buffer_reserve_extra(buffer, size) != 0) return -1;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 0;
}

int tool_byte_buffer_append_byte(ToolByteBuffer *buffer, unsigned int value) {
    unsigned char byte = (unsigned char)(value & 0xffU);
    return tool_byte_buffer_append(buffer, &byte, 1U);
}

int tool_byte_buffer_append_char(ToolByteBuffer *buffer, char ch) {
    return tool_byte_buffer_append(buffer, &ch, 1U);
}

int tool_byte_buffer_append_cstr(ToolByteBuffer *buffer, const char *text) {
    return tool_byte_buffer_append(buffer, text, rt_strlen(text));
}

int tool_byte_buffer_append_u16_be(ToolByteBuffer *buffer, unsigned int value) {
    unsigned char bytes[2];

    bytes[0] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[1] = (unsigned char)(value & 0xffU);
    return tool_byte_buffer_append(buffer, bytes, sizeof(bytes));
}

int tool_byte_buffer_append_u32_be(ToolByteBuffer *buffer, unsigned long long value) {
    unsigned char bytes[4];

    bytes[0] = (unsigned char)((value >> 24U) & 0xffU);
    bytes[1] = (unsigned char)((value >> 16U) & 0xffU);
    bytes[2] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[3] = (unsigned char)(value & 0xffU);
    return tool_byte_buffer_append(buffer, bytes, sizeof(bytes));
}

int tool_byte_buffer_terminate(ToolByteBuffer *buffer) {
    if (tool_byte_buffer_reserve_extra(buffer, 1U) != 0) return -1;
    buffer->data[buffer->size] = 0U;
    return 0;
}

int tool_byte_buffer_append_text(ToolByteBuffer *buffer, const char *text, size_t length) {
    if (tool_byte_buffer_append(buffer, text, length) != 0) return -1;
    return tool_byte_buffer_terminate(buffer);
}

void tool_array_init(ToolArray *array, size_t item_size) {
    if (array == 0) return;
    rt_memset(array, 0, sizeof(*array));
    array->item_size = item_size;
}

void tool_array_free(ToolArray *array) {
    size_t item_size;

    if (array == 0) return;
    item_size = array->item_size;
    rt_free(array->data);
    tool_array_init(array, item_size);
}

int tool_array_reserve(ToolArray *array, size_t needed) {
    size_t next_capacity;
    unsigned char *next;

    if (array == 0 || array->item_size == 0U) return -1;
    if (needed <= array->capacity) return 0;
    next_capacity = array->capacity != 0U ? array->capacity : 16U;
    while (next_capacity < needed) {
        if (next_capacity > ((size_t)-1) / 2U) {
            next_capacity = needed;
            break;
        }
        next_capacity *= 2U;
    }
    next = (unsigned char *)rt_realloc_array(array->data, next_capacity, array->item_size);
    if (next == 0) return -1;
    array->data = next;
    array->capacity = next_capacity;
    return 0;
}

void *tool_array_append(ToolArray *array) {
    void *item;

    if (array == 0 || array->count == (size_t)-1 || tool_array_reserve(array, array->count + 1U) != 0) return 0;
    item = array->data + array->count * array->item_size;
    rt_memset(item, 0, array->item_size);
    array->count += 1U;
    return item;
}

void *tool_array_get(ToolArray *array, size_t index) {
    if (array == 0 || index >= array->count) return 0;
    return array->data + index * array->item_size;
}

const void *tool_array_get_const(const ToolArray *array, size_t index) {
    if (array == 0 || index >= array->count) return 0;
    return array->data + index * array->item_size;
}

void tool_write_usage(const char *program_name, const char *usage_suffix) {
    if (tool_json_is_enabled()) {
        (void)tool_json_write_usage(program_name, usage_suffix);
        return;
    }
    tool_write_styled(2, tool_get_global_color_mode(), TOOL_STYLE_BOLD_CYAN, "Usage:");
    rt_write_char(2, ' ');
    rt_write_cstr(2, program_name);
    if (usage_suffix != 0 && usage_suffix[0] != '\0') {
        rt_write_char(2, ' ');
        rt_write_cstr(2, usage_suffix);
    }
    rt_write_char(2, '\n');
}

void tool_write_error(const char *tool_name, const char *message, const char *detail) {
    if (tool_json_is_enabled()) {
        (void)tool_json_write_diagnostic(tool_name, "error", message, detail);
        return;
    }
    tool_write_styled(2, tool_get_global_color_mode(), TOOL_STYLE_BOLD_RED, tool_name);
    tool_write_styled(2, tool_get_global_color_mode(), TOOL_STYLE_BOLD_RED, ": ");
    if (message != 0) {
        rt_write_cstr(2, message);
    }
    if (detail != 0) {
        rt_write_cstr(2, detail);
    }
    rt_write_char(2, '\n');
}

int tool_write_visible(int fd, const char *text, size_t length) {
    static const char hex[] = "0123456789abcdef";
    size_t i;

    if (text == 0) {
        return 0;
    }

    for (i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)text[i];

        if (ch == '\n') {
            if (rt_write_cstr(fd, "\\n") != 0) {
                return -1;
            }
        } else if (ch == '\r') {
            if (rt_write_cstr(fd, "\\r") != 0) {
                return -1;
            }
        } else if (ch == '\t') {
            if (rt_write_cstr(fd, "\\t") != 0) {
                return -1;
            }
        } else if (ch < 0x20U || ch == 0x7fU) {
            char escaped[4];

            escaped[0] = '\\';
            escaped[1] = 'x';
            escaped[2] = hex[(ch >> 4) & 0x0fU];
            escaped[3] = hex[ch & 0x0fU];
            if (rt_write_all(fd, escaped, sizeof(escaped)) != 0) {
                return -1;
            }
        } else if (rt_write_char(fd, (char)ch) != 0) {
            return -1;
        }
    }

    return 0;
}

int tool_write_visible_line(int fd, const char *text) {
    size_t length = text != 0 ? rt_strlen(text) : 0U;

    if (tool_write_visible(fd, text, length) != 0) {
        return -1;
    }
    return rt_write_char(fd, '\n');
}

int tool_write_record_text(int fd, const char *text, int zero_terminated) {
    if (zero_terminated) {
        return rt_write_all(fd, text, rt_strlen(text)) == 0 && rt_write_char(fd, '\0') == 0 ? 0 : -1;
    }
    return rt_write_line(fd, text);
}

int tool_write_file_all(const char *path, const unsigned char *data, size_t size) {
    int fd = platform_open_write(path, 0644U);
    size_t written = 0U;

    if (fd < 0) {
        return -1;
    }
    while (written < size) {
        long chunk = platform_write(fd, data + written, size - written);

        if (chunk <= 0) {
            platform_close(fd);
            return -1;
        }
        written += (size_t)chunk;
    }
    return platform_close(fd) == 0 ? 0 : -1;
}

int tool_write_file_all_report(const char *path, const unsigned char *data, size_t size, const char *tool_name) {
    if (tool_write_file_all(path, data, size) == 0) return 0;
    tool_write_error(tool_name, "cannot write: ", path);
    return -1;
}

int tool_store_fixed_record_text(char *records, size_t record_size, size_t max_records, size_t *count_io, const char *text, size_t text_length) {
    char *slot;
    size_t copy_length = text_length;

    if (records == 0 || record_size == 0U || count_io == 0 || *count_io >= max_records) return -1;
    if (copy_length >= record_size) copy_length = record_size - 1U;
    slot = records + (*count_io * record_size);
    if (copy_length != 0U) memcpy(slot, text, copy_length);
    slot[copy_length] = '\0';
    *count_io += 1U;
    return 0;
}

int tool_validate_absolute_program_path(const char *tool_name, const char *path) {
    if (path == 0 || path[0] != '/') {
        tool_write_error(tool_name, "refusing non-absolute program path: ", path != 0 ? path : "(null)");
        return -1;
    }
    return 0;
}

void tool_restore_terminal_mode_if_enabled(int fd, int *enabled_io, const PlatformTerminalState *state) {
    if (enabled_io != 0 && *enabled_io && state != 0) {
        (void)platform_terminal_restore_mode(fd, state);
        *enabled_io = 0;
    }
}

int tool_hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int tool_base64_value(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

int tool_bytes_equal(const unsigned char *left, const unsigned char *right, size_t size) {
    size_t index;

    if (size == 0U) {
        return 1;
    }
    if (left == 0 || right == 0) {
        return 0;
    }
    for (index = 0U; index < size; ++index) {
        if (left[index] != right[index]) {
            return 0;
        }
    }
    return 1;
}

int tool_bytes_equal_text(const unsigned char *bytes, const char *text, size_t size) {
    size_t index;

    if (size == 0U) {
        return 1;
    }
    if (bytes == 0 || text == 0) {
        return 0;
    }
    for (index = 0U; index < size; ++index) {
        if (bytes[index] != (unsigned char)text[index]) {
            return 0;
        }
    }
    return 1;
}

void tool_output_buffer_init(ToolOutputBuffer *output, int fd) {
    output->fd = fd;
    output->length = 0U;
}

int tool_output_buffer_flush(ToolOutputBuffer *output) {
    if (output->length == 0U) return 0;
    if (rt_write_all(output->fd, output->buffer, output->length) != 0) return -1;
    output->length = 0U;
    return 0;
}

int tool_output_buffer_write(ToolOutputBuffer *output, const char *text, size_t length) {
    size_t copied = 0U;
    if (length == 0U) return 0;
    if (text == 0) return -1;
    if (length >= sizeof(output->buffer)) {
        if (tool_output_buffer_flush(output) != 0) return -1;
        return rt_write_all(output->fd, text, length);
    }
    while (copied < length) {
        size_t available = sizeof(output->buffer) - output->length;
        size_t chunk;
        if (available == 0U) {
            if (tool_output_buffer_flush(output) != 0) return -1;
            available = sizeof(output->buffer);
        }
        chunk = length - copied;
        if (chunk > available) chunk = available;
        memcpy(output->buffer + output->length, text + copied, chunk);
        output->length += chunk;
        copied += chunk;
    }
    return 0;
}

int tool_output_buffer_write_char(ToolOutputBuffer *output, char ch) {
    return tool_output_buffer_write(output, &ch, 1U);
}

int tool_output_buffer_write_cstr(ToolOutputBuffer *output, const char *text) {
    return tool_output_buffer_write(output, text, rt_strlen(text));
}

void tool_record_reader_init(ToolRecordReader *reader, int fd, char delimiter) {
    reader->fd = fd;
    reader->delimiter = delimiter;
    reader->chunk_len = 0;
    reader->chunk_pos = 0;
    reader->eof = 0;
}

int tool_record_reader_next_buffer(ToolRecordReader *reader, ToolByteBuffer *record, int *has_record_out) {
    if (reader == 0 || record == 0 || has_record_out == 0) return -1;
    record->size = 0U;
    *has_record_out = 0;

    while (!reader->eof) {
        if (reader->chunk_pos >= reader->chunk_len) {
            reader->chunk_len = platform_read(reader->fd, reader->chunk, sizeof(reader->chunk));
            reader->chunk_pos = 0;
            if (reader->chunk_len < 0) return -1;
            if (reader->chunk_len == 0) {
                reader->eof = 1;
                break;
            }
        }

        while (reader->chunk_pos < reader->chunk_len) {
            char ch = reader->chunk[reader->chunk_pos++];

            if (ch == reader->delimiter) {
                if (tool_byte_buffer_terminate(record) != 0) return -1;
                *has_record_out = 1;
                return 0;
            }
            if (tool_byte_buffer_append_char(record, ch) != 0) return -1;
        }
    }

    if (record->size != 0U) {
        if (tool_byte_buffer_terminate(record) != 0) return -1;
        *has_record_out = 1;
    }
    return 0;
}

int tool_record_reader_next(ToolRecordReader *reader, char *record, size_t record_size, int *has_record_out) {
    size_t length = 0U;

    if (record_size == 0U || has_record_out == 0) {
        return -1;
    }

    while (!reader->eof) {
        if (reader->chunk_pos >= reader->chunk_len) {
            reader->chunk_len = platform_read(reader->fd, reader->chunk, sizeof(reader->chunk));
            reader->chunk_pos = 0;
            if (reader->chunk_len < 0) {
                return -1;
            }
            if (reader->chunk_len == 0) {
                reader->eof = 1;
                break;
            }
        }

        while (reader->chunk_pos < reader->chunk_len) {
            char ch = reader->chunk[reader->chunk_pos++];
            if (ch == reader->delimiter) {
                record[length] = '\0';
                *has_record_out = 1;
                return 0;
            }
            if (length + 1U >= record_size) {
                return -1;
            }
            record[length++] = ch;
        }
    }

    if (length > 0U) {
        record[length] = '\0';
        *has_record_out = 1;
        return 0;
    }

    *has_record_out = 0;
    return 0;
}

int tool_parse_escaped_string(const char *text, char *buffer, size_t buffer_size, size_t *length_out) {
    size_t in_index = 0;
    size_t out_index = 0;

    if (text == 0 || buffer == 0 || buffer_size == 0) {
        return -1;
    }

    while (text[in_index] != '\0') {
        char ch = text[in_index];

        if (ch == '\\' && text[in_index + 1U] != '\0') {
            in_index += 1U;
            ch = text[in_index];
            if (ch == 'n') {
                ch = '\n';
            } else if (ch == 'r') {
                ch = '\r';
            } else if (ch == 't') {
                ch = '\t';
            } else if (ch == 'f') {
                ch = '\f';
            } else if (ch == 'v') {
                ch = '\v';
            } else if (ch == '0') {
                ch = '\0';
            }
        }

        if (out_index + 1U >= buffer_size) {
            buffer[buffer_size - 1U] = '\0';
            return -1;
        }

        buffer[out_index++] = ch;
        in_index += 1U;
    }

    buffer[out_index] = '\0';
    if (length_out != 0) {
        *length_out = out_index;
    }
    return 0;
}

int tool_prompt_yes_no(const char *message, const char *path) {
    char ch = '\0';
    char answer = '\0';
    long bytes_read;

    if (message != 0) {
        rt_write_cstr(2, message);
    }
    if (path != 0) {
        rt_write_cstr(2, path);
    }
    rt_write_cstr(2, "? ");

    for (;;) {
        bytes_read = platform_read(0, &ch, 1U);
        if (bytes_read <= 0) {
            return 0;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            break;
        }
        if (answer == '\0' && ch != ' ' && ch != '\t') {
            answer = ch;
        }
    }

    return answer == 'y' || answer == 'Y';
}

size_t tool_buffer_append_char(char *buffer, size_t buffer_size, size_t length, char ch) {
    if (buffer_size == 0) {
        return 0;
    }

    if (length + 1 < buffer_size) {
        buffer[length] = ch;
        length += 1U;
        buffer[length] = '\0';
    } else {
        buffer[buffer_size - 1U] = '\0';
    }

    return length;
}

size_t tool_buffer_append_cstr(char *buffer, size_t buffer_size, size_t length, const char *text) {
    size_t i = 0;

    while (text != 0 && text[i] != '\0') {
        length = tool_buffer_append_char(buffer, buffer_size, length, text[i]);
        i += 1U;
    }

    return length;
}

size_t tool_buffer_append_uint(char *buffer, size_t buffer_size, size_t length, unsigned long long value) {
    char digits[32];

    rt_unsigned_to_string(value, digits, sizeof(digits));
    return tool_buffer_append_cstr(buffer, buffer_size, length, digits);
}

size_t tool_buffer_append_padded_base(char *buffer, size_t buffer_size, size_t length, unsigned long long value, unsigned int base, unsigned int width) {
    char digits[32];
    unsigned int i = 0U;
    const char *alphabet = "0123456789abcdef";

    if (base < 2U || base > 16U) {
        return length;
    }

    do {
        digits[i++] = alphabet[value % base];
        value /= base;
    } while (value > 0ULL && i < sizeof(digits));

    while (i < width) {
        length = tool_buffer_append_char(buffer, buffer_size, length, '0');
        width -= 1U;
    }
    while (i > 0U) {
        i -= 1U;
        length = tool_buffer_append_char(buffer, buffer_size, length, digits[i]);
    }
    return length;
}

int tool_output_flush_buffer(int fd, unsigned char *buffer, size_t *length_io) {
    if (*length_io == 0U) {
        return 0;
    }
    if (rt_write_all(fd, buffer, *length_io) != 0) {
        return -1;
    }
    *length_io = 0U;
    return 0;
}

int tool_output_append_buffer(int fd, unsigned char *buffer, size_t buffer_size, size_t *length_io, const unsigned char *data, size_t data_size) {
    if (data_size > buffer_size) {
        if (tool_output_flush_buffer(fd, buffer, length_io) != 0) {
            return -1;
        }
        return rt_write_all(fd, data, data_size);
    }
    if (*length_io + data_size > buffer_size && tool_output_flush_buffer(fd, buffer, length_io) != 0) {
        return -1;
    }
    memcpy(buffer + *length_io, data, data_size);
    *length_io += data_size;
    return 0;
}

int tool_write_all_fd(int fd, const unsigned char *data, size_t size) {
    size_t written = 0U;

    while (written < size) {
        long chunk = platform_write(fd, data + written, size - written);
        if (chunk <= 0) return -1;
        written += (size_t)chunk;
    }
    return 0;
}

int tool_parse_http_status(const char *headers) {
    size_t index = 0U;
    int code = 0;
    int saw_digit = 0;

    while (headers[index] != '\0' && headers[index] != ' ') index += 1U;
    while (headers[index] == ' ') index += 1U;
    while (headers[index] >= '0' && headers[index] <= '9') {
        saw_digit = 1;
        code = code * 10 + (int)(headers[index] - '0');
        index += 1U;
    }
    return saw_digit ? code : -1;
}

int tool_discard_input_bytes(int fd, unsigned long long count) {
    unsigned char buffer[8192];

    while (count > 0ULL) {
        size_t want = count > sizeof(buffer) ? sizeof(buffer) : (size_t)count;
        long got = platform_read(fd, buffer, want);
        if (got < 0) {
            return -1;
        }
        if (got == 0) {
            return 0;
        }
        count -= (unsigned long long)got;
    }
    return 0;
}

void tool_write_hex_value(int fd, unsigned long long value) {
    char digits[32];
    size_t count = 0U;

    rt_write_cstr(fd, "0x");
    do {
        unsigned int nibble = (unsigned int)(value & 0xfULL);
        digits[count++] = (char)(nibble < 10U ? ('0' + nibble) : ('a' + (nibble - 10U)));
        value >>= 4ULL;
    } while (value != 0ULL && count < sizeof(digits));

    while (count > 0U) {
        count -= 1U;
        rt_write_char(fd, digits[count]);
    }
}

int tool_write_hex_bytes(int fd, const unsigned char *bytes, size_t size) {
    static const char hex[] = "0123456789abcdef";
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (rt_write_char(fd, hex[(bytes[index] >> 4U) & 0x0fU]) != 0) return -1;
        if (rt_write_char(fd, hex[bytes[index] & 0x0fU]) != 0) return -1;
    }
    return 0;
}

int tool_http_connection_connect_timeout(ToolHttpConnection *connection, const char *host, unsigned int port, int use_tls, unsigned int timeout_milliseconds) {
    rt_memset(connection, 0, sizeof(*connection));
    connection->socket_fd = -1;
    connection->use_tls = use_tls;
    if (connection->use_tls) {
        if (platform_tls_connect_timeout(&connection->tls, host, port, timeout_milliseconds) != 0) {
            return -1;
        }
        connection->socket_fd = connection->tls.socket_fd;
        return 0;
    }
    return platform_connect_tcp(host, port, &connection->socket_fd);
}

int tool_http_connection_connect(ToolHttpConnection *connection, const char *host, unsigned int port, int use_tls) {
    return tool_http_connection_connect_timeout(connection, host, port, use_tls, 30000U);
}

unsigned int tool_http_default_port(int use_tls) {
    return use_tls ? 443U : 80U;
}

int tool_http_connection_fd(const ToolHttpConnection *connection) {
    return connection->use_tls ? connection->tls.socket_fd : connection->socket_fd;
}

long tool_http_connection_read(ToolHttpConnection *connection, void *buffer, size_t count) {
    return connection->use_tls ? platform_tls_read(&connection->tls, buffer, count) : platform_read(connection->socket_fd, buffer, count);
}

int tool_http_connection_write_all(ToolHttpConnection *connection, const void *buffer, size_t count) {
    const unsigned char *bytes = (const unsigned char *)buffer;
    size_t written = 0U;

    while (written < count) {
        long result = connection->use_tls ? platform_tls_write(&connection->tls, bytes + written, count - written) : platform_write(connection->socket_fd, bytes + written, count - written);
        if (result <= 0) {
            return -1;
        }
        written += (size_t)result;
    }
    return 0;
}

void tool_http_connection_close(ToolHttpConnection *connection) {
    if (connection->use_tls) {
        platform_tls_close(&connection->tls);
    } else if (connection->socket_fd >= 0) {
        (void)platform_close(connection->socket_fd);
    }
    connection->socket_fd = -1;
}

void tool_write_padding(int fd, size_t count) {
    (void)tool_write_repeated_char(fd, ' ', count);
}

int tool_write_repeated_char(int fd, char ch, size_t count) {
    char buffer[64];
    size_t index;

    for (index = 0U; index < sizeof(buffer); ++index) {
        buffer[index] = ch;
    }
    while (count > 0U) {
        size_t chunk = count > sizeof(buffer) ? sizeof(buffer) : count;
        if (rt_write_all(fd, buffer, chunk) != 0) {
            return -1;
        }
        count -= chunk;
    }
    return 0;
}

void tool_format_unsigned_base(unsigned long long value, unsigned int base, int uppercase, char *buffer, size_t buffer_size) {
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char scratch[64];
    size_t length = 0U;
    size_t index;

    if (buffer_size == 0U) {
        return;
    }
    if (base < 2U || base > 16U) {
        buffer[0] = '\0';
        return;
    }
    if (value == 0ULL) {
        if (buffer_size > 1U) {
            buffer[0] = '0';
            buffer[1] = '\0';
        } else {
            buffer[0] = '\0';
        }
        return;
    }
    while (value != 0ULL && length + 1U < sizeof(scratch)) {
        scratch[length++] = digits[value % base];
        value /= base;
    }
    for (index = 0U; index < length && index + 1U < buffer_size; ++index) {
        buffer[index] = scratch[length - 1U - index];
    }
    buffer[index] = '\0';
}

void tool_write_percent_2(int fd, unsigned long long value, unsigned long long total) {
    unsigned long long scaled;

    if (total == 0ULL) {
        rt_write_cstr(fd, "0.00");
        return;
    }
    scaled = (value * 10000ULL) / total;
    rt_write_uint(fd, scaled / 100ULL);
    rt_write_char(fd, '.');
    rt_write_char(fd, (char)('0' + ((scaled / 10ULL) % 10ULL)));
    rt_write_char(fd, (char)('0' + (scaled % 10ULL)));
}

void tool_write_labeled_text_line(int fd, const char *label, const char *value) {
    if (value == 0 || value[0] == '\0') return;
    rt_write_cstr(fd, label);
    rt_write_cstr(fd, ": ");
    rt_write_line(fd, value);
}

void tool_write_labeled_uint_line(int fd, const char *label, unsigned long long value) {
    rt_write_cstr(fd, label);
    rt_write_cstr(fd, ": ");
    rt_write_uint(fd, value);
    rt_write_char(fd, '\n');
}

unsigned int tool_worker_count_from_env(const char *env_name, unsigned int default_max_workers) {
    const char *value_text = platform_getenv(env_name);
    unsigned long long value;
    unsigned int platform_width;

    if (value_text == 0 || value_text[0] == '\0') {
        platform_width = platform_worker_thread_count();
        if (platform_width == 0U) return 0U;
        return platform_width > default_max_workers ? default_max_workers : platform_width;
    }
    if (rt_parse_uint(value_text, &value) != 0) {
        return default_max_workers;
    }
    if (value > RT_TASK_POOL_MAX_WORKERS) {
        return RT_TASK_POOL_MAX_WORKERS;
    }
    return (unsigned int)value;
}

int tool_stream_from_line(int input_fd, int output_fd, unsigned long long start_line) {
    char buffer[8192];
    long bytes_read;
    unsigned long long current_line = 1ULL;
    int writing = 0;

    if (start_line <= 1ULL) {
        start_line = 1ULL;
    }

    while ((bytes_read = platform_read(input_fd, buffer, sizeof(buffer))) > 0) {
        long index;

        if (writing) {
            if (rt_write_all(output_fd, buffer, (size_t)bytes_read) != 0) {
                return -1;
            }
            continue;
        }

        for (index = 0; index < bytes_read; ++index) {
            if (current_line >= start_line) {
                if (rt_write_all(output_fd, buffer + index, (size_t)(bytes_read - index)) != 0) {
                    return -1;
                }
                writing = 1;
                break;
            }

            if (buffer[index] == '\n') {
                current_line += 1ULL;
            }
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

int tool_stream_from_byte(int input_fd, int output_fd, unsigned long long start_byte) {
    char buffer[8192];
    long bytes_read;
    unsigned long long current_byte = 1ULL;
    int writing = 0;

    if (start_byte <= 1ULL) {
        start_byte = 1ULL;
    }

    while ((bytes_read = platform_read(input_fd, buffer, sizeof(buffer))) > 0) {
        size_t start = 0U;

        if (!writing) {
            if (current_byte + (unsigned long long)bytes_read <= start_byte) {
                current_byte += (unsigned long long)bytes_read;
                continue;
            }
            if (start_byte > current_byte) {
                start = (size_t)(start_byte - current_byte);
            }
            writing = 1;
        }
        if (rt_write_all(output_fd, buffer + start, (size_t)bytes_read - start) != 0) {
            return -1;
        }
        current_byte += (unsigned long long)bytes_read;
    }

    return bytes_read < 0 ? -1 : 0;
}

unsigned int tool_pager_page_lines(unsigned int default_lines) {
    const char *text = platform_getenv("LINES");
    unsigned long long value = 0;
    unsigned int rows = 0U;

    if (text != 0 && rt_parse_uint(text, &value) == 0 && value > 1ULL && value < 1000ULL) {
        return (unsigned int)(value - 1ULL);
    }

    if (platform_get_terminal_size(1, &rows, 0) == 0 && rows > 1U && rows < 1000U) {
        return rows - 1U;
    }

    return default_lines;
}

void tool_format_size(unsigned long long value, int human_readable, char *buffer, size_t buffer_size) {
    static const char units[] = { 'B', 'K', 'M', 'G', 'T', 'P' };
    size_t unit_index = 0;
    unsigned long long scaled = value;
    unsigned long long remainder = 0;
    char digits[32];
    size_t length = 0;

    if (buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    if (!human_readable) {
        rt_unsigned_to_string(value, buffer, buffer_size);
        return;
    }

    while (scaled >= 1024ULL && unit_index + 1U < sizeof(units)) {
        remainder = scaled % 1024ULL;
        scaled /= 1024ULL;
        unit_index += 1U;
    }

    rt_unsigned_to_string(scaled, digits, sizeof(digits));
    length = tool_buffer_append_cstr(buffer, buffer_size, length, digits);

    if (unit_index > 0U && scaled < 10ULL && remainder != 0ULL) {
        unsigned long long tenths = (remainder * 10ULL) / 1024ULL;
        length = tool_buffer_append_char(buffer, buffer_size, length, '.');
        length = tool_buffer_append_char(buffer, buffer_size, length, (char)('0' + (tenths > 9ULL ? 9ULL : tenths)));
    }

    (void)tool_buffer_append_char(buffer, buffer_size, length, units[unit_index]);
}

void tool_format_block_size(unsigned long long value, int human_readable, unsigned long long block_size, char *buffer, size_t buffer_size) {
    if (human_readable) {
        tool_format_size(value, 1, buffer, buffer_size);
        return;
    }

    if (block_size > 1ULL) {
        unsigned long long scaled = (value == 0ULL) ? 0ULL : ((value + block_size - 1ULL) / block_size);
        rt_unsigned_to_string(scaled, buffer, buffer_size);
        return;
    }

    rt_unsigned_to_string(value, buffer, buffer_size);
}

int tool_symbol_type_is_function(const char *type) {
    return rt_strcmp(type, "T") == 0 || rt_strcmp(type, "t") == 0 ||
           rt_strcmp(type, "W") == 0 || rt_strcmp(type, "w") == 0;
}

const char *tool_display_symbol_name(const char *name) {
    if (name != 0 && name[0] == '_' && name[1] != '\0') {
        return name + 1;
    }
    return name;
}

const char *tool_newlinker_text_symbol_name(const char *section) {
    const char *name;

    if (!tool_starts_with(section, ".text.")) {
        return 0;
    }
    name = section + 6;
    if (tool_starts_with(name, "startup.")) {
        name += 8;
    } else if (tool_starts_with(name, "unlikely.")) {
        name += 9;
    } else if (tool_starts_with(name, "hot.")) {
        name += 4;
    }
    return name[0] == '\0' ? 0 : name;
}

int tool_parse_uint_arg(const char *text, unsigned long long *value_out, const char *tool_name, const char *what) {
    if (text == 0 || rt_parse_uint(text, value_out) != 0) {
        tool_write_error(tool_name, "invalid ", what);
        return -1;
    }
    return 0;
}

int tool_parse_int_arg(const char *text, long long *value_out, const char *tool_name, const char *what) {
    unsigned long long magnitude = 0;
    int negative = 0;

    if (text == 0 || value_out == 0 || text[0] == '\0') {
        tool_write_error(tool_name, "invalid ", what);
        return -1;
    }

    if (text[0] == '-') {
        negative = 1;
        text += 1;
    } else if (text[0] == '+') {
        text += 1;
    }

    if (text[0] == '\0' || rt_parse_uint(text, &magnitude) != 0) {
        tool_write_error(tool_name, "invalid ", what);
        return -1;
    }

    *value_out = negative ? -(long long)magnitude : (long long)magnitude;
    return 0;
}

int tool_parse_size_value(const char *text, unsigned long long *value_out) {
    char digits[32];
    size_t len = 0U;
    unsigned long long value;
    unsigned long long multiplier = 1ULL;
    char suffix;

    if (text == 0 || text[0] == '\0' || value_out == 0) {
        return -1;
    }
    while (text[len] >= '0' && text[len] <= '9') {
        if (len + 1U >= sizeof(digits)) {
            return -1;
        }
        digits[len] = text[len];
        len += 1U;
    }
    if (len == 0U) {
        return -1;
    }
    digits[len] = '\0';
    if (rt_parse_uint(digits, &value) != 0) {
        return -1;
    }
    suffix = text[len];
    if (suffix != '\0') {
        if (text[len + 1U] != '\0') {
            return -1;
        }
        if (suffix == 'k' || suffix == 'K') {
            multiplier = 1024ULL;
        } else if (suffix == 'm' || suffix == 'M') {
            multiplier = 1024ULL * 1024ULL;
        } else if (suffix == 'g' || suffix == 'G') {
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        } else {
            return -1;
        }
    }
    if (value > ULLONG_MAX / multiplier) {
        return -1;
    }
    *value_out = value * multiplier;
    return 0;
}

int tool_parse_unsigned_auto(const char *text, unsigned long long *value_out) {
    unsigned long long value = 0ULL;
    size_t index = 0U;
    int is_hex = 0;
    int saw_digit = 0;

    if (text == 0 || text[0] == '\0' || value_out == 0) {
        return -1;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        is_hex = 1;
        index = 2U;
    }
    for (; text[index] != '\0'; ++index) {
        int digit;

        if (is_hex) {
            digit = tool_hex_value(text[index]);
            if (digit < 0) {
                return -1;
            }
            value = (value << 4U) | (unsigned long long)digit;
        } else {
            if (text[index] < '0' || text[index] > '9') {
                return -1;
            }
            value = (value * 10ULL) + (unsigned long long)(text[index] - '0');
        }
        saw_digit = 1;
    }
    if (!saw_digit) {
        return -1;
    }
    *value_out = value;
    return 0;
}

int tool_parse_address_token(const char *text, unsigned long long *value_out) {
    size_t index;
    int has_hex_letter = 0;

    if (text == 0 || text[0] == '\0' || value_out == 0) {
        return -1;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        return tool_parse_unsigned_auto(text, value_out);
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        int digit = tool_hex_value(text[index]);

        if (digit < 0) {
            return -1;
        }
        if ((text[index] >= 'a' && text[index] <= 'f') || (text[index] >= 'A' && text[index] <= 'F')) {
            has_hex_letter = 1;
        }
    }
    if (!has_hex_letter && rt_strlen(text) < 9U) {
        return tool_parse_unsigned_auto(text, value_out);
    }
    *value_out = 0ULL;
    for (index = 0U; text[index] != '\0'; ++index) {
        int digit = tool_hex_value(text[index]);
        if (digit < 0) {
            return -1;
        }
        *value_out = (*value_out << 4U) | (unsigned long long)digit;
    }
    return 0;
}

int tool_parse_fixed_digits(const char *text, size_t start, size_t digits, unsigned int *value_out) {
    unsigned long long value = 0ULL;
    size_t i;

    if (text == 0 || value_out == 0) {
        return -1;
    }
    for (i = 0U; i < digits; ++i) {
        char ch = text[start + i];
        if (ch < '0' || ch > '9') {
            return -1;
        }
        value = value * 10ULL + (unsigned long long)(ch - '0');
    }
    *value_out = (unsigned int)value;
    return 0;
}

int tool_parse_numeric_timezone_offset(const char *text, size_t *index_io, int *offset_seconds_out) {
    size_t index;
    int sign = 1;
    unsigned int hour = 0U;
    unsigned int minute = 0U;

    if (text == 0 || index_io == 0 || offset_seconds_out == 0) {
        return -1;
    }
    index = *index_io;
    if (text[index] != '+' && text[index] != '-') {
        return -1;
    }
    if (text[index] == '-') {
        sign = -1;
    }
    index += 1U;
    if (tool_parse_fixed_digits(text, index, 2U, &hour) != 0) {
        return -1;
    }
    index += 2U;
    if (text[index] == ':') {
        index += 1U;
        if (tool_parse_fixed_digits(text, index, 2U, &minute) != 0) {
            return -1;
        }
        index += 2U;
    } else if (text[index] >= '0' && text[index] <= '9') {
        if (tool_parse_fixed_digits(text, index, 2U, &minute) != 0) {
            return -1;
        }
        index += 2U;
    }
    if (hour > 23U || minute > 59U) {
        return -1;
    }
    *offset_seconds_out = sign * (int)(hour * 3600U + minute * 60U);
    *index_io = index;
    return 0;
}

static int tool_timezone_word_at(const char *text, size_t index, const char *name, size_t *end_out) {
    size_t offset = 0U;

    while (name[offset] != '\0') {
        if (tool_ascii_tolower(text[index + offset]) != tool_ascii_tolower(name[offset])) {
            return 0;
        }
        offset += 1U;
    }
    *end_out = index + offset;
    return 1;
}

int tool_parse_timezone_suffix(const char *text, size_t *index_io, int *offset_seconds_out, int accept_empty) {
    size_t index;
    size_t end = 0U;

    if (text == 0 || index_io == 0 || offset_seconds_out == 0) {
        return -1;
    }
    index = *index_io;
    while (rt_is_space(text[index])) {
        index += 1U;
    }
    if (text[index] == '\0') {
        if (!accept_empty) {
            return -1;
        }
        *offset_seconds_out = 0;
        *index_io = index;
        return 0;
    }
    if (tool_ascii_tolower(text[index]) == 'z') {
        *offset_seconds_out = 0;
        *index_io = index + 1U;
        return 0;
    }
    if (tool_timezone_word_at(text, index, "utc", &end) || tool_timezone_word_at(text, index, "gmt", &end)) {
        index = end;
        if (text[index] == '+' || text[index] == '-') {
            if (tool_parse_numeric_timezone_offset(text, &index, offset_seconds_out) != 0) {
                return -1;
            }
        } else {
            *offset_seconds_out = 0;
        }
        *index_io = index;
        return 0;
    }
    if ((text[index] == '+' || text[index] == '-') && text[index + 1U] >= '0' && text[index + 1U] <= '9') {
        if (tool_parse_numeric_timezone_offset(text, &index, offset_seconds_out) != 0) {
            return -1;
        }
        *index_io = index;
        return 0;
    }
    return -1;
}

int tool_parse_duration_ms(const char *text, unsigned long long *milliseconds_out) {
    unsigned long long whole = 0;
    unsigned long long fraction = 0;
    unsigned long long divisor = 1;
    unsigned long long unit_ms = 1000ULL;
    int saw_digit = 0;
    int saw_fraction = 0;

    if (text == 0 || text[0] == '\0' || milliseconds_out == 0) {
        return -1;
    }

    while (*text >= '0' && *text <= '9') {
        saw_digit = 1;
        whole = (whole * 10ULL) + (unsigned long long)(*text - '0');
        text += 1;
    }

    if (*text == '.') {
        text += 1;
        while (*text >= '0' && *text <= '9') {
            saw_digit = 1;
            saw_fraction = 1;
            if (divisor < 1000000ULL) {
                fraction = (fraction * 10ULL) + (unsigned long long)(*text - '0');
                divisor *= 10ULL;
            }
            text += 1;
        }
    }

    if (!saw_digit) {
        return -1;
    }

    if (text[0] == '\0' || (text[0] == 's' && text[1] == '\0')) {
        unit_ms = 1000ULL;
    } else if (text[0] == 'm' && text[1] == 's' && text[2] == '\0') {
        unit_ms = 1ULL;
    } else if (text[0] == 'm' && text[1] == '\0') {
        unit_ms = 60ULL * 1000ULL;
    } else if (text[0] == 'h' && text[1] == '\0') {
        unit_ms = 60ULL * 60ULL * 1000ULL;
    } else if (text[0] == 'd' && text[1] == '\0') {
        unit_ms = 24ULL * 60ULL * 60ULL * 1000ULL;
    } else {
        return -1;
    }

    *milliseconds_out = whole * unit_ms;
    if (saw_fraction) {
        unsigned long long fraction_ms = ((fraction * unit_ms) + (divisor / 2ULL)) / divisor;
        if (fraction_ms == 0ULL && fraction > 0ULL) {
            fraction_ms = 1ULL;
        }
        *milliseconds_out += fraction_ms;
    }

    return 0;
}

int tool_parse_signal_name(const char *text, int *signal_out) {
    return platform_parse_signal_name(text, signal_out);
}

const char *tool_signal_name(int signal_number) {
    return platform_signal_name(signal_number);
}

void tool_write_signal_list(int fd) {
    platform_write_signal_list(fd);
}

unsigned long long tool_parse_decimal_field(const char *field, size_t field_size) {
    unsigned long long value = 0ULL;
    size_t i = 0U;

    while (i < field_size && (field[i] == ' ' || field[i] == '\0')) i += 1U;
    while (i < field_size && field[i] >= '0' && field[i] <= '9') {
        value = (value * 10ULL) + (unsigned long long)(field[i] - '0');
        i += 1U;
    }
    return value;
}

unsigned short tool_read_u16_le(const unsigned char *bytes) {
    return (unsigned short)((unsigned short)bytes[0] | ((unsigned short)bytes[1] << 8U));
}

unsigned short tool_read_u16_be(const unsigned char *bytes) {
    return (unsigned short)(((unsigned short)bytes[0] << 8U) | (unsigned short)bytes[1]);
}

unsigned int tool_read_u24_le(const unsigned char *bytes) {
    return (unsigned int)bytes[0] |
           ((unsigned int)bytes[1] << 8U) |
           ((unsigned int)bytes[2] << 16U);
}

unsigned int tool_read_u32_le(const unsigned char *bytes) {
    return (unsigned int)bytes[0] |
           ((unsigned int)bytes[1] << 8U) |
           ((unsigned int)bytes[2] << 16U) |
           ((unsigned int)bytes[3] << 24U);
}

unsigned int tool_read_u32_be(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 24U) |
           ((unsigned int)bytes[1] << 16U) |
           ((unsigned int)bytes[2] << 8U) |
           (unsigned int)bytes[3];
}

unsigned long long tool_read_u64_le(const unsigned char *bytes) {
    return (unsigned long long)tool_read_u32_le(bytes) |
           ((unsigned long long)tool_read_u32_le(bytes + 4U) << 32U);
}

unsigned long long tool_read_u64_be(const unsigned char *bytes) {
    return ((unsigned long long)tool_read_u32_be(bytes) << 32U) |
           (unsigned long long)tool_read_u32_be(bytes + 4U);
}

void tool_store_u16_le(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
}

void tool_store_u16_be(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[1] = (unsigned char)(value & 0xffU);
}

void tool_store_u32_le(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xffU);
}

void tool_store_u32_be(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)((value >> 24U) & 0xffU);
    bytes[1] = (unsigned char)((value >> 16U) & 0xffU);
    bytes[2] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[3] = (unsigned char)(value & 0xffU);
}

void tool_store_u64_le(unsigned char *bytes, unsigned long long value) {
    tool_store_u32_le(bytes, (unsigned int)(value & 0xffffffffULL));
    tool_store_u32_le(bytes + 4U, (unsigned int)((value >> 32U) & 0xffffffffULL));
}

void tool_store_u64_be(unsigned char *bytes, unsigned long long value) {
    tool_store_u32_be(bytes, (unsigned int)((value >> 32U) & 0xffffffffULL));
    tool_store_u32_be(bytes + 4U, (unsigned int)(value & 0xffffffffULL));
}

void tool_copy_printable_bytes(char *dest, size_t dest_size, const unsigned char *src, size_t src_size) {
    size_t i;

    if (dest_size == 0U) {
        return;
    }
    for (i = 0U; i + 1U < dest_size && i < src_size && src[i] != 0U; ++i) {
        unsigned char ch = src[i];
        dest[i] = (ch >= 32U && ch <= 126U) ? (char)ch : '?';
    }
    dest[i] = '\0';
}

int tool_str_equal(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

int tool_contains_char(const char *text, char ch) {
    size_t i = 0U;

    while (text[i] != '\0') {
        if (text[i] == ch) {
            return 1;
        }
        i += 1U;
    }
    return 0;
}

int tool_compare_text_slices(const char *left, size_t left_length, const char *right, size_t right_length) {
    size_t i;
    size_t shared = left_length < right_length ? left_length : right_length;

    for (i = 0U; i < shared; ++i) {
        if (left[i] < right[i]) return -1;
        if (left[i] > right[i]) return 1;
    }
    if (left_length < right_length) return -1;
    if (left_length > right_length) return 1;
    return 0;
}

char tool_ascii_tolower(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

char tool_hex_digit(unsigned int value) {
    value &= 0x0fU;
    return (char)(value < 10U ? '0' + value : 'a' + (value - 10U));
}

int tool_ascii_is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

int tool_ascii_is_blank(char ch) {
    return ch == ' ' || ch == '\t';
}

int tool_ascii_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}

int tool_ascii_is_word_byte(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_';
}

int tool_ascii_is_token_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

int tool_next_token(const char **cursor_io, char *token, size_t token_size) {
    const char *cursor;
    size_t length = 0U;

    if (cursor_io == 0 || *cursor_io == 0 || token == 0 || token_size == 0U) {
        return 0;
    }
    cursor = *cursor_io;
    while (*cursor != '\0' && tool_ascii_is_token_space(*cursor)) {
        cursor++;
    }
    if (*cursor == '\0' || *cursor == '#') {
        *cursor_io = cursor;
        token[0] = '\0';
        return 0;
    }
    while (*cursor != '\0' && !tool_ascii_is_token_space(*cursor)) {
        if (length + 1U < token_size) {
            token[length++] = *cursor;
        }
        cursor++;
    }
    token[length] = '\0';
    *cursor_io = cursor;
    return length == 0U ? 0 : 1;
}

int tool_ascii_is_identifier_start(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

int tool_ascii_is_identifier_char(char ch) {
    return tool_ascii_is_identifier_start(ch) || (ch >= '0' && ch <= '9');
}

int tool_str_equal_ignore_case_ascii(const char *left, const char *right) {
    size_t index = 0U;

    if (left == 0 || right == 0) {
        return 0;
    }
    while (left[index] != '\0' && right[index] != '\0') {
        if (tool_ascii_tolower(left[index]) != tool_ascii_tolower(right[index])) {
            return 0;
        }
        index += 1U;
    }
    return left[index] == '\0' && right[index] == '\0';
}

int tool_name_equals_ignore_case_ascii_n(const char *text, size_t text_length, const char *name) {
    size_t index = 0U;

    while (index < text_length && name[index] != '\0') {
        if (tool_ascii_tolower(text[index]) != tool_ascii_tolower(name[index])) return 0;
        index += 1U;
    }
    return index == text_length && name[index] == '\0';
}

int tool_utf8_is_continuation_byte(unsigned char byte) {
    return (byte & 0xc0U) == 0x80U;
}

size_t tool_previous_utf8_codepoint_start(const char *text, size_t index) {
    if (text == 0 || index == 0U) {
        return 0U;
    }

    index -= 1U;
    while (index > 0U && tool_utf8_is_continuation_byte((unsigned char)text[index])) {
        index -= 1U;
    }
    return index;
}

int tool_text_match_has_word_boundaries(const char *text, size_t start, size_t end) {
    size_t length;

    if (text == 0) {
        return 0;
    }

    length = rt_strlen(text);
    return rt_unicode_is_word_boundary(text, length, start) && rt_unicode_is_word_boundary(text, length, end);
}

int tool_text_find_next_match(const char *pattern,
                              const char *text,
                              int ignore_case,
                              int fixed_string,
                              int whole_word,
                              size_t search_start,
                              size_t *start_out,
                              size_t *end_out) {
    return tool_text_find_next_match_ex(pattern, text, ignore_case, fixed_string, whole_word, 1, search_start, start_out, end_out);
}

int tool_text_find_next_match_ex(const char *pattern,
                                 const char *text,
                                 int ignore_case,
                                 int fixed_string,
                                 int whole_word,
                                 int extended,
                                 size_t search_start,
                                 size_t *start_out,
                                 size_t *end_out) {
    size_t candidate_start = 0U;
    size_t candidate_end = 0U;
    size_t next_start = search_start;

    while (1) {
        int found;

        if (fixed_string) {
            size_t pos = next_start;
            size_t pattern_len = rt_strlen(pattern);

            found = 0;
            if (pattern_len == 0U) {
                candidate_start = next_start;
                candidate_end = next_start;
                found = 1;
            } else {
                while (1) {
                    size_t consumed = 0U;

                    if (tool_literal_prefix_matches(pattern, text + pos, ignore_case, &consumed)) {
                        candidate_start = pos;
                        candidate_end = pos + consumed;
                        found = 1;
                        break;
                    }
                    if (text[pos] == '\0') break;
                    pos += 1U;
                    while (tool_utf8_is_continuation_byte((unsigned char)text[pos])) pos += 1U;
                }
            }
        } else {
            found = tool_regex_search_ex(pattern, text, ignore_case, extended, next_start, &candidate_start, &candidate_end);
        }

        if (!found) return 0;
        if (!whole_word || tool_text_match_has_word_boundaries(text, candidate_start, candidate_end)) {
            *start_out = candidate_start;
            *end_out = candidate_end;
            return 1;
        }
        if (text[candidate_start] == '\0') break;
        next_start = candidate_start + 1U;
    }

    return 0;
}

unsigned int tool_unicode_ambiguous_width(void) {
    const char *value = platform_getenv("NEWOS_AMBIGUOUS_WIDTH");

    return value != 0 && rt_strcmp(value, "2") == 0 ? 2U : 1U;
}

size_t tool_text_display_width_n(const char *text, size_t length) {
    return (size_t)rt_text_display_width_n_mode(text, length, 0ULL, 8U, tool_unicode_ambiguous_width());
}

int tool_unicode_space_at(const char *text, size_t length, size_t index, size_t *advance_out) {
    unsigned int codepoint = 0U;
    size_t next = index;

    if (text == 0) {
        if (advance_out != 0) {
            *advance_out = 0U;
        }
        return 0;
    }
    if (index < length && ((unsigned char)text[index]) < 0x80U) {
        unsigned char ch = (unsigned char)text[index];
        if (advance_out != 0) {
            *advance_out = 1U;
        }
        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
    }

    if (text == 0 || index >= length || rt_utf8_decode(text, length, &next, &codepoint) != 0 || next <= index) {
        if (advance_out != 0) {
            *advance_out = index < length ? 1U : 0U;
        }
        return 0;
    }

    if (advance_out != 0) {
        *advance_out = next - index;
    }
    return rt_unicode_is_space(codepoint);
}

int tool_contains_case_insensitive(const char *text, const char *needle) {
    size_t text_len = rt_strlen(text);
    size_t needle_len = rt_strlen(needle);
    return rt_unicode_normalized_contains(text, text_len, needle, needle_len, 1);
}

int tool_parse_pid_filter_list(const char *spec, int *pids_out, size_t max_count, size_t *count_out, const char *tool_name, int require_nonempty) {
    size_t count = 0U;
    size_t i = 0U;

    while (spec[i] != '\0') {
        char token[32];
        size_t token_len = 0U;
        long long pid_value = 0;

        while (spec[i] == ',' || rt_is_space(spec[i])) i += 1U;
        while (spec[i] != '\0' && spec[i] != ',' && token_len + 1U < sizeof(token)) token[token_len++] = spec[i++];
        token[token_len] = '\0';
        tool_trim_whitespace(token);
        if (token[0] == '\0' || count >= max_count || tool_parse_int_arg(token, &pid_value, tool_name, "pid") != 0 || pid_value <= 0) return -1;
        pids_out[count++] = (int)pid_value;
        while (spec[i] != '\0' && spec[i] != ',') i += 1U;
    }
    if (require_nonempty && count == 0U) return -1;
    *count_out = count;
    return 0;
}

static unsigned int tool_permission_mask_for_who(unsigned int who, char permission, unsigned int current_mode, unsigned int flags) {
    unsigned int mask = 0U;
    int allow_exec = (flags & TOOL_SYMBOLIC_MODE_X_ALWAYS) != 0U || (flags & TOOL_SYMBOLIC_MODE_DIRECTORY) != 0U || (current_mode & 0111U) != 0U;

    if (permission == 'r') {
        if ((who & 1U) != 0U) mask |= 0400U;
        if ((who & 2U) != 0U) mask |= 0040U;
        if ((who & 4U) != 0U) mask |= 0004U;
    } else if (permission == 'w') {
        if ((who & 1U) != 0U) mask |= 0200U;
        if ((who & 2U) != 0U) mask |= 0020U;
        if ((who & 4U) != 0U) mask |= 0002U;
    } else if (permission == 'x' || (permission == 'X' && allow_exec)) {
        if ((who & 1U) != 0U) mask |= 0100U;
        if ((who & 2U) != 0U) mask |= 0010U;
        if ((who & 4U) != 0U) mask |= 0001U;
    } else if (permission == 's') {
        if ((who & 1U) != 0U) mask |= 04000U;
        if ((who & 2U) != 0U) mask |= 02000U;
    } else if (permission == 't') {
        mask |= 01000U;
    }
    return mask;
}

static unsigned int tool_permission_copy_mask_for_who(unsigned int who, char source_class, unsigned int current_mode) {
    unsigned int source_bits = 0U;
    unsigned int mask = 0U;

    if (source_class == 'u') source_bits = (current_mode >> 6U) & 07U;
    else if (source_class == 'g') source_bits = (current_mode >> 3U) & 07U;
    else if (source_class == 'o') source_bits = current_mode & 07U;
    if ((source_bits & 4U) != 0U) mask |= tool_permission_mask_for_who(who, 'r', current_mode, 0U);
    if ((source_bits & 2U) != 0U) mask |= tool_permission_mask_for_who(who, 'w', current_mode, 0U);
    if ((source_bits & 1U) != 0U) mask |= tool_permission_mask_for_who(who, 'x', current_mode, 0U);
    return mask;
}

int tool_apply_symbolic_mode(const char *text, unsigned int current_mode, unsigned int flags, unsigned int *mode_out) {
    unsigned int result = current_mode & 07777U;
    size_t i = 0U;

    if (text == 0 || text[0] == '\0') return -1;
    while (text[i] != '\0') {
        unsigned int who = 0U;
        unsigned int set_mask = 0U;
        unsigned int clear_mask = 0U;
        char op;
        int saw_who = 0;
        int saw_permission = 0;

        while (text[i] == 'u' || text[i] == 'g' || text[i] == 'o' || text[i] == 'a') {
            saw_who = 1;
            if (text[i] == 'u') who |= 1U;
            else if (text[i] == 'g') who |= 2U;
            else if (text[i] == 'o') who |= 4U;
            else who |= 7U;
            i += 1U;
        }
        if (!saw_who) who = 7U;
        op = text[i];
        if (op != '+' && op != '-' && op != '=') return -1;
        i += 1U;
        while (text[i] != '\0' && text[i] != ',') {
            unsigned int mask;

            if ((flags & TOOL_SYMBOLIC_MODE_ALLOW_COPY) != 0U && (text[i] == 'u' || text[i] == 'g' || text[i] == 'o')) mask = tool_permission_copy_mask_for_who(who, text[i], result);
            else {
                mask = tool_permission_mask_for_who(who, text[i], result, flags);
                if (mask == 0U && text[i] != 'X') return -1;
            }
            set_mask |= mask;
            saw_permission = 1;
            i += 1U;
        }
        if (!saw_permission && op != '=' && (flags & TOOL_SYMBOLIC_MODE_REQUIRE_PERMISSION) != 0U) return -1;
        if ((who & 1U) != 0U) clear_mask |= 0700U | 04000U;
        if ((who & 2U) != 0U) clear_mask |= 0070U | 02000U;
        if ((who & 4U) != 0U) clear_mask |= 0007U;
        if (who == 7U) clear_mask |= 01000U;
        if (op == '+') result |= set_mask;
        else if (op == '-') result &= ~set_mask;
        else result = (result & ~clear_mask) | set_mask;
        if (text[i] == ',') i += 1U;
    }
    *mode_out = result & 07777U;
    return 0;
}

int tool_literal_prefix_matches(const char *pattern, const char *text, int ignore_case, size_t *consumed_out) {
    size_t pattern_len = rt_strlen(pattern);
    size_t text_len = rt_strlen(text);
    size_t pi = 0U;
    size_t ti = 0U;

    while (pi < pattern_len) {
        unsigned int lhs = 0U;
        unsigned int rhs = 0U;
        unsigned char pattern_ch;
        unsigned char text_ch;

        if (ti >= text_len) return 0;
        pattern_ch = (unsigned char)pattern[pi];
        text_ch = (unsigned char)text[ti];
        if (pattern_ch < 0x80U && text_ch < 0x80U) {
            if (ignore_case) {
                pattern_ch = (unsigned char)tool_ascii_tolower((char)pattern_ch);
                text_ch = (unsigned char)tool_ascii_tolower((char)text_ch);
            }
            if (pattern_ch != text_ch) return 0;
            pi += 1U;
            ti += 1U;
            continue;
        }
        if (rt_utf8_decode(pattern, pattern_len, &pi, &lhs) != 0 || rt_utf8_decode(text, text_len, &ti, &rhs) != 0) return 0;
        if (ignore_case) {
            lhs = rt_unicode_simple_fold(lhs);
            rhs = rt_unicode_simple_fold(rhs);
        }
        if (lhs != rhs) return 0;
    }
    *consumed_out = ti;
    return 1;
}

int tool_find_http_header_end(const char *buffer, size_t length, size_t *offset_out) {
    size_t index;

    if (buffer == 0 || offset_out == 0) {
        return -1;
    }

    for (index = 0U; index + 3U < length; ++index) {
        if (buffer[index] == '\r' && buffer[index + 1U] == '\n' &&
            buffer[index + 2U] == '\r' && buffer[index + 3U] == '\n') {
            *offset_out = index + 4U;
            return 0;
        }
    }

    for (index = 0U; index + 1U < length; ++index) {
        if (buffer[index] == '\n' && buffer[index + 1U] == '\n') {
            *offset_out = index + 2U;
            return 0;
        }
    }
    return -1;
}

void tool_format_uptime_compact(unsigned long long total_seconds, char *buffer, size_t buffer_size) {
    unsigned long long days = total_seconds / 86400ULL;
    unsigned long long hours = (total_seconds % 86400ULL) / 3600ULL;
    unsigned long long minutes = (total_seconds % 3600ULL) / 60ULL;
    unsigned long long seconds = total_seconds % 60ULL;
    size_t length = 0U;

    if (buffer_size == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (days > 0ULL) {
        length = tool_buffer_append_uint(buffer, buffer_size, length, days);
        length = tool_buffer_append_char(buffer, buffer_size, length, 'd');
        length = tool_buffer_append_char(buffer, buffer_size, length, ' ');
        length = tool_buffer_append_uint(buffer, buffer_size, length, hours);
        length = tool_buffer_append_char(buffer, buffer_size, length, 'h');
        length = tool_buffer_append_char(buffer, buffer_size, length, ' ');
        length = tool_buffer_append_uint(buffer, buffer_size, length, minutes);
        (void)tool_buffer_append_char(buffer, buffer_size, length, 'm');
    } else if (hours > 0ULL) {
        length = tool_buffer_append_uint(buffer, buffer_size, length, hours);
        length = tool_buffer_append_char(buffer, buffer_size, length, 'h');
        length = tool_buffer_append_char(buffer, buffer_size, length, ' ');
        length = tool_buffer_append_uint(buffer, buffer_size, length, minutes);
        (void)tool_buffer_append_char(buffer, buffer_size, length, 'm');
    } else if (minutes > 0ULL) {
        length = tool_buffer_append_uint(buffer, buffer_size, length, minutes);
        (void)tool_buffer_append_char(buffer, buffer_size, length, 'm');
    } else {
        length = tool_buffer_append_uint(buffer, buffer_size, length, seconds);
        (void)tool_buffer_append_char(buffer, buffer_size, length, 's');
    }
}

static int tool_is_leap_year(int year) {
    if ((year % 400) == 0) return 1;
    if ((year % 100) == 0) return 0;
    return (year % 4) == 0;
}

int tool_days_in_month(int year, unsigned int month) {
    static const unsigned char days[] = { 31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U };

    if (month == 0U || month > 12U) {
        return 0;
    }
    if (month == 2U && tool_is_leap_year(year)) {
        return 29;
    }
    return (int)days[month - 1U];
}

long long tool_days_from_civil(int year, unsigned int month, unsigned int day) {
    int adjusted_year = year - (month <= 2U ? 1 : 0);
    int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    unsigned int year_of_era = (unsigned int)(adjusted_year - (era * 400));
    unsigned int shifted_month = month + (month > 2U ? (unsigned int)-3 : 9U);
    unsigned int day_of_year = ((153U * shifted_month) + 2U) / 5U + day - 1U;
    unsigned int day_of_era = year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return (long long)era * 146097LL + (long long)day_of_era - 719468LL;
}

void tool_civil_from_days(long long days, int *year_out, unsigned int *month_out, unsigned int *day_out) {
    long long z = days + 719468LL;
    long long era = (z >= 0LL ? z : z - 146096LL) / 146097LL;
    unsigned int day_of_era = (unsigned int)(z - era * 146097LL);
    unsigned int year_of_era = (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
    int year = (int)year_of_era + (int)(era * 400LL);
    unsigned int day_of_year = day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    unsigned int month_index = (5U * day_of_year + 2U) / 153U;
    unsigned int day = day_of_year - (153U * month_index + 2U) / 5U + 1U;
    unsigned int month = month_index + (month_index < 10U ? 3U : (unsigned int)-9);

    year += (month <= 2U) ? 1 : 0;
    if (year_out != 0) {
        *year_out = year;
    }
    if (month_out != 0) {
        *month_out = month;
    }
    if (day_out != 0) {
        *day_out = day;
    }
}

int tool_build_epoch_timestamp(int year, unsigned int month, unsigned int day, unsigned int hour, unsigned int minute, unsigned int second, long long *epoch_out) {
    long long days;

    if (epoch_out == 0 || month < 1U || month > 12U || day < 1U || day > (unsigned int)tool_days_in_month(year, month) ||
        hour > 23U || minute > 59U || second > 59U) {
        return -1;
    }

    days = tool_days_from_civil(year, month, day);
    *epoch_out = days * 86400LL + (long long)hour * 3600LL + (long long)minute * 60LL + (long long)second;
    return 0;
}

int tool_text_is_decimal(const char *text) {
    size_t index = 0U;

    if (text == 0 || text[0] == '\0') {
        return 0;
    }
    while (text[index] != '\0') {
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
        index += 1U;
    }
    return 1;
}

size_t tool_count_decimal_digits(unsigned long long value) {
    size_t digits = 1U;

    while (value >= 10ULL) {
        value /= 10ULL;
        digits += 1U;
    }

    return digits;
}

int tool_buffer_append_char_checked(char *buffer, size_t buffer_size, size_t *length_io, char ch) {
    if (*length_io + 1U >= buffer_size) {
        return -1;
    }
    buffer[*length_io] = ch;
    *length_io += 1U;
    buffer[*length_io] = '\0';
    return 0;
}

int tool_buffer_append_text_checked(char *buffer, size_t buffer_size, size_t *length_io, const char *text) {
    size_t index = 0U;

    while (text[index] != '\0') {
        if (tool_buffer_append_char_checked(buffer, buffer_size, length_io, text[index]) != 0) {
            return -1;
        }
        index += 1U;
    }
    return 0;
}

int tool_buffer_append_uint_checked(char *buffer, size_t buffer_size, size_t *length_io, unsigned long long value) {
    char text[32];

    rt_unsigned_to_string(value, text, sizeof(text));
    return tool_buffer_append_text_checked(buffer, buffer_size, length_io, text);
}
