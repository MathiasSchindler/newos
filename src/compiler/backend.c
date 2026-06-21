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

const char *backend_private_label_prefix(const BackendState *state) {
    if (state != 0 &&
        state->backend != 0 &&
        backend_is_darwin(state) &&
        (state->backend->function_sections || state->backend->data_sections)) {
        return "L";
    }
    return ".L";
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
    const char *prefix = info != 0 ? info->global_symbol_prefix : "";

    rt_copy_string(buffer, buffer_size, prefix);
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), name);
    if (prefix[0] == '\0') {
        rt_copy_string(buffer, buffer_size, name);
    }
}

void build_static_local_symbol_name(const BackendState *state, const char *function_name, const char *name, char *buffer, size_t buffer_size) {
    (void)state;
    rt_copy_string(buffer, buffer_size, "__static_");
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), function_name != 0 && function_name[0] != '\0' ? function_name : "global");
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "_");
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), name != 0 ? name : "obj");
}

const char *copy_next_word(const char *cursor, char *buffer, size_t buffer_size) {
    size_t length = 0;

    while (*cursor != '\0' && *cursor != ' ' && length + 1 < buffer_size) {
        buffer[length++] = *cursor++;
    }
    buffer[length] = '\0';
    return skip_spaces(cursor);
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

static int lookup_builtin_macro_value(const char *name, long long *value_out) {
    if (name == 0 || value_out == 0) {
        return -1;
    }

    if (names_equal(name, "ULLONG_MAX") || names_equal(name, "ULONG_MAX") ||
        names_equal(name, "SIZE_MAX") || names_equal(name, "UINTPTR_MAX")) {
        *value_out = -1LL;
        return 0;
    }
    if (names_equal(name, "UINT_MAX")) {
        *value_out = 4294967295LL;
        return 0;
    }
    if (names_equal(name, "USHRT_MAX")) {
        *value_out = 65535LL;
        return 0;
    }
    if (names_equal(name, "UCHAR_MAX")) {
        *value_out = 255LL;
        return 0;
    }
    if (names_equal(name, "LLONG_MAX") || names_equal(name, "LONG_MAX")) {
        *value_out = 0x7fffffffffffffffLL;
        return 0;
    }
    if (names_equal(name, "INT_MAX")) {
        *value_out = 2147483647LL;
        return 0;
    }
    if (names_equal(name, "SHRT_MAX")) {
        *value_out = 32767LL;
        return 0;
    }
    if (names_equal(name, "LLONG_MIN") || names_equal(name, "LONG_MIN")) {
        *value_out = (-9223372036854775807LL - 1LL);
        return 0;
    }
    if (names_equal(name, "INT_MIN")) {
        *value_out = -2147483647LL - 1LL;
        return 0;
    }
    if (names_equal(name, "SHRT_MIN")) {
        *value_out = -32768LL;
        return 0;
    }
    if (names_equal(name, "CHAR_BIT")) {
        *value_out = 8LL;
        return 0;
    }
    return -1;
}


int write_label_name(const BackendState *state, char *buffer, size_t buffer_size, const char *label) {
    rt_copy_string(buffer, buffer_size, backend_private_label_prefix(state));
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

static int emit_aarch64_offset_address(BackendState *state,
                                       const char *dst_reg,
                                       const char *base_reg,
                                       int offset,
                                       const char *scratch_reg) {
    char line[128];
    char offset_text[32];

    if (!backend_is_aarch64(state)) {
        return 0;
    }
    if (offset == 0) {
        if (names_equal(dst_reg, base_reg)) {
            return 0;
        }
        rt_copy_string(line, sizeof(line), "mov ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), base_reg);
        return emit_instruction(state, line);
    }

    rt_unsigned_to_string((unsigned long long)offset, offset_text, sizeof(offset_text));
    if (offset <= 4095) {
        rt_copy_string(line, sizeof(line), "add ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), base_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", #");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
        return emit_instruction(state, line);
    }

    if (emit_load_immediate_register(state, scratch_reg != 0 ? scratch_reg : "x13", offset) != 0) {
        return -1;
    }
    rt_copy_string(line, sizeof(line), "add ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), base_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
    rt_copy_string(line + rt_strlen(line),
                   sizeof(line) - rt_strlen(line),
                   scratch_reg != 0 ? scratch_reg : "x13");
    return emit_instruction(state, line);
}

static int object_copy_chunk_size(int remaining) {
    if (remaining >= 8) return 8;
    if (remaining >= 4) return 4;
    if (remaining >= 2) return 2;
    return 1;
}

static int emit_aarch64_load_copy_chunk(BackendState *state, int chunk_size) {
    const char *load_op;

    if (chunk_size == 8) {
        load_op = "ldr x11, [x14]";
    } else if (chunk_size == 4) {
        load_op = "ldr w11, [x14]";
    } else if (chunk_size == 2) {
        load_op = "ldrh w11, [x14]";
    } else {
        load_op = "ldrb w11, [x14]";
    }

    return emit_instruction(state, load_op);
}

static int emit_aarch64_store_copy_chunk(BackendState *state, int chunk_size) {
    const char *store_op;

    if (chunk_size == 8) {
        store_op = "str x11, [x14]";
    } else if (chunk_size == 4) {
        store_op = "str w11, [x14]";
    } else if (chunk_size == 2) {
        store_op = "strh w11, [x14]";
    } else {
        store_op = "strb w11, [x14]";
    }

    return emit_instruction(state, store_op);
}

static int emit_x86_copy_chunk(BackendState *state, const char *src_reg, const char *dst_reg, int offset, int chunk_size) {
    char load_line[64];
    char store_line[64];
    char offset_text[32];
    const char *load_op = "movq ";
    const char *store_op = "movq %r11, ";

    if (chunk_size == 4) {
        load_op = "movl ";
        store_op = "movl %r11d, ";
    } else if (chunk_size == 2) {
        load_op = "movw ";
        store_op = "movw %r11w, ";
    } else if (chunk_size == 1) {
        load_op = "movb ";
        store_op = "movb %r11b, ";
    }

    rt_unsigned_to_string((unsigned long long)offset, offset_text, sizeof(offset_text));
    rt_copy_string(load_line, sizeof(load_line), load_op);
    rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line), offset_text);
    rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line), "(");
    rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line), src_reg);
    rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line), "), ");
    rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line),
                   chunk_size == 8 ? "%r11" : chunk_size == 4 ? "%r11d" : chunk_size == 2 ? "%r11w" : "%r11b");
    rt_copy_string(store_line, sizeof(store_line), store_op);
    rt_copy_string(store_line + rt_strlen(store_line), sizeof(store_line) - rt_strlen(store_line), offset_text);
    rt_copy_string(store_line + rt_strlen(store_line), sizeof(store_line) - rt_strlen(store_line), "(");
    rt_copy_string(store_line + rt_strlen(store_line), sizeof(store_line) - rt_strlen(store_line), dst_reg);
    rt_copy_string(store_line + rt_strlen(store_line), sizeof(store_line) - rt_strlen(store_line), ")");
    return emit_instruction(state, load_line) == 0 && emit_instruction(state, store_line) == 0 ? 0 : -1;
}

