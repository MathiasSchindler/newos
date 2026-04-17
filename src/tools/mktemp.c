#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define MKTEMP_PATH_CAPACITY 1024

static int contains_slash(const char *text) {
    while (text != 0 && *text != '\0') {
        if (*text == '/') {
            return 1;
        }
        text += 1;
    }
    return 0;
}

static int path_exists(const char *path) {
    PlatformDirEntry entry;
    return platform_get_path_info(path, &entry) == 0;
}

static int find_x_run(const char *text, size_t *start_out, size_t *length_out) {
    size_t i = 0;
    size_t best_start = 0;
    size_t best_length = 0;

    while (text[i] != '\0') {
        if (text[i] == 'X') {
            size_t start = i;
            while (text[i] == 'X') {
                i += 1;
            }
            if (i - start >= 3) {
                best_start = start;
                best_length = i - start;
            }
            continue;
        }
        i += 1;
    }

    if (best_length == 0) {
        return -1;
    }

    *start_out = best_start;
    *length_out = best_length;
    return 0;
}

static void fill_token(char *buffer, size_t count, unsigned long long seed) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    size_t i;

    for (i = 0; i < count; ++i) {
        buffer[count - i - 1] = alphabet[seed % (sizeof(alphabet) - 1)];
        seed /= (unsigned long long)(sizeof(alphabet) - 1);
    }
}

static int build_candidate(const char *templ, unsigned long long seed, char *buffer, size_t buffer_size) {
    size_t start = 0;
    size_t length = 0;
    size_t template_length;

    if (find_x_run(templ, &start, &length) != 0) {
        return -1;
    }

    template_length = rt_strlen(templ);
    if (template_length + 1 > buffer_size) {
        return -1;
    }

    memcpy(buffer, templ, template_length + 1);
    fill_token(buffer + start, length, seed);
    buffer[template_length] = '\0';
    return 0;
}

static int resolve_template_path(const char *directory, const char *templ, char *buffer, size_t buffer_size) {
    char local_template[MKTEMP_PATH_CAPACITY];

    if (templ == 0 || templ[0] == '\0') {
        rt_copy_string(local_template, sizeof(local_template), "tmp.XXXXXX");
        templ = local_template;
    } else if (find_x_run(templ, &(size_t){0}, &(size_t){0}) != 0) {
        size_t template_length = rt_strlen(templ);
        if (template_length + 8 > sizeof(local_template)) {
            return -1;
        }
        memcpy(local_template, templ, template_length);
        memcpy(local_template + template_length, ".XXXXXX", 8);
        templ = local_template;
    }

    if (contains_slash(templ)) {
        if (rt_strlen(templ) + 1 > buffer_size) {
            return -1;
        }
        memcpy(buffer, templ, rt_strlen(templ) + 1);
        return 0;
    }

    return tool_join_path((directory != 0 && directory[0] != '\0') ? directory : ".", templ, buffer, buffer_size);
}

static int create_file_path(const char *path) {
    int fd;

    if (path_exists(path)) {
        return -1;
    }

    fd = platform_open_write(path, 0600U);
    if (fd < 0) {
        return -1;
    }

    if (platform_close(fd) != 0) {
        return -1;
    }

    return 0;
}

static int create_directory_path(const char *path) {
    if (path_exists(path)) {
        return -1;
    }

    return platform_make_directory(path, 0700U);
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-d] [-u] [-p DIR] [TEMPLATE]");
}

int main(int argc, char **argv) {
    int make_directory = 0;
    int dry_run = 0;
    const char *directory = ".";
    const char *templ = 0;
    char template_path[MKTEMP_PATH_CAPACITY];
    char candidate[MKTEMP_PATH_CAPACITY];
    int argi = 1;
    unsigned long long seed_base = (unsigned long long)platform_get_epoch_time();
    int process_id = platform_get_process_id();
    int attempt;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-d") == 0) {
            make_directory = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-u") == 0) {
            dry_run = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-p") == 0 || rt_strcmp(argv[argi], "--tmpdir") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            directory = argv[argi + 1];
            argi += 2;
            continue;
        }
        break;
    }

    if (argc - argi > 1) {
        print_usage(argv[0]);
        return 1;
    }

    if (argi < argc) {
        templ = argv[argi];
    }

    if (resolve_template_path(directory, templ, template_path, sizeof(template_path)) != 0) {
        tool_write_error("mktemp", "invalid template", 0);
        return 1;
    }

    if (process_id > 0) {
        seed_base ^= (unsigned long long)(unsigned int)process_id;
    }

    for (attempt = 0; attempt < 256; ++attempt) {
        unsigned long long attempt_seed = seed_base + (unsigned long long)(attempt * 977U + 17U);

        if (build_candidate(template_path, attempt_seed, candidate, sizeof(candidate)) != 0) {
            tool_write_error("mktemp", "invalid template", 0);
            return 1;
        }

        if (dry_run) {
            if (!path_exists(candidate)) {
                rt_write_line(1, candidate);
                return 0;
            }
            continue;
        }

        if (make_directory) {
            if (create_directory_path(candidate) == 0) {
                rt_write_line(1, candidate);
                return 0;
            }
        } else if (create_file_path(candidate) == 0) {
            rt_write_line(1, candidate);
            return 0;
        }
    }

    tool_write_error("mktemp", "could not create unique path", 0);
    return 1;
}
