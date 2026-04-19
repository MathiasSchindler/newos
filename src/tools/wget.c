#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define WGET_SCHEME_HTTP  1
#define WGET_SCHEME_HTTPS 2
#define WGET_SCHEME_FILE  3
#define WGET_BUFFER_CAPACITY 4096
#define WGET_HEADER_CAPACITY 8192
#define WGET_URL_CAPACITY 2048

typedef struct {
    int scheme;
    unsigned int port;
    char host[256];
    char path[1024];
} WgetUrl;

typedef struct {
    const char *output_path;
    unsigned long long timeout_ms;
    int quiet;
    int show_headers;
    int output_to_stdout;
} WgetOptions;

static int open_output_for_url(const WgetOptions *options, const WgetUrl *url, char *path_out, size_t path_out_size, int *fd_out);

static int starts_with(const char *text, const char *prefix) {
    size_t index = 0;

    while (prefix[index] != '\0') {
        if (text[index] != prefix[index]) {
            return 0;
        }
        index += 1U;
    }

    return 1;
}

static char to_lower_ascii(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int equals_ignore_case_ascii(const char *left, const char *right) {
    size_t index = 0;

    while (left[index] != '\0' && right[index] != '\0') {
        if (to_lower_ascii(left[index]) != to_lower_ascii(right[index])) {
            return 0;
        }
        index += 1U;
    }

    return left[index] == '\0' && right[index] == '\0';
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-q] [-S] [-T TIMEOUT] [-O FILE] URL...");
}

static size_t buffer_append_char(char *buffer, size_t buffer_size, size_t length, char ch) {
    if (length + 1U < buffer_size) {
        buffer[length++] = ch;
        buffer[length] = '\0';
    }
    return length;
}

static size_t buffer_append_cstr(char *buffer, size_t buffer_size, size_t length, const char *text) {
    size_t index = 0;

    while (text != 0 && text[index] != '\0') {
        length = buffer_append_char(buffer, buffer_size, length, text[index]);
        index += 1U;
    }

    return length;
}

static size_t find_line_end(const char *text, size_t start) {
    size_t index = start;

    while (text[index] != '\0' && text[index] != '\n') {
        index += 1U;
    }
    return index;
}

static int parse_http_status(const char *headers) {
    size_t index = 0;
    int code = 0;
    int saw_digit = 0;

    while (headers[index] != '\0' && headers[index] != ' ') {
        index += 1U;
    }
    while (headers[index] == ' ') {
        index += 1U;
    }
    while (headers[index] >= '0' && headers[index] <= '9') {
        saw_digit = 1;
        code = (code * 10) + (int)(headers[index] - '0');
        index += 1U;
    }

    return saw_digit ? code : -1;
}

static int find_header_end(const char *buffer, size_t length, size_t *offset_out) {
    size_t index;

    for (index = 0; index + 3U < length; ++index) {
        if (buffer[index] == '\r' && buffer[index + 1U] == '\n' &&
            buffer[index + 2U] == '\r' && buffer[index + 3U] == '\n') {
            *offset_out = index + 4U;
            return 0;
        }
    }

    for (index = 0; index + 1U < length; ++index) {
        if (buffer[index] == '\n' && buffer[index + 1U] == '\n') {
            *offset_out = index + 2U;
            return 0;
        }
    }

    return -1;
}

static void trim_ascii_whitespace(char *text) {
    size_t start = 0;
    size_t end = rt_strlen(text);
    size_t index = 0;

    while (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' || text[start] == '\n') {
        start += 1U;
    }
    while (end > start &&
           (text[end - 1U] == ' ' || text[end - 1U] == '\t' || text[end - 1U] == '\r' || text[end - 1U] == '\n')) {
        end -= 1U;
    }

    while (start + index < end) {
        text[index] = text[start + index];
        index += 1U;
    }
    text[index] = '\0';
}

static void derive_output_name(const char *path, char *buffer, size_t buffer_size) {
    size_t length = 0;
    size_t segment_start = 0;
    size_t index = 0;
    size_t out_index = 0;

    if (path == 0 || buffer_size == 0U) {
        return;
    }

    while (path[length] != '\0' && path[length] != '?' && path[length] != '#') {
        length += 1U;
    }

    if (length == 0U) {
        rt_copy_string(buffer, buffer_size, "index.html");
        return;
    }

    for (index = 0; index < length; ++index) {
        if (path[index] == '/') {
            segment_start = index + 1U;
        }
    }

    if (segment_start >= length) {
        rt_copy_string(buffer, buffer_size, "index.html");
        return;
    }

    while (segment_start + out_index < length && out_index + 1U < buffer_size) {
        buffer[out_index] = path[segment_start + out_index];
        out_index += 1U;
    }
    buffer[out_index] = '\0';

    if (buffer[0] == '\0') {
        rt_copy_string(buffer, buffer_size, "index.html");
    }
}

