#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define TOP_MAX_PROCESSES 4096
#define TOP_MAX_FILTER_PIDS 128
#define TOP_DEFAULT_ROWS 20U

typedef enum {
    TOP_SORT_RSS,
    TOP_SORT_PID,
    TOP_SORT_PPID,
    TOP_SORT_UID,
    TOP_SORT_USER,
    TOP_SORT_STATE,
    TOP_SORT_COMMAND
} TopSortKey;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-b] [-n ROWS] [-p PID[,PID...]] [-o FIELD] [-r]");
}

static void trim_token(char *text) {
    size_t start = 0;
    size_t end = rt_strlen(text);

    while (start < end && rt_is_space(text[start])) {
        start += 1U;
    }
    while (end > start && rt_is_space(text[end - 1U])) {
        end -= 1U;
    }

    if (start > 0U) {
        memmove(text, text + start, end - start);
    }
    text[end - start] = '\0';
}

static int parse_pid_filters(const char *spec, int *pids_out, size_t *count_out) {
    size_t count = 0;
    size_t i = 0;

    while (spec[i] != '\0') {
        char token[32];
        size_t token_len = 0;
        long long pid_value = 0;

        while (spec[i] == ',' || rt_is_space(spec[i])) {
            i += 1U;
        }
        while (spec[i] != '\0' && spec[i] != ',' && token_len + 1U < sizeof(token)) {
            token[token_len++] = spec[i++];
        }
        token[token_len] = '\0';
        trim_token(token);

        if (token[0] == '\0' || count >= TOP_MAX_FILTER_PIDS ||
            tool_parse_int_arg(token, &pid_value, "top", "pid") != 0 || pid_value <= 0) {
            return -1;
        }

        pids_out[count++] = (int)pid_value;

        while (spec[i] != '\0' && spec[i] != ',') {
            i += 1U;
        }
    }

    if (count == 0U) {
        return -1;
    }

    *count_out = count;
    return 0;
}

static int parse_sort_key(const char *name, TopSortKey *key_out) {
    if (rt_strcmp(name, "rss") == 0 || rt_strcmp(name, "rss_kb") == 0 || rt_strcmp(name, "mem") == 0) {
        *key_out = TOP_SORT_RSS;
    } else if (rt_strcmp(name, "pid") == 0) {
        *key_out = TOP_SORT_PID;
    } else if (rt_strcmp(name, "ppid") == 0) {
        *key_out = TOP_SORT_PPID;
    } else if (rt_strcmp(name, "uid") == 0) {
        *key_out = TOP_SORT_UID;
    } else if (rt_strcmp(name, "user") == 0) {
        *key_out = TOP_SORT_USER;
    } else if (rt_strcmp(name, "state") == 0 || rt_strcmp(name, "stat") == 0) {
        *key_out = TOP_SORT_STATE;
    } else if (rt_strcmp(name, "command") == 0 || rt_strcmp(name, "cmd") == 0 || rt_strcmp(name, "comm") == 0) {
        *key_out = TOP_SORT_COMMAND;
    } else {
        return -1;
    }
    return 0;
}

static int pid_is_selected(int pid, const int *filters, size_t filter_count) {
    size_t i;

    if (filter_count == 0U) {
        return 1;
    }

    for (i = 0; i < filter_count; ++i) {
        if (filters[i] == pid) {
            return 1;
        }
    }

    return 0;
}

static size_t select_processes(const PlatformProcessEntry *source,
                               size_t source_count,
                               PlatformProcessEntry *dest,
                               size_t dest_capacity,
                               const int *filters,
                               size_t filter_count) {
    size_t count = 0;
    size_t i;

    for (i = 0; i < source_count && count < dest_capacity; ++i) {
        if (!pid_is_selected(source[i].pid, filters, filter_count)) {
            continue;
        }
        dest[count++] = source[i];
    }

    return count;
}

static int compare_numeric_ull(unsigned long long left, unsigned long long right) {
    if (left < right) {
        return -1;
    }
    if (left > right) {
        return 1;
    }
    return 0;
}

static int compare_numeric_int(int left, int right) {
    if (left < right) {
        return -1;
    }
    if (left > right) {
        return 1;
    }
    return 0;
}

