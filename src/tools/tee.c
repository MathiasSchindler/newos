#include "platform.h"
#include "runtime.h"

#define TEE_MAX_OUTPUTS 32

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-a] [file ...]");
}

int main(int argc, char **argv) {
    int output_fds[TEE_MAX_OUTPUTS];
    int output_count = 0;
    char buffer[4096];
    long bytes_read;
    int append_mode = 0;
    int argi = 1;
    int i;
    int exit_code = 0;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-a") == 0) {
            append_mode = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (argc - argi > TEE_MAX_OUTPUTS) {
        rt_write_line(2, "tee: too many output files");
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        int fd = append_mode ? platform_open_append(argv[i], 0644U) : platform_open_write(argv[i], 0644U);
        if (fd < 0) {
            rt_write_cstr(2, "tee: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }
        output_fds[output_count++] = fd;
    }

    while ((bytes_read = platform_read(0, buffer, sizeof(buffer))) > 0) {
        if (rt_write_all(1, buffer, (size_t)bytes_read) != 0) {
            exit_code = 1;
            break;
        }

        for (i = 0; i < output_count; ++i) {
            if (rt_write_all(output_fds[i], buffer, (size_t)bytes_read) != 0) {
                exit_code = 1;
            }
        }
    }

    if (bytes_read < 0) {
        rt_write_line(2, "tee: read error");
        exit_code = 1;
    }

    for (i = 0; i < output_count; ++i) {
        platform_close(output_fds[i]);
    }

    return exit_code;
}
