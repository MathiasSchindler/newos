#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define MAX_PROCESSES 4096
#define PS_MAX_FILTER_PIDS 128
#define PS_MAX_FILTER_ITEMS 64
#define PS_MAX_COLUMNS 16
#define PS_HEADER_CAPACITY 32

typedef enum {
    PS_FIELD_PID,
    PS_FIELD_PPID,
    PS_FIELD_UID,
    PS_FIELD_USER,
    PS_FIELD_STAT,
    PS_FIELD_RSS,
    PS_FIELD_COMMAND
} PsField;

typedef enum {
    PS_SORT_PID,
    PS_SORT_PPID,
    PS_SORT_UID,
    PS_SORT_USER,
    PS_SORT_STATE,
    PS_SORT_RSS,
    PS_SORT_COMMAND
} PsSortKey;

typedef struct {
    PsField field;
    char header[PS_HEADER_CAPACITY];
} PsColumn;

static char ascii_fold_char(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int string_equal_ignore_case(const char *left, const char *right) {
    size_t i = 0;

    while (left[i] != '\0' && right[i] != '\0') {
        if (ascii_fold_char(left[i]) != ascii_fold_char(right[i])) {
            return 0;
        }
        i += 1;
    }
    return left[i] == '\0' && right[i] == '\0';
}

static int string_has_prefix_ignore_case(const char *text, const char *prefix) {
    size_t i = 0;

    while (prefix[i] != '\0') {
        if (text[i] == '\0' || ascii_fold_char(text[i]) != ascii_fold_char(prefix[i])) {
            return 0;
        }
        i += 1;
    }
    return 1;
}

static void trim_token(char *text) {
    size_t start = 0;
    size_t end = rt_strlen(text);

    while (start < end && rt_is_space(text[start])) {
        start += 1;
    }
    while (end > start && rt_is_space(text[end - 1])) {
        end -= 1;
    }

    if (start > 0) {
        memmove(text, text + start, end - start);
    }
    text[end - start] = '\0';
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name,
                     "[-f] [-e|-A|-a|-x] [-h|--no-headers] [-p PID[,PID...]] "
                     "[-u USER[,USER...]] [-s STATE[,STATE...]] "
                     "[-o FIELD[,FIELD...]] [--sort FIELD] [-r]");
}

