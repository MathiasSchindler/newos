#include "linker.h"

#include "platform.h"
#include "runtime.h"
#include "source.h"

#define LINKER_MAX_OBJECTS 320
#define LINKER_MAX_OBJECT_SIZE (1024U * 1024U)
#define LINKER_MAX_ARCHIVE_SIZE (64U * 1024U * 1024U)
#define LINKER_MAX_OUTPUT (64U * 1024U * 1024U)
#define LINKER_MAX_MEMORY (128U * 1024U * 1024U)
#define LINKER_MAX_GLOBALS 8192
#define LINKER_BASE_VADDR 0x400000ULL
#define LINKER_AR_HEADER_SIZE 60U

#define ELFCLASS64 2U
#define ELFDATA2LSB 1U
#define EV_CURRENT 1U
#define ET_EXEC 2U
#define ET_REL 1U
#define EM_X86_64 62U
#define PT_LOAD 1U
#define PF_X 1U
#define PF_W 2U
#define PF_R 4U
#define SHT_SYMTAB 2U
#define SHT_STRTAB 3U
#define SHT_RELA 4U
#define SHN_UNDEF 0U
#define STB_GLOBAL 1U
#define R_X86_64_64 1U
#define R_X86_64_PC32 2U
#define R_X86_64_PLT32 4U

#define ELF64_EHDR_SIZE 64U
#define ELF64_PHDR_SIZE 56U
#define ELF64_SHDR_SIZE 64U
#define ELF64_SYM_SIZE 24U
#define ELF64_RELA_SIZE 24U

typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef int int32_t;

typedef struct {
    char path[COMPILER_PATH_CAPACITY];
    unsigned char *file;
    size_t size;
    uint64_t shoff;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
    uint16_t text_index;
    uint16_t data_index;
    uint16_t bss_index;
    uint16_t symtab_index;
    uint16_t strtab_index;
    uint16_t rela_text_index;
    uint16_t rela_data_index;
    uint64_t text_offset;
    uint64_t text_size;
    uint64_t data_offset;
    uint64_t data_size;
    uint64_t bss_size;
    uint64_t symtab_offset;
    uint64_t symtab_size;
    uint64_t symtab_entsize;
    uint64_t strtab_offset;
    uint64_t strtab_size;
    uint64_t rela_text_offset;
    uint64_t rela_text_size;
    uint64_t rela_text_entsize;
    uint64_t rela_data_offset;
    uint64_t rela_data_size;
    uint64_t rela_data_entsize;
    uint64_t out_text_offset;
    uint64_t out_data_offset;
    uint64_t out_bss_offset;
    int live;
} LinkObject;

typedef struct {
    char name[COMPILER_PATH_CAPACITY];
    uint64_t value;
} LinkGlobal;

typedef struct {
    char name[COMPILER_PATH_CAPACITY];
    size_t object_index;
} LinkDefinedSymbol;

static LinkObject linker_objects[LINKER_MAX_OBJECTS];
static LinkGlobal linker_globals[LINKER_MAX_GLOBALS];
static LinkDefinedSymbol linker_defined_symbols[LINKER_MAX_GLOBALS];
static size_t linker_global_count;
static size_t linker_defined_symbol_count;

static void set_link_error(char *error_out, size_t error_size, const char *message, const char *detail) {
    if (error_out == 0 || error_size == 0U) {
        return;
    }
    rt_copy_string(error_out, error_size, message != 0 ? message : "linker error");
    if (detail != 0 && detail[0] != '\0') {
        size_t used = rt_strlen(error_out);
        if (used + 3U < error_size) {
            rt_copy_string(error_out + used, error_size - used, ": ");
            used = rt_strlen(error_out);
            rt_copy_string(error_out + used, error_size - used, detail);
        }
    }
}

static uint16_t read_u16(const unsigned char *p) {
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8U);
}

static uint32_t read_u32(const unsigned char *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) |
           ((uint32_t)p[3] << 24U);
}

static uint64_t read_u64(const unsigned char *p) {
    uint64_t value = 0;
    unsigned int i;

    for (i = 0; i < 8U; ++i) {
        value |= ((uint64_t)p[i]) << (8U * i);
    }
    return value;
}

static int64_t read_i64(const unsigned char *p) {
    return (int64_t)read_u64(p);
}

static void write_u16(unsigned char *p, uint16_t value) {
    p[0] = (unsigned char)(value & 0xffU);
    p[1] = (unsigned char)((value >> 8U) & 0xffU);
}

static void write_u32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xffU);
    p[1] = (unsigned char)((value >> 8U) & 0xffU);
    p[2] = (unsigned char)((value >> 16U) & 0xffU);
    p[3] = (unsigned char)((value >> 24U) & 0xffU);
}

static void write_u64(unsigned char *p, uint64_t value) {
    unsigned int i;

    for (i = 0; i < 8U; ++i) {
        p[i] = (unsigned char)((value >> (8U * i)) & 0xffU);
    }
}

static uint64_t align_u64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static int range_valid(uint64_t offset, uint64_t size, uint64_t limit) {
    return offset <= limit && size <= limit - offset;
}

