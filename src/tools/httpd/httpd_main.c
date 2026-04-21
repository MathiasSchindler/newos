#include "httpd_impl.h"

#include "runtime.h"
#include "simple_config.h"
#include "tool_util.h"

typedef struct {
    HttpServerOptions *options;
} HttpdConfigContext;

static void httpd_print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-q] [-b HOST] [-p PORT] [-r ROOT] [-i INDEX] [-m MAX] [-t TIMEOUT] [-c CONFIG]");
}

static int httpd_parse_boolean(const char *text, int *value_out) {
    if (text == NULL || value_out == NULL) {
        return -1;
    }
    if (rt_strcmp(text, "1") == 0 || rt_strcmp(text, "true") == 0 || rt_strcmp(text, "yes") == 0 || rt_strcmp(text, "on") == 0) {
        *value_out = 1;
        return 0;
    }
    if (rt_strcmp(text, "0") == 0 || rt_strcmp(text, "false") == 0 || rt_strcmp(text, "no") == 0 || rt_strcmp(text, "off") == 0) {
        *value_out = 0;
        return 0;
    }
    return -1;
}

static int httpd_apply_config_value(const char *key, const char *value, void *context) {
    HttpServerOptions *options = ((HttpdConfigContext *)context)->options;
    unsigned long long number = 0ULL;
    int enabled = 0;

    if (rt_strcmp(key, "bind") == 0) {
        rt_copy_string(options->bind_host, sizeof(options->bind_host), value);
        return 0;
    }
    if (rt_strcmp(key, "root") == 0) {
        rt_copy_string(options->root, sizeof(options->root), value);
        return 0;
    }
    if (rt_strcmp(key, "index") == 0) {
        rt_copy_string(options->index_name, sizeof(options->index_name), value);
        return 0;
    }
    if (rt_strcmp(key, "user") == 0) {
        rt_copy_string(options->drop_user, sizeof(options->drop_user), value);
        return 0;
    }
    if (rt_strcmp(key, "group") == 0) {
        rt_copy_string(options->drop_group, sizeof(options->drop_group), value);
        return 0;
    }
    if (rt_strcmp(key, "port") == 0) {
        if (rt_parse_uint(value, &number) != 0 || number == 0ULL || number > 65535ULL) {
            return -1;
        }
        options->port = (unsigned int)number;
        return 0;
    }
    if (rt_strcmp(key, "max_connections") == 0) {
        if (rt_parse_uint(value, &number) != 0 || number == 0ULL) {
            return -1;
        }
        options->max_connections = (unsigned int)number;
        return 0;
    }
    if (rt_strcmp(key, "idle_timeout") == 0) {
        if (tool_parse_duration_ms(value, &number) != 0) {
            return -1;
        }
        options->idle_timeout_ms = (unsigned int)number;
        return 0;
    }
    if (rt_strcmp(key, "quiet") == 0) {
        if (httpd_parse_boolean(value, &enabled) != 0) {
            return -1;
        }
        options->quiet = enabled;
        return 0;
    }

    return -1;
}

static int httpd_find_free_connection(HttpConnection *connections, size_t count) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (!connections[index].in_use) {
            return (int)index;
        }
    }
    return -1;
}

static void httpd_close_idle_connections(HttpConnection *connections, size_t count, unsigned int idle_timeout_ms) {
    size_t index;
    long long now = platform_get_epoch_time();
    long long timeout_seconds;

    if (idle_timeout_ms == 0U) {
        return;
    }
    timeout_seconds = (long long)((idle_timeout_ms + 999U) / 1000U);
    if (timeout_seconds <= 0) {
        timeout_seconds = 1;
    }

    for (index = 0U; index < count; ++index) {
        if (connections[index].in_use && now - connections[index].last_active >= timeout_seconds) {
            httpd_connection_close(&connections[index]);
        }
    }
}

