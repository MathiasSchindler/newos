#include "linker.h"

#include "platform.h"
#include "runtime.h"
#include "source.h"

#define LINKER_MAX_OBJECTS 320
#define LINKER_MAX_OBJECT_SIZE (1024U * 1024U)
#define LINKER_MAX_ARCHIVE_SIZE (64U * 1024U * 1024U)
#define LINKER_MAX_OUTPUT (64U * 1024U * 1024U)
#define LINKER_MAX_MEMORY (512U * 1024U * 1024U)
#define LINKER_MAX_GLOBALS 8192
#define LINKER_MAX_SECTIONS 512
#define LINKER_MAX_RELA_SECTIONS 512
#define LINKER_BASE_VADDR 0x400000ULL
#define LINKER_AR_HEADER_SIZE 60U
#define LINKER_NO_INDEX ((size_t)-1)
#define LINKER_UNPLACED_OFFSET (~0ULL)

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
#define SHT_PROGBITS 1U
#define SHT_SYMTAB 2U
#define SHT_STRTAB 3U
#define SHT_RELA 4U
#define SHT_NOBITS 8U
#define SHF_WRITE 1ULL
#define SHF_ALLOC 2ULL
#define SHF_EXECINSTR 4ULL
#define SHN_UNDEF 0U
#define SHN_ABS 0xfff1U
#define STB_GLOBAL 1U
#define R_X86_64_NONE 0U
#define R_X86_64_64 1U
#define R_X86_64_PC32 2U
#define R_X86_64_PLT32 4U
#define R_X86_64_32 10U
#define R_X86_64_32S 11U

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

typedef enum {
    LINK_SECTION_NONE = 0,
    LINK_SECTION_TEXT,
    LINK_SECTION_DATA,
    LINK_SECTION_BSS
} LinkSectionKind;

typedef struct {
    uint16_t index;
    uint32_t type;
    uint64_t flags;
    uint64_t offset;
    uint64_t size;
    uint64_t align;
    uint64_t out_offset;
    LinkSectionKind kind;
    int live;
    int folded;
    size_t fold_object_index;
    size_t fold_section_index;
    uint64_t fold_addend;
    size_t parent_object_index;
    size_t parent_section_index;
    char why[COMPILER_PATH_CAPACITY];
} LinkSection;

typedef struct {
    uint16_t index;
    uint16_t target_index;
    uint64_t offset;
    uint64_t size;
    uint64_t entsize;
} LinkRelaSection;

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
    LinkSection sections[LINKER_MAX_SECTIONS];
    size_t section_count;
    LinkRelaSection rela_sections[LINKER_MAX_RELA_SECTIONS];
    size_t rela_section_count;
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

