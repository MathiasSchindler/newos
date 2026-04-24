#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define LOGIN_FIELD_CAPACITY 256
#define LOGIN_ARG_CAPACITY 64

typedef struct {
    PlatformIdentity identity;
    char home[LOGIN_FIELD_CAPACITY];
    char shell[LOGIN_FIELD_CAPACITY];
} LoginAccount;

static void login_usage(const char *program_name) {
    tool_write_usage(program_name, "[-fp] [-h HOST] [-s SHELL] [USER [COMMAND...]]");
}

static void copy_field(char *dst, size_t dst_size, const char *begin, const char *end) {
    size_t used = 0U;

    if (dst_size == 0U) {
        return;
    }
    while (begin < end && used + 1U < dst_size) {
        dst[used] = *begin;
        used += 1U;
        begin += 1;
    }
    dst[used] = '\0';
}

static int read_login_name(char *buffer, size_t buffer_size) {
    size_t used = 0U;
    char ch;
    long bytes;

    if (rt_write_cstr(1, "login: ") != 0) {
        return -1;
    }
    while ((bytes = platform_read(0, &ch, 1U)) > 0) {
        if (ch == '\n' || ch == '\r') {
            break;
        }
        if (used + 1U < buffer_size) {
            buffer[used] = ch;
            used += 1U;
        }
    }
    if (bytes < 0 || used == 0U) {
        return -1;
    }
    buffer[used] = '\0';
    return 0;
}

static int line_name_matches(const char *line_begin, const char *line_end, const char *name) {
    const char *name_end = line_begin;
    size_t name_length;

    while (name_end < line_end && *name_end != ':') {
        name_end += 1;
    }
    name_length = (size_t)(name_end - line_begin);
    return name_length == rt_strlen(name) && rt_strncmp(line_begin, name, name_length) == 0;
}

static int copy_passwd_field(const char *line_begin, const char *line_end, unsigned int wanted_field, char *buffer, size_t buffer_size) {
    const char *field_begin = line_begin;
    const char *cursor = line_begin;
    unsigned int field = 0U;

    while (cursor <= line_end) {
        if (cursor == line_end || *cursor == ':') {
            if (field == wanted_field) {
                copy_field(buffer, buffer_size, field_begin, cursor);
                return 0;
            }
            field += 1U;
            field_begin = cursor + 1;
        }
        cursor += 1;
    }

    return -1;
}

static int load_passwd_fields(const char *username, char *home, size_t home_size, char *shell, size_t shell_size) {
    char data[16384];
    long fd;
    long bytes;
    const char *cursor;

    fd = platform_open_read("/etc/passwd");
    if (fd < 0) {
        return -1;
    }
    bytes = platform_read((int)fd, data, sizeof(data) - 1U);
    platform_close((int)fd);
    if (bytes <= 0) {
        return -1;
    }
    data[bytes] = '\0';

    cursor = data;
    while (*cursor != '\0') {
        const char *line_end = cursor;

        while (*line_end != '\0' && *line_end != '\n') {
            line_end += 1;
        }
        if (line_name_matches(cursor, line_end, username)) {
            (void)copy_passwd_field(cursor, line_end, 5U, home, home_size);
            (void)copy_passwd_field(cursor, line_end, 6U, shell, shell_size);
            return 0;
        }
        cursor = (*line_end == '\n') ? (line_end + 1) : line_end;
    }

    return -1;
}

static int load_account(const char *username, LoginAccount *account) {
    const char *env_shell;

    if (platform_lookup_identity(username, &account->identity) != 0) {
        return -1;
    }

    rt_copy_string(account->home, sizeof(account->home), "/");
    env_shell = platform_getenv("SHELL");
    rt_copy_string(account->shell,
                   sizeof(account->shell),
                   (env_shell != 0 && env_shell[0] != '\0') ? env_shell : "/bin/sh");

    if (load_passwd_fields(account->identity.username, account->home, sizeof(account->home), account->shell, sizeof(account->shell)) != 0) {
        (void)load_passwd_fields(username, account->home, sizeof(account->home), account->shell, sizeof(account->shell));
    }
    if (account->home[0] == '\0') {
        rt_copy_string(account->home, sizeof(account->home), "/");
    }
    if (account->shell[0] == '\0') {
        rt_copy_string(account->shell, sizeof(account->shell), "/bin/sh");
    }
    return 0;
}

