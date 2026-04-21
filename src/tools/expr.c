#include <limits.h>

#include "bignum.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int is_string;
    Bignum int_value;
    const char *string_value;
} ExprValue;

typedef struct {
    int argc;
    char **argv;
    int index;
    int error;
} ExprParser;

static ExprValue make_int_value(long long value) {
    ExprValue result;
    result.is_string = 0;
    bn_from_int(&result.int_value, value);
    result.string_value = 0;
    return result;
}

static ExprValue make_bignum_value(const Bignum *value) {
    ExprValue result;
    result.is_string = 0;
    result.int_value = *value;
    result.string_value = 0;
    return result;
}

static ExprValue make_string_value(const char *value) {
    ExprValue result;
    result.is_string = 1;
    bn_zero(&result.int_value);
    result.string_value = (value != 0) ? value : "";
    return result;
}

static int parse_signed_value(const char *text, long long *value_out) {
    unsigned long long value = 0ULL;
    int negative = 0;
    unsigned long long limit;

    if (text == 0 || text[0] == '\0' || value_out == 0) {
        return -1;
    }

    if (*text == '-') {
        negative = 1;
        text += 1;
    } else if (*text == '+') {
        text += 1;
    }

    if (*text == '\0') {
        return -1;
    }

    while (*text != '\0') {
        unsigned int digit;

        if (*text < '0' || *text > '9') {
            return -1;
        }
        digit = (unsigned int)(*text - '0');
        limit = negative ? ((unsigned long long)LLONG_MAX + 1ULL) : (unsigned long long)LLONG_MAX;
        if (value > (limit / 10ULL) ||
            (value == (limit / 10ULL) && (unsigned long long)digit > (limit % 10ULL))) {
            return -1;
        }
        value = (value * 10ULL) + (unsigned long long)digit;
        text += 1;
    }

    if (negative) {
        if (value == ((unsigned long long)LLONG_MAX + 1ULL)) {
            *value_out = LLONG_MIN;
        } else {
            *value_out = -(long long)value;
        }
    } else {
        *value_out = (long long)value;
    }
    return 0;
}

static int compare_strings(const char *lhs, const char *rhs) {
    return rt_strcmp((lhs != 0) ? lhs : "", (rhs != 0) ? rhs : "");
}

static int value_truthy(const ExprValue *value) {
    if (!value->is_string) {
        return !bn_is_zero(&value->int_value);
    }

    return value->string_value[0] != '\0' &&
           !(value->string_value[0] == '0' && value->string_value[1] == '\0');
}

static const char *value_as_string(const ExprValue *value, char *buffer, size_t buffer_size) {
    if (value->is_string) {
        return value->string_value;
    }
    if (bn_to_string(&value->int_value, buffer, buffer_size) != 0 && buffer_size > 0) {
        buffer[0] = '\0';
    }
    return buffer;
}

static int value_as_bignum(const ExprValue *value, Bignum *out) {
    if (!value->is_string) {
        *out = value->int_value;
        return 0;
    }
    return bn_from_string(out, value->string_value);
}

static ExprValue expr_parse_or(ExprParser *parser);

