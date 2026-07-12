/* IR prescan and assembly dispatch helpers. */

#include "backend_internal.h"

#include <limits.h>

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

static int local_name_declared_count_in_function(const CompilerIr *ir, size_t decl_index, const char *name);

static BackendFunctionName *lookup_function_info(BackendState *state, const char *name) {
    size_t i;

    if (state == 0 || name == 0 || name[0] == '\0') {
        return 0;
    }
    for (i = 0; i < state->function_count; ++i) {
        if (names_equal(state->functions[i].name, name)) {
            return &state->functions[i];
        }
    }
    return 0;
}

static int is_identifier_char(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_';
}

int line_references_identifier(const char *line, const char *name) {
    size_t name_length;
    size_t i = 0;
    int in_string = 0;
    int in_char = 0;

    if (line == 0 || name == 0 || name[0] == '\0') {
        return 0;
    }
    name_length = rt_strlen(name);

    while (line[i] != '\0') {
        if ((in_string || in_char) && line[i] == '\\' && line[i + 1U] != '\0') {
            i += 2U;
            continue;
        }
        if (!in_char && line[i] == '"') {
            in_string = !in_string;
            i += 1U;
            continue;
        }
        if (!in_string && line[i] == '\'') {
            in_char = !in_char;
            i += 1U;
            continue;
        }
        if (!in_string && !in_char &&
            (i == 0U || !is_identifier_char(line[i - 1U])) &&
            rt_strncmp(line + i, name, name_length) == 0 &&
            !is_identifier_char(line[i + name_length])) {
            return 1;
        }
        i += 1U;
    }
    return 0;
}

static int line_has_function_call(const char *line, const char *name) {
    size_t name_length;
    size_t i = 0;
    int in_string = 0;
    int in_char = 0;

    if (line == 0 || name == 0 || name[0] == '\0') {
        return 0;
    }
    name_length = rt_strlen(name);
    while (line[i] != '\0') {
        if ((in_string || in_char) && line[i] == '\\' && line[i + 1U] != '\0') {
            i += 2U;
            continue;
        }
        if (!in_char && line[i] == '"') {
            in_string = !in_string;
            i += 1U;
            continue;
        }
        if (!in_string && line[i] == '\'') {
            in_char = !in_char;
            i += 1U;
            continue;
        }
        if (!in_string && !in_char &&
            (i == 0U || !is_identifier_char(line[i - 1U])) &&
            rt_strncmp(line + i, name, name_length) == 0 &&
            !is_identifier_char(line[i + name_length])) {
            const char *cursor = skip_spaces(line + i + name_length);
            if (*cursor == '(') {
                return 1;
            }
        }
        i += 1U;
    }
    return 0;
}

static int is_void_identifier_eval_line(const char *line, const char *name) {
    const char *cursor = skip_spaces(line != 0 && starts_with(line, "eval ") ? line + 5 : line);
    size_t name_length;

    if (cursor == 0 || name == 0 || name[0] == '\0' || !starts_with(cursor, "(void)")) {
        return 0;
    }
    cursor = skip_spaces(cursor + 6);
    name_length = rt_strlen(name);
    return rt_strncmp(cursor, name, name_length) == 0 && !is_identifier_char(cursor[name_length]) &&
           *skip_spaces(cursor + name_length) == '\0';
}