static const char *x86_reg32_name(const char *reg) {
    if (names_equal(reg, "%rax")) return "%eax";
    if (names_equal(reg, "%rbx")) return "%ebx";
    if (names_equal(reg, "%rcx")) return "%ecx";
    if (names_equal(reg, "%rdx")) return "%edx";
    if (names_equal(reg, "%rsi")) return "%esi";
    if (names_equal(reg, "%rdi")) return "%edi";
    if (names_equal(reg, "%r8")) return "%r8d";
    if (names_equal(reg, "%r9")) return "%r9d";
    if (names_equal(reg, "%r10")) return "%r10d";
    if (names_equal(reg, "%r11")) return "%r11d";
    if (names_equal(reg, "%r12")) return "%r12d";
    if (names_equal(reg, "%r13")) return "%r13d";
    if (names_equal(reg, "%r14")) return "%r14d";
    if (names_equal(reg, "%r15")) return "%r15d";
    return "%eax";
}

static const char *x86_reg16_name(const char *reg) {
    if (names_equal(reg, "%rax")) return "%ax";
    if (names_equal(reg, "%rbx")) return "%bx";
    if (names_equal(reg, "%rcx")) return "%cx";
    if (names_equal(reg, "%rdx")) return "%dx";
    if (names_equal(reg, "%rsi")) return "%si";
    if (names_equal(reg, "%rdi")) return "%di";
    if (names_equal(reg, "%r8")) return "%r8w";
    if (names_equal(reg, "%r9")) return "%r9w";
    if (names_equal(reg, "%r10")) return "%r10w";
    if (names_equal(reg, "%r11")) return "%r11w";
    if (names_equal(reg, "%r12")) return "%r12w";
    if (names_equal(reg, "%r13")) return "%r13w";
    if (names_equal(reg, "%r14")) return "%r14w";
    if (names_equal(reg, "%r15")) return "%r15w";
    return "%ax";
}

static const char *x86_reg8_name(const char *reg) {
    if (names_equal(reg, "%rax")) return "%al";
    if (names_equal(reg, "%rbx")) return "%bl";
    if (names_equal(reg, "%rcx")) return "%cl";
    if (names_equal(reg, "%rdx")) return "%dl";
    if (names_equal(reg, "%rsi")) return "%sil";
    if (names_equal(reg, "%rdi")) return "%dil";
    if (names_equal(reg, "%r8")) return "%r8b";
    if (names_equal(reg, "%r9")) return "%r9b";
    if (names_equal(reg, "%r10")) return "%r10b";
    if (names_equal(reg, "%r11")) return "%r11b";
    if (names_equal(reg, "%r12")) return "%r12b";
    if (names_equal(reg, "%r13")) return "%r13b";
    if (names_equal(reg, "%r14")) return "%r14b";
    if (names_equal(reg, "%r15")) return "%r15b";
    return "%al";
}

const char *backend_x86_cached_register_name(int index) {
    static const char *regs[] = {"%rbx", "%r12", "%r13", "%r14", "%r15"};
    if (index < 0 || index >= (int)(sizeof(regs) / sizeof(regs[0]))) {
        return 0;
    }
    return regs[index];
}

static int emit_load_cached_register(BackendState *state, const char *src_reg, const char *dst_reg, int access_size) {
    char line[64];
    const char *opcode = "movq";
    const char *src = src_reg;
    const char *dst = dst_reg;

    if (access_size == -1) {
        opcode = "movsbq";
        src = x86_reg8_name(src_reg);
    } else if (access_size == 1) {
        opcode = "movzbq";
        src = x86_reg8_name(src_reg);
    } else if (access_size == -2) {
        opcode = "movswq";
        src = x86_reg16_name(src_reg);
    } else if (access_size == 2) {
        opcode = "movzwq";
        src = x86_reg16_name(src_reg);
    } else if (access_size == -4) {
        opcode = "movslq";
        src = x86_reg32_name(src_reg);
    } else if (access_size == 4) {
        opcode = "movl";
        src = x86_reg32_name(src_reg);
        dst = x86_reg32_name(dst_reg);
    } else if (names_equal(src_reg, dst_reg)) {
        return 0;
    }

    rt_copy_string(line, sizeof(line), opcode);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), " ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), src);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst);
    return emit_instruction(state, line);
}

static int scalar_type_access_size(const char *type_text, int word_index) {
    return backend_type_access_size(type_text, word_index);
}

