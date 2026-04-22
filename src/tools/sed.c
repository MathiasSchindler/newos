#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define SED_PATTERN_CAPACITY 256
#define SED_LINE_CAPACITY 8192
#define SED_MAX_COMMANDS 64

typedef enum {
    SED_COMMAND_SUBSTITUTE,
    SED_COMMAND_DELETE,
    SED_COMMAND_PRINT,
    SED_COMMAND_QUIT,
    SED_COMMAND_APPEND,
    SED_COMMAND_INSERT,
    SED_COMMAND_CHANGE
} SedCommandKind;

typedef enum {
    SED_ADDRESS_NONE,
    SED_ADDRESS_LINE,
    SED_ADDRESS_PATTERN
} SedAddressType;

typedef struct {
    SedAddressType type;
    unsigned long long line_number;
    char pattern[SED_PATTERN_CAPACITY];
} SedAddress;

typedef struct {
    SedAddress start;
    SedAddress end;
    SedCommandKind kind;
    char old_text[SED_PATTERN_CAPACITY];
    char new_text[SED_PATTERN_CAPACITY];
    char extra_text[SED_PATTERN_CAPACITY];
    int global;
    int range_active;
} SedCommand;

typedef struct {
    SedCommand commands[SED_MAX_COMMANDS];
    size_t count;
    int suppress_default_output;
} SedProgram;

typedef struct {
    int deleted;
    int quit;
    unsigned int explicit_prints;
    unsigned int before_count;
    unsigned int after_count;
    const char *before_texts[SED_MAX_COMMANDS];
    const char *after_texts[SED_MAX_COMMANDS];
} SedExecutionResult;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-n] [-i[SUFFIX]] [-f script] [expression] [file ...]");
}

static int line_contains(const char *text, const char *pattern) {
    size_t start = 0;
    size_t end = 0;
    return tool_regex_search(pattern, text, 0, 0, &start, &end);
}

static int parse_address(const char *expr, size_t *pos, SedAddress *address) {
    size_t out_len = 0;

    address->type = SED_ADDRESS_NONE;
    address->line_number = 0;
    address->pattern[0] = '\0';

    if (expr[*pos] >= '0' && expr[*pos] <= '9') {
        unsigned long long value = 0;

        while (expr[*pos] >= '0' && expr[*pos] <= '9') {
            value = value * 10ULL + (unsigned long long)(expr[*pos] - '0');
            *pos += 1;
        }

        address->type = SED_ADDRESS_LINE;
        address->line_number = value;
        return 1;
    }

    if (expr[*pos] == '/') {
        *pos += 1;
        while (expr[*pos] != '\0' && expr[*pos] != '/' && out_len + 1 < sizeof(address->pattern)) {
            address->pattern[out_len++] = expr[*pos];
            *pos += 1;
        }

        if (expr[*pos] != '/') {
            return -1;
        }

        address->pattern[out_len] = '\0';
        address->type = SED_ADDRESS_PATTERN;
        *pos += 1;
        return 1;
    }

    return 0;
}

static int parse_command_expression(const char *expr, SedCommand *command) {
    size_t pos = 0;
    int address_result;

    rt_memset(command, 0, sizeof(*command));

    address_result = parse_address(expr, &pos, &command->start);
    if (address_result < 0) {
        return -1;
    }

    if (expr[pos] == ',') {
        pos += 1;
        if (parse_address(expr, &pos, &command->end) <= 0) {
            return -1;
        }
    }

    if (expr[pos] == 's' && expr[pos + 1] != '\0') {
        char delimiter = expr[pos + 1];
        size_t old_len = 0;
        size_t new_len = 0;

        command->kind = SED_COMMAND_SUBSTITUTE;
        pos += 2;

        while (expr[pos] != '\0' && expr[pos] != delimiter && old_len + 1 < sizeof(command->old_text)) {
            command->old_text[old_len++] = expr[pos++];
        }
        if (expr[pos] != delimiter) {
            return -1;
        }
        command->old_text[old_len] = '\0';
        pos += 1;

        while (expr[pos] != '\0' && expr[pos] != delimiter && new_len + 1 < sizeof(command->new_text)) {
            command->new_text[new_len++] = expr[pos++];
        }
        if (expr[pos] != delimiter) {
            return -1;
        }
        command->new_text[new_len] = '\0';
        pos += 1;

        if (expr[pos] == 'g') {
            command->global = 1;
            pos += 1;
        }
    } else if (expr[pos] == 'd' && expr[pos + 1] == '\0') {
        command->kind = SED_COMMAND_DELETE;
        pos += 1;
    } else if (expr[pos] == 'p' && expr[pos + 1] == '\0') {
        command->kind = SED_COMMAND_PRINT;
        pos += 1;
    } else if (expr[pos] == 'q' && expr[pos + 1] == '\0') {
        command->kind = SED_COMMAND_QUIT;
        pos += 1;
    } else if (expr[pos] == 'a' || expr[pos] == 'i' || expr[pos] == 'c') {
        char op = expr[pos];

        if (op == 'a') {
            command->kind = SED_COMMAND_APPEND;
        } else if (op == 'i') {
            command->kind = SED_COMMAND_INSERT;
        } else {
            command->kind = SED_COMMAND_CHANGE;
        }

        pos += 1;
        while (expr[pos] == ' ' || expr[pos] == '\t') {
            pos += 1;
        }
        if (expr[pos] == '\\') {
            pos += 1;
        }
        while (expr[pos] == ' ' || expr[pos] == '\t') {
            pos += 1;
        }
        rt_copy_string(command->extra_text, sizeof(command->extra_text), expr + pos);
        pos = rt_strlen(expr);
    } else {
        return -1;
    }

    while (expr[pos] == ' ' || expr[pos] == '\t') {
        pos += 1;
    }

    return expr[pos] == '\0' ? 0 : -1;
}

