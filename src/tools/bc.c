#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define BC_INPUT_CAPACITY 16384
#define BC_MAX_SCALE 18
#define BC_MAX_VARS 128
#define BC_NAME_CAPACITY 32
#define BC_OUTPUT_CAPACITY 256

typedef struct {
    long long mantissa;
    int scale;
} BcValue;

typedef struct {
    char name[BC_NAME_CAPACITY];
    BcValue value;
} BcVar;

typedef struct {
    BcVar vars[BC_MAX_VARS];
    size_t var_count;
} BcEnv;

typedef enum {
    BC_TOKEN_EOF = 0,
    BC_TOKEN_NUMBER,
    BC_TOKEN_IDENT,
    BC_TOKEN_NEWLINE,
    BC_TOKEN_SEMICOLON,
    BC_TOKEN_LPAREN,
    BC_TOKEN_RPAREN,
    BC_TOKEN_LBRACE,
    BC_TOKEN_RBRACE,
    BC_TOKEN_COMMA,
    BC_TOKEN_PLUS,
    BC_TOKEN_MINUS,
    BC_TOKEN_STAR,
    BC_TOKEN_SLASH,
    BC_TOKEN_PERCENT,
    BC_TOKEN_CARET,
    BC_TOKEN_ASSIGN,
    BC_TOKEN_EQ,
    BC_TOKEN_NE,
    BC_TOKEN_LT,
    BC_TOKEN_LE,
    BC_TOKEN_GT,
    BC_TOKEN_GE,
    BC_TOKEN_AND,
    BC_TOKEN_OR,
    BC_TOKEN_NOT,
    BC_TOKEN_IF,
    BC_TOKEN_ELSE,
    BC_TOKEN_WHILE,
    BC_TOKEN_FOR,
    BC_TOKEN_PRINT
} BcTokenType;

typedef struct {
    BcTokenType type;
    BcValue number;
    char text[BC_NAME_CAPACITY];
} BcToken;

typedef struct {
    const char *text;
    size_t pos;
    int error;
    const char *message;
    BcEnv *env;
    BcToken token;
    int has_token;
} BcParser;

static BcValue bc_make_value(long long mantissa, int scale) {
    BcValue value;

    if (scale < 0) {
        scale = 0;
    }
    if (scale > BC_MAX_SCALE) {
        scale = BC_MAX_SCALE;
    }

    value.mantissa = mantissa;
    value.scale = scale;
    return value;
}

static BcValue bc_make_int(long long value) {
    return bc_make_value(value, 0);
}

static void bc_set_error(BcParser *parser, const char *message) {
    if (!parser->error) {
        parser->error = 1;
        parser->message = message;
    }
}

static unsigned long long bc_unsigned_magnitude(long long value) {
    if (value < 0) {
        return (unsigned long long)(-(value + 1)) + 1ULL;
    }
    return (unsigned long long)value;
}

static unsigned __int128 bc_pow10_u128(int exponent) {
    unsigned __int128 value = 1;

    while (exponent > 0) {
        value *= 10U;
        exponent -= 1;
    }

    return value;
}

static int bc_i128_to_ll(BcParser *parser, __int128 value, long long *out) {
    const __int128 max_value = (((__int128)1) << 63) - 1;
    const __int128 min_value = -((__int128)1 << 63);

    if (value > max_value || value < min_value) {
        bc_set_error(parser, "numeric overflow");
        return -1;
    }

    *out = (long long)value;
    return 0;
}

static int bc_value_truth(BcValue value) {
    return value.mantissa != 0;
}

static BcValue bc_normalize_value(BcValue value) {
    while (value.scale > 0 && (value.mantissa % 10LL) == 0) {
        value.mantissa /= 10LL;
        value.scale -= 1;
    }
    return value;
}

static BcValue bc_rescale(BcParser *parser, BcValue value, int target_scale) {
    __int128 adjusted;
    int delta;
    long long result = 0;

    if (parser->error) {
        return bc_make_int(0);
    }

    if (target_scale < 0) {
        target_scale = 0;
    }
    if (target_scale > BC_MAX_SCALE) {
        target_scale = BC_MAX_SCALE;
    }

    if (value.scale == target_scale) {
        return value;
    }

    if (value.mantissa == 0) {
        return bc_make_value(0, target_scale);
    }

    delta = target_scale - value.scale;
    adjusted = (__int128)value.mantissa;
    if (delta > 0) {
        adjusted *= (__int128)bc_pow10_u128(delta);
    } else if (delta < 0) {
        adjusted /= (__int128)bc_pow10_u128(-delta);
    }

    if (bc_i128_to_ll(parser, adjusted, &result) != 0) {
        return bc_make_int(0);
    }
    return bc_make_value(result, target_scale);
}

static int bc_get_scale_setting(BcEnv *env) {
    size_t i;

    for (i = 0; i < env->var_count; ++i) {
        if (rt_strcmp(env->vars[i].name, "scale") == 0) {
            BcValue value = env->vars[i].value;
            long long divisor = (long long)bc_pow10_u128(value.scale);

            if (value.scale > 0 && divisor != 0) {
                value.mantissa /= divisor;
            }
            if (value.mantissa < 0) {
                return 0;
            }
            if (value.mantissa > BC_MAX_SCALE) {
                return BC_MAX_SCALE;
            }
            return (int)value.mantissa;
        }
    }

    return 6;
}

static int bc_get_ibase_setting(BcEnv *env) {
    size_t i;

    for (i = 0; i < env->var_count; ++i) {
        if (rt_strcmp(env->vars[i].name, "ibase") == 0) {
            BcValue value = env->vars[i].value;
            long long divisor = (long long)bc_pow10_u128(value.scale);

            if (value.scale > 0 && divisor != 0) {
                value.mantissa /= divisor;
            }
            if (value.mantissa < 2) {
                return 10;
            }
            if (value.mantissa > 16) {
                return 16;
            }
            return (int)value.mantissa;
        }
    }

    return 10;
}

