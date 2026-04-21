#include "httpd_impl.h"

#include "runtime.h"
#include "server_log.h"

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

void httpd_log_message(const HttpServerOptions *options, const char *level, const char *message, const char *detail) {
    if (options != NULL && options->quiet && level != NULL && rt_strcmp(level, "ERROR") != 0 && rt_strcmp(level, "WARN") != 0) {
        return;
    }
    (void)server_log_write(2, "httpd", level, message, detail);
}

void httpd_log_request(const HttpServerOptions *options, const HttpRequest *request, int status_code, const char *detail) {
    char message[512];
    size_t used = 0U;

    if (request == NULL) {
        httpd_log_message(options, "INFO", "request", detail);
        return;
    }

    used = httpd_append_text(message, sizeof(message), used, request->method);
    used = httpd_append_char(message, sizeof(message), used, ' ');
    used = httpd_append_text(message, sizeof(message), used, request->path);
    used = httpd_append_text(message, sizeof(message), used, " -> ");
    used = httpd_append_uint(message, sizeof(message), used, (unsigned long long)(status_code < 0 ? 0 : status_code));
    if (detail != NULL && detail[0] != '\0') {
        used = httpd_append_text(message, sizeof(message), used, " (");
        used = httpd_append_text(message, sizeof(message), used, detail);
        used = httpd_append_char(message, sizeof(message), used, ')');
    }
    message[used] = '\0';
    httpd_log_message(options, "INFO", "request", message);
}
