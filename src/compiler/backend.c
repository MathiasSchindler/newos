#include "backend_internal.h"

void backend_set_error(CompilerBackend *backend, const char *message) {
    rt_copy_string(backend->error_message, sizeof(backend->error_message), message != 0 ? message : "backend error");
}

void backend_set_error_with_line(CompilerBackend *backend, const char *message, const char *line) {
    char buffer[COMPILER_ERROR_CAPACITY];
    size_t used;
    size_t i = 0;

    rt_copy_string(buffer, sizeof(buffer), message != 0 ? message : "backend error");
    used = rt_strlen(buffer);
    rt_copy_string(buffer + used, sizeof(buffer) - used, " near `");
    used = rt_strlen(buffer);
    while (line != 0 && line[i] != '\0' && line[i] != '\n' && used + 4 < sizeof(buffer)) {
        buffer[used++] = line[i++];
    }
    if (line != 0 && line[i] != '\0' && used + 4 < sizeof(buffer)) {
        buffer[used++] = '.';
        buffer[used++] = '.';
        buffer[used++] = '.';
    }
    buffer[used++] = '`';
    buffer[used] = '\0';
    backend_set_error(backend, buffer);
}

int emit_text(BackendState *state, const char *text) {
    return rt_write_cstr(state->fd, text);
}

int emit_line(BackendState *state, const char *text) {
    return rt_write_line(state->fd, text);
}

int emit_instruction(BackendState *state, const char *text) {
    if (emit_text(state, "    ") != 0 || emit_line(state, text) != 0) {
        backend_set_error(state->backend, "failed to write assembly output");
        return -1;
    }
    return 0;
}

int names_equal(const char *lhs, const char *rhs) {
    return rt_strcmp(lhs, rhs) == 0;
}

int text_contains(const char *text, const char *needle) {
    size_t i;
    size_t needle_length = rt_strlen(needle);

    if (needle_length == 0U) {
        return 1;
    }
    for (i = 0; text[i] != '\0'; ++i) {
        size_t j = 0;
        while (needle[j] != '\0' && text[i + j] == needle[j]) {
            j += 1U;
        }
        if (j == needle_length) {
            return 1;
        }
    }
    return 0;
}

int starts_with(const char *text, const char *prefix) {
    size_t i = 0;
    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }
    return 1;
}

int name_looks_like_macro_constant(const char *name) {
    size_t i = 0;
    int saw_alpha = 0;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }

    while (name[i] != '\0') {
        char ch = name[i];
        if (ch >= 'a' && ch <= 'z') {
            return 0;
        }
        if ((ch >= 'A' && ch <= 'Z') || ch == '_') {
            saw_alpha = 1;
        } else if (ch < '0' || ch > '9') {
            return 0;
        }
        i += 1U;
    }

    return saw_alpha;
}

const char *skip_spaces(const char *text) {
    while (*text == ' ' || *text == '\t') {
        text += 1;
    }
    return text;
}

int backend_is_aarch64(const BackendState *state) {
    return compiler_target_is_aarch64(state->backend->target);
}

int backend_is_darwin(const BackendState *state) {
    return compiler_target_is_darwin(state->backend->target);
}

int backend_stack_slot_size(const BackendState *state) {
    const CompilerTargetInfo *info = compiler_target_get_info(state->backend->target);
    return info != 0 ? (int)info->stack_slot_size : 8;
}

int backend_register_arg_limit(const BackendState *state) {
    const CompilerTargetInfo *info = compiler_target_get_info(state->backend->target);
    return info != 0 ? (int)info->register_arg_limit : 6;
}

void format_symbol_name(const BackendState *state, const char *name, char *buffer, size_t buffer_size) {
    const CompilerTargetInfo *info = compiler_target_get_info(state->backend->target);
    const char *prefix = (info != 0 && info->global_symbol_prefix != 0) ? info->global_symbol_prefix : "";

    rt_copy_string(buffer, buffer_size, prefix);
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), name);
    if (prefix[0] == '\0') {
        rt_copy_string(buffer, buffer_size, name);
    }
}