static int bc_get_obase_setting(BcEnv *env) {
    size_t i;

    for (i = 0; i < env->var_count; ++i) {
        if (rt_strcmp(env->vars[i].name, "obase") == 0) {
            BcValue value = env->vars[i].value;
            long long divisor = (long long)bc_pow10_u128(value.scale);

            if (value.scale > 0 && divisor != 0) {
                value.mantissa /= divisor;
            }
            if (value.mantissa < 2) {
                return 10;
            }
            if (value.mantissa > 16) {
                return 16;
            }
            return (int)value.mantissa;
        }
    }

    return 10;
}

static BcValue bc_add_values(BcParser *parser, BcValue left, BcValue right) {
    int scale = left.scale > right.scale ? left.scale : right.scale;
    BcValue a = bc_rescale(parser, left, scale);
    BcValue b = bc_rescale(parser, right, scale);
    long long result = 0;

    if (parser->error) {
        return bc_make_int(0);
    }
    if (bc_i128_to_ll(parser, (__int128)a.mantissa + (__int128)b.mantissa, &result) != 0) {
        return bc_make_int(0);
    }
    return bc_normalize_value(bc_make_value(result, scale));
}

static BcValue bc_sub_values(BcParser *parser, BcValue left, BcValue right) {
    int scale = left.scale > right.scale ? left.scale : right.scale;
    BcValue a = bc_rescale(parser, left, scale);
    BcValue b = bc_rescale(parser, right, scale);
    long long result = 0;

    if (parser->error) {
        return bc_make_int(0);
    }
    if (bc_i128_to_ll(parser, (__int128)a.mantissa - (__int128)b.mantissa, &result) != 0) {
        return bc_make_int(0);
    }
    return bc_normalize_value(bc_make_value(result, scale));
}

static BcValue bc_mul_values(BcParser *parser, BcValue left, BcValue right) {
    __int128 result = (__int128)left.mantissa * (__int128)right.mantissa;
    int scale = left.scale + right.scale;
    long long mantissa = 0;

    if (scale > BC_MAX_SCALE) {
        result /= (__int128)bc_pow10_u128(scale - BC_MAX_SCALE);
        scale = BC_MAX_SCALE;
    }

    if (bc_i128_to_ll(parser, result, &mantissa) != 0) {
        return bc_make_int(0);
    }
    return bc_normalize_value(bc_make_value(mantissa, scale));
}

static BcValue bc_div_values(BcParser *parser, BcValue left, BcValue right) {
    int scale;
    __int128 numerator;
    __int128 denominator;
    long long mantissa = 0;

    if (right.mantissa == 0) {
        bc_set_error(parser, "division by zero");
        return bc_make_int(0);
    }

    scale = bc_get_scale_setting(parser->env);
    if (left.scale > scale) {
        scale = left.scale;
    }
    if (right.scale > scale) {
        scale = right.scale;
    }
    if (scale > BC_MAX_SCALE) {
        scale = BC_MAX_SCALE;
    }

    numerator = (__int128)left.mantissa;
    denominator = (__int128)right.mantissa;
    if (scale + right.scale >= left.scale) {
        numerator *= (__int128)bc_pow10_u128(scale + right.scale - left.scale);
    } else {
        denominator *= (__int128)bc_pow10_u128(left.scale - scale - right.scale);
    }

    if (denominator == 0) {
        bc_set_error(parser, "division by zero");
        return bc_make_int(0);
    }

    if (bc_i128_to_ll(parser, numerator / denominator, &mantissa) != 0) {
        return bc_make_int(0);
    }
    return bc_make_value(mantissa, scale);
}

static BcValue bc_mod_values(BcParser *parser, BcValue left, BcValue right) {
    BcValue a = bc_rescale(parser, left, 0);
    BcValue b = bc_rescale(parser, right, 0);

    if (parser->error) {
        return bc_make_int(0);
    }
    if (b.mantissa == 0) {
        bc_set_error(parser, "division by zero");
        return bc_make_int(0);
    }
    return bc_normalize_value(bc_make_int(a.mantissa % b.mantissa));
}

static int bc_compare_values(BcParser *parser, BcValue left, BcValue right) {
    int scale = left.scale > right.scale ? left.scale : right.scale;
    BcValue a = bc_rescale(parser, left, scale);
    BcValue b = bc_rescale(parser, right, scale);

    if (parser->error) {
        return 0;
    }
    if (a.mantissa < b.mantissa) {
        return -1;
    }
    if (a.mantissa > b.mantissa) {
        return 1;
    }
    return 0;
}

static int bc_value_to_integer(BcParser *parser, BcValue value, long long *out) {
    BcValue integer_value = bc_rescale(parser, value, 0);

    if (parser->error) {
        return -1;
    }
    *out = integer_value.mantissa;
    return 0;
}

static BcValue bc_pow_values(BcParser *parser, BcValue base, BcValue exponent) {
    long long exp = 0;
    unsigned long long power;
    BcValue result = bc_make_int(1);
    BcValue factor = base;
    int negative = 0;

    if (exponent.scale > 0) {
        long long divisor = (long long)bc_pow10_u128(exponent.scale);

        if (divisor != 0 && (exponent.mantissa % divisor) != 0) {
            bc_set_error(parser, "non-integer exponent");
            return bc_make_int(0);
        }
    }
    if (bc_value_to_integer(parser, exponent, &exp) != 0) {
        return bc_make_int(0);
    }
    if (parser->error) {
        return bc_make_int(0);
    }

    if (exp < 0) {
        negative = 1;
        power = (unsigned long long)(-(exp + 1)) + 1ULL;
    } else {
        power = (unsigned long long)exp;
    }

    while (power > 0 && !parser->error) {
        if ((power & 1ULL) != 0ULL) {
            result = bc_mul_values(parser, result, factor);
        }
        power >>= 1U;
        if (power != 0) {
            factor = bc_mul_values(parser, factor, factor);
        }
    }

    if (negative && !parser->error) {
        result = bc_div_values(parser, bc_make_int(1), result);
    }
    return result;
}