static int parse_http_url(const char *text, unsigned int default_port, int scheme, WgetUrl *url_out) {
    size_t index = 0;
    size_t host_start = 0;
    size_t host_length = 0;
    unsigned long long parsed_port = default_port;
    int saw_port_digit = 0;

    rt_memset(url_out, 0, sizeof(*url_out));
    url_out->scheme = scheme;
    url_out->port = default_port;

    if (text[0] == '[') {
        host_start = 1U;
        while (text[index] != '\0' && text[index] != ']') {
            index += 1U;
        }
        if (text[index] != ']') {
            return -1;
        }
        host_length = index - host_start;
        index += 1U;
    } else {
        host_start = 0U;
        while (text[index] != '\0' && text[index] != '/' && text[index] != ':' &&
               text[index] != '?' && text[index] != '#') {
            index += 1U;
        }
        host_length = index;
    }

    if (host_length == 0U || host_length + 1U > sizeof(url_out->host)) {
        return -1;
    }

    memcpy(url_out->host, text + host_start, host_length);
    url_out->host[host_length] = '\0';

    if (text[index] == ':') {
        parsed_port = 0ULL;
        index += 1U;
        while (text[index] >= '0' && text[index] <= '9') {
            saw_port_digit = 1;
            parsed_port = (parsed_port * 10ULL) + (unsigned long long)(text[index] - '0');
            index += 1U;
        }
        if (!saw_port_digit || parsed_port == 0ULL || parsed_port > 65535ULL) {
            return -1;
        }
    }

    url_out->port = (unsigned int)parsed_port;

    if (text[index] == '\0') {
        rt_copy_string(url_out->path, sizeof(url_out->path), "/");
        return 0;
    }

    if (text[index] == '?' || text[index] == '#') {
        rt_copy_string(url_out->path, sizeof(url_out->path), "/");
        return 0;
    }

    rt_copy_string(url_out->path, sizeof(url_out->path), text + index);
    return 0;
}

static int parse_url(const char *text, WgetUrl *url_out) {
    if (text == 0 || url_out == 0) {
        return -1;
    }

    if (starts_with(text, "http://")) {
        return parse_http_url(text + 7, 80U, WGET_SCHEME_HTTP, url_out);
    }
    if (starts_with(text, "https://")) {
        return parse_http_url(text + 8, 443U, WGET_SCHEME_HTTPS, url_out);
    }
    if (starts_with(text, "file://")) {
        const char *path = text + 7;

        if (starts_with(path, "localhost/")) {
            path += 9;
        }
        rt_memset(url_out, 0, sizeof(*url_out));
        url_out->scheme = WGET_SCHEME_FILE;
        rt_copy_string(url_out->path, sizeof(url_out->path), path);
        return url_out->path[0] == '\0' ? -1 : 0;
    }

    return -1;
}

static void write_info_line(const char *left, const char *right) {
    rt_write_cstr(2, "wget: ");
    if (left != 0) {
        rt_write_cstr(2, left);
    }
    if (right != 0) {
        rt_write_cstr(2, right);
    }
    rt_write_char(2, '\n');
}

