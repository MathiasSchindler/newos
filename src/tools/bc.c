#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "bignum.h"

#define BC_INPUT_CAPACITY 16384
#define BC_MAX_SCALE 18
#define BC_MAX_VARS 128
#define BC_NAME_CAPACITY 32
#define BC_OUTPUT_CAPACITY 2048
#define BC_MAX_PARSE_DEPTH 128
#define BC_MAX_LOOP_ITERATIONS 1000000ULL

typedef struct {
    Bignum mantissa;
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
    unsigned int nesting_depth;
} BcParser;

static BcValue bc_make_value_bn(const Bignum *mantissa, int scale) {
    BcValue value;

    if (scale < 0) {
        scale = 0;
    }
    if (scale > BC_MAX_SCALE) {
        scale = BC_MAX_SCALE;
    }

    value.mantissa = *mantissa;
    value.scale = scale;
    return value;
}

static BcValue bc_make_int(long long value) {
    BcValue result;
    bn_from_int(&result.mantissa, value);
    result.scale = 0;
    return result;
}

static void bc_set_error(BcParser *parser, const char *message) {
    if (!parser->error) {
        parser->error = 1;
        parser->message = message;
    }
}

static int bc_enter_nesting(BcParser *parser) {
    if (parser->nesting_depth >= BC_MAX_PARSE_DEPTH) {
        bc_set_error(parser, "nesting too deep");
        return -1;
    }
    parser->nesting_depth += 1U;
    return 0;
}

static void bc_leave_nesting(BcParser *parser) {
    if (parser->nesting_depth > 0U) {
        parser->nesting_depth -= 1U;
    }
}

static int bc_value_truth(BcValue value) {
    return !bn_is_zero(&value.mantissa);
}

static BcValue bc_normalize_value(BcValue value) {
    Bignum quotient;
    unsigned int remainder;
    
    while (value.scale > 0) {
        if (bn_divide_digit(&value.mantissa, 10, &quotient, &remainder) != 0) {
            break;
        }
        if (remainder != 0) {
            break;
        }
        value.mantissa = quotient;
        value.scale -= 1;
    }
    return value;
}

static BcValue bc_rescale(BcParser *parser, BcValue value, int target_scale) {
    int delta;
    Bignum result;

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

    if (bn_is_zero(&value.mantissa)) {
        value.scale = target_scale;
        return value;
    }

    delta = target_scale - value.scale;
    
    if (delta > 0) {
        if (bn_scale(&value.mantissa, delta, &result) != 0) {
            bc_set_error(parser, "numeric overflow");
            return bc_make_int(0);
        }
    } else {
        if (bn_scale(&value.mantissa, delta, &result) != 0) {
            bc_set_error(parser, "numeric overflow");
            return bc_make_int(0);
        }
    }
    
    return bc_make_value_bn(&result, target_scale);
}

static int bc_get_scale_setting(BcEnv *env) {
    size_t i;

    for (i = 0; i < env->var_count; ++i) {
        if (rt_strcmp(env->vars[i].name, "scale") == 0) {
            BcValue value = env->vars[i].value;
            Bignum integer_part;
            char buffer[64];

            if (value.scale > 0) {
                if (bn_scale(&value.mantissa, -value.scale, &integer_part) != 0) {
                    return 6;
                }
            } else {
                integer_part = value.mantissa;
            }
            
            if (bn_to_string(&integer_part, buffer, sizeof(buffer)) != 0) {
                return 6;
            }
            
            long long ivalue = 0;
            int is_neg = 0;
            const char *p = buffer;
            if (*p == '-') {
                is_neg = 1;
                p++;
            }
            while (*p >= '0' && *p <= '9') {
                ivalue = ivalue * 10 + (*p - '0');
                p++;
            }
            if (is_neg) ivalue = -ivalue;
            
            if (ivalue < 0) {
                return 0;
            }
            if (ivalue > BC_MAX_SCALE) {
                return BC_MAX_SCALE;
            }
            return (int)ivalue;
        }
    }

    return 6;
}

