#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int tool_open_input(const char *path, int *fd_out, int *should_close_out) {
    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        *fd_out = 0;
        *should_close_out = 0;
        return 0;
    }

    *fd_out = platform_open_read(path);
    if (*fd_out < 0) {
        return -1;
    }

    *should_close_out = 1;
    return 0;
}

void tool_close_input(int fd, int should_close) {
    if (should_close) {
        (void)platform_close(fd);
    }
}

void tool_write_usage(const char *program_name, const char *usage_suffix) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    if (usage_suffix != 0 && usage_suffix[0] != '\0') {
        rt_write_char(2, ' ');
        rt_write_cstr(2, usage_suffix);
    }
    rt_write_char(2, '\n');
}

void tool_write_error(const char *tool_name, const char *message, const char *detail) {
    rt_write_cstr(2, tool_name);
    rt_write_cstr(2, ": ");
    if (message != 0) {
        rt_write_cstr(2, message);
    }
    if (detail != 0) {
        rt_write_cstr(2, detail);
    }
    rt_write_char(2, '\n');
}

int tool_parse_escaped_string(const char *text, char *buffer, size_t buffer_size, size_t *length_out) {
    size_t in_index = 0;
    size_t out_index = 0;

    if (text == 0 || buffer == 0 || buffer_size == 0) {
        return -1;
    }

    while (text[in_index] != '\0') {
        char ch = text[in_index];

        if (ch == '\\' && text[in_index + 1U] != '\0') {
            in_index += 1U;
            ch = text[in_index];
            if (ch == 'n') {
                ch = '\n';
            } else if (ch == 'r') {
                ch = '\r';
            } else if (ch == 't') {
                ch = '\t';
            } else if (ch == 'f') {
                ch = '\f';
            } else if (ch == 'v') {
                ch = '\v';
            } else if (ch == '0') {
                ch = '\0';
            }
        }

        if (out_index + 1U >= buffer_size) {
            buffer[buffer_size - 1U] = '\0';
            return -1;
        }

        buffer[out_index++] = ch;
        in_index += 1U;
    }

    buffer[out_index] = '\0';
    if (length_out != 0) {
        *length_out = out_index;
    }
    return 0;
}

int tool_prompt_yes_no(const char *message, const char *path) {
    char reply[8];
    long bytes_read;

    if (message != 0) {
        rt_write_cstr(2, message);
    }
    if (path != 0) {
        rt_write_cstr(2, path);
    }
    rt_write_cstr(2, "? ");

    bytes_read = platform_read(0, reply, sizeof(reply));
    return bytes_read > 0 && (reply[0] == 'y' || reply[0] == 'Y');
}