static int add_command_expression(const char *expr, SedProgram *program) {
    char buffer[512];

    if (program->count >= SED_MAX_COMMANDS) {
        return -1;
    }

    rt_copy_string(buffer, sizeof(buffer), expr);
    tool_trim_whitespace(buffer);

    if (buffer[0] == '\0' || buffer[0] == '#') {
        return 0;
    }

    if (parse_command_expression(buffer, &program->commands[program->count]) != 0) {
        return -1;
    }

    program->count += 1;
    return 0;
}

static int load_program_text(const char *text, SedProgram *program) {
    char current[512];
    size_t current_len = 0;
    size_t i;

    for (i = 0;; ++i) {
        char ch = text[i];

        if (ch == ';' || ch == '\n' || ch == '\0') {
            current[current_len] = '\0';
            if (add_command_expression(current, program) != 0) {
                return -1;
            }
            current_len = 0;

            if (ch == '\0') {
                break;
            }
        } else if (current_len + 1 < sizeof(current)) {
            current[current_len++] = ch;
        }
    }

    return 0;
}

static int load_script_file(const char *path, SedProgram *program) {
    int fd;
    int should_close;
    char chunk[1024];
    char line[512];
    size_t line_len = 0;
    long bytes_read;

    if (tool_open_input(path, &fd, &should_close) != 0) {
        return -1;
    }

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                line[line_len] = '\0';
                if (load_program_text(line, program) != 0) {
                    tool_close_input(fd, should_close);
                    return -1;
                }
                line_len = 0;
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        tool_close_input(fd, should_close);
        return -1;
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        if (load_program_text(line, program) != 0) {
            tool_close_input(fd, should_close);
            return -1;
        }
    }

    tool_close_input(fd, should_close);
    return 0;
}

static int apply_substitution(const SedCommand *command, const char *input, char *output, size_t output_size) {
    int changed = 0;
    if (tool_regex_replace(command->old_text, command->new_text, input, 0, command->global, output, output_size, &changed) != 0) {
        return -1;
    }
    return 0;
}

static int address_matches(const SedAddress *address, unsigned long long line_no, const char *line) {
    if (address->type == SED_ADDRESS_NONE) {
        return 1;
    }
    if (address->type == SED_ADDRESS_LINE) {
        return address->line_number == line_no;
    }
    return line_contains(line, address->pattern);
}

static int command_applies(SedCommand *command, unsigned long long line_no, const char *line) {
    if (command->end.type != SED_ADDRESS_NONE) {
        if (command->range_active) {
            int end_now = address_matches(&command->end, line_no, line);

            if (end_now) {
                command->range_active = 0;
            }
            return 1;
        }

        if (address_matches(&command->start, line_no, line)) {
            if (!address_matches(&command->end, line_no, line)) {
                command->range_active = 1;
            }
            return 1;
        }

        return 0;
    }

    return address_matches(&command->start, line_no, line);
}

static void reset_program_state(SedProgram *program) {
    size_t i;

    for (i = 0; i < program->count; ++i) {
        program->commands[i].range_active = 0;
    }
}

