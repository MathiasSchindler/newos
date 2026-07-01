#include "archive_util.h"
#include "compression/crc32.h"
#include "platform.h"

unsigned int archive_crc32_update(unsigned int crc, const unsigned char *data, size_t length) {
    return compression_crc32_update(crc, data, length);
}

unsigned int archive_crc32_finish(unsigned int crc) {
    return compression_crc32_finish(crc);
}

int archive_read_exact(int fd, unsigned char *buffer, size_t count) {
    size_t offset = 0U;

    while (offset < count) {
        long bytes = platform_read(fd, buffer + offset, count - offset);
        if (bytes <= 0) return -1;
        offset += (size_t)bytes;
    }
    return 0;
}

int archive_read_file_region(int fd, unsigned long long offset, unsigned char *buffer, size_t count) {
    if (platform_seek(fd, (long long)offset, PLATFORM_SEEK_SET) < 0) return -1;
    return archive_read_exact(fd, buffer, count);
}

int archive_read_region(int fd, unsigned long long base, unsigned long long offset, unsigned char *buffer, size_t count) {
    return archive_read_file_region(fd, base + offset, buffer, count);
}

int archive_has_ar_magic(const unsigned char *buffer, unsigned long long size) {
    static const unsigned char magic[] = { '!', '<', 'a', 'r', 'c', 'h', '>', '\n' };
    size_t i;

    if (size < sizeof(magic)) return 0;
    for (i = 0U; i < sizeof(magic); ++i) {
        if (buffer[i] != magic[i]) return 0;
    }
    return 1;
}

unsigned short archive_read_u16_le(const unsigned char *bytes) {
    return (unsigned short)((unsigned short)bytes[0] | ((unsigned short)bytes[1] << 8U));
}

unsigned int archive_read_u32_le(const unsigned char *bytes) {
    return (unsigned int)bytes[0] |
           ((unsigned int)bytes[1] << 8U) |
           ((unsigned int)bytes[2] << 16U) |
           ((unsigned int)bytes[3] << 24U);
}

unsigned long long archive_read_u64_le(const unsigned char *bytes) {
    return (unsigned long long)archive_read_u32_le(bytes) |
           ((unsigned long long)archive_read_u32_le(bytes + 4U) << 32U);
}

void archive_store_u32_le(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xffU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xffU);
}

void archive_store_u64_le(unsigned char *bytes, unsigned long long value) {
    archive_store_u32_le(bytes, (unsigned int)(value & 0xffffffffULL));
    archive_store_u32_le(bytes + 4U, (unsigned int)((value >> 32U) & 0xffffffffULL));
}

void archive_write_octal(char *field, size_t field_size, unsigned long long value) {
    size_t i;

    for (i = 0U; i < field_size; ++i) field[i] = '0';
    if (field_size > 0U) field[field_size - 1U] = ' ';
    if (field_size > 1U) {
        size_t pos = field_size - 2U;
        field[pos] = '\0';
        do {
            field[pos] = (char)('0' + (value & 7ULL));
            value >>= 3U;
            if (pos == 0U) break;
            pos -= 1U;
        } while (value != 0ULL);
    }
}

unsigned long long archive_parse_octal(const char *field, size_t field_size) {
    unsigned long long value = 0ULL;
    size_t i = 0U;

    while (i < field_size && (field[i] == ' ' || field[i] == '\0')) i += 1U;
    while (i < field_size && field[i] >= '0' && field[i] <= '7') {
        value = (value * 8ULL) + (unsigned long long)(field[i] - '0');
        i += 1U;
    }
    return value;
}