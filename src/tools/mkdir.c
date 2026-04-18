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

static int parse_octal_mode(const char *text, unsigned int *mode_out) {
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

static unsigned int permission_mask_for_who(unsigned int who, char permission, unsigned int current_mode) {
    unsigned int mask = 0U;
    int allow_exec = (current_mode & 0111U) != 0U;

    if (permission == 'r') {
        if ((who & 1U) != 0U) mask |= 0400U;
        if ((who & 2U) != 0U) mask |= 0040U;
        if ((who & 4U) != 0U) mask |= 0004U;
    } else if (permission == 'w') {
        if ((who & 1U) != 0U) mask |= 0200U;
        if ((who & 2U) != 0U) mask |= 0020U;
        if ((who & 4U) != 0U) mask |= 0002U;
    } else if (permission == 'x' || (permission == 'X' && allow_exec)) {
        if ((who & 1U) != 0U) mask |= 0100U;
        if ((who & 2U) != 0U) mask |= 0010U;
        if ((who & 4U) != 0U) mask |= 0001U;
    } else if (permission == 's') {
        if ((who & 1U) != 0U) mask |= 04000U;
        if ((who & 2U) != 0U) mask |= 02000U;
    } else if (permission == 't') {
        mask |= 01000U;
    }

    return mask;
}

static int apply_symbolic_mode(const char *text, unsigned int current_mode, unsigned int *mode_out) {
    unsigned int result = current_mode & 07777U;
    size_t i = 0U;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }

    while (text[i] != '\0') {
        unsigned int who = 0U;
        unsigned int set_mask = 0U;
        unsigned int clear_mask = 0U;
        char op;
        int saw_who = 0;

        while (text[i] == 'u' || text[i] == 'g' || text[i] == 'o' || text[i] == 'a') {
            saw_who = 1;
            if (text[i] == 'u') {
                who |= 1U;
            } else if (text[i] == 'g') {
                who |= 2U;
            } else if (text[i] == 'o') {
                who |= 4U;
            } else {
                who |= 7U;
            }
            i += 1U;
        }

        if (!saw_who) {
            who = 7U;
        }

        op = text[i];
        if (op != '+' && op != '-' && op != '=') {
            return -1;
        }
        i += 1U;

        while (text[i] != '\0' && text[i] != ',') {
            unsigned int mask = permission_mask_for_who(who, text[i], result | 0111U);

            if (mask == 0U && text[i] != 'X') {
                return -1;
            }
            set_mask |= mask;
            i += 1U;
        }

        if ((who & 1U) != 0U) {
            clear_mask |= 0700U | 04000U;
        }
        if ((who & 2U) != 0U) {
            clear_mask |= 0070U | 02000U;
        }
        if ((who & 4U) != 0U) {
            clear_mask |= 0007U;
        }
        if (who == 7U) {
            clear_mask |= 01000U;
        }

        if (op == '+') {
            result |= set_mask;
        } else if (op == '-') {
            result &= ~set_mask;
        } else {
            result = (result & ~clear_mask) | set_mask;
        }

        if (text[i] == ',') {
            i += 1U;
        }
    }

    *mode_out = result & 07777U;
    return 0;
}

static int parse_mode_arg(const char *text, unsigned int *mode_out) {
    if (parse_octal_mode(text, mode_out) == 0) {
        return 0;
    }
    return apply_symbolic_mode(text, 0777U, mode_out);
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
