#include "backend.h"

#include "runtime.h"

#define COMPILER_BACKEND_MAX_FUNCTIONS 256
#define COMPILER_BACKEND_MAX_GLOBALS 256
#define COMPILER_BACKEND_MAX_LOCALS 256

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
} BackendFunctionName;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    long long init_value;
    int initialized;
} BackendGlobal;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    int offset;
} BackendLocal;

typedef struct {
    CompilerBackend *backend;
    int fd;
    BackendFunctionName functions[COMPILER_BACKEND_MAX_FUNCTIONS];
    size_t function_count;
    BackendGlobal globals[COMPILER_BACKEND_MAX_GLOBALS];
    size_t global_count;
    BackendLocal locals[COMPILER_BACKEND_MAX_LOCALS];
    size_t local_count;
    char current_function[COMPILER_IR_NAME_CAPACITY];
    int in_function;
    int param_count;
    int saw_return_in_function;
    unsigned int label_counter;
} BackendState;

typedef enum {
    EXPR_TOKEN_EOF = 0,
    EXPR_TOKEN_IDENTIFIER,
    EXPR_TOKEN_NUMBER,
    EXPR_TOKEN_CHAR,
    EXPR_TOKEN_STRING,
    EXPR_TOKEN_PUNCT
} ExprTokenKind;

typedef struct {
    ExprTokenKind kind;
    char text[COMPILER_IR_LINE_CAPACITY];
    long long number_value;
} ExprToken;

typedef struct {
    const char *cursor;
    ExprToken current;
    BackendState *state;
} ExprParser;

static int expr_parse_expression(ExprParser *parser);

static void set_error(CompilerBackend *backend, const char *message) {
    rt_copy_string(backend->error_message, sizeof(backend->error_message), message != 0 ? message : "backend error");
}

static int emit_text(BackendState *state, const char *text) {
    return rt_write_cstr(state->fd, text);
}

static int emit_line(BackendState *state, const char *text) {
    return rt_write_line(state->fd, text);
}

static int emit_instruction(BackendState *state, const char *text) {
    if (emit_text(state, "    ") != 0 || emit_line(state, text) != 0) {
        set_error(state->backend, "failed to write assembly output");
        return -1;
    }
    return 0;
}

static int names_equal(const char *lhs, const char *rhs) {
    return rt_strcmp(lhs, rhs) == 0;
}

static int starts_with(const char *text, const char *prefix) {
    size_t i = 0;
    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }
    return 1;
}

static const char *skip_spaces(const char *text) {
    while (*text == ' ' || *text == '\t') {
        text += 1;
    }
    return text;
}

static int backend_is_aarch64(const BackendState *state) {
    return state->backend->target == COMPILER_BACKEND_TARGET_LINUX_AARCH64 ||
           state->backend->target == COMPILER_BACKEND_TARGET_MACOS_AARCH64;
}

static int backend_is_darwin(const BackendState *state) {
    return state->backend->target == COMPILER_BACKEND_TARGET_MACOS_AARCH64;
}

static void format_symbol_name(const BackendState *state, const char *name, char *buffer, size_t buffer_size) {
    if (backend_is_darwin(state)) {
        rt_copy_string(buffer, buffer_size, "_");
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), name);
    } else {
        rt_copy_string(buffer, buffer_size, name);
    }
}

