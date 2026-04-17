#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static int print_groups_for_user(const char *username, int include_name) {
    int pipe_fds[2];
    int pid = -1;
    int status = 1;
    long bytes_read;
    char buffer[256];
    char *argv_with_user[] = { "/usr/bin/id", "-Gn", (char *)username, 0 };
    char *argv_self[] = { "/usr/bin/id", "-Gn", 0 };
    char *const *argv = username != 0 ? argv_with_user : argv_self;

    if (platform_create_pipe(pipe_fds) != 0) {
        tool_write_error("groups", "cannot create pipe", 0);
        return 1;
    }

    if (platform_spawn_process(argv, -1, pipe_fds[1], 0, 0, 0, &pid) != 0) {
        (void)platform_close(pipe_fds[0]);
        (void)platform_close(pipe_fds[1]);
        tool_write_error("groups", "cannot inspect groups", 0);
        return 1;
    }

    (void)platform_close(pipe_fds[1]);

    if (include_name) {
        rt_write_cstr(1, username);
        rt_write_cstr(1, " : ");
    }

    while ((bytes_read = platform_read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        if (rt_write_all(1, buffer, (size_t)bytes_read) != 0) {
            (void)platform_close(pipe_fds[0]);
            (void)platform_wait_process(pid, &status);
            return 1;
        }
    }

    (void)platform_close(pipe_fds[0]);
    if (platform_wait_process(pid, &status) != 0 || status != 0 || bytes_read < 0) {
        tool_write_error("groups", "cannot inspect groups for ", username != 0 ? username : "current user");
        return 1;
    }

    return 0;
}

int main(int argc, char **argv) {
    int i;
    int exit_code = 0;

    if (argc == 1) {
        return print_groups_for_user(0, 0);
    }

    for (i = 1; i < argc; ++i) {
        if (print_groups_for_user(argv[i], 1) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
