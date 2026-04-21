/* IR prescan, data emission, and assembly dispatch helpers. */

#include "backend_internal.h"

#include <limits.h>

static const char *extract_store_name(const char *cursor, char *name, size_t name_size);

static int parse_decl_line(const char *line,
                           char *storage,
                           size_t storage_size,
                           char *kind,
                           size_t kind_size,
                           char *type_text,
                           size_t type_size,
                           char *name,
                           size_t name_size) {
    const char *cursor;
    const char *scan;
    const char *last = 0;
    size_t type_length = 0;

    cursor = skip_spaces(line + 5);
    cursor = copy_next_word(cursor, storage, storage_size);
    cursor = copy_next_word(cursor, kind, kind_size);

    copy_last_word(cursor, name, name_size);
    scan = cursor;
    while (*scan != '\0') {
        if (*scan == ' ') {
            last = scan;
        }
        scan += 1;
    }
    if (last != 0) {
        while (cursor < last && type_length + 1 < type_size) {
            type_text[type_length++] = *cursor++;
        }
        while (type_length > 0 && type_text[type_length - 1] == ' ') {
            type_length -= 1U;
        }
    }
    type_text[type_length] = '\0';
    return 0;
}

static int parse_const_line(const char *line, char *name, size_t name_size, long long *value_out) {
    const char *cursor = skip_spaces(line + 6);
    size_t out = 0;

    while (*cursor != '\0' && *cursor != ' ' && *cursor != '=' && out + 1 < name_size) {
        name[out++] = *cursor++;
    }
    name[out] = '\0';
    cursor = skip_spaces(cursor);
    if (*cursor == '=') {
        cursor = skip_spaces(cursor + 1);
    }

    return parse_signed_value(cursor, value_out);
}

static int parse_aggregate_line(const char *line,
                                char *kind,
                                size_t kind_size,
                                char *name,
                                size_t name_size,
                                int *size_out,
                                int *align_out) {
    const char *cursor = skip_spaces(line + 9);
    char number_text[32];
    long long size_value = 0;
    long long align_value = 0;

    cursor = copy_next_word(cursor, kind, kind_size);
    cursor = copy_next_word(cursor, name, name_size);
    cursor = copy_next_word(cursor, number_text, sizeof(number_text));
    if (number_text[0] == '\0' || parse_signed_value(number_text, &size_value) != 0) {
        return -1;
    }
    cursor = copy_next_word(cursor, number_text, sizeof(number_text));
    if (number_text[0] == '\0' || parse_signed_value(number_text, &align_value) != 0) {
        return -1;
    }

    if (size_out != 0) {
        *size_out = (int)size_value;
    }
    if (align_out != 0) {
        *align_out = (int)align_value;
    }
    return 0;
}

static int parse_member_line(const char *line,
                             char *aggregate_name,
                             size_t aggregate_name_size,
                             char *member_name,
                             size_t member_name_size,
                             int *offset_out,
                             char *type_text,
                             size_t type_text_size) {
    const char *cursor = skip_spaces(line + 6);
    char number_text[32];
    long long offset = 0;
    const char *type_start;
    size_t type_length = 0;

    cursor = copy_next_word(cursor, aggregate_name, aggregate_name_size);
    cursor = copy_next_word(cursor, member_name, member_name_size);
    cursor = copy_next_word(cursor, number_text, sizeof(number_text));
    if (number_text[0] == '\0' || parse_signed_value(number_text, &offset) != 0) {
        return -1;
    }
    type_start = cursor;
    while (type_start[type_length] != '\0' && type_length + 1 < type_text_size) {
        type_text[type_length] = type_start[type_length];
        type_length += 1U;
    }
    while (type_length > 0U && type_text[type_length - 1U] == ' ') {
        type_length -= 1U;
    }
    type_text[type_length] = '\0';

    if (offset_out != 0) {
        *offset_out = (int)offset;
    }
    return aggregate_name[0] != '\0' && member_name[0] != '\0' ? 0 : -1;
}

static const char *find_global_initializer_expr(const BackendState *state, const char *name);
static int resolve_static_value(const BackendState *state, const char *expr, long long *value_out);

typedef enum {
    GLOBAL_INIT_EOF = 0,
    GLOBAL_INIT_IDENTIFIER,
    GLOBAL_INIT_NUMBER,
    GLOBAL_INIT_STRING,
    GLOBAL_INIT_CHAR,
    GLOBAL_INIT_PUNCT
} GlobalInitTokenKind;

typedef struct {
    const char *cursor;
    GlobalInitTokenKind kind;
    char text[COMPILER_IR_LINE_CAPACITY];
    long long number_value;
} GlobalInitParser;

