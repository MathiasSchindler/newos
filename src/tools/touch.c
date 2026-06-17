#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int update_access;
    int update_modify;
    int no_create;
    int explicit_time;
    long long atime;
    long long mtime;
} TouchOptions;




static int touch_matches_zone_name(const char *text, size_t index, const char *name) {
    size_t offset = 0;

    if (text == 0 || name == 0) {
        return 0;
    }

    while (name[offset] != '\0') {
        if (text[index + offset] == '\0' ||
            tool_ascii_tolower(text[index + offset]) != tool_ascii_tolower(name[offset])) {
            return 0;
        }
        offset += 1U;
    }

    return 1;
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-acm] [-d DATETIME | -t STAMP | -r FILE] path ...");
}

static int parse_timezone_suffix(const char *text, size_t *index_io, int *offset_seconds_out) {
    size_t index = *index_io;

    while (rt_is_space(text[index])) {
        index += 1U;
    }

    if (text[index] == '\0') {
        *offset_seconds_out = 0;
        *index_io = index;
        return 0;
    }

    if (tool_ascii_tolower(text[index]) == 'z') {
        *offset_seconds_out = 0;
        *index_io = index + 1U;
        return 0;
    }

    if (touch_matches_zone_name(text, index, "utc") ||
        touch_matches_zone_name(text, index, "gmt")) {
        index += 3U;
        if (text[index] == '+' || text[index] == '-') {
            if (tool_parse_numeric_timezone_offset(text, &index, offset_seconds_out) != 0) {
                return -1;
            }
        } else {
            *offset_seconds_out = 0;
        }
        *index_io = index;
        return 0;
    }

    if (text[index] == '+' || text[index] == '-') {
        if (tool_parse_numeric_timezone_offset(text, &index, offset_seconds_out) != 0) {
            return -1;
        }
        *index_io = index;
        return 0;
    }

    return -1;
}

static int parse_datetime_text(const char *text, long long *epoch_out) {
    unsigned int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour = 0U;
    unsigned int minute = 0U;
    unsigned int second = 0U;
    size_t len;
    size_t offset = 10U;
    int timezone_offset_seconds = 0;
    long long explicit_epoch;

    if (text == 0 || epoch_out == 0 || text[0] == '\0') {
        return -1;
    }

    if (text[0] == '@') {
        if (tool_parse_int_arg(text + 1, &explicit_epoch, "touch", "epoch time") != 0) {
            return -1;
        }
        *epoch_out = explicit_epoch;
        return 0;
    }

    len = rt_strlen(text);
    if (len < 10U ||
        tool_parse_fixed_digits(text, 0U, 4U, &year) != 0 ||
        text[4] != '-' ||
        tool_parse_fixed_digits(text, 5U, 2U, &month) != 0 ||
        text[7] != '-' ||
        tool_parse_fixed_digits(text, 8U, 2U, &day) != 0) {
        return -1;
    }

    if (len > 10U) {
        while (rt_is_space(text[offset])) {
            offset += 1U;
        }
        if (text[offset] == 'T') {
            offset += 1U;
        }

        if (text[offset] == '+' || text[offset] == '-' || text[offset] == 'Z' ||
            tool_ascii_tolower(text[offset]) == 'u' || tool_ascii_tolower(text[offset]) == 'g') {
            if (parse_timezone_suffix(text, &offset, &timezone_offset_seconds) != 0) {
                return -1;
            }
        } else {
            if (len < offset + 5U ||
                tool_parse_fixed_digits(text, offset, 2U, &hour) != 0 ||
                text[offset + 2U] != ':' ||
                tool_parse_fixed_digits(text, offset + 3U, 2U, &minute) != 0) {
                return -1;
            }
            offset += 5U;
            if (text[offset] == ':') {
                offset += 1U;
                if (tool_parse_fixed_digits(text, offset, 2U, &second) != 0) {
                    return -1;
                }
                offset += 2U;
            }
            if (text[offset] == '.') {
                offset += 1U;
                if (text[offset] < '0' || text[offset] > '9') {
                    return -1;
                }
                while (text[offset] >= '0' && text[offset] <= '9') {
                    offset += 1U;
                }
            }
            if (parse_timezone_suffix(text, &offset, &timezone_offset_seconds) != 0) {
                return -1;
            }
        }

        while (rt_is_space(text[offset])) {
            offset += 1U;
        }
        if (text[offset] != '\0') {
            return -1;
        }
    }

    if (tool_build_epoch_timestamp((int)year, month, day, hour, minute, second, epoch_out) != 0) {
        return -1;
    }
    *epoch_out -= (long long)timezone_offset_seconds;
    return 0;
}