static const unsigned char *section_header(const LinkObject *object, uint16_t index) {
    uint64_t offset;

    if (index >= object->shnum || object->shentsize < ELF64_SHDR_SIZE) {
        return 0;
    }
    offset = object->shoff + ((uint64_t)index * object->shentsize);
    if (!range_valid(offset, ELF64_SHDR_SIZE, object->size)) {
        return 0;
    }
    return object->file + offset;
}

static const char *section_name(const LinkObject *object, uint16_t index) {
    const unsigned char *section = section_header(object, index);
    const unsigned char *shstr = section_header(object, object->shstrndx);
    uint32_t name_offset;
    uint64_t strings_offset;
    uint64_t strings_size;

    if (section == 0 || shstr == 0) {
        return "";
    }
    name_offset = read_u32(section + 0);
    strings_offset = read_u64(shstr + 24);
    strings_size = read_u64(shstr + 32);
    if (name_offset >= strings_size || !range_valid(strings_offset, strings_size, object->size)) {
        return "";
    }
    return (const char *)(object->file + strings_offset + name_offset);
}

static int section_is(const LinkObject *object, uint16_t index, const char *name) {
    return rt_strcmp(section_name(object, index), name) == 0;
}

static int ends_with_text(const char *text, const char *suffix) {
    size_t text_length = rt_strlen(text);
    size_t suffix_length = rt_strlen(suffix);

    if (suffix_length > text_length) {
        return 0;
    }
    return rt_strcmp(text + text_length - suffix_length, suffix) == 0;
}

static unsigned long long parse_ar_decimal_field(const unsigned char *field, size_t field_size) {
    unsigned long long value = 0ULL;
    size_t i = 0U;

    while (i < field_size && field[i] == ' ') {
        i += 1U;
    }
    while (i < field_size && field[i] >= '0' && field[i] <= '9') {
        value = (value * 10ULL) + (unsigned long long)(field[i] - '0');
        i += 1U;
    }
    return value;
}

static void copy_ar_trimmed_name(char *buffer, size_t buffer_size, const unsigned char *field) {
    size_t start = 0U;
    size_t end = 16U;
    size_t length;

    while (start < end && field[start] == ' ') {
        start += 1U;
    }
    while (end > start && (field[end - 1U] == ' ' || field[end - 1U] == '/')) {
        end -= 1U;
    }
    length = end - start;
    if (length + 1U > buffer_size) {
        length = buffer_size - 1U;
    }
    if (length > 0U) {
        memcpy(buffer, field + start, length);
    }
    buffer[length] = '\0';
}

static void copy_ar_string_table_name(char *buffer, size_t buffer_size, const unsigned char *strings, size_t strings_size, size_t offset) {
    size_t length = 0U;

    if (buffer_size == 0U) {
        return;
    }
    if (offset >= strings_size) {
        buffer[0] = '\0';
        return;
    }
    while (offset + length < strings_size && strings[offset + length] != '\0' && strings[offset + length] != '\n' && strings[offset + length] != '/') {
        if (length + 1U >= buffer_size) {
            break;
        }
        buffer[length] = (char)strings[offset + length];
        length += 1U;
    }
    buffer[length] = '\0';
}

static void remember_section(LinkObject *object, uint16_t index) {
    const unsigned char *section = section_header(object, index);
    uint32_t type;

    if (section == 0) {
        return;
    }
    type = read_u32(section + 4);
    if (section_is(object, index, ".text")) {
        object->text_index = index;
        object->text_offset = read_u64(section + 24);
        object->text_size = read_u64(section + 32);
    } else if (section_is(object, index, ".data")) {
        object->data_index = index;
        object->data_offset = read_u64(section + 24);
        object->data_size = read_u64(section + 32);
    } else if (section_is(object, index, ".bss")) {
        object->bss_index = index;
        object->bss_size = read_u64(section + 32);
    } else if (type == SHT_SYMTAB) {
        object->symtab_index = index;
        object->symtab_offset = read_u64(section + 24);
        object->symtab_size = read_u64(section + 32);
        object->symtab_entsize = read_u64(section + 56);
    } else if (type == SHT_STRTAB && section_is(object, index, ".strtab")) {
        object->strtab_index = index;
        object->strtab_offset = read_u64(section + 24);
        object->strtab_size = read_u64(section + 32);
    } else if (type == SHT_RELA && section_is(object, index, ".rela.text")) {
        object->rela_text_index = index;
        object->rela_text_offset = read_u64(section + 24);
        object->rela_text_size = read_u64(section + 32);
        object->rela_text_entsize = read_u64(section + 56);
    } else if (type == SHT_RELA && section_is(object, index, ".rela.data")) {
        object->rela_data_index = index;
        object->rela_data_offset = read_u64(section + 24);
        object->rela_data_size = read_u64(section + 32);
        object->rela_data_entsize = read_u64(section + 56);
    }
}

