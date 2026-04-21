#ifndef NEWOS_HTTPD_IMPL_H
#define NEWOS_HTTPD_IMPL_H

#include <stddef.h>

#include "platform.h"

#define HTTPD_REQUEST_CAPACITY 4096U
#define HTTPD_PATH_CAPACITY 1024U
#define HTTPD_INDEX_CAPACITY 64U
#define HTTPD_MAX_CONNECTIONS 32U

typedef struct {
    char bind_host[PLATFORM_NETWORK_TEXT_CAPACITY];
    char root[HTTPD_PATH_CAPACITY];
    char index_name[HTTPD_INDEX_CAPACITY];
    char drop_user[PLATFORM_NAME_CAPACITY];
    char drop_group[PLATFORM_NAME_CAPACITY];
    unsigned int port;
    unsigned int max_connections;
    unsigned int idle_timeout_ms;
    int quiet;
} HttpServerOptions;

typedef struct {
    int in_use;
    int fd;
    long long last_active;
    size_t request_length;
    char request[HTTPD_REQUEST_CAPACITY];
} HttpConnection;

typedef struct {
    char method[8];
    char path[HTTPD_PATH_CAPACITY];
    int head_only;
} HttpRequest;

typedef struct {
    int status_code;
    const char *status_text;
    int head_only;
    int file_fd;
    size_t body_length;
    unsigned long long content_length;
    char content_type[64];
    char body[512];
} HttpResponse;

int httpd_main(int argc, char **argv);
int httpd_open_listener(const HttpServerOptions *options, int *listen_fd_out);
void httpd_connection_init(HttpConnection *connection);
void httpd_connection_close(HttpConnection *connection);
int httpd_connection_process(HttpConnection *connection, const HttpServerOptions *options);
int httpd_parse_request(const char *buffer, HttpRequest *request, char *detail, size_t detail_size);
void httpd_build_response(const HttpServerOptions *options, const HttpRequest *request, HttpResponse *response, char *detail, size_t detail_size);
int httpd_build_static_response(const HttpServerOptions *options, const HttpRequest *request, HttpResponse *response, char *detail, size_t detail_size);
const char *httpd_status_text(int status_code);
void httpd_send_response(int fd, const HttpResponse *response);
void httpd_send_simple_response(int fd, int status_code, const char *detail);
void httpd_log_message(const HttpServerOptions *options, const char *level, const char *message, const char *detail);
void httpd_log_request(const HttpServerOptions *options, const HttpRequest *request, int status_code, const char *detail);

#endif
