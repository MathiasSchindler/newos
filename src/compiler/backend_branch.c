/* Branch condition lowering helpers. */

#include "backend_internal.h"

static const char *range_skip_left(const char *start, const char *end) {
    while (start < end && (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')) {
        start += 1;
    }
    return start;
}

static const char *range_skip_right(const char *start, const char *end) {
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' || end[-1] == '\r')) {
        end -= 1;
    }
    return end;
}

static int range_copy_trimmed(char *buffer, size_t buffer_size, const char *start, const char *end) {
    size_t length;

    start = range_skip_left(start, end);
    end = range_skip_right(start, end);
    length = (size_t)(end - start);
    if (length + 1U > buffer_size) {
        return -1;
    }
    {
        size_t index;
        for (index = 0; index < length; ++index) {
            buffer[index] = start[index];
        }
    }
    buffer[length] = '\0';
    return 0;
}

static int range_is_wrapped_in_parens(const char *start, const char *end) {
    const char *cursor = start;
    int depth = 0;
    int in_string = 0;
    int in_char = 0;

    if (end <= start + 1 || *start != '(' || end[-1] != ')') {
        return 0;
    }
    while (cursor < end) {
        if ((in_string || in_char) && *cursor == '\\' && cursor + 1 < end) {
            cursor += 2;
            continue;
        }
        if (!in_char && *cursor == '"') {
            in_string = !in_string;
        } else if (!in_string && *cursor == '\'') {
            in_char = !in_char;
        } else if (!in_string && !in_char) {
            if (*cursor == '(') {
                depth += 1;
            } else if (*cursor == ')') {
                depth -= 1;
                if (depth == 0 && cursor + 1 < end) {
                    return 0;
                }
            }
        }
        cursor += 1;
    }
    return depth == 0;
}

static void range_trim_outer_parens(const char **start_inout, const char **end_inout) {
    const char *start = *start_inout;
    const char *end = *end_inout;

    for (;;) {
        start = range_skip_left(start, end);
        end = range_skip_right(start, end);
        if (!range_is_wrapped_in_parens(start, end)) {
            break;
        }
        start += 1;
        end -= 1;
    }
    *start_inout = start;
    *end_inout = end;
}

static const char *range_find_top_level_token(const char *start, const char *end, const char *token) {
    const char *cursor = start;
    int paren_depth = 0;
    int bracket_depth = 0;
    int in_string = 0;
    int in_char = 0;
    size_t token_length = rt_strlen(token);

    while (cursor < end) {
        if ((in_string || in_char) && *cursor == '\\' && cursor + 1 < end) {
            cursor += 2;
            continue;
        }
        if (!in_char && *cursor == '"') {
            in_string = !in_string;
            cursor += 1;
            continue;
        }
        if (!in_string && *cursor == '\'') {
            in_char = !in_char;
            cursor += 1;
            continue;
        }
        if (!in_string && !in_char) {
            if (*cursor == '(') {
                paren_depth += 1;
            } else if (*cursor == ')' && paren_depth > 0) {
                paren_depth -= 1;
            } else if (*cursor == '[') {
                bracket_depth += 1;
            } else if (*cursor == ']' && bracket_depth > 0) {
                bracket_depth -= 1;
            } else if (paren_depth == 0 && bracket_depth == 0 && cursor + token_length <= end) {
                size_t index = 0;
                while (index < token_length && cursor[index] == token[index]) {
                    index += 1U;
                }
                if (index == token_length) {
                    return cursor;
                }
            }
        }
        cursor += 1;
    }
    return 0;
}

static int branch_range_has_fallback_operator(const char *start, const char *end) {
    return range_find_top_level_token(start, end, "||") != 0 ||
           range_find_top_level_token(start, end, "?") != 0 ||
           range_find_top_level_token(start, end, ",") != 0;
}

