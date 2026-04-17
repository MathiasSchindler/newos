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
    SED_COMMAND_QUIT
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
} SedExecutionResult;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-n] [-f script] [expression] [file ...]");
}

static int starts_with_at(const char *text, size_t pos, const char *pattern) {
    size_t i = 0;

    while (pattern[i] != '\0') {
        if (text[pos + i] == '\0' || text[pos + i] != pattern[i]) {
            return 0;
        }
        i += 1;
    }

    return 1;
}

static void trim_whitespace(char *text) {
    size_t start = 0;
    size_t end = rt_strlen(text);

    while (text[start] == ' ' || text[start] == '\t' || text[start] == '\r') {
        start += 1;
    }

    while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        end -= 1;
    }

    if (start > 0) {
        memmove(text, text + start, end - start);
    }

    text[end - start] = '\0';
}

static int line_contains(const char *text, const char *pattern) {
    size_t i;

    if (pattern[0] == '\0') {
        return 1;
    }

    for (i = 0; text[i] != '\0'; ++i) {
        if (starts_with_at(text, i, pattern)) {
            return 1;
        }
    }

    return 0;
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
    trim_whitespace(buffer);

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
    size_t in_pos = 0;
    size_t out_pos = 0;
    size_t old_len = rt_strlen(command->old_text);
    size_t new_len = rt_strlen(command->new_text);
    int replaced = 0;

    if (old_len == 0) {
        if (rt_strlen(input) + 1 > output_size) {
            return -1;
        }
        memcpy(output, input, rt_strlen(input) + 1);
        return 0;
    }

    while (input[in_pos] != '\0') {
        if ((!replaced || command->global) && starts_with_at(input, in_pos, command->old_text)) {
            if (out_pos + new_len + 1 > output_size) {
                return -1;
            }
            memcpy(output + out_pos, command->new_text, new_len);
            out_pos += new_len;
            in_pos += old_len;
            replaced = 1;
        } else {
            if (out_pos + 2 > output_size) {
                return -1;
            }
            output[out_pos++] = input[in_pos++];
        }
    }

    output[out_pos] = '\0';
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
            }
        }
    }

    if (rt_strlen(current) + 1 > output_size) {
        return -1;
    }

    rt_copy_string(output, output_size, current);
    return 0;
}

static int sed_stream(int fd, SedProgram *program) {
    char chunk[4096];
    char line[SED_LINE_CAPACITY];
    char out[SED_LINE_CAPACITY];
    SedExecutionResult result;
    size_t line_len = 0;
    unsigned long long line_no = 1;
    long bytes_read;

    reset_program_state(program);

    while ((bytes_read = platform_read(fd, chunk, sizeof(chunk))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                line[line_len] = '\0';
                if (process_line(program, line_no, line, out, sizeof(out), &result) != 0) {
                    return -1;
                }
                if (!result.deleted) {
                    while (result.explicit_prints > 0) {
                        if (rt_write_line(1, out) != 0) {
                            return -1;
                        }
                        result.explicit_prints -= 1U;
                    }
                    if (!program->suppress_default_output && rt_write_line(1, out) != 0) {
                        return -1;
                    }
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
        if (!result.deleted) {
            while (result.explicit_prints > 0) {
                if (rt_write_line(1, out) != 0) {
                    return -1;
                }
                result.explicit_prints -= 1U;
            }
            if (!program->suppress_default_output && rt_write_line(1, out) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    SedProgram program;
    int argi = 1;
    int i;
    int exit_code = 0;

    rt_memset(&program, 0, sizeof(program));

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-n") == 0) {
            program.suppress_default_output = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-f") == 0 && argi + 1 < argc) {
            if (load_script_file(argv[argi + 1], &program) != 0) {
                rt_write_cstr(2, "sed: cannot load script ");
                rt_write_line(2, argv[argi + 1]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-e") == 0 && argi + 1 < argc) {
            if (load_program_text(argv[argi + 1], &program) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else {
            break;
        }
    }

    if (program.count == 0) {
        if (argi >= argc || load_program_text(argv[argi], &program) != 0) {
            print_usage(argv[0]);
            return 1;
        }
        argi += 1;
    }

    if (argi == argc) {
        return sed_stream(0, &program) == 0 ? 0 : 1;
    }

    for (i = argi; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "sed: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (sed_stream(fd, &program) != 0) {
            rt_write_cstr(2, "sed: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