static int setup_environment(const LoginAccount *account, const char *host) {
    if (platform_clearenv() != 0 ||
        platform_setenv("HOME", account->home, 1) != 0 ||
        platform_setenv("SHELL", account->shell, 1) != 0 ||
        platform_setenv("USER", account->identity.username, 1) != 0 ||
        platform_setenv("LOGNAME", account->identity.username, 1) != 0 ||
        platform_setenv("PATH", "/bin:/usr/bin", 1) != 0) {
        return -1;
    }
    if (host != 0 && host[0] != '\0' && platform_setenv("REMOTEHOST", host, 1) != 0) {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *program_name = tool_base_name(argv[0]);
    const char *host = 0;
    const char *shell_override = 0;
    const char *username = 0;
    char prompted_username[LOGIN_FIELD_CAPACITY];
    LoginAccount account;
    PlatformIdentity current;
    char gid_text[32];
    char *child_argv[LOGIN_ARG_CAPACITY];
    int preserve_environment = 0;
    int trusted = 0;
    int argi = 1;
    int child_argc = 0;
    int pid = -1;
    int status = 1;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-h") == 0 && argi + 1 < argc) {
            host = argv[argi + 1];
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-s") == 0 && argi + 1 < argc) {
            shell_override = argv[argi + 1];
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-f") == 0) {
            trusted = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-p") == 0) {
            preserve_environment = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "-s") == 0) {
            login_usage(program_name);
            return 1;
        } else if (rt_strcmp(argv[argi], "--help") == 0) {
            login_usage(program_name);
            return 0;
        } else {
            tool_write_error(program_name, "unknown option: ", argv[argi]);
            login_usage(program_name);
            return 1;
        }
    }

    if (argi < argc) {
        username = argv[argi];
        argi += 1;
    } else {
        if (read_login_name(prompted_username, sizeof(prompted_username)) != 0) {
            tool_write_error(program_name, "failed to read login name", 0);
            return 1;
        }
        username = prompted_username;
    }

    if (platform_get_identity(&current) != 0 || load_account(username, &account) != 0) {
        tool_write_error(program_name, "unknown user ", username);
        return 1;
    }
    if (!trusted && account.identity.uid != current.uid) {
        tool_write_error(program_name, "password authentication is not implemented; use -f from a trusted caller", 0);
        return 1;
    }

    if (shell_override != 0 && shell_override[0] != '\0') {
        rt_copy_string(account.shell, sizeof(account.shell), shell_override);
    }
    if (!preserve_environment && setup_environment(&account, host) != 0) {
        tool_write_error(program_name, "failed to prepare environment", 0);
        return 1;
    }

    if (argi < argc) {
        while (argi < argc && child_argc + 1 < LOGIN_ARG_CAPACITY) {
            child_argv[child_argc] = argv[argi];
            child_argc += 1;
            argi += 1;
        }
        if (argi < argc) {
            tool_write_error(program_name, "too many command arguments", 0);
            return 1;
        }
    } else {
        child_argv[child_argc++] = account.shell;
    }
    child_argv[child_argc] = 0;

    rt_unsigned_to_string((unsigned long long)account.identity.gid, gid_text, sizeof(gid_text));
    if (platform_spawn_process_ex(child_argv,
                                  -1,
                                  -1,
                                  0,
                                  0,
                                  0,
                                  account.home,
                                  account.identity.username,
                                  gid_text,
                                  &pid) != 0) {
        tool_write_error(program_name, "failed to execute ", child_argv[0]);
        return 1;
    }
    if (platform_wait_process(pid, &status) != 0) {
        tool_write_error(program_name, "wait failed", 0);
        return 1;
    }

    return status;
}
