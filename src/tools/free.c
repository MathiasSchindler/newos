#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#if __STDC_HOSTED__
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <stdint.h>
#include <sys/sysctl.h>
#endif
#endif

static int read_text_file(const char *path, char *buffer, size_t buffer_size) {
    int fd;
    size_t used = 0;

    if (buffer_size == 0) {
        return -1;
    }

    fd = platform_open_read(path);
    if (fd < 0) {
        buffer[0] = '\0';
        return -1;
    }

    while (used + 1U < buffer_size) {
        long bytes_read = platform_read(fd, buffer + used, buffer_size - used - 1U);
        if (bytes_read < 0) {
            platform_close(fd);
            buffer[0] = '\0';
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }
        used += (size_t)bytes_read;
    }

    buffer[used] = '\0';
    platform_close(fd);
    return 0;
}

static int parse_unsigned_prefix(const char *text, unsigned long long *value_out) {
    unsigned long long value = 0;
    size_t index = 0;

    if (text == 0 || value_out == 0 || text[0] < '0' || text[0] > '9') {
        return -1;
    }

    while (text[index] >= '0' && text[index] <= '9') {
        value = (value * 10ULL) + (unsigned long long)(text[index] - '0');
        index += 1U;
    }

    *value_out = value;
    return 0;
}

static int match_field_name(const char *text, const char *field_name) {
    size_t index = 0;

    while (field_name[index] != '\0') {
        if (text[index] != field_name[index]) {
            return 0;
        }
        index += 1U;
    }

    return text[index] == ':';
}

static int parse_meminfo_field(const char *contents, const char *field_name, unsigned long long *value_out) {
    const char *cursor = contents;

    while (cursor != 0 && *cursor != '\0') {
        if (match_field_name(cursor, field_name)) {
            unsigned long long kibibytes = 0;

            cursor += rt_strlen(field_name) + 1U;
            while (*cursor == ' ' || *cursor == '\t') {
                cursor += 1;
            }

            if (parse_unsigned_prefix(cursor, &kibibytes) != 0) {
                return -1;
            }

            *value_out = kibibytes * 1024ULL;
            return 0;
        }

        while (*cursor != '\0' && *cursor != '\n') {
            cursor += 1;
        }
        if (*cursor == '\n') {
            cursor += 1;
        }
    }

    return -1;
}

static int get_memory_bytes(unsigned long long *total_out, unsigned long long *free_out, unsigned long long *available_out) {
    char meminfo[4096];

    if (read_text_file("/proc/meminfo", meminfo, sizeof(meminfo)) == 0 &&
        parse_meminfo_field(meminfo, "MemTotal", total_out) == 0 &&
        parse_meminfo_field(meminfo, "MemFree", free_out) == 0) {
        if (parse_meminfo_field(meminfo, "MemAvailable", available_out) != 0) {
            *available_out = *free_out;
        }
        return 0;
    }

#if __STDC_HOSTED__
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
#else
    (void)total_out;
    (void)free_out;
    (void)available_out;
    return -1;
#endif
}

typedef enum {
    FREE_UNITS_KIB,
    FREE_UNITS_BYTES,
    FREE_UNITS_MIB,
    FREE_UNITS_GIB,
    FREE_UNITS_HUMAN
} FreeUnitMode;

static void format_memory_value(unsigned long long bytes, FreeUnitMode mode, char *buffer, size_t buffer_size) {
    if (mode == FREE_UNITS_HUMAN) {
        tool_format_size(bytes, 1, buffer, buffer_size);
        return;
    }

    if (mode == FREE_UNITS_BYTES) {
        rt_unsigned_to_string(bytes, buffer, buffer_size);
    } else if (mode == FREE_UNITS_MIB) {
        rt_unsigned_to_string(bytes / (1024ULL * 1024ULL), buffer, buffer_size);
    } else if (mode == FREE_UNITS_GIB) {
        rt_unsigned_to_string(bytes / (1024ULL * 1024ULL * 1024ULL), buffer, buffer_size);
    } else {
        rt_unsigned_to_string(bytes / 1024ULL, buffer, buffer_size);
    }
}

int main(int argc, char **argv) {
    unsigned long long total_bytes = 0;
    unsigned long long free_bytes = 0;
    unsigned long long available_bytes = 0;
    unsigned long long used_bytes;
    FreeUnitMode mode = FREE_UNITS_KIB;
    const char *header = "KiB";
    char total_text[32];
    char used_text[32];
    char free_text[32];
    char avail_text[32];
    int argi;

    for (argi = 1; argi < argc; ++argi) {
        if (rt_strcmp(argv[argi], "-h") == 0) {
            mode = FREE_UNITS_HUMAN;
            header = "human";
        } else if (rt_strcmp(argv[argi], "-b") == 0) {
            mode = FREE_UNITS_BYTES;
            header = "bytes";
        } else if (rt_strcmp(argv[argi], "-k") == 0) {
            mode = FREE_UNITS_KIB;
            header = "KiB";
        } else if (rt_strcmp(argv[argi], "-m") == 0) {
            mode = FREE_UNITS_MIB;
            header = "MiB";
        } else if (rt_strcmp(argv[argi], "-g") == 0) {
            mode = FREE_UNITS_GIB;
            header = "GiB";
        } else {
            tool_write_usage(argv[0], "[-b|-k|-m|-g|-h]");
            return 1;
        }
    }

    if (get_memory_bytes(&total_bytes, &free_bytes, &available_bytes) != 0) {
        tool_write_error("free", "memory information unavailable", 0);
        return 1;
    }

    used_bytes = total_bytes > available_bytes ? (total_bytes - available_bytes) : 0;

    format_memory_value(total_bytes, mode, total_text, sizeof(total_text));
    format_memory_value(used_bytes, mode, used_text, sizeof(used_text));
    format_memory_value(free_bytes, mode, free_text, sizeof(free_text));
    format_memory_value(available_bytes, mode, avail_text, sizeof(avail_text));

    rt_write_cstr(1, "               total        used        free   available (");
    rt_write_cstr(1, header);
    rt_write_line(1, ")");
    rt_write_cstr(1, "Mem:");
    if (rt_strlen(total_text) < 13U) {
        size_t pad = 13U - rt_strlen(total_text);
        while (pad-- > 0U) {
            rt_write_char(1, ' ');
        }
    } else {
        rt_write_char(1, ' ');
    }
    rt_write_cstr(1, total_text);
    if (rt_strlen(used_text) < 12U) {
        size_t pad = 12U - rt_strlen(used_text);
        while (pad-- > 0U) {
            rt_write_char(1, ' ');
        }
    } else {
        rt_write_char(1, ' ');
    }
    rt_write_cstr(1, used_text);
    if (rt_strlen(free_text) < 12U) {
        size_t pad = 12U - rt_strlen(free_text);
        while (pad-- > 0U) {
            rt_write_char(1, ' ');
        }
    } else {
        rt_write_char(1, ' ');
    }
    rt_write_cstr(1, free_text);
    if (rt_strlen(avail_text) < 12U) {
        size_t pad = 12U - rt_strlen(avail_text);
        while (pad-- > 0U) {
            rt_write_char(1, ' ');
        }
    } else {
        rt_write_char(1, ' ');
    }
    rt_write_cstr(1, avail_text);
    rt_write_char(1, '\n');

    return 0;
}