void copy_last_word(const char *text, char *buffer, size_t buffer_size) {
    size_t start = 0;
    size_t end = rt_strlen(text);
    size_t i;
    size_t out = 0;

    while (end > 0 && text[end - 1] == ' ') {
        end -= 1U;
    }
    for (i = end; i > 0; --i) {
        if (text[i - 1] == ' ') {
            start = i;
            break;
        }
    }

    for (i = start; i < end && out + 1 < buffer_size; ++i) {
        buffer[out++] = text[i];
    }
    buffer[out] = '\0';
}

int parse_signed_value(const char *text, long long *value_out) {
    int negative = 0;
    unsigned long long magnitude = 0;
    unsigned int base = 10;
    int saw_digit = 0;

    text = skip_spaces(text);
    if (*text == '-') {
        negative = 1;
        text += 1;
    } else if (*text == '+') {
        text += 1;
    }

    if (text[0] == '0') {
        saw_digit = 1;
        if (text[1] == 'x' || text[1] == 'X') {
            base = 16;
            saw_digit = 0;
            text += 2;
        } else if (text[1] >= '0' && text[1] <= '7') {
            base = 8;
            text += 1;
        }
    }

    while (*text != '\0') {
        unsigned int digit = 0;
        if (*text >= '0' && *text <= '9') {
            digit = (unsigned int)(*text - '0');
        } else if (*text >= 'a' && *text <= 'f') {
            digit = 10U + (unsigned int)(*text - 'a');
        } else if (*text >= 'A' && *text <= 'F') {
            digit = 10U + (unsigned int)(*text - 'A');
        } else if (*text == 'u' || *text == 'U' || *text == 'l' || *text == 'L') {
            text += 1;
            continue;
        } else {
            return -1;
        }

        if (digit >= base) {
            return -1;
        }
        magnitude = magnitude * (unsigned long long)base + (unsigned long long)digit;
        saw_digit = 1;
        text += 1;
    }

    if (!saw_digit) {
        return -1;
    }

    *value_out = negative ? -(long long)magnitude : (long long)magnitude;
    return 0;
}