static unsigned int field_width(PsField field) {
    switch (field) {
        case PS_FIELD_PID:
        case PS_FIELD_PPID:
        case PS_FIELD_UID:
            return 5U;
        case PS_FIELD_USER:
            return 16U;
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
        case PS_FIELD_UID: return "UID";
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
    } else if (rt_strcmp(name, "uid") == 0) {
        *field_out = PS_FIELD_UID;
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

static int parse_sort_key(const char *name, PsSortKey *key_out, int *reverse_out) {
    const char *spec = name;

    if (spec[0] == '-') {
        *reverse_out = !*reverse_out;
        spec += 1;
    } else if (spec[0] == '+') {
        spec += 1;
    }

    if (rt_strcmp(spec, "pid") == 0) {
        *key_out = PS_SORT_PID;
    } else if (rt_strcmp(spec, "ppid") == 0) {
        *key_out = PS_SORT_PPID;
    } else if (rt_strcmp(spec, "uid") == 0) {
        *key_out = PS_SORT_UID;
    } else if (rt_strcmp(spec, "user") == 0) {
        *key_out = PS_SORT_USER;
    } else if (rt_strcmp(spec, "stat") == 0 || rt_strcmp(spec, "state") == 0) {
        *key_out = PS_SORT_STATE;
    } else if (rt_strcmp(spec, "rss") == 0 || rt_strcmp(spec, "rss_kb") == 0) {
        *key_out = PS_SORT_RSS;
    } else if (rt_strcmp(spec, "command") == 0 || rt_strcmp(spec, "cmd") == 0 || rt_strcmp(spec, "comm") == 0) {
        *key_out = PS_SORT_COMMAND;
    } else {
        return -1;
    }

    return 0;
}

static int parse_field_list(const char *spec, PsColumn *columns_out, size_t *count_out) {
    size_t count = 0;
    size_t i = 0;

    while (spec[i] != '\0') {
        char token[64];
        char field_name[32];
        size_t token_len = 0;
        size_t j = 0;
        size_t header_pos = (size_t)-1;
        PsField field;

        while (spec[i] == ',' || rt_is_space(spec[i])) {
            i += 1;
        }
        while (spec[i] != '\0' && spec[i] != ',' && token_len + 1 < sizeof(token)) {
            token[token_len++] = spec[i++];
        }
        token[token_len] = '\0';
        trim_token(token);

        while (token[j] != '\0') {
            if (token[j] == '=') {
                header_pos = j;
                break;
            }
            j += 1;
        }

        if (header_pos == (size_t)-1) {
            rt_copy_string(field_name, sizeof(field_name), token);
        } else {
            if (header_pos >= sizeof(field_name)) {
                return -1;
            }
            memcpy(field_name, token, header_pos);
            field_name[header_pos] = '\0';
        }
        trim_token(field_name);

        if (field_name[0] == '\0' || count >= PS_MAX_COLUMNS || parse_field_name(field_name, &field) != 0) {
            return -1;
        }

        columns_out[count].field = field;
        if (header_pos == (size_t)-1) {
            rt_copy_string(columns_out[count].header, sizeof(columns_out[count].header), field_header(field));
        } else {
            rt_copy_string(columns_out[count].header,
                           sizeof(columns_out[count].header),
                           token + header_pos + 1);
            trim_token(columns_out[count].header);
        }
        count += 1;

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

        while (spec[i] == ',' || rt_is_space(spec[i])) {
            i += 1;
        }
        while (spec[i] != '\0' && spec[i] != ',' && token_len + 1 < sizeof(token)) {
            token[token_len++] = spec[i++];
        }
        token[token_len] = '\0';
        trim_token(token);

        if (token[0] == '\0' || count >= PS_MAX_FILTER_PIDS || tool_parse_int_arg(token, &pid_value, "ps", "pid") != 0 || pid_value <= 0) {
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

static int parse_user_filters(const char *spec,
                              char names_out[PS_MAX_FILTER_ITEMS][PLATFORM_NAME_CAPACITY],
                              unsigned int uids_out[PS_MAX_FILTER_ITEMS],
                              int is_uid_out[PS_MAX_FILTER_ITEMS],
                              size_t *count_out) {
    size_t count = 0;
    size_t i = 0;

    while (spec[i] != '\0') {
        char token[PLATFORM_NAME_CAPACITY];
        size_t token_len = 0;
        unsigned long long uid_value = 0ULL;

        while (spec[i] == ',' || rt_is_space(spec[i])) {
            i += 1;
        }
        while (spec[i] != '\0' && spec[i] != ',' && token_len + 1 < sizeof(token)) {
            token[token_len++] = spec[i++];
        }
        token[token_len] = '\0';
        trim_token(token);

        if (token[0] == '\0' || count >= PS_MAX_FILTER_ITEMS) {
            return -1;
        }

        rt_copy_string(names_out[count], PLATFORM_NAME_CAPACITY, token);
        if (rt_parse_uint(token, &uid_value) == 0) {
            uids_out[count] = (unsigned int)uid_value;
            is_uid_out[count] = 1;
        } else {
            uids_out[count] = 0U;
            is_uid_out[count] = 0;
        }
        count += 1;

        while (spec[i] != '\0' && spec[i] != ',') {
            i += 1;
        }
    }

    *count_out = count;
    return count == 0 ? -1 : 0;
}

static int parse_state_filters(const char *spec,
                               char states_out[PS_MAX_FILTER_ITEMS][16],
                               size_t *count_out) {
    size_t count = 0;
    size_t i = 0;

    while (spec[i] != '\0') {
        char token[16];
        size_t token_len = 0;
        size_t j;

        while (spec[i] == ',' || rt_is_space(spec[i])) {
            i += 1;
        }
        while (spec[i] != '\0' && spec[i] != ',' && token_len + 1 < sizeof(token)) {
            token[token_len++] = spec[i++];
        }
        token[token_len] = '\0';
        trim_token(token);

        if (token[0] == '\0' || count >= PS_MAX_FILTER_ITEMS) {
            return -1;
        }
        for (j = 0; token[j] != '\0'; ++j) {
            token[j] = ascii_fold_char(token[j]);
        }
        rt_copy_string(states_out[count++], sizeof(states_out[0]), token);

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

static int user_is_selected(const PlatformProcessEntry *entry,
                            char names[PS_MAX_FILTER_ITEMS][PLATFORM_NAME_CAPACITY],
                            const unsigned int *uids,
                            const int *is_uid,
                            size_t filter_count) {
    size_t i;

    if (filter_count == 0) {
        return 1;
    }

    for (i = 0; i < filter_count; ++i) {
        if (is_uid[i]) {
            if (entry->uid == uids[i]) {
                return 1;
            }
        } else if (string_equal_ignore_case(entry->user, names[i])) {
            return 1;
        }
    }
    return 0;
}

static int state_is_selected(const PlatformProcessEntry *entry,
                             char states[PS_MAX_FILTER_ITEMS][16],
                             size_t filter_count) {
    size_t i;

    if (filter_count == 0) {
        return 1;
    }

    for (i = 0; i < filter_count; ++i) {
        if (string_has_prefix_ignore_case(entry->state, states[i])) {
            return 1;
        }
    }
    return 0;
}

static int compare_entries(const PlatformProcessEntry *left,
                           const PlatformProcessEntry *right,
                           PsSortKey key) {
    switch (key) {
        case PS_SORT_PPID:
            if (left->ppid != right->ppid) {
                return (left->ppid < right->ppid) ? -1 : 1;
            }
            break;
        case PS_SORT_UID:
            if (left->uid != right->uid) {
                return (left->uid < right->uid) ? -1 : 1;
            }
            break;
        case PS_SORT_USER: {
            int cmp = rt_strcmp(left->user, right->user);
            if (cmp != 0) {
                return cmp;
            }
            break;
        }
        case PS_SORT_STATE: {
            int cmp = rt_strcmp(left->state, right->state);
            if (cmp != 0) {
                return cmp;
            }
            break;
        }
        case PS_SORT_RSS:
            if (left->rss_kb != right->rss_kb) {
                return (left->rss_kb < right->rss_kb) ? -1 : 1;
            }
            break;
        case PS_SORT_COMMAND: {
            int cmp = rt_strcmp(left->name, right->name);
            if (cmp != 0) {
                return cmp;
            }
            break;
        }
        case PS_SORT_PID:
        default:
            if (left->pid != right->pid) {
                return (left->pid < right->pid) ? -1 : 1;
            }
            break;
    }

    if (left->pid != right->pid) {
        return (left->pid < right->pid) ? -1 : 1;
    }
    return 0;
}

static void sort_processes(PlatformProcessEntry *entries, size_t count, PsSortKey key, int reverse_sort) {
    size_t i;
    size_t j;

    for (i = 0; i < count; ++i) {
        for (j = i + 1; j < count; ++j) {
            int cmp = compare_entries(&entries[i], &entries[j], key);
            if ((reverse_sort && cmp < 0) || (!reverse_sort && cmp > 0)) {
                PlatformProcessEntry tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }
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
        case PS_FIELD_UID:
            rt_unsigned_to_string((unsigned long long)entry->uid, number_buffer, sizeof(number_buffer));
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
            return write_text_cell(entry->name[0] != '\0' ? entry->name : "?", 0U, is_last);
    }
}

static int has_only_bsd_flags(const char *arg) {
    size_t i = 0;

    if (arg[0] == '\0') {
        return 0;
    }

    while (arg[i] != '\0') {
        char ch = arg[i];
        if (ch != 'a' && ch != 'u' && ch != 'x' && ch != 'e' && ch != 'f' &&
            ch != 'A' && ch != 'h') {
            return 0;
        }
        i += 1;
    }
    return 1;
}

int main(int argc, char **argv) {
    PlatformProcessEntry entries[MAX_PROCESSES];
    PsColumn columns[PS_MAX_COLUMNS];
    int filter_pids[PS_MAX_FILTER_PIDS];
    char user_filters[PS_MAX_FILTER_ITEMS][PLATFORM_NAME_CAPACITY];
    unsigned int user_uids[PS_MAX_FILTER_ITEMS];
    int user_is_uid[PS_MAX_FILTER_ITEMS];
    char state_filters[PS_MAX_FILTER_ITEMS][16];
    size_t column_count = 0;
    size_t pid_filter_count = 0;
    size_t user_filter_count = 0;
    size_t state_filter_count = 0;
    int show_headers = 1;
    size_t count = 0;
    size_t i;
    PsSortKey sort_key = PS_SORT_PID;
    int reverse_sort = 0;

    columns[column_count].field = PS_FIELD_PID;
    rt_copy_string(columns[column_count++].header, PS_HEADER_CAPACITY, field_header(PS_FIELD_PID));
    columns[column_count].field = PS_FIELD_PPID;
    rt_copy_string(columns[column_count++].header, PS_HEADER_CAPACITY, field_header(PS_FIELD_PPID));
    columns[column_count].field = PS_FIELD_USER;
    rt_copy_string(columns[column_count++].header, PS_HEADER_CAPACITY, field_header(PS_FIELD_USER));
    columns[column_count].field = PS_FIELD_STAT;
    rt_copy_string(columns[column_count++].header, PS_HEADER_CAPACITY, field_header(PS_FIELD_STAT));
    columns[column_count].field = PS_FIELD_RSS;
    rt_copy_string(columns[column_count++].header, PS_HEADER_CAPACITY, field_header(PS_FIELD_RSS));
    columns[column_count].field = PS_FIELD_COMMAND;
    rt_copy_string(columns[column_count++].header, PS_HEADER_CAPACITY, field_header(PS_FIELD_COMMAND));

    for (i = 1; i < (size_t)argc; ++i) {
        const char *arg = argv[i];

        if (rt_strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (arg[0] != '-') {
            if (has_only_bsd_flags(arg)) {
                size_t j = 0;
                while (arg[j] != '\0') {
                    if (arg[j] == 'h') {
                        show_headers = 0;
                        break;
                    }
                    j += 1;
                }
                continue;
            }

            print_usage(argv[0]);
            return 1;
        }

        if (rt_strcmp(arg, "-f") == 0 || rt_strcmp(arg, "-e") == 0 || rt_strcmp(arg, "-A") == 0 ||
            rt_strcmp(arg, "-a") == 0 || rt_strcmp(arg, "-x") == 0) {
            continue;
        }
        if (rt_strcmp(arg, "-h") == 0 || rt_strcmp(arg, "--no-headers") == 0) {
            show_headers = 0;
            continue;
        }
        if (rt_strcmp(arg, "-r") == 0 || rt_strcmp(arg, "--reverse") == 0) {
            reverse_sort = !reverse_sort;
            continue;
        }
        if (rt_strncmp(arg, "--sort=", 7) == 0) {
            if (parse_sort_key(arg + 7, &sort_key, &reverse_sort) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            continue;
        }
        if (rt_strcmp(arg, "--sort") == 0) {
            if (i + 1 >= (size_t)argc || parse_sort_key(argv[++i], &sort_key, &reverse_sort) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            continue;
        }
        if (rt_strcmp(arg, "-p") == 0 || rt_strncmp(arg, "-p", 2) == 0) {
            const char *value = (arg[2] != '\0') ? arg + 2 : 0;
            if (value == 0) {
                if (i + 1 >= (size_t)argc) {
                    print_usage(argv[0]);
                    return 1;
                }
                value = argv[++i];
            }
            if (parse_pid_filters(value, filter_pids, &pid_filter_count) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            continue;
        }
        if (rt_strcmp(arg, "-u") == 0 || rt_strcmp(arg, "--user") == 0 || rt_strncmp(arg, "-u", 2) == 0) {
            const char *value = (arg[0] == '-' && arg[1] == 'u' && arg[2] != '\0') ? arg + 2 : 0;
            if (rt_strcmp(arg, "--user") == 0 || value == 0) {
                if (i + 1 >= (size_t)argc) {
                    print_usage(argv[0]);
                    return 1;
                }
                value = argv[++i];
            }
            if (parse_user_filters(value, user_filters, user_uids, user_is_uid, &user_filter_count) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            continue;
        }
        if (rt_strcmp(arg, "-s") == 0 || rt_strcmp(arg, "--state") == 0 || rt_strncmp(arg, "-s", 2) == 0) {
            const char *value = (arg[0] == '-' && arg[1] == 's' && arg[2] != '\0') ? arg + 2 : 0;
            if (rt_strcmp(arg, "--state") == 0 || value == 0) {
                if (i + 1 >= (size_t)argc) {
                    print_usage(argv[0]);
                    return 1;
                }
                value = argv[++i];
            }
            if (parse_state_filters(value, state_filters, &state_filter_count) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            continue;
        }
        if (rt_strcmp(arg, "-o") == 0 || rt_strncmp(arg, "-o", 2) == 0) {
            const char *value = (arg[2] != '\0') ? arg + 2 : 0;
            if (value == 0) {
                if (i + 1 >= (size_t)argc) {
                    print_usage(argv[0]);
                    return 1;
                }
                value = argv[++i];
            }
            if (parse_field_list(value, columns, &column_count) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            continue;
        }

        print_usage(argv[0]);
        return 1;
    }

    if (platform_list_processes(entries, MAX_PROCESSES, &count) != 0) {
        rt_write_line(2, "ps: not available on this platform");
        return 1;
    }

    sort_processes(entries, count, sort_key, reverse_sort);

    if (show_headers) {
        int has_visible_header = 0;
        for (i = 0; i < column_count; ++i) {
            if (columns[i].header[0] != '\0') {
                has_visible_header = 1;
                break;
            }
        }

        if (has_visible_header) {
            for (i = 0; i < column_count; ++i) {
                if (write_text_cell(columns[i].header, field_width(columns[i].field), i + 1 == column_count) != 0) {
                    return 1;
                }
            }
            if (rt_write_char(1, '\n') != 0) {
                return 1;
            }
        }
    }

    for (i = 0; i < count; ++i) {
        size_t j;

        if (!pid_is_selected(filter_pids, pid_filter_count, entries[i].pid) ||
            !user_is_selected(&entries[i], user_filters, user_uids, user_is_uid, user_filter_count) ||
            !state_is_selected(&entries[i], state_filters, state_filter_count)) {
            continue;
        }

        for (j = 0; j < column_count; ++j) {
            if (write_entry_field(&entries[i], columns[j].field, j + 1 == column_count) != 0) {
                return 1;
            }
        }
        if (rt_write_char(1, '\n') != 0) {
            return 1;
        }
    }

    return 0;
}