static int current_year(void) {
    long long now = platform_get_epoch_time();
    long long days = now / 86400LL;
    int year = 1970;
    unsigned int month = 1U;
    unsigned int day = 1U;

    if (now < 0) {
        return 1970;
    }

    tool_civil_from_days(days, &year, &month, &day);
    return year;
}

static int parse_touch_stamp(const char *text, long long *epoch_out) {
    char digits[16];
    size_t input_index = 0;
    size_t digit_count = 0;
    unsigned int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second = 0U;

    if (text == 0 || epoch_out == 0 || text[0] == '\0') {
        return -1;
    }

    while (text[input_index] != '\0' && text[input_index] != '.') {
        if (digit_count + 1U >= sizeof(digits) || text[input_index] < '0' || text[input_index] > '9') {
            return -1;
        }
        digits[digit_count++] = text[input_index++];
    }
    digits[digit_count] = '\0';

    if (text[input_index] == '.') {
        if (tool_parse_fixed_digits(text, input_index + 1U, 2U, &second) != 0 || text[input_index + 3U] != '\0') {
            return -1;
        }
    }

    if (digit_count == 12U) {
        if (tool_parse_fixed_digits(digits, 0U, 4U, &year) != 0 ||
            tool_parse_fixed_digits(digits, 4U, 2U, &month) != 0 ||
            tool_parse_fixed_digits(digits, 6U, 2U, &day) != 0 ||
            tool_parse_fixed_digits(digits, 8U, 2U, &hour) != 0 ||
            tool_parse_fixed_digits(digits, 10U, 2U, &minute) != 0) {
            return -1;
        }
    } else if (digit_count == 10U) {
        unsigned int short_year;
        if (tool_parse_fixed_digits(digits, 0U, 2U, &short_year) != 0 ||
            tool_parse_fixed_digits(digits, 2U, 2U, &month) != 0 ||
            tool_parse_fixed_digits(digits, 4U, 2U, &day) != 0 ||
            tool_parse_fixed_digits(digits, 6U, 2U, &hour) != 0 ||
            tool_parse_fixed_digits(digits, 8U, 2U, &minute) != 0) {
            return -1;
        }
        year = (short_year >= 69U) ? (1900U + short_year) : (2000U + short_year);
    } else if (digit_count == 8U) {
        year = (unsigned int)current_year();
        if (tool_parse_fixed_digits(digits, 0U, 2U, &month) != 0 ||
            tool_parse_fixed_digits(digits, 2U, 2U, &day) != 0 ||
            tool_parse_fixed_digits(digits, 4U, 2U, &hour) != 0 ||
            tool_parse_fixed_digits(digits, 6U, 2U, &minute) != 0) {
            return -1;
        }
    } else {
        return -1;
    }

    return tool_build_epoch_timestamp((int)year, month, day, hour, minute, second, epoch_out);
}

static int load_reference_times(const char *path, TouchOptions *options) {
    PlatformDirEntry entry;
    char resolved[1024];
    const char *lookup_path = path;

    if (tool_canonicalize_path(path, 1, 0, resolved, sizeof(resolved)) == 0) {
        lookup_path = resolved;
    }

    if (platform_get_path_info(lookup_path, &entry) != 0) {
        return -1;
    }

    options->explicit_time = 1;
    options->atime = entry.atime;
    options->mtime = entry.mtime;
    return 0;
}

static int parse_time_selector(const char *text, TouchOptions *options) {
    if (text == 0 || options == 0) {
        return -1;
    }

    if (tool_str_equal_ignore_case_ascii(text, "access") ||
        tool_str_equal_ignore_case_ascii(text, "atime") ||
        tool_str_equal_ignore_case_ascii(text, "use")) {
        options->update_access = 1;
        return 0;
    }

    if (tool_str_equal_ignore_case_ascii(text, "modify") ||
        tool_str_equal_ignore_case_ascii(text, "mtime")) {
        options->update_modify = 1;
        return 0;
    }

    return -1;
}

