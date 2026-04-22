#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#include <limits.h>

static int parse_signed_value(const char *text, long long *value_out) {
    unsigned long long magnitude = 0;
    int negative = 0;

    if (text == 0 || text[0] == '\0' || value_out == 0) {
        return -1;
    }

    if (*text == '-') {
        negative = 1;
        text += 1;
    } else if (*text == '+') {
        text += 1;
    }

    if (*text == '\0') {
        return -1;
    }

    if (rt_parse_uint(text, &magnitude) != 0) {
        return -1;
    }

    if (!negative) {
        if (magnitude > (unsigned long long)LLONG_MAX) {
            return -1;
        }
        *value_out = (long long)magnitude;
    } else if (magnitude == (unsigned long long)LLONG_MAX + 1ULL) {
        *value_out = LLONG_MIN;
    } else {
        if (magnitude > (unsigned long long)LLONG_MAX) {
            return -1;
        }
        *value_out = -(long long)magnitude;
    }
    return 0;
}

static int parse_date_value(const char *text, long long *epoch_out) {
    long long now;

    if (text == 0 || epoch_out == 0) {
        return -1;
    }

    now = platform_get_epoch_time();
    if (rt_strcmp(text, "now") == 0 || rt_strcmp(text, "today") == 0) {
        *epoch_out = now;
        return 0;
    }
    if (rt_strcmp(text, "yesterday") == 0) {
        *epoch_out = now - 86400LL;
        return 0;
    }
    if (rt_strcmp(text, "tomorrow") == 0) {
        *epoch_out = now + 86400LL;
        return 0;
    }
    if (text[0] == '@') {
        return parse_signed_value(text + 1, epoch_out);
    }
    return parse_signed_value(text, epoch_out);
}

int main(int argc, char **argv) {
    int use_utc = 1;
    const char *format = 0;
    const char *date_value = 0;
    const char *reference_path = 0;
    const char *actual_format = "%Y-%m-%d %H:%M:%S";
    int argi;
    char output[256];
    long long epoch_seconds;

    for (argi = 1; argi < argc; ++argi) {
        if (rt_strcmp(argv[argi], "-u") == 0) {
            use_utc = 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-l") == 0) {
            use_utc = 0;
            continue;
        }
        if (rt_strcmp(argv[argi], "-d") == 0) {
            if (argi + 1 >= argc || date_value != 0) {
                tool_write_usage(argv[0], "[-u|-l] [-d TEXT|-r FILE] [+FORMAT]");
                return 1;
            }
            date_value = argv[argi + 1];
            argi += 1;
            continue;
        }
        if (tool_starts_with(argv[argi], "--date=")) {
            if (date_value != 0) {
                tool_write_usage(argv[0], "[-u|-l] [-d TEXT|-r FILE] [+FORMAT]");
                return 1;
            }
            date_value = argv[argi] + 7;
            continue;
        }
        if (rt_strcmp(argv[argi], "-r") == 0) {
            if (argi + 1 >= argc || reference_path != 0) {
                tool_write_usage(argv[0], "[-u|-l] [-d TEXT|-r FILE] [+FORMAT]");
                return 1;
            }
            reference_path = argv[argi + 1];
            argi += 1;
            continue;
        }
        if (tool_starts_with(argv[argi], "--reference=")) {
            if (reference_path != 0) {
                tool_write_usage(argv[0], "[-u|-l] [-d TEXT|-r FILE] [+FORMAT]");
                return 1;
            }
            reference_path = argv[argi] + 12;
            continue;
        }
        if (argv[argi][0] == '+' && argv[argi][1] != '\0' && format == 0) {
            format = argv[argi] + 1;
            continue;
        }
        tool_write_usage(argv[0], "[-u|-l] [-d TEXT|-r FILE] [+FORMAT]");
        return 1;
    }

    if (date_value != 0 && reference_path != 0) {
        tool_write_usage(argv[0], "[-u|-l] [-d TEXT|-r FILE] [+FORMAT]");
        return 1;
    }

    if (format != 0) {
        actual_format = format;
    }

    if (reference_path != 0) {
        PlatformDirEntry entry;

        if (platform_get_path_info(reference_path, &entry) != 0) {
            tool_write_error("date", "cannot read reference file ", reference_path);
            return 1;
        }
        epoch_seconds = entry.mtime;
    } else if (date_value != 0) {
        if (parse_date_value(date_value, &epoch_seconds) != 0) {
            tool_write_error("date", "unsupported date value: ", date_value);
            return 1;
        }
    } else {
        epoch_seconds = platform_get_epoch_time();
    }

    if (platform_format_time(epoch_seconds, use_utc ? 0 : 1, actual_format, output, sizeof(output)) != 0) {
        if (platform_format_time(epoch_seconds, 0, "%Y-%m-%d %H:%M:%S", output, sizeof(output)) != 0) {
            tool_write_error("date", "failed to format time", 0);
            return 1;
        }
        use_utc = 1;
        format = 0;
    }

    if (rt_write_cstr(1, output) != 0) {
        return 1;
    }
    if (format == 0 && use_utc) {
        return rt_write_line(1, " UTC") == 0 ? 0 : 1;
    }
    return rt_write_char(1, '\n') == 0 ? 0 : 1;
}
