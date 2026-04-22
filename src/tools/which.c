#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define WHICH_PATH_CAPACITY 1024

typedef struct {
    char self_dir[WHICH_PATH_CAPACITY];
    const char *path_env;
} SearchContext;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-a] COMMAND...");
}

static int contains_slash(const char *text) {
    size_t i = 0;

    while (text[i] != '\0') {
        if (text[i] == '/') {
            return 1;
        }
        i += 1U;
    }

    return 0;
}

static int path_exists_as_file(const char *path) {
    PlatformDirEntry entry;
    return platform_get_path_info(path, &entry) == 0 &&
           !entry.is_dir &&
           platform_path_access(path, PLATFORM_ACCESS_EXECUTE) == 0;
}

static int is_shell_builtin(const char *name) {
    static const char *builtins[] = {
        "cd", "exit", "jobs", "history", "fg", "bg", "export", "unset", "command", "alias"
    };
    size_t i;

    for (i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i) {
        if (rt_strcmp(name, builtins[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

static int is_exported_shell_function(const char *name) {
    size_t index = 0;
    size_t name_len = rt_strlen(name);

    for (;;) {
        const char *entry = platform_getenv_entry(index);
        const char *cursor;
        size_t matched = 0;

        if (entry == 0) {
            break;
        }
        index += 1U;

        if (!tool_starts_with(entry, "BASH_FUNC_")) {
            continue;
        }

        cursor = entry + 10;
        while (matched < name_len && cursor[matched] == name[matched]) {
            matched += 1U;
        }
        if (matched != name_len) {
            continue;
        }

        cursor += matched;
        if ((cursor[0] == '%' &&
             cursor[1] != '\0' &&
             cursor[1] == '%' &&
             cursor[2] != '\0' &&
             cursor[2] == '=') ||
            (cursor[0] == '(' &&
             cursor[1] != '\0' &&
             cursor[1] == ')' &&
             cursor[2] != '\0' &&
             cursor[2] == '=')) {
            return 1;
        }
    }

    return 0;
}

static int write_shell_descriptor(const char *name, const char *kind) {
    if (rt_write_cstr(1, name) != 0 ||
        rt_write_cstr(1, ": ") != 0 ||
        rt_write_line(1, kind) != 0) {
        return -1;
    }

    return 0;
}

static int resolve_shell_name(const char *name) {
    if (is_exported_shell_function(name)) {
        return write_shell_descriptor(name, "shell function");
    }
    if (is_shell_builtin(name)) {
        return write_shell_descriptor(name, "shell built-in");
    }
    return -1;
}

static void set_self_dir(const char *argv0, char *buffer, size_t buffer_size) {
    size_t len;
    size_t i;

    if (argv0 == 0 || argv0[0] == '\0' || !contains_slash(argv0)) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    len = rt_strlen(argv0);
    if (len + 1U > buffer_size) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    memcpy(buffer, argv0, len + 1U);
    for (i = len; i > 0; --i) {
        if (buffer[i - 1U] == '/') {
            if (i == 1U) {
                buffer[1] = '\0';
            } else {
                buffer[i - 1U] = '\0';
            }
            return;
        }
    }

    rt_copy_string(buffer, buffer_size, ".");
}

static int search_in_list(const char *dirs, const char *name, char *buffer, size_t buffer_size) {
    size_t i = 0;

    while (dirs != 0 && dirs[i] != '\0') {
        char dir[WHICH_PATH_CAPACITY];
        size_t length = 0;

        while (dirs[i] != '\0' && dirs[i] != ':') {
            if (length + 1U < sizeof(dir)) {
                dir[length++] = dirs[i];
            }
            i += 1U;
        }

        dir[length] = '\0';
        if (dirs[i] == ':') {
            i += 1U;
        }
        if (dir[0] == '\0') {
            rt_copy_string(dir, sizeof(dir), ".");
        }

        if (tool_join_path(dir, name, buffer, buffer_size) == 0 && path_exists_as_file(buffer)) {
            return 0;
        }
    }

    return -1;
}

static int resolve_command(const SearchContext *ctx, const char *name, char *buffer, size_t buffer_size) {
    if (contains_slash(name)) {
        if (path_exists_as_file(name)) {
            rt_copy_string(buffer, buffer_size, name);
            return 0;
        }
        return -1;
    }

    if (is_exported_shell_function(name)) {
        rt_copy_string(buffer, buffer_size, "shell function");
        return 1;
    }

    if (is_shell_builtin(name)) {
        rt_copy_string(buffer, buffer_size, "shell built-in");
        return 1;
    }

    if (ctx->self_dir[0] != '\0' &&
        tool_join_path(ctx->self_dir, name, buffer, buffer_size) == 0 &&
        path_exists_as_file(buffer)) {
        return 0;
    }

    if (search_in_list(ctx->path_env, name, buffer, buffer_size) == 0) {
        return 0;
    }

    return -1;
}

static int print_all_matches(const SearchContext *ctx, const char *name) {
    int found = 0;
    char path[WHICH_PATH_CAPACITY];
    size_t i = 0;

    if (contains_slash(name)) {
        if (path_exists_as_file(name)) {
            rt_write_line(1, name);
            return 0;
        }
        return -1;
    }

    if (is_exported_shell_function(name)) {
        if (write_shell_descriptor(name, "shell function") == 0) {
            found = 1;
        }
    }

    if (is_shell_builtin(name)) {
        if (write_shell_descriptor(name, "shell built-in") == 0) {
            found = 1;
        }
    }

    if (ctx->self_dir[0] != '\0' &&
        tool_join_path(ctx->self_dir, name, path, sizeof(path)) == 0 &&
        path_exists_as_file(path)) {
        rt_write_line(1, path);
        found = 1;
    }

    while (ctx->path_env != 0 && ctx->path_env[i] != '\0') {
        char dir[WHICH_PATH_CAPACITY];
        size_t length = 0;

        while (ctx->path_env[i] != '\0' && ctx->path_env[i] != ':') {
            if (length + 1U < sizeof(dir)) {
                dir[length++] = ctx->path_env[i];
            }
            i += 1U;
        }
        dir[length] = '\0';
        if (ctx->path_env[i] == ':') {
            i += 1U;
        }
        if (dir[0] == '\0') {
            rt_copy_string(dir, sizeof(dir), ".");
        }
        if (tool_join_path(dir, name, path, sizeof(path)) == 0 && path_exists_as_file(path)) {
            rt_write_line(1, path);
            found = 1;
        }
    }

    return found ? 0 : -1;
}

int main(int argc, char **argv) {
    SearchContext ctx;
    int i;
    int exit_code = 0;
    int print_all = 0;
    int argi = 1;

    set_self_dir((argc > 0) ? argv[0] : "which", ctx.self_dir, sizeof(ctx.self_dir));
    ctx.path_env = platform_getenv("PATH");
    if (ctx.path_env == 0) {
        ctx.path_env = "/bin:/usr/bin:/usr/local/bin";
    }

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-a") == 0) {
            print_all = 1;
            argi += 1;
            continue;
        }
        print_usage(argv[0]);
        return 1;
    }

    if (argi >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        if (print_all) {
            if (print_all_matches(&ctx, argv[i]) != 0) {
                exit_code = 1;
            }
        } else {
            char path[WHICH_PATH_CAPACITY];

            int result = resolve_command(&ctx, argv[i], path, sizeof(path));

            if (result == 0) {
                rt_write_line(1, path);
            } else if (result == 1) {
                if (resolve_shell_name(argv[i]) != 0) {
                    exit_code = 1;
                }
            } else {
                exit_code = 1;
            }
        }
    }

    return exit_code;
}