static size_t tool_buffer_append_char(char *buffer, size_t buffer_size, size_t length, char ch) {
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

static size_t tool_buffer_append_cstr(char *buffer, size_t buffer_size, size_t length, const char *text) {
    size_t i = 0;

    while (text != 0 && text[i] != '\0') {
        length = tool_buffer_append_char(buffer, buffer_size, length, text[i]);
        i += 1U;
    }

    return length;
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

int tool_parse_uint_arg(const char *text, unsigned long long *value_out, const char *tool_name, const char *what) {
    if (text == 0 || rt_parse_uint(text, value_out) != 0) {
        tool_write_error(tool_name, "invalid ", what);
        return -1;
    }
    return 0;
}

int tool_parse_int_arg(const char *text, long long *value_out, const char *tool_name, const char *what) {
    unsigned long long magnitude = 0;
    int negative = 0;

    if (text == 0 || value_out == 0 || text[0] == '\0') {
        tool_write_error(tool_name, "invalid ", what);
        return -1;
    }

    if (text[0] == '-') {
        negative = 1;
        text += 1;
    } else if (text[0] == '+') {
        text += 1;
    }

    if (text[0] == '\0' || rt_parse_uint(text, &magnitude) != 0) {
        tool_write_error(tool_name, "invalid ", what);
        return -1;
    }

    *value_out = negative ? -(long long)magnitude : (long long)magnitude;
    return 0;
}

int tool_parse_signal_name(const char *text, int *signal_out) {
    return platform_parse_signal_name(text, signal_out);
}

const char *tool_signal_name(int signal_number) {
    return platform_signal_name(signal_number);
}

void tool_write_signal_list(int fd) {
    platform_write_signal_list(fd);
}

const char *tool_base_name(const char *path) {
    const char *last = path;
    size_t i = 0;

    if (path == 0) {
        return "";
    }

    while (path[i] != '\0') {
        if (path[i] == '/') {
            last = path + i + 1;
        }
        i += 1;
    }

    return last;
}

int tool_join_path(const char *dir_path, const char *name, char *buffer, size_t buffer_size) {
    return rt_join_path(dir_path, name, buffer, buffer_size);
}

typedef struct {
    const char *starts[10];
    const char *ends[10];
} ToolRegexCaptures;

#define TOOL_REGEX_REPEAT_UNBOUNDED (~0ULL)

static int tool_regex_is_utf8_continuation(unsigned char ch) {
    return (ch & 0xc0U) == 0x80U;
}

static size_t tool_regex_decode_codepoint(const char *text, unsigned int *codepoint_out) {
    size_t index = 0U;
    size_t length;
    unsigned int local_codepoint = 0U;
    unsigned int *target = codepoint_out != 0 ? codepoint_out : &local_codepoint;

    if (text == 0 || text[0] == '\0') {
        if (codepoint_out != 0) {
            *codepoint_out = 0U;
        }
        return 0U;
    }

    length = rt_strlen(text);
    if (rt_utf8_decode(text, length, &index, target) != 0 || index == 0U) {
        if (codepoint_out != 0) {
            *codepoint_out = (unsigned char)text[0];
        }
        return 1U;
    }

    return index;
}

static char tool_regex_to_lower_ascii(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int tool_regex_codepoints_equal(unsigned int lhs, unsigned int rhs, int ignore_case) {
    if (ignore_case) {
        lhs = rt_unicode_simple_fold(lhs);
        rhs = rt_unicode_simple_fold(rhs);
    }
    return lhs == rhs;
}

static int tool_regex_chars_equal(char lhs, char rhs, int ignore_case) {
    return tool_regex_codepoints_equal((unsigned char)lhs, (unsigned char)rhs, ignore_case);
}

static int tool_regex_is_word_char(unsigned int ch) {
    return rt_unicode_is_word(ch);
}

static void tool_regex_copy_captures(ToolRegexCaptures *dst, const ToolRegexCaptures *src) {
    size_t i;
    for (i = 0; i < sizeof(dst->starts) / sizeof(dst->starts[0]); ++i) {
        dst->starts[i] = src->starts[i];
        dst->ends[i] = src->ends[i];
    }
}

static void tool_regex_clear_captures(ToolRegexCaptures *captures) {
    rt_memset(captures, 0, sizeof(*captures));
}

static int tool_regex_decode_escape(char code, char *out) {
    if (out == 0) {
        return -1;
    }

    if (code == 'n') {
        *out = '\n';
    } else if (code == 'r') {
        *out = '\r';
    } else if (code == 't') {
        *out = '\t';
    } else if (code == 'f') {
        *out = '\f';
    } else if (code == 'v') {
        *out = '\v';
    } else {
        *out = code;
    }
    return 0;
}

static int tool_regex_escape_matches(char code, unsigned int ch, int ignore_case) {
    char decoded = '\0';

    if (code == 'd') {
        return ch >= '0' && ch <= '9';
    }
    if (code == 'D') {
        return !(ch >= '0' && ch <= '9');
    }
    if (code == 'w') {
        return tool_regex_is_word_char(ch);
    }
    if (code == 'W') {
        return !tool_regex_is_word_char(ch);
    }
    if (code == 's') {
        return rt_unicode_is_space(ch);
    }
    if (code == 'S') {
        return !rt_unicode_is_space(ch);
    }

    (void)tool_regex_decode_escape(code, &decoded);
    return tool_regex_codepoints_equal((unsigned char)decoded, ch, ignore_case);
}

static size_t tool_regex_skip_class(const char *pattern, size_t pos, size_t end) {
    size_t i = pos + 1U;

    if (i < end && pattern[i] == '^') {
        i += 1U;
    }
    if (i < end && pattern[i] == ']') {
        i += 1U;
    }

    while (i < end) {
        if (pattern[i] == '\\' && i + 1U < end) {
            i += 2U;
            continue;
        }
        if (pattern[i] == ']') {
            return i + 1U;
        }
        i += 1U;
    }

    return pos + 1U;
}

static size_t tool_regex_find_group_end(const char *pattern, size_t pos, size_t end) {
    size_t i = pos + 1U;
    unsigned long long depth = 1ULL;

    while (i < end) {
        if (pattern[i] == '\\' && i + 1U < end) {
            i += 2U;
            continue;
        }
        if (pattern[i] == '[') {
            i = tool_regex_skip_class(pattern, i, end);
            continue;
        }
        if (pattern[i] == '(') {
            depth += 1ULL;
        } else if (pattern[i] == ')') {
            depth -= 1ULL;
            if (depth == 0ULL) {
                return i;
            }
        }
        i += 1U;
    }

    return end;
}

static size_t tool_regex_group_index(const char *pattern, size_t group_pos) {
    size_t i = 0U;
    size_t count = 0U;
    size_t end = rt_strlen(pattern);

    while (i < end && i <= group_pos) {
        if (pattern[i] == '\\' && i + 1U < end) {
            i += 2U;
            continue;
        }
        if (pattern[i] == '[') {
            i = tool_regex_skip_class(pattern, i, end);
            continue;
        }
        if (pattern[i] == '(') {
            count += 1U;
            if (i == group_pos) {
                return count;
            }
        }
        i += 1U;
    }

    return 0U;
}

static size_t tool_regex_atom_span(const char *pattern, size_t pos, size_t end) {
    if (pos >= end) {
        return 0U;
    }
    if (pattern[pos] == '\\' && pos + 1U < end) {
        return 2U;
    }
    if (pattern[pos] == '[') {
        return tool_regex_skip_class(pattern, pos, end) - pos;
    }
    if (pattern[pos] == '(') {
        size_t close = tool_regex_find_group_end(pattern, pos, end);
        if (close < end) {
            return close - pos + 1U;
        }
    }
    if (((unsigned char)pattern[pos] & 0x80U) != 0U) {
        size_t i = pos + 1U;
        while (i < end && tool_regex_is_utf8_continuation((unsigned char)pattern[i])) {
            i += 1U;
        }
        return i - pos;
    }
    return 1U;
}

static int tool_regex_class_matches(const char *pattern, size_t atom_len, unsigned int ch, int ignore_case) {
    size_t i = 1U;
    int negate = 0;
    int matched = 0;
    char compare_ch = (char)(ch <= 0x7fU ? ch : 0);

    if (ignore_case && ch <= 0x7fU) {
        compare_ch = tool_regex_to_lower_ascii(compare_ch);
    }

    if (pattern == 0 || atom_len < 2U || pattern[0] != '[') {
        return 0;
    }

    if (pattern[i] == '^') {
        negate = 1;
        i += 1U;
    }

    while (i + 1U < atom_len) {
        int current_is_special = 0;
        char current = '\0';

        if (pattern[i] == '\\' && i + 2U < atom_len) {
            char code = pattern[i + 1U];
            int special = tool_regex_escape_matches(code, ch, ignore_case);
            if (code == 'd' || code == 'D' || code == 'w' || code == 'W' || code == 's' || code == 'S') {
                if (special) {
                    matched = 1;
                }
                current_is_special = 1;
            } else {
                (void)tool_regex_decode_escape(code, &current);
            }
            i += 2U;
        } else {
            current = pattern[i];
            i += 1U;
        }

        if (current_is_special) {
            continue;
        }

        if (ignore_case) {
            current = tool_regex_to_lower_ascii(current);
        }

        if (i + 1U < atom_len && pattern[i] == '-' && pattern[i + 1U] != ']') {
            char range_end = '\0';

            i += 1U;
            if (pattern[i] == '\\' && i + 1U < atom_len) {
                (void)tool_regex_decode_escape(pattern[i + 1U], &range_end);
                i += 2U;
            } else {
                range_end = pattern[i];
                i += 1U;
            }

            if (ignore_case) {
                range_end = tool_regex_to_lower_ascii(range_end);
            }

            if (current <= compare_ch && compare_ch <= range_end) {
                matched = 1;
            }
        } else if (current == compare_ch) {
            matched = 1;
        }
    }

    return negate ? !matched : matched;
}

static int tool_regex_match_expression(const char *pattern,
                                       size_t pos,
                                       size_t end,
                                       const char *text,
                                       const char *origin,
                                       int ignore_case,
                                       ToolRegexCaptures *captures,
                                       const char **end_out);

static int tool_regex_match_atom(const char *pattern,
                                 size_t pos,
                                 size_t atom_end,
                                 const char *text,
                                 const char *origin,
                                 int ignore_case,
                                 ToolRegexCaptures *captures,
                                 const char **next_text_out) {
    unsigned int text_codepoint = 0U;
    size_t text_advance = 0U;
    if (pos >= atom_end) {
        return 0;
    }

    if (pattern[pos] == '^' && atom_end == pos + 1U) {
        if (text == origin) {
            *next_text_out = text;
            return 1;
        }
        return 0;
    }

    if (pattern[pos] == '$' && atom_end == pos + 1U) {
        if (*text == '\0') {
            *next_text_out = text;
            return 1;
        }
        return 0;
    }

    if (pattern[pos] == '(' && atom_end > pos + 1U && pattern[atom_end - 1U] == ')') {
        ToolRegexCaptures local;
        const char *group_end = 0;
        size_t group_index = tool_regex_group_index(pattern, pos);

        tool_regex_copy_captures(&local, captures);
        if (!tool_regex_match_expression(pattern, pos + 1U, atom_end - 1U, text, origin, ignore_case, &local, &group_end)) {
            return 0;
        }
        if (group_index < sizeof(local.starts) / sizeof(local.starts[0])) {
            local.starts[group_index] = text;
            local.ends[group_index] = group_end;
        }
        *captures = local;
        *next_text_out = group_end;
        return 1;
    }

    if (pattern[pos] == '\\' && pos + 1U < atom_end && pattern[pos + 1U] >= '0' && pattern[pos + 1U] <= '9') {
        size_t capture_index = (size_t)(pattern[pos + 1U] - '0');
        const char *capture_start = captures->starts[capture_index];
        const char *capture_end = captures->ends[capture_index];
        size_t i = 0U;
        size_t length;

        if (capture_start == 0 || capture_end == 0 || capture_end < capture_start) {
            return 0;
        }
        length = (size_t)(capture_end - capture_start);
        while (i < length) {
            if (text[i] == '\0' || !tool_regex_chars_equal(capture_start[i], text[i], ignore_case)) {
                return 0;
            }
            i += 1U;
        }
        *next_text_out = text + length;
        return 1;
    }

    if (*text == '\0') {
        return 0;
    }

    text_advance = tool_regex_decode_codepoint(text, &text_codepoint);
    if (text_advance == 0U) {
        return 0;
    }

    if (pattern[pos] == '.' && atom_end == pos + 1U) {
        *next_text_out = text + text_advance;
        return 1;
    }
    if (pattern[pos] == '[') {
        if (tool_regex_class_matches(pattern + pos, atom_end - pos, text_codepoint, ignore_case)) {
            *next_text_out = text + text_advance;
            return 1;
        }
        return 0;
    }
    if (pattern[pos] == '\\' && pos + 1U < atom_end) {
        if (tool_regex_escape_matches(pattern[pos + 1U], text_codepoint, ignore_case)) {
            *next_text_out = text + text_advance;
            return 1;
        }
        return 0;
    }

    {
        unsigned int pattern_codepoint = 0U;
        size_t pattern_advance = tool_regex_decode_codepoint(pattern + pos, &pattern_codepoint);

        if (pattern_advance > 0U && tool_regex_codepoints_equal(pattern_codepoint, text_codepoint, ignore_case)) {
            *next_text_out = text + text_advance;
            return 1;
        }
    }

    return 0;
}

static int tool_regex_parse_repeat_count(const char *pattern, size_t *pos, size_t end, unsigned long long *value_out) {
    unsigned long long value = 0ULL;
    size_t index = *pos;
    int saw_digit = 0;

    while (index < end && pattern[index] >= '0' && pattern[index] <= '9') {
        saw_digit = 1;
        value = (value * 10ULL) + (unsigned long long)(pattern[index] - '0');
        index += 1U;
    }

    if (!saw_digit) {
        return 0;
    }

    *pos = index;
    *value_out = value;
    return 1;
}

static int tool_regex_parse_quantifier(const char *pattern,
                                       size_t pos,
                                       size_t end,
                                       unsigned long long *min_out,
                                       unsigned long long *max_out,
                                       size_t *quantifier_end_out) {
    if (pos >= end) {
        return 0;
    }

    if (pattern[pos] == '*') {
        *min_out = 0ULL;
        *max_out = TOOL_REGEX_REPEAT_UNBOUNDED;
        *quantifier_end_out = pos + 1U;
        return 1;
    }
    if (pattern[pos] == '+') {
        *min_out = 1ULL;
        *max_out = TOOL_REGEX_REPEAT_UNBOUNDED;
        *quantifier_end_out = pos + 1U;
        return 1;
    }
    if (pattern[pos] == '?') {
        *min_out = 0ULL;
        *max_out = 1ULL;
        *quantifier_end_out = pos + 1U;
        return 1;
    }
    if (pattern[pos] == '{') {
        size_t index = pos + 1U;
        unsigned long long minimum = 0ULL;
        unsigned long long maximum = 0ULL;
        int has_minimum = 0;
        int has_maximum = 0;

        has_minimum = tool_regex_parse_repeat_count(pattern, &index, end, &minimum);
        if (!has_minimum) {
            return 0;
        }

        if (index < end && pattern[index] == '}') {
            *min_out = minimum;
            *max_out = minimum;
            *quantifier_end_out = index + 1U;
            return 1;
        }

        if (index >= end || pattern[index] != ',') {
            return 0;
        }
        index += 1U;

        has_maximum = tool_regex_parse_repeat_count(pattern, &index, end, &maximum);
        if (index >= end || pattern[index] != '}') {
            return 0;
        }

        *min_out = minimum;
        *max_out = has_maximum ? maximum : TOOL_REGEX_REPEAT_UNBOUNDED;
        *quantifier_end_out = index + 1U;
        return *max_out == TOOL_REGEX_REPEAT_UNBOUNDED || *max_out >= *min_out;
    }

    return 0;
}

static int tool_regex_match_quantified(const char *pattern,
                                       size_t pos,
                                       size_t atom_end,
                                       size_t rest_pos,
                                       size_t end,
                                       const char *text,
                                       const char *origin,
                                       int ignore_case,
                                       unsigned long long min_count,
                                       unsigned long long max_count,
                                       unsigned long long count,
                                       ToolRegexCaptures *captures,
                                       const char **end_out);

static int tool_regex_match_sequence(const char *pattern,
                                     size_t pos,
                                     size_t end,
                                     const char *text,
                                     const char *origin,
                                     int ignore_case,
                                     ToolRegexCaptures *captures,
                                     const char **end_out) {
    size_t atom_len;
    size_t atom_end;
    size_t rest_pos;
    unsigned long long min_count = 1ULL;
    unsigned long long max_count = 1ULL;
    size_t quantifier_end = 0U;

    if (pos >= end) {
        *end_out = text;
        return 1;
    }

    atom_len = tool_regex_atom_span(pattern, pos, end);
    if (atom_len == 0U) {
        *end_out = text;
        return 1;
    }

    atom_end = pos + atom_len;
    rest_pos = atom_end;
    if (tool_regex_parse_quantifier(pattern, atom_end, end, &min_count, &max_count, &quantifier_end)) {
        rest_pos = quantifier_end;
        return tool_regex_match_quantified(pattern,
                                           pos,
                                           atom_end,
                                           rest_pos,
                                           end,
                                           text,
                                           origin,
                                           ignore_case,
                                           min_count,
                                           max_count,
                                           0ULL,
                                           captures,
                                           end_out);
    }

    {
        ToolRegexCaptures local;
        const char *next = 0;

        tool_regex_copy_captures(&local, captures);
        if (!tool_regex_match_atom(pattern, pos, atom_end, text, origin, ignore_case, &local, &next)) {
            return 0;
        }
        if (tool_regex_match_sequence(pattern, rest_pos, end, next, origin, ignore_case, &local, end_out)) {
            *captures = local;
            return 1;
        }
        return 0;
    }
}

static int tool_regex_match_quantified(const char *pattern,
                                       size_t pos,
                                       size_t atom_end,
                                       size_t rest_pos,
                                       size_t end,
                                       const char *text,
                                       const char *origin,
                                       int ignore_case,
                                       unsigned long long min_count,
                                       unsigned long long max_count,
                                       unsigned long long count,
                                       ToolRegexCaptures *captures,
                                       const char **end_out) {
    if (max_count == TOOL_REGEX_REPEAT_UNBOUNDED || count < max_count) {
        ToolRegexCaptures local;
        const char *next = 0;

        tool_regex_copy_captures(&local, captures);
        if (tool_regex_match_atom(pattern, pos, atom_end, text, origin, ignore_case, &local, &next) &&
            next != text &&
            tool_regex_match_quantified(pattern,
                                        pos,
                                        atom_end,
                                        rest_pos,
                                        end,
                                        next,
                                        origin,
                                        ignore_case,
                                        min_count,
                                        max_count,
                                        count + 1ULL,
                                        &local,
                                        end_out)) {
            *captures = local;
            return 1;
        }
    }

    if (count >= min_count) {
        return tool_regex_match_sequence(pattern, rest_pos, end, text, origin, ignore_case, captures, end_out);
    }

    return 0;
}

static int tool_regex_match_expression(const char *pattern,
                                       size_t pos,
                                       size_t end,
                                       const char *text,
                                       const char *origin,
                                       int ignore_case,
                                       ToolRegexCaptures *captures,
                                       const char **end_out) {
    size_t branch_start = pos;
    size_t i = pos;
    unsigned long long depth = 0ULL;

    while (i < end) {
        if (pattern[i] == '\\' && i + 1U < end) {
            i += 2U;
            continue;
        }
        if (pattern[i] == '[') {
            i = tool_regex_skip_class(pattern, i, end);
            continue;
        }
        if (pattern[i] == '(') {
            depth += 1ULL;
            i += 1U;
            continue;
        }
        if (pattern[i] == ')') {
            if (depth > 0ULL) {
                depth -= 1ULL;
            }
            i += 1U;
            continue;
        }
        if (pattern[i] == '|' && depth == 0ULL) {
            ToolRegexCaptures local;

            tool_regex_copy_captures(&local, captures);
            if (tool_regex_match_sequence(pattern, branch_start, i, text, origin, ignore_case, &local, end_out)) {
                *captures = local;
                return 1;
            }
            branch_start = i + 1U;
        }
        i += 1U;
    }

    {
        ToolRegexCaptures local;

        tool_regex_copy_captures(&local, captures);
        if (tool_regex_match_sequence(pattern, branch_start, end, text, origin, ignore_case, &local, end_out)) {
            *captures = local;
            return 1;
        }
    }

    return 0;
}

static int tool_regex_search_internal(const char *pattern,
                                      const char *text,
                                      int ignore_case,
                                      size_t search_start,
                                      size_t *start_out,
                                      size_t *end_out,
                                      ToolRegexCaptures *captures_out) {
    size_t pos = search_start;
    size_t pattern_end = rt_strlen(pattern);

    if (pattern == 0 || text == 0 || start_out == 0 || end_out == 0) {
        return 0;
    }

    while (1) {
        ToolRegexCaptures captures;
        const char *end_ptr = 0;

        tool_regex_clear_captures(&captures);
        if (tool_regex_match_expression(pattern, 0U, pattern_end, text + pos, text, ignore_case, &captures, &end_ptr)) {
            captures.starts[0] = text + pos;
            captures.ends[0] = end_ptr;
            *start_out = pos;
            *end_out = (size_t)(end_ptr - text);
            if (captures_out != 0) {
                *captures_out = captures;
            }
            return 1;
        }

        if (text[pos] == '\0') {
            break;
        }
        if (tool_regex_is_utf8_continuation((unsigned char)text[pos])) {
            pos += 1U;
        } else {
            size_t advance = tool_regex_decode_codepoint(text + pos, 0);
            pos += advance > 0U ? advance : 1U;
        }
    }

    return 0;
}

int tool_regex_search(const char *pattern, const char *text, int ignore_case, size_t search_start, size_t *start_out, size_t *end_out) {
    return tool_regex_search_internal(pattern, text, ignore_case, search_start, start_out, end_out, 0);
}

static int tool_regex_append_text(char *buffer, size_t buffer_size, size_t *used, const char *text, size_t length) {
    if (*used + length + 1U > buffer_size) {
        return -1;
    }
    if (length > 0U) {
        memcpy(buffer + *used, text, length);
        *used += length;
    }
    buffer[*used] = '\0';
    return 0;
}

static int tool_regex_append_replacement(char *buffer,
                                         size_t buffer_size,
                                         size_t *used,
                                         const char *replacement,
                                         const ToolRegexCaptures *captures) {
    size_t i = 0U;

    while (replacement != 0 && replacement[i] != '\0') {
        if (replacement[i] == '&') {
            if (captures != 0 && captures->starts[0] != 0 && captures->ends[0] != 0 &&
                tool_regex_append_text(buffer, buffer_size, used, captures->starts[0], (size_t)(captures->ends[0] - captures->starts[0])) != 0) {
                return -1;
            }
            i += 1U;
            continue;
        }

        if (replacement[i] == '\\' && replacement[i + 1U] != '\0') {
            char code = replacement[i + 1U];
            char decoded = '\0';

            if (code >= '0' && code <= '9') {
                size_t capture_index = (size_t)(code - '0');
                if (captures != 0 && captures->starts[capture_index] != 0 && captures->ends[capture_index] != 0 &&
                    tool_regex_append_text(buffer,
                                           buffer_size,
                                           used,
                                           captures->starts[capture_index],
                                           (size_t)(captures->ends[capture_index] - captures->starts[capture_index])) != 0) {
                    return -1;
                }
            } else {
                if (code == '&' || code == '\\') {
                    decoded = code;
                } else {
                    (void)tool_regex_decode_escape(code, &decoded);
                }
                if (tool_regex_append_text(buffer, buffer_size, used, &decoded, 1U) != 0) {
                    return -1;
                }
            }
            i += 2U;
            continue;
        }

        if (tool_regex_append_text(buffer, buffer_size, used, replacement + i, 1U) != 0) {
            return -1;
        }
        i += 1U;
    }

    return 0;
}

int tool_regex_replace(const char *pattern,
                       const char *replacement,
                       const char *input,
                       int ignore_case,
                       int global,
                       char *output,
                       size_t output_size,
                       int *changed_out) {
    size_t in_pos = 0U;
    size_t out_pos = 0U;
    int changed = 0;

    if (pattern == 0 || replacement == 0 || input == 0 || output == 0 || output_size == 0U) {
        return -1;
    }

    if (changed_out != 0) {
        *changed_out = 0;
    }

    output[0] = '\0';
    if (pattern[0] == '\0') {
        return tool_regex_append_text(output, output_size, &out_pos, input, rt_strlen(input));
    }

    while (1) {
        size_t match_start = 0U;
        size_t match_end = 0U;
        ToolRegexCaptures captures;

        if (!tool_regex_search_internal(pattern, input, ignore_case, in_pos, &match_start, &match_end, &captures)) {
            break;
        }

        if (tool_regex_append_text(output, output_size, &out_pos, input + in_pos, match_start - in_pos) != 0 ||
            tool_regex_append_replacement(output, output_size, &out_pos, replacement, &captures) != 0) {
            return -1;
        }

        changed = 1;
        if (!global) {
            in_pos = match_end;
            break;
        }

        if (match_end > match_start) {
            in_pos = match_end;
        } else {
            if (input[in_pos] == '\0') {
                break;
            }
            if (tool_regex_append_text(output, output_size, &out_pos, input + in_pos, 1U) != 0) {
                return -1;
            }
            in_pos += 1U;
        }
    }

    if (tool_regex_append_text(output, output_size, &out_pos, input + in_pos, rt_strlen(input + in_pos)) != 0) {
        return -1;
    }

    if (changed_out != 0) {
        *changed_out = changed;
    }
    return 0;
}

int tool_wildcard_match(const char *pattern, const char *text) {
    if (pattern[0] == '\0') {
        return text[0] == '\0';
    }

    if (pattern[0] == '*') {
        return tool_wildcard_match(pattern + 1, text) || (text[0] != '\0' && tool_wildcard_match(pattern, text + 1));
    }

    if (pattern[0] == '?') {
        return text[0] != '\0' && tool_wildcard_match(pattern + 1, text + 1);
    }

    return pattern[0] == text[0] && tool_wildcard_match(pattern + 1, text + 1);
}

int tool_resolve_destination(const char *source_path, const char *dest_path, char *buffer, size_t buffer_size) {
    int is_directory = 0;
    size_t path_len;

    if (platform_path_is_directory(dest_path, &is_directory) == 0 && is_directory) {
        return tool_join_path(dest_path, tool_base_name(source_path), buffer, buffer_size);
    }

    path_len = rt_strlen(dest_path);
    if (path_len + 1 > buffer_size) {
        return -1;
    }

    memcpy(buffer, dest_path, path_len + 1);
    return 0;
}

static void tool_pop_path_component(char *path) {
    size_t len;

    if (path == 0) {
        return;
    }

    len = rt_strlen(path);
    while (len > 1U && path[len - 1U] == '/') {
        path[len - 1U] = '\0';
        len -= 1U;
    }

    while (len > 1U && path[len - 1U] != '/') {
        path[len - 1U] = '\0';
        len -= 1U;
    }

    while (len > 1U && path[len - 1U] == '/') {
        path[len - 1U] = '\0';
        len -= 1U;
    }

    if (len == 0U) {
        rt_copy_string(path, 2U, "/");
    }
}

static int tool_append_path_component(char *path, size_t buffer_size, const char *component) {
    size_t len;
    size_t component_len;

    if (path == 0 || component == 0) {
        return -1;
    }

    len = rt_strlen(path);
    component_len = rt_strlen(component);
    if (len == 0U) {
        if (buffer_size < 2U) {
            return -1;
        }
        path[0] = '/';
        path[1] = '\0';
        len = 1U;
    }

    if (!(len == 1U && path[0] == '/')) {
        if (len + 1U >= buffer_size) {
            return -1;
        }
        path[len++] = '/';
        path[len] = '\0';
    }

    if (len + component_len + 1U > buffer_size) {
        return -1;
    }

    memcpy(path + len, component, component_len + 1U);
    return 0;
}

static int tool_build_absolute_path(const char *path, char *buffer, size_t buffer_size) {
    char cwd[2048];

    if (path == 0 || buffer == 0 || buffer_size == 0U || path[0] == '\0') {
        return -1;
    }

    if (path[0] == '/') {
        if (rt_strlen(path) + 1U > buffer_size) {
            return -1;
        }
        rt_copy_string(buffer, buffer_size, path);
        return 0;
    }

    if (platform_get_current_directory(cwd, sizeof(cwd)) != 0) {
        return -1;
    }

    return tool_join_path(cwd, path, buffer, buffer_size);
}

static int tool_concat_path_suffix(const char *prefix, const char *suffix, char *buffer, size_t buffer_size) {
    size_t prefix_len;
    size_t suffix_index = 0;

    if (prefix == 0 || buffer == 0 || buffer_size == 0U) {
        return -1;
    }

    prefix_len = rt_strlen(prefix);
    if (prefix_len + 1U > buffer_size) {
        return -1;
    }

    memcpy(buffer, prefix, prefix_len + 1U);
    while (prefix_len > 1U && buffer[prefix_len - 1U] == '/' && suffix != 0 && suffix[suffix_index] == '/') {
        buffer[prefix_len - 1U] = '\0';
        prefix_len -= 1U;
    }

    while (suffix != 0 && suffix[suffix_index] != '\0') {
        if (prefix_len + 1U >= buffer_size) {
            return -1;
        }
        buffer[prefix_len++] = suffix[suffix_index++];
    }
    buffer[prefix_len] = '\0';
    return 0;
}

int tool_canonicalize_path_policy(
    const char *path,
    int resolve_symlinks,
    int allow_missing,
    int logical_policy,
    char *buffer,
    size_t buffer_size
) {
    char pending[2048];
    char resolved[2048];
    size_t index = 0;
    unsigned int symlink_count = 0U;

    if (tool_build_absolute_path(path, pending, sizeof(pending)) != 0) {
        return -1;
    }

    rt_copy_string(resolved, sizeof(resolved), "/");

    while (pending[index] != '\0') {
        char component[256];
        size_t component_len = 0U;
        char candidate[2048];

        while (pending[index] == '/') {
            index += 1U;
        }
        if (pending[index] == '\0') {
            break;
        }

        while (pending[index] != '\0' && pending[index] != '/' && component_len + 1U < sizeof(component)) {
            component[component_len++] = pending[index++];
        }
        component[component_len] = '\0';

        while (pending[index] != '\0' && pending[index] != '/') {
            index += 1U;
        }

        if (rt_strcmp(component, ".") == 0) {
            continue;
        }

        if (rt_strcmp(component, "..") == 0) {
            tool_pop_path_component(resolved);
            continue;
        }

        rt_copy_string(candidate, sizeof(candidate), resolved);
        if (tool_append_path_component(candidate, sizeof(candidate), component) != 0) {
            return -1;
        }

        if (resolve_symlinks && !logical_policy) {
            char target[2048];

            if (platform_read_symlink(candidate, target, sizeof(target)) == 0) {
                char replacement[2048];
                char remainder[2048];

                if (symlink_count >= 64U) {
                    return -1;
                }
                symlink_count += 1U;

                if (target[0] == '/') {
                    rt_copy_string(replacement, sizeof(replacement), target);
                } else {
                    if (tool_join_path(resolved, target, replacement, sizeof(replacement)) != 0) {
                        return -1;
                    }
                }

                rt_copy_string(remainder, sizeof(remainder), pending + index);
                if (tool_concat_path_suffix(replacement, remainder, pending, sizeof(pending)) != 0) {
                    return -1;
                }

                rt_copy_string(resolved, sizeof(resolved), "/");
                index = 0U;
                continue;
            }
        }

        if (!allow_missing) {
            PlatformDirEntry entry;
            if (platform_get_path_info(candidate, &entry) != 0) {
                return -1;
            }
        }

        rt_copy_string(resolved, sizeof(resolved), candidate);
    }

    if (resolved[0] == '\0') {
        rt_copy_string(resolved, sizeof(resolved), "/");
    }

    if (resolve_symlinks && logical_policy) {
        return tool_canonicalize_path_policy(resolved, 1, allow_missing, 0, buffer, buffer_size);
    }

    rt_copy_string(buffer, buffer_size, resolved);
    return 0;
}

int tool_canonicalize_path(const char *path, int resolve_symlinks, int allow_missing, char *buffer, size_t buffer_size) {
    return tool_canonicalize_path_policy(path, resolve_symlinks, allow_missing, 0, buffer, buffer_size);
}

int tool_path_exists(const char *path) {
    PlatformDirEntry entry;
    return path != 0 && platform_get_path_info(path, &entry) == 0;
}

static void tool_normalize_for_compare(const char *path, char *buffer, size_t buffer_size) {
    size_t len;

    if (buffer_size == 0U) {
        return;
    }

    if (path == 0 || path[0] == '\0') {
        buffer[0] = '\0';
        return;
    }

    rt_copy_string(buffer, buffer_size, path);
    len = rt_strlen(buffer);
    while (len > 1U && buffer[len - 1U] == '/') {
        buffer[len - 1U] = '\0';
        len -= 1U;
    }
}

int tool_paths_equal(const char *left_path, const char *right_path) {
    char left[2048];
    char right[2048];

    if (left_path == 0 || right_path == 0) {
        return 0;
    }

    if (tool_canonicalize_path(left_path, 0, 1, left, sizeof(left)) == 0 &&
        tool_canonicalize_path(right_path, 0, 1, right, sizeof(right)) == 0) {
        return rt_strcmp(left, right) == 0;
    }

    tool_normalize_for_compare(left_path, left, sizeof(left));
    tool_normalize_for_compare(right_path, right, sizeof(right));
    return rt_strcmp(left, right) == 0;
}

int tool_path_is_root(const char *path) {
    char normalized[2048];

    if (path == 0) {
        return 0;
    }

    if (tool_canonicalize_path(path, 0, 1, normalized, sizeof(normalized)) == 0) {
        return rt_strcmp(normalized, "/") == 0;
    }

    tool_normalize_for_compare(path, normalized, sizeof(normalized));
    return rt_strcmp(normalized, "/") == 0;
}

int tool_copy_file(const char *source_path, const char *dest_path) {
    int src_fd = platform_open_read(source_path);
    int dst_fd;
    char buffer[4096];

    if (src_fd < 0) {
        return -1;
    }

    dst_fd = platform_open_write(dest_path, 0644U);
    if (dst_fd < 0) {
        platform_close(src_fd);
        return -1;
    }

    for (;;) {
        long bytes_read = platform_read(src_fd, buffer, sizeof(buffer));
        long offset = 0;

        if (bytes_read == 0) {
            break;
        }

        if (bytes_read < 0) {
            platform_close(src_fd);
            platform_close(dst_fd);
            return -1;
        }

        while (offset < bytes_read) {
            long bytes_written = platform_write(dst_fd, buffer + offset, (size_t)(bytes_read - offset));
            if (bytes_written <= 0) {
                platform_close(src_fd);
                platform_close(dst_fd);
                return -1;
            }
            offset += bytes_written;
        }
    }

    platform_close(src_fd);
    platform_close(dst_fd);
    return 0;
}

int tool_copy_path(const char *source_path, const char *dest_path, int recursive, int preserve_mode, int preserve_symlinks) {
    PlatformDirEntry source_info;
    char link_target[2048];

    if (source_path == 0 || dest_path == 0 || platform_get_path_info(source_path, &source_info) != 0) {
        return -1;
    }

    if (preserve_symlinks && platform_read_symlink(source_path, link_target, sizeof(link_target)) == 0) {
        if (tool_paths_equal(source_path, dest_path)) {
            return 0;
        }
        (void)platform_remove_file(dest_path);
        if (platform_create_symbolic_link(link_target, dest_path) != 0) {
            return -1;
        }
        return 0;
    }

    if (!source_info.is_dir) {
        if (tool_copy_file(source_path, dest_path) != 0) {
            return -1;
        }
        if (preserve_mode) {
            (void)platform_change_mode(dest_path, source_info.mode & 07777U);
        }
        return 0;
    }

    if (!recursive) {
        return -2;
    }

    {
        int dest_is_directory = 0;
        unsigned int dir_mode = preserve_mode ? (source_info.mode & 07777U) : 0755U;

        if (platform_path_is_directory(dest_path, &dest_is_directory) != 0) {
            if (platform_make_directory(dest_path, dir_mode) != 0) {
                return -1;
            }
        } else if (!dest_is_directory) {
            return -1;
        }
    }

    {
        enum { TOOL_COPY_ENTRY_CAPACITY = 1024, TOOL_COPY_PATH_CAPACITY = 2048 };
        PlatformDirEntry entries[TOOL_COPY_ENTRY_CAPACITY];
        size_t count = 0;
        size_t i;
        int path_is_directory = 0;

        if (platform_collect_entries(source_path, 1, entries, TOOL_COPY_ENTRY_CAPACITY, &count, &path_is_directory) != 0 ||
            !path_is_directory) {
            return -1;
        }

        for (i = 0; i < count; ++i) {
            char child_source[TOOL_COPY_PATH_CAPACITY];
            char child_dest[TOOL_COPY_PATH_CAPACITY];

            if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
                continue;
            }

            if (tool_join_path(source_path, entries[i].name, child_source, sizeof(child_source)) != 0 ||
                tool_join_path(dest_path, entries[i].name, child_dest, sizeof(child_dest)) != 0 ||
                tool_copy_path(child_source, child_dest, recursive, preserve_mode, preserve_symlinks) != 0) {
                platform_free_entries(entries, count);
                return -1;
            }
        }

        platform_free_entries(entries, count);
    }

    if (preserve_mode) {
        (void)platform_change_mode(dest_path, source_info.mode & 07777U);
    }

    return 0;
}

int tool_remove_path(const char *path, int recursive) {
    enum { TOOL_REMOVE_ENTRY_CAPACITY = 1024, TOOL_REMOVE_PATH_CAPACITY = 2048 };
    PlatformDirEntry entries[TOOL_REMOVE_ENTRY_CAPACITY];
    size_t count = 0;
    size_t i;
    int is_directory = 0;

    if (path == 0 || platform_collect_entries(path, 1, entries, TOOL_REMOVE_ENTRY_CAPACITY, &count, &is_directory) != 0) {
        return -1;
    }

    if (!is_directory) {
        platform_free_entries(entries, count);
        return platform_remove_file(path) == 0 ? 0 : -1;
    }

    if (!recursive) {
        platform_free_entries(entries, count);
        return -2;
    }

    for (i = 0; i < count; ++i) {
        char child_path[TOOL_REMOVE_PATH_CAPACITY];

        if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
            continue;
        }

        if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0 ||
            tool_remove_path(child_path, 1) != 0) {
            platform_free_entries(entries, count);
            return -1;
        }
    }

    platform_free_entries(entries, count);
    return platform_remove_directory(path) == 0 ? 0 : -1;
}
