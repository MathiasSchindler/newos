#include "ir_internal.h"

int ir_text_equals(const char *lhs, const char *rhs);
int ir_starts_with(const char *text, const char *prefix);
const char *ir_skip_spaces(const char *text);
int ir_is_identifier_char(char ch);
static int ir_operator_is_embedded(const char *expr, size_t index, const char *op);
static int ir_expr_has_side_effect_risk(const char *expr);
static void ir_format_signed_value(long long value, char *buffer, size_t buffer_size);
static int ir_copy_identifier(char *buffer, size_t buffer_size, const char *start, const char *end);
static void ir_clear_local_values(IrOptimizerState *state);
static void ir_invalidate_local_value(IrOptimizerState *state, const char *name);
static int ir_set_tracked_value(IrTrackedValue *values, size_t *count, const char *name, long long value);
static int ir_starts_block_boundary(const char *line);
static int ir_is_terminator_line(const char *line);
static int ir_label_is_referenced(const CompilerIr *ir, const char *label, size_t skip_index);
static const char *ir_find_separator_outside_quotes(const char *text, const char *separator);
static const char *ir_trim_trailing_spaces(const char *start, const char *end);
static int ir_strip_outer_parens(const char *expr, char *buffer, size_t buffer_size);
static int ir_find_top_level_operator(const char *expr, const char *op, const char **position_out);
static int ir_operator_precedence(const char *op);
static int ir_operator_has_right_precedence_boundary(const char *expr, const char *position, const char *op);
static int ir_find_top_level_conditional(const char *expr, const char **question_out, const char **colon_out);
static int ir_try_simplify_conditional_expr(const char *expr, char *buffer, size_t buffer_size);
static int ir_try_simplify_identity_expr(const char *expr, const IrOptimizerState *state, char *buffer, size_t buffer_size);
static int ir_has_many_logical_operators(const char *expr);
static int ir_normalize_expr_text(const char *expr, char *buffer, size_t buffer_size);
static int ir_expr_is_trivial_value(const char *expr);
static int ir_expr_text_equals(const char *lhs, const char *rhs);
static int ir_assignment_is_noop(const char *name, const char *expr);
static int ir_optimize_expr_text(const char *expr,
                                 const IrOptimizerState *state,
                                 char *buffer,
                                 size_t buffer_size,
                                 int *changed_out,
                                 int *constant_out,
                                 long long *value_out);
static int ir_assignment_op_is_noop(const char *op, const char *rhs, const IrOptimizerState *state);
static int ir_rewrite_expr_line(CompilerIr *ir, size_t index, const char *prefix, const char *expr);
static int ir_rewrite_branch_line(CompilerIr *ir, size_t index, const char *expr, const char *label);
static int ir_rewrite_jump_line(CompilerIr *ir, size_t index, const char *label);
static void ir_remove_line(CompilerIr *ir, size_t index);
static void ir_remove_unreachable_after_terminator(CompilerIr *ir, size_t index);
static int ir_extract_store_parts(const char *line, char *name, size_t name_size, const char **expr_out);
static int ir_extract_assignment_parts(const char *expr, char *name, size_t name_size, char *op, size_t op_size, const char **rhs_out);
static int ir_extract_branch_parts(const char *line, char *label, size_t label_size, char *expr, size_t expr_size);
static int ir_extract_jump_target(const char *line, char *label, size_t label_size);
static int ir_extract_const_parts(const char *line, char *name, size_t name_size, const char **expr_out);
static int ir_reserve_lines(CompilerIr *ir, size_t needed);
static char *ir_build_line(CompilerIr *ir, const char *lhs, const char *mid, const char *rhs, const char *tail);
static int ir_replace_line(CompilerIr *ir, size_t index, const char *line);

static void set_error(CompilerIr *ir, const char *message) {
    rt_copy_string(ir->error_message, sizeof(ir->error_message), message != 0 ? message : "IR error");
}

int ir_text_equals(const char *lhs, const char *rhs) {
    size_t i = 0;

    while (lhs[i] != '\0' && rhs[i] != '\0') {
        if (lhs[i] != rhs[i]) {
            return 0;
        }
        i += 1U;
    }

    return lhs[i] == rhs[i];
}

int ir_starts_with(const char *text, const char *prefix) {
    size_t i = 0;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }

    return 1;
}

int ir_is_identifier_char(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_';
}

const char *ir_skip_spaces(const char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') {
        text += 1;
    }
    return text;
}

static int ir_operator_is_embedded(const char *expr, size_t index, const char *op) {
    size_t op_length = rt_strlen(op);
    char prev = index > 0U ? expr[index - 1U] : '\0';
    char next = expr[index + op_length];

    if (op_length == 1U) {
        if ((op[0] == '|' && (prev == '|' || next == '|' || next == '=')) ||
            (op[0] == '&' && (prev == '&' || next == '&' || next == '=')) ||
            ((op[0] == '+' || op[0] == '-') && (prev == op[0] || next == op[0] || next == '=')) ||
            (op[0] == '-' && next == '>') ||
            (op[0] == '<' && (prev == '<' || next == '<' || next == '=')) ||
            (op[0] == '>' && (prev == '>' || prev == '-' || next == '>' || next == '=')) ||
            ((op[0] == '*' || op[0] == '/' || op[0] == '%' || op[0] == '^') && next == '=')) {
            return 1;
        }
    }

    if ((ir_text_equals(op, "<<") || ir_text_equals(op, ">>")) && next == '=') {
        return 1;
    }

    if ((ir_text_equals(op, "<=") && prev == '<') ||
        (ir_text_equals(op, ">=") && prev == '>')) {
        return 1;
    }

    return 0;
}

