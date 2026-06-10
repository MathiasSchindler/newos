#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define STRINGS_BUFFER_CAPACITY 1024
#define STRINGS_MAX_SECTIONS 256U
#define STRINGS_MAX_MACHO_COMMANDS 256U

typedef enum {
    STRINGS_ENCODING_7BIT,
    STRINGS_ENCODING_8BIT,
    STRINGS_ENCODING_16LE,
    STRINGS_ENCODING_16BE,
    STRINGS_ENCODING_32LE,
    STRINGS_ENCODING_32BE
} StringsEncoding;

typedef struct {
    size_t min_length;
    int show_offset;
    unsigned int offset_base;
    int print_file_name;
    int scan_data_sections;
    StringsEncoding encoding;
} StringsOptions;

typedef struct {
    unsigned long long offset;
    unsigned long long size;
} StringsRange;

static int is_printable_byte(unsigned char ch) {
    return ch >= 32U && ch <= 126U;
}


static int read_region(int fd, unsigned long long offset, unsigned char *buffer, size_t size) {
    size_t total = 0U;

    if (platform_seek(fd, (long long)offset, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }
    while (total < size) {
        long chunk = platform_read(fd, buffer + total, size - total);
        if (chunk <= 0) {
            return -1;
        }
        total += (size_t)chunk;
    }
    return 0;
}

static void format_unsigned_value(unsigned long long value, unsigned int base, char *buffer, size_t buffer_size) {
    const char *digits = "0123456789abcdef";
    char scratch[64];
    size_t length = 0;
    size_t i;

    if (buffer_size == 0U) {
        return;
    }

    if (value == 0ULL) {
        if (buffer_size > 1U) {
            buffer[0] = '0';
            buffer[1] = '\0';
        } else {
            buffer[0] = '\0';
        }
        return;
    }

    while (value != 0ULL && length + 1U < sizeof(scratch)) {
        scratch[length++] = digits[value % base];
        value /= base;
    }

    for (i = 0; i < length && i + 1U < buffer_size; ++i) {
        buffer[i] = scratch[length - 1U - i];
    }
    buffer[i] = '\0';
}

static size_t strings_unit_size(StringsEncoding encoding) {
    if (encoding == STRINGS_ENCODING_16LE || encoding == STRINGS_ENCODING_16BE) {
        return 2U;
    }
    if (encoding == STRINGS_ENCODING_32LE || encoding == STRINGS_ENCODING_32BE) {
        return 4U;
    }
    return 1U;
}

static int decode_string_unit(const unsigned char *unit, StringsEncoding encoding, char *out) {
    if (encoding == STRINGS_ENCODING_7BIT) {
        if (unit[0] > 127U || !is_printable_byte(unit[0])) {
            return 0;
        }
        *out = (char)unit[0];
        return 1;
    }

    if (encoding == STRINGS_ENCODING_8BIT) {
        if (!is_printable_byte(unit[0])) {
            return 0;
        }
        *out = (char)unit[0];
        return 1;
    }

    if (encoding == STRINGS_ENCODING_16LE) {
        if (unit[1] != 0U || !is_printable_byte(unit[0])) {
            return 0;
        }
        *out = (char)unit[0];
        return 1;
    }

    if (encoding == STRINGS_ENCODING_16BE) {
        if (unit[0] != 0U || !is_printable_byte(unit[1])) {
            return 0;
        }
        *out = (char)unit[1];
        return 1;
    }

    if (encoding == STRINGS_ENCODING_32LE) {
        if (unit[1] != 0U || unit[2] != 0U || unit[3] != 0U || !is_printable_byte(unit[0])) {
            return 0;
        }
        *out = (char)unit[0];
        return 1;
    }

    if (unit[0] != 0U || unit[1] != 0U || unit[2] != 0U || !is_printable_byte(unit[3])) {
        return 0;
    }
    *out = (char)unit[3];
    return 1;
}

static int flush_sequence(char *buffer,
                          size_t *length,
                          unsigned long long start_offset,
                          const StringsOptions *options,
                          const char *file_name) {
    if (*length >= options->min_length) {
        char offset_buffer[64];

        buffer[*length] = '\0';
        if (options->print_file_name && file_name != 0) {
            if (rt_write_cstr(1, file_name) != 0 || rt_write_cstr(1, ": ") != 0) {
                return -1;
            }
        }
        if (options->show_offset) {
            format_unsigned_value(start_offset, options->offset_base, offset_buffer, sizeof(offset_buffer));
            if (rt_write_cstr(1, offset_buffer) != 0 || rt_write_char(1, ' ') != 0) {
                return -1;
            }
        }
        if (rt_write_line(1, buffer) != 0) {
            return -1;
        }
    }
    *length = 0U;
    return 0;
}

static int strings_range(int fd, const StringsOptions *options, const char *file_name, unsigned long long offset, unsigned long long limit, int bounded) {
    unsigned char chunk[4096];
    unsigned char buffered[4096 + 4];
    char current[STRINGS_BUFFER_CAPACITY];
    size_t current_len = 0U;
    unsigned long long absolute_offset = offset;
    unsigned long long start_offset = 0ULL;
    size_t pending = 0U;
    size_t unit_size = strings_unit_size(options->encoding);
    long bytes_read = 0;

    if (offset != 0ULL && platform_seek(fd, (long long)offset, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }

    while (!bounded || limit > 0ULL) {
        size_t request = sizeof(chunk);
        if (bounded && limit < (unsigned long long)request) {
            request = (size_t)limit;
        }
        if (request == 0U) {
            break;
        }
        bytes_read = platform_read(fd, chunk, request);
        if (bytes_read <= 0) {
            break;
        }
        if (bounded) {
            limit -= (unsigned long long)bytes_read;
        }

        size_t total = pending + (size_t)bytes_read;
        size_t i = 0U;

        memcpy(buffered + pending, chunk, (size_t)bytes_read);

        while (i + unit_size <= total) {
            char decoded = '\0';

            if (decode_string_unit(buffered + i, options->encoding, &decoded)) {
                if (current_len == 0U) {
                    start_offset = absolute_offset;
                }
                if (current_len + 1U < sizeof(current)) {
                    current[current_len++] = decoded;
                }
            } else if (flush_sequence(current, &current_len, start_offset, options, file_name) != 0) {
                return -1;
            }

            absolute_offset += (unsigned long long)unit_size;
            i += unit_size;
        }

        pending = total - i;
        if (pending > 0U) {
            memmove(buffered, buffered + i, pending);
        }
    }

    if (bytes_read < 0) {
        return -1;
    }

    return flush_sequence(current, &current_len, start_offset, options, file_name);
}

static int strings_stream(int fd, const StringsOptions *options, const char *file_name) {
    return strings_range(fd, options, file_name, 0ULL, 0ULL, 0);
}

static int add_range(StringsRange *ranges, size_t *count, unsigned long long offset, unsigned long long size) {
    if (size == 0ULL) {
        return 0;
    }
    if (*count >= STRINGS_MAX_SECTIONS) {
        return -1;
    }
    ranges[*count].offset = offset;
    ranges[*count].size = size;
    *count += 1U;
    return 0;
}

static int load_elf_ranges(int fd, StringsRange *ranges, size_t *count) {
    unsigned char header[64];
    unsigned long long shoff;
    unsigned short shentsize;
    unsigned short shnum;
    unsigned short i;

    if (read_region(fd, 0ULL, header, sizeof(header)) != 0) return -1;
    if (!(header[0] == 0x7fU && header[1] == 'E' && header[2] == 'L' && header[3] == 'F')) return -1;
    if (header[4] != 2U || header[5] != 1U) return -1;

    shoff = tool_read_u64_le(header + 40);
    shentsize = tool_read_u16_le(header + 58);
    shnum = tool_read_u16_le(header + 60);
    if (shnum > STRINGS_MAX_SECTIONS || shentsize < 64U) return -1;

    for (i = 0U; i < shnum; ++i) {
        unsigned char section[64];
        unsigned int type;
        unsigned long long flags;
        unsigned long long offset;
        unsigned long long size;

        if (read_region(fd, shoff + ((unsigned long long)i * (unsigned long long)shentsize), section, sizeof(section)) != 0) return -1;
        type = tool_read_u32_le(section + 4);
        flags = tool_read_u64_le(section + 8);
        offset = tool_read_u64_le(section + 24);
        size = tool_read_u64_le(section + 32);
        if (type != 8U && (flags & 0x2ULL) != 0ULL) {
            if (add_range(ranges, count, offset, size) != 0) return -1;
        }
    }
    return 0;
}

static int load_macho_ranges(int fd, StringsRange *ranges, size_t *count) {
    unsigned char header[32];
    unsigned int magic;
    unsigned int ncmds;
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;
    unsigned long long file_size = 0ULL;
    long long end_offset;

    if (read_region(fd, 0ULL, header, sizeof(header)) != 0) return -1;
    magic = tool_read_u32_le(header);
    if (magic != 0xfeedfacfU) return -1;
    ncmds = tool_read_u32_le(header + 16);
    if (ncmds > STRINGS_MAX_MACHO_COMMANDS) return -1;
    end_offset = platform_seek(fd, 0, PLATFORM_SEEK_END);
    if (end_offset > 0) file_size = (unsigned long long)end_offset;

    for (command_index = 0U; command_index < ncmds; ++command_index) {
        unsigned char command_header[8];
        unsigned int command;
        unsigned int command_size;

        if (read_region(fd, command_offset, command_header, sizeof(command_header)) != 0) return -1;
        command = tool_read_u32_le(command_header);
        command_size = tool_read_u32_le(command_header + 4);
        if (command_size < 8U) return -1;
        if (command == 0x19U && command_size >= 72U) {
            unsigned char segment[72];
            unsigned int nsects;
            unsigned int section_index;

            if (read_region(fd, command_offset, segment, sizeof(segment)) != 0) return -1;
            nsects = tool_read_u32_le(segment + 64);
            if (72U + nsects * 80U > command_size) return -1;
            for (section_index = 0U; section_index < nsects; ++section_index) {
                unsigned char section[80];
                unsigned long long section_offset = command_offset + 72ULL + ((unsigned long long)section_index * 80ULL);
                unsigned long long size;
                unsigned int offset;
                unsigned int flags;
                unsigned int type;

                if (read_region(fd, section_offset, section, sizeof(section)) != 0) return -1;
                size = tool_read_u64_le(section + 40);
                offset = tool_read_u32_le(section + 48);
                flags = tool_read_u32_le(section + 64);
                type = flags & 0xffU;
                if (type != 1U && type != 8U && type != 12U && type != 18U && offset != 0U && size != 0ULL &&
                    (file_size == 0ULL || ((unsigned long long)offset <= file_size && size <= file_size - (unsigned long long)offset))) {
                    if (add_range(ranges, count, (unsigned long long)offset, size) != 0) return -1;
                }
            }
        }
        command_offset += (unsigned long long)command_size;
    }
    return *count > 0U ? 0 : -1;
}

static int load_pe_ranges(int fd, StringsRange *ranges, size_t *count) {
    unsigned char dos[64];
    unsigned char coff[24];
    unsigned int pe_offset;
    unsigned short section_count;
    unsigned short optional_size;
    unsigned short i;
    unsigned long long section_offset;

    if (read_region(fd, 0ULL, dos, sizeof(dos)) != 0) return -1;
    if (dos[0] != 'M' || dos[1] != 'Z') return -1;
    pe_offset = tool_read_u32_le(dos + 0x3cU);
    if (read_region(fd, (unsigned long long)pe_offset, coff, sizeof(coff)) != 0) return -1;
    if (!(coff[0] == 'P' && coff[1] == 'E' && coff[2] == 0U && coff[3] == 0U)) return -1;
    section_count = tool_read_u16_le(coff + 6);
    optional_size = tool_read_u16_le(coff + 20);
    if (section_count > STRINGS_MAX_SECTIONS) return -1;
    section_offset = (unsigned long long)pe_offset + 24ULL + (unsigned long long)optional_size;

    for (i = 0U; i < section_count; ++i) {
        unsigned char section[40];
        unsigned int raw_size;
        unsigned int raw_offset;
        unsigned int characteristics;

        if (read_region(fd, section_offset + ((unsigned long long)i * 40ULL), section, sizeof(section)) != 0) return -1;
        raw_size = tool_read_u32_le(section + 16);
        raw_offset = tool_read_u32_le(section + 20);
        characteristics = tool_read_u32_le(section + 36);
        if (raw_size > 0U && (characteristics & 0x02000000U) == 0U) {
            if (add_range(ranges, count, (unsigned long long)raw_offset, (unsigned long long)raw_size) != 0) return -1;
        }
    }
    return *count > 0U ? 0 : -1;
}

static int load_object_ranges(int fd, StringsRange *ranges, size_t *count) {
    *count = 0U;
    if (load_elf_ranges(fd, ranges, count) == 0 && *count > 0U) return 0;
    *count = 0U;
    if (load_macho_ranges(fd, ranges, count) == 0 && *count > 0U) return 0;
    *count = 0U;
    if (load_pe_ranges(fd, ranges, count) == 0 && *count > 0U) return 0;
    *count = 0U;
    return -1;
}

static int strings_object_sections(int fd, const StringsOptions *options, const char *file_name) {
    StringsRange ranges[STRINGS_MAX_SECTIONS];
    size_t count = 0U;
    size_t i;

    if (load_object_ranges(fd, ranges, &count) != 0) {
        if (platform_seek(fd, 0, PLATFORM_SEEK_SET) < 0) return -1;
        return strings_stream(fd, options, file_name);
    }

    for (i = 0U; i < count; ++i) {
        if (strings_range(fd, options, file_name, ranges[i].offset, ranges[i].size, 1) != 0) return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    StringsOptions options;
    int argi = 1;
    int exit_code = 0;
    int i;

    options.min_length = 4U;
    options.show_offset = 0;
    options.offset_base = 10U;
    options.print_file_name = 0;
    options.scan_data_sections = 0;
    options.encoding = STRINGS_ENCODING_8BIT;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-n") == 0 || (argv[argi][1] == 'n' && argv[argi][2] != '\0')) {
            unsigned long long parsed = 0ULL;
            const char *value_text = (argv[argi][2] != '\0') ? argv[argi] + 2 : ((argi + 1 < argc) ? argv[argi + 1] : 0);

            if (value_text == 0 || rt_parse_uint(value_text, &parsed) != 0 || parsed == 0ULL) {
                rt_write_line(2, "strings: invalid minimum length");
                return 1;
            }
            options.min_length = (size_t)parsed;
            argi += (argv[argi][2] != '\0') ? 1 : 2;
        } else if (argv[argi][1] >= '0' && argv[argi][1] <= '9') {
            unsigned long long parsed = 0ULL;
            if (rt_parse_uint(argv[argi] + 1, &parsed) != 0 || parsed == 0ULL) {
                rt_write_line(2, "strings: invalid minimum length");
                return 1;
            }
            options.min_length = (size_t)parsed;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-o") == 0) {
            options.show_offset = 1;
            options.offset_base = 8U;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-f") == 0) {
            options.print_file_name = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-a") == 0 || rt_strcmp(argv[argi], "--all") == 0) {
            options.scan_data_sections = 0;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-d") == 0 || rt_strcmp(argv[argi], "--data") == 0) {
            options.scan_data_sections = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-t") == 0 || (argv[argi][1] == 't' && argv[argi][2] != '\0')) {
            const char *radix = (argv[argi][2] != '\0') ? argv[argi] + 2 : ((argi + 1 < argc) ? argv[argi + 1] : 0);

            if (radix == 0) {
                rt_write_line(2, "strings: missing radix for -t");
                return 1;
            }

            if (radix[0] == 'd' && radix[1] == '\0') {
                options.offset_base = 10U;
            } else if (radix[0] == 'o' && radix[1] == '\0') {
                options.offset_base = 8U;
            } else if (radix[0] == 'x' && radix[1] == '\0') {
                options.offset_base = 16U;
            } else {
                rt_write_line(2, "strings: invalid radix for -t");
                return 1;
            }
            options.show_offset = 1;
            argi += (argv[argi][2] != '\0') ? 1 : 2;
        } else if (rt_strcmp(argv[argi], "-e") == 0 || (argv[argi][1] == 'e' && argv[argi][2] != '\0')) {
            const char *encoding = (argv[argi][2] != '\0') ? argv[argi] + 2 : ((argi + 1 < argc) ? argv[argi + 1] : 0);

            if (encoding == 0 || encoding[1] != '\0') {
                rt_write_line(2, "strings: invalid encoding for -e");
                return 1;
            }

            if (encoding[0] == 's') {
                options.encoding = STRINGS_ENCODING_7BIT;
            } else if (encoding[0] == 'S') {
                options.encoding = STRINGS_ENCODING_8BIT;
            } else if (encoding[0] == 'l') {
                options.encoding = STRINGS_ENCODING_16LE;
            } else if (encoding[0] == 'b') {
                options.encoding = STRINGS_ENCODING_16BE;
            } else if (encoding[0] == 'L') {
                options.encoding = STRINGS_ENCODING_32LE;
            } else if (encoding[0] == 'B') {
                options.encoding = STRINGS_ENCODING_32BE;
            } else {
                rt_write_line(2, "strings: invalid encoding for -e");
                return 1;
            }

            argi += (argv[argi][2] != '\0') ? 1 : 2;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            break;
        }
    }

    if (argi == argc) {
        return strings_stream(0, &options, 0) == 0 ? 0 : 1;
    }

    for (i = argi; i < argc; ++i) {
        int fd;
        int should_close;

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "strings: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if ((options.scan_data_sections ? strings_object_sections(fd, &options, argv[i]) : strings_stream(fd, &options, argv[i])) != 0) {
            rt_write_cstr(2, "strings: read error on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
