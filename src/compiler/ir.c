#include "ir.h"

#include "runtime.h"

static void set_error(CompilerIr *ir, const char *message) {
    rt_copy_string(ir->error_message, sizeof(ir->error_message), message != 0 ? message : "IR error");
}

static int append_text(char *buffer, size_t buffer_size, size_t *offset, const char *text) {
    size_t i = 0;

    while (text[i] != '\0') {
        if (*offset + 1 >= buffer_size) {
            return -1;
        }
        buffer[*offset] = text[i];
        *offset += 1U;
        i += 1U;
    }

    buffer[*offset] = '\0';
    return 0;
}

static int append_uint(char *buffer, size_t buffer_size, size_t *offset, unsigned long long value) {
    char scratch[32];
    rt_unsigned_to_string(value, scratch, sizeof(scratch));
    return append_text(buffer, buffer_size, offset, scratch);
}

static int emit_line(CompilerIr *ir, const char *lhs, const char *mid, const char *rhs, const char *tail) {
    size_t offset = 0;
    char *line;

    if (ir->count >= COMPILER_MAX_IR_LINES) {
        set_error(ir, "IR instruction capacity exceeded");
        return -1;
    }

    line = ir->lines[ir->count];
    line[0] = '\0';

    if ((lhs != 0 && append_text(line, COMPILER_IR_LINE_CAPACITY, &offset, lhs) != 0) ||
        (mid != 0 && append_text(line, COMPILER_IR_LINE_CAPACITY, &offset, mid) != 0) ||
        (rhs != 0 && append_text(line, COMPILER_IR_LINE_CAPACITY, &offset, rhs) != 0) ||
        (tail != 0 && append_text(line, COMPILER_IR_LINE_CAPACITY, &offset, tail) != 0)) {
        set_error(ir, "IR instruction text exceeded line capacity");
        return -1;
    }

    ir->count += 1U;
    return 0;
}

static void format_type(const CompilerType *type, char *buffer, size_t buffer_size) {
    size_t i;
    const char *base = "int";

    if (type->base == COMPILER_BASE_VOID) {
        base = "void";
    } else if (type->base == COMPILER_BASE_CHAR) {
        base = "char";
    } else if (type->base == COMPILER_BASE_STRUCT) {
        base = "struct";
    } else if (type->base == COMPILER_BASE_UNION) {
        base = "union";
    } else if (type->base == COMPILER_BASE_ENUM) {
        base = "enum";
    }

    buffer[0] = '\0';
    if (type->is_unsigned) {
        rt_copy_string(buffer, buffer_size, "unsigned ");
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), base);
    } else {
        rt_copy_string(buffer, buffer_size, base);
    }

    if (type->is_array && rt_strlen(buffer) + 2U < buffer_size) {
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "[]");
    }

    for (i = 0; i < (size_t)type->pointer_depth && rt_strlen(buffer) + 2U < buffer_size; ++i) {
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "*");
    }
}

void compiler_ir_init(CompilerIr *ir) {
    rt_memset(ir, 0, sizeof(*ir));
}

int compiler_ir_make_label(CompilerIr *ir, const char *prefix, char *buffer, size_t buffer_size) {
    size_t offset = 0;

    if (append_text(buffer, buffer_size, &offset, prefix != 0 ? prefix : "L") != 0 ||
        append_uint(buffer, buffer_size, &offset, (unsigned long long)ir->label_counter) != 0) {
        set_error(ir, "failed to format IR label");
        return -1;
    }

    ir->label_counter += 1U;
    return 0;
}

int compiler_ir_emit_function_begin(CompilerIr *ir, const char *name, const CompilerType *type) {
    char type_text[64];
    format_type(type, type_text, sizeof(type_text));
    return emit_line(ir, "func ", name, " : ", type_text);
}

int compiler_ir_emit_function_end(CompilerIr *ir, const char *name) {
    return emit_line(ir, "endfunc ", name, 0, 0);
}

int compiler_ir_emit_decl(CompilerIr *ir, const char *storage, int is_function, const CompilerType *type, const char *name) {
    char prefix[128];
    char type_text[64];
    size_t offset = 0;

    format_type(type, type_text, sizeof(type_text));
    if (append_text(prefix, sizeof(prefix), &offset, "decl ") != 0 ||
        append_text(prefix, sizeof(prefix), &offset, storage) != 0 ||
        append_text(prefix, sizeof(prefix), &offset, " ") != 0 ||
        append_text(prefix, sizeof(prefix), &offset, is_function ? "func" : "obj") != 0 ||
        append_text(prefix, sizeof(prefix), &offset, " ") != 0 ||
        append_text(prefix, sizeof(prefix), &offset, type_text) != 0 ||
        append_text(prefix, sizeof(prefix), &offset, " ") != 0) {
        set_error(ir, "failed to format declaration IR");
        return -1;
    }

    return emit_line(ir, prefix, name, 0, 0);
}

int compiler_ir_emit_assign(CompilerIr *ir, const char *name, const char *expr) {
    return emit_line(ir, "store ", name, " <- ", expr);
}

int compiler_ir_emit_eval(CompilerIr *ir, const char *expr) {
    return emit_line(ir, "eval ", expr, 0, 0);
}

int compiler_ir_emit_return(CompilerIr *ir, const char *expr) {
    if (expr == 0 || expr[0] == '\0') {
        return emit_line(ir, "ret", 0, 0, 0);
    }
    return emit_line(ir, "ret ", expr, 0, 0);
}

int compiler_ir_emit_branch_zero(CompilerIr *ir, const char *expr, const char *label) {
    return emit_line(ir, "brfalse ", expr, " -> ", label);
}

int compiler_ir_emit_jump(CompilerIr *ir, const char *label) {
    return emit_line(ir, "jump ", label, 0, 0);
}

int compiler_ir_emit_label(CompilerIr *ir, const char *label) {
    return emit_line(ir, "label ", label, 0, 0);
}

int compiler_ir_emit_case(CompilerIr *ir, const char *expr) {
    return emit_line(ir, "case ", expr, 0, 0);
}

int compiler_ir_emit_default(CompilerIr *ir) {
    return emit_line(ir, "default", 0, 0, 0);
}

int compiler_ir_emit_note(CompilerIr *ir, const char *keyword, const char *detail) {
    if (detail == 0 || detail[0] == '\0') {
        return emit_line(ir, keyword, 0, 0, 0);
    }
    return emit_line(ir, keyword, " ", detail, 0);
}

int compiler_ir_write_dump(const CompilerIr *ir, int fd) {
    size_t i;

    for (i = 0; i < ir->count; ++i) {
        if (rt_write_line(fd, ir->lines[i]) != 0) {
            return -1;
        }
    }

    return 0;
}

const char *compiler_ir_error_message(const CompilerIr *ir) {
    return ir->error_message;
}
