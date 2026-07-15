#include "runtime.h"

static size_t rt_utf8_sequence_length(unsigned char lead) {
    if ((lead & 0x80U) == 0U) {
        return 1U;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2U;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3U;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return 4U;
    }
    return 0U;
}

int rt_utf8_decode(const char *text, size_t text_length, size_t *index_io, unsigned int *codepoint_out) {
    size_t index;
    unsigned char lead;
    size_t length;
    unsigned int codepoint;

    if (text == 0 || index_io == 0 || codepoint_out == 0) {
        return -1;
    }

    index = *index_io;
    if (index >= text_length) {
        return -1;
    }

    lead = (unsigned char)text[index];
    length = rt_utf8_sequence_length(lead);
    if (length == 0U || index + length > text_length) {
        *index_io = index + 1U;
        *codepoint_out = 0xfffdU;
        return -1;
    }

    if (length == 1U) {
        *index_io = index + 1U;
        *codepoint_out = (unsigned int)lead;
        return 0;
    }

    if (length == 2U) {
        unsigned char b1 = (unsigned char)text[index + 1U];
        if ((b1 & 0xc0U) != 0x80U) {
            *index_io = index + 1U;
            *codepoint_out = 0xfffdU;
            return -1;
        }
        codepoint = ((unsigned int)(lead & 0x1fU) << 6) | (unsigned int)(b1 & 0x3fU);
        if (codepoint < 0x80U) {
            *index_io = index + 1U;
            *codepoint_out = 0xfffdU;
            return -1;
        }
    } else if (length == 3U) {
        unsigned char b1 = (unsigned char)text[index + 1U];
        unsigned char b2 = (unsigned char)text[index + 2U];
        if ((b1 & 0xc0U) != 0x80U || (b2 & 0xc0U) != 0x80U) {
            *index_io = index + 1U;
            *codepoint_out = 0xfffdU;
            return -1;
        }
        codepoint = ((unsigned int)(lead & 0x0fU) << 12) |
                    ((unsigned int)(b1 & 0x3fU) << 6) |
                    (unsigned int)(b2 & 0x3fU);
        if (codepoint < 0x800U || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            *index_io = index + 1U;
            *codepoint_out = 0xfffdU;
            return -1;
        }
    } else {
        unsigned char b1 = (unsigned char)text[index + 1U];
        unsigned char b2 = (unsigned char)text[index + 2U];
        unsigned char b3 = (unsigned char)text[index + 3U];
        if ((b1 & 0xc0U) != 0x80U || (b2 & 0xc0U) != 0x80U || (b3 & 0xc0U) != 0x80U) {
            *index_io = index + 1U;
            *codepoint_out = 0xfffdU;
            return -1;
        }
        codepoint = ((unsigned int)(lead & 0x07U) << 18) |
                    ((unsigned int)(b1 & 0x3fU) << 12) |
                    ((unsigned int)(b2 & 0x3fU) << 6) |
                    (unsigned int)(b3 & 0x3fU);
        if (codepoint < 0x10000U || codepoint > 0x10ffffU) {
            *index_io = index + 1U;
            *codepoint_out = 0xfffdU;
            return -1;
        }
    }

    *index_io = index + length;
    *codepoint_out = codepoint;
    return 0;
}

int rt_utf8_validate(const char *text, size_t text_length) {
    size_t index = 0;

    while (index < text_length) {
        unsigned int codepoint = 0;
        if (rt_utf8_decode(text, text_length, &index, &codepoint) != 0) {
            return -1;
        }
    }

    return 0;
}

unsigned long long rt_utf8_codepoint_count(const char *text, size_t text_length) {
    unsigned long long count = 0;
    size_t index = 0;

    while (text != 0 && index < text_length) {
        unsigned int codepoint = 0;
        size_t before = index;
        (void)rt_utf8_decode(text, text_length, &index, &codepoint);
        if (index == before) {
            index += 1U;
        }
        count += 1ULL;
    }

    return count;
}