static int is_void_discard_identifier_expr(const char *expr) {
    const char *cursor = skip_spaces(expr);

    if (!starts_with(cursor, "(void)")) {
        return 0;
    }
    cursor = skip_spaces(cursor + 6);
    if (!((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') || *cursor == '_')) {
        return 0;
    }
    cursor += 1;
    while (is_identifier_char(*cursor)) {
        cursor += 1;
    }
    return *skip_spaces(cursor) == '\0';
}

static int parameter_is_used_after_decl(const CompilerIr *ir, size_t decl_index, const char *name) {
    size_t i;

    for (i = decl_index + 1U; i < ir->count; ++i) {
        const char *line = ir->lines[i];
        if (starts_with(line, "endfunc ")) {
            break;
        }
        if (is_void_identifier_eval_line(line, name)) {
            continue;
        }
        if (line_references_identifier(line, name)) {
            return 1;
        }
    }
    return 0;
}

static int text_has_identifier_at(const char *text, const char *name) {
    size_t name_length;

    if (text == 0 || name == 0 || name[0] == '\0') {
        return 0;
    }
    name_length = rt_strlen(name);
    return rt_strncmp(text, name, name_length) == 0 && !is_identifier_char(text[name_length]);
}

static int line_takes_identifier_address(const char *line, const char *name) {
    size_t i = line != 0 && starts_with(line, "eval ") ? 5U : 0U;
    int in_string = 0;
    int in_char = 0;

    while (line != 0 && line[i] != '\0') {
        if ((in_string || in_char) && line[i] == '\\' && line[i + 1U] != '\0') {
            i += 2U;
            continue;
        }
        if (!in_char && line[i] == '"') {
            in_string = !in_string;
            i += 1U;
            continue;
        }
        if (!in_string && line[i] == '\'') {
            in_char = !in_char;
            i += 1U;
            continue;
        }
        if (!in_string && !in_char && line[i] == '&' && line[i + 1U] != '&' && (i == 0U || line[i - 1U] != '&')) {
            size_t previous = i;
            while (previous > 0U && (line[previous - 1U] == ' ' || line[previous - 1U] == '\t')) {
                previous -= 1U;
            }
            if (previous > 0U &&
                (is_identifier_char(line[previous - 1U]) ||
                 line[previous - 1U] == ')' ||
                 line[previous - 1U] == ']')) {
                if (line[previous - 1U] == ')' &&
                    text_has_identifier_at(skip_spaces(line + i + 1U), name)) {
                    return 1;
                }
                i += 1U;
                continue;
            }
            const char *cursor = skip_spaces(line + i + 1U);
            while (*cursor == '(') {
                cursor = skip_spaces(cursor + 1);
            }
            if (*cursor == '&') {
                cursor = skip_spaces(cursor + 1);
            }
            if (text_has_identifier_at(cursor, name)) {
                return 1;
            }
        }
        i += 1U;
    }
    return 0;
}

static int is_assignment_operator_after_identifier(const char *cursor) {
    cursor = skip_spaces(cursor);
    while (*cursor == ')') {
        cursor = skip_spaces(cursor + 1);
    }
    if (cursor[0] == '=' && cursor[1] != '=') {
        return 1;
    }
    if ((cursor[0] == '+' || cursor[0] == '-' || cursor[0] == '*' || cursor[0] == '/' ||
         cursor[0] == '%' || cursor[0] == '&' || cursor[0] == '|' || cursor[0] == '^') &&
        cursor[1] == '=') {
        return 1;
    }
    if ((cursor[0] == '<' || cursor[0] == '>') && cursor[1] == cursor[0] && cursor[2] == '=') {
        return 1;
    }
    return (cursor[0] == '+' && cursor[1] == '+') || (cursor[0] == '-' && cursor[1] == '-');
}

static int identifier_has_prefix_incdec(const char *line, size_t offset) {
    size_t cursor = offset;

    while (cursor > 0U && line[cursor - 1U] == ' ') {
        cursor -= 1U;
    }
    return cursor >= 2U &&
           ((line[cursor - 2U] == '+' && line[cursor - 1U] == '+') ||
            (line[cursor - 2U] == '-' && line[cursor - 1U] == '-'));
}

static int line_modifies_identifier(const char *line, const char *name) {
    size_t name_length;
    size_t i = 0;
    int in_string = 0;
    int in_char = 0;

    if (line == 0 || name == 0 || name[0] == '\0') {
        return 0;
    }
    if (starts_with(line, "store ")) {
        char store_name[COMPILER_IR_NAME_CAPACITY];
        (void)extract_store_name(line + 6, store_name, sizeof(store_name));
        if (names_equal(store_name, name)) {
            return 1;
        }
    }
    name_length = rt_strlen(name);
    while (line[i] != '\0') {
        if ((in_string || in_char) && line[i] == '\\' && line[i + 1U] != '\0') {
            i += 2U;
            continue;
        }
        if (!in_char && line[i] == '"') {
            in_string = !in_string;
            i += 1U;
            continue;
        }
        if (!in_string && line[i] == '\'') {
            in_char = !in_char;
            i += 1U;
            continue;
        }
        if (!in_string && !in_char &&
            (i == 0U || !is_identifier_char(line[i - 1U])) &&
            rt_strncmp(line + i, name, name_length) == 0 &&
            !is_identifier_char(line[i + name_length])) {
            if (is_assignment_operator_after_identifier(line + i + name_length) ||
                identifier_has_prefix_incdec(line, i)) {
                return 1;
            }
        }
        i += 1U;
    }
    return 0;
}

static int line_is_simple_identifier_incdec(const char *line, const char *name) {
    const char *cursor = line != 0 && starts_with(line, "eval ") ? skip_spaces(line + 5) : skip_spaces(line);
    size_t name_length = rt_strlen(name != 0 ? name : "");

    if (name_length == 0U) {
        return 0;
    }
    if ((starts_with(cursor, "++") || starts_with(cursor, "--")) &&
        text_has_identifier_at(skip_spaces(cursor + 2), name)) {
        cursor = skip_spaces(cursor + 2);
        return *skip_spaces(cursor + name_length) == '\0';
    }
    if (text_has_identifier_at(cursor, name)) {
        cursor = skip_spaces(cursor + name_length);
        return (starts_with(cursor, "++") || starts_with(cursor, "--")) && *skip_spaces(cursor + 2) == '\0';
    }
    return 0;
}

static int line_is_direct_identifier_assignment(const char *line, const char *name) {
    const char *cursor = line != 0 && starts_with(line, "eval ") ? skip_spaces(line + 5) : skip_spaces(line);
    size_t name_length = rt_strlen(name != 0 ? name : "");
    int wrapped = 0;

    if (name_length == 0) {
        return 0;
    }
    if (*cursor == '(') {
        wrapped = 1;
        cursor = skip_spaces(cursor + 1);
    }
    if (rt_strncmp(cursor, name, name_length) != 0 || is_identifier_char(cursor[name_length])) {
        return 0;
    }
    cursor = skip_spaces(cursor + name_length);
    if (wrapped) {
        if (*cursor != ')') {
            return 0;
        }
        cursor = skip_spaces(cursor + 1);
    }
    return *cursor == '=' ||
           ((cursor[0] == '+' || cursor[0] == '-' || cursor[0] == '*' || cursor[0] == '/' ||
             cursor[0] == '%' || cursor[0] == '&' || cursor[0] == '|' || cursor[0] == '^') &&
            cursor[1] == '=') ||
           ((cursor[0] == '<' || cursor[0] == '>') && cursor[1] == cursor[0] && cursor[2] == '=');
}

static int parameter_can_use_cached_register(const BackendState *state,
                                             const CompilerIr *ir,
                                             size_t decl_index,
                                             const char *function_name,
                                             const char *name,
                                             const char *type_text,
                                             int source_param_index) {
    size_t i;
    int abi_param_index = source_param_index + (function_returns_object(state, function_name) ? 1 : 0);
    int reference_count = 0;

    if (backend_is_aarch64(state) || source_param_index < 0 || abi_param_index >= backend_register_arg_limit(state) ||
        text_contains(type_text, "__int128") || text_contains(type_text, "double") ||
        (decl_requires_object_storage(type_text) && !text_contains(type_text, "[")) ||
        local_name_declared_count_in_function(ir, decl_index, name) > 0) {
        return 0;
    }
    for (i = decl_index + 1U; i < ir->count; ++i) {
        const char *line = ir->lines[i];
        if (starts_with(line, "endfunc ")) {
            break;
        }
        if (!line_references_identifier(line, name)) {
            continue;
        }
        if (line_takes_identifier_address(line, name) || line_modifies_identifier(line, name)) {
            return 0;
        }
        reference_count += 1;
    }
    return text_contains(type_text, "[") ? reference_count >= 2 : reference_count >= 3;
}

static int indirect_parameter_can_use_cached_register(const BackendState *state,
                                                      const CompilerIr *ir,
                                                      size_t decl_index,
                                                      const char *function_name,
                                                      const char *name,
                                                      const char *type_text,
                                                      int source_param_index) {
    size_t i;
    int abi_param_index = source_param_index + (function_returns_object(state, function_name) ? 1 : 0);
    int reference_count = 0;

    if (backend_is_aarch64(state) || source_param_index < 0 || abi_param_index >= backend_register_arg_limit(state) ||
        text_contains(type_text, "__int128") ||
        !decl_requires_object_storage(type_text) ||
        text_contains(type_text, "[") ||
        local_name_declared_count_in_function(ir, decl_index, name) > 0) {
        return 0;
    }
    for (i = decl_index + 1U; i < ir->count; ++i) {
        const char *line = ir->lines[i];
        if (starts_with(line, "endfunc ")) {
            break;
        }
        if (!line_references_identifier(line, name)) {
            continue;
        }
        if (line_modifies_identifier(line, name)) {
            return 0;
        }
        reference_count += 1;
    }
    return reference_count >= 3;
}

static int local_name_declared_count_in_function(const CompilerIr *ir, size_t decl_index, const char *name) {
    size_t start = decl_index;
    size_t i;
    int count = 0;

    while (start > 0) {
        if (starts_with(ir->lines[start], "func ")) {
            break;
        }
        start -= 1U;
    }
    for (i = start; i < ir->count; ++i) {
        const char *line = ir->lines[i];
        char storage[16];
        char kind[16];
        char type_text[128];
        char decl_name[COMPILER_IR_NAME_CAPACITY];

        if (starts_with(line, "endfunc ")) {
            break;
        }
        if (!starts_with(line, "decl ")) {
            continue;
        }
        parse_decl_line(line, storage, sizeof(storage), kind, sizeof(kind), type_text, sizeof(type_text), decl_name, sizeof(decl_name));
        if (names_equal(storage, "local") && names_equal(decl_name, name)) {
            count += 1;
        }
    }

    return count;
}

static int local_can_use_cached_register(const BackendState *state,
                                         const CompilerIr *ir,
                                         size_t decl_index,
                                         const char *name,
                                         const char *type_text) {
    size_t i;
    int reference_count = 0;
    int store_count = 0;
    int saw_store = 0;

    if (backend_is_aarch64(state) || text_contains(type_text, "__int128") ||
        decl_requires_object_storage(type_text) ||
        local_name_declared_count_in_function(ir, decl_index, name) > 1) {
        return 0;
    }
    for (i = decl_index + 1U; i < ir->count; ++i) {
        const char *line = ir->lines[i];
        if (starts_with(line, "endfunc ")) {
            break;
        }
        if (starts_with(line, "store ")) {
            char store_name[COMPILER_IR_NAME_CAPACITY];
            (void)extract_store_name(line + 6, store_name, sizeof(store_name));
            if (names_equal(store_name, name)) {
                store_count += 1;
                saw_store = 1;
                continue;
            }
        }
        if (!line_references_identifier(line, name)) {
            continue;
        }
        if (line_takes_identifier_address(line, name)) {
            return 0;
        }
        if (line_modifies_identifier(line, name)) {
            if (line_is_simple_identifier_incdec(line, name) ||
                line_is_direct_identifier_assignment(line, name)) {
                saw_store = 1;
                reference_count += 1;
                continue;
            }
            return 0;
        }
        if (!saw_store) {
            return 0;
        }
        reference_count += 1;
    }
    return store_count >= 1 && reference_count >= 3;
}

static int function_line_has_call(const BackendState *state, const char *line, int object_returns_only) {
    size_t i;

    for (i = 0; i < state->function_count; ++i) {
        if (object_returns_only && !state->functions[i].returns_object) {
            continue;
        }
        if (line_has_function_call(line, state->functions[i].name)) {
            return 1;
        }
    }
    return 0;
}

static int function_line_object_callret_bytes(const BackendState *state, const char *line) {
    int max_bytes = 0;
    size_t i;

    for (i = 0; i < state->function_count; ++i) {
        int bytes;
        if (!state->functions[i].returns_object) {
            continue;
        }
        if (!line_has_function_call(line, state->functions[i].name)) {
            continue;
        }
        bytes = decl_slot_size(state, state->functions[i].return_type);
        if (bytes > max_bytes) {
            max_bytes = bytes;
        }
    }
    return max_bytes;
}

static int compound_literal_type_stack_bytes(const BackendState *state, const char *text, size_t start, size_t end) {
    char type_text[128];
    char first_identifier[COMPILER_IR_NAME_CAPACITY];
    size_t out = 0;
    size_t i = start;
    int aggregate_size;

    while (i < end && (text[i] == ' ' || text[i] == '\t')) {
        i += 1U;
    }
    while (end > i && (text[end - 1U] == ' ' || text[end - 1U] == '\t')) {
        end -= 1U;
    }
    while (i < end && out + 1U < sizeof(type_text)) {
        type_text[out++] = text[i++];
    }
    type_text[out] = '\0';

    aggregate_size = lookup_aggregate_size(state, type_text);
    if (aggregate_size > 0) {
        return aggregate_size;
    }

    first_identifier[0] = '\0';
    i = 0;
    while (type_text[i] != '\0') {
        if ((type_text[i] >= 'A' && type_text[i] <= 'Z') ||
            (type_text[i] >= 'a' && type_text[i] <= 'z') ||
            type_text[i] == '_') {
            size_t first_length = 0;

            while ((type_text[i + first_length] >= 'A' && type_text[i + first_length] <= 'Z') ||
                   (type_text[i + first_length] >= 'a' && type_text[i + first_length] <= 'z') ||
                   (type_text[i + first_length] >= '0' && type_text[i + first_length] <= '9') ||
                   type_text[i + first_length] == '_') {
                if (first_length + 1U < sizeof(first_identifier)) {
                    first_identifier[first_length] = type_text[i + first_length];
                }
                first_length += 1U;
            }
            if (first_length < sizeof(first_identifier)) {
                first_identifier[first_length] = '\0';
            } else {
                first_identifier[sizeof(first_identifier) - 1U] = '\0';
            }
            break;
        }
        i += 1U;
    }

    if (first_identifier[0] != '\0') {
        char aggregate_name[128];
        if (names_equal(first_identifier, "struct") || names_equal(first_identifier, "union")) {
            const char *cursor = type_text + rt_strlen(first_identifier);
            size_t name_out = rt_strlen(first_identifier) + 1U;
            rt_copy_string(aggregate_name, sizeof(aggregate_name), first_identifier);
            aggregate_name[rt_strlen(aggregate_name)] = ':';
            aggregate_name[name_out] = '\0';
            while (*cursor == ' ' || *cursor == '\t') {
                cursor += 1;
            }
            rt_copy_string(aggregate_name + name_out, sizeof(aggregate_name) - name_out, cursor);
        } else {
            rt_copy_string(aggregate_name, sizeof(aggregate_name), "struct:");
            rt_copy_string(aggregate_name + 7, sizeof(aggregate_name) - 7, first_identifier);
        }
        aggregate_size = lookup_aggregate_size(state, aggregate_name);
        if (aggregate_size > 0) {
            return aggregate_size;
        }
    }

    return decl_slot_size(state, type_text);
}

static int compound_literal_stack_bytes(const BackendState *state, const char *text) {
    int bytes = 0;
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
                size_t start = i;
                int depth = 0;

                while (start > 0) {
                    if (text[start] == ')') {
                        depth += 1;
                    } else if (text[start] == '(') {
                        depth -= 1;
                        if (depth == 0) {
                            break;
                        }
                    }
                    start -= 1U;
                }
                if (text[start] == '(') {
                    bytes += compound_literal_type_stack_bytes(state, text, start + 1U, i);
                } else {
                    bytes += backend_stack_slot_size(state);
                }
            }
        }
        i += 1U;
    }

    return bytes;
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