static int branch_comparison_operator_at(const char *start, const char *cursor, const char *end, const char **op_out) {
    char previous = cursor > start ? cursor[-1] : '\0';
    char next = cursor + 1 < end ? cursor[1] : '\0';

    if (cursor + 2 <= end) {
        if (cursor[0] == '=' && cursor[1] == '=') {
            *op_out = "==";
            return 2;
        }
        if (cursor[0] == '!' && cursor[1] == '=') {
            *op_out = "!=";
            return 2;
        }
        if (cursor[0] == '<' && cursor[1] == '=') {
            *op_out = "<=";
            return 2;
        }
        if (cursor[0] == '>' && cursor[1] == '=') {
            *op_out = ">=";
            return 2;
        }
    }
    if (*cursor == '<' && previous != '<' && next != '<') {
        *op_out = "<";
        return 1;
    }
    if (*cursor == '>' && previous != '-' && previous != '>' && next != '>') {
        *op_out = ">";
        return 1;
    }
    return 0;
}

static const char *range_find_single_comparison(const char *start, const char *end, const char **op_out) {
    const char *cursor = start;
    const char *found = 0;
    int paren_depth = 0;
    int bracket_depth = 0;
    int in_string = 0;
    int in_char = 0;

    while (cursor < end) {
        if ((in_string || in_char) && *cursor == '\\' && cursor + 1 < end) {
            cursor += 2;
            continue;
        }
        if (!in_char && *cursor == '"') {
            in_string = !in_string;
            cursor += 1;
            continue;
        }
        if (!in_string && *cursor == '\'') {
            in_char = !in_char;
            cursor += 1;
            continue;
        }
        if (!in_string && !in_char) {
            if (*cursor == '(') {
                paren_depth += 1;
            } else if (*cursor == ')' && paren_depth > 0) {
                paren_depth -= 1;
            } else if (*cursor == '[') {
                bracket_depth += 1;
            } else if (*cursor == ']' && bracket_depth > 0) {
                bracket_depth -= 1;
            } else if (paren_depth == 0 && bracket_depth == 0) {
                const char *op = 0;
                int width = branch_comparison_operator_at(start, cursor, end, &op);
                if (width > 0) {
                    if (found != 0) {
                        return 0;
                    }
                    found = cursor;
                    *op_out = op;
                    cursor += width;
                    continue;
                }
            }
        }
        cursor += 1;
    }
    return found;
}

static int branch_expr_is_zero_literal(const char *expr) {
    long long value = 1;
    const char *cursor = skip_spaces(expr);

    if (parse_signed_value(cursor, &value) != 0 || value != 0) {
        return 0;
    }
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != '\n' && *cursor != '\r') {
        cursor += 1;
    }
    cursor = skip_spaces(cursor);
    return *cursor == '\0';
}

static int branch_expr_is_double(BackendState *state, const char *expr) {
    ExprParser parser;
    char type_text[128];

    parser.cursor = expr;
    parser.state = state;
    expr_next(&parser);
    expr_infer_result_type(&parser, type_text, sizeof(type_text));
    return text_contains(type_text, "double") &&
           !text_contains(type_text, "*") && !text_contains(type_text, "[");
}

typedef enum {
    BRANCH_COMPARE_OPERAND_NONE = 0,
    BRANCH_COMPARE_OPERAND_IMMEDIATE,
    BRANCH_COMPARE_OPERAND_NAME
} BranchCompareOperandKind;

typedef struct {
    BranchCompareOperandKind kind;
    char name[COMPILER_IR_NAME_CAPACITY];
    long long value;
} BranchCompareOperand;

static int branch_name_is_direct_loadable(const BackendState *state, const char *name) {
    return find_local(state, name) >= 0 ||
           find_global(state, name) >= 0 ||
           find_constant(state, name) >= 0 ||
           is_function_name(state, name) ||
           names_equal(name, "NULL") ||
           names_equal(name, "errno") ||
           name_looks_like_macro_constant(name);
}