static void copy_last_word(const char *text, char *buffer, size_t buffer_size) {
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

static int parse_signed_value(const char *text, long long *value_out) {
    int negative = 0;
    unsigned long long magnitude = 0;

    text = skip_spaces(text);
    if (*text == '-') {
        negative = 1;
        text += 1;
    } else if (*text == '+') {
        text += 1;
    }

    if (rt_parse_uint(text, &magnitude) != 0) {
        return -1;
    }

    *value_out = negative ? -(long long)magnitude : (long long)magnitude;
    return 0;
}

static int add_function_name(BackendState *state, const char *name) {
    size_t i;

    for (i = 0; i < state->function_count; ++i) {
        if (names_equal(state->functions[i].name, name)) {
            return 0;
        }
    }

    if (state->function_count >= COMPILER_BACKEND_MAX_FUNCTIONS) {
        set_error(state->backend, "too many functions for backend");
        return -1;
    }

    rt_copy_string(state->functions[state->function_count].name, sizeof(state->functions[state->function_count].name), name);
    state->function_count += 1U;
    return 0;
}

static int is_function_name(const BackendState *state, const char *name) {
    size_t i;
    for (i = 0; i < state->function_count; ++i) {
        if (names_equal(state->functions[i].name, name)) {
            return 1;
        }
    }
    return 0;
}

static int find_global(const BackendState *state, const char *name) {
    size_t i;
    for (i = 0; i < state->global_count; ++i) {
        if (names_equal(state->globals[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

static int add_global(BackendState *state, const char *name) {
    int existing = find_global(state, name);

    if (existing >= 0) {
        return existing;
    }

    if (state->global_count >= COMPILER_BACKEND_MAX_GLOBALS) {
        set_error(state->backend, "too many globals for backend");
        return -1;
    }

    rt_copy_string(state->globals[state->global_count].name, sizeof(state->globals[state->global_count].name), name);
    state->globals[state->global_count].init_value = 0;
    state->globals[state->global_count].initialized = 0;
    state->global_count += 1U;
    return (int)(state->global_count - 1U);
}

static int find_local(const BackendState *state, const char *name) {
    size_t i = state->local_count;
    while (i > 0) {
        i -= 1U;
        if (names_equal(state->locals[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

static int allocate_local(BackendState *state, const char *name) {
    char line[64];
    char offset_text[32];

    if (find_local(state, name) >= 0) {
        return 0;
    }

    if (state->local_count >= COMPILER_BACKEND_MAX_LOCALS) {
        set_error(state->backend, "too many local variables for backend");
        return -1;
    }

    rt_copy_string(state->locals[state->local_count].name, sizeof(state->locals[state->local_count].name), name);
    state->locals[state->local_count].offset = (int)((state->local_count + 1U) * (backend_is_aarch64(state) ? 16U : 8U));
    state->local_count += 1U;

    rt_unsigned_to_string((unsigned long long)(backend_is_aarch64(state) ? 16U : 8U), offset_text, sizeof(offset_text));
    if (backend_is_aarch64(state)) {
        rt_copy_string(line, sizeof(line), "sub sp, sp, #");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
    } else {
        rt_copy_string(line, sizeof(line), "subq $");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rsp");
    }
    return emit_instruction(state, line);
}

static int write_label_name(char *buffer, size_t buffer_size, const char *label) {
    rt_copy_string(buffer, buffer_size, ".L");
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), label);
    return 0;
}

static int emit_load_name(BackendState *state, const char *name) {
    char line[128];
    int local_index = find_local(state, name);
    int global_index;

    if (local_index >= 0) {
        char offset_text[32];
        rt_unsigned_to_string((unsigned long long)state->locals[local_index].offset, offset_text, sizeof(offset_text));
        if (backend_is_aarch64(state)) {
            rt_copy_string(line, sizeof(line), "ldur x0, [x29, #-");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
            return emit_instruction(state, line);
        }
        rt_copy_string(line, sizeof(line), "movq -");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rbp), %rax");
        return emit_instruction(state, line);
    }

    global_index = find_global(state, name);
    if (global_index >= 0) {
        char symbol[COMPILER_IR_NAME_CAPACITY];
        format_symbol_name(state, name, symbol, sizeof(symbol));
        if (backend_is_aarch64(state)) {
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

    if (names_equal(name, "NULL")) {
        return backend_is_aarch64(state) ? emit_instruction(state, "mov x0, #0") :
                                           emit_instruction(state, "movq $0, %rax");
    }

    set_error(state->backend, "unsupported value reference in backend");
    return -1;
}

static int emit_store_name(BackendState *state, const char *name) {
    char line[128];
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index >= 0) {
        char offset_text[32];
        rt_unsigned_to_string((unsigned long long)state->locals[local_index].offset, offset_text, sizeof(offset_text));
        if (backend_is_aarch64(state)) {
            rt_copy_string(line, sizeof(line), "stur x0, [x29, #-");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
            return emit_instruction(state, line);
        }
        rt_copy_string(line, sizeof(line), "movq %rax, -");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rbp)");
        return emit_instruction(state, line);
    }

    if (global_index >= 0) {
        char symbol[COMPILER_IR_NAME_CAPACITY];
        format_symbol_name(state, name, symbol, sizeof(symbol));
        if (backend_is_aarch64(state)) {
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

    set_error(state->backend, "unsupported assignment target in backend");
    return -1;
}

static int emit_load_immediate(BackendState *state, long long value) {
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
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rax");
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
                rt_copy_string(line, sizeof(line), "movz x0, #");
            } else {
                rt_copy_string(line, sizeof(line), "movk x0, #");
            }
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
            return emit_instruction(state, "mov x0, #0");
        }
    }

    return 0;
}

static int emit_push_value(BackendState *state) {
    if (backend_is_aarch64(state)) {
        return emit_instruction(state, "sub sp, sp, #16") == 0 &&
               emit_instruction(state, "str x0, [sp]") == 0 ? 0 : -1;
    }
    return emit_instruction(state, "pushq %rax");
}

static int emit_cmp_zero(BackendState *state) {
    return backend_is_aarch64(state) ? emit_instruction(state, "cmp x0, #0") :
                                       emit_instruction(state, "cmpq $0, %rax");
}

static int emit_set_condition(BackendState *state, const char *condition) {
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

static int emit_jump_to_label(BackendState *state, const char *mnemonic, const char *label) {
    char asm_label[96];

    if (backend_is_aarch64(state)) {
        rt_copy_string(asm_label, sizeof(asm_label), mnemonic);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), " .L");
    } else {
        rt_copy_string(asm_label, sizeof(asm_label), mnemonic);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), " .L");
    }
    rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), label);
    return emit_instruction(state, asm_label);
}

static int expr_match_punct(ExprParser *parser, const char *text);

static void expr_next(ExprParser *parser) {
    const char *cursor = skip_spaces(parser->cursor);
    size_t length = 0;

    parser->cursor = cursor;
    parser->current.text[0] = '\0';
    parser->current.number_value = 0;

    if (*cursor == '\0') {
        parser->current.kind = EXPR_TOKEN_EOF;
        return;
    }

    if ((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') || *cursor == '_') {
        parser->current.kind = EXPR_TOKEN_IDENTIFIER;
        while (((*cursor >= 'a' && *cursor <= 'z') ||
                (*cursor >= 'A' && *cursor <= 'Z') ||
                (*cursor >= '0' && *cursor <= '9') ||
                *cursor == '_') &&
               length + 1 < sizeof(parser->current.text)) {
            parser->current.text[length++] = *cursor++;
        }
        parser->current.text[length] = '\0';
        parser->cursor = cursor;
        return;
    }

    if (*cursor >= '0' && *cursor <= '9') {
        parser->current.kind = EXPR_TOKEN_NUMBER;
        while (*cursor >= '0' && *cursor <= '9' && length + 1 < sizeof(parser->current.text)) {
            parser->current.text[length++] = *cursor++;
        }
        parser->current.text[length] = '\0';
        (void)parse_signed_value(parser->current.text, &parser->current.number_value);
        parser->cursor = cursor;
        return;
    }

    if (*cursor == '\'') {
        char ch = 0;
        parser->current.kind = EXPR_TOKEN_CHAR;
        cursor += 1;
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor += 1;
            if (*cursor == 'n') ch = '\n';
            else if (*cursor == 't') ch = '\t';
            else if (*cursor == 'r') ch = '\r';
            else if (*cursor == '0') ch = '\0';
            else ch = *cursor;
        } else {
            ch = *cursor;
        }
        parser->current.number_value = (long long)(unsigned char)ch;
        while (*cursor != '\0' && *cursor != '\'') {
            cursor += 1;
        }
        if (*cursor == '\'') {
            cursor += 1;
        }
        parser->cursor = cursor;
        return;
    }

    if (*cursor == '"') {
        parser->current.kind = EXPR_TOKEN_STRING;
        cursor += 1;
        while (*cursor != '\0' && *cursor != '"' && length + 1 < sizeof(parser->current.text)) {
            parser->current.text[length++] = *cursor++;
        }
        parser->current.text[length] = '\0';
        if (*cursor == '"') {
            cursor += 1;
        }
        parser->cursor = cursor;
        return;
    }

    parser->current.kind = EXPR_TOKEN_PUNCT;
    if ((cursor[0] == '&' && cursor[1] == '&') ||
        (cursor[0] == '|' && cursor[1] == '|') ||
        (cursor[0] == '=' && cursor[1] == '=') ||
        (cursor[0] == '!' && cursor[1] == '=') ||
        (cursor[0] == '<' && cursor[1] == '=') ||
        (cursor[0] == '>' && cursor[1] == '=') ||
        (cursor[0] == '<' && cursor[1] == '<') ||
        (cursor[0] == '>' && cursor[1] == '>') ||
        (cursor[0] == '+' && cursor[1] == '=') ||
        (cursor[0] == '-' && cursor[1] == '=') ||
        (cursor[0] == '*' && cursor[1] == '=') ||
        (cursor[0] == '/' && cursor[1] == '=') ||
        (cursor[0] == '%' && cursor[1] == '=') ||
        (cursor[0] == '+' && cursor[1] == '+') ||
        (cursor[0] == '-' && cursor[1] == '-') ||
        (cursor[0] == '-' && cursor[1] == '>')) {
        parser->current.text[0] = cursor[0];
        parser->current.text[1] = cursor[1];
        parser->current.text[2] = '\0';
        parser->cursor = cursor + 2;
        return;
    }

    parser->current.text[0] = *cursor;
    parser->current.text[1] = '\0';
    parser->cursor = cursor + 1;
}

static int expr_match_punct(ExprParser *parser, const char *text) {
    if (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, text)) {
        expr_next(parser);
        return 1;
    }
    return 0;
}

static int expr_expect_punct(ExprParser *parser, const char *text) {
    if (!expr_match_punct(parser, text)) {
        set_error(parser->state->backend, "unsupported expression syntax in backend");
        return -1;
    }
    return 0;
}

static int expr_parse_assignment(ExprParser *parser);

static int expr_parse_primary(ExprParser *parser) {
    if (parser->current.kind == EXPR_TOKEN_NUMBER || parser->current.kind == EXPR_TOKEN_CHAR) {
        long long value = parser->current.number_value;
        expr_next(parser);
        return emit_load_immediate(parser->state, value);
    }

    if (parser->current.kind == EXPR_TOKEN_IDENTIFIER) {
        char name[COMPILER_IR_NAME_CAPACITY];
        ExprParser snapshot = *parser;

        rt_copy_string(name, sizeof(name), parser->current.text);
        expr_next(&snapshot);
        if (snapshot.current.kind == EXPR_TOKEN_PUNCT && names_equal(snapshot.current.text, "(")) {
            static const char *const x86_arg_regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
            static const char *const aarch64_arg_regs[] = {"x0", "x1", "x2", "x3", "x4", "x5"};
            int arg_count = 0;

            expr_next(parser);
            (void)expr_match_punct(parser, "(");
            if (!(parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, ")"))) {
                for (;;) {
                    if (expr_parse_expression(parser) != 0 || emit_push_value(parser->state) != 0) {
                        return -1;
                    }
                    arg_count += 1;
                    if (arg_count > 6) {
                        set_error(parser->state->backend, "backend only supports up to 6 call arguments");
                        return -1;
                    }
                    if (!expr_match_punct(parser, ",")) {
                        break;
                    }
                }
            }
            if (expr_expect_punct(parser, ")") != 0) {
                return -1;
            }
            while (arg_count > 0) {
                arg_count -= 1;
                {
                    char line[64];
                    const char *reg = backend_is_aarch64(parser->state) ? aarch64_arg_regs[arg_count] : x86_arg_regs[arg_count];
                    if (backend_is_aarch64(parser->state)) {
                        rt_copy_string(line, sizeof(line), "ldr ");
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", [sp]");
                        if (emit_instruction(parser->state, line) != 0 ||
                            emit_instruction(parser->state, "add sp, sp, #16") != 0) {
                            return -1;
                        }
                        continue;
                    }
                    rt_copy_string(line, sizeof(line), "popq ");
                    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
                    if (emit_instruction(parser->state, line) != 0) {
                        return -1;
                    }
                }
            }
            {
                char line[96];
                char symbol[COMPILER_IR_NAME_CAPACITY];
                format_symbol_name(parser->state, name, symbol, sizeof(symbol));
                rt_copy_string(line, sizeof(line), backend_is_aarch64(parser->state) ? "bl " : "call ");
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
                return emit_instruction(parser->state, line);
            }
        }

        expr_next(parser);
        return emit_load_name(parser->state, name);
    }

    if (expr_match_punct(parser, "(")) {
        if (expr_parse_expression(parser) != 0 || expr_expect_punct(parser, ")") != 0) {
            return -1;
        }
        return 0;
    }

    set_error(parser->state->backend, "unsupported primary expression in backend");
    return -1;
}