static const char *linker_entry_symbol(const CompilerLinkerOptions *options) {
    if (options != 0 && options->entry_symbol != 0 && options->entry_symbol[0] != '\0') {
        return options->entry_symbol;
    }
    return "_start";
}

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
    if (alignment <= 1ULL) {
        return value;
    }
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static uint64_t trim_trailing_zero_bytes(const unsigned char *data, uint64_t size, uint64_t minimum_size) {
    while (size > minimum_size && data[size - 1ULL] == 0U) {
        size -= 1ULL;
    }
    return size;
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

static LinkSection *find_link_section(LinkObject *object, uint16_t index) {
    size_t i;

    for (i = 0; i < object->section_count; ++i) {
        if (object->sections[i].index == index) {
            return &object->sections[i];
        }
    }
    return 0;
}

static const LinkSection *find_link_section_const(const LinkObject *object, uint16_t index) {
    size_t i;

    for (i = 0; i < object->section_count; ++i) {
        if (object->sections[i].index == index) {
            return &object->sections[i];
        }
    }
    return 0;
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
    uint64_t flags;

    if (section == 0) {
        return;
    }
    type = read_u32(section + 4);
    flags = read_u64(section + 8);
    if ((flags & SHF_ALLOC) != 0ULL && (type == SHT_PROGBITS || type == SHT_NOBITS) && object->section_count < LINKER_MAX_SECTIONS) {
        LinkSection *link_section = &object->sections[object->section_count++];

        link_section->index = index;
        link_section->type = type;
        link_section->flags = flags;
        link_section->offset = read_u64(section + 24);
        link_section->size = read_u64(section + 32);
        link_section->align = read_u64(section + 48);
        link_section->out_offset = 0ULL;
        link_section->live = 0;
        link_section->folded = 0;
        link_section->fold_object_index = 0U;
        link_section->fold_section_index = 0U;
        link_section->fold_addend = 0ULL;
        link_section->parent_object_index = LINKER_NO_INDEX;
        link_section->parent_section_index = LINKER_NO_INDEX;
        link_section->why[0] = '\0';
        if (type == SHT_NOBITS) {
            link_section->kind = LINK_SECTION_BSS;
        } else if ((flags & SHF_WRITE) != 0ULL) {
            link_section->kind = LINK_SECTION_DATA;
        } else {
            link_section->kind = LINK_SECTION_TEXT;
        }
    }
    if (type == SHT_RELA && object->rela_section_count < LINKER_MAX_RELA_SECTIONS) {
        LinkRelaSection *rela = &object->rela_sections[object->rela_section_count++];

        rela->index = index;
        rela->target_index = (uint16_t)read_u32(section + 44);
        rela->offset = read_u64(section + 24);
        rela->size = read_u64(section + 32);
        rela->entsize = read_u64(section + 56);
    }
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
    if (object->section_count == 0 || object->symtab_index == 0 || object->strtab_index == 0 || object->symtab_entsize < ELF64_SYM_SIZE) {
        set_link_error(error_out, error_size, "object is missing required linker sections", path);
        return -1;
    }
    for (i = 0; i < object->section_count; ++i) {
        if (object->sections[i].type != SHT_NOBITS && !range_valid(object->sections[i].offset, object->sections[i].size, object->size)) {
            set_link_error(error_out, error_size, "object section extends past end of file", path);
            return -1;
        }
    }
    if (!range_valid(object->symtab_offset, object->symtab_size, object->size) ||
        !range_valid(object->strtab_offset, object->strtab_size, object->size)) {
        set_link_error(error_out, error_size, "object section extends past end of file", path);
        return -1;
    }
    for (i = 0; i < object->rela_section_count; ++i) {
        if (!range_valid(object->rela_sections[i].offset, object->rela_sections[i].size, object->size)) {
            set_link_error(error_out, error_size, "object section extends past end of file", path);
            return -1;
        }
        if (object->rela_sections[i].entsize < ELF64_RELA_SIZE) {
            set_link_error(error_out, error_size, "unsupported relocation entry size", path);
            return -1;
        }
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
        rt_copy_string(object_name, sizeof(object_name), path);
        if (rt_strlen(object_name) + rt_strlen(member_name) + 3U < sizeof(object_name)) {
            size_t used = rt_strlen(object_name);
            object_name[used++] = '(';
            rt_copy_string(object_name + used, sizeof(object_name) - used, member_name);
            used = rt_strlen(object_name);
            object_name[used++] = ')';
            object_name[used] = '\0';
        }
        if (parse_loaded_object(&objects[*object_count], object_name, archive + data_offset, data_size, error_out, error_size) != 0) {
            rt_free(archive);
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

static int find_defined_global_symbol(LinkObject *objects,
                                      size_t object_count,
                                      const char *name,
                                      size_t *object_index_out,
                                      const unsigned char **symbol_out) {
    int owner_index = find_defined_global_object(objects, object_count, name);
    uint32_t count;
    uint32_t symbol_index;
    LinkObject *object;

    if (owner_index < 0) {
        return -1;
    }
    object = &objects[owner_index];
    count = (uint32_t)(object->symtab_size / object->symtab_entsize);
    for (symbol_index = 0; symbol_index < count; ++symbol_index) {
        const unsigned char *symbol = symbol_entry(object, symbol_index);

        if (symbol == 0) {
            continue;
        }
        if ((symbol[4] >> 4U) == STB_GLOBAL && read_u16(symbol + 6) != SHN_UNDEF && rt_strcmp(symbol_name(object, symbol), name) == 0) {
            *object_index_out = (size_t)owner_index;
            *symbol_out = symbol;
            return 0;
        }
    }
    return -1;
}

static int mark_symbol_section_live(LinkObject *objects,
                                    size_t object_count,
                                    size_t object_index,
                                    const unsigned char *symbol,
                                    const char *reason,
                                    size_t parent_object_index,
                                    size_t parent_section_index,
                                    int *changed_out,
                                    char *error_out,
                                    size_t error_size) {
    LinkObject *object = &objects[object_index];
    uint16_t shndx = read_u16(symbol + 6);
    LinkSection *section;
    const char *name;
    const unsigned char *definition;
    size_t definition_object_index;

    if (shndx == SHN_ABS) {
        return 0;
    }
    if (shndx == SHN_UNDEF) {
        name = symbol_name(object, symbol);
        if (find_defined_global_symbol(objects, object_count, name, &definition_object_index, &definition) != 0) {
            set_link_error(error_out, error_size, "undefined symbol", name);
            return -1;
        }
        return mark_symbol_section_live(objects, object_count, definition_object_index, definition, name, parent_object_index, parent_section_index, changed_out, error_out, error_size);
    }
    section = find_link_section(object, shndx);
    if (section == 0) {
        set_link_error(error_out, error_size, "unsupported symbol section", object->path);
        return -1;
    }
    object->live = 1;
    if (!section->live) {
        section->live = 1;
        rt_copy_string(section->why, sizeof(section->why), reason != 0 && reason[0] != '\0' ? reason : symbol_name(object, symbol));
        section->parent_object_index = parent_object_index;
        section->parent_section_index = parent_section_index;
        *changed_out = 1;
    }
    return 0;
}

static int mark_relocation_dependencies(LinkObject *objects,
                                        size_t object_count,
                                        LinkObject *object,
                                        uint64_t rela_offset,
                                        uint64_t rela_size,
                                        uint64_t rela_entsize,
                                        size_t *queue,
                                        size_t *queue_tail,
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
            if (queue != 0 && queue_tail != 0 && *queue_tail < LINKER_MAX_OBJECTS) {
                queue[*queue_tail] = (size_t)owner_index;
                *queue_tail += 1U;
            }
            *changed_out = 1;
        }
    }
    return 0;
}

static int mark_live_objects(LinkObject *objects, size_t object_count, const char *entry_symbol, char *error_out, size_t error_size) {
    int root_index = find_defined_global_object(objects, object_count, entry_symbol);
    size_t queue[LINKER_MAX_OBJECTS];
    size_t queue_head = 0U;
    size_t queue_tail = 0U;

    if (root_index < 0) {
        set_link_error(error_out, error_size, "undefined entry symbol", entry_symbol);
        return -1;
    }
    objects[root_index].live = 1;
    queue[queue_tail++] = (size_t)root_index;

    while (queue_head < queue_tail) {
        size_t i = queue[queue_head++];
        LinkObject *object = &objects[i];
        size_t rela_index;
        int changed = 0;

        (void)changed;
        if (!object->live) {
            continue;
        }
        for (rela_index = 0; rela_index < object->rela_section_count; ++rela_index) {
            LinkRelaSection *rela = &object->rela_sections[rela_index];

            if (mark_relocation_dependencies(objects, object_count, object, rela->offset, rela->size, rela->entsize, queue, &queue_tail, &changed, error_out, error_size) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int mark_live_sections(LinkObject *objects, size_t object_count, const char *entry_symbol, char *error_out, size_t error_size) {
    const unsigned char *entry_symbol_entry;
    size_t entry_object_index;
    int changed = 0;

    if (find_defined_global_symbol(objects, object_count, entry_symbol, &entry_object_index, &entry_symbol_entry) != 0) {
        set_link_error(error_out, error_size, "undefined entry symbol", entry_symbol);
        return -1;
    }
    if (mark_symbol_section_live(objects, object_count, entry_object_index, entry_symbol_entry, "entry", LINKER_NO_INDEX, LINKER_NO_INDEX, &changed, error_out, error_size) != 0) {
        return -1;
    }

    while (changed) {
        size_t i;

        changed = 0;
        for (i = 0; i < object_count; ++i) {
            LinkObject *object = &objects[i];
            size_t rela_index;

            for (rela_index = 0; rela_index < object->rela_section_count; ++rela_index) {
                LinkRelaSection *rela = &object->rela_sections[rela_index];
                LinkSection *target = find_link_section(object, rela->target_index);
                uint64_t entry_count;
                uint64_t reloc_index;
                size_t parent_section_index;

                if (target == 0 || !target->live || rela->size == 0) {
                    continue;
                }
                parent_section_index = (size_t)(target - object->sections);
                entry_count = rela->size / rela->entsize;
                for (reloc_index = 0; reloc_index < entry_count; ++reloc_index) {
                    const unsigned char *reloc = object->file + rela->offset + (reloc_index * rela->entsize);
                    uint64_t info = read_u64(reloc + 8);
                    uint32_t symbol_index = (uint32_t)(info >> 32U);
                    uint32_t type = (uint32_t)info;
                    const unsigned char *symbol = symbol_entry(object, symbol_index);

                    if (symbol == 0) {
                        set_link_error(error_out, error_size, "invalid relocation in object", object->path);
                        return -1;
                    }
                    if (type == R_X86_64_NONE) {
                        continue;
                    }
                    if (mark_symbol_section_live(objects, object_count, i, symbol, symbol_name(object, symbol), i, parent_section_index, &changed, error_out, error_size) != 0) {
                        return -1;
                    }
                }
            }
        }
    }
    return 0;
}

static void mark_all_sections_in_live_objects(LinkObject *objects, size_t object_count) {
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            objects[i].sections[section_index].live = 1;
            objects[i].sections[section_index].parent_object_index = LINKER_NO_INDEX;
            objects[i].sections[section_index].parent_section_index = LINKER_NO_INDEX;
            rt_copy_string(objects[i].sections[section_index].why, sizeof(objects[i].sections[section_index].why), "live object");
        }
    }
}

static int section_has_relocations(const LinkObject *object, uint16_t section_index) {
    size_t rela_index;

    for (rela_index = 0; rela_index < object->rela_section_count; ++rela_index) {
        if (object->rela_sections[rela_index].target_index == section_index && object->rela_sections[rela_index].size != 0ULL) {
            return 1;
        }
    }
    return 0;
}

static int sections_have_same_bytes(const LinkObject *left_object, const LinkSection *left, const LinkObject *right_object, const LinkSection *right) {
    if (left->size != right->size || left->align != right->align || left->type != right->type || left->size == 0ULL) {
        return 0;
    }
    if (left->type == SHT_NOBITS || right->type == SHT_NOBITS) {
        return 0;
    }
    return memcmp(left_object->file + left->offset, right_object->file + right->offset, (size_t)left->size) == 0;
}

static int section_alignment_satisfies(uint64_t master_align, uint64_t folded_align, uint64_t addend) {
    if (folded_align <= 1ULL) {
        return 1;
    }
    if (master_align <= 1ULL) {
        return 0;
    }
    return master_align >= folded_align && (master_align % folded_align) == 0ULL && (addend % folded_align) == 0ULL;
}

static int section_is_readonly_data(const LinkSection *section) {
    return section->kind == LINK_SECTION_TEXT && (section->flags & SHF_EXECINSTR) == 0ULL && section->type == SHT_PROGBITS;
}

static int sections_have_suffix_bytes(const LinkObject *folded_object,
                                      const LinkSection *folded,
                                      const LinkObject *master_object,
                                      const LinkSection *master,
                                      uint64_t *addend_out) {
    uint64_t addend;

    if (!section_is_readonly_data(folded) || !section_is_readonly_data(master) || folded->size == 0ULL || folded->size >= master->size) {
        return 0;
    }
    if (folded->type == SHT_NOBITS || master->type == SHT_NOBITS) {
        return 0;
    }
    addend = master->size - folded->size;
    if (!section_alignment_satisfies(master->align, folded->align, addend)) {
        return 0;
    }
    if (memcmp(folded_object->file + folded->offset, master_object->file + master->offset + addend, (size_t)folded->size) != 0) {
        return 0;
    }
    *addend_out = addend;
    return 1;
}

static void fold_identical_sections(LinkObject *objects, size_t object_count) {
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            LinkSection *section = &objects[i].sections[section_index];
            size_t master_object_index;

            if (!section->live || section->folded || section->kind != LINK_SECTION_TEXT || section_has_relocations(&objects[i], section->index)) {
                continue;
            }
            for (master_object_index = 0; master_object_index <= i; ++master_object_index) {
                size_t master_section_index;

                for (master_section_index = 0; master_section_index < objects[master_object_index].section_count; ++master_section_index) {
                    LinkSection *master = &objects[master_object_index].sections[master_section_index];

                    if (master_object_index == i && master_section_index >= section_index) {
                        break;
                    }
                    if (!master->live || master->folded || master->kind != LINK_SECTION_TEXT || section_has_relocations(&objects[master_object_index], master->index)) {
                        continue;
                    }
                    if (sections_have_same_bytes(&objects[i], section, &objects[master_object_index], master)) {
                        section->folded = 1;
                        section->fold_object_index = master_object_index;
                        section->fold_section_index = master_section_index;
                        section->fold_addend = 0ULL;
                        rt_copy_string(section->why, sizeof(section->why), "identical section folded");
                        break;
                    }
                    if (section_is_readonly_data(section) && section_is_readonly_data(master)) {
                        uint64_t fold_addend;

                        if (sections_have_suffix_bytes(&objects[i], section, &objects[master_object_index], master, &fold_addend)) {
                            section->folded = 1;
                            section->fold_object_index = master_object_index;
                            section->fold_section_index = master_section_index;
                            section->fold_addend = fold_addend;
                            rt_copy_string(section->why, sizeof(section->why), "read-only suffix folded");
                            break;
                        }
                    }
                    if (section->folded) {
                        break;
                    }
                }
                if (section->folded) {
                    break;
                }
            }
        }
    }
}

static int symbol_value(const LinkObject *object, const unsigned char *symbol, uint64_t *value_out, char *error_out, size_t error_size) {
    uint16_t shndx = read_u16(symbol + 6);
    uint64_t value = read_u64(symbol + 8);
    const LinkSection *section;
    const char *name;
    int global_index;

    if (shndx == SHN_ABS) {
        *value_out = value;
        return 0;
    }
    if (shndx != SHN_UNDEF) {
        section = find_link_section_const(object, shndx);
        if (section != 0) {
            if (section->folded) {
                const LinkSection *folded = &linker_objects[section->fold_object_index].sections[section->fold_section_index];
                *value_out = LINKER_BASE_VADDR + folded->out_offset + section->fold_addend + value;
                return 0;
            }
            *value_out = LINKER_BASE_VADDR + section->out_offset + value;
            return 0;
        }
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
            if (shndx != SHN_ABS) {
                const LinkSection *section = find_link_section_const(object, shndx);
                if (section == 0 || !section->live) {
                    continue;
                }
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
        if (type == R_X86_64_NONE) {
            continue;
        } else if (type == R_X86_64_PC32 || type == R_X86_64_PLT32) {
            int64_t patched;

            (void)is_text;
            if (offset + 4ULL > target_size) {
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
        } else if (type == R_X86_64_32 || type == R_X86_64_32S) {
            int64_t patched;

            if (offset + 4ULL > target_size) {
                set_link_error(error_out, error_size, "invalid relocation in object", object->path);
                return -1;
            }
            if (symbol_value(object, symbol, &symbol_addr, error_out, error_size) != 0) {
                return -1;
            }
            patched = (int64_t)symbol_addr + addend;
            if (type == R_X86_64_32 && (patched < 0 || patched > 4294967295LL)) {
                set_link_error(error_out, error_size, "relocation is out of range", object->path);
                return -1;
            }
            if (type == R_X86_64_32S && (patched < -2147483648LL || patched > 2147483647LL)) {
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
        size_t rela_index;

        if (!object->live) {
            continue;
        }
        for (rela_index = 0; rela_index < object->rela_section_count; ++rela_index) {
            LinkRelaSection *rela = &object->rela_sections[rela_index];
            LinkSection *target = find_link_section(object, rela->target_index);

            if (target == 0) {
                set_link_error(error_out, error_size, "relocation targets unsupported section", object->path);
                return -1;
            }
            if (!target->live || target->folded) {
                continue;
            }
            if (target->kind == LINK_SECTION_BSS) {
                continue;
            }
            if (apply_relocation_table(object, rela->offset, rela->size, rela->entsize, target->size, target->out_offset, target->kind == LINK_SECTION_TEXT, output, error_out, error_size) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static uint64_t section_trailing_zero_bytes(const LinkObject *object, const LinkSection *section) {
    uint64_t count = 0ULL;

    if (section->type == SHT_NOBITS || section->size == 0ULL) {
        return 0ULL;
    }
    while (count < section->size && object->file[section->offset + section->size - count - 1ULL] == 0U) {
        count += 1ULL;
    }
    return count;
}

static int layout_section_is_better(const LinkObject *candidate_object,
                                    const LinkSection *candidate,
                                    const LinkObject *best_object,
                                    const LinkSection *best,
                                    LinkSectionKind kind) {
    if (best == 0) {
        return 1;
    }
    if (candidate->align != best->align) {
        return candidate->align > best->align;
    }
    if (kind == LINK_SECTION_DATA) {
        uint64_t candidate_zero_tail = section_trailing_zero_bytes(candidate_object, candidate);
        uint64_t best_zero_tail = section_trailing_zero_bytes(best_object, best);

        if (candidate_zero_tail != best_zero_tail) {
            return candidate_zero_tail < best_zero_tail;
        }
    }
    return candidate->size > best->size;
}

static uint64_t layout_sections_of_kind(LinkObject *objects, size_t object_count, LinkSectionKind kind) {
    uint64_t size = 0ULL;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            LinkSection *section = &objects[i].sections[section_index];

            if (section->live && !section->folded && section->kind == kind) {
                section->out_offset = LINKER_UNPLACED_OFFSET;
            }
        }
    }
    for (;;) {
        LinkSection *best = 0;
        LinkObject *best_object = 0;
        size_t best_object_index = 0U;
        size_t best_section_index = 0U;

        for (i = 0; i < object_count; ++i) {
            size_t section_index;

            if (!objects[i].live) {
                continue;
            }
            for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
                LinkSection *section = &objects[i].sections[section_index];

                if (!section->live || section->folded || section->kind != kind || section->out_offset != LINKER_UNPLACED_OFFSET) {
                    continue;
                }
                if (layout_section_is_better(&objects[i], section, best_object, best, kind)) {
                    best = section;
                    best_object = &objects[i];
                    best_object_index = i;
                    best_section_index = section_index;
                }
            }
        }
        if (best == 0) {
            break;
        }
        (void)best_object_index;
        (void)best_section_index;
        size = align_u64(size, best->align);
        best->out_offset = size;
        size += best->size;
    }
    return size;
}

static void layout_objects(LinkObject *objects, size_t object_count, uint64_t *text_size_out, uint64_t *data_size_out, uint64_t *bss_size_out) {
    *text_size_out = layout_sections_of_kind(objects, object_count, LINK_SECTION_TEXT);
    *data_size_out = layout_sections_of_kind(objects, object_count, LINK_SECTION_DATA);
    *bss_size_out = layout_sections_of_kind(objects, object_count, LINK_SECTION_BSS);
}

static void copy_sections(LinkObject *objects, size_t object_count, unsigned char *output) {
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];

            if (!section->live || section->folded || section->kind == LINK_SECTION_BSS || section->size == 0ULL) {
                continue;
            }
            memcpy(output + section->out_offset, objects[i].file + section->offset, (size_t)section->size);
        }
    }
}

static void write_load_header(unsigned char *output, uint16_t index, uint32_t flags, uint64_t offset, uint64_t file_size, uint64_t memory_size, uint64_t alignment) {
    unsigned char *program = output + ELF64_EHDR_SIZE + ((uint64_t)index * ELF64_PHDR_SIZE);

    write_u32(program + 0, PT_LOAD);
    write_u32(program + 4, flags);
    write_u64(program + 8, offset);
    write_u64(program + 16, LINKER_BASE_VADDR + offset);
    write_u64(program + 24, LINKER_BASE_VADDR + offset);
    write_u64(program + 32, file_size);
    write_u64(program + 40, memory_size);
    write_u64(program + 48, alignment);
}

static void write_elf_header(unsigned char *output,
                             uint64_t entry,
                             uint64_t text_file_offset,
                             uint64_t text_size,
                             uint64_t data_file_offset,
                             uint64_t data_size,
                             uint64_t bss_size,
                             uint64_t file_size,
                             uint64_t memory_size,
                             int tiny) {
    uint16_t program_count = tiny ? 1U : (data_size != 0 || bss_size != 0 ? 2U : 1U);
    uint64_t segment_alignment = tiny ? 1U : 0x1000U;

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

    if (tiny) {
        uint32_t flags = PF_R | PF_X;
        if (data_size != 0 || bss_size != 0) {
            flags |= PF_W;
        }
        write_load_header(output, 0U, flags, 0U, file_size, memory_size, segment_alignment);
        return;
    }

    write_load_header(output, 0U, PF_R | PF_X, 0U, file_size < text_file_offset + text_size ? file_size : text_file_offset + text_size, text_file_offset + text_size, segment_alignment);
    if (program_count > 1U) {
        uint64_t data_file_size = 0ULL;

        if (file_size > data_file_offset) {
            data_file_size = file_size - data_file_offset;
            if (data_file_size > data_size) {
                data_file_size = data_size;
            }
        }
        write_load_header(output, 1U, PF_R | PF_W, data_file_offset, data_file_size, data_size + bss_size, segment_alignment);
    }
}

static int linker_write_hex64(int fd, uint64_t value) {
    char buffer[18];
    const char *digits = "0123456789abcdef";
    int i;

    buffer[0] = '0';
    buffer[1] = 'x';
    for (i = 0; i < 16; ++i) {
        buffer[2 + i] = digits[(value >> (uint64_t)((15 - i) * 4)) & 0xfU];
    }
    return rt_write_all(fd, buffer, sizeof(buffer));
}

static uint64_t count_live_objects(const LinkObject *objects, size_t object_count) {
    uint64_t count = 0;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        if (objects[i].live) {
            count += 1ULL;
        }
    }
    return count;
}

static uint64_t count_live_sections(const LinkObject *objects, size_t object_count) {
    uint64_t count = 0;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            if (objects[i].sections[section_index].live) {
                count += 1ULL;
            }
        }
    }
    return count;
}

static uint64_t count_folded_section_bytes(const LinkObject *objects, size_t object_count) {
    uint64_t count = 0;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            if (objects[i].sections[section_index].live && objects[i].sections[section_index].folded) {
                count += objects[i].sections[section_index].size;
            }
        }
    }
    return count;
}