static int ir_expr_has_side_effect_risk(const char *expr) {
    size_t i = 0;
    int in_string = 0;
    int in_char = 0;

    while (expr[i] != '\0') {
        if ((in_string || in_char) && expr[i] == '\\' && expr[i + 1U] != '\0') {
            i += 2U;
            continue;
        }
        if (!in_char && expr[i] == '"') {
            in_string = !in_string;
            i += 1U;
            continue;
        }
        if (!in_string && expr[i] == '\'') {
            in_char = !in_char;
            i += 1U;
            continue;
        }
        if (in_string || in_char) {
            i += 1U;
            continue;
        }

        if (expr[i] == '&' && expr[i + 1U] != '&' && expr[i + 1U] != '=') {
            return 1;
        }
        if (expr[i] == '(') {
            size_t k = i;
            while (k > 0U && (expr[k - 1U] == ' ' || expr[k - 1U] == '\t')) {
                k -= 1U;
            }
            if (k > 0U &&
                (ir_is_identifier_char(expr[k - 1U]) || expr[k - 1U] == ')' || expr[k - 1U] == ']' ||
                 expr[k - 1U] == '*')) {
                return 1;
            }
        }
        i += 1U;
    }

    return 0;
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

static int ir_reserve_lines(CompilerIr *ir, size_t needed) {
    size_t new_capacity;
    char **new_lines;

    if (needed <= ir->capacity) {
        return 0;
    }
    new_capacity = ir->capacity > 0U ? ir->capacity : COMPILER_IR_INITIAL_LINE_CAPACITY;
    while (new_capacity < needed) {
        if (new_capacity > ((size_t)-1) / 2U) {
            set_error(ir, "IR instruction capacity exceeded");
            return -1;
        }
        new_capacity *= 2U;
    }
    new_lines = (char **)rt_realloc_array(ir->lines, new_capacity, sizeof(ir->lines[0]));
    if (new_lines == 0) {
        set_error(ir, "out of memory while growing IR line table");
        return -1;
    }
    ir->lines = new_lines;
    ir->capacity = new_capacity;
    return 0;
}

static char *ir_build_line(CompilerIr *ir, const char *lhs, const char *mid, const char *rhs, const char *tail) {
    size_t length = 0;
    size_t offset = 0;
    char *line;

    if (lhs != 0) length += rt_strlen(lhs);
    if (mid != 0) length += rt_strlen(mid);
    if (rhs != 0) length += rt_strlen(rhs);
    if (tail != 0) length += rt_strlen(tail);
    if (length == (size_t)-1) {
        set_error(ir, "IR instruction text exceeded addressable size");
        return 0;
    }
    line = (char *)rt_malloc(length + 1U);
    if (line == 0) {
        set_error(ir, "out of memory while storing IR instruction");
        return 0;
    }
    line[0] = '\0';
    if ((lhs != 0 && append_text(line, length + 1U, &offset, lhs) != 0) ||
        (mid != 0 && append_text(line, length + 1U, &offset, mid) != 0) ||
        (rhs != 0 && append_text(line, length + 1U, &offset, rhs) != 0) ||
        (tail != 0 && append_text(line, length + 1U, &offset, tail) != 0)) {
        rt_free(line);
        set_error(ir, "IR instruction text exceeded line capacity");
        return 0;
    }
    return line;
}

static int ir_replace_line(CompilerIr *ir, size_t index, const char *line) {
    char *copy;

    if (index >= ir->count) {
        set_error(ir, "IR rewrite index out of range");
        return -1;
    }
    copy = ir_build_line(ir, line, 0, 0, 0);
    if (copy == 0) {
        return -1;
    }
    rt_free(ir->lines[index]);
    ir->lines[index] = copy;
    return 0;
}

static int emit_line(CompilerIr *ir, const char *lhs, const char *mid, const char *rhs, const char *tail) {
    char *line;

    if (ir_reserve_lines(ir, ir->count + 1U) != 0) {
        return -1;
    }

    line = ir_build_line(ir, lhs, mid, rhs, tail);
    if (line == 0) {
        return -1;
    }

    ir->lines[ir->count] = line;
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
    } else if (type->base == COMPILER_BASE_INT) {
        if (type->scalar_bytes >= 16U) {
            base = "__int128";
        } else if (type->scalar_bytes >= 8U) {
            base = "long";
        } else if (type->scalar_bytes == 2U) {
            base = "short";
        }
    }

    buffer[0] = '\0';
    if (type->is_unsigned) {
        rt_copy_string(buffer, buffer_size, "unsigned ");
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), base);
    } else {
        rt_copy_string(buffer, buffer_size, base);
    }

    if ((type->base == COMPILER_BASE_STRUCT || type->base == COMPILER_BASE_UNION || type->base == COMPILER_BASE_ENUM) &&
        type->aggregate_name[0] != '\0' &&
        rt_strlen(buffer) + 1U + rt_strlen(type->aggregate_name) < buffer_size) {
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), ":");
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), type->aggregate_name);
    }

    if (type->is_array && type->pointer_to_array && rt_strlen(buffer) + 3U < buffer_size) {
        char digits[32];
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "[");
        if (type->array_length > 0ULL) {
            rt_unsigned_to_string(type->array_length, digits, sizeof(digits));
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), digits);
        }
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "]");
        if (type->array_inner_length > 0ULL && type->array_stride > type->array_inner_length &&
            type->array_stride % type->array_inner_length == 0ULL) {
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "[");
            rt_unsigned_to_string(type->array_inner_length, digits, sizeof(digits));
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), digits);
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "]");
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "[");
            rt_unsigned_to_string(type->array_stride / type->array_inner_length, digits, sizeof(digits));
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), digits);
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "]");
        } else if (type->array_stride > 0ULL) {
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "[");
            rt_unsigned_to_string(type->array_stride, digits, sizeof(digits));
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), digits);
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "]");
        }
    }

    for (i = 0; i < (size_t)type->pointer_depth && rt_strlen(buffer) + 2U < buffer_size; ++i) {
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "*");
    }

    if (type->is_array && !type->pointer_to_array && rt_strlen(buffer) + 3U < buffer_size) {
        char digits[32];
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "[");
        if (type->array_length > 0ULL) {
            rt_unsigned_to_string(type->array_length, digits, sizeof(digits));
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), digits);
        }
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "]");
        if (type->array_inner_length > 0ULL && type->array_stride > type->array_inner_length &&
            type->array_stride % type->array_inner_length == 0ULL) {
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "[");
            rt_unsigned_to_string(type->array_inner_length, digits, sizeof(digits));
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), digits);
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "]");
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "[");
            rt_unsigned_to_string(type->array_stride / type->array_inner_length, digits, sizeof(digits));
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), digits);
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "]");
        } else if (type->array_stride > 0ULL) {
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "[");
            rt_unsigned_to_string(type->array_stride, digits, sizeof(digits));
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), digits);
            rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "]");
        }
    }
}

void compiler_ir_init(CompilerIr *ir) {
    rt_memset(ir, 0, sizeof(*ir));
}

void compiler_ir_destroy(CompilerIr *ir) {
    size_t i;

    if (ir == 0) {
        return;
    }
    for (i = 0; i < ir->count; ++i) {
        rt_free(ir->lines[i]);
    }
    rt_free(ir->lines);
    rt_memset(ir, 0, sizeof(*ir));
}

static void ir_format_signed_value(long long value, char *buffer, size_t buffer_size) {
    if (value < 0) {
        buffer[0] = '-';
        rt_unsigned_to_string((unsigned long long)(-value), buffer + 1, buffer_size > 0 ? buffer_size - 1U : 0U);
    } else {
        rt_unsigned_to_string((unsigned long long)value, buffer, buffer_size);
    }
}

static int ir_copy_identifier(char *buffer, size_t buffer_size, const char *start, const char *end) {
    size_t length = 0;

    while (start < end && length + 1U < buffer_size) {
        buffer[length++] = *start++;
    }
    buffer[length] = '\0';
    return (start == end && length > 0U) ? 0 : -1;
}

static int ir_starts_block_boundary(const char *line) {
    return ir_starts_with(line, "func ") ||
           ir_starts_with(line, "endfunc ") ||
           ir_starts_with(line, "label ") ||
           ir_starts_with(line, "switch ") ||
           ir_starts_with(line, "case ") ||
           ir_text_equals(line, "default") ||
           ir_starts_with(line, "endswitch");
}

static int ir_is_terminator_line(const char *line) {
    return ir_text_equals(line, "ret") ||
           ir_starts_with(line, "ret ") ||
           ir_starts_with(line, "jump ");
}

static int ir_label_is_referenced(const CompilerIr *ir, const char *label, size_t skip_index) {
    size_t i;
    char line_buffer[COMPILER_IR_LINE_CAPACITY];

    rt_copy_string(line_buffer, sizeof(line_buffer), "jump ");
    rt_copy_string(line_buffer + rt_strlen(line_buffer), sizeof(line_buffer) - rt_strlen(line_buffer), label);
    for (i = 0; i < ir->count; ++i) {
        if (i == skip_index) {
            continue;
        }
        if (ir_text_equals(ir->lines[i], line_buffer)) {
            return 1;
        }
        if (ir_starts_with(ir->lines[i], "brfalse ")) {
            char branch_label[COMPILER_IR_NAME_CAPACITY];
            char expr[COMPILER_IR_LINE_CAPACITY];
            if (ir_extract_branch_parts(ir->lines[i], branch_label, sizeof(branch_label), expr, sizeof(expr)) == 0 &&
                ir_text_equals(branch_label, label)) {
                return 1;
            }
        }
    }
    return 0;
}

static const char *ir_trim_trailing_spaces(const char *start, const char *end) {
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) {
        end -= 1;
    }
    return end;
}

