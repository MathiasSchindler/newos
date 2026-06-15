#include "pgp.h"
#include "crypto/sha1.h"
#include "crypto/sha256.h"
#include "runtime.h"

#define PGP_ARMOR_CRC24_INIT 0xb704ceU
#define PGP_ARMOR_CRC24_POLY 0x1864cfbU

static const char pgp_base64_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void pgp_set_error(char *error, size_t error_size, const char *message) {
    if (error != 0 && error_size > 0U) {
        rt_copy_string(error, error_size, message != 0 ? message : "OpenPGP error");
    }
}

static int pgp_base64_value(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static int pgp_hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int pgp_starts_with_n(const unsigned char *data, size_t size, size_t offset, const char *prefix) {
    size_t index = 0U;

    while (prefix[index] != '\0') {
        if (offset + index >= size || data[offset + index] != (unsigned char)prefix[index]) {
            return 0;
        }
        index += 1U;
    }
    return 1;
}

static int pgp_line_contains_colon(const unsigned char *line, size_t length) {
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (line[index] == ':') return 1;
    }
    return 0;
}

static unsigned int pgp_read_u16_be(const unsigned char *data) {
    return ((unsigned int)data[0] << 8U) | (unsigned int)data[1];
}

static unsigned int pgp_read_u32_be(const unsigned char *data) {
    return ((unsigned int)data[0] << 24U) |
           ((unsigned int)data[1] << 16U) |
           ((unsigned int)data[2] << 8U) |
           (unsigned int)data[3];
}

static unsigned long long pgp_read_u32_be_ull(const unsigned char *data) {
    return (unsigned long long)pgp_read_u32_be(data);
}

static unsigned int pgp_crc24_update(unsigned int crc, const unsigned char *data, size_t size) {
    size_t index;

    for (index = 0U; index < size; ++index) {
        unsigned int bit;

        crc ^= (unsigned int)data[index] << 16U;
        for (bit = 0U; bit < 8U; ++bit) {
            crc <<= 1U;
            if ((crc & 0x1000000U) != 0U) {
                crc ^= PGP_ARMOR_CRC24_POLY;
            }
        }
    }
    return crc & 0xffffffU;
}

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} PgpNormalizeBuffer;

static void pgp_normalize_buffer_free(PgpNormalizeBuffer *buffer) {
    if (buffer->data != 0) rt_free(buffer->data);
    buffer->data = 0;
    buffer->size = 0U;
    buffer->capacity = 0U;
}

static int pgp_normalize_buffer_reserve(PgpNormalizeBuffer *buffer, size_t extra) {
    size_t needed;
    size_t capacity;
    unsigned char *grown;

    if (extra > ((size_t)-1) - buffer->size) return -1;
    needed = buffer->size + extra;
    if (needed <= buffer->capacity) return 0;
    capacity = buffer->capacity != 0U ? buffer->capacity : 256U;
    while (capacity < needed) {
        if (capacity > ((size_t)-1) / 2U) {
            capacity = needed;
            break;
        }
        capacity *= 2U;
    }
    grown = (unsigned char *)rt_realloc(buffer->data, capacity);
    if (grown == 0) return -1;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 0;
}

static int pgp_normalize_buffer_append(PgpNormalizeBuffer *buffer, const unsigned char *data, size_t size) {
    if (size == 0U) return 0;
    if (pgp_normalize_buffer_reserve(buffer, size) != 0) return -1;
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 0;
}

static int pgp_normalize_buffer_append_byte(PgpNormalizeBuffer *buffer, unsigned int value) {
    unsigned char byte = (unsigned char)(value & 0xffU);

    return pgp_normalize_buffer_append(buffer, &byte, 1U);
}

static int pgp_normalize_buffer_append_u32_be(PgpNormalizeBuffer *buffer, size_t value) {
    unsigned char bytes[4];

    bytes[0] = (unsigned char)(((unsigned long long)value >> 24U) & 0xffU);
    bytes[1] = (unsigned char)(((unsigned long long)value >> 16U) & 0xffU);
    bytes[2] = (unsigned char)(((unsigned long long)value >> 8U) & 0xffU);
    bytes[3] = (unsigned char)((unsigned long long)value & 0xffU);
    return pgp_normalize_buffer_append(buffer, bytes, sizeof(bytes));
}

static int pgp_normalize_buffer_append_packet_length(PgpNormalizeBuffer *buffer, size_t length) {
    if (length < 192U) return pgp_normalize_buffer_append_byte(buffer, (unsigned int)length);
    if (length <= 8383U) {
        size_t encoded = length - 192U;

        return pgp_normalize_buffer_append_byte(buffer, (unsigned int)((encoded >> 8U) + 192U)) != 0 ||
               pgp_normalize_buffer_append_byte(buffer, (unsigned int)(encoded & 0xffU)) != 0 ? -1 : 0;
    }
    if (length > 0xffffffffULL) return -1;
    return pgp_normalize_buffer_append_byte(buffer, 255U) != 0 || pgp_normalize_buffer_append_u32_be(buffer, length) != 0 ? -1 : 0;
}

static int pgp_decode_new_packet_length_octet(const unsigned char *data, size_t size, size_t *offset_io, unsigned int first, size_t *length_out, int *partial_out, char *error, size_t error_size) {
    *partial_out = 0;
    if (first < 192U) {
        *length_out = first;
        return 0;
    }
    if (first < 224U) {
        if (*offset_io >= size) {
            pgp_set_error(error, error_size, "truncated OpenPGP two-octet length");
            return -1;
        }
        *length_out = ((size_t)(first - 192U) << 8U) + (size_t)data[(*offset_io)++] + 192U;
        return 0;
    }
    if (first == 255U) {
        if (*offset_io + 4U > size) {
            pgp_set_error(error, error_size, "truncated OpenPGP five-octet length");
            return -1;
        }
        *length_out = (size_t)pgp_read_u32_be(data + *offset_io);
        *offset_io += 4U;
        return 0;
    }
    if ((first & 0x1fU) >= sizeof(size_t) * 8U) {
        pgp_set_error(error, error_size, "OpenPGP partial length is too large");
        return -1;
    }
    *length_out = (size_t)1U << (first & 0x1fU);
    *partial_out = 1;
    return 0;
}

