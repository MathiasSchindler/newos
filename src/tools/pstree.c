#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PSTREE_MAX_PROCESSES 4096
#define PSTREE_NAME_CAPACITY 256
#define PSTREE_PREFIX_CAPACITY 512

typedef struct {
    int pid;
    int ppid;
    char name[PSTREE_NAME_CAPACITY];
    char user[PSTREE_NAME_CAPACITY];
} PstreeProcess;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-u] [PID]");
}

static void copy_name(char *dst, size_t dst_size, const char *src) {
    rt_copy_string(dst, dst_size, (src != 0 && src[0] != '\0') ? src : "?");
}

static void sort_processes(PstreeProcess *entries, size_t count) {
    size_t i;
    size_t j;

    for (i = 0; i < count; ++i) {
        for (j = i + 1; j < count; ++j) {
            if (entries[j].pid < entries[i].pid) {
                PstreeProcess tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }
}

static int collect_processes(PstreeProcess *entries, size_t capacity, size_t *count_out) {
    static PlatformProcessEntry platform_entries[PSTREE_MAX_PROCESSES];
    size_t count = 0;
    size_t i;

    if (platform_list_processes(platform_entries, PSTREE_MAX_PROCESSES, &count) != 0) {
        return -1;
    }

    if (count > capacity) {
        count = capacity;
    }

    for (i = 0; i < count; ++i) {
        entries[i].pid = platform_entries[i].pid;
        entries[i].ppid = platform_entries[i].ppid;
        copy_name(entries[i].name, sizeof(entries[i].name), platform_entries[i].name);
        copy_name(entries[i].user, sizeof(entries[i].user), platform_entries[i].user);
    }

    *count_out = count;
    return count == 0 ? -1 : 0;
}

static int find_process_index(const PstreeProcess *entries, size_t count, int pid) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (entries[i].pid == pid) {
            return (int)i;
        }
    }

    return -1;
}

static int is_root_process(const PstreeProcess *entries, size_t count, size_t index) {
    return entries[index].ppid <= 1 || find_process_index(entries, count, entries[index].ppid) < 0;
}

static void write_process_line(const PstreeProcess *entry, const char *prefix, int is_last, int is_root, int show_user) {
    if (!is_root) {
        rt_write_cstr(1, prefix);
        rt_write_cstr(1, is_last ? "`- " : "|- ");
    }

    rt_write_cstr(1, entry->name);
    if (show_user && entry->user[0] != '\0') {
        rt_write_cstr(1, "{");
        rt_write_cstr(1, entry->user);
        rt_write_cstr(1, "}");
    }
    rt_write_char(1, '(');
    rt_write_int(1, (long long)entry->pid);
    rt_write_line(1, ")");
}

static void render_tree(const PstreeProcess *entries, size_t count, int pid, const char *prefix, int is_last, int is_root, int show_user) {
    int index = find_process_index(entries, count, pid);
    char next_prefix[PSTREE_PREFIX_CAPACITY];
    size_t i;
    size_t child_count = 0;
    size_t child_seen = 0;

    if (index < 0) {
        return;
    }

    write_process_line(&entries[index], prefix, is_last, is_root, show_user);

    for (i = 0; i < count; ++i) {
        if (entries[i].ppid == pid) {
            child_count += 1;
        }
    }

    if (is_root) {
        next_prefix[0] = '\0';
    } else {
        size_t prefix_len = rt_strlen(prefix);
        rt_copy_string(next_prefix, sizeof(next_prefix), prefix);
        if (prefix_len + 4 < sizeof(next_prefix)) {
            rt_copy_string(next_prefix + prefix_len, sizeof(next_prefix) - prefix_len, is_last ? "   " : "|  ");
        }
    }

    for (i = 0; i < count; ++i) {
        if (entries[i].ppid == pid) {
            child_seen += 1;
            render_tree(entries, count, entries[i].pid, next_prefix, child_seen == child_count, 0, show_user);
        }
    }
}

int main(int argc, char **argv) {
    static PstreeProcess entries[PSTREE_MAX_PROCESSES];
    size_t count = 0;
    unsigned long long root_pid = 0;
    size_t i;
    int printed = 0;
    int show_user = 0;

    if (argc > 3) {
        print_usage(argv[0]);
        return 1;
    }

    for (i = 1; i < (size_t)argc; ++i) {
        if (rt_strcmp(argv[i], "-u") == 0) {
            show_user = 1;
        } else if (tool_parse_uint_arg(argv[i], &root_pid, "pstree", "pid") != 0) {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (collect_processes(entries, PSTREE_MAX_PROCESSES, &count) != 0) {
        tool_write_error("pstree", "failed to inspect processes", 0);
        return 1;
    }

    sort_processes(entries, count);

    if (root_pid != 0) {
        if (find_process_index(entries, count, (int)root_pid) < 0) {
            tool_write_error("pstree", "unknown pid ", argc > 1 ? argv[argc - 1] : "");
            return 1;
        }
        render_tree(entries, count, (int)root_pid, "", 1, 1, show_user);
        return 0;
    }

    for (i = 0; i < count; ++i) {
        if (is_root_process(entries, count, i)) {
            if (printed) {
                rt_write_char(1, '\n');
            }
            render_tree(entries, count, entries[i].pid, "", 1, 1, show_user);
            printed = 1;
        }
    }

    return printed ? 0 : 1;
}
