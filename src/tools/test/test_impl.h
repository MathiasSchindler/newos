#ifndef NEWOS_TEST_IMPL_H
#define NEWOS_TEST_IMPL_H

#include "platform.h"
#include "runtime.h"

typedef struct {
    int argc;
    char **argv;
    int index;
    int error;
} TestParser;

static int test_parse_integer(const char *text, long long *value_out) {
    long long value = 0;
    long long sign = 1;

    if (text == 0 || text[0] == '\0' || value_out == 0) {
        return -1;
    }

    if (*text == '-') {
        sign = -1;
        text += 1;
    } else if (*text == '+') {
        text += 1;
    }

    if (*text == '\0') {
        return -1;
    }

    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            return -1;
        }
        value = (value * 10) + (long long)(*text - '0');
        text += 1;
    }

    *value_out = value * sign;
    return 0;
}

static int test_path_info(const char *path, PlatformDirEntry *entry_out) {
    PlatformDirEntry local_entry;
    PlatformDirEntry *entry = (entry_out != 0) ? entry_out : &local_entry;
    return platform_get_path_info(path, entry);
}

static int test_is_unary_operator(const char *text) {
    return rt_strcmp(text, "-n") == 0 ||
           rt_strcmp(text, "-z") == 0 ||
           rt_strcmp(text, "-e") == 0 ||
           rt_strcmp(text, "-f") == 0 ||
           rt_strcmp(text, "-d") == 0 ||
           rt_strcmp(text, "-h") == 0 ||
           rt_strcmp(text, "-L") == 0 ||
           rt_strcmp(text, "-s") == 0 ||
           rt_strcmp(text, "-r") == 0 ||
           rt_strcmp(text, "-w") == 0 ||
           rt_strcmp(text, "-x") == 0 ||
           rt_strcmp(text, "-t") == 0 ||
           rt_strcmp(text, "-u") == 0 ||
           rt_strcmp(text, "-g") == 0 ||
           rt_strcmp(text, "-k") == 0;
}

static int test_is_binary_operator(const char *text) {
    return rt_strcmp(text, "=") == 0 ||
           rt_strcmp(text, "==") == 0 ||
           rt_strcmp(text, "!=") == 0 ||
           rt_strcmp(text, "<") == 0 ||
           rt_strcmp(text, ">") == 0 ||
           rt_strcmp(text, "-eq") == 0 ||
           rt_strcmp(text, "-ne") == 0 ||
           rt_strcmp(text, "-gt") == 0 ||
           rt_strcmp(text, "-ge") == 0 ||
           rt_strcmp(text, "-lt") == 0 ||
           rt_strcmp(text, "-le") == 0 ||
           rt_strcmp(text, "-nt") == 0 ||
           rt_strcmp(text, "-ot") == 0 ||
           rt_strcmp(text, "-ef") == 0;
}

static int test_eval_unary(const char *op, const char *arg, int *result_out) {
    PlatformDirEntry entry;

    if (rt_strcmp(op, "-n") == 0) {
        *result_out = (arg[0] != '\0');
        return 0;
    }
    if (rt_strcmp(op, "-z") == 0) {
        *result_out = (arg[0] == '\0');
        return 0;
    }
    if (rt_strcmp(op, "-e") == 0) {
        *result_out = (test_path_info(arg, &entry) == 0);
        return 0;
    }
    if (rt_strcmp(op, "-f") == 0) {
        *result_out = (test_path_info(arg, &entry) == 0 && !entry.is_dir);
        return 0;
    }
    if (rt_strcmp(op, "-d") == 0) {
        *result_out = (test_path_info(arg, &entry) == 0 && entry.is_dir);
        return 0;
    }
    if (rt_strcmp(op, "-h") == 0 || rt_strcmp(op, "-L") == 0) {
        *result_out = (test_path_info(arg, &entry) == 0 && (entry.mode & 0170000U) == 0120000U);
        return 0;
    }
    if (rt_strcmp(op, "-s") == 0) {
        *result_out = (test_path_info(arg, &entry) == 0 && entry.size > 0ULL);
        return 0;
    }
    if (rt_strcmp(op, "-r") == 0) {
        *result_out = (platform_path_access(arg, PLATFORM_ACCESS_READ) == 0);
        return 0;
    }
    if (rt_strcmp(op, "-w") == 0) {
        *result_out = (platform_path_access(arg, PLATFORM_ACCESS_WRITE) == 0);
        return 0;
    }
    if (rt_strcmp(op, "-x") == 0) {
        *result_out = (platform_path_access(arg, PLATFORM_ACCESS_EXECUTE) == 0);
        return 0;
    }
    if (rt_strcmp(op, "-t") == 0) {
        long long fd = 0;
        if (test_parse_integer(arg, &fd) != 0 || fd < 0) {
            return -1;
        }
        *result_out = platform_isatty((int)fd) != 0;
        return 0;
    }
    if (rt_strcmp(op, "-u") == 0) {
        *result_out = (test_path_info(arg, &entry) == 0 && (entry.mode & 04000U) != 0U);
        return 0;
    }
    if (rt_strcmp(op, "-g") == 0) {
        *result_out = (test_path_info(arg, &entry) == 0 && (entry.mode & 02000U) != 0U);
        return 0;
    }
    if (rt_strcmp(op, "-k") == 0) {
        *result_out = (test_path_info(arg, &entry) == 0 && (entry.mode & 01000U) != 0U);
        return 0;
    }

    return -1;
}

