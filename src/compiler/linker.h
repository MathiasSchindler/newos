#ifndef NEWOS_COMPILER_LINKER_H
#define NEWOS_COMPILER_LINKER_H

#include <stddef.h>

#include "compiler.h"

int compiler_link_elf64_x86_64_static(const char *const *object_paths, size_t object_count, const char *output_path, char *error_out, size_t error_size);

#endif
