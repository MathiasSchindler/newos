#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define JQ_MAX_INPUT (1024U * 1024U)
#define JQ_MAX_RESULTS 512U
#define JQ_MAX_FILTER 512U
#define JQ_MAX_GENERATED (1024U * 1024U)
#define JQ_MAX_VARIABLES 32U
#define JQ_MAX_FUNCTIONS 16U
#define JQ_MAX_FUNCTION_ARGS 4U
#define JQ_MAX_PATH_TOKENS 32U
#define JQ_NAME_CAPACITY 64U

typedef struct {
    const char *start;
    const char *end;
} JqSlice;

typedef struct {
    char name[JQ_NAME_CAPACITY];
    JqSlice value;
} JqVariable;

typedef struct {
    char name[JQ_NAME_CAPACITY];
    char args[JQ_MAX_FUNCTION_ARGS][JQ_NAME_CAPACITY];
    size_t arg_count;
    char body[JQ_MAX_FILTER];
} JqFunction;

typedef struct {
    int is_index;
    char key[JQ_NAME_CAPACITY];
    size_t index;
} JqPathToken;

static char jq_input[JQ_MAX_INPUT + 1U];
static char jq_generated[JQ_MAX_GENERATED + 1U];
static size_t jq_generated_used;
static JqVariable jq_variables[JQ_MAX_VARIABLES];
static size_t jq_variable_count;
static JqFunction jq_functions[JQ_MAX_FUNCTIONS];
static size_t jq_function_count;
static int raw_output;

static int json_array_count(const char *array, size_t *count_out);

static void print_usage(void) {
    tool_write_usage("jq", "[-r] FILTER [FILE]");
}

static int read_all(int fd, char *buffer, size_t capacity, size_t *size_out) {
    size_t used = 0U;
    long bytes;

    while (used < capacity && (bytes = platform_read(fd, buffer + used, capacity - used)) > 0) {
        used += (size_t)bytes;
    }
    if (used == capacity) return -1;
    buffer[used] = '\0';
    *size_out = used;
    return 0;
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *skip_string(const char *p) {
    if (*p != '"') return 0;
    p++;
    while (*p != '\0') {
        if (*p == '\\' && p[1] != '\0') {
            p += 2;
        } else if (*p == '"') {
            return p + 1;
        } else {
            p++;
        }
    }
    return 0;
}

static const char *skip_value(const char *p) {
    int depth = 0;

    p = skip_ws(p);
    if (*p == '"') return skip_string(p);
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = open == '{' ? '}' : ']';
        (void)close;
        while (*p != '\0') {
            if (*p == '"') {
                p = skip_string(p);
                if (p == 0) return 0;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0) return p + 1;
            }
            p++;
        }
        return 0;
    }
    while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') p++;
    return p;
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int parse_hex4(const char *p, unsigned int *codepoint_out) {
    unsigned int codepoint = 0U;
    size_t i;

    for (i = 0U; i < 4U; ++i) {
        int value = hex_value(p[i]);
        if (value < 0) return -1;
        codepoint = (codepoint << 4U) | (unsigned int)value;
    }
    *codepoint_out = codepoint;
    return 0;
}

static int write_utf8_codepoint(unsigned int codepoint) {
    char encoded[4];
    size_t length = 0U;

    if (rt_utf8_encode(codepoint, encoded, sizeof(encoded), &length) != 0) return -1;
    return rt_write_all(1, encoded, length);
}

static int write_json_escape(char escape) {
    switch (escape) {
        case '"': return rt_write_char(1, '"');
        case '\\': return rt_write_char(1, '\\');
        case '/': return rt_write_char(1, '/');
        case 'b': return rt_write_char(1, '\b');
        case 'f': return rt_write_char(1, '\f');
        case 'n': return rt_write_char(1, '\n');
        case 'r': return rt_write_char(1, '\r');
        case 't': return rt_write_char(1, '\t');
        default: return -1;
    }
}

static int append_utf8_codepoint(char *buffer, size_t capacity, size_t *length_io, unsigned int codepoint) {
    char encoded[4];
    size_t encoded_length = 0U;

    if (rt_utf8_encode(codepoint, encoded, sizeof(encoded), &encoded_length) != 0) return -1;
    if (*length_io + encoded_length >= capacity) return -1;
    memcpy(buffer + *length_io, encoded, encoded_length);
    *length_io += encoded_length;
    return 0;
}

static int append_json_escape(char *buffer, size_t capacity, size_t *length_io, char escape) {
    char ch;

    switch (escape) {
        case '"': ch = '"'; break;
        case '\\': ch = '\\'; break;
        case '/': ch = '/'; break;
        case 'b': ch = '\b'; break;
        case 'f': ch = '\f'; break;
        case 'n': ch = '\n'; break;
        case 'r': ch = '\r'; break;
        case 't': ch = '\t'; break;
        default: return -1;
    }
    if (*length_io + 1U >= capacity) return -1;
    buffer[(*length_io)++] = ch;
    return 0;
}

static int append_json_u_escape(char *buffer, size_t capacity, size_t *length_io, const char **p_io) {
    const char *p = *p_io;
    unsigned int codepoint;

    if (parse_hex4(p, &codepoint) != 0) return -1;
    p += 4;
    if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
        unsigned int low;
        if (p[0] != '\\' || p[1] != 'u' || parse_hex4(p + 2, &low) != 0 || low < 0xdc00U || low > 0xdfffU) return -1;
        codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) + (low - 0xdc00U);
        p += 6;
    } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
        return -1;
    }
    if (append_utf8_codepoint(buffer, capacity, length_io, codepoint) != 0) return -1;
    *p_io = p;
    return 0;
}

static int string_key_equals(const char *start, const char *key, const char **after_out) {
    const char *p = start;
    size_t i = 0U;

    if (*p != '"') return 0;
    p++;
    while (*p != '\0' && *p != '"') {
        char decoded[5];
        size_t decoded_length = 0U;
        size_t j;

        if (*p == '\\') {
            p++;
            if (*p == 'u') {
                p++;
                if (append_json_u_escape(decoded, sizeof(decoded), &decoded_length, &p) != 0) return 0;
            } else {
                if (append_json_escape(decoded, sizeof(decoded), &decoded_length, *p) != 0) return 0;
                p++;
            }
        } else {
            decoded[decoded_length++] = *p++;
        }
        for (j = 0U; j < decoded_length; ++j) {
            if (key[i] != decoded[j]) return 0;
            i++;
        }
    }
    if (*p != '"' || key[i] != '\0') return 0;
    *after_out = p + 1;
    return 1;
}

static int find_key(const char *object, const char *key, const char **value_start, const char **value_end) {
    const char *p = skip_ws(object);

    if (*p != '{') return -1;
    p++;
    for (;;) {
        const char *after_key;
        const char *end;

        p = skip_ws(p);
        if (*p == '}') return -1;
        if (*p != '"') return -1;
        if (!string_key_equals(p, key, &after_key)) {
            p = skip_string(p);
            if (p == 0) return -1;
            p = skip_ws(p);
            if (*p != ':') return -1;
            p = skip_value(p + 1);
            if (p == 0) return -1;
        } else {
            p = skip_ws(after_key);
            if (*p != ':') return -1;
            *value_start = skip_ws(p + 1);
            end = skip_value(*value_start);
            if (end == 0) return -1;
            *value_end = end;
            return 0;
        }
        p = skip_ws(p);
        if (*p == ',') p++;
    }
}

static int find_array_index(const char *array, size_t target_index, const char **value_start, const char **value_end) {
    const char *p = skip_ws(array);
    size_t index = 0U;

    if (*p != '[') return -1;
    p++;
    for (;;) {
        const char *end;

        p = skip_ws(p);
        if (*p == ']') return -1;
        *value_start = p;
        end = skip_value(p);
        if (end == 0) return -1;
        if (index == target_index) {
            *value_end = end;
            return 0;
        }
        index++;
        p = skip_ws(end);
        if (*p == ',') {
            p++;
        } else if (*p == ']') {
            return -1;
        } else {
            return -1;
        }
    }
}

static int parse_filter_string_key(const char *filter, size_t *pos_io, char *key, size_t key_capacity) {
    size_t length = 0U;
    size_t pos = *pos_io;

    if (filter[pos] != '"') return -1;
    pos++;
    while (filter[pos] != '\0' && filter[pos] != '"') {
        if (filter[pos] == '\\') {
            const char *escape_pos;
            pos++;
            if (filter[pos] == 'u') {
                pos++;
                escape_pos = filter + pos;
                if (append_json_u_escape(key, key_capacity, &length, &escape_pos) != 0) return -1;
                pos = (size_t)(escape_pos - filter);
            } else {
                if (append_json_escape(key, key_capacity, &length, filter[pos]) != 0) return -1;
                pos++;
            }
        } else {
            if (length + 1U >= key_capacity) return -1;
            key[length++] = filter[pos++];
        }
    }
    if (filter[pos] != '"') return -1;
    key[length] = '\0';
    *pos_io = pos + 1U;
    return 0;
}

static int emit_raw_string(const char *start, const char *end) {
    const char *p = start + 1;
    const char *limit = end - 1;

    while (p < limit) {
        if (*p == '\\') {
            p++;
            if (p >= limit) return -1;
            if (*p == 'u') {
                unsigned int codepoint;
                p++;
                if (parse_hex4(p, &codepoint) != 0) return -1;
                p += 4;
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    unsigned int low;
                    if (p + 6 > limit || p[0] != '\\' || p[1] != 'u' || parse_hex4(p + 2, &low) != 0 || low < 0xdc00U || low > 0xdfffU) return -1;
                    codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) + (low - 0xdc00U);
                    p += 6;
                } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                    return -1;
                }
                if (write_utf8_codepoint(codepoint) != 0) return -1;
            } else {
                if (write_json_escape(*p) != 0) return -1;
                p++;
            }
        } else {
            if (rt_write_char(1, *p) != 0) return -1;
            p++;
        }
    }
    return rt_write_char(1, '\n');
}

static int emit_value(const char *start, const char *end) {
    if (raw_output && *start == '"' && end > start + 1 && end[-1] == '"') {
        return emit_raw_string(start, end);
    }
    if (rt_write_all(1, start, (size_t)(end - start)) != 0) return -1;
    return rt_write_char(1, '\n');
}

static int append_result(JqSlice *out, size_t *count_io, const char *start, const char *end) {
    if (*count_io >= JQ_MAX_RESULTS) return -1;
    out[*count_io].start = start;
    out[*count_io].end = end;
    *count_io += 1U;
    return 0;
}

static int generated_append(const char *text, size_t length) {
    if (jq_generated_used + length >= JQ_MAX_GENERATED) return -1;
    memmove(jq_generated + jq_generated_used, text, length);
    jq_generated_used += length;
    jq_generated[jq_generated_used] = '\0';
    return 0;
}

static int generated_append_cstr(const char *text) {
    return generated_append(text, rt_strlen(text));
}

static int generated_append_uint(size_t value) {
    char digits[32];
    size_t count = 0U;

    if (value == 0U) return generated_append_cstr("0");
    while (value != 0U && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count != 0U) {
        count--;
        if (generated_append(digits + count, 1U) != 0) return -1;
    }
    return 0;
}

static int generated_append_int(long long value) {
    unsigned long long magnitude;

    if (value < 0) {
        if (generated_append_cstr("-") != 0) return -1;
        magnitude = (unsigned long long)(-(value + 1LL)) + 1ULL;
    } else {
        magnitude = (unsigned long long)value;
    }
    return generated_append_uint((size_t)magnitude);
}

static int append_generated_value(JqSlice *out, size_t *count_io, const char *text) {
    size_t start = jq_generated_used;

    if (generated_append_cstr(text) != 0) return -1;
    return append_result(out, count_io, jq_generated + start, jq_generated + jq_generated_used);
}

static int append_generated_uint_value(JqSlice *out, size_t *count_io, size_t value) {
    size_t start = jq_generated_used;

    if (generated_append_uint(value) != 0) return -1;
    return append_result(out, count_io, jq_generated + start, jq_generated + jq_generated_used);
}

static int append_generated_int_value(JqSlice *out, size_t *count_io, long long value) {
    size_t start = jq_generated_used;

    if (generated_append_int(value) != 0) return -1;
    return append_result(out, count_io, jq_generated + start, jq_generated + jq_generated_used);
}

