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
int tool_prompt_yes_no(const char *message, const char *path);
const char *tool_base_name(const char *path);
void tool_format_size(unsigned long long value, int human_readable, char *buffer, size_t buffer_size);
int tool_join_path(const char *dir_path, const char *name, char *buffer, size_t buffer_size);
int tool_wildcard_match(const char *pattern, const char *text);
int tool_resolve_destination(const char *source_path, const char *dest_path, char *buffer, size_t buffer_size);
int tool_canonicalize_path_policy(const char *path, int resolve_symlinks, int allow_missing, int logical_policy, char *buffer, size_t buffer_size);
int tool_canonicalize_path(const char *path, int resolve_symlinks, int allow_missing, char *buffer, size_t buffer_size);
int tool_path_exists(const char *path);
int tool_paths_equal(const char *left_path, const char *right_path);
int tool_path_is_root(const char *path);
int tool_copy_file(const char *source_path, const char *dest_path);
int tool_copy_path(const char *source_path, const char *dest_path, int recursive, int preserve_mode, int preserve_symlinks);
int tool_remove_path(const char *path, int recursive);

#endif
