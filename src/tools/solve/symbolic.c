#if !defined(SOLVE_FROM_SOLVE_C)
#ifndef SOLVE_PRIVATE_CONTEXT
#define SOLVE_PRIVATE_CONTEXT 1
#endif
#include "../solve.c"
#else
typedef struct {
    const char *text;
    size_t pos;
    const SolveOptions *options;
    int error;
    const char *message;
} SolveSymParser;

typedef struct {
    char expr[SOLVE_EXPR_CAPACITY];
    char deriv[SOLVE_EXPR_CAPACITY];
    int constant;
    double value;
} SolveSymNode;

static void solve_sym_set_error(SolveSymParser *parser, const char *message) {
    if (!parser->error) {
        parser->error = 1;
        parser->message = message;
    }
}

static int solve_sym_copy(char *dst, size_t dst_size, const char *src) {
    if (rt_strlen(src) >= dst_size) return -1;
    rt_copy_string(dst, dst_size, src);
    return 0;
}

static int solve_sym_join3(char *out, size_t out_size, const char *a, const char *b, const char *c) {
    size_t used = 0U;
    out[0] = '\0';
    return solve_append_text(out, out_size, &used, a) == 0 && solve_append_text(out, out_size, &used, b) == 0 && solve_append_text(out, out_size, &used, c) == 0 ? 0 : -1;
}

static int solve_sym_binary(char *out, size_t out_size, const char *left, const char *op, const char *right) {
    size_t used = 0U;
    out[0] = '\0';
    if (solve_append_char(out, out_size, &used, '(') != 0) return -1;
    if (solve_append_text(out, out_size, &used, left) != 0) return -1;
    if (solve_append_text(out, out_size, &used, op) != 0) return -1;
    if (solve_append_text(out, out_size, &used, right) != 0) return -1;
    return solve_append_char(out, out_size, &used, ')');
}

static int solve_sym_function(char *out, size_t out_size, const char *name, const char *arg) {
    size_t used = 0U;
    out[0] = '\0';
    if (solve_append_text(out, out_size, &used, name) != 0) return -1;
    if (solve_append_char(out, out_size, &used, '(') != 0) return -1;
    if (solve_append_text(out, out_size, &used, arg) != 0) return -1;
    return solve_append_char(out, out_size, &used, ')');
}

static int solve_sym_is_zero(const char *text);
static int solve_sym_is_one(const char *text);

static int solve_sym_is_wrapped_negative(const char *text) {
    size_t length = rt_strlen(text);
    return length >= 4U && text[0] == '(' && text[1] == '-' && text[length - 1U] == ')';
}

static int solve_sym_is_simple_atom(const char *text) {
    size_t pos = 0U;
    if ((text[pos] >= '0' && text[pos] <= '9') || text[pos] == '.') {
        while ((text[pos] >= '0' && text[pos] <= '9') || text[pos] == '.') pos += 1U;
        return text[pos] == '\0';
    }
    if (!tool_ascii_is_identifier_start(text[pos])) return 0;
    pos += 1U;
    while (tool_ascii_is_identifier_char(text[pos])) pos += 1U;
    return text[pos] == '\0';
}

static int solve_sym_negate_text(char *out, size_t out_size, const char *text) {
    size_t length;
    if (solve_sym_is_zero(text)) return solve_sym_copy(out, out_size, "0");
    length = rt_strlen(text);
    if (solve_sym_is_wrapped_negative(text)) return solve_copy_range(out, out_size, text, 2U, length - 1U);
    if (length == 2U && text[0] == '-' && text[1] == '1') return solve_sym_copy(out, out_size, "1");
    if (solve_sym_is_simple_atom(text)) return solve_sym_join3(out, out_size, "-", text, "");
    return solve_sym_join3(out, out_size, "(-", text, ")");
}

static int solve_sym_is_negative_one(const char *text) {
    return rt_strcmp(text, "-1") == 0 || rt_strcmp(text, "(-1)") == 0;
}