static int ir_strip_outer_parens(const char *expr, char *buffer, size_t buffer_size) {
    const char *start = ir_skip_spaces(expr);
    const char *end = ir_trim_trailing_spaces(start, start + rt_strlen(start));
    const char *cursor;
    int depth = 0;
    int in_string = 0;
    int in_char = 0;

    if (end <= start + 1 || *start != '(' || end[-1] != ')') {
        return 0;
    }

    for (cursor = start; cursor < end; ++cursor) {
        if ((in_string || in_char) && *cursor == '\\' && cursor + 1 < end) {
            cursor += 1;
            continue;
        }
        if (!in_char && *cursor == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string && *cursor == '\'') {
            in_char = !in_char;
            continue;
        }
        if (in_string || in_char) {
            continue;
        }
        if (*cursor == '(') {
            depth += 1;
        } else if (*cursor == ')') {
            depth -= 1;
            if (depth == 0 && cursor + 1 < end) {
                return 0;
            }
        }
    }

    if (depth != 0) {
        return 0;
    }

    start = ir_skip_spaces(start + 1);
    end = ir_trim_trailing_spaces(start, end - 1);
    return ir_copy_identifier(buffer, buffer_size, start, end) == 0 ? 1 : -1;
}

static int ir_find_top_level_operator(const char *expr, const char *op, const char **position_out) {
    const char *last = 0;
    size_t i = 0;
    size_t op_length = rt_strlen(op);
    int paren_depth = 0;
    int bracket_depth = 0;
    int brace_depth = 0;
    int in_string = 0;
    int in_char = 0;

    while (expr[i] != '\0') {
        if ((in_string || in_char) && expr[i] == '\\' && expr[i + 1] != '\0') {
            i += 2U;
            continue;
        }
        if (!in_char && expr[i] == '"') {
            in_string = !in_string;
            i += 1U;
            continue;
        }
        if (!in_string && expr[i] == '\'') {
            in_char = !in_char;
            i += 1U;
            continue;
        }
        if (in_string || in_char) {
            i += 1U;
            continue;
        }

        if (expr[i] == '(') {
            paren_depth += 1;
            i += 1U;
            continue;
        }
        if (expr[i] == ')') {
            if (paren_depth > 0) {
                paren_depth -= 1;
            }
            i += 1U;
            continue;
        }
        if (expr[i] == '[') {
            bracket_depth += 1;
            i += 1U;
            continue;
        }
        if (expr[i] == ']') {
            if (bracket_depth > 0) {
                bracket_depth -= 1;
            }
            i += 1U;
            continue;
        }
        if (expr[i] == '{') {
            brace_depth += 1;
            i += 1U;
            continue;
        }
        if (expr[i] == '}') {
            if (brace_depth > 0) {
                brace_depth -= 1;
            }
            i += 1U;
            continue;
        }

        if (paren_depth == 0 && bracket_depth == 0 && brace_depth == 0) {
            size_t j = 0;
            while (j < op_length && expr[i + j] == op[j]) {
                j += 1U;
            }
                if (j == op_length) {
                    int skip = 0;

                    if (ir_operator_is_embedded(expr, i, op)) {
                        i += op_length;
                        continue;
                    }

                    if (op_length == 1U && (op[0] == '+' || op[0] == '-' || op[0] == '*')) {
                        size_t k = i;
                    while (k > 0U && (expr[k - 1U] == ' ' || expr[k - 1U] == '\t')) {
                        k -= 1U;
                    }
                    if (k == 0U ||
                        expr[k - 1U] == '(' || expr[k - 1U] == '[' || expr[k - 1U] == '{' ||
                        expr[k - 1U] == ',' || expr[k - 1U] == '?' || expr[k - 1U] == ':' ||
                        expr[k - 1U] == '=' || expr[k - 1U] == '!' || expr[k - 1U] == '~' ||
                        expr[k - 1U] == '+' || expr[k - 1U] == '-' || expr[k - 1U] == '*' ||
                        expr[k - 1U] == '/' || expr[k - 1U] == '%' || expr[k - 1U] == '<' ||
                        expr[k - 1U] == '>' || expr[k - 1U] == '&' || expr[k - 1U] == '|' ||
                        expr[k - 1U] == '^') {
                        skip = 1;
                    }
                }

                if (!skip) {
                    last = expr + i;
                }
                i += op_length;
                continue;
            }
        }
        i += 1U;
    }

    if (last == 0) {
        return -1;
    }
    *position_out = last;
    return 0;
}

static int ir_operator_precedence(const char *op) {
    if (ir_text_equals(op, "||")) return 1;
    if (ir_text_equals(op, "&&")) return 2;
    if (ir_text_equals(op, "|")) return 3;
    if (ir_text_equals(op, "^")) return 4;
    if (ir_text_equals(op, "&")) return 5;
    if (ir_text_equals(op, "==") || ir_text_equals(op, "!=")) return 6;
    if (ir_text_equals(op, "<") || ir_text_equals(op, ">") || ir_text_equals(op, "<=") || ir_text_equals(op, ">=")) return 7;
    if (ir_text_equals(op, "<<") || ir_text_equals(op, ">>")) return 8;
    if (ir_text_equals(op, "+") || ir_text_equals(op, "-")) return 9;
    if (ir_text_equals(op, "*") || ir_text_equals(op, "/") || ir_text_equals(op, "%")) return 10;
    return 0;
}

static int ir_operator_has_right_precedence_boundary(const char *expr, const char *position, const char *op) {
    const char ops[][3] = {"||", "&&", "|", "^", "&", "==", "!=", "<=", ">=", "<", ">", "<<", ">>", "+", "-", "*", "/", "%"};
    size_t i;
    int precedence = ir_operator_precedence(op);

    if (precedence == 0) {
        return 0;
    }

    for (i = 0; i < sizeof(ops) / sizeof(ops[0]); ++i) {
        const char *other_position = 0;
        if (ir_find_top_level_operator(expr, ops[i], &other_position) == 0 && other_position > position &&
            ir_operator_precedence(ops[i]) <= precedence) {
            return 1;
        }
    }

    return 0;
}

static int ir_find_top_level_conditional(const char *expr, const char **question_out, const char **colon_out) {
    size_t i = 0;
    int paren_depth = 0;
    int bracket_depth = 0;
    int brace_depth = 0;
    int conditional_depth = 0;
    int in_string = 0;
    int in_char = 0;
    const char *question = 0;

    while (expr[i] != '\0') {
        if ((in_string || in_char) && expr[i] == '\\' && expr[i + 1] != '\0') {
            i += 2U;
            continue;
        }
        if (!in_char && expr[i] == '"') {
            in_string = !in_string;
            i += 1U;
            continue;
        }
        if (!in_string && expr[i] == '\'') {
            in_char = !in_char;
            i += 1U;
            continue;
        }
        if (in_string || in_char) {
            i += 1U;
            continue;
        }

        if (expr[i] == '(') {
            paren_depth += 1;
        } else if (expr[i] == ')') {
            if (paren_depth > 0) {
                paren_depth -= 1;
            }
        } else if (expr[i] == '[') {
            bracket_depth += 1;
        } else if (expr[i] == ']') {
            if (bracket_depth > 0) {
                bracket_depth -= 1;
            }
        } else if (expr[i] == '{') {
            brace_depth += 1;
        } else if (expr[i] == '}') {
            if (brace_depth > 0) {
                brace_depth -= 1;
            }
        } else if (paren_depth == 0 && bracket_depth == 0 && brace_depth == 0) {
            if (expr[i] == '?') {
                if (question == 0) {
                    question = expr + i;
                } else {
                    conditional_depth += 1;
                }
            } else if (expr[i] == ':' && question != 0) {
                if (conditional_depth == 0) {
                    *question_out = question;
                    *colon_out = expr + i;
                    return 0;
                }
                conditional_depth -= 1;
            }
        }
        i += 1U;
    }

    return -1;
}