static unsigned __int128 bc_isqrt_u128(unsigned __int128 value) {
    unsigned __int128 low = 0;
    unsigned __int128 high = value;
    unsigned __int128 best = 0;

    if (value == 0) {
        return 0;
    }
    if (high > (((unsigned __int128)1) << 64)) {
        high = ((unsigned __int128)1) << 64;
    }

    while (low <= high) {
        unsigned __int128 mid = low + ((high - low) / 2);
        unsigned __int128 square = mid * mid;

        if (square == value) {
            return mid;
        }
        if (square < value) {
            best = mid;
            low = mid + 1;
        } else {
            if (mid == 0) {
                break;
            }
            high = mid - 1;
        }
    }

    return best;
}

static BcValue bc_sqrt_value(BcParser *parser, BcValue value) {
    int target_scale;
    int exponent;
    unsigned __int128 magnitude;
    unsigned __int128 radicand;
    unsigned __int128 root;
    long long mantissa = 0;

    if (value.mantissa < 0) {
        bc_set_error(parser, "square root of negative value");
        return bc_make_int(0);
    }

    target_scale = bc_get_scale_setting(parser->env);
    if ((value.scale + 1) / 2 > target_scale) {
        target_scale = (value.scale + 1) / 2;
    }
    if (target_scale > BC_MAX_SCALE) {
        target_scale = BC_MAX_SCALE;
    }

    magnitude = (unsigned __int128)bc_unsigned_magnitude(value.mantissa);
    exponent = (target_scale * 2) - value.scale;
    if (exponent < 0) {
        exponent = 0;
    }
    radicand = magnitude * bc_pow10_u128(exponent);
    root = bc_isqrt_u128(radicand);

    if (bc_i128_to_ll(parser, (__int128)root, &mantissa) != 0) {
        return bc_make_int(0);
    }
    return bc_make_value(mantissa, target_scale);
}

static BcValue bc_length_value(BcValue value) {
    unsigned long long integer_part;
    long long mantissa = value.mantissa;
    int digits = 1;

    if (value.scale > 0) {
        mantissa /= (long long)bc_pow10_u128(value.scale);
    }
    integer_part = bc_unsigned_magnitude(mantissa);
    while (integer_part >= 10ULL) {
        integer_part /= 10ULL;
        digits += 1;
    }
    return bc_make_int(digits);
}

static BcValue bc_scale_value(BcValue value) {
    return bc_make_int(value.scale);
}

static int bc_is_name_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

static int bc_is_name_char(char ch) {
    return bc_is_name_start(ch) || (ch >= '0' && ch <= '9');
}

static int bc_hex_digit_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

static char bc_digit_char(int value) {
    static const char digits[] = "0123456789ABCDEF";

    if (value < 0 || value >= 16) {
        return '?';
    }
    return digits[value];
}

static size_t bc_find_var_index(BcEnv *env, const char *name) {
    size_t i;

    for (i = 0; i < env->var_count; ++i) {
        if (rt_strcmp(env->vars[i].name, name) == 0) {
            return i;
        }
    }

    return env->var_count;
}

static BcValue bc_get_var(BcEnv *env, const char *name) {
    size_t index = bc_find_var_index(env, name);

    if (index < env->var_count) {
        return env->vars[index].value;
    }
    return bc_make_int(0);
}

static void bc_store_var(BcParser *parser, const char *name, BcValue value) {
    size_t index;

    if (rt_strcmp(name, "scale") == 0) {
        long long ivalue = 0;

        if (bc_value_to_integer(parser, value, &ivalue) != 0) {
            return;
        }
        if (ivalue < 0 || ivalue > BC_MAX_SCALE) {
            bc_set_error(parser, "scale out of range");
            return;
        }
        value = bc_make_int(ivalue);
    } else if (rt_strcmp(name, "ibase") == 0 || rt_strcmp(name, "obase") == 0) {
        long long ivalue = 0;

        if (bc_value_to_integer(parser, value, &ivalue) != 0) {
            return;
        }
        if (ivalue < 2 || ivalue > 16) {
            bc_set_error(parser, "base out of range");
            return;
        }
        value = bc_make_int(ivalue);
    }

    index = bc_find_var_index(parser->env, name);
    if (index == parser->env->var_count) {
        if (parser->env->var_count >= BC_MAX_VARS) {
            bc_set_error(parser, "too many variables");
            return;
        }
        rt_copy_string(parser->env->vars[index].name, sizeof(parser->env->vars[index].name), name);
        parser->env->var_count += 1;
    }

    parser->env->vars[index].value = value;
}

static void bc_env_init(BcEnv *env, int math_mode) {
    env->var_count = 0;

    rt_copy_string(env->vars[0].name, sizeof(env->vars[0].name), "scale");
    env->vars[0].value = bc_make_int(math_mode ? 18 : 6);
    rt_copy_string(env->vars[1].name, sizeof(env->vars[1].name), "ibase");
    env->vars[1].value = bc_make_int(10);
    rt_copy_string(env->vars[2].name, sizeof(env->vars[2].name), "obase");
    env->vars[2].value = bc_make_int(10);
    rt_copy_string(env->vars[3].name, sizeof(env->vars[3].name), "last");
    env->vars[3].value = bc_make_int(0);
    env->var_count = 4;

    if (math_mode) {
        rt_copy_string(env->vars[4].name, sizeof(env->vars[4].name), "pi");
        env->vars[4].value = bc_make_value(3141592653589793238LL, 18);
        rt_copy_string(env->vars[5].name, sizeof(env->vars[5].name), "e");
        env->vars[5].value = bc_make_value(2718281828459045235LL, 18);
        env->var_count = 6;
    }
}

