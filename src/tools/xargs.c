#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define XARGS_MAX_ARGS 256
#define XARGS_MAX_ARG_LENGTH 256

typedef struct {
    int zero_terminated;
    unsigned long long max_args;
    const char *replace_text;
} XargsOptions;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-0] [-n MAXARGS] [-I REPLSTR] [command [initial-args...]]");
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

static int collect_args(
    char args[XARGS_MAX_ARGS][XARGS_MAX_ARG_LENGTH],
    int *count_out,
    const XargsOptions *options
) {
    char buffer[4096];
    char current[XARGS_MAX_ARG_LENGTH];
    size_t current_len = 0;
    int count = 0;
    int preserve_empty = (options->zero_terminated || options->replace_text != 0) ? 1 : 0;
    long bytes_read;

    while ((bytes_read = platform_read(0, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];
            int is_delimiter;

            if (options->zero_terminated) {
                is_delimiter = (ch == '\0');
            } else if (options->replace_text != 0) {
                is_delimiter = (ch == '\n');
            } else {
                is_delimiter = rt_is_space(ch);
            }

            if (is_delimiter) {
                if (current_len > 0U || preserve_empty) {
                    if (append_item(args, &count, current, current_len) != 0) {
                        return -1;
                    }
                }
                current_len = 0U;
                continue;
            }

            if (options->replace_text != 0 && ch == '\r') {
                continue;
            }

            if (current_len + 1U < sizeof(current)) {
                current[current_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (current_len > 0U) {
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

static int run_with_placeholder(
    char *const base_argv[],
    int base_count,
    char input_args[XARGS_MAX_ARGS][XARGS_MAX_ARG_LENGTH],
    int input_count,
    const XargsOptions *options
) {
    char generated_args[XARGS_MAX_ARGS][XARGS_MAX_ARG_LENGTH];
    char *spawn_argv[XARGS_MAX_ARGS + 2];
    int exit_status = 0;
    int invocation_count = input_count > 0 ? input_count : 1;
    int item_index;

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
        exit_status = execute_command(spawn_argv);
        if (exit_status != 0) {
            return exit_status < 0 ? 1 : exit_status;
        }
    }

    return 0;
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
        int i;

        for (i = 0; i < base_count; ++i) {
            spawn_argv[argv_count++] = base_argv[i];
        }

        while (start < input_count && used < batch_limit) {
            spawn_argv[argv_count++] = input_args[start++];
            used += 1ULL;
        }
        spawn_argv[argv_count] = 0;

        exit_status = execute_command(spawn_argv);
        if (exit_status != 0) {
            return exit_status < 0 ? 1 : exit_status;
        }
    }

    return 0;
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
    options.max_args = 0ULL;
    options.replace_text = 0;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-0") == 0) {
            options.zero_terminated = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-n") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.max_args, "xargs", "max-args") != 0 || options.max_args == 0ULL) {
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
