#include "platform.h"
#include "runtime.h"
#include "ssh/ssh_core.h"
#include "sshd/sshd.h"
#include "tool_util.h"

static int sshd_read_line_fd(int fd, char *buffer, size_t buffer_size) {
    size_t used = 0U;
    char ch;

    if (buffer == 0 || buffer_size == 0U) {
        return -1;
    }
    buffer[0] = '\0';
    while (used + 1U < buffer_size) {
        long n = platform_read(fd, &ch, 1U);
        if (n < 0) {
            return -1;
        }
        if (n == 0 || ch == '\n' || ch == '\r') {
            buffer[used] = '\0';
            return used == 0U ? -1 : 0;
        }
        buffer[used++] = ch;
    }
    buffer[used] = '\0';
    return 0;
}

static int sshd_read_file_trimmed(const char *path, char *buffer, size_t buffer_size) {
    PlatformDirEntry entry;
    size_t used = 0U;
    int fd;

    fd = platform_open_read_secure(path, &entry);
    if (fd < 0) {
        tool_write_error("sshd", "refusing unreadable or insecure file ", path);
        return -1;
    }
    while (used + 1U < buffer_size) {
        long n = platform_read(fd, buffer + used, buffer_size - used - 1U);
        if (n < 0) {
            platform_close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        used += (size_t)n;
    }
    if (used + 1U == buffer_size) {
        char extra;
        long n = platform_read(fd, &extra, 1U);
        if (n < 0) {
            platform_close(fd);
            return -1;
        }
        if (n > 0) {
            platform_close(fd);
            tool_write_error("sshd", "file is too large ", path);
            return -1;
        }
    }
    platform_close(fd);
    buffer[used] = '\0';
    rt_trim_newline(buffer);
    return buffer[0] == '\0' ? -1 : 0;
}

static int sshd_copy_arg(char *dst, size_t dst_size, const char *src, const char *what) {
    if (dst == 0 || dst_size == 0U || src == 0 || rt_strlen(src) + 1U > dst_size) {
        tool_write_error("sshd", "argument too long for ", what);
        return -1;
    }
    rt_copy_string(dst, dst_size, src);
    return 0;
}

static int sshd_hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static int sshd_parse_seed_text(const char *text, unsigned char seed[32]) {
    size_t len = rt_strlen(text);
    size_t i;

    if (len != 64U) {
        return -1;
    }
    for (i = 0U; i < 32U; ++i) {
        int hi = sshd_hex_nibble(text[i * 2U]);
        int lo = sshd_hex_nibble(text[i * 2U + 1U]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        seed[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

static int sshd_load_seed(const char *path, unsigned char seed[32]) {
    PlatformDirEntry entry;
    unsigned char raw[65];
    size_t used = 0U;
    int fd = platform_open_read_secure(path, &entry);

    if (fd < 0) {
        tool_write_error("sshd", "refusing unreadable or insecure host seed ", path);
        return -1;
    }
    while (used < sizeof(raw)) {
        long n = platform_read(fd, raw + used, sizeof(raw) - used);
        if (n < 0) {
            platform_close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        used += (size_t)n;
    }
    platform_close(fd);
    if (used == 32U) {
        memcpy(seed, raw, 32U);
        return 0;
    }
    while (used > 0U && (raw[used - 1U] == '\n' || raw[used - 1U] == '\r')) {
        used -= 1U;
    }
    if (used == 64U) {
        char text[65];
        memcpy(text, raw, 64U);
        text[64] = '\0';
        return sshd_parse_seed_text(text, seed);
    }
    tool_write_error("sshd", "host seed must be 32 raw bytes or 64 hex bytes in ", path);
    return -1;
}

static int sshd_load_password_arg(const char *arg, char *buffer, size_t buffer_size) {
    if (arg == 0 || buffer == 0 || buffer_size == 0U) {
        return -1;
    }
    if (rt_strcmp(arg, "-") == 0) {
        return sshd_read_line_fd(0, buffer, buffer_size);
    }
    if (arg[0] == '@' && arg[1] == '@') {
        if (sshd_copy_arg(buffer, buffer_size, arg + 1, "password") != 0) return -1;
        return buffer[0] == '\0' ? -1 : 0;
    }
    if (arg[0] == '@') {
        return sshd_read_file_trimmed(arg + 1, buffer, buffer_size);
    }
    if (sshd_copy_arg(buffer, buffer_size, arg, "password") != 0) return -1;
    return buffer[0] == '\0' ? -1 : 0;
}

int main(int argc, char **argv) {
    static const char USAGE[] = "[-v] [-1] [-p PORT] [-l ADDRESS] [-u USER] -P PASSWORD|@file|@@literal|- [-k HOSTKEY_SEED_FILE] [-s SHELL]";
    SshdConfig config;
    ToolOptState s;
    unsigned long long port_value = 0ULL;
    int r;

    rt_memset(&config, 0, sizeof(config));
    rt_copy_string(config.address, sizeof(config.address), "0.0.0.0");
    rt_copy_string(config.user, sizeof(config.user), "ssh-lab");
    rt_copy_string(config.shell_path, sizeof(config.shell_path), "sh");
    config.port = 2222U;

    tool_opt_init(&s, argc, argv, "sshd", USAGE);
    while ((r = tool_opt_next(&s)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(s.flag, "-v") == 0) {
            config.verbose = 1;
        } else if (rt_strcmp(s.flag, "-1") == 0) {
            config.single_client = 1;
        } else if (rt_strcmp(s.flag, "-p") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            if (tool_parse_uint_arg(s.value, &port_value, "sshd", "port") != 0 ||
                port_value == 0ULL || port_value > 65535ULL) {
                tool_write_usage("sshd", USAGE);
                return 1;
            }
            config.port = (unsigned int)port_value;
        } else if (rt_strcmp(s.flag, "-l") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            if (sshd_copy_arg(config.address, sizeof(config.address), s.value, "listen address") != 0) return 1;
        } else if (rt_strcmp(s.flag, "-u") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            if (!ssh_destination_user_is_safe(s.value)) {
                tool_write_error("sshd", "invalid user ", s.value);
                tool_write_usage("sshd", USAGE);
                return 1;
            }
            if (sshd_copy_arg(config.user, sizeof(config.user), s.value, "user") != 0) return 1;
        } else if (rt_strcmp(s.flag, "-P") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            if (sshd_load_password_arg(s.value, config.password, sizeof(config.password)) != 0) {
                tool_write_error("sshd", "invalid password source ", s.value);
                return 1;
            }
            config.password_set = 1;
        } else if (rt_strcmp(s.flag, "-k") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            if (sshd_load_seed(s.value, config.host_seed) != 0) return 1;
            config.host_seed_set = 1;
        } else if (rt_strcmp(s.flag, "-s") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            if (sshd_copy_arg(config.shell_path, sizeof(config.shell_path), s.value, "shell") != 0) return 1;
        } else {
            tool_write_error("sshd", "unknown option: ", s.flag);
            tool_write_usage("sshd", USAGE);
            return 1;
        }
    }
    if (r == TOOL_OPT_HELP) {
        rt_write_line(1, "sshd - minimal newos-native SSH server");
        rt_write_line(1, "Usage: sshd [-v] [-1] [-p PORT] [-l ADDRESS] [-u USER] -P PASSWORD|@file|@@literal|- [-k HOSTKEY_SEED_FILE] [-s SHELL]");
        rt_write_line(1, "Current scope: SSH transport, password auth, one session channel, and bounded exec via SHELL -c.");
        rt_write_line(1, "PTY and interactive shell requests are rejected cleanly.");
        return 0;
    }
    if (r == TOOL_OPT_ERROR || s.argi != argc || !config.password_set) {
        tool_write_usage("sshd", USAGE);
        return 1;
    }

    return sshd_run(&config) == 0 ? 0 : 1;
}
