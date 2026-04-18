/* IR prescan, data emission, and assembly dispatch helpers. */

static int parse_decl_line(const char *line,
                           char *storage,
                           size_t storage_size,
                           char *kind,
                           size_t kind_size,
                           char *type_text,
                           size_t type_size,
                           char *name,
                           size_t name_size) {
    const char *cursor = line + 5;
    size_t length = 0;
    const char *scan;
    const char *last = 0;
    size_t type_length = 0;

    cursor = skip_spaces(cursor);
    while (*cursor != '\0' && *cursor != ' ' && length + 1 < storage_size) {
        storage[length++] = *cursor++;
    }
    storage[length] = '\0';
    cursor = skip_spaces(cursor);

    length = 0;
    while (*cursor != '\0' && *cursor != ' ' && length + 1 < kind_size) {
        kind[length++] = *cursor++;
    }
    kind[length] = '\0';

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
            if (add_function_name(state, name) != 0) {
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
            continue;
        }
        if (starts_with(line, "endfunc ")) {
            in_function = 0;
            continue;
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

        if (!in_function && starts_with(line, "decl global ")) {
            char storage[16];
            char kind[16];
            char type_text[128];
            char name[COMPILER_IR_NAME_CAPACITY];

            parse_decl_line(line, storage, sizeof(storage), kind, sizeof(kind), type_text, sizeof(type_text), name, sizeof(name));
            if (names_equal(kind, "obj") &&
                !is_function_name(state, name) &&
                add_global(state, name, text_contains(type_text, "[]"), should_prefer_word_index(name, type_text)) < 0) {
                return -1;
            }
            continue;
        }

        if (!in_function && starts_with(line, "store ")) {
            char name[COMPILER_IR_NAME_CAPACITY];
            const char *arrow = line + 6;
            size_t out = 0;
            long long value = 0;
            int global_index;

            while (*arrow != '\0' && !(arrow[0] == ' ' && arrow[1] == '<') && out + 1 < sizeof(name)) {
                name[out++] = *arrow++;
            }
            name[out] = '\0';
            global_index = find_global(state, name);
            if (global_index >= 0) {
                const char *expr = skip_spaces(arrow + 4);
                if (resolve_static_value(state, expr, &value) == 0) {
                    state->globals[global_index].init_value = value;
                    state->globals[global_index].initialized = 1;
                }
            }
        }
    }

    return 0;
}

static int emit_globals(BackendState *state) {
    size_t i;

    if (state->global_count == 0) {
        return 0;
    }

    if (emit_line(state, ".data") != 0) {
        set_error(state->backend, "failed to emit data section");
        return -1;
    }

    for (i = 0; i < state->global_count; ++i) {
        char digits[32];
        char line[128];
        char symbol[COMPILER_IR_NAME_CAPACITY];
        format_symbol_name(state, state->globals[i].name, symbol, sizeof(symbol));

        if (emit_text(state, ".globl ") != 0 ||
            emit_line(state, symbol) != 0 ||
            emit_text(state, symbol) != 0 ||
            emit_line(state, ":") != 0) {
            set_error(state->backend, "failed to emit global symbol");
            return -1;
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
            set_error(state->backend, "failed to emit global initializer");
            return -1;
        }
    }

    return 0;
}

static int emit_string_literals(BackendState *state) {
    size_t i;

    if (state->string_count == 0) {
        return 0;
    }
    if (emit_line(state, ".data") != 0) {
        set_error(state->backend, "failed to emit string literal section");
        return -1;
    }
    for (i = 0; i < state->string_count; ++i) {
        char line[COMPILER_IR_LINE_CAPACITY];
        size_t out = 0;
        size_t j;

        rt_copy_string(line, sizeof(line), ".L");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), state->strings[i].label);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ":");
        if (emit_line(state, line) != 0) {
            set_error(state->backend, "failed to emit string literal label");
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
            set_error(state->backend, "failed to emit string literal contents");
            return -1;
        }
    }

    return 0;
}

static int begin_function(BackendState *state, const char *name) {
    char symbol[COMPILER_IR_NAME_CAPACITY];
    state->in_function = 1;
    state->local_count = 0;
    state->param_count = 0;
    state->saw_return_in_function = 0;
    state->stack_size = 0;
    rt_copy_string(state->current_function, sizeof(state->current_function), name);
    format_symbol_name(state, name, symbol, sizeof(symbol));

    if (emit_line(state, ".text") != 0) {
        set_error(state->backend, "failed to emit text section");
        return -1;
    }
    if (backend_is_aarch64(state) && emit_line(state, ".p2align 2") != 0) {
        set_error(state->backend, "failed to emit function alignment");
        return -1;
    }
    if (emit_text(state, ".globl ") != 0 ||
        emit_line(state, symbol) != 0 ||
        emit_text(state, symbol) != 0 ||
        emit_line(state, ":") != 0) {
        set_error(state->backend, "failed to emit function label");
        return -1;
    }
    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "stp x29, x30, [sp, #-16]!") != 0 ||
            emit_instruction(state, "mov x29, sp") != 0) {
            set_error(state->backend, "failed to emit AArch64 function prologue");
            return -1;
        }
    } else if (emit_instruction(state, "pushq %rbp") != 0 ||
               emit_instruction(state, "movq %rsp, %rbp") != 0) {
        set_error(state->backend, "failed to emit x86_64 function prologue");
        return -1;
    }
    return 0;
}