static ExprValue expr_parse_primary(ExprParser *parser) {
    ExprValue result;

    if (parser->index >= parser->argc) {
        parser->error = 1;
        return make_int_value(0);
    }

    if (rt_strcmp(parser->argv[parser->index], "(") == 0) {
        parser->index += 1;
        result = expr_parse_or(parser);
        if (parser->index >= parser->argc || rt_strcmp(parser->argv[parser->index], ")") != 0) {
            parser->error = 1;
            return make_int_value(0);
        }
        parser->index += 1;
        return result;
    }

    if (rt_strcmp(parser->argv[parser->index], "length") == 0) {
        if (parser->index + 1 >= parser->argc) {
            parser->error = 1;
            return make_int_value(0);
        }
        parser->index += 1;
        result = make_int_value((long long)rt_strlen(parser->argv[parser->index]));
        parser->index += 1;
        return result;
    }

    if (rt_strcmp(parser->argv[parser->index], "index") == 0) {
        if (parser->index + 2 >= parser->argc) {
            parser->error = 1;
            return make_int_value(0);
        }
        {
            const char *text = parser->argv[parser->index + 1];
            const char *chars = parser->argv[parser->index + 2];
            size_t i;
            for (i = 0; text[i] != '\0'; ++i) {
                size_t j = 0;
                while (chars[j] != '\0') {
                    if (text[i] == chars[j]) {
                        parser->index += 3;
                        return make_int_value((long long)(i + 1));
                    }
                    j += 1;
                }
            }
        }
        parser->index += 3;
        return make_int_value(0);
    }

    if (rt_strcmp(parser->argv[parser->index], "substr") == 0) {
        if (parser->index + 3 >= parser->argc) {
            parser->error = 1;
            return make_int_value(0);
        }
        {
            static char substring[256];
            const char *text = parser->argv[parser->index + 1];
            long long start = 0;
            long long length = 0;
            size_t text_len = rt_strlen(text);
            size_t output_len = 0;

            if (parse_signed_value(parser->argv[parser->index + 2], &start) != 0 ||
                parse_signed_value(parser->argv[parser->index + 3], &length) != 0) {
                parser->error = 1;
                return make_int_value(0);
            }

            if (start < 1) {
                start = 1;
            }
            if (length < 0) {
                length = 0;
            }

            substring[0] = '\0';
            if ((size_t)(start - 1) < text_len) {
                const char *from = text + (start - 1);
                while (*from != '\0' && output_len < (size_t)length && output_len + 1 < sizeof(substring)) {
                    substring[output_len++] = *from++;
                }
                substring[output_len] = '\0';
            }

            parser->index += 4;
            return make_string_value(substring);
        }
    }

    result = make_string_value(parser->argv[parser->index]);
    parser->index += 1;
    return result;
}

static ExprValue expr_parse_mul(ExprParser *parser) {
    ExprValue value = expr_parse_primary(parser);

    while (!parser->error && parser->index < parser->argc) {
        const char *op = parser->argv[parser->index];
        Bignum lhs;
        Bignum rhs;
        Bignum result;
        Bignum remainder;

        if (rt_strcmp(op, "*") != 0 && rt_strcmp(op, "/") != 0 && rt_strcmp(op, "%") != 0) {
            break;
        }

        parser->index += 1;
        {
            ExprValue right = expr_parse_primary(parser);
            if (value_as_bignum(&value, &lhs) != 0 || value_as_bignum(&right, &rhs) != 0) {
                parser->error = 1;
                return make_int_value(0);
            }
        }

        if ((rt_strcmp(op, "/") == 0 || rt_strcmp(op, "%") == 0) && bn_is_zero(&rhs)) {
            tool_write_error("expr", "division by zero", 0);
            parser->error = 1;
            return make_int_value(0);
        }

        if (rt_strcmp(op, "*") == 0) {
            if (bn_multiply(&lhs, &rhs, &result) != 0) {
                tool_write_error("expr", "numeric overflow", 0);
                parser->error = 1;
                return make_int_value(0);
            }
            value = make_bignum_value(&result);
        } else if (rt_strcmp(op, "/") == 0) {
            bn_zero(&remainder);
            if (bn_divide(&lhs, &rhs, &result, &remainder) != 0) {
                tool_write_error("expr", "numeric overflow", 0);
                parser->error = 1;
                return make_int_value(0);
            }
            value = make_bignum_value(&result);
        } else {
            bn_zero(&remainder);
            if (bn_divide(&lhs, &rhs, &result, &remainder) != 0) {
                tool_write_error("expr", "numeric overflow", 0);
                parser->error = 1;
                return make_int_value(0);
            }
            value = make_bignum_value(&remainder);
        }
    }

    return value;
}

static ExprValue expr_parse_add(ExprParser *parser) {
    ExprValue value = expr_parse_mul(parser);

    while (!parser->error && parser->index < parser->argc) {
        const char *op = parser->argv[parser->index];
        Bignum lhs;
        Bignum rhs;
        Bignum result;

        if (rt_strcmp(op, "+") != 0 && rt_strcmp(op, "-") != 0) {
            break;
        }

        parser->index += 1;
        {
            ExprValue right = expr_parse_mul(parser);
            if (value_as_bignum(&value, &lhs) != 0 || value_as_bignum(&right, &rhs) != 0) {
                parser->error = 1;
                return make_int_value(0);
            }
        }

        if (rt_strcmp(op, "+") == 0) {
            if (bn_add(&lhs, &rhs, &result) != 0) {
                tool_write_error("expr", "numeric overflow", 0);
                parser->error = 1;
                return make_int_value(0);
            }
        } else {
            if (bn_subtract(&lhs, &rhs, &result) != 0) {
                tool_write_error("expr", "numeric overflow", 0);
                parser->error = 1;
                return make_int_value(0);
            }
        }
        value = make_bignum_value(&result);
    }

    return value;
}