int httpd_main(int argc, char **argv) {
    HttpServerOptions options;
    HttpConnection connections[HTTPD_MAX_CONNECTIONS];
    HttpdConfigContext config_context;
    ToolOptState opt;
    int parse_result;
    const char *config_path = NULL;
    int listener_fd = -1;
    int connection_map[HTTPD_MAX_CONNECTIONS + 1U];
    size_t index;

    rt_memset(&options, 0, sizeof(options));
    rt_copy_string(options.bind_host, sizeof(options.bind_host), "127.0.0.1");
    rt_copy_string(options.root, sizeof(options.root), ".");
    rt_copy_string(options.index_name, sizeof(options.index_name), "index.html");
    options.port = 8080U;
    options.max_connections = 16U;
    options.idle_timeout_ms = 5000U;
    options.quiet = 0;
    config_context.options = &options;

    tool_opt_init(&opt, argc, argv, tool_base_name(argv[0]), "[-q] [-b HOST] [-p PORT] [-r ROOT] [-i INDEX] [-m MAX] [-t TIMEOUT] [-c CONFIG]");
    while ((parse_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        unsigned long long number = 0ULL;

        if (rt_strcmp(opt.flag, "-q") == 0 || rt_strcmp(opt.flag, "--quiet") == 0) {
            options.quiet = 1;
        } else if (rt_strcmp(opt.flag, "-c") == 0 || rt_strcmp(opt.flag, "--config") == 0) {
            if (tool_opt_require_value(&opt) != 0) {
                httpd_print_usage(argv[0]);
                return 1;
            }
            config_path = opt.value;
        } else if (rt_strcmp(opt.flag, "-b") == 0 || rt_strcmp(opt.flag, "--bind") == 0) {
            if (tool_opt_require_value(&opt) != 0) {
                httpd_print_usage(argv[0]);
                return 1;
            }
            rt_copy_string(options.bind_host, sizeof(options.bind_host), opt.value);
        } else if (rt_strcmp(opt.flag, "-r") == 0 || rt_strcmp(opt.flag, "--root") == 0) {
            if (tool_opt_require_value(&opt) != 0) {
                httpd_print_usage(argv[0]);
                return 1;
            }
            rt_copy_string(options.root, sizeof(options.root), opt.value);
        } else if (rt_strcmp(opt.flag, "-i") == 0 || rt_strcmp(opt.flag, "--index") == 0) {
            if (tool_opt_require_value(&opt) != 0) {
                httpd_print_usage(argv[0]);
                return 1;
            }
            rt_copy_string(options.index_name, sizeof(options.index_name), opt.value);
        } else if (rt_strcmp(opt.flag, "-p") == 0 || rt_strcmp(opt.flag, "--port") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_parse_uint(opt.value, &number) != 0 || number == 0ULL || number > 65535ULL) {
                httpd_print_usage(argv[0]);
                return 1;
            }
            options.port = (unsigned int)number;
        } else if (rt_strcmp(opt.flag, "-m") == 0 || rt_strcmp(opt.flag, "--max-connections") == 0) {
            if (tool_opt_require_value(&opt) != 0 || rt_parse_uint(opt.value, &number) != 0 || number == 0ULL) {
                httpd_print_usage(argv[0]);
                return 1;
            }
            options.max_connections = (unsigned int)number;
        } else if (rt_strcmp(opt.flag, "-t") == 0 || rt_strcmp(opt.flag, "--idle-timeout") == 0) {
            if (tool_opt_require_value(&opt) != 0 || tool_parse_duration_ms(opt.value, &number) != 0) {
                httpd_print_usage(argv[0]);
                return 1;
            }
            options.idle_timeout_ms = (unsigned int)number;
        } else {
            tool_write_error("httpd", "unknown option: ", opt.flag);
            httpd_print_usage(argv[0]);
            return 1;
        }
    }

    if (parse_result == TOOL_OPT_HELP) {
        httpd_print_usage(argv[0]);
        rt_write_line(1, "Serve a small, bounded static HTTP tree with GET and HEAD support.");
        return 0;
    }

    if (config_path != NULL && simple_config_parse_file(config_path, httpd_apply_config_value, &config_context) != 0) {
        tool_write_error("httpd", "failed to parse config: ", config_path);
        return 1;
    }

    if (options.max_connections > HTTPD_MAX_CONNECTIONS) {
        options.max_connections = HTTPD_MAX_CONNECTIONS;
    }
    if (tool_canonicalize_path(options.root, 1, 0, options.root, sizeof(options.root)) != 0) {
        tool_write_error("httpd", "invalid document root: ", options.root);
        return 1;
    }
    if (platform_ignore_signal(13) != 0) {
        httpd_log_message(&options, "WARN", "failed to ignore broken pipe", "continuing");
    }
    if (httpd_open_listener(&options, &listener_fd) != 0) {
        tool_write_error("httpd", "failed to open listener", 0);
        return 1;
    }
    if (platform_drop_privileges(options.drop_user, options.drop_group) != 0) {
        tool_write_error("httpd", "failed to drop privileges", 0);
        (void)platform_close(listener_fd);
        return 1;
    }

    for (index = 0U; index < HTTPD_MAX_CONNECTIONS; ++index) {
        httpd_connection_init(&connections[index]);
    }
    httpd_log_message(&options, "INFO", "listening", options.root);

    for (;;) {
        int fds[HTTPD_MAX_CONNECTIONS + 1U];
        size_t fd_count = 1U;
        size_t ready_index = 0U;
        int poll_result;

        fds[0] = listener_fd;
        connection_map[0] = -1;
        for (index = 0U; index < options.max_connections; ++index) {
            if (connections[index].in_use) {
                fds[fd_count] = connections[index].fd;
                connection_map[fd_count] = (int)index;
                fd_count += 1U;
            }
        }

        httpd_close_idle_connections(connections, options.max_connections, options.idle_timeout_ms);
        poll_result = platform_poll_fds(fds, fd_count, &ready_index, 250);
        if (poll_result <= 0) {
            continue;
        }

        if (connection_map[ready_index] < 0) {
            int client_fd = -1;
            int slot = -1;

            if (platform_accept_tcp(listener_fd, &client_fd) != 0) {
                continue;
            }
            slot = httpd_find_free_connection(connections, options.max_connections);
            if (slot < 0) {
                httpd_send_simple_response(client_fd, 503, "server busy\n");
                (void)platform_close(client_fd);
                continue;
            }
            connections[slot].in_use = 1;
            connections[slot].fd = client_fd;
            connections[slot].last_active = platform_get_epoch_time();
            connections[slot].request_length = 0U;
            connections[slot].request[0] = '\0';
        } else {
            (void)httpd_connection_process(&connections[connection_map[ready_index]], &options);
        }
    }
}
