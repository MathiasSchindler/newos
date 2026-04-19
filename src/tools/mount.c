#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define MOUNT_USAGE "[-rvwB] [-t TYPE] [-o OPTIONS] [SOURCE TARGET]"

static int token_equals(const char *text, size_t text_length, const char *token) {
    size_t i = 0U;

    while (i < text_length && token[i] != '\0') {
        if (text[i] != token[i]) {
            return 0;
        }
        i += 1U;
    }
    return i == text_length && token[i] == '\0';
}

static int starts_with(const char *text, const char *prefix) {
    size_t i = 0U;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }
    return 1;
}

static int append_data_token(char *buffer, size_t buffer_size, size_t *used, const char *token, size_t token_length) {
    if (buffer == 0 || used == 0) {
        return -1;
    }
    if (*used != 0U) {
        if (*used + 1U >= buffer_size) {
            return -1;
        }
        buffer[*used] = ',';
        *used += 1U;
    }
    if (*used + token_length + 1U > buffer_size) {
        return -1;
    }
    memcpy(buffer + *used, token, token_length);
    *used += token_length;
    buffer[*used] = '\0';
    return 0;
}

static int parse_option_token(
    const char *token,
    size_t token_length,
    unsigned long long *flags_io,
    char *data_buffer,
    size_t data_buffer_size,
    size_t *data_used_io
) {
    if (token_length == 0U || flags_io == 0 || data_used_io == 0) {
        return 0;
    }

    if (token_equals(token, token_length, "ro")) {
        *flags_io |= PLATFORM_MOUNT_RDONLY;
    } else if (token_equals(token, token_length, "rw")) {
        *flags_io &= ~PLATFORM_MOUNT_RDONLY;
    } else if (token_equals(token, token_length, "nosuid")) {
        *flags_io |= PLATFORM_MOUNT_NOSUID;
    } else if (token_equals(token, token_length, "nodev")) {
        *flags_io |= PLATFORM_MOUNT_NODEV;
    } else if (token_equals(token, token_length, "noexec")) {
        *flags_io |= PLATFORM_MOUNT_NOEXEC;
    } else if (token_equals(token, token_length, "sync")) {
        *flags_io |= PLATFORM_MOUNT_SYNC;
    } else if (token_equals(token, token_length, "remount")) {
        *flags_io |= PLATFORM_MOUNT_REMOUNT;
    } else if (token_equals(token, token_length, "mand")) {
        *flags_io |= PLATFORM_MOUNT_MANDLOCK;
    } else if (token_equals(token, token_length, "dirsync")) {
        *flags_io |= PLATFORM_MOUNT_DIRSYNC;
    } else if (token_equals(token, token_length, "noatime")) {
        *flags_io |= PLATFORM_MOUNT_NOATIME;
    } else if (token_equals(token, token_length, "nodiratime")) {
        *flags_io |= PLATFORM_MOUNT_NODIRATIME;
    } else if (token_equals(token, token_length, "bind")) {
        *flags_io |= PLATFORM_MOUNT_BIND;
    } else if (token_equals(token, token_length, "rbind") || token_equals(token, token_length, "bind,rec")) {
        *flags_io |= PLATFORM_MOUNT_BIND | PLATFORM_MOUNT_REC;
    } else if (token_equals(token, token_length, "rec")) {
        *flags_io |= PLATFORM_MOUNT_REC;
    } else if (token_equals(token, token_length, "silent")) {
        *flags_io |= PLATFORM_MOUNT_SILENT;
    } else if (token_equals(token, token_length, "relatime")) {
        *flags_io |= PLATFORM_MOUNT_RELATIME;
    } else if (token_equals(token, token_length, "strictatime")) {
        *flags_io |= PLATFORM_MOUNT_STRICTATIME;
    } else if (token_equals(token, token_length, "lazytime")) {
        *flags_io |= PLATFORM_MOUNT_LAZYTIME;
    } else {
        if (append_data_token(data_buffer, data_buffer_size, data_used_io, token, token_length) != 0) {
            return -1;
        }
    }

    return 0;
}