static uint64_t count_discarded_section_bytes(const LinkObject *objects, size_t object_count) {
    uint64_t count = 0;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            if (!objects[i].sections[section_index].live) {
                count += objects[i].sections[section_index].size;
            }
        }
    }
    return count;
}

static uint64_t count_total_sections(const LinkObject *objects, size_t object_count) {
    uint64_t count = 0;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        count += (uint64_t)objects[i].section_count;
    }
    return count;
}

static uint64_t count_live_relocations(const LinkObject *objects, size_t object_count) {
    uint64_t count = 0;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t rela_index;

        for (rela_index = 0; rela_index < objects[i].rela_section_count; ++rela_index) {
            const LinkRelaSection *rela = &objects[i].rela_sections[rela_index];
            const LinkSection *target = find_link_section_const(&objects[i], rela->target_index);

            if (target != 0 && target->live && rela->entsize != 0) {
                count += rela->size / rela->entsize;
            }
        }
    }
    return count;
}

static int write_link_stats(int fd,
                            const LinkObject *objects,
                            size_t object_count,
                            uint64_t text_size,
                            uint64_t data_size,
                            uint64_t bss_size,
                            uint64_t file_size,
                            uint64_t memory_size,
                            uint64_t header_size,
                            uint64_t padding_size,
                            int tiny,
                            int gc_sections) {
    if (rt_write_cstr(fd, "linker stats\nobjects live/total: ") != 0) return -1;
    if (rt_write_uint(fd, count_live_objects(objects, object_count)) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, (unsigned long long)object_count) != 0) return -1;
    if (rt_write_cstr(fd, "\nsections live/total: ") != 0) return -1;
    if (rt_write_uint(fd, count_live_sections(objects, object_count)) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, count_total_sections(objects, object_count)) != 0) return -1;
    if (rt_write_cstr(fd, "\nrelocations applied: ") != 0) return -1;
    if (rt_write_uint(fd, count_live_relocations(objects, object_count)) != 0) return -1;
    if (rt_write_cstr(fd, "\nfolded/discarded bytes: ") != 0) return -1;
    if (rt_write_uint(fd, count_folded_section_bytes(objects, object_count)) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, count_discarded_section_bytes(objects, object_count)) != 0) return -1;
    if (rt_write_cstr(fd, "\ntext/data/bss: ") != 0) return -1;
    if (rt_write_uint(fd, text_size) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, data_size) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, bss_size) != 0) return -1;
    if (rt_write_cstr(fd, "\nfile/memory: ") != 0) return -1;
    if (rt_write_uint(fd, file_size) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, memory_size) != 0) return -1;
    if (rt_write_cstr(fd, "\nheaders/padding: ") != 0) return -1;
    if (rt_write_uint(fd, header_size) != 0) return -1;
    if (rt_write_cstr(fd, "/") != 0) return -1;
    if (rt_write_uint(fd, padding_size) != 0) return -1;
    if (rt_write_cstr(fd, "\npolicy: ") != 0) return -1;
    if (rt_write_cstr(fd, tiny ? "tiny" : "page-aligned") != 0) return -1;
    if (rt_write_cstr(fd, gc_sections ? " gc-sections\n" : " object-gc\n") != 0) return -1;
    return 0;
}

