#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define BC_INPUT_CAPACITY 8192

typedef struct {
    const char *text;
    size_t pos;
    int error;
    const char *message;
} BcParser;

static void bc_skip_space(BcParser *parser) {
    while (parser->text[parser->pos] == ' ' || parser->text[parser->pos] == '\t' || parser->text[parser->pos] == '\r') {
        parser->pos += 1;
    }
}

static int bc_is_stop_char(char ch) {
    return ch == '\0' || ch == '\n' || ch == ';';
}

static void bc_set_error(BcParser *parser, const char *message) {
    parser->error = 1;
    parser->message = message;
}

static long long bc_parse_expression(BcParser *parser);

static long long bc_parse_number(BcParser *parser) {
    unsigned long long value = 0;
    int saw_digit = 0;

    while (parser->text[parser->pos] >= '0' && parser->text[parser->pos] <= '9') {
        value = (value * 10ULL) + (unsigned long long)(parser->text[parser->pos] - '0');
        parser->pos += 1;
        saw_digit = 1;
    }

    if (!saw_digit) {
        bc_set_error(parser, "syntax error");
        return 0;
    }

    return (long long)value;
}

static long long bc_parse_factor(BcParser *parser) {
    char ch;

    bc_skip_space(parser);
    ch = parser->text[parser->pos];

    if (ch == '+') {
        parser->pos += 1;
        return bc_parse_factor(parser);
    }

    if (ch == '-') {
        parser->pos += 1;
        return -bc_parse_factor(parser);
    }

    if (ch == '(') {
        long long value;
        parser->pos += 1;
        value = bc_parse_expression(parser);
        bc_skip_space(parser);
        if (parser->text[parser->pos] != ')') {
            bc_set_error(parser, "missing ')'");
            return 0;
        }
        parser->pos += 1;
        return value;
    }

    return bc_parse_number(parser);
}

static long long bc_parse_term(BcParser *parser) {
    long long value = bc_parse_factor(parser);

    while (!parser->error) {
        char op;
        long long rhs;

        bc_skip_space(parser);
        op = parser->text[parser->pos];
        if (op != '*' && op != '/' && op != '%') {
            break;
        }

        parser->pos += 1;
        rhs = bc_parse_factor(parser);
        if (parser->error) {
            return 0;
        }

        if ((op == '/' || op == '%') && rhs == 0) {
            bc_set_error(parser, "division by zero");
            return 0;
        }

        if (op == '*') {
            value *= rhs;
        } else if (op == '/') {
            value /= rhs;
        } else {
            value %= rhs;
        }
    }

    return value;
}

static long long bc_parse_expression(BcParser *parser) {
    long long value = bc_parse_term(parser);

    while (!parser->error) {
        char op;
        long long rhs;

        bc_skip_space(parser);
        op = parser->text[parser->pos];
        if (op != '+' && op != '-') {
            break;
        }

        parser->pos += 1;
        rhs = bc_parse_term(parser);
        if (parser->error) {
            return 0;
        }

        if (op == '+') {
            value += rhs;
        } else {
            value -= rhs;
        }
    }

    return value;
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

static int bc_join_arguments(int argc, char **argv, char *buffer, size_t buffer_size) {
    size_t used = 0;
    int i;

    if (buffer_size == 0) {
        return -1;
    }

    buffer[0] = '\0';

    for (i = 1; i < argc; ++i) {
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
    BcParser parser;

    if (argc > 1 && rt_strcmp(argv[1], "--help") == 0) {
        tool_write_usage(tool_base_name(argv[0]), "[expression]");
        return 0;
    }

    if (argc > 1) {
        if (bc_join_arguments(argc, argv, input, sizeof(input)) != 0) {
            tool_write_error("bc", "expression too large", 0);
            return 1;
        }
    } else if (bc_read_stdin(input, sizeof(input)) != 0) {
        tool_write_error("bc", "failed to read input", 0);
        return 1;
    }

    parser.text = input;
    parser.pos = 0;
    parser.error = 0;
    parser.message = 0;

    while (input[parser.pos] != '\0') {
        long long value;

        while (bc_is_stop_char(input[parser.pos]) || input[parser.pos] == ' ' || input[parser.pos] == '\t' || input[parser.pos] == '\r') {
            parser.pos += 1;
        }

        if (input[parser.pos] == '\0') {
            break;
        }

        value = bc_parse_expression(&parser);
        bc_skip_space(&parser);

        if (parser.error || !bc_is_stop_char(input[parser.pos])) {
            tool_write_error("bc", parser.message != 0 ? parser.message : "syntax error", 0);
            return 1;
        }

        if (rt_write_int(1, value) != 0 || rt_write_char(1, '\n') != 0) {
            tool_write_error("bc", "failed to write output", 0);
            return 1;
        }

        if (input[parser.pos] == '\n' || input[parser.pos] == ';') {
            parser.pos += 1;
        }
    }

    return 0;
}