static int parse_loaded_object(LinkObject *object, const char *path, unsigned char *file, size_t size, char *error_out, size_t error_size) {
    uint16_t i;

    rt_memset(object, 0, sizeof(*object));
    rt_copy_string(object->path, sizeof(object->path), path);
    object->file = file;
    object->size = size;
    if (object->size < ELF64_EHDR_SIZE || object->file[0] != 0x7fU || object->file[1] != 'E' || object->file[2] != 'L' || object->file[3] != 'F' ||
        object->file[4] != ELFCLASS64 || object->file[5] != ELFDATA2LSB || read_u16(object->file + 16) != ET_REL || read_u16(object->file + 18) != EM_X86_64) {
        set_link_error(error_out, error_size, "unsupported object format", path);
        return -1;
    }
    object->shoff = read_u64(object->file + 40);
    object->shentsize = read_u16(object->file + 58);
    object->shnum = read_u16(object->file + 60);
    object->shstrndx = read_u16(object->file + 62);
    if (object->shnum == 0 || object->shstrndx >= object->shnum || !range_valid(object->shoff, (uint64_t)object->shnum * object->shentsize, object->size)) {
        set_link_error(error_out, error_size, "invalid section table in object", path);
        return -1;
    }

    for (i = 1; i < object->shnum; ++i) {
        remember_section(object, i);
    }
    if (object->text_index == 0 || object->symtab_index == 0 || object->strtab_index == 0 || object->symtab_entsize < ELF64_SYM_SIZE) {
        set_link_error(error_out, error_size, "object is missing required linker sections", path);
        return -1;
    }
    if (!range_valid(object->text_offset, object->text_size, object->size) ||
        !range_valid(object->data_offset, object->data_size, object->size) ||
        !range_valid(object->symtab_offset, object->symtab_size, object->size) ||
        !range_valid(object->strtab_offset, object->strtab_size, object->size) ||
        !range_valid(object->rela_text_offset, object->rela_text_size, object->size) ||
        !range_valid(object->rela_data_offset, object->rela_data_size, object->size)) {
        set_link_error(error_out, error_size, "object section extends past end of file", path);
        return -1;
    }
    if (object->rela_text_index != 0 && object->rela_text_entsize < ELF64_RELA_SIZE) {
        set_link_error(error_out, error_size, "unsupported relocation entry size", path);
        return -1;
    }
    if (object->rela_data_index != 0 && object->rela_data_entsize < ELF64_RELA_SIZE) {
        set_link_error(error_out, error_size, "unsupported relocation entry size", path);
        return -1;
    }
    return 0;
}

static int read_file_alloc(const char *path, size_t capacity, unsigned char **file_out, size_t *size_out, char *error_out, size_t error_size) {
    int fd;
    long bytes_read;
    size_t total = 0;
    unsigned char *file;

    fd = platform_open_read(path);
    if (fd < 0) {
        set_link_error(error_out, error_size, "failed to open object", path);
        return -1;
    }
    file = (unsigned char *)rt_malloc(capacity);
    if (file == 0) {
        (void)platform_close(fd);
        set_link_error(error_out, error_size, "failed to allocate linker input buffer", path);
        return -1;
    }
    while ((bytes_read = platform_read(fd, file + total, capacity - total)) > 0) {
        total += (size_t)bytes_read;
        if (total == capacity) {
            break;
        }
    }
    (void)platform_close(fd);
    if (bytes_read < 0) {
        rt_free(file);
        set_link_error(error_out, error_size, "failed to read linker input", path);
        return -1;
    }
    if (total == capacity) {
        rt_free(file);
        set_link_error(error_out, error_size, "linker input exceeds native linker capacity", path);
        return -1;
    }
    *file_out = file;
    *size_out = total;
    return 0;
}

static int load_object(LinkObject *object, const char *path, char *error_out, size_t error_size) {
    unsigned char *file;
    size_t size;

    if (read_file_alloc(path, LINKER_MAX_OBJECT_SIZE, &file, &size, error_out, error_size) != 0) {
        return -1;
    }
    return parse_loaded_object(object, path, file, size, error_out, error_size);
}

