#ifndef NEWOS_TOOL_UTIL_H
#define NEWOS_TOOL_UTIL_H

#include <stddef.h>

int tool_open_input(const char *path, int *fd_out, int *should_close_out);
void tool_close_input(int fd, int should_close);
void tool_write_usage(const char *program_name, const char *usage_suffix);
void tool_write_error(const char *tool_name, const char *message, const char *detail);
int tool_parse_uint_arg(const char *text, unsigned long long *value_out, const char *tool_name, const char *what);
int tool_parse_int_arg(const char *text, long long *value_out, const char *tool_name, const char *what);
int tool_parse_signal_name(const char *text, int *signal_out);
const char *tool_signal_name(int signal_number);
void tool_write_signal_list(int fd);
const char *tool_base_name(const char *path);
void tool_format_size(unsigned long long value, int human_readable, char *buffer, size_t buffer_size);
int tool_join_path(const char *dir_path, const char *name, char *buffer, size_t buffer_size);
int tool_wildcard_match(const char *pattern, const char *text);
int tool_resolve_destination(const char *source_path, const char *dest_path, char *buffer, size_t buffer_size);
int tool_copy_file(const char *source_path, const char *dest_path);

#endif
