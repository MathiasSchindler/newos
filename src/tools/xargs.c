#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define XARGS_MAX_ARGS 256
#define XARGS_MAX_ARG_LENGTH 256
#define XARGS_MAX_PROCESSES 64

typedef struct {
    int zero_terminated;
    int custom_delimiter;
    char delimiter;
    int no_run_if_empty;
    int trace;
    unsigned long long max_args;
    unsigned long long max_chars;
    unsigned long long max_procs;
    const char *replace_text;
} XargsOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(
        program_name,
        "[-0] [-r] [-t] [-d DELIM] [-n MAXARGS] [-s MAXCHARS] [-P MAXPROCS] [-I REPLSTR] [command [initial-args...]]"
    );
}

static int parse_delimiter_text(const char *text, char *delimiter_out) {
    if (text[0] == '\0') {
        return -1;
    }
    if (text[0] != '\\') {
        if (text[1] != '\0') {
            return -1;
        }
        *delimiter_out = text[0];
        return 0;
    }

    if (text[1] == '0' && text[2] == '\0') {
        *delimiter_out = '\0';
        return 0;
    }
    if (text[1] == 'n' && text[2] == '\0') {
        *delimiter_out = '\n';
        return 0;
    }
    if (text[1] == 'r' && text[2] == '\0') {
        *delimiter_out = '\r';
        return 0;
    }
    if (text[1] == 't' && text[2] == '\0') {
        *delimiter_out = '\t';
        return 0;
    }
    if (text[1] == '\\' && text[2] == '\0') {
        *delimiter_out = '\\';
        return 0;
    }

    return -1;
}

static int append_item(
    char args[XARGS_MAX_ARGS][XARGS_MAX_ARG_LENGTH],
    int *count_io,
    const char *current,
    size_t current_len
) {
    size_t copy_len = current_len;

    if (*count_io >= XARGS_MAX_ARGS) {
        return -1;
    }

    if (copy_len >= XARGS_MAX_ARG_LENGTH) {
        copy_len = XARGS_MAX_ARG_LENGTH - 1U;
    }

    memcpy(args[*count_io], current, copy_len);
    args[*count_io][copy_len] = '\0';
    *count_io += 1;
    return 0;
}

static void reset_token(size_t *current_len_io, int *token_started_io) {
    *current_len_io = 0U;
    *token_started_io = 0;
}

