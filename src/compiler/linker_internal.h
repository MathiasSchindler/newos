#ifndef NEWOS_LINKER_INTERNAL_H
#define NEWOS_LINKER_INTERNAL_H

#include "linker.h"
#include "platform.h"
#include "runtime.h"
#include "source.h"

/* ── capacity limits ────────────────────────────────────────────────────── */
#define LINKER_MAX_OBJECTS         320
#define LINKER_MAX_OBJECT_SIZE     (16U * 1024U * 1024U)
#define LINKER_MAX_ARCHIVE_SIZE    (64U * 1024U * 1024U)
#define LINKER_MAX_OUTPUT          (64U * 1024U * 1024U)
#define LINKER_MAX_MEMORY          (512U * 1024U * 1024U)
#define LINKER_MAX_GLOBALS         8192
#define LINKER_MAX_SECTIONS        512
#define LINKER_MAX_RELA_SECTIONS   512
#define LINKER_BASE_VADDR          0x400000ULL
#define LINKER_AR_HEADER_SIZE      60U
#define LINKER_NO_INDEX            ((size_t)-1)
#define LINKER_UNPLACED_OFFSET     (~0ULL)

/* ── feature flags ───────────────────────────────────────────────────────── */
#ifndef COMPILER_LINKER_ENABLE_REPORTING
#define COMPILER_LINKER_ENABLE_REPORTING 1
#endif

#ifndef COMPILER_LINKER_ENABLE_CONST_MERGE
#define COMPILER_LINKER_ENABLE_CONST_MERGE 0
#endif

/* ── ELF constants ───────────────────────────────────────────────────────── */
#define ELFCLASS64    2U
#define ELFDATA2LSB   1U
#define EV_CURRENT    1U
#define ET_EXEC       2U
#define ET_REL        1U
#define EM_X86_64     62U
#define PT_LOAD       1U
#define PF_X          1U
#define PF_W          2U
#define PF_R          4U
#define SHT_PROGBITS  1U
#define SHT_SYMTAB    2U
#define SHT_STRTAB    3U
#define SHT_RELA      4U
#define SHT_NOBITS    8U
#define SHF_WRITE     1ULL
#define SHF_ALLOC     2ULL
#define SHF_EXECINSTR 4ULL
#define SHF_MERGE     16ULL
#define SHF_STRINGS   32ULL
#define SHN_UNDEF     0U
#define SHN_ABS       0xfff1U
#define STB_GLOBAL    1U
#define R_X86_64_NONE  0U
#define R_X86_64_64    1U
#define R_X86_64_PC32  2U
#define R_X86_64_PLT32 4U
#define R_X86_64_32    10U
#define R_X86_64_32S   11U

/* ── ELF structure sizes ─────────────────────────────────────────────────── */
#define ELF64_EHDR_SIZE 64U
#define ELF64_PHDR_SIZE 56U
#define ELF64_SHDR_SIZE 64U
#define ELF64_SYM_SIZE  24U
#define ELF64_RELA_SIZE 24U

/* ── integer types ───────────────────────────────────────────────────────── */
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef long long          int64_t;
typedef int                int32_t;

typedef enum {
    LINK_LTO_NONE = 0,
    LINK_LTO_GCC,
    LINK_LTO_LLVM
} LinkLtoKind;

/* ── core data types ─────────────────────────────────────────────────────── */
typedef enum {
    LINK_SECTION_NONE = 0,
    LINK_SECTION_TEXT,
    LINK_SECTION_DATA,
    LINK_SECTION_BSS
} LinkSectionKind;

typedef struct {
    uint16_t      index;
    uint32_t      type;
    uint64_t      flags;
    uint64_t      offset;
    uint64_t      size;
    uint64_t      align;
    uint64_t      out_offset;
    LinkSectionKind kind;
    int           live;
    int           folded;
    size_t        fold_object_index;
    size_t        fold_section_index;
    uint64_t      fold_addend;
    size_t        parent_object_index;
    size_t        parent_section_index;
    char          why[COMPILER_PATH_CAPACITY];
} LinkSection;