static int solve_sym_text_equal(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

static void solve_sym_format_number(double value, char *out, size_t out_size) {
    long long nearest = value >= 0.0 ? (long long)(value + 0.5) : (long long)(value - 0.5);
    if (solve_abs(value - (double)nearest) <= 0.0000000001) {
        size_t length = 0U;
        out[0] = '\0';
        if (solve_append_signed_ll(out, out_size, &length, nearest) == 0) return;
    }
    solve_format_double(value, 10, out, out_size);
}

static void solve_sym_make_zero(SolveSymNode *node) {
    rt_copy_string(node->expr, sizeof(node->expr), "0");
    rt_copy_string(node->deriv, sizeof(node->deriv), "0");
    node->constant = 1;
    node->value = 0.0;
}

static int solve_sym_make_const(SolveSymNode *node, const char *text, double value) {
    if (solve_sym_copy(node->expr, sizeof(node->expr), text) != 0 || solve_sym_copy(node->deriv, sizeof(node->deriv), "0") != 0) return -1;
    node->constant = 1;
    node->value = value;
    return 0;
}

static int solve_sym_make_var(SolveSymNode *node, const char *name) {
    if (solve_sym_copy(node->expr, sizeof(node->expr), name) != 0 || solve_sym_copy(node->deriv, sizeof(node->deriv), "1") != 0) return -1;
    node->constant = 0;
    node->value = 0.0;
    return 0;
}

static int solve_sym_is_zero(const char *text) { return rt_strcmp(text, "0") == 0 || rt_strcmp(text, "(-0)") == 0; }
static int solve_sym_is_one(const char *text) { return rt_strcmp(text, "1") == 0 || rt_strcmp(text, "1.0000000000") == 0; }

static int solve_sym_parse_expr(SolveSymParser *parser, SolveSymNode *out);

static int solve_sym_binary_simplified(char *out, size_t out_size, const char *left, const char *op, const char *right) {
    if (rt_strcmp(op, " + ") == 0) {
        if (solve_sym_is_zero(left)) return solve_sym_copy(out, out_size, right);
        if (solve_sym_is_zero(right)) return solve_sym_copy(out, out_size, left);
    } else if (rt_strcmp(op, " - ") == 0) {
        if (solve_sym_text_equal(left, right)) return solve_sym_copy(out, out_size, "0");
        if (solve_sym_is_zero(right)) return solve_sym_copy(out, out_size, left);
        if (solve_sym_is_zero(left)) return solve_sym_negate_text(out, out_size, right);
    } else if (rt_strcmp(op, "*") == 0) {
        if (solve_sym_is_zero(left) || solve_sym_is_zero(right)) return solve_sym_copy(out, out_size, "0");
        if (solve_sym_is_one(left)) return solve_sym_copy(out, out_size, right);
        if (solve_sym_is_one(right)) return solve_sym_copy(out, out_size, left);
        if (solve_sym_is_negative_one(left)) return solve_sym_negate_text(out, out_size, right);
        if (solve_sym_is_negative_one(right)) return solve_sym_negate_text(out, out_size, left);
    } else if (rt_strcmp(op, "/") == 0) {
        if (solve_sym_is_zero(left)) return solve_sym_copy(out, out_size, "0");
        if (solve_sym_is_one(right)) return solve_sym_copy(out, out_size, left);
    } else if (rt_strcmp(op, "^") == 0) {
        if (solve_sym_is_zero(right)) return solve_sym_copy(out, out_size, "1");
        if (solve_sym_is_one(right)) return solve_sym_copy(out, out_size, left);
        if (solve_sym_is_zero(left)) return solve_sym_copy(out, out_size, "0");
    }
    return solve_sym_binary(out, out_size, left, op, right);
}

static int solve_sym_assign_binary(SolveSymNode *out, const SolveSymNode *left, const char *op, const SolveSymNode *right) {
    char expr[SOLVE_EXPR_CAPACITY];
    char deriv[SOLVE_EXPR_CAPACITY];
    if (solve_sym_binary_simplified(expr, sizeof(expr), left->expr, op, right->expr) != 0) return -1;
    if (rt_strcmp(op, " + ") == 0) {
        if (solve_sym_is_zero(left->deriv)) rt_copy_string(deriv, sizeof(deriv), right->deriv);
        else if (solve_sym_is_zero(right->deriv)) rt_copy_string(deriv, sizeof(deriv), left->deriv);
        else if (solve_sym_binary_simplified(deriv, sizeof(deriv), left->deriv, " + ", right->deriv) != 0) return -1;
    } else if (rt_strcmp(op, " - ") == 0) {
        if (solve_sym_is_zero(right->deriv)) rt_copy_string(deriv, sizeof(deriv), left->deriv);
        else if (solve_sym_binary_simplified(deriv, sizeof(deriv), left->deriv, " - ", right->deriv) != 0) return -1;
    } else if (rt_strcmp(op, "*") == 0) {
        char left_part[SOLVE_EXPR_CAPACITY];
        char right_part[SOLVE_EXPR_CAPACITY];
        if (solve_sym_is_zero(left->deriv) && solve_sym_is_zero(right->deriv)) rt_copy_string(deriv, sizeof(deriv), "0");
        else if (solve_sym_is_zero(left->deriv)) {
            if (solve_sym_binary_simplified(deriv, sizeof(deriv), left->expr, "*", right->deriv) != 0) return -1;
        } else if (solve_sym_is_zero(right->deriv)) {
            if (solve_sym_binary_simplified(deriv, sizeof(deriv), left->deriv, "*", right->expr) != 0) return -1;
        } else {
            if (solve_sym_binary_simplified(left_part, sizeof(left_part), left->deriv, "*", right->expr) != 0) return -1;
            if (solve_sym_binary_simplified(right_part, sizeof(right_part), left->expr, "*", right->deriv) != 0) return -1;
            if (solve_sym_binary_simplified(deriv, sizeof(deriv), left_part, " + ", right_part) != 0) return -1;
        }
    } else if (rt_strcmp(op, "/") == 0) {
        char left_part[SOLVE_EXPR_CAPACITY];
        char right_part[SOLVE_EXPR_CAPACITY];
        char numerator[SOLVE_EXPR_CAPACITY];
        char denom_power[SOLVE_EXPR_CAPACITY];
        if (solve_sym_is_zero(left->deriv) && solve_sym_is_zero(right->deriv)) rt_copy_string(deriv, sizeof(deriv), "0");
        else {
            if (solve_sym_binary_simplified(left_part, sizeof(left_part), left->deriv, "*", right->expr) != 0) return -1;
            if (solve_sym_binary_simplified(right_part, sizeof(right_part), left->expr, "*", right->deriv) != 0) return -1;
            if (solve_sym_binary_simplified(numerator, sizeof(numerator), left_part, " - ", right_part) != 0) return -1;
            if (solve_sym_binary_simplified(denom_power, sizeof(denom_power), right->expr, "^", "2") != 0) return -1;
            if (solve_sym_binary_simplified(deriv, sizeof(deriv), numerator, "/", denom_power) != 0) return -1;
        }
    } else return -1;
    if (solve_sym_copy(out->expr, sizeof(out->expr), expr) != 0 || solve_sym_copy(out->deriv, sizeof(out->deriv), deriv) != 0) return -1;
    out->constant = left->constant && right->constant;
    out->value = 0.0;
    return 0;
}

static int solve_sym_assign_power(SolveSymNode *out, const SolveSymNode *base, const SolveSymNode *exponent) {
    char expr[SOLVE_EXPR_CAPACITY];
    char deriv[SOLVE_EXPR_CAPACITY];
    if (solve_sym_binary_simplified(expr, sizeof(expr), base->expr, "^", exponent->expr) != 0) return -1;
    if (solve_sym_is_zero(base->deriv) && solve_sym_is_zero(exponent->deriv)) {
        rt_copy_string(deriv, sizeof(deriv), "0");
    } else if (exponent->constant) {
        char n_text[64];
        char minus_one[64];
        char power[SOLVE_EXPR_CAPACITY];
        char coeff_power[SOLVE_EXPR_CAPACITY];
        solve_sym_format_number(exponent->value, n_text, sizeof(n_text));
        solve_sym_format_number(exponent->value - 1.0, minus_one, sizeof(minus_one));
        if (solve_abs(exponent->value) <= 0.0000000001) rt_copy_string(deriv, sizeof(deriv), "0");
        else if (solve_sym_binary_simplified(power, sizeof(power), base->expr, "^", minus_one) != 0 || solve_sym_binary_simplified(coeff_power, sizeof(coeff_power), n_text, "*", power) != 0) return -1;
        else if (solve_sym_is_one(base->deriv)) rt_copy_string(deriv, sizeof(deriv), coeff_power);
        else if (solve_sym_binary_simplified(deriv, sizeof(deriv), coeff_power, "*", base->deriv) != 0) return -1;
    } else {
        char log_base[SOLVE_EXPR_CAPACITY];
        char exp_log[SOLVE_EXPR_CAPACITY];
        char base_ratio[SOLVE_EXPR_CAPACITY];
        char sum[SOLVE_EXPR_CAPACITY];
        if (solve_sym_function(log_base, sizeof(log_base), "log", base->expr) != 0) return -1;
        if (solve_sym_binary_simplified(exp_log, sizeof(exp_log), exponent->deriv, "*", log_base) != 0) return -1;
        if (solve_sym_binary_simplified(base_ratio, sizeof(base_ratio), base->deriv, "/", base->expr) != 0) return -1;
        if (solve_sym_binary_simplified(base_ratio, sizeof(base_ratio), exponent->expr, "*", base_ratio) != 0) return -1;
        if (solve_sym_binary_simplified(sum, sizeof(sum), exp_log, " + ", base_ratio) != 0) return -1;
        if (solve_sym_binary_simplified(deriv, sizeof(deriv), expr, "*", sum) != 0) return -1;
    }
    if (solve_sym_copy(out->expr, sizeof(out->expr), expr) != 0 || solve_sym_copy(out->deriv, sizeof(out->deriv), deriv) != 0) return -1;
    out->constant = base->constant && exponent->constant;
    out->value = out->constant ? solve_pow(base->value, exponent->value) : 0.0;
    return 0;
}

static int solve_sym_read_identifier(SolveSymParser *parser, char *name, size_t name_size) {
    size_t used = 0U;
    if (!tool_ascii_is_identifier_start(parser->text[parser->pos])) return -1;
    while (tool_ascii_is_identifier_char(parser->text[parser->pos])) {
        if (used + 1U >= name_size) { solve_sym_set_error(parser, "identifier too long"); return -1; }
        name[used++] = parser->text[parser->pos++];
    }
    name[used] = '\0';
    return 0;
}

static int solve_sym_parse_primary(SolveSymParser *parser, SolveSymNode *out) {
    char name[SOLVE_NAME_CAPACITY];
    solve_skip_text_spaces(parser->text, &parser->pos);
    if (parser->text[parser->pos] == '(') {
        parser->pos += 1U;
        if (solve_sym_parse_expr(parser, out) != 0) return -1;
        solve_skip_text_spaces(parser->text, &parser->pos);
        if (parser->text[parser->pos] != ')') { solve_sym_set_error(parser, "missing ')'"); return -1; }
        parser->pos += 1U;
        return 0;
    }
    if ((parser->text[parser->pos] >= '0' && parser->text[parser->pos] <= '9') || parser->text[parser->pos] == '.') {
        size_t start = parser->pos;
        double value;
        char text[128];
        if (solve_parse_double(parser->text, &parser->pos, &value) != 0 || solve_copy_range(text, sizeof(text), parser->text, start, parser->pos) != 0) return -1;
        return solve_sym_make_const(out, text, value);
    }
    if (!tool_ascii_is_identifier_start(parser->text[parser->pos])) { solve_sym_set_error(parser, "syntax error"); return -1; }
    if (solve_sym_read_identifier(parser, name, sizeof(name)) != 0) return -1;
    solve_skip_text_spaces(parser->text, &parser->pos);
    if (rt_strcmp(name, parser->options->var_name) == 0 && parser->text[parser->pos] != '(') return solve_sym_make_var(out, name);
    if ((rt_strcmp(name, "pi") == 0 || rt_strcmp(name, "e") == 0 || solve_is_param_name(parser->options, name)) && parser->text[parser->pos] != '(') return solve_sym_make_const(out, name, rt_strcmp(name, "pi") == 0 ? SOLVE_PI : (rt_strcmp(name, "e") == 0 ? SOLVE_E : 0.0));
    if (parser->text[parser->pos] == '(' && solve_is_known_function(name)) {
        SolveSymNode arg;
        char expr[SOLVE_EXPR_CAPACITY];
        char deriv[SOLVE_EXPR_CAPACITY];
        parser->pos += 1U;
        if (solve_sym_parse_expr(parser, &arg) != 0) return -1;
        solve_skip_text_spaces(parser->text, &parser->pos);
        if (parser->text[parser->pos] == ',') { solve_sym_set_error(parser, "symbolic derivative supports unary functions only"); return -1; }
        if (parser->text[parser->pos] != ')') { solve_sym_set_error(parser, "missing ')'"); return -1; }
        parser->pos += 1U;
        if (rt_strcmp(name, "e") == 0) rt_copy_string(name, sizeof(name), "exp");
        if (rt_strcmp(name, "ln") == 0 || rt_strcmp(name, "l") == 0) rt_copy_string(name, sizeof(name), "log");
        if (rt_strcmp(name, "s") == 0) rt_copy_string(name, sizeof(name), "sin");
        if (rt_strcmp(name, "c") == 0) rt_copy_string(name, sizeof(name), "cos");
        if (rt_strcmp(name, "t") == 0) rt_copy_string(name, sizeof(name), "tan");
        if (rt_strcmp(name, "q") == 0) rt_copy_string(name, sizeof(name), "sqrt");
        if (solve_sym_function(expr, sizeof(expr), name, arg.expr) != 0) return -1;
        if (solve_sym_is_zero(arg.deriv)) rt_copy_string(deriv, sizeof(deriv), "0");
        else if (rt_strcmp(name, "sin") == 0) { char inner[SOLVE_EXPR_CAPACITY]; if (solve_sym_function(inner, sizeof(inner), "cos", arg.expr) != 0 || solve_sym_binary_simplified(deriv, sizeof(deriv), inner, "*", arg.deriv) != 0) return -1; }
        else if (rt_strcmp(name, "cos") == 0) { char inner[SOLVE_EXPR_CAPACITY]; char neg[SOLVE_EXPR_CAPACITY]; if (solve_sym_function(inner, sizeof(inner), "sin", arg.expr) != 0 || solve_sym_negate_text(neg, sizeof(neg), inner) != 0 || solve_sym_binary_simplified(deriv, sizeof(deriv), neg, "*", arg.deriv) != 0) return -1; }
        else if (rt_strcmp(name, "tan") == 0) { char inner[SOLVE_EXPR_CAPACITY]; char denom[SOLVE_EXPR_CAPACITY]; if (solve_sym_function(inner, sizeof(inner), "cos", arg.expr) != 0 || solve_sym_binary_simplified(denom, sizeof(denom), inner, "^", "2") != 0 || solve_sym_binary_simplified(deriv, sizeof(deriv), arg.deriv, "/", denom) != 0) return -1; }
        else if (rt_strcmp(name, "asin") == 0) { char square[SOLVE_EXPR_CAPACITY]; char denom[SOLVE_EXPR_CAPACITY]; char root[SOLVE_EXPR_CAPACITY]; if (solve_sym_binary_simplified(square, sizeof(square), arg.expr, "^", "2") != 0 || solve_sym_binary_simplified(denom, sizeof(denom), "1", " - ", square) != 0 || solve_sym_function(root, sizeof(root), "sqrt", denom) != 0 || solve_sym_binary_simplified(deriv, sizeof(deriv), arg.deriv, "/", root) != 0) return -1; }
        else if (rt_strcmp(name, "acos") == 0) { char square[SOLVE_EXPR_CAPACITY]; char denom[SOLVE_EXPR_CAPACITY]; char root[SOLVE_EXPR_CAPACITY]; char neg[SOLVE_EXPR_CAPACITY]; if (solve_sym_binary_simplified(square, sizeof(square), arg.expr, "^", "2") != 0 || solve_sym_binary_simplified(denom, sizeof(denom), "1", " - ", square) != 0 || solve_sym_function(root, sizeof(root), "sqrt", denom) != 0 || solve_sym_negate_text(neg, sizeof(neg), arg.deriv) != 0 || solve_sym_binary_simplified(deriv, sizeof(deriv), neg, "/", root) != 0) return -1; }
        else if (rt_strcmp(name, "sinh") == 0) { char inner[SOLVE_EXPR_CAPACITY]; if (solve_sym_function(inner, sizeof(inner), "cosh", arg.expr) != 0 || solve_sym_binary_simplified(deriv, sizeof(deriv), inner, "*", arg.deriv) != 0) return -1; }
        else if (rt_strcmp(name, "cosh") == 0) { char inner[SOLVE_EXPR_CAPACITY]; if (solve_sym_function(inner, sizeof(inner), "sinh", arg.expr) != 0 || solve_sym_binary_simplified(deriv, sizeof(deriv), inner, "*", arg.deriv) != 0) return -1; }
        else if (rt_strcmp(name, "tanh") == 0) { char inner[SOLVE_EXPR_CAPACITY]; char denom[SOLVE_EXPR_CAPACITY]; if (solve_sym_function(inner, sizeof(inner), "cosh", arg.expr) != 0 || solve_sym_binary_simplified(denom, sizeof(denom), inner, "^", "2") != 0 || solve_sym_binary_simplified(deriv, sizeof(deriv), arg.deriv, "/", denom) != 0) return -1; }
        else if (rt_strcmp(name, "exp") == 0) { if (solve_sym_binary_simplified(deriv, sizeof(deriv), expr, "*", arg.deriv) != 0) return -1; }
        else if (rt_strcmp(name, "log") == 0) { if (solve_sym_binary_simplified(deriv, sizeof(deriv), arg.deriv, "/", arg.expr) != 0) return -1; }
        else if (rt_strcmp(name, "sqrt") == 0) { char denom[SOLVE_EXPR_CAPACITY]; if (solve_sym_join3(denom, sizeof(denom), "(2*", expr, ")") != 0 || solve_sym_binary_simplified(deriv, sizeof(deriv), arg.deriv, "/", denom) != 0) return -1; }
        else if (rt_strcmp(name, "atan") == 0 || rt_strcmp(name, "a") == 0) { char square[SOLVE_EXPR_CAPACITY]; char denom[SOLVE_EXPR_CAPACITY]; if (solve_sym_binary_simplified(square, sizeof(square), arg.expr, "^", "2") != 0 || solve_sym_binary_simplified(denom, sizeof(denom), "1", " + ", square) != 0 || solve_sym_binary_simplified(deriv, sizeof(deriv), arg.deriv, "/", denom) != 0) return -1; }
        else { solve_sym_set_error(parser, "symbolic derivative unsupported for this function"); return -1; }
        if (solve_sym_copy(out->expr, sizeof(out->expr), expr) != 0 || solve_sym_copy(out->deriv, sizeof(out->deriv), deriv) != 0) return -1;
        out->constant = arg.constant;
        out->value = 0.0;
        return 0;
    }
    solve_sym_set_error(parser, "unknown identifier");
    return -1;
}

static int solve_sym_parse_unary(SolveSymParser *parser, SolveSymNode *out) {
    solve_skip_text_spaces(parser->text, &parser->pos);
    if (parser->text[parser->pos] == '+') { parser->pos += 1U; return solve_sym_parse_unary(parser, out); }
    if (parser->text[parser->pos] == '-') {
        SolveSymNode inner;
        parser->pos += 1U;
        if (solve_sym_parse_unary(parser, &inner) != 0) return -1;
        if (solve_sym_negate_text(out->expr, sizeof(out->expr), inner.expr) != 0 || solve_sym_negate_text(out->deriv, sizeof(out->deriv), inner.deriv) != 0) return -1;
        out->constant = inner.constant;
        out->value = -inner.value;
        return 0;
    }
    return solve_sym_parse_primary(parser, out);
}

static int solve_sym_parse_power(SolveSymParser *parser, SolveSymNode *out) {
    SolveSymNode base;
    if (solve_sym_parse_unary(parser, &base) != 0) return -1;
    solve_skip_text_spaces(parser->text, &parser->pos);
    if (parser->text[parser->pos] == '^') {
        SolveSymNode exponent;
        parser->pos += 1U;
        if (solve_sym_parse_power(parser, &exponent) != 0 || solve_sym_assign_power(out, &base, &exponent) != 0) return -1;
        return 0;
    }
    *out = base;
    return 0;
}

static int solve_sym_parse_term(SolveSymParser *parser, SolveSymNode *out) {
    if (solve_sym_parse_power(parser, out) != 0) return -1;
    while (!parser->error) {
        char op;
        SolveSymNode right;
        char op_text[2];
        solve_skip_text_spaces(parser->text, &parser->pos);
        op = parser->text[parser->pos];
        if (op != '*' && op != '/') break;
        parser->pos += 1U;
        if (solve_sym_parse_power(parser, &right) != 0) return -1;
        op_text[0] = op; op_text[1] = '\0';
        if (solve_sym_assign_binary(out, out, op_text, &right) != 0) { solve_sym_set_error(parser, "symbolic derivative too large"); return -1; }
    }
    return 0;
}

static int solve_sym_parse_expr(SolveSymParser *parser, SolveSymNode *out) {
    if (solve_sym_parse_term(parser, out) != 0) return -1;
    while (!parser->error) {
        char op;
        SolveSymNode right;
        const char *op_text;
        solve_skip_text_spaces(parser->text, &parser->pos);
        op = parser->text[parser->pos];
        if (op != '+' && op != '-') break;
        parser->pos += 1U;
        if (solve_sym_parse_term(parser, &right) != 0) return -1;
        op_text = op == '+' ? " + " : " - ";
        if (solve_sym_assign_binary(out, out, op_text, &right) != 0) { solve_sym_set_error(parser, "symbolic derivative too large"); return -1; }
    }
    return 0;
}

static int solve_sym_copy_trimmed_unwrapped(char *dst, size_t dst_size, const char *src) {
    size_t start = 0U;
    size_t end = rt_strlen(src);
    while (tool_ascii_is_space(src[start])) start += 1U;
    while (end > start && tool_ascii_is_space(src[end - 1U])) end -= 1U;
    while (end > start + 1U && src[start] == '(' && src[end - 1U] == ')') {
        size_t i;
        int depth = 0;
        int wraps = 1;
        for (i = start; i < end; ++i) {
            if (src[i] == '(') depth += 1;
            else if (src[i] == ')') {
                depth -= 1;
                if (depth == 0 && i + 1U < end) wraps = 0;
            }
        }
        if (!wraps) break;
        start += 1U;
        end -= 1U;
        while (tool_ascii_is_space(src[start])) start += 1U;
        while (end > start && tool_ascii_is_space(src[end - 1U])) end -= 1U;
    }
    return solve_copy_range(dst, dst_size, src, start, end);
}

static int solve_sym_find_top_level_mul(const char *text, size_t *index_out) {
    size_t i;
    int depth = 0;
    for (i = 0U; text[i] != '\0'; ++i) {
        if (text[i] == '(') depth += 1;
        else if (text[i] == ')' && depth > 0) depth -= 1;
        else if (text[i] == '*' && depth == 0) {
            *index_out = i;
            return 0;
        }
    }
    return -1;
}

static int solve_sym_extract_exp_arg(const char *text, char *arg, size_t arg_size) {
    size_t i;
    int depth = 0;
    if (rt_strncmp(text, "exp(", 4U) != 0) return -1;
    for (i = 3U; text[i] != '\0'; ++i) {
        if (text[i] == '(') depth += 1;
        else if (text[i] == ')') {
            depth -= 1;
            if (depth == 0) return text[i + 1U] == '\0' ? solve_copy_range(arg, arg_size, text, 4U, i) : -1;
        }
    }
    return -1;
}

static int solve_sym_parse_exp_poly_factor(const char *expr, const SolveOptions *options, SolveRatPoly *poly_out, SolveRatPoly *exp_arg_out, int *exp_count_out) {
    char text[SOLVE_EXPR_CAPACITY];
    size_t mul_index;
    SolveRat zero;
    if (solve_sym_copy_trimmed_unwrapped(text, sizeof(text), expr) != 0 || solve_rat_make(0, 1, &zero) != 0) return -1;
    if (solve_sym_find_top_level_mul(text, &mul_index) == 0) {
        char left_text[SOLVE_EXPR_CAPACITY];
        char right_text[SOLVE_EXPR_CAPACITY];
        SolveRatPoly left_poly;
        SolveRatPoly right_poly;
        SolveRatPoly left_exp;
        SolveRatPoly right_exp;
        int left_count;
        int right_count;
        if (solve_copy_range(left_text, sizeof(left_text), text, 0U, mul_index) != 0 || solve_copy_range(right_text, sizeof(right_text), text, mul_index + 1U, rt_strlen(text)) != 0) return -1;
        if (solve_sym_parse_exp_poly_factor(left_text, options, &left_poly, &left_exp, &left_count) != 0 || solve_sym_parse_exp_poly_factor(right_text, options, &right_poly, &right_exp, &right_count) != 0) return -1;
        if (solve_rat_poly_mul(&left_poly, &right_poly, poly_out) != 0 || solve_rat_poly_add(&left_exp, &right_exp, 0, exp_arg_out) != 0) return -1;
        *exp_count_out = left_count + right_count;
        return 0;
    }
    {
        char arg[SOLVE_EXPR_CAPACITY];
        if (solve_sym_extract_exp_arg(text, arg, sizeof(arg)) == 0) {
            SolveRat one;
            if (solve_rat_make(1, 1, &one) != 0 || solve_parse_rat_text(arg, options->var_name, exp_arg_out) != 0 || solve_rat_poly_degree(exp_arg_out) > 1) return -1;
            *poly_out = solve_rat_poly_constant(one);
            *exp_count_out = 1;
            return 0;
        }
    }
    if (solve_parse_rat_text(text, options->var_name, poly_out) != 0) return -1;
    solve_rat_poly_zero(exp_arg_out);
    *exp_count_out = 0;
    return 0;
}

static int solve_sym_exp_poly_model(const char *expr, const SolveOptions *options, SolveRatPoly *poly_out, SolveRatPoly *exp_arg_out) {
    int exp_count;
    if (solve_sym_parse_exp_poly_factor(expr, options, poly_out, exp_arg_out, &exp_count) != 0 || exp_count <= 0 || solve_rat_poly_degree(exp_arg_out) > 1) return -1;
    return 0;
}

static int solve_sym_exp_poly_derivative_factor(const char *expr, const SolveOptions *options, int order, SolveRatPoly *factor_out, SolveRatPoly *exp_arg_out) {
    SolveRatPoly current;
    SolveRatPoly exp_arg;
    SolveRat slope;
    int step;
    if (order < 0 || solve_sym_exp_poly_model(expr, options, &current, &exp_arg) != 0) return -1;
    slope = exp_arg.coeff[1];
    for (step = 0; step < order; ++step) {
        SolveRatPoly derivative;
        SolveRatPoly scaled;
        SolveRatPoly next;
        if (solve_rat_poly_derivative(&current, 1, &derivative) != 0 || solve_rat_poly_scale(&current, slope, &scaled) != 0 || solve_rat_poly_add(&derivative, &scaled, 0, &next) != 0) return -1;
        current = next;
    }
    *factor_out = current;
    *exp_arg_out = exp_arg;
    return 0;
}

static int solve_sym_exp_poly_derivative_text(const char *expr, const SolveOptions *options, int order, char *out, size_t out_size) {
    SolveRatPoly factor;
    SolveRatPoly exp_arg;
    char factor_text[SOLVE_EXPR_CAPACITY];
    char safe_factor_text[SOLVE_EXPR_CAPACITY];
    char exp_text[SOLVE_EXPR_CAPACITY];
    size_t used = 0U;
    if (solve_sym_exp_poly_derivative_factor(expr, options, order, &factor, &exp_arg) != 0) return -1;
    if (solve_rat_poly_degree(&factor) < 0) return solve_sym_copy(out, out_size, "0");
    if (solve_rat_poly_format(&exp_arg, options->var_name, exp_text, sizeof(exp_text)) != 0 || solve_rat_poly_format(&factor, options->var_name, factor_text, sizeof(factor_text)) != 0) return -1;
    if (factor_text[0] == '-') {
        size_t factor_used = 0U;
        safe_factor_text[0] = '\0';
        if (solve_append_text(safe_factor_text, sizeof(safe_factor_text), &factor_used, "0 - ") != 0 || solve_append_text(safe_factor_text, sizeof(safe_factor_text), &factor_used, factor_text + 1) != 0) return -1;
    } else {
        rt_copy_string(safe_factor_text, sizeof(safe_factor_text), factor_text);
    }
    out[0] = '\0';
    if (solve_append_text(out, out_size, &used, "exp(") != 0 || solve_append_text(out, out_size, &used, exp_text) != 0 || solve_append_char(out, out_size, &used, ')') != 0) return -1;
    if (solve_rat_poly_degree(&factor) == 0 && factor.coeff[0].num == 1 && factor.coeff[0].den == 1) return 0;
    if (solve_append_text(out, out_size, &used, "*(") != 0 || solve_append_text(out, out_size, &used, safe_factor_text) != 0 || solve_append_char(out, out_size, &used, ')') != 0) return -1;
    return 0;
}

static int solve_symbolic_derivative_text(const char *expr, const SolveOptions *options, int order, char *out, size_t out_size) {
    char current[SOLVE_EXPR_CAPACITY];
    int i;
    if (rt_strlen(expr) >= sizeof(current)) return -1;
    rt_copy_string(current, sizeof(current), expr);
    if (order == 0) return solve_sym_copy(out, out_size, current);
    if (solve_sym_exp_poly_derivative_text(expr, options, order, out, out_size) == 0) return 0;
    for (i = 0; i < order; ++i) {
        SolveSymParser parser;
        SolveSymNode node;
        parser.text = current;
        parser.pos = 0U;
        parser.options = options;
        parser.error = 0;
        parser.message = 0;
        solve_sym_make_zero(&node);
        if (solve_sym_parse_expr(&parser, &node) != 0) return -1;
        solve_skip_text_spaces(parser.text, &parser.pos);
        if (parser.error || parser.text[parser.pos] != '\0') return -1;
        if (solve_sym_copy(current, sizeof(current), node.deriv) != 0) return -1;
    }
    return solve_sym_copy(out, out_size, current);
}

static int solve_symbolic_simplify_text(const char *expr, const SolveOptions *options, char *out, size_t out_size) {
    SolveSymParser parser;
    SolveSymNode node;
    char text[SOLVE_EXPR_CAPACITY];
    parser.text = expr;
    parser.pos = 0U;
    parser.options = options;
    parser.error = 0;
    parser.message = 0;
    solve_sym_make_zero(&node);
    if (solve_sym_parse_expr(&parser, &node) != 0) return -1;
    solve_skip_text_spaces(parser.text, &parser.pos);
    if (parser.error || parser.text[parser.pos] != '\0') return -1;
    if (solve_sym_copy_trimmed_unwrapped(text, sizeof(text), node.expr) != 0) return -1;
    return solve_sym_copy(out, out_size, text);
}

#endif
