#include "targets/target_info.h"

#include "platform.h"
#include "runtime.h"

static int target_text_equals(const char *lhs, const char *rhs) {
    size_t i = 0U;

    if (lhs == 0 || rhs == 0) {
        return 0;
    }

    while (lhs[i] != '\0' && rhs[i] != '\0') {
        if (lhs[i] != rhs[i]) {
            return 0;
        }
        i += 1U;
    }

    return lhs[i] == '\0' && rhs[i] == '\0';
}

static const CompilerTargetInfo COMPILER_TARGETS[] = {
    {
        COMPILER_TARGET_LINUX_X86_64,
        "linux-x86_64",
        "x86_64-linux-none",
        "src/arch/x86_64/linux",
        "src/platform/linux",
        "src/platform/common",
        "",
        COMPILER_OBJECT_FORMAT_ELF64,
        6U,
        8U,
        0,
        0
    },
    {
        COMPILER_TARGET_LINUX_AARCH64,
        "linux-aarch64",
        "aarch64-linux-none",
        "src/arch/aarch64/linux",
        "src/platform/linux",
        "src/platform/common",
        "",
        COMPILER_OBJECT_FORMAT_ELF64,
        8U,
        16U,
        1,
        0
    },
    {
        COMPILER_TARGET_MACOS_AARCH64,
        "macos-aarch64",
        "arm64-apple-darwin",
        "src/arch/aarch64/linux",
        "src/platform/posix",
        "src/platform/common",
        "_",
        COMPILER_OBJECT_FORMAT_MACHO64,
        8U,
        16U,
        1,
        1
    }
};

const CompilerTargetInfo *compiler_target_get_info(CompilerTarget target) {
    size_t index = (size_t)target;

    if (index >= sizeof(COMPILER_TARGETS) / sizeof(COMPILER_TARGETS[0])) {
        return 0;
    }

    return &COMPILER_TARGETS[index];
}

const char *compiler_target_name(CompilerTarget target) {
    const CompilerTargetInfo *info = compiler_target_get_info(target);
    return info != 0 ? info->name : "unknown";
}

void compiler_target_write_names(int fd) {
    size_t index;
    for (index = 0; index < sizeof(COMPILER_TARGETS) / sizeof(COMPILER_TARGETS[0]); ++index) {
        if (index != 0U) {
            rt_write_cstr(fd, ", ");
        }
        rt_write_cstr(fd, COMPILER_TARGETS[index].name);
    }
}

CompilerTarget compiler_target_default(void) {
    char sysname[64];
    char nodename[64];
    char release[64];
    char version[64];
    char machine[64];

    if (platform_get_uname(sysname, sizeof(sysname), nodename, sizeof(nodename), release, sizeof(release), version, sizeof(version), machine, sizeof(machine)) != 0) {
        return COMPILER_TARGET_MACOS_AARCH64;
    }

    (void)nodename;
    (void)release;
    (void)version;

    if (target_text_equals(sysname, "Linux")) {
        if (target_text_equals(machine, "x86_64")) {
            return COMPILER_TARGET_LINUX_X86_64;
        }
        return COMPILER_TARGET_LINUX_AARCH64;
    }

    if (target_text_equals(sysname, "Darwin")) {
        return COMPILER_TARGET_MACOS_AARCH64;
    }

    if (target_text_equals(sysname, "Windows") && target_text_equals(machine, "x86_64")) {
        return COMPILER_TARGET_LINUX_X86_64;
    }

    return COMPILER_TARGET_MACOS_AARCH64;
}

int compiler_target_parse(const char *text, CompilerTarget *target_out) {
    if (target_out == 0) {
        return -1;
    }
    if (target_text_equals(text, "linux-x86_64") || target_text_equals(text, "linux-x64") ||
        target_text_equals(text, "x86_64-linux-none") || target_text_equals(text, "x86_64-linux-gnu")) {
        *target_out = COMPILER_TARGET_LINUX_X86_64;
        return 0;
    }
    if (target_text_equals(text, "linux-aarch64") || target_text_equals(text, "linux-arm64") ||
        target_text_equals(text, "aarch64-linux-none") || target_text_equals(text, "aarch64-linux-gnu")) {
        *target_out = COMPILER_TARGET_LINUX_AARCH64;
        return 0;
    }
    if (target_text_equals(text, "macos-aarch64") || target_text_equals(text, "macos-arm64") ||
        target_text_equals(text, "darwin-aarch64") || target_text_equals(text, "darwin-arm64")) {
        *target_out = COMPILER_TARGET_MACOS_AARCH64;
        return 0;
    }
    return -1;
}

int compiler_target_is_aarch64(CompilerTarget target) {
    const CompilerTargetInfo *info = compiler_target_get_info(target);
    return info != 0 ? info->is_aarch64 : 0;
}

int compiler_target_is_darwin(CompilerTarget target) {
    const CompilerTargetInfo *info = compiler_target_get_info(target);
    return info != 0 ? info->is_darwin : 0;
}

int compiler_target_apply_preprocessor_defaults(CompilerPreprocessor *preprocessor, CompilerTarget target, int freestanding) {
    const CompilerTargetInfo *info = compiler_target_get_info(target);

    if (preprocessor == 0 || info == 0) {
        return -1;
    }
    (void)freestanding;

    if (compiler_preprocessor_add_include_dir(preprocessor, info->arch_include_dir) != 0 ||
        compiler_preprocessor_add_include_dir(preprocessor, info->platform_include_dir) != 0 ||
        compiler_preprocessor_add_include_dir(preprocessor, info->platform_common_include_dir) != 0) {
        return -1;
    }

    if (info->is_darwin) {
        if (compiler_preprocessor_define(preprocessor, "__APPLE__", "1") != 0) {
            return -1;
        }
    } else if (compiler_preprocessor_define(preprocessor, "__linux__", "1") != 0) {
        return -1;
    }

    if (info->is_aarch64) {
        if (compiler_preprocessor_define(preprocessor, "__aarch64__", "1") != 0) {
            return -1;
        }
    } else {
        if (compiler_preprocessor_define(preprocessor, "__x86_64__", "1") != 0 ||
            compiler_preprocessor_define(preprocessor, "__SIZEOF_INT128__", "16") != 0) {
            return -1;
        }
    }

    return 0;
}
