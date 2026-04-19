#include "platform.h"
#include "runtime.h"
#include "ssh_client.h"
#include "ssh_core.h"
#include "tool_util.h"

int main(int argc, char **argv) {
    const char *destination = 0;
    const char *identity_path = 0;
    const char *password = 0;
    const char *user_override = 0;
    const char *env_user = platform_getenv("USER");
    char default_user[SSH_USER_CAPACITY];
    SshDestination parsed;
    SshClientConfig config;
    unsigned long long port_override = 0ULL;
    int verbose = 0;
    int argi = 1;

    default_user[0] = '\0';
    if (env_user != 0 && env_user[0] != '\0') {
        rt_copy_string(default_user, sizeof(default_user), env_user);
    }

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) {
            rt_write_line(1, "ssh - minimal interactive SSH client");
            rt_write_line(1, "Usage: ssh [-v] [-i IDENTITY] [-l USER] [-p PORT] HOST|USER@HOST[:PORT] [PASSWORD]");
            rt_write_line(1, "Current scope: interactive shell sessions with curve25519-sha256, ssh-ed25519,");
            rt_write_line(1, "and chacha20-poly1305@openssh.com. Public-key auth supports unencrypted Ed25519 keys.");
            return 0;
        } else if (rt_strcmp(argv[argi], "-v") == 0) {
            verbose = 1;
        } else if (rt_strcmp(argv[argi], "-i") == 0) {
            if (argi + 1 >= argc) {
                tool_write_usage("ssh", "[-v] [-i IDENTITY] [-l USER] [-p PORT] HOST|USER@HOST[:PORT] [PASSWORD]");
                return 1;
            }
            identity_path = argv[++argi];
        } else if (rt_strcmp(argv[argi], "-l") == 0) {
            if (argi + 1 >= argc) {
                tool_write_usage("ssh", "[-v] [-i IDENTITY] [-l USER] [-p PORT] HOST|USER@HOST[:PORT] [PASSWORD]");
                return 1;
            }
            user_override = argv[++argi];
        } else if (rt_strcmp(argv[argi], "-p") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &port_override, "ssh", "port") != 0 ||
                port_override == 0ULL || port_override > 65535ULL) {
                tool_write_usage("ssh", "[-v] [-i IDENTITY] [-l USER] [-p PORT] HOST|USER@HOST[:PORT] [PASSWORD]");
                return 1;
            }
            argi += 1;
        } else {
            tool_write_usage("ssh", "[-v] [-i IDENTITY] [-l USER] [-p PORT] HOST|USER@HOST[:PORT] [PASSWORD]");
            return 1;
        }
        argi += 1;
    }

    if (argi >= argc) {
        tool_write_usage("ssh", "[-v] [-i IDENTITY] [-l USER] [-p PORT] HOST|USER@HOST[:PORT] [PASSWORD]");
        return 1;
    }

    destination = argv[argi++];
    if (argi < argc) {
        password = argv[argi++];
    }
    if (argi != argc) {
        tool_write_usage("ssh", "[-v] [-i IDENTITY] [-l USER] [-p PORT] HOST|USER@HOST[:PORT] [PASSWORD]");
        return 1;
    }

    if (ssh_parse_destination(destination, default_user, port_override == 0ULL ? SSH_DEFAULT_PORT : (unsigned int)port_override, &parsed) != 0) {
        tool_write_error("ssh", "invalid destination ", destination);
        return 1;
    }
    if (user_override != 0 && user_override[0] != '\0') {
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
