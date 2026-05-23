#include "linker_internal.h"

static int buffer_contains_text(const unsigned char *file, size_t size, const char *needle) {
    size_t needle_size = rt_strlen(needle);
    size_t i;
    size_t j;

    if (needle_size == 0U || needle_size > size) {
        return 0;
    }
    for (i = 0; i + needle_size <= size; ++i) {
        for (j = 0; j < needle_size; ++j) {
            if (file[i + j] != (unsigned char)needle[j]) {
                break;
            }
        }
        if (j == needle_size) {
            return 1;
        }
    }
    return 0;
}

static int looks_like_macho64(const unsigned char *file, size_t size) {
    if (size < 4U) {
        return 0;
    }
    return (file[0] == 0xcfU && file[1] == 0xfaU && file[2] == 0xedU && file[3] == 0xfeU) ||
           (file[0] == 0xfeU && file[1] == 0xedU && file[2] == 0xfaU && file[3] == 0xcfU);
}

LinkLtoKind detect_lto_ir_kind(const unsigned char *file, size_t size) {
    uint64_t shoff;
    uint16_t shentsize, shnum, shstrndx;
    const unsigned char *shstr_hdr;
    uint64_t shstr_off, shstr_size;
    uint16_t i;

    if (size >= 4U) {
        if ((file[0] == 'B' && file[1] == 'C' && file[2] == 0xc0U && file[3] == 0xdeU) ||
            (file[0] == 0xdeU && file[1] == 0xc0U && file[2] == 0x17U && file[3] == 0x0bU)) {
            return LINK_LTO_LLVM;
        }
    }
    if (looks_like_macho64(file, size) && buffer_contains_text(file, size, "__LLVM") && buffer_contains_text(file, size, "__bitcode")) {
        return LINK_LTO_LLVM;
    }
    if (size < ELF64_EHDR_SIZE) return LINK_LTO_NONE;
    if (file[0] != 0x7fU || file[1] != 'E' || file[2] != 'L' || file[3] != 'F') return LINK_LTO_NONE;
    shoff = read_u64(file + 40);
    shentsize = read_u16(file + 58);
    shnum = read_u16(file + 60);
    shstrndx = read_u16(file + 62);
    if (shnum == 0 || shstrndx >= shnum || shentsize == 0) return LINK_LTO_NONE;
    if (!range_valid(shoff, (uint64_t)shnum * shentsize, size)) return LINK_LTO_NONE;
    shstr_hdr = file + shoff + (uint64_t)shstrndx * shentsize;
    shstr_off = read_u64(shstr_hdr + 24);
    shstr_size = read_u64(shstr_hdr + 32);
    if (!range_valid(shstr_off, shstr_size, size) || shstr_size == 0) return LINK_LTO_NONE;
    for (i = 1; i < shnum; ++i) {
        const unsigned char *shdr = file + shoff + (uint64_t)i * shentsize;
        uint32_t name_off = read_u32(shdr + 0);
        if (name_off + 9U < shstr_size) {
            if (rt_strncmp((const char *)(file + shstr_off + name_off), ".gnu.lto_", 9) == 0) {
                return LINK_LTO_GCC;
            }
        }
    }
    return LINK_LTO_NONE;
}

int detect_lto_ir(const unsigned char *file, size_t size) {
    return detect_lto_ir_kind(file, size) != LINK_LTO_NONE;
}