static int write_link_map(const char *path,
                          const LinkObject *objects,
                          size_t object_count,
                          const char *output_path,
                          const char *entry_symbol,
                          uint64_t entry,
                          uint64_t text_size,
                          uint64_t data_size,
                          uint64_t bss_size,
                          uint64_t file_size,
                          uint64_t memory_size,
                          int tiny,
                          int gc_sections,
                          char *error_out,
                          size_t error_size) {
    int fd = platform_open_write(path, 0644U);
    size_t i;

    if (fd < 0) {
        set_link_error(error_out, error_size, "failed to open map file", path);
        return -1;
    }
    if (rt_write_cstr(fd, "newos linker map\noutput: ") != 0 || rt_write_cstr(fd, output_path) != 0 ||
        rt_write_cstr(fd, "\nentry: ") != 0 || rt_write_cstr(fd, entry_symbol) != 0 || rt_write_cstr(fd, " ") != 0 || linker_write_hex64(fd, entry) != 0 ||
        rt_write_cstr(fd, "\npolicy: ") != 0 || rt_write_cstr(fd, tiny ? "tiny" : "page-aligned") != 0 || rt_write_cstr(fd, gc_sections ? " gc-sections\n" : " object-gc\n") != 0 ||
        rt_write_cstr(fd, "text/data/bss/file/memory: ") != 0 || rt_write_uint(fd, text_size) != 0 || rt_write_cstr(fd, "/") != 0 ||
        rt_write_uint(fd, data_size) != 0 || rt_write_cstr(fd, "/") != 0 || rt_write_uint(fd, bss_size) != 0 || rt_write_cstr(fd, "/") != 0 ||
        rt_write_uint(fd, file_size) != 0 || rt_write_cstr(fd, "/") != 0 || rt_write_uint(fd, memory_size) != 0 || rt_write_cstr(fd, "\n\nLive sections:\n") != 0) {
        (void)platform_close(fd);
        set_link_error(error_out, error_size, "failed to write map file", path);
        return -1;
    }
    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];

            if (!section->live) {
                continue;
            }
            if (linker_write_hex64(fd, LINKER_BASE_VADDR + (section->folded ? objects[section->fold_object_index].sections[section->fold_section_index].out_offset + section->fold_addend : section->out_offset)) != 0 || rt_write_cstr(fd, " ") != 0 ||
                rt_write_uint(fd, section->size) != 0 || rt_write_cstr(fd, " ") != 0 || rt_write_cstr(fd, section_name(&objects[i], section->index)) != 0 ||
                rt_write_cstr(fd, " ") != 0 || rt_write_cstr(fd, objects[i].path) != 0 || rt_write_cstr(fd, " why=") != 0 ||
                rt_write_cstr(fd, section->why[0] != '\0' ? section->why : "live") != 0 ||
                (section->folded && (rt_write_cstr(fd, " folded-to=") != 0 || rt_write_cstr(fd, objects[section->fold_object_index].path) != 0 || rt_write_cstr(fd, ":") != 0 || rt_write_cstr(fd, section_name(&objects[section->fold_object_index], objects[section->fold_object_index].sections[section->fold_section_index].index)) != 0)) ||
                rt_write_cstr(fd, "\n") != 0) {
                (void)platform_close(fd);
                set_link_error(error_out, error_size, "failed to write map file", path);
                return -1;
            }
        }
    }
    if (platform_close(fd) != 0) {
        set_link_error(error_out, error_size, "failed to close map file", path);
        return -1;
    }
    return 0;
}

