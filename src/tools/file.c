#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    const char *description;
    const char *mime;
    const char *magic;
    const char *details;
} FileTypeInfo;

typedef struct {
    int mime_only;
    int follow_symlinks;
    int brief;
    int verbose;
} FileOptions;

static char dynamic_description[256];
static char dynamic_details[2048];

static unsigned char ascii_lower(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (unsigned char)(ch - 'A' + 'a');
    }
    return ch;
}

static int starts_with_ci(const unsigned char *buffer, size_t length, const char *text) {
    size_t i = 0;

    while (text[i] != '\0') {
        if (i >= length || ascii_lower(buffer[i]) != ascii_lower((unsigned char)text[i])) {
            return 0;
        }
        i += 1U;
    }
    return 1;
}

static int find_text_ci(const unsigned char *buffer, size_t length, const char *needle) {
    size_t i;
    size_t needle_length = rt_strlen(needle);

    if (needle_length == 0U || needle_length > length) {
        return 0;
    }

    for (i = 0; i + needle_length <= length; ++i) {
        size_t j;
        int match = 1;
        for (j = 0; j < needle_length; ++j) {
            if (ascii_lower(buffer[i + j]) != ascii_lower((unsigned char)needle[j])) {
                match = 0;
                break;
            }
        }
        if (match) {
            return 1;
        }
    }
    return 0;
}

static int looks_like_text(const unsigned char *buffer, size_t length) {
    size_t i;

    if (length == 0U) {
        return 1;
    }

    for (i = 0; i < length; ++i) {
        unsigned char ch = buffer[i];
        if (ch == 0U) {
            return 0;
        }
        if ((ch < 32U || ch > 126U) && ch != '\n' && ch != '\r' && ch != '\t') {
            return 0;
        }
    }

    return 1;
}

static int looks_like_json(const unsigned char *buffer, size_t length) {
    size_t start = 0;
    size_t end = length;

    while (start < length && (buffer[start] == ' ' || buffer[start] == '\n' || buffer[start] == '\r' || buffer[start] == '\t')) {
        start += 1U;
    }
    while (end > start && (buffer[end - 1U] == ' ' || buffer[end - 1U] == '\n' || buffer[end - 1U] == '\r' || buffer[end - 1U] == '\t')) {
        end -= 1U;
    }

    if (end <= start) {
        return 0;
    }

    return (buffer[start] == '{' && buffer[end - 1U] == '}') || (buffer[start] == '[' && buffer[end - 1U] == ']');
}

static int looks_like_html(const unsigned char *buffer, size_t length) {
    size_t start = 0;

    while (start < length && (buffer[start] == ' ' || buffer[start] == '\n' || buffer[start] == '\r' || buffer[start] == '\t')) {
        start += 1U;
    }

    if (start >= length) {
        return 0;
    }

    return starts_with_ci(buffer + start, length - start, "<!doctype html") ||
           starts_with_ci(buffer + start, length - start, "<html") ||
           starts_with_ci(buffer + start, length - start, "<head") ||
           starts_with_ci(buffer + start, length - start, "<body");
}

static int looks_like_xml(const unsigned char *buffer, size_t length) {
    size_t start = 0;

    while (start < length && (buffer[start] == ' ' || buffer[start] == '\n' || buffer[start] == '\r' || buffer[start] == '\t')) {
        start += 1U;
    }

    if (start >= length) {
        return 0;
    }

    return starts_with_ci(buffer + start, length - start, "<?xml") || starts_with_ci(buffer + start, length - start, "<svg");
}

static unsigned short read_u16_le(const unsigned char *buffer) {
    return (unsigned short)((unsigned short)buffer[0] | ((unsigned short)buffer[1] << 8U));
}

static unsigned short read_u16_be(const unsigned char *buffer) {
    return (unsigned short)(((unsigned short)buffer[0] << 8U) | (unsigned short)buffer[1]);
}

static unsigned int read_u32_le(const unsigned char *buffer) {
    return (unsigned int)buffer[0] |
           ((unsigned int)buffer[1] << 8U) |
           ((unsigned int)buffer[2] << 16U) |
           ((unsigned int)buffer[3] << 24U);
}

static unsigned int read_u32_be(const unsigned char *buffer) {
    return ((unsigned int)buffer[0] << 24U) |
           ((unsigned int)buffer[1] << 16U) |
           ((unsigned int)buffer[2] << 8U) |
           (unsigned int)buffer[3];
}

static void dynamic_set(const char *text) {
    rt_copy_string(dynamic_description, sizeof(dynamic_description), text);
}

static void dynamic_append(const char *text) {
    size_t used = rt_strlen(dynamic_description);
    if (used < sizeof(dynamic_description)) rt_copy_string(dynamic_description + used, sizeof(dynamic_description) - used, text);
}

static void dynamic_append_uint(unsigned long long value) {
    char number[32];
    rt_unsigned_to_string(value, number, sizeof(number));
    dynamic_append(number);
}

static void details_clear(void) {
    dynamic_details[0] = '\0';
}