static int parse_mount_options(
    const char *text,
    unsigned long long *flags_io,
    char *data_buffer,
    size_t data_buffer_size
) {
    size_t index = 0U;
    size_t data_used = rt_strlen(data_buffer);

    if (text == 0 || flags_io == 0 || data_buffer == 0 || data_buffer_size == 0U) {
        return -1;
    }

    while (text[index] != '\0') {
        size_t start;
        size_t length;

        while (text[index] == ' ' || text[index] == '\t' || text[index] == ',') {
            index += 1U;
        }
        start = index;
        while (text[index] != '\0' && text[index] != ',') {
            index += 1U;
        }
        length = index - start;
        while (length > 0U && (text[start + length - 1U] == ' ' || text[start + length - 1U] == '\t')) {
            length -= 1U;
        }

        if (parse_option_token(text + start, length, flags_io, data_buffer, data_buffer_size, &data_used) != 0) {
            return -1;
        }
    }

    return 0;
}

static int list_mounts(void) {
    if (platform_stream_file_to_stdout("/proc/self/mounts") == 0) {
        return 0;
    }
    if (platform_stream_file_to_stdout("/proc/mounts") == 0) {
        return 0;
    }

    tool_write_error("mount", "mount listing is not available on this platform", 0);
    return 1;
}

static void print_help(const char *program_name) {
    rt_write_cstr(1, "Usage: ");
    rt_write_cstr(1, program_name);
    rt_write_cstr(1, " " MOUNT_USAGE "\n");
    rt_write_line(1, "Without SOURCE and TARGET, mount lists the current kernel mount table when available.");
}

int main(int argc, char **argv) {
    ToolOptState options;
    const char *filesystem_type = 0;
    char mount_data[512];
    unsigned long long flags = 0ULL;
    int verbose = 0;
    int parse_result;

    mount_data[0] = '\0';
    tool_opt_init(&options, argc, argv, tool_base_name(argv[0]), MOUNT_USAGE);

    while ((parse_result = tool_opt_next(&options)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(options.flag, "-t") == 0 || rt_strcmp(options.flag, "--types") == 0) {
            if (tool_opt_require_value(&options) != 0) {
                return 1;
            }
            filesystem_type = options.value;
        } else if (rt_strcmp(options.flag, "-o") == 0 || rt_strcmp(options.flag, "--options") == 0) {
            if (tool_opt_require_value(&options) != 0 ||
                parse_mount_options(options.value, &flags, mount_data, sizeof(mount_data)) != 0) {
                tool_write_error("mount", "invalid option list", 0);
                return 1;
            }
        } else if (starts_with(options.flag, "--options=")) {
            if (parse_mount_options(options.flag + 10, &flags, mount_data, sizeof(mount_data)) != 0) {
                tool_write_error("mount", "invalid option list", 0);
                return 1;
            }
        } else if (rt_strcmp(options.flag, "-r") == 0 || rt_strcmp(options.flag, "--read-only") == 0) {
            flags |= PLATFORM_MOUNT_RDONLY;
        } else if (rt_strcmp(options.flag, "-w") == 0 || rt_strcmp(options.flag, "--read-write") == 0) {
            flags &= ~PLATFORM_MOUNT_RDONLY;
        } else if (rt_strcmp(options.flag, "-B") == 0 || rt_strcmp(options.flag, "--bind") == 0) {
            flags |= PLATFORM_MOUNT_BIND;
        } else if (rt_strcmp(options.flag, "-v") == 0 || rt_strcmp(options.flag, "--verbose") == 0) {
            verbose = 1;
        } else {
            tool_write_error("mount", "unknown option: ", options.flag);
            print_help(argv[0]);
            return 1;
        }
    }

    if (parse_result == TOOL_OPT_HELP) {
        print_help(argv[0]);
        return 0;
    }

    if (options.argi >= argc) {
        return list_mounts();
    }
    if (argc - options.argi != 2) {
        print_help(argv[0]);
        return 1;
    }

    if (platform_mount_filesystem(argv[options.argi], argv[options.argi + 1], filesystem_type, flags,
                                  mount_data[0] != '\0' ? mount_data : 0) != 0) {
        tool_write_error("mount", "mount failed for ", argv[options.argi + 1]);
        return 1;
    }

    if (verbose) {
        rt_write_cstr(1, "mounted ");
        rt_write_cstr(1, argv[options.argi]);
        rt_write_cstr(1, " on ");
        rt_write_line(1, argv[options.argi + 1]);
    }
    return 0;
}