static int load_archive(const char *path, LinkObject *objects, size_t *object_count, char *error_out, size_t error_size) {
    unsigned char *archive;
    const unsigned char *string_table = 0;
    size_t archive_size;
    size_t string_table_size = 0U;
    size_t offset = 8U;
    size_t loaded = 0U;

    if (read_file_alloc(path, LINKER_MAX_ARCHIVE_SIZE, &archive, &archive_size, error_out, error_size) != 0) {
        return -1;
    }
    if (archive_size < 8U || archive[0] != '!' || archive[1] != '<' || archive[2] != 'a' || archive[3] != 'r' ||
        archive[4] != 'c' || archive[5] != 'h' || archive[6] != '>' || archive[7] != '\n') {
        rt_free(archive);
        set_link_error(error_out, error_size, "unsupported archive format", path);
        return -1;
    }

    while (offset + LINKER_AR_HEADER_SIZE <= archive_size) {
        const unsigned char *header = archive + offset;
        size_t payload_offset = offset + LINKER_AR_HEADER_SIZE;
        unsigned long long payload_size_value = parse_ar_decimal_field(header + 48, 10U);
        size_t payload_size = (size_t)payload_size_value;
        size_t data_offset = payload_offset;
        size_t data_size = payload_size;
        size_t next_offset;
        char member_name[COMPILER_PATH_CAPACITY];
        char object_name[COMPILER_PATH_CAPACITY];
        unsigned char *object_file;

        if (header[58] != '`' || header[59] != '\n' || payload_size_value > (unsigned long long)(archive_size - payload_offset)) {
            rt_free(archive);
            set_link_error(error_out, error_size, "invalid archive member", path);
            return -1;
        }
        next_offset = payload_offset + payload_size + ((payload_size & 1U) != 0U ? 1U : 0U);
        if (next_offset > archive_size) {
            rt_free(archive);
            set_link_error(error_out, error_size, "invalid archive member size", path);
            return -1;
        }

        copy_ar_trimmed_name(member_name, sizeof(member_name), header);
        if (rt_strcmp(member_name, "//") == 0) {
            string_table = archive + payload_offset;
            string_table_size = payload_size;
            offset = next_offset;
            continue;
        }
        if (rt_strcmp(member_name, "/") == 0 || rt_strcmp(member_name, "__.SYMDEF") == 0 || rt_strcmp(member_name, "__.SYMDEF SORTED") == 0) {
            offset = next_offset;
            continue;
        }
        if (member_name[0] == '/' && member_name[1] >= '0' && member_name[1] <= '9') {
            size_t string_offset = 0U;
            size_t i = 1U;
            while (member_name[i] >= '0' && member_name[i] <= '9') {
                string_offset = (string_offset * 10U) + (size_t)(member_name[i] - '0');
                i += 1U;
            }
            copy_ar_string_table_name(member_name, sizeof(member_name), string_table, string_table_size, string_offset);
        } else if (member_name[0] == '#' && member_name[1] == '1' && member_name[2] == '/') {
            unsigned long long name_length_value = parse_ar_decimal_field((const unsigned char *)member_name + 3, rt_strlen(member_name + 3));
            size_t name_length = (size_t)name_length_value;
            size_t copy_length;

            if (name_length_value > payload_size) {
                rt_free(archive);
                set_link_error(error_out, error_size, "invalid archive member name", path);
                return -1;
            }
            copy_length = name_length + 1U < sizeof(member_name) ? name_length : sizeof(member_name) - 1U;
            memcpy(member_name, archive + payload_offset, copy_length);
            member_name[copy_length] = '\0';
            data_offset = payload_offset + name_length;
            data_size = payload_size - name_length;
        }

        if (!ends_with_text(member_name, ".o") && !(data_size >= ELF64_EHDR_SIZE && archive[data_offset] == 0x7fU && archive[data_offset + 1U] == 'E')) {
            offset = next_offset;
            continue;
        }
        if (*object_count >= LINKER_MAX_OBJECTS) {
            rt_free(archive);
            set_link_error(error_out, error_size, "too many objects for native linker", path);
            return -1;
        }
        if (data_size > LINKER_MAX_OBJECT_SIZE) {
            rt_free(archive);
            set_link_error(error_out, error_size, "archive member exceeds native linker capacity", member_name);
            return -1;
        }
        object_file = (unsigned char *)rt_malloc(data_size);
        if (object_file == 0) {
            rt_free(archive);
            set_link_error(error_out, error_size, "failed to allocate archive member", member_name);
            return -1;
        }
        memcpy(object_file, archive + data_offset, data_size);
        rt_copy_string(object_name, sizeof(object_name), path);
        if (rt_strlen(object_name) + rt_strlen(member_name) + 3U < sizeof(object_name)) {
            size_t used = rt_strlen(object_name);
            object_name[used++] = '(';
            rt_copy_string(object_name + used, sizeof(object_name) - used, member_name);
            used = rt_strlen(object_name);
            object_name[used++] = ')';
            object_name[used] = '\0';
        }
        if (parse_loaded_object(&objects[*object_count], object_name, object_file, data_size, error_out, error_size) != 0) {
            rt_free(archive);
            rt_free(object_file);
            return -1;
        }
        *object_count += 1U;
        loaded += 1U;
        offset = next_offset;
    }
    if (loaded == 0U) {
        rt_free(archive);
        set_link_error(error_out, error_size, "archive contains no supported objects", path);
        return -1;
    }
    rt_free(archive);
    return 0;
}

static const unsigned char *symbol_entry(const LinkObject *object, uint32_t index) {
    uint64_t offset;

    if (object->symtab_entsize == 0 || (uint64_t)(index + 1U) * object->symtab_entsize > object->symtab_size) {
        return 0;
    }
    offset = object->symtab_offset + ((uint64_t)index * object->symtab_entsize);
    if (!range_valid(offset, ELF64_SYM_SIZE, object->size)) {
        return 0;
    }
    return object->file + offset;
}

static const char *symbol_name(const LinkObject *object, const unsigned char *symbol) {
    uint32_t name_offset = read_u32(symbol + 0);

    if (name_offset >= object->strtab_size) {
        return "";
    }
    return (const char *)(object->file + object->strtab_offset + name_offset);
}

