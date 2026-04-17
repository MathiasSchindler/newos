#include "backend.h"

#include "runtime.h"

#define COMPILER_BACKEND_MAX_FUNCTIONS 256
#define COMPILER_BACKEND_MAX_GLOBALS 256
#define COMPILER_BACKEND_MAX_LOCALS 256
#define COMPILER_BACKEND_MAX_STRINGS 1024
#define COMPILER_BACKEND_MAX_CONSTANTS 512
#define BACKEND_ARRAY_STACK_BYTES 4096

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
} BackendFunctionName;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    long long init_value;
    int initialized;
    int is_array;
    int prefers_word_index;
} BackendGlobal;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    int offset;
    int stack_bytes;
    int is_array;
    int prefers_word_index;
} BackendLocal;

typedef struct {
    char label[32];
    char text[COMPILER_IR_LINE_CAPACITY];
} BackendStringLiteral;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    long long value;
} BackendConstant;

typedef struct {
    CompilerBackend *backend;
    int fd;
    BackendFunctionName functions[COMPILER_BACKEND_MAX_FUNCTIONS];
    size_t function_count;
    BackendGlobal globals[COMPILER_BACKEND_MAX_GLOBALS];
    size_t global_count;
    BackendStringLiteral strings[COMPILER_BACKEND_MAX_STRINGS];
    size_t string_count;
    BackendConstant constants[COMPILER_BACKEND_MAX_CONSTANTS];
    size_t constant_count;
    BackendLocal locals[COMPILER_BACKEND_MAX_LOCALS];
    size_t local_count;
    char current_function[COMPILER_IR_NAME_CAPACITY];
    int in_function;
    int param_count;
    int saw_return_in_function;
    int stack_size;
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
static int expr_parse_assignment(ExprParser *parser);
static int expr_parse_lvalue_address(ExprParser *parser, int *byte_sized);
static int expr_parse_unary(ExprParser *parser);
static int expr_parse_multiplicative(ExprParser *parser);
static int expr_parse_additive(ExprParser *parser);
static int expr_parse_shift(ExprParser *parser);
static int expr_parse_relational(ExprParser *parser);
static int expr_parse_equality(ExprParser *parser);
static int expr_parse_bitand(ExprParser *parser);
static int expr_parse_bitxor(ExprParser *parser);
static int expr_parse_bitor(ExprParser *parser);
static int emit_binary_op(BackendState *state, const char *op);
static int emit_push_value(BackendState *state);
static int emit_load_immediate(BackendState *state, long long value);
static int emit_binary_op(BackendState *state, const char *op);
static int expr_read_punctuator_width(const char *cursor);
static int is_assignment_operator_text(const char *text);
static int is_assignment_stop_text(const char *text);
static const char *binary_op_for_assignment(const char *op);

static void set_error(CompilerBackend *backend, const char *message) {
    rt_copy_string(backend->error_message, sizeof(backend->error_message), message != 0 ? message : "backend error");
}

static void set_error_with_line(CompilerBackend *backend, const char *message, const char *line) {
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
    set_error(backend, buffer);
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

static int text_contains(const char *text, const char *needle) {
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

static int should_prefer_word_index(const char *name, const char *type_text) {
    if (names_equal(name, "argv") || names_equal(name, "envp")) {
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

static int find_constant(const BackendState *state, const char *name) {
    size_t i;
    for (i = 0; i < state->constant_count; ++i) {
        if (names_equal(state->constants[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

static int add_constant(BackendState *state, const char *name, long long value) {
    int existing = find_constant(state, name);

    if (existing >= 0) {
        state->constants[existing].value = value;
        return 0;
    }

    if (state->constant_count >= COMPILER_BACKEND_MAX_CONSTANTS) {
        set_error(state->backend, "too many constants for backend");
        return -1;
    }

    rt_copy_string(state->constants[state->constant_count].name, sizeof(state->constants[state->constant_count].name), name);
    state->constants[state->constant_count].value = value;
    state->constant_count += 1U;
    return 0;
}

static int add_global(BackendState *state, const char *name, int is_array, int prefers_word_index) {
    int existing = find_global(state, name);

    if (existing >= 0) {
        if (is_array) {
            state->globals[existing].is_array = 1;
        }
        if (prefers_word_index) {
            state->globals[existing].prefers_word_index = 1;
        }
        return existing;
    }

    if (state->global_count >= COMPILER_BACKEND_MAX_GLOBALS) {
        set_error(state->backend, "too many globals for backend");
        return -1;
    }

    rt_copy_string(state->globals[state->global_count].name, sizeof(state->globals[state->global_count].name), name);
    state->globals[state->global_count].init_value = 0;
    state->globals[state->global_count].initialized = 0;
    state->globals[state->global_count].is_array = is_array;
    state->globals[state->global_count].prefers_word_index = prefers_word_index;
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

static int emit_load_immediate_register(BackendState *state, const char *reg, long long value);
static int emit_binary_op(BackendState *state, const char *op);

static int allocate_local(BackendState *state, const char *name, int is_array, int prefers_word_index) {
    char line[96];
    char offset_text[32];
    int existing = find_local(state, name);
    int slot_size = is_array ? BACKEND_ARRAY_STACK_BYTES : (backend_is_aarch64(state) ? 16 : 8);

    if (existing >= 0) {
        if (is_array) {
            state->locals[existing].is_array = 1;
        }
        if (prefers_word_index) {
            state->locals[existing].prefers_word_index = 1;
        }
        return 0;
    }

    if (state->local_count >= COMPILER_BACKEND_MAX_LOCALS) {
        set_error(state->backend, "too many local variables for backend");
        return -1;
    }

    rt_copy_string(state->locals[state->local_count].name, sizeof(state->locals[state->local_count].name), name);
    state->locals[state->local_count].stack_bytes = slot_size;
    state->locals[state->local_count].offset = state->stack_size + slot_size;
    state->locals[state->local_count].is_array = is_array;
    state->locals[state->local_count].prefers_word_index = prefers_word_index;
    state->stack_size += slot_size;
    state->local_count += 1U;

    rt_unsigned_to_string((unsigned long long)slot_size, offset_text, sizeof(offset_text));
    if (backend_is_aarch64(state)) {
        if (slot_size <= 4095) {
            rt_copy_string(line, sizeof(line), "sub sp, sp, #");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
            return emit_instruction(state, line);
        }
        if (emit_load_immediate_register(state, "x9", slot_size) != 0) {
            return -1;
        }
        return emit_instruction(state, "sub sp, sp, x9");
    } else {
        rt_copy_string(line, sizeof(line), "subq $");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rsp");
        return emit_instruction(state, line);
    }
}

static int write_label_name(char *buffer, size_t buffer_size, const char *label) {
    rt_copy_string(buffer, buffer_size, ".L");
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), label);
    return 0;
}

static int emit_pop_to_register(BackendState *state, const char *reg) {
    char line[64];
    if (backend_is_aarch64(state)) {
        rt_copy_string(line, sizeof(line), "ldr ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", [sp]");
        return emit_instruction(state, line) == 0 &&
               emit_instruction(state, "add sp, sp, #16") == 0 ? 0 : -1;
    }
    if (names_equal(reg, "%rax")) {
        return emit_instruction(state, "popq %rax");
    }
    rt_copy_string(line, sizeof(line), "movq %rax, ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
    return emit_instruction(state, "popq %rax") == 0 && emit_instruction(state, line) == 0 ? 0 : -1;
}

static int emit_local_address(BackendState *state, int offset, const char *reg) {
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

static int emit_load_from_address_register(BackendState *state, const char *reg, int byte_value) {
    char line[64];
    if (backend_is_aarch64(state)) {
        rt_copy_string(line, sizeof(line), byte_value ? "ldrb w0, [" : "ldr x0, [");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
        return emit_instruction(state, line);
    }

    rt_copy_string(line, sizeof(line), byte_value ? "movzbq (" : "movq (");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "), %rax");
    return emit_instruction(state, line);
}

static int emit_store_to_address_register(BackendState *state, const char *reg, int byte_value) {
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

static int emit_pop_address_and_store(BackendState *state, int byte_value) {
    return emit_pop_to_register(state, backend_is_aarch64(state) ? "x1" : "%rcx") == 0 &&
           emit_store_to_address_register(state, backend_is_aarch64(state) ? "x1" : "%rcx", byte_value) == 0 ? 0 : -1;
}

static int find_string_literal(const BackendState *state, const char *text) {
    size_t i;
    for (i = 0; i < state->string_count; ++i) {
        if (names_equal(state->strings[i].text, text)) {
            return (int)i;
        }
    }
    return -1;
}

static int add_string_literal(BackendState *state, const char *text) {
    char digits[32];
    int existing = find_string_literal(state, text);

    if (existing >= 0) {
        return existing;
    }
    if (state->string_count >= COMPILER_BACKEND_MAX_STRINGS) {
        set_error(state->backend, "too many string literals for backend");
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

static int emit_address_of_name(BackendState *state, const char *name) {
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

    set_error(state->backend, "backend only supports address-of on known storage");
    return -1;
}

static int emit_load_string_literal(BackendState *state, const char *text) {
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

static int emit_load_name(BackendState *state, const char *name) {
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

    if (names_equal(name, "NULL")) {
        return backend_is_aarch64(state) ? emit_instruction(state, "mov x0, #0") :
                                           emit_instruction(state, "movq $0, %rax");
    }

    set_error(state->backend, "unsupported value reference in backend");
    return -1;
}

static int emit_store_name(BackendState *state, const char *name) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index >= 0) {
        if (state->locals[local_index].is_array) {
            set_error(state->backend, "unsupported local array assignment in backend");
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

    set_error(state->backend, "unknown assignment target in backend");
    return -1;
}

static int lookup_array_storage(const BackendState *state, const char *name, int *word_index_out) {
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

static int emit_load_immediate_register(BackendState *state, const char *reg, long long value) {
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

static int emit_load_immediate(BackendState *state, long long value) {
    return emit_load_immediate_register(state, backend_is_aarch64(state) ? "x0" : "%rax", value);
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


#include "backend_expressions.inc"

#include "backend_codegen.inc"
