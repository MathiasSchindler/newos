#include "service_impl.h"

#include "runtime.h"
#include "simple_config.h"
#include "tool_util.h"

enum {
    SERVICE_DEFAULT_STOP_TIMEOUT_MS = 3000
};

static const char *service_path_leaf(const char *path) {
    const char *leaf = path;
    size_t index = 0U;

    if (path == NULL) {
        return "";
    }
    while (path[index] != '\0') {
        if (path[index] == '/') {
            leaf = path + index + 1U;
        }
        index += 1U;
    }
    return leaf;
}

static int service_normalize_path(char *path, size_t path_size, const char *base_dir, int allow_missing) {
    char joined[SERVICE_PATH_CAPACITY];
    char resolved[SERVICE_PATH_CAPACITY];

    if (path == NULL || path_size == 0U || path[0] == '\0') {
        return 0;
    }

    if (path[0] == '/' || base_dir == NULL || base_dir[0] == '\0') {
        rt_copy_string(joined, sizeof(joined), path);
    } else {
        if (tool_join_path(base_dir, path, joined, sizeof(joined)) != 0) {
            return -1;
        }
    }

    if (allow_missing) {
        char parent[SERVICE_PATH_CAPACITY];
        const char *leaf = service_path_leaf(joined);
        size_t length;

        rt_copy_string(parent, sizeof(parent), joined);
        length = rt_strlen(parent);
        while (length > 0U && parent[length - 1U] != '/') {
            parent[length - 1U] = '\0';
            length -= 1U;
        }
        while (length > 1U && parent[length - 1U] == '/') {
            parent[length - 1U] = '\0';
            length -= 1U;
        }
        if (parent[0] == '\0') {
            rt_copy_string(parent, sizeof(parent), ".");
        }
        if (tool_canonicalize_path(parent, 1, 0, resolved, sizeof(resolved)) != 0) {
            return -1;
        }
        if (leaf[0] != '\0') {
            if (tool_join_path(resolved, leaf, joined, sizeof(joined)) != 0) {
                return -1;
            }
            rt_copy_string(path, path_size, joined);
            return 0;
        }
    }

    if (tool_canonicalize_path(joined, 1, allow_missing, resolved, sizeof(resolved)) != 0) {
        return -1;
    }
    rt_copy_string(path, path_size, resolved);
    return 0;
}

static int service_copy_value(char *buffer, size_t buffer_size, const char *value) {
    if (buffer == NULL || value == NULL || rt_strlen(value) + 1U > buffer_size) {
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
    if (rt_strcmp(key, "user") == 0) {
        return service_copy_value(config->drop_user, sizeof(config->drop_user), value);
    }
    if (rt_strcmp(key, "group") == 0) {
        return service_copy_value(config->drop_group, sizeof(config->drop_group), value);
    }
    if (rt_strcmp(key, "stop_timeout") == 0) {
        return tool_parse_duration_ms(value, &config->stop_timeout_ms);
    }

    return -1;
}

static int service_set_default_pidfile(const char *config_path, ServiceConfig *config) {
    size_t used;

    if (config->pidfile[0] != '\0') {
        return 0;
    }
    if (config_path == NULL) {
        return -1;
    }

    used = rt_strlen(config_path);
    if (used + 5U > sizeof(config->pidfile)) {
        return -1;
    }
    rt_copy_string(config->pidfile, sizeof(config->pidfile), config_path);
    rt_copy_string(config->pidfile + used, sizeof(config->pidfile) - used, ".pid");
    return 0;
}

int service_load_config(const char *config_path, ServiceConfig *config_out) {
    if (config_path == NULL || config_out == NULL) {
        return -1;
    }

    rt_memset(config_out, 0, sizeof(*config_out));
    config_out->stop_timeout_ms = SERVICE_DEFAULT_STOP_TIMEOUT_MS;

    if (simple_config_parse_file(config_path, service_config_visitor, config_out) != 0) {
        return -1;
    }
    if (config_out->command[0] == '\0') {
        return -1;
    }
    if (config_out->workdir[0] != '\0' && service_normalize_path(config_out->workdir, sizeof(config_out->workdir), NULL, 0) != 0) {
        return -1;
    }
    if (service_set_default_pidfile(config_path, config_out) != 0) {
        return -1;
    }
    if (service_normalize_path(config_out->pidfile, sizeof(config_out->pidfile), config_out->workdir, 1) != 0) {
        return -1;
    }
    if (service_normalize_path(config_out->stdout_path, sizeof(config_out->stdout_path), config_out->workdir, 1) != 0) {
        return -1;
    }
    if (service_normalize_path(config_out->stderr_path, sizeof(config_out->stderr_path), config_out->workdir, 1) != 0) {
        return -1;
    }
    return 0;
}
