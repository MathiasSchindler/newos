#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static void print_usage(void) {
    tool_write_usage("time", "[-v] COMMAND [ARG ...]");
}

static void write_padded_six(unsigned long long value) {
    unsigned long long divisor = 100000ULL;

    while (divisor > 0ULL) {
        rt_write_char(2, (char)('0' + ((value / divisor) % 10ULL)));
        divisor /= 10ULL;
    }
}

static void write_elapsed_line(const char *label, unsigned long long nanoseconds) {
    unsigned long long seconds = nanoseconds / 1000000000ULL;
    unsigned long long microseconds = ((nanoseconds % 1000000000ULL) + 500ULL) / 1000ULL;

    if (microseconds == 1000000ULL) {
        seconds += 1ULL;
        microseconds = 0ULL;
    }

    rt_write_cstr(2, label);
    rt_write_char(2, ' ');
    rt_write_uint(2, seconds);
    rt_write_char(2, '.');
    write_padded_six(microseconds);
    rt_write_char(2, '\n');
}

static void write_usage_uint_line(const char *label, unsigned long long value) {
    rt_write_cstr(2, label);
    rt_write_char(2, ' ');
    rt_write_uint(2, value);
    rt_write_char(2, '\n');
}

static void write_cpu_line(unsigned long long cpu_ns, unsigned long long elapsed_ns) {
    rt_write_cstr(2, "cpu ");
    tool_write_percent_2(2, cpu_ns, elapsed_ns);
    rt_write_cstr(2, "%\n");
}

int main(int argc, char **argv) {
    int argi = 1;
    int verbose = 0;
    int pid;
    int status = 0;
    PlatformProcessUsage usage;
    unsigned long long start_time;
    unsigned long long end_time;
    unsigned long long elapsed = 0ULL;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--help") == 0 || rt_strcmp(argv[argi], "-h") == 0) {
            print_usage();
            return 0;
        }
        if (rt_strcmp(argv[argi], "-v") == 0 || rt_strcmp(argv[argi], "--verbose") == 0) {
            verbose = 1;
            argi += 1;
            continue;
        }
        break;
    }
    if (argi < argc && rt_strcmp(argv[argi], "--") == 0) {
        argi += 1;
    }
    if (argi >= argc) {
        print_usage();
        return 1;
    }

    rt_memset(&usage, 0, sizeof(usage));

    start_time = platform_get_monotonic_time_ns();
    if (platform_spawn_process(&argv[argi], -1, -1, 0, 0, 0, &pid) != 0) {
        tool_write_error("time", "failed to execute ", argv[argi]);
        return 127;
    }
    if (platform_wait_process_usage(pid, &status, &usage) != 0) {
        tool_write_error("time", "wait failed", 0);
        return 125;
    }
    end_time = platform_get_monotonic_time_ns();

    if (end_time >= start_time) {
        elapsed = end_time - start_time;
    }
    write_elapsed_line("real", elapsed);
    write_elapsed_line("user", usage.user_time_ns);
    write_elapsed_line("sys", usage.system_time_ns);
    if (verbose) {
        write_cpu_line(usage.user_time_ns + usage.system_time_ns, elapsed);
        write_usage_uint_line("minor_faults", usage.minor_faults);
        write_usage_uint_line("major_faults", usage.major_faults);
        write_usage_uint_line("voluntary_context_switches", usage.voluntary_context_switches);
        write_usage_uint_line("involuntary_context_switches", usage.involuntary_context_switches);
        write_usage_uint_line("migrations", usage.migrations);
    }

    return status;
}
