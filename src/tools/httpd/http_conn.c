#include "httpd_impl.h"

#include "runtime.h"

static size_t httpd_append_char(char *buffer, size_t buffer_size, size_t used, char ch) {
    if (used + 1U < buffer_size) {
        buffer[used++] = ch;
        buffer[used] = '\0';
    }
    return used;
}

static size_t httpd_append_text(char *buffer, size_t buffer_size, size_t used, const char *text) {
    size_t index = 0U;

    while (text != NULL && text[index] != '\0') {
        used = httpd_append_char(buffer, buffer_size, used, text[index]);
        index += 1U;
    }
    return used;
}

static size_t httpd_append_uint(char *buffer, size_t buffer_size, size_t used, unsigned long long value) {
    char number[32];

    rt_unsigned_to_string(value, number, sizeof(number));
    return httpd_append_text(buffer, buffer_size, used, number);
}

static int httpd_request_complete(const char *buffer, size_t length) {
    size_t index;

    for (index = 0U; index + 3U < length; ++index) {
        if (buffer[index] == '\r' && buffer[index + 1U] == '\n' &&
            buffer[index + 2U] == '\r' && buffer[index + 3U] == '\n') {
            return 1;
        }
    }
    for (index = 0U; index + 1U < length; ++index) {
        if (buffer[index] == '\n' && buffer[index + 1U] == '\n') {
            return 1;
        }
    }
    return 0;
}

void httpd_connection_init(HttpConnection *connection) {
    if (connection == NULL) {
        return;
    }
    rt_memset(connection, 0, sizeof(*connection));
    connection->fd = -1;
}

void httpd_connection_close(HttpConnection *connection) {
    if (connection == NULL) {
        return;
    }
    if (connection->fd >= 0) {
        (void)platform_close(connection->fd);
    }
    httpd_connection_init(connection);
}

void httpd_send_response(int fd, const HttpResponse *response) {
    char header[512];
    char buffer[1024];
    size_t used = 0U;

    if (response == NULL) {
        return;
    }

    header[0] = '\0';
    used = httpd_append_text(header, sizeof(header), used, "HTTP/1.1 ");
    used = httpd_append_uint(header, sizeof(header), used, (unsigned long long)response->status_code);
    used = httpd_append_char(header, sizeof(header), used, ' ');
    used = httpd_append_text(header, sizeof(header), used, response->status_text != NULL ? response->status_text : "OK");
    used = httpd_append_text(header, sizeof(header), used, "\r\nContent-Length: ");
    used = httpd_append_uint(header, sizeof(header), used, response->content_length);
    used = httpd_append_text(header, sizeof(header), used, "\r\nContent-Type: ");
    used = httpd_append_text(header, sizeof(header), used, response->content_type[0] != '\0' ? response->content_type : "text/plain; charset=utf-8");
    used = httpd_append_text(header, sizeof(header), used, "\r\nX-Content-Type-Options: nosniff");
    used = httpd_append_text(header, sizeof(header), used, "\r\nX-Frame-Options: DENY");
    used = httpd_append_text(header, sizeof(header), used, "\r\nReferrer-Policy: no-referrer");
    used = httpd_append_text(header, sizeof(header), used, "\r\nConnection: close\r\n\r\n");

    (void)rt_write_all(fd, header, used);
    if (response->head_only) {
        return;
    }

    if (response->file_fd >= 0) {
        for (;;) {
            long bytes = platform_read(response->file_fd, buffer, sizeof(buffer));
            if (bytes <= 0) {
                break;
            }
            if (rt_write_all(fd, buffer, (size_t)bytes) != 0) {
                break;
            }
        }
    } else if (response->body_length > 0U) {
        (void)rt_write_all(fd, response->body, response->body_length);
    }
}

void httpd_send_simple_response(int fd, int status_code, const char *detail) {
    HttpResponse response;

    rt_memset(&response, 0, sizeof(response));
    response.status_code = status_code;
    response.status_text = httpd_status_text(status_code);
    response.file_fd = -1;
    rt_copy_string(response.content_type, sizeof(response.content_type), "text/plain; charset=utf-8");
    rt_copy_string(response.body, sizeof(response.body), detail != NULL && detail[0] != '\0' ? detail : response.status_text);
    response.body_length = rt_strlen(response.body);
    response.content_length = (unsigned long long)response.body_length;
    httpd_send_response(fd, &response);
}

int httpd_connection_process(HttpConnection *connection, const HttpServerOptions *options) {
    char incoming[1024];
    HttpRequest request;
    HttpResponse response;
    char detail[256];
    long bytes;
    int parse_status;

    if (connection == NULL || !connection->in_use || connection->fd < 0) {
        return 0;
    }

    bytes = platform_read(connection->fd, incoming, sizeof(incoming));
    if (bytes <= 0) {
        httpd_connection_close(connection);
        return 1;
    }

    if (connection->request_length + (size_t)bytes + 1U >= sizeof(connection->request)) {
        httpd_send_simple_response(connection->fd, 413, "request headers too large\n");
        httpd_log_message(options, "WARN", "request rejected", "request headers too large");
        httpd_connection_close(connection);
        return 1;
    }

    memcpy(connection->request + connection->request_length, incoming, (size_t)bytes);
    connection->request_length += (size_t)bytes;
    connection->request[connection->request_length] = '\0';
    connection->last_active = platform_get_epoch_time();

    if (!httpd_request_complete(connection->request, connection->request_length)) {
        return 0;
    }

    parse_status = httpd_parse_request(connection->request, &request, detail, sizeof(detail));
    if (parse_status != 0) {
        httpd_send_simple_response(connection->fd, parse_status, detail[0] != '\0' ? detail : httpd_status_text(parse_status));
        httpd_log_message(options, "WARN", "bad request", detail);
        httpd_connection_close(connection);
        return 1;
    }

    httpd_build_response(options, &request, &response, detail, sizeof(detail));
    httpd_send_response(connection->fd, &response);
    if (response.file_fd >= 0) {
        (void)platform_close(response.file_fd);
    }
    httpd_log_request(options, &request, response.status_code, detail);
    httpd_connection_close(connection);
    return 1;
}
