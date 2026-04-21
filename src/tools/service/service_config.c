#include "service_impl.h"

#include "runtime.h"
#include "simple_config.h"
#include "tool_util.h"

#include <errno.h>

enum {
    SERVICE_DEFAULT_STOP_TIMEOUT_MS = 3000
};

static int service_copy_value(char *buffer, size_t buffer_size, const char *value) {
    if (buffer == NULL || value == NULL || rt_strlen(value) + 1U > buffer_size) {
        errno = EINVAL;
        return -1;
    }
    rt_copy_string(buffer, buffer_size, value);
    return 0;
}

static int service_config_visitor(const char *key, const char *value, void *context) {
    ServiceConfig *config = (ServiceConfig *)context;

    if (rt_strcmp(key, "command") == 0) {
        return service_copy_value(config->command, sizeof(config->command), value);
    }
    if (rt_strcmp(key, "pidfile") == 0) {
        return service_copy_value(config->pidfile, sizeof(config->pidfile), value);
    }
    if (rt_strcmp(key, "stdout") == 0) {
        return service_copy_value(config->stdout_path, sizeof(config->stdout_path), value);
    }
    if (rt_strcmp(key, "stderr") == 0) {
        return service_copy_value(config->stderr_path, sizeof(config->stderr_path), value);
    }
    if (rt_strcmp(key, "workdir") == 0) {
        return service_copy_value(config->workdir, sizeof(config->workdir), value);
    }
    if (rt_strcmp(key, "stop_timeout") == 0) {
        return tool_parse_duration_ms(value, &config->stop_timeout_ms);
    }

    errno = EINVAL;
    return -1;
}

static int service_set_default_pidfile(const char *config_path, ServiceConfig *config) {
    size_t used;

    if (config->pidfile[0] != '\0') {
        return 0;
    }
    if (config_path == NULL) {
        errno = EINVAL;
        return -1;
    }

    used = rt_strlen(config_path);
    if (used + 5U > sizeof(config->pidfile)) {
        errno = EINVAL;
        return -1;
    }
    rt_copy_string(config->pidfile, sizeof(config->pidfile), config_path);
    rt_copy_string(config->pidfile + used, sizeof(config->pidfile) - used, ".pid");
    return 0;
}

int service_load_config(const char *config_path, ServiceConfig *config_out) {
    if (config_path == NULL || config_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    rt_memset(config_out, 0, sizeof(*config_out));
    config_out->stop_timeout_ms = SERVICE_DEFAULT_STOP_TIMEOUT_MS;

    if (simple_config_parse_file(config_path, service_config_visitor, config_out) != 0) {
        return -1;
    }
    if (config_out->command[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    return service_set_default_pidfile(config_path, config_out);
}
