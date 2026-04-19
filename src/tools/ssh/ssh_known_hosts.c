#include "ssh_known_hosts.h"
#include "crypto/crypto_util.h"
#include "platform.h"
#include "runtime.h"
#include "ssh_core.h"

static int ssh_local_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int ssh_copy_text(char *dst, size_t dst_size, const char *src, size_t src_length) {
    size_t i;

    if (dst == 0 || src == 0 || dst_size == 0 || src_length + 1U > dst_size) {
        return -1;
    }

    for (i = 0; i < src_length; ++i) {
        dst[i] = src[i];
    }
    dst[src_length] = '\0';
    return 0;
}

static int ssh_line_host_matches(const char *field, const char *host, unsigned int port) {
    char expected[SSH_DESTINATION_CAPACITY];
    char token[SSH_DESTINATION_CAPACITY];
    size_t expected_length = 0U;
    size_t token_length = 0U;
    size_t i = 0U;

    if (field == 0 || host == 0) {
        return 0;
    }

    if (port == SSH_DEFAULT_PORT) {
        rt_copy_string(expected, sizeof(expected), host);
    } else {
        SshDestination destination;
        memset(&destination, 0, sizeof(destination));
        rt_copy_string(destination.host, sizeof(destination.host), host);
        destination.port = port;
        if (ssh_format_destination(&destination, 0, expected, sizeof(expected)) != 0) {
            return 0;
        }
    }
    expected_length = rt_strlen(expected);

    while (field[i] != '\0') {
        token_length = 0U;
        while (field[i] != '\0' && field[i] != ',') {
            if (token_length + 1U < sizeof(token)) {
                token[token_length++] = field[i];
            }
            i += 1U;
        }
        token[token_length] = '\0';

        if (token[0] != '\0' &&
            token[0] != '|' &&
            token[0] != '!' &&
            token_length == expected_length &&
            rt_strcmp(token, expected) == 0) {
            return 1;
        }

        if (field[i] == ',') {
            i += 1U;
        }
    }

    return 0;
}

static int ssh_read_line(int fd, char *buffer, size_t buffer_size, int *has_line_out) {
    char ch;
    long bytes;
    size_t used = 0U;

    if (buffer == 0 || buffer_size == 0U || has_line_out == 0) {
        return -1;
    }

    *has_line_out = 0;
    while ((bytes = platform_read(fd, &ch, 1U)) > 0) {
        *has_line_out = 1;
        if (ch == '\n') {
            break;
        }
        if (used + 1U >= buffer_size) {
            return -1;
        }
        buffer[used++] = ch;
    }

    if (bytes < 0) {
        return -1;
    }
    if (!*has_line_out && used == 0U) {
        buffer[0] = '\0';
        return 0;
    }

    if (used > 0U && buffer[used - 1U] == '\r') {
        used -= 1U;
    }
    buffer[used] = '\0';
    return 1;
}

static int ssh_split_known_hosts_line(char *line, char **hosts_out, char **algo_out, char **key_out) {
    char *cursor = line;
    char *start;

    if (line == 0 || hosts_out == 0 || algo_out == 0 || key_out == 0) {
        return -1;
    }

    while (*cursor != '\0' && ssh_local_is_space(*cursor)) {
        cursor += 1;
    }
    if (*cursor == '\0' || *cursor == '#') {
        return 1;
    }

    start = cursor;
    while (*cursor != '\0' && !ssh_local_is_space(*cursor)) {
        cursor += 1;
    }
    if (*cursor == '\0') {
        return -1;
    }
    *cursor++ = '\0';
    *hosts_out = start;

    while (*cursor != '\0' && ssh_local_is_space(*cursor)) {
        cursor += 1;
    }
    if (*cursor == '\0') {
        return -1;
    }
    start = cursor;
    while (*cursor != '\0' && !ssh_local_is_space(*cursor)) {
        cursor += 1;
    }
    if (*cursor == '\0') {
        return -1;
    }
    *cursor++ = '\0';
    *algo_out = start;

    while (*cursor != '\0' && ssh_local_is_space(*cursor)) {
        cursor += 1;
    }
    if (*cursor == '\0') {
        return -1;
    }
    start = cursor;
    while (*cursor != '\0' && !ssh_local_is_space(*cursor)) {
        cursor += 1;
    }
    *cursor = '\0';
    *key_out = start;
    return 0;
}

static int ssh_ensure_parent_dir(const char *path) {
    char parent[SSH_KNOWN_HOSTS_PATH_CAPACITY];
    size_t length = 0U;
    size_t i;
    int is_dir = 0;

    if (path == 0) {
        return -1;
    }

    length = rt_strlen(path);
    for (i = length; i > 0U; --i) {
        if (path[i - 1U] == '/') {
            if (ssh_copy_text(parent, sizeof(parent), path, i - 1U) != 0) {
                return -1;
            }
            if (platform_path_is_directory(parent, &is_dir) == 0 && is_dir) {
                return 0;
            }
            return platform_make_directory(parent, 0700U);
        }
    }

    return 0;
}