int emit_load_from_address_into_register(BackendState *state, const char *address_reg, const char *dst_reg, int byte_value) {
    char line[64];
    int access_size = byte_value;
    int sign_extend = 0;

    if (access_size == 0) {
        access_size = backend_stack_slot_size(state);
    } else if (access_size < 0) {
        sign_extend = 1;
        access_size = -access_size;
    }

    if (backend_is_aarch64(state)) {
        if (access_size == 1 && sign_extend) {
            rt_copy_string(line, sizeof(line), "ldrsb ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
        } else if (access_size == 1) {
            rt_copy_string(line, sizeof(line), "ldrb w");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg + 1);
        } else if (access_size == 2 && sign_extend) {
            rt_copy_string(line, sizeof(line), "ldrsh ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
        } else if (access_size == 2) {
            rt_copy_string(line, sizeof(line), "ldrh w");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg + 1);
        } else if (access_size == 4 && sign_extend) {
            rt_copy_string(line, sizeof(line), "ldrsw ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
        } else if (access_size == 4) {
            rt_copy_string(line, sizeof(line), "ldr w");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg + 1);
        } else {
            rt_copy_string(line, sizeof(line), "ldr x");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg + 1);
        }
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", [");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), address_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
        return emit_instruction(state, line);
    }

    if (access_size == 1 && sign_extend) {
        rt_copy_string(line, sizeof(line), "movsbq (");
    } else if (access_size == 1) {
        rt_copy_string(line, sizeof(line), "movzbq (");
    } else if (access_size == 2 && sign_extend) {
        rt_copy_string(line, sizeof(line), "movswq (");
    } else if (access_size == 2) {
        rt_copy_string(line, sizeof(line), "movzwq (");
    } else if (access_size == 4 && sign_extend) {
        rt_copy_string(line, sizeof(line), "movslq (");
    } else if (access_size == 4) {
        rt_copy_string(line, sizeof(line), "movl (");
    } else {
        rt_copy_string(line, sizeof(line), "movq (");
    }
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), address_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "), ");
    rt_copy_string(line + rt_strlen(line),
                   sizeof(line) - rt_strlen(line),
                   (access_size == 4 && !sign_extend) ? x86_reg32_name(dst_reg) : dst_reg);
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

void backend_invalidate_block_cache(BackendState *state) {
    state->block_cache_local = -1;
}

void backend_invalidate_block_cache_name(BackendState *state, const char *name) {
    if (state->block_cache_local >= 0 &&
        state->block_cache_local < (int)state->local_count &&
        names_equal(state->locals[state->block_cache_local].name, name)) {
        backend_invalidate_block_cache(state);
    }
}

static int backend_line_is_block_boundary(const char *line) {
    return starts_with(line, "label ") ||
           starts_with(line, "case ") ||
           starts_with(line, "default") ||
           starts_with(line, "brfalse ") ||
           starts_with(line, "jump ") ||
           starts_with(line, "ret") ||
           starts_with(line, "switch ") ||
           starts_with(line, "endswitch") ||
           starts_with(line, "endfunc ");
}

static int backend_line_stores_name(const char *line, const char *name) {
    const char *cursor;
    char target[COMPILER_IR_NAME_CAPACITY];
    size_t out = 0;

    if (starts_with(line, "store ")) {
        cursor = skip_spaces(line + 6);
    } else if (starts_with(line, "eval ")) {
        cursor = skip_spaces(line + 5);
    } else {
        return 0;
    }
    while (((*cursor >= 'a' && *cursor <= 'z') ||
            (*cursor >= 'A' && *cursor <= 'Z') ||
            (*cursor >= '0' && *cursor <= '9') ||
            *cursor == '_') &&
           out + 1U < sizeof(target)) {
        target[out++] = *cursor++;
    }
    target[out] = '\0';
    cursor = skip_spaces(cursor);
    if (!starts_with(line, "store ") && (*cursor != '=' || cursor[1] == '=')) {
        return 0;
    }
    return names_equal(target, name);
}

static int backend_local_can_use_block_cache(const BackendState *state, int local_index) {
    return !backend_is_aarch64(state) &&
           local_index >= 0 &&
           local_index < (int)state->local_count &&
           state->locals[local_index].cached_register < 0 &&
           !state->locals[local_index].static_storage &&
           !state->locals[local_index].is_array;
}

static int backend_local_future_uses_in_current_block(const BackendState *state, int local_index) {
    size_t i;
    const char *name;
    int uses = 0;

    if (state->ir == 0 || state->current_ir_index < 0 || local_index < 0 || local_index >= (int)state->local_count) {
        return 0;
    }
    name = state->locals[local_index].name;
    for (i = (size_t)state->current_ir_index + 1U; i < state->ir->count; ++i) {
        const char *line = state->ir->lines[i];

        if (backend_line_is_block_boundary(line)) {
            return uses;
        }
        if (backend_line_stores_name(line, name)) {
            return uses;
        }
        if (line_references_identifier(line, name)) {
            uses += 1;
        }
    }
    return uses;
}

static int emit_load_cached_block_local(BackendState *state, int local_index, const char *dst_reg, int access_size) {
    if (state->block_cache_local == local_index) {
        return emit_load_cached_register(state, "%r10", dst_reg, access_size) == 0 ? 1 : -1;
    }
    return 0;
}

int backend_seed_block_cache_from_register(BackendState *state, int local_index, const char *src_reg) {
    int access_size;
    int future_uses;
    int minimum_future_uses;
    char line[64];

    if (!backend_local_can_use_block_cache(state, local_index)) {
        return 0;
    }
    access_size = scalar_type_access_size(state->locals[local_index].type_text,
                                          state->locals[local_index].prefers_word_index);
    if (state->locals[local_index].pointer_depth > 0 && !state->locals[local_index].is_array) {
        access_size = 0;
    }
    future_uses = backend_local_future_uses_in_current_block(state, local_index);
    minimum_future_uses = access_size == 0 ? 1 : 2;
    if (future_uses < minimum_future_uses) {
        return 0;
    }
    if (!names_equal(src_reg, "%r10")) {
        rt_copy_string(line, sizeof(line), "movq ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), src_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %r10");
        if (emit_instruction(state, line) != 0) {
            return -1;
        }
    }
    state->block_cache_local = local_index;
    return 1;
}

int emit_store_to_address_register(BackendState *state, const char *reg, int byte_value) {
    char line[64];
    int access_size = byte_value;

    if (access_size == 0) {
        access_size = backend_stack_slot_size(state);
    } else if (access_size < 0) {
        access_size = -access_size;
    }
    if (backend_is_aarch64(state)) {
        if (access_size == 1) {
            rt_copy_string(line, sizeof(line), "strb w0, [");
        } else if (access_size == 2) {
            rt_copy_string(line, sizeof(line), "strh w0, [");
        } else if (access_size == 4) {
            rt_copy_string(line, sizeof(line), "str w0, [");
        } else {
            rt_copy_string(line, sizeof(line), "str x0, [");
        }
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
        return emit_instruction(state, line);
    }

    if (access_size == 1) {
        rt_copy_string(line, sizeof(line), "movb %al, (");
    } else if (access_size == 2) {
        rt_copy_string(line, sizeof(line), "movw %ax, (");
    } else if (access_size == 4) {
        rt_copy_string(line, sizeof(line), "movl %eax, (");
    } else {
        rt_copy_string(line, sizeof(line), "movq %rax, (");
    }
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ")");
    return emit_instruction(state, line);
}

