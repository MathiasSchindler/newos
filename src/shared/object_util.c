#include "object_util.h"
#include "archive_util.h"
#include "runtime.h"
#include "tool_util.h"

#define OBJECT_MACHO_MAGIC_64 0xfeedfacfU
#define OBJECT_MACHO_LC_SEGMENT_64 0x19U
#define OBJECT_MACHO_LC_SYMTAB 0x2U

int object_read_region(int fd, unsigned long long base, unsigned long long object_size, unsigned long long offset, unsigned char *buffer, size_t count) {
    if (object_size != 0ULL && ((unsigned long long)count > object_size || offset > object_size - (unsigned long long)count)) {
        return -1;
    }
    return archive_read_region(fd, base, offset, buffer, count);
}

int object_macho_select_fat_slice(int fd, unsigned int preferred_cputype, unsigned int max_arches, unsigned long long *offset_out, unsigned long long *size_out) {
    unsigned char header[8];
    unsigned int magic;
    unsigned int arch_count;
    unsigned int index;

    *offset_out = 0ULL;
    if (size_out != 0) {
        *size_out = 0ULL;
    }
    if (archive_read_file_region(fd, 0ULL, header, sizeof(header)) != 0) return -1;
    magic = tool_read_u32_le(header + 0);
    if (magic != OBJECT_MACHO_FAT_MAGIC_LE && magic != OBJECT_MACHO_FAT_MAGIC_64_LE) return -1;
    arch_count = tool_read_u32_be(header + 4);
    if (arch_count > max_arches) return -1;
    for (index = 0U; index < arch_count; ++index) {
        unsigned char raw[32];
        unsigned int entry_size = magic == OBJECT_MACHO_FAT_MAGIC_64_LE ? 32U : 20U;
        unsigned long long entry = 8ULL + (unsigned long long)index * (unsigned long long)entry_size;
        unsigned int cputype;
        unsigned long long slice_offset;
        unsigned long long slice_size;

        if (archive_read_file_region(fd, entry, raw, entry_size) != 0) return -1;
        cputype = tool_read_u32_be(raw + 0);
        slice_offset = magic == OBJECT_MACHO_FAT_MAGIC_64_LE ? tool_read_u64_be(raw + 8) : (unsigned long long)tool_read_u32_be(raw + 8);
        slice_size = magic == OBJECT_MACHO_FAT_MAGIC_64_LE ? tool_read_u64_be(raw + 16) : (unsigned long long)tool_read_u32_be(raw + 12);
        if (cputype == preferred_cputype || index == 0U) {
            *offset_out = slice_offset;
            if (size_out != 0) {
                *size_out = slice_size;
            }
            if (cputype == preferred_cputype) return 0;
        }
    }
    return *offset_out != 0ULL ? 0 : -1;
}

int object_macho_parse_header(int fd, unsigned long long base, unsigned long long object_size, ObjectMachHeaderInfo *info) {
    unsigned char header[32];
    unsigned int magic;

    if (object_read_region(fd, base, object_size, 0ULL, header, sizeof(header)) != 0) {
        return -1;
    }
    magic = tool_read_u32_le(header + 0);
    if (magic != OBJECT_MACHO_MAGIC_64) {
        return -1;
    }

    info->magic = magic;
    info->cputype = tool_read_u32_le(header + 4);
    info->cpusubtype = tool_read_u32_le(header + 8);
    info->filetype = tool_read_u32_le(header + 12);
    info->ncmds = tool_read_u32_le(header + 16);
    info->sizeofcmds = tool_read_u32_le(header + 20);
    info->flags = tool_read_u32_le(header + 24);
    return 0;
}

