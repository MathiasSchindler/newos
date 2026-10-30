#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

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
