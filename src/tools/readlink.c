#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define READLINK_CAPACITY 1024

static void trim_to_parent(const char *path, char *buffer, size_t buffer_size) {
    size_t len;

    rt_copy_string(buffer, buffer_size, path);
    len = rt_strlen(buffer);

    while (len > 1U && buffer[len - 1U] == '/') {
        buffer[len - 1U] = '\0';
        len -= 1U;
    }
    while (len > 0U && buffer[len - 1U] != '/') {
        buffer[len - 1U] = '\0';
        len -= 1U;
    }
    while (len > 1U && buffer[len - 1U] == '/') {
        buffer[len - 1U] = '\0';
        len -= 1U;
    }

    if (len == 0U) {
        rt_copy_string(buffer, buffer_size, ".");
    }
}

static int canonicalize_readlink_path(const char *path, int mode, char *buffer, size_t buffer_size) {
    if (mode == 2) {
        return tool_canonicalize_path(path, 1, 0, buffer, buffer_size);
    }
    if (mode == 3) {
        return tool_canonicalize_path(path, 1, 1, buffer, buffer_size);
    }
    if (mode == 1) {
        if (tool_path_exists(path)) {
            return tool_canonicalize_path(path, 1, 0, buffer, buffer_size);
        }

        {
            char parent[READLINK_CAPACITY];
            const char *base;

            trim_to_parent(path, parent, sizeof(parent));
            if (tool_canonicalize_path(parent, 1, 0, parent, sizeof(parent)) != 0) {
                return -1;
            }

            base = tool_base_name(path);
            return tool_join_path(parent, base, buffer, buffer_size);
        }
    }

    return -1;
}

int main(int argc, char **argv) {
    char buffer[READLINK_CAPACITY];
    int argi = 1;
    int canonicalize_mode = 0;
    int no_newline = 0;
    int zero_terminated = 0;
    int quiet = 0;
    int verbose = 0;
    int exit_code = 0;
    int i;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }

        if (rt_strcmp(argv[argi], "--canonicalize") == 0) {
            canonicalize_mode = 1;
            argi += 1;
            continue;
        } else if (rt_strcmp(argv[argi], "--canonicalize-existing") == 0) {
            canonicalize_mode = 2;
            argi += 1;
            continue;
        } else if (rt_strcmp(argv[argi], "--canonicalize-missing") == 0) {
            canonicalize_mode = 3;
            argi += 1;
            continue;
        } else if (rt_strcmp(argv[argi], "--no-newline") == 0) {
            no_newline = 1;
            argi += 1;
            continue;
        } else if (rt_strcmp(argv[argi], "--silent") == 0 || rt_strcmp(argv[argi], "--quiet") == 0) {
            quiet = 1;
            argi += 1;
            continue;
        } else if (rt_strcmp(argv[argi], "--verbose") == 0) {
            verbose = 1;
            argi += 1;
            continue;
        } else if (rt_strcmp(argv[argi], "--zero") == 0) {
            zero_terminated = 1;
            no_newline = 0;
            argi += 1;
            continue;
        }

        while (*flag != '\0') {
            if (*flag == 'n') {
                no_newline = 1;
            } else if (*flag == 'f') {
                canonicalize_mode = 1;
            } else if (*flag == 'e') {
                canonicalize_mode = 2;
            } else if (*flag == 'm') {
                canonicalize_mode = 3;
            } else if (*flag == 'z') {
                zero_terminated = 1;
                no_newline = 0;
            } else if (*flag == 'q' || *flag == 's') {
                quiet = 1;
            } else if (*flag == 'v') {
                verbose = 1;
            } else {
                rt_write_line(2, "Usage: readlink [-n] [-f|-e|-m] [-q] [-v] [-z] PATH...");
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (argi >= argc) {
        rt_write_line(2, "Usage: readlink [-n] [-f|-e|-m] [-q] [-v] [-z] PATH...");
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        int status;

        if (canonicalize_mode != 0) {
            status = canonicalize_readlink_path(argv[i], canonicalize_mode, buffer, sizeof(buffer));
        } else {
            status = platform_read_symlink(argv[i], buffer, sizeof(buffer));
        }

        if (status != 0) {
            if (!quiet) {
                rt_write_cstr(2, "readlink: cannot read ");
                rt_write_line(2, argv[i]);
            }
            exit_code = 1;
            continue;
        }

        if (zero_terminated) {
            if (verbose) {
                if (rt_write_cstr(1, argv[i]) != 0 || rt_write_cstr(1, ": ") != 0) {
                    return 1;
                }
            }
            if (rt_write_all(1, buffer, rt_strlen(buffer)) != 0 || rt_write_char(1, '\0') != 0) {
                return 1;
            }
        } else if (no_newline) {
            if (verbose) {
                if (rt_write_cstr(1, argv[i]) != 0 || rt_write_cstr(1, ": ") != 0) {
                    return 1;
                }
            }
            if (rt_write_cstr(1, buffer) != 0) {
                return 1;
            }
        } else {
            if (verbose) {
                if (rt_write_cstr(1, argv[i]) != 0 || rt_write_cstr(1, ": ") != 0) {
                    return 1;
                }
            }
            if (rt_write_line(1, buffer) != 0) {
                return 1;
            }
        }
    }

    return exit_code;
}