static int append_generated_null(JqSlice *out, size_t *count_io) {
    return append_generated_value(out, count_io, "null");
}

static int generated_append_json_escaped_content(const char *text, size_t length) {
    static const char hex[] = "0123456789abcdef";
    size_t i;

    for (i = 0U; i < length; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '"') {
            if (generated_append_cstr("\\\"") != 0) return -1;
        } else if (ch == '\\') {
            if (generated_append_cstr("\\\\") != 0) return -1;
        } else if (ch == '\b') {
            if (generated_append_cstr("\\b") != 0) return -1;
        } else if (ch == '\f') {
            if (generated_append_cstr("\\f") != 0) return -1;
        } else if (ch == '\n') {
            if (generated_append_cstr("\\n") != 0) return -1;
        } else if (ch == '\r') {
            if (generated_append_cstr("\\r") != 0) return -1;
        } else if (ch == '\t') {
            if (generated_append_cstr("\\t") != 0) return -1;
        } else if (ch < 0x20U) {
            char escape[6];
            escape[0] = '\\';
            escape[1] = 'u';
            escape[2] = '0';
            escape[3] = '0';
            escape[4] = hex[ch >> 4U];
            escape[5] = hex[ch & 0xfU];
            if (generated_append(escape, sizeof(escape)) != 0) return -1;
        } else if (generated_append((const char *)&text[i], 1U) != 0) {
            return -1;
        }
    }
    return 0;
}

static int generated_append_json_string_content_from_value(const char *start, const char *end) {
    const char *p = skip_ws(start);

    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
    if (p < end && *p == '"' && end[-1] == '"') {
        const char *limit = end - 1;
        p++;
        while (p < limit) {
            if (*p == '\\') {
                char decoded[5];
                size_t decoded_length = 0U;
                p++;
                if (p >= limit) return -1;
                if (*p == 'u') {
                    p++;
                    if (append_json_u_escape(decoded, sizeof(decoded), &decoded_length, &p) != 0) return -1;
                } else {
                    if (append_json_escape(decoded, sizeof(decoded), &decoded_length, *p) != 0) return -1;
                    p++;
                }
                if (generated_append_json_escaped_content(decoded, decoded_length) != 0) return -1;
            } else {
                if (generated_append_json_escaped_content(p, 1U) != 0) return -1;
                p++;
            }
        }
        return 0;
    }
    return generated_append_json_escaped_content(p, (size_t)(end - p));
}

static int append_generated_json_string_from_value(const char *start, const char *end, JqSlice *out, size_t *count_io) {
    size_t generated_start = jq_generated_used;

    if (generated_append_cstr("\"") != 0 || generated_append_json_string_content_from_value(start, end) != 0 || generated_append_cstr("\"") != 0) return -1;
    return append_result(out, count_io, jq_generated + generated_start, jq_generated + jq_generated_used);
}

static int append_generated_json_string_from_slice(const char *start, const char *end, JqSlice *out, size_t *count_io) {
    const char *p = skip_ws(start);
    size_t generated_start;

    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
    generated_start = jq_generated_used;
    if (generated_append_cstr("\"") != 0 || generated_append_json_escaped_content(p, (size_t)(end - p)) != 0 || generated_append_cstr("\"") != 0) return -1;
    return append_result(out, count_io, jq_generated + generated_start, jq_generated + jq_generated_used);
}

static int decode_json_string_to_buffer(const char *start, const char *end, char *buffer, size_t capacity, size_t *length_out) {
    const char *p = skip_ws(start);
    const char *limit = end;
    size_t length = 0U;

    while (limit > p && (limit[-1] == ' ' || limit[-1] == '\t' || limit[-1] == '\n' || limit[-1] == '\r')) limit--;
    if (p >= limit || *p != '"' || limit[-1] != '"') return -1;
    p++;
    limit--;
    while (p < limit) {
        if (*p == '\\') {
            p++;
            if (p >= limit) return -1;
            if (*p == 'u') {
                p++;
                if (append_json_u_escape(buffer, capacity, &length, &p) != 0) return -1;
            } else {
                if (append_json_escape(buffer, capacity, &length, *p) != 0) return -1;
                p++;
            }
        } else {
            if (length + 1U >= capacity) return -1;
            buffer[length++] = *p++;
        }
    }
    if (length >= capacity) return -1;
    buffer[length] = '\0';
    *length_out = length;
    return 0;
}