int run_gcc_lto_prelink(const char *const *paths, size_t count, const char *entry_symbol,
                        const char *lto_cc, const char *out_path,
                        char *error_out, size_t error_size) {
    char entry_keep[COMPILER_PATH_CAPACITY];
    char **argv;
    size_t argc = 0;
    size_t i;
    int pid = 0;
    int exit_status = 0;

    /* "-Wl,-u," is 8 characters */
    rt_copy_string(entry_keep, sizeof(entry_keep), "-Wl,-u,");
    rt_copy_string(entry_keep + 7, sizeof(entry_keep) - 7, entry_symbol);

    argv = (char **)rt_malloc((20U + count + 3U) * sizeof(char *));
    if (argv == 0) {
        set_link_error(error_out, error_size, "out of memory for LTO prelink argv", out_path);
        return -1;
    }
    argv[argc++] = (char *)lto_cc;
    argv[argc++] = "-flto";
    argv[argc++] = "-flinker-output=nolto-rel";
    argv[argc++] = "-r";
    argv[argc++] = "-nostdlib";
    argv[argc++] = "-m64";
    argv[argc++] = "-Oz";
    argv[argc++] = "-ffreestanding";
    argv[argc++] = "-fno-builtin";
    argv[argc++] = "-fno-stack-protector";
    argv[argc++] = "-fno-unwind-tables";
    argv[argc++] = "-fno-asynchronous-unwind-tables";
    argv[argc++] = "-ffunction-sections";
    argv[argc++] = "-fdata-sections";
    argv[argc++] = "-fno-pic";
    argv[argc++] = "-fno-pie";
    argv[argc++] = "-fmerge-all-constants";
    argv[argc++] = "-Wl,--gc-sections";
    argv[argc++] = entry_keep;
    for (i = 0; i < count; ++i) {
        argv[argc++] = (char *)paths[i];
    }
    argv[argc++] = "-o";
    argv[argc++] = (char *)out_path;
    argv[argc] = 0;

    if (platform_spawn_process(argv, -1, -1, 0, 0, 0, &pid) != 0) {
        rt_free(argv);
        set_link_error(error_out, error_size, "failed to spawn GCC for LTO prelink", out_path);
        return -1;
    }
    if (platform_wait_process(pid, &exit_status) != 0 || exit_status != 0) {
        rt_free(argv);
        set_link_error(error_out, error_size, "GCC LTO prelink failed", out_path);
        return -1;
    }
    rt_free(argv);
    return 0;
}

int run_clang_lto_prelink_elf64_x86_64(const char *const *paths, size_t count, const char *entry_symbol,
                                       const char *lto_cc, const char *out_path,
                                       char *error_out, size_t error_size) {
    char entry_keep[COMPILER_PATH_CAPACITY];
    char **argv;
    size_t argc = 0;
    size_t i;
    int pid = 0;
    int exit_status = 0;

    rt_copy_string(entry_keep, sizeof(entry_keep), "-Wl,-u,");
    rt_copy_string(entry_keep + 7, sizeof(entry_keep) - 7, entry_symbol);

    argv = (char **)rt_malloc((24U + count + 3U) * sizeof(char *));
    if (argv == 0) {
        set_link_error(error_out, error_size, "out of memory for Clang LTO prelink argv", out_path);
        return -1;
    }
    argv[argc++] = (char *)lto_cc;
    argv[argc++] = "-target";
    argv[argc++] = "x86_64-unknown-linux-elf";
    argv[argc++] = "-flto";
    argv[argc++] = "-fuse-ld=lld";
    argv[argc++] = "-r";
    argv[argc++] = "-nostdlib";
    argv[argc++] = "-Oz";
    argv[argc++] = "-ffreestanding";
    argv[argc++] = "-fno-builtin";
    argv[argc++] = "-fno-stack-protector";
    argv[argc++] = "-fno-unwind-tables";
    argv[argc++] = "-fno-asynchronous-unwind-tables";
    argv[argc++] = "-ffunction-sections";
    argv[argc++] = "-fdata-sections";
    argv[argc++] = "-fno-pic";
    argv[argc++] = "-fno-pie";
    argv[argc++] = "-fmerge-all-constants";
    argv[argc++] = "-Wl,--gc-sections";
    argv[argc++] = entry_keep;
    for (i = 0; i < count; ++i) {
        argv[argc++] = (char *)paths[i];
    }
    argv[argc++] = "-o";
    argv[argc++] = (char *)out_path;
    argv[argc] = 0;

    if (platform_spawn_process(argv, -1, -1, 0, 0, 0, &pid) != 0) {
        rt_free(argv);
        set_link_error(error_out, error_size, "failed to spawn Clang for LTO prelink", out_path);
        return -1;
    }
    if (platform_wait_process(pid, &exit_status) != 0 || exit_status != 0) {
        rt_free(argv);
        set_link_error(error_out, error_size, "Clang LTO prelink failed", out_path);
        return -1;
    }
    rt_free(argv);
    return 0;
}