static void details_append(const char *text) {
    size_t used = rt_strlen(dynamic_details);
    if (used < sizeof(dynamic_details)) rt_copy_string(dynamic_details + used, sizeof(dynamic_details) - used, text);
}

static void details_append_uint(unsigned long long value) {
    char number[32];
    rt_unsigned_to_string(value, number, sizeof(number));
    details_append(number);
}

static void details_append_hex(unsigned long long value) {
    static const char digits[] = "0123456789abcdef";
    char scratch[32];
    size_t count = 0U;

    details_append("0x");
    do {
        scratch[count++] = digits[value & 0xfULL];
        value >>= 4U;
    } while (value != 0ULL && count < sizeof(scratch));
    while (count > 0U) {
        char single[2];
        count -= 1U;
        single[0] = scratch[count];
        single[1] = '\0';
        details_append(single);
    }
}

static unsigned long long read_u64_le(const unsigned char *buffer) {
    return (unsigned long long)read_u32_le(buffer) | ((unsigned long long)read_u32_le(buffer + 4U) << 32U);
}

static const char *elf_type_name(unsigned int type) {
    if (type == 1U) return "relocatable";
    if (type == 2U) return "executable";
    if (type == 3U) return "shared object";
    if (type == 4U) return "core";
    return "unknown";
}

static const char *elf_machine_name(unsigned int machine) {
    if (machine == 3U) return "Intel 80386";
    if (machine == 40U) return "ARM";
    if (machine == 62U) return "x86-64";
    if (machine == 183U) return "AArch64";
    if (machine == 243U) return "RISC-V";
    return "unknown architecture";
}

static int describe_elf(const unsigned char *buffer, size_t length, FileTypeInfo *info) {
    unsigned int file_class;
    unsigned int data;
    unsigned int type;
    unsigned int machine;

    if (length < 20U || !(buffer[0] == 0x7fU && buffer[1] == 'E' && buffer[2] == 'L' && buffer[3] == 'F')) return 0;
    file_class = buffer[4];
    data = buffer[5];
    if ((file_class != 1U && file_class != 2U) || (data != 1U && data != 2U)) {
        info->description = "ELF binary";
        info->mime = "application/x-executable";
        info->magic = "ELF";
        return 1;
    }
    type = data == 1U ? read_u16_le(buffer + 16) : read_u16_be(buffer + 16);
    machine = data == 1U ? read_u16_le(buffer + 18) : read_u16_be(buffer + 18);
    dynamic_set("ELF ");
    dynamic_append(file_class == 1U ? "32-bit " : "64-bit ");
    dynamic_append(data == 1U ? "LSB " : "MSB ");
    dynamic_append(elf_type_name(type));
    dynamic_append(", ");
    dynamic_append(elf_machine_name(machine));
    info->description = dynamic_description;
    info->mime = "application/x-executable";
    info->magic = "ELF";
    return 1;
}

static const char *macho_type_name(unsigned int type) {
    if (type == 1U) return "object";
    if (type == 2U) return "executable";
    if (type == 6U) return "dynamic library";
    if (type == 8U) return "bundle";
    return "binary";
}

static const char *macho_machine_name(unsigned int cputype) {
    unsigned int family = cputype & 0x00ffffffU;
    if (cputype == 0x01000007U) return "x86-64";
    if (cputype == 0x0100000cU) return "arm64";
    if (family == 7U) return "x86";
    if (family == 12U) return "arm";
    return "unknown architecture";
}

static int describe_macho(const unsigned char *buffer, size_t length, FileTypeInfo *info) {
    unsigned int magic;
    unsigned int cputype;
    unsigned int filetype;
    int is_big_endian = 0;
    int is_64_bit = 0;

    if (length < 16U) return 0;
    magic = read_u32_le(buffer);
    if (magic == 0xcafebabeU || magic == 0xbebafecaU) {
        info->description = "Mach-O universal binary";
        info->mime = "application/x-mach-binary";
        info->magic = "Mach-O fat";
        return 1;
    }
    if (magic == 0xfeedfaceU) {
        is_64_bit = 0;
    } else if (magic == 0xfeedfacfU) {
        is_64_bit = 1;
    } else if (magic == 0xcefaedfeU) {
        is_big_endian = 1;
        is_64_bit = 0;
    } else if (magic == 0xcffaedfeU) {
        is_big_endian = 1;
        is_64_bit = 1;
    } else {
        return 0;
    }
    cputype = is_big_endian ? read_u32_be(buffer + 4) : read_u32_le(buffer + 4);
    filetype = is_big_endian ? read_u32_be(buffer + 12) : read_u32_le(buffer + 12);
    dynamic_set("Mach-O ");
    dynamic_append(is_64_bit ? "64-bit " : "32-bit ");
    dynamic_append(macho_type_name(filetype));
    dynamic_append(" ");
    dynamic_append(macho_machine_name(cputype));
    info->description = dynamic_description;
    info->mime = "application/x-mach-binary";
    info->magic = "Mach-O";
    return 1;
}