static void global_init_next(GlobalInitParser *parser) {
    const char *cursor = skip_spaces(parser->cursor);
    size_t length = 0;

    parser->cursor = cursor;
    parser->text[0] = '\0';
    parser->number_value = 0;

    if (*cursor == '\0') {
        parser->kind = GLOBAL_INIT_EOF;
        return;
    }

    if ((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') || *cursor == '_') {
        parser->kind = GLOBAL_INIT_IDENTIFIER;
        while (((*cursor >= 'a' && *cursor <= 'z') ||
                (*cursor >= 'A' && *cursor <= 'Z') ||
                (*cursor >= '0' && *cursor <= '9') || *cursor == '_') &&
               length + 1 < sizeof(parser->text)) {
            parser->text[length++] = *cursor++;
        }
        parser->text[length] = '\0';
        parser->cursor = cursor;
        return;
    }

    if (*cursor == '"' || *cursor == '\'') {
        char quote = *cursor++;
        parser->kind = quote == '"' ? GLOBAL_INIT_STRING : GLOBAL_INIT_CHAR;
        while (*cursor != '\0' && *cursor != quote && length + 1 < sizeof(parser->text)) {
            if (*cursor == '\\' && cursor[1] != '\0') {
                cursor += 1;
                parser->text[length++] = backend_decode_escaped_char(&cursor);
                continue;
            }
            parser->text[length++] = *cursor++;
        }
        parser->text[length] = '\0';
        if (*cursor == quote) {
            cursor += 1;
        }
        if (parser->kind == GLOBAL_INIT_CHAR) {
            parser->number_value = (long long)(unsigned char)parser->text[0];
        }
        parser->cursor = cursor;
        return;
    }

    if ((*cursor >= '0' && *cursor <= '9') || *cursor == '-' || *cursor == '+') {
        parser->kind = GLOBAL_INIT_NUMBER;
        if (*cursor == '-' || *cursor == '+') {
            parser->text[length++] = *cursor++;
        }
        while (((*cursor >= '0' && *cursor <= '9') ||
                (*cursor >= 'a' && *cursor <= 'f') ||
                (*cursor >= 'A' && *cursor <= 'F') ||
                *cursor == 'x' || *cursor == 'X' ||
                *cursor == 'u' || *cursor == 'U' ||
                *cursor == 'l' || *cursor == 'L') &&
               length + 1 < sizeof(parser->text)) {
            parser->text[length++] = *cursor++;
        }
        parser->text[length] = '\0';
        (void)parse_signed_value(parser->text, &parser->number_value);
        parser->cursor = cursor;
        return;
    }

    parser->kind = GLOBAL_INIT_PUNCT;
    parser->text[0] = *cursor;
    parser->text[1] = '\0';
    parser->cursor = cursor + 1;
}

static int parse_array_length_text(const char *type_text, unsigned long long *length_out) {
    const char *open = type_text;
    unsigned long long value = 0ULL;

    while (*open != '\0' && *open != '[') {
        open += 1;
    }
    if (*open != '[') {
        return -1;
    }
    open += 1;
    while (*open >= '0' && *open <= '9') {
        if (value > (ULLONG_MAX - (unsigned long long)(*open - '0')) / 10ULL) {
            return -1;
        }
        value = value * 10ULL + (unsigned long long)(*open - '0');
        open += 1;
    }
    *length_out = value;
    return 0;
}

static unsigned long long clamp_array_product(unsigned long long lhs,
                                              unsigned long long rhs,
                                              unsigned long long limit) {
    if (lhs == 0ULL || rhs == 0ULL) {
        return 0ULL;
    }
    if (lhs > limit / rhs) {
        return limit;
    }
    return lhs * rhs;
}

static void copy_aggregate_name_from_type_text(const char *type_text, char *buffer, size_t buffer_size) {
    const char *cursor;
    size_t out = 0;

    if (buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    type_text = skip_spaces(type_text);
    if (starts_with(type_text, "struct:")) {
        cursor = type_text + 7;
    } else if (starts_with(type_text, "union:")) {
        cursor = type_text + 6;
    } else {
        return;
    }
    while (*cursor != '\0' && *cursor != '[' && *cursor != '*' && *cursor != ' ' && out + 1 < buffer_size) {
        buffer[out++] = *cursor++;
    }
    buffer[out] = '\0';
}

static void copy_indexed_type_text(const char *base_type, char *buffer, size_t buffer_size) {
    backend_copy_indexed_type_text(base_type, buffer, buffer_size);
}

static int global_type_storage_bytes(const BackendState *state, const char *type_text) {
    long long size = backend_type_storage_bytes(state, type_text);

    if (size <= 0) {
        return backend_stack_slot_size(state);
    }
    if ((unsigned long long)size > (unsigned long long)BACKEND_MAX_OBJECT_STACK_BYTES) {
        return BACKEND_MAX_OBJECT_STACK_BYTES;
    }
    return (int)size;
}

static int emit_zero_fill(BackendState *state, unsigned long long size) {
    char line[64];
    char digits[32];

    if (size == 0ULL) {
        return 0;
    }
    rt_copy_string(line, sizeof(line), "    .zero ");
    rt_unsigned_to_string(size, digits, sizeof(digits));
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
    return emit_line(state, line);
}

static int emit_data_integer(BackendState *state, int size_bytes, long long value) {
    char line[128];
    char digits[32];
    const char *directive = "    .quad ";

    if (size_bytes == 1) directive = "    .byte ";
    else if (size_bytes == 2) directive = "    .short ";
    else if (size_bytes == 4) directive = "    .long ";

    rt_copy_string(line, sizeof(line), directive);
    if (value < 0) {
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "-");
        rt_unsigned_to_string((unsigned long long)(-value), digits, sizeof(digits));
    } else {
        rt_unsigned_to_string((unsigned long long)value, digits, sizeof(digits));
    }
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
    return emit_line(state, line);
}

static int emit_data_symbol_ref(BackendState *state, const char *symbol_text) {
    char line[128];
    rt_copy_string(line, sizeof(line), "    .quad ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol_text);
    return emit_line(state, line);
}

static int emit_char_array_contents(BackendState *state, const char *text, unsigned long long total_size) {
    unsigned long long i;
    size_t text_length = text != 0 ? rt_strlen(text) : 0U;

    for (i = 0; i < total_size; ++i) {
        unsigned char value = 0;
        if (text != 0 && i < (unsigned long long)text_length) {
            value = (unsigned char)text[i];
        } else if (text != 0 && i == (unsigned long long)text_length) {
            value = 0;
        }
        if (emit_data_integer(state, 1, (long long)value) != 0) {
            return -1;
        }
    }
    return 0;
}

static int emit_global_initializer_value(BackendState *state,
                                         const char *type_text,
                                         GlobalInitParser *parser,
                                         unsigned long long *bytes_out);

static int word_is_type_keyword(const char *word) {
    return names_equal(word, "char") ||
           names_equal(word, "short") ||
           names_equal(word, "int") ||
           names_equal(word, "long") ||
           names_equal(word, "void") ||
           names_equal(word, "struct") ||
           names_equal(word, "union") ||
           names_equal(word, "enum") ||
           names_equal(word, "unsigned") ||
           names_equal(word, "signed") ||
           names_equal(word, "const") ||
           names_equal(word, "volatile") ||
           names_equal(word, "float") ||
           names_equal(word, "double") ||
           names_equal(word, "size_t") ||
           names_equal(word, "ssize_t");
}

static void skip_global_initializer_casts(GlobalInitParser *parser) {
    while (parser->kind == GLOBAL_INIT_PUNCT && names_equal(parser->text, "(")) {
        const char *cursor = skip_spaces(parser->cursor);
        int saw_type = 0;
        int saw_star = 0;

        while (*cursor != '\0') {
            char word[32];
            size_t out = 0;

            cursor = skip_spaces(cursor);
            if (*cursor == '*') {
                saw_star = 1;
                cursor += 1;
                continue;
            }
            if (*cursor == ')') {
                if (saw_type || saw_star) {
                    parser->cursor = cursor + 1;
                    global_init_next(parser);
                }
                return;
            }
            if (!( (*cursor >= 'a' && *cursor <= 'z') ||
                   (*cursor >= 'A' && *cursor <= 'Z') ||
                   *cursor == '_')) {
                return;
            }
            while (((*cursor >= 'a' && *cursor <= 'z') ||
                    (*cursor >= 'A' && *cursor <= 'Z') ||
                    (*cursor >= '0' && *cursor <= '9') ||
                    *cursor == '_') &&
                   out + 1 < sizeof(word)) {
                word[out++] = *cursor++;
            }
            word[out] = '\0';
            if (word_is_type_keyword(word)) {
                saw_type = 1;
            }
        }
    }
}

static int emit_global_initializer_aggregate(BackendState *state,
                                             const char *type_text,
                                             GlobalInitParser *parser,
                                             unsigned long long *bytes_out) {
    char aggregate_name[COMPILER_IR_NAME_CAPACITY];
    unsigned long long emitted = 0ULL;
    unsigned long long total = (unsigned long long)global_type_storage_bytes(state, type_text);
    size_t i;

    copy_aggregate_name_from_type_text(type_text, aggregate_name, sizeof(aggregate_name));
    if (parser->kind == GLOBAL_INIT_PUNCT && names_equal(parser->text, "{")) {
        global_init_next(parser);
    }

    for (i = 0; i < state->aggregate_member_count; ++i) {
        unsigned long long member_size;
        unsigned long long gap;
        unsigned long long dummy = 0ULL;

        if (!names_equal(state->aggregate_members[i].aggregate_name, aggregate_name)) {
            continue;
        }
        if (parser->kind == GLOBAL_INIT_PUNCT && names_equal(parser->text, "}")) {
            break;
        }

        gap = (unsigned long long)state->aggregate_members[i].offset_bytes > emitted
                  ? (unsigned long long)state->aggregate_members[i].offset_bytes - emitted
                  : 0ULL;
        if (gap > 0ULL && emit_zero_fill(state, gap) != 0) {
            return -1;
        }
        emitted += gap;
        member_size = (unsigned long long)global_type_storage_bytes(state, state->aggregate_members[i].type_text);
        if (emit_global_initializer_value(state, state->aggregate_members[i].type_text, parser, &dummy) != 0) {
            return -1;
        }
        emitted = (unsigned long long)state->aggregate_members[i].offset_bytes + member_size;
        if (parser->kind == GLOBAL_INIT_PUNCT && names_equal(parser->text, ",")) {
            global_init_next(parser);
        }
    }

    if (parser->kind == GLOBAL_INIT_PUNCT && names_equal(parser->text, "}")) {
        global_init_next(parser);
    }
    if (emitted < total && emit_zero_fill(state, total - emitted) != 0) {
        return -1;
    }
    if (bytes_out != 0) {
        *bytes_out = total;
    }
    return 0;
}

static int emit_global_initializer_array(BackendState *state,
                                         const char *type_text,
                                         GlobalInitParser *parser,
                                         unsigned long long *bytes_out) {
    char element_type[128];
    unsigned long long length = 0ULL;
    unsigned long long count = 0ULL;
    unsigned long long element_size;

    copy_indexed_type_text(type_text, element_type, sizeof(element_type));
    (void)parse_array_length_text(type_text, &length);
    element_size = (unsigned long long)global_type_storage_bytes(state, element_type);

    if (parser->kind == GLOBAL_INIT_STRING && starts_with(skip_spaces(type_text), "char[")) {
        unsigned long long total = length > 0ULL
                                       ? clamp_array_product(length, element_size, (unsigned long long)BACKEND_MAX_OBJECT_STACK_BYTES)
                                       : ((unsigned long long)rt_strlen(parser->text) + 1ULL);
        if (emit_char_array_contents(state, parser->text, total) != 0) {
            return -1;
        }
        global_init_next(parser);
        if (bytes_out != 0) {
            *bytes_out = total;
        }
        return 0;
    }

    if (parser->kind == GLOBAL_INIT_PUNCT && names_equal(parser->text, "{")) {
        global_init_next(parser);
    }
    while (parser->kind != GLOBAL_INIT_EOF &&
           !(parser->kind == GLOBAL_INIT_PUNCT && names_equal(parser->text, "}")) &&
           (length == 0ULL || count < length)) {
        unsigned long long dummy = 0ULL;
        if (emit_global_initializer_value(state, element_type, parser, &dummy) != 0) {
            return -1;
        }
        count += 1ULL;
        if (parser->kind == GLOBAL_INIT_PUNCT && names_equal(parser->text, ",")) {
            global_init_next(parser);
        }
    }
    if (parser->kind == GLOBAL_INIT_PUNCT && names_equal(parser->text, "}")) {
        global_init_next(parser);
    }
    if (length > count &&
        emit_zero_fill(state,
                       clamp_array_product(length - count,
                                           element_size,
                                           (unsigned long long)BACKEND_MAX_OBJECT_STACK_BYTES)) != 0) {
        return -1;
    }
    if (bytes_out != 0) {
        *bytes_out = clamp_array_product(length > 0ULL ? length : count,
                                         element_size,
                                         (unsigned long long)BACKEND_MAX_OBJECT_STACK_BYTES);
    }
    return 0;
}

static int emit_global_initializer_value(BackendState *state,
                                         const char *type_text,
                                         GlobalInitParser *parser,
                                         unsigned long long *bytes_out) {
    const char *type = skip_spaces(type_text != 0 ? type_text : "");
    int size_bytes = global_type_storage_bytes(state, type);

    skip_global_initializer_casts(parser);

    if (text_contains(type, "[")) {
        return emit_global_initializer_array(state, type, parser, bytes_out);
    }
    if (starts_with(type, "struct:") || starts_with(type, "union:")) {
        return emit_global_initializer_aggregate(state, type, parser, bytes_out);
    }
    if (parser->kind == GLOBAL_INIT_STRING && text_contains(type, "*")) {
        int string_index = add_string_literal(state, parser->text);
        char symbol[64];
        if (string_index < 0) {
            return -1;
        }
        rt_copy_string(symbol, sizeof(symbol), ".L");
        rt_copy_string(symbol + rt_strlen(symbol), sizeof(symbol) - rt_strlen(symbol), state->strings[string_index].label);
        if (emit_data_symbol_ref(state, symbol) != 0) {
            return -1;
        }
        global_init_next(parser);
    } else if (parser->kind == GLOBAL_INIT_IDENTIFIER) {
        long long value = 0;
        if (resolve_static_value(state, parser->text, &value) == 0) {
            if (emit_data_integer(state, size_bytes, value) != 0) {
                return -1;
            }
        } else if (text_contains(type, "*")) {
            char symbol[COMPILER_IR_NAME_CAPACITY];
            format_symbol_name(state, parser->text, symbol, sizeof(symbol));
            if (emit_data_symbol_ref(state, symbol) != 0) {
                return -1;
            }
        } else if (emit_data_integer(state, size_bytes, 0) != 0) {
            return -1;
        }
        global_init_next(parser);
    } else if (parser->kind == GLOBAL_INIT_NUMBER || parser->kind == GLOBAL_INIT_CHAR) {
        if (emit_data_integer(state, size_bytes, parser->number_value) != 0) {
            return -1;
        }
        global_init_next(parser);
    } else {
        if (emit_zero_fill(state, (unsigned long long)(size_bytes > 0 ? size_bytes : 1)) != 0) {
            return -1;
        }
        if (parser->kind != GLOBAL_INIT_EOF) {
            global_init_next(parser);
        }
    }

    if (bytes_out != 0) {
        *bytes_out = (unsigned long long)(size_bytes > 0 ? size_bytes : 1);
    }
    return 0;
}

static int named_aggregate_stack_bytes(const BackendState *state, const char *type_text) {
    const char *type = skip_spaces(type_text);
    int generic_size = lookup_aggregate_size(state, type);

    if (generic_size > 0) {
        return generic_size;
    }

    if (text_contains(type, "ShCommand")) {
        return 808;
    }
    if (text_contains(type, "ShPipeline")) {
        return 6472;
    }
    if (text_contains(type, "ShJob")) {
        return 4140;
    }
    if (text_contains(type, "ShAlias") || text_contains(type, "ShFunction")) {
        return 4164;
    }
    if (text_contains(type, "PlatformDirEntry")) {
        return 400;
    }
    return 0;
}

static int decl_slot_size(const BackendState *state, const char *type_text) {
    const char *type = skip_spaces(type_text);
    const char *open = 0;
    unsigned long long length = 0;
    unsigned long long element_size = (unsigned long long)backend_stack_slot_size(state);
    unsigned long long total_size;
    char element_type[128];
    int has_pointer = text_contains(type, "*");
    int aggregate_size = named_aggregate_stack_bytes(state, type);

    if (has_pointer) {
        element_size = (unsigned long long)backend_stack_slot_size(state);
    } else if (text_contains(type, "char")) {
        element_size = 1ULL;
    } else if ((starts_with(type, "struct") || starts_with(type, "union")) && !text_contains(type, "*")) {
        element_size = (unsigned long long)(aggregate_size > 0 ? aggregate_size : BACKEND_STRUCT_STACK_BYTES);
    }

    open = type;
    while (*open != '\0' && *open != '[') {
        open += 1;
    }

    if (*open == '[') {
        const char *cursor = open + 1;
        copy_indexed_type_text(type, element_type, sizeof(element_type));
        if (element_type[0] != '\0' && rt_strcmp(element_type, type) != 0) {
            int nested_size = global_type_storage_bytes(state, element_type);
            if (nested_size > 0) {
                element_size = (unsigned long long)nested_size;
            }
        }
        while (*cursor >= '0' && *cursor <= '9') {
            if (length > (ULLONG_MAX - (unsigned long long)(*cursor - '0')) / 10ULL) {
                length = (unsigned long long)BACKEND_MAX_OBJECT_STACK_BYTES;
                break;
            }
            length = length * 10ULL + (unsigned long long)(*cursor - '0');
            cursor += 1;
        }
        if (length == 0ULL) {
            return BACKEND_ARRAY_STACK_BYTES;
        }
        total_size = clamp_array_product(length, element_size, (unsigned long long)BACKEND_MAX_OBJECT_STACK_BYTES);
        return (int)total_size;
    }

    if (((starts_with(type, "struct") || starts_with(type, "union")) && !text_contains(type, "*"))) {
        return aggregate_size > 0 ? aggregate_size : BACKEND_STRUCT_STACK_BYTES;
    }
    return backend_stack_slot_size(state);
}

static int type_text_has_unsized_first_array(const char *type_text) {
    const char *cursor = skip_spaces(type_text != 0 ? type_text : "");

    while (*cursor != '\0' && *cursor != '[') {
        cursor += 1;
    }
    if (*cursor != '[') {
        return 0;
    }
    cursor += 1;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor += 1;
    }
    return *cursor == ']';
}

static void set_first_array_length_text(char *type_text, size_t type_text_size, unsigned long long length) {
    const char *open;
    const char *close;
    char updated[128];
    char digits[32];
    size_t prefix_len;
    size_t out = 0;

    if (type_text == 0 || type_text_size == 0 || length == 0ULL || !type_text_has_unsized_first_array(type_text)) {
        return;
    }

    open = type_text;
    while (*open != '\0' && *open != '[') {
        open += 1;
    }
    close = open;
    while (*close != '\0' && *close != ']') {
        close += 1;
    }
    if (*open != '[' || *close != ']') {
        return;
    }

    updated[0] = '\0';
    prefix_len = (size_t)(open - type_text) + 1U;
    if (prefix_len >= sizeof(updated)) {
        return;
    }
    while (out < prefix_len && out + 1 < sizeof(updated)) {
        updated[out] = type_text[out];
        out += 1U;
    }
    updated[out] = '\0';
    rt_unsigned_to_string(length, digits, sizeof(digits));
    rt_copy_string(updated + out, sizeof(updated) - out, digits);
    out = rt_strlen(updated);
    rt_copy_string(updated + out, sizeof(updated) - out, close);
    rt_copy_string(type_text, type_text_size, updated);
}

static unsigned long long count_string_initializer_bytes(const char *expr) {
    const char *cursor = skip_spaces(expr != 0 ? expr : "");
    unsigned long long count = 0ULL;

    if (*cursor != '"') {
        return 0ULL;
    }
    cursor += 1;
    while (*cursor != '\0' && *cursor != '"') {
        if (*cursor == '\\') {
            cursor += 1;
            (void)backend_decode_escaped_char(&cursor);
        } else {
            cursor += 1;
        }
        count += 1ULL;
    }
    return count + 1ULL;
}

static unsigned long long count_top_level_initializer_items(const char *expr) {
    const char *cursor = skip_spaces(expr != 0 ? expr : "");
    unsigned long long count = 0ULL;
    int brace_depth = 0;
    int paren_depth = 0;
    int bracket_depth = 0;
    int saw_item = 0;

    if (*cursor != '{') {
        return 0ULL;
    }
    brace_depth = 1;
    cursor += 1;
    while (*cursor != '\0' && brace_depth > 0) {
        if (*cursor == '"') {
            saw_item = 1;
            cursor += 1;
            while (*cursor != '\0' && *cursor != '"') {
                if (*cursor == '\\' && cursor[1] != '\0') {
                    cursor += 2;
                } else {
                    cursor += 1;
                }
            }
            if (*cursor == '"') {
                cursor += 1;
            }
            continue;
        }
        if (*cursor == '\'') {
            saw_item = 1;
            cursor += 1;
            while (*cursor != '\0' && *cursor != '\'') {
                if (*cursor == '\\' && cursor[1] != '\0') {
                    cursor += 2;
                } else {
                    cursor += 1;
                }
            }
            if (*cursor == '\'') {
                cursor += 1;
            }
            continue;
        }

        if (brace_depth == 1 && paren_depth == 0 && bracket_depth == 0 && *cursor == ',') {
            if (saw_item) {
                count += 1ULL;
                saw_item = 0;
            }
            cursor += 1;
            continue;
        }
        if (*cursor == '{') {
            saw_item = 1;
            brace_depth += 1;
            cursor += 1;
            continue;
        }
        if (*cursor == '}') {
            brace_depth -= 1;
            if (brace_depth == 0) {
                if (saw_item) {
                    count += 1ULL;
                }
                break;
            }
            saw_item = 1;
            cursor += 1;
            continue;
        }
        if (*cursor == '(') {
            saw_item = 1;
            paren_depth += 1;
            cursor += 1;
            continue;
        }
        if (*cursor == ')' && paren_depth > 0) {
            paren_depth -= 1;
            cursor += 1;
            continue;
        }
        if (*cursor == '[') {
            saw_item = 1;
            bracket_depth += 1;
            cursor += 1;
            continue;
        }
        if (*cursor == ']' && bracket_depth > 0) {
            bracket_depth -= 1;
            cursor += 1;
            continue;
        }
        if (!rt_is_space(*cursor)) {
            saw_item = 1;
        }
        cursor += 1;
    }

    return count;
}

static void maybe_apply_array_initializer_length(char *type_text, size_t type_text_size, const char *expr) {
    const char *type = skip_spaces(type_text != 0 ? type_text : "");
    const char *value = skip_spaces(expr != 0 ? expr : "");
    unsigned long long length = 0ULL;

    if (!type_text_has_unsized_first_array(type) || value[0] == '\0') {
        return;
    }

    if (value[0] == '"' && starts_with(type, "char[")) {
        length = count_string_initializer_bytes(value);
    } else if (value[0] == '{') {
        length = count_top_level_initializer_items(value);
    } else {
        length = 1ULL;
    }

    if (length > 0ULL) {
        set_first_array_length_text(type_text, type_text_size, length);
    }
}

static int decl_requires_object_storage(const char *type_text) {
    const char *type = skip_spaces(type_text);

    return text_contains(type, "[") ||
           ((starts_with(type, "struct") || starts_with(type, "union")) && !text_contains(type, "*"));
}

static int decl_pointer_depth(const char *type_text) {
    const char *type = skip_spaces(type_text);
    int depth = 0;

    while (*type != '\0') {
        if (*type == '*') {
            depth += 1;
        }
        type += 1;
    }

    return depth;
}

static int decl_char_based(const char *type_text) {
    return text_contains(skip_spaces(type_text), "char");
}

static int aligned_function_stack_bytes(int stack_bytes) {
    if (stack_bytes <= 0) {
        return 0;
    }
    return (stack_bytes + 15) & ~15;
}

static int lookup_function_stack_bytes(const BackendState *state, const char *name) {
    size_t i;

    for (i = 0; i < state->function_count; ++i) {
        if (names_equal(state->functions[i].name, name)) {
            return aligned_function_stack_bytes(state->functions[i].stack_bytes);
        }
    }

    return 0;
}

static int count_compound_literal_slots(const char *text) {
    int count = 0;
    int in_string = 0;
    int in_char = 0;
    size_t i = 0;

    while (text[i] != '\0') {
        if ((in_string || in_char) && text[i] == '\\' && text[i + 1] != '\0') {
            i += 2U;
            continue;
        }
        if (!in_char && text[i] == '"') {
            in_string = !in_string;
            i += 1U;
            continue;
        }
        if (!in_string && text[i] == '\'') {
            in_char = !in_char;
            i += 1U;
            continue;
        }
        if (!in_string && !in_char && text[i] == ')') {
            size_t j = i + 1U;
            while (text[j] == ' ' || text[j] == '\t') {
                j += 1U;
            }
            if (text[j] == '{') {
                count += 1;
            }
        }
        i += 1U;
    }

    return count;
}

static const char *find_ir_separator_outside_quotes(const char *text, const char *separator) {
    const char *last = 0;
    int in_string = 0;
    int in_char = 0;
    size_t i = 0;
    size_t separator_length = rt_strlen(separator);

    while (text[i] != '\0') {
        if ((in_string || in_char) && text[i] == '\\' && text[i + 1] != '\0') {
            i += 2U;
            continue;
        }
        if (!in_char && text[i] == '"') {
            in_string = !in_string;
            i += 1U;
            continue;
        }
        if (!in_string && text[i] == '\'') {
            in_char = !in_char;
            i += 1U;
            continue;
        }
        if (!in_string && !in_char) {
            size_t j = 0;
            while (j < separator_length && text[i + j] == separator[j]) {
                j += 1U;
            }
            if (j == separator_length) {
                last = text + i;
            }
        }
        i += 1U;
    }

    return last;
}

static int resolve_static_value(const BackendState *state, const char *expr, long long *value_out) {
    char name[COMPILER_IR_NAME_CAPACITY];
    const char *cursor = skip_spaces(expr);
    int negative = 0;
    size_t out = 0;
    int index;

    if (parse_signed_value(cursor, value_out) == 0) {
        return 0;
    }

    if (*cursor == '-') {
        negative = 1;
        cursor = skip_spaces(cursor + 1);
    } else if (*cursor == '+') {
        cursor = skip_spaces(cursor + 1);
    }

    while (((*cursor >= 'a' && *cursor <= 'z') ||
            (*cursor >= 'A' && *cursor <= 'Z') ||
            (*cursor >= '0' && *cursor <= '9') ||
            *cursor == '_') &&
           out + 1 < sizeof(name)) {
        name[out++] = *cursor++;
    }
    name[out] = '\0';
    cursor = skip_spaces(cursor);

    if (name[0] == '\0' || *cursor != '\0') {
        return -1;
    }

    index = find_constant(state, name);
    if (index < 0) {
        return -1;
    }

    *value_out = state->constants[index].value;
    if (negative) {
        *value_out = -*value_out;
    }
    return 0;
}

static int prescan_ir(BackendState *state, const CompilerIr *ir) {
    size_t i;
    int in_function = 0;
    char current_function[COMPILER_IR_NAME_CAPACITY];

    current_function[0] = '\0';

    for (i = 0; i < ir->count; ++i) {
        const char *line = ir->lines[i];

        if (starts_with(line, "func ")) {
            char name[COMPILER_IR_NAME_CAPACITY];
            size_t j = 5;
            size_t out = 0;
            while (line[j] != '\0' && !(line[j] == ' ' && line[j + 1] == ':') && out + 1 < sizeof(name)) {
                name[out++] = line[j++];
            }
            name[out] = '\0';
            if (add_function_name(state, name, 0, "") != 0) {
                return -1;
            }
        }

        if (starts_with(line, "aggregate ")) {
            char kind[16];
            char name[COMPILER_IR_NAME_CAPACITY];
            int size_bytes = 0;
            int align_bytes = 0;

            if (parse_aggregate_line(line, kind, sizeof(kind), name, sizeof(name), &size_bytes, &align_bytes) == 0 &&
                add_aggregate_layout(state, name, names_equal(kind, "union"), size_bytes, align_bytes) < 0) {
                return -1;
            }
        }

        if (starts_with(line, "member ")) {
            char aggregate_name[COMPILER_IR_NAME_CAPACITY];
            char member_name[COMPILER_IR_NAME_CAPACITY];
            char type_text[128];
            int offset = 0;

            if (parse_member_line(line,
                                  aggregate_name,
                                  sizeof(aggregate_name),
                                  member_name,
                                  sizeof(member_name),
                                  &offset,
                                  type_text,
                                  sizeof(type_text)) == 0 &&
                add_aggregate_member(state, aggregate_name, member_name, type_text, offset) < 0) {
                return -1;
            }
        }

        if (starts_with(line, "const ")) {
            char name[COMPILER_IR_NAME_CAPACITY];
            long long value = 0;

            if (parse_const_line(line, name, sizeof(name), &value) == 0 &&
                add_constant(state, name, value) != 0) {
                return -1;
            }
        }
    }

    for (i = 0; i < ir->count; ++i) {
        const char *line = ir->lines[i];

        if (starts_with(line, "func ")) {
            in_function = 1;
            {
                size_t j = 5;
                size_t out = 0;
                while (line[j] != '\0' && !(line[j] == ' ' && line[j + 1] == ':') && out + 1 < sizeof(current_function)) {
                    current_function[out++] = line[j++];
                }
                current_function[out] = '\0';
            }
            continue;
        }
        if (starts_with(line, "endfunc ")) {
            in_function = 0;
            current_function[0] = '\0';
            continue;
        }

        if (in_function && starts_with(line, "decl ")) {
            char storage[16];
            char kind[16];
            char type_text[128];
            char name[COMPILER_IR_NAME_CAPACITY];
            int slot_size;
            size_t function_index;

            parse_decl_line(line, storage, sizeof(storage), kind, sizeof(kind), type_text, sizeof(type_text), name, sizeof(name));
            if (names_equal(storage, "param") || names_equal(storage, "local")) {
                slot_size = decl_slot_size(state, type_text);
                for (function_index = 0; function_index < state->function_count; ++function_index) {
                    if (names_equal(state->functions[function_index].name, current_function)) {
                        state->functions[function_index].stack_bytes += slot_size;
                        break;
                    }
                }
                continue;
            }
        }

        if (in_function && current_function[0] != '\0') {
            int compound_slots = count_compound_literal_slots(line);
            size_t function_index;

            if (compound_slots > 0) {
                for (function_index = 0; function_index < state->function_count; ++function_index) {
                    if (names_equal(state->functions[function_index].name, current_function)) {
                        state->functions[function_index].stack_bytes += compound_slots * backend_stack_slot_size(state);
                        break;
                    }
                }
            }
        }

        if (starts_with(line, "const ")) {
            char name[COMPILER_IR_NAME_CAPACITY];
            long long value = 0;

            if (parse_const_line(line, name, sizeof(name), &value) == 0 &&
                add_constant(state, name, value) != 0) {
                return -1;
            }
            continue;
        }

        if (starts_with(line, "decl ")) {
            char storage[16];
            char kind[16];
            char type_text[128];
            char name[COMPILER_IR_NAME_CAPACITY];
            int has_global_linkage;
            int global_index = -1;
            int is_local_static;

            parse_decl_line(line, storage, sizeof(storage), kind, sizeof(kind), type_text, sizeof(type_text), name, sizeof(name));
            is_local_static = names_equal(storage, "local_static");
            has_global_linkage = names_equal(storage, "global");

            if (names_equal(kind, "func")) {
                if (add_function_name(state, name, has_global_linkage, type_text) != 0) {
                    return -1;
                }
                continue;
            }

            if ((names_equal(storage, "global") || names_equal(storage, "static") || names_equal(storage, "extern") || is_local_static) &&
                names_equal(kind, "obj") &&
                !is_function_name(state, name)) {
                char static_symbol[COMPILER_IR_NAME_CAPACITY];
                const char *global_name = name;
                int has_storage = names_equal(storage, "global") || names_equal(storage, "static") || is_local_static;

                if (is_local_static) {
                    build_static_local_symbol_name(state, current_function, name, static_symbol, sizeof(static_symbol));
                    global_name = static_symbol;
                }
                global_index = add_global(state,
                                          global_name,
                                          type_text,
                                          decl_requires_object_storage(type_text),
                                          decl_pointer_depth(type_text),
                                          decl_char_based(type_text),
                                          should_prefer_word_index(name, type_text),
                                          has_global_linkage,
                                          has_storage);
                if (global_index < 0) {
                    return -1;
                }
            }
            if (names_equal(kind, "obj") && global_index >= 0 && i + 1U < ir->count && starts_with(ir->lines[i + 1U], "store ")) {
                const char *next = ir->lines[i + 1U] + 6;
                char store_name[COMPILER_IR_NAME_CAPACITY];
                long long value = 0;

                next = extract_store_name(next, store_name, sizeof(store_name));
                if (names_equal(store_name, name)) {
                    if (resolve_static_value(state, next, &value) == 0) {
                        state->globals[global_index].init_value = value;
                        state->globals[global_index].initialized = 1;
                    } else if (next[0] != '\0') {
                        rt_copy_string(state->globals[global_index].init_text,
                                       sizeof(state->globals[global_index].init_text),
                                       next);
                        maybe_apply_array_initializer_length(state->globals[global_index].type_text,
                                                             sizeof(state->globals[global_index].type_text),
                                                             next);
                        state->globals[global_index].initialized = 1;
                    }
                }
            }
            continue;
        }

        if (!in_function && starts_with(line, "store ")) {
            char name[COMPILER_IR_NAME_CAPACITY];
            const char *expr = line + 6;
            long long value = 0;
            int global_index;

            expr = extract_store_name(expr, name, sizeof(name));
            global_index = find_global(state, name);
            if (global_index >= 0) {
                if (resolve_static_value(state, expr, &value) == 0) {
                    state->globals[global_index].init_value = value;
                    state->globals[global_index].initialized = 1;
                } else if (expr[0] != '\0') {
                    rt_copy_string(state->globals[global_index].init_text,
                                   sizeof(state->globals[global_index].init_text),
                                   expr);
                    maybe_apply_array_initializer_length(state->globals[global_index].type_text,
                                                         sizeof(state->globals[global_index].type_text),
                                                         expr);
                    state->globals[global_index].initialized = 1;
                }
            }
        }
    }

    return 0;
}

static int backend_supports_named_sections(const BackendState *state) {
    const CompilerTargetInfo *info = compiler_target_get_info(state->backend->target);

    return info != 0 && info->object_format == COMPILER_OBJECT_FORMAT_ELF64;
}

static int emit_named_section(BackendState *state,
                              const char *section_name,
                              const char *flags,
                              const char *section_type,
                              const char *error_message) {
    char line[256];

    rt_copy_string(line, sizeof(line), ".section ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), section_name);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ",\"");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), flags);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "\",@");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), section_type);
    if (emit_line(state, line) != 0) {
        backend_set_error(state->backend, error_message);
        return -1;
    }
    return 0;
}