static int write_section_label(int fd, const LinkObject *objects, size_t object_index, size_t section_index) {
    const LinkSection *section;

    if (object_index == LINKER_NO_INDEX || section_index == LINKER_NO_INDEX || section_index >= objects[object_index].section_count) {
        return rt_write_cstr(fd, "<root>");
    }
    section = &objects[object_index].sections[section_index];
    if (rt_write_cstr(fd, section_name(&objects[object_index], section->index)) != 0 || rt_write_cstr(fd, " in ") != 0 || rt_write_cstr(fd, objects[object_index].path) != 0) {
        return -1;
    }
    return 0;
}

static int write_live_chain(int fd, const LinkObject *objects, size_t object_count, size_t object_index, size_t section_index) {
    size_t depth;

    if (rt_write_cstr(fd, "chain: ") != 0) {
        return -1;
    }
    for (depth = 0; depth < object_count * LINKER_MAX_SECTIONS; ++depth) {
        const LinkSection *section;

        if (object_index == LINKER_NO_INDEX || section_index == LINKER_NO_INDEX || object_index >= object_count || section_index >= objects[object_index].section_count) {
            break;
        }
        section = &objects[object_index].sections[section_index];
        if (depth != 0 && rt_write_cstr(fd, " <- ") != 0) {
            return -1;
        }
        if (write_section_label(fd, objects, object_index, section_index) != 0) {
            return -1;
        }
        if (section->parent_object_index == LINKER_NO_INDEX || section->parent_section_index == LINKER_NO_INDEX) {
            break;
        }
        object_index = section->parent_object_index;
        section_index = section->parent_section_index;
    }
    if (rt_write_cstr(fd, "\n") != 0) {
        return -1;
    }
    return 0;
}