int rt_utf8_encode(unsigned int codepoint, char *buffer, size_t buffer_size, size_t *length_out) {
    if (buffer == 0 || buffer_size == 0 || length_out == 0 ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU) || codepoint > 0x10ffffU) {
        return -1;
    }

    if (codepoint <= 0x7fU) {
        if (buffer_size < 1U) {
            return -1;
        }
        buffer[0] = (char)codepoint;
        *length_out = 1U;
        return 0;
    }

    if (codepoint <= 0x7ffU) {
        if (buffer_size < 2U) {
            return -1;
        }
        buffer[0] = (char)(0xc0U | (codepoint >> 6));
        buffer[1] = (char)(0x80U | (codepoint & 0x3fU));
        *length_out = 2U;
        return 0;
    }

    if (codepoint <= 0xffffU) {
        if (buffer_size < 3U) {
            return -1;
        }
        buffer[0] = (char)(0xe0U | (codepoint >> 12));
        buffer[1] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
        buffer[2] = (char)(0x80U | (codepoint & 0x3fU));
        *length_out = 3U;
        return 0;
    }

    if (buffer_size < 4U) {
        return -1;
    }
    buffer[0] = (char)(0xf0U | (codepoint >> 18));
    buffer[1] = (char)(0x80U | ((codepoint >> 12) & 0x3fU));
    buffer[2] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
    buffer[3] = (char)(0x80U | (codepoint & 0x3fU));
    *length_out = 4U;
    return 0;
}

static unsigned int rt_load_u16(const unsigned char *source, int big_endian) {
    if (big_endian) return ((unsigned int)source[0] << 8U) | (unsigned int)source[1];
    return (unsigned int)source[0] | ((unsigned int)source[1] << 8U);
}

static void rt_store_u16(unsigned char *output, unsigned int value, int big_endian) {
    if (big_endian) {
        output[0] = (unsigned char)(value >> 8U);
        output[1] = (unsigned char)value;
    } else {
        output[0] = (unsigned char)value;
        output[1] = (unsigned char)(value >> 8U);
    }
}

int rt_utf16_decode(const unsigned char *source, size_t source_length, size_t *index_io, int big_endian, unsigned int *codepoint_out) {
    size_t index;
    unsigned int first;

    if (source == 0 || index_io == 0 || codepoint_out == 0) return -1;
    index = *index_io;
    if (index + 2U > source_length) return -1;
    first = rt_load_u16(source + index, big_endian);
    if (first >= 0xd800U && first <= 0xdbffU) {
        unsigned int second;
        if (index + 4U > source_length) return -1;
        second = rt_load_u16(source + index + 2U, big_endian);
        if (second < 0xdc00U || second > 0xdfffU) return -1;
        *codepoint_out = 0x10000U + ((first - 0xd800U) << 10U) + (second - 0xdc00U);
        *index_io = index + 4U;
        return 0;
    }
    if (first >= 0xdc00U && first <= 0xdfffU) return -1;
    *codepoint_out = first;
    *index_io = index + 2U;
    return 0;
}