static int trim_filter(const char *filter, size_t length, const char **start_out, size_t *length_out) {
    const char *start = filter;

    while (length != 0U && (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')) {
        start++;
        length--;
    }
    while (length != 0U && (start[length - 1U] == ' ' || start[length - 1U] == '\t' || start[length - 1U] == '\n' || start[length - 1U] == '\r')) {
        length--;
    }
    if (length >= JQ_MAX_FILTER) return -1;
    *start_out = start;
    *length_out = length;
    return 0;
}

static int copy_filter_text(const char *filter, size_t length, char *out, size_t out_size) {
    const char *trimmed;
    size_t trimmed_length;

    if (trim_filter(filter, length, &trimmed, &trimmed_length) != 0 || trimmed_length + 1U > out_size) return -1;
    memcpy(out, trimmed, trimmed_length);
    out[trimmed_length] = '\0';
    return 0;
}

static int is_ident_start(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

static int is_ident_char(char ch) {
    return is_ident_start(ch) || (ch >= '0' && ch <= '9');
}

static int parse_json_int(const char *start, const char *end, long long *value_out) {
    const char *p = skip_ws(start);
    int negative = 0;
    unsigned long long value = 0ULL;

    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
    if (p < end && *p == '-') {
        negative = 1;
        p++;
    }
    if (p >= end || *p < '0' || *p > '9') return -1;
    while (p < end && *p >= '0' && *p <= '9') {
        unsigned int digit = (unsigned int)(*p - '0');
        if (value > (9223372036854775807ULL + (negative ? 1ULL : 0ULL) - digit) / 10ULL) return -1;
        value = value * 10ULL + digit;
        p++;
    }
    if (p != end) return -1;
    if (negative) {
        if (value == 9223372036854775808ULL) *value_out = (-9223372036854775807LL - 1LL);
        else *value_out = -(long long)value;
    } else {
        *value_out = (long long)value;
    }
    return 0;
}

static int parse_expression_int(const char *expression, long long *value_out) {
    return parse_json_int(expression, expression + rt_strlen(expression), value_out);
}

static int parse_path_int(const char *text, size_t start, size_t end, long long *value_out) {
    return parse_json_int(text + start, text + end, value_out);
}

static int is_json_literal_text(const char *expression) {
    const char *end;
    if (expression[0] == '"') {
        end = skip_string(expression);
        return end != 0 && *end == '\0';
    }
    if (rt_strcmp(expression, "true") == 0 || rt_strcmp(expression, "false") == 0 || rt_strcmp(expression, "null") == 0) return 1;
    if (expression[0] == '-' || (expression[0] >= '0' && expression[0] <= '9')) {
        long long value;
        return parse_expression_int(expression, &value) == 0;
    }
    return 0;
}

static int find_top_level_word_operator(const char *text, const char *word) {
    int bracket_depth = 0;
    int paren_depth = 0;
    int brace_depth = 0;
    size_t word_length = rt_strlen(word);
    size_t pos = 0U;

    while (text[pos] != '\0') {
        if (text[pos] == '"') {
            const char *end = skip_string(text + pos);
            if (end == 0) return -1;
            pos = (size_t)(end - text);
            continue;
        }
        if (text[pos] == '[') bracket_depth++;
        else if (text[pos] == ']') bracket_depth--;
        else if (text[pos] == '(') paren_depth++;
        else if (text[pos] == ')') paren_depth--;
        else if (text[pos] == '{') brace_depth++;
        else if (text[pos] == '}') brace_depth--;
        if (bracket_depth == 0 && paren_depth == 0 && brace_depth == 0 && rt_strncmp(text + pos, word, word_length) == 0 &&
            (pos == 0U || !is_ident_char(text[pos - 1U])) && !is_ident_char(text[pos + word_length])) {
            return (int)pos;
        }
        if (bracket_depth < 0 || paren_depth < 0 || brace_depth < 0) return -1;
        pos++;
    }
    return -1;
}

static int find_top_level_single_operator_from_right(const char *text, const char *ops) {
    int bracket_depth = 0;
    int paren_depth = 0;
    int brace_depth = 0;
    size_t length = rt_strlen(text);
    size_t pos = length;

    while (pos != 0U) {
        size_t op_index;
        pos--;
        if (text[pos] == '"') {
            size_t scan = pos;
            while (scan != 0U) {
                scan--;
                if (text[scan] == '"') {
                    size_t backslashes = 0U;
                    size_t b = scan;
                    while (b != 0U && text[b - 1U] == '\\') { backslashes++; b--; }
                    if ((backslashes & 1U) == 0U) { pos = scan; break; }
                }
            }
            continue;
        }
        if (text[pos] == ']') bracket_depth++;
        else if (text[pos] == '[') bracket_depth--;
        else if (text[pos] == ')') paren_depth++;
        else if (text[pos] == '(') paren_depth--;
        else if (text[pos] == '}') brace_depth++;
        else if (text[pos] == '{') brace_depth--;
        if (bracket_depth == 0 && paren_depth == 0 && brace_depth == 0) {
            for (op_index = 0U; ops[op_index] != '\0'; ++op_index) {
                if (text[pos] == ops[op_index] && !(text[pos] == '-' && (pos == 0U || text[pos - 1U] == '(' || text[pos - 1U] == '[' || text[pos - 1U] == '{' || text[pos - 1U] == ':' || text[pos - 1U] == ',' || text[pos - 1U] == '|'))) return (int)pos;
            }
        }
        if (bracket_depth < 0 || paren_depth < 0 || brace_depth < 0) return -1;
    }
    return -1;
}

static int find_top_level_operator(const char *text, char op) {
    int bracket_depth = 0;
    int paren_depth = 0;
    int brace_depth = 0;
    size_t pos = 0U;

    while (text[pos] != '\0') {
        if (text[pos] == '"') {
            const char *end = skip_string(text + pos);
            if (end == 0) return -1;
            pos = (size_t)(end - text);
            continue;
        }
        if (text[pos] == '[') bracket_depth++;
        else if (text[pos] == ']') bracket_depth--;
        else if (text[pos] == '(') paren_depth++;
        else if (text[pos] == ')') paren_depth--;
        else if (text[pos] == '{') brace_depth++;
        else if (text[pos] == '}') brace_depth--;
        else if (text[pos] == op && bracket_depth == 0 && paren_depth == 0 && brace_depth == 0) return (int)pos;
        if (bracket_depth < 0 || paren_depth < 0 || brace_depth < 0) return -1;
        pos++;
    }
    return -1;
}

static int find_top_level_two_char_operator(const char *text, char first, char second) {
    int bracket_depth = 0;
    int paren_depth = 0;
    int brace_depth = 0;
    size_t pos = 0U;

    while (text[pos] != '\0') {
        if (text[pos] == '"') {
            const char *end = skip_string(text + pos);
            if (end == 0) return -1;
            pos = (size_t)(end - text);
            continue;
        }
        if (text[pos] == '[') bracket_depth++;
        else if (text[pos] == ']') bracket_depth--;
        else if (text[pos] == '(') paren_depth++;
        else if (text[pos] == ')') paren_depth--;
        else if (text[pos] == '{') brace_depth++;
        else if (text[pos] == '}') brace_depth--;
        else if (text[pos] == first && text[pos + 1U] == second && bracket_depth == 0 && paren_depth == 0 && brace_depth == 0) return (int)pos;
        if (bracket_depth < 0 || paren_depth < 0 || brace_depth < 0) return -1;
        pos++;
    }
    return -1;
}

static int find_array_signed_index(const char *array, long long target_index, const char **value_start, const char **value_end) {
    size_t count;

    if (target_index < 0) {
        unsigned long long magnitude = (unsigned long long)(-(target_index + 1LL)) + 1ULL;
        if (json_array_count(array, &count) != 0 || magnitude > (unsigned long long)count) return -1;
        return find_array_index(array, count - (size_t)magnitude, value_start, value_end);
    }
    return find_array_index(array, (size_t)target_index, value_start, value_end);
}

static int append_array_slice_value(const char *array, long long start_index, int has_start, long long end_index, int has_end, JqSlice *out, size_t *count_io) {
    const char *p = skip_ws(array);
    size_t count;
    size_t start_pos;
    size_t end_pos;
    size_t index = 0U;
    size_t generated_start;
    int first = 1;

    if (*p != '[' || json_array_count(p, &count) != 0) return -1;
    if (has_start) {
        if (start_index < 0) {
            unsigned long long magnitude = (unsigned long long)(-(start_index + 1LL)) + 1ULL;
            start_pos = magnitude > (unsigned long long)count ? 0U : count - (size_t)magnitude;
        } else {
            start_pos = (size_t)start_index;
            if (start_pos > count) start_pos = count;
        }
    } else {
        start_pos = 0U;
    }
    if (has_end) {
        if (end_index < 0) {
            unsigned long long magnitude = (unsigned long long)(-(end_index + 1LL)) + 1ULL;
            end_pos = magnitude > (unsigned long long)count ? 0U : count - (size_t)magnitude;
        } else {
            end_pos = (size_t)end_index;
            if (end_pos > count) end_pos = count;
        }
    } else {
        end_pos = count;
    }
    if (end_pos < start_pos) end_pos = start_pos;
    generated_start = jq_generated_used;
    if (generated_append_cstr("[") != 0) return -1;
    p++;
    for (;;) {
        const char *value_start;
        const char *value_end;

        p = skip_ws(p);
        if (*p == ']') break;
        value_start = p;
        value_end = skip_value(value_start);
        if (value_end == 0) return -1;
        if (index >= start_pos && index < end_pos) {
            if (!first && generated_append_cstr(",") != 0) return -1;
            if (generated_append(value_start, (size_t)(value_end - value_start)) != 0) return -1;
            first = 0;
        }
        index++;
        p = skip_ws(value_end);
        if (*p == ',') p++;
        else if (*p == ']') break;
        else return -1;
    }
    if (generated_append_cstr("]") != 0) return -1;
    return append_result(out, count_io, jq_generated + generated_start, jq_generated + jq_generated_used);
}

static int find_top_level_comparison(const char *text, const char **op_out) {
    int bracket_depth = 0;
    int paren_depth = 0;
    int brace_depth = 0;
    size_t pos = 0U;

    while (text[pos] != '\0') {
        if (text[pos] == '"') {
            const char *end = skip_string(text + pos);
            if (end == 0) return -1;
            pos = (size_t)(end - text);
            continue;
        }
        if (text[pos] == '[') bracket_depth++;
        else if (text[pos] == ']') bracket_depth--;
        else if (text[pos] == '(') paren_depth++;
        else if (text[pos] == ')') paren_depth--;
        else if (text[pos] == '{') brace_depth++;
        else if (text[pos] == '}') brace_depth--;
        else if (bracket_depth == 0 && paren_depth == 0 && brace_depth == 0 && ((text[pos] == '=' && text[pos + 1U] == '=') || (text[pos] == '!' && text[pos + 1U] == '=') || (text[pos] == '<' && text[pos + 1U] == '=') || (text[pos] == '>' && text[pos + 1U] == '=') || text[pos] == '<' || text[pos] == '>')) {
            *op_out = text + pos;
            return (int)pos;
        }
        if (bracket_depth < 0 || paren_depth < 0 || brace_depth < 0) return -1;
        pos++;
    }
    return -1;
}

static int parse_function_arg(const char *expression, const char *name, const char **arg_out, size_t *arg_length_out) {
    size_t name_length = rt_strlen(name);
    size_t pos;
    int depth = 1;

    if (rt_strncmp(expression, name, name_length) != 0 || expression[name_length] != '(') return 0;
    pos = name_length + 1U;
    *arg_out = expression + pos;
    while (expression[pos] != '\0') {
        if (expression[pos] == '"') {
            const char *end = skip_string(expression + pos);
            if (end == 0) return -1;
            pos = (size_t)(end - expression);
            continue;
        }
        if (expression[pos] == '(') depth++;
        else if (expression[pos] == ')') {
            depth--;
            if (depth == 0) {
                if (expression[pos + 1U] != '\0') return -1;
                *arg_length_out = (size_t)(expression + pos - *arg_out);
                return 1;
            }
        }
        pos++;
    }
    return -1;
}

static JqVariable *find_variable(const char *name) {
    size_t i;

    for (i = jq_variable_count; i != 0U; --i) {
        if (rt_strcmp(jq_variables[i - 1U].name, name) == 0) return &jq_variables[i - 1U];
    }
    return 0;
}

static int push_variable(const char *name, const JqSlice *value) {
    if (jq_variable_count >= JQ_MAX_VARIABLES || rt_strlen(name) >= JQ_NAME_CAPACITY) return -1;
    rt_copy_string(jq_variables[jq_variable_count].name, sizeof(jq_variables[jq_variable_count].name), name);
    jq_variables[jq_variable_count].value = *value;
    jq_variable_count++;
    return 0;
}

static JqFunction *find_function(const char *name) {
    size_t i;

    for (i = 0U; i < jq_function_count; ++i) {
        if (rt_strcmp(jq_functions[i].name, name) == 0) return &jq_functions[i];
    }
    return 0;
}

static int split_top_level_args(const char *text, const char *starts[], size_t lengths[], size_t max_args, size_t *count_out) {
    int bracket_depth = 0;
    int paren_depth = 0;
    int brace_depth = 0;
    const char *segment = text;
    size_t count = 0U;
    size_t pos = 0U;

    if (text[0] == '\0') {
        *count_out = 0U;
        return 0;
    }
    while (1) {
        if (text[pos] == '"') {
            const char *end = skip_string(text + pos);
            if (end == 0) return -1;
            pos = (size_t)(end - text);
            continue;
        }
        if (text[pos] == '[') bracket_depth++;
        else if (text[pos] == ']') bracket_depth--;
        else if (text[pos] == '(') paren_depth++;
        else if (text[pos] == ')') paren_depth--;
        else if (text[pos] == '{') brace_depth++;
        else if (text[pos] == '}') brace_depth--;
        if ((text[pos] == ';' || text[pos] == ',' || text[pos] == '\0') && bracket_depth == 0 && paren_depth == 0 && brace_depth == 0) {
            const char *trimmed;
            size_t trimmed_length;
            if (count >= max_args || trim_filter(segment, (size_t)(text + pos - segment), &trimmed, &trimmed_length) != 0) return -1;
            starts[count] = trimmed;
            lengths[count] = trimmed_length;
            count++;
            if (text[pos] == '\0') break;
            segment = text + pos + 1U;
        }
        if (bracket_depth < 0 || paren_depth < 0 || brace_depth < 0) return -1;
        pos++;
    }
    *count_out = count;
    return 0;
}

static int parse_call_expression(const char *expression, char *name, size_t name_capacity, const char **arg_text_out, size_t *arg_length_out) {
    size_t pos = 0U;
    int depth = 1;
    const char *arg_start;

    if (!is_ident_start(expression[0])) return 0;
    while (is_ident_char(expression[pos])) {
        if (pos + 1U >= name_capacity) return -1;
        name[pos] = expression[pos];
        pos++;
    }
    name[pos] = '\0';
    if (expression[pos] != '(') return 0;
    pos++;
    arg_start = expression + pos;
    while (expression[pos] != '\0') {
        if (expression[pos] == '"') {
            const char *end = skip_string(expression + pos);
            if (end == 0) return -1;
            pos = (size_t)(end - expression);
            continue;
        }
        if (expression[pos] == '(') depth++;
        else if (expression[pos] == ')') {
            depth--;
            if (depth == 0) {
                if (expression[pos + 1U] != '\0') return -1;
                *arg_text_out = arg_start;
                *arg_length_out = (size_t)(expression + pos - arg_start);
                return 1;
            }
        }
        pos++;
    }
    return -1;
}

static int json_value_type(const char *start, const char *end, const char **type_out) {
    const char *p = skip_ws(start);
    (void)end;

    if (*p == '{') *type_out = "object";
    else if (*p == '[') *type_out = "array";
    else if (*p == '"') *type_out = "string";
    else if (*p == 't' || *p == 'f') *type_out = "boolean";
    else if (*p == 'n') *type_out = "null";
    else if ((*p >= '0' && *p <= '9') || *p == '-') *type_out = "number";
    else return -1;
    return 0;
}

static int json_truthy(const char *start, const char *end) {
    const char *p = skip_ws(start);
    (void)end;

    if (rt_strncmp(p, "false", 5U) == 0) return 0;
    if (rt_strncmp(p, "null", 4U) == 0) return 0;
    return 1;
}

static int json_array_count(const char *array, size_t *count_out) {
    const char *p = skip_ws(array);
    size_t count = 0U;

    if (*p != '[') return -1;
    p++;
    for (;;) {
        const char *end;

        p = skip_ws(p);
        if (*p == ']') {
            *count_out = count;
            return 0;
        }
        end = skip_value(p);
        if (end == 0) return -1;
        count++;
        p = skip_ws(end);
        if (*p == ',') p++;
        else if (*p != ']') return -1;
    }
}

static int json_object_count(const char *object, size_t *count_out) {
    const char *p = skip_ws(object);
    size_t count = 0U;

    if (*p != '{') return -1;
    p++;
    for (;;) {
        const char *end;

        p = skip_ws(p);
        if (*p == '}') {
            *count_out = count;
            return 0;
        }
        if (*p != '"') return -1;
        p = skip_string(p);
        if (p == 0) return -1;
        p = skip_ws(p);
        if (*p != ':') return -1;
        end = skip_value(p + 1);
        if (end == 0) return -1;
        count++;
        p = skip_ws(end);
        if (*p == ',') p++;
        else if (*p != '}') return -1;
    }
}

static int json_string_length(const char *string, const char *end, size_t *length_out) {
    const char *p = string + 1;
    const char *limit = end - 1;
    size_t count = 0U;

    if (*string != '"' || end <= string || end[-1] != '"') return -1;
    while (p < limit) {
        if (*p == '\\') {
            p++;
            if (p >= limit) return -1;
            if (*p == 'u') {
                unsigned int codepoint;
                p++;
                if (parse_hex4(p, &codepoint) != 0) return -1;
                p += 4;
                if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                    unsigned int low;
                    if (p + 6 > limit || p[0] != '\\' || p[1] != 'u' || parse_hex4(p + 2, &low) != 0 || low < 0xdc00U || low > 0xdfffU) return -1;
                    p += 6;
                } else if (codepoint >= 0xdc00U && codepoint <= 0xdfffU) {
                    return -1;
                }
            } else {
                switch (*p) {
                    case '"':
                    case '\\':
                    case '/':
                    case 'b':
                    case 'f':
                    case 'n':
                    case 'r':
                    case 't':
                        p++;
                        break;
                    default:
                        return -1;
                }
            }
        } else {
            size_t index = 0U;
            unsigned int codepoint;
            if (rt_utf8_decode(p, (size_t)(limit - p), &index, &codepoint) != 0) return -1;
            (void)codepoint;
            p += index;
        }
        count++;
    }
    *length_out = count;
    return 0;
}

static int slice_equals_text(const JqSlice *slice, const char *text, size_t length) {
    const char *start = skip_ws(slice->start);
    const char *end = slice->end;

    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
    return (size_t)(end - start) == length && memcmp(start, text, length) == 0;
}

static int emit_results(const JqSlice *results, size_t count) {
    size_t i;

    for (i = 0U; i < count; ++i) {
        if (emit_value(results[i].start, results[i].end) != 0) return -1;
    }
    return 0;
}

static int eval_filter_text(const char *filter, size_t filter_length, const char *current_start, const char *current_end, JqSlice *out, size_t *count_io);

static int eval_path_collect(const char *filter, size_t pos, const char *current_start, const char *current_end, JqSlice *out, size_t *count_io) {
    if (filter[pos] == '\0') {
        return append_result(out, count_io, current_start, current_end);
    }
    if (filter[pos] == '.') {
        char key[128];
        size_t len = 0U;
        const char *next_start;
        const char *next_end;
        int optional = 0;

        pos++;
        if (filter[pos] == '\0') return -1;
        if (filter[pos] == '[') return eval_path_collect(filter, pos, current_start, current_end, out, count_io);
        if (filter[pos] == '"') {
            if (parse_filter_string_key(filter, &pos, key, sizeof(key)) != 0) return -1;
        } else {
            while (filter[pos] != '\0' && filter[pos] != '.' && filter[pos] != '[' && filter[pos] != '?') {
                if (len + 1U >= sizeof(key)) return -1;
                key[len++] = filter[pos++];
            }
            key[len] = '\0';
        }
        if (filter[pos] == '?') {
            optional = 1;
            pos++;
        }
        if (key[0] == '\0') return -1;
        if (find_key(current_start, key, &next_start, &next_end) != 0) return optional ? append_generated_null(out, count_io) : -1;
        return eval_path_collect(filter, pos, next_start, next_end, out, count_io);
    }
    if (filter[pos] == '[') {
        const char *next_start;
        const char *next_end;

        pos++;
        if (filter[pos] == ']') {
            const char *p = skip_ws(current_start);
            int optional = 0;

            if (filter[pos + 1U] == '?') optional = 1;
            if (*p != '[') return optional ? append_generated_null(out, count_io) : -1;
            p++;
            pos++;
            if (optional) pos++;
            for (;;) {
                const char *end;

                p = skip_ws(p);
                if (*p == ']') return 0;
                end = skip_value(p);
                if (end == 0) return -1;
                if (eval_path_collect(filter, pos, p, end, out, count_io) != 0) return -1;
                p = skip_ws(end);
                if (*p == ',') {
                    p++;
                } else if (*p == ']') {
                    return 0;
                } else {
                    return -1;
                }
            }
        }
        if (filter[pos] == '"') {
            char key[128];
            int optional = 0;

            if (parse_filter_string_key(filter, &pos, key, sizeof(key)) != 0 || filter[pos] != ']') return -1;
            pos++;
            if (filter[pos] == '?') {
                optional = 1;
                pos++;
            }
            if (find_key(current_start, key, &next_start, &next_end) != 0) return optional ? append_generated_null(out, count_io) : -1;
            return eval_path_collect(filter, pos, next_start, next_end, out, count_io);
        } else {
            size_t number_start = pos;
            long long index;
            int optional = 0;

            while (filter[pos] == ' ' || filter[pos] == '\t' || filter[pos] == '\n' || filter[pos] == '\r') pos++;
            number_start = pos;
            while (filter[pos] != '\0' && filter[pos] != ']' && filter[pos] != ':') pos++;
            if (filter[pos] == ':') {
                long long start_index = 0;
                long long end_index = 0;
                int has_start = pos > number_start;
                int has_end;
                JqSlice slice[1];
                size_t slice_count = 0U;

                if (has_start && parse_path_int(filter, number_start, pos, &start_index) != 0) return -1;
                pos++;
                number_start = pos;
                while (filter[pos] != '\0' && filter[pos] != ']') pos++;
                if (filter[pos] != ']') return -1;
                has_end = pos > number_start;
                if (has_end && parse_path_int(filter, number_start, pos, &end_index) != 0) return -1;
                pos++;
                if (filter[pos] == '?') {
                    optional = 1;
                    pos++;
                }
                if (append_array_slice_value(current_start, start_index, has_start, end_index, has_end, slice, &slice_count) != 0 || slice_count != 1U) return optional ? append_generated_null(out, count_io) : -1;
                if (filter[pos] == '\0') return append_result(out, count_io, slice[0].start, slice[0].end);
                return eval_path_collect(filter, pos, slice[0].start, slice[0].end, out, count_io);
            }

            if (filter[pos] != ']') return -1;
            if (parse_path_int(filter, number_start, pos, &index) != 0) return -1;
            pos++;
            if (filter[pos] == '?') {
                optional = 1;
                pos++;
            }
            if (find_array_signed_index(current_start, index, &next_start, &next_end) != 0) return optional ? append_generated_null(out, count_io) : -1;
            return eval_path_collect(filter, pos, next_start, next_end, out, count_io);
        }
    }
    return -1;
}

static int collect_descendants(const char *start, const char *end, JqSlice *out, size_t *count_io) {
    const char *p = skip_ws(start);

    if (append_result(out, count_io, start, end) != 0) return -1;
    if (*p == '[') {
        p++;
        for (;;) {
            const char *child_end;

            p = skip_ws(p);
            if (*p == ']') return 0;
            child_end = skip_value(p);
            if (child_end == 0 || collect_descendants(p, child_end, out, count_io) != 0) return -1;
            p = skip_ws(child_end);
            if (*p == ',') p++;
            else if (*p == ']') return 0;
            else return -1;
        }
    }
    if (*p == '{') {
        p++;
        for (;;) {
            const char *child_end;

            p = skip_ws(p);
            if (*p == '}') return 0;
            if (*p != '"') return -1;
            p = skip_string(p);
            if (p == 0) return -1;
            p = skip_ws(p);
            if (*p != ':') return -1;
            p = skip_ws(p + 1);
            child_end = skip_value(p);
            if (child_end == 0 || collect_descendants(p, child_end, out, count_io) != 0) return -1;
            p = skip_ws(child_end);
            if (*p == ',') p++;
            else if (*p == '}') return 0;
            else return -1;
        }
    }
    return 0;
}

static int append_type_result(const char *start, const char *end, JqSlice *out, size_t *count_io) {
    const char *type;
    size_t generated_start;

    if (json_value_type(start, end, &type) != 0) return -1;
    generated_start = jq_generated_used;
    if (generated_append_cstr("\"") != 0 || generated_append_cstr(type) != 0 || generated_append_cstr("\"") != 0) return -1;
    return append_result(out, count_io, jq_generated + generated_start, jq_generated + jq_generated_used);
}

static int append_length_result(const char *start, const char *end, JqSlice *out, size_t *count_io) {
    const char *p = skip_ws(start);
    size_t count;

    if (*p == '[') {
        if (json_array_count(p, &count) != 0) return -1;
    } else if (*p == '{') {
        if (json_object_count(p, &count) != 0) return -1;
    } else if (*p == '"') {
        if (json_string_length(p, end, &count) != 0) return -1;
    } else {
        return -1;
    }
    return append_generated_uint_value(out, count_io, count);
}

static int append_keys_result(const char *start, const char *end, JqSlice *out, size_t *count_io) {
    const char *p = skip_ws(start);
    size_t generated_start = jq_generated_used;
    int first = 1;
    (void)end;

    if (generated_append_cstr("[") != 0) return -1;
    if (*p == '[') {
        size_t count;
        size_t i;

        if (json_array_count(p, &count) != 0) return -1;
        for (i = 0U; i < count; ++i) {
            if (!first && generated_append_cstr(",") != 0) return -1;
            if (generated_append_uint(i) != 0) return -1;
            first = 0;
        }
    } else if (*p == '{') {
        p++;
        for (;;) {
            const char *key_start;
            const char *key_end;
            const char *value_end;

            p = skip_ws(p);
            if (*p == '}') break;
            if (*p != '"') return -1;
            key_start = p;
            key_end = skip_string(p);
            if (key_end == 0) return -1;
            if (!first && generated_append_cstr(",") != 0) return -1;
            if (generated_append(key_start, (size_t)(key_end - key_start)) != 0) return -1;
            first = 0;
            p = skip_ws(key_end);
            if (*p != ':') return -1;
            value_end = skip_value(p + 1);
            if (value_end == 0) return -1;
            p = skip_ws(value_end);
            if (*p == ',') p++;
            else if (*p != '}') return -1;
        }
    } else {
        return -1;
    }
    if (generated_append_cstr("]") != 0) return -1;
    return append_result(out, count_io, jq_generated + generated_start, jq_generated + jq_generated_used);
}

static int parse_has_arg(const char *arg, size_t arg_length, char *key, size_t key_capacity, size_t *index_out, int *is_string_out) {
    char text[JQ_MAX_FILTER];
    size_t pos = 0U;
    size_t index = 0U;

    if (copy_filter_text(arg, arg_length, text, sizeof(text)) != 0 || text[0] == '\0') return -1;
    if (text[0] == '"') {
        if (parse_filter_string_key(text, &pos, key, key_capacity) != 0 || text[pos] != '\0') return -1;
        *is_string_out = 1;
        return 0;
    }
    if (text[0] < '0' || text[0] > '9') return -1;
    while (text[pos] >= '0' && text[pos] <= '9') {
        size_t digit = (size_t)(text[pos] - '0');
        if (index > (((size_t)-1) - digit) / 10U) return -1;
        index = index * 10U + digit;
        pos++;
    }
    if (text[pos] != '\0') return -1;
    *index_out = index;
    *is_string_out = 0;
    return 0;
}

static int append_has_result(const char *start, const char *end, const char *arg, size_t arg_length, JqSlice *out, size_t *count_io) {
    char key[128];
    size_t index = 0U;
    int is_string = 0;
    int has_value = 0;
    const char *value_start;
    const char *value_end;
    const char *p = skip_ws(start);

    (void)end;
    if (parse_has_arg(arg, arg_length, key, sizeof(key), &index, &is_string) != 0) return -1;
    if (is_string && *p == '{') {
        has_value = find_key(p, key, &value_start, &value_end) == 0;
    } else if (!is_string && *p == '[') {
        has_value = find_array_index(p, index, &value_start, &value_end) == 0;
    }
    return append_generated_value(out, count_io, has_value ? "true" : "false");
}

static int append_map_result(const char *start, const char *end, const char *arg, size_t arg_length, JqSlice *out, size_t *count_io) {
    const char *p = skip_ws(start);
    JqSlice collected[JQ_MAX_RESULTS];
    size_t collected_count = 0U;
    size_t generated_start;
    size_t i;
    (void)end;

    if (*p != '[') return -1;
    p++;
    for (;;) {
        const char *element_end;
        JqSlice mapped[JQ_MAX_RESULTS];
        size_t mapped_count = 0U;
        size_t j;

        p = skip_ws(p);
        if (*p == ']') break;
        element_end = skip_value(p);
        if (element_end == 0) return -1;
        if (eval_filter_text(arg, arg_length, p, element_end, mapped, &mapped_count) != 0) return -1;
        for (j = 0U; j < mapped_count; ++j) {
            if (collected_count >= JQ_MAX_RESULTS) return -1;
            collected[collected_count++] = mapped[j];
        }
        p = skip_ws(element_end);
        if (*p == ',') p++;
        else if (*p != ']') return -1;
    }
    generated_start = jq_generated_used;
    if (generated_append_cstr("[") != 0) return -1;
    for (i = 0U; i < collected_count; ++i) {
        if (i != 0U && generated_append_cstr(",") != 0) return -1;
        if (generated_append(collected[i].start, (size_t)(collected[i].end - collected[i].start)) != 0) return -1;
    }
    if (generated_append_cstr("]") != 0) return -1;
    return append_result(out, count_io, jq_generated + generated_start, jq_generated + jq_generated_used);
}

static int append_select_result(const char *start, const char *end, const char *arg, size_t arg_length, JqSlice *out, size_t *count_io) {
    JqSlice selected[JQ_MAX_RESULTS];
    size_t selected_count = 0U;
    size_t i;

    if (eval_filter_text(arg, arg_length, start, end, selected, &selected_count) != 0) return -1;
    for (i = 0U; i < selected_count; ++i) {
        if (json_truthy(selected[i].start, selected[i].end)) return append_result(out, count_io, start, end);
    }
    return 0;
}

static int eval_comparison(const char *expression, int op_pos, const char *op, const char *start, const char *end, JqSlice *out, size_t *count_io) {
    JqSlice left[JQ_MAX_RESULTS];
    JqSlice right_values[JQ_MAX_RESULTS];
    size_t left_count = 0U;
    size_t right_count = 0U;
    size_t i, j;
    int matched = 0;

    if (eval_filter_text(expression, (size_t)op_pos, start, end, left, &left_count) != 0) return -1;
    if (eval_filter_text(expression + op_pos + ((op[1] == '=') ? 2 : 1), rt_strlen(expression + op_pos + ((op[1] == '=') ? 2 : 1)), start, end, right_values, &right_count) != 0) return -1;
    if (left_count == 0U || right_count == 0U) return append_generated_value(out, count_io, "false");
    for (i = 0U; i < left_count; ++i) {
        for (j = 0U; j < right_count; ++j) {
            if (op[0] == '=' || op[0] == '!') {
                if (slice_equals_text(&left[i], right_values[j].start, (size_t)(right_values[j].end - right_values[j].start))) matched = 1;
            } else {
                long long left_number;
                long long right_number;
                if (parse_json_int(left[i].start, left[i].end, &left_number) != 0 || parse_json_int(right_values[j].start, right_values[j].end, &right_number) != 0) return -1;
                if ((op[0] == '<' && op[1] == '=' && left_number <= right_number) ||
                    (op[0] == '>' && op[1] == '=' && left_number >= right_number) ||
                    (op[0] == '<' && op[1] != '=' && left_number < right_number) ||
                    (op[0] == '>' && op[1] != '=' && left_number > right_number)) matched = 1;
            }
            if (matched) break;
        }
        if (matched) break;
    }
    if (op[0] == '!') matched = !matched;
    return append_generated_value(out, count_io, matched ? "true" : "false");
}

static int eval_filter_single(const char *filter, size_t filter_length, const char *start, const char *end, JqSlice *value_out) {
    JqSlice values[JQ_MAX_RESULTS];
    size_t count = 0U;

    if (eval_filter_text(filter, filter_length, start, end, values, &count) != 0 || count != 1U) return -1;
    *value_out = values[0];
    return 0;
}

static int append_value_text_to_buffer(const char *start, const char *end, char *buffer, size_t capacity, size_t *length_io);

static int append_interpolated_string(const char *expression, const char *start, const char *end, JqSlice *out, size_t *count_io) {
    const char *p = expression + 1;
    const char *limit = expression + rt_strlen(expression) - 1U;
    char text[JQ_MAX_FILTER];
    size_t text_length = 0U;
    size_t generated_start;

    text[0] = '\0';
    while (p < limit) {
        if (*p == '\\' && p + 1 < limit && p[1] == '(') {
            const char *inner_start = p + 2;
            const char *scan = inner_start;
            int depth = 1;
            JqSlice value;

            while (scan < limit) {
                if (*scan == '"') {
                    scan = skip_string(scan);
                    if (scan == 0 || scan > limit) return -1;
                    continue;
                }
                if (*scan == '(') depth++;
                else if (*scan == ')') {
                    depth--;
                    if (depth == 0) break;
                }
                scan++;
            }
            if (scan >= limit || eval_filter_single(inner_start, (size_t)(scan - inner_start), start, end, &value) != 0) return -1;
            if (append_value_text_to_buffer(value.start, value.end, text, sizeof(text), &text_length) != 0) return -1;
            p = scan + 1;
        } else if (*p == '\\') {
            char decoded[5];
            size_t decoded_length = 0U;
            p++;
            if (p >= limit) return -1;
            if (*p == 'u') {
                p++;
                if (append_json_u_escape(decoded, sizeof(decoded), &decoded_length, &p) != 0) return -1;
            } else {
                if (append_json_escape(decoded, sizeof(decoded), &decoded_length, *p) != 0) return -1;
                p++;
            }
            if (text_length + decoded_length >= sizeof(text)) return -1;
            memcpy(text + text_length, decoded, decoded_length);
            text_length += decoded_length;
            text[text_length] = '\0';
        } else {
            if (text_length + 1U >= sizeof(text)) return -1;
            text[text_length++] = *p;
            text[text_length] = '\0';
            p++;
        }
    }
    generated_start = jq_generated_used;
    if (generated_append_cstr("\"") != 0 || generated_append_json_escaped_content(text, text_length) != 0 || generated_append_cstr("\"") != 0) return -1;
    return append_result(out, count_io, jq_generated + generated_start, jq_generated + jq_generated_used);
}

static int string_has_interpolation(const char *expression) {
    const char *p = expression;
    const char *end = skip_string(expression);

    if (end == 0 || *end != '\0') return 0;
    while (*p != '\0') {
        if (p[0] == '\\' && p[1] == '(') return 1;
        p++;
    }
    return 0;
}

static int value_to_text_buffer(const char *start, const char *end, char *buffer, size_t capacity, size_t *length_out) {
    const char *p = skip_ws(start);

    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) end--;
    if (p < end && *p == '"' && end[-1] == '"') return decode_json_string_to_buffer(p, end, buffer, capacity, length_out);
    if ((size_t)(end - p) >= capacity) return -1;
    memcpy(buffer, p, (size_t)(end - p));
    buffer[end - p] = '\0';
    *length_out = (size_t)(end - p);
    return 0;
}