static const char *pe_machine_name(unsigned int machine) {
    if (machine == 0x014cU) return "Intel 80386";
    if (machine == 0x01c0U) return "ARM";
    if (machine == 0x01c4U) return "ARMv7";
    if (machine == 0x8664U) return "x86-64";
    if (machine == 0xaa64U) return "ARM64";
    return "unknown architecture";
}

static const char *pe_subsystem_name(unsigned int subsystem) {
    if (subsystem == 1U) return "native";
    if (subsystem == 2U) return "GUI";
    if (subsystem == 3U) return "console";
    if (subsystem == 9U) return "Windows CE";
    if (subsystem == 10U) return "EFI application";
    if (subsystem == 11U) return "EFI boot service driver";
    if (subsystem == 12U) return "EFI runtime driver";
    return "unknown subsystem";
}

static void pe_append_coff_flags(unsigned int characteristics) {
    int wrote = 0;

    details_append("  pe-characteristics:");
    if ((characteristics & 0x0002U) != 0U) { details_append(wrote ? ", executable" : " executable"); wrote = 1; }
    if ((characteristics & 0x2000U) != 0U) { details_append(wrote ? ", dll" : " dll"); wrote = 1; }
    if ((characteristics & 0x0020U) != 0U) { details_append(wrote ? ", large-address-aware" : " large-address-aware"); wrote = 1; }
    if ((characteristics & 0x0100U) != 0U) { details_append(wrote ? ", 32-bit-machine" : " 32-bit-machine"); wrote = 1; }
    if ((characteristics & 0x0001U) != 0U) { details_append(wrote ? ", relocs-stripped" : " relocs-stripped"); wrote = 1; }
    if ((characteristics & 0x0200U) != 0U) { details_append(wrote ? ", debug-stripped" : " debug-stripped"); wrote = 1; }
    if (!wrote) details_append(" -");
    details_append("\n");
}

static void pe_append_dll_flags(unsigned int flags) {
    int wrote = 0;

    details_append("  pe-dll-characteristics:");
    if ((flags & 0x0020U) != 0U) { details_append(wrote ? ", high-entropy-va" : " high-entropy-va"); wrote = 1; }
    if ((flags & 0x0040U) != 0U) { details_append(wrote ? ", dynamic-base" : " dynamic-base"); wrote = 1; }
    if ((flags & 0x0100U) != 0U) { details_append(wrote ? ", nx-compatible" : " nx-compatible"); wrote = 1; }
    if ((flags & 0x0400U) != 0U) { details_append(wrote ? ", no-seh" : " no-seh"); wrote = 1; }
    if ((flags & 0x0800U) != 0U) { details_append(wrote ? ", no-bind" : " no-bind"); wrote = 1; }
    if ((flags & 0x1000U) != 0U) { details_append(wrote ? ", appcontainer" : " appcontainer"); wrote = 1; }
    if ((flags & 0x2000U) != 0U) { details_append(wrote ? ", wdm-driver" : " wdm-driver"); wrote = 1; }
    if ((flags & 0x4000U) != 0U) { details_append(wrote ? ", guard-cf" : " guard-cf"); wrote = 1; }
    if ((flags & 0x8000U) != 0U) { details_append(wrote ? ", terminal-server-aware" : " terminal-server-aware"); wrote = 1; }
    if (!wrote) details_append(" -");
    details_append("\n");
}

static void pe_append_timestamp(unsigned int timestamp) {
    char time_text[64];

    details_append("  pe-timestamp: ");
    if (timestamp != 0U && platform_format_time((long long)timestamp, 0, "%Y-%m-%d %H:%M:%S UTC", time_text, sizeof(time_text)) == 0) {
        details_append(time_text);
        details_append(" (");
        details_append_uint(timestamp);
        details_append(")\n");
    } else {
        details_append_uint(timestamp);
        details_append("\n");
    }
}

static void pe_append_section_name(const unsigned char *name) {
    size_t i;
    for (i = 0U; i < 8U && name[i] != 0U; ++i) {
        char text[2];
        unsigned char ch = name[i];
        text[0] = (ch >= 32U && ch <= 126U) ? (char)ch : '?';
        text[1] = '\0';
        details_append(text);
    }
}

