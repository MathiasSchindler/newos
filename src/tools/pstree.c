#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "tool_util.h"

#include <stdio.h>

#define PSTREE_MAX_PROCESSES 8192
#define PSTREE_NAME_CAPACITY 256
#define PSTREE_PREFIX_CAPACITY 512

typedef struct {
    int pid;
    int ppid;
    char name[PSTREE_NAME_CAPACITY];
} PstreeProcess;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[PID]");
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
    FILE *pipe = popen("/bin/ps -axo pid=,ppid=,comm=", "r");
    char line[512];
    size_t count = 0;

    if (pipe == 0) {
        pipe = popen("ps -axo pid=,ppid=,comm=", "r");
    }

    if (pipe == 0) {
        return -1;
    }

    while (fgets(line, sizeof(line), pipe) != 0 && count < capacity) {
        int pid = 0;
        int ppid = 0;
        char name[PSTREE_NAME_CAPACITY];

        name[0] = '\0';
        if (sscanf(line, " %d %d %255[^\n]", &pid, &ppid, name) == 3 && pid > 0) {
            entries[count].pid = pid;
            entries[count].ppid = ppid;
            copy_name(entries[count].name, sizeof(entries[count].name), name);
            count += 1;
        }
    }

    (void)pclose(pipe);
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

static void write_process_line(const PstreeProcess *entry, const char *prefix, int is_last, int is_root) {
    if (!is_root) {
        rt_write_cstr(1, prefix);
        rt_write_cstr(1, is_last ? "`- " : "|- ");
    }

    rt_write_cstr(1, entry->name);
    rt_write_char(1, '(');
    rt_write_int(1, (long long)entry->pid);
    rt_write_line(1, ")");
}

static void render_tree(const PstreeProcess *entries, size_t count, int pid, const char *prefix, int is_last, int is_root) {
    int index = find_process_index(entries, count, pid);
    char next_prefix[PSTREE_PREFIX_CAPACITY];
    size_t i;
    size_t child_count = 0;
    size_t child_seen = 0;

    if (index < 0) {
        return;
    }

    write_process_line(&entries[index], prefix, is_last, is_root);

    for (i = 0; i < count; ++i) {
        if (entries[i].ppid == pid) {
            child_count += 1;
        }
    }

    if (is_root) {
        next_prefix[0] = '\0';
    } else {
        int written = snprintf(next_prefix, sizeof(next_prefix), "%s%s", prefix, is_last ? "   " : "|  ");
        if (written < 0 || (size_t)written >= sizeof(next_prefix)) {
            next_prefix[0] = '\0';
        }
    }

    for (i = 0; i < count; ++i) {
        if (entries[i].ppid == pid) {
            child_seen += 1;
            render_tree(entries, count, entries[i].pid, next_prefix, child_seen == child_count, 0);
        }
    }
}

int main(int argc, char **argv) {
    PstreeProcess entries[PSTREE_MAX_PROCESSES];
    size_t count = 0;
    unsigned long long root_pid = 0;
    size_t i;
    int printed = 0;

    if (argc > 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (argc == 2 && tool_parse_uint_arg(argv[1], &root_pid, "pstree", "pid") != 0) {
        print_usage(argv[0]);
        return 1;
    }

    if (collect_processes(entries, PSTREE_MAX_PROCESSES, &count) != 0) {
        tool_write_error("pstree", "failed to inspect processes", 0);
        return 1;
    }

    sort_processes(entries, count);

    if (root_pid != 0) {
        if (find_process_index(entries, count, (int)root_pid) < 0) {
            tool_write_error("pstree", "unknown pid ", argv[1]);
            return 1;
        }
        render_tree(entries, count, (int)root_pid, "", 1, 1);
        return 0;
    }

    for (i = 0; i < count; ++i) {
        if (is_root_process(entries, count, i)) {
            if (printed) {
                rt_write_char(1, '\n');
            }
            render_tree(entries, count, entries[i].pid, "", 1, 1);
            printed = 1;
        }
    }

    return printed ? 0 : 1;
}