static int process_line(SedProgram *program,
                        unsigned long long line_no,
                        const char *input,
                        char *output,
                        size_t output_size,
                        SedExecutionResult *result) {
    char current[SED_LINE_CAPACITY];
    char scratch[SED_LINE_CAPACITY];
    size_t i;

    rt_memset(result, 0, sizeof(*result));
    rt_copy_string(current, sizeof(current), input);

    for (i = 0; i < program->count; ++i) {
        if (command_applies(&program->commands[i], line_no, current)) {
            if (program->commands[i].kind == SED_COMMAND_SUBSTITUTE) {
                if (apply_substitution(&program->commands[i], current, scratch, sizeof(scratch)) != 0) {
                    return -1;
                }
                rt_copy_string(current, sizeof(current), scratch);
            } else if (program->commands[i].kind == SED_COMMAND_DELETE) {
                result->deleted = 1;
                break;
            } else if (program->commands[i].kind == SED_COMMAND_PRINT) {
                result->explicit_prints += 1U;
            } else if (program->commands[i].kind == SED_COMMAND_QUIT) {
                result->quit = 1;
                break;
            } else if (program->commands[i].kind == SED_COMMAND_INSERT) {
                if (result->before_count >= SED_MAX_COMMANDS) {
                    return -1;
                }
                result->before_texts[result->before_count++] = program->commands[i].extra_text;
            } else if (program->commands[i].kind == SED_COMMAND_APPEND) {
                if (result->after_count >= SED_MAX_COMMANDS) {
                    return -1;
                }
                result->after_texts[result->after_count++] = program->commands[i].extra_text;
            } else if (program->commands[i].kind == SED_COMMAND_CHANGE) {
                if (result->before_count >= SED_MAX_COMMANDS) {
                    return -1;
                }
                result->before_texts[result->before_count++] = program->commands[i].extra_text;
                result->deleted = 1;
                break;
            }
        }
    }

    if (rt_strlen(current) + 1 > output_size) {
        return -1;
    }

    rt_copy_string(output, output_size, current);
    return 0;
}

static int write_result_output(int output_fd, const SedProgram *program, const char *out, SedExecutionResult *result) {
    unsigned int i;

    for (i = 0U; i < result->before_count; ++i) {
        if (rt_write_line(output_fd, result->before_texts[i]) != 0) {
            return -1;
        }
    }

    if (!result->deleted) {
        while (result->explicit_prints > 0U) {
            if (rt_write_line(output_fd, out) != 0) {
                return -1;
            }
            result->explicit_prints -= 1U;
        }
        if (!program->suppress_default_output && rt_write_line(output_fd, out) != 0) {
            return -1;
        }
    }

    for (i = 0U; i < result->after_count; ++i) {
        if (rt_write_line(output_fd, result->after_texts[i]) != 0) {
            return -1;
        }
    }

    return 0;
}

