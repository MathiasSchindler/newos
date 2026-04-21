#include "platform.h"
#include "runtime.h"
#include "ssh/ssh_client.h"
#include "ssh/ssh_core.h"
#include "tool_util.h"

int main(int argc, char **argv) {
    static const char USAGE[] = "[-v] [-i IDENTITY] [-l USER] [-p PORT] HOST|USER@HOST[:PORT] [PASSWORD]";
    const char *destination = 0;
    const char *identity_path = 0;
    const char *password = 0;
    const char *user_override = 0;
    const char *env_user = platform_getenv("USER");
    char default_user[SSH_USER_CAPACITY];
    SshDestination parsed;
    SshClientConfig config;
    ToolOptState s;
    unsigned long long port_override = 0ULL;
    int verbose = 0;
    int r;

    default_user[0] = '\0';
    if (env_user != 0 && env_user[0] != '\0') {
        rt_copy_string(default_user, sizeof(default_user), env_user);
    }

    tool_opt_init(&s, argc, argv, "ssh", USAGE);
    while ((r = tool_opt_next(&s)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(s.flag, "-v") == 0) {
            verbose = 1;
        } else if (rt_strcmp(s.flag, "-i") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            identity_path = s.value;
        } else if (rt_strcmp(s.flag, "-l") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            user_override = s.value;
        } else if (rt_strcmp(s.flag, "-p") == 0) {
            if (tool_opt_require_value(&s) != 0) return 1;
            if (tool_parse_uint_arg(s.value, &port_override, "ssh", "port") != 0 ||
                port_override == 0ULL || port_override > 65535ULL) {
                tool_write_usage("ssh", USAGE);
                return 1;
            }
        } else {
            tool_write_error("ssh", "unknown option: ", s.flag);
            tool_write_usage("ssh", USAGE);
            return 1;
        }
    }
    if (r == TOOL_OPT_HELP) {
        rt_write_line(1, "ssh - minimal interactive SSH client");
        rt_write_line(1, "Usage: ssh [-v] [-i IDENTITY] [-l USER] [-p PORT] HOST|USER@HOST[:PORT] [PASSWORD]");
        rt_write_line(1, "Current scope: interactive shell sessions with curve25519-sha256, ssh-ed25519,");
        rt_write_line(1, "and chacha20-poly1305@openssh.com. Public-key auth supports unencrypted Ed25519 keys.");
        return 0;
    }

    if (s.argi >= argc) {
        tool_write_usage("ssh", USAGE);
        return 1;
    }

    destination = argv[s.argi++];
    if (s.argi < argc) {
        password = argv[s.argi++];
    }
    if (s.argi != argc) {
        tool_write_usage("ssh", USAGE);
        return 1;
    }

    if (ssh_parse_destination(destination, default_user, port_override == 0ULL ? SSH_DEFAULT_PORT : (unsigned int)port_override, &parsed) != 0) {
        tool_write_error("ssh", "invalid destination ", destination);
        tool_write_usage("ssh", USAGE);
        return 1;
    }
    if (user_override != 0 && user_override[0] != '\0') {
        if (!ssh_destination_user_is_safe(user_override)) {
            tool_write_error("ssh", "invalid remote user ", user_override);
            tool_write_usage("ssh", USAGE);
            return 1;
        }
        rt_copy_string(parsed.user, sizeof(parsed.user), user_override);
        parsed.has_user = 1;
    }
    if (port_override != 0ULL) {
        parsed.port = (unsigned int)port_override;
    }
    if (!parsed.has_user || parsed.user[0] == '\0') {
        tool_write_error("ssh", "missing remote user for ", destination);
        return 1;
    }

    config.host = parsed.host;
    config.user = parsed.user;
    config.port = parsed.port;
    config.password = password;
    config.identity_path = identity_path;
    config.verbose = verbose;

    return ssh_client_connect_and_run(&config) == 0 ? 0 : 1;
}
