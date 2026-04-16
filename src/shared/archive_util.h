#ifndef NEWOS_ARCHIVE_UTIL_H
#define NEWOS_ARCHIVE_UTIL_H

#include <stddef.h>

unsigned int archive_crc32_update(unsigned int crc, const unsigned char *data, size_t length);
void archive_write_octal(char *field, size_t field_size, unsigned long long value);
unsigned long long archive_parse_octal(const char *field, size_t field_size);

#endif
