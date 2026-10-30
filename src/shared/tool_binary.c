#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

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

char tool_hex_digit(unsigned int value) {
    value &= 0x0fU;
    return (char)(value < 10U ? '0' + value : 'a' + (value - 10U));
}
