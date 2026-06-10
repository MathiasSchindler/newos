#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

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

char tool_ascii_tolower(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
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
    if (start > 0U) {
        size_t prev = tool_previous_utf8_codepoint_start(text, start);
        size_t index = prev;
        unsigned int codepoint = 0U;

        if (((unsigned char)text[prev]) < 0x80U) {
            if (tool_ascii_is_word_byte((unsigned char)text[prev])) {
                return 0;
            }
        } else if (rt_utf8_decode(text, length, &index, &codepoint) == 0 && rt_unicode_is_word(codepoint)) {
            return 0;
        }
    }

    if (end < length) {
        size_t index = end;
        unsigned int codepoint = 0U;

        if (((unsigned char)text[end]) < 0x80U) {
            if (tool_ascii_is_word_byte((unsigned char)text[end])) {
                return 0;
            }
        } else if (rt_utf8_decode(text, length, &index, &codepoint) == 0 && rt_unicode_is_word(codepoint)) {
            return 0;
        }
    }

    return 1;
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