int main(int argc, char **argv) {
    TouchOptions options;
    PlatformDirEntry existing_entry;
    int exit_code = 0;
    int argi = 1;
    int i;
    long long timestamp = 0;

    rt_memset(&options, 0, sizeof(options));

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag = argv[argi] + 1;
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "--no-create") == 0) {
            options.no_create = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--date") == 0 ||
            rt_strcmp(argv[argi], "--reference") == 0 ||
            rt_strcmp(argv[argi], "--time") == 0) {
            const char *value_text = 0;

            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            value_text = argv[argi + 1];

            if (rt_strcmp(argv[argi], "--date") == 0) {
                if (parse_datetime_text(value_text, &timestamp) != 0) {
                    tool_write_error("touch", "invalid date ", value_text);
                    return 1;
                }
                options.explicit_time = 1;
                options.atime = timestamp;
                options.mtime = timestamp;
            } else if (rt_strcmp(argv[argi], "--reference") == 0) {
                if (load_reference_times(value_text, &options) != 0) {
                    tool_write_error("touch", "cannot read reference ", value_text);
                    return 1;
                }
            } else {
                if (parse_time_selector(value_text, &options) != 0) {
                    tool_write_error("touch", "invalid time selector ", value_text);
                    return 1;
                }
            }

            argi += 2;
            continue;
        }
        if (tool_starts_with(argv[argi], "--date=")) {
            if (parse_datetime_text(argv[argi] + 7, &timestamp) != 0) {
                tool_write_error("touch", "invalid date ", argv[argi] + 7);
                return 1;
            }
            options.explicit_time = 1;
            options.atime = timestamp;
            options.mtime = timestamp;
            argi += 1;
            continue;
        }
        if (tool_starts_with(argv[argi], "--reference=")) {
            if (load_reference_times(argv[argi] + 12, &options) != 0) {
                tool_write_error("touch", "cannot read reference ", argv[argi] + 12);
                return 1;
            }
            argi += 1;
            continue;
        }
        if (tool_starts_with(argv[argi], "--time=")) {
            if (parse_time_selector(argv[argi] + 7, &options) != 0) {
                tool_write_error("touch", "invalid time selector ", argv[argi] + 7);
                return 1;
            }
            argi += 1;
            continue;
        }

        while (*flag != '\0') {
            char option = *flag;

            if (option == 'a') {
                options.update_access = 1;
                flag += 1;
            } else if (option == 'm') {
                options.update_modify = 1;
                flag += 1;
            } else if (option == 'c') {
                options.no_create = 1;
                flag += 1;
            } else if (option == 'd' || option == 't' || option == 'r') {
                const char *value_text = 0;

                if (flag[1] != '\0') {
                    value_text = flag + 1;
                    flag += rt_strlen(flag);
                } else {
                    if (argi + 1 >= argc) {
                        print_usage(argv[0]);
                        return 1;
                    }
                    value_text = argv[argi + 1];
                    argi += 1;
                    flag += 1;
                }

                if (option == 'd') {
                    if (parse_datetime_text(value_text, &timestamp) != 0) {
                        tool_write_error("touch", "invalid date ", value_text);
                        return 1;
                    }
                    options.explicit_time = 1;
                    options.atime = timestamp;
                    options.mtime = timestamp;
                } else if (option == 't') {
                    if (parse_touch_stamp(value_text, &timestamp) != 0) {
                        tool_write_error("touch", "invalid timestamp ", value_text);
                        return 1;
                    }
                    options.explicit_time = 1;
                    options.atime = timestamp;
                    options.mtime = timestamp;
                } else {
                    if (load_reference_times(value_text, &options) != 0) {
                        tool_write_error("touch", "cannot read reference ", value_text);
                        return 1;
                    }
                }
            } else {
                print_usage(argv[0]);
                return 1;
            }
        }

        argi += 1;
    }

    if (!options.update_access && !options.update_modify) {
        options.update_access = 1;
        options.update_modify = 1;
    }

    if (argi >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        long long desired_atime = options.atime;
        long long desired_mtime = options.mtime;
        int have_existing_times = (platform_get_path_info(argv[i], &existing_entry) == 0);

        if (!options.explicit_time) {
            timestamp = platform_get_epoch_time();
            if (timestamp < 0) {
                tool_write_error("touch", "cannot get current time for ", argv[i]);
                exit_code = 1;
                continue;
            }
            desired_atime = timestamp;
            desired_mtime = timestamp;
        }

        if (options.no_create && !tool_path_exists(argv[i])) {
            continue;
        }

        if (have_existing_times) {
            if (!options.update_access) {
                desired_atime = existing_entry.atime;
            } else if (!options.explicit_time && desired_atime <= existing_entry.atime) {
                desired_atime = existing_entry.atime + 1LL;
            }

            if (!options.update_modify) {
                desired_mtime = existing_entry.mtime;
            } else if (!options.explicit_time && desired_mtime <= existing_entry.mtime) {
                desired_mtime = existing_entry.mtime + 1LL;
            }
        }

        if (platform_set_path_times(
                argv[i],
                desired_atime,
                desired_mtime,
                options.no_create ? 0 : 1,
            options.update_access,
            options.update_modify
            ) != 0) {
            rt_write_cstr(2, "touch: cannot touch ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }
    }

    return exit_code;
}