static int describe_pe(const unsigned char *buffer, size_t length, FileTypeInfo *info) {
    unsigned int pe_offset;
    unsigned int machine;
    unsigned int section_count;
    unsigned int optional_size;
    unsigned int characteristics;
    unsigned int timestamp;
    unsigned int optional_magic = 0U;
    unsigned int subsystem = 0U;
    unsigned int linker_major = 0U;
    unsigned int linker_minor = 0U;
    unsigned int entry_rva = 0U;
    unsigned long long image_base = 0ULL;
    unsigned int section_alignment = 0U;
    unsigned int file_alignment = 0U;
    unsigned int size_of_image = 0U;
    unsigned int size_of_headers = 0U;
    unsigned int checksum = 0U;
    unsigned int major_os = 0U;
    unsigned int minor_os = 0U;
    unsigned int major_subsystem = 0U;
    unsigned int minor_subsystem = 0U;
    unsigned int dll_characteristics = 0U;
    size_t optional_offset;
    size_t section_offset;
    unsigned int section_index;

    if (length < 64U || buffer[0] != 'M' || buffer[1] != 'Z') return 0;
    pe_offset = read_u32_le(buffer + 0x3cU);
    if (pe_offset > 4096U || pe_offset + 24U > length) {
        info->description = "DOS/PE executable";
        info->mime = "application/vnd.microsoft.portable-executable";
        info->magic = "MZ";
        return 1;
    }
    if (!(buffer[pe_offset] == 'P' && buffer[pe_offset + 1U] == 'E' && buffer[pe_offset + 2U] == 0U && buffer[pe_offset + 3U] == 0U)) {
        info->description = "DOS executable";
        info->mime = "application/x-msdos-program";
        info->magic = "MZ";
        return 1;
    }

    machine = read_u16_le(buffer + pe_offset + 4U);
    section_count = read_u16_le(buffer + pe_offset + 6U);
    timestamp = read_u32_le(buffer + pe_offset + 8U);
    optional_size = read_u16_le(buffer + pe_offset + 20U);
    characteristics = read_u16_le(buffer + pe_offset + 22U);
    optional_offset = (size_t)pe_offset + 24U;
    if (optional_size >= 2U && optional_offset + 2U <= length) {
        optional_magic = read_u16_le(buffer + optional_offset);
    }
    if (optional_size >= 70U && optional_offset + 70U <= length && (optional_magic == 0x010bU || optional_magic == 0x020bU)) {
        linker_major = buffer[optional_offset + 2U];
        linker_minor = buffer[optional_offset + 3U];
        entry_rva = read_u32_le(buffer + optional_offset + 16U);
        if (optional_magic == 0x020bU && optional_offset + 32U <= length) image_base = read_u64_le(buffer + optional_offset + 24U);
        else if (optional_magic == 0x010bU && optional_offset + 32U <= length) image_base = read_u32_le(buffer + optional_offset + 28U);
        section_alignment = read_u32_le(buffer + optional_offset + 32U);
        file_alignment = read_u32_le(buffer + optional_offset + 36U);
        major_os = read_u16_le(buffer + optional_offset + 40U);
        minor_os = read_u16_le(buffer + optional_offset + 42U);
        major_subsystem = read_u16_le(buffer + optional_offset + 48U);
        minor_subsystem = read_u16_le(buffer + optional_offset + 50U);
        size_of_image = read_u32_le(buffer + optional_offset + 56U);
        size_of_headers = read_u32_le(buffer + optional_offset + 60U);
        checksum = read_u32_le(buffer + optional_offset + 64U);
        subsystem = read_u16_le(buffer + optional_offset + 68U);
        if (optional_size >= 72U && optional_offset + 72U <= length) dll_characteristics = read_u16_le(buffer + optional_offset + 70U);
    }

    dynamic_set("PE/COFF ");
    if ((characteristics & 0x2000U) != 0U) dynamic_append("DLL");
    else if ((characteristics & 0x0002U) != 0U) dynamic_append("executable");
    else dynamic_append("object");
    if (optional_magic == 0x020bU) dynamic_append(" PE32+");
    else if (optional_magic == 0x010bU) dynamic_append(" PE32");
    dynamic_append(" ");
    dynamic_append(pe_machine_name(machine));
    if (subsystem != 0U) {
        dynamic_append(", ");
        dynamic_append(pe_subsystem_name(subsystem));
    }
    dynamic_append(", ");
    dynamic_append_uint(section_count);
    dynamic_append(section_count == 1U ? " section" : " sections");

    info->description = dynamic_description;
    info->mime = "application/vnd.microsoft.portable-executable";
    info->magic = "PE/COFF";
    details_clear();
    pe_append_timestamp(timestamp);
    details_append("  pe-linker-version: ");
    details_append_uint(linker_major);
    details_append(".");
    details_append_uint(linker_minor);
    details_append("\n");
    details_append("  pe-entry-rva: ");
    details_append_hex(entry_rva);
    details_append("\n");
    details_append("  pe-image-base: ");
    details_append_hex(image_base);
    details_append("\n");
    details_append("  pe-image-size: ");
    details_append_uint(size_of_image);
    details_append(" bytes\n");
    details_append("  pe-headers-size: ");
    details_append_uint(size_of_headers);
    details_append(" bytes\n");
    details_append("  pe-section-alignment: ");
    details_append_uint(section_alignment);
    details_append("\n");
    details_append("  pe-file-alignment: ");
    details_append_uint(file_alignment);
    details_append("\n");
    details_append("  pe-checksum: ");
    details_append_hex(checksum);
    details_append("\n");
    if (subsystem != 0U) {
        details_append("  pe-subsystem-version: ");
        details_append_uint(major_subsystem);
        details_append(".");
        details_append_uint(minor_subsystem);
        details_append("\n");
    }
    details_append("  pe-os-version: ");
    details_append_uint(major_os);
    details_append(".");
    details_append_uint(minor_os);
    details_append("\n");
    pe_append_coff_flags(characteristics);
    pe_append_dll_flags(dll_characteristics);

    section_offset = optional_offset + optional_size;
    if (section_count > 0U && section_offset + 40U <= length) {
        details_append("  pe-sections:\n");
        for (section_index = 0U; section_index < section_count && section_index < 12U; ++section_index) {
            size_t offset = section_offset + (size_t)section_index * 40U;
            if (offset + 40U > length) break;
            details_append("    ");
            pe_append_section_name(buffer + offset);
            details_append(": rva=");
            details_append_hex(read_u32_le(buffer + offset + 12U));
            details_append(" vsize=");
            details_append_uint(read_u32_le(buffer + offset + 8U));
            details_append(" raw=");
            details_append_uint(read_u32_le(buffer + offset + 16U));
            details_append(" flags=");
            details_append_hex(read_u32_le(buffer + offset + 36U));
            details_append("\n");
        }
        if (section_count > 12U) details_append("    ...\n");
    }
    info->details = dynamic_details;
    return 1;
}