static int find_function_index(const BackendState *state, const char *name) {
    size_t i;

    for (i = 0; i < state->function_count; ++i) {
        if (names_equal(state->functions[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

int add_function_name(BackendState *state, const char *name, int global) {
    int existing = find_function_index(state, name);

    if (existing >= 0) {
        if (global) {
            state->functions[existing].global = 1;
        }
        return 0;
    }

    if (state->function_count >= COMPILER_BACKEND_MAX_FUNCTIONS) {
        backend_set_error(state->backend, "too many functions for backend");
        return -1;
    }

    rt_copy_string(state->functions[state->function_count].name, sizeof(state->functions[state->function_count].name), name);
    state->functions[state->function_count].global = global ? 1 : 0;
    state->functions[state->function_count].stack_bytes = 0;
    state->function_count += 1U;
    return 0;
}

int should_prefer_word_index(const char *name, const char *type_text) {
    if (names_equal(name, "argv") || names_equal(name, "envp")) {
        return 1;
    }
    if (text_contains(type_text, "[") && text_contains(type_text, "*")) {
        return 1;
    }
    if (text_contains(type_text, "**")) {
        return 1;
    }
    if (text_contains(type_text, "*") && !text_contains(type_text, "char")) {
        return 1;
    }
    return 0;
}

int is_function_name(const BackendState *state, const char *name) {
    return find_function_index(state, name) >= 0;
}

int find_global(const BackendState *state, const char *name) {
    size_t i;
    for (i = 0; i < state->global_count; ++i) {
        if (names_equal(state->globals[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

int find_constant(const BackendState *state, const char *name) {
    size_t i;
    for (i = 0; i < state->constant_count; ++i) {
        if (names_equal(state->constants[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

int add_constant(BackendState *state, const char *name, long long value) {
    int existing = find_constant(state, name);

    if (existing >= 0) {
        state->constants[existing].value = value;
        return 0;
    }

    if (state->constant_count >= COMPILER_BACKEND_MAX_CONSTANTS) {
        backend_set_error(state->backend, "too many constants for backend");
        return -1;
    }

    rt_copy_string(state->constants[state->constant_count].name, sizeof(state->constants[state->constant_count].name), name);
    state->constants[state->constant_count].value = value;
    state->constant_count += 1U;
    return 0;
}

int add_global(
    BackendState *state,
    const char *name,
    const char *type_text,
    int is_array,
    int pointer_depth,
    int char_based,
    int prefers_word_index,
    int global,
    int has_storage
) {
    int existing = find_global(state, name);

    if (existing >= 0) {
        if (is_array) {
            state->globals[existing].is_array = 1;
        }
        if (type_text != 0 && type_text[0] != '\0') {
            rt_copy_string(state->globals[existing].type_text, sizeof(state->globals[existing].type_text), type_text);
        }
        if (prefers_word_index) {
            state->globals[existing].prefers_word_index = 1;
        }
        if (global) {
            state->globals[existing].global = 1;
        }
        if (has_storage) {
            state->globals[existing].has_storage = 1;
        }
        return existing;
    }

    if (state->global_count >= COMPILER_BACKEND_MAX_GLOBALS) {
        backend_set_error(state->backend, "too many globals for backend");
        return -1;
    }

    rt_copy_string(state->globals[state->global_count].name, sizeof(state->globals[state->global_count].name), name);
    rt_copy_string(state->globals[state->global_count].type_text, sizeof(state->globals[state->global_count].type_text), type_text != 0 ? type_text : "");
    state->globals[state->global_count].init_value = 0;
    state->globals[state->global_count].initialized = 0;
    state->globals[state->global_count].is_array = is_array;
    state->globals[state->global_count].pointer_depth = pointer_depth;
    state->globals[state->global_count].char_based = char_based;
    state->globals[state->global_count].prefers_word_index = prefers_word_index;
    state->globals[state->global_count].global = global ? 1 : 0;
    state->globals[state->global_count].has_storage = has_storage ? 1 : 0;
    state->global_count += 1U;
    return (int)(state->global_count - 1U);
}

int find_local(const BackendState *state, const char *name) {
    size_t i = state->local_count;
    while (i > 0) {
        i -= 1U;
        if (names_equal(state->locals[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

int allocate_local(BackendState *state, const char *name, const char *type_text, int stack_bytes, int is_array, int pointer_depth, int char_based, int prefers_word_index) {
    int slot_size = stack_bytes > 0 ? stack_bytes : (is_array ? BACKEND_ARRAY_STACK_BYTES : backend_stack_slot_size(state));

    if (state->local_count >= COMPILER_BACKEND_MAX_LOCALS) {
        backend_set_error(state->backend, "too many local variables for backend");
        return -1;
    }

    rt_copy_string(state->locals[state->local_count].name, sizeof(state->locals[state->local_count].name), name);
    rt_copy_string(state->locals[state->local_count].type_text, sizeof(state->locals[state->local_count].type_text), type_text != 0 ? type_text : "");
    state->locals[state->local_count].stack_bytes = slot_size;
    state->locals[state->local_count].offset = state->stack_size + slot_size;
    state->locals[state->local_count].is_array = is_array;
    state->locals[state->local_count].pointer_depth = pointer_depth;
    state->locals[state->local_count].char_based = char_based;
    state->locals[state->local_count].prefers_word_index = prefers_word_index;
    state->stack_size += slot_size;
    state->local_count += 1U;

    if (state->reserved_stack_size > 0 && state->stack_size > state->reserved_stack_size) {
        backend_set_error(state->backend, "local stack reservation mismatch");
        return -1;
    }

    return 0;
}

const char *lookup_name_type_text(const BackendState *state, const char *name) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index >= 0) {
        return state->locals[local_index].type_text;
    }
    if (global_index >= 0) {
        return state->globals[global_index].type_text;
    }
    return "";
}

int write_label_name(const BackendState *state, char *buffer, size_t buffer_size, const char *label) {
    rt_copy_string(buffer, buffer_size, ".L");
    if (state != 0 && state->current_function[0] != '\0') {
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), state->current_function);
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "_");
    }
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), label);
    return 0;
}

int emit_pop_to_register(BackendState *state, const char *reg) {
    char line[64];
    if (backend_is_aarch64(state)) {
        rt_copy_string(line, sizeof(line), "ldr ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", [sp]");
        return emit_instruction(state, line) == 0 &&
               emit_instruction(state, "add sp, sp, #16") == 0 ? 0 : -1;
    }
    rt_copy_string(line, sizeof(line), "popq ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
    return emit_instruction(state, line);
}

int emit_local_address(BackendState *state, int offset, const char *reg) {
    char line[128];
    char offset_text[32];

    rt_unsigned_to_string((unsigned long long)offset, offset_text, sizeof(offset_text));
    if (backend_is_aarch64(state)) {
        if (offset <= 4095) {
            rt_copy_string(line, sizeof(line), "sub ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", x29, #");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
            return emit_instruction(state, line);
        }
        if (emit_load_immediate_register(state, names_equal(reg, "x9") ? "x10" : "x9", offset) != 0) {
            return -1;
        }
        rt_copy_string(line, sizeof(line), "sub ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", x29, ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), names_equal(reg, "x9") ? "x10" : "x9");
        return emit_instruction(state, line);
    }

    rt_copy_string(line, sizeof(line), "leaq -");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rbp), ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
    return emit_instruction(state, line);
}

int emit_load_from_address_into_register(BackendState *state, const char *address_reg, const char *dst_reg, int byte_value) {
    char line[64];

    if (backend_is_aarch64(state)) {
        rt_copy_string(line, sizeof(line), byte_value ? "ldrb " : "ldr ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), byte_value ? "w" : "x");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg + 1);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", [");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), address_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
        return emit_instruction(state, line);
    }

    rt_copy_string(line, sizeof(line), byte_value ? "movzbq (" : "movq (");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), address_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "), ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
    return emit_instruction(state, line);
}

int emit_load_from_address_register(BackendState *state, const char *reg, int byte_value) {
    return emit_load_from_address_into_register(state, reg, backend_is_aarch64(state) ? "x0" : "%rax", byte_value);
}

int emit_move_value_register(BackendState *state, const char *dst_reg) {
    char line[64];

    if (backend_is_aarch64(state)) {
        if (names_equal(dst_reg, "x0")) {
            return 0;
        }
        rt_copy_string(line, sizeof(line), "mov ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", x0");
        return emit_instruction(state, line);
    }

    if (names_equal(dst_reg, "%rax")) {
        return 0;
    }
    rt_copy_string(line, sizeof(line), "movq %rax, ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
    return emit_instruction(state, line);
}

int emit_store_to_address_register(BackendState *state, const char *reg, int byte_value) {
    char line[64];
    if (backend_is_aarch64(state)) {
        rt_copy_string(line, sizeof(line), byte_value ? "strb w0, [" : "str x0, [");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
        return emit_instruction(state, line);
    }

    rt_copy_string(line, sizeof(line), byte_value ? "movb %al, (" : "movq %rax, (");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ")");
    return emit_instruction(state, line);
}

int emit_pop_address_and_store(BackendState *state, int byte_value) {
    return emit_pop_to_register(state, backend_is_aarch64(state) ? "x1" : "%rcx") == 0 &&
           emit_store_to_address_register(state, backend_is_aarch64(state) ? "x1" : "%rcx", byte_value) == 0 ? 0 : -1;
}

int find_string_literal(const BackendState *state, const char *text) {
    size_t i;
    for (i = 0; i < state->string_count; ++i) {
        if (names_equal(state->strings[i].text, text)) {
            return (int)i;
        }
    }
    return -1;
}

int add_string_literal(BackendState *state, const char *text) {
    char digits[32];
    int existing = find_string_literal(state, text);

    if (existing >= 0) {
        return existing;
    }
    if (state->string_count >= COMPILER_BACKEND_MAX_STRINGS) {
        backend_set_error(state->backend, "too many string literals for backend");
        return -1;
    }

    rt_copy_string(state->strings[state->string_count].label, sizeof(state->strings[state->string_count].label), "str");
    rt_unsigned_to_string((unsigned long long)state->string_count, digits, sizeof(digits));
    rt_copy_string(state->strings[state->string_count].label + rt_strlen(state->strings[state->string_count].label),
                   sizeof(state->strings[state->string_count].label) - rt_strlen(state->strings[state->string_count].label),
                   digits);
    rt_copy_string(state->strings[state->string_count].text, sizeof(state->strings[state->string_count].text), text);
    state->string_count += 1U;
    return (int)(state->string_count - 1U);
}

static int emit_darwin_global_address(BackendState *state, const char *symbol, const char *dst_reg) {
    char line[128];

    rt_copy_string(line, sizeof(line), "adrp ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@GOTPAGE");
    if (emit_instruction(state, line) != 0) {
        return -1;
    }

    rt_copy_string(line, sizeof(line), "ldr ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", [");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@GOTPAGEOFF]");
    return emit_instruction(state, line);
}

int emit_address_of_name(BackendState *state, const char *name) {
    char line[128];
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index >= 0) {
        return emit_local_address(state, state->locals[local_index].offset, backend_is_aarch64(state) ? "x0" : "%rax");
    }

    if (global_index >= 0) {
        char symbol[COMPILER_IR_NAME_CAPACITY];
        format_symbol_name(state, name, symbol, sizeof(symbol));
        if (backend_is_aarch64(state)) {
            if (backend_is_darwin(state)) {
                return emit_darwin_global_address(state, symbol, "x0");
            }
            rt_copy_string(line, sizeof(line), "adrp x0, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? "@PAGE" : "");
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
            rt_copy_string(line, sizeof(line), "add x0, x0, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? symbol : ":lo12:");
            if (!backend_is_darwin(state)) {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            } else {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@PAGEOFF");
            }
            return emit_instruction(state, line);
        }
        rt_copy_string(line, sizeof(line), "leaq ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), name);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip), %rax");
        return emit_instruction(state, line);
    }

    if (is_function_name(state, name)) {
        char symbol[COMPILER_IR_NAME_CAPACITY];
        format_symbol_name(state, name, symbol, sizeof(symbol));
        if (backend_is_aarch64(state)) {
            rt_copy_string(line, sizeof(line), "adrp x0, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? "@PAGE" : "");
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
            rt_copy_string(line, sizeof(line), "add x0, x0, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? symbol : ":lo12:");
            if (!backend_is_darwin(state)) {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            } else {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@PAGEOFF");
            }
            return emit_instruction(state, line);
        }
        rt_copy_string(line, sizeof(line), "leaq ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), name);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip), %rax");
        return emit_instruction(state, line);
    }

    backend_set_error(state->backend, "backend only supports address-of on known storage");
    return -1;
}

int emit_load_string_literal(BackendState *state, const char *text) {
    char line[128];
    int index = add_string_literal(state, text);
    const char *label;

    if (index < 0) {
        return -1;
    }
    label = state->strings[index].label;
    if (backend_is_aarch64(state)) {
        rt_copy_string(line, sizeof(line), "adrp x0, .L");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), label);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                       backend_is_darwin(state) ? "@PAGE" : "");
        if (emit_instruction(state, line) != 0) {
            return -1;
        }
        rt_copy_string(line, sizeof(line), "add x0, x0, ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                       backend_is_darwin(state) ? ".L" : ":lo12:.L");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), label);
        if (backend_is_darwin(state)) {
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@PAGEOFF");
        }
        return emit_instruction(state, line);
    }
    rt_copy_string(line, sizeof(line), "leaq .L");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), label);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip), %rax");
    return emit_instruction(state, line);
}

int emit_load_name_into_register(BackendState *state, const char *name, const char *dst_reg) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);
    int constant_index = find_constant(state, name);

    if (local_index >= 0) {
        const char *address_reg = backend_is_aarch64(state) ? "x9" : "%rax";
        if (state->locals[local_index].is_array) {
            return emit_local_address(state, state->locals[local_index].offset, dst_reg);
        }
        return emit_local_address(state, state->locals[local_index].offset, address_reg) == 0 &&
               emit_load_from_address_into_register(state, address_reg, dst_reg, 0) == 0 ? 0 : -1;
    }

    if (global_index >= 0) {
        char line[128];
        char symbol[COMPILER_IR_NAME_CAPACITY];
        if (state->globals[global_index].is_array) {
            return emit_address_of_name(state, name) == 0 &&
                   emit_move_value_register(state, dst_reg) == 0 ? 0 : -1;
        }
        format_symbol_name(state, name, symbol, sizeof(symbol));
        if (backend_is_aarch64(state)) {
            if (backend_is_darwin(state)) {
                return emit_darwin_global_address(state, symbol, dst_reg) == 0 &&
                       emit_load_from_address_into_register(state, dst_reg, dst_reg, 0) == 0 ? 0 : -1;
            }
            const char *base_reg = names_equal(dst_reg, "x9") ? "x10" : "x9";
            rt_copy_string(line, sizeof(line), "adrp ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), base_reg);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? "@PAGE" : "");
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
            rt_copy_string(line, sizeof(line), "ldr ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", [");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), base_reg);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? symbol : ":lo12:");
            if (!backend_is_darwin(state)) {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            } else {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@PAGEOFF");
            }
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
            return emit_instruction(state, line);
        }
        rt_copy_string(line, sizeof(line), "movq ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), name);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip), ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
        return emit_instruction(state, line);
    }

    if (constant_index >= 0) {
        return emit_load_immediate_register(state, dst_reg, state->constants[constant_index].value);
    }

    if (is_function_name(state, name)) {
        return emit_address_of_name(state, name) == 0 &&
               emit_move_value_register(state, dst_reg) == 0 ? 0 : -1;
    }

    if (names_equal(name, "NULL") || names_equal(name, "errno") || name_looks_like_macro_constant(name)) {
        return emit_load_immediate_register(state, dst_reg, 0);
    }

    backend_set_error(state->backend, "unsupported value reference in backend");
    return -1;
}

int emit_load_name(BackendState *state, const char *name) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);
    int constant_index = find_constant(state, name);

    if (local_index >= 0) {
        if (state->locals[local_index].is_array) {
            return emit_address_of_name(state, name);
        }
        return emit_local_address(state, state->locals[local_index].offset, backend_is_aarch64(state) ? "x9" : "%rax") == 0 &&
               emit_load_from_address_register(state, backend_is_aarch64(state) ? "x9" : "%rax", 0) == 0 ? 0 : -1;
    }

    if (global_index >= 0) {
        char line[128];
        char symbol[COMPILER_IR_NAME_CAPACITY];
        if (state->globals[global_index].is_array) {
            return emit_address_of_name(state, name);
        }
        format_symbol_name(state, name, symbol, sizeof(symbol));
        if (backend_is_aarch64(state)) {
            if (backend_is_darwin(state)) {
                return emit_darwin_global_address(state, symbol, "x9") == 0 &&
                       emit_instruction(state, "ldr x0, [x9]") == 0 ? 0 : -1;
            }
            rt_copy_string(line, sizeof(line), "adrp x9, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? "@PAGE" : "");
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
            rt_copy_string(line, sizeof(line), "ldr x0, [x9, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? symbol : ":lo12:");
            if (!backend_is_darwin(state)) {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            } else {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@PAGEOFF");
            }
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
            return emit_instruction(state, line);
        }
        rt_copy_string(line, sizeof(line), "movq ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), name);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip), %rax");
        return emit_instruction(state, line);
    }

    if (constant_index >= 0) {
        return emit_load_immediate(state, state->constants[constant_index].value);
    }

    if (is_function_name(state, name)) {
        return emit_address_of_name(state, name);
    }

    if (names_equal(name, "NULL") || names_equal(name, "errno") || name_looks_like_macro_constant(name)) {
        return backend_is_aarch64(state) ? emit_instruction(state, "mov x0, #0") :
                                           emit_instruction(state, "movq $0, %rax");
    }

    backend_set_error(state->backend, "unsupported value reference in backend");
    return -1;
}

int emit_store_name(BackendState *state, const char *name) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index >= 0) {
        if (state->locals[local_index].is_array) {
            backend_set_error(state->backend, "unsupported local array assignment in backend");
            return -1;
        }
        return emit_local_address(state, state->locals[local_index].offset, backend_is_aarch64(state) ? "x9" : "%rcx") == 0 &&
               emit_store_to_address_register(state, backend_is_aarch64(state) ? "x9" : "%rcx", 0) == 0 ? 0 : -1;
    }

    if (global_index >= 0) {
        char line[128];
        char symbol[COMPILER_IR_NAME_CAPACITY];
        format_symbol_name(state, name, symbol, sizeof(symbol));
        if (backend_is_aarch64(state)) {
            if (backend_is_darwin(state)) {
                return emit_darwin_global_address(state, symbol, "x9") == 0 &&
                       emit_instruction(state, "str x0, [x9]") == 0 ? 0 : -1;
            }
            rt_copy_string(line, sizeof(line), "adrp x9, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? "@PAGE" : "");
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
            rt_copy_string(line, sizeof(line), "str x0, [x9, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? symbol : ":lo12:");
            if (!backend_is_darwin(state)) {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            } else {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@PAGEOFF");
            }
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
            return emit_instruction(state, line);
        }
        rt_copy_string(line, sizeof(line), "movq %rax, ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), name);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip)");
        return emit_instruction(state, line);
    }

    if (names_equal(name, "errno") || name_looks_like_macro_constant(name)) {
        return 0;
    }

    backend_set_error(state->backend, "unknown assignment target in backend");
    return -1;
}

int emit_copy_object_to_name(BackendState *state, const char *name) {
    int local_index = find_local(state, name);
    int bytes;
    int offset;
    int chunk;

    if (local_index < 0 || !state->locals[local_index].is_array) {
        backend_set_error(state->backend, "unsupported object assignment target in backend");
        return -1;
    }

    bytes = state->locals[local_index].stack_bytes;
    offset = state->locals[local_index].offset;
    if (bytes <= 0) {
        return 0;
    }

    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "mov x10, x0") != 0 ||
            emit_local_address(state, offset, "x9") != 0) {
            return -1;
        }
        for (chunk = 0; chunk < bytes; chunk += 8) {
            char load_line[64];
            char store_line[64];
            char chunk_text[32];

            rt_unsigned_to_string((unsigned long long)chunk, chunk_text, sizeof(chunk_text));
            rt_copy_string(load_line, sizeof(load_line), "ldr x11, [x10, #");
            rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line), chunk_text);
            rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line), "]");
            rt_copy_string(store_line, sizeof(store_line), "str x11, [x9, #");
            rt_copy_string(store_line + rt_strlen(store_line), sizeof(store_line) - rt_strlen(store_line), chunk_text);
            rt_copy_string(store_line + rt_strlen(store_line), sizeof(store_line) - rt_strlen(store_line), "]");
            if (emit_instruction(state, load_line) != 0 || emit_instruction(state, store_line) != 0) {
                return -1;
            }
        }
        return emit_instruction(state, "mov x0, x9");
    }

    if (emit_instruction(state, "movq %rax, %rdx") != 0 ||
        emit_local_address(state, offset, "%rcx") != 0) {
        return -1;
    }
    for (chunk = 0; chunk < bytes; chunk += 8) {
        char load_line[64];
        char store_line[64];
        char chunk_text[32];

        rt_unsigned_to_string((unsigned long long)chunk, chunk_text, sizeof(chunk_text));
        rt_copy_string(load_line, sizeof(load_line), "movq ");
        rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line), chunk_text);
        rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line), "(%rdx), %r11");
        rt_copy_string(store_line, sizeof(store_line), "movq %r11, ");
        rt_copy_string(store_line + rt_strlen(store_line), sizeof(store_line) - rt_strlen(store_line), chunk_text);
        rt_copy_string(store_line + rt_strlen(store_line), sizeof(store_line) - rt_strlen(store_line), "(%rcx)");
        if (emit_instruction(state, load_line) != 0 || emit_instruction(state, store_line) != 0) {
            return -1;
        }
    }
    return emit_instruction(state, "movq %rcx, %rax");
}

int lookup_array_storage(const BackendState *state, const char *name, int *word_index_out) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index >= 0 && state->locals[local_index].is_array) {
        *word_index_out = state->locals[local_index].prefers_word_index;
        return 1;
    }
    if (global_index >= 0 && state->globals[global_index].is_array) {
        *word_index_out = state->globals[global_index].prefers_word_index;
        return 1;
    }
    return 0;
}

int emit_load_immediate_register(BackendState *state, const char *reg, long long value) {
    char digits[32];
    char line[96];

    if (!backend_is_aarch64(state)) {
        if (value < 0) {
            rt_unsigned_to_string((unsigned long long)(-value), digits, sizeof(digits));
            rt_copy_string(line, sizeof(line), "movq $-");
        } else {
            rt_unsigned_to_string((unsigned long long)value, digits, sizeof(digits));
            rt_copy_string(line, sizeof(line), "movq $");
        }
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        return emit_instruction(state, line);
    }

    {
        unsigned long long bits = (unsigned long long)value;
        unsigned int shifts[] = {0U, 16U, 32U, 48U};
        int emitted = 0;
        size_t i;

        for (i = 0; i < sizeof(shifts) / sizeof(shifts[0]); ++i) {
            unsigned long long part = (bits >> shifts[i]) & 0xffffULL;
            char part_text[32];

            if (part == 0ULL && emitted) {
                continue;
            }
            rt_unsigned_to_string(part, part_text, sizeof(part_text));
            if (!emitted) {
                rt_copy_string(line, sizeof(line), "movz ");
            } else {
                rt_copy_string(line, sizeof(line), "movk ");
            }
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", #");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), part_text);
            if (shifts[i] != 0U) {
                char shift_text[32];
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", lsl #");
                rt_unsigned_to_string((unsigned long long)shifts[i], shift_text, sizeof(shift_text));
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), shift_text);
            }
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
            emitted = 1;
        }

        if (!emitted) {
            rt_copy_string(line, sizeof(line), "mov ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", #0");
            return emit_instruction(state, line);
        }
    }

    return 0;
}

