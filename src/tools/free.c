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

static void write_padding(size_t count) {
    while (count > 0U) {
        rt_write_char(1, ' ');
        count -= 1U;
    }
}

static void write_table_cell(const char *text, size_t width) {
    size_t length = rt_strlen(text);
    if (length < width) {
        write_padding(width - length);
    } else {
        rt_write_char(1, ' ');
    }
    rt_write_cstr(1, text);
}

static void write_table_row(const char *label, const char *const *values, size_t count) {
    size_t i;
    size_t label_length = rt_strlen(label);

    rt_write_cstr(1, label);
    if (label_length < 6U) {
        write_padding(6U - label_length);
    }
    for (i = 0; i < count; ++i) {
        write_table_cell(values[i], 12U);
    }
    rt_write_char(1, '\n');
}

int main(int argc, char **argv) {
    PlatformMemoryInfo memory;
    unsigned long long used_bytes;
    unsigned long long buffer_cache_bytes;
    unsigned long long swap_used_bytes;
    FreeUnitMode mode = FREE_UNITS_KIB;
    const char *header = "KiB";
    char total_text[32];
    char used_text[32];
    char free_text[32];
    char avail_text[32];
    char shared_text[32];
    char cache_text[32];
    char swap_total_text[32];
    char swap_used_text[32];
    char swap_free_text[32];
    char total_total_text[32];
    char total_used_total_text[32];
    char total_free_total_text[32];
    char total_avail_text[32];
    const char *header_values[6];
    const char *mem_values[6];
    const char *swap_values[6];
    const char *total_values[6];
    size_t column_count = 0;
    int wide_output = 0;
    int totals_output = 0;
    int argi;

    for (argi = 1; argi < argc; ++argi) {
        const char *arg = argv[argi];
        size_t j;

        if (arg[0] != '-' || arg[1] == '\0') {
            tool_write_usage(argv[0], "[-b|-k|-m|-g|-h] [-w] [-t]");
            return 1;
        }

        for (j = 1; arg[j] != '\0'; ++j) {
            if (arg[j] == 'h') {
                mode = FREE_UNITS_HUMAN;
                header = "human";
            } else if (arg[j] == 'b') {
                mode = FREE_UNITS_BYTES;
                header = "bytes";
            } else if (arg[j] == 'k') {
                mode = FREE_UNITS_KIB;
                header = "KiB";
            } else if (arg[j] == 'm') {
                mode = FREE_UNITS_MIB;
                header = "MiB";
            } else if (arg[j] == 'g') {
                mode = FREE_UNITS_GIB;
                header = "GiB";
            } else if (arg[j] == 'w') {
                wide_output = 1;
            } else if (arg[j] == 't') {
                totals_output = 1;
            } else {
                tool_write_usage(argv[0], "[-b|-k|-m|-g|-h] [-w] [-t]");
                return 1;
            }
        }
    }

    if (platform_get_memory_info(&memory) != 0) {
        tool_write_error("free", "memory information unavailable", 0);
        return 1;
    }

    buffer_cache_bytes = memory.buffer_bytes + memory.cache_bytes;
    if (memory.total_bytes > memory.free_bytes + buffer_cache_bytes) {
        used_bytes = memory.total_bytes - memory.free_bytes - buffer_cache_bytes;
    } else if (memory.total_bytes > memory.available_bytes) {
        used_bytes = memory.total_bytes - memory.available_bytes;
    } else {
        used_bytes = 0;
    }
    swap_used_bytes = memory.swap_total_bytes > memory.swap_free_bytes ? (memory.swap_total_bytes - memory.swap_free_bytes) : 0;

    format_memory_value(memory.total_bytes, mode, total_text, sizeof(total_text));
    format_memory_value(used_bytes, mode, used_text, sizeof(used_text));
    format_memory_value(memory.free_bytes, mode, free_text, sizeof(free_text));
    format_memory_value(memory.available_bytes, mode, avail_text, sizeof(avail_text));
    format_memory_value(memory.shared_bytes, mode, shared_text, sizeof(shared_text));
    format_memory_value(buffer_cache_bytes, mode, cache_text, sizeof(cache_text));
    format_memory_value(memory.swap_total_bytes, mode, swap_total_text, sizeof(swap_total_text));
    format_memory_value(swap_used_bytes, mode, swap_used_text, sizeof(swap_used_text));
    format_memory_value(memory.swap_free_bytes, mode, swap_free_text, sizeof(swap_free_text));
    format_memory_value(memory.total_bytes + memory.swap_total_bytes, mode, total_total_text, sizeof(total_total_text));
    format_memory_value(used_bytes + swap_used_bytes, mode, total_used_total_text, sizeof(total_used_total_text));
    format_memory_value(memory.free_bytes + memory.swap_free_bytes, mode, total_free_total_text, sizeof(total_free_total_text));
    format_memory_value(memory.available_bytes + memory.swap_free_bytes, mode, total_avail_text, sizeof(total_avail_text));

    header_values[column_count++] = "total";
    header_values[column_count++] = "used";
    header_values[column_count++] = "free";
    if (wide_output) {
        header_values[column_count++] = "shared";
        header_values[column_count++] = "buff/cache";
    }
    header_values[column_count++] = "available";

    mem_values[0] = total_text;
    mem_values[1] = used_text;
    mem_values[2] = free_text;
    if (wide_output) {
        mem_values[3] = shared_text;
        mem_values[4] = cache_text;
        mem_values[5] = avail_text;
    } else {
        mem_values[3] = avail_text;
    }

    swap_values[0] = swap_total_text;
    swap_values[1] = swap_used_text;
    swap_values[2] = swap_free_text;
    if (wide_output) {
        swap_values[3] = "0";
        swap_values[4] = "0";
        swap_values[5] = swap_free_text;
    } else {
        swap_values[3] = swap_free_text;
    }

    total_values[0] = total_total_text;
    total_values[1] = total_used_total_text;
    total_values[2] = total_free_total_text;
    if (wide_output) {
        total_values[3] = shared_text;
        total_values[4] = cache_text;
        total_values[5] = total_avail_text;
    } else {
        total_values[3] = total_avail_text;
    }

    rt_write_cstr(1, "      ");
    for (argi = 0; argi < (int)column_count; ++argi) {
        write_table_cell(header_values[argi], 12U);
    }
    rt_write_cstr(1, " (");
    rt_write_cstr(1, header);
    rt_write_line(1, ")");

    write_table_row("Mem:", mem_values, column_count);
    write_table_row("Swap:", swap_values, column_count);
    if (totals_output) {
        write_table_row("Total:", total_values, column_count);
    }

    return 0;
}
