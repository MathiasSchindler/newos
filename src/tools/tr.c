#include "platform.h"
#include "runtime.h"

#define TR_SET_CAPACITY 512

typedef struct {
    int complement_set1;
    int delete_chars;
    int squeeze_chars;
    int translate_chars;
    int squeeze_complement;
    unsigned int set1[TR_SET_CAPACITY];
    size_t set1_len;
    unsigned int set2[TR_SET_CAPACITY];
    size_t set2_len;
    unsigned int squeeze_set[TR_SET_CAPACITY];
    size_t squeeze_set_len;
} TrOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-c] [-d] [-s] SET1 [SET2]");
}

static int class_name_equals(const char *name, size_t name_len, const char *literal) {
    size_t i = 0U;

    while (i < name_len && literal[i] != '\0') {
        if (name[i] != literal[i]) {
            return 0;
        }
        i += 1U;
    }

    return i == name_len && literal[i] == '\0';
}

static int tr_is_upper(unsigned char ch) {
    return ch >= 'A' && ch <= 'Z';
}

static int tr_is_lower(unsigned char ch) {
    return ch >= 'a' && ch <= 'z';
}

static int tr_is_digit(unsigned char ch) {
    return ch >= '0' && ch <= '9';
}

static int tr_is_alpha(unsigned char ch) {
    return tr_is_upper(ch) || tr_is_lower(ch);
}

static int tr_is_alnum(unsigned char ch) {
    return tr_is_alpha(ch) || tr_is_digit(ch);
}

static int tr_is_space(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
}

static int tr_is_blank(unsigned char ch) {
    return ch == ' ' || ch == '\t';
}

