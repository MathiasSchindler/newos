#include "httpd_impl.h"

#include "runtime.h"
#include "tool_util.h"

static int httpd_path_is_within_root(const char *root, const char *path) {
    size_t root_len;

    if (root == NULL || path == NULL) {
        return 0;
    }
    if (rt_strcmp(root, "/") == 0) {
        return 1;
    }

    root_len = rt_strlen(root);
    if (rt_strncmp(root, path, root_len) != 0) {
        return 0;
    }
    return path[root_len] == '\0' || path[root_len] == '/';
}

static int httpd_path_has_hidden_component(const char *path) {
    size_t index = 0U;

    while (path != NULL && path[index] != '\0') {
        while (path[index] == '/') {
            index += 1U;
        }
        if (path[index] == '.') {
            return 1;
        }
        while (path[index] != '\0' && path[index] != '/') {
            index += 1U;
        }
    }
    return 0;
}

static const char *httpd_content_type_for_path(const char *path) {
    const char *cursor = path;
    const char *dot = NULL;

    while (cursor != NULL && *cursor != '\0') {
        if (*cursor == '.') {
            dot = cursor;
        }
        cursor += 1;
    }

    if (dot == NULL) {
        return "application/octet-stream";
    }
    if (rt_strcmp(dot, ".html") == 0 || rt_strcmp(dot, ".htm") == 0) {
        return "text/html; charset=utf-8";
    }
    if (rt_strcmp(dot, ".txt") == 0 || rt_strcmp(dot, ".log") == 0) {
        return "text/plain; charset=utf-8";
    }
    if (rt_strcmp(dot, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (rt_strcmp(dot, ".js") == 0) {
        return "application/javascript";
    }
    if (rt_strcmp(dot, ".json") == 0) {
        return "application/json";
    }
    if (rt_strcmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (rt_strcmp(dot, ".jpg") == 0 || rt_strcmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (rt_strcmp(dot, ".gif") == 0) {
        return "image/gif";
    }
    return "application/octet-stream";
}

int httpd_build_static_response(const HttpServerOptions *options, const HttpRequest *request, HttpResponse *response, char *detail, size_t detail_size) {
    char relative[HTTPD_PATH_CAPACITY];
    char joined[HTTPD_PATH_CAPACITY];
    char canonical[HTTPD_PATH_CAPACITY];
    PlatformDirEntry entry;
    const char *content_type;
    int fd;

    if (detail != NULL && detail_size > 0U) {
        detail[0] = '\0';
    }
    if (options == NULL || request == NULL || response == NULL) {
        rt_copy_string(detail, detail_size, "invalid server state");
        return 500;
    }

    if (request->path[0] == '\0' || rt_strcmp(request->path, "/") == 0) {
        rt_copy_string(relative, sizeof(relative), options->index_name);
    } else if (request->path[rt_strlen(request->path) - 1U] == '/') {
        if (tool_join_path(request->path + 1U, options->index_name, relative, sizeof(relative)) != 0) {
            rt_copy_string(detail, detail_size, "path too long");
            return 400;
        }
    } else {
        rt_copy_string(relative, sizeof(relative), request->path + 1U);
    }

    if (httpd_path_has_hidden_component(relative)) {
        rt_copy_string(detail, detail_size, "hidden paths are not served");
        return 403;
    }
    if (tool_join_path(options->root, relative, joined, sizeof(joined)) != 0) {
        rt_copy_string(detail, detail_size, "path too long");
        return 400;
    }
    if (tool_canonicalize_path(joined, 1, 1, canonical, sizeof(canonical)) != 0 || !httpd_path_is_within_root(options->root, canonical)) {
        rt_copy_string(detail, detail_size, "path rejected");
        return 403;
    }

    if (platform_get_path_info(canonical, &entry) != 0) {
        rt_copy_string(detail, detail_size, "file not found");
        return 404;
    }
    if (entry.is_dir) {
        rt_copy_string(detail, detail_size, "directory listing disabled");
        return 403;
    }
    if (entry.nlink > 1UL) {
        rt_copy_string(detail, detail_size, "multiply linked files are not served");
        return 403;
    }
    if (entry.size > (1024ULL * 1024ULL)) {
        rt_copy_string(detail, detail_size, "file too large for version one server");
        return 413;
    }

    fd = platform_open_read(canonical);
    if (fd < 0) {
        rt_copy_string(detail, detail_size, "failed to open file");
        return 500;
    }

    rt_memset(response, 0, sizeof(*response));
    response->status_code = 200;
    response->status_text = httpd_status_text(200);
    response->head_only = request->head_only;
    response->file_fd = fd;
    response->content_length = entry.size;
    content_type = httpd_content_type_for_path(canonical);
    rt_copy_string(response->content_type, sizeof(response->content_type), content_type);
    return 0;
}