static int describe_png(const unsigned char *buffer, size_t length, FileTypeInfo *info) {
    if (length < 24U || !(buffer[0] == 0x89U && buffer[1] == 'P' && buffer[2] == 'N' && buffer[3] == 'G' &&
        buffer[4] == '\r' && buffer[5] == '\n' && buffer[6] == 0x1aU && buffer[7] == '\n')) return 0;
    dynamic_set("PNG image data");
    if (buffer[12] == 'I' && buffer[13] == 'H' && buffer[14] == 'D' && buffer[15] == 'R') {
        dynamic_append(", ");
        dynamic_append_uint(read_u32_be(buffer + 16));
        dynamic_append(" x ");
        dynamic_append_uint(read_u32_be(buffer + 20));
    }
    info->description = dynamic_description;
    info->mime = "image/png";
    info->magic = "PNG";
    return 1;
}

static int describe_gif(const unsigned char *buffer, size_t length, FileTypeInfo *info) {
    if (length < 10U || !(buffer[0] == 'G' && buffer[1] == 'I' && buffer[2] == 'F' &&
        buffer[3] == '8' && (buffer[4] == '7' || buffer[4] == '9') && buffer[5] == 'a')) return 0;
    dynamic_set("GIF image data, ");
    dynamic_append_uint(read_u16_le(buffer + 6));
    dynamic_append(" x ");
    dynamic_append_uint(read_u16_le(buffer + 8));
    info->description = dynamic_description;
    info->mime = "image/gif";
    info->magic = "GIF";
    return 1;
}

static int describe_bmp(const unsigned char *buffer, size_t length, FileTypeInfo *info) {
    if (length < 26U || !(buffer[0] == 'B' && buffer[1] == 'M')) return 0;
    dynamic_set("BMP image data, ");
    dynamic_append_uint(read_u32_le(buffer + 18));
    dynamic_append(" x ");
    dynamic_append_uint(read_u32_le(buffer + 22));
    info->description = dynamic_description;
    info->mime = "image/bmp";
    info->magic = "BMP";
    return 1;
}

static int jpeg_dimensions(const unsigned char *buffer, size_t length, unsigned int *width_out, unsigned int *height_out) {
    size_t pos = 2U;
    while (pos + 9U < length) {
        unsigned char marker;
        unsigned int segment_length;
        while (pos < length && buffer[pos] == 0xffU) pos += 1U;
        if (pos >= length) break;
        marker = buffer[pos++];
        if (marker == 0xd9U || marker == 0xdaU) break;
        if (pos + 2U > length) break;
        segment_length = read_u16_be(buffer + pos);
        if (segment_length < 2U || pos + segment_length > length) break;
        if ((marker >= 0xc0U && marker <= 0xc3U) || (marker >= 0xc5U && marker <= 0xc7U) ||
            (marker >= 0xc9U && marker <= 0xcbU) || (marker >= 0xcdU && marker <= 0xcfU)) {
            if (segment_length >= 7U) {
                *height_out = read_u16_be(buffer + pos + 3U);
                *width_out = read_u16_be(buffer + pos + 5U);
                return 0;
            }
            break;
        }
        pos += segment_length;
    }
    return -1;
}

static int describe_jpeg(const unsigned char *buffer, size_t length, FileTypeInfo *info) {
    unsigned int width = 0U;
    unsigned int height = 0U;
    if (length < 3U || !(buffer[0] == 0xffU && buffer[1] == 0xd8U && buffer[2] == 0xffU)) return 0;
    dynamic_set("JPEG image data");
    if (jpeg_dimensions(buffer, length, &width, &height) == 0) {
        dynamic_append(", ");
        dynamic_append_uint(width);
        dynamic_append(" x ");
        dynamic_append_uint(height);
    }
    info->description = dynamic_description;
    info->mime = "image/jpeg";
    info->magic = "JPEG";
    return 1;
}