static int tr_is_xdigit(unsigned char ch) {
    return tr_is_digit(ch) ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

static int tr_is_cntrl(unsigned char ch) {
    return ch < 32U || ch == 127U;
}

static int tr_is_print(unsigned char ch) {
    return ch >= 32U && ch <= 126U;
}

static int tr_is_graph(unsigned char ch) {
    return ch >= 33U && ch <= 126U;
}

static int tr_is_punct(unsigned char ch) {
    return tr_is_graph(ch) && !tr_is_alnum(ch);
}

static int tr_class_matches(const char *name, size_t name_len, unsigned char ch) {
    if (class_name_equals(name, name_len, "digit")) {
        return tr_is_digit(ch);
    }
    if (class_name_equals(name, name_len, "lower")) {
        return tr_is_lower(ch);
    }
    if (class_name_equals(name, name_len, "upper")) {
        return tr_is_upper(ch);
    }
    if (class_name_equals(name, name_len, "alpha")) {
        return tr_is_alpha(ch);
    }
    if (class_name_equals(name, name_len, "alnum")) {
        return tr_is_alnum(ch);
    }
    if (class_name_equals(name, name_len, "space")) {
        return tr_is_space(ch);
    }
    if (class_name_equals(name, name_len, "blank")) {
        return tr_is_blank(ch);
    }
    if (class_name_equals(name, name_len, "xdigit")) {
        return tr_is_xdigit(ch);
    }
    if (class_name_equals(name, name_len, "cntrl")) {
        return tr_is_cntrl(ch);
    }
    if (class_name_equals(name, name_len, "print")) {
        return tr_is_print(ch);
    }
    if (class_name_equals(name, name_len, "graph")) {
        return tr_is_graph(ch);
    }
    if (class_name_equals(name, name_len, "punct")) {
        return tr_is_punct(ch);
    }
    return 0;
}

static int append_codepoint(unsigned int *buffer, size_t buffer_size, size_t *length, unsigned int codepoint) {
    if (*length >= buffer_size) {
        return -1;
    }
    buffer[*length] = codepoint;
    *length += 1U;
    return 0;
}

static int append_class_chars(unsigned int *buffer, size_t buffer_size, size_t *length, const char *name, size_t name_len) {
    unsigned int ch;

    for (ch = 0U; ch <= 255U; ++ch) {
        if (tr_class_matches(name, name_len, (unsigned char)ch) &&
            append_codepoint(buffer, buffer_size, length, ch) != 0) {
            return -1;
        }
    }

    return 0;
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static size_t decode_escape_codepoint(const char *text, size_t index, unsigned int *out) {
    unsigned char code;
    size_t pos;
    int value;
    size_t text_length;
    size_t decode_index;

    if (text[index] != '\\' || text[index + 1U] == '\0') {
        text_length = rt_strlen(text);
        decode_index = index;
        if (rt_utf8_decode(text, text_length, &decode_index, out) == 0 && decode_index > index) {
            return decode_index - index;
        }
        *out = (unsigned char)text[index];
        return 1U;
    }

    code = (unsigned char)text[index + 1U];
    switch (code) {
        case 'a':
            *out = '\a';
            return 2U;
        case 'b':
            *out = '\b';
            return 2U;
        case 'f':
            *out = '\f';
            return 2U;
        case 'n':
            *out = '\n';
            return 2U;
        case 'r':
            *out = '\r';
            return 2U;
        case 't':
            *out = '\t';
            return 2U;
        case 'v':
            *out = '\v';
            return 2U;
        case '\\':
            *out = '\\';
            return 2U;
        case 'x':
            value = 0;
            pos = index + 2U;
            if (hex_value(text[pos]) < 0) {
                *out = 'x';
                return 2U;
            }
            while (hex_value(text[pos]) >= 0) {
                value = (value * 16) + hex_value(text[pos]);
                pos += 1U;
            }
            *out = (unsigned int)value;
            return pos - index;
        default:
            if (code >= '0' && code <= '7') {
                value = 0;
                pos = index + 1U;
                while (pos < index + 4U && text[pos] >= '0' && text[pos] <= '7') {
                    value = (value * 8) + (text[pos] - '0');
                    pos += 1U;
                }
                *out = (unsigned int)value;
                return pos - index;
            }
            *out = code;
            return 2U;
    }
}

static size_t parse_class_token(const char *text, size_t index, const char **name_out, size_t *name_len_out) {
    size_t end = index + 2U;

    if (text[index] != '[' || text[index + 1U] != ':') {
        return 0U;
    }

    while (text[end] != '\0') {
        if (text[end] == ':' && text[end + 1U] == ']') {
            *name_out = text + index + 2U;
            *name_len_out = end - (index + 2U);
            return (end + 2U) - index;
        }
        end += 1U;
    }

    return 0U;
}

static int expand_set(const char *text, unsigned int *buffer, size_t buffer_size, size_t *length_out) {
    size_t length = 0U;
    size_t i = 0U;

    while (text[i] != '\0') {
        const char *class_name = 0;
        size_t class_name_len = 0U;
        size_t class_len = parse_class_token(text, i, &class_name, &class_name_len);
        unsigned int first;
        size_t first_len;

        if (class_len != 0U) {
            if (append_class_chars(buffer, buffer_size, &length, class_name, class_name_len) != 0) {
                return -1;
            }
            i += class_len;
            continue;
        }

        first_len = decode_escape_codepoint(text, i, &first);
        if (text[i + first_len] == '-' && text[i + first_len + 1U] != '\0') {
            const char *range_class = 0;
            size_t range_class_name_len = 0U;
            size_t range_class_len = parse_class_token(text, i + first_len + 1U, &range_class, &range_class_name_len);

            if (range_class_len == 0U) {
                unsigned int last;
                size_t last_len = decode_escape_codepoint(text, i + first_len + 1U, &last);
                int step = (first <= last) ? 1 : -1;
                unsigned int value = first;

                while (1) {
                    if (append_codepoint(buffer, buffer_size, &length, value) != 0) {
                        return -1;
                    }
                    if (value == last) {
                        break;
                    }
                    value = (unsigned int)((int)value + step);
                }

                i += first_len + 1U + last_len;
                continue;
            }
        }

        if (append_codepoint(buffer, buffer_size, &length, first) != 0) {
            return -1;
        }
        i += first_len;
    }

    *length_out = length;
    return 0;
}

static int codepoint_in_set(unsigned int ch, const unsigned int *set, size_t set_len) {
    size_t i = 0U;
    while (i < set_len) {
        if (set[i] == ch) {
            return 1;
        }
        i += 1U;
    }
    return 0;
}

static int codepoint_in_effective_set(unsigned int ch, const unsigned int *set, size_t set_len, int complement) {
    int present = codepoint_in_set(ch, set, set_len);
    return complement ? !present : present;
}

static unsigned int translate_codepoint(unsigned int ch,
                                        const unsigned int *set1,
                                        size_t set1_len,
                                        const unsigned int *set2,
                                        size_t set2_len,
                                        int complement) {
    size_t i;

    if (complement) {
        if (!codepoint_in_set(ch, set1, set1_len)) {
            if (set2_len == 0U) {
                return ch;
            }
            return set2[set2_len - 1U];
        }
        return ch;
    }

    for (i = 0U; i < set1_len; ++i) {
        if (set1[i] == ch) {
            if (set2_len == 0U) {
                return ch;
            }
            if (i < set2_len) {
                return set2[i];
            }
            return set2[set2_len - 1U];
        }
    }

    return ch;
}

static size_t decode_input_codepoint(const char *buffer, size_t length, size_t start, unsigned int *codepoint_out, int *incomplete_out) {
    size_t index = start;
    unsigned char lead;
    size_t needed = 1U;

    *incomplete_out = 0;
    if (start >= length) {
        *codepoint_out = 0U;
        return 0U;
    }

    lead = (unsigned char)buffer[start];
    if ((lead & 0x80U) == 0U) {
        needed = 1U;
    } else if ((lead & 0xe0U) == 0xc0U) {
        needed = 2U;
    } else if ((lead & 0xf0U) == 0xe0U) {
        needed = 3U;
    } else if ((lead & 0xf8U) == 0xf0U) {
        needed = 4U;
    }

    if (needed > 1U && length - start < needed) {
        *incomplete_out = 1;
        return 0U;
    }

    if (rt_utf8_decode(buffer, length, &index, codepoint_out) == 0 && index > start) {
        return index - start;
    }

    *codepoint_out = lead;
    return 1U;
}

static int parse_options(int argc, char **argv, TrOptions *options) {
    int arg_index = 1;
    int remaining;

    rt_memset(options, 0, sizeof(*options));

    while (arg_index < argc && argv[arg_index][0] == '-' && argv[arg_index][1] != '\0') {
        const char *flag = argv[arg_index] + 1;

        if (rt_strcmp(argv[arg_index], "--") == 0) {
            arg_index += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 'c') {
                options->complement_set1 = 1;
            } else if (*flag == 'd') {
                options->delete_chars = 1;
            } else if (*flag == 's') {
                options->squeeze_chars = 1;
            } else {
                return -1;
            }
            flag += 1;
        }

        arg_index += 1;
    }

    remaining = argc - arg_index;
    if (!options->delete_chars && !options->squeeze_chars) {
        if (remaining != 2) {
            return -1;
        }
        if (expand_set(argv[arg_index], options->set1, sizeof(options->set1), &options->set1_len) != 0 ||
            expand_set(argv[arg_index + 1], options->set2, sizeof(options->set2), &options->set2_len) != 0) {
            return -1;
        }
        options->translate_chars = 1;
        return 0;
    }

    if (options->delete_chars && !options->squeeze_chars) {
        if (remaining != 1) {
            return -1;
        }
        return expand_set(argv[arg_index], options->set1, sizeof(options->set1), &options->set1_len);
    }

    if (!options->delete_chars && options->squeeze_chars) {
        if (remaining != 1 && remaining != 2) {
            return -1;
        }
        if (expand_set(argv[arg_index], options->set1, sizeof(options->set1), &options->set1_len) != 0) {
            return -1;
        }
        if (remaining == 2) {
            if (expand_set(argv[arg_index + 1], options->set2, sizeof(options->set2), &options->set2_len) != 0) {
                return -1;
            }
            options->translate_chars = 1;
            memcpy(options->squeeze_set, options->set2, options->set2_len);
            options->squeeze_set_len = options->set2_len;
            options->squeeze_complement = 0;
        } else {
            memcpy(options->squeeze_set, options->set1, options->set1_len);
            options->squeeze_set_len = options->set1_len;
            options->squeeze_complement = options->complement_set1;
        }
        return 0;
    }

    if (remaining != 1 && remaining != 2) {
        return -1;
    }

    if (expand_set(argv[arg_index], options->set1, sizeof(options->set1), &options->set1_len) != 0) {
        return -1;
    }

    if (remaining == 2) {
        options->squeeze_complement = 0;
        return expand_set(argv[arg_index + 1], options->squeeze_set, sizeof(options->squeeze_set), &options->squeeze_set_len);
    }

    memcpy(options->squeeze_set, options->set1, options->set1_len);
    options->squeeze_set_len = options->set1_len;
    options->squeeze_complement = options->complement_set1;
    return 0;
}

int main(int argc, char **argv) {
    TrOptions options;
    char read_buffer[4096];
    char input[4104];
    char carry[8];
    size_t carry_len = 0U;
    long bytes_read;
    int have_last_output = 0;
    unsigned int last_output = 0U;

    if (parse_options(argc, argv, &options) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    while ((bytes_read = platform_read(0, read_buffer, sizeof(read_buffer))) > 0) {
        char out[4096];
        size_t out_len = 0U;
        size_t total_len;
        size_t index = 0U;

        if (carry_len > 0U) {
            memcpy(input, carry, carry_len);
        }
        memcpy(input + carry_len, read_buffer, (size_t)bytes_read);
        total_len = carry_len + (size_t)bytes_read;

        while (index < total_len) {
            unsigned int original = 0U;
            unsigned int current;
            int incomplete = 0;
            size_t advance = decode_input_codepoint(input, total_len, index, &original, &incomplete);
            char encoded[8];
            size_t encoded_len = 0U;

            if (incomplete) {
                break;
            }

            current = original;
            if (options.translate_chars) {
                current = translate_codepoint(current,
                                              options.set1,
                                              options.set1_len,
                                              options.set2,
                                              options.set2_len,
                                              options.complement_set1);
            }

            if (options.delete_chars &&
                codepoint_in_effective_set(original, options.set1, options.set1_len, options.complement_set1)) {
                index += advance;
                continue;
            }

            if (options.squeeze_chars &&
                have_last_output &&
                current == last_output &&
                codepoint_in_effective_set(current, options.squeeze_set, options.squeeze_set_len, options.squeeze_complement)) {
                index += advance;
                continue;
            }

            if (rt_utf8_encode(current, encoded, sizeof(encoded), &encoded_len) != 0 || encoded_len == 0U) {
                encoded[0] = (char)(current & 0xffU);
                encoded_len = 1U;
            }

            if (out_len + encoded_len > sizeof(out)) {
                if (rt_write_all(1, out, out_len) != 0) {
                    return 1;
                }
                out_len = 0U;
            }

            memcpy(out + out_len, encoded, encoded_len);
            out_len += encoded_len;
            have_last_output = 1;
            last_output = current;
            index += advance;
        }

        if (out_len > 0U && rt_write_all(1, out, out_len) != 0) {
            return 1;
        }

        carry_len = total_len - index;
        if (carry_len > 0U) {
            memcpy(carry, input + index, carry_len);
        }
    }

    if (bytes_read < 0) {
        return 1;
    }

    if (carry_len > 0U) {
        if (rt_write_all(1, carry, carry_len) != 0) {
            return 1;
        }
    }

    return 0;
}