static int append_value_text_to_buffer(const char *start, const char *end, char *buffer, size_t capacity, size_t *length_io) {
    char text[JQ_MAX_FILTER];
    size_t text_length;

    if (value_to_text_buffer(start, end, text, sizeof(text), &text_length) != 0 || *length_io + text_length >= capacity) return -1;
    memcpy(buffer + *length_io, text, text_length);
    *length_io += text_length;
    buffer[*length_io] = '\0';
    return 0;
}

static int append_string_predicate_result(const char *start, const char *end, const char *arg, size_t arg_length, const char *name, JqSlice *out, size_t *count_io) {
    char text[JQ_MAX_FILTER];
    char needle[JQ_MAX_FILTER];
    size_t text_length;
    size_t needle_length;
    JqSlice arg_value;
    int matched = 0;

    if (decode_json_string_to_buffer(start, end, text, sizeof(text), &text_length) != 0 || eval_filter_single(arg, arg_length, start, end, &arg_value) != 0 || value_to_text_buffer(arg_value.start, arg_value.end, needle, sizeof(needle), &needle_length) != 0) return -1;
    if (rt_strcmp(name, "contains") == 0) {
        size_t i;
        if (needle_length == 0U) matched = 1;
        for (i = 0U; !matched && i + needle_length <= text_length; ++i) {
            if (memcmp(text + i, needle, needle_length) == 0) matched = 1;
        }
    } else if (rt_strcmp(name, "startswith") == 0) {
        matched = needle_length <= text_length && memcmp(text, needle, needle_length) == 0;
    } else if (rt_strcmp(name, "endswith") == 0) {
        matched = needle_length <= text_length && memcmp(text + text_length - needle_length, needle, needle_length) == 0;
    } else return -1;
    return append_generated_value(out, count_io, matched ? "true" : "false");
}