int emit_pop_address_and_store(BackendState *state, int byte_value) {
    backend_invalidate_block_cache(state);
    return emit_pop_to_register(state, backend_is_aarch64(state) ? "x1" : "%rcx") == 0 &&
           emit_store_to_address_register(state, backend_is_aarch64(state) ? "x1" : "%rcx", byte_value) == 0 ? 0 : -1;
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
        if (state->locals[local_index].indirect_object &&
            state->locals[local_index].cached_register >= 0) {
            const char *src_reg = backend_x86_cached_register_name(state->locals[local_index].cached_register);
            if (src_reg == 0 || backend_is_aarch64(state)) {
                backend_set_error(state->backend, "invalid cached indirect object register");
                return -1;
            }
            return emit_load_cached_register(state, src_reg, "%rax", 0);
        }
        if (state->locals[local_index].cached_register >= 0) {
            char offset_text[32];
            const char *opcode = "movq";
            const char *src_reg = backend_x86_cached_register_name(state->locals[local_index].cached_register);
            int access_size = scalar_type_access_size(state->locals[local_index].type_text,
                                                      state->locals[local_index].prefers_word_index);

            if (src_reg == 0 || backend_is_aarch64(state) || state->locals[local_index].offset <= 0) {
                rt_copy_string(line, sizeof(line), "backend cannot take address of cached local ");
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), name);
                backend_set_error(state->backend, line);
                return -1;
            }
            if (state->locals[local_index].pointer_depth > 0) {
                access_size = 0;
            }
            if (access_size < 0) {
                access_size = -access_size;
            }
            if (access_size == 1) {
                opcode = "movb";
                src_reg = x86_reg8_name(src_reg);
            } else if (access_size == 2) {
                opcode = "movw";
                src_reg = x86_reg16_name(src_reg);
            } else if (access_size == 4) {
                opcode = "movl";
                src_reg = x86_reg32_name(src_reg);
            }
            rt_unsigned_to_string((unsigned long long)state->locals[local_index].offset,
                                  offset_text,
                                  sizeof(offset_text));
            rt_copy_string(line, sizeof(line), opcode);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), " ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), src_reg);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", -");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rbp)");
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
            state->locals[local_index].cached_register = -1;
        }
        if (state->locals[local_index].static_storage) {
            char formatted_symbol[COMPILER_IR_NAME_CAPACITY];
            const char *symbol = state->locals[local_index].symbol_name[0] != '\0'
                                     ? state->locals[local_index].symbol_name
                                     : state->locals[local_index].name;
            format_symbol_name(state, symbol, formatted_symbol, sizeof(formatted_symbol));
            if (backend_is_aarch64(state)) {
                if (backend_is_darwin(state)) {
                    return emit_darwin_global_address(state, formatted_symbol, "x0");
                }
                rt_copy_string(line, sizeof(line), "adrp x0, ");
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), formatted_symbol);
                if (emit_instruction(state, line) != 0) {
                    return -1;
                }
                rt_copy_string(line, sizeof(line), "add x0, x0, :lo12:");
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), formatted_symbol);
                return emit_instruction(state, line);
            }
            rt_copy_string(line, sizeof(line), "leaq ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), formatted_symbol);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip), %rax");
            return emit_instruction(state, line);
        }
        if (state->locals[local_index].indirect_object) {
            const char *addr_reg = backend_is_aarch64(state) ? "x9" : "%rax";
            return emit_local_address(state, state->locals[local_index].offset, addr_reg) == 0 &&
                   emit_load_from_address_register(state, addr_reg, 0) == 0 ? 0 : -1;
        }
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
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
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
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip), %rax");
        return emit_instruction(state, line);
    }

    backend_set_error(state->backend, "backend only supports address-of on known storage");
    return -1;
}

int emit_load_string_literal(BackendState *state, const char *text) {
    return emit_load_string_literal_bytes(state, text, rt_strlen(text != 0 ? text : ""));
}