static int pgp_decode_base64_text(const unsigned char *text, size_t text_size, unsigned char *out, size_t *out_size, char *error, size_t error_size) {
    int values[4];
    int value_count = 0;
    int pad_count = 0;
    size_t out_used = 0U;
    size_t index;

    for (index = 0U; index < text_size; ++index) {
        char ch = (char)text[index];
        int value;

        if (rt_is_space(ch)) {
            continue;
        }
        if (ch == '=') {
            values[value_count++] = 0;
            pad_count += 1;
        } else {
            value = pgp_base64_value(ch);
            if (value < 0 || pad_count > 0) {
                pgp_set_error(error, error_size, "invalid ASCII armor base64 data");
                return -1;
            }
            values[value_count++] = value;
        }
        if (value_count == 4) {
            if (pad_count > 2) {
                pgp_set_error(error, error_size, "invalid ASCII armor padding");
                return -1;
            }
            out[out_used++] = (unsigned char)((values[0] << 2U) | ((values[1] >> 4U) & 0x03U));
            if (pad_count < 2) out[out_used++] = (unsigned char)(((values[1] & 0x0fU) << 4U) | ((values[2] >> 2U) & 0x0fU));
            if (pad_count < 1) out[out_used++] = (unsigned char)(((values[2] & 0x03U) << 6U) | values[3]);
            value_count = 0;
            pad_count = 0;
        }
    }
    if (value_count != 0) {
        pgp_set_error(error, error_size, "truncated ASCII armor base64 data");
        return -1;
    }
    *out_size = out_used;
    return 0;
}

static int pgp_decode_armor(const unsigned char *input, size_t input_size, unsigned char **data_out, size_t *size_out, char *error, size_t error_size) {
    unsigned char *base64;
    unsigned char *decoded;
    size_t base64_size = 0U;
    size_t decoded_size = 0U;
    size_t offset = 0U;
    int in_body = 0;
    int saw_blank = 0;
    int saw_end = 0;
    int have_crc = 0;
    unsigned int expected_crc = 0U;

    base64 = (unsigned char *)rt_malloc(input_size + 1U);
    decoded = (unsigned char *)rt_malloc(input_size + 1U);
    if (base64 == 0 || decoded == 0) {
        if (base64 != 0) rt_free(base64);
        if (decoded != 0) rt_free(decoded);
        pgp_set_error(error, error_size, "out of memory while decoding ASCII armor");
        return -1;
    }

    while (offset < input_size) {
        size_t line_start = offset;
        size_t line_end;
        const unsigned char *line;
        size_t line_length;

        while (offset < input_size && input[offset] != '\n' && input[offset] != '\r') {
            offset += 1U;
        }
        line_end = offset;
        while (offset < input_size && (input[offset] == '\n' || input[offset] == '\r')) {
            offset += 1U;
        }
        line = input + line_start;
        line_length = line_end - line_start;

        if (!in_body) {
            if (pgp_starts_with_n(input, input_size, line_start, "-----BEGIN PGP ")) {
                in_body = 1;
            }
            continue;
        }
        if (pgp_starts_with_n(input, input_size, line_start, "-----END PGP ")) {
            saw_end = 1;
            break;
        }
        if (line_length == 0U) {
            saw_blank = 1;
            continue;
        }
        if (!saw_blank && pgp_line_contains_colon(line, line_length)) {
            continue;
        }
        if (line[0] == '=') {
            unsigned char crc_bytes[3];
            size_t crc_size = 0U;

            if (pgp_decode_base64_text(line + 1U, line_length - 1U, crc_bytes, &crc_size, error, error_size) != 0 || crc_size != 3U) {
                rt_free(base64);
                rt_free(decoded);
                pgp_set_error(error, error_size, "invalid ASCII armor checksum");
                return -1;
            }
            expected_crc = ((unsigned int)crc_bytes[0] << 16U) | ((unsigned int)crc_bytes[1] << 8U) | (unsigned int)crc_bytes[2];
            have_crc = 1;
            continue;
        }
        if (base64_size + line_length > input_size) {
            rt_free(base64);
            rt_free(decoded);
            pgp_set_error(error, error_size, "ASCII armor is too large");
            return -1;
        }
        memcpy(base64 + base64_size, line, line_length);
        base64_size += line_length;
    }

    if (!saw_end) {
        rt_free(base64);
        rt_free(decoded);
        pgp_set_error(error, error_size, "unterminated ASCII armor block");
        return -1;
    }
    if (pgp_decode_base64_text(base64, base64_size, decoded, &decoded_size, error, error_size) != 0) {
        rt_free(base64);
        rt_free(decoded);
        return -1;
    }
    if (have_crc) {
        unsigned int actual_crc = pgp_crc24_update(PGP_ARMOR_CRC24_INIT, decoded, decoded_size);
        if (actual_crc != expected_crc) {
            rt_free(base64);
            rt_free(decoded);
            pgp_set_error(error, error_size, "ASCII armor checksum mismatch");
            return -1;
        }
    }
    rt_free(base64);
    *data_out = decoded;
    *size_out = decoded_size;
    return 0;
}

int pgp_decode_input(const unsigned char *input, size_t input_size, unsigned char **data_out, size_t *size_out, char *error, size_t error_size) {
    size_t offset = 0U;
    unsigned char *copy;
    unsigned char *decoded = 0;
    size_t decoded_size = 0U;

    *data_out = 0;
    *size_out = 0U;
    while (offset < input_size && rt_is_space((char)input[offset])) {
        offset += 1U;
    }
    if (pgp_starts_with_n(input, input_size, offset, "-----BEGIN PGP ")) {
        if (pgp_decode_armor(input, input_size, &decoded, &decoded_size, error, error_size) != 0) return -1;
        if (pgp_normalize_packets(decoded, decoded_size, data_out, size_out, error, error_size) != 0) {
            rt_free(decoded);
            return -1;
        }
        rt_free(decoded);
        return 0;
    }
    copy = (unsigned char *)rt_malloc(input_size == 0U ? 1U : input_size);
    if (copy == 0) {
        pgp_set_error(error, error_size, "out of memory while reading OpenPGP data");
        return -1;
    }
    if (input_size != 0U) memcpy(copy, input, input_size);
    if (pgp_normalize_packets(copy, input_size, data_out, size_out, error, error_size) != 0) {
        rt_free(copy);
        return -1;
    }
    rt_free(copy);
    return 0;
}

