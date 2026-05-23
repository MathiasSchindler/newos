#include "linker_internal.h"

void set_link_error(char *error_out, size_t error_size, const char *message, const char *detail) {
    if (error_out == 0 || error_size == 0U) {
        return;
    }
    rt_copy_string(error_out, error_size, message);
    if (detail != 0 && detail[0] != '\0') {
        size_t used = rt_strlen(error_out);
        if (used + 3U < error_size) {
            error_out[used++] = ':';
            error_out[used++] = ' ';
            rt_copy_string(error_out + used, error_size - used, detail);
        }
    }
}

int ends_with_text(const char *text, const char *suffix) {
    size_t text_len = rt_strlen(text);
    size_t suffix_len = rt_strlen(suffix);

    if (suffix_len > text_len) {
        return 0;
    }
    return rt_strcmp(text + text_len - suffix_len, suffix) == 0;
}

uint64_t trim_trailing_zero_bytes(const unsigned char *data, uint64_t size, uint64_t minimum_size) {
    while (size > minimum_size && data[size - 1U] == 0U) {
        size -= 1U;
    }
    return size;
}
