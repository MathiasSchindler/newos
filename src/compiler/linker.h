#ifndef NEWOS_COMPILER_LINKER_H
#define NEWOS_COMPILER_LINKER_H

#include <stddef.h>

#include "compiler.h"

typedef struct {
	int tiny;
	int gc_sections;
	int stats;
	int icf_safe;
	const char *map_path;
	const char *why_live;
	const char *entry_symbol;
} CompilerLinkerOptions;

int compiler_link_elf64_x86_64_static(const char *const *object_paths, size_t object_count, const char *output_path, char *error_out, size_t error_size);
int compiler_link_elf64_x86_64_static_options(const char *const *object_paths,
											  size_t object_count,
											  const char *output_path,
											  const CompilerLinkerOptions *options,
											  char *error_out,
											  size_t error_size);

#endif
