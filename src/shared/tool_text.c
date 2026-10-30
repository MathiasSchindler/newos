#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

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

int tool_contains_char(const char *text, char ch) {
    size_t i = 0U;

    while (text[i] != '\0') {
        if (text[i] == ch) {
            return 1;
        }
        i += 1U;
    }
    return 0;
}

int tool_compare_text_slices(const char *left, size_t left_length, const char *right, size_t right_length) {
    size_t i;
    size_t shared = left_length < right_length ? left_length : right_length;

    for (i = 0U; i < shared; ++i) {
        if (left[i] < right[i]) return -1;
        if (left[i] > right[i]) return 1;
    }
    if (left_length < right_length) return -1;
    if (left_length > right_length) return 1;
    return 0;
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

int tool_next_token(const char **cursor_io, char *token, size_t token_size) {
    const char *cursor;
    size_t length = 0U;

    if (cursor_io == 0 || *cursor_io == 0 || token == 0 || token_size == 0U) {
        return 0;
    }
    cursor = *cursor_io;
    while (*cursor != '\0' && tool_ascii_is_token_space(*cursor)) {
        cursor++;
    }
    if (*cursor == '\0' || *cursor == '#') {
        *cursor_io = cursor;
        token[0] = '\0';
        return 0;
    }
    while (*cursor != '\0' && !tool_ascii_is_token_space(*cursor)) {
        if (length + 1U < token_size) {
            token[length++] = *cursor;
        }
        cursor++;
    }
    token[length] = '\0';
    *cursor_io = cursor;
    return length == 0U ? 0 : 1;
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

int tool_name_equals_ignore_case_ascii_n(const char *text, size_t text_length, const char *name) {
    size_t index = 0U;

    while (index < text_length && name[index] != '\0') {
        if (tool_ascii_tolower(text[index]) != tool_ascii_tolower(name[index])) return 0;
        index += 1U;
    }
    return index == text_length && name[index] == '\0';
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
    return rt_unicode_is_word_boundary(text, length, start) && rt_unicode_is_word_boundary(text, length, end);
}

int tool_text_find_next_match(const char *pattern,
                              const char *text,
                              int ignore_case,
                              int fixed_string,
                              int whole_word,
                              size_t search_start,
                              size_t *start_out,
                              size_t *end_out) {
    return tool_text_find_next_match_ex(pattern, text, ignore_case, fixed_string, whole_word, 1, search_start, start_out, end_out);
}

int tool_text_find_next_match_ex(const char *pattern,
                                 const char *text,
                                 int ignore_case,
                                 int fixed_string,
                                 int whole_word,
                                 int extended,
                                 size_t search_start,
                                 size_t *start_out,
                                 size_t *end_out) {
    size_t candidate_start = 0U;
    size_t candidate_end = 0U;
    size_t next_start = search_start;

    while (1) {
        int found;

        if (fixed_string) {
            size_t pos = next_start;
            size_t pattern_len = rt_strlen(pattern);

            found = 0;
            if (pattern_len == 0U) {
                candidate_start = next_start;
                candidate_end = next_start;
                found = 1;
            } else {
                while (1) {
                    size_t consumed = 0U;

                    if (tool_literal_prefix_matches(pattern, text + pos, ignore_case, &consumed)) {
                        candidate_start = pos;
                        candidate_end = pos + consumed;
                        found = 1;
                        break;
                    }
                    if (text[pos] == '\0') break;
                    pos += 1U;
                    while (tool_utf8_is_continuation_byte((unsigned char)text[pos])) pos += 1U;
                }
            }
        } else {
            found = tool_regex_search_ex(pattern, text, ignore_case, extended, next_start, &candidate_start, &candidate_end);
        }

        if (!found) return 0;
        if (!whole_word || tool_text_match_has_word_boundaries(text, candidate_start, candidate_end)) {
            *start_out = candidate_start;
            *end_out = candidate_end;
            return 1;
        }
        if (text[candidate_start] == '\0') break;
        next_start = candidate_start + 1U;
    }

    return 0;
}

unsigned int tool_unicode_ambiguous_width(void) {
    const char *value = platform_getenv("NEWOS_AMBIGUOUS_WIDTH");

    return value != 0 && rt_strcmp(value, "2") == 0 ? 2U : 1U;
}

size_t tool_text_display_width_n(const char *text, size_t length) {
    return (size_t)rt_text_display_width_n_mode(text, length, 0ULL, 8U, tool_unicode_ambiguous_width());
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
    return rt_unicode_normalized_contains(text, text_len, needle, needle_len, 1);
}

int tool_literal_prefix_matches(const char *pattern, const char *text, int ignore_case, size_t *consumed_out) {
    size_t pattern_len = rt_strlen(pattern);
    size_t text_len = rt_strlen(text);
    size_t pi = 0U;
    size_t ti = 0U;

    while (pi < pattern_len) {
        unsigned int lhs = 0U;
        unsigned int rhs = 0U;
        unsigned char pattern_ch;
        unsigned char text_ch;

        if (ti >= text_len) return 0;
        pattern_ch = (unsigned char)pattern[pi];
        text_ch = (unsigned char)text[ti];
        if (pattern_ch < 0x80U && text_ch < 0x80U) {
            if (ignore_case) {
                pattern_ch = (unsigned char)tool_ascii_tolower((char)pattern_ch);
                text_ch = (unsigned char)tool_ascii_tolower((char)text_ch);
            }
            if (pattern_ch != text_ch) return 0;
            pi += 1U;
            ti += 1U;
            continue;
        }
        if (rt_utf8_decode(pattern, pattern_len, &pi, &lhs) != 0 || rt_utf8_decode(text, text_len, &ti, &rhs) != 0) return 0;
        if (ignore_case) {
            lhs = rt_unicode_simple_fold(lhs);
            rhs = rt_unicode_simple_fold(rhs);
        }
        if (lhs != rhs) return 0;
    }
    *consumed_out = ti;
    return 1;
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

int tool_starts_with(const char *text, const char *prefix) {
    size_t index = 0U;

    if (text == 0 || prefix == 0) {
        return 0;
    }

    while (prefix[index] != '\0') {
        if (text[index] != prefix[index]) {
            return 0;
        }
        index += 1U;
    }

    return 1;
}

int tool_token_equals(const char *text, size_t text_length, const char *token) {
    size_t index = 0U;

    if (text == 0 || token == 0) {
        return 0;
    }

    while (index < text_length && token[index] != '\0') {
        if (text[index] != token[index]) {
            return 0;
        }
        index += 1U;
    }

    return index == text_length && token[index] == '\0';
}

int tool_parse_tabstop_list(const char *text, unsigned long long *stops, size_t max_stops, size_t *count_out) {
    unsigned long long previous = 0ULL;
    size_t count = 0U;
    size_t start = 0U;
    size_t index = 0U;

    if (text == 0 || stops == 0 || count_out == 0 || max_stops == 0U) {
        return -1;
    }

    while (1) {
        char ch = text[index];
        if (ch == ',' || ch == '\0') {
            char token[32];
            size_t length;
            unsigned long long value = 0ULL;

            if (count >= max_stops || index == start) {
                return -1;
            }
            length = index - start;
            if (length + 1U > sizeof(token)) {
                return -1;
            }
            memcpy(token, text + start, length);
            token[length] = '\0';
            if (rt_parse_uint(token, &value) != 0 || value == 0ULL) {
                return -1;
            }
            if (count > 0U && value <= previous) {
                return -1;
            }

            stops[count++] = value;
            previous = value;

            if (ch == '\0') {
                break;
            }
            start = index + 1U;
        }
        index += 1U;
    }

    *count_out = count;
    return count > 0U ? 0 : -1;
}

unsigned long long tool_next_tabstop(const unsigned long long *stops, size_t stop_count, unsigned long long column) {
    size_t index;

    if (stop_count == 0U) {
        return column + 8ULL - (column % 8ULL);
    }

    if (stop_count == 1U) {
        unsigned long long stop = stops[0];
        if (stop == 0ULL) {
            return column + 1ULL;
        }
        return column + stop - (column % stop);
    }

    for (index = 0U; index < stop_count; ++index) {
        if (stops[index] > column) {
            return stops[index];
        }
    }

    return column + 1ULL;
}

void tool_trim_whitespace(char *text) {
    size_t start = 0U;
    size_t end;
    size_t out = 0U;

    if (text == 0) {
        return;
    }

    end = rt_strlen(text);
    while (text[start] != '\0' && rt_is_space(text[start])) {
        start += 1U;
    }
    while (end > start && rt_is_space(text[end - 1U])) {
        end -= 1U;
    }

    while (start + out < end) {
        text[out] = text[start + out];
        out += 1U;
    }
    text[out] = '\0';
}