static int branch_parse_simple_operand(BackendState *state, const char *expr, BranchCompareOperand *operand) {
    const char *start = expr;
    const char *end = expr + rt_strlen(expr);
    char trimmed[COMPILER_IR_LINE_CAPACITY];
    ExprParser parser;

    operand->kind = BRANCH_COMPARE_OPERAND_NONE;
    operand->name[0] = '\0';
    operand->value = 0;

    range_trim_outer_parens(&start, &end);
    if (range_copy_trimmed(trimmed, sizeof(trimmed), start, end) != 0) {
        return -1;
    }

    parser.cursor = trimmed;
    parser.state = state;
    expr_next(&parser);
    if (parser.current.kind == EXPR_TOKEN_NUMBER || parser.current.kind == EXPR_TOKEN_CHAR) {
        operand->kind = BRANCH_COMPARE_OPERAND_IMMEDIATE;
        operand->value = parser.current.number_value;
        expr_next(&parser);
        return parser.current.kind == EXPR_TOKEN_EOF ? 1 : 0;
    }
    if (parser.current.kind == EXPR_TOKEN_IDENTIFIER && branch_name_is_direct_loadable(state, parser.current.text)) {
        operand->kind = BRANCH_COMPARE_OPERAND_NAME;
        rt_copy_string(operand->name, sizeof(operand->name), parser.current.text);
        expr_next(&parser);
        return parser.current.kind == EXPR_TOKEN_EOF ? 1 : 0;
    }
    return 0;
}

static int emit_branch_load_operand_to_register(BackendState *state, const BranchCompareOperand *operand, const char *reg) {
    if (operand->kind == BRANCH_COMPARE_OPERAND_IMMEDIATE) {
        return emit_load_immediate_register(state, reg, operand->value);
    }
    if (operand->kind == BRANCH_COMPARE_OPERAND_NAME) {
        return emit_load_name_into_register(state, operand->name, reg);
    }
    return -1;
}

static int emit_x86_cmp_immediate_rax(BackendState *state, long long value) {
    unsigned long long magnitude;
    char digits[32];
    char line[96];

    if (value < -2147483648LL || value > 2147483647LL) {
        if (emit_load_immediate_register(state, "%rcx", value) != 0) {
            return -1;
        }
        return emit_instruction(state, "cmpq %rcx, %rax");
    }

    if (value < 0) {
        magnitude = (unsigned long long)(-(value + 1LL)) + 1ULL;
        rt_unsigned_to_string(magnitude, digits, sizeof(digits));
        rt_copy_string(line, sizeof(line), "cmpq $-");
    } else {
        magnitude = (unsigned long long)value;
        rt_unsigned_to_string(magnitude, digits, sizeof(digits));
        rt_copy_string(line, sizeof(line), "cmpq $");
    }
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", %rax");
    return emit_instruction(state, line);
}

static int emit_x86_branch_false_simple_compare(BackendState *state,
                                                const char *lhs,
                                                const char *rhs,
                                                const char *mnemonic,
                                                const char *label) {
    BranchCompareOperand lhs_operand;
    BranchCompareOperand rhs_operand;
    int lhs_simple;
    int rhs_simple;

    if (backend_is_aarch64(state)) {
        return 0;
    }
    lhs_simple = branch_parse_simple_operand(state, lhs, &lhs_operand);
    rhs_simple = branch_parse_simple_operand(state, rhs, &rhs_operand);
    if (lhs_simple < 0 || rhs_simple < 0) {
        return -1;
    }
    if (!lhs_simple || !rhs_simple) {
        return 0;
    }

    if (rhs_operand.kind == BRANCH_COMPARE_OPERAND_NAME &&
        emit_branch_load_operand_to_register(state, &rhs_operand, "%rcx") != 0) {
        return -1;
    }
    if (emit_branch_load_operand_to_register(state, &lhs_operand, "%rax") != 0) {
        return -1;
    }
    if (rhs_operand.kind == BRANCH_COMPARE_OPERAND_IMMEDIATE) {
        if (emit_x86_cmp_immediate_rax(state, rhs_operand.value) != 0) {
            return -1;
        }
    } else if (emit_instruction(state, "cmpq %rcx, %rax") != 0) {
        return -1;
    }
    if (lhs_operand.kind == BRANCH_COMPARE_OPERAND_NAME) {
        int lhs_local = find_local(state, lhs_operand.name);
        int seeded = backend_seed_block_cache_from_register(state, lhs_local, "%rax");
        if (seeded < 0) {
            return -1;
        }
        if (seeded == 0 && rhs_operand.kind == BRANCH_COMPARE_OPERAND_NAME) {
            int rhs_local = find_local(state, rhs_operand.name);
            if (backend_seed_block_cache_from_register(state, rhs_local, "%rcx") < 0) {
                return -1;
            }
        }
    } else if (rhs_operand.kind == BRANCH_COMPARE_OPERAND_NAME) {
        int rhs_local = find_local(state, rhs_operand.name);
        if (backend_seed_block_cache_from_register(state, rhs_local, "%rcx") < 0) {
            return -1;
        }
    }
    return emit_jump_to_label(state, mnemonic, label) == 0 ? 1 : -1;
}

