/* Declaration layout and global data emission helpers. */

#include "backend_internal.h"

#include <limits.h>

static const char *find_global_initializer_expr(const BackendState *state, const char *name);
int resolve_static_value(const BackendState *state, const char *expr, long long *value_out);

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
    size_t text_length;
    long long number_value;
} GlobalInitParser;

static void global_init_next(GlobalInitParser *parser) {
    const char *cursor = skip_spaces(parser->cursor);
    size_t length = 0;

    parser->cursor = cursor;
    parser->text[0] = '\0';
    parser->text_length = 0U;
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
        parser->text_length = length;
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

static int emit_char_array_contents(BackendState *state, const char *text, size_t text_length, unsigned long long total_size) {
    unsigned long long i;

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
                                       : ((unsigned long long)parser->text_length + 1ULL);
        if (emit_char_array_contents(state, parser->text, parser->text_length, total) != 0) {
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
        int string_index = add_string_literal_bytes(state, parser->text, parser->text_length);
        char symbol[64];
        if (string_index < 0) {
            return -1;
        }
        rt_copy_string(symbol, sizeof(symbol), backend_private_label_prefix(state));
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

    if (backend_is_darwin(state)) {
        if (text_contains(type, "addrinfo")) return 48;
        if (text_contains(type, "sockaddr_in6")) return 28;
        if (text_contains(type, "sockaddr_in")) return 16;
        if (text_contains(type, "sockaddr")) return 16;
        if (text_contains(type, "in6_addr")) return 16;
        if (text_contains(type, "in_addr")) return 4;
        if (text_contains(type, "termios")) return 72;
    } else {
        if (text_contains(type, "addrinfo")) return 48;
        if (text_contains(type, "sockaddr_in6")) return 28;
        if (text_contains(type, "sockaddr_in")) return 16;
        if (text_contains(type, "sockaddr")) return 16;
        if (text_contains(type, "in6_addr")) return 16;
        if (text_contains(type, "in_addr")) return 4;
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

int decl_slot_size(const BackendState *state, const char *type_text) {
    const char *type = skip_spaces(type_text);
    const char *open = 0;
    unsigned long long length = 0;
    unsigned long long element_size = (unsigned long long)backend_stack_slot_size(state);
    unsigned long long total_size;
    char element_type[128];
    int has_pointer = text_contains(type, "*");
    int aggregate_size = named_aggregate_stack_bytes(state, type);

    if (backend_type_is_pointer_like(type)) {
        return backend_stack_slot_size(state);
    }

    if (has_pointer) {
        element_size = 8ULL;
    } else if (text_contains(type, "__int128")) {
        element_size = 16ULL;
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
    if (text_contains(type, "__int128") && !text_contains(type, "*")) {
        return 16;
    }
    return backend_stack_slot_size(state);
}

int decl_alignment_bytes(const BackendState *state, const char *type_text) {
    const char *type = skip_spaces(type_text != 0 ? type_text : "");
    const char *open = type;
    char element_type[128];

    if (backend_type_is_pointer_like(type)) {
        return backend_stack_slot_size(state);
    }

    while (*open != '\0' && *open != '[') {
        open += 1;
    }
    if (*open == '[') {
        copy_indexed_type_text(type, element_type, sizeof(element_type));
        if (element_type[0] != '\0' && rt_strcmp(element_type, type) != 0) {
            return decl_alignment_bytes(state, element_type);
        }
    }

    if (text_contains(type, "*")) {
        return 8;
    }

    if (text_contains(type, "char")) {
        return 1;
    }
    if (text_contains(type, "short")) {
        return 2;
    }
    if (text_contains(type, "int") || starts_with(type, "enum")) {
        return 4;
    }
    if (text_contains(type, "long") || text_contains(type, "double")) {
        return backend_stack_slot_size(state);
    }
    if (starts_with(type, "struct") || starts_with(type, "union")) {
        return backend_stack_slot_size(state);
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

void maybe_apply_array_initializer_length(char *type_text, size_t type_text_size, const char *expr) {
    const char *type = skip_spaces(type_text != 0 ? type_text : "");
    const char *value = skip_spaces(expr != 0 ? expr : "");
    unsigned long long length = 0ULL;

    if (!type_text_has_unsized_first_array(type) || value[0] == '\0') {
        return;
    }

    if (value[0] == '"' &&
        (starts_with(type, "char[") ||
         starts_with(type, "signed char[") ||
         starts_with(type, "unsigned char["))) {
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

int decl_requires_object_storage(const char *type_text) {
    const char *type = skip_spaces(type_text);

    if (backend_type_is_pointer_like(type)) {
        return 0;
    }

    return text_contains(type, "[") ||
           ((starts_with(type, "struct") || starts_with(type, "union")) && !text_contains(type, "*"));
}

int decl_pointer_depth(const char *type_text) {
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

int decl_char_based(const char *type_text) {
    return text_contains(skip_spaces(type_text), "char");
}

int resolve_static_value(const BackendState *state, const char *expr, long long *value_out) {
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

    if (names_equal(name, "NULL")) {
        *value_out = 0;
        return 0;
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
int backend_supports_named_sections(const BackendState *state) {
    const CompilerTargetInfo *info = compiler_target_get_info(state->backend->target);

    return info != 0 && info->object_format == COMPILER_OBJECT_FORMAT_ELF64;
}

int backend_supports_subsections_via_symbols(const BackendState *state) {
    return backend_is_darwin(state);
}

int emit_named_section(BackendState *state,
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

int emit_globals(BackendState *state) {
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
        int align_bytes = decl_alignment_bytes(state, state->globals[i].type_text);
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
        if (align_bytes > 1) {
            int shift = 0;
            unsigned int value = (unsigned int)align_bytes;
            rt_copy_string(line, sizeof(line), ".p2align ");
            while (value > 1U) {
                value >>= 1U;
                shift += 1;
            }
            rt_unsigned_to_string((unsigned long long)shift, digits, sizeof(digits));
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
            if (emit_line(state, line) != 0) {
                backend_set_error(state->backend, "failed to emit global alignment");
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

const char *extract_store_name(const char *cursor, char *name, size_t name_size) {
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

void collect_global_initializers(BackendState *state, const CompilerIr *ir) {
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

int emit_string_literals(BackendState *state) {
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
        size_t j;

        if (state->backend->data_sections && backend_supports_named_sections(state)) {
            char section_name[256];

            rt_copy_string(section_name, sizeof(section_name), ".rodata.");
            rt_copy_string(section_name + rt_strlen(section_name), sizeof(section_name) - rt_strlen(section_name), state->strings[i].label);
            if (emit_named_section(state, section_name, "a", "progbits", "failed to emit string literal section") != 0) {
                return -1;
            }
        }

        rt_copy_string(line, sizeof(line), backend_private_label_prefix(state));
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), state->strings[i].label);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ":");
        if (emit_line(state, line) != 0) {
            backend_set_error(state->backend, "failed to emit string literal label");
            return -1;
        }

        for (j = 0; j < state->strings[i].length; ++j) {
            if (emit_data_integer(state, 1, (long long)(unsigned char)state->strings[i].text[j]) != 0) {
                backend_set_error(state->backend, "failed to emit string literal contents");
                return -1;
            }
        }
        if (emit_data_integer(state, 1, 0) != 0) {
            backend_set_error(state->backend, "failed to emit string literal terminator");
            return -1;
        }
    }

    return 0;
}