static int pgp_copy_old_format_packet(const unsigned char *input, size_t input_size, size_t header_offset, unsigned int header, size_t *offset_io, PgpNormalizeBuffer *out, char *error, size_t error_size) {
    size_t body_size;
    unsigned int length_type = header & 0x03U;

    if (length_type == 0U) {
        if (*offset_io >= input_size) { pgp_set_error(error, error_size, "truncated OpenPGP old-format length"); return -1; }
        body_size = input[(*offset_io)++];
    } else if (length_type == 1U) {
        if (*offset_io + 2U > input_size) { pgp_set_error(error, error_size, "truncated OpenPGP old-format length"); return -1; }
        body_size = pgp_read_u16_be(input + *offset_io);
        *offset_io += 2U;
    } else if (length_type == 2U) {
        if (*offset_io + 4U > input_size) { pgp_set_error(error, error_size, "truncated OpenPGP old-format length"); return -1; }
        body_size = (size_t)pgp_read_u32_be(input + *offset_io);
        *offset_io += 4U;
    } else {
        body_size = input_size - *offset_io;
    }
    if (body_size > input_size - *offset_io) { pgp_set_error(error, error_size, "truncated OpenPGP packet body"); return -1; }
    if (pgp_normalize_buffer_append(out, input + header_offset, (*offset_io + body_size) - header_offset) != 0) return -1;
    *offset_io += body_size;
    return 0;
}

int pgp_normalize_packets(const unsigned char *input, size_t input_size, unsigned char **data_out, size_t *size_out, char *error, size_t error_size) {
    PgpNormalizeBuffer out;
    size_t offset = 0U;

    rt_memset(&out, 0, sizeof(out));
    *data_out = 0;
    *size_out = 0U;
    while (offset < input_size) {
        size_t header_offset = offset;
        unsigned int header = input[offset++];

        if ((header & 0x80U) == 0U) { pgp_set_error(error, error_size, "invalid OpenPGP packet header"); goto fail; }
        if ((header & 0x40U) == 0U) {
            if (pgp_copy_old_format_packet(input, input_size, header_offset, header, &offset, &out, error, error_size) != 0) goto fail;
        } else {
            size_t first_length_offset = offset;
            size_t scan_offset = offset;
            size_t total_body_size = 0U;
            int saw_partial = 0;

            while (1) {
                unsigned int first;
                size_t chunk_size;
                int partial;

                if (scan_offset >= input_size) { pgp_set_error(error, error_size, "truncated OpenPGP packet length"); goto fail; }
                first = input[scan_offset++];
                if (pgp_decode_new_packet_length_octet(input, input_size, &scan_offset, first, &chunk_size, &partial, error, error_size) != 0) goto fail;
                if (chunk_size > input_size - scan_offset || chunk_size > ((size_t)-1) - total_body_size) { pgp_set_error(error, error_size, "truncated OpenPGP packet body"); goto fail; }
                total_body_size += chunk_size;
                scan_offset += chunk_size;
                if (partial) saw_partial = 1;
                else break;
            }
            if (!saw_partial) {
                if (pgp_normalize_buffer_append(&out, input + header_offset, scan_offset - header_offset) != 0) goto oom;
            } else {
                size_t body_offset = first_length_offset;

                if (pgp_normalize_buffer_append_byte(&out, header) != 0 || pgp_normalize_buffer_append_packet_length(&out, total_body_size) != 0) goto oom;
                while (body_offset < scan_offset) {
                    unsigned int first = input[body_offset++];
                    size_t chunk_size;
                    int partial;

                    if (pgp_decode_new_packet_length_octet(input, input_size, &body_offset, first, &chunk_size, &partial, error, error_size) != 0) goto fail;
                    if (pgp_normalize_buffer_append(&out, input + body_offset, chunk_size) != 0) goto oom;
                    body_offset += chunk_size;
                    if (!partial) break;
                }
            }
            offset = scan_offset;
        }
    }
    *data_out = out.data;
    *size_out = out.size;
    return 0;

oom:
    pgp_set_error(error, error_size, "out of memory while normalizing OpenPGP packets");
fail:
    pgp_normalize_buffer_free(&out);
    return -1;
}

static int pgp_write_base64_byte(int fd, char ch, unsigned int *column_io) {
    if (rt_write_char(fd, ch) != 0) return -1;
    *column_io += 1U;
    if (*column_io >= 64U) {
        if (rt_write_char(fd, '\n') != 0) return -1;
        *column_io = 0U;
    }
    return 0;
}

static int pgp_write_base64_data(int fd, const unsigned char *data, size_t size, unsigned int *column_io) {
    size_t index = 0U;

    while (index < size) {
        size_t chunk_start = index;
        size_t remaining;
        unsigned int b0 = data[index++];
        unsigned int b1 = index < size ? data[index++] : 0U;
        unsigned int b2 = index < size ? data[index++] : 0U;
        remaining = size - chunk_start;
        int have_b1 = remaining >= 2U;
        int have_b2 = remaining >= 3U;

        if (pgp_write_base64_byte(fd, pgp_base64_alphabet[(b0 >> 2U) & 0x3fU], column_io) != 0) return -1;
        if (pgp_write_base64_byte(fd, pgp_base64_alphabet[((b0 << 4U) | (b1 >> 4U)) & 0x3fU], column_io) != 0) return -1;
        if (pgp_write_base64_byte(fd, have_b1 ? pgp_base64_alphabet[((b1 << 2U) | (b2 >> 6U)) & 0x3fU] : '=', column_io) != 0) return -1;
        if (pgp_write_base64_byte(fd, have_b2 ? pgp_base64_alphabet[b2 & 0x3fU] : '=', column_io) != 0) return -1;
    }
    return 0;
}

static int pgp_write_armor_block(int fd, const char *kind, const unsigned char *data, size_t size) {
    unsigned int column = 0U;
    unsigned int crc;
    unsigned char crc_bytes[3];

    if (rt_write_cstr(fd, "-----BEGIN PGP ") != 0 || rt_write_cstr(fd, kind) != 0 || rt_write_cstr(fd, "-----\n\n") != 0) return -1;
    if (pgp_write_base64_data(fd, data, size, &column) != 0) return -1;
    if (column != 0U && rt_write_char(fd, '\n') != 0) return -1;
    crc = pgp_crc24_update(PGP_ARMOR_CRC24_INIT, data, size);
    crc_bytes[0] = (unsigned char)((crc >> 16U) & 0xffU);
    crc_bytes[1] = (unsigned char)((crc >> 8U) & 0xffU);
    crc_bytes[2] = (unsigned char)(crc & 0xffU);
    if (rt_write_char(fd, '=') != 0) return -1;
    column = 0U;
    if (pgp_write_base64_data(fd, crc_bytes, sizeof(crc_bytes), &column) != 0) return -1;
    if (column != 0U && rt_write_char(fd, '\n') != 0) return -1;
    if (rt_write_cstr(fd, "-----END PGP ") != 0 || rt_write_cstr(fd, kind) != 0) return -1;
    return rt_write_cstr(fd, "-----\n");
}

