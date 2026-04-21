#include "preprocessor.h"

#include "platform.h"
#include "runtime.h"
#include "source.h"
#include "tool_util.h"

#define COMPILER_MAX_PREPROCESS_DEPTH 32
#define COMPILER_MAX_LINE_LENGTH 4096
#define COMPILER_MAX_CONDITIONAL_DEPTH 64

typedef struct {
    int parent_active;
    int current_active;
    int branch_taken;
} CompilerConditionalFrame;

static int is_identifier_start(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

static int is_identifier_continue(char ch) {
    return is_identifier_start(ch) || (ch >= '0' && ch <= '9');
}

static size_t text_length(const char *text) {
    return rt_strlen(text);
}

static void set_error(CompilerPreprocessor *preprocessor, const char *path, unsigned long long line, const char *message) {
    rt_copy_string(preprocessor->error_path, sizeof(preprocessor->error_path), path != 0 ? path : "");
    preprocessor->error_line = line;
    rt_copy_string(preprocessor->error_message, sizeof(preprocessor->error_message), message != 0 ? message : "preprocessor error");
}

static int append_char(CompilerSource *source_out, size_t *offset, char ch) {
    if (*offset + 1 >= sizeof(source_out->data)) {
        return -1;
    }

    source_out->data[*offset] = ch;
    *offset += 1;
    source_out->data[*offset] = '\0';
    return 0;
}

static int append_text(CompilerSource *source_out, size_t *offset, const char *text) {
    size_t i = 0;

    while (text[i] != '\0') {
        if (append_char(source_out, offset, text[i]) != 0) {
            return -1;
        }
        i += 1;
    }

    return 0;
}

static int append_text_to_buffer(char *buffer, size_t buffer_size, size_t *offset, const char *text) {
    size_t i = 0;

    while (text[i] != '\0') {
        if (*offset + 1U >= buffer_size) {
            return -1;
        }
        buffer[*offset] = text[i];
        *offset += 1U;
        i += 1;
    }

    buffer[*offset] = '\0';
    return 0;
}

static int build_replacement1(char *buffer,
                              size_t buffer_size,
                              const char *prefix,
                              const char *arg,
                              const char *suffix) {
    size_t offset = 0;

    if (buffer_size == 0) {
        return -1;
    }
    buffer[0] = '\0';

    return append_text_to_buffer(buffer, buffer_size, &offset, prefix) == 0 &&
           append_text_to_buffer(buffer, buffer_size, &offset, arg) == 0 &&
           append_text_to_buffer(buffer, buffer_size, &offset, suffix) == 0 ? 0 : -1;
}

static int build_replacement2(char *buffer,
                              size_t buffer_size,
                              const char *prefix,
                              const char *arg0,
                              const char *middle,
                              const char *arg1,
                              const char *suffix) {
    size_t offset = 0;

    if (buffer_size == 0) {
        return -1;
    }
    buffer[0] = '\0';

    return append_text_to_buffer(buffer, buffer_size, &offset, prefix) == 0 &&
           append_text_to_buffer(buffer, buffer_size, &offset, arg0) == 0 &&
           append_text_to_buffer(buffer, buffer_size, &offset, middle) == 0 &&
           append_text_to_buffer(buffer, buffer_size, &offset, arg1) == 0 &&
           append_text_to_buffer(buffer, buffer_size, &offset, suffix) == 0 ? 0 : -1;
}

static const char *skip_spaces(const char *text) {
    while (*text == ' ' || *text == '\t') {
        text += 1;
    }
    return text;
}

static void trim_trailing_spaces(char *text) {
    size_t length = text_length(text);

    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[length - 1] = '\0';
        length -= 1;
    }
}

static int path_exists(const char *path) {
    int fd = platform_open_read(path);
    if (fd < 0) {
        return 0;
    }
    (void)platform_close(fd);
    return 1;
}

static void get_directory_name(const char *path, char *buffer, size_t buffer_size) {
    size_t length = text_length(path);
    size_t i;

    if (length == 0 || length + 1 > buffer_size) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    rt_copy_string(buffer, buffer_size, path);
    for (i = length; i > 0; --i) {
        if (buffer[i - 1] == '/') {
            if (i == 1) {
                buffer[1] = '\0';
            } else {
                buffer[i - 1] = '\0';
            }
            return;
        }
    }

    rt_copy_string(buffer, buffer_size, ".");
}