static void bc_skip_ignored(BcParser *parser) {
    for (;;) {
        while (parser->text[parser->pos] == ' ' || parser->text[parser->pos] == '\t' ||
               parser->text[parser->pos] == '\r' || parser->text[parser->pos] == '\v' ||
               parser->text[parser->pos] == '\f') {
            parser->pos += 1;
        }

        if (parser->text[parser->pos] == '#') {
            while (parser->text[parser->pos] != '\0' && parser->text[parser->pos] != '\n') {
                parser->pos += 1;
            }
            continue;
        }

        if (parser->text[parser->pos] == '/' && parser->text[parser->pos + 1] == '/') {
            parser->pos += 2;
            while (parser->text[parser->pos] != '\0' && parser->text[parser->pos] != '\n') {
                parser->pos += 1;
            }
            continue;
        }

        if (parser->text[parser->pos] == '/' && parser->text[parser->pos + 1] == '*') {
            parser->pos += 2;
            while (parser->text[parser->pos] != '\0' &&
                   !(parser->text[parser->pos] == '*' && parser->text[parser->pos + 1] == '/')) {
                parser->pos += 1;
            }
            if (parser->text[parser->pos] == '\0') {
                bc_set_error(parser, "unterminated comment");
                return;
            }
            parser->pos += 2;
            continue;
        }

        break;
    }
}

static void bc_read_token(BcParser *parser) {
    char ch;
    int base;

    if (parser->has_token || parser->error) {
        return;
    }

    bc_skip_ignored(parser);
    if (parser->error) {
        return;
    }

    ch = parser->text[parser->pos];
    base = bc_get_ibase_setting(parser->env);
    parser->token.text[0] = '\0';
    parser->token.number = bc_make_int(0);

    if (ch == '\0') {
        parser->token.type = BC_TOKEN_EOF;
    } else if (ch == '\n') {
        parser->pos += 1;
        parser->token.type = BC_TOKEN_NEWLINE;
    } else if (ch == ';') {
        parser->pos += 1;
        parser->token.type = BC_TOKEN_SEMICOLON;
    } else if (ch == '(') {
        parser->pos += 1;
        parser->token.type = BC_TOKEN_LPAREN;
    } else if (ch == ')') {
        parser->pos += 1;
        parser->token.type = BC_TOKEN_RPAREN;
    } else if (ch == '{') {
        parser->pos += 1;
        parser->token.type = BC_TOKEN_LBRACE;
    } else if (ch == '}') {
        parser->pos += 1;
        parser->token.type = BC_TOKEN_RBRACE;
    } else if (ch == ',') {
        parser->pos += 1;
        parser->token.type = BC_TOKEN_COMMA;
    } else if (ch == '+') {
        parser->pos += 1;
        parser->token.type = BC_TOKEN_PLUS;
    } else if (ch == '-') {
        parser->pos += 1;
        parser->token.type = BC_TOKEN_MINUS;
    } else if (ch == '*') {
        parser->pos += 1;
        parser->token.type = BC_TOKEN_STAR;
    } else if (ch == '/') {
        parser->pos += 1;
        parser->token.type = BC_TOKEN_SLASH;
    } else if (ch == '%') {
        parser->pos += 1;
        parser->token.type = BC_TOKEN_PERCENT;
    } else if (ch == '^') {
        parser->pos += 1;
        parser->token.type = BC_TOKEN_CARET;
    } else if (ch == '!') {
        parser->pos += 1;
        if (parser->text[parser->pos] == '=') {
            parser->pos += 1;
            parser->token.type = BC_TOKEN_NE;
        } else {
            parser->token.type = BC_TOKEN_NOT;
        }
    } else if (ch == '=') {
        parser->pos += 1;
        if (parser->text[parser->pos] == '=') {
            parser->pos += 1;
            parser->token.type = BC_TOKEN_EQ;
        } else {
            parser->token.type = BC_TOKEN_ASSIGN;
        }
    } else if (ch == '<') {
        parser->pos += 1;
        if (parser->text[parser->pos] == '=') {
            parser->pos += 1;
            parser->token.type = BC_TOKEN_LE;
        } else {
            parser->token.type = BC_TOKEN_LT;
        }
    } else if (ch == '>') {
        parser->pos += 1;
        if (parser->text[parser->pos] == '=') {
            parser->pos += 1;
            parser->token.type = BC_TOKEN_GE;
        } else {
            parser->token.type = BC_TOKEN_GT;
        }
    } else if (ch == '&' && parser->text[parser->pos + 1] == '&') {
        parser->pos += 2;
        parser->token.type = BC_TOKEN_AND;
    } else if (ch == '|' && parser->text[parser->pos + 1] == '|') {
        parser->pos += 2;
        parser->token.type = BC_TOKEN_OR;
    } else if ((ch >= '0' && ch <= '9') ||
               (base > 10 && ch >= 'A' && ch <= 'F') ||
               (ch == '.' && parser->text[parser->pos + 1] >= '0' && parser->text[parser->pos + 1] <= '9')) {
        unsigned __int128 mantissa = 0;
        int scale = 0;
        int saw_digit = 0;
        int saw_dot = 0;

        if (ch == '.' && base != 10) {
            bc_set_error(parser, "fractional input requires ibase=10");
            return;
        }

        while (1) {
            ch = parser->text[parser->pos];
            if (ch == '.' && !saw_dot) {
                if (base != 10) {
                    bc_set_error(parser, "fractional input requires ibase=10");
                    return;
                }
                saw_dot = 1;
                parser->pos += 1;
                continue;
            }

            if (base == 10) {
                if (ch < '0' || ch > '9') {
                    break;
                }
                saw_digit = 1;
                if (!saw_dot || scale < BC_MAX_SCALE) {
                    mantissa = (mantissa * 10U) + (unsigned int)(ch - '0');
                    if (saw_dot) {
                        scale += 1;
                    }
                }
                parser->pos += 1;
            } else {
                int digit = bc_hex_digit_value(ch);

                if (digit < 0 || digit >= base) {
                    break;
                }
                saw_digit = 1;
                mantissa = (mantissa * (unsigned int)base) + (unsigned int)digit;
                parser->pos += 1;
            }
        }

        if (!saw_digit) {
            bc_set_error(parser, "syntax error");
            return;
        }
        if (bc_i128_to_ll(parser, (__int128)mantissa, &parser->token.number.mantissa) != 0) {
            return;
        }
        parser->token.number.scale = scale;
        parser->token.type = BC_TOKEN_NUMBER;
    } else if (bc_is_name_start(ch)) {
        size_t used = 0;

        while (bc_is_name_char(parser->text[parser->pos])) {
            if (used + 1 >= sizeof(parser->token.text)) {
                bc_set_error(parser, "identifier too long");
                return;
            }
            parser->token.text[used++] = parser->text[parser->pos++];
        }
        parser->token.text[used] = '\0';

        if (rt_strcmp(parser->token.text, "if") == 0) {
            parser->token.type = BC_TOKEN_IF;
        } else if (rt_strcmp(parser->token.text, "else") == 0) {
            parser->token.type = BC_TOKEN_ELSE;
        } else if (rt_strcmp(parser->token.text, "while") == 0) {
            parser->token.type = BC_TOKEN_WHILE;
        } else if (rt_strcmp(parser->token.text, "for") == 0) {
            parser->token.type = BC_TOKEN_FOR;
        } else if (rt_strcmp(parser->token.text, "print") == 0) {
            parser->token.type = BC_TOKEN_PRINT;
        } else {
            parser->token.type = BC_TOKEN_IDENT;
        }
    } else {
        bc_set_error(parser, "syntax error");
        return;
    }

    parser->has_token = 1;
}