typedef struct {
    uint16_t index;
    uint16_t target_index;
    uint64_t offset;
    uint64_t size;
    uint64_t entsize;
} LinkRelaSection;

typedef struct {
    char            path[COMPILER_PATH_CAPACITY];
    unsigned char  *file;
    size_t          size;
    uint64_t        shoff;
    uint16_t        shentsize;
    uint16_t        shnum;
    uint16_t        shstrndx;
    uint16_t        text_index;
    uint16_t        data_index;
    uint16_t        bss_index;
    uint16_t        symtab_index;
    uint16_t        strtab_index;
    uint16_t        rela_text_index;
    uint16_t        rela_data_index;
    uint64_t        text_offset;
    uint64_t        text_size;
    uint64_t        data_offset;
    uint64_t        data_size;
    uint64_t        bss_size;
    uint64_t        symtab_offset;
    uint64_t        symtab_size;
    uint64_t        symtab_entsize;
    uint64_t        strtab_offset;
    uint64_t        strtab_size;
    uint64_t        rela_text_offset;
    uint64_t        rela_text_size;
    uint64_t        rela_text_entsize;
    uint64_t        rela_data_offset;
    uint64_t        rela_data_size;
    uint64_t        rela_data_entsize;
    LinkSection    *sections;
    size_t          section_count;
    size_t          section_capacity;
    LinkRelaSection *rela_sections;
    size_t          rela_section_count;
    size_t          rela_section_capacity;
    int             is_lto_ir;
    uint64_t        out_text_offset;
    uint64_t        out_data_offset;
    uint64_t        out_bss_offset;
    int             live;
} LinkObject;

typedef struct {
    char     name[COMPILER_PATH_CAPACITY];
    uint64_t value;
} LinkGlobal;

typedef struct {
    char   name[COMPILER_PATH_CAPACITY];
    size_t object_index;
} LinkDefinedSymbol;

typedef struct {
    size_t   object_index;
    size_t   section_index;
    uint64_t offset;
    uint64_t length;
    int      emitted;
} LinkMergeStringRecord;

/* ── global state (defined in their owning modules) ──────────────────────── */
extern LinkObject        linker_objects[LINKER_MAX_OBJECTS];
extern LinkGlobal        linker_globals[LINKER_MAX_GLOBALS];
extern LinkDefinedSymbol linker_defined_symbols[LINKER_MAX_GLOBALS];
extern size_t            linker_global_count;
extern size_t            linker_defined_symbol_count;
extern unsigned char    *linker_merge_string_pool;
extern uint64_t          linker_merge_string_pool_size;
extern uint64_t          linker_merge_string_pool_capacity;
extern int               linker_merge_string_pool_active;
extern size_t            linker_merge_master_object_index;
extern size_t            linker_merge_master_section_index;
extern uint64_t          linker_merge_master_input_size;
#if COMPILER_LINKER_ENABLE_CONST_MERGE
extern unsigned char    *linker_merge_const_pool;
extern uint64_t          linker_merge_const_pool_size;
extern uint64_t          linker_merge_const_pool_capacity;
extern int               linker_merge_const_pool_active;
extern size_t            linker_merge_const_master_object_index;
extern size_t            linker_merge_const_master_section_index;
extern uint64_t          linker_merge_const_master_input_size;
#endif

/* ── inline byte-level utilities ─────────────────────────────────────────── */
static inline uint16_t read_u16(const unsigned char *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8U));
}

static inline uint32_t read_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static inline uint64_t read_u64(const unsigned char *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8U)  | ((uint64_t)p[2] << 16U) | ((uint64_t)p[3] << 24U) |
           ((uint64_t)p[4] << 32U) | ((uint64_t)p[5] << 40U) | ((uint64_t)p[6] << 48U) | ((uint64_t)p[7] << 56U);
}

static inline int64_t read_i64(const unsigned char *p) {
    return (int64_t)read_u64(p);
}