static FileTypeInfo detect_type(const unsigned char *buffer, size_t length) {
    FileTypeInfo info;

    info.description = "data";
    info.mime = "application/octet-stream";
    info.magic = "";
    info.details = "";

    if (length == 0U) {
        info.description = "empty";
        info.mime = "application/x-empty";
        return info;
    }
    if (describe_elf(buffer, length, &info)) return info;
    if (length >= 2U && buffer[0] == 0x1fU && buffer[1] == 0x8bU) {
        info.description = "gzip compressed data";
        info.mime = "application/gzip";
        return info;
    }
    if (length >= 6U && buffer[0] == '7' && buffer[1] == 'z' && buffer[2] == 0xbcU && buffer[3] == 0xafU && buffer[4] == 0x27U && buffer[5] == 0x1cU) {
        info.description = "7-zip archive data";
        info.mime = "application/x-7z-compressed";
        return info;
    }
    if (length >= 4U && buffer[0] == 'B' && buffer[1] == 'Z' && buffer[2] == 'h') {
        info.description = "bzip2 compressed data";
        info.mime = "application/x-bzip2";
        return info;
    }
    if (length >= 6U && buffer[0] == 0xfdU && buffer[1] == '7' && buffer[2] == 'z' && buffer[3] == 'X' && buffer[4] == 'Z' && buffer[5] == 0x00U) {
        info.description = "XZ compressed data";
        info.mime = "application/x-xz";
        return info;
    }
    if (length >= 2U && buffer[0] == '#' && buffer[1] == '!') {
        if (find_text_ci(buffer, length, "python")) {
            info.description = "Python script text executable";
            info.mime = "text/x-python; charset=us-ascii";
        } else if (find_text_ci(buffer, length, "perl")) {
            info.description = "Perl script text executable";
            info.mime = "text/x-perl; charset=us-ascii";
        } else if (find_text_ci(buffer, length, "awk")) {
            info.description = "AWK script text executable";
            info.mime = "text/x-awk; charset=us-ascii";
        } else {
            info.description = "script text executable";
            info.mime = "text/x-shellscript; charset=us-ascii";
        }
        return info;
    }
    if (length >= 4U && buffer[0] == 'R' && buffer[1] == 'I' && buffer[2] == 'F' && buffer[3] == 'F') {
        if (length >= 12U && buffer[8] == 'W' && buffer[9] == 'A' && buffer[10] == 'V' && buffer[11] == 'E') {
            info.description = "WAV audio data";
            info.mime = "audio/wav";
            return info;
        }
        if (length >= 12U && buffer[8] == 'W' && buffer[9] == 'E' && buffer[10] == 'B' && buffer[11] == 'P') {
            info.description = "WebP image data";
            info.mime = "image/webp";
            return info;
        }
        if (length >= 12U && buffer[8] == 'A' && buffer[9] == 'V' && buffer[10] == 'I' && buffer[11] == ' ') {
            info.description = "AVI video data";
            info.mime = "video/x-msvideo";
            return info;
        }
    }
    if (length > 262U && buffer[257] == 'u' && buffer[258] == 's' && buffer[259] == 't' && buffer[260] == 'a' && buffer[261] == 'r') {
        info.description = "tar archive";
        info.mime = "application/x-tar";
        return info;
    }
    if (describe_png(buffer, length, &info)) return info;
    if (describe_jpeg(buffer, length, &info)) return info;
    if (describe_gif(buffer, length, &info)) return info;
    if (length >= 5U && buffer[0] == '%' && buffer[1] == 'P' && buffer[2] == 'D' && buffer[3] == 'F' && buffer[4] == '-') {
        info.description = "PDF document";
        info.mime = "application/pdf";
        return info;
    }
    if (length >= 10U && buffer[0] == '%' && buffer[1] == '!' && buffer[2] == 'P' && buffer[3] == 'S') {
        info.description = "PostScript document";
        info.mime = "application/postscript";
        return info;
    }
    if (length >= 4U && buffer[0] == 'P' && buffer[1] == 'K' && (buffer[2] == 0x03U || buffer[2] == 0x05U || buffer[2] == 0x07U)) {
        info.description = "ZIP archive data";
        info.mime = "application/zip";
        return info;
    }
    if (length >= 8U && buffer[0] == '!' && buffer[1] == '<' && buffer[2] == 'a' && buffer[3] == 'r' &&
        buffer[4] == 'c' && buffer[5] == 'h' && buffer[6] == '>' && buffer[7] == '\n') {
        info.description = "ar archive";
        info.mime = "application/x-archive";
        return info;
    }
    if (describe_bmp(buffer, length, &info)) return info;
    if (length >= 4U && buffer[0] == 'O' && buffer[1] == 'g' && buffer[2] == 'g' && buffer[3] == 'S') {
        info.description = "Ogg data";
        info.mime = "application/ogg";
        return info;
    }
    if (length >= 4U && buffer[0] == 'f' && buffer[1] == 'L' && buffer[2] == 'a' && buffer[3] == 'C') {
        info.description = "FLAC audio bitstream data";
        info.mime = "audio/flac";
        return info;
    }
    if (length >= 3U && buffer[0] == 'I' && buffer[1] == 'D' && buffer[2] == '3') {
        info.description = "MP3 audio with ID3 tag";
        info.mime = "audio/mpeg";
        return info;
    }
    if (describe_pe(buffer, length, &info)) return info;
    if (describe_macho(buffer, length, &info)) return info;
    if (length >= 16U &&
        buffer[0] == 'S' && buffer[1] == 'Q' && buffer[2] == 'L' && buffer[3] == 'i' &&
        buffer[4] == 't' && buffer[5] == 'e' && buffer[6] == ' ' && buffer[7] == 'f' &&
        buffer[8] == 'o' && buffer[9] == 'r' && buffer[10] == 'm' && buffer[11] == 'a' &&
        buffer[12] == 't' && buffer[13] == ' ' && buffer[14] == '3' && buffer[15] == 0x00U) {
        info.description = "SQLite 3.x database";
        info.mime = "application/vnd.sqlite3";
        return info;
    }
    if (length >= 4U && buffer[0] == 0x00U && buffer[1] == 'a' && buffer[2] == 's' && buffer[3] == 'm') {
        info.description = "WebAssembly binary module";
        info.mime = "application/wasm";
        return info;
    }
    if (length >= 3U && buffer[0] == 0xefU && buffer[1] == 0xbbU && buffer[2] == 0xbfU) {
        info.description = "UTF-8 Unicode text";
        info.mime = "text/plain; charset=utf-8";
        return info;
    }
    if (length >= 2U &&
        ((buffer[0] == 0xffU && buffer[1] == 0xfeU) || (buffer[0] == 0xfeU && buffer[1] == 0xffU))) {
        info.description = "UTF-16 Unicode text";
        info.mime = "text/plain; charset=utf-16";
        return info;
    }
    if (looks_like_text(buffer, length)) {
        if (looks_like_json(buffer, length)) {
            info.description = "JSON text";
            info.mime = "application/json";
        } else if (looks_like_html(buffer, length)) {
            info.description = "HTML document text";
            info.mime = "text/html; charset=us-ascii";
        } else if (looks_like_xml(buffer, length)) {
            if (find_text_ci(buffer, length, "<svg")) {
                info.description = "SVG Scalable Vector Graphics image";
                info.mime = "image/svg+xml";
            } else {
                info.description = "XML document text";
                info.mime = "application/xml";
            }
        } else {
            info.description = "ASCII text";
            info.mime = "text/plain; charset=us-ascii";
        }
    }
    return info;
}