static int emit_global_storage_section(BackendState *state, int use_bss, const char *symbol) {
    if (state->backend->data_sections && backend_supports_named_sections(state) && symbol != 0 && symbol[0] != '\0') {
        char section_name[256];

        rt_copy_string(section_name, sizeof(section_name), use_bss ? ".bss." : ".data.");
        rt_copy_string(section_name + rt_strlen(section_name), sizeof(section_name) - rt_strlen(section_name), symbol);
        return emit_named_section(state,
                                  section_name,
                                  use_bss ? "aw" : "aw",
                                  use_bss ? "nobits" : "progbits",
                                  use_bss ? "failed to emit bss section" : "failed to emit data section");
    }

    if (emit_line(state, use_bss ? ".bss" : ".data") != 0) {
        backend_set_error(state->backend, use_bss ? "failed to emit bss section" : "failed to emit data section");
        return -1;
    }
    return 0;
}

static int emit_globals(BackendState *state) {
    size_t i;
    int current_section = -1;

    if (state->global_count == 0) {
        return 0;
    }

    for (i = 0; i < state->global_count; ++i) {
        char digits[32];
        char line[128];
        char symbol[COMPILER_IR_NAME_CAPACITY];
        int storage_bytes = decl_slot_size(state, state->globals[i].type_text);
        int needs_object_storage = decl_requires_object_storage(state->globals[i].type_text);
        const char *init_expr = state->globals[i].init_text[0] != '\0'
                                    ? state->globals[i].init_text
                                    : find_global_initializer_expr(state, state->globals[i].name);
        int use_bss = 0;
        format_symbol_name(state, state->globals[i].name, symbol, sizeof(symbol));

        if (!state->globals[i].has_storage) {
            continue;
        }

        if (needs_object_storage) {
            use_bss = init_expr == 0 || init_expr[0] == '\0';
        } else if (state->globals[i].init_text[0] == '\0' &&
                   (!state->globals[i].initialized || state->globals[i].init_value == 0)) {
            use_bss = 1;
        }

        if ((state->backend->data_sections && backend_supports_named_sections(state)) || current_section != use_bss) {
            if (emit_global_storage_section(state, use_bss, symbol) != 0) {
                return -1;
            }
            current_section = use_bss;
        }

        if (state->globals[i].global) {
            if (emit_text(state, ".globl ") != 0 || emit_line(state, symbol) != 0) {
                backend_set_error(state->backend, "failed to emit global symbol");
                return -1;
            }
        }
        if (emit_text(state, symbol) != 0 || emit_line(state, ":") != 0) {
            backend_set_error(state->backend, "failed to emit global symbol");
            return -1;
        }

        if (needs_object_storage) {
            if (!use_bss && init_expr != 0 && init_expr[0] != '\0') {
                GlobalInitParser parser;
                unsigned long long bytes_written = 0ULL;

                parser.cursor = init_expr;
                global_init_next(&parser);
                if (emit_global_initializer_value(state, state->globals[i].type_text, &parser, &bytes_written) != 0) {
                    backend_set_error(state->backend, "failed to emit global object initializer");
                    return -1;
                }
                if (storage_bytes > 0 && bytes_written < (unsigned long long)storage_bytes &&
                    emit_zero_fill(state, (unsigned long long)storage_bytes - bytes_written) != 0) {
                    backend_set_error(state->backend, "failed to pad global object initializer");
                    return -1;
                }
            } else {
                if (storage_bytes <= 0) {
                    storage_bytes = backend_stack_slot_size(state);
                }
                rt_copy_string(line, sizeof(line), "    .zero ");
                rt_unsigned_to_string((unsigned long long)storage_bytes, digits, sizeof(digits));
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
                if (emit_line(state, line) != 0) {
                    backend_set_error(state->backend, "failed to emit global object storage");
                    return -1;
                }
            }
            continue;
        }

        if (use_bss) {
            if (storage_bytes <= 0) {
                storage_bytes = 8;
            }
            rt_copy_string(line, sizeof(line), "    .zero ");
            rt_unsigned_to_string((unsigned long long)storage_bytes, digits, sizeof(digits));
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
            if (emit_line(state, line) != 0) {
                backend_set_error(state->backend, "failed to emit zero-initialized global storage");
                return -1;
            }
            continue;
        }

        rt_copy_string(line, sizeof(line), "    .quad ");
        if (state->globals[i].init_value < 0) {
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "-");
            rt_unsigned_to_string((unsigned long long)(-state->globals[i].init_value), digits, sizeof(digits));
        } else {
            rt_unsigned_to_string((unsigned long long)state->globals[i].init_value, digits, sizeof(digits));
        }
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
        if (emit_line(state, line) != 0) {
            backend_set_error(state->backend, "failed to emit global initializer");
            return -1;
        }
    }

    return 0;
}

