#include "platform.h"
#include "runtime.h"

#define MAX_PROCESSES 4096

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

int main(void) {
    PlatformProcessEntry entries[MAX_PROCESSES];
    size_t count = 0;
    size_t i;

    if (platform_list_processes(entries, MAX_PROCESSES, &count) != 0) {
        rt_write_line(2, "ps: not available on this platform");
        return 1;
    }

    sort_processes(entries, count);

    rt_write_line(1, "PID NAME");
    for (i = 0; i < count; ++i) {
        rt_write_uint(1, (unsigned long long)entries[i].pid);
        rt_write_char(1, ' ');
        rt_write_line(1, entries[i].name);
    }

    return 0;
}
