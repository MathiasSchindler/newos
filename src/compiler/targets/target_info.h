#ifndef NEWOS_COMPILER_TARGET_INFO_H
#define NEWOS_COMPILER_TARGET_INFO_H

#include "preprocessor.h"

typedef enum {
    COMPILER_TARGET_LINUX_X86_64 = 0,
    COMPILER_TARGET_LINUX_AARCH64,
    COMPILER_TARGET_MACOS_AARCH64
} CompilerTarget;

typedef enum {
    COMPILER_OBJECT_FORMAT_NONE = 0,
    COMPILER_OBJECT_FORMAT_ELF64,
    COMPILER_OBJECT_FORMAT_MACHO64
} CompilerObjectFormat;

typedef struct {
    CompilerTarget      target;
    const char         *name;
    const char         *clang_triple;
    const char         *arch_include_dir;
    const char         *global_symbol_prefix;
    CompilerObjectFormat object_format;
    unsigned int        register_arg_limit;
    unsigned int        stack_slot_size;
    int                 is_aarch64;
    int                 is_darwin;
} CompilerTargetInfo;

CompilerTarget compiler_target_default(void);
int compiler_target_parse(const char *text, CompilerTarget *target_out);
const char *compiler_target_name(CompilerTarget target);
const CompilerTargetInfo *compiler_target_get_info(CompilerTarget target);
int compiler_target_is_aarch64(CompilerTarget target);
int compiler_target_is_darwin(CompilerTarget target);
int compiler_target_apply_preprocessor_defaults(CompilerPreprocessor *preprocessor, CompilerTarget target, int freestanding);

#endif