int emit_load_string_literal_bytes(BackendState *state, const char *text, size_t length) {
    char line[128];
    int index = add_string_literal_bytes(state, text, length);
    const char *label;

    if (index < 0) {
        return -1;
    }
    label = state->strings[index].label;
    if (backend_is_aarch64(state)) {
        rt_copy_string(line, sizeof(line), "adrp x0, ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), backend_private_label_prefix(state));
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), label);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                       backend_is_darwin(state) ? "@PAGE" : "");
        if (emit_instruction(state, line) != 0) {
            return -1;
        }
        rt_copy_string(line, sizeof(line), "add x0, x0, ");
        if (backend_is_darwin(state)) {
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), backend_private_label_prefix(state));
        } else {
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ":lo12:.L");
        }
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
    long long builtin_value = 0;

    if (!backend_is_aarch64(state) && names_equal(dst_reg, "%r10")) {
        backend_invalidate_block_cache(state);
    }

    if (local_index >= 0) {
        if (state->locals[local_index].indirect_object &&
            state->locals[local_index].cached_register >= 0) {
            const char *src_reg = backend_x86_cached_register_name(state->locals[local_index].cached_register);
            if (src_reg == 0 || backend_is_aarch64(state)) {
                backend_set_error(state->backend, "invalid cached indirect object register");
                return -1;
            }
            return emit_load_cached_register(state, src_reg, dst_reg, 0);
        }
        if (state->locals[local_index].cached_register >= 0) {
            const char *src_reg = backend_x86_cached_register_name(state->locals[local_index].cached_register);
            int access_size = scalar_type_access_size(state->locals[local_index].type_text, state->locals[local_index].prefers_word_index);
            if (src_reg == 0 || backend_is_aarch64(state)) {
                backend_set_error(state->backend, "invalid cached local register");
                return -1;
            }
            if (state->locals[local_index].pointer_depth > 0) {
                access_size = 0;
            }
            return emit_load_cached_register(state, src_reg, dst_reg, access_size);
        }
        if (state->locals[local_index].static_storage) {
            if (state->locals[local_index].is_array) {
                return emit_address_of_name(state, name) == 0 &&
                       emit_move_value_register(state, dst_reg) == 0 ? 0 : -1;
            }
            {
                int access_size =
                    scalar_type_access_size(state->locals[local_index].type_text, state->locals[local_index].prefers_word_index);
                const char *address_reg = dst_reg;
                if (emit_address_of_name(state, name) != 0) {
                    return -1;
                }
                if ((backend_is_aarch64(state) && !names_equal(dst_reg, "x0")) ||
                    (!backend_is_aarch64(state) && !names_equal(dst_reg, "%rax"))) {
                    if (emit_move_value_register(state, dst_reg) != 0) {
                        return -1;
                    }
                } else {
                    address_reg = backend_is_aarch64(state) ? "x0" : "%rax";
                }
                return emit_load_from_address_into_register(state, address_reg, dst_reg, access_size);
            }
        }
        const char *address_reg = backend_is_aarch64(state) ? "x9" : "%rax";
        if (state->locals[local_index].is_array) {
            if (state->locals[local_index].indirect_object) {
                return emit_address_of_name(state, name) == 0 &&
                       emit_move_value_register(state, dst_reg) == 0 ? 0 : -1;
            }
            return emit_local_address(state, state->locals[local_index].offset, dst_reg);
        }
        {
            int cached_load;
            int access_size =
                scalar_type_access_size(state->locals[local_index].type_text, state->locals[local_index].prefers_word_index);
            if (state->locals[local_index].pointer_depth > 0 && !state->locals[local_index].is_array) {
                access_size = 0;
            }
            if (!names_equal(dst_reg, "%r10") && backend_local_can_use_block_cache(state, local_index)) {
                cached_load = emit_load_cached_block_local(state, local_index, dst_reg, access_size);
                if (cached_load != 0) {
                    return cached_load < 0 ? -1 : 0;
                }
            }
            return emit_local_address(state, state->locals[local_index].offset, address_reg) == 0 &&
                   emit_load_from_address_into_register(state, address_reg, dst_reg, access_size) == 0 ? 0 : -1;
        }
    }

    if (global_index >= 0) {
        if (state->globals[global_index].is_array) {
            return emit_address_of_name(state, name) == 0 &&
                   emit_move_value_register(state, dst_reg) == 0 ? 0 : -1;
        }
        {
            int access_size =
                scalar_type_access_size(state->globals[global_index].type_text, state->globals[global_index].prefers_word_index);
            if (state->globals[global_index].pointer_depth > 0 && !state->globals[global_index].is_array) {
                access_size = 0;
            }
            const char *address_reg = dst_reg;
            if (emit_address_of_name(state, name) != 0) {
                return -1;
            }
            if ((backend_is_aarch64(state) && !names_equal(dst_reg, "x0")) ||
                (!backend_is_aarch64(state) && !names_equal(dst_reg, "%rax"))) {
                if (emit_move_value_register(state, dst_reg) != 0) {
                    return -1;
                }
            } else {
                address_reg = backend_is_aarch64(state) ? "x0" : "%rax";
            }
            return emit_load_from_address_into_register(state, address_reg, dst_reg, access_size);
        }
    }

    if (constant_index >= 0) {
        return emit_load_immediate_register(state, dst_reg, state->constants[constant_index].value);
    }

    if (is_function_name(state, name)) {
        return emit_address_of_name(state, name) == 0 &&
               emit_move_value_register(state, dst_reg) == 0 ? 0 : -1;
    }

    if (lookup_builtin_macro_value(name, &builtin_value) == 0) {
        return emit_load_immediate_register(state, dst_reg, builtin_value);
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
    long long builtin_value = 0;

    if (local_index >= 0) {
        if (state->locals[local_index].indirect_object &&
            state->locals[local_index].cached_register >= 0) {
            const char *src_reg = backend_x86_cached_register_name(state->locals[local_index].cached_register);
            if (src_reg == 0 || backend_is_aarch64(state)) {
                backend_set_error(state->backend, "invalid cached indirect object register");
                return -1;
            }
            return emit_load_cached_register(state, src_reg, "%rax", 0);
        }
        if (state->locals[local_index].cached_register >= 0) {
            const char *src_reg = backend_x86_cached_register_name(state->locals[local_index].cached_register);
            int access_size = scalar_type_access_size(state->locals[local_index].type_text, state->locals[local_index].prefers_word_index);
            if (src_reg == 0 || backend_is_aarch64(state)) {
                backend_set_error(state->backend, "invalid cached local register");
                return -1;
            }
            if (state->locals[local_index].pointer_depth > 0) {
                access_size = 0;
            }
            return emit_load_cached_register(state, src_reg, "%rax", access_size);
        }
        if (state->locals[local_index].static_storage) {
            if (state->locals[local_index].is_array) {
                if (state->locals[local_index].indirect_object) {
                    return emit_address_of_name(state, name);
                }
                const char *type_text = skip_spaces(state->locals[local_index].type_text);
                if (!((starts_with(type_text, "struct:") || starts_with(type_text, "union:")) &&
                      !text_contains(type_text, "*") &&
                      !text_contains(type_text, "["))) {
                    return emit_address_of_name(state, name);
                }
            }
            {
                int access_size =
                    scalar_type_access_size(state->locals[local_index].type_text, state->locals[local_index].prefers_word_index);
                return emit_address_of_name(state, name) == 0 &&
                       emit_load_from_address_register(state, backend_is_aarch64(state) ? "x0" : "%rax", access_size) == 0 ? 0 : -1;
            }
        }
        if (state->locals[local_index].is_array) {
            const char *type_text = skip_spaces(state->locals[local_index].type_text);
            if ((starts_with(type_text, "struct:") || starts_with(type_text, "union:")) &&
                !text_contains(type_text, "*") &&
                !text_contains(type_text, "[")) {
                return emit_address_of_name(state, name) == 0 &&
                       emit_load_from_address_register(state, backend_is_aarch64(state) ? "x0" : "%rax", 0) == 0 ? 0 : -1;
            }
            return emit_address_of_name(state, name);
        }
        {
            int cached_load;
            int access_size =
                scalar_type_access_size(state->locals[local_index].type_text, state->locals[local_index].prefers_word_index);
            if (state->locals[local_index].pointer_depth > 0 && !state->locals[local_index].is_array) {
                access_size = 0;
            }
            if (backend_local_can_use_block_cache(state, local_index)) {
                cached_load = emit_load_cached_block_local(state, local_index, "%rax", access_size);
                if (cached_load != 0) {
                    return cached_load < 0 ? -1 : 0;
                }
            }
            return emit_local_address(state, state->locals[local_index].offset, backend_is_aarch64(state) ? "x9" : "%rax") == 0 &&
                   emit_load_from_address_register(state, backend_is_aarch64(state) ? "x9" : "%rax", access_size) == 0 ? 0 : -1;
        }
    }

    if (global_index >= 0) {
        if (state->globals[global_index].is_array) {
            const char *type_text = skip_spaces(state->globals[global_index].type_text);
            if (!((starts_with(type_text, "struct:") || starts_with(type_text, "union:")) &&
                  !text_contains(type_text, "*") &&
                  !text_contains(type_text, "["))) {
                return emit_address_of_name(state, name);
            }
        }
        {
            int access_size =
                scalar_type_access_size(state->globals[global_index].type_text, state->globals[global_index].prefers_word_index);
            if (state->globals[global_index].pointer_depth > 0 && !state->globals[global_index].is_array) {
                access_size = 0;
            }
            return emit_address_of_name(state, name) == 0 &&
                   emit_load_from_address_register(state, backend_is_aarch64(state) ? "x0" : "%rax", access_size) == 0 ? 0 : -1;
        }
    }

    if (constant_index >= 0) {
        return emit_load_immediate(state, state->constants[constant_index].value);
    }

    if (is_function_name(state, name)) {
        return emit_address_of_name(state, name);
    }

    if (lookup_builtin_macro_value(name, &builtin_value) == 0) {
        return emit_load_immediate(state, builtin_value);
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
    int access_size = 0;

    backend_invalidate_block_cache_name(state, name);

    if (local_index >= 0) {
        if (state->locals[local_index].cached_register >= 0) {
            const char *dst_reg = backend_x86_cached_register_name(state->locals[local_index].cached_register);
            return dst_reg != 0 ? emit_move_value_register(state, dst_reg) : -1;
        }
        if (state->locals[local_index].is_array) {
            backend_set_error(state->backend, "unsupported local array assignment in backend");
            return -1;
        }
        access_size = scalar_type_access_size(state->locals[local_index].type_text,
                                              state->locals[local_index].prefers_word_index);
        if (state->locals[local_index].pointer_depth > 0) {
            access_size = 0;
        }
        if (state->locals[local_index].static_storage) {
            char line[128];
            char symbol[COMPILER_IR_NAME_CAPACITY];
            const char *opcode = "movq";
            const char *src_reg = "%rax";
            const char *static_symbol = state->locals[local_index].symbol_name[0] != '\0'
                                            ? state->locals[local_index].symbol_name
                                            : state->locals[local_index].name;
            format_symbol_name(state, static_symbol, symbol, sizeof(symbol));
            if (access_size < 0) {
                access_size = -access_size;
            }
            if (backend_is_aarch64(state)) {
                if (backend_is_darwin(state)) {
                    return emit_darwin_global_address(state, symbol, "x9") == 0 &&
                           emit_store_to_address_register(state, "x9", access_size) == 0 ? 0 : -1;
                }
                rt_copy_string(line, sizeof(line), "adrp x9, ");
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
                if (emit_instruction(state, line) != 0) {
                    return -1;
                }
                rt_copy_string(line, sizeof(line), access_size == 1 ? "strb w0, [x9, :lo12:" :
                                               access_size == 2 ? "strh w0, [x9, :lo12:" :
                                               access_size == 4 ? "str w0, [x9, :lo12:" :
                                                                  "str x0, [x9, :lo12:");
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
                return emit_instruction(state, line);
            }
            if (access_size == 1) {
                opcode = "movb";
                src_reg = "%al";
            } else if (access_size == 2) {
                opcode = "movw";
                src_reg = "%ax";
            } else if (access_size == 4) {
                opcode = "movl";
                src_reg = "%eax";
            }
            rt_copy_string(line, sizeof(line), opcode);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), " ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), src_reg);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip)");
            return emit_instruction(state, line);
        }
        if (emit_local_address(state, state->locals[local_index].offset, backend_is_aarch64(state) ? "x9" : "%rcx") != 0 ||
            emit_store_to_address_register(state, backend_is_aarch64(state) ? "x9" : "%rcx", access_size) != 0) {
            return -1;
        }
        return backend_seed_block_cache_from_register(state, local_index, "%rax") < 0 ? -1 : 0;
    }

    if (global_index >= 0) {
        char line[128];
        char symbol[COMPILER_IR_NAME_CAPACITY];
        const char *opcode = "movq";
        const char *src_reg = "%rax";
        format_symbol_name(state, name, symbol, sizeof(symbol));
        access_size = scalar_type_access_size(state->globals[global_index].type_text,
                                              state->globals[global_index].prefers_word_index);
        if (state->globals[global_index].pointer_depth > 0) {
            access_size = 0;
        }
        if (access_size < 0) {
            access_size = -access_size;
        }
        if (backend_is_aarch64(state)) {
            if (backend_is_darwin(state)) {
                return emit_darwin_global_address(state, symbol, "x9") == 0 &&
                       emit_store_to_address_register(state, "x9", access_size) == 0 ? 0 : -1;
            }
            rt_copy_string(line, sizeof(line), "adrp x9, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? "@PAGE" : "");
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
            rt_copy_string(line, sizeof(line), access_size == 1 ? "strb w0, [x9, " :
                                           access_size == 2 ? "strh w0, [x9, " :
                                           access_size == 4 ? "str w0, [x9, " :
                                                              "str x0, [x9, ");
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
        if (access_size == 1) {
            opcode = "movb";
            src_reg = "%al";
        } else if (access_size == 2) {
            opcode = "movw";
            src_reg = "%ax";
        } else if (access_size == 4) {
            opcode = "movl";
            src_reg = "%eax";
        }
        rt_copy_string(line, sizeof(line), opcode);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), " ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), src_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip)");
        return emit_instruction(state, line);
    }

    if (names_equal(name, "errno") || name_looks_like_macro_constant(name)) {
        return 0;
    }

    backend_set_error(state->backend, "unknown assignment target in backend");
    return -1;
}

