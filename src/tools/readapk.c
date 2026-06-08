#include "archive_zip.h"
#include "archive_util.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define READAPK_MAX_ENTRY_SIZE 268435456ULL
#define READAPK_MAX_PRINTED_ITEMS 64U

typedef struct {
    int show_summary;
    int show_entries;
    int verify;
    int show_manifest;
    int show_resources;
    int show_dex;
    int show_native;
    int show_signatures;
    int json;
} ReadApkOptions;

typedef struct {
    unsigned long long manifest_count;
    unsigned long long dex_count;
    unsigned long long resources_count;
    unsigned long long native_library_count;
    unsigned long long asset_count;
    unsigned long long res_count;
    unsigned long long meta_inf_count;
    unsigned long long v1_signature_count;
    unsigned long long v1_signature_manifest_count;
    unsigned long long encrypted_count;
    unsigned long long data_descriptor_count;
    unsigned long long zip64_entry_count;
    unsigned long long deflated_count;
    unsigned long long stored_count;
    unsigned long long other_method_count;
} ReadApkStats;

typedef struct {
    const ReadApkOptions *options;
    ReadApkStats *stats;
    const char *path;
    int fd;
    const ArchiveZipInfo *info;
    const ArchiveZipSigningBlock *signing_block;
} ReadApkEntryContext;

static void print_usage(void) {
    tool_write_usage("readapk", "[-a] [-s] [-l] [--verify] [--manifest] [--resources] [--dex] [--native] [--signatures] [--json] FILE ...");
}

static int text_starts_with(const char *text, const char *prefix) {
    return rt_strncmp(text, prefix, rt_strlen(prefix)) == 0;
}

static char ascii_lower(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int text_ends_with_ignore_case(const char *text, const char *suffix) {
    size_t text_length = rt_strlen(text);
    size_t suffix_length = rt_strlen(suffix);
    size_t index;

    if (suffix_length > text_length) {
        return 0;
    }
    for (index = 0U; index < suffix_length; ++index) {
        if (ascii_lower(text[text_length - suffix_length + index]) != ascii_lower(suffix[index])) {
            return 0;
        }
    }
    return 1;
}

static unsigned int read_u16_le_at(const unsigned char *data, size_t size, size_t offset) {
    if (offset + 2U > size) return 0U;
    return archive_read_u16_le(data + offset);
}

static unsigned int read_u32_le_at(const unsigned char *data, size_t size, size_t offset) {
    if (offset + 4U > size) return 0U;
    return archive_read_u32_le(data + offset);
}

static unsigned long long read_u64_le_at(const unsigned char *data, size_t size, size_t offset) {
    if (offset + 8U > size) return 0ULL;
    return archive_read_u64_le(data + offset);
}

static const char *dex_version_text(const unsigned char *data, size_t size) {
    static char version[4];

    if (size < 8U || data[0] != 'd' || data[1] != 'e' || data[2] != 'x' || data[3] != '\n') {
        return "unknown";
    }
    version[0] = (char)data[4];
    version[1] = (char)data[5];
    version[2] = (char)data[6];
    version[3] = '\0';
    return version;
}

static const char *elf_machine_name(unsigned int machine) {
    switch (machine) {
        case 3U: return "x86";
        case 8U: return "mips";
        case 40U: return "arm";
        case 62U: return "x86-64";
        case 183U: return "aarch64";
        default: return "unknown";
    }
}

static const char *elf_class_name(unsigned int value) {
    if (value == 1U) return "ELF32";
    if (value == 2U) return "ELF64";
    return "ELF";
}

static const char *elf_type_name(unsigned int value) {
    if (value == 2U) return "executable";
    if (value == 3U) return "shared";
    if (value == 1U) return "relocatable";
    return "unknown";
}

static int entry_name_matches(const ArchiveZipEntry *entry, const char *name) {
    return rt_strcmp(entry->name, name) == 0;
}

static int is_probable_apk(const ReadApkStats *stats, const ArchiveZipSigningBlock *signing_block) {
    return stats->manifest_count > 0ULL || stats->dex_count > 0ULL || stats->resources_count > 0ULL ||
           stats->native_library_count > 0ULL || signing_block->present;
}

static void classify_entry(const ArchiveZipEntry *entry, ReadApkStats *stats) {
    const char *name = entry->name;

    if (rt_strcmp(name, "AndroidManifest.xml") == 0) {
        stats->manifest_count += 1ULL;
    }
    if (rt_strcmp(name, "resources.arsc") == 0) {
        stats->resources_count += 1ULL;
    }
    if (text_ends_with_ignore_case(name, ".dex")) {
        stats->dex_count += 1ULL;
    }
    if (text_starts_with(name, "lib/") && text_ends_with_ignore_case(name, ".so")) {
        stats->native_library_count += 1ULL;
    }
    if (text_starts_with(name, "assets/")) {
        stats->asset_count += 1ULL;
    }
    if (text_starts_with(name, "res/")) {
        stats->res_count += 1ULL;
    }
    if (text_starts_with(name, "META-INF/")) {
        stats->meta_inf_count += 1ULL;
        if (text_ends_with_ignore_case(name, ".RSA") || text_ends_with_ignore_case(name, ".DSA") || text_ends_with_ignore_case(name, ".EC")) {
            stats->v1_signature_count += 1ULL;
        }
        if (text_ends_with_ignore_case(name, ".SF")) {
            stats->v1_signature_manifest_count += 1ULL;
        }
    }
    if ((entry->flags & ARCHIVE_ZIP_FLAG_ENCRYPTED) != 0U) {
        stats->encrypted_count += 1ULL;
    }
    if ((entry->flags & ARCHIVE_ZIP_FLAG_DATA_DESCRIPTOR) != 0U) {
        stats->data_descriptor_count += 1ULL;
    }
    if (entry->zip64) {
        stats->zip64_entry_count += 1ULL;
    }
    if (entry->method == ARCHIVE_ZIP_METHOD_STORE) {
        stats->stored_count += 1ULL;
    } else if (entry->method == ARCHIVE_ZIP_METHOD_DEFLATE) {
        stats->deflated_count += 1ULL;
    } else {
        stats->other_method_count += 1ULL;
    }
}

static int write_bool_text(int fd, int value) {
    return rt_write_cstr(fd, value ? "yes" : "no");
}

static void format_hex32(unsigned int value, char *digits);

static int write_hex32(int fd, unsigned int value) {
    char digits[9];

    format_hex32(value, digits);
    return rt_write_cstr(fd, digits);
}

static void format_hex32(unsigned int value, char *digits) {
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        unsigned int nibble = (value >> ((7U - index) * 4U)) & 0xfU;
        digits[index] = (char)(nibble < 10U ? ('0' + nibble) : ('a' + nibble - 10U));
    }
    digits[8] = '\0';
}