static int write_gc_sections(int fd, const LinkObject *objects, size_t object_count) {
    size_t i;

    if (rt_write_cstr(fd, "discarded sections\n") != 0) {
        return -1;
    }
    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];

            if (section->live) {
                continue;
            }
            if (rt_write_uint(fd, section->size) != 0 || rt_write_cstr(fd, " ") != 0 || rt_write_cstr(fd, section_name(&objects[i], section->index)) != 0 ||
                rt_write_cstr(fd, " ") != 0 || rt_write_cstr(fd, objects[i].path) != 0 || rt_write_cstr(fd, "\n") != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int write_why_live(int fd, const LinkObject *objects, size_t object_count, const char *query) {
    int found = 0;
    size_t i;

    if (query == 0 || query[0] == '\0') {
        return 0;
    }
    if (rt_write_cstr(fd, "why-live ") != 0 || rt_write_cstr(fd, query) != 0 || rt_write_cstr(fd, "\n") != 0) {
        return -1;
    }
    for (i = 0; i < object_count; ++i) {
        uint32_t symbol_count = (uint32_t)(objects[i].symtab_size / objects[i].symtab_entsize);
        uint32_t symbol_index;
        size_t section_index;

        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];

            if (section->live && rt_strcmp(section_name(&objects[i], section->index), query) == 0) {
                found = 1;
                if (rt_write_cstr(fd, "section ") != 0 || rt_write_cstr(fd, query) != 0 || rt_write_cstr(fd, " in ") != 0 ||
                    rt_write_cstr(fd, objects[i].path) != 0 || rt_write_cstr(fd, " because ") != 0 || rt_write_cstr(fd, section->why) != 0 || rt_write_cstr(fd, "\n") != 0) {
                    return -1;
                }
                if (write_live_chain(fd, objects, object_count, i, section_index) != 0) {
                    return -1;
                }
            }
        }
        for (symbol_index = 0; symbol_index < symbol_count; ++symbol_index) {
            const unsigned char *symbol = symbol_entry(&objects[i], symbol_index);
            const LinkSection *section;
            uint16_t shndx;

            if (symbol == 0 || rt_strcmp(symbol_name(&objects[i], symbol), query) != 0) {
                continue;
            }
            shndx = read_u16(symbol + 6);
            section = shndx != SHN_ABS && shndx != SHN_UNDEF ? find_link_section_const(&objects[i], shndx) : 0;
            if (section != 0 && section->live) {
                size_t live_section_index;

                found = 1;
                if (rt_write_cstr(fd, "symbol ") != 0 || rt_write_cstr(fd, query) != 0 || rt_write_cstr(fd, " in ") != 0 ||
                    rt_write_cstr(fd, section_name(&objects[i], section->index)) != 0 || rt_write_cstr(fd, " from ") != 0 ||
                    rt_write_cstr(fd, objects[i].path) != 0 || rt_write_cstr(fd, " because ") != 0 || rt_write_cstr(fd, section->why) != 0 || rt_write_cstr(fd, "\n") != 0) {
                    return -1;
                }
                live_section_index = (size_t)(section - objects[i].sections);
                if (write_live_chain(fd, objects, object_count, i, live_section_index) != 0) {
                    return -1;
                }
            }
        }
    }
    if (!found && rt_write_cstr(fd, "not live\n") != 0) {
        return -1;
    }
    return 0;
}