int pgp_write_public_key_armor(int fd, const unsigned char *data, size_t size) {
    return pgp_write_armor_block(fd, "PUBLIC KEY BLOCK", data, size);
}

int pgp_write_private_key_armor(int fd, const unsigned char *data, size_t size) {
    return pgp_write_armor_block(fd, "PRIVATE KEY BLOCK", data, size);
}

int pgp_write_message_armor(int fd, const unsigned char *data, size_t size) {
    return pgp_write_armor_block(fd, "MESSAGE", data, size);
}

int pgp_write_signature_armor(int fd, const unsigned char *data, size_t size) {
    return pgp_write_armor_block(fd, "SIGNATURE", data, size);
}

static int pgp_write_byte_fd(int fd, unsigned int value) {
    unsigned char byte = (unsigned char)(value & 0xffU);

    return rt_write_all(fd, &byte, 1U);
}

int pgp_write_new_packet_header(int fd, unsigned int tag) {
    if (tag > 63U) return -1;
    return pgp_write_byte_fd(fd, 0xc0U | tag);
}

int pgp_write_packet_length(int fd, size_t length) {
    unsigned char bytes[5];

    if (length < 192U) return pgp_write_byte_fd(fd, (unsigned int)length);
    if (length <= 8383U) {
        size_t encoded = length - 192U;

        bytes[0] = (unsigned char)((encoded >> 8U) + 192U);
        bytes[1] = (unsigned char)(encoded & 0xffU);
        return rt_write_all(fd, bytes, 2U);
    }
    if (length > 0xffffffffULL) return -1;
    bytes[0] = 255U;
    bytes[1] = (unsigned char)(((unsigned long long)length >> 24U) & 0xffU);
    bytes[2] = (unsigned char)(((unsigned long long)length >> 16U) & 0xffU);
    bytes[3] = (unsigned char)(((unsigned long long)length >> 8U) & 0xffU);
    bytes[4] = (unsigned char)((unsigned long long)length & 0xffU);
    return rt_write_all(fd, bytes, sizeof(bytes));
}

int pgp_write_partial_body_length(int fd, size_t length) {
    size_t value = length;
    unsigned int exponent = 0U;

    if (length < 512U || length > 0x40000000ULL) return -1;
    while (value > 1U) {
        if ((value & 1U) != 0U) return -1;
        value >>= 1U;
        exponent += 1U;
    }
    if (exponent < 9U || exponent > 30U) return -1;
    return pgp_write_byte_fd(fd, 224U + exponent);
}

void pgp_packet_reader_init(PgpPacketReader *reader, const unsigned char *data, size_t size) {
    reader->data = data;
    reader->size = size;
    reader->offset = 0U;
}

static int pgp_read_packet_length_new(PgpPacketReader *reader, size_t *length_out, char *error, size_t error_size) {
    unsigned int first;

    if (reader->offset >= reader->size) {
        pgp_set_error(error, error_size, "truncated OpenPGP packet length");
        return -1;
    }
    first = reader->data[reader->offset++];
    if (first < 192U) {
        *length_out = first;
        return 0;
    }
    if (first < 224U) {
        if (reader->offset >= reader->size) {
            pgp_set_error(error, error_size, "truncated OpenPGP two-octet length");
            return -1;
        }
        *length_out = ((size_t)(first - 192U) << 8U) + (size_t)reader->data[reader->offset++] + 192U;
        return 0;
    }
    if (first == 255U) {
        if (reader->offset + 4U > reader->size) {
            pgp_set_error(error, error_size, "truncated OpenPGP five-octet length");
            return -1;
        }
        *length_out = (size_t)pgp_read_u32_be(reader->data + reader->offset);
        reader->offset += 4U;
        return 0;
    }
    pgp_set_error(error, error_size, "partial OpenPGP packet lengths are not supported");
    return -1;
}

static int pgp_read_packet_length_old(PgpPacketReader *reader, unsigned int length_type, size_t *length_out, char *error, size_t error_size) {
    if (length_type == 0U) {
        if (reader->offset >= reader->size) {
            pgp_set_error(error, error_size, "truncated OpenPGP old-format length");
            return -1;
        }
        *length_out = reader->data[reader->offset++];
        return 0;
    }
    if (length_type == 1U) {
        if (reader->offset + 2U > reader->size) {
            pgp_set_error(error, error_size, "truncated OpenPGP old-format length");
            return -1;
        }
        *length_out = pgp_read_u16_be(reader->data + reader->offset);
        reader->offset += 2U;
        return 0;
    }
    if (length_type == 2U) {
        if (reader->offset + 4U > reader->size) {
            pgp_set_error(error, error_size, "truncated OpenPGP old-format length");
            return -1;
        }
        *length_out = (size_t)pgp_read_u32_be(reader->data + reader->offset);
        reader->offset += 4U;
        return 0;
    }
    *length_out = reader->size - reader->offset;
    return 0;
}

int pgp_packet_reader_next(PgpPacketReader *reader, PgpPacket *packet_out, int *has_packet_out, char *error, size_t error_size) {
    unsigned int header;
    size_t body_size;

    *has_packet_out = 0;
    if (reader->offset >= reader->size) {
        return 0;
    }
    packet_out->header_offset = reader->offset;
    header = reader->data[reader->offset++];
    if ((header & 0x80U) == 0U) {
        pgp_set_error(error, error_size, "invalid OpenPGP packet header");
        return -1;
    }
    if ((header & 0x40U) != 0U) {
        packet_out->new_format = 1;
        packet_out->tag = header & 0x3fU;
        if (pgp_read_packet_length_new(reader, &body_size, error, error_size) != 0) return -1;
    } else {
        packet_out->new_format = 0;
        packet_out->tag = (header >> 2U) & 0x0fU;
        if (pgp_read_packet_length_old(reader, header & 0x03U, &body_size, error, error_size) != 0) return -1;
    }
    if (body_size > reader->size - reader->offset) {
        pgp_set_error(error, error_size, "truncated OpenPGP packet body");
        return -1;
    }
    packet_out->body_offset = reader->offset;
    packet_out->body_size = body_size;
    reader->offset += body_size;
    *has_packet_out = 1;
    return 0;
}

static int pgp_read_mpi_bits(const unsigned char *body, size_t body_size, size_t *offset_io, unsigned int *bits_out) {
    unsigned int bits;
    size_t bytes;

    if (*offset_io + 2U > body_size) return -1;
    bits = pgp_read_u16_be(body + *offset_io);
    *offset_io += 2U;
    bytes = ((size_t)bits + 7U) / 8U;
    if (bytes > body_size - *offset_io) return -1;
    *offset_io += bytes;
    *bits_out = bits;
    return 0;
}