static int prescan_ir(BackendState *state, const CompilerIr *ir) {
    size_t i;
    int in_function = 0;
    char current_function[COMPILER_IR_NAME_CAPACITY];
    int current_param_index = 0;
    int current_local_index = 0;

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
            current_param_index = 0;
            current_local_index = 0;
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
            current_param_index = 0;
            current_local_index = 0;
            continue;
        }

        if (in_function && starts_with(line, "decl ")) {
            char storage[16];
            char kind[16];
            char type_text[128];
            char name[COMPILER_IR_NAME_CAPACITY];
            int slot_size;

            parse_decl_line(line, storage, sizeof(storage), kind, sizeof(kind), type_text, sizeof(type_text), name, sizeof(name));
            if (names_equal(storage, "param") || names_equal(storage, "local")) {
                BackendFunctionName *function_info = lookup_function_info(state, current_function);
                int used_param = 1;

                if (names_equal(storage, "param")) {
                    used_param = parameter_is_used_after_decl(ir, i, name);
                    if (!used_param && function_info != 0 && current_param_index < 64) {
                        function_info->unused_param_mask |= 1ULL << (unsigned int)current_param_index;
                    }
                    if (used_param && function_info != 0 && current_param_index < 64 && function_info->cached_param_count < 5 &&
                        (parameter_can_use_cached_register(state, ir, i, current_function, name, type_text, current_param_index) ||
                         indirect_parameter_can_use_cached_register(state, ir, i, current_function, name, type_text, current_param_index))) {
                        function_info->cached_param_mask |= 1ULL << (unsigned int)current_param_index;
                        function_info->cached_param_count += 1;
                        current_param_index += 1;
                        continue;
                    }
                    current_param_index += 1;
                }
                if (names_equal(storage, "param") && !used_param) {
                    continue;
                }
                if (names_equal(storage, "local") && function_info != 0 && current_local_index < 64 &&
                    function_info->cached_param_count + function_info->cached_local_count < 5 &&
                    local_can_use_cached_register(state, ir, i, name, type_text)) {
                    function_info->cached_local_mask |= 1ULL << (unsigned int)current_local_index;
                    function_info->cached_local_count += 1;
                    current_local_index += 1;
                    continue;
                }
                if (names_equal(storage, "local")) {
                    current_local_index += 1;
                }
                if (names_equal(storage, "param") && text_contains(type_text, "[")) {
                    slot_size = backend_stack_slot_size(state);
                } else {
                    slot_size = decl_slot_size(state, type_text);
                }
                if (function_info != 0) {
                    function_info->stack_bytes += slot_size;
                }
                continue;
            }
        }

        if (in_function && current_function[0] != '\0') {
            int compound_bytes = compound_literal_stack_bytes(state, line);
            BackendFunctionName *function_info = lookup_function_info(state, current_function);

            if (function_info != 0 && function_line_has_call(state, line, 0)) {
                function_info->has_call = 1;
            }
            if (function_info != 0) {
                int callret_bytes = function_line_object_callret_bytes(state, line);
                if (callret_bytes > function_info->callret_bytes) {
                    function_info->callret_bytes = callret_bytes;
                }
            }
            if (function_info != 0 && function_info->callret_bytes > 0) {
                function_info->needs_callret = 1;
            }
            if (compound_bytes > 0) {
                if (function_info != 0) {
                    function_info->stack_bytes += compound_bytes;
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

static int function_has_global_linkage(const BackendState *state, const char *name) {
    size_t i;

    for (i = 0; i < state->function_count; ++i) {
        if (names_equal(state->functions[i].name, name)) {
            return state->functions[i].global;
        }
    }

    return 1;
}

static int cached_register_save_offset(int index) {
    return (index + 1) * 8;
}

static int emit_cached_register_save_restore(BackendState *state, int restore) {
    int index;

    if (backend_is_aarch64(state)) {
        return 0;
    }
    for (index = 0; index < state->saved_register_count; ++index) {
        char line[96];
        char offset_text[32];
        const char *reg = backend_x86_cached_register_name(index);

        if (reg == 0) {
            backend_set_error(state->backend, "invalid cached register save slot");
            return -1;
        }
        rt_unsigned_to_string((unsigned long long)cached_register_save_offset(index), offset_text, sizeof(offset_text));
        if (restore) {
            rt_copy_string(line, sizeof(line), "movq -");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rbp), ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        } else {
            rt_copy_string(line, sizeof(line), "movq ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", -");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rbp)");
        }
        if (emit_instruction(state, line) != 0) {
            return -1;
        }
    }
    return 0;
}

static int begin_function(BackendState *state, const char *name) {
    char symbol[COMPILER_IR_NAME_CAPACITY];
    BackendFunctionName *function_info = lookup_function_info(state, name);
    int export_symbol = function_has_global_linkage(state, name);
    int returns_object = function_returns_object(state, name);
    const char *return_type = function_return_type(state, name);
    state->in_function = 1;
    state->local_count = 0;
    reset_local_index(state);
    state->local_scope_count = 0;
    state->param_count = returns_object ? 1 : 0;
    state->gpr_param_count = returns_object ? 1 : 0;
    state->xmm_param_count = 0;
    state->saw_return_in_function = 0;
    state->frameless_function = 0;
    state->stack_size = 0;
    state->saved_register_count = (!backend_is_aarch64(state) && function_info != 0)
                                      ? function_info->cached_param_count + function_info->cached_local_count
                                      : 0;
    state->cached_param_count = 0;
    state->cached_local_count = 0;
    state->local_decl_count = 0;
    state->reserved_stack_size = lookup_function_stack_bytes(state, name);
    state->reserved_stack_size += state->saved_register_count * backend_stack_slot_size(state);
    if (function_info != 0 && function_info->needs_callret) {
        state->reserved_stack_size += function_info->callret_bytes > 0 ? function_info->callret_bytes : BACKEND_ARRAY_STACK_BYTES;
    }
    if (returns_object) {
        state->reserved_stack_size += backend_stack_slot_size(state);
        state->reserved_stack_size += decl_slot_size(state, return_type);
    }
    state->reserved_stack_size = aligned_function_stack_bytes(state->reserved_stack_size);
    state->frameless_function = !backend_is_aarch64(state) && state->reserved_stack_size == 0 &&
                                !returns_object && (function_info == 0 || !function_info->has_call);
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
    if (state->frameless_function) {
        return 0;
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

    if (emit_cached_register_save_restore(state, 0) != 0) {
        return -1;
    }
    state->stack_size = state->saved_register_count * backend_stack_slot_size(state);

    if (function_info != 0 && function_info->needs_callret) {
        int callret_bytes = function_info->callret_bytes > 0 ? function_info->callret_bytes : BACKEND_ARRAY_STACK_BYTES;
        char callret_type[64];
        char size_text[32];

        rt_unsigned_to_string((unsigned long long)callret_bytes, size_text, sizeof(size_text));
        rt_copy_string(callret_type, sizeof(callret_type), "char[");
        rt_copy_string(callret_type + rt_strlen(callret_type), sizeof(callret_type) - rt_strlen(callret_type), size_text);
        rt_copy_string(callret_type + rt_strlen(callret_type), sizeof(callret_type) - rt_strlen(callret_type), "]");
        if (allocate_local(state, "__callret", callret_type, callret_bytes, 1, 0, 1, 0) != 0) {
            return -1;
        }
    }

    if (returns_object) {
        int retbuf_index;
        char offset_text[32];
        char asm_line[128];

        if (allocate_local(state,
                           "__retobj",
                           return_type,
                           decl_slot_size(state, return_type),
                           decl_requires_object_storage(return_type),
                           decl_pointer_depth(return_type),
                           decl_char_based(return_type),
                           should_prefer_word_index("__retobj", return_type)) != 0 ||
            allocate_local(state, "__retbuf", "void*", backend_stack_slot_size(state), 0, 1, 0, 1) != 0) {
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
    state->local_scope_count = 0;
    state->param_count = 0;
    state->gpr_param_count = 0;
    state->xmm_param_count = 0;
    state->saw_return_in_function = 0;
    state->frameless_function = 0;
    state->stack_size = 0;
    state->reserved_stack_size = 0;
    state->saved_register_count = 0;
    state->cached_param_count = 0;
    state->cached_local_count = 0;
    state->local_decl_count = 0;
    backend_invalidate_block_cache(state);
    state->current_ir_index = -1;
    state->current_function[0] = '\0';
    return 0;
}

static int enter_local_scope(BackendState *state) {
    if (state->local_scope_count >= COMPILER_BACKEND_MAX_LOCAL_SCOPES) {
        backend_set_error(state->backend, "local scope nesting exceeded backend limits");
        return -1;
    }
    state->local_scope_markers[state->local_scope_count] = state->local_count;
    state->local_scope_count += 1U;
    return 0;
}

static void exit_local_scope(BackendState *state) {
    if (state->local_scope_count == 0U) {
        return;
    }
    state->local_scope_count -= 1U;
    state->local_count = state->local_scope_markers[state->local_scope_count];
    rebuild_local_index(state);
    backend_invalidate_block_cache(state);
}

static int emit_function_return(BackendState *state) {
    if (backend_is_aarch64(state)) {
        return emit_instruction(state, "mov sp, x29") == 0 &&
               emit_instruction(state, "ldp x29, x30, [sp], #16") == 0 &&
               emit_instruction(state, "ret") == 0 ? 0 : -1;
    }
    if (state->frameless_function) {
        return emit_instruction(state, "ret");
    }
    if (emit_cached_register_save_restore(state, 1) != 0) {
        return -1;
    }
    return emit_instruction(state, "leave") == 0 && emit_instruction(state, "ret") == 0 ? 0 : -1;
}

static int emit_normalize_integer_return(BackendState *state) {
    const char *type = function_return_type(state, state->current_function);
    int is_unsigned;

    if (type == 0 || type[0] == '\0' || text_contains(type, "*") || text_contains(type, "long")) {
        return 0;
    }
    is_unsigned = type_is_unsigned_like(type);
    if (text_contains(type, "char")) {
        if (backend_is_aarch64(state)) {
            return emit_instruction(state, is_unsigned ? "and x0, x0, #0xff" : "sxtb x0, w0");
        }
        return emit_instruction(state, is_unsigned ? "movzbl %al, %eax" : "movsbq %al, %rax");
    }
    if (text_contains(type, "short")) {
        if (backend_is_aarch64(state)) {
            return emit_instruction(state, is_unsigned ? "and x0, x0, #0xffff" : "sxth x0, w0");
        }
        return emit_instruction(state, is_unsigned ? "movzwl %ax, %eax" : "movswq %ax, %rax");
    }
    if (text_contains(type, "int") || names_equal(skip_spaces(type), "signed") || names_equal(skip_spaces(type), "unsigned")) {
        if (backend_is_aarch64(state)) {
            return emit_instruction(state, is_unsigned ? "uxtw x0, w0" : "sxtw x0, w0");
        }
        return emit_instruction(state, is_unsigned ? "movl %eax, %eax" : "movslq %eax, %rax");
    }
    return 0;
}

static int emit_x86_64_syscall_inline_asm(BackendState *state) {
    if (backend_is_aarch64(state)) {
        backend_set_error(state->backend, "syscall inline asm is only supported on x86_64");
        return -1;
    }
    if ((find_local(state, "rdi") >= 0 && emit_load_name_into_register(state, "rdi", "%rdi") != 0) ||
        (find_local(state, "rsi") >= 0 && emit_load_name_into_register(state, "rsi", "%rsi") != 0) ||
        (find_local(state, "rdx") >= 0 && emit_load_name_into_register(state, "rdx", "%rdx") != 0) ||
        (find_local(state, "r10") >= 0 && emit_load_name_into_register(state, "r10", "%r10") != 0) ||
        (find_local(state, "r8") >= 0 && emit_load_name_into_register(state, "r8", "%r8") != 0) ||
        (find_local(state, "r9") >= 0 && emit_load_name_into_register(state, "r9", "%r9") != 0) ||
        emit_load_name_into_register(state, "rax", "%rax") != 0 ||
        emit_instruction(state, "syscall") != 0 ||
        emit_store_name(state, "rax") != 0) {
        return -1;
    }
    return 0;
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
        const char x86_arg_regs[][5] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
        const char aarch64_arg_regs[][3] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
        char asm_line[128];
        int index = state->gpr_param_count;
        BackendFunctionName *function_info = lookup_function_info(state, state->current_function);
        int source_param_index = function_returns_object(state, state->current_function) ? state->param_count - 1 : state->param_count;
        int object_by_value_param = is_array && !text_contains(type_text, "[") && pointer_depth == 0;
        int double_param = text_contains(type_text, "double") &&
                   !text_contains(type_text, "*") && !text_contains(type_text, "[");

        if (function_info != 0 && source_param_index >= 0 && source_param_index < 64 &&
            (function_info->unused_param_mask & (1ULL << (unsigned int)source_param_index)) != 0ULL) {
            state->param_count += 1;
            if (double_param) {
                state->xmm_param_count += 1;
            } else {
                state->gpr_param_count += 1;
            }
            return 0;
        }

        if (double_param) {
            int local_index;
            char offset_text[32];

            if (backend_is_aarch64(state) || state->xmm_param_count >= 8) {
                backend_set_error(state->backend, "double parameters outside x86-64 XMM registers are not yet supported");
                return -1;
            }
            if (allocate_local(state, name, type_text, slot_size, is_array, pointer_depth, char_based, prefers_word_index) != 0) {
                return -1;
            }
            local_index = find_local(state, name);
            rt_unsigned_to_string((unsigned long long)state->locals[local_index].offset, offset_text, sizeof(offset_text));
            rt_copy_string(asm_line, sizeof(asm_line), "movq %xmm0, %rax");
            asm_line[9] = (char)('0' + state->xmm_param_count);
            if (emit_instruction(state, asm_line) != 0) {
                return -1;
            }
            rt_copy_string(asm_line, sizeof(asm_line), "movq %rax, -");
            rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), offset_text);
            rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "(%rbp)");
            if (emit_instruction(state, asm_line) != 0) {
                return -1;
            }
            state->param_count += 1;
            state->xmm_param_count += 1;
            return 0;
        }

        if (!object_by_value_param &&
            !backend_is_aarch64(state) && function_info != 0 && source_param_index >= 0 && source_param_index < 64 &&
            index < backend_register_arg_limit(state) &&
            (function_info->cached_param_mask & (1ULL << (unsigned int)source_param_index)) != 0ULL) {
            const char *cached_reg = backend_x86_cached_register_name(state->cached_param_count);

            if (cached_reg == 0 ||
                allocate_cached_local(state, name, type_text, pointer_depth, char_based, prefers_word_index, state->cached_param_count) != 0) {
                return -1;
            }
            rt_copy_string(asm_line, sizeof(asm_line), "movq ");
            rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), x86_arg_regs[index]);
            rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), ", ");
            rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), cached_reg);
            if (emit_instruction(state, asm_line) != 0) {
                return -1;
            }
            state->cached_param_count += 1;
            state->param_count += 1;
            state->gpr_param_count += 1;
            return 0;
        }

        if (object_by_value_param) {
            int max_register_params = backend_register_arg_limit(state);
            int local_index;
            char offset_text[32];

            if (!backend_is_aarch64(state) && function_info != 0 && source_param_index >= 0 && source_param_index < 64 &&
                index < max_register_params &&
                (function_info->cached_param_mask & (1ULL << (unsigned int)source_param_index)) != 0ULL) {
                const char *cached_reg = backend_x86_cached_register_name(state->cached_param_count);

                if (cached_reg == 0 ||
                    allocate_cached_indirect_object_local(state,
                                                          name,
                                                          type_text,
                                                          slot_size,
                                                          char_based,
                                                          prefers_word_index,
                                                          state->cached_param_count) != 0) {
                    return -1;
                }
                rt_copy_string(asm_line, sizeof(asm_line), "movq ");
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), x86_arg_regs[index]);
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), ", ");
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), cached_reg);
                if (emit_instruction(state, asm_line) != 0) {
                    return -1;
                }
                state->cached_param_count += 1;
                state->param_count += 1;
                state->gpr_param_count += 1;
                return 0;
            }

            if (allocate_indirect_object_local(state, name, type_text, slot_size, char_based, prefers_word_index) != 0) {
                return -1;
            }
            local_index = find_local(state, name);
            rt_unsigned_to_string((unsigned long long)state->locals[local_index].offset, offset_text, sizeof(offset_text));
            {
                if (index < max_register_params) {
                    if (backend_is_aarch64(state)) {
                        if (!names_equal(aarch64_arg_regs[index], "x0")) {
                            rt_copy_string(asm_line, sizeof(asm_line), "mov x0, ");
                            rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), aarch64_arg_regs[index]);
                            if (emit_instruction(state, asm_line) != 0) {
                                return -1;
                            }
                        }
                    } else {
                        rt_copy_string(asm_line, sizeof(asm_line), "movq ");
                        rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), x86_arg_regs[index]);
                        rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), ", %rax");
                        if (emit_instruction(state, asm_line) != 0) {
                            return -1;
                        }
                    }
                } else if (backend_is_aarch64(state)) {
                    unsigned long long stack_offset = 16ULL + (unsigned long long)(index - max_register_params) * 16ULL;
                    char stack_text[32];
                    rt_unsigned_to_string(stack_offset, stack_text, sizeof(stack_text));
                    rt_copy_string(asm_line, sizeof(asm_line), "ldr x0, [x29, #");
                    rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), stack_text);
                    rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "]");
                    if (emit_instruction(state, asm_line) != 0) {
                        return -1;
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
                }
                if (backend_is_aarch64(state)) {
                    if (state->locals[local_index].offset <= 255) {
                        rt_copy_string(asm_line, sizeof(asm_line), "stur x0, [x29, #-");
                        rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), offset_text);
                        rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "]");
                    } else {
                        if (emit_local_address(state, state->locals[local_index].offset, "x10") != 0) {
                            return -1;
                        }
                        rt_copy_string(asm_line, sizeof(asm_line), "str x0, [x10]");
                    }
                } else {
                    rt_copy_string(asm_line, sizeof(asm_line), "movq %rax, -");
                    rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), offset_text);
                    rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "(%rbp)");
                }
                if (emit_instruction(state, asm_line) != 0) {
                    return -1;
                }
                state->param_count += 1;
                state->gpr_param_count += 1;
                return 0;
            }
        }

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
                rt_copy_string(asm_line, sizeof(asm_line), "ldr x11, [x29, #");
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), stack_text);
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "]");
                if (emit_instruction(state, asm_line) != 0) {
                    return -1;
                }
                if (state->locals[local_index].offset <= 255) {
                    rt_copy_string(asm_line, sizeof(asm_line), "stur x11, [x29, #-");
                    rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), offset_text);
                    rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "]");
                } else {
                    if (emit_local_address(state, state->locals[local_index].offset, "x10") != 0) {
                        return -1;
                    }
                    rt_copy_string(asm_line, sizeof(asm_line), "str x11, [x10]");
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
        state->gpr_param_count += 1;
        return 0;
    }

    if (names_equal(storage, "local")) {
        int local_decl_index = state->local_decl_count;
        BackendFunctionName *function_info = lookup_function_info(state, state->current_function);

        state->local_decl_count += 1;
        if (!backend_is_aarch64(state) && function_info != 0 && local_decl_index >= 0 && local_decl_index < 64 &&
            (function_info->cached_local_mask & (1ULL << (unsigned int)local_decl_index)) != 0ULL) {
            int cached_register = function_info->cached_param_count + state->cached_local_count;
            const char *cached_reg = backend_x86_cached_register_name(cached_register);

            if (cached_reg == 0 ||
                allocate_cached_local(state, name, type_text, pointer_depth, char_based, prefers_word_index, cached_register) != 0) {
                return -1;
            }
            state->cached_local_count += 1;
            return 0;
        }
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
    state.block_cache_local = -1;
    state.current_ir_index = -1;

    if (prescan_ir(&state, ir) != 0) {
        return -1;
    }
    if ((backend->function_sections || backend->data_sections) &&
        backend_supports_subsections_via_symbols(&state) &&
        emit_line(&state, ".subsections_via_symbols") != 0) {
        backend_set_error(backend, "failed to enable Mach-O dead-strip subsections");
        return -1;
    }
    collect_global_initializers(&state, ir);
    if (emit_globals(&state) != 0) {
        return -1;
    }

    for (i = 0; i < ir->count; ++i) {
        const char *line = ir->lines[i];
        state.current_ir_index = (int)i;

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
            backend_invalidate_block_cache(&state);
            continue;
        }

        if (starts_with(line, "endfunc ")) {
            if (state.in_function &&
                !(i > 0U && starts_with(ir->lines[i - 1U], "ret")) &&
                emit_function_return(&state) != 0) {
                return -1;
            }
            end_function(&state);
            continue;
        }

        if (!state.in_function) {
            continue;
        }

        if (starts_with(line, "scope-enter")) {
            if (enter_local_scope(&state) != 0) {
                return -1;
            }
            continue;
        }

        if (starts_with(line, "scope-exit")) {
            exit_local_scope(&state);
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
            {
                int u128_store = emit_u128_store_expression(&state, name, expr);
                if (u128_store < 0) {
                    backend_set_error_with_line(backend, compiler_backend_error_message(backend), line);
                    return -1;
                }
                if (u128_store > 0) {
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
            if (is_void_discard_identifier_expr(line + 5)) {
                continue;
            }
            {
                int u128_eval = emit_u128_eval_expression(&state, line + 5);
                if (u128_eval < 0) {
                    backend_set_error_with_line(backend, compiler_backend_error_message(backend), line);
                    return -1;
                }
                if (u128_eval > 0) {
                    continue;
                }
            }
            if (emit_expression(&state, line + 5) != 0) {
                backend_set_error_with_line(backend, compiler_backend_error_message(backend), line);
                return -1;
            }
            continue;
        }

        if (starts_with(line, "asm-syscall")) {
            backend_invalidate_block_cache(&state);
            if (emit_x86_64_syscall_inline_asm(&state) != 0) {
                backend_set_error_with_line(backend, compiler_backend_error_message(backend), line);
                return -1;
            }
            backend_invalidate_block_cache(&state);
            continue;
        }

        if (starts_with(line, "ret")) {
            const char *expr = skip_spaces(line + 3);
            if (function_returns_object(&state, state.current_function)) {
                if (expr[0] != '\0') {
                    const char *return_type = function_return_type(&state, state.current_function);
                    int return_bytes = decl_slot_size(&state, return_type);
                    if (emit_object_copy_to_pointer_name(&state, "__retbuf", expr, return_bytes) != 0) {
                        if (emit_object_copy_store(&state, "__retobj", expr) != 0) {
                            backend_set_error_with_line(backend, compiler_backend_error_message(backend), line);
                            return -1;
                        }
                        if (emit_copy_name_to_pointer_name(&state, "__retobj", "__retbuf") != 0) {
                            return -1;
                        }
                    }
                } else if (emit_copy_name_to_pointer_name(&state, "__retobj", "__retbuf") != 0) {
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
            backend_invalidate_block_cache(&state);
            if (!function_returns_object(&state, state.current_function) &&
                emit_normalize_integer_return(&state) != 0) {
                return -1;
            }
            if (!function_returns_object(&state, state.current_function) &&
                text_contains(function_return_type(&state, state.current_function), "double") &&
                emit_instruction(&state, "movq %rax, %xmm0") != 0) {
                return -1;
            }
            if (emit_function_return(&state) != 0) {
                return -1;
            }
            continue;
        }

        if (starts_with(line, "switch ")) {
            backend_invalidate_block_cache(&state);
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
                backend_invalidate_block_cache(&state);
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
                backend_invalidate_block_cache(&state);
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
                backend_invalidate_block_cache(&state);
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
            if (emit_branch_false(&state, expr, label) != 0) {
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
            backend_invalidate_block_cache(&state);
            continue;
        }

        if (starts_with(line, "label ")) {
            char asm_label[96];
            write_label_name(&state, asm_label, sizeof(asm_label), line + 6);
            rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
            backend_invalidate_block_cache(&state);
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
