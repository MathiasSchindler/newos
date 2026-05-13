#include "linker.h"

#include "platform.h"
#include "runtime.h"
#include "source.h"

#define LINKER_MAX_OBJECTS 320
#define LINKER_MAX_OBJECT_SIZE (1024U * 1024U)
#define LINKER_MAX_OUTPUT (6U * 1024U * 1024U)
#define LINKER_MAX_MEMORY (64U * 1024U * 1024U)
#define LINKER_MAX_GLOBALS 8192
#define LINKER_BASE_VADDR 0x400000ULL

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
    unsigned char file[LINKER_MAX_OBJECT_SIZE];
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

static LinkObject linker_objects[LINKER_MAX_OBJECTS];
static LinkGlobal linker_globals[LINKER_MAX_GLOBALS];
static unsigned char linker_output[LINKER_MAX_OUTPUT];
static size_t linker_global_count;

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

static int load_object(LinkObject *object, const char *path, char *error_out, size_t error_size) {
    int fd;
    long bytes_read;
    size_t total = 0;
    uint16_t i;

    rt_memset(object, 0, sizeof(*object));
    rt_copy_string(object->path, sizeof(object->path), path);
    fd = platform_open_read(path);
    if (fd < 0) {
        set_link_error(error_out, error_size, "failed to open object", path);
        return -1;
    }
    while ((bytes_read = platform_read(fd, object->file + total, sizeof(object->file) - total)) > 0) {
        total += (size_t)bytes_read;
        if (total == sizeof(object->file)) {
            break;
        }
    }
    (void)platform_close(fd);
    if (bytes_read < 0) {
        set_link_error(error_out, error_size, "failed to read object", path);
        return -1;
    }
    object->size = total;
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

static int find_defined_global_object(LinkObject *objects, size_t object_count, const char *name) {
    size_t i;

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

static void write_elf_header(unsigned char *output, uint64_t entry, uint64_t file_size, uint64_t memory_size) {
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
    write_u16(output + 56, 1U);
    write_u16(output + 58, 0U);
    write_u16(output + 60, 0U);
    write_u16(output + 62, 0U);

    write_u32(output + ELF64_EHDR_SIZE + 0, PT_LOAD);
    write_u32(output + ELF64_EHDR_SIZE + 4, PF_R | PF_W | PF_X);
    write_u64(output + ELF64_EHDR_SIZE + 8, 0U);
    write_u64(output + ELF64_EHDR_SIZE + 16, LINKER_BASE_VADDR);
    write_u64(output + ELF64_EHDR_SIZE + 24, LINKER_BASE_VADDR);
    write_u64(output + ELF64_EHDR_SIZE + 32, file_size);
    write_u64(output + ELF64_EHDR_SIZE + 40, memory_size);
    write_u64(output + ELF64_EHDR_SIZE + 48, 0x1000U);
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
    uint64_t entry;
    int start_index;
    int fd;
    size_t i;

    if (error_out != 0 && error_size > 0U) {
        error_out[0] = '\0';
    }
    if (object_count == 0 || object_count > LINKER_MAX_OBJECTS) {
        set_link_error(error_out, error_size, "invalid object count for native linker", "");
        return -1;
    }
    for (i = 0; i < object_count; ++i) {
        if (load_object(&linker_objects[i], object_paths[i], error_out, error_size) != 0) {
            return -1;
        }
    }
    if (mark_live_objects(linker_objects, object_count, error_out, error_size) != 0) {
        return -1;
    }

    layout_objects(linker_objects, object_count, &text_size, &data_size, &bss_size);
    text_file_offset = align_u64(ELF64_EHDR_SIZE + ELF64_PHDR_SIZE, 16U);
    data_file_offset = align_u64(text_file_offset + text_size, 8U);
    bss_vaddr_offset = align_u64(data_file_offset + data_size, 8U);
    file_size = data_file_offset + data_size;
    memory_size = bss_vaddr_offset + bss_size;
    if (file_size > sizeof(linker_output) || memory_size > LINKER_MAX_MEMORY) {
        set_link_error(error_out, error_size, "linked executable exceeds native linker capacity", output_path);
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

    rt_memset(linker_output, 0, (size_t)file_size);
    copy_sections(linker_objects, object_count, linker_output);
    if (collect_globals(linker_objects, object_count, error_out, error_size) != 0) {
        return -1;
    }
    start_index = find_global("_start");
    if (start_index < 0) {
        set_link_error(error_out, error_size, "undefined entry symbol", "_start");
        return -1;
    }
    entry = linker_globals[start_index].value;
    if (apply_relocations(linker_objects, object_count, linker_output, error_out, error_size) != 0) {
        return -1;
    }
    write_elf_header(linker_output, entry, file_size, memory_size);

    fd = platform_open_write(output_path, 0755U);
    if (fd < 0) {
        set_link_error(error_out, error_size, "failed to open output executable", output_path);
        return -1;
    }
    if (rt_write_all(fd, linker_output, (size_t)file_size) != 0) {
        (void)platform_close(fd);
        set_link_error(error_out, error_size, "failed to write output executable", output_path);
        return -1;
    }
    if (platform_close(fd) != 0) {
        set_link_error(error_out, error_size, "failed to close output executable", output_path);
        return -1;
    }
    (void)platform_change_mode(output_path, 0755U);
    return 0;
}