static int ir_try_simplify_conditional_expr(const char *expr, char *buffer, size_t buffer_size) {
    const char *question = 0;
    const char *colon = 0;
    const char *condition_start;
    const char *condition_end;
    const char *true_start;
    const char *true_end;
    const char *false_start;
    const char *false_end;
    char condition[COMPILER_IR_LINE_CAPACITY];
    char true_expr[COMPILER_IR_LINE_CAPACITY];
    char false_expr[COMPILER_IR_LINE_CAPACITY];
    char normalized_true[COMPILER_IR_LINE_CAPACITY];
    char normalized_false[COMPILER_IR_LINE_CAPACITY];

    if (ir_find_top_level_conditional(expr, &question, &colon) != 0) {
        return 0;
    }

    condition_start = ir_skip_spaces(expr);
    condition_end = ir_trim_trailing_spaces(condition_start, question);
    true_start = ir_skip_spaces(question + 1);
    true_end = ir_trim_trailing_spaces(true_start, colon);
    false_start = ir_skip_spaces(colon + 1);
    false_end = ir_trim_trailing_spaces(false_start, false_start + rt_strlen(false_start));

    if (ir_copy_identifier(condition, sizeof(condition), condition_start, condition_end) != 0 ||
        ir_copy_identifier(true_expr, sizeof(true_expr), true_start, true_end) != 0 ||
        ir_copy_identifier(false_expr, sizeof(false_expr), false_start, false_end) != 0) {
        return 0;
    }
    if (ir_expr_has_side_effect_risk(condition)) {
        return 0;
    }
    if (ir_normalize_expr_text(true_expr, normalized_true, sizeof(normalized_true)) != 0 ||
        ir_normalize_expr_text(false_expr, normalized_false, sizeof(normalized_false)) != 0) {
        return 0;
    }
    if (ir_text_equals(normalized_true, normalized_false) && ir_expr_is_trivial_value(normalized_true)) {
        rt_copy_string(buffer, buffer_size, normalized_true);
        return 1;
    }

    return 0;
}

static int ir_try_simplify_identity_expr(const char *expr, const IrOptimizerState *state, char *buffer, size_t buffer_size) {
    const char ops[][3] = {"&&", "||", "==", "!=", "<=", ">=", "<", ">", "&", "|", "^", "<<", ">>", "+", "-", "*", "/"};
    size_t op_count = sizeof(ops) / sizeof(ops[0]);
    size_t op_index;

    {
        const char *logical_position = 0;
        if (ir_find_top_level_operator(expr, "&&", &logical_position) == 0 ||
            ir_find_top_level_operator(expr, "||", &logical_position) == 0) {
            op_count = 2U;
        }
    }

    for (op_index = 0; op_index < op_count; ++op_index) {
        const char *op = ops[op_index];
        const char *position = 0;
        const char *lhs_start;
        const char *lhs_end;
        const char *rhs_start;
        const char *rhs_end;
        char lhs[COMPILER_IR_LINE_CAPACITY];
        char rhs[COMPILER_IR_LINE_CAPACITY];
        long long lhs_value = 0;
        long long rhs_value = 0;
        int lhs_is_constant;
        int rhs_is_constant;

        if (ir_find_top_level_operator(expr, op, &position) != 0) {
            continue;
        }
        if (ir_operator_has_right_precedence_boundary(expr, position, op)) {
            continue;
        }

        lhs_start = ir_skip_spaces(expr);
        lhs_end = ir_trim_trailing_spaces(lhs_start, position);
        rhs_start = ir_skip_spaces(position + rt_strlen(op));
        rhs_end = ir_trim_trailing_spaces(rhs_start, rhs_start + rt_strlen(rhs_start));

        if (ir_copy_identifier(lhs, sizeof(lhs), lhs_start, lhs_end) != 0 ||
            ir_copy_identifier(rhs, sizeof(rhs), rhs_start, rhs_end) != 0) {
            continue;
        }

        {
            char optimized_lhs[COMPILER_IR_LINE_CAPACITY];
            char optimized_rhs[COMPILER_IR_LINE_CAPACITY];
            long long nested_value = 0;
            int nested_changed = 0;
            int nested_constant = 0;

            if (ir_optimize_expr_text(lhs, state, optimized_lhs, sizeof(optimized_lhs), &nested_changed, &nested_constant, &nested_value) == 0 &&
                (nested_changed || nested_constant) &&
                !ir_expr_text_equals(lhs, optimized_lhs) &&
                ir_expr_is_trivial_value(optimized_lhs)) {
                size_t offset = 0;
                buffer[0] = '\0';
                if (append_text(buffer, buffer_size, &offset, optimized_lhs) != 0 ||
                    append_text(buffer, buffer_size, &offset, " ") != 0 ||
                    append_text(buffer, buffer_size, &offset, op) != 0 ||
                    append_text(buffer, buffer_size, &offset, " ") != 0 ||
                    append_text(buffer, buffer_size, &offset, rhs) != 0) {
                    return -1;
                }
                return 1;
            }

            nested_changed = 0;
            nested_constant = 0;
            nested_value = 0;
            if (ir_optimize_expr_text(rhs, state, optimized_rhs, sizeof(optimized_rhs), &nested_changed, &nested_constant, &nested_value) == 0 &&
                (nested_changed || nested_constant) &&
                !ir_expr_text_equals(rhs, optimized_rhs) &&
                ir_expr_is_trivial_value(optimized_rhs)) {
                size_t offset = 0;
                buffer[0] = '\0';
                if (append_text(buffer, buffer_size, &offset, lhs) != 0 ||
                    append_text(buffer, buffer_size, &offset, " ") != 0 ||
                    append_text(buffer, buffer_size, &offset, op) != 0 ||
                    append_text(buffer, buffer_size, &offset, " ") != 0 ||
                    append_text(buffer, buffer_size, &offset, optimized_rhs) != 0) {
                    return -1;
                }
                return 1;
            }
        }

        lhs_is_constant = ir_evaluate_constant_expression(lhs, state, &lhs_value) == 0;
        rhs_is_constant = ir_evaluate_constant_expression(rhs, state, &rhs_value) == 0;

        if (ir_expr_text_equals(lhs, rhs) && ir_expr_is_trivial_value(lhs)) {
            if (ir_text_equals(op, "-") || ir_text_equals(op, "^")) {
                rt_copy_string(buffer, buffer_size, "0");
                return 1;
            }
            if (ir_text_equals(op, "&") || ir_text_equals(op, "|")) {
                rt_copy_string(buffer, buffer_size, lhs);
                return 1;
            }
            if (ir_text_equals(op, "==") || ir_text_equals(op, "<=") || ir_text_equals(op, ">=")) {
                rt_copy_string(buffer, buffer_size, "1");
                return 1;
            }
            if (ir_text_equals(op, "!=") || ir_text_equals(op, "<") || ir_text_equals(op, ">")) {
                rt_copy_string(buffer, buffer_size, "0");
                return 1;
            }
        }

        if (ir_text_equals(op, "&&") && lhs_is_constant && lhs_value == 0) {
            rt_copy_string(buffer, buffer_size, "0");
            return 1;
        }
        if (ir_text_equals(op, "||") && lhs_is_constant && lhs_value != 0) {
            rt_copy_string(buffer, buffer_size, "1");
            return 1;
        }
        if ((ir_text_equals(op, "+") || ir_text_equals(op, "|") || ir_text_equals(op, "^")) &&
            lhs_is_constant && lhs_value == 0) {
            rt_copy_string(buffer, buffer_size, rhs);
            return 1;
        }
        if ((ir_text_equals(op, "*") || ir_text_equals(op, "&")) &&
            lhs_is_constant && lhs_value == 0 && ir_expr_is_trivial_value(rhs)) {
            rt_copy_string(buffer, buffer_size, "0");
            return 1;
        }
        if (ir_expr_is_trivial_value(lhs)) {
            if (ir_text_equals(op, "&&") && rhs_is_constant && rhs_value == 0) {
                rt_copy_string(buffer, buffer_size, "0");
                return 1;
            }
            if (ir_text_equals(op, "||") && rhs_is_constant && rhs_value != 0) {
                rt_copy_string(buffer, buffer_size, "1");
                return 1;
            }
        }
        if ((ir_text_equals(op, "+") || ir_text_equals(op, "-") || ir_text_equals(op, "|") ||
             ir_text_equals(op, "^") || ir_text_equals(op, "<<") || ir_text_equals(op, ">>")) &&
            rhs_is_constant && rhs_value == 0) {
            rt_copy_string(buffer, buffer_size, lhs);
            return 1;
        }
        if ((ir_text_equals(op, "*") || ir_text_equals(op, "&")) &&
            rhs_is_constant && rhs_value == 0 && ir_expr_is_trivial_value(lhs)) {
            rt_copy_string(buffer, buffer_size, "0");
            return 1;
        }
        if (ir_text_equals(op, "*") && lhs_is_constant && lhs_value == 1) {
            rt_copy_string(buffer, buffer_size, rhs);
            return 1;
        }
        if (ir_text_equals(op, "*") && rhs_is_constant && rhs_value == 1) {
            rt_copy_string(buffer, buffer_size, lhs);
            return 1;
        }
        if (ir_text_equals(op, "/") && rhs_is_constant && rhs_value == 1) {
            rt_copy_string(buffer, buffer_size, lhs);
            return 1;
        }
    }

    return 0;
}