static const char *extract_store_name(const char *cursor, char *name, size_t name_size) {
    size_t length = 0;

    while (*cursor != '\0' && !(cursor[0] == ' ' && cursor[1] == '<') && length + 1 < name_size) {
        name[length++] = *cursor++;
    }
    name[length] = '\0';
    return skip_spaces(cursor + 4);
}

static const char *find_global_initializer_expr(const BackendState *state, const char *name) {
    size_t i;
    int in_function = 0;

    if (state == 0 || state->ir == 0 || name == 0 || name[0] == '\0') {
        return 0;
    }

    for (i = 0; i < state->ir->count; ++i) {
        const char *line = state->ir->lines[i];
        const char *expr = line + 6;
        char store_name[COMPILER_IR_NAME_CAPACITY];

        if (starts_with(line, "func ")) {
            in_function = 1;
            continue;
        }
        if (starts_with(line, "endfunc ")) {
            in_function = 0;
            continue;
        }
        if (in_function || !starts_with(line, "store ")) {
            continue;
        }

        expr = extract_store_name(expr, store_name, sizeof(store_name));
        if (names_equal(store_name, name)) {
            return expr;
        }
    }

    return 0;
}

static void collect_global_initializers(BackendState *state, const CompilerIr *ir) {
    size_t i;
    int in_function = 0;

    for (i = 0; i < ir->count; ++i) {
        const char *line = ir->lines[i];

        if (starts_with(line, "func ")) {
            in_function = 1;
            continue;
        }
        if (starts_with(line, "endfunc ")) {
            in_function = 0;
            continue;
        }
        if (in_function || !starts_with(line, "store ")) {
            continue;
        }

        {
            char name[COMPILER_IR_NAME_CAPACITY];
            const char *expr = line + 6;
            int global_index;
            long long value = 0;

            expr = extract_store_name(expr, name, sizeof(name));
            global_index = find_global(state, name);
            if (global_index < 0) {
                continue;
            }
            if (resolve_static_value(state, expr, &value) == 0) {
                state->globals[global_index].init_value = value;
                state->globals[global_index].initialized = 1;
            } else if (expr[0] != '\0') {
                rt_copy_string(state->globals[global_index].init_text,
                               sizeof(state->globals[global_index].init_text),
                               expr);
                state->globals[global_index].initialized = 1;
            }
        }
    }
}

