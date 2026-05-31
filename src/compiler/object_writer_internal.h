#ifndef NEWOS_COMPILER_OBJECT_WRITER_INTERNAL_H
#define NEWOS_COMPILER_OBJECT_WRITER_INTERNAL_H

#include "object_writer.h"

#include "platform.h"
#include "runtime.h"

typedef signed char int8_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;

#define OBJECT_WRITER_MAX_TEXT 262144
#define OBJECT_WRITER_MAX_DATA 65536
#define OBJECT_WRITER_MAX_SYMBOLS 1024
#define OBJECT_WRITER_SYMBOL_INDEX_CAPACITY 2048
#define OBJECT_WRITER_MAX_LABELS 2048
#define OBJECT_WRITER_LABEL_INDEX_CAPACITY 4096
#define OBJECT_WRITER_MAX_FIXUPS 4096
#define OBJECT_WRITER_MAX_RELOCS 4096
#define OBJECT_WRITER_MAX_OUTPUT (OBJECT_WRITER_MAX_TEXT + OBJECT_WRITER_MAX_DATA + 65536)

#define ELFCLASS64 2U
#define ELFDATA2LSB 1U
#define EV_CURRENT 1U
#define ET_REL 1U
#define EM_X86_64 62U

#define SHT_NULL 0U
#define SHT_PROGBITS 1U
#define SHT_SYMTAB 2U
#define SHT_STRTAB 3U
#define SHT_RELA 4U
#define SHT_NOBITS 8U

#define SHF_WRITE 0x1ULL
#define SHF_ALLOC 0x2ULL
#define SHF_EXECINSTR 0x4ULL

#define STB_LOCAL 0U
#define STB_GLOBAL 1U
#define STT_NOTYPE 0U
#define STT_OBJECT 1U
#define STT_FUNC 2U
#define STT_SECTION 3U

#define ELF64_SYMBOL_SIZE 24U
#define ELF64_SECTION_HEADER_SIZE 64U

#define R_X86_64_64 1U
#define R_X86_64_PC32 2U
#define R_X86_64_PLT32 4U

typedef enum {
    OBJECT_SECTION_NONE = 0,
    OBJECT_SECTION_TEXT = 1,
    OBJECT_SECTION_DATA = 2,
    OBJECT_SECTION_BSS = 3
} ObjectSection;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    ObjectSection section;
    size_t offset;
    int defined;
    int global;
    int is_function;
    uint32_t name_offset;
    unsigned int sym_index;
} ObjectSymbol;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    ObjectSection section;
    size_t offset;
} ObjectLabel;

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    size_t offset;
    uint32_t type;
    int64_t addend;
} ObjectFixup;

typedef struct {
    size_t offset;
    ObjectSection section;
    char name[COMPILER_IR_NAME_CAPACITY];
    uint32_t type;
    int64_t addend;
} ObjectRelocation;

typedef struct {
    CompilerObjectWriter *writer;
    unsigned char text[OBJECT_WRITER_MAX_TEXT];
    unsigned char data[OBJECT_WRITER_MAX_DATA];
    size_t text_size;
    size_t data_size;
    size_t bss_size;
    ObjectSymbol symbols[OBJECT_WRITER_MAX_SYMBOLS];
    unsigned int symbol_index[OBJECT_WRITER_SYMBOL_INDEX_CAPACITY];
    size_t symbol_count;
    ObjectLabel labels[OBJECT_WRITER_MAX_LABELS];
    unsigned int label_index[OBJECT_WRITER_LABEL_INDEX_CAPACITY];
    size_t label_count;
    ObjectFixup fixups[OBJECT_WRITER_MAX_FIXUPS];
    size_t fixup_count;
    ObjectRelocation relocs[OBJECT_WRITER_MAX_RELOCS];
    size_t reloc_count;
    ObjectSection current_section;
} ObjectAssembler;

void object_writer_set_error(CompilerObjectWriter *writer, const char *message);
int object_writer_find_symbol(const ObjectAssembler *assembler, const char *name);
int object_writer_get_symbol(ObjectAssembler *assembler, const char *name);
int object_writer_find_label(const ObjectAssembler *assembler, const char *name);
int object_writer_add_label(ObjectAssembler *assembler, const char *name, ObjectSection section, size_t offset);
int object_writer_add_fixup(ObjectAssembler *assembler, const char *name, size_t offset, uint32_t type, int64_t addend);
int object_writer_add_relocation(ObjectAssembler *assembler, ObjectSection section, const char *name, size_t offset, uint32_t type, int64_t addend);

#endif