static int expr_parse_unary(ExprParser *parser) {
    char op[4];

    if (parser->current.kind == EXPR_TOKEN_PUNCT &&
        (names_equal(parser->current.text, "-") ||
         names_equal(parser->current.text, "!") ||
         names_equal(parser->current.text, "~") ||
         names_equal(parser->current.text, "&") ||
         names_equal(parser->current.text, "*") ||
         names_equal(parser->current.text, "+"))) {
        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);

        if (names_equal(op, "+")) {
            return expr_parse_unary(parser);
        }

        if (names_equal(op, "&")) {
            if (parser->current.kind != EXPR_TOKEN_IDENTIFIER) {
                set_error(parser->state->backend, "backend only supports address-of on identifiers");
                return -1;
            }
            {
                char line[128];
                int local_index = find_local(parser->state, parser->current.text);
                int global_index = find_global(parser->state, parser->current.text);

                if (local_index >= 0) {
                    char offset_text[32];
                    rt_unsigned_to_string((unsigned long long)parser->state->locals[local_index].offset, offset_text, sizeof(offset_text));
                    if (backend_is_aarch64(parser->state)) {
                        rt_copy_string(line, sizeof(line), "sub x0, x29, #");
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
                    } else {
                        rt_copy_string(line, sizeof(line), "leaq -");
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rbp), %rax");
                    }
                } else if (global_index >= 0) {
                    char symbol[COMPILER_IR_NAME_CAPACITY];
                    format_symbol_name(parser->state, parser->current.text, symbol, sizeof(symbol));
                    if (backend_is_aarch64(parser->state)) {
                        rt_copy_string(line, sizeof(line), "adrp x0, ");
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                                       backend_is_darwin(parser->state) ? "@PAGE" : "");
                        if (emit_instruction(parser->state, line) != 0) {
                            return -1;
                        }
                        rt_copy_string(line, sizeof(line), "add x0, x0, ");
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                                       backend_is_darwin(parser->state) ? symbol : ":lo12:");
                        if (!backend_is_darwin(parser->state)) {
                            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
                        } else {
                            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@PAGEOFF");
                        }
                    } else {
                        rt_copy_string(line, sizeof(line), "leaq ");
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), parser->current.text);
                        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip), %rax");
                    }
                } else {
                    set_error(parser->state->backend, "backend only supports address-of on known storage");
                    return -1;
                }
                expr_next(parser);
                return emit_instruction(parser->state, line);
            }
        }

        if (expr_parse_unary(parser) != 0) {
            return -1;
        }

        if (names_equal(op, "-")) {
            return emit_instruction(parser->state, backend_is_aarch64(parser->state) ? "neg x0, x0" : "negq %rax");
        }
        if (names_equal(op, "!")) {
            return emit_cmp_zero(parser->state) == 0 && emit_set_condition(parser->state, "eq") == 0 ? 0 : -1;
        }
        if (names_equal(op, "~")) {
            return emit_instruction(parser->state, backend_is_aarch64(parser->state) ? "mvn x0, x0" : "notq %rax");
        }
        if (names_equal(op, "*")) {
            return emit_instruction(parser->state, backend_is_aarch64(parser->state) ? "ldr x0, [x0]" : "movq (%rax), %rax");
        }
    }

    return expr_parse_primary(parser);
}