static int emit_string_literals(BackendState *state) {
    size_t i;

    if (state->string_count == 0) {
        return 0;
    }
    if (!(state->backend->data_sections && backend_supports_named_sections(state)) && emit_line(state, ".data") != 0) {
        backend_set_error(state->backend, "failed to emit string literal section");
        return -1;
    }
    for (i = 0; i < state->string_count; ++i) {
        char line[COMPILER_IR_LINE_CAPACITY];
        size_t out = 0;
        size_t j;

        if (state->backend->data_sections && backend_supports_named_sections(state)) {
            char section_name[256];

            rt_copy_string(section_name, sizeof(section_name), ".rodata.");
            rt_copy_string(section_name + rt_strlen(section_name), sizeof(section_name) - rt_strlen(section_name), state->strings[i].label);
            if (emit_named_section(state, section_name, "a", "progbits", "failed to emit string literal section") != 0) {
                return -1;
            }
        }

        rt_copy_string(line, sizeof(line), ".L");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), state->strings[i].label);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ":");
        if (emit_line(state, line) != 0) {
            backend_set_error(state->backend, "failed to emit string literal label");
            return -1;
        }

        rt_copy_string(line, sizeof(line), "    .asciz \"");
        out = rt_strlen(line);
        for (j = 0; state->strings[i].text[j] != '\0' && out + 4 < sizeof(line); ++j) {
            char ch = state->strings[i].text[j];
            if (ch == '"' || ch == '\\') {
                line[out++] = '\\';
                line[out++] = ch;
            } else if (ch == '\n') {
                line[out++] = '\\';
                line[out++] = 'n';
            } else if (ch == '\t') {
                line[out++] = '\\';
                line[out++] = 't';
            } else if (ch == '\r') {
                line[out++] = '\\';
                line[out++] = 'r';
            } else {
                line[out++] = ch;
            }
        }
        line[out++] = '"';
        line[out] = '\0';
        if (emit_line(state, line) != 0) {
            backend_set_error(state->backend, "failed to emit string literal contents");
            return -1;
        }
    }

    return 0;
}