static int write_hex_value(int fd, unsigned long long value) {
    char digits[17];
    unsigned int index;
    int started = 0;

    for (index = 0U; index < 16U; ++index) {
        unsigned int nibble = (unsigned int)((value >> ((15U - index) * 4U)) & 0xfULL);
        digits[index] = (char)(nibble < 10U ? ('0' + nibble) : ('a' + nibble - 10U));
    }
    if (rt_write_cstr(fd, "0x") != 0) return -1;
    for (index = 0U; index < 16U; ++index) {
        if (digits[index] != '0' || index == 15U) started = 1;
        if (started && rt_write_char(fd, digits[index]) != 0) return -1;
    }
    return 0;
}

static char *readapk_copy_text(const unsigned char *data, size_t length) {
    char *copy = (char *)rt_malloc(length + 1U);

    if (copy == 0) return 0;
    if (length != 0U) memcpy(copy, data, length);
    copy[length] = '\0';
    return copy;
}

typedef struct {
    char **items;
    size_t count;
} ReadApkStringPool;

static void string_pool_free(ReadApkStringPool *pool) {
    size_t index;

    for (index = 0U; index < pool->count; ++index) rt_free(pool->items[index]);
    rt_free(pool->items);
    pool->items = 0;
    pool->count = 0U;
}

static unsigned int read_length8(const unsigned char *data, size_t size, size_t *offset_io) {
    unsigned int value;

    if (*offset_io >= size) return 0U;
    value = data[*offset_io];
    *offset_io += 1U;
    if ((value & 0x80U) != 0U && *offset_io < size) {
        value = ((value & 0x7fU) << 8U) | data[*offset_io];
        *offset_io += 1U;
    }
    return value;
}

static unsigned int read_length16(const unsigned char *data, size_t size, size_t *offset_io) {
    unsigned int value;

    if (*offset_io + 2U > size) return 0U;
    value = archive_read_u16_le(data + *offset_io);
    *offset_io += 2U;
    if ((value & 0x8000U) != 0U && *offset_io + 2U <= size) {
        value = ((value & 0x7fffU) << 16U) | archive_read_u16_le(data + *offset_io);
        *offset_io += 2U;
    }
    return value;
}

static char *decode_android_string(const unsigned char *data, size_t size, int utf8) {
    size_t offset = 0U;
    size_t index;

    if (utf8) {
        unsigned int utf16_length = read_length8(data, size, &offset);
        unsigned int byte_length = read_length8(data, size, &offset);
        (void)utf16_length;
        if (offset + byte_length > size) return 0;
        return readapk_copy_text(data + offset, byte_length);
    }
    {
        unsigned int utf16_length = read_length16(data, size, &offset);
        char *text;

        if (offset + ((size_t)utf16_length * 2U) > size) return 0;
        text = (char *)rt_malloc((size_t)utf16_length + 1U);
        if (text == 0) return 0;
        for (index = 0U; index < utf16_length; ++index) {
            unsigned int unit = archive_read_u16_le(data + offset + index * 2U);
            text[index] = (char)(unit >= 32U && unit < 127U ? unit : '?');
        }
        text[utf16_length] = '\0';
        return text;
    }
}

static int parse_string_pool(const unsigned char *data, size_t size, size_t chunk_offset, ReadApkStringPool *pool) {
    unsigned int chunk_size;
    unsigned int string_count;
    unsigned int flags;
    unsigned int strings_start;
    unsigned int index;
    int utf8;

    rt_memset(pool, 0, sizeof(*pool));
    if (chunk_offset + 28U > size || read_u16_le_at(data, size, chunk_offset) != 0x0001U) return -1;
    chunk_size = read_u32_le_at(data, size, chunk_offset + 4U);
    if (chunk_size < 28U || chunk_offset + chunk_size > size) return -1;
    string_count = read_u32_le_at(data, size, chunk_offset + 8U);
    flags = read_u32_le_at(data, size, chunk_offset + 16U);
    strings_start = read_u32_le_at(data, size, chunk_offset + 20U);
    if (string_count > 65536U || 28U + string_count * 4U > chunk_size || strings_start >= chunk_size) return -1;
    pool->items = (char **)rt_malloc_array(string_count == 0U ? 1U : string_count, sizeof(*pool->items));
    if (pool->items == 0) return -1;
    pool->count = string_count;
    rt_memset(pool->items, 0, sizeof(*pool->items) * (string_count == 0U ? 1U : string_count));
    utf8 = (flags & 0x00000100U) != 0U;
    for (index = 0U; index < string_count; ++index) {
        unsigned int string_offset = read_u32_le_at(data, size, chunk_offset + 28U + index * 4U);
        size_t absolute = chunk_offset + strings_start + string_offset;

        pool->items[index] = absolute < chunk_offset + chunk_size ? decode_android_string(data + absolute, chunk_offset + chunk_size - absolute, utf8) : 0;
        if (pool->items[index] == 0) pool->items[index] = readapk_copy_text((const unsigned char *)"", 0U);
        if (pool->items[index] == 0) return -1;
    }
    return 0;
}

static const char *string_pool_get(const ReadApkStringPool *pool, unsigned int index) {
    if (index == 0xffffffffU || index >= pool->count || pool->items[index] == 0) return "";
    return pool->items[index];
}

