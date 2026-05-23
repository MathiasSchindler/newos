#ifndef NEWOS_COMPILER_LINKER_H
#define NEWOS_COMPILER_LINKER_H

#include <stddef.h>

#include "compiler.h"

typedef enum {
	COMPILER_LINKER_TARGET_ELF64_X86_64 = 0,
	COMPILER_LINKER_TARGET_MACHO64_AARCH64 = 1
} CompilerLinkerTarget;

typedef struct {
	CompilerLinkerTarget target;
	int tiny;
	int gc_sections;
	int stats;
	int icf_safe;
	int print_gc_sections;
	const char *map_path;
	const char *why_live;
	const char *entry_symbol;
	const char *lto_cc;
} CompilerLinkerOptions;

const char *compiler_linker_target_name(CompilerLinkerTarget target);
int compiler_linker_target_parse(const char *text, CompilerLinkerTarget *target_out);
int compiler_link_static_options(const char *const *object_paths,
								 size_t object_count,
								 const char *output_path,
								 const CompilerLinkerOptions *options,
								 char *error_out,
								 size_t error_size);
int compiler_link_elf64_x86_64_static(const char *const *object_paths, size_t object_count, const char *output_path, char *error_out, size_t error_size);
int compiler_link_elf64_x86_64_static_options(const char *const *object_paths,
											  size_t object_count,
											  const char *output_path,
											  const CompilerLinkerOptions *options,
											  char *error_out,
											  size_t error_size);

#endif
