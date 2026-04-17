#ifndef NEWOS_COMPILER_SOURCE_H
#define NEWOS_COMPILER_SOURCE_H

#include <stddef.h>

#define COMPILER_PATH_CAPACITY 512
#define COMPILER_MAX_SOURCE_SIZE (1024U * 1024U)

typedef struct {
    char path[COMPILER_PATH_CAPACITY];
    char data[COMPILER_MAX_SOURCE_SIZE + 1U];
    size_t size;
} CompilerSource;

int compiler_load_source(const char *path, CompilerSource *source_out);

#endif