static int emit_copy_memory_call(BackendState *state, int bytes, const char *dst_reg, const char *src_reg) {
    char line[96];
    char symbol[COMPILER_IR_NAME_CAPACITY];

    if (bytes <= 0) {
        return 0;
    }

    format_symbol_name(state, "memcpy", symbol, sizeof(symbol));
    if (backend_is_aarch64(state)) {
        if (!names_equal(dst_reg, "x0")) {
            rt_copy_string(line, sizeof(line), "mov x0, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
        }
        if (!names_equal(src_reg, "x1")) {
            rt_copy_string(line, sizeof(line), "mov x1, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), src_reg);
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
        }
        if (emit_load_immediate_register(state, "x2", bytes) != 0) {
            return -1;
        }
        backend_invalidate_block_cache(state);
        rt_copy_string(line, sizeof(line), "bl ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
        return emit_instruction(state, line);
    }

    if (!names_equal(dst_reg, "%rdi")) {
        rt_copy_string(line, sizeof(line), "movq ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rdi");
        if (emit_instruction(state, line) != 0) {
            return -1;
        }
    }
    if (!names_equal(src_reg, "%rsi")) {
        rt_copy_string(line, sizeof(line), "movq ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), src_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rsi");
        if (emit_instruction(state, line) != 0) {
            return -1;
        }
    }
    if (emit_instruction(state, "movq %rdi, %rax") != 0 ||
        emit_load_immediate_register(state, "%rcx", bytes) != 0 ||
        emit_instruction(state, "rep movsb") != 0) {
        return -1;
    }
    backend_invalidate_block_cache(state);
    return 0;
}

int emit_copy_object_to_name(BackendState *state, const char *name) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);
    int bytes;
    int offset;
    int chunk;

    if ((local_index < 0 || !state->locals[local_index].is_array) &&
        (global_index < 0 || !state->globals[global_index].is_array || !state->globals[global_index].has_storage)) {
        backend_set_error(state->backend, "unsupported object assignment target in backend");
        return -1;
    }

    bytes = local_index >= 0 ? state->locals[local_index].stack_bytes
                             : (int)backend_type_storage_bytes(state, state->globals[global_index].type_text);
    offset = local_index >= 0 ? state->locals[local_index].offset : 0;
    if (bytes <= 0) {
        return 0;
    }

    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "mov x12, x0") != 0 ||
            ((local_index >= 0 && !state->locals[local_index].static_storage) ? emit_local_address(state, offset, "x13")
                                                                              : emit_address_of_name(state, name)) != 0) {
            return -1;
        }
        if ((local_index < 0 || state->locals[local_index].static_storage) && emit_instruction(state, "mov x13, x0") != 0) {
            return -1;
        }
        if (bytes > 128) {
            return emit_copy_memory_call(state, bytes, "x13", "x12");
        }
        for (chunk = 0; chunk < bytes; chunk += object_copy_chunk_size(bytes - chunk)) {
            int chunk_size = object_copy_chunk_size(bytes - chunk);
            if (emit_aarch64_offset_address(state, "x14", "x12", chunk, "x15") != 0 ||
                emit_aarch64_load_copy_chunk(state, chunk_size) != 0 ||
                emit_aarch64_offset_address(state, "x14", "x13", chunk, "x15") != 0 ||
                emit_aarch64_store_copy_chunk(state, chunk_size) != 0) {
                return -1;
            }
        }
        return emit_instruction(state, "mov x0, x13");
    }

    if (emit_instruction(state, "movq %rax, %rdx") != 0 ||
        ((local_index >= 0 && !state->locals[local_index].static_storage && !state->locals[local_index].indirect_object)
             ? emit_local_address(state, offset, "%rcx")
             : emit_address_of_name(state, name)) != 0) {
        return -1;
    }
    if ((local_index < 0 || state->locals[local_index].static_storage) && emit_instruction(state, "movq %rax, %rcx") != 0) {
        return -1;
    }
    if (bytes > 128) {
        return emit_copy_memory_call(state, bytes, "%rcx", "%rdx");
    }
    for (chunk = 0; chunk < bytes; chunk += object_copy_chunk_size(bytes - chunk)) {
        int chunk_size = object_copy_chunk_size(bytes - chunk);
        if (emit_x86_copy_chunk(state, "%rdx", "%rcx", chunk, chunk_size) != 0) {
            return -1;
        }
    }
    return emit_instruction(state, "movq %rcx, %rax");
}