static int ir_normalize_expr_text(const char *expr, char *buffer, size_t buffer_size) {
    const char *start = ir_skip_spaces(expr != 0 ? expr : "");
    const char *end = ir_trim_trailing_spaces(start, start + rt_strlen(start));
    char current[COMPILER_IR_LINE_CAPACITY];

    if (start == end || ir_copy_identifier(current, sizeof(current), start, end) != 0) {
        return -1;
    }

    for (;;) {
        char simplified[COMPILER_IR_LINE_CAPACITY];
        int result = ir_strip_outer_parens(current, simplified, sizeof(simplified));

        if (result < 0) {
            return -1;
        }
        if (result == 0) {
            break;
        }
        rt_copy_string(current, sizeof(current), simplified);
    }

    rt_copy_string(buffer, buffer_size, current);
    return 0;
}

static int ir_expr_is_trivial_value(const char *expr) {
    char normalized[COMPILER_IR_LINE_CAPACITY];
    IrConstParser parser;

    if (ir_normalize_expr_text(expr, normalized, sizeof(normalized)) != 0) {
        return 0;
    }

    parser.cursor = normalized;
    parser.state = 0;
    ir_const_next(&parser);
    if (parser.current.kind != IR_CONST_TOKEN_IDENTIFIER &&
        parser.current.kind != IR_CONST_TOKEN_NUMBER &&
        parser.current.kind != IR_CONST_TOKEN_CHAR) {
        return 0;
    }
    ir_const_next(&parser);
    return parser.current.kind == IR_CONST_TOKEN_EOF;
}

static int ir_expr_text_equals(const char *lhs, const char *rhs) {
    char left[COMPILER_IR_LINE_CAPACITY];
    char right[COMPILER_IR_LINE_CAPACITY];

    if (ir_normalize_expr_text(lhs, left, sizeof(left)) != 0 ||
        ir_normalize_expr_text(rhs, right, sizeof(right)) != 0) {
        return 0;
    }
    return ir_text_equals(left, right);
}

static int ir_assignment_is_noop(const char *name, const char *expr) {
    return name != 0 && expr != 0 && ir_expr_text_equals(name, expr);
}

static int ir_has_many_logical_operators(const char *expr) {
    size_t i = 0;
    unsigned int count = 0;
    int in_string = 0;
    int in_char = 0;

    while (expr[i] != '\0') {
        if ((in_string || in_char) && expr[i] == '\\' && expr[i + 1U] != '\0') {
            i += 2U;
            continue;
        }
        if (!in_char && expr[i] == '"') {
            in_string = !in_string;
            i += 1U;
            continue;
        }
        if (!in_string && expr[i] == '\'') {
            in_char = !in_char;
            i += 1U;
            continue;
        }
        if (!in_string && !in_char &&
            ((expr[i] == '|' && expr[i + 1U] == '|') || (expr[i] == '&' && expr[i + 1U] == '&'))) {
            count += 1U;
            if (count > 4U) {
                return 1;
            }
            i += 2U;
            continue;
        }
        i += 1U;
    }

    return 0;
}

static int ir_optimize_expr_text(const char *expr,
                                 const IrOptimizerState *state,
                                 char *buffer,
                                 size_t buffer_size,
                                 int *changed_out,
                                 int *constant_out,
                                 long long *value_out) {
    char current[COMPILER_IR_LINE_CAPACITY];
    IrOptimizerState cautious_state;
    const IrOptimizerState *effective_state = state;
    int changed = 0;
    int local_change;

    if (changed_out != 0) {
        *changed_out = 0;
    }
    if (constant_out != 0) {
        *constant_out = 0;
    }

    expr = ir_skip_spaces(expr != 0 ? expr : "");
    if (state != 0 && ir_expr_has_side_effect_risk(expr)) {
        cautious_state = *state;
        cautious_state.local_value_count = 0;
        effective_state = &cautious_state;
    }
    if (ir_copy_identifier(current,
                           sizeof(current),
                           expr,
                           ir_trim_trailing_spaces(expr, expr + rt_strlen(expr))) != 0) {
        return -1;
    }

    do {
        local_change = 0;

        if (ir_evaluate_constant_expression(current, effective_state, value_out) == 0) {
            ir_format_signed_value(*value_out, buffer, buffer_size);
            if (changed_out != 0) {
                *changed_out = 1;
            }
            if (constant_out != 0) {
                *constant_out = 1;
            }
            return 0;
        }

        {
            char simplified[COMPILER_IR_LINE_CAPACITY];
            int result = ir_strip_outer_parens(current, simplified, sizeof(simplified));
            if (result < 0) {
                return -1;
            }
            if (result > 0) {
                rt_copy_string(current, sizeof(current), simplified);
                changed = 1;
                local_change = 1;
                continue;
            }
        }

        {
            char simplified[COMPILER_IR_LINE_CAPACITY];
            int result = ir_try_simplify_conditional_expr(current, simplified, sizeof(simplified));
            if (result < 0) {
                return -1;
            }
            if (result > 0) {
                rt_copy_string(current, sizeof(current), simplified);
                changed = 1;
                local_change = 1;
                continue;
            }
        }

        {
            char simplified[COMPILER_IR_LINE_CAPACITY];
            const char *conditional_question = 0;
            const char *conditional_colon = 0;
            int has_top_level_conditional = ir_find_top_level_conditional(current, &conditional_question, &conditional_colon) == 0;
            int result = (has_top_level_conditional || ir_has_many_logical_operators(current)) ? 0 :
                         ir_try_simplify_identity_expr(current, effective_state, simplified, sizeof(simplified));
            if (result < 0) {
                return -1;
            }
            if (result > 0) {
                rt_copy_string(current, sizeof(current), simplified);
                changed = 1;
                local_change = 1;
            }
        }
    } while (local_change);

    rt_copy_string(buffer, buffer_size, current);
    if (changed_out != 0) {
        *changed_out = changed;
    }
    return 0;
}

