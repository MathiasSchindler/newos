#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define STRACE_MAX_FILTERS 64
#define STRACE_FILTER_TOKEN_CAPACITY 32

static int show_numbers;
static int json_output;
static PlatformSyscallEvent pending_event;
static PlatformSyscallEvent pending_exit_event;
static int have_pending;
static long filter_numbers[STRACE_MAX_FILTERS];
static size_t filter_count;

static const char *syscall_name(long number) {
#if defined(__APPLE__)
    switch (number) {
        case 1: return "exit";
        case 2: return "fork";
        case 3: return "read";
        case 4: return "write";
        case 5: return "open";
        case 6: return "close";
        case 7: return "wait4";
        case 9: return "link";
        case 10: return "unlink";
        case 12: return "chdir";
        case 14: return "mknod";
        case 15: return "chmod";
        case 16: return "chown";
        case 20: return "getpid";
        case 24: return "getuid";
        case 26: return "ptrace";
        case 29: return "recvfrom";
        case 30: return "accept";
        case 33: return "access";
        case 36: return "sync";
        case 37: return "kill";
        case 42: return "pipe";
        case 47: return "getgid";
        case 54: return "ioctl";
        case 57: return "symlink";
        case 58: return "readlink";
        case 59: return "execve";
        case 73: return "munmap";
        case 79: return "getgroups";
        case 90: return "dup2";
        case 92: return "fcntl";
        case 93: return "select";
        case 95: return "fsync";
        case 97: return "socket";
        case 98: return "connect";
        case 104: return "bind";
        case 105: return "setsockopt";
        case 106: return "listen";
        case 116: return "gettimeofday";
        case 118: return "getsockopt";
        case 128: return "rename";
        case 132: return "mkfifo";
        case 133: return "sendto";
        case 134: return "shutdown";
        case 136: return "mkdir";
        case 137: return "rmdir";
        case 138: return "utimes";
        case 157: return "statfs";
        case 159: return "unmount";
        case 197: return "mmap";
        case 199: return "lseek";
        case 201: return "ftruncate";
        case 202: return "sysctl";
        case 230: return "poll";
        case 274: return "sysctlbyname";
        case 336: return "proc_info";
        case 338: return "stat64";
        case 339: return "fstat64";
        case 340: return "lstat64";
        case 344: return "getdirentries64";
        case 345: return "statfs64";
        case 364: return "lchown";
        case 500: return "getentropy";
        default: return "syscall";
    }
#else
    switch (number) {
        case 0: return "read";
        case 1: return "write";
        case 2: return "open";
        case 3: return "close";
        case 4: return "stat";
        case 5: return "fstat";
        case 6: return "lstat";
        case 7: return "poll";
        case 8: return "lseek";
        case 9: return "mmap";
        case 10: return "mprotect";
        case 11: return "munmap";
        case 12: return "brk";
        case 13: return "rt_sigaction";
        case 14: return "rt_sigprocmask";
        case 16: return "ioctl";
        case 17: return "pread64";
        case 18: return "pwrite64";
        case 20: return "writev";
        case 21: return "access";
        case 23: return "select";
        case 24: return "sched_yield";
        case 25: return "mremap";
        case 26: return "msync";
        case 28: return "madvise";
        case 32: return "dup";
        case 33: return "dup2";
        case 35: return "nanosleep";
        case 38: return "setitimer";
        case 39: return "getpid";
        case 41: return "socket";
        case 42: return "connect";
        case 43: return "accept";
        case 44: return "sendto";
        case 45: return "recvfrom";
        case 46: return "sendmsg";
        case 47: return "recvmsg";
        case 48: return "shutdown";
        case 49: return "bind";
        case 50: return "listen";
        case 51: return "getsockname";
        case 52: return "getpeername";
        case 53: return "socketpair";
        case 54: return "setsockopt";
        case 55: return "getsockopt";
        case 56: return "clone";
        case 57: return "fork";
        case 58: return "vfork";
        case 59: return "execve";
        case 60: return "exit";
        case 61: return "wait4";
        case 62: return "kill";
        case 63: return "uname";
        case 72: return "fcntl";
        case 73: return "flock";
        case 74: return "fsync";
        case 75: return "fdatasync";
        case 76: return "truncate";
        case 77: return "ftruncate";
        case 78: return "getdents";
        case 79: return "getcwd";
        case 80: return "chdir";
        case 81: return "fchdir";
        case 82: return "rename";
        case 83: return "mkdir";
        case 84: return "rmdir";
        case 85: return "creat";
        case 86: return "link";
        case 87: return "unlink";
        case 88: return "symlink";
        case 89: return "readlink";
        case 90: return "chmod";
        case 91: return "fchmod";
        case 97: return "getrlimit";
        case 101: return "ptrace";
        case 102: return "getuid";
        case 103: return "syslog";
        case 104: return "getgid";
        case 105: return "setuid";
        case 106: return "setgid";
        case 107: return "geteuid";
        case 108: return "getegid";
        case 110: return "getppid";
        case 111: return "getpgrp";
        case 112: return "setsid";
        case 113: return "setreuid";
        case 114: return "setregid";
        case 116: return "setgroups";
        case 118: return "setresuid";
        case 119: return "getresuid";
        case 131: return "sigaltstack";
        case 137: return "statfs";
        case 138: return "fstatfs";
        case 157: return "prctl";
        case 158: return "arch_prctl";
        case 162: return "sync";
        case 165: return "mount";
        case 166: return "umount2";
        case 169: return "reboot";
        case 170: return "sethostname";
        case 186: return "gettid";
        case 202: return "futex";
        case 204: return "sched_getaffinity";
        case 217: return "getdents64";
        case 228: return "clock_gettime";
        case 231: return "exit_group";
        case 232: return "epoll_wait";
        case 233: return "epoll_ctl";
        case 257: return "openat";
        case 258: return "mkdirat";
        case 259: return "mknodat";
        case 260: return "fchownat";
        case 261: return "futimesat";
        case 262: return "newfstatat";
        case 263: return "unlinkat";
        case 264: return "renameat";
        case 265: return "linkat";
        case 266: return "symlinkat";
        case 267: return "readlinkat";
        case 268: return "fchmodat";
        case 270: return "pselect6";
        case 271: return "ppoll";
        case 273: return "set_robust_list";
        case 280: return "utimensat";
        case 288: return "accept4";
        case 291: return "epoll_create1";
        case 292: return "dup3";
        case 293: return "pipe2";
        case 302: return "prlimit64";
        case 318: return "getrandom";
        case 332: return "statx";
        case 334: return "rseq";
        case 436: return "close_range";
        default: return "syscall";
    }
#endif
}