int rt_utf16_encode(unsigned int codepoint, int big_endian, unsigned char *buffer, size_t buffer_size, size_t *length_out) {
    if (buffer == 0 || length_out == 0 || codepoint > 0x10ffffU ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return -1;
    if (codepoint < 0x10000U) {
        if (buffer_size < 2U) return -1;
        rt_store_u16(buffer, codepoint, big_endian);
        *length_out = 2U;
        return 0;
    }
    if (buffer_size < 4U) return -1;
    codepoint -= 0x10000U;
    rt_store_u16(buffer, 0xd800U + (codepoint >> 10U), big_endian);
    rt_store_u16(buffer + 2U, 0xdc00U + (codepoint & 0x3ffU), big_endian);
    *length_out = 4U;
    return 0;
}

static unsigned int rt_windows_1252_decode(unsigned char byte) {
    static const unsigned short mapping[32] = {
        0x20acU, 0x0081U, 0x201aU, 0x0192U, 0x201eU, 0x2026U, 0x2020U, 0x2021U,
        0x02c6U, 0x2030U, 0x0160U, 0x2039U, 0x0152U, 0x008dU, 0x017dU, 0x008fU,
        0x0090U, 0x2018U, 0x2019U, 0x201cU, 0x201dU, 0x2022U, 0x2013U, 0x2014U,
        0x02dcU, 0x2122U, 0x0161U, 0x203aU, 0x0153U, 0x009dU, 0x017eU, 0x0178U
    };
    return byte >= 0x80U && byte <= 0x9fU ? (unsigned int)mapping[byte - 0x80U] : (unsigned int)byte;
}

static int rt_windows_1252_encode(unsigned int codepoint, unsigned char *byte_out) {
    unsigned int byte;

    if (codepoint <= 0x7fU || (codepoint >= 0xa0U && codepoint <= 0xffU)) {
        *byte_out = (unsigned char)codepoint;
        return 0;
    }
    for (byte = 0x80U; byte <= 0x9fU; ++byte) {
        if (rt_windows_1252_decode((unsigned char)byte) == codepoint) {
            *byte_out = (unsigned char)byte;
            return 0;
        }
    }
    return -1;
}

static int rt_byte_buffer_grow(unsigned char **buffer_io, size_t *capacity_io, size_t needed) {
    size_t capacity = *capacity_io;
    unsigned char *buffer;

    if (needed <= capacity) return 0;
    if (capacity == 0U) capacity = 64U;
    while (capacity < needed) {
        if (capacity > ((size_t)-1) / 2U) { capacity = needed; break; }
        capacity *= 2U;
    }
    buffer = (unsigned char *)rt_realloc(*buffer_io, capacity);
    if (buffer == 0) return -1;
    *buffer_io = buffer;
    *capacity_io = capacity;
    return 0;
}

int rt_transcode_to_utf8(const unsigned char *source, size_t source_length, RtTextEncoding encoding, char **output_out, size_t *output_length_out) {
    unsigned char *output = 0;
    size_t output_length = 0U;
    size_t output_capacity = 0U;
    size_t index = 0U;
    int big_endian = encoding == RT_TEXT_ENCODING_UTF16BE;

    if (source == 0 || output_out == 0 || output_length_out == 0) return -1;
    if (encoding == RT_TEXT_ENCODING_UTF16) {
        if (source_length < 2U) return -1;
        if (source[0] == 0xfeU && source[1] == 0xffU) big_endian = 1;
        else if (source[0] == 0xffU && source[1] == 0xfeU) big_endian = 0;
        else return -1;
        index = 2U;
    } else if (encoding == RT_TEXT_ENCODING_UTF16LE || encoding == RT_TEXT_ENCODING_UTF16BE) {
        if (source_length >= 2U && ((source[0] == 0xffU && source[1] == 0xfeU) || (source[0] == 0xfeU && source[1] == 0xffU))) index = 2U;
    }
    while (index < source_length) {
        unsigned int codepoint;
        char encoded[4];
        size_t encoded_length;

        if (encoding == RT_TEXT_ENCODING_UTF8) {
            size_t before = index;
            if (rt_utf8_decode((const char *)source, source_length, &index, &codepoint) != 0) goto fail;
            encoded_length = index - before;
            if (rt_byte_buffer_grow(&output, &output_capacity, output_length + encoded_length + 1U) != 0) goto fail;
            memcpy(output + output_length, source + before, encoded_length);
            output_length += encoded_length;
            continue;
        } else if (encoding == RT_TEXT_ENCODING_UTF16 || encoding == RT_TEXT_ENCODING_UTF16LE || encoding == RT_TEXT_ENCODING_UTF16BE) {
            if (rt_utf16_decode(source, source_length, &index, big_endian, &codepoint) != 0) goto fail;
        } else {
            unsigned char byte = source[index++];
            codepoint = encoding == RT_TEXT_ENCODING_WINDOWS_1252 ? rt_windows_1252_decode(byte) : (unsigned int)byte;
        }
        if (rt_utf8_encode(codepoint, encoded, sizeof(encoded), &encoded_length) != 0 ||
            rt_byte_buffer_grow(&output, &output_capacity, output_length + encoded_length + 1U) != 0) goto fail;
        memcpy(output + output_length, encoded, encoded_length);
        output_length += encoded_length;
    }
    if (rt_byte_buffer_grow(&output, &output_capacity, output_length + 1U) != 0) goto fail;
    output[output_length] = 0U;
    *output_out = (char *)output;
    *output_length_out = output_length;
    return 0;
fail:
    rt_free(output);
    return -1;
}

int rt_transcode_from_utf8(const char *source, size_t source_length, RtTextEncoding encoding, unsigned char **output_out, size_t *output_length_out) {
    unsigned char *output = 0;
    size_t output_length = 0U;
    size_t output_capacity = 0U;
    size_t index = 0U;
    int big_endian = encoding == RT_TEXT_ENCODING_UTF16BE;

    if (source == 0 || output_out == 0 || output_length_out == 0) return -1;
    if (encoding == RT_TEXT_ENCODING_UTF16) {
        if (rt_byte_buffer_grow(&output, &output_capacity, 2U) != 0) goto fail;
        output[0] = 0xffU; output[1] = 0xfeU; output_length = 2U;
    }
    while (index < source_length) {
        unsigned int codepoint;
        size_t before = index;
        unsigned char encoded[4];
        size_t encoded_length = 0U;

        if (rt_utf8_decode(source, source_length, &index, &codepoint) != 0) goto fail;
        if (encoding == RT_TEXT_ENCODING_UTF8) {
            encoded_length = index - before;
            memcpy(encoded, source + before, encoded_length);
        } else if (encoding == RT_TEXT_ENCODING_UTF16 || encoding == RT_TEXT_ENCODING_UTF16LE || encoding == RT_TEXT_ENCODING_UTF16BE) {
            if (rt_utf16_encode(codepoint, big_endian, encoded, sizeof(encoded), &encoded_length) != 0) goto fail;
        } else if (encoding == RT_TEXT_ENCODING_ISO_8859_1) {
            if (codepoint > 0xffU) goto fail;
            encoded[0] = (unsigned char)codepoint; encoded_length = 1U;
        } else {
            if (rt_windows_1252_encode(codepoint, encoded) != 0) goto fail;
            encoded_length = 1U;
        }
        if (rt_byte_buffer_grow(&output, &output_capacity, output_length + encoded_length + 1U) != 0) goto fail;
        memcpy(output + output_length, encoded, encoded_length);
        output_length += encoded_length;
    }
    if (rt_byte_buffer_grow(&output, &output_capacity, output_length + 1U) != 0) goto fail;
    output[output_length] = 0U;
    *output_out = output;
    *output_length_out = output_length;
    return 0;
fail:
    rt_free(output);
    return -1;
}

unsigned int rt_unicode_simple_fold(unsigned int codepoint) {
    if (codepoint >= 'A' && codepoint <= 'Z') {
        return codepoint + 32U;
    }
    if ((codepoint >= 0x00c0U && codepoint <= 0x00d6U) ||
        (codepoint >= 0x00d8U && codepoint <= 0x00deU)) {
        return codepoint + 32U;
    }
    if (codepoint >= 0x0100U && codepoint <= 0x017fU) {
        if ((codepoint & 1U) == 0U) {
            return codepoint + 1U;
        }
        return codepoint;
    }
    if (codepoint >= 0x0391U && codepoint <= 0x03abU && codepoint != 0x03a2U) {
        return codepoint + 32U;
    }
    switch (codepoint) {
        case 0x0178U: return 0x00ffU;
        case 0x0181U: return 0x0253U;
        case 0x0186U: return 0x0254U;
        case 0x0189U: return 0x0256U;
        case 0x018aU: return 0x0257U;
        case 0x018eU: return 0x01ddU;
        case 0x018fU: return 0x0259U;
        case 0x0190U: return 0x025bU;
        case 0x0193U: return 0x0260U;
        case 0x0194U: return 0x0263U;
        case 0x0196U: return 0x0269U;
        case 0x0197U: return 0x0268U;
        case 0x019cU: return 0x026fU;
        case 0x019dU: return 0x0272U;
        case 0x019fU: return 0x0275U;
        case 0x01a6U: return 0x0280U;
        case 0x01a7U: return 0x01a8U;
        case 0x01acU: return 0x01adU;
        case 0x01aeU: return 0x0288U;
        case 0x01afU: return 0x01b0U;
        case 0x01b1U: return 0x028aU;
        case 0x01b2U: return 0x028bU;
        case 0x01b7U: return 0x0292U;
        case 0x03cfU: return 0x03d7U;
        default: return codepoint;
    }
}

int rt_unicode_is_space(unsigned int codepoint) {
    switch (codepoint) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case '\v':
        case '\f':
        case 0x85U:
        case 0xa0U:
        case 0x1680U:
        case 0x2028U:
        case 0x2029U:
        case 0x202fU:
        case 0x205fU:
        case 0x3000U:
            return 1;
        default:
            break;
    }

    return codepoint >= 0x2000U && codepoint <= 0x200aU;
}