static int collect_args(
    char args[XARGS_MAX_ARGS][XARGS_MAX_ARG_LENGTH],
    int *count_out,
    const XargsOptions *options
) {
    char buffer[4096];
    char current[XARGS_MAX_ARG_LENGTH];
    size_t current_len = 0;
    int count = 0;
    int preserve_empty = (options->zero_terminated || options->replace_text != 0 || options->custom_delimiter) ? 1 : 0;
    int simple_delimited = options->zero_terminated || options->replace_text != 0 || options->custom_delimiter;
    char delimiter = options->zero_terminated ? '\0' : (options->replace_text != 0 ? '\n' : options->delimiter);
    int token_started = 0;
    int in_single = 0;
    int in_double = 0;
    int escape_next = 0;
    long bytes_read;

    while ((bytes_read = platform_read(0, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];

            if (simple_delimited) {
                int is_delimiter = (ch == delimiter);

                if (is_delimiter) {
                    if (current_len > 0U || token_started || preserve_empty) {
                        if (append_item(args, &count, current, current_len) != 0) {
                            return -1;
                        }
                    }
                    reset_token(&current_len, &token_started);
                    continue;
                }

                if ((options->replace_text != 0 || delimiter == '\n') && ch == '\r') {
                    continue;
                }

                token_started = 1;
                if (current_len + 1U < sizeof(current)) {
                    current[current_len++] = ch;
                }
                continue;
            }

            if (escape_next) {
                token_started = 1;
                escape_next = 0;
                if (current_len + 1U < sizeof(current)) {
                    current[current_len++] = ch;
                }
                continue;
            }

            if (in_single) {
                token_started = 1;
                if (ch == '\'') {
                    in_single = 0;
                } else if (current_len + 1U < sizeof(current)) {
                    current[current_len++] = ch;
                }
                continue;
            }

            if (in_double) {
                token_started = 1;
                if (ch == '"') {
                    in_double = 0;
                } else if (ch == '\\') {
                    escape_next = 1;
                } else if (current_len + 1U < sizeof(current)) {
                    current[current_len++] = ch;
                }
                continue;
            }

            if (rt_is_space(ch)) {
                if (current_len > 0U || token_started) {
                    if (append_item(args, &count, current, current_len) != 0) {
                        return -1;
                    }
                }
                reset_token(&current_len, &token_started);
                continue;
            }

            if (ch == '\'') {
                in_single = 1;
                token_started = 1;
                continue;
            }

            if (ch == '"') {
                in_double = 1;
                token_started = 1;
                continue;
            }

            if (ch == '\\') {
                escape_next = 1;
                token_started = 1;
                continue;
            }

            token_started = 1;
            if (current_len + 1U < sizeof(current)) {
                current[current_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (in_single || in_double || escape_next) {
        rt_write_line(2, "xargs: unmatched quote or backslash");
        return -1;
    }

    if (current_len > 0U || token_started) {
        if (append_item(args, &count, current, current_len) != 0) {
            return -1;
        }
    }

    *count_out = count;
    return 0;
}

static int text_starts_with(const char *text, const char *prefix) {
    while (*prefix != '\0') {
        if (*text != *prefix) {
            return 0;
        }
        text += 1;
        prefix += 1;
    }
    return 1;
}

static int replace_placeholder(
    const char *template_text,
    const char *placeholder,
    const char *value,
    char *buffer,
    size_t buffer_size,
    int *changed_out
) {
    size_t length = 0;
    size_t placeholder_len = rt_strlen(placeholder);
    int changed = 0;

    if (buffer_size == 0U) {
        return -1;
    }

    if (placeholder_len == 0U) {
        return -1;
    }

    while (*template_text != '\0') {
        if (text_starts_with(template_text, placeholder)) {
            size_t i = 0;
            changed = 1;
            while (value[i] != '\0') {
                if (length + 1U >= buffer_size) {
                    return -1;
                }
                buffer[length++] = value[i++];
            }
            template_text += placeholder_len;
        } else {
            if (length + 1U >= buffer_size) {
                return -1;
            }
            buffer[length++] = *template_text++;
        }
    }

    buffer[length] = '\0';
    if (changed_out != 0) {
        *changed_out = changed;
    }
    return 0;
}

static int execute_command(char *const spawn_argv[]) {
    int pid;
    int status;

    if (platform_spawn_process(spawn_argv, -1, -1, 0, 0, 0, &pid) != 0) {
        rt_write_line(2, "xargs: failed to execute command");
        return -1;
    }

    if (platform_wait_process(pid, &status) != 0) {
        rt_write_line(2, "xargs: failed to wait for command");
        return -1;
    }

    return status;
}

static void trace_command(const XargsOptions *options, char *const spawn_argv[]) {
    int i;

    if (!options->trace) {
        return;
    }

    for (i = 0; spawn_argv[i] != 0; ++i) {
        if (i > 0) {
            rt_write_char(2, ' ');
        }
        rt_write_cstr(2, spawn_argv[i]);
    }
    rt_write_char(2, '\n');
}

static int effective_parallelism(const XargsOptions *options) {
    if (options->max_procs == 0ULL || options->max_procs > (unsigned long long)XARGS_MAX_PROCESSES) {
        return XARGS_MAX_PROCESSES;
    }

    return (int)options->max_procs;
}

static void record_failure(int status, int *exit_status_io) {
    int normalized = status < 0 ? 1 : status;

    if (normalized != 0 && *exit_status_io == 0) {
        *exit_status_io = normalized;
    }
}

static int wait_for_pid(int pid) {
    int status;

    if (platform_wait_process(pid, &status) != 0) {
        rt_write_line(2, "xargs: failed to wait for command");
        return 1;
    }

    return status < 0 ? 1 : status;
}

static int spawn_only(char *const spawn_argv[], int *pid_out) {
    if (platform_spawn_process(spawn_argv, -1, -1, 0, 0, 0, pid_out) != 0) {
        rt_write_line(2, "xargs: failed to execute command");
        return -1;
    }

    return 0;
}

static void wait_for_active(int active_pids[XARGS_MAX_PROCESSES], int *active_count_io, int *exit_status_io) {
    int status;
    int i;

    if (*active_count_io <= 0) {
        return;
    }

    status = wait_for_pid(active_pids[0]);
    record_failure(status, exit_status_io);

    for (i = 1; i < *active_count_io; ++i) {
        active_pids[i - 1] = active_pids[i];
    }
    *active_count_io -= 1;
}

static int queue_command(
    char *const spawn_argv[],
    const XargsOptions *options,
    int active_pids[XARGS_MAX_PROCESSES],
    int *active_count_io,
    int *exit_status_io
) {
    int limit = effective_parallelism(options);
    int pid;

    if (limit <= 1) {
        trace_command(options, spawn_argv);
        record_failure(execute_command(spawn_argv), exit_status_io);
        return 0;
    }

    while (*active_count_io >= limit) {
        wait_for_active(active_pids, active_count_io, exit_status_io);
        if (*exit_status_io != 0) {
            return 0;
        }
    }

    if (*exit_status_io != 0) {
        return 0;
    }

    trace_command(options, spawn_argv);
    if (spawn_only(spawn_argv, &pid) != 0) {
        record_failure(1, exit_status_io);
        return -1;
    }

    active_pids[*active_count_io] = pid;
    *active_count_io += 1;
    return 0;
}

static void flush_active_commands(
    int active_pids[XARGS_MAX_PROCESSES],
    int *active_count_io,
    int *exit_status_io
) {
    while (*active_count_io > 0) {
        wait_for_active(active_pids, active_count_io, exit_status_io);
    }
}

static unsigned long long batch_chars_after_add(unsigned long long current_chars, const char *value) {
    unsigned long long length = (unsigned long long)rt_strlen(value);
    return current_chars + length + (current_chars > 0ULL ? 1ULL : 0ULL);
}

static int run_with_placeholder(
    char *const base_argv[],
    int base_count,
    char input_args[XARGS_MAX_ARGS][XARGS_MAX_ARG_LENGTH],
    int input_count,
    const XargsOptions *options
) {
    char generated_args[XARGS_MAX_ARGS][XARGS_MAX_ARG_LENGTH];
    char *spawn_argv[XARGS_MAX_ARGS + 2];
    int active_pids[XARGS_MAX_PROCESSES];
    int active_count = 0;
    int exit_status = 0;
    int invocation_count = input_count > 0 ? input_count : 1;
    int item_index;

    if (input_count == 0 && options->no_run_if_empty) {
        return 0;
    }

    for (item_index = 0; item_index < invocation_count; ++item_index) {
        const char *value = (item_index < input_count) ? input_args[item_index] : "";
        int argv_count = 0;
        int saw_replacement = 0;
        int i;

        for (i = 0; i < base_count; ++i) {
            int changed = 0;

            if (replace_placeholder(
                    base_argv[i],
                    options->replace_text,
                    value,
                    generated_args[argv_count],
                    sizeof(generated_args[argv_count]),
                    &changed
                ) != 0) {
                rt_write_line(2, "xargs: substituted argument too long");
                return 1;
            }

            if (changed) {
                saw_replacement = 1;
            }
            spawn_argv[argv_count] = generated_args[argv_count];
            argv_count += 1;
        }

        if (!saw_replacement) {
            if (argv_count >= XARGS_MAX_ARGS + 1) {
                rt_write_line(2, "xargs: too many arguments");
                return 1;
            }
            spawn_argv[argv_count++] = (char *)value;
        }

        spawn_argv[argv_count] = 0;
        if (queue_command(spawn_argv, options, active_pids, &active_count, &exit_status) != 0) {
            break;
        }
        if (exit_status != 0) {
            break;
        }
    }

    flush_active_commands(active_pids, &active_count, &exit_status);
    return exit_status;
}

static int run_in_batches(
    char *const base_argv[],
    int base_count,
    char input_args[XARGS_MAX_ARGS][XARGS_MAX_ARG_LENGTH],
    int input_count,
    const XargsOptions *options
) {
    char *spawn_argv[XARGS_MAX_ARGS + 2];
    unsigned long long batch_limit = options->max_args;
    int active_pids[XARGS_MAX_PROCESSES];
    int active_count = 0;
    int exit_status = 0;
    int start = 0;

    if (batch_limit == 0ULL || batch_limit > (unsigned long long)(XARGS_MAX_ARGS - base_count - 1)) {
        batch_limit = (unsigned long long)(XARGS_MAX_ARGS - base_count - 1);
    }
    if (batch_limit == 0ULL) {
        batch_limit = 1ULL;
    }

    if (input_count == 0) {
        int i;
        if (options->no_run_if_empty) {
            return 0;
        }
        for (i = 0; i < base_count; ++i) {
            spawn_argv[i] = base_argv[i];
        }
        spawn_argv[base_count] = 0;
        exit_status = execute_command(spawn_argv);
        return exit_status < 0 ? 1 : exit_status;
    }

    while (start < input_count) {
        int argv_count = 0;
        unsigned long long used = 0ULL;
        unsigned long long used_chars = 0ULL;
        int i;

        for (i = 0; i < base_count; ++i) {
            spawn_argv[argv_count++] = base_argv[i];
        }

        while (start < input_count && used < batch_limit) {
            if (options->max_chars > 0ULL) {
                unsigned long long next_chars = batch_chars_after_add(used_chars, input_args[start]);
                if (used > 0ULL && next_chars > options->max_chars) {
                    break;
                }
                used_chars = next_chars;
            }

            spawn_argv[argv_count++] = input_args[start++];
            used += 1ULL;
        }
        spawn_argv[argv_count] = 0;

        if (queue_command(spawn_argv, options, active_pids, &active_count, &exit_status) != 0) {
            break;
        }
        if (exit_status != 0) {
            break;
        }
    }

    flush_active_commands(active_pids, &active_count, &exit_status);
    return exit_status;
}

int main(int argc, char **argv) {
    char input_args[XARGS_MAX_ARGS][XARGS_MAX_ARG_LENGTH];
    char *base_argv[XARGS_MAX_ARGS + 1];
    XargsOptions options;
    int input_count = 0;
    int base_count = 0;
    int argi = 1;
    int i;

    options.zero_terminated = 0;
    options.custom_delimiter = 0;
    options.delimiter = '\n';
    options.no_run_if_empty = 0;
    options.trace = 0;
    options.max_args = 0ULL;
    options.max_chars = 0ULL;
    options.max_procs = 1ULL;
    options.replace_text = 0;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-0") == 0) {
            options.zero_terminated = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-r") == 0 || rt_strcmp(argv[argi], "--no-run-if-empty") == 0) {
            options.no_run_if_empty = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-t") == 0) {
            options.trace = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-d") == 0) {
            if (argi + 1 >= argc || parse_delimiter_text(argv[argi + 1], &options.delimiter) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            options.custom_delimiter = 1;
            options.zero_terminated = (options.delimiter == '\0');
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-n") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.max_args, "xargs", "max-args") != 0 || options.max_args == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-s") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.max_chars, "xargs", "max-chars") != 0 || options.max_chars == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-P") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.max_procs, "xargs", "max-procs") != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-I") == 0) {
            if (argi + 1 >= argc || argv[argi + 1][0] == '\0') {
                print_usage(argv[0]);
                return 1;
            }
            options.replace_text = argv[argi + 1];
            argi += 2;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (collect_args(input_args, &input_count, &options) != 0) {
        rt_write_line(2, "xargs: failed to read input");
        return 1;
    }

    if (argi < argc) {
        for (i = argi; i < argc; ++i) {
            base_argv[base_count++] = argv[i];
        }
    } else {
        base_argv[base_count++] = "echo";
    }
    base_argv[base_count] = 0;

    if (options.replace_text != 0) {
        return run_with_placeholder(base_argv, base_count, input_args, input_count, &options);
    }

    return run_in_batches(base_argv, base_count, input_args, input_count, &options);
}
