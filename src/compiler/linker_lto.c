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
    argv[argc++] = "-fno-common";
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
    argv[argc++] = "-fno-common";
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

static int lto_output_is_macho64_aarch64_object(const char *path) {
    unsigned char *file = 0;
    size_t size = 0U;
    int ok = 0;

    if (read_file_alloc(path, LINKER_MAX_OBJECT_SIZE, &file, &size, 0, 0) != 0) {
        return 0;
    }
    if (size >= 32U && read_u32(file) == 0xfeedfacfU && read_u32(file + 4U) == 0x0100000cU && read_u32(file + 12U) == 1U) {
        ok = 1;
    }
    rt_free(file);
    return ok;
}

int run_clang_lto_prelink_macho64_aarch64(const char *const *paths, size_t count, const char *entry_symbol,
                                          const char *lto_cc, const char *out_path, int dead_strip,
                                          char *error_out, size_t error_size) {
    char entry_keep[COMPILER_PATH_CAPACITY];
    char entry_flag[COMPILER_PATH_CAPACITY];
    char entry_name[COMPILER_PATH_CAPACITY];
    char object_path_lto[COMPILER_PATH_CAPACITY];
    char final_path[COMPILER_PATH_CAPACITY];
    const char *sdkroot;
    char **argv;
    size_t out_len;
    size_t argc = 0;
    size_t i;
    int pid = 0;
    int exit_status = 0;
    int quiet_attempt = 1;

    sdkroot = platform_getenv("SDKROOT");
    if ((sdkroot == 0 || sdkroot[0] == '\0') && platform_path_access("/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk", PLATFORM_ACCESS_EXISTS) == 0) {
        sdkroot = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk";
    }
    if ((sdkroot == 0 || sdkroot[0] == '\0') && platform_path_access("/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk", PLATFORM_ACCESS_EXISTS) == 0) {
        sdkroot = "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk";
    }

    if (rt_strcmp(entry_symbol, "_start") == 0) {
        rt_copy_string(entry_name, sizeof(entry_name), "__start");
    } else {
        rt_copy_string(entry_name, sizeof(entry_name), entry_symbol);
    }
    rt_copy_string(entry_flag, sizeof(entry_flag), "-Wl,-e,");
    rt_copy_string(entry_flag + rt_strlen(entry_flag), sizeof(entry_flag) - rt_strlen(entry_flag), entry_name);
    rt_copy_string(entry_keep, sizeof(entry_keep), "-Wl,-u,");
    rt_copy_string(entry_keep + 7, sizeof(entry_keep) - 7, entry_name);
    rt_copy_string(object_path_lto, sizeof(object_path_lto), "-Wl,-object_path_lto,");
    rt_copy_string(object_path_lto + rt_strlen(object_path_lto), sizeof(object_path_lto) - rt_strlen(object_path_lto), out_path);
    out_len = rt_strlen(out_path);
    if (out_len + 7U >= sizeof(final_path)) {
        set_link_error(error_out, error_size, "LTO prelink output path is too long", out_path);
        return -1;
    }
    rt_copy_string(final_path, sizeof(final_path), out_path);
    rt_copy_string(final_path + out_len, sizeof(final_path) - out_len, ".final");

retry_prelink:
    argv = (char **)rt_malloc((28U + count + 3U) * sizeof(char *));
    if (argv == 0) {
        set_link_error(error_out, error_size, "out of memory for Mach-O Clang LTO prelink argv", out_path);
        return -1;
    }
    argv[argc++] = (char *)lto_cc;
    argv[argc++] = "-target";
    argv[argc++] = "arm64-apple-macos11";
    argv[argc++] = "-flto";
    if (quiet_attempt && sdkroot != 0 && sdkroot[0] != '\0') {
        argv[argc++] = "-isysroot";
        argv[argc++] = (char *)sdkroot;
    }
    if (quiet_attempt) {
        argv[argc++] = "-nodefaultlibs";
        argv[argc++] = "-lSystem";
    } else {
        argv[argc++] = "-nostdlib";
    }
    argv[argc++] = "-Oz";
    argv[argc++] = "-ffreestanding";
    argv[argc++] = "-fno-builtin";
    argv[argc++] = "-fno-common";
    argv[argc++] = "-fno-stack-protector";
    argv[argc++] = "-fno-unwind-tables";
    argv[argc++] = "-fno-asynchronous-unwind-tables";
    if (dead_strip) {
        argv[argc++] = "-Wl,-dead_strip";
    }
    argv[argc++] = entry_flag;
    argv[argc++] = entry_keep;
    argv[argc++] = object_path_lto;
    for (i = 0; i < count; ++i) {
        argv[argc++] = (char *)paths[i];
    }
    argv[argc++] = "-o";
    argv[argc++] = final_path;
    argv[argc] = 0;

    platform_remove_file(out_path);
    platform_remove_file(final_path);
    if (platform_spawn_process(argv, -1, -1, 0, 0, 0, &pid) != 0) {
        rt_free(argv);
        set_link_error(error_out, error_size, "failed to spawn Clang for Mach-O LTO prelink", out_path);
        return -1;
    }
    (void)platform_wait_process(pid, &exit_status);
    rt_free(argv);
    platform_remove_file(final_path);
    if (lto_output_is_macho64_aarch64_object(out_path)) {
        return 0;
    }
    if (quiet_attempt) {
        quiet_attempt = 0;
        argc = 0U;
        pid = 0;
        exit_status = 0;
        platform_remove_file(final_path);
        platform_remove_file(out_path);
        goto retry_prelink;
    }
    set_link_error(error_out, error_size, "Clang Mach-O LTO prelink failed", out_path);
    return -1;
}
