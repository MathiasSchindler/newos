#include "linker_internal.h"

int detect_lto_ir(const unsigned char *file, size_t size) {
    uint64_t shoff;
    uint16_t shentsize, shnum, shstrndx;
    const unsigned char *shstr_hdr;
    uint64_t shstr_off, shstr_size;
    uint16_t i;

    if (size < ELF64_EHDR_SIZE) return 0;
    if (file[0] != 0x7fU || file[1] != 'E' || file[2] != 'L' || file[3] != 'F') return 0;
    shoff = read_u64(file + 40);
    shentsize = read_u16(file + 58);
    shnum = read_u16(file + 60);
    shstrndx = read_u16(file + 62);
    if (shnum == 0 || shstrndx >= shnum || shentsize == 0) return 0;
    if (!range_valid(shoff, (uint64_t)shnum * shentsize, size)) return 0;
    shstr_hdr = file + shoff + (uint64_t)shstrndx * shentsize;
    shstr_off = read_u64(shstr_hdr + 24);
    shstr_size = read_u64(shstr_hdr + 32);
    if (!range_valid(shstr_off, shstr_size, size) || shstr_size == 0) return 0;
    for (i = 1; i < shnum; ++i) {
        const unsigned char *shdr = file + shoff + (uint64_t)i * shentsize;
        uint32_t name_off = read_u32(shdr + 0);
        if (name_off + 9U < shstr_size) {
            if (rt_strncmp((const char *)(file + shstr_off + name_off), ".gnu.lto_", 9) == 0) {
                return 1;
            }
        }
    }
    return 0;
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