RtWordBreakClass rt_unicode_word_break_class(unsigned int codepoint) {
    if (rt_unicode_is_space(codepoint)) return RT_WORD_BREAK_SPACE;
    if ((codepoint >= 'a' && codepoint <= 'z') || (codepoint >= 'A' && codepoint <= 'Z')) return RT_WORD_BREAK_LETTER;
    if (codepoint >= '0' && codepoint <= '9') return RT_WORD_BREAK_NUMERIC;
    if (codepoint == '_' || codepoint == 0x203fU || codepoint == 0x2040U) return RT_WORD_BREAK_EXTEND_NUMERIC_LETTER;
    if (codepoint == '\'' || codepoint == 0x00b7U || codepoint == 0x05f4U || codepoint == 0x2019U) return RT_WORD_BREAK_MID_LETTER;
    if (codepoint == '.' || codepoint == ',' || codepoint == ':' || codepoint == 0x066bU || codepoint == 0x066cU) return RT_WORD_BREAK_MID_NUMERIC;
    if ((codepoint >= 0x0300U && codepoint <= 0x036fU) ||
        (codepoint >= 0x0483U && codepoint <= 0x0489U) ||
        (codepoint >= 0x0591U && codepoint <= 0x05bdU) ||
        (codepoint >= 0x0610U && codepoint <= 0x061aU) ||
        (codepoint >= 0x064bU && codepoint <= 0x065fU) ||
        (codepoint >= 0x0900U && codepoint <= 0x0903U) ||
        (codepoint >= 0x093aU && codepoint <= 0x094fU) ||
        (codepoint >= 0x1ab0U && codepoint <= 0x1aceU) ||
        (codepoint >= 0x1dc0U && codepoint <= 0x1dffU) ||
        codepoint == 0x200cU || codepoint == 0x200dU ||
        (codepoint >= 0xfe00U && codepoint <= 0xfe0fU) ||
        (codepoint >= 0xfe20U && codepoint <= 0xfe2fU) ||
        (codepoint >= 0x1f3fbU && codepoint <= 0x1f3ffU) ||
        (codepoint >= 0xe0100U && codepoint <= 0xe01efU)) return RT_WORD_BREAK_EXTEND;
    if ((codepoint >= 0x30a0U && codepoint <= 0x30ffU) || (codepoint >= 0x31f0U && codepoint <= 0x31ffU) ||
        (codepoint >= 0xff66U && codepoint <= 0xff9fU)) return RT_WORD_BREAK_KATAKANA;
    if (codepoint < 0x80U ||
        (codepoint >= 0x0609U && codepoint <= 0x060dU) ||
        codepoint == 0x061bU || (codepoint >= 0x061dU && codepoint <= 0x061fU) ||
        (codepoint >= 0x066aU && codepoint <= 0x066dU) ||
        (codepoint >= 0x2000U && codepoint <= 0x206fU) ||
        (codepoint >= 0x20a0U && codepoint <= 0x20cfU) ||
        (codepoint >= 0x2190U && codepoint <= 0x2bffU) ||
        (codepoint >= 0x2e00U && codepoint <= 0x2e7fU) ||
        (codepoint >= 0x3000U && codepoint <= 0x303fU) ||
        (codepoint >= 0xfe10U && codepoint <= 0xfe1fU) ||
        (codepoint >= 0xfe30U && codepoint <= 0xfe6fU) ||
        (codepoint >= 0xff01U && codepoint <= 0xff20U) ||
        (codepoint >= 0xff3bU && codepoint <= 0xff40U) ||
        (codepoint >= 0xff5bU && codepoint <= 0xff65U) ||
        (codepoint >= 0x1f000U && codepoint <= 0x1faffU)) return RT_WORD_BREAK_OTHER;
    return codepoint <= 0x10ffffU ? RT_WORD_BREAK_LETTER : RT_WORD_BREAK_OTHER;
}