static int compare_processes(const PlatformProcessEntry *left,
                             const PlatformProcessEntry *right,
                             TopSortKey key,
                             int reverse) {
    int cmp = 0;
    int descending = (key == TOP_SORT_RSS) ? 1 : 0;

    switch (key) {
        case TOP_SORT_PID:
            cmp = compare_numeric_int(left->pid, right->pid);
            break;
        case TOP_SORT_PPID:
            cmp = compare_numeric_int(left->ppid, right->ppid);
            break;
        case TOP_SORT_UID:
            cmp = compare_numeric_ull(left->uid, right->uid);
            break;
        case TOP_SORT_USER:
            cmp = rt_strcmp(left->user, right->user);
            break;
        case TOP_SORT_STATE:
            cmp = rt_strcmp(left->state, right->state);
            break;
        case TOP_SORT_COMMAND:
            cmp = rt_strcmp(left->name, right->name);
            break;
        case TOP_SORT_RSS:
        default:
            cmp = compare_numeric_ull(left->rss_kb, right->rss_kb);
            break;
    }

    if (cmp == 0) {
        cmp = compare_numeric_int(left->pid, right->pid);
    }

    if (reverse) {
        descending = !descending;
    }
    return descending ? -cmp : cmp;
}

static void sort_processes(PlatformProcessEntry *entries, size_t count, TopSortKey key, int reverse) {
    size_t i;
    size_t j;

    for (i = 0; i < count; ++i) {
        for (j = i + 1U; j < count; ++j) {
            if (compare_processes(&entries[i], &entries[j], key, reverse) > 0) {
                PlatformProcessEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }
}

static void write_padding(size_t count) {
    while (count > 0U) {
        (void)rt_write_char(1, ' ');
        count -= 1U;
    }
}

static void write_left_cell(const char *text, size_t width) {
    size_t length = rt_strlen(text);
    (void)rt_write_cstr(1, text);
    if (length < width) {
        write_padding(width - length);
    }
    (void)rt_write_char(1, ' ');
}

static void write_right_cell(const char *text, size_t width) {
    size_t length = rt_strlen(text);
    if (length < width) {
        write_padding(width - length);
    }
    (void)rt_write_cstr(1, text);
    (void)rt_write_char(1, ' ');
}

static size_t append_char(char *buffer, size_t buffer_size, size_t length, char ch) {
    if (buffer_size == 0U) {
        return 0U;
    }

    if (length + 1U < buffer_size) {
        buffer[length++] = ch;
        buffer[length] = '\0';
    } else {
        buffer[buffer_size - 1U] = '\0';
    }

    return length;
}

static size_t append_cstr(char *buffer, size_t buffer_size, size_t length, const char *text) {
    size_t i = 0;

    while (text != NULL && text[i] != '\0') {
        length = append_char(buffer, buffer_size, length, text[i]);
        i += 1U;
    }

    return length;
}

static size_t append_uint(char *buffer, size_t buffer_size, size_t length, unsigned long long value) {
    char digits[32];
    rt_unsigned_to_string(value, digits, sizeof(digits));
    return append_cstr(buffer, buffer_size, length, digits);
}

static void format_uptime_compact(unsigned long long total_seconds, char *buffer, size_t buffer_size) {
    unsigned long long days = total_seconds / 86400ULL;
    unsigned long long hours = (total_seconds % 86400ULL) / 3600ULL;
    unsigned long long minutes = (total_seconds % 3600ULL) / 60ULL;
    unsigned long long seconds = total_seconds % 60ULL;
    size_t length = 0;

    if (buffer_size == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (days > 0ULL) {
        length = append_uint(buffer, buffer_size, length, days);
        length = append_char(buffer, buffer_size, length, 'd');
        length = append_char(buffer, buffer_size, length, ' ');
        length = append_uint(buffer, buffer_size, length, hours);
        length = append_char(buffer, buffer_size, length, 'h');
        length = append_char(buffer, buffer_size, length, ' ');
        length = append_uint(buffer, buffer_size, length, minutes);
        (void)append_char(buffer, buffer_size, length, 'm');
    } else if (hours > 0ULL) {
        length = append_uint(buffer, buffer_size, length, hours);
        length = append_char(buffer, buffer_size, length, 'h');
        length = append_char(buffer, buffer_size, length, ' ');
        length = append_uint(buffer, buffer_size, length, minutes);
        (void)append_char(buffer, buffer_size, length, 'm');
    } else if (minutes > 0ULL) {
        length = append_uint(buffer, buffer_size, length, minutes);
        (void)append_char(buffer, buffer_size, length, 'm');
    } else {
        length = append_uint(buffer, buffer_size, length, seconds);
        (void)append_char(buffer, buffer_size, length, 's');
    }
}

static void format_memory_value(unsigned long long bytes, char *buffer, size_t buffer_size) {
    tool_format_size(bytes, 1, buffer, buffer_size);
}

static void summarize_states(const PlatformProcessEntry *entries,
                             size_t count,
                             unsigned long long *running_out,
                             unsigned long long *sleeping_out,
                             unsigned long long *stopped_out,
                             unsigned long long *zombie_out) {
    size_t i;

    *running_out = 0ULL;
    *sleeping_out = 0ULL;
    *stopped_out = 0ULL;
    *zombie_out = 0ULL;

    for (i = 0; i < count; ++i) {
        char state = entries[i].state[0];

        if (state == 'R') {
            *running_out += 1ULL;
        } else if (state == 'T') {
            *stopped_out += 1ULL;
        } else if (state == 'Z') {
            *zombie_out += 1ULL;
        } else {
            *sleeping_out += 1ULL;
        }
    }
}

static void write_summary(const PlatformProcessEntry *entries, size_t count) {
    PlatformUptimeInfo uptime;
    PlatformMemoryInfo memory;
    char now_text[32];
    char uptime_text[64];
    char total_text[32];
    char used_text[32];
    char free_text[32];
    char avail_text[32];
    unsigned long long running = 0ULL;
    unsigned long long sleeping = 0ULL;
    unsigned long long stopped = 0ULL;
    unsigned long long zombie = 0ULL;
    int have_uptime;
    int have_memory;

    summarize_states(entries, count, &running, &sleeping, &stopped, &zombie);
    have_uptime = platform_get_uptime_info(&uptime) == 0;
    have_memory = platform_get_memory_info(&memory) == 0;
    now_text[0] = '\0';
    uptime_text[0] = '\0';

    if (platform_format_time(platform_get_epoch_time(), 1, "%H:%M:%S", now_text, sizeof(now_text)) != 0) {
        now_text[0] = '\0';
    }
    if (have_uptime) {
        format_uptime_compact(uptime.uptime_seconds, uptime_text, sizeof(uptime_text));
    }

    (void)rt_write_cstr(1, "top - ");
    if (now_text[0] != '\0') {
        (void)rt_write_cstr(1, now_text);
        (void)rt_write_cstr(1, " ");
    }
    if (have_uptime) {
        (void)rt_write_cstr(1, "up ");
        (void)rt_write_cstr(1, uptime_text);
        (void)rt_write_cstr(1, ", ");
    }
    (void)rt_write_uint(1, (unsigned long long)count);
    (void)rt_write_cstr(1, count == 1U ? " task" : " tasks");
    if (have_uptime) {
        (void)rt_write_cstr(1, ", load average: ");
        (void)rt_write_cstr(1, uptime.load_average[0] != '\0' ? uptime.load_average : "0.00 0.00 0.00");
    }
    (void)rt_write_char(1, '\n');

    (void)rt_write_cstr(1, "Tasks: ");
    (void)rt_write_uint(1, (unsigned long long)count);
    (void)rt_write_cstr(1, " total, ");
    (void)rt_write_uint(1, running);
    (void)rt_write_cstr(1, " running, ");
    (void)rt_write_uint(1, sleeping);
    (void)rt_write_cstr(1, " sleeping, ");
    (void)rt_write_uint(1, stopped);
    (void)rt_write_cstr(1, " stopped, ");
    (void)rt_write_uint(1, zombie);
    (void)rt_write_cstr(1, " zombie\n");

    if (have_memory) {
        unsigned long long buffer_cache = memory.buffer_bytes + memory.cache_bytes;
        unsigned long long used_bytes;

        if (memory.total_bytes > memory.free_bytes + buffer_cache) {
            used_bytes = memory.total_bytes - memory.free_bytes - buffer_cache;
        } else if (memory.total_bytes > memory.available_bytes) {
            used_bytes = memory.total_bytes - memory.available_bytes;
        } else {
            used_bytes = 0ULL;
        }

        format_memory_value(memory.total_bytes, total_text, sizeof(total_text));
        format_memory_value(used_bytes, used_text, sizeof(used_text));
        format_memory_value(memory.free_bytes, free_text, sizeof(free_text));
        format_memory_value(memory.available_bytes, avail_text, sizeof(avail_text));

        (void)rt_write_cstr(1, "Mem:   ");
        (void)rt_write_cstr(1, total_text);
        (void)rt_write_cstr(1, " total, ");
        (void)rt_write_cstr(1, used_text);
        (void)rt_write_cstr(1, " used, ");
        (void)rt_write_cstr(1, free_text);
        (void)rt_write_cstr(1, " free, ");
        (void)rt_write_cstr(1, avail_text);
        (void)rt_write_cstr(1, " avail\n");
    }

    (void)rt_write_char(1, '\n');
}

static void write_header_row(void) {
    write_left_cell("PID", 6U);
    write_left_cell("USER", 16U);
    write_left_cell("STATE", 6U);
    write_left_cell("RSS_KB", 10U);
    (void)rt_write_line(1, "COMMAND");
}

static void write_process_row(const PlatformProcessEntry *entry) {
    char pid_text[32];
    char rss_text[32];
    const char *user = entry->user[0] != '\0' ? entry->user : "?";
    const char *state = entry->state[0] != '\0' ? entry->state : "?";
    const char *name = entry->name[0] != '\0' ? entry->name : "?";

    rt_unsigned_to_string((unsigned long long)entry->pid, pid_text, sizeof(pid_text));
    rt_unsigned_to_string(entry->rss_kb, rss_text, sizeof(rss_text));

    write_left_cell(pid_text, 6U);
    write_left_cell(user, 16U);
    write_left_cell(state, 6U);
    write_right_cell(rss_text, 10U);
    (void)rt_write_line(1, name);
}

int main(int argc, char **argv) {
    static PlatformProcessEntry all_entries[TOP_MAX_PROCESSES];
    static PlatformProcessEntry selected_entries[TOP_MAX_PROCESSES];
    TopSortKey sort_key = TOP_SORT_RSS;
    unsigned long long row_limit = TOP_DEFAULT_ROWS;
    int pid_filters[TOP_MAX_FILTER_PIDS];
    size_t pid_filter_count = 0U;
    size_t process_count = 0U;
    size_t selected_count;
    ToolOptState options;
    int parse_result;
    int reverse = 0;

    tool_opt_init(&options, argc, argv, argv[0], "[-b] [-n ROWS] [-p PID[,PID...]] [-o FIELD] [-r]");

    while ((parse_result = tool_opt_next(&options)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(options.flag, "-b") == 0 || rt_strcmp(options.flag, "--batch") == 0) {
            continue;
        } else if (rt_strcmp(options.flag, "-n") == 0 || rt_strcmp(options.flag, "--lines") == 0) {
            if (tool_opt_require_value(&options) != 0 ||
                tool_parse_uint_arg(options.value, &row_limit, "top", "rows") != 0 ||
                row_limit == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (tool_starts_with(options.flag, "--lines=")) {
            if (tool_parse_uint_arg(options.flag + 8, &row_limit, "top", "rows") != 0 || row_limit == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (rt_strcmp(options.flag, "-p") == 0 || rt_strcmp(options.flag, "--pid") == 0) {
            if (tool_opt_require_value(&options) != 0 || parse_pid_filters(options.value, pid_filters, &pid_filter_count) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (tool_starts_with(options.flag, "--pid=")) {
            if (parse_pid_filters(options.flag + 6, pid_filters, &pid_filter_count) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (rt_strcmp(options.flag, "-o") == 0 || rt_strcmp(options.flag, "--sort") == 0) {
            if (tool_opt_require_value(&options) != 0 || parse_sort_key(options.value, &sort_key) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (tool_starts_with(options.flag, "--sort=")) {
            if (parse_sort_key(options.flag + 7, &sort_key) != 0) {
                print_usage(argv[0]);
                return 1;
            }
        } else if (rt_strcmp(options.flag, "-r") == 0 || rt_strcmp(options.flag, "--reverse") == 0) {
            reverse = 1;
        } else {
            tool_write_error("top", "unknown option: ", options.flag);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (parse_result == TOOL_OPT_HELP) {
        print_usage(argv[0]);
        return 0;
    }
    if (parse_result == TOOL_OPT_ERROR || options.argi != argc) {
        print_usage(argv[0]);
        return 1;
    }

    if (platform_list_processes(all_entries, TOP_MAX_PROCESSES, &process_count) != 0) {
        tool_write_error("top", "process information unavailable", 0);
        return 1;
    }

    selected_count = select_processes(all_entries,
                                      process_count,
                                      selected_entries,
                                      TOP_MAX_PROCESSES,
                                      pid_filters,
                                      pid_filter_count);

    sort_processes(selected_entries, selected_count, sort_key, reverse);
    write_summary(selected_entries, selected_count);
    write_header_row();

    if (row_limit > selected_count) {
        row_limit = (unsigned long long)selected_count;
    }

    while (row_limit > 0ULL) {
        size_t index = (size_t)((unsigned long long)selected_count - row_limit);
        write_process_row(&selected_entries[index]);
        row_limit -= 1ULL;
    }

    return 0;
}