static int find_global(const char *name) {
    size_t i;

    for (i = 0; i < linker_global_count; ++i) {
        if (rt_strcmp(linker_globals[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int add_global(const char *name, uint64_t value, char *error_out, size_t error_size) {
    int existing;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }
    existing = find_global(name);
    if (existing >= 0) {
        set_link_error(error_out, error_size, "duplicate global symbol", name);
        return -1;
    }
    if (linker_global_count >= LINKER_MAX_GLOBALS) {
        set_link_error(error_out, error_size, "too many global symbols", name);
        return -1;
    }
    rt_copy_string(linker_globals[linker_global_count].name, sizeof(linker_globals[linker_global_count].name), name);
    linker_globals[linker_global_count].value = value;
    linker_global_count += 1U;
    return 0;
}

static int find_defined_symbol_owner(const char *name) {
    size_t i;

    for (i = 0; i < linker_defined_symbol_count; ++i) {
        if (rt_strcmp(linker_defined_symbols[i].name, name) == 0) {
            return (int)linker_defined_symbols[i].object_index;
        }
    }
    return -1;
}

static int add_defined_symbol_owner(const char *name, size_t object_index, char *error_out, size_t error_size) {
    if (name == 0 || name[0] == '\0') {
        return 0;
    }
    if (find_defined_symbol_owner(name) >= 0) {
        return 0;
    }
    if (linker_defined_symbol_count >= LINKER_MAX_GLOBALS) {
        set_link_error(error_out, error_size, "too many global symbols", name);
        return -1;
    }
    rt_copy_string(linker_defined_symbols[linker_defined_symbol_count].name,
                   sizeof(linker_defined_symbols[linker_defined_symbol_count].name),
                   name);
    linker_defined_symbols[linker_defined_symbol_count].object_index = object_index;
    linker_defined_symbol_count += 1U;
    return 0;
}

static int collect_defined_symbol_owners(LinkObject *objects, size_t object_count, char *error_out, size_t error_size) {
    size_t i;

    linker_defined_symbol_count = 0;
    for (i = 0; i < object_count; ++i) {
        LinkObject *object = &objects[i];
        uint32_t count = (uint32_t)(object->symtab_size / object->symtab_entsize);
        uint32_t symbol_index;

        for (symbol_index = 0; symbol_index < count; ++symbol_index) {
            const unsigned char *symbol = symbol_entry(object, symbol_index);
            if (symbol == 0) {
                set_link_error(error_out, error_size, "invalid symbol table", object->path);
                return -1;
            }
            if ((symbol[4] >> 4U) == STB_GLOBAL && read_u16(symbol + 6) != SHN_UNDEF &&
                add_defined_symbol_owner(symbol_name(object, symbol), i, error_out, error_size) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int find_defined_global_object(LinkObject *objects, size_t object_count, const char *name) {
    int indexed = find_defined_symbol_owner(name);
    size_t i;

    if (indexed >= 0) {
        return indexed;
    }

    for (i = 0; i < object_count; ++i) {
        LinkObject *object = &objects[i];
        uint32_t count = (uint32_t)(object->symtab_size / object->symtab_entsize);
        uint32_t symbol_index;

        for (symbol_index = 0; symbol_index < count; ++symbol_index) {
            const unsigned char *symbol = symbol_entry(object, symbol_index);
            if (symbol == 0) {
                continue;
            }
            if ((symbol[4] >> 4U) == STB_GLOBAL && read_u16(symbol + 6) != SHN_UNDEF && rt_strcmp(symbol_name(object, symbol), name) == 0) {
                return (int)i;
            }
        }
    }
    return -1;
}

static int mark_relocation_dependencies(LinkObject *objects,
                                        size_t object_count,
                                        LinkObject *object,
                                        uint64_t rela_offset,
                                        uint64_t rela_size,
                                        uint64_t rela_entsize,
                                        int *changed_out,
                                        char *error_out,
                                        size_t error_size) {
    uint64_t entry_count;
    uint64_t reloc_index;

    if (rela_size == 0) {
        return 0;
    }
    entry_count = rela_size / rela_entsize;
    for (reloc_index = 0; reloc_index < entry_count; ++reloc_index) {
        const unsigned char *reloc = object->file + rela_offset + (reloc_index * rela_entsize);
        uint64_t info = read_u64(reloc + 8);
        uint32_t symbol_index = (uint32_t)(info >> 32U);
        const unsigned char *symbol = symbol_entry(object, symbol_index);
        int owner_index;
        const char *name;

        if (symbol == 0) {
            set_link_error(error_out, error_size, "invalid relocation in object", object->path);
            return -1;
        }
        if (read_u16(symbol + 6) != SHN_UNDEF) {
            continue;
        }
        name = symbol_name(object, symbol);
        owner_index = find_defined_global_object(objects, object_count, name);
        if (owner_index < 0) {
            set_link_error(error_out, error_size, "undefined symbol", name);
            return -1;
        }
        if (!objects[owner_index].live) {
            objects[owner_index].live = 1;
            *changed_out = 1;
        }
    }
    return 0;
}

static int mark_live_objects(LinkObject *objects, size_t object_count, char *error_out, size_t error_size) {
    int root_index = find_defined_global_object(objects, object_count, "_start");
    int changed = 1;

    if (root_index < 0) {
        set_link_error(error_out, error_size, "undefined entry symbol", "_start");
        return -1;
    }
    objects[root_index].live = 1;

    while (changed) {
        size_t i;

        changed = 0;
        for (i = 0; i < object_count; ++i) {
            LinkObject *object = &objects[i];

            if (!object->live) {
                continue;
            }
            if (object->rela_text_index != 0 &&
                mark_relocation_dependencies(objects, object_count, object, object->rela_text_offset, object->rela_text_size, object->rela_text_entsize, &changed, error_out, error_size) != 0) {
                return -1;
            }
            if (object->rela_data_index != 0 &&
                mark_relocation_dependencies(objects, object_count, object, object->rela_data_offset, object->rela_data_size, object->rela_data_entsize, &changed, error_out, error_size) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int symbol_value(const LinkObject *object, const unsigned char *symbol, uint64_t *value_out, char *error_out, size_t error_size) {
    uint16_t shndx = read_u16(symbol + 6);
    uint64_t value = read_u64(symbol + 8);
    const char *name;
    int global_index;

    if (shndx == object->text_index) {
        *value_out = LINKER_BASE_VADDR + object->out_text_offset + value;
        return 0;
    }
    if (object->data_index != 0 && shndx == object->data_index) {
        *value_out = LINKER_BASE_VADDR + object->out_data_offset + value;
        return 0;
    }
    if (object->bss_index != 0 && shndx == object->bss_index) {
        *value_out = LINKER_BASE_VADDR + object->out_bss_offset + value;
        return 0;
    }
    if (shndx != SHN_UNDEF) {
        set_link_error(error_out, error_size, "unsupported symbol section", object->path);
        return -1;
    }

    name = symbol_name(object, symbol);
    global_index = find_global(name);
    if (global_index < 0) {
        set_link_error(error_out, error_size, "undefined symbol", name);
        return -1;
    }
    *value_out = linker_globals[global_index].value;
    return 0;
}

static int collect_globals(LinkObject *objects, size_t object_count, char *error_out, size_t error_size) {
    size_t i;

    linker_global_count = 0;
    for (i = 0; i < object_count; ++i) {
        LinkObject *object = &objects[i];
        uint32_t symbol_count = (uint32_t)(object->symtab_size / object->symtab_entsize);
        uint32_t symbol_index;

        if (!object->live) {
            continue;
        }
        for (symbol_index = 0; symbol_index < symbol_count; ++symbol_index) {
            const unsigned char *symbol = symbol_entry(object, symbol_index);
            unsigned int bind;
            uint16_t shndx;
            uint64_t value;

            if (symbol == 0) {
                set_link_error(error_out, error_size, "invalid symbol table", object->path);
                return -1;
            }
            bind = (unsigned int)(symbol[4] >> 4U);
            shndx = read_u16(symbol + 6);
            if (bind != STB_GLOBAL || shndx == SHN_UNDEF) {
                continue;
            }
            if (symbol_value(object, symbol, &value, error_out, error_size) != 0) {
                return -1;
            }
            if (add_global(symbol_name(object, symbol), value, error_out, error_size) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int apply_relocation_table(LinkObject *object,
                                  uint64_t rela_offset,
                                  uint64_t rela_size,
                                  uint64_t rela_entsize,
                                  uint64_t target_size,
                                  uint64_t out_target_offset,
                                  int is_text,
                                  unsigned char *output,
                                  char *error_out,
                                  size_t error_size) {
    uint64_t entry_count;
    uint64_t reloc_index;

    if (rela_size == 0) {
        return 0;
    }
    entry_count = rela_size / rela_entsize;
    for (reloc_index = 0; reloc_index < entry_count; ++reloc_index) {
        const unsigned char *reloc = object->file + rela_offset + (reloc_index * rela_entsize);
        uint64_t offset = read_u64(reloc + 0);
        uint64_t info = read_u64(reloc + 8);
        int64_t addend = read_i64(reloc + 16);
        uint32_t symbol_index = (uint32_t)(info >> 32U);
        uint32_t type = (uint32_t)info;
        const unsigned char *symbol = symbol_entry(object, symbol_index);
        uint64_t symbol_addr;
        uint64_t place_addr;
        uint64_t patch_offset;

        if (symbol == 0) {
            set_link_error(error_out, error_size, "invalid relocation in object", object->path);
            return -1;
        }
        if (type == R_X86_64_PC32 || type == R_X86_64_PLT32) {
            int64_t patched;

            if (!is_text || offset + 4ULL > target_size) {
                set_link_error(error_out, error_size, "invalid relocation in object", object->path);
                return -1;
            }
            if (symbol_value(object, symbol, &symbol_addr, error_out, error_size) != 0) {
                return -1;
            }
            place_addr = LINKER_BASE_VADDR + out_target_offset + offset;
            patched = (int64_t)symbol_addr + addend - (int64_t)place_addr;
            if (patched < -2147483648LL || patched > 2147483647LL) {
                set_link_error(error_out, error_size, "relocation is out of range", object->path);
                return -1;
            }
            patch_offset = out_target_offset + offset;
            write_u32(output + patch_offset, (uint32_t)(int32_t)patched);
        } else if (type == R_X86_64_64) {
            uint64_t patched;

            if (offset + 8ULL > target_size) {
                set_link_error(error_out, error_size, "invalid relocation in object", object->path);
                return -1;
            }
            if (symbol_value(object, symbol, &symbol_addr, error_out, error_size) != 0) {
                return -1;
            }
            patched = (uint64_t)((int64_t)symbol_addr + addend);
            patch_offset = out_target_offset + offset;
            write_u64(output + patch_offset, patched);
        } else {
            set_link_error(error_out, error_size, "unsupported x86_64 relocation in object", object->path);
            return -1;
        }
    }
    return 0;
}

static int apply_relocations(LinkObject *objects, size_t object_count, unsigned char *output, char *error_out, size_t error_size) {
    size_t i;

    for (i = 0; i < object_count; ++i) {
        LinkObject *object = &objects[i];

        if (!object->live) {
            continue;
        }
        if (object->rela_text_index != 0 &&
            apply_relocation_table(object, object->rela_text_offset, object->rela_text_size, object->rela_text_entsize, object->text_size, object->out_text_offset, 1, output, error_out, error_size) != 0) {
            return -1;
        }
        if (object->rela_data_index != 0 &&
            apply_relocation_table(object, object->rela_data_offset, object->rela_data_size, object->rela_data_entsize, object->data_size, object->out_data_offset, 0, output, error_out, error_size) != 0) {
            return -1;
        }
    }
    return 0;
}

static void layout_objects(LinkObject *objects, size_t object_count, uint64_t *text_size_out, uint64_t *data_size_out, uint64_t *bss_size_out) {
    uint64_t text_size = 0;
    uint64_t data_size = 0;
    uint64_t bss_size = 0;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        if (!objects[i].live) {
            continue;
        }
        objects[i].out_text_offset = text_size;
        text_size = align_u64(text_size + objects[i].text_size, 16U);
    }
    for (i = 0; i < object_count; ++i) {
        if (!objects[i].live) {
            continue;
        }
        objects[i].out_data_offset = data_size;
        data_size = align_u64(data_size + objects[i].data_size, 8U);
    }
    for (i = 0; i < object_count; ++i) {
        if (!objects[i].live) {
            continue;
        }
        objects[i].out_bss_offset = bss_size;
        bss_size = align_u64(bss_size + objects[i].bss_size, 8U);
    }
    *text_size_out = text_size;
    *data_size_out = data_size;
    *bss_size_out = bss_size;
}

static void copy_sections(LinkObject *objects, size_t object_count, unsigned char *output) {
    size_t i;

    for (i = 0; i < object_count; ++i) {
        if (!objects[i].live) {
            continue;
        }
        if (objects[i].text_size > 0) {
            memcpy(output + objects[i].out_text_offset, objects[i].file + objects[i].text_offset, (size_t)objects[i].text_size);
        }
        if (objects[i].data_size > 0) {
            memcpy(output + objects[i].out_data_offset, objects[i].file + objects[i].data_offset, (size_t)objects[i].data_size);
        }
    }
}

static void write_load_header(unsigned char *output, uint16_t index, uint32_t flags, uint64_t offset, uint64_t file_size, uint64_t memory_size) {
    unsigned char *program = output + ELF64_EHDR_SIZE + ((uint64_t)index * ELF64_PHDR_SIZE);

    write_u32(program + 0, PT_LOAD);
    write_u32(program + 4, flags);
    write_u64(program + 8, offset);
    write_u64(program + 16, LINKER_BASE_VADDR + offset);
    write_u64(program + 24, LINKER_BASE_VADDR + offset);
    write_u64(program + 32, file_size);
    write_u64(program + 40, memory_size);
    write_u64(program + 48, 0x1000U);
}

static void write_elf_header(unsigned char *output,
                             uint64_t entry,
                             uint64_t text_file_offset,
                             uint64_t text_size,
                             uint64_t data_file_offset,
                             uint64_t data_size,
                             uint64_t bss_size) {
    uint16_t program_count = data_size != 0 || bss_size != 0 ? 2U : 1U;

    output[0] = 0x7fU;
    output[1] = 'E';
    output[2] = 'L';
    output[3] = 'F';
    output[4] = ELFCLASS64;
    output[5] = ELFDATA2LSB;
    output[6] = EV_CURRENT;
    output[7] = 0U;
    write_u16(output + 16, ET_EXEC);
    write_u16(output + 18, EM_X86_64);
    write_u32(output + 20, EV_CURRENT);
    write_u64(output + 24, entry);
    write_u64(output + 32, ELF64_EHDR_SIZE);
    write_u64(output + 40, 0U);
    write_u32(output + 48, 0U);
    write_u16(output + 52, ELF64_EHDR_SIZE);
    write_u16(output + 54, ELF64_PHDR_SIZE);
    write_u16(output + 56, program_count);
    write_u16(output + 58, 0U);
    write_u16(output + 60, 0U);
    write_u16(output + 62, 0U);

    write_load_header(output, 0U, PF_R | PF_X, 0U, text_file_offset + text_size, text_file_offset + text_size);
    if (program_count > 1U) {
        write_load_header(output, 1U, PF_R | PF_W, data_file_offset, data_size, data_size + bss_size);
    }
}

int compiler_link_elf64_x86_64_static(const char *const *object_paths, size_t object_count, const char *output_path, char *error_out, size_t error_size) {
    uint64_t text_size;
    uint64_t data_size;
    uint64_t bss_size;
    uint64_t text_file_offset;
    uint64_t data_file_offset;
    uint64_t bss_vaddr_offset;
    uint64_t file_size;
    uint64_t memory_size;
    int has_writable_segment;
    uint64_t entry;
    int start_index;
    int fd;
    size_t loaded_object_count = 0U;
    size_t i;
    unsigned char *output;

    if (error_out != 0 && error_size > 0U) {
        error_out[0] = '\0';
    }
    if (object_count == 0 || object_count > LINKER_MAX_OBJECTS) {
        set_link_error(error_out, error_size, "invalid object count for native linker", "");
        return -1;
    }
    for (i = 0; i < object_count; ++i) {
        if (ends_with_text(object_paths[i], ".a")) {
            if (load_archive(object_paths[i], linker_objects, &loaded_object_count, error_out, error_size) != 0) {
                return -1;
            }
        } else {
            if (loaded_object_count >= LINKER_MAX_OBJECTS) {
                set_link_error(error_out, error_size, "too many objects for native linker", object_paths[i]);
                return -1;
            }
            if (load_object(&linker_objects[loaded_object_count], object_paths[i], error_out, error_size) != 0) {
                return -1;
            }
            loaded_object_count += 1U;
        }
    }
    object_count = loaded_object_count;
    if (object_count == 0U) {
        set_link_error(error_out, error_size, "invalid object count for native linker", "");
        return -1;
    }
    if (collect_defined_symbol_owners(linker_objects, object_count, error_out, error_size) != 0) {
        return -1;
    }
    if (mark_live_objects(linker_objects, object_count, error_out, error_size) != 0) {
        return -1;
    }

    layout_objects(linker_objects, object_count, &text_size, &data_size, &bss_size);
    has_writable_segment = data_size != 0 || bss_size != 0;
    text_file_offset = align_u64(ELF64_EHDR_SIZE + ((uint64_t)(has_writable_segment ? 2U : 1U) * ELF64_PHDR_SIZE), 16U);
    if (has_writable_segment) {
        data_file_offset = align_u64(text_file_offset + text_size, 0x1000U);
        bss_vaddr_offset = align_u64(data_file_offset + data_size, 8U);
        file_size = data_file_offset + data_size;
        memory_size = bss_vaddr_offset + bss_size;
    } else {
        data_file_offset = text_file_offset + text_size;
        bss_vaddr_offset = data_file_offset;
        file_size = data_file_offset;
        memory_size = file_size;
    }
    if (file_size > LINKER_MAX_OUTPUT || memory_size > LINKER_MAX_MEMORY) {
        set_link_error(error_out, error_size, "linked executable exceeds native linker capacity", output_path);
        return -1;
    }
    output = (unsigned char *)rt_malloc((size_t)file_size);
    if (output == 0) {
        set_link_error(error_out, error_size, "failed to allocate linker output", output_path);
        return -1;
    }
    for (i = 0; i < object_count; ++i) {
        if (!linker_objects[i].live) {
            continue;
        }
        linker_objects[i].out_text_offset += text_file_offset;
        linker_objects[i].out_data_offset += data_file_offset;
        linker_objects[i].out_bss_offset += bss_vaddr_offset;
    }

    rt_memset(output, 0, (size_t)file_size);
    copy_sections(linker_objects, object_count, output);
    if (collect_globals(linker_objects, object_count, error_out, error_size) != 0) {
        rt_free(output);
        return -1;
    }
    start_index = find_global("_start");
    if (start_index < 0) {
        set_link_error(error_out, error_size, "undefined entry symbol", "_start");
        rt_free(output);
        return -1;
    }
    entry = linker_globals[start_index].value;
    if (apply_relocations(linker_objects, object_count, output, error_out, error_size) != 0) {
        rt_free(output);
        return -1;
    }
    write_elf_header(output, entry, text_file_offset, text_size, data_file_offset, data_size, bss_size);

    fd = platform_open_write(output_path, 0755U);
    if (fd < 0) {
        set_link_error(error_out, error_size, "failed to open output executable", output_path);
        rt_free(output);
        return -1;
    }
    if (rt_write_all(fd, output, (size_t)file_size) != 0) {
        (void)platform_close(fd);
        set_link_error(error_out, error_size, "failed to write output executable", output_path);
        rt_free(output);
        return -1;
    }
    if (platform_close(fd) != 0) {
        set_link_error(error_out, error_size, "failed to close output executable", output_path);
        rt_free(output);
        return -1;
    }
    rt_free(output);
    (void)platform_change_mode(output_path, 0755U);
    return 0;
}