int compiler_link_elf64_x86_64_static_options(const char *const *object_paths,
                                              size_t object_count,
                                              const char *output_path,
                                              const CompilerLinkerOptions *options,
                                              char *error_out,
                                              size_t error_size) {
    uint64_t text_size;
    uint64_t data_size;
    uint64_t bss_size;
    uint64_t text_file_offset;
    uint64_t data_file_offset;
    uint64_t bss_vaddr_offset;
    uint64_t file_size;
    uint64_t memory_size;
    int has_writable_segment;
    uint64_t header_size;
    uint64_t padding_size;
    uint64_t entry;
    int start_index;
    int fd;
    size_t loaded_object_count = 0U;
    size_t i;
    unsigned char *output;
    int tiny = options != 0 && options->tiny != 0;
    int gc_sections = options != 0 && options->gc_sections != 0;
    const char *entry_symbol = linker_entry_symbol(options);

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
    if (gc_sections) {
        if (mark_live_sections(linker_objects, object_count, entry_symbol, error_out, error_size) != 0) {
            return -1;
        }
    } else {
        if (mark_live_objects(linker_objects, object_count, entry_symbol, error_out, error_size) != 0) {
            return -1;
        }
        mark_all_sections_in_live_objects(linker_objects, object_count);
    }
    if (options != 0 && options->icf_safe) {
        fold_identical_sections(linker_objects, object_count);
    }

    layout_objects(linker_objects, object_count, &text_size, &data_size, &bss_size);
    has_writable_segment = data_size != 0 || bss_size != 0;
    header_size = ELF64_EHDR_SIZE + ((uint64_t)((tiny || !has_writable_segment) ? 1U : 2U) * ELF64_PHDR_SIZE);
    text_file_offset = align_u64(header_size, 16U);
    if (has_writable_segment) {
        data_file_offset = align_u64(text_file_offset + text_size, tiny ? 16U : 0x1000U);
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
    padding_size = text_file_offset - header_size;
    if (has_writable_segment && data_file_offset > text_file_offset + text_size) {
        padding_size += data_file_offset - (text_file_offset + text_size);
    }
    output = (unsigned char *)rt_malloc((size_t)file_size);
    if (output == 0) {
        set_link_error(error_out, error_size, "failed to allocate linker output", output_path);
        return -1;
    }
    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!linker_objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < linker_objects[i].section_count; ++section_index) {
            LinkSection *section = &linker_objects[i].sections[section_index];

            if (section->kind == LINK_SECTION_TEXT) {
                section->out_offset += text_file_offset;
            } else if (section->kind == LINK_SECTION_DATA) {
                section->out_offset += data_file_offset;
            } else if (section->kind == LINK_SECTION_BSS) {
                section->out_offset += bss_vaddr_offset;
            }
        }
    }

    rt_memset(output, 0, (size_t)file_size);
    copy_sections(linker_objects, object_count, output);
    if (collect_globals(linker_objects, object_count, error_out, error_size) != 0) {
        rt_free(output);
        return -1;
    }
    start_index = find_global(entry_symbol);
    if (start_index < 0) {
        set_link_error(error_out, error_size, "undefined entry symbol", entry_symbol);
        rt_free(output);
        return -1;
    }
    entry = linker_globals[start_index].value;
    if (apply_relocations(linker_objects, object_count, output, error_out, error_size) != 0) {
        rt_free(output);
        return -1;
    }
    file_size = trim_trailing_zero_bytes(output, file_size, (!tiny && has_writable_segment) ? data_file_offset : header_size);
    write_elf_header(output, entry, text_file_offset, text_size, data_file_offset, data_size, bss_size, file_size, memory_size, tiny);

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
    if (options != 0 && options->map_path != 0 && options->map_path[0] != '\0') {
        if (write_link_map(options->map_path,
                           linker_objects,
                           object_count,
                           output_path,
                           entry_symbol,
                           entry,
                           text_size,
                           data_size,
                           bss_size,
                           file_size,
                           memory_size,
                           tiny,
                           gc_sections,
                           error_out,
                           error_size) != 0) {
            return -1;
        }
    }
    if (options != 0 && options->stats) {
        if (write_link_stats(1,
                             linker_objects,
                             object_count,
                             text_size,
                             data_size,
                             bss_size,
                             file_size,
                             memory_size,
                             header_size,
                             padding_size,
                             tiny,
                             gc_sections) != 0) {
            set_link_error(error_out, error_size, "failed to write linker stats", output_path);
            return -1;
        }
    }
    if (options != 0 && options->print_gc_sections) {
        if (write_gc_sections(1, linker_objects, object_count) != 0) {
            set_link_error(error_out, error_size, "failed to write discarded section report", output_path);
            return -1;
        }
    }
    if (options != 0 && options->why_live != 0 && options->why_live[0] != '\0') {
        if (write_why_live(1, linker_objects, object_count, options->why_live) != 0) {
            set_link_error(error_out, error_size, "failed to write why-live report", options->why_live);
            return -1;
        }
    }
    return 0;
}

int compiler_link_elf64_x86_64_static(const char *const *object_paths, size_t object_count, const char *output_path, char *error_out, size_t error_size) {
    return compiler_link_elf64_x86_64_static_options(object_paths, object_count, output_path, 0, error_out, error_size);
}
