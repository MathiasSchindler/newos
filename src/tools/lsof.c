#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define LSOF_MAX_PROCESSES 4096U
#define LSOF_MAX_FDS 4096U
#define LSOF_PATH_CAPACITY 512U

static PlatformProcessEntry lsof_processes[LSOF_MAX_PROCESSES];
static PlatformDirEntry lsof_fd_entries[LSOF_MAX_FDS];

static int is_decimal_text(const char *text) {
    size_t i = 0U;

    if (text == 0 || text[0] == '\0') {
        return 0;
    }
    while (text[i] != '\0') {
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        i += 1U;
    }
    return 1;
}

static int append_path(char *buffer, size_t buffer_size, const char *left, const char *right) {
    size_t used = 0U;
    size_t i = 0U;

    while (left[i] != '\0') {
        if (used + 1U >= buffer_size) return -1;
        buffer[used++] = left[i++];
    }
    if (used == 0U || buffer[used - 1U] != '/') {
        if (used + 1U >= buffer_size) return -1;
        buffer[used++] = '/';
    }
    i = 0U;
    while (right[i] != '\0') {
        if (used + 1U >= buffer_size) return -1;
        buffer[used++] = right[i++];
    }
    buffer[used] = '\0';
    return 0;
}

static int append_text(char *buffer, size_t buffer_size, size_t *used_io, const char *text) {
    size_t i = 0U;

    while (text[i] != '\0') {
        if (*used_io + 1U >= buffer_size) {
            return -1;
        }
        buffer[*used_io] = text[i];
        *used_io += 1U;
        buffer[*used_io] = '\0';
        i += 1U;
    }
    return 0;
}

static int contains_text(const char *text, const char *needle) {
    size_t i;
    size_t needle_len = rt_strlen(needle);

    if (needle_len == 0U) {
        return 1;
    }
    for (i = 0U; text[i] != '\0'; ++i) {
        if (rt_strncmp(text + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static const PlatformProcessEntry *find_process(const PlatformProcessEntry *processes, size_t count, int pid) {
    size_t i;

    for (i = 0U; i < count; ++i) {
        if (processes[i].pid == pid) {
            return processes + i;
        }
    }
    return 0;
}

static int path_matches_filters(const char *path, char **filters, int filter_count) {
    int i;

    if (filter_count == 0) {
        return 1;
    }
    for (i = 0; i < filter_count; ++i) {
        if (contains_text(path, filters[i])) {
            return 1;
        }
    }
    return 0;
}

static void print_row(const PlatformProcessEntry *process, const char *fd_name, const char *path) {
    rt_write_cstr(1, process != 0 && process->name[0] != '\0' ? process->name : "?");
    rt_write_char(1, ' ');
    rt_write_uint(1, process != 0 ? (unsigned long long)process->pid : 0ULL);
    rt_write_char(1, ' ');
    rt_write_cstr(1, process != 0 && process->user[0] != '\0' ? process->user : "?");
    rt_write_char(1, ' ');
    rt_write_cstr(1, fd_name);
    rt_write_char(1, ' ');
    rt_write_line(1, path);
}

static int list_pid_fds(int pid, const PlatformProcessEntry *processes, size_t process_count, char **filters, int filter_count, size_t *rows_out) {
    size_t count = 0U;
    size_t i;
    int is_dir = 0;
    char pid_text[32];
    char fd_dir[LSOF_PATH_CAPACITY];
    size_t used = 0U;
    const PlatformProcessEntry *process;

    rt_unsigned_to_string((unsigned long long)pid, pid_text, sizeof(pid_text));
    fd_dir[0] = '\0';
    if (append_text(fd_dir, sizeof(fd_dir), &used, "/proc/") != 0 ||
        append_text(fd_dir, sizeof(fd_dir), &used, pid_text) != 0 ||
        append_text(fd_dir, sizeof(fd_dir), &used, "/fd") != 0) {
        return -1;
    }

    if (platform_collect_entries(fd_dir, 0, lsof_fd_entries, sizeof(lsof_fd_entries) / sizeof(lsof_fd_entries[0]), &count, &is_dir) != 0 || !is_dir) {
        return 0;
    }
    process = find_process(processes, process_count, pid);
    for (i = 0U; i < count; ++i) {
        char fd_path[LSOF_PATH_CAPACITY];
        char target[LSOF_PATH_CAPACITY];

        if (!is_decimal_text(lsof_fd_entries[i].name)) {
            continue;
        }
        if (append_path(fd_path, sizeof(fd_path), fd_dir, lsof_fd_entries[i].name) != 0) {
            continue;
        }
        if (platform_read_symlink(fd_path, target, sizeof(target)) != 0) {
            continue;
        }
        if (!path_matches_filters(target, filters, filter_count)) {
            continue;
        }
        print_row(process, lsof_fd_entries[i].name, target);
        *rows_out += 1U;
    }
    return 0;
}

static void print_help(void) {
    rt_write_line(1, "lsof - list open files");
    rt_write_line(1, "Usage: lsof [-p PID] [PATH ...]");
    rt_write_line(1, "On platforms with /proc/<pid>/fd, lsof prints COMMAND PID USER FD NAME rows. PATH arguments filter NAME by substring.");
}

int main(int argc, char **argv) {
    size_t process_count = 0U;
    size_t rows = 0U;
    int selected_pid = -1;
    ToolOptState opt;
    int r;
    size_t i;

    tool_opt_init(&opt, argc, argv, "lsof", "[-p PID] [PATH ...]");
    while ((r = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-p") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            if (tool_parse_pid(opt.value, &selected_pid) != 0) {
                tool_write_usage("lsof", "[-p PID] [PATH ...]");
                return 1;
            }
        } else {
            tool_write_error("lsof", "unknown option: ", opt.flag);
            tool_write_usage("lsof", "[-p PID] [PATH ...]");
            return 1;
        }
    }
    if (r == TOOL_OPT_HELP) {
        print_help();
        return 0;
    }
    if (r == TOOL_OPT_ERROR) return 1;

    if (platform_list_processes(lsof_processes, sizeof(lsof_processes) / sizeof(lsof_processes[0]), &process_count) != 0) {
        tool_write_error("lsof", "process table unavailable", 0);
        return 1;
    }

    rt_write_line(1, "COMMAND PID USER FD NAME");
    if (selected_pid >= 0) {
        (void)list_pid_fds(selected_pid, lsof_processes, process_count, argv + opt.argi, argc - opt.argi, &rows);
    } else {
        for (i = 0U; i < process_count; ++i) {
            (void)list_pid_fds(lsof_processes[i].pid, lsof_processes, process_count, argv + opt.argi, argc - opt.argi, &rows);
        }
    }

    return rows > 0U ? 0 : 1;
}