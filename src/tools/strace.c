#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static int show_numbers;
static int json_output;
static PlatformSyscallEvent pending_event;
static int have_pending;

static const char *syscall_name(long number) {
    switch (number) {
        case 0: return "read";
        case 1: return "write";
        case 2: return "open";
        case 3: return "close";
        case 8: return "lseek";
        case 9: return "mmap";
        case 12: return "brk";
        case 16: return "ioctl";
        case 21: return "access";
        case 35: return "nanosleep";
        case 39: return "getpid";
        case 41: return "socket";
        case 42: return "connect";
        case 56: return "clone";
        case 57: return "fork";
        case 59: return "execve";
        case 60: return "exit";
        case 61: return "wait4";
        case 62: return "kill";
        case 72: return "fcntl";
        case 79: return "getcwd";
        case 80: return "chdir";
        case 89: return "readlink";
        case 101: return "ptrace";
        case 102: return "getuid";
        case 104: return "getgid";
        case 158: return "arch_prctl";
        case 202: return "futex";
        case 217: return "getdents64";
        case 228: return "clock_gettime";
        case 257: return "openat";
        case 262: return "newfstatat";
        case 263: return "unlinkat";
        case 267: return "readlinkat";
        case 268: return "fchmodat";
        case 280: return "utimensat";
        case 292: return "dup3";
        case 318: return "getrandom";
        default: return "syscall";
    }
}

static void write_hex_long(int fd, unsigned long value) {
    static const char digits[] = "0123456789abcdef";
    char temp[32];
    size_t count = 0U;
    rt_write_cstr(fd, "0x");
    if (value == 0UL) {
        rt_write_char(fd, '0');
        return;
    }
    while (value != 0UL && count < sizeof(temp)) {
        temp[count++] = digits[value & 0xfUL];
        value >>= 4U;
    }
    while (count > 0U) rt_write_char(fd, temp[--count]);
}

static int trace_callback(const PlatformSyscallEvent *event, void *user_data) {
    PlatformSyscallEvent completed;

    (void)user_data;
    if (event->entering) {
        pending_event = *event;
        have_pending = 1;
        return 0;
    }
    completed = have_pending ? pending_event : *event;
    completed.result = event->result;
    have_pending = 0;
    if (json_output) {
        int i;
        if (tool_json_begin_event(2, "strace", "stderr", "syscall") != 0) return -1;
        if (rt_write_cstr(2, ",\"data\":{\"number\":") != 0) return -1;
        if (rt_write_uint(2, (unsigned long long)completed.number) != 0) return -1;
        if (rt_write_cstr(2, ",\"name\":") != 0) return -1;
        if (tool_json_write_string(2, syscall_name(completed.number)) != 0) return -1;
        if (rt_write_cstr(2, ",\"args\":[") != 0) return -1;
        for (i = 0; i < 6; ++i) {
            if (i != 0 && rt_write_char(2, ',') != 0) return -1;
            if (rt_write_uint(2, (unsigned long long)completed.args[i]) != 0) return -1;
        }
        if (rt_write_cstr(2, "],\"result\":") != 0) return -1;
        if (rt_write_int(2, completed.result) != 0) return -1;
        if (rt_write_char(2, '}') != 0) return -1;
        return tool_json_end_event(2);
    }
    rt_write_cstr(2, syscall_name(completed.number));
    if (show_numbers) {
        rt_write_cstr(2, "#");
        rt_write_int(2, completed.number);
    }
    rt_write_char(2, '(');
    write_hex_long(2, (unsigned long)completed.args[0]); rt_write_cstr(2, ", ");
    write_hex_long(2, (unsigned long)completed.args[1]); rt_write_cstr(2, ", ");
    write_hex_long(2, (unsigned long)completed.args[2]);
    rt_write_cstr(2, ") = ");
    rt_write_int(2, completed.result);
    rt_write_char(2, '\n');
    return 0;
}

static void print_usage(void) {
    tool_write_usage("strace", "[-n] [--json] COMMAND [ARG ...]");
}

int main(int argc, char **argv) {
    int argi = 1;
    int exit_status = 1;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-n") == 0 || rt_strcmp(argv[argi], "--numbers") == 0) {
            show_numbers = 1;
        } else if (rt_strcmp(argv[argi], "--json") == 0) {
            json_output = 1;
            tool_json_set_enabled(1);
        } else if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) {
            print_usage();
            return 0;
        } else {
            break;
        }
        argi++;
    }
    if (argi >= argc) {
        print_usage();
        return 1;
    }
    if (platform_trace_syscalls(&argv[argi], trace_callback, 0, &exit_status) != 0) {
        tool_write_error("strace", "tracing is not supported or failed", 0);
        return 1;
    }
    return exit_status;
}