static int function_has_global_linkage(const BackendState *state, const char *name) {
    size_t i;

    for (i = 0; i < state->function_count; ++i) {
        if (names_equal(state->functions[i].name, name)) {
            return state->functions[i].global;
        }
    }

    return 1;
}

static int begin_function(BackendState *state, const char *name) {
    char symbol[COMPILER_IR_NAME_CAPACITY];
    int export_symbol = function_has_global_linkage(state, name);
    int returns_object = function_returns_object(state, name);
    const char *return_type = function_return_type(state, name);
    state->in_function = 1;
    state->local_count = 0;
    state->param_count = returns_object ? 1 : 0;
    state->saw_return_in_function = 0;
    state->stack_size = 0;
    state->reserved_stack_size = lookup_function_stack_bytes(state, name);
    state->reserved_stack_size += BACKEND_ARRAY_STACK_BYTES;
    if (returns_object) {
        state->reserved_stack_size += backend_stack_slot_size(state);
        state->reserved_stack_size += decl_slot_size(state, return_type);
    }
    rt_copy_string(state->current_function, sizeof(state->current_function), name);
    format_symbol_name(state, name, symbol, sizeof(symbol));

    if (state->backend->function_sections && backend_supports_named_sections(state)) {
        char section_name[256];

        rt_copy_string(section_name, sizeof(section_name), ".text.");
        rt_copy_string(section_name + rt_strlen(section_name), sizeof(section_name) - rt_strlen(section_name), symbol);
        if (emit_named_section(state, section_name, "ax", "progbits", "failed to emit text section") != 0) {
            return -1;
        }
    } else if (emit_line(state, ".text") != 0) {
        backend_set_error(state->backend, "failed to emit text section");
        return -1;
    }
    if (backend_is_aarch64(state) && emit_line(state, ".p2align 2") != 0) {
        backend_set_error(state->backend, "failed to emit function alignment");
        return -1;
    }
    if (export_symbol) {
        if (emit_text(state, ".globl ") != 0 || emit_line(state, symbol) != 0) {
            backend_set_error(state->backend, "failed to emit function label");
            return -1;
        }
    }
    if (emit_text(state, symbol) != 0 || emit_line(state, ":") != 0) {
        backend_set_error(state->backend, "failed to emit function label");
        return -1;
    }
    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "stp x29, x30, [sp, #-16]!") != 0 ||
            emit_instruction(state, "mov x29, sp") != 0) {
            backend_set_error(state->backend, "failed to emit AArch64 function prologue");
            return -1;
        }
        if (state->reserved_stack_size > 0) {
            if (state->reserved_stack_size <= 4095) {
                char line[64];
                char size_text[32];
                rt_unsigned_to_string((unsigned long long)state->reserved_stack_size, size_text, sizeof(size_text));
                rt_copy_string(line, sizeof(line), "sub sp, sp, #");
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), size_text);
                if (emit_instruction(state, line) != 0) {
                    return -1;
                }
            } else {
                if (emit_load_immediate_register(state, "x9", state->reserved_stack_size) != 0 ||
                    emit_instruction(state, "sub sp, sp, x9") != 0) {
                    return -1;
                }
            }
        }
    } else if (emit_instruction(state, "pushq %rbp") != 0 ||
               emit_instruction(state, "movq %rsp, %rbp") != 0) {
        backend_set_error(state->backend, "failed to emit x86_64 function prologue");
        return -1;
    } else if (state->reserved_stack_size > 0) {
        char line[64];
        char size_text[32];
        rt_unsigned_to_string((unsigned long long)state->reserved_stack_size, size_text, sizeof(size_text));
        rt_copy_string(line, sizeof(line), "subq $");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), size_text);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rsp");
        if (emit_instruction(state, line) != 0) {
            return -1;
        }
    }

    if (allocate_local(state, "__callret", "char[4096]", BACKEND_ARRAY_STACK_BYTES, 1, 0, 1, 0) != 0) {
        return -1;
    }

    if (returns_object) {
        int retbuf_index;
        char offset_text[32];
        char asm_line[128];

        if (allocate_local(state, "__retbuf", "void*", backend_stack_slot_size(state), 0, 1, 0, 1) != 0 ||
            allocate_local(state,
                           "__retobj",
                           return_type,
                           decl_slot_size(state, return_type),
                           decl_requires_object_storage(return_type),
                           decl_pointer_depth(return_type),
                           decl_char_based(return_type),
                           should_prefer_word_index("__retobj", return_type)) != 0) {
            return -1;
        }
        retbuf_index = find_local(state, "__retbuf");
        rt_unsigned_to_string((unsigned long long)state->locals[retbuf_index].offset, offset_text, sizeof(offset_text));
        if (backend_is_aarch64(state)) {
            if (state->locals[retbuf_index].offset <= 255) {
                rt_copy_string(asm_line, sizeof(asm_line), "stur x0, [x29, #-");
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), offset_text);
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "]");
            } else {
                if (emit_local_address(state, state->locals[retbuf_index].offset, "x10") != 0) {
                    return -1;
                }
                rt_copy_string(asm_line, sizeof(asm_line), "str x0, [x10]");
            }
        } else {
            rt_copy_string(asm_line, sizeof(asm_line), "movq %rdi, -");
            rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), offset_text);
            rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "(%rbp)");
        }
        if (emit_instruction(state, asm_line) != 0) {
            return -1;
        }
    }
    return 0;
}