static int bc_get_ibase_setting(BcEnv *env) {
    size_t i;

    for (i = 0; i < env->var_count; ++i) {
        if (rt_strcmp(env->vars[i].name, "ibase") == 0) {
            BcValue value = env->vars[i].value;
            Bignum integer_part;
            char buffer[64];

            if (value.scale > 0) {
                if (bn_scale(&value.mantissa, -value.scale, &integer_part) != 0) {
                    return 10;
                }
            } else {
                integer_part = value.mantissa;
            }
            
            if (bn_to_string(&integer_part, buffer, sizeof(buffer)) != 0) {
                return 10;
            }
            
            long long ivalue = 0;
            int is_neg = 0;
            const char *p = buffer;
            if (*p == '-') {
                is_neg = 1;
                p++;
            }
            while (*p >= '0' && *p <= '9') {
                ivalue = ivalue * 10 + (*p - '0');
                p++;
            }
            if (is_neg) ivalue = -ivalue;
            
            if (ivalue < 2) {
                return 10;
            }
            if (ivalue > 16) {
                return 16;
            }
            return (int)ivalue;
        }
    }

    return 10;
}

static int bc_get_obase_setting(BcEnv *env) {
    size_t i;

    for (i = 0; i < env->var_count; ++i) {
        if (rt_strcmp(env->vars[i].name, "obase") == 0) {
            BcValue value = env->vars[i].value;
            Bignum integer_part;
            char buffer[64];

            if (value.scale > 0) {
                if (bn_scale(&value.mantissa, -value.scale, &integer_part) != 0) {
                    return 10;
                }
            } else {
                integer_part = value.mantissa;
            }
            
            if (bn_to_string(&integer_part, buffer, sizeof(buffer)) != 0) {
                return 10;
            }
            
            long long ivalue = 0;
            int is_neg = 0;
            const char *p = buffer;
            if (*p == '-') {
                is_neg = 1;
                p++;
            }
            while (*p >= '0' && *p <= '9') {
                ivalue = ivalue * 10 + (*p - '0');
                p++;
            }
            if (is_neg) ivalue = -ivalue;
            
            if (ivalue < 2) {
                return 10;
            }
            if (ivalue > 16) {
                return 16;
            }
            return (int)ivalue;
        }
    }

    return 10;
}

static BcValue bc_add_values(BcParser *parser, BcValue left, BcValue right) {
    int scale = left.scale > right.scale ? left.scale : right.scale;
    BcValue a = bc_rescale(parser, left, scale);
    BcValue b = bc_rescale(parser, right, scale);
    Bignum result;

    if (parser->error) {
        return bc_make_int(0);
    }
    if (bn_add(&a.mantissa, &b.mantissa, &result) != 0) {
        bc_set_error(parser, "numeric overflow");
        return bc_make_int(0);
    }
    return bc_normalize_value(bc_make_value_bn(&result, scale));
}

static BcValue bc_sub_values(BcParser *parser, BcValue left, BcValue right) {
    int scale = left.scale > right.scale ? left.scale : right.scale;
    BcValue a = bc_rescale(parser, left, scale);
    BcValue b = bc_rescale(parser, right, scale);
    Bignum result;

    if (parser->error) {
        return bc_make_int(0);
    }
    if (bn_subtract(&a.mantissa, &b.mantissa, &result) != 0) {
        bc_set_error(parser, "numeric overflow");
        return bc_make_int(0);
    }
    return bc_normalize_value(bc_make_value_bn(&result, scale));
}

static BcValue bc_mul_values(BcParser *parser, BcValue left, BcValue right) {
    Bignum result;
    int scale = left.scale + right.scale;

    if (bn_multiply(&left.mantissa, &right.mantissa, &result) != 0) {
        bc_set_error(parser, "numeric overflow");
        return bc_make_int(0);
    }

    if (scale > BC_MAX_SCALE) {
        Bignum scaled;
        if (bn_scale(&result, -(scale - BC_MAX_SCALE), &scaled) != 0) {
            bc_set_error(parser, "numeric overflow");
            return bc_make_int(0);
        }
        result = scaled;
        scale = BC_MAX_SCALE;
    }

    return bc_normalize_value(bc_make_value_bn(&result, scale));
}

