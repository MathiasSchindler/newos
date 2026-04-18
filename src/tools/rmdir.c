#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define RMDIR_PATH_CAPACITY 1024

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-p] [-v] [--ignore-fail-on-non-empty] directory ...");
}

static void trim_trailing_slashes(char *path) {
    size_t len = rt_strlen(path);

    while (len > 1U && path[len - 1U] == '/') {
        path[len - 1U] = '\0';
        len -= 1U;
    }
}

static void print_removed(const char *path) {
    rt_write_cstr(1, "rmdir: removed directory ");
    rt_write_line(1, path);
}

static int path_is_protected(const char *path) {
    const char *base = tool_base_name(path);
    return rt_strcmp(base, ".") == 0 || rt_strcmp(base, "..") == 0 || tool_path_is_root(path);
}

static int ignore_failure_for_existing_directory(const char *path) {
    int is_directory = 0;
    return platform_path_is_directory(path, &is_directory) == 0 && is_directory;
}

static void path_parent(char *path) {
    size_t len;

    trim_trailing_slashes(path);
    len = rt_strlen(path);
    while (len > 0U && path[len - 1U] != '/') {
        path[len - 1U] = '\0';
        len -= 1U;
    }
    while (len > 1U && path[len - 1U] == '/') {
        path[len - 1U] = '\0';
        len -= 1U;
    }
    if (len == 0U) {
        rt_copy_string(path, RMDIR_PATH_CAPACITY, ".");
    }
}

static int remove_one_directory(const char *path, int remove_parents, int verbose, int ignore_fail_on_non_empty) {
    char current[RMDIR_PATH_CAPACITY];

    if (rt_strlen(path) + 1U > sizeof(current)) {
        return -1;
    }

    rt_copy_string(current, sizeof(current), path);
    trim_trailing_slashes(current);

    for (;;) {
        if (platform_remove_directory(current) != 0) {
            if (ignore_fail_on_non_empty && ignore_failure_for_existing_directory(current)) {
                return 0;
            }
            return -1;
        }
        if (verbose) {
            print_removed(current);
        }
        if (!remove_parents) {
            return 0;
        }
        path_parent(current);
        if (rt_strcmp(current, ".") == 0 || rt_strcmp(current, "/") == 0) {
            return 0;
        }
    }
}

int main(int argc, char **argv) {
    int remove_parents = 0;
    int verbose = 0;
    int ignore_fail_on_non_empty = 0;
    int exit_code = 0;
    int i;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    for (i = 1; i < argc; ++i) {
        if (rt_strcmp(argv[i], "--") == 0) {
            i += 1;
            for (; i < argc; ++i) {
                if (path_is_protected(argv[i])) {
                    rt_write_cstr(2, "rmdir: refusing to remove ");
                    rt_write_line(2, argv[i]);
                    exit_code = 1;
                } else if (remove_one_directory(argv[i], remove_parents, verbose, ignore_fail_on_non_empty) != 0) {
                    rt_write_cstr(2, "rmdir: cannot remove ");
                    rt_write_line(2, argv[i]);
                    exit_code = 1;
                }
            }
            return exit_code;
        }
        if (rt_strcmp(argv[i], "--ignore-fail-on-non-empty") == 0) {
            ignore_fail_on_non_empty = 1;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            size_t j;

            for (j = 1; argv[i][j] != '\0'; ++j) {
                if (argv[i][j] == 'p') {
                    remove_parents = 1;
                } else if (argv[i][j] == 'v') {
                    verbose = 1;
                } else {
                    print_usage(argv[0]);
                    return 1;
                }
            }
            continue;
        }
        if (path_is_protected(argv[i])) {
            rt_write_cstr(2, "rmdir: refusing to remove ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        } else if (remove_one_directory(argv[i], remove_parents, verbose, ignore_fail_on_non_empty) != 0) {
            rt_write_cstr(2, "rmdir: cannot remove ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }
    }

    return exit_code;
}