static int find_attribute(const unsigned char *data, size_t size, size_t attr_base, unsigned int attr_size, unsigned int attr_count,
                          const ReadApkStringPool *pool, const char *name, unsigned int *type_out, unsigned int *value_out, const char **raw_out) {
    unsigned int index;

    for (index = 0U; index < attr_count; ++index) {
        size_t offset = attr_base + (size_t)index * attr_size;
        unsigned int name_index;
        unsigned int raw_index;

        if (offset + 20U > size) return 0;
        name_index = read_u32_le_at(data, size, offset + 4U);
        if (rt_strcmp(string_pool_get(pool, name_index), name) != 0) continue;
        raw_index = read_u32_le_at(data, size, offset + 8U);
        *type_out = read_u32_le_at(data, size, offset + 12U) >> 24U;
        *value_out = read_u32_le_at(data, size, offset + 16U);
        *raw_out = string_pool_get(pool, raw_index);
        return 1;
    }
    return 0;
}

static int write_typed_value(int fd, const ReadApkStringPool *pool, unsigned int type, unsigned int value) {
    if (type == 0x03U) return rt_write_cstr(fd, string_pool_get(pool, value));
    if (type == 0x12U) return rt_write_cstr(fd, value != 0U ? "true" : "false");
    if (type == 0x01U) return write_hex_value(fd, value);
    return rt_write_uint(fd, value);
}

static int print_manifest_attr(const unsigned char *data, size_t size, size_t attr_base, unsigned int attr_size, unsigned int attr_count,
                               const ReadApkStringPool *pool, const char *label, const char *name) {
    unsigned int type;
    unsigned int value;
    const char *raw;

    if (!find_attribute(data, size, attr_base, attr_size, attr_count, pool, name, &type, &value, &raw)) return 0;
    if (rt_write_cstr(1, "  ") != 0 || rt_write_cstr(1, label) != 0 || rt_write_cstr(1, ": ") != 0) return -1;
    if (raw[0] != '\0') {
        if (rt_write_cstr(1, raw) != 0) return -1;
    } else if (write_typed_value(1, pool, type, value) != 0) {
        return -1;
    }
    return rt_write_char(1, '\n');
}

static int inspect_manifest_data(const unsigned char *data, size_t size, const char *entry_name) {
    ReadApkStringPool pool;
    size_t offset;
    unsigned int printed_permissions = 0U;
    unsigned int printed_components = 0U;

    if (size < 8U || read_u16_le_at(data, size, 0U) != 0x0003U) {
        return rt_write_cstr(1, "Manifest: ") != 0 || rt_write_cstr(1, entry_name) != 0 || rt_write_line(1, " is not Android binary XML") != 0 ? -1 : 0;
    }
    rt_memset(&pool, 0, sizeof(pool));
    offset = 8U;
    while (offset + 8U <= size) {
        unsigned int type = read_u16_le_at(data, size, offset);
        unsigned int chunk_size = read_u32_le_at(data, size, offset + 4U);
        if (chunk_size < 8U || offset + chunk_size > size) break;
        if (type == 0x0001U && parse_string_pool(data, size, offset, &pool) == 0) break;
        offset += chunk_size;
    }
    if (pool.items == 0) return rt_write_line(1, "Manifest: string pool not found");
    if (rt_write_cstr(1, "Manifest: ") != 0 || rt_write_line(1, entry_name) != 0) {
        string_pool_free(&pool);
        return -1;
    }
    offset = 8U;
    while (offset + 8U <= size) {
        unsigned int type = read_u16_le_at(data, size, offset);
        unsigned int chunk_size = read_u32_le_at(data, size, offset + 4U);
        if (chunk_size < 8U || offset + chunk_size > size) break;
        if (type == 0x0102U && offset + 36U <= size) {
            const char *element = string_pool_get(&pool, read_u32_le_at(data, size, offset + 20U));
            unsigned int attr_start = read_u16_le_at(data, size, offset + 24U);
            unsigned int attr_size = read_u16_le_at(data, size, offset + 26U);
            unsigned int attr_count = read_u16_le_at(data, size, offset + 28U);
            size_t attr_base = offset + 16U + attr_start;

            if (attr_size < 20U) {
                offset += chunk_size;
                continue;
            }
            if (rt_strcmp(element, "manifest") == 0) {
                (void)print_manifest_attr(data, size, attr_base, attr_size, attr_count, &pool, "package", "package");
                (void)print_manifest_attr(data, size, attr_base, attr_size, attr_count, &pool, "versionCode", "versionCode");
                (void)print_manifest_attr(data, size, attr_base, attr_size, attr_count, &pool, "versionName", "versionName");
            } else if (rt_strcmp(element, "uses-sdk") == 0) {
                (void)print_manifest_attr(data, size, attr_base, attr_size, attr_count, &pool, "minSdkVersion", "minSdkVersion");
                (void)print_manifest_attr(data, size, attr_base, attr_size, attr_count, &pool, "targetSdkVersion", "targetSdkVersion");
            } else if (rt_strcmp(element, "application") == 0) {
                (void)print_manifest_attr(data, size, attr_base, attr_size, attr_count, &pool, "application label", "label");
                (void)print_manifest_attr(data, size, attr_base, attr_size, attr_count, &pool, "debuggable", "debuggable");
                (void)print_manifest_attr(data, size, attr_base, attr_size, attr_count, &pool, "allowBackup", "allowBackup");
                (void)print_manifest_attr(data, size, attr_base, attr_size, attr_count, &pool, "networkSecurityConfig", "networkSecurityConfig");
            } else if (rt_strcmp(element, "uses-permission") == 0 && printed_permissions < READAPK_MAX_PRINTED_ITEMS) {
                (void)print_manifest_attr(data, size, attr_base, attr_size, attr_count, &pool, "permission", "name");
                printed_permissions += 1U;
            } else if ((rt_strcmp(element, "activity") == 0 || rt_strcmp(element, "service") == 0 || rt_strcmp(element, "receiver") == 0 || rt_strcmp(element, "provider") == 0) && printed_components < READAPK_MAX_PRINTED_ITEMS) {
                unsigned int value_type;
                unsigned int value;
                const char *raw;
                if (find_attribute(data, size, attr_base, attr_size, attr_count, &pool, "name", &value_type, &value, &raw)) {
                    (void)value_type;
                    if (rt_write_cstr(1, "  ") != 0 || rt_write_cstr(1, element) != 0 || rt_write_cstr(1, ": ") != 0 || rt_write_line(1, raw[0] != '\0' ? raw : string_pool_get(&pool, value)) != 0) {
                        string_pool_free(&pool);
                        return -1;
                    }
                    printed_components += 1U;
                }
            }
        }
        offset += chunk_size;
    }
    string_pool_free(&pool);
    return 0;
}

