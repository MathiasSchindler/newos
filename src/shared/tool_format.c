#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

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

size_t tool_count_decimal_digits(unsigned long long value) {
    size_t digits = 1U;

    while (value >= 10ULL) {
        value /= 10ULL;
        digits += 1U;
    }

    return digits;
}