static int append_regex_test_result(const char *start, const char *end, const char *arg, size_t arg_length, JqSlice *out, size_t *count_io) {
    char text[JQ_MAX_FILTER];
    char pattern[JQ_MAX_FILTER];
    size_t text_length;
    size_t pattern_length;
    size_t match_start = 0U;
    size_t match_end = 0U;
    JqSlice arg_value;
    int matched;

    if (decode_json_string_to_buffer(start, end, text, sizeof(text), &text_length) != 0 || eval_filter_single(arg, arg_length, start, end, &arg_value) != 0 || value_to_text_buffer(arg_value.start, arg_value.end, pattern, sizeof(pattern), &pattern_length) != 0) return -1;
    (void)text_length;
    (void)pattern_length;
    matched = tool_regex_search(pattern, text, 0, 0U, &match_start, &match_end);
    return append_generated_value(out, count_io, matched ? "true" : "false");
}

static int append_split_result(const char *start, const char *end, const char *arg, size_t arg_length, JqSlice *out, size_t *count_io) {
    char text[JQ_MAX_FILTER];
    char separator[JQ_MAX_FILTER];
    size_t text_length;
    size_t separator_length;
    size_t pos = 0U;
    size_t generated_start;
    int first = 1;
    JqSlice arg_value;

    if (decode_json_string_to_buffer(start, end, text, sizeof(text), &text_length) != 0 || eval_filter_single(arg, arg_length, start, end, &arg_value) != 0 || value_to_text_buffer(arg_value.start, arg_value.end, separator, sizeof(separator), &separator_length) != 0 || separator_length == 0U) return -1;
    generated_start = jq_generated_used;
    if (generated_append_cstr("[") != 0) return -1;
    while (pos <= text_length) {
        size_t next = pos;
        while (next + separator_length <= text_length && memcmp(text + next, separator, separator_length) != 0) next++;
        if (!first && generated_append_cstr(",") != 0) return -1;
        if (generated_append_cstr("\"") != 0 || generated_append_json_escaped_content(text + pos, next - pos) != 0 || generated_append_cstr("\"") != 0) return -1;
        first = 0;
        if (next + separator_length > text_length) break;
        pos = next + separator_length;
    }
    if (generated_append_cstr("]") != 0) return -1;
    return append_result(out, count_io, jq_generated + generated_start, jq_generated + jq_generated_used);
}

static int append_join_result(const char *start, const char *end, const char *arg, size_t arg_length, JqSlice *out, size_t *count_io) {
    char separator[JQ_MAX_FILTER];
    size_t separator_length;
    const char *p = skip_ws(start);
    size_t generated_start;
    int first = 1;
    JqSlice arg_value;

    if (*p != '[' || eval_filter_single(arg, arg_length, start, end, &arg_value) != 0 || value_to_text_buffer(arg_value.start, arg_value.end, separator, sizeof(separator), &separator_length) != 0) return -1;
    generated_start = jq_generated_used;
    if (generated_append_cstr("\"") != 0) return -1;
    p++;
    for (;;) {
        const char *value_start;
        const char *value_end;

        p = skip_ws(p);
        if (*p == ']') break;
        value_start = p;
        value_end = skip_value(value_start);
        if (value_end == 0) return -1;
        if (!first && generated_append_json_escaped_content(separator, separator_length) != 0) return -1;
        if (generated_append_json_string_content_from_value(value_start, value_end) != 0) return -1;
        first = 0;
        p = skip_ws(value_end);
        if (*p == ',') p++;
        else if (*p == ']') break;
        else return -1;
    }
    if (generated_append_cstr("\"") != 0) return -1;
    return append_result(out, count_io, jq_generated + generated_start, jq_generated + jq_generated_used);
}

static int append_ascii_case_result(const char *start, const char *end, int uppercase, JqSlice *out, size_t *count_io) {
    char text[JQ_MAX_FILTER];
    size_t text_length;
    size_t i;
    size_t generated_start;

    if (decode_json_string_to_buffer(start, end, text, sizeof(text), &text_length) != 0) return -1;
    for (i = 0U; i < text_length; ++i) {
        if (uppercase && text[i] >= 'a' && text[i] <= 'z') text[i] = (char)(text[i] - 'a' + 'A');
        else if (!uppercase && text[i] >= 'A' && text[i] <= 'Z') text[i] = (char)(text[i] - 'A' + 'a');
    }
    generated_start = jq_generated_used;
    if (generated_append_cstr("\"") != 0 || generated_append_json_escaped_content(text, text_length) != 0 || generated_append_cstr("\"") != 0) return -1;
    return append_result(out, count_io, jq_generated + generated_start, jq_generated + jq_generated_used);
}

static int eval_numeric_binary(const char *expression, int op_pos, char op, const char *start, const char *end, JqSlice *out, size_t *count_io) {
    JqSlice left;
    JqSlice right;
    long long left_number;
    long long right_number;
    long long result;

    if (eval_filter_single(expression, (size_t)op_pos, start, end, &left) != 0 ||
        eval_filter_single(expression + op_pos + 1, rt_strlen(expression + op_pos + 1), start, end, &right) != 0) return -1;
    if (op == '+') {
        const char *left_start = skip_ws(left.start);
        const char *left_end = left.end;
        const char *right_start = skip_ws(right.start);
        const char *right_end = right.end;

        while (left_end > left_start && (left_end[-1] == ' ' || left_end[-1] == '\t' || left_end[-1] == '\n' || left_end[-1] == '\r')) left_end--;
        while (right_end > right_start && (right_end[-1] == ' ' || right_end[-1] == '\t' || right_end[-1] == '\n' || right_end[-1] == '\r')) right_end--;
        if (left_end > left_start + 1 && right_end > right_start + 1 && left_start[0] == '"' && left_end[-1] == '"' && right_start[0] == '"' && right_end[-1] == '"') {
            size_t generated_start = jq_generated_used;
            if (generated_append(left_start, (size_t)(left_end - left_start - 1)) != 0 || generated_append(right_start + 1, (size_t)(right_end - right_start - 1)) != 0) return -1;
            return append_result(out, count_io, jq_generated + generated_start, jq_generated + jq_generated_used);
        }
    }
    if (parse_json_int(left.start, left.end, &left_number) != 0 || parse_json_int(right.start, right.end, &right_number) != 0) return -1;
    if (op == '+') result = left_number + right_number;
    else if (op == '-') result = left_number - right_number;
    else if (op == '*') result = left_number * right_number;
    else if (op == '/') { if (right_number == 0) return -1; result = left_number / right_number; }
    else if (op == '%') { if (right_number == 0) return -1; result = left_number % right_number; }
    else return -1;
    return append_generated_int_value(out, count_io, result);
}

