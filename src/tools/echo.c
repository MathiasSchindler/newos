#include "runtime.h"

static int is_echo_option(const char *arg) {
    size_t i = 1;

    if (arg == 0 || arg[0] != '-' || arg[1] == '\0') {
        return 0;
    }

    while (arg[i] != '\0') {
        if (arg[i] != 'n' && arg[i] != 'e' && arg[i] != 'E') {
            return 0;
        }
        i += 1;
    }

    return 1;
}

static int hex_digit_value(char ch) {
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

static int write_escaped_text(const char *text, int *stop_output) {
    size_t i = 0;

    while (text[i] != '\0') {
        char ch = text[i];

        if (ch != '\\') {
            if (rt_write_char(1, ch) != 0) {
                return -1;
            }
            i += 1;
            continue;
        }

        i += 1;
        if (text[i] == '\0') {
            return rt_write_char(1, '\\');
        }

        ch = text[i];
        if (ch == 'a') {
            ch = '\a';
        } else if (ch == 'b') {
            ch = '\b';
        } else if (ch == 'c') {
            *stop_output = 1;
            return 0;
        } else if (ch == 'e') {
            ch = 27;
        } else if (ch == 'f') {
            ch = '\f';
        } else if (ch == 'n') {
            ch = '\n';
        } else if (ch == 'r') {
            ch = '\r';
        } else if (ch == 't') {
            ch = '\t';
        } else if (ch == 'v') {
            ch = '\v';
        } else if (ch == 'x') {
            int value = 0;
            int digits = 0;
            int digit;

            while (digits < 2 && (digit = hex_digit_value(text[i + 1])) >= 0) {
                value = (value * 16) + digit;
                i += 1;
                digits += 1;
            }

            if (digits == 0) {
                if (rt_write_char(1, '\\') != 0 || rt_write_char(1, 'x') != 0) {
                    return -1;
                }
                i += 1;
                continue;
            }

            ch = (char)value;
        } else if (ch >= '0' && ch <= '7') {
            int value = ch - '0';
            int digits = 1;

            while (digits < 3 && text[i + 1] >= '0' && text[i + 1] <= '7') {
                value = (value * 8) + (text[i + 1] - '0');
                i += 1;
                digits += 1;
            }

            ch = (char)value;
        } else if (ch != '\\') {
            if (rt_write_char(1, '\\') != 0) {
                return -1;
            }
        }

        if (rt_write_char(1, ch) != 0) {
            return -1;
        }

        i += 1;
    }

    return 0;
}

int main(int argc, char **argv) {
    int i = 1;
    int trailing_newline = 1;
    int interpret_escapes = 0;
    int first_argument = 1;
    int stop_output = 0;

    while (i < argc && is_echo_option(argv[i])) {
        size_t j = 1;
        while (argv[i][j] != '\0') {
            if (argv[i][j] == 'n') {
                trailing_newline = 0;
            } else if (argv[i][j] == 'e') {
                interpret_escapes = 1;
            } else if (argv[i][j] == 'E') {
                interpret_escapes = 0;
            }
            j += 1;
        }
        i += 1;
    }

    for (; i < argc && !stop_output; ++i) {
        if (!first_argument && rt_write_char(1, ' ') != 0) {
            return 1;
        }

        if (interpret_escapes) {
            if (write_escaped_text(argv[i], &stop_output) != 0) {
                return 1;
            }
        } else if (rt_write_cstr(1, argv[i]) != 0) {
            return 1;
        }

        first_argument = 0;
    }

    if (trailing_newline && !stop_output) {
        return rt_write_char(1, '\n') == 0 ? 0 : 1;
    }

    return 0;
}