static int test_eval_binary(const char *lhs, const char *op, const char *rhs, int *result_out) {
    long long left_value = 0;
    long long right_value = 0;

    if (rt_strcmp(op, "=") == 0 || rt_strcmp(op, "==") == 0) {
        *result_out = (rt_strcmp(lhs, rhs) == 0);
        return 0;
    }
    if (rt_strcmp(op, "!=") == 0) {
        *result_out = (rt_strcmp(lhs, rhs) != 0);
        return 0;
    }
    if (rt_strcmp(op, "<") == 0) {
        *result_out = (rt_strcmp(lhs, rhs) < 0);
        return 0;
    }
    if (rt_strcmp(op, ">") == 0) {
        *result_out = (rt_strcmp(lhs, rhs) > 0);
        return 0;
    }
    if (rt_strcmp(op, "-nt") == 0 || rt_strcmp(op, "-ot") == 0 || rt_strcmp(op, "-ef") == 0) {
        PlatformDirEntry left_entry;
        PlatformDirEntry right_entry;

        if ((rt_strcmp(op, "-ef") == 0
                 ? (platform_get_path_info_follow(lhs, &left_entry) != 0 ||
                    platform_get_path_info_follow(rhs, &right_entry) != 0)
                 : (test_path_info(lhs, &left_entry) != 0 || test_path_info(rhs, &right_entry) != 0))) {
            *result_out = 0;
            return 0;
        }

        if (rt_strcmp(op, "-nt") == 0) {
            *result_out = left_entry.mtime > right_entry.mtime;
        } else if (rt_strcmp(op, "-ot") == 0) {
            *result_out = left_entry.mtime < right_entry.mtime;
        } else {
            *result_out = left_entry.device == right_entry.device && left_entry.inode == right_entry.inode;
        }
        return 0;
    }

    if (test_parse_integer(lhs, &left_value) != 0 || test_parse_integer(rhs, &right_value) != 0) {
        return -1;
    }

    if (rt_strcmp(op, "-eq") == 0) {
        *result_out = (left_value == right_value);
        return 0;
    }
    if (rt_strcmp(op, "-ne") == 0) {
        *result_out = (left_value != right_value);
        return 0;
    }
    if (rt_strcmp(op, "-gt") == 0) {
        *result_out = (left_value > right_value);
        return 0;
    }
    if (rt_strcmp(op, "-ge") == 0) {
        *result_out = (left_value >= right_value);
        return 0;
    }
    if (rt_strcmp(op, "-lt") == 0) {
        *result_out = (left_value < right_value);
        return 0;
    }
    if (rt_strcmp(op, "-le") == 0) {
        *result_out = (left_value <= right_value);
        return 0;
    }

    return -1;
}

static int test_parse_expr(TestParser *parser, int *result_out);

static int test_parse_primary(TestParser *parser, int *result_out) {
    const char *token;

    if (parser->index >= parser->argc) {
        parser->error = 1;
        return -1;
    }

    token = parser->argv[parser->index];
    if (rt_strcmp(token, "(") == 0) {
        parser->index += 1;
        if (test_parse_expr(parser, result_out) != 0) {
            return -1;
        }
        if (parser->index >= parser->argc || rt_strcmp(parser->argv[parser->index], ")") != 0) {
            parser->error = 1;
            return -1;
        }
        parser->index += 1;
        return 0;
    }

    if (test_is_unary_operator(token)) {
        if (parser->index + 1 >= parser->argc) {
            parser->error = 1;
            return -1;
        }
        if (test_eval_unary(token, parser->argv[parser->index + 1], result_out) != 0) {
            parser->error = 1;
            return -1;
        }
        parser->index += 2;
        return 0;
    }

    if (parser->index + 2 < parser->argc && test_is_binary_operator(parser->argv[parser->index + 1])) {
        if (test_eval_binary(token, parser->argv[parser->index + 1], parser->argv[parser->index + 2], result_out) != 0) {
            parser->error = 1;
            return -1;
        }
        parser->index += 3;
        return 0;
    }

    parser->index += 1;
    *result_out = (token[0] != '\0');
    return 0;
}

static int test_parse_not(TestParser *parser, int *result_out) {
    if (parser->index < parser->argc && rt_strcmp(parser->argv[parser->index], "!") == 0) {
        int inner = 0;
        parser->index += 1;
        if (test_parse_not(parser, &inner) != 0) {
            return -1;
        }
        *result_out = !inner;
        return 0;
    }

    return test_parse_primary(parser, result_out);
}

static int test_parse_and(TestParser *parser, int *result_out) {
    int result = 0;

    if (test_parse_not(parser, &result) != 0) {
        return -1;
    }

    while (parser->index < parser->argc && rt_strcmp(parser->argv[parser->index], "-a") == 0) {
        int rhs = 0;
        parser->index += 1;
        if (test_parse_not(parser, &rhs) != 0) {
            return -1;
        }
        result = result && rhs;
    }

    *result_out = result;
    return 0;
}

static int test_parse_expr(TestParser *parser, int *result_out) {
    int result = 0;

    if (test_parse_and(parser, &result) != 0) {
        return -1;
    }

    while (parser->index < parser->argc && rt_strcmp(parser->argv[parser->index], "-o") == 0) {
        int rhs = 0;
        parser->index += 1;
        if (test_parse_and(parser, &rhs) != 0) {
            return -1;
        }
        result = result || rhs;
    }

    *result_out = result;
    return 0;
}

static int test_run_expression(int argc, char **argv) {
    TestParser parser;
    int result = 0;

    if (argc == 0) {
        return 1;
    }

    parser.argc = argc;
    parser.argv = argv;
    parser.index = 0;
    parser.error = 0;

    if (test_parse_expr(&parser, &result) != 0 || parser.error || parser.index != argc) {
        return 2;
    }

    return result ? 0 : 1;
}

#endif
