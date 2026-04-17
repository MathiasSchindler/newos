#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "runtime.h"
#include "tool_util.h"

#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <stdint.h>
#include <sys/sysctl.h>
#endif

static int get_memory_bytes(unsigned long long *total_out, unsigned long long *free_out, unsigned long long *available_out) {
#if defined(__APPLE__)
    uint64_t total_memory = 0;
    size_t total_size = sizeof(total_memory);
    mach_port_t host = mach_host_self();
    vm_statistics64_data_t vm_stats;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page_size = 0;
    unsigned long long free_memory;

    if (sysctlbyname("hw.memsize", &total_memory, &total_size, 0, 0) != 0) {
        return -1;
    }

    if (host_page_size(host, &page_size) != KERN_SUCCESS) {
        return -1;
    }

    if (host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm_stats, &count) != KERN_SUCCESS) {
        return -1;
    }

    free_memory = (unsigned long long)(vm_stats.free_count + vm_stats.inactive_count + vm_stats.speculative_count) *
                  (unsigned long long)page_size;

    *total_out = (unsigned long long)total_memory;
    *free_out = free_memory;
    *available_out = free_memory;
    return 0;
#elif defined(_SC_PHYS_PAGES)
    long page_size = sysconf(_SC_PAGESIZE);
    long phys_pages = sysconf(_SC_PHYS_PAGES);
    long avail_pages = -1;

#if defined(_SC_AVPHYS_PAGES)
    avail_pages = sysconf(_SC_AVPHYS_PAGES);
#endif

    if (page_size <= 0 || phys_pages <= 0) {
        return -1;
    }

    if (avail_pages < 0) {
        avail_pages = phys_pages / 2;
    }

    *total_out = (unsigned long long)page_size * (unsigned long long)phys_pages;
    *free_out = (unsigned long long)page_size * (unsigned long long)avail_pages;
    *available_out = *free_out;
    return 0;
#else
    (void)total_out;
    (void)free_out;
    (void)available_out;
    return -1;
#endif
}

int main(int argc, char **argv) {
    unsigned long long total_bytes = 0;
    unsigned long long free_bytes = 0;
    unsigned long long available_bytes = 0;
    unsigned long long used_bytes;

    if (argc != 1) {
        tool_write_usage(argv[0], "");
        return 1;
    }

    if (get_memory_bytes(&total_bytes, &free_bytes, &available_bytes) != 0) {
        tool_write_error("free", "memory information unavailable", 0);
        return 1;
    }

    used_bytes = total_bytes > available_bytes ? (total_bytes - available_bytes) : 0;

    rt_write_line(1, "            total        used        free   available");
    rt_write_cstr(1, "Mem: ");
    rt_write_uint(1, total_bytes / 1024ULL);
    rt_write_char(1, ' ');
    rt_write_uint(1, used_bytes / 1024ULL);
    rt_write_char(1, ' ');
    rt_write_uint(1, free_bytes / 1024ULL);
    rt_write_char(1, ' ');
    rt_write_uint(1, available_bytes / 1024ULL);
    rt_write_char(1, '\n');

    return 0;
}
