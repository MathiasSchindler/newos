#ifndef NEWOS_COMPRESSION_BZIP2_H
#define NEWOS_COMPRESSION_BZIP2_H

#include <stddef.h>

typedef int (*CompressionBzip2ReadFn)(void *context, unsigned char *buffer, size_t capacity, size_t *size_out);
typedef int (*CompressionBzip2WriteFn)(void *context, const unsigned char *data, size_t size);

int compression_bzip2_decompress_stream(CompressionBzip2ReadFn read_fn, void *read_context, CompressionBzip2WriteFn write_fn, void *write_context);

#endif