static int sed_stream_to_fd(int input_fd, int output_fd, SedProgram *program) {
    char chunk[4096];
    char line[SED_LINE_CAPACITY];
    char out[SED_LINE_CAPACITY];
    SedExecutionResult result;
    size_t line_len = 0;
    unsigned long long line_no = 1;
    long bytes_read;

    reset_program_state(program);

    while ((bytes_read = platform_read(input_fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                line[line_len] = '\0';
                if (process_line(program, line_no, line, out, sizeof(out), &result) != 0) {
                    return -1;
                }
                if (write_result_output(output_fd, program, out, &result) != 0) {
                    return -1;
                }
                line_len = 0;
                line_no += 1;
                if (result.quit) {
                    return 0;
                }
            } else if (line_len + 1 < sizeof(line)) {
                line[line_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (line_len > 0) {
        line[line_len] = '\0';
        if (process_line(program, line_no, line, out, sizeof(out), &result) != 0) {
            return -1;
        }
        if (write_result_output(output_fd, program, out, &result) != 0) {
            return -1;
        }
    }

    return 0;
}

static int build_appended_path(const char *path, const char *suffix, char *buffer, size_t buffer_size) {
    size_t path_len = rt_strlen(path);
    size_t suffix_len = rt_strlen(suffix);

    if (path_len + suffix_len + 1U > buffer_size) {
        return -1;
    }

    memcpy(buffer, path, path_len);
    memcpy(buffer + path_len, suffix, suffix_len);
    buffer[path_len + suffix_len] = '\0';
    return 0;
}

static int build_temp_sidecar_path(const char *path, const char *tag, char *buffer, size_t buffer_size) {
    char pid_text[32];

    rt_unsigned_to_string((unsigned long long)platform_get_process_id(), pid_text, sizeof(pid_text));
    if (build_appended_path(path, tag, buffer, buffer_size) != 0) {
        return -1;
    }
    return build_appended_path(buffer, pid_text, buffer, buffer_size);
}

static int sed_rewrite_in_place(const char *path, SedProgram *program, const char *backup_suffix) {
    PlatformDirEntry info;
    char input_copy[1024];
    char output_copy[1024];
    char backup_path[1024];
    int input_fd = -1;
    int output_fd = -1;
    int status = -1;

    if (platform_get_path_info(path, &info) != 0) {
        return -1;
    }
    if (build_temp_sidecar_path(path, ".sedin.", input_copy, sizeof(input_copy)) != 0 ||
        build_temp_sidecar_path(path, ".sedout.", output_copy, sizeof(output_copy)) != 0) {
        return -1;
    }

    if (tool_copy_file(path, input_copy) != 0) {
        return -1;
    }

    if (backup_suffix != 0 && backup_suffix[0] != '\0') {
        if (build_appended_path(path, backup_suffix, backup_path, sizeof(backup_path)) != 0 ||
            tool_copy_file(path, backup_path) != 0) {
            (void)platform_remove_file(input_copy);
            return -1;
        }
    }

    input_fd = platform_open_read(input_copy);
    output_fd = platform_open_write(output_copy, info.mode & 0777U);
    if (input_fd < 0 || output_fd < 0) {
        goto cleanup;
    }

    if (sed_stream_to_fd(input_fd, output_fd, program) != 0) {
        goto cleanup;
    }

    platform_close(input_fd);
    platform_close(output_fd);
    input_fd = -1;
    output_fd = -1;

    if (platform_rename_path(output_copy, path) != 0) {
        goto cleanup;
    }

    status = 0;

cleanup:
    if (input_fd >= 0) {
        platform_close(input_fd);
    }
    if (output_fd >= 0) {
        platform_close(output_fd);
    }
    (void)platform_remove_file(input_copy);
    if (status != 0) {
        (void)platform_remove_file(output_copy);
    }
    return status;
}

int main(int argc, char **argv) {
    SedProgram program;
    ToolOptState s;
    int r;
    int i;
    int exit_code = 0;
    int in_place = 0;
    const char *backup_suffix = 0;

    rt_memset(&program, 0, sizeof(program));

    tool_opt_init(&s, argc, argv, tool_base_name(argv[0]),
                  "[-n] [-i[SUFFIX]] [-f script] [expression] [file ...]");
    while ((r = tool_opt_next(&s)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(s.flag, "-n") == 0) {
            program.suppress_default_output = 1;
        } else if (rt_strcmp(s.flag, "-i") == 0) {
            in_place = 1;
            backup_suffix = "";
        } else if (s.flag[1] == 'i') {
            /* -iSUFFIX: inline edit with backup suffix embedded in flag */
            in_place = 1;
            backup_suffix = s.flag + 2;
        } else if (rt_strcmp(s.flag, "-f") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            if (load_script_file(s.value, &program) != 0) {
                rt_write_cstr(2, "sed: cannot load script ");
                rt_write_line(2, s.value);
                return 1;
            }
        } else if (rt_strcmp(s.flag, "-e") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            if (load_program_text(s.value, &program) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else {
            /* Unknown flag: stop option scanning (may be implicit script) */
            s.argi -= 1;
            break;
        }
    }
    if (r == TOOL_OPT_HELP) {
        print_usage(argv[0]);
        return 0;
    }

    if (program.count == 0) {
        if (s.argi >= argc || load_program_text(argv[s.argi], &program) != 0) {
            print_usage(argv[0]);
            return 1;
        }
        s.argi += 1;
    }

    if (in_place && s.argi == argc) {
        print_usage(argv[0]);
        return 1;
    }

    if (s.argi == argc) {
        return sed_stream_to_fd(0, 1, &program) == 0 ? 0 : 1;
    }

    for (i = s.argi; i < argc; ++i) {
        int fd;
        int should_close;

        if (in_place) {
            if (rt_strcmp(argv[i], "-") == 0) {
                rt_write_line(2, "sed: -i requires file paths");
                exit_code = 1;
                continue;
            }
            if (sed_rewrite_in_place(argv[i], &program, backup_suffix) != 0) {
                rt_write_cstr(2, "sed: cannot edit ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
            }
            continue;
        }

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "sed: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (sed_stream_to_fd(fd, 1, &program) != 0) {
            rt_write_cstr(2, "sed: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