int emit_copy_name_to_pointer_name(BackendState *state, const char *src_name, const char *dst_pointer_name) {
    int local_index = find_local(state, src_name);
    int dst_index = find_local(state, dst_pointer_name);
    int bytes;
    int offset;
    int chunk;

    if (local_index < 0 || dst_index < 0) {
        backend_set_error(state->backend, "unsupported object return in backend");
        return -1;
    }

    bytes = state->locals[local_index].stack_bytes;
    offset = state->locals[local_index].offset;
    if (bytes <= 0) {
        return emit_load_name(state, dst_pointer_name);
    }

    if (backend_is_aarch64(state)) {
        if (emit_load_name_into_register(state, dst_pointer_name, "x13") != 0 ||
            (state->locals[local_index].static_storage ? emit_address_of_name(state, src_name)
                                                       : emit_local_address(state, offset, "x12")) != 0) {
            return -1;
        }
        if (state->locals[local_index].static_storage && emit_instruction(state, "mov x12, x0") != 0) {
            return -1;
        }
        if (bytes > 128) {
            return emit_copy_memory_call(state, bytes, "x13", "x12");
        }
        for (chunk = 0; chunk < bytes; chunk += object_copy_chunk_size(bytes - chunk)) {
            int chunk_size = object_copy_chunk_size(bytes - chunk);
            if (emit_aarch64_offset_address(state, "x14", "x12", chunk, "x15") != 0 ||
                emit_aarch64_load_copy_chunk(state, chunk_size) != 0 ||
                emit_aarch64_offset_address(state, "x14", "x13", chunk, "x15") != 0 ||
                emit_aarch64_store_copy_chunk(state, chunk_size) != 0) {
                return -1;
            }
        }
        return emit_instruction(state, "mov x0, x13");
    }

    if (emit_load_name_into_register(state, dst_pointer_name, "%rcx") != 0 ||
        ((state->locals[local_index].static_storage || state->locals[local_index].indirect_object)
             ? emit_address_of_name(state, src_name)
             : emit_local_address(state, offset, "%rdx")) != 0) {
        return -1;
    }
    if (state->locals[local_index].static_storage && emit_instruction(state, "movq %rax, %rdx") != 0) {
        return -1;
    }
    if (bytes > 128) {
        return emit_copy_memory_call(state, bytes, "%rcx", "%rdx");
    }
    for (chunk = 0; chunk < bytes; chunk += object_copy_chunk_size(bytes - chunk)) {
        int chunk_size = object_copy_chunk_size(bytes - chunk);
        if (emit_x86_copy_chunk(state, "%rdx", "%rcx", chunk, chunk_size) != 0) {
            return -1;
        }
    }
    return emit_instruction(state, "movq %rcx, %rax");
}

