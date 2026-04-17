#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define MAX_PROCESSES 4096
#define PS_MAX_FILTER_PIDS 128
#define PS_MAX_FIELDS 8

typedef enum {
    PS_FIELD_PID,
    PS_FIELD_PPID,
    PS_FIELD_USER,
    PS_FIELD_STAT,
    PS_FIELD_RSS,
    PS_FIELD_COMMAND
} PsField;

static void sort_processes(PlatformProcessEntry *entries, size_t count) {
    size_t i;
    size_t j;

    for (i = 0; i < count; ++i) {
        for (j = i + 1; j < count; ++j) {
            if (entries[j].pid < entries[i].pid) {
                PlatformProcessEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-f] [-h] [-p PID[,PID...]] [-o FIELD[,FIELD...]]");
}

static unsigned int field_width(PsField field) {
    switch (field) {
        case PS_FIELD_PID:
        case PS_FIELD_PPID:
            return 5U;
        case PS_FIELD_USER:
            return 8U;
        case PS_FIELD_STAT:
            return 4U;
        case PS_FIELD_RSS:
            return 7U;
        case PS_FIELD_COMMAND:
        default:
            return 0U;
    }
}

static const char *field_header(PsField field) {
    switch (field) {
        case PS_FIELD_PID: return "PID";
        case PS_FIELD_PPID: return "PPID";
        case PS_FIELD_USER: return "USER";
        case PS_FIELD_STAT: return "STAT";
        case PS_FIELD_RSS: return "RSS_KB";
        case PS_FIELD_COMMAND: return "COMMAND";
        default: return "?";
    }
}

static int parse_field_name(const char *name, PsField *field_out) {
    if (rt_strcmp(name, "pid") == 0) {
        *field_out = PS_FIELD_PID;
    } else if (rt_strcmp(name, "ppid") == 0) {
        *field_out = PS_FIELD_PPID;
    } else if (rt_strcmp(name, "user") == 0) {
        *field_out = PS_FIELD_USER;
    } else if (rt_strcmp(name, "stat") == 0 || rt_strcmp(name, "state") == 0) {
        *field_out = PS_FIELD_STAT;
    } else if (rt_strcmp(name, "rss") == 0 || rt_strcmp(name, "rss_kb") == 0) {
        *field_out = PS_FIELD_RSS;
    } else if (rt_strcmp(name, "command") == 0 || rt_strcmp(name, "cmd") == 0 || rt_strcmp(name, "comm") == 0) {
        *field_out = PS_FIELD_COMMAND;
    } else {
        return -1;
    }
    return 0;
}

static int parse_field_list(const char *spec, PsField *fields_out, size_t *count_out) {
    size_t count = 0;
    size_t i = 0;

    while (spec[i] != '\0') {
        char token[32];
        size_t token_len = 0;
        PsField field;

        while (spec[i] == ',') {
            i += 1;
        }
        while (spec[i] != '\0' && spec[i] != ',' && token_len + 1 < sizeof(token)) {
            token[token_len++] = spec[i++];
        }
        token[token_len] = '\0';

        if (token_len == 0 || count >= PS_MAX_FIELDS || parse_field_name(token, &field) != 0) {
            return -1;
        }
        fields_out[count++] = field;

        while (spec[i] != '\0' && spec[i] != ',') {
            i += 1;
        }
    }

    if (count == 0) {
        return -1;
    }

    *count_out = count;
    return 0;
}

static int parse_pid_filters(const char *spec, int *pids_out, size_t *count_out) {
    size_t count = 0;
    size_t i = 0;

    while (spec[i] != '\0') {
        char token[32];
        size_t token_len = 0;
        long long pid_value = 0;

        while (spec[i] == ',') {
            i += 1;
        }
        while (spec[i] != '\0' && spec[i] != ',' && token_len + 1 < sizeof(token)) {
            token[token_len++] = spec[i++];
        }
        token[token_len] = '\0';

        if (token_len == 0 || count >= PS_MAX_FILTER_PIDS || tool_parse_int_arg(token, &pid_value, "ps", "pid") != 0 || pid_value <= 0) {
            return -1;
        }
        pids_out[count++] = (int)pid_value;

        while (spec[i] != '\0' && spec[i] != ',') {
            i += 1;
        }
    }

    *count_out = count;
    return count == 0 ? -1 : 0;
}

static int pid_is_selected(const int *filters, size_t filter_count, int pid) {
    size_t i;

    if (filter_count == 0) {
        return 1;
    }

    for (i = 0; i < filter_count; ++i) {
        if (filters[i] == pid) {
            return 1;
        }
    }
    return 0;
}

static int write_spaces(unsigned int count) {
    while (count-- > 0U) {
        if (rt_write_char(1, ' ') != 0) {
            return -1;
        }
    }
    return 0;
}

static int write_text_cell(const char *text, unsigned int width, int is_last) {
    size_t len = rt_strlen(text);

    if (rt_write_cstr(1, text) != 0) {
        return -1;
    }
    if (!is_last) {
        if (width > len && write_spaces(width - (unsigned int)len) != 0) {
            return -1;
        }
        if (rt_write_char(1, ' ') != 0) {
            return -1;
        }
    }
    return 0;
}

static int write_entry_field(const PlatformProcessEntry *entry, PsField field, int is_last) {
    char number_buffer[32];

    switch (field) {
        case PS_FIELD_PID:
            rt_unsigned_to_string((unsigned long long)entry->pid, number_buffer, sizeof(number_buffer));
            return write_text_cell(number_buffer, field_width(field), is_last);
        case PS_FIELD_PPID:
            rt_unsigned_to_string((unsigned long long)entry->ppid, number_buffer, sizeof(number_buffer));
            return write_text_cell(number_buffer, field_width(field), is_last);
        case PS_FIELD_USER:
            return write_text_cell(entry->user[0] != '\0' ? entry->user : "?", field_width(field), is_last);
        case PS_FIELD_STAT:
            return write_text_cell(entry->state[0] != '\0' ? entry->state : "?", field_width(field), is_last);
        case PS_FIELD_RSS:
            rt_unsigned_to_string(entry->rss_kb, number_buffer, sizeof(number_buffer));
            return write_text_cell(number_buffer, field_width(field), is_last);
        case PS_FIELD_COMMAND:
        default:
            return write_text_cell(entry->name, 0U, is_last);
    }
}

int main(int argc, char **argv) {
    PlatformProcessEntry entries[MAX_PROCESSES];
    PsField fields[PS_MAX_FIELDS];
    size_t field_count = 0;
    int filter_pids[PS_MAX_FILTER_PIDS];
    size_t filter_count = 0;
    int show_headers = 1;
    size_t count = 0;
    size_t i;
    int argi;

    fields[field_count++] = PS_FIELD_PID;
    fields[field_count++] = PS_FIELD_PPID;
    fields[field_count++] = PS_FIELD_USER;
    fields[field_count++] = PS_FIELD_STAT;
    fields[field_count++] = PS_FIELD_RSS;
    fields[field_count++] = PS_FIELD_COMMAND;

    for (argi = 1; argi < argc; ++argi) {
        if (rt_strcmp(argv[argi], "-f") == 0 || rt_strcmp(argv[argi], "-e") == 0 ||
            rt_strcmp(argv[argi], "-A") == 0 || rt_strcmp(argv[argi], "-a") == 0 ||
            rt_strcmp(argv[argi], "-x") == 0) {
            continue;
        }
        if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--no-headers") == 0) {
            show_headers = 0;
            continue;
        }
        if (rt_strcmp(argv[argi], "-p") == 0) {
            if (argi + 1 >= argc || parse_pid_filters(argv[argi + 1], filter_pids, &filter_count) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-o") == 0) {
            if (argi + 1 >= argc || parse_field_list(argv[argi + 1], fields, &field_count) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 1;
            continue;
        }

        print_usage(argv[0]);
        return 1;
    }

    if (platform_list_processes(entries, MAX_PROCESSES, &count) != 0) {
        rt_write_line(2, "ps: not available on this platform");
        return 1;
    }

    sort_processes(entries, count);

    if (show_headers) {
        for (i = 0; i < field_count; ++i) {
            if (write_text_cell(field_header(fields[i]), field_width(fields[i]), i + 1 == field_count) != 0) {
                return 1;
            }
        }
        if (rt_write_char(1, '\n') != 0) {
            return 1;
        }
    }

    for (i = 0; i < count; ++i) {
        size_t j;

        if (!pid_is_selected(filter_pids, filter_count, entries[i].pid)) {
            continue;
        }
        for (j = 0; j < field_count; ++j) {
            if (write_entry_field(&entries[i], fields[j], j + 1 == field_count) != 0) {
                return 1;
            }
        }
        if (rt_write_char(1, '\n') != 0) {
            return 1;
        }
    }

    return 0;
}