static BcValue bc_div_values(BcParser *parser, BcValue left, BcValue right) {
    int scale;
    Bignum numerator;
    Bignum result;
    Bignum remainder;

    if (bn_is_zero(&right.mantissa)) {
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

    numerator = left.mantissa;
    if (scale + right.scale >= left.scale) {
        int shift = scale + right.scale - left.scale;
        Bignum shifted;
        if (bn_scale(&numerator, shift, &shifted) != 0) {
            bc_set_error(parser, "numeric overflow");
            return bc_make_int(0);
        }
        numerator = shifted;
    } else {
        bc_set_error(parser, "division scale error");
        return bc_make_int(0);
    }

    if (bn_divide(&numerator, &right.mantissa, &result, &remainder) != 0) {
        bc_set_error(parser, "division error");
        return bc_make_int(0);
    }
    
    return bc_make_value_bn(&result, scale);
}

static BcValue bc_mod_values(BcParser *parser, BcValue left, BcValue right) {
    BcValue a = bc_rescale(parser, left, 0);
    BcValue b = bc_rescale(parser, right, 0);
    Bignum quotient;
    Bignum remainder;

    if (parser->error) {
        return bc_make_int(0);
    }
    if (bn_is_zero(&b.mantissa)) {
        bc_set_error(parser, "division by zero");
        return bc_make_int(0);
    }
    
    if (bn_divide(&a.mantissa, &b.mantissa, &quotient, &remainder) != 0) {
        bc_set_error(parser, "modulo error");
        return bc_make_int(0);
    }
    
    return bc_normalize_value(bc_make_value_bn(&remainder, 0));
}

static int bc_compare_values(BcParser *parser, BcValue left, BcValue right) {
    int scale = left.scale > right.scale ? left.scale : right.scale;
    BcValue a = bc_rescale(parser, left, scale);
    BcValue b = bc_rescale(parser, right, scale);

    if (parser->error) {
        return 0;
    }
    
    return bn_compare(&a.mantissa, &b.mantissa);
}

static int bc_value_to_integer(BcParser *parser, BcValue value, long long *out) {
    BcValue integer_value = bc_rescale(parser, value, 0);
    char buffer[128];

    if (parser->error) {
        return -1;
    }
    
    if (bn_to_string(&integer_value.mantissa, buffer, sizeof(buffer)) != 0) {
        bc_set_error(parser, "conversion error");
        return -1;
    }
    
    *out = 0;
    int is_neg = 0;
    const char *p = buffer;
    if (*p == '-') {
        is_neg = 1;
        p++;
    }
    
    while (*p >= '0' && *p <= '9') {
        long long digit = *p - '0';
        if (*out > (9223372036854775807LL / 10) ||
            (*out == (9223372036854775807LL / 10) && digit > 7)) {
            bc_set_error(parser, "integer overflow");
            return -1;
        }
        *out = *out * 10 + digit;
        p++;
    }
    
    if (is_neg) {
        *out = -*out;
    }
    
    return 0;
}

static BcValue bc_pow_values(BcParser *parser, BcValue base, BcValue exponent) {
    long long exp = 0;
    unsigned long long power;
    BcValue result = bc_make_int(1);
    BcValue factor = base;
    int negative = 0;
    Bignum exp_int;

    if (exponent.scale > 0) {
        exp_int = exponent.mantissa;
        if (bn_scale(&exp_int, -exponent.scale, &exp_int) != 0) {
            bc_set_error(parser, "exponent scale error");
            return bc_make_int(0);
        }
        
        Bignum original_exp = exponent.mantissa;
        Bignum scaled_back;
        if (bn_scale(&exp_int, exponent.scale, &scaled_back) != 0) {
            bc_set_error(parser, "exponent verification error");
            return bc_make_int(0);
        }
        
        if (bn_compare(&original_exp, &scaled_back) != 0) {
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

static BcValue bc_sqrt_value(BcParser *parser, BcValue value) {
    int target_scale;
    int exponent;
    Bignum scaled_value;
    Bignum guess;
    Bignum two;
    int i;
    char buffer[256];
    size_t len;
    size_t half_len;
    char guess_str[256];
    size_t j;

    if (value.mantissa.is_negative) {
        bc_set_error(parser, "square root of negative value");
        return bc_make_int(0);
    }

    if (bn_is_zero(&value.mantissa)) {
        return bc_make_int(0);
    }

    target_scale = bc_get_scale_setting(parser->env);
    if ((value.scale + 1) / 2 > target_scale) {
        target_scale = (value.scale + 1) / 2;
    }
    if (target_scale > BC_MAX_SCALE) {
        target_scale = BC_MAX_SCALE;
    }

    exponent = (target_scale * 2) - value.scale;
    if (exponent < 0) {
        exponent = 0;
    }

    if (bn_scale(&value.mantissa, exponent, &scaled_value) != 0) {
        bc_set_error(parser, "numeric overflow");
        return bc_make_int(0);
    }
    if (bn_to_string(&scaled_value, buffer, sizeof(buffer)) != 0) {
        bc_set_error(parser, "numeric overflow");
        return bc_make_int(0);
    }

    len = rt_strlen(buffer);
    half_len = (len + 1U) / 2U;
    guess_str[0] = '1';
    for (j = 1; j < half_len && j + 1 < sizeof(guess_str); ++j) {
        guess_str[j] = '0';
    }
    guess_str[j] = '\0';

    if (bn_from_string(&guess, guess_str) != 0) {
        guess = scaled_value;
    }

    bn_from_uint(&two, 2);

    for (i = 0; i < 50; ++i) {
        Bignum quotient;
        Bignum remainder;
        Bignum sum;
        Bignum new_guess;

        if (bn_divide(&scaled_value, &guess, &quotient, &remainder) != 0) {
            break;
        }
        if (bn_add(&guess, &quotient, &sum) != 0) {
            break;
        }
        if (bn_divide(&sum, &two, &new_guess, &remainder) != 0) {
            break;
        }
        if (bn_compare(&new_guess, &guess) == 0) {
            break;
        }
        guess = new_guess;
    }

    return bc_make_value_bn(&guess, target_scale);
}

static BcValue bc_length_value(BcValue value) {
    Bignum integer_part;
    char buffer[512];
    int digits;

    if (value.scale > 0) {
        if (bn_scale(&value.mantissa, -value.scale, &integer_part) != 0) {
            return bc_make_int(1);
        }
    } else {
        integer_part = value.mantissa;
    }
    
    integer_part.is_negative = 0;
    
    if (bn_is_zero(&integer_part)) {
        return bc_make_int(1);
    }
    
    if (bn_to_string(&integer_part, buffer, sizeof(buffer)) != 0) {
        return bc_make_int(1);
    }
    
    digits = 0;
    for (const char *p = buffer; *p != '\0'; p++) {
        if (*p >= '0' && *p <= '9') {
            digits++;
        }
    }
    
    if (digits == 0) {
        digits = 1;
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
        Bignum pi_mantissa, e_mantissa;
        rt_copy_string(env->vars[4].name, sizeof(env->vars[4].name), "pi");
        bn_from_string(&pi_mantissa, "3141592653589793238");
        env->vars[4].value = bc_make_value_bn(&pi_mantissa, 18);
        rt_copy_string(env->vars[5].name, sizeof(env->vars[5].name), "e");
        bn_from_string(&e_mantissa, "2718281828459045235");
        env->vars[5].value = bc_make_value_bn(&e_mantissa, 18);
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
        char num_buffer[512];
        size_t num_len = 0;
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
                if (num_len + 1U >= sizeof(num_buffer)) {
                    bc_set_error(parser, "numeric literal too long");
                    return;
                }
                num_buffer[num_len++] = ch;
                if (saw_dot) {
                    if (scale < BC_MAX_SCALE) {
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
                
                Bignum temp;
                Bignum digit_bn;
                Bignum multiplier;
                
                temp = parser->token.number.mantissa;
                bn_from_uint(&multiplier, (unsigned int)base);
                bn_from_uint(&digit_bn, (unsigned int)digit);
                
                if (bn_multiply(&temp, &multiplier, &temp) != 0) {
                    bc_set_error(parser, "numeric overflow");
                    return;
                }
                if (bn_add(&temp, &digit_bn, &parser->token.number.mantissa) != 0) {
                    bc_set_error(parser, "numeric overflow");
                    return;
                }
                
                parser->pos += 1;
            }
        }

        if (!saw_digit) {
            bc_set_error(parser, "syntax error");
            return;
        }
        
        if (base == 10) {
            num_buffer[num_len] = '\0';
            if (bn_from_string(&parser->token.number.mantissa, num_buffer) != 0) {
                bc_set_error(parser, "invalid number");
                return;
            }
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
                arg.mantissa.is_negative = 0;
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
    BcValue value;
    BcTokenType type = bc_peek_token(parser)->type;

    if (bc_enter_nesting(parser) != 0) {
        return bc_make_int(0);
    }

    if (type == BC_TOKEN_PLUS) {
        parser->has_token = 0;
        value = bc_parse_unary(parser, evaluate);
        bc_leave_nesting(parser);
        return value;
    }
    if (type == BC_TOKEN_MINUS) {
        parser->has_token = 0;
        value = bc_parse_unary(parser, evaluate);
        if (evaluate && !parser->error) {
            bn_negate(&value.mantissa);
        }
        bc_leave_nesting(parser);
        return value;
    }
    if (type == BC_TOKEN_NOT) {
        parser->has_token = 0;
        value = bc_parse_unary(parser, evaluate);
        if (!evaluate) {
            bc_leave_nesting(parser);
            return bc_make_int(0);
        }
        value = bc_make_int(!bc_value_truth(value));
        bc_leave_nesting(parser);
        return value;
    }

    value = bc_parse_primary(parser, evaluate);
    bc_leave_nesting(parser);
    return value;
}

static BcValue bc_parse_power(BcParser *parser, int evaluate) {
    BcValue value;

    if (bc_enter_nesting(parser) != 0) {
        return bc_make_int(0);
    }

    value = bc_parse_unary(parser, evaluate);

    if (bc_match(parser, BC_TOKEN_CARET)) {
        BcValue exponent = bc_parse_power(parser, evaluate);

        if (!evaluate) {
            bc_leave_nesting(parser);
            return bc_make_int(0);
        }
        value = bc_pow_values(parser, value, exponent);
        bc_leave_nesting(parser);
        return value;
    }

    bc_leave_nesting(parser);
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
    BcParser snapshot;
    BcValue value = bc_make_int(0);

    if (bc_enter_nesting(parser) != 0) {
        return value;
    }
    snapshot = *parser;

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
            bc_leave_nesting(parser);
            return value;
        }
    }

    *parser = snapshot;
    value = bc_parse_logical_or(parser, evaluate);
    bc_leave_nesting(parser);
    return value;
}

static void bc_format_decimal(BcValue value, char *buffer, size_t buffer_size) {
    char full_number[BC_OUTPUT_CAPACITY];
    Bignum integer_part;
    Bignum frac_part;
    Bignum divisor;
    size_t len = 0;
    size_t int_len;
    int i;

    if (buffer_size == 0) {
        return;
    }

    if (value.scale == 0) {
        if (bn_to_string(&value.mantissa, buffer, buffer_size) == 0) {
            return;
        }
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    if (bn_scale(&value.mantissa, -value.scale, &integer_part) != 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }

    if (bn_to_string(&integer_part, full_number, sizeof(full_number)) != 0) {
        buffer[0] = '0';
        buffer[1] = '\0';
        return;
    }
    
    int_len = rt_strlen(full_number);
    if (int_len + 1 > buffer_size) {
        int_len = buffer_size - 1;
    }
    
    for (i = 0; i < (int)int_len; i++) {
        buffer[len++] = full_number[i];
    }
    
    if (value.scale > 0 && len + 1 < buffer_size) {
        Bignum ten;
        bn_from_uint(&ten, 10);
        
        if (bn_power(&ten, value.scale, &divisor) != 0) {
            buffer[len] = '\0';
            return;
        }
        
        Bignum temp;
        if (bn_multiply(&integer_part, &divisor, &temp) != 0) {
            buffer[len] = '\0';
            return;
        }
        
        if (bn_subtract(&value.mantissa, &temp, &frac_part) != 0) {
            buffer[len] = '\0';
            return;
        }
        
        frac_part.is_negative = 0;
        
        char frac_buffer[BC_MAX_SCALE + 10];
        if (bn_to_string(&frac_part, frac_buffer, sizeof(frac_buffer)) != 0) {
            buffer[len] = '\0';
            return;
        }
        
        buffer[len++] = '.';
        
        size_t frac_len = rt_strlen(frac_buffer);
        int padding = value.scale - (int)frac_len;
        
        for (i = 0; i < padding && len + 1 < buffer_size; i++) {
            buffer[len++] = '0';
        }
        
        for (i = 0; i < (int)frac_len && len + 1 < buffer_size; i++) {
            buffer[len++] = frac_buffer[i];
        }
    }

    buffer[len] = '\0';
}

static void bc_format_based(BcParser *parser, BcValue value, char *buffer, size_t buffer_size) {
    char reverse_digits[256];
    int base = bc_get_obase_setting(parser->env);
    size_t rev_len = 0;
    size_t len = 0;
    Bignum integer_part;
    Bignum temp;

    if (buffer_size == 0) {
        return;
    }

    if (base == 10) {
        bc_format_decimal(value, buffer, buffer_size);
        return;
    }

    if (value.scale > 0) {
        if (bn_scale(&value.mantissa, -value.scale, &integer_part) != 0) {
            buffer[0] = '0';
            buffer[1] = '\0';
            return;
        }
    } else {
        integer_part = value.mantissa;
    }

    if (integer_part.is_negative && len + 1 < buffer_size) {
        buffer[len++] = '-';
        integer_part.is_negative = 0;
    }

    temp = integer_part;
    if (bn_is_zero(&temp)) {
        reverse_digits[rev_len++] = '0';
    } else {
        while (!bn_is_zero(&temp) && rev_len < sizeof(reverse_digits)) {
            Bignum quotient;
            unsigned int remainder;
            if (bn_divide_digit(&temp, (unsigned int)base, &quotient, &remainder) != 0) {
                break;
            }
            reverse_digits[rev_len++] = bc_digit_char((int)remainder);
            temp = quotient;
        }
    }

    while (rev_len > 0 && len + 1 < buffer_size) {
        buffer[len++] = reverse_digits[--rev_len];
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
    unsigned long long iterations = 0ULL;

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

        iterations += 1ULL;
        if (iterations > BC_MAX_LOOP_ITERATIONS) {
            bc_set_error(parser, "loop iteration limit exceeded");
            return;
        }

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
    unsigned long long iterations = 0ULL;

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

        iterations += 1ULL;
        if (iterations > BC_MAX_LOOP_ITERATIONS) {
            bc_set_error(parser, "loop iteration limit exceeded");
            return;
        }

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

    if (bc_enter_nesting(parser) != 0) {
        return;
    }

    bc_skip_statement_separators(parser);
    type = bc_peek_token(parser)->type;

    if (type == BC_TOKEN_EOF || type == BC_TOKEN_RBRACE) {
        bc_leave_nesting(parser);
        return;
    }

    if (type == BC_TOKEN_LBRACE) {
        bc_parse_block(parser, execute);
        bc_leave_nesting(parser);
        return;
    }

    if (type == BC_TOKEN_IF) {
        bc_parse_if(parser, execute);
        bc_leave_nesting(parser);
        return;
    }

    if (type == BC_TOKEN_WHILE) {
        bc_parse_while(parser, execute);
        bc_leave_nesting(parser);
        return;
    }

    if (type == BC_TOKEN_FOR) {
        bc_parse_for(parser, execute);
        bc_leave_nesting(parser);
        return;
    }

    if (type == BC_TOKEN_PRINT) {
        BcValue value;

        parser->has_token = 0;
        value = bc_parse_expression(parser, execute, 0);
        if (execute && !parser->error) {
            (void)bc_print_value(parser, value);
        }
        bc_leave_nesting(parser);
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

    bc_leave_nesting(parser);
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
    parser.nesting_depth = 0U;

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