int emit_copy_object_to_pushed_address(BackendState *state, int bytes) {
    int chunk;

    if (bytes <= 0) {
        return 0;
    }

    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "mov x12, x0") != 0 ||
            emit_instruction(state, "ldr x13, [sp]") != 0 ||
            emit_instruction(state, "add sp, sp, #16") != 0) {
            return -1;
        }
        if (bytes > 128) {
            return emit_copy_memory_call(state, bytes, "x13", "x12");
        }
        for (chunk = 0; chunk < bytes; chunk += object_copy_chunk_size(bytes - chunk)) {
            int chunk_size = object_copy_chunk_size(bytes - chunk);
            if (emit_aarch64_offset_address(state, "x14", "x12", chunk, "x15") != 0 ||
                emit_aarch64_load_copy_chunk(state, chunk_size) != 0 ||
                emit_aarch64_offset_address(state, "x14", "x13", chunk, "x15") != 0 ||
                emit_aarch64_store_copy_chunk(state, chunk_size) != 0) {
                return -1;
            }
        }
        return emit_instruction(state, "mov x0, x13");
    }

    if (emit_instruction(state, "movq %rax, %rdx") != 0 ||
        emit_pop_to_register(state, "%rcx") != 0) {
        return -1;
    }
    if (bytes > 128) {
        return emit_copy_memory_call(state, bytes, "%rcx", "%rdx");
    }
    for (chunk = 0; chunk < bytes; chunk += object_copy_chunk_size(bytes - chunk)) {
        int chunk_size = object_copy_chunk_size(bytes - chunk);
        if (emit_x86_copy_chunk(state, "%rdx", "%rcx", chunk, chunk_size) != 0) {
            return -1;
        }
    }
    return emit_instruction(state, "movq %rcx, %rax");
}

int lookup_array_storage(const BackendState *state, const char *name, int *word_index_out) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index >= 0 &&
        (state->locals[local_index].is_array ||
         ((!text_contains(state->locals[local_index].type_text, "*")) &&
          (starts_with(skip_spaces(state->locals[local_index].type_text), "struct:") ||
           starts_with(skip_spaces(state->locals[local_index].type_text), "union:"))))) {
        *word_index_out = state->locals[local_index].prefers_word_index;
        return 1;
    }
    if (global_index >= 0 &&
        (state->globals[global_index].is_array ||
         ((!text_contains(state->globals[global_index].type_text, "*")) &&
          (starts_with(skip_spaces(state->globals[global_index].type_text), "struct:") ||
           starts_with(skip_spaces(state->globals[global_index].type_text), "union:"))))) {
        *word_index_out = state->globals[global_index].prefers_word_index;
        return 1;
    }
    return 0;
}

int emit_load_immediate_register(BackendState *state, const char *reg, long long value) {
    char digits[32];
    char line[96];

    if (!backend_is_aarch64(state)) {
        unsigned long long magnitude;
        int needs_movabs = value < -2147483648LL || value > 2147483647LL;

        if (names_equal(reg, "%rax") && value == 0) {
            return emit_instruction(state, "xor %eax, %eax");
        }
        if (names_equal(reg, "%rax") && value >= -128 && value <= 127) {
            if (value < 0) {
                magnitude = (unsigned long long)(-(value + 1LL)) + 1ULL;
                rt_unsigned_to_string(magnitude, digits, sizeof(digits));
                rt_copy_string(line, sizeof(line), "pushq $-");
            } else {
                magnitude = (unsigned long long)value;
                rt_unsigned_to_string(magnitude, digits, sizeof(digits));
                rt_copy_string(line, sizeof(line), "pushq $");
            }
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
            return emit_instruction(state, "popq %rax");
        }
        if (names_equal(reg, "%rax") && value > 0 && value <= 2147483647LL) {
            rt_unsigned_to_string((unsigned long long)value, digits, sizeof(digits));
            rt_copy_string(line, sizeof(line), "movl $");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %eax");
            return emit_instruction(state, line);
        }

        if (value < 0) {
            magnitude = (unsigned long long)(-(value + 1LL)) + 1ULL;
            rt_unsigned_to_string(magnitude, digits, sizeof(digits));
            rt_copy_string(line, sizeof(line), needs_movabs ? "movabsq $-" : "movq $-");
        } else {
            magnitude = (unsigned long long)value;
            rt_unsigned_to_string(magnitude, digits, sizeof(digits));
            rt_copy_string(line, sizeof(line), needs_movabs ? "movabsq $" : "movq $");
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
                                       emit_instruction(state, "testq %rax, %rax");
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
               emit_instruction(state, "movzbl %al, %eax") == 0 ? 0 : -1;
    }
}

int emit_jump_to_label(BackendState *state, const char *mnemonic, const char *label) {
    char asm_label[160];
    char scoped_label[128];

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