static BcToken *bc_peek_token(BcParser *parser) {
    bc_read_token(parser);
    return &parser->token;
}

static BcToken bc_take_token(BcParser *parser) {
    BcToken token;

    bc_read_token(parser);
    token = parser->token;
    parser->has_token = 0;
    return token;
}

static int bc_match(BcParser *parser, BcTokenType type) {
    if (bc_peek_token(parser)->type == type) {
        parser->has_token = 0;
        return 1;
    }
    return 0;
}

static int bc_expect(BcParser *parser, BcTokenType type, const char *message) {
    if (bc_peek_token(parser)->type != type) {
        bc_set_error(parser, message);
        return -1;
    }
    parser->has_token = 0;
    return 0;
}

static void bc_skip_statement_separators(BcParser *parser) {
    while (!parser->error) {
        BcTokenType type = bc_peek_token(parser)->type;

        if (type != BC_TOKEN_NEWLINE && type != BC_TOKEN_SEMICOLON) {
            break;
        }
        parser->has_token = 0;
    }
}

static BcValue bc_parse_expression(BcParser *parser, int evaluate, int *assigned_out);
static void bc_parse_statement(BcParser *parser, int execute);

static BcValue bc_parse_primary(BcParser *parser, int evaluate) {
    BcToken token = bc_take_token(parser);

    if (parser->error) {
        return bc_make_int(0);
    }

    if (token.type == BC_TOKEN_NUMBER) {
        if (!evaluate) {
            return bc_make_int(0);
        }
        return token.number;
    }

    if (token.type == BC_TOKEN_IDENT) {
        if (bc_match(parser, BC_TOKEN_LPAREN)) {
            BcValue arg = bc_make_int(0);
            int has_arg = 0;

            if (bc_peek_token(parser)->type != BC_TOKEN_RPAREN) {
                has_arg = 1;
                arg = bc_parse_expression(parser, evaluate, 0);
                if (bc_match(parser, BC_TOKEN_COMMA)) {
                    bc_set_error(parser, "too many function arguments");
                    return bc_make_int(0);
                }
            }
            if (bc_expect(parser, BC_TOKEN_RPAREN, "missing ')'") != 0) {
                return bc_make_int(0);
            }

            if (!evaluate) {
                return bc_make_int(0);
            }
            if (!has_arg) {
                bc_set_error(parser, "missing function argument");
                return bc_make_int(0);
            }
            if (rt_strcmp(token.text, "sqrt") == 0) {
                return bc_sqrt_value(parser, arg);
            }
            if (rt_strcmp(token.text, "length") == 0) {
                return bc_length_value(arg);
            }
            if (rt_strcmp(token.text, "scale") == 0) {
                return bc_scale_value(arg);
            }
            if (rt_strcmp(token.text, "abs") == 0) {
                if (arg.mantissa < 0) {
                    arg.mantissa = -arg.mantissa;
                }
                return arg;
            }
            bc_set_error(parser, "unknown function");
            return bc_make_int(0);
        }

        if (!evaluate) {
            return bc_make_int(0);
        }
        return bc_get_var(parser->env, token.text);
    }

    if (token.type == BC_TOKEN_LPAREN) {
        BcValue value = bc_parse_expression(parser, evaluate, 0);

        if (bc_expect(parser, BC_TOKEN_RPAREN, "missing ')'") != 0) {
            return bc_make_int(0);
        }
        return value;
    }

    bc_set_error(parser, "syntax error");
    return bc_make_int(0);
}

static BcValue bc_parse_unary(BcParser *parser, int evaluate) {
    BcTokenType type = bc_peek_token(parser)->type;

    if (type == BC_TOKEN_PLUS) {
        parser->has_token = 0;
        return bc_parse_unary(parser, evaluate);
    }
    if (type == BC_TOKEN_MINUS) {
        BcValue value;

        parser->has_token = 0;
        value = bc_parse_unary(parser, evaluate);
        if (evaluate && !parser->error) {
            value.mantissa = -value.mantissa;
        }
        return value;
    }
    if (type == BC_TOKEN_NOT) {
        BcValue value;

        parser->has_token = 0;
        value = bc_parse_unary(parser, evaluate);
        if (!evaluate) {
            return bc_make_int(0);
        }
        return bc_make_int(!bc_value_truth(value));
    }

    return bc_parse_primary(parser, evaluate);
}

static BcValue bc_parse_power(BcParser *parser, int evaluate) {
    BcValue value = bc_parse_unary(parser, evaluate);

    if (bc_match(parser, BC_TOKEN_CARET)) {
        BcValue exponent = bc_parse_power(parser, evaluate);

        if (!evaluate) {
            return bc_make_int(0);
        }
        return bc_pow_values(parser, value, exponent);
    }

    return value;
}