static int rt_word_class_is_core(RtWordBreakClass word_class) {
    return word_class == RT_WORD_BREAK_LETTER || word_class == RT_WORD_BREAK_NUMERIC ||
           word_class == RT_WORD_BREAK_KATAKANA || word_class == RT_WORD_BREAK_EXTEND_NUMERIC_LETTER;
}

int rt_unicode_is_word(unsigned int codepoint) {
    return rt_word_class_is_core(rt_unicode_word_break_class(codepoint));
}

static int rt_word_decode_next(const char *text, size_t length, size_t index, size_t *next_out, RtWordBreakClass *class_out) {
    unsigned int codepoint;
    size_t next = index;
    if (index >= length) return -1;
    if (rt_utf8_decode(text, length, &next, &codepoint) != 0) {
        next = index + 1U;
        codepoint = (unsigned char)text[index];
    }
    *next_out = next;
    *class_out = rt_unicode_word_break_class(codepoint);
    return 0;
}

static int rt_word_decode_previous(const char *text, size_t end, size_t *start_out, RtWordBreakClass *class_out) {
    size_t start;
    size_t next;
    if (end == 0U) return -1;
    start = end - 1U;
    while (start > 0U && (((unsigned char)text[start] & 0xc0U) == 0x80U)) start -= 1U;
    if (rt_word_decode_next(text, end, start, &next, class_out) != 0 || next != end) {
        start = end - 1U;
        *class_out = rt_unicode_word_break_class((unsigned char)text[start]);
    }
    *start_out = start;
    return 0;
}