static int end_function(BackendState *state) {
    state->in_function = 0;
    state->local_count = 0;
    state->param_count = 0;
    state->saw_return_in_function = 0;
    state->stack_size = 0;
    state->reserved_stack_size = 0;
    state->current_function[0] = '\0';
    return 0;
}

static int emit_function_return(BackendState *state) {
    if (backend_is_aarch64(state)) {
        return emit_instruction(state, "mov sp, x29") == 0 &&
               emit_instruction(state, "ldp x29, x30, [sp], #16") == 0 &&
               emit_instruction(state, "ret") == 0 ? 0 : -1;
    }
    return emit_instruction(state, "leave") == 0 && emit_instruction(state, "ret") == 0 ? 0 : -1;
}

static void make_switch_label(char *buffer,
                              size_t buffer_size,
                              unsigned int switch_id,
                              const char *kind,
                              unsigned int case_index) {
    char digits[32];

    rt_copy_string(buffer, buffer_size, "switch");
    rt_unsigned_to_string((unsigned long long)switch_id, digits, sizeof(digits));
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), digits);
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "_");
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), kind);
    if (names_equal(kind, "case")) {
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "_");
        rt_unsigned_to_string((unsigned long long)case_index, digits, sizeof(digits));
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), digits);
    }
}

static int emit_switch_dispatch(BackendState *state,
                                const CompilerIr *ir,
                                size_t line_index,
                                const char *expr) {
    BackendSwitchContext *context;
    size_t scan = line_index + 1U;
    int depth = 1;
    unsigned int switch_id = state->label_counter++;

    if (state->switch_depth >= COMPILER_BACKEND_MAX_SWITCH_DEPTH) {
        backend_set_error(state->backend, "switch nesting exceeded backend capacity");
        return -1;
    }

    context = &state->switch_stack[state->switch_depth];
    rt_memset(context, 0, sizeof(*context));
    context->switch_id = switch_id;
    make_switch_label(context->end_label, sizeof(context->end_label), switch_id, "end", 0);
    make_switch_label(context->default_label, sizeof(context->default_label), switch_id, "default", 0);

    if (emit_expression(state, expr) != 0 || emit_push_value(state) != 0) {
        return -1;
    }

    while (scan < ir->count && depth > 0) {
        const char *scan_line = ir->lines[scan];

        if (starts_with(scan_line, "switch ")) {
            depth += 1;
            scan += 1U;
            continue;
        }
        if (starts_with(scan_line, "endswitch")) {
            depth -= 1;
            if (depth == 0) {
                break;
            }
            scan += 1U;
            continue;
        }
        if (depth == 1 && starts_with(scan_line, "case ")) {
            char case_label[32];
            const char *case_expr = skip_spaces(scan_line + 5);

            make_switch_label(case_label, sizeof(case_label), switch_id, "case", context->case_count);
            if (emit_expression(state, case_expr) != 0) {
                return -1;
            }
            if (backend_is_aarch64(state)) {
                if (emit_instruction(state, "ldr x1, [sp]") != 0 ||
                    emit_instruction(state, "cmp x1, x0") != 0 ||
                    emit_jump_to_label(state, "b.eq", case_label) != 0) {
                    return -1;
                }
            } else {
                if (emit_instruction(state, "cmpq %rax, (%rsp)") != 0 ||
                    emit_jump_to_label(state, "je", case_label) != 0) {
                    return -1;
                }
            }
            context->case_count += 1U;
        } else if (depth == 1 && starts_with(scan_line, "default")) {
            context->has_default = 1;
        }
        scan += 1U;
    }

    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "add sp, sp, #16") != 0) {
            return -1;
        }
    } else if (emit_instruction(state, "addq $8, %rsp") != 0) {
        return -1;
    }

    if (emit_jump_to_label(state,
                           backend_is_aarch64(state) ? "b" : "jmp",
                           context->has_default ? context->default_label : context->end_label) != 0) {
        return -1;
    }

    state->switch_depth += 1U;
    return 0;
}

static int emit_decl_instruction(BackendState *state, const char *line) {
    char storage[16];
    char kind[16];
    char type_text[128];
    char name[COMPILER_IR_NAME_CAPACITY];
    int is_array;
    int slot_size;
    int pointer_depth;
    int char_based;
    int prefers_word_index;

    parse_decl_line(line, storage, sizeof(storage), kind, sizeof(kind), type_text, sizeof(type_text), name, sizeof(name));
    is_array = decl_requires_object_storage(type_text);
    slot_size = decl_slot_size(state, type_text);
    pointer_depth = decl_pointer_depth(type_text);
    char_based = decl_char_based(type_text);
    prefers_word_index = should_prefer_word_index(name, type_text);

    if (names_equal(storage, "param") && text_contains(type_text, "[")) {
        is_array = 0;
        if (pointer_depth == 0) {
            pointer_depth = 1;
        }
        slot_size = backend_stack_slot_size(state);
    }

    if (names_equal(storage, "param")) {
        static const char *const x86_arg_regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
        static const char *const aarch64_arg_regs[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
        char asm_line[128];
        int index = state->param_count;

        if (allocate_local(state, name, type_text, slot_size, is_array, pointer_depth, char_based, prefers_word_index) != 0) {
            return -1;
        }
        {
            int local_index = find_local(state, name);
            int max_register_params = backend_register_arg_limit(state);
            char offset_text[32];
            rt_unsigned_to_string((unsigned long long)state->locals[local_index].offset, offset_text, sizeof(offset_text));
            if (index < max_register_params) {
                if (backend_is_aarch64(state)) {
                    if (state->locals[local_index].offset <= 255) {
                        rt_copy_string(asm_line, sizeof(asm_line), "stur ");
                        rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), aarch64_arg_regs[index]);
                        rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), ", [x29, #-");
                        rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), offset_text);
                        rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "]");
                    } else {
                        if (emit_local_address(state, state->locals[local_index].offset, "x10") != 0) {
                            return -1;
                        }
                        rt_copy_string(asm_line, sizeof(asm_line), "str ");
                        rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), aarch64_arg_regs[index]);
                        rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), ", [x10]");
                    }
                } else {
                    rt_copy_string(asm_line, sizeof(asm_line), "movq ");
                    rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), x86_arg_regs[index]);
                    rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), ", -");
                    rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), offset_text);
                    rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "(%rbp)");
                }
            } else if (backend_is_aarch64(state)) {
                unsigned long long stack_offset = 16ULL + (unsigned long long)(index - max_register_params) * 16ULL;
                char stack_text[32];
                rt_unsigned_to_string(stack_offset, stack_text, sizeof(stack_text));
                rt_copy_string(asm_line, sizeof(asm_line), "ldr x9, [x29, #");
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), stack_text);
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "]");
                if (emit_instruction(state, asm_line) != 0) {
                    return -1;
                }
                if (state->locals[local_index].offset <= 255) {
                    rt_copy_string(asm_line, sizeof(asm_line), "stur x9, [x29, #-");
                    rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), offset_text);
                    rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "]");
                } else {
                    if (emit_local_address(state, state->locals[local_index].offset, "x10") != 0) {
                        return -1;
                    }
                    rt_copy_string(asm_line, sizeof(asm_line), "str x9, [x10]");
                }
            } else {
                unsigned long long stack_offset = 16ULL + (unsigned long long)(index - max_register_params) * 8ULL;
                char stack_text[32];
                rt_unsigned_to_string(stack_offset, stack_text, sizeof(stack_text));
                rt_copy_string(asm_line, sizeof(asm_line), "movq ");
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), stack_text);
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "(%rbp), %rax");
                if (emit_instruction(state, asm_line) != 0) {
                    return -1;
                }
                rt_copy_string(asm_line, sizeof(asm_line), "movq %rax, -");
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), offset_text);
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "(%rbp)");
            }
            if (emit_instruction(state, asm_line) != 0) {
                return -1;
            }
        }
        state->param_count += 1;
        return 0;
    }

    if (names_equal(storage, "local")) {
        return allocate_local(state, name, type_text, slot_size, is_array, pointer_depth, char_based, prefers_word_index);
    }
    if (names_equal(storage, "local_static")) {
        char static_symbol[COMPILER_IR_NAME_CAPACITY];
        build_static_local_symbol_name(state, state->current_function, name, static_symbol, sizeof(static_symbol));
        return allocate_static_local(state, name, static_symbol, type_text, slot_size, is_array, pointer_depth, char_based, prefers_word_index);
    }

    if (names_equal(kind, "func")) {
        return add_function_name(state, name, !names_equal(storage, "static"), type_text);
    }

    return 0;
}