static int stream_fd_to_output(int input_fd, int output_fd) {
    char buffer[WGET_BUFFER_CAPACITY];
    long bytes_read;

    while ((bytes_read = platform_read(input_fd, buffer, sizeof(buffer))) > 0) {
        if (rt_write_all(output_fd, buffer, (size_t)bytes_read) != 0) {
            return -1;
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int compose_redirect_url(const WgetUrl *base, const char *location, char *buffer, size_t buffer_size) {
    size_t length = 0;
    char port_text[16];
    char base_dir[1024];
    size_t index = 0;

    if (starts_with(location, "http://") || starts_with(location, "https://") || starts_with(location, "file://")) {
        rt_copy_string(buffer, buffer_size, location);
        return 0;
    }

    if (base == 0 || base->scheme != WGET_SCHEME_HTTP) {
        return -1;
    }

    length = buffer_append_cstr(buffer, buffer_size, length, "http://");
    length = buffer_append_cstr(buffer, buffer_size, length, base->host);
    if (base->port != 80U) {
        rt_unsigned_to_string(base->port, port_text, sizeof(port_text));
        length = buffer_append_char(buffer, buffer_size, length, ':');
        length = buffer_append_cstr(buffer, buffer_size, length, port_text);
    }

    if (location[0] == '/') {
        (void)buffer_append_cstr(buffer, buffer_size, length, location);
        return 0;
    }

    rt_copy_string(base_dir, sizeof(base_dir), base->path);
    while (base_dir[index] != '\0' && base_dir[index] != '?' && base_dir[index] != '#') {
        index += 1U;
    }
    base_dir[index] = '\0';

    while (index > 1U && base_dir[index - 1U] != '/') {
        base_dir[index - 1U] = '\0';
        index -= 1U;
    }
    if (base_dir[0] == '\0') {
        rt_copy_string(base_dir, sizeof(base_dir), "/");
    }

    (void)buffer_append_cstr(buffer, buffer_size, length, base_dir);
    if (base_dir[rt_strlen(base_dir) - 1U] != '/') {
        length = rt_strlen(buffer);
        length = buffer_append_char(buffer, buffer_size, length, '/');
    }
    (void)buffer_append_cstr(buffer, buffer_size, rt_strlen(buffer), location);
    return 0;
}

static int parse_http_headers(
    const char *headers,
    int *status_code_out,
    char *location_out,
    size_t location_size
) {
    size_t line_start = 0;
    int line_index = 0;

    if (status_code_out == 0 || location_out == 0) {
        return -1;
    }

    location_out[0] = '\0';
    *status_code_out = parse_http_status(headers);
    if (*status_code_out < 0) {
        return -1;
    }

    while (headers[line_start] != '\0') {
        size_t line_end = find_line_end(headers, line_start);
        size_t length = (line_end > line_start) ? (line_end - line_start) : 0U;

        if (length > 0U && headers[line_end - 1U] == '\r') {
            length -= 1U;
        }

        if (line_index > 0 && length > 9U) {
            char line[1024];
            size_t copy_length = length < sizeof(line) - 1U ? length : sizeof(line) - 1U;
            size_t colon_index = 0;

            memcpy(line, headers + line_start, copy_length);
            line[copy_length] = '\0';

            while (line[colon_index] != '\0' && line[colon_index] != ':') {
                colon_index += 1U;
            }

            if (line[colon_index] == ':') {
                line[colon_index] = '\0';
                trim_ascii_whitespace(line);
                trim_ascii_whitespace(line + colon_index + 1U);
                if (equals_ignore_case_ascii(line, "Location")) {
                    rt_copy_string(location_out, location_size, line + colon_index + 1U);
                }
            }
        }

        if (headers[line_end] == '\0') {
            break;
        }
        line_start = line_end + 1U;
        line_index += 1;
    }

    trim_ascii_whitespace(location_out);
    return 0;
}

static int maybe_wait_for_socket(int socket_fd, unsigned long long timeout_ms) {
    int fds[1];
    size_t ready_index = 0;

    if (timeout_ms == 0ULL) {
        return 0;
    }

    fds[0] = socket_fd;
    return platform_poll_fds(fds, 1U, &ready_index, (int)timeout_ms) > 0 ? 0 : -1;
}

static int fetch_http_body(
    const WgetUrl *url,
    const WgetOptions *options,
    char *redirect_url,
    size_t redirect_size
) {
    int socket_fd = -1;
    int output_fd = -1;
    int should_close_output = 0;
    char output_path[512];
    char request[2048];
    char header_buffer[WGET_HEADER_CAPACITY];
    size_t request_length = 0;
    size_t header_length = 0;
    int header_complete = 0;
    int status_code = 0;
    char buffer[WGET_BUFFER_CAPACITY];
    long bytes_read = 0;

    if (platform_connect_tcp(url->host, url->port, &socket_fd) != 0) {
        return -1;
    }

    request_length = buffer_append_cstr(request, sizeof(request), request_length, "GET ");
    request_length = buffer_append_cstr(request, sizeof(request), request_length, url->path[0] != '\0' ? url->path : "/");
    request_length = buffer_append_cstr(request, sizeof(request), request_length, " HTTP/1.0\r\nHost: ");
    request_length = buffer_append_cstr(request, sizeof(request), request_length, url->host);
    if (url->port != 80U) {
        char port_text[16];
        rt_unsigned_to_string(url->port, port_text, sizeof(port_text));
        request_length = buffer_append_char(request, sizeof(request), request_length, ':');
        request_length = buffer_append_cstr(request, sizeof(request), request_length, port_text);
    }
    request_length = buffer_append_cstr(request, sizeof(request), request_length, "\r\nUser-Agent: newos-wget/0.1\r\nAccept: */*\r\nConnection: close\r\n\r\n");

    if (rt_write_all(socket_fd, request, request_length) != 0) {
        (void)platform_close(socket_fd);
        return -1;
    }

    for (;;) {
        if (maybe_wait_for_socket(socket_fd, options->timeout_ms) != 0) {
            bytes_read = -1;
            break;
        }

        bytes_read = platform_read(socket_fd, buffer, sizeof(buffer));
        if (bytes_read <= 0) {
            break;
        }

        if (!header_complete) {
            size_t body_offset = 0;

            if (header_length + (size_t)bytes_read >= sizeof(header_buffer)) {
                (void)platform_close(socket_fd);
                return -1;
            }

            memcpy(header_buffer + header_length, buffer, (size_t)bytes_read);
            header_length += (size_t)bytes_read;
            header_buffer[header_length] = '\0';

            if (find_header_end(header_buffer, header_length, &body_offset) != 0) {
                continue;
            }

            header_complete = 1;
            {
                char saved_char = header_buffer[body_offset];
                header_buffer[body_offset] = '\0';

                if (options->show_headers && rt_write_all(2, header_buffer, body_offset) != 0) {
                    (void)platform_close(socket_fd);
                    return -1;
                }

                if (parse_http_headers(header_buffer, &status_code, redirect_url, redirect_size) != 0) {
                    (void)platform_close(socket_fd);
                    return -1;
                }

                header_buffer[body_offset] = saved_char;
            }

            if (status_code >= 300 && status_code < 400 && redirect_url[0] != '\0') {
                (void)platform_close(socket_fd);
                return 1;
            }

            if (status_code < 200 || status_code >= 300) {
                (void)platform_close(socket_fd);
                return -1;
            }

            if (open_output_for_url(options, url, output_path, sizeof(output_path), &output_fd) != 0) {
                (void)platform_close(socket_fd);
                return -1;
            }
            should_close_output = output_fd > 1;
            if (!options->quiet && !options->output_to_stdout) {
                write_info_line("saving to ", output_path);
            }

            if (header_length > body_offset &&
                rt_write_all(output_fd, header_buffer + body_offset, header_length - body_offset) != 0) {
                if (should_close_output) {
                    (void)platform_close(output_fd);
                }
                (void)platform_close(socket_fd);
                return -1;
            }
        } else if (rt_write_all(output_fd, buffer, (size_t)bytes_read) != 0) {
            if (should_close_output) {
                (void)platform_close(output_fd);
            }
            (void)platform_close(socket_fd);
            return -1;
        }
    }

    (void)platform_close(socket_fd);
    if (should_close_output) {
        (void)platform_close(output_fd);
    }
    if (!header_complete || bytes_read < 0) {
        return -1;
    }

    return 0;
}

static int copy_file_url(const WgetUrl *url, int output_fd) {
    int input_fd = -1;
    int should_close = 0;
    int result;

    if (tool_open_input(url->path, &input_fd, &should_close) != 0) {
        return -1;
    }
    result = stream_fd_to_output(input_fd, output_fd);
    tool_close_input(input_fd, should_close);
    return result;
}

static int open_output_for_url(const WgetOptions *options, const WgetUrl *url, char *path_out, size_t path_out_size, int *fd_out) {
    char default_name[256];

    if (options->output_to_stdout) {
        *fd_out = 1;
        path_out[0] = '\0';
        return 0;
    }

    if (options->output_path != 0) {
        rt_copy_string(path_out, path_out_size, options->output_path);
    } else {
        derive_output_name(url->path, default_name, sizeof(default_name));
        rt_copy_string(path_out, path_out_size, default_name);
    }

    if (url->scheme == WGET_SCHEME_FILE && tool_paths_equal(url->path, path_out)) {
        tool_write_error("wget", "refusing to overwrite input file ", path_out);
        return -1;
    }

    *fd_out = platform_open_write(path_out, 0644U);
    return *fd_out < 0 ? -1 : 0;
}

static int fetch_one_url(const char *source_url, const WgetOptions *options) {
    char current_url[WGET_URL_CAPACITY];
    char redirect_url[WGET_URL_CAPACITY];
    int redirects = 0;

    rt_copy_string(current_url, sizeof(current_url), source_url);

    while (redirects < 5) {
        WgetUrl url;
        char output_path[512];
        int output_fd = -1;
        int result;

        if (parse_url(current_url, &url) != 0) {
            tool_write_error("wget", "unsupported URL ", current_url);
            return 1;
        }

        if (url.scheme == WGET_SCHEME_HTTPS) {
            tool_write_error("wget", "https is not yet supported for ", current_url);
            return 1;
        }

        if (url.scheme == WGET_SCHEME_FILE &&
            open_output_for_url(options, &url, output_path, sizeof(output_path), &output_fd) != 0) {
            tool_write_error("wget", "cannot open output for ", current_url);
            return 1;
        }

        if (url.scheme == WGET_SCHEME_FILE && !options->quiet && !options->output_to_stdout) {
            write_info_line("saving to ", output_path);
        }

        redirect_url[0] = '\0';
        if (url.scheme == WGET_SCHEME_FILE) {
            result = copy_file_url(&url, output_fd);
        } else {
            result = fetch_http_body(&url, options, redirect_url, sizeof(redirect_url));
        }

        if (url.scheme == WGET_SCHEME_FILE && output_fd > 1) {
            (void)platform_close(output_fd);
        }

        if (result == 0) {
            return 0;
        }

        if (result == 1) {
            if (compose_redirect_url(&url, redirect_url, current_url, sizeof(current_url)) != 0) {
                tool_write_error("wget", "cannot follow redirect for ", source_url);
                return 1;
            }
            if (!options->quiet) {
                write_info_line("redirecting to ", current_url);
            }
            redirects += 1;
            continue;
        }

        if (url.scheme == WGET_SCHEME_HTTP) {
            tool_write_error("wget", "request failed for ", current_url);
        } else {
            tool_write_error("wget", "cannot read ", url.path);
        }
        return 1;
    }

    tool_write_error("wget", "too many redirects for ", source_url);
    return 1;
}

int main(int argc, char **argv) {
    ToolOptState options_state;
    WgetOptions options;
    int parse_result;
    int exit_code = 0;
    int index;

    rt_memset(&options, 0, sizeof(options));
    tool_opt_init(&options_state, argc, argv, argv[0], "[-q] [-S] [-T TIMEOUT] [-O FILE] URL...");

    while ((parse_result = tool_opt_next(&options_state)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(options_state.flag, "-q") == 0 || rt_strcmp(options_state.flag, "--quiet") == 0) {
            options.quiet = 1;
        } else if (rt_strcmp(options_state.flag, "-S") == 0 || rt_strcmp(options_state.flag, "--server-response") == 0) {
            options.show_headers = 1;
        } else if (rt_strcmp(options_state.flag, "-O") == 0) {
            if (tool_opt_require_value(&options_state) != 0) {
                return 1;
            }
            options.output_path = options_state.value;
        } else if (starts_with(options_state.flag, "-O") && options_state.flag[2] != '\0') {
            options.output_path = options_state.flag + 2;
        } else if (rt_strcmp(options_state.flag, "-T") == 0 || rt_strcmp(options_state.flag, "--timeout") == 0) {
            if (tool_opt_require_value(&options_state) != 0 || tool_parse_duration_ms(options_state.value, &options.timeout_ms) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (starts_with(options_state.flag, "--timeout=")) {
            if (tool_parse_duration_ms(options_state.flag + 10, &options.timeout_ms) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else {
            tool_write_error("wget", "unknown option: ", options_state.flag);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (parse_result == TOOL_OPT_HELP) {
        print_usage(argv[0]);
        return 0;
    }

    if (options_state.argi >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    if (options.output_path != 0 && equals_ignore_case_ascii(options.output_path, "-")) {
        options.output_path = 0;
        options.output_to_stdout = 1;
    }

    if (options.output_path != 0 && argc - options_state.argi > 1) {
        tool_write_error("wget", "-O FILE expects a single URL", 0);
        return 1;
    }

    for (index = options_state.argi; index < argc; ++index) {
        if (fetch_one_url(argv[index], &options) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