static int write_time_value(long long epoch_seconds) {
    char buffer[64];

    if (platform_format_time(epoch_seconds, 1, "%Y-%m-%d %H:%M:%S", buffer, sizeof(buffer)) == 0) {
        return rt_write_cstr(1, buffer);
    }
    return rt_write_int(1, epoch_seconds);
}

static int write_single_line_result(const char *path, const FileOptions *options, const char *text) {
    if (!options->brief) {
        rt_write_cstr(1, path != 0 ? path : "stdin");
        rt_write_cstr(1, ": ");
    }
    return rt_write_line(1, text);
}

static int write_verbose_metadata(const PlatformDirEntry *entry) {
    char mode_text[11];

    if (entry == 0) return 0;
    platform_format_mode(entry->mode, mode_text);
    if (rt_write_cstr(1, "  size: ") != 0 || rt_write_uint(1, entry->size) != 0 || rt_write_line(1, " bytes") != 0) return -1;
    if (rt_write_cstr(1, "  mode: ") != 0 || rt_write_line(1, mode_text) != 0) return -1;
    if (rt_write_cstr(1, "  inode: ") != 0 || rt_write_uint(1, entry->inode) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  device: ") != 0 || rt_write_uint(1, entry->device) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  links: ") != 0 || rt_write_uint(1, entry->nlink) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  owner: ") != 0 || rt_write_cstr(1, entry->owner[0] != '\0' ? entry->owner : "?") != 0 ||
        rt_write_cstr(1, " (") != 0 || rt_write_uint(1, entry->uid) != 0 || rt_write_line(1, ")") != 0) return -1;
    if (rt_write_cstr(1, "  group: ") != 0 || rt_write_cstr(1, entry->group[0] != '\0' ? entry->group : "?") != 0 ||
        rt_write_cstr(1, " (") != 0 || rt_write_uint(1, entry->gid) != 0 || rt_write_line(1, ")") != 0) return -1;
    if (rt_write_cstr(1, "  modified: ") != 0 || write_time_value(entry->mtime) != 0 || rt_write_char(1, '\n') != 0) return -1;
    return 0;
}