static inline void write_u16(unsigned char *p, uint16_t value) {
    p[0] = (unsigned char)(value & 0xffU);
    p[1] = (unsigned char)((value >> 8U) & 0xffU);
}

static inline void write_u32(unsigned char *p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xffU);
    p[1] = (unsigned char)((value >> 8U)  & 0xffU);
    p[2] = (unsigned char)((value >> 16U) & 0xffU);
    p[3] = (unsigned char)((value >> 24U) & 0xffU);
}

static inline void write_u64(unsigned char *p, uint64_t value) {
    p[0] = (unsigned char)(value & 0xffU);
    p[1] = (unsigned char)((value >> 8U)  & 0xffU);
    p[2] = (unsigned char)((value >> 16U) & 0xffU);
    p[3] = (unsigned char)((value >> 24U) & 0xffU);
    p[4] = (unsigned char)((value >> 32U) & 0xffU);
    p[5] = (unsigned char)((value >> 40U) & 0xffU);
    p[6] = (unsigned char)((value >> 48U) & 0xffU);
    p[7] = (unsigned char)((value >> 56U) & 0xffU);
}

static inline uint64_t align_u64(uint64_t value, uint64_t alignment) {
    if (alignment <= 1ULL) return value;
    return (value + alignment - 1ULL) & ~(alignment - 1ULL);
}

static inline int range_valid(uint64_t offset, uint64_t size, uint64_t limit) {
    return size <= limit && offset <= limit - size;
}

/* ── linker_util.c ───────────────────────────────────────────────────────── */
void     set_link_error(char *error_out, size_t error_size, const char *message, const char *detail);
int      ends_with_text(const char *text, const char *suffix);
uint64_t trim_trailing_zero_bytes(const unsigned char *data, uint64_t size, uint64_t minimum_size);

/* ── linker_elf.c ────────────────────────────────────────────────────────── */
const unsigned char  *section_header(const LinkObject *object, uint16_t index);
const char           *section_name(const LinkObject *object, uint16_t index);
int                   section_is(const LinkObject *object, uint16_t index, const char *name);
LinkSection          *find_link_section(LinkObject *object, uint16_t index);
const LinkSection    *find_link_section_const(const LinkObject *object, uint16_t index);
const unsigned char  *symbol_entry(const LinkObject *object, uint32_t index);
const char           *symbol_name(const LinkObject *object, const unsigned char *symbol);
int                   section_has_relocations(const LinkObject *object, uint16_t section_index);
const LinkRelaSection *find_rela_section_const(const LinkObject *object, uint16_t section_index);

/* ── linker_object.c ─────────────────────────────────────────────────────── */
int read_file_alloc(const char *path, size_t capacity, unsigned char **file_out, size_t *size_out, char *error_out, size_t error_size);
int load_object(LinkObject *object, const char *path, char *error_out, size_t error_size);
int load_archive(const char *path, LinkObject *objects, size_t *object_count, char *error_out, size_t error_size);

/* ── linker_symbols.c ────────────────────────────────────────────────────── */
int  linker_find_global(const char *name);
int  linker_add_global(const char *name, uint64_t value, char *error_out, size_t error_size);
int  find_defined_symbol_owner(const char *name);
int  add_defined_symbol_owner(const char *name, size_t object_index, char *error_out, size_t error_size);
int  collect_defined_symbol_owners(LinkObject *objects, size_t object_count, char *error_out, size_t error_size);

/* ── linker_gc.c ─────────────────────────────────────────────────────────── */
int  mark_live_objects(LinkObject *objects, size_t object_count, const char *entry_symbol, char *error_out, size_t error_size);
int  mark_live_sections(LinkObject *objects, size_t object_count, const char *entry_symbol, char *error_out, size_t error_size);
void mark_all_sections_in_live_objects(LinkObject *objects, size_t object_count);

