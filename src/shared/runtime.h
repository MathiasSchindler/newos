#ifndef NEWOS_RUNTIME_H
#define NEWOS_RUNTIME_H

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t count);
void *memmove(void *dst, const void *src, size_t count);
void *memset(void *buffer, int byte_value, size_t count);

size_t rt_strlen(const char *text);
int rt_strcmp(const char *lhs, const char *rhs);
void rt_copy_string(char *dst, size_t dst_size, const char *src);
int rt_join_path(const char *dir_path, const char *name, char *buffer, size_t buffer_size);
void rt_unsigned_to_string(unsigned long long value, char *buffer, size_t buffer_size);
void rt_memset(void *buffer, int byte_value, size_t count);
int rt_is_space(char ch);
int rt_is_digit_string(const char *text);
int rt_parse_pid_value(const char *text);
void rt_trim_newline(char *text);
int rt_parse_uint(const char *text, unsigned long long *value_out);

int rt_write_all(int fd, const void *data, size_t count);
int rt_write_cstr(int fd, const char *text);
int rt_write_line(int fd, const char *text);
int rt_write_char(int fd, char ch);
int rt_write_uint(int fd, unsigned long long value);
int rt_write_int(int fd, long long value);

#endif
