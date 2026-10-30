#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int tool_parse_http_status(const char *headers) {
    size_t index = 0U;
    int code = 0;
    int saw_digit = 0;

    while (headers[index] != '\0' && headers[index] != ' ') index += 1U;
    while (headers[index] == ' ') index += 1U;
    while (headers[index] >= '0' && headers[index] <= '9') {
        saw_digit = 1;
        code = code * 10 + (int)(headers[index] - '0');
        index += 1U;
    }
    return saw_digit ? code : -1;
}

int tool_http_connection_connect_timeout(ToolHttpConnection *connection, const char *host, unsigned int port, int use_tls, unsigned int timeout_milliseconds) {
    rt_memset(connection, 0, sizeof(*connection));
    connection->socket_fd = -1;
    connection->use_tls = use_tls;
    if (connection->use_tls) {
        if (platform_tls_connect_timeout(&connection->tls, host, port, timeout_milliseconds) != 0) {
            return -1;
        }
        connection->socket_fd = connection->tls.socket_fd;
        return 0;
    }
    return platform_connect_tcp(host, port, &connection->socket_fd);
}

int tool_http_connection_connect(ToolHttpConnection *connection, const char *host, unsigned int port, int use_tls) {
    return tool_http_connection_connect_timeout(connection, host, port, use_tls, 30000U);
}

unsigned int tool_http_default_port(int use_tls) {
    return use_tls ? 443U : 80U;
}

int tool_http_connection_fd(const ToolHttpConnection *connection) {
    return connection->use_tls ? connection->tls.socket_fd : connection->socket_fd;
}

long tool_http_connection_read(ToolHttpConnection *connection, void *buffer, size_t count) {
    return connection->use_tls ? platform_tls_read(&connection->tls, buffer, count) : platform_read(connection->socket_fd, buffer, count);
}

int tool_http_connection_write_all(ToolHttpConnection *connection, const void *buffer, size_t count) {
    const unsigned char *bytes = (const unsigned char *)buffer;
    size_t written = 0U;

    while (written < count) {
        long result = connection->use_tls ? platform_tls_write(&connection->tls, bytes + written, count - written) : platform_write(connection->socket_fd, bytes + written, count - written);
        if (result <= 0) {
            return -1;
        }
        written += (size_t)result;
    }
    return 0;
}

void tool_http_connection_close(ToolHttpConnection *connection) {
    if (connection->use_tls) {
        platform_tls_close(&connection->tls);
    } else if (connection->socket_fd >= 0) {
        (void)platform_close(connection->socket_fd);
    }
    connection->socket_fd = -1;
}

int tool_find_http_header_end(const char *buffer, size_t length, size_t *offset_out) {
    size_t index;

    if (buffer == 0 || offset_out == 0) {
        return -1;
    }

    for (index = 0U; index + 3U < length; ++index) {
        if (buffer[index] == '\r' && buffer[index + 1U] == '\n' &&
            buffer[index + 2U] == '\r' && buffer[index + 3U] == '\n') {
            *offset_out = index + 4U;
            return 0;
        }
    }

    for (index = 0U; index + 1U < length; ++index) {
        if (buffer[index] == '\n' && buffer[index + 1U] == '\n') {
            *offset_out = index + 2U;
            return 0;
        }
    }
    return -1;
}