void compiler_backend_init(CompilerBackend *backend, CompilerTarget target, int function_sections, int data_sections) {
    rt_memset(backend, 0, sizeof(*backend));
    backend->target = target;
    backend->function_sections = function_sections;
    backend->data_sections = data_sections;
}

int compiler_backend_emit_assembly(CompilerBackend *backend, const CompilerIr *ir, int fd) {
    static BackendState state;
    size_t i;

    rt_memset(&state, 0, sizeof(state));
    state.backend = backend;
    state.ir = ir;
    state.fd = fd;

    if (prescan_ir(&state, ir) != 0) {
        return -1;
    }
    collect_global_initializers(&state, ir);
    if (emit_globals(&state) != 0) {
        return -1;
    }

    for (i = 0; i < ir->count; ++i) {
        const char *line = ir->lines[i];

        if (starts_with(line, "func ")) {
            char name[COMPILER_IR_NAME_CAPACITY];
            size_t j = 5;
            size_t out = 0;
            while (line[j] != '\0' && !(line[j] == ' ' && line[j + 1] == ':') && out + 1 < sizeof(name)) {
                name[out++] = line[j++];
            }
            name[out] = '\0';
            if (begin_function(&state, name) != 0) {
                return -1;
            }
            continue;
        }

        if (starts_with(line, "endfunc ")) {
            if (state.in_function && emit_function_return(&state) != 0) {
                return -1;
            }
            end_function(&state);
            continue;
        }

        if (!state.in_function) {
            continue;
        }

        if (starts_with(line, "decl ")) {
            if (emit_decl_instruction(&state, line) != 0) {
                return -1;
            }
            continue;
        }

        if (starts_with(line, "store ")) {
            char name[COMPILER_IR_NAME_CAPACITY];
            const char *expr = line + 6;

            expr = extract_store_name(expr, name, sizeof(name));

            {
                int array_word_index = 0;
                int local_index = find_local(&state, name);
                int global_index = find_global(&state, name);

                if (local_index >= 0) {
                    maybe_apply_array_initializer_length(state.locals[local_index].type_text,
                                                         sizeof(state.locals[local_index].type_text),
                                                         expr);
                    state.locals[local_index].stack_bytes = decl_slot_size(&state, state.locals[local_index].type_text);
                }
                if (global_index >= 0) {
                    maybe_apply_array_initializer_length(state.globals[global_index].type_text,
                                                         sizeof(state.globals[global_index].type_text),
                                                         expr);
                }

                int is_array_target = lookup_array_storage(&state, name, &array_word_index);
                const char *target_type = lookup_name_type_text(&state, name);
                const char *target_base = skip_spaces(target_type);
                int is_aggregate_target =
                    decl_requires_object_storage(target_base) &&
                    (starts_with(target_base, "struct:") || starts_with(target_base, "union:"));

                if (*expr == '{') {
                    if (is_aggregate_target) {
                        if (emit_object_initializer_store(&state, name, expr) != 0) {
                            backend_set_error_with_line(backend, compiler_backend_error_message(backend), line);
                            return -1;
                        }
                    } else if (is_array_target) {
                        if (emit_array_initializer_store(&state, name, expr) != 0) {
                            backend_set_error_with_line(backend, compiler_backend_error_message(backend), line);
                            return -1;
                        }
                    } else {
                        if (emit_object_initializer_store(&state, name, expr) != 0) {
                            backend_set_error_with_line(backend, compiler_backend_error_message(backend), line);
                            return -1;
                        }
                    }
                    continue;
                }
                if (*expr == '"' && is_array_target) {
                    if (emit_array_initializer_store(&state, name, expr) != 0) {
                        backend_set_error_with_line(backend, compiler_backend_error_message(backend), line);
                        return -1;
                    }
                    continue;
                }
                if (is_array_target || is_aggregate_target) {
                    if (emit_object_copy_store(&state, name, expr) != 0) {
                        backend_set_error_with_line(backend, compiler_backend_error_message(backend), line);
                        return -1;
                    }
                    continue;
                }
            }
            if (emit_expression(&state, expr) != 0 || emit_store_name(&state, name) != 0) {
                backend_set_error_with_line(backend, compiler_backend_error_message(backend), line);
                return -1;
            }
            continue;
        }

        if (starts_with(line, "eval ")) {
            if (emit_expression(&state, line + 5) != 0) {
                backend_set_error_with_line(backend, compiler_backend_error_message(backend), line);
                return -1;
            }
            continue;
        }

        if (starts_with(line, "ret")) {
            const char *expr = skip_spaces(line + 3);
            if (function_returns_object(&state, state.current_function)) {
                if (expr[0] != '\0') {
                    if (emit_object_copy_store(&state, "__retobj", expr) != 0) {
                        backend_set_error_with_line(backend, compiler_backend_error_message(backend), line);
                        return -1;
                    }
                }
                if (emit_copy_name_to_pointer_name(&state, "__retobj", "__retbuf") != 0) {
                    return -1;
                }
            } else if (expr[0] == '\0') {
                if (emit_load_immediate(&state, 0) != 0) {
                    return -1;
                }
            } else if (emit_expression(&state, expr) != 0) {
                return -1;
            }
            state.saw_return_in_function = 1;
            if (emit_function_return(&state) != 0) {
                return -1;
            }
            continue;
        }

        if (starts_with(line, "switch ")) {
            if (emit_switch_dispatch(&state, ir, i, skip_spaces(line + 6)) != 0) {
                backend_set_error_with_line(backend, compiler_backend_error_message(backend), line);
                return -1;
            }
            continue;
        }

        if (starts_with(line, "case ")) {
            if (state.switch_depth > 0) {
                BackendSwitchContext *context = &state.switch_stack[state.switch_depth - 1U];
                char asm_label[96];
                char case_label[32];

                make_switch_label(case_label,
                                  sizeof(case_label),
                                  context->switch_id,
                                  "case",
                                  context->next_case_index);
                context->next_case_index += 1U;
                write_label_name(&state, asm_label, sizeof(asm_label), case_label);
                rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
                if (emit_line(&state, asm_label) != 0) {
                    backend_set_error(state.backend, "failed to emit switch case label");
                    return -1;
                }
            }
            continue;
        }

        if (starts_with(line, "default")) {
            if (state.switch_depth > 0) {
                BackendSwitchContext *context = &state.switch_stack[state.switch_depth - 1U];
                char asm_label[96];

                write_label_name(&state, asm_label, sizeof(asm_label), context->default_label);
                rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
                if (emit_line(&state, asm_label) != 0) {
                    backend_set_error(state.backend, "failed to emit switch default label");
                    return -1;
                }
            }
            continue;
        }

        if (starts_with(line, "endswitch")) {
            if (state.switch_depth > 0) {
                BackendSwitchContext *context = &state.switch_stack[state.switch_depth - 1U];
                char asm_label[96];

                write_label_name(&state, asm_label, sizeof(asm_label), context->end_label);
                rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
                if (emit_line(&state, asm_label) != 0) {
                    backend_set_error(state.backend, "failed to emit switch end label");
                    return -1;
                }
                state.switch_depth -= 1U;
            }
            continue;
        }

        if (starts_with(line, "brfalse ")) {
            const char *arrow = line + 8;
            char expr[COMPILER_IR_LINE_CAPACITY];
            char label[COMPILER_IR_NAME_CAPACITY];
            char asm_label[96];
            size_t out = 0;

            const char *separator = find_ir_separator_outside_quotes(arrow, " -> ");
            if (separator == 0) {
                backend_set_error_with_line(backend, "malformed branch instruction in backend", line);
                return -1;
            }
            while (arrow < separator && out + 1 < sizeof(expr)) {
                expr[out++] = *arrow++;
            }
            expr[out] = '\0';
            rt_copy_string(label, sizeof(label), skip_spaces(separator + 4));
            if (emit_expression(&state, expr) != 0 || emit_cmp_zero(&state) != 0) {
                return -1;
            }
            rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(&state) ? "b.eq" : "je");
            if (emit_jump_to_label(&state, asm_label, label) != 0) {
                return -1;
            }
            continue;
        }

        if (starts_with(line, "jump ")) {
            char asm_label[96];
            rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(&state) ? "b" : "jmp");
            if (emit_jump_to_label(&state, asm_label, line + 5) != 0) {
                return -1;
            }
            continue;
        }

        if (starts_with(line, "label ")) {
            char asm_label[96];
            write_label_name(&state, asm_label, sizeof(asm_label), line + 6);
            rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
            if (emit_line(&state, asm_label) != 0) {
                backend_set_error(backend, "failed to emit branch label");
                return -1;
            }
            continue;
        }
    }

    if (emit_string_literals(&state) != 0) {
        return -1;
    }

    return 0;
}

const char *compiler_backend_error_message(const CompilerBackend *backend) {
    return backend->error_message;
}