static const char *branch_false_jump_for_compare(BackendState *state, const char *op, int use_unsigned) {
    if (backend_is_aarch64(state)) {
        if (names_equal(op, "==")) return "b.ne";
        if (names_equal(op, "!=")) return "b.eq";
        if (use_unsigned && names_equal(op, "<")) return "b.hs";
        if (use_unsigned && names_equal(op, "<=")) return "b.hi";
        if (use_unsigned && names_equal(op, ">")) return "b.ls";
        if (use_unsigned && names_equal(op, ">=")) return "b.lo";
        if (names_equal(op, "<")) return "b.ge";
        if (names_equal(op, "<=")) return "b.gt";
        if (names_equal(op, ">")) return "b.le";
        if (names_equal(op, ">=")) return "b.lt";
    } else {
        if (names_equal(op, "==")) return "jne";
        if (names_equal(op, "!=")) return "je";
        if (use_unsigned && names_equal(op, "<")) return "jae";
        if (use_unsigned && names_equal(op, "<=")) return "ja";
        if (use_unsigned && names_equal(op, ">")) return "jbe";
        if (use_unsigned && names_equal(op, ">=")) return "jb";
        if (names_equal(op, "<")) return "jge";
        if (names_equal(op, "<=")) return "jg";
        if (names_equal(op, ">")) return "jle";
        if (names_equal(op, ">=")) return "jl";
    }
    return 0;
}

static int emit_branch_false_fallback(BackendState *state, const char *expr, const char *label) {
    const char *mnemonic = backend_is_aarch64(state) ? "b.eq" : "je";

    if (branch_expr_is_double(state, expr)) {
        char truth_expr[COMPILER_IR_LINE_CAPACITY];

        if (rt_strlen(expr) + 10U >= sizeof(truth_expr)) {
            backend_set_error(state->backend, "branch condition too large for double truthiness");
            return -1;
        }
        rt_copy_string(truth_expr, sizeof(truth_expr), "(");
        rt_copy_string(truth_expr + rt_strlen(truth_expr), sizeof(truth_expr) - rt_strlen(truth_expr), expr);
        rt_copy_string(truth_expr + rt_strlen(truth_expr), sizeof(truth_expr) - rt_strlen(truth_expr), ") != 0.0");
        return emit_expression(state, truth_expr) == 0 &&
               emit_cmp_zero(state) == 0 &&
               emit_jump_to_label(state, mnemonic, label) == 0 ? 0 : -1;
    }

    return emit_expression(state, expr) == 0 &&
           emit_cmp_zero(state) == 0 &&
           emit_jump_to_label(state, mnemonic, label) == 0 ? 0 : -1;
}

