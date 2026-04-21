#include "httpd_impl.h"

#include "runtime.h"

const char *httpd_status_text(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

static void httpd_prepare_text_response(HttpResponse *response, int status_code, const char *body) {
    rt_memset(response, 0, sizeof(*response));
    response->status_code = status_code;
    response->status_text = httpd_status_text(status_code);
    response->file_fd = -1;
    rt_copy_string(response->content_type, sizeof(response->content_type), "text/plain; charset=utf-8");
    if (body != NULL) {
        rt_copy_string(response->body, sizeof(response->body), body);
        response->body_length = rt_strlen(response->body);
        response->content_length = (unsigned long long)response->body_length;
    }
}

void httpd_build_response(const HttpServerOptions *options, const HttpRequest *request, HttpResponse *response, char *detail, size_t detail_size) {
    int rc;

    response->head_only = request != NULL ? request->head_only : 0;
    if (request != NULL && (rt_strcmp(request->path, "/health") == 0 || rt_strcmp(request->path, "/_status") == 0)) {
        httpd_prepare_text_response(response, 200, request->head_only ? "" : "ok\n");
        return;
    }

    rc = httpd_build_static_response(options, request, response, detail, detail_size);
    if (rc != 0) {
        if (detail == NULL || detail[0] == '\0') {
            detail = (char *)httpd_status_text(rc);
        }
        httpd_prepare_text_response(response, rc, request != NULL && request->head_only ? "" : httpd_status_text(rc));
        response->head_only = request != NULL ? request->head_only : 0;
    }
}