static int pgp_read_mpi_view(const unsigned char *body, size_t body_size, size_t *offset_io, const unsigned char **data_out, size_t *size_out, unsigned int *bits_out) {
    unsigned int bits;
    size_t bytes;

    if (*offset_io + 2U > body_size) return -1;
    bits = pgp_read_u16_be(body + *offset_io);
    *offset_io += 2U;
    bytes = ((size_t)bits + 7U) / 8U;
    if (bytes > body_size - *offset_io) return -1;
    *data_out = body + *offset_io;
    *size_out = bytes;
    *bits_out = bits;
    *offset_io += bytes;
    return 0;
}

static void pgp_store_public_material(PgpPublicKeyInfo *info, const unsigned char *data, size_t size) {
    size_t copy_size = size < sizeof(info->public_material) ? size : sizeof(info->public_material);

    if (copy_size != 0U) memcpy(info->public_material, data, copy_size);
    info->public_material_size = copy_size;
}

static int pgp_skip_ec_public_key_material(PgpPublicKeyInfo *info, const unsigned char *body, size_t body_size, size_t *offset_io, char *error, size_t error_size) {
    unsigned int oid_size;
    unsigned int point_bits;
    const unsigned char *point;
    size_t point_size;

    if (*offset_io >= body_size) {
        pgp_set_error(error, error_size, "truncated OpenPGP EC curve OID");
        return -1;
    }
    oid_size = body[(*offset_io)++];
    if (oid_size > body_size - *offset_io) {
        pgp_set_error(error, error_size, "truncated OpenPGP EC curve OID");
        return -1;
    }
    *offset_io += oid_size;
    if (pgp_read_mpi_view(body, body_size, offset_io, &point, &point_size, &point_bits) != 0) {
        pgp_set_error(error, error_size, "truncated OpenPGP EC public point");
        return -1;
    }
    if (point_size == 33U && point[0] == 0x40U) pgp_store_public_material(info, point + 1U, point_size - 1U);
    else pgp_store_public_material(info, point, point_size);
    return 0;
}

static int pgp_skip_public_key_material(PgpPublicKeyInfo *info, unsigned int algorithm, const unsigned char *body, size_t body_size, size_t *offset_io, unsigned int *bits_out, char *error, size_t error_size) {
    unsigned int first_bits = 0U;
    unsigned int ignored_bits = 0U;

    if (algorithm == 1U || algorithm == 2U || algorithm == 3U) {
        if (pgp_read_mpi_bits(body, body_size, offset_io, &first_bits) != 0 ||
            pgp_read_mpi_bits(body, body_size, offset_io, &ignored_bits) != 0) {
            pgp_set_error(error, error_size, "truncated RSA public key material");
            return -1;
        }
        *bits_out = first_bits;
        return 0;
    }
    if (algorithm == 16U) {
        size_t material_offset = *offset_io;

        if (pgp_read_mpi_bits(body, body_size, offset_io, &first_bits) != 0 ||
            pgp_read_mpi_bits(body, body_size, offset_io, &ignored_bits) != 0 ||
            pgp_read_mpi_bits(body, body_size, offset_io, &ignored_bits) != 0) {
            pgp_set_error(error, error_size, "truncated Elgamal public key material");
            return -1;
        }
        pgp_store_public_material(info, body + material_offset, *offset_io - material_offset);
        *bits_out = first_bits;
        return 0;
    }
    if (algorithm == 17U) {
        if (pgp_read_mpi_bits(body, body_size, offset_io, &first_bits) != 0 ||
            pgp_read_mpi_bits(body, body_size, offset_io, &ignored_bits) != 0 ||
            pgp_read_mpi_bits(body, body_size, offset_io, &ignored_bits) != 0 ||
            pgp_read_mpi_bits(body, body_size, offset_io, &ignored_bits) != 0) {
            pgp_set_error(error, error_size, "truncated DSA public key material");
            return -1;
        }
        *bits_out = first_bits;
        return 0;
    }
    if (algorithm == 18U || algorithm == 19U || algorithm == 22U) {
        if (pgp_skip_ec_public_key_material(info, body, body_size, offset_io, error, error_size) != 0) return -1;
        if (algorithm == 18U) {
            unsigned int kdf_size;

            if (*offset_io >= body_size) {
                pgp_set_error(error, error_size, "truncated OpenPGP ECDH KDF parameters");
                return -1;
            }
            kdf_size = body[(*offset_io)++];
            if (kdf_size > body_size - *offset_io) {
                pgp_set_error(error, error_size, "truncated OpenPGP ECDH KDF parameters");
                return -1;
            }
            *offset_io += kdf_size;
        }
        *bits_out = 256U;
        return 0;
    }
    if (algorithm == 25U || algorithm == 27U) {
        if (*offset_io + 32U > body_size) {
            pgp_set_error(error, error_size, algorithm == 25U ? "truncated OpenPGP X25519 public key material" : "truncated OpenPGP Ed25519 public key material");
            return -1;
        }
        pgp_store_public_material(info, body + *offset_io, 32U);
        *offset_io += 32U;
        *bits_out = 256U;
        return 0;
    }
    return 0;
}