int ssh_known_hosts_default_path(char *buffer, size_t buffer_size) {
    char ssh_dir[SSH_KNOWN_HOSTS_PATH_CAPACITY];
    const char *home = platform_getenv("HOME");

    if (buffer == 0 || buffer_size == 0U) {
        return -1;
    }

    if (home != 0 && home[0] != '\0') {
        if (rt_join_path(home, ".ssh", ssh_dir, sizeof(ssh_dir)) != 0 ||
            rt_join_path(ssh_dir, "known_hosts", buffer, buffer_size) != 0) {
            return -1;
        }
        return 0;
    }

    return ssh_copy_text(buffer, buffer_size, "known_hosts", sizeof("known_hosts") - 1U);
}

int ssh_known_hosts_lookup(
    const char *path,
    const char *host,
    unsigned int port,
    const char *algorithm,
    const unsigned char *key_blob,
    size_t key_blob_length,
    SshKnownHostStatus *status_out
) {
    int fd;
    char line[2048];
    unsigned char decoded[1024];

    if (host == 0 || algorithm == 0 || (key_blob == 0 && key_blob_length != 0U) || status_out == 0) {
        return -1;
    }

    *status_out = SSH_KNOWN_HOST_UNKNOWN;
    fd = platform_open_read(path);
    if (fd < 0) {
        return 0;
    }

    for (;;) {
        int has_line = 0;
        int rc = ssh_read_line(fd, line, sizeof(line), &has_line);
        char *hosts = 0;
        char *algo = 0;
        char *key = 0;
        size_t decoded_length = 0U;

        if (rc < 0) {
            platform_close(fd);
            return -1;
        }
        if (rc == 0) {
            break;
        }
        if (!has_line) {
            continue;
        }
        rc = ssh_split_known_hosts_line(line, &hosts, &algo, &key);
        if (rc != 0) {
            if (rc < 0) {
                platform_close(fd);
                return -1;
            }
            continue;
        }
        if (!ssh_line_host_matches(hosts, host, port) || rt_strcmp(algo, algorithm) != 0) {
            continue;
        }
        if (ssh_base64_decode(key, decoded, sizeof(decoded), &decoded_length) != 0) {
            platform_close(fd);
            return -1;
        }
        if (decoded_length == key_blob_length && crypto_constant_time_equal(decoded, key_blob, key_blob_length)) {
            *status_out = SSH_KNOWN_HOST_MATCH;
        } else {
            *status_out = SSH_KNOWN_HOST_MISMATCH;
        }
        break;
    }

    platform_close(fd);
    return 0;
}

int ssh_known_hosts_append(
    const char *path,
    const char *host,
    unsigned int port,
    const char *algorithm,
    const unsigned char *key_blob,
    size_t key_blob_length
) {
    char base64_key[1536];
    char host_field[SSH_DESTINATION_CAPACITY];
    char line[2048];
    int fd;
    size_t used = 0U;
    SshDestination destination;

    if (path == 0 || host == 0 || algorithm == 0 || (key_blob == 0 && key_blob_length != 0U)) {
        return -1;
    }
    if (ssh_base64_encode(key_blob, key_blob_length, base64_key, sizeof(base64_key), 0) != 0) {
        return -1;
    }

    memset(&destination, 0, sizeof(destination));
    rt_copy_string(destination.host, sizeof(destination.host), host);
    destination.port = port;
    if (ssh_format_destination(&destination, 0, host_field, sizeof(host_field)) != 0) {
        return -1;
    }
    if (ssh_ensure_parent_dir(path) != 0) {
        return -1;
    }

    line[0] = '\0';
    rt_copy_string(line, sizeof(line), host_field);
    used = rt_strlen(line);
    if (used + 1U + rt_strlen(algorithm) + 1U + rt_strlen(base64_key) + 2U > sizeof(line)) {
        return -1;
    }
    line[used++] = ' ';
    line[used] = '\0';
    rt_copy_string(line + used, sizeof(line) - used, algorithm);
    used += rt_strlen(algorithm);
    line[used++] = ' ';
    line[used] = '\0';
    rt_copy_string(line + used, sizeof(line) - used, base64_key);
    used += rt_strlen(base64_key);
    line[used++] = '\n';
    line[used] = '\0';

    fd = platform_open_append(path, 0600U);
    if (fd < 0) {
        return -1;
    }
    if (platform_write(fd, line, used) != (long)used) {
        platform_close(fd);
        return -1;
    }
    platform_close(fd);
    return 0;
}