int object_macho_load_layout(int fd,
                             unsigned long long base,
                             unsigned long long object_size,
                             const ObjectMachHeaderInfo *header,
                             ObjectMachSectionInfo *sections,
                             unsigned int max_sections,
                             unsigned int max_commands,
                             unsigned int *section_count_out,
                             ObjectMachSymtabInfo *symtab) {
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;
    unsigned int section_count = 0U;

    if (section_count_out != 0) {
        *section_count_out = 0U;
    }
    if (symtab != 0) {
        symtab->symoff = 0U;
        symtab->nsyms = 0U;
        symtab->stroff = 0U;
        symtab->strsize = 0U;
    }
    if (header->ncmds > max_commands) {
        return -1;
    }

    for (command_index = 0U; command_index < header->ncmds; ++command_index) {
        unsigned char command_header[24];
        unsigned int command;
        unsigned int command_size;

        if (object_read_region(fd, base, object_size, command_offset, command_header, 8U) != 0) {
            return -1;
        }
        command = tool_read_u32_le(command_header + 0);
        command_size = tool_read_u32_le(command_header + 4);
        if (command_size < 8U) {
            return -1;
        }

        if (command == OBJECT_MACHO_LC_SEGMENT_64 && command_size >= 72U && sections != 0) {
            unsigned char segment[72];
            unsigned int nsects;
            unsigned int section_index;
            char segment_name[17];

            if (object_read_region(fd, base, object_size, command_offset, segment, sizeof(segment)) != 0) {
                return -1;
            }
            tool_copy_printable_bytes(segment_name, sizeof(segment_name), segment + 8, 16U);
            nsects = tool_read_u32_le(segment + 64);
            if (nsects > (command_size - 72U) / 80U) {
                return -1;
            }
            for (section_index = 0U; section_index < nsects; ++section_index) {
                unsigned char raw[80];
                unsigned long long section_offset = command_offset + 72ULL + ((unsigned long long)section_index * 80ULL);

                if (section_count >= max_sections) {
                    return -1;
                }
                if (object_read_region(fd, base, object_size, section_offset, raw, sizeof(raw)) != 0) {
                    return -1;
                }
                tool_copy_printable_bytes(sections[section_count].section, sizeof(sections[section_count].section), raw + 0, 16U);
                tool_copy_printable_bytes(sections[section_count].segment, sizeof(sections[section_count].segment), raw + 16, 16U);
                if (sections[section_count].segment[0] == '\0') {
                    rt_copy_string(sections[section_count].segment, sizeof(sections[section_count].segment), segment_name);
                }
                sections[section_count].addr = tool_read_u64_le(raw + 32);
                sections[section_count].size = tool_read_u64_le(raw + 40);
                sections[section_count].offset = tool_read_u32_le(raw + 48);
                sections[section_count].align = tool_read_u32_le(raw + 52);
                sections[section_count].reloff = tool_read_u32_le(raw + 56);
                sections[section_count].nreloc = tool_read_u32_le(raw + 60);
                sections[section_count].flags = tool_read_u32_le(raw + 64);
                sections[section_count].reserved1 = tool_read_u32_le(raw + 68);
                sections[section_count].reserved2 = tool_read_u32_le(raw + 72);
                section_count += 1U;
            }
        } else if (command == OBJECT_MACHO_LC_SYMTAB && command_size >= 24U && symtab != 0) {
            if (object_read_region(fd, base, object_size, command_offset, command_header, sizeof(command_header)) != 0) {
                return -1;
            }
            symtab->symoff = tool_read_u32_le(command_header + 8);
            symtab->nsyms = tool_read_u32_le(command_header + 12);
            symtab->stroff = tool_read_u32_le(command_header + 16);
            symtab->strsize = tool_read_u32_le(command_header + 20);
        }
        command_offset += (unsigned long long)command_size;
    }

    if (section_count_out != 0) {
        *section_count_out = section_count;
    }
    return 0;
}

int object_macho_load_sections(int fd,
                               unsigned long long base,
                               unsigned long long object_size,
                               const ObjectMachHeaderInfo *header,
                               ObjectMachSectionInfo *sections,
                               unsigned int max_sections,
                               unsigned int max_commands,
                               unsigned int *section_count_out) {
    return object_macho_load_layout(fd, base, object_size, header, sections, max_sections, max_commands, section_count_out, 0);
}

int object_macho_load_symtab(int fd,
                             unsigned long long base,
                             unsigned long long object_size,
                             const ObjectMachHeaderInfo *header,
                             ObjectMachSymtabInfo *symtab,
                             unsigned int max_commands) {
    return object_macho_load_layout(fd, base, object_size, header, 0, 0U, max_commands, 0, symtab);
}

int object_elf_load_sections(int fd,
                             unsigned long long base,
                             unsigned long long object_size,
                             unsigned long long section_header_offset,
                             unsigned int section_count,
                             unsigned int section_entry_size,
                             ObjectElfSectionInfo *sections,
                             unsigned int max_sections) {
    unsigned int index;

    if (section_count == 0U) return 0;
    if (sections == 0 || section_count > max_sections || section_entry_size < 64U) return -1;

    for (index = 0U; index < section_count; ++index) {
        unsigned char raw[64];
        unsigned long long offset = section_header_offset + ((unsigned long long)index * (unsigned long long)section_entry_size);

        if (object_read_region(fd, base, object_size, offset, raw, sizeof(raw)) != 0) return -1;
        sections[index].name = tool_read_u32_le(raw + 0);
        sections[index].type = tool_read_u32_le(raw + 4);
        sections[index].flags = tool_read_u64_le(raw + 8);
        sections[index].addr = tool_read_u64_le(raw + 16);
        sections[index].offset = tool_read_u64_le(raw + 24);
        sections[index].size = tool_read_u64_le(raw + 32);
        sections[index].link = tool_read_u32_le(raw + 40);
        sections[index].entsize = tool_read_u64_le(raw + 56);
    }
    return 0;
}

int object_elf_load_name_table(int fd,
                               unsigned long long base,
                               unsigned long long object_size,
                               unsigned int shstrndx,
                               unsigned int shnum,
                               unsigned long long section_offset,
                               unsigned long long section_size,
                               char *buffer,
                               size_t buffer_capacity,
                               size_t *size_out) {
    size_t to_read;

    *size_out = 0U;
    if (shstrndx >= shnum) {
        return 0;
    }
    if (buffer_capacity == 0U) {
        return 0;
    }

    to_read = (size_t)(section_size < (unsigned long long)(buffer_capacity - 1U) ? section_size : (unsigned long long)(buffer_capacity - 1U));
    if (to_read == 0U) {
        return 0;
    }
    if (object_read_region(fd, base, object_size, section_offset, (unsigned char *)buffer, to_read) != 0) {
        return -1;
    }
    buffer[to_read] = '\0';
    *size_out = to_read;
    return 0;
}