int rt_unicode_is_word_boundary(const char *text, size_t text_length, size_t index) {
    size_t left_start;
    size_t right_end;
    RtWordBreakClass left;
    RtWordBreakClass right;

    if (text == 0 || index > text_length) return 1;
    if (index == 0U || index == text_length) return 1;
    if (rt_word_decode_previous(text, index, &left_start, &left) != 0 ||
        rt_word_decode_next(text, text_length, index, &right_end, &right) != 0) return 1;

    if (right == RT_WORD_BREAK_EXTEND) return 0;
    while (left == RT_WORD_BREAK_EXTEND && left_start > 0U) {
        if (rt_word_decode_previous(text, left_start, &left_start, &left) != 0) break;
    }
    if (rt_word_class_is_core(left) && rt_word_class_is_core(right)) return 0;
    if (rt_word_class_is_core(left) && (right == RT_WORD_BREAK_MID_LETTER || right == RT_WORD_BREAK_MID_NUMERIC)) {
        RtWordBreakClass after;
        size_t after_end;
        if (rt_word_decode_next(text, text_length, right_end, &after_end, &after) == 0 && rt_word_class_is_core(after)) return 0;
    }
    if ((left == RT_WORD_BREAK_MID_LETTER || left == RT_WORD_BREAK_MID_NUMERIC) && rt_word_class_is_core(right)) {
        RtWordBreakClass before;
        size_t before_start;
        if (rt_word_decode_previous(text, left_start, &before_start, &before) == 0 && rt_word_class_is_core(before)) return 0;
    }
    return 1;
}

int rt_word_next(const char *text, size_t text_length, size_t start, RtWordSpan *span_out) {
    size_t end;
    RtWordBreakClass first_class;

    if (text == 0 || span_out == 0 || start >= text_length) return -1;
    if (rt_word_decode_next(text, text_length, start, &end, &first_class) != 0) return -1;
    while (end < text_length && !rt_unicode_is_word_boundary(text, text_length, end)) {
        RtWordBreakClass ignored;
        size_t next;
        if (rt_word_decode_next(text, text_length, end, &next, &ignored) != 0 || next <= end) break;
        end = next;
    }
    span_out->start = start;
    span_out->end = end;
    span_out->is_word = rt_word_class_is_core(first_class);
    return 0;
}

typedef struct {
    unsigned int *items;
    size_t count;
    size_t capacity;
} RtNormalizedText;

static unsigned int rt_unicode_combining_class(unsigned int codepoint) {
    if ((codepoint >= 0x0300U && codepoint <= 0x0314U) ||
        (codepoint >= 0x033dU && codepoint <= 0x0344U) || codepoint == 0x0346U) return 230U;
    if ((codepoint >= 0x0316U && codepoint <= 0x0319U) ||
        (codepoint >= 0x031cU && codepoint <= 0x0320U) ||
        (codepoint >= 0x0323U && codepoint <= 0x0326U) ||
        (codepoint >= 0x0329U && codepoint <= 0x0333U) || codepoint == 0x0339U || codepoint == 0x033aU) return 220U;
    switch (codepoint) {
        case 0x0315U: case 0x031aU: case 0x0358U: return 232U;
        case 0x031bU: return 216U;
        case 0x0321U: case 0x0322U: case 0x0327U: case 0x0328U: return 202U;
        case 0x0334U: case 0x0335U: case 0x0336U: case 0x0337U: case 0x0338U: return 1U;
        case 0x0345U: return 240U;
        default: return 0U;
    }
}

static int rt_normalized_append(RtNormalizedText *text, unsigned int codepoint) {
    size_t index;
    unsigned int combining_class = rt_unicode_combining_class(codepoint);

    if (text->count == text->capacity) {
        size_t new_capacity = text->capacity == 0U ? 32U : text->capacity * 2U;
        unsigned int *new_items = (unsigned int *)rt_realloc_array(text->items, new_capacity, sizeof(unsigned int));
        if (new_items == 0) return -1;
        text->items = new_items;
        text->capacity = new_capacity;
    }
    index = text->count;
    if (combining_class != 0U) {
        while (index > 0U) {
            unsigned int previous_class = rt_unicode_combining_class(text->items[index - 1U]);
            if (previous_class == 0U || previous_class <= combining_class) break;
            text->items[index] = text->items[index - 1U];
            index -= 1U;
        }
    }
    text->items[index] = codepoint;
    text->count += 1U;
    return 0;
}