static int emit_branch_false_compare(BackendState *state, const char *expr, const char *label) {
    const char *start = expr;
    const char *end = expr + rt_strlen(expr);
    const char *op = 0;
    const char *op_at;
    char lhs[COMPILER_IR_LINE_CAPACITY];
    char rhs[COMPILER_IR_LINE_CAPACITY];
    int use_unsigned;
    const char *mnemonic;

    range_trim_outer_parens(&start, &end);
    op_at = range_find_single_comparison(start, end, &op);
    if (op_at == 0 || op == 0) {
        return emit_branch_false_fallback(state, expr, label);
    }
    if (range_copy_trimmed(lhs, sizeof(lhs), start, op_at) != 0 ||
        range_copy_trimmed(rhs, sizeof(rhs), op_at + rt_strlen(op), end) != 0) {
        backend_set_error(state->backend, "branch condition too large for backend");
        return -1;
    }
    if (lhs[0] == '\0' || rhs[0] == '\0') {
        return emit_branch_false_fallback(state, expr, label);
    }
    if (branch_expr_is_double(state, lhs) || branch_expr_is_double(state, rhs)) {
        return emit_branch_false_fallback(state, expr, label);
    }
    {
        const char *lhs_start = lhs;
        const char *lhs_end = lhs + rt_strlen(lhs);
        const char *rhs_start = rhs;
        const char *rhs_end = rhs + rt_strlen(rhs);
        range_trim_outer_parens(&lhs_start, &lhs_end);
        range_trim_outer_parens(&rhs_start, &rhs_end);
        if (branch_range_has_fallback_operator(lhs_start, lhs_end) ||
            branch_range_has_fallback_operator(rhs_start, rhs_end)) {
            return emit_branch_false_fallback(state, expr, label);
        }
    }

    mnemonic = branch_false_jump_for_compare(state, op, 0);
    if (mnemonic == 0) {
        return emit_branch_false_fallback(state, expr, label);
    }

    if ((names_equal(op, "==") || names_equal(op, "!=")) && branch_expr_is_zero_literal(rhs)) {
        return emit_expression(state, lhs) == 0 &&
               emit_cmp_zero(state) == 0 &&
               emit_jump_to_label(state, mnemonic, label) == 0 ? 0 : -1;
    }
    if ((names_equal(op, "==") || names_equal(op, "!=")) && branch_expr_is_zero_literal(lhs)) {
        return emit_expression(state, rhs) == 0 &&
               emit_cmp_zero(state) == 0 &&
               emit_jump_to_label(state, mnemonic, label) == 0 ? 0 : -1;
    }

    use_unsigned = expr_text_looks_unsigned(state, lhs) || expr_text_looks_unsigned(state, rhs);
    mnemonic = branch_false_jump_for_compare(state, op, use_unsigned);
    if (mnemonic == 0) {
        return emit_branch_false_fallback(state, expr, label);
    }

    {
        int simple_result = emit_x86_branch_false_simple_compare(state, lhs, rhs, mnemonic, label);
        if (simple_result != 0) {
            return simple_result < 0 ? -1 : 0;
        }
    }

    if (emit_expression(state, lhs) != 0 || emit_push_value(state) != 0 || emit_expression(state, rhs) != 0) {
        return -1;
    }
    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "mov x2, x0") != 0 ||
            emit_instruction(state, "ldr x1, [sp]") != 0 ||
            emit_instruction(state, "add sp, sp, #16") != 0 ||
            emit_instruction(state, "cmp x1, x2") != 0) {
            return -1;
        }
    } else if (emit_instruction(state, "movq %rax, %rcx") != 0 ||
               emit_pop_to_register(state, "%rax") != 0 ||
               emit_instruction(state, "cmpq %rcx, %rax") != 0) {
        return -1;
    }
    return emit_jump_to_label(state, mnemonic, label);
}

static int emit_branch_false_range(BackendState *state, const char *start, const char *end, const char *label) {
    const char *and_at;
    char expr[COMPILER_IR_LINE_CAPACITY];

    range_trim_outer_parens(&start, &end);
    if (start >= end) {
        backend_set_error(state->backend, "empty branch condition in backend");
        return -1;
    }
    if (branch_range_has_fallback_operator(start, end)) {
        if (range_copy_trimmed(expr, sizeof(expr), start, end) != 0) {
            backend_set_error(state->backend, "branch condition too large for backend");
            return -1;
        }
        return emit_branch_false_fallback(state, expr, label);
    }

    and_at = range_find_top_level_token(start, end, "&&");
    while (and_at != 0) {
        if (emit_branch_false_range(state, start, and_at, label) != 0) {
            return -1;
        }
        start = and_at + 2;
        range_trim_outer_parens(&start, &end);
        and_at = range_find_top_level_token(start, end, "&&");
    }

    if (range_copy_trimmed(expr, sizeof(expr), start, end) != 0) {
        backend_set_error(state->backend, "branch condition too large for backend");
        return -1;
    }
    return emit_branch_false_compare(state, expr, label);
}

int emit_branch_false(BackendState *state, const char *expr, const char *label) {
    const char *start = expr;
    const char *end = expr + rt_strlen(expr);

    return emit_branch_false_range(state, start, end, label);
}