/* ── linker_merge.c ──────────────────────────────────────────────────────── */
void     reset_merge_string_pool(void);
int      section_uses_merge_string_pool(const LinkSection *section);
uint64_t merge_string_input_size(const LinkSection *section);
int      translate_merge_string_offset(const LinkObject *object, const LinkSection *section, uint64_t input_offset, uint64_t *output_offset_out);
int      merge_string_sections(LinkObject *objects, size_t object_count, char *error_out, size_t error_size);
#if COMPILER_LINKER_ENABLE_CONST_MERGE
void     reset_merge_const_pool(void);
int      section_uses_merge_const_pool(const LinkSection *section);
uint64_t merge_const_input_size(const LinkSection *section);
int      translate_merge_const_offset(const LinkObject *object, const LinkSection *section, uint64_t input_offset, uint64_t *output_offset_out);
int      merge_const_sections(LinkObject *objects, size_t object_count, char *error_out, size_t error_size);
#endif

/* ── linker_icf.c ────────────────────────────────────────────────────────── */
void fold_identical_sections(LinkObject *objects, size_t object_count);

/* ── linker_reloc.c ──────────────────────────────────────────────────────── */
int collect_globals(LinkObject *objects, size_t object_count, char *error_out, size_t error_size);
int apply_relocations(LinkObject *objects, size_t object_count, unsigned char *output, char *error_out, size_t error_size);

/* ── linker_layout.c ─────────────────────────────────────────────────────── */
void     layout_objects(LinkObject *objects, size_t object_count, uint64_t *text_size_out, uint64_t *data_size_out, uint64_t *bss_size_out);
uint64_t max_live_section_alignment(const LinkObject *objects, size_t object_count, LinkSectionKind kind);
void     copy_sections(LinkObject *objects, size_t object_count, unsigned char *output);
void     write_elf_header(unsigned char *output, uint64_t entry, uint64_t text_file_offset, uint64_t text_size, uint64_t data_file_offset, uint64_t data_size, uint64_t bss_size, uint64_t file_size, uint64_t memory_size, int tiny);

/* ── linker_report.c ─────────────────────────────────────────────────────── */
#if COMPILER_LINKER_ENABLE_REPORTING
int write_link_stats(int fd, const LinkObject *objects, size_t object_count, uint64_t text_size, uint64_t data_size, uint64_t bss_size, uint64_t file_size, uint64_t memory_size, uint64_t header_size, uint64_t padding_size, int tiny, int gc_sections);
int write_link_map(const char *path, const LinkObject *objects, size_t object_count, const char *output_path, const char *entry_symbol, uint64_t entry, uint64_t text_size, uint64_t data_size, uint64_t bss_size, uint64_t file_size, uint64_t memory_size, int tiny, int gc_sections, char *error_out, size_t error_size);
int write_gc_sections(int fd, const LinkObject *objects, size_t object_count);
int write_why_live(int fd, const LinkObject *objects, size_t object_count, const char *query);
#endif

/* ── linker_lto.c ────────────────────────────────────────────────────────── */
LinkLtoKind detect_lto_ir_kind(const unsigned char *file, size_t size);
int detect_lto_ir(const unsigned char *file, size_t size);
int run_gcc_lto_prelink(const char *const *paths, size_t count, const char *entry_symbol, const char *lto_cc, const char *out_path, char *error_out, size_t error_size);
int run_clang_lto_prelink_elf64_x86_64(const char *const *paths, size_t count, const char *entry_symbol, const char *lto_cc, const char *out_path, char *error_out, size_t error_size);
int run_clang_lto_prelink_macho64_aarch64(const char *const *paths, size_t count, const char *entry_symbol, const char *lto_cc, const char *out_path, char *error_out, size_t error_size);

/* ── linker_macho.c ─────────────────────────────────────────────────────── */
int compiler_link_macho64_aarch64_static_options(const char *const *object_paths, size_t object_count, const char *output_path, const CompilerLinkerOptions *options, char *error_out, size_t error_size);

#endif /* NEWOS_LINKER_INTERNAL_H */