static int rt_unicode_decompose_pair(unsigned int codepoint, unsigned int *first_out, unsigned int *second_out) {
    switch (codepoint) {
        case 0x00c0U: *first_out = 'A'; *second_out = 0x0300U; return 1;
        case 0x00c1U: *first_out = 'A'; *second_out = 0x0301U; return 1;
        case 0x00c2U: *first_out = 'A'; *second_out = 0x0302U; return 1;
        case 0x00c3U: *first_out = 'A'; *second_out = 0x0303U; return 1;
        case 0x00c4U: *first_out = 'A'; *second_out = 0x0308U; return 1;
        case 0x00c5U: *first_out = 'A'; *second_out = 0x030aU; return 1;
        case 0x00c7U: *first_out = 'C'; *second_out = 0x0327U; return 1;
        case 0x00c8U: *first_out = 'E'; *second_out = 0x0300U; return 1;
        case 0x00c9U: *first_out = 'E'; *second_out = 0x0301U; return 1;
        case 0x00caU: *first_out = 'E'; *second_out = 0x0302U; return 1;
        case 0x00cbU: *first_out = 'E'; *second_out = 0x0308U; return 1;
        case 0x00ccU: *first_out = 'I'; *second_out = 0x0300U; return 1;
        case 0x00cdU: *first_out = 'I'; *second_out = 0x0301U; return 1;
        case 0x00ceU: *first_out = 'I'; *second_out = 0x0302U; return 1;
        case 0x00cfU: *first_out = 'I'; *second_out = 0x0308U; return 1;
        case 0x00d1U: *first_out = 'N'; *second_out = 0x0303U; return 1;
        case 0x00d2U: *first_out = 'O'; *second_out = 0x0300U; return 1;
        case 0x00d3U: *first_out = 'O'; *second_out = 0x0301U; return 1;
        case 0x00d4U: *first_out = 'O'; *second_out = 0x0302U; return 1;
        case 0x00d5U: *first_out = 'O'; *second_out = 0x0303U; return 1;
        case 0x00d6U: *first_out = 'O'; *second_out = 0x0308U; return 1;
        case 0x00d9U: *first_out = 'U'; *second_out = 0x0300U; return 1;
        case 0x00daU: *first_out = 'U'; *second_out = 0x0301U; return 1;
        case 0x00dbU: *first_out = 'U'; *second_out = 0x0302U; return 1;
        case 0x00dcU: *first_out = 'U'; *second_out = 0x0308U; return 1;
        case 0x00ddU: *first_out = 'Y'; *second_out = 0x0301U; return 1;
        case 0x00e0U: *first_out = 'a'; *second_out = 0x0300U; return 1;
        case 0x00e1U: *first_out = 'a'; *second_out = 0x0301U; return 1;
        case 0x00e2U: *first_out = 'a'; *second_out = 0x0302U; return 1;
        case 0x00e3U: *first_out = 'a'; *second_out = 0x0303U; return 1;
        case 0x00e4U: *first_out = 'a'; *second_out = 0x0308U; return 1;
        case 0x00e5U: *first_out = 'a'; *second_out = 0x030aU; return 1;
        case 0x00e7U: *first_out = 'c'; *second_out = 0x0327U; return 1;
        case 0x00e8U: *first_out = 'e'; *second_out = 0x0300U; return 1;
        case 0x00e9U: *first_out = 'e'; *second_out = 0x0301U; return 1;
        case 0x00eaU: *first_out = 'e'; *second_out = 0x0302U; return 1;
        case 0x00ebU: *first_out = 'e'; *second_out = 0x0308U; return 1;
        case 0x00ecU: *first_out = 'i'; *second_out = 0x0300U; return 1;
        case 0x00edU: *first_out = 'i'; *second_out = 0x0301U; return 1;
        case 0x00eeU: *first_out = 'i'; *second_out = 0x0302U; return 1;
        case 0x00efU: *first_out = 'i'; *second_out = 0x0308U; return 1;
        case 0x00f1U: *first_out = 'n'; *second_out = 0x0303U; return 1;
        case 0x00f2U: *first_out = 'o'; *second_out = 0x0300U; return 1;
        case 0x00f3U: *first_out = 'o'; *second_out = 0x0301U; return 1;
        case 0x00f4U: *first_out = 'o'; *second_out = 0x0302U; return 1;
        case 0x00f5U: *first_out = 'o'; *second_out = 0x0303U; return 1;
        case 0x00f6U: *first_out = 'o'; *second_out = 0x0308U; return 1;
        case 0x00f9U: *first_out = 'u'; *second_out = 0x0300U; return 1;
        case 0x00faU: *first_out = 'u'; *second_out = 0x0301U; return 1;
        case 0x00fbU: *first_out = 'u'; *second_out = 0x0302U; return 1;
        case 0x00fcU: *first_out = 'u'; *second_out = 0x0308U; return 1;
        case 0x00fdU: *first_out = 'y'; *second_out = 0x0301U; return 1;
        case 0x00ffU: *first_out = 'y'; *second_out = 0x0308U; return 1;
        case 0x2126U: *first_out = 0x03a9U; *second_out = 0U; return 1;
        case 0x212aU: *first_out = 'K'; *second_out = 0U; return 1;
        case 0x212bU: *first_out = 0x00c5U; *second_out = 0U; return 1;
        default: return 0;
    }
}