static ExprValue expr_parse_cmp(ExprParser *parser) {
    ExprValue value = expr_parse_add(parser);

    while (!parser->error && parser->index < parser->argc) {
        const char *op = parser->argv[parser->index];
        ExprValue right;
        Bignum lhs_num;
        Bignum rhs_num;
        int comparison;

        if (rt_strcmp(op, "=") != 0 && rt_strcmp(op, "!=") != 0 &&
            rt_strcmp(op, "<") != 0 && rt_strcmp(op, "<=") != 0 &&
            rt_strcmp(op, ">") != 0 && rt_strcmp(op, ">=") != 0) {
            break;
        }

        parser->index += 1;
        right = expr_parse_add(parser);

        if (rt_strcmp(op, "=") == 0) {
            char lhs_buffer[2048];
            char rhs_buffer[2048];
            comparison = compare_strings(value_as_string(&value, lhs_buffer, sizeof(lhs_buffer)),
                                         value_as_string(&right, rhs_buffer, sizeof(rhs_buffer)));
            value = make_int_value(comparison == 0 ? 1 : 0);
        } else if (rt_strcmp(op, "!=") == 0) {
            char lhs_buffer[2048];
            char rhs_buffer[2048];
            comparison = compare_strings(value_as_string(&value, lhs_buffer, sizeof(lhs_buffer)),
                                         value_as_string(&right, rhs_buffer, sizeof(rhs_buffer)));
            value = make_int_value(comparison != 0 ? 1 : 0);
        } else {
            if (value_as_bignum(&value, &lhs_num) == 0 && value_as_bignum(&right, &rhs_num) == 0) {
                comparison = bn_compare(&lhs_num, &rhs_num);
            } else {
                char lhs_buffer[2048];
                char rhs_buffer[2048];
                comparison = compare_strings(value_as_string(&value, lhs_buffer, sizeof(lhs_buffer)),
                                             value_as_string(&right, rhs_buffer, sizeof(rhs_buffer)));
            }

            if (rt_strcmp(op, "<") == 0) {
                value = make_int_value(comparison < 0 ? 1 : 0);
            } else if (rt_strcmp(op, "<=") == 0) {
                value = make_int_value(comparison <= 0 ? 1 : 0);
            } else if (rt_strcmp(op, ">") == 0) {
                value = make_int_value(comparison > 0 ? 1 : 0);
            } else {
                value = make_int_value(comparison >= 0 ? 1 : 0);
            }
        }
    }

    return value;
}

static ExprValue expr_parse_and(ExprParser *parser) {
    ExprValue value = expr_parse_cmp(parser);

    while (!parser->error && parser->index < parser->argc && rt_strcmp(parser->argv[parser->index], "&") == 0) {
        ExprValue right;
        parser->index += 1;
        right = expr_parse_cmp(parser);
        if (!(value_truthy(&value) && value_truthy(&right))) {
            value = make_int_value(0);
        }
    }

    return value;
}

static ExprValue expr_parse_or(ExprParser *parser) {
    ExprValue value = expr_parse_and(parser);

    while (!parser->error && parser->index < parser->argc && rt_strcmp(parser->argv[parser->index], "|") == 0) {
        ExprValue right;
        parser->index += 1;
        right = expr_parse_and(parser);
        if (!value_truthy(&value)) {
            value = right;
        }
    }

    return value;
}

static int write_value(const ExprValue *value) {
    if (value->is_string) {
        return rt_write_line(1, value->string_value);
    }
    {
        char buffer[2048];
        if (bn_to_string(&value->int_value, buffer, sizeof(buffer)) != 0) {
            return -1;
        }
        return rt_write_line(1, buffer);
    }
}

int main(int argc, char **argv) {
    ExprParser parser;
    ExprValue result;

    if (argc < 2) {
        tool_write_usage(argv[0], "EXPRESSION");
        return 2;
    }

    parser.argc = argc;
    parser.argv = argv;
    parser.index = 1;
    parser.error = 0;

    result = expr_parse_or(&parser);
    if (parser.error || parser.index != argc) {
        tool_write_error("expr", "syntax error", 0);
        return 2;
    }

    if (write_value(&result) != 0) {
        return 2;
    }

    return value_truthy(&result) ? 0 : 1;
}
