#include "platform.h"
#include "runtime.h"

#define REALPATH_COMPONENT_CAPACITY 128
#define REALPATH_MAX_COMPONENTS 128
#define REALPATH_PATH_CAPACITY 2048

static int normalize_path(const char *input_path, char *output, size_t output_size) {
    char combined[REALPATH_PATH_CAPACITY];
    char components[REALPATH_MAX_COMPONENTS][REALPATH_COMPONENT_CAPACITY];
    size_t component_count = 0;
    size_t i = 0;
    size_t out_len = 0;

    if (input_path[0] == '/') {
        rt_copy_string(combined, sizeof(combined), input_path);
    } else {
        char cwd[REALPATH_PATH_CAPACITY];
        size_t cwd_len;

        if (platform_get_current_directory(cwd, sizeof(cwd)) != 0) {
            return -1;
        }
        cwd_len = rt_strlen(cwd);
        if (cwd_len + 1U + rt_strlen(input_path) + 1U > sizeof(combined)) {
            return -1;
        }
        rt_copy_string(combined, sizeof(combined), cwd);
        if (cwd_len > 0U && combined[cwd_len - 1U] != '/') {
            combined[cwd_len++] = '/';
            combined[cwd_len] = '\0';
        }
        rt_copy_string(combined + cwd_len, sizeof(combined) - cwd_len, input_path);
    }

    while (combined[i] != '\0') {
        char part[REALPATH_COMPONENT_CAPACITY];
        size_t part_len = 0;

        while (combined[i] == '/') {
            i += 1U;
        }
        if (combined[i] == '\0') {
            break;
        }

        while (combined[i] != '\0' && combined[i] != '/' && part_len + 1U < sizeof(part)) {
            part[part_len++] = combined[i++];
        }
        part[part_len] = '\0';

        while (combined[i] != '\0' && combined[i] != '/') {
            i += 1U;
        }

        if (rt_strcmp(part, ".") == 0) {
            continue;
        }
        if (rt_strcmp(part, "..") == 0) {
            if (component_count > 0U) {
                component_count -= 1U;
            }
            continue;
        }

        if (component_count >= REALPATH_MAX_COMPONENTS) {
            return -1;
        }
        rt_copy_string(components[component_count++], REALPATH_COMPONENT_CAPACITY, part);
    }

    if (output_size < 2U) {
        return -1;
    }

    output[out_len++] = '/';
    output[out_len] = '\0';

    for (i = 0; i < component_count; ++i) {
        size_t part_len = rt_strlen(components[i]);
        if (out_len + part_len + 2U > output_size) {
            return -1;
        }
        memcpy(output + out_len, components[i], part_len);
        out_len += part_len;
        if (i + 1U < component_count) {
            output[out_len++] = '/';
        }
        output[out_len] = '\0';
    }

    return 0;
}

int main(int argc, char **argv) {
    char resolved[REALPATH_PATH_CAPACITY];
    int i;
    int exit_code = 0;

    if (argc < 2) {
        rt_write_line(2, "Usage: realpath path ...");
        return 1;
    }

    for (i = 1; i < argc; ++i) {
        if (normalize_path(argv[i], resolved, sizeof(resolved)) != 0) {
            rt_write_cstr(2, "realpath: cannot resolve ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }
        rt_write_line(1, resolved);
    }

    return exit_code;
}