static void ir_clear_local_values(IrOptimizerState *state) {
    state->local_value_count = 0;
}

static void ir_invalidate_local_value(IrOptimizerState *state, const char *name) {
    size_t i;
    size_t j = 0;

    for (i = 0; i < state->local_value_count; ++i) {
        if (!ir_text_equals(state->local_values[i].name, name)) {
            if (j != i) {
                state->local_values[j] = state->local_values[i];
            }
            j += 1U;
        }
    }
    state->local_value_count = j;
}

static int ir_set_tracked_value(IrTrackedValue *values, size_t *count, const char *name, long long value) {
    size_t i;

    for (i = 0; i < *count; ++i) {
        if (ir_text_equals(values[i].name, name)) {
            values[i].value = value;
            return 0;
        }
    }

    if (*count >= COMPILER_IR_TRACKED_VALUE_CAPACITY) {
        return -1;
    }

    rt_copy_string(values[*count].name, sizeof(values[*count].name), name);
    values[*count].value = value;
    *count += 1U;
    return 0;
}

static int ir_rewrite_expr_line(CompilerIr *ir, size_t index, const char *prefix, const char *expr) {
    size_t offset = 0;
    char line[COMPILER_IR_LINE_CAPACITY];

    line[0] = '\0';
    if (append_text(line, sizeof(line), &offset, prefix) != 0 ||
        append_text(line, sizeof(line), &offset, expr) != 0) {
        set_error(ir, "IR instruction text exceeded line capacity");
        return -1;
    }
    return ir_replace_line(ir, index, line);
}

static int ir_rewrite_branch_line(CompilerIr *ir, size_t index, const char *expr, const char *label) {
    size_t offset = 0;
    char line[COMPILER_IR_LINE_CAPACITY];

    line[0] = '\0';
    if (append_text(line, sizeof(line), &offset, "brfalse ") != 0 ||
        append_text(line, sizeof(line), &offset, expr) != 0 ||
        append_text(line, sizeof(line), &offset, " -> ") != 0 ||
        append_text(line, sizeof(line), &offset, label) != 0) {
        set_error(ir, "IR instruction text exceeded line capacity");
        return -1;
    }
    return ir_replace_line(ir, index, line);
}

static int ir_rewrite_jump_line(CompilerIr *ir, size_t index, const char *label) {
    return ir_rewrite_expr_line(ir, index, "jump ", label);
}

static void ir_remove_line(CompilerIr *ir, size_t index) {
    size_t i;

    if (index >= ir->count) {
        return;
    }
    rt_free(ir->lines[index]);
    for (i = index + 1U; i < ir->count; ++i) {
        ir->lines[i - 1U] = ir->lines[i];
    }
    if (ir->count > 0) {
        ir->count -= 1U;
        ir->lines[ir->count] = 0;
    }
}

