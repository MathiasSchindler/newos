#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "archive_util.h"

#define STRIP_BUFFER_SIZE 4096U
#define STRIP_AR_HEADER_SIZE 60U

typedef enum {
    STRIP_FORMAT_UNKNOWN,
    STRIP_FORMAT_ELF64_LE,
    STRIP_FORMAT_MACHO64_LE,
    STRIP_FORMAT_PE_COFF,
    STRIP_FORMAT_ARCHIVE
} StripFormat;

typedef enum {
    STRIP_MODE_ALL,
    STRIP_MODE_DEBUG
} StripMode;

typedef struct {
    const char *output_path;
    StripMode mode;
    int dry_run;
    int verbose;
} StripOptions;

typedef struct {
    StripFormat format;
    unsigned long long input_size;
    unsigned long long output_size;
    unsigned long long copy_size;
    unsigned int pe_offset;
    unsigned int pe_symbol_offset;
    unsigned int pe_symbol_count;
    unsigned int pe_debug_directory_offset;
    unsigned int pe_debug_directory_size;
    unsigned short elf_type;
    unsigned int macho_ncmds;
    unsigned int macho_symoff;
    unsigned int macho_nsyms;
    unsigned int macho_stroff;
    unsigned int macho_strsize;
    unsigned int macho_code_signature_offset;
    unsigned int macho_code_signature_size;
    unsigned int archive_members;
    int patch_elf_header;
    int patch_pe_symbols;
    int patch_pe_debug_directory;
    int unsupported_relocatable;
    const char *format_name;
    const char *action;
} StripPlan;