static long syscall_number_by_name(const char *name) {
    unsigned long long numeric;
    long number;

    if (rt_parse_uint(name, &numeric) == 0) {
        return (long)numeric;
    }
    for (number = 0; number <= 512; ++number) {
        if (rt_strcmp(syscall_name(number), name) == 0) {
            return number;
        }
    }
    return -1;
}

static int filter_includes(long number) {
    size_t i;

    if (filter_count == 0U) {
        return 1;
    }
    for (i = 0; i < filter_count; ++i) {
        if (filter_numbers[i] == number) {
            return 1;
        }
    }
    return 0;
}

static int add_filter_number(long number) {
    if (number < 0 || filter_count >= STRACE_MAX_FILTERS) {
        return -1;
    }
    filter_numbers[filter_count++] = number;
    return 0;
}

static int parse_filter_spec(const char *spec) {
    size_t index = 0U;

    if (rt_strncmp(spec, "trace=", 6U) == 0) {
        spec += 6;
    }
    while (spec[index] != '\0') {
        char token[STRACE_FILTER_TOKEN_CAPACITY];
        size_t length = 0U;
        long number;

        while (spec[index] == ',' || spec[index] == ' ') {
            index += 1U;
        }
        while (spec[index] != '\0' && spec[index] != ',' && spec[index] != ' ') {
            if (length + 1U >= sizeof(token)) {
                return -1;
            }
            token[length++] = spec[index++];
        }
        if (length == 0U) {
            continue;
        }
        token[length] = '\0';
        number = syscall_number_by_name(token);
        if (add_filter_number(number) != 0) {
            return -1;
        }
    }
    return 0;
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

static int write_escaped_text(int fd, const char *text, size_t length, int truncated) {
    size_t index;

    if (rt_write_char(fd, '"') != 0) return -1;
    for (index = 0U; index < length; ++index) {
        unsigned char ch = (unsigned char)text[index];

        if (ch == '"' || ch == '\\') {
            if (rt_write_char(fd, '\\') != 0 || rt_write_char(fd, (char)ch) != 0) return -1;
        } else if (ch == '\n') {
            if (rt_write_cstr(fd, "\\n") != 0) return -1;
        } else if (ch == '\r') {
            if (rt_write_cstr(fd, "\\r") != 0) return -1;
        } else if (ch == '\t') {
            if (rt_write_cstr(fd, "\\t") != 0) return -1;
        } else if (ch >= 0x20U && ch < 0x7fU) {
            if (rt_write_char(fd, (char)ch) != 0) return -1;
        } else {
            static const char hex[] = "0123456789abcdef";
            if (rt_write_cstr(fd, "\\x") != 0 ||
                rt_write_char(fd, hex[(ch >> 4U) & 0x0fU]) != 0 ||
                rt_write_char(fd, hex[ch & 0x0fU]) != 0) return -1;
        }
    }
    if (rt_write_char(fd, '"') != 0) return -1;
    if (truncated) {
        if (rt_write_cstr(fd, "...") != 0) return -1;
    }
    return 0;
}

static int write_decoded_arg(int fd, const PlatformSyscallEvent *event, int arg_index) {
    const PlatformSyscallEvent *decoded_source = event;

    if (pending_exit_event.decoded_kind != 0U && pending_exit_event.decoded_arg == (unsigned int)arg_index) {
        decoded_source = &pending_exit_event;
    }
    if (decoded_source->decoded_kind != 0U && decoded_source->decoded_arg == (unsigned int)arg_index) {
        size_t length = decoded_source->decoded_length;
        if (length > sizeof(decoded_source->decoded)) length = sizeof(decoded_source->decoded);
        return write_escaped_text(fd, decoded_source->decoded, length, decoded_source->decoded_truncated != 0U);
    }
    write_hex_long(fd, (unsigned long)event->args[arg_index]);
    return 0;
}

static int json_write_decoded(const PlatformSyscallEvent *event) {
    const PlatformSyscallEvent *decoded_source = event;
    size_t length;

    if (pending_exit_event.decoded_kind != 0U) decoded_source = &pending_exit_event;
    if (decoded_source->decoded_kind == 0U) return 0;
    length = decoded_source->decoded_length;
    if (length > sizeof(decoded_source->decoded)) length = sizeof(decoded_source->decoded);
    if (rt_write_cstr(2, ",\"decoded\":{") != 0) return -1;
    if (rt_write_cstr(2, "\"arg\":") != 0) return -1;
    if (rt_write_uint(2, decoded_source->decoded_arg) != 0) return -1;
    if (rt_write_cstr(2, ",\"kind\":") != 0) return -1;
    if (tool_json_write_string(2, decoded_source->decoded_kind == 1U ? "string" : "bytes") != 0) return -1;
    if (rt_write_cstr(2, ",\"value\":") != 0) return -1;
    if (tool_json_write_string_n(2, decoded_source->decoded, length) != 0) return -1;
    if (rt_write_cstr(2, ",\"truncated\":") != 0) return -1;
    if (rt_write_cstr(2, decoded_source->decoded_truncated ? "true" : "false") != 0) return -1;
    return rt_write_char(2, '}');
}

static int trace_callback(const PlatformSyscallEvent *event, void *user_data) {
    PlatformSyscallEvent completed;

    (void)user_data;
    if (event->entering) {
        pending_event = *event;
        have_pending = 1;
        return 0;
    }
    pending_exit_event = *event;
    completed = have_pending ? pending_event : *event;
    completed.result = event->result;
    have_pending = 0;
    if (!filter_includes(completed.number)) {
        return 0;
    }
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
        if (json_write_decoded(&completed) != 0) return -1;
        if (rt_write_char(2, '}') != 0) return -1;
        return tool_json_end_event(2);
    }
    rt_write_cstr(2, syscall_name(completed.number));
    if (show_numbers) {
        rt_write_cstr(2, "#");
        rt_write_int(2, completed.number);
    }
    rt_write_char(2, '(');
    if (write_decoded_arg(2, &completed, 0) != 0 || rt_write_cstr(2, ", ") != 0) return -1;
    if (write_decoded_arg(2, &completed, 1) != 0 || rt_write_cstr(2, ", ") != 0) return -1;
    if (write_decoded_arg(2, &completed, 2) != 0) return -1;
    rt_write_cstr(2, ") = ");
    rt_write_int(2, completed.result);
    rt_write_char(2, '\n');
    return 0;
}

