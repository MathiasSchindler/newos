#ifndef NEWOS_SHELL_SHARED_H
#define NEWOS_SHELL_SHARED_H

#include "platform.h"
#include <stddef.h>

#define SH_MAX_LINE 4096
#define SH_CAPTURE_CAPACITY 2048
#define SH_ENTRY_CAPACITY 1024
#define SH_HEREDOC_PREFIX "/tmp/newos-sh-heredoc-"

typedef enum {
    SH_NEXT_ALWAYS = 0,
    SH_NEXT_AND = 1,
    SH_NEXT_OR = 2,
    SH_NEXT_BACKGROUND = 3
} ShNextMode;

typedef struct {
    char **argv;
    int *no_expand;
    int argc;
    size_t argv_capacity;
    char **owned_words;
    size_t owned_word_count;
    size_t owned_word_capacity;
    char *input_path;
    char *output_path;
    int output_append;
} ShCommand;

typedef struct {
    ShCommand *commands;
    size_t count;
    size_t capacity;
} ShPipeline;

typedef struct {
    int active;
    int job_id;
    int pid_count;
    int *pids;
    char command[SH_MAX_LINE];
} ShJob;

typedef struct {
    int active;
    char name[64];
    char value[SH_MAX_LINE];
} ShAlias;

typedef struct {
    int active;
    char name[64];
    char body[SH_MAX_LINE];
} ShFunction;

extern int shell_should_exit;
extern int shell_exit_status;
extern int shell_last_status;
extern int shell_next_job_id;
extern char shell_self_path[SH_MAX_LINE];
extern char **shell_history;
extern size_t shell_history_count;
extern size_t shell_history_capacity;
extern ShJob *shell_jobs;
extern size_t shell_jobs_count;
extern size_t shell_jobs_capacity;
extern ShAlias *shell_aliases;
extern size_t shell_alias_count;
extern size_t shell_alias_capacity;
extern ShFunction *shell_functions;
extern size_t shell_function_count;
extern size_t shell_function_capacity;

int sh_contains_slash(const char *text);
int sh_contains_wildcards(const char *text);
int sh_is_name_start_char(char ch);
int sh_is_name_char(char ch);
void sh_add_history_entry(const char *line);
size_t sh_shell_builtin_count(void);
const char *sh_shell_builtin_name_at(size_t index);
void sh_skip_spaces(char **cursor);
char *sh_scan_quoted_text(char *scan, char quote);
int sh_is_shell_builtin_name(const char *name);
const char *sh_lookup_shell_alias(const char *name);
int sh_set_shell_alias(const char *assignment);
const char *sh_lookup_shell_function(const char *name);
int sh_set_shell_function(const char *name, const char *body);
const char *sh_get_shell_parameter_zero(void);
void sh_set_shell_invocation_name(const char *name);
int sh_set_shell_positionals_from_argv(int argc, char *const *argv, int start_index);
int sh_shift_shell_positionals(unsigned int count);
int sh_resolve_shell_command_path(const char *name, char *buffer, size_t buffer_size);
int sh_read_line_from_fd(int fd, char *buffer, size_t buffer_size, int *eof_out);
int sh_read_logical_line_from_fd(int fd, char *buffer, size_t buffer_size, int *eof_out);
int sh_line_ends_with_continuation(const char *line);
int sh_append_continuation_line(char *line, size_t line_size, const char *next_line);
int sh_prepare_heredoc_from_fd(int fd, char *line, size_t line_size);
int sh_parse_pipeline(char *line, ShPipeline *pipeline, int *empty_out);
void sh_cleanup_pipeline_temp_inputs(const ShPipeline *pipeline);
void sh_free_pipeline(ShPipeline *pipeline);
ShJob *sh_find_job_by_id(int job_id);
int sh_execute_pipeline(const ShPipeline *pipeline, int background, const char *command_text);
int sh_try_run_builtin(const ShPipeline *pipeline, int *status_out);
int sh_shell_is_interactive(int fd);
int sh_process_interactive_stream(int (*run_line_fn)(char *line));

#endif