static int copy_prefix(int input_fd, int output_fd, unsigned long long count) {
    unsigned char buffer[STRIP_BUFFER_SIZE];

    if (platform_seek(input_fd, 0, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }

    while (count > 0ULL) {
        size_t chunk = count > sizeof(buffer) ? sizeof(buffer) : (size_t)count;
        long bytes_read = platform_read(input_fd, buffer, chunk);
        if (bytes_read <= 0) {
            return -1;
        }
        if (rt_write_all(output_fd, buffer, (size_t)bytes_read) != 0) {
            return -1;
        }
        count -= (unsigned long long)bytes_read;
    }
    return 0;
}

static const char *mode_name(StripMode mode) {
    return mode == STRIP_MODE_DEBUG ? "strip-debug" : "strip-all";
}

static int checked_range(unsigned long long offset, unsigned long long size, unsigned long long file_size) {
    return offset <= file_size && size <= file_size - offset;
}

static unsigned long long parse_decimal_field(const char *field, size_t field_size) {
    unsigned long long value = 0ULL;
    size_t i = 0U;

    while (i < field_size && (field[i] == ' ' || field[i] == '\0')) i += 1U;
    while (i < field_size && field[i] >= '0' && field[i] <= '9') {
        value = (value * 10ULL) + (unsigned long long)(field[i] - '0');
        i += 1U;
    }
    return value;
}

static int analyze_elf(int input_fd, const unsigned char *header, unsigned long long file_size, StripPlan *plan) {
    unsigned long long phoff;
    unsigned short phentsize;
    unsigned short phnum;
    unsigned short i;
    unsigned long long load_end = 0ULL;
    unsigned long long phdr_end;

    plan->format = STRIP_FORMAT_ELF64_LE;
    plan->format_name = "ELF64 little-endian";
    plan->elf_type = tool_read_u16_le(header + 16);
    plan->copy_size = file_size;
    plan->output_size = file_size;

    if (plan->elf_type == 1U) {
        plan->unsupported_relocatable = 1;
        plan->action = "rejected relocatable object";
        return 0;
    }

    phoff = tool_read_u64_le(header + 32);
    phentsize = tool_read_u16_le(header + 54);
    phnum = tool_read_u16_le(header + 56);
    if (phnum == 0U) {
        plan->action = "safe copy: no program headers";
        return 0;
    }
    if (phentsize < 56U) {
        plan->action = "safe copy: unsupported program-header size";
        return 0;
    }
    phdr_end = phoff + ((unsigned long long)phnum * (unsigned long long)phentsize);
    if (phoff > file_size || phdr_end < phoff || phdr_end > file_size) {
        plan->action = "safe copy: invalid program-header range";
        return 0;
    }

    for (i = 0U; i < phnum; ++i) {
        unsigned char phdr[56];
        unsigned int ph_type;
        unsigned long long ph_offset;
        unsigned long long ph_filesz;
        unsigned long long end_offset;

        if (platform_seek(input_fd, (long long)(phoff + ((unsigned long long)i * (unsigned long long)phentsize)), PLATFORM_SEEK_SET) < 0 ||
            archive_read_exact(input_fd, phdr, sizeof(phdr)) != 0) {
            return -1;
        }
        ph_type = tool_read_u32_le(phdr + 0);
        ph_offset = tool_read_u64_le(phdr + 8);
        ph_filesz = tool_read_u64_le(phdr + 32);
        if (ph_type != 1U || ph_filesz == 0ULL) {
            continue;
        }
        if (!checked_range(ph_offset, ph_filesz, file_size)) {
            plan->action = "safe copy: invalid load segment range";
            return 0;
        }
        end_offset = ph_offset + ph_filesz;
        if (end_offset > load_end) {
            load_end = end_offset;
        }
    }

    if (load_end > 0ULL && load_end < file_size) {
        plan->copy_size = load_end;
        plan->output_size = load_end;
        plan->patch_elf_header = 1;
        plan->action = "removed ELF section-header metadata";
    } else {
        plan->action = "safe copy: no trailing section metadata removed";
    }
    return 0;
}

static int analyze_pe(int input_fd, const unsigned char *header, unsigned long long file_size, StripPlan *plan) {
    unsigned char coff[24];
    unsigned char optional[240];
    unsigned int pe_offset;
    unsigned int symbol_offset;
    unsigned int symbol_count;
    unsigned short section_count;
    unsigned short optional_size;
    unsigned short optional_magic = 0U;
    unsigned int data_directory_offset = 0U;
    unsigned int debug_rva = 0U;
    unsigned int debug_size = 0U;
    unsigned int debug_file_offset = 0U;

    if (file_size < 64ULL || header[0] != 'M' || header[1] != 'Z') {
        return 0;
    }
    pe_offset = tool_read_u32_le(header + 0x3cU);
    if (!checked_range((unsigned long long)pe_offset, sizeof(coff), file_size)) {
        return 0;
    }
    if (platform_seek(input_fd, (long long)pe_offset, PLATFORM_SEEK_SET) < 0 || archive_read_exact(input_fd, coff, sizeof(coff)) != 0) {
        return -1;
    }
    if (!(coff[0] == 'P' && coff[1] == 'E' && coff[2] == 0U && coff[3] == 0U)) {
        return 0;
    }

    symbol_offset = tool_read_u32_le(coff + 12);
    symbol_count = tool_read_u32_le(coff + 16);
    section_count = tool_read_u16_le(coff + 6);
    optional_size = tool_read_u16_le(coff + 20);
    plan->format = STRIP_FORMAT_PE_COFF;
    plan->format_name = "PE/COFF";
    plan->pe_offset = pe_offset;
    plan->pe_symbol_offset = symbol_offset;
    plan->pe_symbol_count = symbol_count;
    plan->copy_size = file_size;
    plan->output_size = file_size;

    if (optional_size >= 2U && optional_size <= sizeof(optional) && checked_range((unsigned long long)pe_offset + 24ULL, optional_size, file_size)) {
        if (platform_seek(input_fd, (long long)pe_offset + 24LL, PLATFORM_SEEK_SET) < 0 || archive_read_exact(input_fd, optional, optional_size) != 0) {
            return -1;
        }
        optional_magic = tool_read_u16_le(optional + 0);
        if (optional_magic == 0x020bU && optional_size >= 168U) data_directory_offset = 112U;
        else if (optional_magic == 0x010bU && optional_size >= 152U) data_directory_offset = 96U;
        if (data_directory_offset != 0U) {
            debug_rva = tool_read_u32_le(optional + data_directory_offset + 48U);
            debug_size = tool_read_u32_le(optional + data_directory_offset + 52U);
            if (debug_rva != 0U || debug_size != 0U) {
                unsigned long long section_table = (unsigned long long)pe_offset + 24ULL + (unsigned long long)optional_size;
                unsigned short i;
                if (section_count <= 256U && checked_range(section_table, (unsigned long long)section_count * 40ULL, file_size)) {
                    for (i = 0U; i < section_count; ++i) {
                        unsigned char section[40];
                        unsigned int virtual_size;
                        unsigned int virtual_address;
                        unsigned int raw_size;
                        unsigned int raw_offset;
                        unsigned int mapped_size;
                        if (platform_seek(input_fd, (long long)(section_table + ((unsigned long long)i * 40ULL)), PLATFORM_SEEK_SET) < 0 ||
                            archive_read_exact(input_fd, section, sizeof(section)) != 0) {
                            return -1;
                        }
                        virtual_size = tool_read_u32_le(section + 8);
                        virtual_address = tool_read_u32_le(section + 12);
                        raw_size = tool_read_u32_le(section + 16);
                        raw_offset = tool_read_u32_le(section + 20);
                        mapped_size = virtual_size > raw_size ? virtual_size : raw_size;
                        if (debug_rva >= virtual_address && debug_rva - virtual_address < mapped_size) {
                            debug_file_offset = raw_offset + (debug_rva - virtual_address);
                            break;
                        }
                    }
                }
                (void)debug_file_offset;
                plan->pe_debug_directory_offset = pe_offset + 24U + data_directory_offset + 48U;
                plan->pe_debug_directory_size = debug_size;
                plan->patch_pe_debug_directory = 1;
            }
        }
    }

    if (symbol_offset != 0U || symbol_count != 0U) {
        plan->patch_pe_symbols = 1;
    }
    if (plan->patch_pe_symbols && plan->patch_pe_debug_directory) {
        plan->action = "cleared PE/COFF symbol table and debug directory";
    } else if (plan->patch_pe_symbols) {
        plan->action = "cleared PE/COFF symbol-table pointer";
    } else if (plan->patch_pe_debug_directory) {
        plan->action = "cleared PE/COFF debug directory";
    } else {
        plan->action = "safe copy: no COFF symbol table present";
    }
    return 0;
}

static int analyze_macho(int input_fd, const unsigned char *header, unsigned long long file_size, StripPlan *plan) {
    unsigned int ncmds;
    unsigned int command_index;
    unsigned long long command_offset = 32ULL;

    plan->format = STRIP_FORMAT_MACHO64_LE;
    plan->format_name = "Mach-O 64-bit little-endian";
    plan->copy_size = file_size;
    plan->output_size = file_size;
    if (tool_read_u32_le(header + 12) == 1U) {
        plan->unsupported_relocatable = 1;
        plan->action = "rejected relocatable object";
        return 0;
    }

    ncmds = tool_read_u32_le(header + 16);
    plan->macho_ncmds = ncmds;
    if (ncmds > 256U) {
        plan->action = "safe copy: too many Mach-O load commands";
        return 0;
    }

    for (command_index = 0U; command_index < ncmds; ++command_index) {
        unsigned char command_header[8];
        unsigned int command;
        unsigned int command_size;

        if (!checked_range(command_offset, sizeof(command_header), file_size) ||
            platform_seek(input_fd, (long long)command_offset, PLATFORM_SEEK_SET) < 0 ||
            archive_read_exact(input_fd, command_header, sizeof(command_header)) != 0) {
            return -1;
        }
        command = tool_read_u32_le(command_header + 0);
        command_size = tool_read_u32_le(command_header + 4);
        if (command_size < 8U || !checked_range(command_offset, command_size, file_size)) {
            plan->action = "safe copy: invalid Mach-O load-command range";
            return 0;
        }

        if (command == 0x2U && command_size >= 24U) {
            unsigned char symtab[24];
            if (platform_seek(input_fd, (long long)command_offset, PLATFORM_SEEK_SET) < 0 || archive_read_exact(input_fd, symtab, sizeof(symtab)) != 0) {
                return -1;
            }
            plan->macho_symoff = tool_read_u32_le(symtab + 8);
            plan->macho_nsyms = tool_read_u32_le(symtab + 12);
            plan->macho_stroff = tool_read_u32_le(symtab + 16);
            plan->macho_strsize = tool_read_u32_le(symtab + 20);
        } else if (command == 0x1dU && command_size >= 16U) {
            unsigned char linkedit[16];
            if (platform_seek(input_fd, (long long)command_offset, PLATFORM_SEEK_SET) < 0 || archive_read_exact(input_fd, linkedit, sizeof(linkedit)) != 0) {
                return -1;
            }
            plan->macho_code_signature_offset = tool_read_u32_le(linkedit + 8);
            plan->macho_code_signature_size = tool_read_u32_le(linkedit + 12);
        }

        command_offset += (unsigned long long)command_size;
    }

    if (plan->macho_code_signature_offset != 0U || plan->macho_code_signature_size != 0U) {
        plan->action = "safe copy: Mach-O code signature present";
    } else if (plan->macho_symoff != 0U || plan->macho_stroff != 0U) {
        plan->action = "safe copy: Mach-O symbol table requires load-command rewriting";
    } else {
        plan->action = "safe copy: no Mach-O symbol table present";
    }
    return 0;
}

static int analyze_archive(int input_fd, unsigned long long file_size, StripPlan *plan) {
    unsigned long long offset = 8ULL;
    unsigned int members = 0U;

    plan->format = STRIP_FORMAT_ARCHIVE;
    plan->format_name = "ar archive";
    plan->copy_size = file_size;
    plan->output_size = file_size;
    while (offset + STRIP_AR_HEADER_SIZE <= file_size) {
        unsigned char header[STRIP_AR_HEADER_SIZE];
        unsigned long long payload_size;
        if (platform_seek(input_fd, (long long)offset, PLATFORM_SEEK_SET) < 0 || archive_read_exact(input_fd, header, sizeof(header)) != 0) {
            return -1;
        }
        if (header[58] != '`' || header[59] != '\n') {
            plan->action = "safe copy: malformed archive member header";
            return 0;
        }
        payload_size = parse_decimal_field((const char *)header + 48, 10U);
        if (!checked_range(offset + STRIP_AR_HEADER_SIZE, payload_size, file_size)) {
            plan->action = "safe copy: invalid archive member size";
            return 0;
        }
        members += 1U;
        offset += STRIP_AR_HEADER_SIZE + payload_size + ((payload_size & 1ULL) != 0ULL ? 1ULL : 0ULL);
    }
    plan->archive_members = members;
    plan->action = "safe copy: archive member rewriting not implemented";
    return 0;
}

static void analyze_file(int input_fd, const unsigned char *header, unsigned long long file_size, StripPlan *plan) {
    plan->format = STRIP_FORMAT_UNKNOWN;
    plan->input_size = file_size;
    plan->output_size = file_size;
    plan->copy_size = file_size;
    plan->pe_offset = 0U;
    plan->pe_symbol_offset = 0U;
    plan->pe_symbol_count = 0U;
    plan->pe_debug_directory_offset = 0U;
    plan->pe_debug_directory_size = 0U;
    plan->elf_type = 0U;
    plan->macho_ncmds = 0U;
    plan->macho_symoff = 0U;
    plan->macho_nsyms = 0U;
    plan->macho_stroff = 0U;
    plan->macho_strsize = 0U;
    plan->macho_code_signature_offset = 0U;
    plan->macho_code_signature_size = 0U;
    plan->archive_members = 0U;
    plan->patch_elf_header = 0;
    plan->patch_pe_symbols = 0;
    plan->patch_pe_debug_directory = 0;
    plan->unsupported_relocatable = 0;
    plan->format_name = "unknown";
    plan->action = "safe copy: unsupported format";

    if (archive_has_ar_magic(header, file_size)) {
        if (analyze_archive(input_fd, file_size, plan) != 0) {
            plan->action = "safe copy: archive analysis failed";
        }
        return;
    }

    if (file_size >= 64ULL && header[0] == 0x7fU && header[1] == 'E' && header[2] == 'L' && header[3] == 'F' && header[4] == 2U && header[5] == 1U) {
        if (analyze_elf(input_fd, header, file_size, plan) != 0) {
            plan->action = "safe copy: ELF analysis failed";
        }
        return;
    }

    if (file_size >= 32ULL && tool_read_u32_le(header) == 0xfeedfacfU) {
        if (analyze_macho(input_fd, header, file_size, plan) != 0) {
            plan->action = "safe copy: Mach-O analysis failed";
        }
        return;
    }

    if (analyze_pe(input_fd, header, file_size, plan) != 0) {
        plan->action = "safe copy: PE/COFF analysis failed";
    }
}

static void print_verbose_plan(const char *path, const StripOptions *options, const StripPlan *plan) {
    rt_write_cstr(1, "strip: ");
    rt_write_cstr(1, path);
    rt_write_cstr(1, ": ");
    rt_write_cstr(1, plan->format_name);
    rt_write_cstr(1, ", ");
    rt_write_cstr(1, mode_name(options->mode));
    if (options->dry_run) rt_write_cstr(1, ", dry-run");
    rt_write_cstr(1, ", input=");
    rt_write_uint(1, plan->input_size);
    rt_write_cstr(1, ", output=");
    rt_write_uint(1, plan->output_size);
    rt_write_cstr(1, ", action: ");
    rt_write_line(1, plan->action);
    if (plan->format == STRIP_FORMAT_MACHO64_LE) {
        rt_write_cstr(1, "  mach-o-load-commands: ");
        rt_write_uint(1, plan->macho_ncmds);
        rt_write_cstr(1, "\n  mach-o-symtab: symoff=");
        rt_write_uint(1, plan->macho_symoff);
        rt_write_cstr(1, " nsyms=");
        rt_write_uint(1, plan->macho_nsyms);
        rt_write_cstr(1, " stroff=");
        rt_write_uint(1, plan->macho_stroff);
        rt_write_cstr(1, " strsize=");
        rt_write_uint(1, plan->macho_strsize);
        rt_write_cstr(1, "\n  mach-o-code-signature: off=");
        rt_write_uint(1, plan->macho_code_signature_offset);
        rt_write_cstr(1, " size=");
        rt_write_uint(1, plan->macho_code_signature_size);
        rt_write_char(1, '\n');
    } else if (plan->format == STRIP_FORMAT_PE_COFF) {
        rt_write_cstr(1, "  pe-symbol-table: off=");
        rt_write_uint(1, plan->pe_symbol_offset);
        rt_write_cstr(1, " count=");
        rt_write_uint(1, plan->pe_symbol_count);
        rt_write_cstr(1, "\n  pe-debug-directory: entry-off=");
        rt_write_uint(1, plan->pe_debug_directory_offset);
        rt_write_cstr(1, " size=");
        rt_write_uint(1, plan->pe_debug_directory_size);
        rt_write_char(1, '\n');
    } else if (plan->format == STRIP_FORMAT_ARCHIVE) {
        rt_write_cstr(1, "  archive-members: ");
        rt_write_uint(1, plan->archive_members);
        rt_write_char(1, '\n');
    }
}

static int strip_one_file(const char *input_path, const StripOptions *options, int inplace) {
    PlatformDirEntry entry;
    unsigned char header[64];
    unsigned long long file_size;
    StripPlan plan;
    int input_fd = -1;
    int output_fd = -1;
    char temp_path[1024];
    char temp_prefix[1024];
    const char *output_path = options->output_path != 0 ? options->output_path : input_path;

    if (platform_get_path_info(input_path, &entry) != 0 || entry.is_dir) {
        rt_write_cstr(2, "strip: cannot access ");
        rt_write_line(2, input_path);
        return 1;
    }

    input_fd = platform_open_read(input_path);
    if (input_fd < 0) {
        rt_write_cstr(2, "strip: cannot open ");
        rt_write_line(2, input_path);
        return 1;
    }

    file_size = entry.size;
    if (file_size < sizeof(header)) {
        size_t to_read = (size_t)file_size;
        memset(header, 0, sizeof(header));
        if (to_read > 0U && archive_read_exact(input_fd, header, to_read) != 0) {
            rt_write_cstr(2, "strip: cannot read ");
            rt_write_line(2, input_path);
            platform_close(input_fd);
            return 1;
        }
    } else if (archive_read_exact(input_fd, header, sizeof(header)) != 0) {
        rt_write_cstr(2, "strip: cannot read ");
        rt_write_line(2, input_path);
        platform_close(input_fd);
        return 1;
    }

    analyze_file(input_fd, header, file_size, &plan);
    if (plan.unsupported_relocatable) {
        rt_write_cstr(2, "strip: relocatable objects are not yet supported: ");
        rt_write_line(2, input_path);
        platform_close(input_fd);
        return 1;
    }
    if (options->verbose || options->dry_run) {
        print_verbose_plan(input_path, options, &plan);
    }
    if (options->dry_run) {
        platform_close(input_fd);
        return 0;
    }

    if (inplace) {
        tool_path_build_temp_prefix(input_path, ".newos-strip-", temp_prefix, sizeof(temp_prefix));
        output_fd = platform_create_temp_file(temp_path, sizeof(temp_path), temp_prefix, (entry.mode & 0777U) != 0U ? (entry.mode & 0777U) : 0644U);
    } else {
        output_fd = platform_open_write(output_path, (entry.mode & 0777U) != 0U ? (entry.mode & 0777U) : 0644U);
        temp_path[0] = '\0';
    }

    if (output_fd < 0) {
        rt_write_cstr(2, "strip: cannot create output for ");
        rt_write_line(2, input_path);
        platform_close(input_fd);
        return 1;
    }

    if (copy_prefix(input_fd, output_fd, plan.copy_size) != 0) {
        rt_write_cstr(2, "strip: failed while copying ");
        rt_write_line(2, input_path);
        platform_close(input_fd);
        platform_close(output_fd);
        if (inplace) {
            (void)platform_remove_file(temp_path);
        }
        return 1;
    }

    if (plan.patch_elf_header) {
        tool_store_u64_le(header + 40, 0ULL);
        tool_store_u16_le(header + 58, 0U);
        tool_store_u16_le(header + 60, 0U);
        tool_store_u16_le(header + 62, 0U);

        if (platform_seek(output_fd, 0, PLATFORM_SEEK_SET) < 0 || rt_write_all(output_fd, header, sizeof(header)) != 0) {
            rt_write_cstr(2, "strip: failed to patch ELF header for ");
            rt_write_line(2, input_path);
            platform_close(input_fd);
            platform_close(output_fd);
            if (inplace) {
                (void)platform_remove_file(temp_path);
            }
            return 1;
        }
    }

    if (plan.patch_pe_symbols) {
        unsigned char pe_patch[8];
        tool_store_u32_le(pe_patch + 0, 0U);
        tool_store_u32_le(pe_patch + 4, 0U);
        if (platform_seek(output_fd, (long long)plan.pe_offset + 12LL, PLATFORM_SEEK_SET) < 0 || rt_write_all(output_fd, pe_patch, sizeof(pe_patch)) != 0) {
            rt_write_cstr(2, "strip: failed to patch PE/COFF header for ");
            rt_write_line(2, input_path);
            platform_close(input_fd);
            platform_close(output_fd);
            if (inplace) {
                (void)platform_remove_file(temp_path);
            }
            return 1;
        }
    }

    if (plan.patch_pe_debug_directory) {
        unsigned char pe_debug_patch[8];
        tool_store_u32_le(pe_debug_patch + 0, 0U);
        tool_store_u32_le(pe_debug_patch + 4, 0U);
        if (platform_seek(output_fd, (long long)plan.pe_debug_directory_offset, PLATFORM_SEEK_SET) < 0 || rt_write_all(output_fd, pe_debug_patch, sizeof(pe_debug_patch)) != 0) {
            rt_write_cstr(2, "strip: failed to patch PE/COFF debug directory for ");
            rt_write_line(2, input_path);
            platform_close(input_fd);
            platform_close(output_fd);
            if (inplace) {
                (void)platform_remove_file(temp_path);
            }
            return 1;
        }
    }

    platform_close(input_fd);
    platform_close(output_fd);

    if (inplace) {
        if (platform_rename_path(temp_path, input_path) != 0) {
            rt_write_cstr(2, "strip: failed to replace ");
            rt_write_line(2, input_path);
            (void)platform_remove_file(temp_path);
            return 1;
        }
    }

    (void)platform_set_path_times(output_path, entry.atime, entry.mtime, 0, 1, 1);

    return 0;
}

int main(int argc, char **argv) {
    StripOptions options;
    int argi = 1;
    int exit_code = 0;
    int i;

    options.output_path = 0;
    options.mode = STRIP_MODE_ALL;
    options.dry_run = 0;
    options.verbose = 0;

    while (argi < argc && argv[argi][0] == '-') {
        if (rt_strcmp(argv[argi], "-o") == 0) {
            if (argi + 1 >= argc) {
                tool_write_usage("strip", "[-nv] [--strip-all|--strip-debug] [-o output] file ...");
                return 1;
            }
            options.output_path = argv[argi + 1];
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-n") == 0 || rt_strcmp(argv[argi], "--dry-run") == 0) {
            options.dry_run = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-v") == 0 || rt_strcmp(argv[argi], "--verbose") == 0) {
            options.verbose = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--strip-all") == 0) {
            options.mode = STRIP_MODE_ALL;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--strip-debug") == 0) {
            options.mode = STRIP_MODE_DEBUG;
            argi += 1;
        } else {
            tool_write_usage("strip", "[-nv] [--strip-all|--strip-debug] [-o output] file ...");
            return 1;
        }
    }

    if (argi >= argc) {
        tool_write_usage("strip", "[-nv] [--strip-all|--strip-debug] [-o output] file ...");
        return 1;
    }

    if (options.output_path != 0 && (argc - argi) != 1) {
        rt_write_line(2, "strip: -o requires exactly one input file");
        return 1;
    }

    for (i = argi; i < argc; ++i) {
        if (strip_one_file(argv[i], &options, options.output_path == 0) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