static int eval_fallback_binary(const char *expression, int op_pos, const char *start, const char *end, JqSlice *out, size_t *count_io) {
    JqSlice left[JQ_MAX_RESULTS];
    size_t left_count = 0U;
    size_t before_count = *count_io;
    size_t i;

    if (eval_filter_text(expression, (size_t)op_pos, start, end, left, &left_count) == 0) {
        for (i = 0U; i < left_count; ++i) {
            if (json_truthy(left[i].start, left[i].end)) {
                if (append_result(out, count_io, left[i].start, left[i].end) != 0) return -1;
            }
        }
        if (*count_io != before_count) return 0;
    }
    return eval_filter_text(expression + op_pos + 2, rt_strlen(expression + op_pos + 2), start, end, out, count_io);
}

static int eval_boolean_binary(const char *expression, int op_pos, const char *word, const char *start, const char *end, JqSlice *out, size_t *count_io) {
    JqSlice left;
    JqSlice right;
    int left_truthy;
    int result;

    if (eval_filter_single(expression, (size_t)op_pos, start, end, &left) != 0) return -1;
    left_truthy = json_truthy(left.start, left.end);
    if (rt_strcmp(word, "or") == 0 && left_truthy) return append_generated_value(out, count_io, "true");
    if (rt_strcmp(word, "and") == 0 && !left_truthy) return append_generated_value(out, count_io, "false");
    if (eval_filter_single(expression + op_pos + rt_strlen(word), rt_strlen(expression + op_pos + rt_strlen(word)), start, end, &right) != 0) return -1;
    result = rt_strcmp(word, "or") == 0 ? (left_truthy || json_truthy(right.start, right.end)) : (left_truthy && json_truthy(right.start, right.end));
    return append_generated_value(out, count_io, result ? "true" : "false");
}

static int append_array_constructor(const char *inner, size_t inner_length, const char *start, const char *end, JqSlice *out, size_t *count_io) {
    JqSlice values[JQ_MAX_RESULTS];
    size_t value_count = 0U;
    size_t generated_start;
    size_t i;

    if (inner_length != 0U) {
        if (eval_filter_text(inner, inner_length, start, end, values, &value_count) != 0) return -1;
    }
    generated_start = jq_generated_used;
    if (generated_append_cstr("[") != 0) return -1;
    if (inner_length != 0U) {
        for (i = 0U; i < value_count; ++i) {
            if (i != 0U && generated_append_cstr(",") != 0) return -1;
            if (generated_append(values[i].start, (size_t)(values[i].end - values[i].start)) != 0) return -1;
        }
    }
    if (generated_append_cstr("]") != 0) return -1;
    return append_result(out, count_io, jq_generated + generated_start, jq_generated + jq_generated_used);
}

static int append_object_key(const char *key_start, size_t key_length, int quoted) {
    if (quoted) return generated_append(key_start, key_length);
    if (generated_append_cstr("\"") != 0 || generated_append(key_start, key_length) != 0 || generated_append_cstr("\"") != 0) return -1;
    return 0;
}

static int append_object_constructor(const char *inner, size_t inner_length, const char *start, const char *end, JqSlice *out, size_t *count_io) {
    const char *key_starts[64];
    size_t key_lengths[64];
    int quoted_keys[64];
    JqSlice values[64];
    size_t pair_count = 0U;
    size_t generated_start;
    size_t pos = 0U;

    while (pos < inner_length) {
        const char *key_start;
        size_t key_length;
        int quoted = 0;
        int colon;
        int comma;
        JqSlice value;

        if (pair_count >= 64U) return -1;
        while (pos < inner_length && (inner[pos] == ' ' || inner[pos] == '\t' || inner[pos] == '\n' || inner[pos] == '\r')) pos++;
        if (pos >= inner_length) break;
        key_start = inner + pos;
        if (inner[pos] == '"') {
            const char *key_end = skip_string(inner + pos);
            if (key_end == 0 || key_end > inner + inner_length) return -1;
            quoted = 1;
            key_length = (size_t)(key_end - key_start);
            pos = (size_t)(key_end - inner);
        } else {
            if (!is_ident_start(inner[pos])) return -1;
            while (pos < inner_length && is_ident_char(inner[pos])) pos++;
            key_length = (size_t)(inner + pos - key_start);
        }
        while (pos < inner_length && (inner[pos] == ' ' || inner[pos] == '\t' || inner[pos] == '\n' || inner[pos] == '\r')) pos++;
        if (pos >= inner_length || inner[pos] != ':') return -1;
        pos++;
        colon = (int)pos;
        comma = find_top_level_operator(inner + pos, ',');
        if (comma < 0) comma = (int)(inner_length - pos);
        if (eval_filter_single(inner + colon, (size_t)comma, start, end, &value) != 0) return -1;
        key_starts[pair_count] = key_start;
        key_lengths[pair_count] = key_length;
        quoted_keys[pair_count] = quoted;
        values[pair_count] = value;
        pair_count++;
        pos = (size_t)(colon + comma);
        while (pos < inner_length && (inner[pos] == ' ' || inner[pos] == '\t' || inner[pos] == '\n' || inner[pos] == '\r')) pos++;
        if (pos < inner_length) {
            if (inner[pos] != ',') return -1;
            pos++;
        }
    }
    generated_start = jq_generated_used;
    if (generated_append_cstr("{") != 0) return -1;
    for (pos = 0U; pos < pair_count; ++pos) {
        if (pos != 0U && generated_append_cstr(",") != 0) return -1;
        if (append_object_key(key_starts[pos], key_lengths[pos], quoted_keys[pos]) != 0 || generated_append_cstr(":") != 0 || generated_append(values[pos].start, (size_t)(values[pos].end - values[pos].start)) != 0) return -1;
    }
    if (generated_append_cstr("}") != 0) return -1;
    return append_result(out, count_io, jq_generated + generated_start, jq_generated + jq_generated_used);
}

static int find_assignment_operator(const char *text, int *is_update_out) {
    int bracket_depth = 0;
    int paren_depth = 0;
    int brace_depth = 0;
    size_t pos = 0U;

    while (text[pos] != '\0') {
        if (text[pos] == '"') {
            const char *end = skip_string(text + pos);
            if (end == 0) return -1;
            pos = (size_t)(end - text);
            continue;
        }
        if (text[pos] == '[') bracket_depth++;
        else if (text[pos] == ']') bracket_depth--;
        else if (text[pos] == '(') paren_depth++;
        else if (text[pos] == ')') paren_depth--;
        else if (text[pos] == '{') brace_depth++;
        else if (text[pos] == '}') brace_depth--;
        else if (bracket_depth == 0 && paren_depth == 0 && brace_depth == 0) {
            if (text[pos] == '|' && text[pos + 1U] == '=') { *is_update_out = 1; return (int)pos; }
            if (text[pos] == '=' && text[pos + 1U] != '=' && (pos == 0U || (text[pos - 1U] != '=' && text[pos - 1U] != '!' && text[pos - 1U] != '<' && text[pos - 1U] != '>'))) { *is_update_out = 0; return (int)pos; }
        }
        if (bracket_depth < 0 || paren_depth < 0 || brace_depth < 0) return -1;
        pos++;
    }
    return -1;
}

static int parse_path_tokens(const char *path, size_t path_length, JqPathToken *tokens, size_t *token_count_out) {
    size_t pos = 0U;
    size_t count = 0U;

    if (path_length == 0U || path[pos] != '.') return -1;
    while (pos < path_length) {
        if (count >= JQ_MAX_PATH_TOKENS) return -1;
        if (path[pos] == '.') {
            pos++;
            if (pos >= path_length) return -1;
            tokens[count].is_index = 0;
            tokens[count].index = 0U;
            if (path[pos] == '"') {
                if (parse_filter_string_key(path, &pos, tokens[count].key, sizeof(tokens[count].key)) != 0) return -1;
            } else {
                size_t length = 0U;
                if (!is_ident_start(path[pos])) return -1;
                while (pos < path_length && is_ident_char(path[pos])) {
                    if (length + 1U >= sizeof(tokens[count].key)) return -1;
                    tokens[count].key[length++] = path[pos++];
                }
                tokens[count].key[length] = '\0';
            }
            count++;
        } else if (path[pos] == '[') {
            pos++;
            if (pos >= path_length) return -1;
            if (path[pos] == '"') {
                tokens[count].is_index = 0;
                if (parse_filter_string_key(path, &pos, tokens[count].key, sizeof(tokens[count].key)) != 0 || pos >= path_length || path[pos] != ']') return -1;
                pos++;
            } else {
                size_t index = 0U;
                tokens[count].is_index = 1;
                tokens[count].key[0] = '\0';
                if (path[pos] < '0' || path[pos] > '9') return -1;
                while (pos < path_length && path[pos] >= '0' && path[pos] <= '9') {
                    size_t digit = (size_t)(path[pos] - '0');
                    if (index > (((size_t)-1) - digit) / 10U) return -1;
                    index = index * 10U + digit;
                    pos++;
                }
                if (pos >= path_length || path[pos] != ']') return -1;
                tokens[count].index = index;
                pos++;
            }
            count++;
        } else {
            return -1;
        }
    }
    *token_count_out = count;
    return count == 0U ? -1 : 0;
}

static int append_created_path(const JqPathToken *tokens, size_t token_index, size_t token_count, const char *replacement_start, const char *replacement_end) {
    if (token_index == token_count) return generated_append(replacement_start, (size_t)(replacement_end - replacement_start));
    if (!tokens[token_index].is_index) {
        if (generated_append_cstr("{") != 0 || append_object_key(tokens[token_index].key, rt_strlen(tokens[token_index].key), 0) != 0 || generated_append_cstr(":") != 0) return -1;
        if (append_created_path(tokens, token_index + 1U, token_count, replacement_start, replacement_end) != 0) return -1;
        return generated_append_cstr("}");
    }
    {
        size_t index;
        if (generated_append_cstr("[") != 0) return -1;
        for (index = 0U; index <= tokens[token_index].index; ++index) {
            if (index != 0U && generated_append_cstr(",") != 0) return -1;
            if (index == tokens[token_index].index) {
                if (append_created_path(tokens, token_index + 1U, token_count, replacement_start, replacement_end) != 0) return -1;
            } else if (generated_append_cstr("null") != 0) return -1;
        }
        return generated_append_cstr("]");
    }
}