static void print_usage(void) {
    tool_write_usage("strace", "[-n] [-e SYSCALL[,SYSCALL...]] [--json] COMMAND [ARG ...]");
}

int main(int argc, char **argv) {
    int argi = 1;
    int exit_status = 1;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-n") == 0 || rt_strcmp(argv[argi], "--numbers") == 0) {
            show_numbers = 1;
        } else if (rt_strcmp(argv[argi], "-e") == 0 || rt_strcmp(argv[argi], "--trace") == 0) {
            if (argi + 1 >= argc || parse_filter_spec(argv[argi + 1]) != 0) {
                tool_write_error("strace", "invalid syscall filter", argi + 1 < argc ? argv[argi + 1] : 0);
                return 1;
            }
            argi += 1;
        } else if (rt_strncmp(argv[argi], "-e", 2U) == 0 && argv[argi][2] != '\0') {
            if (parse_filter_spec(argv[argi] + 2) != 0) {
                tool_write_error("strace", "invalid syscall filter", argv[argi] + 2);
                return 1;
            }
        } else if (rt_strncmp(argv[argi], "--trace=", 8U) == 0) {
            if (parse_filter_spec(argv[argi] + 8) != 0) {
                tool_write_error("strace", "invalid syscall filter", argv[argi] + 8);
                return 1;
            }
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