static int end_function(BackendState *state) {
    state->in_function = 0;
    state->local_count = 0;
    state->param_count = 0;
    state->saw_return_in_function = 0;
    state->stack_size = 0;
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

static int emit_decl_instruction(BackendState *state, const char *line) {
    char storage[16];
    char kind[16];
    char type_text[128];
    char name[COMPILER_IR_NAME_CAPACITY];
    int is_array;
    int prefers_word_index;

    parse_decl_line(line, storage, sizeof(storage), kind, sizeof(kind), type_text, sizeof(type_text), name, sizeof(name));
    is_array = text_contains(type_text, "[]");
    prefers_word_index = should_prefer_word_index(name, type_text);

    if (names_equal(storage, "param")) {
        static const char *const x86_arg_regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
        static const char *const aarch64_arg_regs[] = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
        char asm_line[128];
        int index = state->param_count;

        if (allocate_local(state, name, is_array, prefers_word_index) != 0) {
            return -1;
        }
        {
            int local_index = find_local(state, name);
            int max_register_params = backend_is_aarch64(state) ? 8 : 6;
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
        return allocate_local(state, name, is_array, prefers_word_index);
    }

    if (names_equal(kind, "func")) {
        return add_function_name(state, name);
    }

    return 0;
}

void compiler_backend_init(CompilerBackend *backend, CompilerBackendTarget target) {
    rt_memset(backend, 0, sizeof(*backend));
    backend->target = target;
}

int compiler_backend_emit_assembly(CompilerBackend *backend, const CompilerIr *ir, int fd) {
    static BackendState state;
    size_t i;

    rt_memset(&state, 0, sizeof(state));
    state.backend = backend;
    state.fd = fd;

    if (prescan_ir(&state, ir) != 0) {
        return -1;
    }
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
            if (state.in_function && !state.saw_return_in_function && emit_function_return(&state) != 0) {
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
            size_t out = 0;

            while (*expr != '\0' && !(expr[0] == ' ' && expr[1] == '<') && out + 1 < sizeof(name)) {
                name[out++] = *expr++;
            }
            name[out] = '\0';
            expr = skip_spaces(expr + 4);

            {
                int array_word_index = 0;
                int is_array_target = lookup_array_storage(&state, name, &array_word_index);

                if (*expr == '{') {
                    if (is_array_target) {
                        if (emit_array_initializer_store(&state, name, expr) != 0) {
                            set_error_with_line(backend, compiler_backend_error_message(backend), line);
                            return -1;
                        }
                    } else {
                        if (emit_object_initializer_store(&state, name, expr) != 0) {
                            set_error_with_line(backend, compiler_backend_error_message(backend), line);
                            return -1;
                        }
                    }
                    continue;
                }
                if (*expr == '"' && is_array_target) {
                    if (emit_array_initializer_store(&state, name, expr) != 0) {
                        set_error_with_line(backend, compiler_backend_error_message(backend), line);
                        return -1;
                    }
                    continue;
                }
            }
            if (emit_expression(&state, expr) != 0 || emit_store_name(&state, name) != 0) {
                set_error_with_line(backend, compiler_backend_error_message(backend), line);
                return -1;
            }
            continue;
        }

        if (starts_with(line, "eval ")) {
            if (emit_expression(&state, line + 5) != 0) {
                set_error_with_line(backend, compiler_backend_error_message(backend), line);
                return -1;
            }
            continue;
        }

        if (starts_with(line, "ret")) {
            const char *expr = skip_spaces(line + 3);
            if (expr[0] == '\0') {
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

        if (starts_with(line, "brfalse ")) {
            const char *arrow = line + 8;
            char expr[COMPILER_IR_LINE_CAPACITY];
            char label[COMPILER_IR_NAME_CAPACITY];
            char asm_label[96];
            size_t out = 0;

            const char *separator = find_ir_separator_outside_quotes(arrow, " -> ");
            if (separator == 0) {
                set_error_with_line(backend, "malformed branch instruction in backend", line);
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
            write_label_name(asm_label, sizeof(asm_label), line + 6);
            rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
            if (emit_line(&state, asm_label) != 0) {
                set_error(backend, "failed to emit branch label");
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