static int append_updated_value(const char *start, const char *end, const JqPathToken *tokens, size_t token_index, size_t token_count, const char *replacement_start, const char *replacement_end) {
    const char *p = skip_ws(start);

    if (token_index == token_count) return generated_append(replacement_start, (size_t)(replacement_end - replacement_start));
    if (!tokens[token_index].is_index && *p == '{') {
        int any_matched = 0;
        size_t entry_count = 0U;
        p++;
        if (generated_append(start, (size_t)(p - start)) != 0) return -1;
        for (;;) {
            const char *key_start;
            const char *key_end;
            const char *value_start;
            const char *value_end;
            const char *after_value;
            int matched;

            p = skip_ws(p);
            if (*p == '}') {
                if (!any_matched) {
                    if (entry_count != 0U && generated_append_cstr(",") != 0) return -1;
                    if (append_object_key(tokens[token_index].key, rt_strlen(tokens[token_index].key), 0) != 0 || generated_append_cstr(":") != 0) return -1;
                    if (append_created_path(tokens, token_index + 1U, token_count, replacement_start, replacement_end) != 0) return -1;
                }
                return generated_append(p, (size_t)(end - p));
            }
            key_start = p;
            key_end = skip_string(p);
            if (key_end == 0) return -1;
            matched = string_key_equals(key_start, tokens[token_index].key, &after_value);
            p = skip_ws(key_end);
            if (*p != ':') return -1;
            value_start = skip_ws(p + 1);
            value_end = skip_value(value_start);
            if (value_end == 0) return -1;
            if (generated_append(key_start, (size_t)(value_start - key_start)) != 0) return -1;
            if (matched) {
                any_matched = 1;
                if (append_updated_value(value_start, value_end, tokens, token_index + 1U, token_count, replacement_start, replacement_end) != 0) return -1;
            } else if (generated_append(value_start, (size_t)(value_end - value_start)) != 0) {
                return -1;
            }
            after_value = value_end;
            p = skip_ws(value_end);
            if (*p == ',') {
                if (generated_append(after_value, (size_t)(p + 1 - after_value)) != 0) return -1;
                p++;
            } else if (*p == '}') {
                if (!any_matched) {
                    if (generated_append(after_value, (size_t)(p - after_value)) != 0 || generated_append_cstr(",") != 0) return -1;
                    if (append_object_key(tokens[token_index].key, rt_strlen(tokens[token_index].key), 0) != 0 || generated_append_cstr(":") != 0) return -1;
                    if (append_created_path(tokens, token_index + 1U, token_count, replacement_start, replacement_end) != 0) return -1;
                    return generated_append(p, (size_t)(end - p));
                }
                if (generated_append(after_value, (size_t)(end - after_value)) != 0) return -1;
                return 0;
            } else return -1;
            entry_count++;
        }
    }
    if (tokens[token_index].is_index && *p == '[') {
        size_t index = 0U;
        int any_matched = 0;
        p++;
        if (generated_append(start, (size_t)(p - start)) != 0) return -1;
        for (;;) {
            const char *value_start;
            const char *value_end;
            const char *after_value;

            p = skip_ws(p);
            if (*p == ']') {
                while (index <= tokens[token_index].index) {
                    if (index != 0U && generated_append_cstr(",") != 0) return -1;
                    if (index == tokens[token_index].index) {
                        if (append_created_path(tokens, token_index + 1U, token_count, replacement_start, replacement_end) != 0) return -1;
                    } else if (generated_append_cstr("null") != 0) return -1;
                    index++;
                }
                return generated_append(p, (size_t)(end - p));
            }
            value_start = p;
            value_end = skip_value(value_start);
            if (value_end == 0) return -1;
            if (index == tokens[token_index].index) {
                any_matched = 1;
                if (append_updated_value(value_start, value_end, tokens, token_index + 1U, token_count, replacement_start, replacement_end) != 0) return -1;
            } else if (generated_append(value_start, (size_t)(value_end - value_start)) != 0) return -1;
            after_value = value_end;
            p = skip_ws(value_end);
            if (*p == ',') {
                if (generated_append(after_value, (size_t)(p + 1 - after_value)) != 0) return -1;
                p++;
            } else if (*p == ']') {
                if (!any_matched) {
                    size_t next_index = index + 1U;
                    if (generated_append(after_value, (size_t)(p - after_value)) != 0) return -1;
                    while (next_index <= tokens[token_index].index) {
                        if (generated_append_cstr(",") != 0) return -1;
                        if (next_index == tokens[token_index].index) {
                            if (append_created_path(tokens, token_index + 1U, token_count, replacement_start, replacement_end) != 0) return -1;
                        } else if (generated_append_cstr("null") != 0) return -1;
                        next_index++;
                    }
                    return generated_append(p, (size_t)(end - p));
                }
                if (generated_append(after_value, (size_t)(end - after_value)) != 0) return -1;
                return 0;
            } else return -1;
            index++;
        }
    }
    return -1;
}

static int make_generated_null_slice(JqSlice *value_out);

static int eval_assignment_filter(const char *expression, int op_pos, int is_update, const char *start, const char *end, JqSlice *out, size_t *count_io) {
    JqPathToken tokens[JQ_MAX_PATH_TOKENS];
    size_t token_count = 0U;
    JqSlice replacement;
    char path_text[JQ_MAX_FILTER];
    const char *path_starts[JQ_MAX_PATH_TOKENS];
    size_t path_lengths[JQ_MAX_PATH_TOKENS];
    size_t path_count = 0U;
    JqSlice current;
    size_t i;

    if (copy_filter_text(expression, (size_t)op_pos, path_text, sizeof(path_text)) != 0 || split_top_level_args(path_text, path_starts, path_lengths, JQ_MAX_PATH_TOKENS, &path_count) != 0 || path_count == 0U) return -1;
    current.start = start;
    current.end = end;
    for (i = 0U; i < path_count; ++i) {
        char single_path[JQ_MAX_FILTER];
        size_t generated_start;
        if (copy_filter_text(path_starts[i], path_lengths[i], single_path, sizeof(single_path)) != 0 || parse_path_tokens(single_path, rt_strlen(single_path), tokens, &token_count) != 0) return -1;
        if (is_update) {
            JqSlice target_values[JQ_MAX_RESULTS];
            size_t target_count = 0U;
            if (eval_path_collect(single_path, 0U, current.start, current.end, target_values, &target_count) != 0 || target_count == 0U) {
                if (make_generated_null_slice(&target_values[0]) != 0) return -1;
                target_count = 1U;
            }
            if (target_count != 1U || eval_filter_single(expression + op_pos + 2, rt_strlen(expression + op_pos + 2), target_values[0].start, target_values[0].end, &replacement) != 0) return -1;
        } else {
            if (eval_filter_single(expression + op_pos + 1, rt_strlen(expression + op_pos + 1), start, end, &replacement) != 0) return -1;
        }
        generated_start = jq_generated_used;
        if (append_updated_value(current.start, current.end, tokens, 0U, token_count, replacement.start, replacement.end) != 0) return -1;
        current.start = jq_generated + generated_start;
        current.end = jq_generated + jq_generated_used;
    }
    return append_result(out, count_io, current.start, current.end);
}

static int bind_pattern_variables(const char *pattern, size_t pattern_length, const char *value_start, const char *value_end);

static int eval_as_binding(const char *expression, int as_pos, const char *start, const char *end, JqSlice *out, size_t *count_io) {
    JqSlice values[JQ_MAX_RESULTS];
    size_t value_count = 0U;
    size_t pos = (size_t)as_pos + 2U;
    int pipe_pos;
    size_t saved_count;
    size_t i;

    if (eval_filter_text(expression, (size_t)as_pos, start, end, values, &value_count) != 0) return -1;
    while (expression[pos] == ' ' || expression[pos] == '\t' || expression[pos] == '\n' || expression[pos] == '\r') pos++;
    pipe_pos = find_top_level_operator(expression + pos, '|');
    if (pipe_pos <= 0) return -1;
    saved_count = jq_variable_count;
    for (i = 0U; i < value_count; ++i) {
        jq_variable_count = saved_count;
        if (bind_pattern_variables(expression + pos, (size_t)pipe_pos, values[i].start, values[i].end) != 0) return -1;
        if (eval_filter_text(expression + pos + (size_t)pipe_pos + 1U, rt_strlen(expression + pos + (size_t)pipe_pos + 1U), start, end, out, count_io) != 0) return -1;
    }
    jq_variable_count = saved_count;
    return 0;
}

static int eval_user_function_call(JqFunction *function, const char *arg_text, size_t arg_length, const char *start, const char *end, JqSlice *out, size_t *count_io) {
    char args_buffer[JQ_MAX_FILTER];
    const char *arg_starts[JQ_MAX_FUNCTION_ARGS];
    size_t arg_lengths[JQ_MAX_FUNCTION_ARGS];
    size_t arg_count = 0U;
    JqSlice arg_values[JQ_MAX_FUNCTION_ARGS];
    size_t saved_count = jq_variable_count;
    size_t i;

    if (arg_length + 1U > sizeof(args_buffer)) return -1;
    memcpy(args_buffer, arg_text, arg_length);
    args_buffer[arg_length] = '\0';
    if (split_top_level_args(args_buffer, arg_starts, arg_lengths, JQ_MAX_FUNCTION_ARGS, &arg_count) != 0 || arg_count != function->arg_count) return -1;
    for (i = 0U; i < arg_count; ++i) {
        if (eval_filter_single(arg_starts[i], arg_lengths[i], start, end, &arg_values[i]) != 0) return -1;
    }
    for (i = 0U; i < arg_count; ++i) {
        if (push_variable(function->args[i], &arg_values[i]) != 0) { jq_variable_count = saved_count; return -1; }
    }
    if (eval_filter_text(function->body, rt_strlen(function->body), start, end, out, count_io) != 0) { jq_variable_count = saved_count; return -1; }
    jq_variable_count = saved_count;
    return 0;
}

static int make_generated_null_slice(JqSlice *value_out) {
    size_t generated_start = jq_generated_used;

    if (generated_append_cstr("null") != 0) return -1;
    value_out->start = jq_generated + generated_start;
    value_out->end = jq_generated + jq_generated_used;
    return 0;
}

static int bind_pattern_variables(const char *pattern, size_t pattern_length, const char *value_start, const char *value_end) {
    const char *trimmed;
    size_t trimmed_length;
    char text[JQ_MAX_FILTER];

    if (trim_filter(pattern, pattern_length, &trimmed, &trimmed_length) != 0 || trimmed_length + 1U > sizeof(text)) return -1;
    memcpy(text, trimmed, trimmed_length);
    text[trimmed_length] = '\0';
    if (text[0] == '_') return text[1] == '\0' ? 0 : -1;
    if (text[0] == '$') {
        size_t pos = 1U;
        char name[JQ_NAME_CAPACITY];
        if (!is_ident_start(text[pos])) return -1;
        while (is_ident_char(text[pos])) {
            if (pos >= sizeof(name)) return -1;
            name[pos - 1U] = text[pos];
            pos++;
        }
        name[pos - 1U] = '\0';
        if (text[pos] != '\0') return -1;
        {
            JqSlice value;
            value.start = value_start;
            value.end = value_end;
            return push_variable(name, &value);
        }
    }
    if (text[0] == '{' && trimmed_length >= 2U && text[trimmed_length - 1U] == '}') {
        size_t pos = 1U;
        size_t inner_end = trimmed_length - 1U;
        while (pos < inner_end) {
            const char *key_start;
            char key[JQ_NAME_CAPACITY];
            size_t key_length = 0U;
            int colon;
            int comma;
            const char *member_start;
            const char *member_end;
            JqSlice missing;

            while (pos < inner_end && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) pos++;
            if (pos >= inner_end) break;
            if (text[pos] == '"') {
                if (parse_filter_string_key(text, &pos, key, sizeof(key)) != 0) return -1;
            } else {
                if (!is_ident_start(text[pos])) return -1;
                key_start = text + pos;
                while (pos < inner_end && is_ident_char(text[pos])) {
                    if (key_length + 1U >= sizeof(key)) return -1;
                    key[key_length++] = text[pos++];
                }
                key[key_length] = '\0';
                (void)key_start;
            }
            while (pos < inner_end && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) pos++;
            if (pos < inner_end && text[pos] == ':') {
                pos++;
                colon = (int)pos;
                comma = find_top_level_operator(text + pos, ',');
                if (comma < 0 || pos + (size_t)comma > inner_end) comma = (int)(inner_end - pos);
                if (find_key(value_start, key, &member_start, &member_end) != 0) {
                    if (make_generated_null_slice(&missing) != 0) return -1;
                    member_start = missing.start;
                    member_end = missing.end;
                }
                if (bind_pattern_variables(text + colon, (size_t)comma, member_start, member_end) != 0) return -1;
                pos = (size_t)(colon + comma);
            } else {
                JqSlice member_value;
                if (find_key(value_start, key, &member_start, &member_end) != 0) {
                    if (make_generated_null_slice(&missing) != 0) return -1;
                    member_start = missing.start;
                    member_end = missing.end;
                }
                member_value.start = member_start;
                member_value.end = member_end;
                if (push_variable(key, &member_value) != 0) return -1;
            }
            while (pos < inner_end && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) pos++;
            if (pos < inner_end) {
                if (text[pos] != ',') return -1;
                pos++;
            }
        }
        return 0;
    }
    if (text[0] == '[' && trimmed_length >= 2U && text[trimmed_length - 1U] == ']') {
        char inner[JQ_MAX_FILTER];
        const char *starts[JQ_MAX_FUNCTION_ARGS * 4U];
        size_t lengths[JQ_MAX_FUNCTION_ARGS * 4U];
        size_t count = 0U;
        size_t i;

        if (trimmed_length - 1U >= sizeof(inner)) return -1;
        memcpy(inner, text + 1, trimmed_length - 2U);
        inner[trimmed_length - 2U] = '\0';
        if (split_top_level_args(inner, starts, lengths, JQ_MAX_FUNCTION_ARGS * 4U, &count) != 0) return -1;
        for (i = 0U; i < count; ++i) {
            const char *element_start;
            const char *element_end;
            JqSlice missing;
            if (find_array_index(value_start, i, &element_start, &element_end) != 0) {
                if (make_generated_null_slice(&missing) != 0) return -1;
                element_start = missing.start;
                element_end = missing.end;
            }
            if (bind_pattern_variables(starts[i], lengths[i], element_start, element_end) != 0) return -1;
        }
        return 0;
    }
    return -1;
}