static int names_equal(const char *lhs, const char *rhs) {
    size_t i = 0;
    while (lhs[i] != '\0' && rhs[i] != '\0') {
        if (lhs[i] != rhs[i]) {
            return 0;
        }
        i += 1;
    }
    return lhs[i] == rhs[i];
}

static int expand_text(
    CompilerPreprocessor *preprocessor,
    const char *input,
    CompilerSource *source_out,
    size_t *offset,
    int depth,
    int *in_block_comment
);

static int find_macro(const CompilerPreprocessor *preprocessor, const char *name) {
    size_t i;

    for (i = 0; i < preprocessor->macro_count; ++i) {
        if (preprocessor->macros[i].defined && names_equal(preprocessor->macros[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static void copy_trimmed_slice(const char *start, size_t length, char *buffer, size_t buffer_size) {
    while (length > 0 && (*start == ' ' || *start == '\t')) {
        start += 1;
        length -= 1U;
    }
    while (length > 0 && (start[length - 1] == ' ' || start[length - 1] == '\t')) {
        length -= 1U;
    }
    if (length + 1U > buffer_size) {
        length = buffer_size - 1U;
    }
    memcpy(buffer, start, length);
    buffer[length] = '\0';
}

static int parse_macro_arguments(const char *input,
                                 size_t open_paren,
                                 size_t *end_out,
                                 char args[][COMPILER_MAX_LINE_LENGTH],
                                 size_t max_args,
                                 size_t *arg_count_out) {
    size_t i = open_paren + 1U;
    size_t arg_start = i;
    size_t arg_count = 0;
    int depth = 1;
    int in_string = 0;
    int in_char = 0;

    while (input[i] != '\0') {
        if ((in_string || in_char) && input[i] == '\\' && input[i + 1] != '\0') {
            i += 2U;
            continue;
        }
        if (!in_char && input[i] == '"') {
            in_string = !in_string;
            i += 1U;
            continue;
        }
        if (!in_string && input[i] == '\'') {
            in_char = !in_char;
            i += 1U;
            continue;
        }
        if (!in_string && !in_char) {
            if (input[i] == '(') {
                depth += 1;
            } else if (input[i] == ')') {
                depth -= 1;
                if (depth == 0) {
                    if (arg_count < max_args) {
                        copy_trimmed_slice(input + arg_start, i - arg_start, args[arg_count], COMPILER_MAX_LINE_LENGTH);
                    }
                    *arg_count_out = (arg_count == 0 && i == arg_start) ? 0U : (arg_count + 1U);
                    *end_out = i + 1U;
                    return 0;
                }
            } else if (input[i] == ',' && depth == 1) {
                if (arg_count >= max_args) {
                    return -1;
                }
                copy_trimmed_slice(input + arg_start, i - arg_start, args[arg_count], COMPILER_MAX_LINE_LENGTH);
                arg_count += 1U;
                arg_start = i + 1U;
            }
        }
        i += 1U;
    }

    return -1;
}

static int substitute_macro_parameter(const CompilerMacro *macro,
                                      const char *argument,
                                      char *buffer,
                                      size_t buffer_size) {
    size_t in = 0;
    size_t out = 0;

    while (macro->value[in] != '\0' && out + 1U < buffer_size) {
        if (is_identifier_start(macro->value[in])) {
            char identifier[COMPILER_MACRO_NAME_CAPACITY];
            size_t length = 0;
            size_t start = in;

            while (is_identifier_continue(macro->value[in]) && length + 1U < sizeof(identifier)) {
                identifier[length++] = macro->value[in++];
            }
            identifier[length] = '\0';

            if (names_equal(identifier, macro->parameter_name)) {
                size_t arg_index = 0;
                while (argument[arg_index] != '\0' && out + 1U < buffer_size) {
                    buffer[out++] = argument[arg_index++];
                }
            } else {
                while (start < in && out + 1U < buffer_size) {
                    buffer[out++] = macro->value[start++];
                }
            }
            continue;
        }

        buffer[out++] = macro->value[in++];
    }

    buffer[out] = '\0';
    return macro->value[in] == '\0' ? 0 : -1;
}

static int try_expand_builtin_macro(CompilerPreprocessor *preprocessor,
                                    const char *input,
                                    size_t *index_inout,
                                    CompilerSource *source_out,
                                    size_t *offset,
                                    int depth,
                                    int *in_block_comment) {
    char name[COMPILER_MACRO_NAME_CAPACITY];
    char args[2][COMPILER_MAX_LINE_LENGTH];
    char replacement[COMPILER_MAX_LINE_LENGTH];
    size_t i = *index_inout;
    size_t name_length = 0;
    size_t cursor;
    size_t end = 0;
    size_t arg_count = 0;
    int nested_block_comment = 0;

    while (is_identifier_continue(input[i]) && name_length + 1U < sizeof(name)) {
        name[name_length++] = input[i++];
    }
    name[name_length] = '\0';

    cursor = i;
    while (input[cursor] == ' ' || input[cursor] == '\t') {
        cursor += 1U;
    }
    if (input[cursor] != '(' || parse_macro_arguments(input, cursor, &end, args, sizeof(args) / sizeof(args[0]), &arg_count) != 0) {
        return 0;
    }

    replacement[0] = '\0';
    if (arg_count == 1U) {
        if (names_equal(name, "WIFEXITED")) {
            if (build_replacement1(replacement, sizeof(replacement), "((((", args[0], ")) & 0x7f) == 0)") != 0) {
                return -1;
            }
        } else if (names_equal(name, "WEXITSTATUS")) {
            if (build_replacement1(replacement, sizeof(replacement), "(((((", args[0], ")) >> 8) & 0xff))") != 0) {
                return -1;
            }
        } else if (names_equal(name, "WIFSIGNALED")) {
            if (build_replacement1(replacement, sizeof(replacement), "((((((", args[0], ")) & 0x7f) + 1) >= 2))") != 0) {
                return -1;
            }
        } else if (names_equal(name, "WTERMSIG")) {
            if (build_replacement1(replacement, sizeof(replacement), "((((", args[0], ")) & 0x7f))") != 0) {
                return -1;
            }
        } else if (names_equal(name, "S_ISDIR")) {
            if (build_replacement1(replacement, sizeof(replacement), "((((", args[0], ")) & 0xF000) == 0x4000)") != 0) {
                return -1;
            }
        } else if (names_equal(name, "S_ISLNK")) {
            if (build_replacement1(replacement, sizeof(replacement), "((((", args[0], ")) & 0xF000) == 0xA000)") != 0) {
                return -1;
            }
        } else if (names_equal(name, "S_ISCHR")) {
            if (build_replacement1(replacement, sizeof(replacement), "((((", args[0], ")) & 0xF000) == 0x2000)") != 0) {
                return -1;
            }
        } else if (names_equal(name, "S_ISBLK")) {
            if (build_replacement1(replacement, sizeof(replacement), "((((", args[0], ")) & 0xF000) == 0x6000)") != 0) {
                return -1;
            }
        } else if (names_equal(name, "S_ISFIFO")) {
            if (build_replacement1(replacement, sizeof(replacement), "((((", args[0], ")) & 0xF000) == 0x1000)") != 0) {
                return -1;
            }
        } else if (names_equal(name, "S_ISSOCK")) {
            if (build_replacement1(replacement, sizeof(replacement), "((((", args[0], ")) & 0xF000) == 0xC000)") != 0) {
                return -1;
            }
        } else if (names_equal(name, "FD_ZERO")) {
            if (build_replacement1(replacement, sizeof(replacement), "posix_fd_zero((void *)(", args[0], "))") != 0) {
                return -1;
            }
        }
    } else if (arg_count == 2U) {
        if (names_equal(name, "FD_SET")) {
            if (build_replacement2(replacement, sizeof(replacement), "posix_fd_set_bit((void *)(", args[1], "), (", args[0], "))") != 0) {
                return -1;
            }
        } else if (names_equal(name, "FD_ISSET")) {
            if (build_replacement2(replacement, sizeof(replacement), "posix_fd_is_set_bit((const void *)(", args[1], "), (", args[0], "))") != 0) {
                return -1;
            }
        }
    }

    if (replacement[0] == '\0') {
        return 0;
    }

    *index_inout = end;
    if (expand_text(preprocessor, replacement, source_out, offset, depth + 1, &nested_block_comment) != 0) {
        return -1;
    }
    (void)in_block_comment;
    return 1;
}

void compiler_preprocessor_init(CompilerPreprocessor *preprocessor) {
    rt_memset(preprocessor, 0, sizeof(*preprocessor));
}

int compiler_preprocessor_add_include_dir(CompilerPreprocessor *preprocessor, const char *path) {
    if (preprocessor->include_dir_count >= COMPILER_MAX_INCLUDE_DIRS) {
        return -1;
    }

    rt_copy_string(
        preprocessor->include_dirs[preprocessor->include_dir_count],
        sizeof(preprocessor->include_dirs[preprocessor->include_dir_count]),
        path
    );
    preprocessor->include_dir_count += 1U;
    return 0;
}

int compiler_preprocessor_define(CompilerPreprocessor *preprocessor, const char *name, const char *value) {
    int index = find_macro(preprocessor, name);

    if (index < 0) {
        if (preprocessor->macro_count >= COMPILER_MAX_MACROS) {
            return -1;
        }
        index = (int)preprocessor->macro_count;
        preprocessor->macro_count += 1U;
    }

    rt_copy_string(preprocessor->macros[index].name, sizeof(preprocessor->macros[index].name), name);
    rt_copy_string(preprocessor->macros[index].value, sizeof(preprocessor->macros[index].value), value != 0 ? value : "");
    preprocessor->macros[index].parameter_name[0] = '\0';
    preprocessor->macros[index].defined = 1;
    preprocessor->macros[index].is_function_like = 0;
    return 0;
}

static int define_function_like_macro(CompilerPreprocessor *preprocessor,
                                      const char *name,
                                      const char *parameter_name,
                                      const char *value) {
    int index = find_macro(preprocessor, name);

    if (index < 0) {
        if (preprocessor->macro_count >= COMPILER_MAX_MACROS) {
            return -1;
        }
        index = (int)preprocessor->macro_count;
        preprocessor->macro_count += 1U;
    }

    rt_copy_string(preprocessor->macros[index].name, sizeof(preprocessor->macros[index].name), name);
    rt_copy_string(preprocessor->macros[index].value, sizeof(preprocessor->macros[index].value), value != 0 ? value : "");
    rt_copy_string(preprocessor->macros[index].parameter_name, sizeof(preprocessor->macros[index].parameter_name), parameter_name != 0 ? parameter_name : "");
    preprocessor->macros[index].defined = 1;
    preprocessor->macros[index].is_function_like = 1;
    return 0;
}

int compiler_preprocessor_undefine(CompilerPreprocessor *preprocessor, const char *name) {
    int index = find_macro(preprocessor, name);

    if (index < 0) {
        return 0;
    }

    preprocessor->macros[index].defined = 0;
    preprocessor->macros[index].is_function_like = 0;
    preprocessor->macros[index].name[0] = '\0';
    preprocessor->macros[index].value[0] = '\0';
    preprocessor->macros[index].parameter_name[0] = '\0';
    return 0;
}

static int evaluate_expr(CompilerPreprocessor *preprocessor, const char *expr, int *value_out) {
    char name[COMPILER_MACRO_NAME_CAPACITY];
    char number[COMPILER_MACRO_VALUE_CAPACITY];
    const char *cursor = skip_spaces(expr);
    size_t length = 0;
    unsigned long long number_value = 0;
    int negate = 0;
    int index;

    while (*cursor == '!') {
        negate = !negate;
        cursor = skip_spaces(cursor + 1);
    }

    if (cursor[0] == '(') {
        size_t expr_len = text_length(cursor);
        if (expr_len >= 2 && cursor[expr_len - 1] == ')') {
            char inner[COMPILER_MACRO_VALUE_CAPACITY];
            size_t i;
            if (expr_len - 1 >= sizeof(inner)) {
                return -1;
            }
            for (i = 1; i + 1 < expr_len; ++i) {
                inner[i - 1] = cursor[i];
            }
            inner[expr_len - 2] = '\0';
            if (evaluate_expr(preprocessor, inner, value_out) != 0) {
                return -1;
            }
            if (negate) {
                *value_out = !*value_out;
            }
            return 0;
        }
    }

    if (cursor[0] == 'd' && cursor[1] == 'e' && cursor[2] == 'f' && cursor[3] == 'i' && cursor[4] == 'n' && cursor[5] == 'e' && cursor[6] == 'd') {
        cursor = skip_spaces(cursor + 7);
        if (*cursor == '(') {
            cursor = skip_spaces(cursor + 1);
        }
        while (is_identifier_continue(*cursor) && length + 1 < sizeof(name)) {
            name[length++] = *cursor++;
        }
        name[length] = '\0';
        *value_out = find_macro(preprocessor, name) >= 0 ? 1 : 0;
        if (negate) {
            *value_out = !*value_out;
        }
        return 0;
    }

    if ((*cursor >= '0' && *cursor <= '9') || ((*cursor == '+' || *cursor == '-') && cursor[1] >= '0' && cursor[1] <= '9')) {
        rt_copy_string(number, sizeof(number), cursor);
        trim_trailing_spaces(number);
        if (rt_parse_uint(number[0] == '+' ? number + 1 : number, &number_value) != 0) {
            return -1;
        }
        *value_out = (int)number_value;
        if (number[0] == '-') {
            *value_out = -*value_out;
        }
        if (negate) {
            *value_out = !*value_out;
        }
        return 0;
    }

    while (is_identifier_continue(*cursor) && length + 1 < sizeof(name)) {
        name[length++] = *cursor++;
    }
    name[length] = '\0';

    if (name[0] == '\0') {
        return -1;
    }

    index = find_macro(preprocessor, name);
    if (index < 0 || preprocessor->macros[index].value[0] == '\0') {
        *value_out = 0;
    } else {
        rt_copy_string(number, sizeof(number), preprocessor->macros[index].value);
        trim_trailing_spaces(number);
        if (rt_parse_uint(number, &number_value) != 0) {
            *value_out = 1;
        } else {
            *value_out = (int)number_value;
        }
    }

    if (negate) {
        *value_out = !*value_out;
    }
    return 0;
}

static int expand_text(
    CompilerPreprocessor *preprocessor,
    const char *input,
    CompilerSource *source_out,
    size_t *offset,
    int depth,
    int *in_block_comment
) {
    size_t i = 0;

    if (depth > 16) {
        return -1;
    }

    while (input[i] != '\0') {
        if (in_block_comment != 0 && *in_block_comment) {
            if (input[i] == '*' && input[i + 1] == '/') {
                *in_block_comment = 0;
                i += 2;
                if (append_char(source_out, offset, ' ') != 0) {
                    return -1;
                }
                continue;
            }
            i += 1;
            continue;
        }

        if (input[i] == '"' || input[i] == '\'') {
            char quote = input[i++];
            if (append_char(source_out, offset, quote) != 0) {
                return -1;
            }
            while (input[i] != '\0') {
                if (append_char(source_out, offset, input[i]) != 0) {
                    return -1;
                }
                if (input[i] == '\\' && input[i + 1] != '\0') {
                    i += 1;
                    if (append_char(source_out, offset, input[i]) != 0) {
                        return -1;
                    }
                } else if (input[i] == quote) {
                    i += 1;
                    break;
                }
                i += 1;
            }
            continue;
        }

        if (input[i] == '/' && input[i + 1] == '/') {
            break;
        }

        if (input[i] == '/' && input[i + 1] == '*') {
            if (append_char(source_out, offset, ' ') != 0) {
                return -1;
            }
            if (in_block_comment != 0) {
                *in_block_comment = 1;
            }
            i += 2;
            continue;
        }

        if (is_identifier_start(input[i])) {
            char name[COMPILER_MACRO_NAME_CAPACITY];
            size_t length = 0;
            int index;
            int builtin_result = try_expand_builtin_macro(preprocessor, input, &i, source_out, offset, depth, in_block_comment);

            if (builtin_result < 0) {
                return -1;
            }
            if (builtin_result > 0) {
                continue;
            }

            while (is_identifier_continue(input[i]) && length + 1 < sizeof(name)) {
                name[length++] = input[i++];
            }
            name[length] = '\0';

            index = find_macro(preprocessor, name);
            if (index >= 0) {
                int nested_block_comment = 0;
                if (preprocessor->macros[index].is_function_like) {
                    char args[8][COMPILER_MAX_LINE_LENGTH];
                    char expanded[COMPILER_MAX_LINE_LENGTH];
                    size_t call_cursor = i;
                    size_t end = 0;
                    size_t arg_count = 0;

                    while (input[call_cursor] == ' ' || input[call_cursor] == '\t') {
                        call_cursor += 1U;
                    }
                    if (input[call_cursor] != '(' ||
                        parse_macro_arguments(input, call_cursor, &end, args, sizeof(args) / sizeof(args[0]), &arg_count) != 0) {
                        if (append_text(source_out, offset, name) != 0) {
                            return -1;
                        }
                    } else {
                        if (preprocessor->macros[index].parameter_name[0] == '\0') {
                            rt_copy_string(expanded, sizeof(expanded), preprocessor->macros[index].value);
                        } else if (arg_count == 1U &&
                                   substitute_macro_parameter(&preprocessor->macros[index], args[0], expanded, sizeof(expanded)) == 0) {
                            /* substituted into expanded */
                        } else {
                            if (append_text(source_out, offset, name) != 0) {
                                return -1;
                            }
                            continue;
                        }
                        i = end;
                        if (expand_text(preprocessor, expanded, source_out, offset, depth + 1, &nested_block_comment) != 0) {
                            return -1;
                        }
                    }
                } else if (expand_text(preprocessor, preprocessor->macros[index].value, source_out, offset, depth + 1, &nested_block_comment) != 0) {
                    return -1;
                }
            } else {
                if (append_text(source_out, offset, name) != 0) {
                    return -1;
                }
            }
            continue;
        }

        if (append_char(source_out, offset, input[i]) != 0) {
            return -1;
        }
        i += 1;
    }

    return 0;
}

static int resolve_include_path(
    const CompilerPreprocessor *preprocessor,
    const char *name,
    const char *current_dir,
    int quoted,
    char *resolved_path,
    size_t resolved_size
) {
    size_t i;

    if (quoted && tool_join_path(current_dir, name, resolved_path, resolved_size) == 0 && path_exists(resolved_path)) {
        return 0;
    }

    for (i = 0; i < preprocessor->include_dir_count; ++i) {
        if (tool_join_path(preprocessor->include_dirs[i], name, resolved_path, resolved_size) == 0 && path_exists(resolved_path)) {
            return 0;
        }
    }

    if (!quoted && path_exists(name)) {
        rt_copy_string(resolved_path, resolved_size, name);
        return 0;
    }

    return -1;
}

static int preprocess_file_internal(
    CompilerPreprocessor *preprocessor,
    const char *path,
    CompilerSource *source_out,
    size_t *offset,
    int depth
);

static int handle_directive(
    CompilerPreprocessor *preprocessor,
    const char *directive_line,
    const char *path,
    const char *current_dir,
    unsigned long long line_no,
    CompilerConditionalFrame *frames,
    size_t *frame_count,
    int *active_out,
    CompilerSource *source_out,
    size_t *offset,
    int depth
) {
    char directive[32];
    char argument[COMPILER_MAX_LINE_LENGTH];
    const char *cursor = skip_spaces(directive_line + 1);
    size_t length = 0;
    size_t arg_length = 0;
    int active = *active_out;

    while (is_identifier_continue(*cursor) && length + 1 < sizeof(directive)) {
        directive[length++] = *cursor++;
    }
    directive[length] = '\0';
    cursor = skip_spaces(cursor);
    rt_copy_string(argument, sizeof(argument), cursor);
    trim_trailing_spaces(argument);

    if (names_equal(directive, "ifdef") || names_equal(directive, "ifndef") || names_equal(directive, "if")) {
        int condition = 0;
        CompilerConditionalFrame frame;

        if (*frame_count >= COMPILER_MAX_CONDITIONAL_DEPTH) {
            set_error(preprocessor, path, line_no, "conditional nesting too deep");
            return -1;
        }

        frame.parent_active = active;
        if (!active) {
            condition = 0;
        } else if (names_equal(directive, "ifdef")) {
            condition = find_macro(preprocessor, argument) >= 0 ? 1 : 0;
        } else if (names_equal(directive, "ifndef")) {
            condition = find_macro(preprocessor, argument) < 0 ? 1 : 0;
        } else if (evaluate_expr(preprocessor, argument, &condition) != 0) {
            condition = 0;
        }

        frame.current_active = frame.parent_active && condition;
        frame.branch_taken = frame.current_active;
        frames[*frame_count] = frame;
        *frame_count += 1U;
        *active_out = frame.current_active;
        return append_char(source_out, offset, '\n');
    }

    if (names_equal(directive, "elif")) {
        CompilerConditionalFrame *frame;
        int condition = 0;

        if (*frame_count == 0) {
            set_error(preprocessor, path, line_no, "unexpected #elif");
            return -1;
        }

        frame = &frames[*frame_count - 1];
        if (frame->parent_active && !frame->branch_taken) {
            if (evaluate_expr(preprocessor, argument, &condition) != 0) {
                condition = 0;
            }
        }
        frame->current_active = frame->parent_active && !frame->branch_taken && condition;
        if (frame->current_active) {
            frame->branch_taken = 1;
        }
        *active_out = frame->current_active;
        return append_char(source_out, offset, '\n');
    }

    if (names_equal(directive, "else")) {
        CompilerConditionalFrame *frame;

        if (*frame_count == 0) {
            set_error(preprocessor, path, line_no, "unexpected #else");
            return -1;
        }

        frame = &frames[*frame_count - 1];
        frame->current_active = frame->parent_active && !frame->branch_taken;
        frame->branch_taken = 1;
        *active_out = frame->current_active;
        return append_char(source_out, offset, '\n');
    }

    if (names_equal(directive, "endif")) {
        if (*frame_count == 0) {
            set_error(preprocessor, path, line_no, "unexpected #endif");
            return -1;
        }

        *frame_count -= 1U;
        *active_out = (*frame_count == 0) ? 1 : frames[*frame_count - 1].current_active;
        return append_char(source_out, offset, '\n');
    }

    if (!active) {
        return append_char(source_out, offset, '\n');
    }

    if (names_equal(directive, "define")) {
        char name[COMPILER_MACRO_NAME_CAPACITY];
        const char *arg_cursor = skip_spaces(argument);
        while (is_identifier_continue(*arg_cursor) && arg_length + 1 < sizeof(name)) {
            name[arg_length++] = *arg_cursor++;
        }
        name[arg_length] = '\0';

        if (name[0] == '\0') {
            set_error(preprocessor, path, line_no, "invalid #define");
            return -1;
        }
        if (*arg_cursor == '(') {
            char parameter_name[COMPILER_MACRO_NAME_CAPACITY];
            size_t parameter_length = 0;
            size_t parameter_count = 0;
            int simple_one_parameter = 1;

            arg_cursor += 1;
            parameter_name[0] = '\0';
            for (;;) {
                arg_cursor = skip_spaces(arg_cursor);
                if (*arg_cursor == ')') {
                    break;
                }
                if (arg_cursor[0] == '.' && arg_cursor[1] == '.' && arg_cursor[2] == '.') {
                    simple_one_parameter = 0;
                    arg_cursor += 3;
                } else {
                    size_t current_length = 0;
                    if (!is_identifier_start(*arg_cursor)) {
                        set_error(preprocessor, path, line_no, "invalid function-like macro parameters");
                        return -1;
                    }
                    if (parameter_count == 0) {
                        while (is_identifier_continue(*arg_cursor) && parameter_length + 1 < sizeof(parameter_name)) {
                            parameter_name[parameter_length++] = *arg_cursor++;
                        }
                        parameter_name[parameter_length] = '\0';
                    } else {
                        while (is_identifier_continue(*arg_cursor)) {
                            arg_cursor += 1;
                            current_length += 1U;
                        }
                        (void)current_length;
                        simple_one_parameter = 0;
                    }
                    if (parameter_count > 0) {
                        simple_one_parameter = 0;
                    }
                    parameter_count += 1U;
                }
                arg_cursor = skip_spaces(arg_cursor);
                if (*arg_cursor == ',') {
                    simple_one_parameter = 0;
                    arg_cursor += 1;
                    continue;
                }
                if (*arg_cursor == ')') {
                    break;
                }
                set_error(preprocessor, path, line_no, "invalid function-like macro parameters");
                return -1;
            }
            arg_cursor = skip_spaces(arg_cursor + 1);
            if (define_function_like_macro(preprocessor, name, simple_one_parameter ? parameter_name : "", arg_cursor) != 0) {
                set_error(preprocessor, path, line_no, "too many macros");
                return -1;
            }
            return append_char(source_out, offset, '\n');
        }
        arg_cursor = skip_spaces(arg_cursor);
        if (compiler_preprocessor_define(preprocessor, name, arg_cursor) != 0) {
            set_error(preprocessor, path, line_no, "too many macros");
            return -1;
        }
        return append_char(source_out, offset, '\n');
    }

    if (names_equal(directive, "undef")) {
        (void)compiler_preprocessor_undefine(preprocessor, argument);
        return append_char(source_out, offset, '\n');
    }

    if (names_equal(directive, "include")) {
        char include_name[COMPILER_PATH_CAPACITY];
        char resolved_path[COMPILER_PATH_CAPACITY];
        const char *arg_cursor = skip_spaces(argument);
        char terminator = '\0';

        if (*arg_cursor == '"' || *arg_cursor == '<') {
            terminator = (*arg_cursor == '"') ? '"' : '>';
            arg_cursor += 1;
            while (*arg_cursor != '\0' && *arg_cursor != terminator && arg_length + 1 < sizeof(include_name)) {
                include_name[arg_length++] = *arg_cursor++;
            }
            include_name[arg_length] = '\0';
        }

        if (include_name[0] == '\0') {
            set_error(preprocessor, path, line_no, "invalid #include");
            return -1;
        }

        if (resolve_include_path(preprocessor, include_name, current_dir, terminator == '"', resolved_path, sizeof(resolved_path)) == 0) {
            if (preprocess_file_internal(preprocessor, resolved_path, source_out, offset, depth + 1) != 0) {
                return -1;
            }
        } else if (terminator == '"') {
            set_error(preprocessor, path, line_no, "cannot resolve local include");
            return -1;
        } else {
            if (append_char(source_out, offset, '\n') != 0) {
                return -1;
            }
        }
        return 0;
    }

    return append_char(source_out, offset, '\n');
}

static int preprocess_file_internal(
    CompilerPreprocessor *preprocessor,
    const char *path,
    CompilerSource *source_out,
    size_t *offset,
    int depth
) {
    CompilerSource loaded;
    CompilerConditionalFrame frames[COMPILER_MAX_CONDITIONAL_DEPTH];
    char current_dir[COMPILER_PATH_CAPACITY];
    unsigned long long line_no = 1;
    size_t frame_count = 0;
    int active = 1;
    int in_block_comment = 0;
    size_t pos = 0;
    int load_result;

    if (depth > COMPILER_MAX_PREPROCESS_DEPTH) {
        set_error(preprocessor, path, line_no, "include nesting too deep");
        return -1;
    }

    load_result = compiler_load_source(path, &loaded);
    if (load_result != 0) {
        set_error(preprocessor, path, line_no, load_result == -2 ? "source file too large" : "cannot read source file");
        return -1;
    }

    get_directory_name(path, current_dir, sizeof(current_dir));

    while (pos < loaded.size) {
        char line[COMPILER_MAX_LINE_LENGTH];
        const char *trimmed;
        size_t line_length = 0;
        unsigned long long consumed_lines = 0;
        int continued;

        do {
            continued = 0;
            while (pos < loaded.size && loaded.data[pos] != '\n' && line_length + 1 < sizeof(line)) {
                line[line_length++] = loaded.data[pos++];
            }
            if (pos < loaded.size && loaded.data[pos] == '\n') {
                pos += 1;
                consumed_lines += 1ULL;
            }
            if (line_length > 0 && line[line_length - 1] == '\\') {
                line_length -= 1U;
                continued = 1;
            }
        } while (continued && pos < loaded.size);
        line[line_length] = '\0';

        if (consumed_lines == 0ULL) {
            consumed_lines = 1ULL;
        }

        trimmed = skip_spaces(line);
        if (trimmed[0] == '#') {
            if (handle_directive(preprocessor, trimmed, path, current_dir, line_no, frames, &frame_count, &active, source_out, offset, depth) != 0) {
                return -1;
            }
        } else if (active) {
            if (expand_text(preprocessor, line, source_out, offset, 0, &in_block_comment) != 0 || append_char(source_out, offset, '\n') != 0) {
                set_error(preprocessor, path, line_no, "preprocessed output exceeds stage0 capacity");
                return -1;
            }
        } else if (append_char(source_out, offset, '\n') != 0) {
            set_error(preprocessor, path, line_no, "preprocessed output exceeds stage0 capacity");
            return -1;
        }

        line_no += consumed_lines;
    }

    return 0;
}

int compiler_preprocess_file(CompilerPreprocessor *preprocessor, const char *path, CompilerSource *source_out) {
    size_t offset = 0;

    rt_memset(source_out, 0, sizeof(*source_out));
    rt_copy_string(source_out->path, sizeof(source_out->path), path);

    if (preprocess_file_internal(preprocessor, path, source_out, &offset, 0) != 0) {
        return -1;
    }

    source_out->size = offset;
    source_out->data[offset] = '\0';
    return 0;
}

const char *compiler_preprocessor_error_message(const CompilerPreprocessor *preprocessor) {
    return preprocessor->error_message;
}

const char *compiler_preprocessor_error_path(const CompilerPreprocessor *preprocessor) {
    return preprocessor->error_path;
}

unsigned long long compiler_preprocessor_error_line(const CompilerPreprocessor *preprocessor) {
    return preprocessor->error_line;
}
