#include "platform.h"
#include "runtime.h"

#define XARGS_MAX_ARGS 256
#define XARGS_MAX_ARG_LENGTH 256

static int collect_args(char args[XARGS_MAX_ARGS][XARGS_MAX_ARG_LENGTH], int *count_out) {
    char buffer[4096];
    char current[XARGS_MAX_ARG_LENGTH];
    size_t current_len = 0;
    int count = 0;
    long bytes_read;

    while ((bytes_read = platform_read(0, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            char ch = buffer[i];

            if (rt_is_space(ch)) {
                if (current_len > 0U) {
                    current[current_len] = '\0';
                    if (count >= XARGS_MAX_ARGS) {
                        return -1;
                    }
                    rt_copy_string(args[count++], XARGS_MAX_ARG_LENGTH, current);
                    current_len = 0U;
                }
            } else if (current_len + 1U < sizeof(current)) {
                current[current_len++] = ch;
            }
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    if (current_len > 0U) {
        current[current_len] = '\0';
        if (count >= XARGS_MAX_ARGS) {
            return -1;
        }
        rt_copy_string(args[count++], XARGS_MAX_ARG_LENGTH, current);
    }

    *count_out = count;
    return 0;
}

int main(int argc, char **argv) {
    char input_args[XARGS_MAX_ARGS][XARGS_MAX_ARG_LENGTH];
    char *spawn_argv[XARGS_MAX_ARGS * 2 + 2];
    int input_count = 0;
    int base_count = 0;
    int pid;
    int status;
    int i;

    if (collect_args(input_args, &input_count) != 0) {
        rt_write_line(2, "xargs: failed to read input");
        return 1;
    }

    if (argc > 1) {
        for (i = 1; i < argc; ++i) {
            spawn_argv[base_count++] = argv[i];
        }
    } else {
        spawn_argv[base_count++] = "echo";
    }

    for (i = 0; i < input_count; ++i) {
        spawn_argv[base_count + i] = input_args[i];
    }
    spawn_argv[base_count + input_count] = 0;

    if (platform_spawn_process(spawn_argv, -1, -1, 0, 0, 0, &pid) != 0) {
        rt_write_line(2, "xargs: failed to execute command");
        return 1;
    }

    if (platform_wait_process(pid, &status) != 0) {
        rt_write_line(2, "xargs: failed to wait for command");
        return 1;
    }

    return status;
}