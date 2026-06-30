#ifndef NEWOS_OBJECT_UTIL_H
#define NEWOS_OBJECT_UTIL_H

#include <stddef.h>

#define OBJECT_MACHO_FAT_MAGIC_LE 0xbebafecaU
#define OBJECT_MACHO_FAT_MAGIC_64_LE 0xbfbafecaU
#define OBJECT_MACHO_CPU_TYPE_ARM64 0x0100000cU

typedef struct {
    unsigned int magic;
    unsigned int cputype;
    unsigned int cpusubtype;
    unsigned int filetype;
    unsigned int ncmds;
    unsigned int sizeofcmds;
    unsigned int flags;
} ObjectMachHeaderInfo;

typedef struct {
    char segment[17];
    char section[17];
    unsigned long long addr;
    unsigned long long size;
    unsigned int offset;
    unsigned int align;
    unsigned int reloff;
    unsigned int nreloc;
    unsigned int flags;
    unsigned int reserved1;
    unsigned int reserved2;
} ObjectMachSectionInfo;

typedef struct {
    unsigned int symoff;
    unsigned int nsyms;
    unsigned int stroff;
    unsigned int strsize;
} ObjectMachSymtabInfo;

typedef struct {
    unsigned int name;
    unsigned int type;
    unsigned long long flags;
    unsigned long long addr;
    unsigned long long offset;
    unsigned long long size;
    unsigned int link;
    unsigned long long entsize;
} ObjectElfSectionInfo;

int object_read_region(int fd, unsigned long long base, unsigned long long object_size, unsigned long long offset, unsigned char *buffer, size_t count);
int object_macho_select_fat_slice(int fd, unsigned int preferred_cputype, unsigned int max_arches, unsigned long long *offset_out, unsigned long long *size_out);
int object_macho_parse_header(int fd, unsigned long long base, unsigned long long object_size, ObjectMachHeaderInfo *info);
int object_macho_load_sections(int fd, unsigned long long base, unsigned long long object_size, const ObjectMachHeaderInfo *header, ObjectMachSectionInfo *sections, unsigned int max_sections, unsigned int max_commands, unsigned int *section_count_out);
int object_macho_load_symtab(int fd, unsigned long long base, unsigned long long object_size, const ObjectMachHeaderInfo *header, ObjectMachSymtabInfo *symtab, unsigned int max_commands);
int object_macho_load_layout(int fd, unsigned long long base, unsigned long long object_size, const ObjectMachHeaderInfo *header, ObjectMachSectionInfo *sections, unsigned int max_sections, unsigned int max_commands, unsigned int *section_count_out, ObjectMachSymtabInfo *symtab);
int object_elf_load_sections(int fd, unsigned long long base, unsigned long long object_size, unsigned long long section_header_offset, unsigned int section_count, unsigned int section_entry_size, ObjectElfSectionInfo *sections, unsigned int max_sections);
int object_elf_load_name_table(int fd, unsigned long long base, unsigned long long object_size, unsigned int shstrndx, unsigned int shnum, unsigned long long section_offset, unsigned long long section_size, char *buffer, size_t buffer_capacity, size_t *size_out);

#endif
