#ifndef NEWOS_TOOL_UTIL_H
#define NEWOS_TOOL_UTIL_H

#include <stddef.h>

int tool_open_input(const char *path, int *fd_out, int *should_close_out);
void tool_close_input(int fd, int should_close);
void tool_write_usage(const char *program_name, const char *usage_suffix);
void tool_write_error(const char *tool_name, const char *message, const char *detail);

/*
 * Lightweight iterative option parser.
 *
 * Pattern:
 *   ToolOptState s;
 *   int r;
 *   tool_opt_init(&s, argc, argv, "prog", "[-x] [-f FILE] ...");
 *   while ((r = tool_opt_next(&s)) == TOOL_OPT_FLAG) {
 *       if (rt_strcmp(s.flag, "-f") == 0) {
 *           if (tool_opt_require_value(&s) != 0) return 1;
 *           path = s.value;
 *       } else if (rt_strcmp(s.flag, "-x") == 0) {
 *           opt_x = 1;
 *       } else {
 *           tool_write_error(s.prog, "unknown option: ", s.flag);
 *           tool_write_usage(s.prog, s.usage_suffix);
 *           return 1;
 *       }
 *   }
 *   if (r == TOOL_OPT_HELP) { ... print help ...; return 0; }
 *   if (r == TOOL_OPT_ERROR) return 1;
 *   // s.argi is index of first non-option argument
 */

typedef struct {
    int          argc;
    char       **argv;
    const char  *prog;
    const char  *usage_suffix;
    int          argi;   /* index of next argv entry to examine */
    const char  *flag;   /* current option string set by tool_opt_next */
    const char  *value;  /* option value set by tool_opt_require_value */
} ToolOptState;

#define TOOL_OPT_END   0  /* no more options; s.argi = first positional index */
#define TOOL_OPT_FLAG  1  /* s.flag holds the current option string */
#define TOOL_OPT_HELP  2  /* -h or --help was seen */
#define TOOL_OPT_ERROR 3  /* invalid option; error already written to stderr */

void tool_opt_init(ToolOptState *s, int argc, char **argv,
                   const char *prog, const char *usage_suffix);
int  tool_opt_next(ToolOptState *s);
int  tool_opt_require_value(ToolOptState *s);
int tool_parse_uint_arg(const char *text, unsigned long long *value_out, const char *tool_name, const char *what);
int tool_parse_int_arg(const char *text, long long *value_out, const char *tool_name, const char *what);
int tool_parse_escaped_string(const char *text, char *buffer, size_t buffer_size, size_t *length_out);
int tool_parse_signal_name(const char *text, int *signal_out);
const char *tool_signal_name(int signal_number);
void tool_write_signal_list(int fd);
int tool_prompt_yes_no(const char *message, const char *path);
const char *tool_base_name(const char *path);
void tool_format_size(unsigned long long value, int human_readable, char *buffer, size_t buffer_size);
int tool_join_path(const char *dir_path, const char *name, char *buffer, size_t buffer_size);
int tool_wildcard_match(const char *pattern, const char *text);
int tool_regex_search(const char *pattern, const char *text, int ignore_case, size_t search_start, size_t *start_out, size_t *end_out);
int tool_regex_replace(const char *pattern, const char *replacement, const char *input, int ignore_case, int global, char *output, size_t output_size, int *changed_out);
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
