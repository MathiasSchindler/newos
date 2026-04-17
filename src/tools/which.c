#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#if __STDC_HOSTED__
#include <stdlib.h>
#endif

#define WHICH_PATH_CAPACITY 1024

typedef struct {
    char self_dir[WHICH_PATH_CAPACITY];
    const char *path_env;
} SearchContext;

static int contains_slash(const char *text) {
    size_t i = 0;
    while (text[i] != '\0') {
        if (text[i] == '/') {
            return 1;
        }
        i += 1;
    }
    return 0;
}

static int path_exists_as_file(const char *path) {
    PlatformDirEntry entry;
    return platform_get_path_info(path, &entry) == 0 && !entry.is_dir;
}

static void set_self_dir(const char *argv0, char *buffer, size_t buffer_size) {
    size_t len;
    size_t i;

    if (argv0 == 0 || argv0[0] == '\0') {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    if (!contains_slash(argv0)) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    len = rt_strlen(argv0);
    if (len + 1 > buffer_size) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    memcpy(buffer, argv0, len + 1);
    for (i = len; i > 0; --i) {
        if (buffer[i - 1] == '/') {
            if (i == 1) {
                buffer[1] = '\0';
            } else {
                buffer[i - 1] = '\0';
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
            if (length + 1 < sizeof(dir)) {
                dir[length++] = dirs[i];
            }
            i += 1;
        }

        dir[length] = '\0';
        if (dirs[i] == ':') {
            i += 1;
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

    if (ctx->self_dir[0] != '\0' && tool_join_path(ctx->self_dir, name, buffer, buffer_size) == 0 && path_exists_as_file(buffer)) {
        return 0;
    }

    if (search_in_list(ctx->path_env, name, buffer, buffer_size) == 0) {
        return 0;
    }

    return -1;
}

int main(int argc, char **argv) {
    SearchContext ctx;
    int i;
    int exit_code = 0;

    set_self_dir((argc > 0) ? argv[0] : "which", ctx.self_dir, sizeof(ctx.self_dir));
#if __STDC_HOSTED__
    ctx.path_env = getenv("PATH");
#else
    ctx.path_env = "/bin:/usr/bin:/usr/local/bin";
#endif

    if (argc < 2) {
        rt_write_line(2, "Usage: which COMMAND...");
        return 1;
    }

    for (i = 1; i < argc; ++i) {
        char path[WHICH_PATH_CAPACITY];
        if (resolve_command(&ctx, argv[i], path, sizeof(path)) == 0) {
            rt_write_line(1, path);
        } else {
            exit_code = 1;
        }
    }

    return exit_code;
}