static BcValue bc_parse_term(BcParser *parser, int evaluate) {
    BcValue value = bc_parse_power(parser, evaluate);

    while (!parser->error) {
        BcTokenType type = bc_peek_token(parser)->type;
        BcValue right;

        if (type != BC_TOKEN_STAR && type != BC_TOKEN_SLASH && type != BC_TOKEN_PERCENT) {
            break;
        }
        parser->has_token = 0;
        right = bc_parse_power(parser, evaluate);
        if (!evaluate) {
            value = bc_make_int(0);
        } else if (type == BC_TOKEN_STAR) {
            value = bc_mul_values(parser, value, right);
        } else if (type == BC_TOKEN_SLASH) {
            value = bc_div_values(parser, value, right);
        } else {
            value = bc_mod_values(parser, value, right);
        }
    }

    return value;
}

static BcValue bc_parse_additive(BcParser *parser, int evaluate) {
    BcValue value = bc_parse_term(parser, evaluate);

    while (!parser->error) {
        BcTokenType type = bc_peek_token(parser)->type;
        BcValue right;

        if (type != BC_TOKEN_PLUS && type != BC_TOKEN_MINUS) {
            break;
        }
        parser->has_token = 0;
        right = bc_parse_term(parser, evaluate);
        if (!evaluate) {
            value = bc_make_int(0);
        } else if (type == BC_TOKEN_PLUS) {
            value = bc_add_values(parser, value, right);
        } else {
            value = bc_sub_values(parser, value, right);
        }
    }

    return value;
}

static BcValue bc_parse_comparison(BcParser *parser, int evaluate) {
    BcValue value = bc_parse_additive(parser, evaluate);

    while (!parser->error) {
        BcTokenType type = bc_peek_token(parser)->type;
        BcValue right;
        int compare_result = 0;

        if (type != BC_TOKEN_LT && type != BC_TOKEN_LE && type != BC_TOKEN_GT && type != BC_TOKEN_GE) {
            break;
        }
        parser->has_token = 0;
        right = bc_parse_additive(parser, evaluate);
        if (!evaluate) {
            value = bc_make_int(0);
        } else {
            compare_result = bc_compare_values(parser, value, right);
            if (type == BC_TOKEN_LT) {
                value = bc_make_int(compare_result < 0);
            } else if (type == BC_TOKEN_LE) {
                value = bc_make_int(compare_result <= 0);
            } else if (type == BC_TOKEN_GT) {
                value = bc_make_int(compare_result > 0);
            } else {
                value = bc_make_int(compare_result >= 0);
            }
        }
    }

    return value;
}

static BcValue bc_parse_equality(BcParser *parser, int evaluate) {
    BcValue value = bc_parse_comparison(parser, evaluate);

    while (!parser->error) {
        BcTokenType type = bc_peek_token(parser)->type;
        BcValue right;
        int compare_result = 0;

        if (type != BC_TOKEN_EQ && type != BC_TOKEN_NE) {
            break;
        }
        parser->has_token = 0;
        right = bc_parse_comparison(parser, evaluate);
        if (!evaluate) {
            value = bc_make_int(0);
        } else {
            compare_result = bc_compare_values(parser, value, right);
            if (type == BC_TOKEN_EQ) {
                value = bc_make_int(compare_result == 0);
            } else {
                value = bc_make_int(compare_result != 0);
            }
        }
    }

    return value;
}

static BcValue bc_parse_logical_and(BcParser *parser, int evaluate) {
    BcValue value = bc_parse_equality(parser, evaluate);

    while (!parser->error && bc_peek_token(parser)->type == BC_TOKEN_AND) {
        BcValue right;

        parser->has_token = 0;
        right = bc_parse_equality(parser, evaluate && bc_value_truth(value));
        if (!evaluate) {
            value = bc_make_int(0);
        } else {
            value = bc_make_int(bc_value_truth(value) && bc_value_truth(right));
        }
    }

    return value;
}

static BcValue bc_parse_logical_or(BcParser *parser, int evaluate) {
    BcValue value = bc_parse_logical_and(parser, evaluate);

    while (!parser->error && bc_peek_token(parser)->type == BC_TOKEN_OR) {
        BcValue right;

        parser->has_token = 0;
        right = bc_parse_logical_and(parser, evaluate && !bc_value_truth(value));
        if (!evaluate) {
            value = bc_make_int(0);
        } else {
            value = bc_make_int(bc_value_truth(value) || bc_value_truth(right));
        }
    }

    return value;
}

static BcValue bc_parse_expression(BcParser *parser, int evaluate, int *assigned_out) {
    BcParser snapshot = *parser;

    if (assigned_out != 0) {
        *assigned_out = 0;
    }

    if (bc_peek_token(parser)->type == BC_TOKEN_IDENT) {
        BcToken ident = bc_take_token(parser);

        if (bc_match(parser, BC_TOKEN_ASSIGN)) {
            BcValue value = bc_parse_expression(parser, evaluate, 0);

            if (evaluate && !parser->error) {
                bc_store_var(parser, ident.text, value);
            }
            if (assigned_out != 0) {
                *assigned_out = 1;
            }
            return value;
        }
    }

    *parser = snapshot;
    return bc_parse_logical_or(parser, evaluate);
}

static void bc_format_decimal(BcValue value, char *buffer, size_t buffer_size) {
    char integer_digits[64];
    char fraction_digits[BC_MAX_SCALE + 1];
    unsigned long long magnitude = bc_unsigned_magnitude(value.mantissa);
    unsigned long long divisor = (unsigned long long)bc_pow10_u128(value.scale);
    unsigned long long integer_part = value.scale > 0 ? (magnitude / divisor) : magnitude;
    unsigned long long fraction_part = value.scale > 0 ? (magnitude % divisor) : 0;
    size_t len = 0;
    size_t int_len;
    int i;

    if (buffer_size == 0) {
        return;
    }

    if (value.mantissa < 0 && len + 1 < buffer_size) {
        buffer[len++] = '-';
    }

    rt_unsigned_to_string(integer_part, integer_digits, sizeof(integer_digits));
    int_len = rt_strlen(integer_digits);
    if (len + int_len + 1 > buffer_size) {
        int_len = (buffer_size > len + 1U) ? (buffer_size - len - 1U) : 0U;
    }
    for (i = 0; i < (int)int_len; ++i) {
        buffer[len++] = integer_digits[i];
    }

    if (value.scale > 0) {
        for (i = value.scale - 1; i >= 0; --i) {
            fraction_digits[i] = (char)('0' + (fraction_part % 10ULL));
            fraction_part /= 10ULL;
        }
        fraction_digits[value.scale] = '\0';
        if (len + 1 < buffer_size) {
            buffer[len++] = '.';
            if (len + (size_t)value.scale + 1 > buffer_size) {
                value.scale = (int)(buffer_size - len - 1);
            }
            memcpy(buffer + len, fraction_digits, (size_t)value.scale);
            len += (size_t)value.scale;
        }
    }

    buffer[len] = '\0';
}