static const char *ir_find_separator_outside_quotes(const char *text, const char *separator) {
    const char *last = 0;
    int in_string = 0;
    int in_char = 0;
    size_t i = 0;
    size_t separator_length = rt_strlen(separator);

    while (text[i] != '\0') {
        if ((in_string || in_char) && text[i] == '\\' && text[i + 1U] != '\0') {
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

static void ir_remove_unreachable_after_terminator(CompilerIr *ir, size_t index) {
    while (index + 1U < ir->count && !ir_starts_block_boundary(ir->lines[index + 1U])) {
        ir_remove_line(ir, index + 1U);
    }
}

static int ir_extract_name_after_prefix(const char *cursor, char *name, size_t name_size, const char *separator, const char **expr_out) {
    const char *name_end = cursor;
    while (ir_is_identifier_char(*name_end)) {
        name_end += 1;
    }
    if (ir_copy_identifier(name, name_size, cursor, name_end) != 0) {
        return -1;
    }
    if (!ir_starts_with(name_end, separator)) {
        return -1;
    }
    *expr_out = name_end + rt_strlen(separator);
    return 0;
}

static int ir_extract_store_parts(const char *line, char *name, size_t name_size, const char **expr_out) {
    return ir_extract_name_after_prefix(line + 6, name, name_size, " <- ", expr_out);
}

static int ir_extract_branch_parts(const char *line, char *label, size_t label_size, char *expr, size_t expr_size) {
    const char *expr_start = line + 8;
    const char *arrow = ir_find_separator_outside_quotes(expr_start, " -> ");

    if (arrow != 0) {
        if (ir_copy_identifier(label, label_size, arrow + 4, arrow + 4 + rt_strlen(arrow + 4)) != 0) {
            return -1;
        }
        if (ir_copy_identifier(expr, expr_size, expr_start, arrow) != 0) {
            return -1;
        }
        return 0;
    }

    return -1;
}

static int ir_extract_jump_target(const char *line, char *label, size_t label_size) {
    const char *start = ir_skip_spaces(line + 5);
    const char *end = ir_trim_trailing_spaces(start, start + rt_strlen(start));

    return ir_copy_identifier(label, label_size, start, end);
}

static int ir_extract_const_parts(const char *line, char *name, size_t name_size, const char **expr_out) {
    return ir_extract_name_after_prefix(line + 6, name, name_size, " = ", expr_out);
}

static int ir_extract_assignment_parts(const char *expr, char *name, size_t name_size, char *op, size_t op_size, const char **rhs_out) {
    IrConstParser parser;

    parser.cursor = expr != 0 ? expr : "";
    parser.state = 0;
    ir_const_next(&parser);
    if (parser.current.kind != IR_CONST_TOKEN_IDENTIFIER) {
        return -1;
    }
    rt_copy_string(name, name_size, parser.current.text);
    ir_const_next(&parser);
    if (parser.current.kind != IR_CONST_TOKEN_PUNCT ||
        !(ir_text_equals(parser.current.text, "=") ||
          ir_text_equals(parser.current.text, "+=") ||
          ir_text_equals(parser.current.text, "-=") ||
          ir_text_equals(parser.current.text, "*=") ||
          ir_text_equals(parser.current.text, "/=") ||
          ir_text_equals(parser.current.text, "%=") ||
          ir_text_equals(parser.current.text, "<<=") ||
          ir_text_equals(parser.current.text, ">>=") ||
          ir_text_equals(parser.current.text, "&=") ||
          ir_text_equals(parser.current.text, "^=") ||
          ir_text_equals(parser.current.text, "|="))) {
        return -1;
    }
    rt_copy_string(op, op_size, parser.current.text);
    *rhs_out = ir_skip_spaces(parser.cursor);
    return **rhs_out == '\0' ? -1 : 0;
}

static int ir_assignment_op_is_noop(const char *op, const char *rhs, const IrOptimizerState *state) {
    long long value = 0;

    if (op == 0 || rhs == 0 || ir_evaluate_constant_expression(rhs, state, &value) != 0) {
        return 0;
    }

    if (value == 0 &&
        (ir_text_equals(op, "+=") || ir_text_equals(op, "-=") ||
         ir_text_equals(op, "|=") || ir_text_equals(op, "^=") ||
         ir_text_equals(op, "<<=") || ir_text_equals(op, ">>="))) {
        return 1;
    }
    if (value == 1 && (ir_text_equals(op, "*=") || ir_text_equals(op, "/="))) {
        return 1;
    }

    return 0;
}

int compiler_ir_optimize(CompilerIr *ir) {
    IrOptimizerState state;
    size_t i = 0;

    rt_memset(&state, 0, sizeof(state));

    while (i < ir->count) {
        char *line = ir->lines[i];

        if (ir_starts_with(line, "label ")) {
            if (i > 0U && ir_starts_with(ir->lines[i - 1U], "jump ")) {
                char current_label[COMPILER_IR_NAME_CAPACITY];
                char previous_target[COMPILER_IR_NAME_CAPACITY];

                if (ir_copy_identifier(current_label,
                                       sizeof(current_label),
                                       line + 6,
                                       ir_trim_trailing_spaces(line + 6, line + rt_strlen(line))) == 0 &&
                    ir_extract_jump_target(ir->lines[i - 1U], previous_target, sizeof(previous_target)) == 0 &&
                    ir_text_equals(current_label, previous_target)) {
                    ir_remove_line(ir, i - 1U);
                    i -= 1U;
                    continue;
                }
            }
            {
                char label[COMPILER_IR_NAME_CAPACITY];

                if (ir_copy_identifier(label,
                                       sizeof(label),
                                       line + 6,
                                       ir_trim_trailing_spaces(line + 6, line + rt_strlen(line))) == 0 &&
                    !ir_label_is_referenced(ir, label, i)) {
                    ir_remove_line(ir, i);
                    while (i > 0U && i < ir->count &&
                           ir_is_terminator_line(ir->lines[i - 1U]) &&
                           !ir_starts_block_boundary(ir->lines[i])) {
                        ir_remove_line(ir, i);
                    }
                    continue;
                }
            }
            ir_clear_local_values(&state);
            i += 1U;
            continue;
        }

        if (ir_starts_with(line, "func ") || ir_starts_with(line, "endfunc ") ||
            ir_starts_with(line, "switch ") || ir_starts_with(line, "case ") ||
            ir_text_equals(line, "default") || ir_starts_with(line, "endswitch") ||
            ir_starts_with(line, "scope-enter") || ir_starts_with(line, "scope-exit")) {
            ir_clear_local_values(&state);
            i += 1U;
            continue;
        }

        if (ir_starts_with(line, "asm-syscall")) {
            ir_clear_local_values(&state);
            i += 1U;
            continue;
        }

        if (ir_starts_with(line, "const ")) {
            char name[COMPILER_IR_NAME_CAPACITY];
            const char *expr = 0;
            long long value = 0;

            if (ir_extract_const_parts(line, name, sizeof(name), &expr) == 0 &&
                ir_evaluate_constant_expression(expr, &state, &value) == 0 &&
                ir_set_tracked_value(state.global_constants, &state.global_constant_count, name, value) == 0) {
                char number[32];
                char rewritten[COMPILER_IR_LINE_CAPACITY];
                size_t offset = 0;

                ir_format_signed_value(value, number, sizeof(number));
                rewritten[0] = '\0';
                if (append_text(rewritten, sizeof(rewritten), &offset, "const ") != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, name) != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, " = ") != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, number) != 0) {
                    set_error(ir, "IR instruction text exceeded line capacity");
                    return -1;
                }
                if (ir_replace_line(ir, i, rewritten) != 0) {
                    return -1;
                }
            }
            i += 1U;
            continue;
        }

        if (ir_starts_with(line, "decl ")) {
            const char *cursor = line + rt_strlen(line);
            while (cursor > line && cursor[-1] != ' ') {
                cursor -= 1;
            }
            if (*cursor != '\0') {
                ir_invalidate_local_value(&state, cursor);
            }
            i += 1U;
            continue;
        }

        if (ir_starts_with(line, "store ")) {
            char name[COMPILER_IR_NAME_CAPACITY];
            const char *expr = 0;
            long long value = 0;
            char optimized_expr[COMPILER_IR_LINE_CAPACITY];
            int expr_changed = 0;
            int expr_constant = 0;
            int parsed_store = ir_extract_store_parts(line, name, sizeof(name), &expr) == 0;
            int optimized_store = 0;

            if (parsed_store) {
                optimized_store =
                    ir_optimize_expr_text(expr, &state, optimized_expr, sizeof(optimized_expr), &expr_changed, &expr_constant, &value) == 0;
            }

            if (parsed_store && optimized_store && !expr_constant && ir_assignment_is_noop(name, optimized_expr)) {
                ir_remove_line(ir, i);
                continue;
            }

            if (parsed_store && optimized_store && expr_constant) {
                char number[32];
                char rewritten[COMPILER_IR_LINE_CAPACITY];
                size_t offset = 0;

                ir_format_signed_value(value, number, sizeof(number));
                rewritten[0] = '\0';
                if (append_text(rewritten, sizeof(rewritten), &offset, "store ") != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, name) != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, " <- ") != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, number) != 0) {
                    set_error(ir, "IR instruction text exceeded line capacity");
                    return -1;
                }
                if (ir_replace_line(ir, i, rewritten) != 0) {
                    return -1;
                }
                if (ir_set_tracked_value(state.local_values, &state.local_value_count, name, value) != 0) {
                    set_error(ir, "IR optimizer tracking capacity exceeded");
                    return -1;
                }
            } else if (parsed_store && optimized_store && expr_changed) {
                char rewritten[COMPILER_IR_LINE_CAPACITY];
                size_t offset = 0;

                rewritten[0] = '\0';
                if (append_text(rewritten, sizeof(rewritten), &offset, "store ") != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, name) != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, " <- ") != 0 ||
                    append_text(rewritten, sizeof(rewritten), &offset, optimized_expr) != 0) {
                    set_error(ir, "IR instruction text exceeded line capacity");
                    return -1;
                }
                if (ir_replace_line(ir, i, rewritten) != 0) {
                    return -1;
                }
                ir_invalidate_local_value(&state, name);
            } else if (parsed_store) {
                ir_invalidate_local_value(&state, name);
            }
            if (parsed_store && expr != 0 && ir_expr_has_side_effect_risk(expr)) {
                ir_clear_local_values(&state);
            }
            i += 1U;
            continue;
        }

        if (ir_starts_with(line, "eval ")) {
            char name[COMPILER_IR_NAME_CAPACITY];
            char op[4];
            const char *expr = line + 5;
            const char *rhs = 0;
            long long value = 0;
            char optimized_expr[COMPILER_IR_LINE_CAPACITY];
            int expr_changed = 0;
            int expr_constant = 0;

            if (ir_extract_assignment_parts(expr, name, sizeof(name), op, sizeof(op), &rhs) == 0) {
                if (ir_text_equals(op, "=")) {
                    int optimized_rhs =
                        ir_optimize_expr_text(rhs, &state, optimized_expr, sizeof(optimized_expr), &expr_changed, &expr_constant, &value) == 0;

                    if (optimized_rhs && expr_constant) {
                        char number[32];
                        char rewritten[COMPILER_IR_LINE_CAPACITY];
                        size_t offset = 0;

                        ir_format_signed_value(value, number, sizeof(number));
                        rewritten[0] = '\0';
                        if (append_text(rewritten, sizeof(rewritten), &offset, "store ") != 0 ||
                            append_text(rewritten, sizeof(rewritten), &offset, name) != 0 ||
                            append_text(rewritten, sizeof(rewritten), &offset, " <- ") != 0 ||
                            append_text(rewritten, sizeof(rewritten), &offset, number) != 0) {
                            set_error(ir, "IR instruction text exceeded line capacity");
                            return -1;
                        }
                        if (ir_replace_line(ir, i, rewritten) != 0) {
                            return -1;
                        }
                        if (ir_set_tracked_value(state.local_values, &state.local_value_count, name, value) != 0) {
                            set_error(ir, "IR optimizer tracking capacity exceeded");
                            return -1;
                        }
                        i += 1U;
                        continue;
                    } else if (optimized_rhs && ir_assignment_is_noop(name, optimized_expr)) {
                        ir_remove_line(ir, i);
                        continue;
                    } else if (optimized_rhs && expr_changed) {
                        char rewritten[COMPILER_IR_LINE_CAPACITY];
                        size_t offset = 0;

                        rewritten[0] = '\0';
                        if (append_text(rewritten, sizeof(rewritten), &offset, "eval ") != 0 ||
                            append_text(rewritten, sizeof(rewritten), &offset, name) != 0 ||
                            append_text(rewritten, sizeof(rewritten), &offset, " = ") != 0 ||
                            append_text(rewritten, sizeof(rewritten), &offset, optimized_expr) != 0) {
                            set_error(ir, "IR instruction text exceeded line capacity");
                            return -1;
                        }
                        if (ir_replace_line(ir, i, rewritten) != 0) {
                            return -1;
                        }
                    }
                } else {
                    long long lhs = 0;
                    long long rhs_value = 0;
                    const char *binary_op = 0;

                    if (ir_assignment_op_is_noop(op, rhs, &state)) {
                        ir_remove_line(ir, i);
                        continue;
                    }
                    if (ir_lookup_value(&state, name, &lhs) == 0 &&
                        ir_evaluate_constant_expression(rhs, &state, &rhs_value) == 0) {
                        if (ir_text_equals(op, "+=")) binary_op = "+";
                        else if (ir_text_equals(op, "-=")) binary_op = "-";
                        else if (ir_text_equals(op, "*=")) binary_op = "*";
                        else if (ir_text_equals(op, "/=")) binary_op = "/";
                        else if (ir_text_equals(op, "%=")) binary_op = "%";
                        else if (ir_text_equals(op, "<<=")) binary_op = "<<";
                        else if (ir_text_equals(op, ">>=")) binary_op = ">>";
                        else if (ir_text_equals(op, "&=")) binary_op = "&";
                        else if (ir_text_equals(op, "^=")) binary_op = "^";
                        else if (ir_text_equals(op, "|=")) binary_op = "|";
                        if (binary_op != 0 && ir_apply_binary_op(binary_op, lhs, rhs_value, &value) == 0) {
                            char number[32];
                            char rewritten[COMPILER_IR_LINE_CAPACITY];
                            size_t offset = 0;

                            ir_format_signed_value(value, number, sizeof(number));
                            rewritten[0] = '\0';
                            if (append_text(rewritten, sizeof(rewritten), &offset, "store ") != 0 ||
                                append_text(rewritten, sizeof(rewritten), &offset, name) != 0 ||
                                append_text(rewritten, sizeof(rewritten), &offset, " <- ") != 0 ||
                                append_text(rewritten, sizeof(rewritten), &offset, number) != 0) {
                                set_error(ir, "IR instruction text exceeded line capacity");
                                return -1;
                            }
                            if (ir_replace_line(ir, i, rewritten) != 0) {
                                return -1;
                            }
                            if (ir_set_tracked_value(state.local_values, &state.local_value_count, name, value) != 0) {
                                set_error(ir, "IR optimizer tracking capacity exceeded");
                                return -1;
                            }
                            i += 1U;
                            continue;
                        }
                    }
                }
                ir_invalidate_local_value(&state, name);
                i += 1U;
                if (ir_expr_has_side_effect_risk(expr)) {
                    ir_clear_local_values(&state);
                }
                continue;
            }

            if (ir_optimize_expr_text(expr, &state, optimized_expr, sizeof(optimized_expr), &expr_changed, &expr_constant, &value) == 0) {
                if (expr_constant) {
                    if (!ir_expr_has_side_effect_risk(expr)) {
                        ir_remove_line(ir, i);
                        continue;
                    }
                    char number[32];
                    ir_format_signed_value(value, number, sizeof(number));
                    if (ir_rewrite_expr_line(ir, i, "eval ", number) != 0) {
                        return -1;
                    }
                } else if (!ir_expr_has_side_effect_risk(optimized_expr) && ir_expr_is_trivial_value(optimized_expr)) {
                    ir_remove_line(ir, i);
                    continue;
                } else if (expr_changed && ir_rewrite_expr_line(ir, i, "eval ", optimized_expr) != 0) {
                    return -1;
                }
            } else {
                ir_clear_local_values(&state);
            }
            if (ir_expr_has_side_effect_risk(expr)) {
                ir_clear_local_values(&state);
            }
            i += 1U;
            continue;
        }

        if (ir_starts_with(line, "jump ")) {
            char label[COMPILER_IR_NAME_CAPACITY];
            char next_label[COMPILER_IR_NAME_CAPACITY];

            if (ir_extract_jump_target(line, label, sizeof(label)) == 0 &&
                i + 1U < ir->count &&
                ir_starts_with(ir->lines[i + 1U], "label ") &&
                ir_copy_identifier(next_label,
                                   sizeof(next_label),
                                   ir->lines[i + 1U] + 6,
                                   ir_trim_trailing_spaces(ir->lines[i + 1U] + 6,
                                                           ir->lines[i + 1U] + rt_strlen(ir->lines[i + 1U]))) == 0 &&
                ir_text_equals(label, next_label)) {
                ir_remove_line(ir, i);
                continue;
            }

            ir_clear_local_values(&state);
            ir_remove_unreachable_after_terminator(ir, i);
            i += 1U;
            continue;
        }

        if (ir_text_equals(line, "ret") || ir_starts_with(line, "ret ")) {
            long long value = 0;
            char optimized_expr[COMPILER_IR_LINE_CAPACITY];
            int expr_changed = 0;
            int expr_constant = 0;

            if (ir_starts_with(line, "ret ") &&
                ir_optimize_expr_text(line + 4, &state, optimized_expr, sizeof(optimized_expr), &expr_changed, &expr_constant, &value) == 0) {
                if (expr_constant) {
                    char number[32];
                    ir_format_signed_value(value, number, sizeof(number));
                    if (ir_rewrite_expr_line(ir, i, "ret ", number) != 0) {
                        return -1;
                    }
                } else if (expr_changed && ir_rewrite_expr_line(ir, i, "ret ", optimized_expr) != 0) {
                    return -1;
                }
            }
            ir_clear_local_values(&state);
            ir_remove_unreachable_after_terminator(ir, i);
            i += 1U;
            continue;
        }

        if (ir_starts_with(line, "brfalse ")) {
            char label[COMPILER_IR_NAME_CAPACITY];
            char expr[COMPILER_IR_LINE_CAPACITY];
            long long value = 0;
            char optimized_expr[COMPILER_IR_LINE_CAPACITY];
            int expr_changed = 0;
            int expr_constant = 0;

            if (ir_extract_branch_parts(line, label, sizeof(label), expr, sizeof(expr)) == 0 &&
                ir_optimize_expr_text(expr, &state, optimized_expr, sizeof(optimized_expr), &expr_changed, &expr_constant, &value) == 0) {
                if (expr_constant) {
                    if (value == 0) {
                        if (ir_rewrite_jump_line(ir, i, label) != 0) {
                            return -1;
                        }
                        ir_clear_local_values(&state);
                        continue;
                    }
                    ir_remove_line(ir, i);
                    continue;
                }
                if (expr_changed && ir_rewrite_branch_line(ir, i, optimized_expr, label) != 0) {
                    return -1;
                }
            }

            ir_clear_local_values(&state);
            i += 1U;
            continue;
        }

        i += 1U;
    }

    return 0;
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

int compiler_ir_emit_constant(CompilerIr *ir, const char *name, long long value) {
    char number_text[32];
    ir_format_signed_value(value, number_text, sizeof(number_text));
    return emit_line(ir, "const ", name, " = ", number_text);
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
