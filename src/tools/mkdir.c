#include "platform.h"
#include "runtime.h"

#define MKDIR_PATH_CAPACITY 1024
#define MKDIR_DEFAULT_MODE 0755U

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-p] [-v] [-m mode] directory ...");
}

static int path_is_directory(const char *path) {
    int is_directory = 0;
    return platform_path_is_directory(path, &is_directory) == 0 && is_directory;
}

static int parse_mode_arg(const char *text, unsigned int *mode_out) {
    unsigned long long value = 0;
    size_t i = 0;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }

    while (text[i] != '\0') {
        if (text[i] < '0' || text[i] > '7') {
            return -1;
        }
        value = (value * 8ULL) + (unsigned long long)(text[i] - '0');
        if (value > 07777U) {
            return -1;
        }
        i += 1U;
    }

    *mode_out = (unsigned int)value;
    return 0;
}

static int make_one_directory(const char *path, int create_parents, unsigned int mode) {
    if (!create_parents) {
        return platform_make_directory(path, mode) == 0 ? 0 : -1;
    }

    {
        char buffer[MKDIR_PATH_CAPACITY];
        size_t len = rt_strlen(path);
        size_t i;

        if (len + 1 > sizeof(buffer)) {
            return -1;
        }

        memcpy(buffer, path, len + 1);

        for (i = 1; buffer[i] != '\0'; ++i) {
            if (buffer[i] == '/') {
                buffer[i] = '\0';
                if (buffer[0] != '\0' && !path_is_directory(buffer)) {
                    if (platform_make_directory(buffer, mode) != 0 && !path_is_directory(buffer)) {
                        return -1;
                    }
                }
                buffer[i] = '/';
            }
        }

        if (platform_make_directory(buffer, mode) != 0 && !path_is_directory(buffer)) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    int create_parents = 0;
    int verbose = 0;
    unsigned int mode = MKDIR_DEFAULT_MODE;
    int first_path_index = 1;
    int exit_code = 0;
    int i;

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        size_t j;

        if (rt_strcmp(arg, "--") == 0) {
            first_path_index = i + 1;
            break;
        }

        if (arg[0] != '-' || arg[1] == '\0') {
            first_path_index = i;
            break;
        }

        for (j = 1; arg[j] != '\0'; ++j) {
            if (arg[j] == 'p') {
                create_parents = 1;
            } else if (arg[j] == 'v') {
                verbose = 1;
            } else if (arg[j] == 'm') {
                const char *mode_text = (arg[j + 1] != '\0') ? (arg + j + 1) : ((i + 1 < argc) ? argv[++i] : 0);
                if (parse_mode_arg(mode_text, &mode) != 0) {
                    print_usage(argv[0]);
                    return 1;
                }
                break;
            } else {
                print_usage(argv[0]);
                return 1;
            }
        }
        first_path_index = i + 1;
    }

    if (first_path_index >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    for (i = first_path_index; i < argc; ++i) {
        if (make_one_directory(argv[i], create_parents, mode) != 0) {
            rt_write_cstr(2, "mkdir: cannot create ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        } else if (verbose) {
            rt_write_cstr(1, "created directory ");
            rt_write_line(1, argv[i]);
        }
    }

    return exit_code;
}