static void bc_format_based(BcParser *parser, BcValue value, char *buffer, size_t buffer_size) {
    char reverse_digits[128];
    char fraction_digits[BC_MAX_SCALE + 1];
    int base = bc_get_obase_setting(parser->env);
    unsigned long long magnitude = bc_unsigned_magnitude(value.mantissa);
    unsigned long long divisor = (unsigned long long)bc_pow10_u128(value.scale);
    unsigned long long integer_part = value.scale > 0 ? (magnitude / divisor) : magnitude;
    unsigned long long remainder = value.scale > 0 ? (magnitude % divisor) : 0;
    size_t rev_len = 0;
    size_t len = 0;
    int frac_len = 0;

    if (buffer_size == 0) {
        return;
    }

    if (base == 10) {
        bc_format_decimal(value, buffer, buffer_size);
        return;
    }

    if (value.mantissa < 0 && len + 1 < buffer_size) {
        buffer[len++] = '-';
    }

    if (integer_part == 0) {
        reverse_digits[rev_len++] = '0';
    } else {
        while (integer_part > 0 && rev_len < sizeof(reverse_digits)) {
            reverse_digits[rev_len++] = bc_digit_char((int)(integer_part % (unsigned long long)base));
            integer_part /= (unsigned long long)base;
        }
    }

    while (rev_len > 0 && len + 1 < buffer_size) {
        buffer[len++] = reverse_digits[--rev_len];
    }

    if (value.scale > 0) {
        while (frac_len < value.scale && frac_len < (int)sizeof(fraction_digits) - 1) {
            unsigned __int128 expanded = (unsigned __int128)remainder * (unsigned int)base;
            unsigned long long digit = (unsigned long long)(expanded / divisor);

            remainder = (unsigned long long)(expanded % divisor);
            fraction_digits[frac_len++] = bc_digit_char((int)digit);
        }
        if (len + 1 < buffer_size) {
            int i;

            buffer[len++] = '.';
            for (i = 0; i < frac_len && len + 1 < buffer_size; ++i) {
                buffer[len++] = fraction_digits[i];
            }
        }
    }

    buffer[len] = '\0';
}

static int bc_print_value(BcParser *parser, BcValue value) {
    char buffer[BC_OUTPUT_CAPACITY];

    bc_format_based(parser, value, buffer, sizeof(buffer));
    if (rt_write_line(1, buffer) != 0) {
        tool_write_error("bc", "failed to write output", 0);
        return -1;
    }
    bc_store_var(parser, "last", value);
    if (parser->error) {
        return -1;
    }
    return 0;
}

static void bc_parse_block(BcParser *parser, int execute) {
    if (bc_expect(parser, BC_TOKEN_LBRACE, "missing '{'") != 0) {
        return;
    }

    while (!parser->error) {
        bc_skip_statement_separators(parser);
        if (bc_match(parser, BC_TOKEN_RBRACE)) {
            return;
        }
        if (bc_peek_token(parser)->type == BC_TOKEN_EOF) {
            bc_set_error(parser, "missing '}'");
            return;
        }
        bc_parse_statement(parser, execute);
    }
}

static void bc_parse_if(BcParser *parser, int execute) {
    BcValue condition;

    parser->has_token = 0;
    if (bc_expect(parser, BC_TOKEN_LPAREN, "missing '('") != 0) {
        return;
    }
    condition = bc_parse_expression(parser, execute, 0);
    if (bc_expect(parser, BC_TOKEN_RPAREN, "missing ')'") != 0) {
        return;
    }

    bc_parse_statement(parser, execute && bc_value_truth(condition));
    bc_skip_statement_separators(parser);
    if (bc_match(parser, BC_TOKEN_ELSE)) {
        bc_parse_statement(parser, execute && !bc_value_truth(condition));
    }
}

static void bc_parse_while(BcParser *parser, int execute) {
    size_t condition_pos;
    size_t body_end = parser->pos;

    parser->has_token = 0;
    if (bc_expect(parser, BC_TOKEN_LPAREN, "missing '('") != 0) {
        return;
    }
    condition_pos = parser->pos;

    if (!execute) {
        (void)bc_parse_expression(parser, 0, 0);
        if (bc_expect(parser, BC_TOKEN_RPAREN, "missing ')'") != 0) {
            return;
        }
        bc_parse_statement(parser, 0);
        return;
    }

    for (;;) {
        BcValue condition;

        parser->pos = condition_pos;
        parser->has_token = 0;
        condition = bc_parse_expression(parser, 1, 0);
        if (bc_expect(parser, BC_TOKEN_RPAREN, "missing ')'") != 0) {
            return;
        }
        if (!bc_value_truth(condition)) {
            bc_parse_statement(parser, 0);
            body_end = parser->pos;
            break;
        }
        bc_parse_statement(parser, 1);
        body_end = parser->pos;
    }

    parser->pos = body_end;
    parser->has_token = 0;
}

