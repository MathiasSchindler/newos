#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

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
    PlatformMemoryInfo memory;
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

    if (platform_get_memory_info(&memory) != 0) {
        tool_write_error("free", "memory information unavailable", 0);
        return 1;
    }

    used_bytes = memory.total_bytes > memory.available_bytes ? (memory.total_bytes - memory.available_bytes) : 0;

    format_memory_value(memory.total_bytes, mode, total_text, sizeof(total_text));
    format_memory_value(used_bytes, mode, used_text, sizeof(used_text));
    format_memory_value(memory.free_bytes, mode, free_text, sizeof(free_text));
    format_memory_value(memory.available_bytes, mode, avail_text, sizeof(avail_text));

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