int pgp_parse_public_key_packet(PgpPublicKeyInfo *info, unsigned int tag, const unsigned char *body, size_t body_size, char *error, size_t error_size) {
    size_t offset;
    size_t public_body_size;
    unsigned long long public_material_size;

    rt_memset(info, 0, sizeof(*info));
    if (body_size < 6U) {
        pgp_set_error(error, error_size, "truncated OpenPGP public key packet");
        return -1;
    }
    info->tag = tag;
    info->version = body[0];
    if (info->version != 4U && info->version != 6U) {
        info->algorithm = body_size > 5U ? body[5] : 0U;
        info->present = 1;
        return 0;
    }
    info->created = pgp_read_u32_be(body + 1U);
    info->algorithm = body[5];

    if (info->version == 6U) {
        if (body_size < 10U) {
            pgp_set_error(error, error_size, "truncated OpenPGP v6 public key packet");
            return -1;
        }
        public_material_size = pgp_read_u32_be(body + 6U);
        if (public_material_size > (unsigned long long)(body_size - 10U)) {
            pgp_set_error(error, error_size, "truncated OpenPGP v6 public key material");
            return -1;
        }
        offset = 10U;
        public_body_size = 10U + (size_t)public_material_size;
        if (pgp_skip_public_key_material(info, info->algorithm, body, public_body_size, &offset, &info->bits, error, error_size) != 0) return -1;
        if (offset != public_body_size) {
            pgp_set_error(error, error_size, "malformed OpenPGP v6 public key material");
            return -1;
        }
        {
            CryptoSha256Context sha256;
            unsigned char prefix[5];

            prefix[0] = 0x9bU;
            prefix[1] = (unsigned char)(((unsigned long long)public_body_size >> 24U) & 0xffU);
            prefix[2] = (unsigned char)(((unsigned long long)public_body_size >> 16U) & 0xffU);
            prefix[3] = (unsigned char)(((unsigned long long)public_body_size >> 8U) & 0xffU);
            prefix[4] = (unsigned char)((unsigned long long)public_body_size & 0xffU);
            crypto_sha256_init(&sha256);
            crypto_sha256_update(&sha256, prefix, sizeof(prefix));
            crypto_sha256_update(&sha256, body, public_body_size);
            crypto_sha256_final(&sha256, info->fingerprint);
            info->fingerprint_size = CRYPTO_SHA256_DIGEST_SIZE;
            memcpy(info->key_id, info->fingerprint, PGP_KEY_ID_SIZE);
        }
        info->present = 1;
        return 0;
    }

    offset = 6U;
    if (pgp_skip_public_key_material(info, info->algorithm, body, body_size, &offset, &info->bits, error, error_size) != 0) return -1;
    public_body_size = tag == 5U || tag == 7U ? offset : body_size;
    if (public_body_size <= 65535U) {
        CryptoSha1Context sha1;
        unsigned char prefix[3];

        prefix[0] = 0x99U;
        prefix[1] = (unsigned char)((public_body_size >> 8U) & 0xffU);
        prefix[2] = (unsigned char)(public_body_size & 0xffU);
        crypto_sha1_init(&sha1);
        crypto_sha1_update(&sha1, prefix, sizeof(prefix));
        crypto_sha1_update(&sha1, body, public_body_size);
        crypto_sha1_final(&sha1, info->fingerprint);
        info->fingerprint_size = CRYPTO_SHA1_DIGEST_SIZE;
        memcpy(info->key_id, info->fingerprint + CRYPTO_SHA1_DIGEST_SIZE - PGP_KEY_ID_SIZE, PGP_KEY_ID_SIZE);
    }
    info->present = 1;
    return 0;
}

static void pgp_certificate_init(PgpCertificateInfo *certificate, size_t start_offset) {
    rt_memset(certificate, 0, sizeof(*certificate));
    certificate->start_offset = start_offset;
    certificate->end_offset = start_offset;
}

static void pgp_copy_user_id(PgpCertificateInfo *certificate, const unsigned char *body, size_t body_size) {
    size_t copy_size;

    if (certificate->user_id_count >= PGP_MAX_USER_IDS) {
        return;
    }
    copy_size = body_size < PGP_USER_ID_CAPACITY - 1U ? body_size : PGP_USER_ID_CAPACITY - 1U;
    if (copy_size != 0U) memcpy(certificate->user_ids[certificate->user_id_count], body, copy_size);
    certificate->user_ids[certificate->user_id_count][copy_size] = '\0';
    certificate->user_id_count += 1U;
}

static void pgp_copy_limited_bytes(unsigned char *dst, size_t dst_capacity, size_t *dst_size, const unsigned char *src, size_t src_size) {
    size_t copy_size = src_size < dst_capacity ? src_size : dst_capacity;

    if (copy_size != 0U) memcpy(dst, src, copy_size);
    *dst_size = copy_size;
}

static int pgp_parse_signature_subpacket_length(const unsigned char *data, size_t size, size_t *offset_io, size_t *length_out) {
    unsigned int first;

    if (*offset_io >= size) return -1;
    first = data[(*offset_io)++];
    if (first < 192U) {
        *length_out = first;
        return 0;
    }
    if (first < 255U) {
        unsigned int second;

        if (*offset_io >= size) return -1;
        second = data[(*offset_io)++];
        *length_out = ((size_t)(first - 192U) << 8U) + (size_t)second + 192U;
        return 0;
    }
    if (*offset_io + 4U > size) return -1;
    *length_out = (size_t)pgp_read_u32_be(data + *offset_io);
    *offset_io += 4U;
    return 0;
}

static void pgp_parse_signature_subpackets(PgpSignatureInfo *info, const unsigned char *data, size_t size) {
    size_t offset = 0U;

    while (offset < size) {
        size_t subpacket_length;
        unsigned int type;
        const unsigned char *body;
        size_t body_size;

        if (pgp_parse_signature_subpacket_length(data, size, &offset, &subpacket_length) != 0 || subpacket_length == 0U || subpacket_length > size - offset) {
            break;
        }
        type = data[offset++] & 0x7fU;
        body = data + offset;
        body_size = subpacket_length - 1U;
        offset += body_size;

        if (type == 2U && body_size >= 4U) {
            info->created = pgp_read_u32_be_ull(body);
        } else if (type == 3U && body_size >= 4U) {
            info->signature_expiration_seconds = pgp_read_u32_be_ull(body);
            info->has_signature_expiration = 1;
        } else if (type == 9U && body_size >= 4U) {
            info->key_expiration_seconds = pgp_read_u32_be_ull(body);
            info->has_key_expiration = 1;
        } else if (type == 11U) {
            pgp_copy_limited_bytes(info->preferred_symmetric, PGP_SIGNATURE_PREFERENCE_CAPACITY, &info->preferred_symmetric_count, body, body_size);
        } else if (type == 16U && body_size >= PGP_KEY_ID_SIZE) {
            memcpy(info->issuer_key_id, body + body_size - PGP_KEY_ID_SIZE, PGP_KEY_ID_SIZE);
            info->has_issuer_key_id = 1;
        } else if (type == 21U) {
            pgp_copy_limited_bytes(info->preferred_hash, PGP_SIGNATURE_PREFERENCE_CAPACITY, &info->preferred_hash_count, body, body_size);
        } else if (type == 22U) {
            pgp_copy_limited_bytes(info->preferred_compression, PGP_SIGNATURE_PREFERENCE_CAPACITY, &info->preferred_compression_count, body, body_size);
        } else if (type == 25U && body_size >= 1U) {
            info->has_primary_user_id = 1;
            info->primary_user_id = body[0] != 0U;
        } else if (type == 27U) {
            pgp_copy_limited_bytes(info->key_flags, PGP_SIGNATURE_KEY_FLAGS_CAPACITY, &info->key_flags_size, body, body_size);
        } else if (type == 30U) {
            pgp_copy_limited_bytes(info->features, PGP_SIGNATURE_FEATURE_CAPACITY, &info->feature_count, body, body_size);
        } else if (type == 33U && body_size >= 2U) {
            size_t fingerprint_offset = 1U;
            pgp_copy_limited_bytes(info->issuer_fingerprint, PGP_FINGERPRINT_MAX_SIZE, &info->issuer_fingerprint_size, body + fingerprint_offset, body_size - fingerprint_offset);
        }
    }
}