static void bc_parse_for(BcParser *parser, int execute) {
    size_t condition_pos;
    size_t step_pos;
    size_t body_pos;
    size_t body_end = parser->pos;
    int has_condition = 0;
    int has_step = 0;

    parser->has_token = 0;
    if (bc_expect(parser, BC_TOKEN_LPAREN, "missing '('") != 0) {
        return;
    }

    if (bc_peek_token(parser)->type != BC_TOKEN_SEMICOLON) {
        (void)bc_parse_expression(parser, execute, 0);
    }
    if (bc_expect(parser, BC_TOKEN_SEMICOLON, "missing ';'") != 0) {
        return;
    }

    condition_pos = parser->pos;
    if (bc_peek_token(parser)->type != BC_TOKEN_SEMICOLON) {
        has_condition = 1;
        (void)bc_parse_expression(parser, 0, 0);
    }
    if (bc_expect(parser, BC_TOKEN_SEMICOLON, "missing ';'") != 0) {
        return;
    }

    step_pos = parser->pos;
    if (bc_peek_token(parser)->type != BC_TOKEN_RPAREN) {
        has_step = 1;
        (void)bc_parse_expression(parser, 0, 0);
    }
    if (bc_expect(parser, BC_TOKEN_RPAREN, "missing ')'") != 0) {
        return;
    }

    body_pos = parser->pos;

    if (!execute) {
        bc_parse_statement(parser, 0);
        return;
    }

    for (;;) {
        int condition_true = 1;

        if (has_condition) {
            BcValue condition;

            parser->pos = condition_pos;
            parser->has_token = 0;
            condition = bc_parse_expression(parser, 1, 0);
            if (bc_expect(parser, BC_TOKEN_SEMICOLON, "missing ';'") != 0) {
                return;
            }
            condition_true = bc_value_truth(condition);
        }

        parser->pos = body_pos;
        parser->has_token = 0;
        if (!condition_true) {
            bc_parse_statement(parser, 0);
            body_end = parser->pos;
            break;
        }

        bc_parse_statement(parser, 1);
        body_end = parser->pos;

        if (has_step) {
            parser->pos = step_pos;
            parser->has_token = 0;
            (void)bc_parse_expression(parser, 1, 0);
            if (bc_expect(parser, BC_TOKEN_RPAREN, "missing ')'") != 0) {
                return;
            }
        }
    }

    parser->pos = body_end;
    parser->has_token = 0;
}

static void bc_parse_statement(BcParser *parser, int execute) {
    BcTokenType type;

    bc_skip_statement_separators(parser);
    type = bc_peek_token(parser)->type;

    if (type == BC_TOKEN_EOF || type == BC_TOKEN_RBRACE) {
        return;
    }

    if (type == BC_TOKEN_LBRACE) {
        bc_parse_block(parser, execute);
        return;
    }

    if (type == BC_TOKEN_IF) {
        bc_parse_if(parser, execute);
        return;
    }

    if (type == BC_TOKEN_WHILE) {
        bc_parse_while(parser, execute);
        return;
    }

    if (type == BC_TOKEN_FOR) {
        bc_parse_for(parser, execute);
        return;
    }

    if (type == BC_TOKEN_PRINT) {
        BcValue value;

        parser->has_token = 0;
        value = bc_parse_expression(parser, execute, 0);
        if (execute && !parser->error) {
            (void)bc_print_value(parser, value);
        }
        return;
    }

    {
        BcValue value;
        int assigned = 0;

        value = bc_parse_expression(parser, execute, &assigned);
        if (execute && !parser->error && !assigned) {
            (void)bc_print_value(parser, value);
        }
    }
}

static int bc_read_stdin(char *buffer, size_t buffer_size) {
    size_t used = 0;
    char chunk[512];
    long bytes_read;

    while ((bytes_read = platform_read(0, chunk, sizeof(chunk))) > 0) {
        size_t copy_size = (size_t)bytes_read;

        if (used + copy_size + 1 > buffer_size) {
            return -1;
        }

        memcpy(buffer + used, chunk, copy_size);
        used += copy_size;
    }

    if (bytes_read < 0) {
        return -1;
    }

    buffer[used] = '\0';
    return 0;
}

static int bc_join_arguments(int start_index, int argc, char **argv, char *buffer, size_t buffer_size) {
    size_t used = 0;
    int i;

    if (buffer_size == 0) {
        return -1;
    }

    buffer[0] = '\0';

    for (i = start_index; i < argc; ++i) {
        size_t arg_len = rt_strlen(argv[i]);

        if (used + arg_len + 2 > buffer_size) {
            return -1;
        }

        if (used > 0) {
            buffer[used++] = ' ';
        }

        memcpy(buffer + used, argv[i], arg_len);
        used += arg_len;
        buffer[used] = '\0';
    }

    return 0;
}

int main(int argc, char **argv) {
    char input[BC_INPUT_CAPACITY];
    BcEnv env;
    BcParser parser;
    ToolOptState opt;
    int math_mode = 0;
    int opt_result;

    tool_opt_init(&opt, argc, argv, tool_base_name(argv[0]), "[-l] [program]");
    while ((opt_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-l") == 0) {
            math_mode = 1;
        } else {
            tool_write_error(opt.prog, "unknown option: ", opt.flag);
            tool_write_usage(opt.prog, opt.usage_suffix);
            return 1;
        }
    }
    if (opt_result == TOOL_OPT_HELP) {
        tool_write_usage(tool_base_name(argv[0]), "[-l] [program]");
        return 0;
    }
    if (opt_result == TOOL_OPT_ERROR) {
        return 1;
    }

    if (opt.argi < argc) {
        if (bc_join_arguments(opt.argi, argc, argv, input, sizeof(input)) != 0) {
            tool_write_error("bc", "expression too large", 0);
            return 1;
        }
    } else if (bc_read_stdin(input, sizeof(input)) != 0) {
        tool_write_error("bc", "failed to read input", 0);
        return 1;
    }

    bc_env_init(&env, math_mode);
    parser.text = input;
    parser.pos = 0;
    parser.error = 0;
    parser.message = 0;
    parser.env = &env;
    parser.has_token = 0;

    while (!parser.error) {
        bc_skip_statement_separators(&parser);
        if (bc_peek_token(&parser)->type == BC_TOKEN_EOF) {
            break;
        }
        if (bc_peek_token(&parser)->type == BC_TOKEN_RBRACE) {
            bc_set_error(&parser, "unexpected '}'");
            break;
        }
        bc_parse_statement(&parser, 1);
    }

    if (parser.error) {
        tool_write_error("bc", parser.message != 0 ? parser.message : "syntax error", 0);
        return 1;
    }

    return 0;
}