static int emit_binary_op(BackendState *state, const char *op) {
    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "mov x2, x0") != 0 ||
            emit_instruction(state, "ldr x1, [sp]") != 0 ||
            emit_instruction(state, "add sp, sp, #16") != 0) {
            return -1;
        }

        if (names_equal(op, "+")) return emit_instruction(state, "add x0, x1, x2");
        if (names_equal(op, "-")) return emit_instruction(state, "sub x0, x1, x2");
        if (names_equal(op, "*")) return emit_instruction(state, "mul x0, x1, x2");
        if (names_equal(op, "&")) return emit_instruction(state, "and x0, x1, x2");
        if (names_equal(op, "|")) return emit_instruction(state, "orr x0, x1, x2");
        if (names_equal(op, "^")) return emit_instruction(state, "eor x0, x1, x2");
        if (names_equal(op, "<<")) return emit_instruction(state, "lsl x0, x1, x2");
        if (names_equal(op, ">>")) return emit_instruction(state, "asr x0, x1, x2");
        if (names_equal(op, "/") || names_equal(op, "%")) {
            if (emit_instruction(state, "sdiv x3, x1, x2") != 0) {
                return -1;
            }
            if (names_equal(op, "%")) {
                return emit_instruction(state, "msub x0, x3, x2, x1");
            }
            return emit_instruction(state, "mov x0, x3");
        }

        if (emit_instruction(state, "cmp x1, x2") != 0) {
            return -1;
        }
        if (names_equal(op, "<")) return emit_set_condition(state, "lt");
        if (names_equal(op, "<=")) return emit_set_condition(state, "le");
        if (names_equal(op, ">")) return emit_set_condition(state, "gt");
        if (names_equal(op, ">=")) return emit_set_condition(state, "ge");
        if (names_equal(op, "==")) return emit_set_condition(state, "eq");
        if (names_equal(op, "!=")) return emit_set_condition(state, "ne");
    } else {
        if (emit_instruction(state, "movq %rax, %rcx") != 0 || emit_instruction(state, "popq %rax") != 0) {
            return -1;
        }

        if (names_equal(op, "+")) return emit_instruction(state, "addq %rcx, %rax");
        if (names_equal(op, "-")) return emit_instruction(state, "subq %rcx, %rax");
        if (names_equal(op, "*")) return emit_instruction(state, "imulq %rcx, %rax");
        if (names_equal(op, "&")) return emit_instruction(state, "andq %rcx, %rax");
        if (names_equal(op, "|")) return emit_instruction(state, "orq %rcx, %rax");
        if (names_equal(op, "^")) return emit_instruction(state, "xorq %rcx, %rax");
        if (names_equal(op, "<<")) return emit_instruction(state, "salq %cl, %rax");
        if (names_equal(op, ">>")) return emit_instruction(state, "sarq %cl, %rax");
        if (names_equal(op, "/") || names_equal(op, "%")) {
            if (emit_instruction(state, "movq %rax, %r11") != 0 ||
                emit_instruction(state, "movq %rcx, %r10") != 0 ||
                emit_instruction(state, "movq %r11, %rcx") != 0) {
                return -1;
            }
            if (emit_instruction(state, "cqto") != 0 || emit_instruction(state, "idivq %rcx") != 0) {
                return -1;
            }
            if (names_equal(op, "%")) {
                return emit_instruction(state, "movq %rdx, %rax");
            }
            return 0;
        }

        if (emit_instruction(state, "cmpq %rcx, %rax") != 0) {
            return -1;
        }
        if (names_equal(op, "<")) return emit_set_condition(state, "l");
        if (names_equal(op, "<=")) return emit_set_condition(state, "le");
        if (names_equal(op, ">")) return emit_set_condition(state, "g");
        if (names_equal(op, ">=")) return emit_set_condition(state, "ge");
        if (names_equal(op, "==")) return emit_set_condition(state, "e");
        if (names_equal(op, "!=")) return emit_set_condition(state, "ne");
    }

    set_error(state->backend, "unsupported binary operation in backend");
    return -1;
}