static int pgp_parse_signature_packet(PgpSignatureInfo *info, unsigned int target_tag, size_t target_index, unsigned long long packet_index, const unsigned char *body, size_t body_size, char *error, size_t error_size) {
    size_t hashed_size;
    size_t unhashed_offset;
    size_t unhashed_size;

    rt_memset(info, 0, sizeof(*info));
    if (body_size < 1U) {
        pgp_set_error(error, error_size, "truncated OpenPGP signature packet");
        return -1;
    }
    info->version = body[0];
    info->target_tag = target_tag;
    info->target_index = target_index;
    info->packet_index = packet_index;
    if (info->version == 3U) {
        if (body_size >= 17U) {
            info->signature_type = body[2];
            info->created = pgp_read_u32_be(body + 3U);
            memcpy(info->issuer_key_id, body + 7U, PGP_KEY_ID_SIZE);
            info->has_issuer_key_id = 1;
            info->public_key_algorithm = body[15];
            info->hash_algorithm = body[16];
        }
        info->present = 1;
        return 0;
    }
    if (info->version != 4U && info->version != 6U) {
        info->signature_type = body_size > 2U ? body[2] : 0U;
        info->public_key_algorithm = body_size > 3U ? body[3] : 0U;
        info->hash_algorithm = body_size > 4U ? body[4] : 0U;
        info->present = 1;
        return 0;
    }
    if (info->version == 6U) {
        if (body_size < 8U) {
            pgp_set_error(error, error_size, "truncated OpenPGP v6 signature packet");
            return -1;
        }
        info->signature_type = body[1];
        info->public_key_algorithm = body[2];
        info->hash_algorithm = body[3];
        hashed_size = pgp_read_u32_be(body + 4U);
        if (hashed_size > body_size - 8U) {
            pgp_set_error(error, error_size, "truncated OpenPGP v6 signature hashed subpackets");
            return -1;
        }
        pgp_parse_signature_subpackets(info, body + 8U, hashed_size);
        unhashed_offset = 8U + hashed_size;
        if (unhashed_offset + 4U > body_size) {
            pgp_set_error(error, error_size, "truncated OpenPGP v6 signature unhashed subpacket length");
            return -1;
        }
        unhashed_size = pgp_read_u32_be(body + unhashed_offset);
        unhashed_offset += 4U;
        if (unhashed_size > body_size - unhashed_offset) {
            pgp_set_error(error, error_size, "truncated OpenPGP v6 signature unhashed subpackets");
            return -1;
        }
        pgp_parse_signature_subpackets(info, body + unhashed_offset, unhashed_size);
        info->present = 1;
        return 0;
    }
    if (body_size < 6U) {
        pgp_set_error(error, error_size, "truncated OpenPGP v4 signature packet");
        return -1;
    }
    info->signature_type = body[1];
    info->public_key_algorithm = body[2];
    info->hash_algorithm = body[3];
    hashed_size = pgp_read_u16_be(body + 4U);
    if (hashed_size > body_size - 6U) {
        pgp_set_error(error, error_size, "truncated OpenPGP signature hashed subpackets");
        return -1;
    }
    pgp_parse_signature_subpackets(info, body + 6U, hashed_size);
    unhashed_offset = 6U + hashed_size;
    if (unhashed_offset + 2U > body_size) {
        pgp_set_error(error, error_size, "truncated OpenPGP signature unhashed subpacket length");
        return -1;
    }
    unhashed_size = pgp_read_u16_be(body + unhashed_offset);
    unhashed_offset += 2U;
    if (unhashed_size > body_size - unhashed_offset) {
        pgp_set_error(error, error_size, "truncated OpenPGP signature unhashed subpackets");
        return -1;
    }
    pgp_parse_signature_subpackets(info, body + unhashed_offset, unhashed_size);
    info->present = 1;
    return 0;
}

int pgp_for_each_certificate(const unsigned char *data, size_t size, PgpCertificateCallback callback, void *ctx, char *error, size_t error_size) {
    PgpPacketReader reader;
    PgpCertificateInfo certificate;
    int have_certificate = 0;
    unsigned int current_target_tag = PGP_SIGNATURE_TARGET_PRIMARY;
    size_t current_target_index = 0U;

    pgp_packet_reader_init(&reader, data, size);
    while (1) {
        PgpPacket packet;
        int has_packet;

        if (pgp_packet_reader_next(&reader, &packet, &has_packet, error, error_size) != 0) return -1;
        if (!has_packet) break;

        if (packet.tag == 6U || packet.tag == 5U) {
            if (have_certificate) {
                certificate.end_offset = packet.header_offset;
                if (callback(&certificate, ctx) != 0) return -1;
            }
            pgp_certificate_init(&certificate, packet.header_offset);
            have_certificate = 1;
            current_target_tag = PGP_SIGNATURE_TARGET_PRIMARY;
            current_target_index = 0U;
            certificate.packet_count += 1ULL;
            if (pgp_parse_public_key_packet(&certificate.primary, packet.tag, data + packet.body_offset, packet.body_size, error, error_size) != 0) return -1;
            continue;
        }
        if (!have_certificate) {
            continue;
        }
        certificate.packet_count += 1ULL;
        if (packet.tag == 14U || packet.tag == 7U) {
            if (certificate.subkey_count < PGP_MAX_SUBKEYS) {
                if (pgp_parse_public_key_packet(&certificate.subkeys[certificate.subkey_count], packet.tag, data + packet.body_offset, packet.body_size, error, error_size) != 0) return -1;
                current_target_tag = PGP_SIGNATURE_TARGET_SUBKEY;
                current_target_index = certificate.subkey_count;
                certificate.subkey_count += 1U;
            }
        } else if (packet.tag == 13U) {
            size_t old_user_id_count = certificate.user_id_count;
            pgp_copy_user_id(&certificate, data + packet.body_offset, packet.body_size);
            if (certificate.user_id_count > old_user_id_count) {
                current_target_tag = PGP_SIGNATURE_TARGET_USER_ID;
                current_target_index = certificate.user_id_count - 1U;
            }
        } else if (packet.tag == 17U) {
            current_target_tag = PGP_SIGNATURE_TARGET_USER_ATTRIBUTE;
            current_target_index = (size_t)certificate.user_attribute_count;
            certificate.user_attribute_count += 1ULL;
        } else if (packet.tag == 2U) {
            if (certificate.signature_count < PGP_MAX_SIGNATURES) certificate.signature_count += 1ULL;
            if (certificate.signature_info_count < PGP_MAX_SIGNATURE_INFOS) {
                if (pgp_parse_signature_packet(&certificate.signatures[certificate.signature_info_count], current_target_tag, current_target_index, certificate.packet_count, data + packet.body_offset, packet.body_size, error, error_size) != 0) return -1;
                certificate.signature_info_count += 1U;
            }
        }
    }
    if (have_certificate) {
        certificate.end_offset = size;
        if (callback(&certificate, ctx) != 0) return -1;
    }
    return 0;
}