static int rt_normalized_decompose(RtNormalizedText *text, unsigned int codepoint, int ignore_case) {
    unsigned int first;
    unsigned int second;

    if (codepoint >= 0xac00U && codepoint <= 0xd7a3U) {
        unsigned int syllable = codepoint - 0xac00U;
        unsigned int leading = 0x1100U + syllable / 588U;
        unsigned int vowel = 0x1161U + (syllable % 588U) / 28U;
        unsigned int trailing = syllable % 28U;
        if (rt_normalized_append(text, leading) != 0 || rt_normalized_append(text, vowel) != 0) return -1;
        return trailing == 0U ? 0 : rt_normalized_append(text, 0x11a7U + trailing);
    }
    if (rt_unicode_decompose_pair(codepoint, &first, &second)) {
        if (rt_normalized_decompose(text, first, ignore_case) != 0) return -1;
        return second == 0U ? 0 : rt_normalized_decompose(text, second, ignore_case);
    }
    if (ignore_case) codepoint = rt_unicode_simple_fold(codepoint);
    return rt_normalized_append(text, codepoint);
}

static int rt_normalized_decode(const char *source, size_t length, int ignore_case, RtNormalizedText *text) {
    size_t index = 0U;

    rt_memset(text, 0, sizeof(*text));
    while (index < length) {
        unsigned int codepoint;
        size_t before = index;
        if (rt_utf8_decode(source, length, &index, &codepoint) != 0) {
            if (index <= before) index = before + 1U;
            codepoint = 0xfffdU;
        }
        if (rt_normalized_decompose(text, codepoint, ignore_case) != 0) {
            rt_free(text->items);
            rt_memset(text, 0, sizeof(*text));
            return -1;
        }
    }
    return 0;
}

int rt_unicode_normalized_compare(const char *left, size_t left_length, const char *right, size_t right_length, int ignore_case, int *result_out) {
    RtNormalizedText normalized_left;
    RtNormalizedText normalized_right;
    size_t index;
    int result = 0;

    if (left == 0 || right == 0 || result_out == 0) return -1;
    if (rt_normalized_decode(left, left_length, ignore_case, &normalized_left) != 0) return -1;
    if (rt_normalized_decode(right, right_length, ignore_case, &normalized_right) != 0) {
        rt_free(normalized_left.items);
        return -1;
    }
    for (index = 0U; index < normalized_left.count && index < normalized_right.count; ++index) {
        if (normalized_left.items[index] < normalized_right.items[index]) { result = -1; break; }
        if (normalized_left.items[index] > normalized_right.items[index]) { result = 1; break; }
    }
    if (result == 0 && normalized_left.count != normalized_right.count) {
        result = normalized_left.count < normalized_right.count ? -1 : 1;
    }
    rt_free(normalized_left.items);
    rt_free(normalized_right.items);
    *result_out = result;
    return 0;
}

int rt_unicode_normalized_equal(const char *left, size_t left_length, const char *right, size_t right_length, int ignore_case) {
    int result;
    return rt_unicode_normalized_compare(left, left_length, right, right_length, ignore_case, &result) == 0 && result == 0;
}

int rt_unicode_normalized_contains(const char *text, size_t text_length, const char *needle, size_t needle_length, int ignore_case) {
    RtNormalizedText normalized_text;
    RtNormalizedText normalized_needle;
    size_t start;
    int found = 0;

    if (text == 0 || needle == 0) return 0;
    if (rt_normalized_decode(text, text_length, ignore_case, &normalized_text) != 0) return 0;
    if (rt_normalized_decode(needle, needle_length, ignore_case, &normalized_needle) != 0) {
        rt_free(normalized_text.items);
        return 0;
    }
    if (normalized_needle.count == 0U) {
        found = 1;
    } else if (normalized_needle.count <= normalized_text.count) {
        for (start = 0U; start + normalized_needle.count <= normalized_text.count; ++start) {
            if (memcmp(normalized_text.items + start, normalized_needle.items,
                       normalized_needle.count * sizeof(unsigned int)) == 0) {
                found = 1;
                break;
            }
        }
    }
    rt_free(normalized_text.items);
    rt_free(normalized_needle.items);
    return found;
}