static int expr_parse_chain(ExprParser *parser, int (*subexpr)(ExprParser *), const char *const *ops, size_t op_count) {
    size_t i;

    if (subexpr(parser) != 0) {
        return -1;
    }

    for (;;) {
        int matched = 0;
        char op[4];

        if (parser->current.kind != EXPR_TOKEN_PUNCT) {
            break;
        }

        for (i = 0; i < op_count; ++i) {
            if (names_equal(parser->current.text, ops[i])) {
                matched = 1;
                break;
            }
        }
        if (!matched) {
            break;
        }

        rt_copy_string(op, sizeof(op), parser->current.text);
        expr_next(parser);
        if (emit_push_value(parser->state) != 0 || subexpr(parser) != 0 || emit_binary_op(parser->state, op) != 0) {
            return -1;
        }
    }

    return 0;
}

static int expr_parse_multiplicative(ExprParser *parser) {
    static const char *const ops[] = {"*", "/", "%"};
    return expr_parse_chain(parser, expr_parse_unary, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_additive(ExprParser *parser) {
    static const char *const ops[] = {"+", "-"};
    return expr_parse_chain(parser, expr_parse_multiplicative, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_shift(ExprParser *parser) {
    static const char *const ops[] = {"<<", ">>"};
    return expr_parse_chain(parser, expr_parse_additive, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_relational(ExprParser *parser) {
    static const char *const ops[] = {"<", ">", "<=", ">="};
    return expr_parse_chain(parser, expr_parse_shift, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_equality(ExprParser *parser) {
    static const char *const ops[] = {"==", "!="};
    return expr_parse_chain(parser, expr_parse_relational, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_bitand(ExprParser *parser) {
    static const char *const ops[] = {"&"};
    return expr_parse_chain(parser, expr_parse_equality, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_bitxor(ExprParser *parser) {
    static const char *const ops[] = {"^"};
    return expr_parse_chain(parser, expr_parse_bitand, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_parse_bitor(ExprParser *parser) {
    static const char *const ops[] = {"|"};
    return expr_parse_chain(parser, expr_parse_bitxor, ops, sizeof(ops) / sizeof(ops[0]));
}

static int expr_make_logic_label(BackendState *state, const char *prefix, char *buffer, size_t buffer_size) {
    char digits[32];
    rt_copy_string(buffer, buffer_size, prefix);
    rt_unsigned_to_string((unsigned long long)state->label_counter, digits, sizeof(digits));
    state->label_counter += 1U;
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), digits);
    return 0;
}

static int expr_parse_logical_and(ExprParser *parser) {
    if (expr_parse_bitor(parser) != 0) {
        return -1;
    }

    while (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "&&")) {
        char false_label[32];
        char end_label[32];
        char asm_label[64];

        expr_make_logic_label(parser->state, "land_false", false_label, sizeof(false_label));
        expr_make_logic_label(parser->state, "land_end", end_label, sizeof(end_label));
        expr_next(parser);
        if (emit_cmp_zero(parser->state) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b.eq" : "je");
        if (emit_jump_to_label(parser->state, asm_label, false_label) != 0 || expr_parse_bitor(parser) != 0) {
            return -1;
        }
        if (emit_cmp_zero(parser->state) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b.eq" : "je");
        if (emit_jump_to_label(parser->state, asm_label, false_label) != 0 ||
            emit_load_immediate(parser->state, 1) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b" : "jmp");
        if (emit_jump_to_label(parser->state, asm_label, end_label) != 0) {
            return -1;
        }
        write_label_name(asm_label, sizeof(asm_label), false_label);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
        if (emit_line(parser->state, asm_label) != 0 || emit_load_immediate(parser->state, 0) != 0) {
            return -1;
        }
        write_label_name(asm_label, sizeof(asm_label), end_label);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
        if (emit_line(parser->state, asm_label) != 0) {
            return -1;
        }
    }

    return 0;
}

static int expr_parse_logical_or(ExprParser *parser) {
    if (expr_parse_logical_and(parser) != 0) {
        return -1;
    }

    while (parser->current.kind == EXPR_TOKEN_PUNCT && names_equal(parser->current.text, "||")) {
        char true_label[32];
        char end_label[32];
        char asm_label[64];

        expr_make_logic_label(parser->state, "lor_true", true_label, sizeof(true_label));
        expr_make_logic_label(parser->state, "lor_end", end_label, sizeof(end_label));
        expr_next(parser);
        if (emit_cmp_zero(parser->state) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b.ne" : "jne");
        if (emit_jump_to_label(parser->state, asm_label, true_label) != 0 || expr_parse_logical_and(parser) != 0) {
            return -1;
        }
        if (emit_cmp_zero(parser->state) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b.ne" : "jne");
        if (emit_jump_to_label(parser->state, asm_label, true_label) != 0 ||
            emit_load_immediate(parser->state, 0) != 0) {
            return -1;
        }
        rt_copy_string(asm_label, sizeof(asm_label), backend_is_aarch64(parser->state) ? "b" : "jmp");
        if (emit_jump_to_label(parser->state, asm_label, end_label) != 0) {
            return -1;
        }
        write_label_name(asm_label, sizeof(asm_label), true_label);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
        if (emit_line(parser->state, asm_label) != 0 || emit_load_immediate(parser->state, 1) != 0) {
            return -1;
        }
        write_label_name(asm_label, sizeof(asm_label), end_label);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), ":");
        if (emit_line(parser->state, asm_label) != 0) {
            return -1;
        }
    }

    return 0;
}

static int expr_parse_assignment(ExprParser *parser) {
    ExprParser snapshot = *parser;
    char name[COMPILER_IR_NAME_CAPACITY];
    char op[4];

    if (snapshot.current.kind == EXPR_TOKEN_IDENTIFIER) {
        rt_copy_string(name, sizeof(name), snapshot.current.text);
        expr_next(&snapshot);
        if (snapshot.current.kind == EXPR_TOKEN_PUNCT &&
            (names_equal(snapshot.current.text, "=") ||
             names_equal(snapshot.current.text, "+=") ||
             names_equal(snapshot.current.text, "-=") ||
             names_equal(snapshot.current.text, "*=") ||
             names_equal(snapshot.current.text, "/=") ||
             names_equal(snapshot.current.text, "%="))) {
            rt_copy_string(op, sizeof(op), snapshot.current.text);
            expr_next(parser);
            expr_next(parser);

            if (!names_equal(op, "=")) {
                if (emit_load_name(parser->state, name) != 0 || emit_push_value(parser->state) != 0) {
                    return -1;
                }
            }

            if (expr_parse_assignment(parser) != 0) {
                return -1;
            }

            if (!names_equal(op, "=") && emit_binary_op(parser->state, names_equal(op, "+=") ? "+" :
                                                                names_equal(op, "-=") ? "-" :
                                                                names_equal(op, "*=") ? "*" :
                                                                names_equal(op, "/=") ? "/" : "%") != 0) {
                return -1;
            }

            return emit_store_name(parser->state, name);
        }
    }

    return expr_parse_logical_or(parser);
}

static int expr_parse_expression(ExprParser *parser) {
    if (expr_parse_assignment(parser) != 0) {
        return -1;
    }

    while (expr_match_punct(parser, ",")) {
        if (expr_parse_assignment(parser) != 0) {
            return -1;
        }
    }

    return 0;
}

static int emit_expression(BackendState *state, const char *expr) {
    ExprParser parser;

    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);
    if (expr_parse_expression(&parser) != 0) {
        return -1;
    }
    return 0;
}

static int parse_decl_line(const char *line, char *storage, size_t storage_size, char *kind, size_t kind_size, char *name, size_t name_size) {
    const char *cursor = line + 5;
    size_t length = 0;

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

        if (!in_function && starts_with(line, "decl global ")) {
            char storage[16];
            char kind[16];
            char name[COMPILER_IR_NAME_CAPACITY];

            parse_decl_line(line, storage, sizeof(storage), kind, sizeof(kind), name, sizeof(name));
            if (names_equal(kind, "obj") && !is_function_name(state, name) && add_global(state, name) < 0) {
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
                if (parse_signed_value(expr, &value) == 0) {
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

static int begin_function(BackendState *state, const char *name) {
    char symbol[COMPILER_IR_NAME_CAPACITY];
    state->in_function = 1;
    state->local_count = 0;
    state->param_count = 0;
    state->saw_return_in_function = 0;
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
    char name[COMPILER_IR_NAME_CAPACITY];

    parse_decl_line(line, storage, sizeof(storage), kind, sizeof(kind), name, sizeof(name));

    if (names_equal(storage, "param")) {
        static const char *const x86_arg_regs[] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
        static const char *const aarch64_arg_regs[] = {"x0", "x1", "x2", "x3", "x4", "x5"};
        char asm_line[128];
        int index = state->param_count;

        if (allocate_local(state, name) != 0) {
            return -1;
        }
        if (index < 6) {
            int local_index = find_local(state, name);
            char offset_text[32];
            rt_unsigned_to_string((unsigned long long)state->locals[local_index].offset, offset_text, sizeof(offset_text));
            if (backend_is_aarch64(state)) {
                rt_copy_string(asm_line, sizeof(asm_line), "stur ");
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), aarch64_arg_regs[index]);
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), ", [x29, #-");
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), offset_text);
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), "]");
            } else {
                rt_copy_string(asm_line, sizeof(asm_line), "movq ");
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), x86_arg_regs[index]);
                rt_copy_string(asm_line + rt_strlen(asm_line), sizeof(asm_line) - rt_strlen(asm_line), ", -");
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
        return allocate_local(state, name);
    }

    (void)kind;
    return 0;
}

void compiler_backend_init(CompilerBackend *backend, CompilerBackendTarget target) {
    rt_memset(backend, 0, sizeof(*backend));
    backend->target = target;
}

int compiler_backend_emit_assembly(CompilerBackend *backend, const CompilerIr *ir, int fd) {
    BackendState state;
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

            if (emit_expression(&state, expr) != 0 || emit_store_name(&state, name) != 0) {
                return -1;
            }
            continue;
        }

        if (starts_with(line, "eval ")) {
            if (emit_expression(&state, line + 5) != 0) {
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

            while (*arrow != '\0' && !(arrow[0] == ' ' && arrow[1] == '-' && arrow[2] == '>') && out + 1 < sizeof(expr)) {
                expr[out++] = *arrow++;
            }
            expr[out] = '\0';
            rt_copy_string(label, sizeof(label), skip_spaces(arrow + 4));
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

    return 0;
}

const char *compiler_backend_error_message(const CompilerBackend *backend) {
    return backend->error_message;
}
