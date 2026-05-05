#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void print_usage(void) {
    tool_write_usage("time", "COMMAND [ARG ...]");
}

static void write_seconds_line(const char *label, unsigned long long seconds) {
    rt_write_cstr(2, label);
    rt_write_char(2, ' ');
    rt_write_uint(2, seconds);
    rt_write_cstr(2, ".00\n");
}

int main(int argc, char **argv) {
    int argi = 1;
    int pid;
    int status = 0;
    long long start_time;
    long long end_time;
    unsigned long long elapsed = 0ULL;

    if (argi < argc && rt_strcmp(argv[argi], "--help") == 0) {
        print_usage();
        return 0;
    }
    if (argi < argc && rt_strcmp(argv[argi], "--") == 0) {
        argi += 1;
    }
    if (argi >= argc) {
        print_usage();
        return 1;
    }

    start_time = platform_get_epoch_time();
    if (platform_spawn_process(&argv[argi], -1, -1, 0, 0, 0, &pid) != 0) {
        tool_write_error("time", "failed to execute ", argv[argi]);
        return 127;
    }
    if (platform_wait_process(pid, &status) != 0) {
        tool_write_error("time", "wait failed", 0);
        return 125;
    }
    end_time = platform_get_epoch_time();

    if (end_time >= start_time) {
        elapsed = (unsigned long long)(end_time - start_time);
    }
    write_seconds_line("real", elapsed);
    write_seconds_line("user", 0ULL);
    write_seconds_line("sys", 0ULL);

    return status;
}