static int write_verbose_file_result(const char *path, const FileTypeInfo *info, const PlatformDirEntry *entry, long bytes_read) {
    rt_write_cstr(1, path != 0 ? path : "stdin");
    rt_write_line(1, ":");
    if (rt_write_cstr(1, "  type: ") != 0 || rt_write_line(1, info->description) != 0) return -1;
    if (rt_write_cstr(1, "  mime: ") != 0 || rt_write_line(1, info->mime) != 0) return -1;
    if (info->magic != 0 && info->magic[0] != '\0') {
        if (rt_write_cstr(1, "  magic: ") != 0 || rt_write_line(1, info->magic) != 0) return -1;
    }
    if (info->details != 0 && info->details[0] != '\0') {
        if (rt_write_cstr(1, info->details) != 0) return -1;
    }
    if (entry != 0) return write_verbose_metadata(entry);
    if (rt_write_cstr(1, "  sampled: ") != 0 || rt_write_uint(1, bytes_read < 0 ? 0ULL : (unsigned long long)bytes_read) != 0 || rt_write_line(1, " bytes") != 0) return -1;
    return 0;
}

static int write_verbose_node_result(const char *path, const char *type, const char *mime, const PlatformDirEntry *entry, const char *target) {
    rt_write_cstr(1, path != 0 ? path : "stdin");
    rt_write_line(1, ":");
    if (rt_write_cstr(1, "  type: ") != 0 || rt_write_line(1, type) != 0) return -1;
    if (rt_write_cstr(1, "  mime: ") != 0 || rt_write_line(1, mime) != 0) return -1;
    if (target != 0 && target[0] != '\0') {
        if (rt_write_cstr(1, "  target: ") != 0 || rt_write_line(1, target) != 0) return -1;
    }
    return write_verbose_metadata(entry);
}

static int describe_path(const char *path, const FileOptions *options) {
    int fd;
    int should_close;
    unsigned char buffer[4096];
    long bytes_read;
    char target[1024];
    char resolved[1024];
    const char *lookup_path = path;
    FileTypeInfo info;
    PlatformDirEntry entry;
    int have_entry = 0;

    if (path != 0) {
        have_entry = (options->follow_symlinks ? platform_get_path_info_follow(path, &entry) : platform_get_path_info(path, &entry)) == 0;
    }

    if (path != 0 && !options->follow_symlinks && platform_read_symlink(path, target, sizeof(target)) == 0) {
        if (options->verbose) return write_verbose_node_result(path, "symbolic link", "inode/symlink", have_entry ? &entry : 0, target);
        if (options->mime_only) return write_single_line_result(path, options, "inode/symlink");
        rt_copy_string(dynamic_description, sizeof(dynamic_description), "symbolic link to ");
        dynamic_append(target);
        return write_single_line_result(path, options, dynamic_description);
    }

    if (path != 0 && options->follow_symlinks && tool_canonicalize_path(path, 1, 0, resolved, sizeof(resolved)) == 0) {
        lookup_path = resolved;
    }

    if (have_entry && entry.is_dir) {
        if (options->verbose) return write_verbose_node_result(path, "directory", "inode/directory", &entry, 0);
        return write_single_line_result(path, options, options->mime_only ? "inode/directory" : "directory");
    }

    if (tool_open_input(lookup_path, &fd, &should_close) != 0) {
        return -1;
    }

    bytes_read = platform_read(fd, buffer, sizeof(buffer));
    tool_close_input(fd, should_close);
    if (bytes_read < 0) {
        return -1;
    }

    info = detect_type(buffer, (size_t)bytes_read);
    if (options->verbose) return write_verbose_file_result(path, &info, have_entry ? &entry : 0, bytes_read);
    return write_single_line_result(path, options, options->mime_only ? info.mime : info.description);
}

static void print_usage(void) {
    tool_write_usage("file", "[-biv] [-L|-h] [file ...]");
}

int main(int argc, char **argv) {
    int exit_code = 0;
    FileOptions options;
    int argi = 1;
    int i;

    options.mime_only = 0;
    options.follow_symlinks = 0;
    options.brief = 0;
    options.verbose = 0;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-i") == 0 || rt_strcmp(argv[argi], "--mime") == 0 || rt_strcmp(argv[argi], "--mime-type") == 0) {
            options.mime_only = 1;
        } else if (rt_strcmp(argv[argi], "-b") == 0 || rt_strcmp(argv[argi], "--brief") == 0) {
            options.brief = 1;
        } else if (rt_strcmp(argv[argi], "-v") == 0 || rt_strcmp(argv[argi], "--verbose") == 0) {
            options.verbose = 1;
        } else if (rt_strcmp(argv[argi], "-L") == 0 || rt_strcmp(argv[argi], "--dereference") == 0) {
            options.follow_symlinks = 1;
        } else if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--no-dereference") == 0) {
            options.follow_symlinks = 0;
        } else {
            print_usage();
            return 1;
        }
        argi += 1;
    }

    if (argi >= argc) {
        return describe_path(NULL, &options) == 0 ? 0 : 1;
    }

    for (i = argi; i < argc; ++i) {
        if (describe_path(argv[i], &options) != 0) {
            rt_write_cstr(2, "file: cannot inspect ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }
    }

    return exit_code;
}
