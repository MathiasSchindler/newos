#include "httpd_impl.h"

#include "runtime.h"
#include "server_log.h"
#include "tool_util.h"




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

    used = tool_buffer_append_cstr(message, sizeof(message), used, request->method);
    used = tool_buffer_append_char(message, sizeof(message), used, ' ');
    used = tool_buffer_append_cstr(message, sizeof(message), used, request->path);
    used = tool_buffer_append_cstr(message, sizeof(message), used, " -> ");
    used = tool_buffer_append_uint(message, sizeof(message), used, (unsigned long long)(status_code < 0 ? 0 : status_code));
    if (detail != NULL && detail[0] != '\0') {
        used = tool_buffer_append_cstr(message, sizeof(message), used, " (");
        used = tool_buffer_append_cstr(message, sizeof(message), used, detail);
        used = tool_buffer_append_char(message, sizeof(message), used, ')');
    }
    message[used] = '\0';
    httpd_log_message(options, "INFO", "request", message);
}