int emit_load_immediate(BackendState *state, long long value) {
    return emit_load_immediate_register(state, backend_is_aarch64(state) ? "x0" : "%rax", value);
}

int emit_push_value(BackendState *state) {
    if (backend_is_aarch64(state)) {
        return emit_instruction(state, "sub sp, sp, #16") == 0 &&
               emit_instruction(state, "str x0, [sp]") == 0 ? 0 : -1;
    }
    return emit_instruction(state, "pushq %rax");
}

int emit_cmp_zero(BackendState *state) {
    return backend_is_aarch64(state) ? emit_instruction(state, "cmp x0, #0") :
                                       emit_instruction(state, "cmpq $0, %rax");
}

int emit_set_condition(BackendState *state, const char *condition) {
    if (backend_is_aarch64(state)) {
        char line[32];
        rt_copy_string(line, sizeof(line), "cset x0, ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), condition);
        return emit_instruction(state, line);
    }

    {
        char line[32];
        const char *x86_condition = condition;
        if (names_equal(condition, "eq")) x86_condition = "e";
        else if (names_equal(condition, "gt")) x86_condition = "g";
        else if (names_equal(condition, "lt")) x86_condition = "l";
        rt_copy_string(line, sizeof(line), "set");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), x86_condition);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), " %al");
        return emit_instruction(state, line) == 0 &&
               emit_instruction(state, "movzbq %al, %rax") == 0 ? 0 : -1;
    }
}

int emit_jump_to_label(BackendState *state, const char *mnemonic, const char *label) {
    char asm_label[96];
    char scoped_label[64];

    write_label_name(state, scoped_label, sizeof(scoped_label), label);

    if (backend_is_aarch64(state)) {
        rt_copy_string(asm_label, sizeof(asm_label), mnemonic);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), " ");
    } else {
        rt_copy_string(asm_label, sizeof(asm_label), mnemonic);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), " ");
    }
    rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), scoped_label);
    return emit_instruction(state, asm_label);
}