static int print_resource_package_name(const unsigned char *data, size_t size, size_t offset) {
    char name[129];
    size_t index;

    for (index = 0U; index < 128U && offset + 12U + index * 2U + 2U <= size; ++index) {
        unsigned int unit = archive_read_u16_le(data + offset + 12U + index * 2U);
        if (unit == 0U) break;
        name[index] = (char)(unit >= 32U && unit < 127U ? unit : '?');
    }
    name[index] = '\0';
    return rt_write_cstr(1, name);
}

static int inspect_resources_data(const unsigned char *data, size_t size, const char *entry_name) {
    size_t offset = 12U;
    unsigned int package_count;
    unsigned int string_pool_count = 0U;
    unsigned int package_chunks = 0U;
    unsigned int type_spec_chunks = 0U;
    unsigned int type_chunks = 0U;

    if (size < 12U || read_u16_le_at(data, size, 0U) != 0x0002U) return rt_write_line(1, "resources.arsc: not an Android resource table");
    package_count = read_u32_le_at(data, size, 8U);
    if (rt_write_cstr(1, "Resources: ") != 0 || rt_write_line(1, entry_name) != 0) return -1;
    if (rt_write_cstr(1, "  packages: ") != 0 || rt_write_uint(1, package_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    while (offset + 8U <= size) {
        unsigned int type = read_u16_le_at(data, size, offset);
        unsigned int chunk_size = read_u32_le_at(data, size, offset + 4U);

        if (chunk_size < 8U || offset + chunk_size > size) break;
        if (type == 0x0001U) string_pool_count += 1U;
        else if (type == 0x0200U) {
            size_t child_offset = offset + read_u16_le_at(data, size, offset + 2U);
            package_chunks += 1U;
            if (rt_write_cstr(1, "  package ") != 0 || rt_write_uint(1, read_u32_le_at(data, size, offset + 8U)) != 0 || rt_write_cstr(1, ": ") != 0 || print_resource_package_name(data, size, offset) != 0 || rt_write_char(1, '\n') != 0) return -1;
            while (child_offset + 8U <= offset + chunk_size) {
                unsigned int child_type = read_u16_le_at(data, size, child_offset);
                unsigned int child_size = read_u32_le_at(data, size, child_offset + 4U);
                if (child_size < 8U || child_offset + child_size > offset + chunk_size) break;
                if (child_type == 0x0001U) string_pool_count += 1U;
                else if (child_type == 0x0202U) type_spec_chunks += 1U;
                else if (child_type == 0x0201U) type_chunks += 1U;
                child_offset += child_size;
            }
        } else if (type == 0x0202U) type_spec_chunks += 1U;
        else if (type == 0x0201U) type_chunks += 1U;
        offset += chunk_size;
    }
    if (rt_write_cstr(1, "  string pools: ") != 0 || rt_write_uint(1, string_pool_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  package chunks: ") != 0 || rt_write_uint(1, package_chunks) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  type specs: ") != 0 || rt_write_uint(1, type_spec_chunks) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  type configs: ") != 0 || rt_write_uint(1, type_chunks) != 0 || rt_write_char(1, '\n') != 0) return -1;
    return 0;
}

static unsigned int read_uleb128(const unsigned char *data, size_t size, size_t *offset_io) {
    unsigned int result = 0U;
    unsigned int shift = 0U;

    while (*offset_io < size && shift < 32U) {
        unsigned int byte = data[*offset_io];
        *offset_io += 1U;
        result |= (byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0U) break;
        shift += 7U;
    }
    return result;
}

static const char *dex_string_at(const unsigned char *data, size_t size, unsigned int string_ids_off, unsigned int string_count, unsigned int index) {
    static char empty[1] = { '\0' };
    size_t offset;

    if (index >= string_count || string_ids_off + index * 4U + 4U > size) return empty;
    offset = read_u32_le_at(data, size, string_ids_off + index * 4U);
    if (offset >= size) return empty;
    (void)read_uleb128(data, size, &offset);
    if (offset >= size) return empty;
    return (const char *)(data + offset);
}

static int inspect_dex_data(const unsigned char *data, size_t size, const char *entry_name) {
    unsigned int string_count;
    unsigned int string_off;
    unsigned int type_count;
    unsigned int type_off;
    unsigned int proto_count;
    unsigned int field_count;
    unsigned int method_count;
    unsigned int class_count;
    unsigned int class_off;
    unsigned int index;

    if (size < 112U || memcmp(data, "dex\n", 4U) != 0) return rt_write_cstr(1, "DEX: ") != 0 || rt_write_cstr(1, entry_name) != 0 || rt_write_line(1, " is not a DEX file") != 0 ? -1 : 0;
    string_count = read_u32_le_at(data, size, 56U);
    string_off = read_u32_le_at(data, size, 60U);
    type_count = read_u32_le_at(data, size, 64U);
    type_off = read_u32_le_at(data, size, 68U);
    proto_count = read_u32_le_at(data, size, 72U);
    field_count = read_u32_le_at(data, size, 80U);
    method_count = read_u32_le_at(data, size, 88U);
    class_count = read_u32_le_at(data, size, 96U);
    class_off = read_u32_le_at(data, size, 100U);
    if (rt_write_cstr(1, "DEX: ") != 0 || rt_write_line(1, entry_name) != 0) return -1;
    if (rt_write_cstr(1, "  version: ") != 0 || rt_write_cstr(1, dex_version_text(data, size)) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  file size: ") != 0 || rt_write_uint(1, read_u32_le_at(data, size, 32U)) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  strings/types/protos: ") != 0 || rt_write_uint(1, string_count) != 0 || rt_write_cstr(1, "/") != 0 || rt_write_uint(1, type_count) != 0 || rt_write_cstr(1, "/") != 0 || rt_write_uint(1, proto_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  fields/methods/classes: ") != 0 || rt_write_uint(1, field_count) != 0 || rt_write_cstr(1, "/") != 0 || rt_write_uint(1, method_count) != 0 || rt_write_cstr(1, "/") != 0 || rt_write_uint(1, class_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    for (index = 0U; index < class_count && index < 20U; ++index) {
        unsigned int class_idx;
        unsigned int descriptor_idx;

        if (class_off + index * 32U + 4U > size) break;
        class_idx = read_u32_le_at(data, size, class_off + index * 32U);
        if (type_off + class_idx * 4U + 4U > size) continue;
        descriptor_idx = read_u32_le_at(data, size, type_off + class_idx * 4U);
        if (rt_write_cstr(1, "  class: ") != 0 || rt_write_line(1, dex_string_at(data, size, string_off, string_count, descriptor_idx)) != 0) return -1;
    }
    return 0;
}

static const char *abi_from_path(const char *name) {
    if (text_starts_with(name, "lib/arm64-v8a/")) return "arm64-v8a";
    if (text_starts_with(name, "lib/armeabi-v7a/")) return "armeabi-v7a";
    if (text_starts_with(name, "lib/x86_64/")) return "x86_64";
    if (text_starts_with(name, "lib/x86/")) return "x86";
    return "unknown";
}

static int inspect_native_data(const unsigned char *data, size_t size, const char *entry_name) {
    unsigned int elf_class;
    unsigned int machine;
    unsigned int type;
    unsigned long long entry;
    unsigned int phnum;
    unsigned int shnum;

    if (size < 52U || data[0] != 0x7fU || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') return rt_write_cstr(1, "Native library: ") != 0 || rt_write_cstr(1, entry_name) != 0 || rt_write_line(1, " is not ELF") != 0 ? -1 : 0;
    elf_class = data[4];
    type = read_u16_le_at(data, size, 16U);
    machine = read_u16_le_at(data, size, 18U);
    entry = elf_class == 2U ? read_u64_le_at(data, size, 24U) : read_u32_le_at(data, size, 24U);
    phnum = read_u16_le_at(data, size, elf_class == 2U ? 56U : 44U);
    shnum = read_u16_le_at(data, size, elf_class == 2U ? 60U : 48U);
    if (rt_write_cstr(1, "Native library: ") != 0 || rt_write_line(1, entry_name) != 0) return -1;
    if (rt_write_cstr(1, "  ABI: ") != 0 || rt_write_line(1, abi_from_path(entry_name)) != 0) return -1;
    if (rt_write_cstr(1, "  ELF: ") != 0 || rt_write_cstr(1, elf_class_name(elf_class)) != 0 || rt_write_cstr(1, " ") != 0 || rt_write_line(1, elf_machine_name(machine)) != 0) return -1;
    if (rt_write_cstr(1, "  type: ") != 0 || rt_write_cstr(1, elf_type_name(type)) != 0 || rt_write_cstr(1, ", entry: ") != 0 || write_hex_value(1, entry) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  program headers: ") != 0 || rt_write_uint(1, phnum) != 0 || rt_write_cstr(1, ", sections: ") != 0 || rt_write_uint(1, shnum) != 0 || rt_write_char(1, '\n') != 0) return -1;
    return 0;
}

static int write_entry_flags(int fd, const ArchiveZipEntry *entry) {
    if (rt_write_char(fd, (entry->flags & ARCHIVE_ZIP_FLAG_ENCRYPTED) != 0U ? 'E' : '-') != 0) return -1;
    if (rt_write_char(fd, (entry->flags & ARCHIVE_ZIP_FLAG_DATA_DESCRIPTOR) != 0U ? 'D' : '-') != 0) return -1;
    if (rt_write_char(fd, (entry->flags & ARCHIVE_ZIP_FLAG_UTF8) != 0U ? 'U' : '-') != 0) return -1;
    if (rt_write_char(fd, entry->zip64 ? 'Z' : '-') != 0) return -1;
    return 0;
}

static int write_json_summary(const char *path, const ArchiveZipInfo *info, const ArchiveZipSigningBlock *signing_block, const ReadApkStats *stats) {
    if (tool_json_begin_event(1, "readapk", "stdout", "apk_summary") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{\"file\":") != 0) return -1;
    if (tool_json_write_string(1, path) != 0) return -1;
    if (rt_write_cstr(1, ",\"probable_apk\":") != 0) return -1;
    if (rt_write_cstr(1, is_probable_apk(stats, signing_block) ? "true" : "false") != 0) return -1;
    if (rt_write_cstr(1, ",\"entries\":") != 0) return -1;
    if (rt_write_uint(1, info->entry_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"central_directory_offset\":") != 0) return -1;
    if (rt_write_uint(1, info->central_directory_offset) != 0) return -1;
    if (rt_write_cstr(1, ",\"central_directory_size\":") != 0) return -1;
    if (rt_write_uint(1, info->central_directory_size) != 0) return -1;
    if (rt_write_cstr(1, ",\"zip64\":") != 0) return -1;
    if (rt_write_cstr(1, info->zip64 ? "true" : "false") != 0) return -1;
    if (rt_write_cstr(1, ",\"manifest\":") != 0) return -1;
    if (rt_write_uint(1, stats->manifest_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"dex_files\":") != 0) return -1;
    if (rt_write_uint(1, stats->dex_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"resources_arsc\":") != 0) return -1;
    if (rt_write_uint(1, stats->resources_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"native_libraries\":") != 0) return -1;
    if (rt_write_uint(1, stats->native_library_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"assets\":") != 0) return -1;
    if (rt_write_uint(1, stats->asset_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"res_files\":") != 0) return -1;
    if (rt_write_uint(1, stats->res_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"v1_signature_files\":") != 0) return -1;
    if (rt_write_uint(1, stats->v1_signature_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"apk_signing_block\":") != 0) return -1;
    if (rt_write_cstr(1, signing_block->present ? "true" : "false") != 0) return -1;
    if (rt_write_cstr(1, ",\"apk_signature_v2\":") != 0) return -1;
    if (rt_write_cstr(1, signing_block->v2 ? "true" : "false") != 0) return -1;
    if (rt_write_cstr(1, ",\"apk_signature_v3\":") != 0) return -1;
    if (rt_write_cstr(1, signing_block->v3 ? "true" : "false") != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int write_json_entry(const char *path, const ArchiveZipEntry *entry) {
    char crc_text[9];

    format_hex32(entry->crc32, crc_text);
    if (tool_json_begin_event(1, "readapk", "stdout", "apk_entry") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{\"file\":") != 0) return -1;
    if (tool_json_write_string(1, path) != 0) return -1;
    if (rt_write_cstr(1, ",\"name\":") != 0) return -1;
    if (tool_json_write_string(1, entry->name) != 0) return -1;
    if (rt_write_cstr(1, ",\"method\":") != 0) return -1;
    if (tool_json_write_string(1, archive_zip_method_name(entry->method)) != 0) return -1;
    if (rt_write_cstr(1, ",\"method_id\":") != 0) return -1;
    if (rt_write_uint(1, entry->method) != 0) return -1;
    if (rt_write_cstr(1, ",\"compressed_size\":") != 0) return -1;
    if (rt_write_uint(1, entry->compressed_size) != 0) return -1;
    if (rt_write_cstr(1, ",\"uncompressed_size\":") != 0) return -1;
    if (rt_write_uint(1, entry->uncompressed_size) != 0) return -1;
    if (rt_write_cstr(1, ",\"crc32\":") != 0) return -1;
    if (tool_json_write_string(1, crc_text) != 0) return -1;
    if (rt_write_cstr(1, ",\"local_header_offset\":") != 0) return -1;
    if (rt_write_uint(1, entry->local_header_offset) != 0) return -1;
    if (rt_write_cstr(1, ",\"encrypted\":") != 0) return -1;
    if (rt_write_cstr(1, (entry->flags & ARCHIVE_ZIP_FLAG_ENCRYPTED) != 0U ? "true" : "false") != 0) return -1;
    if (rt_write_cstr(1, ",\"utf8\":") != 0) return -1;
    if (rt_write_cstr(1, (entry->flags & ARCHIVE_ZIP_FLAG_UTF8) != 0U ? "true" : "false") != 0) return -1;
    if (rt_write_cstr(1, ",\"zip64\":") != 0) return -1;
    if (rt_write_cstr(1, entry->zip64 ? "true" : "false") != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int print_summary(const char *path, const ArchiveZipInfo *info, const ArchiveZipSigningBlock *signing_block, const ReadApkStats *stats) {
    if (rt_write_cstr(1, "File: ") != 0 || rt_write_line(1, path) != 0) return -1;
    if (rt_write_cstr(1, "Type: ") != 0 || rt_write_line(1, is_probable_apk(stats, signing_block) ? "Android APK / ZIP archive" : "ZIP archive (APK features not detected)") != 0) return -1;
    if (rt_write_cstr(1, "Entries: ") != 0 || rt_write_uint(1, info->entry_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "Central directory: offset ") != 0 || rt_write_uint(1, info->central_directory_offset) != 0) return -1;
    if (rt_write_cstr(1, ", size ") != 0 || rt_write_uint(1, info->central_directory_size) != 0) return -1;
    if (rt_write_cstr(1, ", comment ") != 0 || rt_write_uint(1, info->comment_length) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "Zip64: ") != 0 || write_bool_text(1, info->zip64) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "Multi-disk: ") != 0 || write_bool_text(1, info->multi_disk) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "AndroidManifest.xml: ") != 0 || rt_write_uint(1, stats->manifest_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "resources.arsc: ") != 0 || rt_write_uint(1, stats->resources_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "DEX files: ") != 0 || rt_write_uint(1, stats->dex_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "Native libraries: ") != 0 || rt_write_uint(1, stats->native_library_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "res/ files: ") != 0 || rt_write_uint(1, stats->res_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "assets/ files: ") != 0 || rt_write_uint(1, stats->asset_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "META-INF files: ") != 0 || rt_write_uint(1, stats->meta_inf_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "APK v1 signature files: ") != 0 || rt_write_uint(1, stats->v1_signature_count) != 0) return -1;
    if (rt_write_cstr(1, " cert, ") != 0 || rt_write_uint(1, stats->v1_signature_manifest_count) != 0 || rt_write_line(1, " manifest") != 0) return -1;
    if (rt_write_cstr(1, "APK Signing Block: ") != 0 || write_bool_text(1, signing_block->present) != 0) return -1;
    if (signing_block->present) {
        if (rt_write_cstr(1, " offset ") != 0 || rt_write_uint(1, signing_block->offset) != 0) return -1;
        if (rt_write_cstr(1, ", size ") != 0 || rt_write_uint(1, signing_block->size) != 0) return -1;
        if (rt_write_cstr(1, ", v2 ") != 0 || write_bool_text(1, signing_block->v2) != 0) return -1;
        if (rt_write_cstr(1, ", v3 ") != 0 || write_bool_text(1, signing_block->v3) != 0) return -1;
        if (rt_write_cstr(1, ", v3.1 ") != 0 || write_bool_text(1, signing_block->v31) != 0) return -1;
    }
    if (rt_write_char(1, '\n') != 0) return -1;
    if (stats->encrypted_count > 0ULL || stats->data_descriptor_count > 0ULL || stats->zip64_entry_count > 0ULL) {
        if (rt_write_cstr(1, "ZIP flags: encrypted ") != 0 || rt_write_uint(1, stats->encrypted_count) != 0) return -1;
        if (rt_write_cstr(1, ", data-descriptor ") != 0 || rt_write_uint(1, stats->data_descriptor_count) != 0) return -1;
        if (rt_write_cstr(1, ", zip64 entries ") != 0 || rt_write_uint(1, stats->zip64_entry_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    }
    return 0;
}

static int print_entry(const ArchiveZipEntry *entry, void *user_data) {
    ReadApkEntryContext *context = (ReadApkEntryContext *)user_data;
    unsigned char *data = 0;
    size_t data_size = 0U;
    int should_read_payload = 0;
    int payload_result = 0;

    classify_entry(entry, context->stats);
    if (context->options->show_manifest && entry_name_matches(entry, "AndroidManifest.xml")) should_read_payload = 1;
    if (context->options->show_resources && entry_name_matches(entry, "resources.arsc")) should_read_payload = 1;
    if (context->options->show_dex && text_ends_with_ignore_case(entry->name, ".dex")) should_read_payload = 1;
    if (context->options->show_native && text_starts_with(entry->name, "lib/") && text_ends_with_ignore_case(entry->name, ".so")) should_read_payload = 1;
    if (should_read_payload && !context->options->json) {
        if (archive_zip_read_entry_data(context->fd, context->info, entry, READAPK_MAX_ENTRY_SIZE, &data, &data_size) != 0) {
            if (rt_write_cstr(1, "Cannot read entry payload: ") != 0 || rt_write_line(1, entry->name) != 0) return -1;
        } else {
            if (context->options->show_manifest && entry_name_matches(entry, "AndroidManifest.xml")) payload_result = inspect_manifest_data(data, data_size, entry->name);
            if (payload_result == 0 && context->options->show_resources && entry_name_matches(entry, "resources.arsc")) payload_result = inspect_resources_data(data, data_size, entry->name);
            if (payload_result == 0 && context->options->show_dex && text_ends_with_ignore_case(entry->name, ".dex")) payload_result = inspect_dex_data(data, data_size, entry->name);
            if (payload_result == 0 && context->options->show_native && text_starts_with(entry->name, "lib/") && text_ends_with_ignore_case(entry->name, ".so")) payload_result = inspect_native_data(data, data_size, entry->name);
            rt_free(data);
            if (payload_result != 0) return payload_result;
        }
    }
    if (context->options->json && context->options->show_entries) {
        return write_json_entry(context->path, entry);
    }
    if (!context->options->show_entries || context->options->json) {
        return 0;
    }
    if (rt_write_uint(1, entry->local_header_offset) != 0 || rt_write_char(1, '\t') != 0) return -1;
    if (rt_write_cstr(1, archive_zip_method_name(entry->method)) != 0 || rt_write_char(1, '\t') != 0) return -1;
    if (rt_write_uint(1, entry->compressed_size) != 0 || rt_write_char(1, '\t') != 0) return -1;
    if (rt_write_uint(1, entry->uncompressed_size) != 0 || rt_write_char(1, '\t') != 0) return -1;
    if (write_hex32(1, entry->crc32) != 0 || rt_write_char(1, '\t') != 0) return -1;
    if (write_entry_flags(1, entry) != 0 || rt_write_char(1, '\t') != 0) return -1;
    return rt_write_line(1, entry->name);
}

static int print_validation(const ArchiveZipValidation *validation) {
    unsigned long long errors = validation->local_header_errors + validation->range_errors + validation->name_mismatch_errors +
                                validation->method_errors + validation->crc_errors;

    if (rt_write_line(1, "Validation:") != 0) return -1;
    if (rt_write_cstr(1, "  checked entries: ") != 0 || rt_write_uint(1, validation->checked_entries) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  structural errors: ") != 0 || rt_write_uint(1, errors) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  local header errors: ") != 0 || rt_write_uint(1, validation->local_header_errors) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  range errors: ") != 0 || rt_write_uint(1, validation->range_errors) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  name mismatches: ") != 0 || rt_write_uint(1, validation->name_mismatch_errors) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  method mismatches: ") != 0 || rt_write_uint(1, validation->method_errors) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  CRC/decode errors: ") != 0 || rt_write_uint(1, validation->crc_errors) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  duplicate names: ") != 0 || rt_write_uint(1, validation->duplicate_names) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  suspicious names: ") != 0 || rt_write_uint(1, validation->suspicious_names) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  unsupported methods: ") != 0 || rt_write_uint(1, validation->unsupported_methods) != 0 || rt_write_char(1, '\n') != 0) return -1;
    return 0;
}

static int validation_has_errors(const ArchiveZipValidation *validation) {
    return validation->local_header_errors != 0ULL || validation->range_errors != 0ULL || validation->name_mismatch_errors != 0ULL ||
           validation->method_errors != 0ULL || validation->crc_errors != 0ULL || validation->suspicious_names != 0ULL || validation->unsupported_methods != 0ULL;
}

static int print_signature_details(const ArchiveZipSigningBlock *signing_block, const ReadApkStats *stats) {
    if (rt_write_line(1, "Signatures:") != 0) return -1;
    if (rt_write_cstr(1, "  v1/JAR cert files: ") != 0 || rt_write_uint(1, stats->v1_signature_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  v1/JAR .SF files: ") != 0 || rt_write_uint(1, stats->v1_signature_manifest_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "  APK Signing Block: ") != 0 || write_bool_text(1, signing_block->present) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (signing_block->present) {
        if (rt_write_cstr(1, "  block offset: ") != 0 || rt_write_uint(1, signing_block->offset) != 0 || rt_write_char(1, '\n') != 0) return -1;
        if (rt_write_cstr(1, "  block size: ") != 0 || rt_write_uint(1, signing_block->size) != 0 || rt_write_char(1, '\n') != 0) return -1;
        if (rt_write_cstr(1, "  v2/v3/v3.1/source-stamp: ") != 0 || write_bool_text(1, signing_block->v2) != 0 || rt_write_char(1, '/') != 0 || write_bool_text(1, signing_block->v3) != 0 || rt_write_char(1, '/') != 0 || write_bool_text(1, signing_block->v31) != 0 || rt_write_char(1, '/') != 0 || write_bool_text(1, signing_block->source_stamp) != 0 || rt_write_char(1, '\n') != 0) return -1;
        if (rt_write_cstr(1, "  v2 signers/signatures: ") != 0 || rt_write_uint(1, signing_block->v2_signer_count) != 0 || rt_write_char(1, '/') != 0 || rt_write_uint(1, signing_block->v2_signature_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
        if (rt_write_cstr(1, "  v3 signers/signatures: ") != 0 || rt_write_uint(1, signing_block->v3_signer_count) != 0 || rt_write_char(1, '/') != 0 || rt_write_uint(1, signing_block->v3_signature_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
        if (rt_write_cstr(1, "  signer certificates: ") != 0 || rt_write_uint(1, signing_block->certificate_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    }
    return rt_write_line(1, "  cryptographic verification: not implemented");
}

static int inspect_apk(const char *path, const ReadApkOptions *options) {
    int fd = platform_open_read(path);
    ArchiveZipInfo info;
    ArchiveZipSigningBlock signing_block;
    ReadApkStats stats;
    ReadApkEntryContext context;
    int result;

    if (fd < 0) {
        tool_write_error("readapk", "cannot open: ", path);
        return 1;
    }
    if (archive_zip_read_info(fd, &info) != 0) {
        platform_close(fd);
        tool_write_error("readapk", "not a readable ZIP/APK: ", path);
        return 1;
    }
    if (archive_zip_read_signing_block(fd, &info, &signing_block) != 0) {
        platform_close(fd);
        tool_write_error("readapk", "invalid APK Signing Block: ", path);
        return 1;
    }

    rt_memset(&stats, 0, sizeof(stats));
    context.options = options;
    context.stats = &stats;
    context.path = path;
    context.fd = fd;
    context.info = &info;
    context.signing_block = &signing_block;

    if (options->verify && !options->json) {
        ArchiveZipValidation validation;

        if (archive_zip_validate(fd, &info, &validation) != 0) {
            platform_close(fd);
            tool_write_error("readapk", "cannot validate ZIP/APK: ", path);
            return 1;
        }
        if (print_validation(&validation) != 0) {
            platform_close(fd);
            return 1;
        }
        if (validation_has_errors(&validation)) {
            result = 1;
        } else {
            result = 0;
        }
    } else {
        result = 0;
    }

    if (options->show_entries && !options->json) {
        if (options->verify && rt_write_char(1, '\n') != 0) {
            platform_close(fd);
            return 1;
        }
        if (rt_write_line(1, "offset\tmethod\tcompressed\tuncompressed\tcrc32\tflags\tname") != 0) {
            platform_close(fd);
            return 1;
        }
    }
    if (archive_zip_iterate_entries(fd, &info, print_entry, &context) != 0) {
        platform_close(fd);
        tool_write_error("readapk", "cannot read central directory: ", path);
        return 1;
    }
    platform_close(fd);

    if (options->show_signatures && !options->json) {
        if ((options->show_summary || options->show_entries || options->show_manifest || options->show_resources || options->show_dex || options->show_native || options->verify) && rt_write_char(1, '\n') != 0) return 1;
        if (print_signature_details(&signing_block, &stats) != 0) return 1;
    }

    if (options->json) {
        if (options->show_summary && write_json_summary(path, &info, &signing_block, &stats) != 0) {
            return 1;
        }
    } else if (options->show_summary) {
        if ((options->show_entries || options->show_manifest || options->show_resources || options->show_dex || options->show_native || options->verify || options->show_signatures) && rt_write_char(1, '\n') != 0) return 1;
        if (print_summary(path, &info, &signing_block, &stats) != 0) return 1;
    }
    return result;
}

int main(int argc, char **argv) {
    ReadApkOptions options;
    ToolOptState opt;
    int opt_result;
    int exit_status = 0;
    int index;

    rt_memset(&options, 0, sizeof(options));
    tool_opt_init(&opt, argc, argv, "readapk", "[-a] [-s] [-l] [--verify] [--manifest] [--resources] [--dex] [--native] [--signatures] [--json] FILE ...");
    while ((opt_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-a") == 0 || rt_strcmp(opt.flag, "--all") == 0) {
            options.show_summary = 1;
            options.show_entries = 1;
        } else if (rt_strcmp(opt.flag, "-s") == 0 || rt_strcmp(opt.flag, "--summary") == 0) {
            options.show_summary = 1;
        } else if (rt_strcmp(opt.flag, "-l") == 0 || rt_strcmp(opt.flag, "--list") == 0) {
            options.show_entries = 1;
        } else if (rt_strcmp(opt.flag, "--verify") == 0) {
            options.verify = 1;
        } else if (rt_strcmp(opt.flag, "--manifest") == 0) {
            options.show_manifest = 1;
        } else if (rt_strcmp(opt.flag, "--resources") == 0) {
            options.show_resources = 1;
        } else if (rt_strcmp(opt.flag, "--dex") == 0) {
            options.show_dex = 1;
        } else if (rt_strcmp(opt.flag, "--native") == 0) {
            options.show_native = 1;
        } else if (rt_strcmp(opt.flag, "--signatures") == 0) {
            options.show_signatures = 1;
        } else if (rt_strcmp(opt.flag, "--json") == 0) {
            options.json = 1;
        } else {
            tool_write_error("readapk", "unknown option: ", opt.flag);
            print_usage();
            return 1;
        }
    }
    if (opt_result == TOOL_OPT_HELP) {
        print_usage();
        return 0;
    }
    if (opt_result == TOOL_OPT_ERROR) {
        return 1;
    }
    options.json = tool_json_is_enabled();
    if (!options.show_summary && !options.show_entries && !options.verify && !options.show_manifest && !options.show_resources && !options.show_dex && !options.show_native && !options.show_signatures) {
        options.show_summary = 1;
    }
    if (opt.argi >= argc) {
        print_usage();
        return 1;
    }

    for (index = opt.argi; index < argc; ++index) {
        if (index > opt.argi && !options.json && rt_write_char(1, '\n') != 0) {
            return 1;
        }
        if (inspect_apk(argv[index], &options) != 0) {
            exit_status = 1;
        }
    }
    return exit_status;
}