static int eval_filter_text(const char *filter, size_t filter_length, const char *current_start, const char *current_end, JqSlice *out, size_t *count_io) {
    char expression[JQ_MAX_FILTER];
    const char *arg;
    const char *op;
    size_t arg_length;
    int pos;
    int function_result;

    if (copy_filter_text(filter, filter_length, expression, sizeof(expression)) != 0 || expression[0] == '\0') return -1;
    pos = find_top_level_word_operator(expression, "as");
    if (pos > 0) return eval_as_binding(expression, pos, current_start, current_end, out, count_io);
    {
        int is_update = 0;
        pos = find_assignment_operator(expression, &is_update);
        if (pos > 0) return eval_assignment_filter(expression, pos, is_update, current_start, current_end, out, count_io);
    }
    pos = find_top_level_operator(expression, '|');
    if (pos > 0) {
        JqSlice intermediate[JQ_MAX_RESULTS];
        size_t intermediate_count = 0U;
        size_t i;

        if (eval_filter_text(expression, (size_t)pos, current_start, current_end, intermediate, &intermediate_count) != 0) return -1;
        for (i = 0U; i < intermediate_count; ++i) {
            if (eval_filter_text(expression + pos + 1, rt_strlen(expression + pos + 1), intermediate[i].start, intermediate[i].end, out, count_io) != 0) return -1;
        }
        return 0;
    }
    pos = find_top_level_operator(expression, ',');
    if (pos > 0) {
        if (eval_filter_text(expression, (size_t)pos, current_start, current_end, out, count_io) != 0) return -1;
        return eval_filter_text(expression + pos + 1, rt_strlen(expression + pos + 1), current_start, current_end, out, count_io);
    }
    pos = find_top_level_two_char_operator(expression, '/', '/');
    if (pos > 0) return eval_fallback_binary(expression, pos, current_start, current_end, out, count_io);
    pos = find_top_level_word_operator(expression, "or");
    if (pos > 0) return eval_boolean_binary(expression, pos, "or", current_start, current_end, out, count_io);
    pos = find_top_level_word_operator(expression, "and");
    if (pos > 0) return eval_boolean_binary(expression, pos, "and", current_start, current_end, out, count_io);
    if (rt_strncmp(expression, "not ", 4U) == 0) {
        JqSlice value;
        if (eval_filter_single(expression + 4, rt_strlen(expression + 4), current_start, current_end, &value) != 0) return -1;
        return append_generated_value(out, count_io, json_truthy(value.start, value.end) ? "false" : "true");
    }
    pos = find_top_level_comparison(expression, &op);
    if (pos > 0) return eval_comparison(expression, pos, op, current_start, current_end, out, count_io);
    pos = find_top_level_single_operator_from_right(expression, "+-");
    if (pos > 0) return eval_numeric_binary(expression, pos, expression[pos], current_start, current_end, out, count_io);
    pos = find_top_level_single_operator_from_right(expression, "*/%");
    if (pos > 0) return eval_numeric_binary(expression, pos, expression[pos], current_start, current_end, out, count_io);

    if (expression[0] == '@') {
        if (rt_strcmp(expression, "@json") == 0) return append_generated_json_string_from_slice(current_start, current_end, out, count_io);
        if (rt_strcmp(expression, "@text") == 0 || rt_strcmp(expression, "@string") == 0) return append_generated_json_string_from_value(current_start, current_end, out, count_io);
        return -1;
    }
    if (expression[0] == '"' && string_has_interpolation(expression)) return append_interpolated_string(expression, current_start, current_end, out, count_io);
    if (is_json_literal_text(expression)) return append_generated_value(out, count_io, expression);
    if (expression[0] == '$') {
        JqVariable *variable;
        char name[JQ_NAME_CAPACITY];
        size_t pos = 1U;
        size_t name_length = 0U;
        if (!is_ident_start(expression[1])) return -1;
        while (is_ident_char(expression[pos])) {
            if (name_length + 1U >= sizeof(name)) return -1;
            name[name_length++] = expression[pos++];
        }
        name[name_length] = '\0';
        variable = find_variable(name);
        if (variable == 0) return -1;
        if (expression[pos] == '.' || expression[pos] == '[') return eval_path_collect(expression, pos, variable->value.start, variable->value.end, out, count_io);
        if (expression[pos] != '\0') return -1;
        return append_result(out, count_io, variable->value.start, variable->value.end);
    }
    if (expression[0] == '[' && expression[rt_strlen(expression) - 1U] == ']') return append_array_constructor(expression + 1, rt_strlen(expression) - 2U, current_start, current_end, out, count_io);
    if (expression[0] == '{' && expression[rt_strlen(expression) - 1U] == '}') return append_object_constructor(expression + 1, rt_strlen(expression) - 2U, current_start, current_end, out, count_io);
    if (rt_strcmp(expression, ".") == 0) return append_result(out, count_io, current_start, current_end);
    if (rt_strcmp(expression, "..") == 0) return collect_descendants(current_start, current_end, out, count_io);
    if (rt_strcmp(expression, "type") == 0) return append_type_result(current_start, current_end, out, count_io);
    if (rt_strcmp(expression, "length") == 0) return append_length_result(current_start, current_end, out, count_io);
    if (rt_strcmp(expression, "keys") == 0) return append_keys_result(current_start, current_end, out, count_io);
    if (rt_strcmp(expression, "tostring") == 0) return append_generated_json_string_from_value(current_start, current_end, out, count_io);
    if (rt_strcmp(expression, "ascii_upcase") == 0) return append_ascii_case_result(current_start, current_end, 1, out, count_io);
    if (rt_strcmp(expression, "ascii_downcase") == 0) return append_ascii_case_result(current_start, current_end, 0, out, count_io);

    function_result = parse_function_arg(expression, "has", &arg, &arg_length);
    if (function_result < 0) return -1;
    if (function_result > 0) return append_has_result(current_start, current_end, arg, arg_length, out, count_io);
    function_result = parse_function_arg(expression, "map", &arg, &arg_length);
    if (function_result < 0) return -1;
    if (function_result > 0) return append_map_result(current_start, current_end, arg, arg_length, out, count_io);
    function_result = parse_function_arg(expression, "select", &arg, &arg_length);
    if (function_result < 0) return -1;
    if (function_result > 0) return append_select_result(current_start, current_end, arg, arg_length, out, count_io);
    function_result = parse_function_arg(expression, "contains", &arg, &arg_length);
    if (function_result < 0) return -1;
    if (function_result > 0) return append_string_predicate_result(current_start, current_end, arg, arg_length, "contains", out, count_io);
    function_result = parse_function_arg(expression, "startswith", &arg, &arg_length);
    if (function_result < 0) return -1;
    if (function_result > 0) return append_string_predicate_result(current_start, current_end, arg, arg_length, "startswith", out, count_io);
    function_result = parse_function_arg(expression, "endswith", &arg, &arg_length);
    if (function_result < 0) return -1;
    if (function_result > 0) return append_string_predicate_result(current_start, current_end, arg, arg_length, "endswith", out, count_io);
    function_result = parse_function_arg(expression, "test", &arg, &arg_length);
    if (function_result < 0) return -1;
    if (function_result > 0) return append_regex_test_result(current_start, current_end, arg, arg_length, out, count_io);
    function_result = parse_function_arg(expression, "split", &arg, &arg_length);
    if (function_result < 0) return -1;
    if (function_result > 0) return append_split_result(current_start, current_end, arg, arg_length, out, count_io);
    function_result = parse_function_arg(expression, "join", &arg, &arg_length);
    if (function_result < 0) return -1;
    if (function_result > 0) return append_join_result(current_start, current_end, arg, arg_length, out, count_io);

    {
        char call_name[JQ_NAME_CAPACITY];
        const char *call_args;
        size_t call_args_length;
        JqFunction *function;
        function_result = parse_call_expression(expression, call_name, sizeof(call_name), &call_args, &call_args_length);
        if (function_result < 0) return -1;
        if (function_result > 0) {
            function = find_function(call_name);
            if (function == 0) return -1;
            return eval_user_function_call(function, call_args, call_args_length, current_start, current_end, out, count_io);
        }
    }

    if (expression[0] == '.') return eval_path_collect(expression, 0U, current_start, current_end, out, count_io);
    return -1;
}

static int parse_function_definitions(const char *filter, const char **body_out) {
    const char *p = filter;

    jq_function_count = 0U;
    for (;;) {
        JqFunction *function;
        size_t name_length = 0U;
        char params[JQ_MAX_FILTER];
        const char *param_starts[JQ_MAX_FUNCTION_ARGS];
        size_t param_lengths[JQ_MAX_FUNCTION_ARGS];
        size_t param_count = 0U;
        size_t params_length;
        int semicolon;
        size_t i;

        p = skip_ws(p);
        if (rt_strncmp(p, "def", 3U) != 0 || is_ident_char(p[3])) {
            *body_out = p;
            return 0;
        }
        p += 3;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!is_ident_start(*p) || jq_function_count >= JQ_MAX_FUNCTIONS) return -1;
        function = &jq_functions[jq_function_count];
        rt_memset(function, 0, sizeof(*function));
        while (is_ident_char(*p)) {
            if (name_length + 1U >= sizeof(function->name)) return -1;
            function->name[name_length++] = *p++;
        }
        function->name[name_length] = '\0';
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != '(') return -1;
        p++;
        params_length = 0U;
        while (*p != '\0' && *p != ')') {
            if (params_length + 1U >= sizeof(params)) return -1;
            params[params_length++] = *p++;
        }
        if (*p != ')') return -1;
        params[params_length] = '\0';
        p++;
        if (split_top_level_args(params, param_starts, param_lengths, JQ_MAX_FUNCTION_ARGS, &param_count) != 0) return -1;
        if (param_count == 1U && param_lengths[0] == 0U) param_count = 0U;
        function->arg_count = param_count;
        for (i = 0U; i < param_count; ++i) {
            const char *param = param_starts[i];
            size_t length = param_lengths[i];
            if (length < 2U || param[0] != '$' || !is_ident_start(param[1]) || length >= sizeof(function->args[i])) return -1;
            memcpy(function->args[i], param + 1, length - 1U);
            function->args[i][length - 1U] = '\0';
        }
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != ':') return -1;
        p++;
        semicolon = find_top_level_operator(p, ';');
        if (semicolon < 0 || (size_t)semicolon >= sizeof(function->body)) return -1;
        if (copy_filter_text(p, (size_t)semicolon, function->body, sizeof(function->body)) != 0) return -1;
        p += (size_t)semicolon + 1U;
        jq_function_count++;
    }
}

static int run_filter(const char *filter, const char *input) {
    const char *current_start = skip_ws(input);
    const char *current_end = skip_value(current_start);
    const char *filter_body;
    JqSlice results[JQ_MAX_RESULTS];
    size_t result_count = 0U;

    if (current_end == 0) return -1;
    jq_generated_used = 0U;
    jq_variable_count = 0U;
    if (parse_function_definitions(filter, &filter_body) != 0) return -1;
    if (eval_filter_text(filter_body, rt_strlen(filter_body), current_start, current_end, results, &result_count) != 0) return -1;
    return emit_results(results, result_count);
}

int main(int argc, char **argv) {
    const char *filter = 0;
    const char *path = "-";
    int argi = 1;
    int fd, should_close;
    size_t size;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-r") == 0 || rt_strcmp(argv[argi], "--raw-output") == 0) {
            raw_output = 1;
        } else if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) {
            print_usage();
            return 0;
        } else {
            break;
        }
        argi++;
    }
    if (argi >= argc) {
        print_usage();
        return 1;
    }
    filter = argv[argi++];
    if (argi < argc) path = argv[argi++];
    if (argi != argc) {
        print_usage();
        return 1;
    }
    if (tool_open_input(path, &fd, &should_close) != 0) {
        tool_write_error("jq", "cannot open ", path);
        return 1;
    }
    if (read_all(fd, jq_input, JQ_MAX_INPUT, &size) != 0) {
        tool_close_input(fd, should_close);
        tool_write_error("jq", "input too large", 0);
        return 1;
    }
    tool_close_input(fd, should_close);
    (void)size;
    if (run_filter(filter, jq_input) != 0) {
        tool_write_error("jq", "filter failed", filter);
        return 1;
    }
    return 0;
}
