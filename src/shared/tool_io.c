#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

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