int pgp_parse_fingerprint_text(const char *text, unsigned char out[PGP_FINGERPRINT_MAX_SIZE], size_t *size_out) {
    size_t hex_count = 0U;
    int high = -1;
    size_t out_size = 0U;

    while (*text != '\0') {
        int value = pgp_hex_value(*text);

        if (value < 0) {
            if (*text == ' ' || *text == ':' || *text == '-' || *text == '\t' || *text == '\n' || *text == '\r') {
                text += 1;
                continue;
            }
            return -1;
        }
        if (high < 0) {
            high = value;
        } else {
            if (out_size >= PGP_FINGERPRINT_MAX_SIZE) return -1;
            out[out_size++] = (unsigned char)((high << 4U) | value);
            high = -1;
        }
        hex_count += 1U;
        text += 1;
    }
    if (high >= 0 || hex_count == 0U) return -1;
    *size_out = out_size;
    return 0;
}

int pgp_fingerprint_matches_text(const PgpPublicKeyInfo *key, const char *text) {
    unsigned char parsed[PGP_FINGERPRINT_MAX_SIZE];
    size_t parsed_size;

    if (key == 0 || key->fingerprint_size == 0U || pgp_parse_fingerprint_text(text, parsed, &parsed_size) != 0) {
        return 0;
    }
    if (parsed_size == key->fingerprint_size) {
        return memcmp(parsed, key->fingerprint, parsed_size) == 0;
    }
    if (parsed_size == PGP_KEY_ID_SIZE && key->fingerprint_size >= PGP_KEY_ID_SIZE) {
        return memcmp(parsed, key->key_id, PGP_KEY_ID_SIZE) == 0;
    }
    return 0;
}

const char *pgp_packet_tag_name(unsigned int tag) {
    if (tag == 1U) return "public-key encrypted session key";
    if (tag == 2U) return "signature";
    if (tag == 3U) return "symmetric-key encrypted session key";
    if (tag == 4U) return "one-pass signature";
    if (tag == 5U) return "secret key";
    if (tag == 6U) return "public key";
    if (tag == 7U) return "secret subkey";
    if (tag == 8U) return "compressed data";
    if (tag == 9U) return "symmetrically encrypted data";
    if (tag == 11U) return "literal data";
    if (tag == 13U) return "user ID";
    if (tag == 14U) return "public subkey";
    if (tag == 17U) return "user attribute";
    if (tag == 18U) return "symmetrically encrypted integrity protected data";
    if (tag == 19U) return "modification detection code";
    return "unknown";
}

const char *pgp_public_key_algorithm_name(unsigned int algorithm) {
    if (algorithm == 1U) return "RSA encrypt/sign";
    if (algorithm == 2U) return "RSA encrypt-only";
    if (algorithm == 3U) return "RSA sign-only";
    if (algorithm == 16U) return "Elgamal encrypt-only";
    if (algorithm == 17U) return "DSA";
    if (algorithm == 18U) return "ECDH";
    if (algorithm == 19U) return "ECDSA";
    if (algorithm == 22U) return "EdDSA legacy";
    if (algorithm == 25U) return "X25519";
    if (algorithm == 27U) return "Ed25519";
    return "unknown";
}

const char *pgp_signature_type_name(unsigned int signature_type) {
    if (signature_type == 0x00U) return "binary document signature";
    if (signature_type == 0x01U) return "canonical text signature";
    if (signature_type == 0x10U) return "generic user ID certification";
    if (signature_type == 0x11U) return "persona user ID certification";
    if (signature_type == 0x12U) return "casual user ID certification";
    if (signature_type == 0x13U) return "positive user ID certification";
    if (signature_type == 0x18U) return "subkey binding";
    if (signature_type == 0x19U) return "primary key binding";
    if (signature_type == 0x1fU) return "direct key signature";
    if (signature_type == 0x20U) return "key revocation";
    if (signature_type == 0x28U) return "subkey revocation";
    if (signature_type == 0x30U) return "certification revocation";
    if (signature_type == 0x40U) return "timestamp signature";
    if (signature_type == 0x50U) return "third-party confirmation";
    return "unknown signature";
}

const char *pgp_hash_algorithm_name(unsigned int algorithm) {
    if (algorithm == 1U) return "MD5";
    if (algorithm == 2U) return "SHA-1";
    if (algorithm == 3U) return "RIPEMD-160";
    if (algorithm == 8U) return "SHA-256";
    if (algorithm == 9U) return "SHA-384";
    if (algorithm == 10U) return "SHA-512";
    if (algorithm == 11U) return "SHA-224";
    return "unknown";
}

const char *pgp_symmetric_algorithm_name(unsigned int algorithm) {
    if (algorithm == 0U) return "Plaintext";
    if (algorithm == 1U) return "IDEA";
    if (algorithm == 2U) return "TripleDES";
    if (algorithm == 3U) return "CAST5";
    if (algorithm == 7U) return "AES-128";
    if (algorithm == 8U) return "AES-192";
    if (algorithm == 9U) return "AES-256";
    if (algorithm == 10U) return "Twofish";
    if (algorithm == 11U) return "Camellia-128";
    if (algorithm == 12U) return "Camellia-192";
    if (algorithm == 13U) return "Camellia-256";
    return "unknown";
}

const char *pgp_compression_algorithm_name(unsigned int algorithm) {
    if (algorithm == 0U) return "uncompressed";
    if (algorithm == 1U) return "ZIP";
    if (algorithm == 2U) return "ZLIB";
    if (algorithm == 3U) return "BZip2";
    return "unknown";
}

const char *pgp_key_kind_name(unsigned int tag) {
    if (tag == 5U) return "secret primary";
    if (tag == 6U) return "public primary";
    if (tag == 7U) return "secret subkey";
    if (tag == 14U) return "public subkey";
    return "key";
}