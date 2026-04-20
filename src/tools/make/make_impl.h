/*
 * make_impl.h - internal shared types and declarations for the make tool.
 *
 * Included by make.c, make_parse.c, and make_exec.c only.
 */

#ifndef MAKE_IMPL_H
#define MAKE_IMPL_H

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define MAKE_MAX_RULES 128
#define MAKE_MAX_DEPS 128
#define MAKE_MAX_COMMANDS 16
#define MAKE_MAX_VARS 128
#define MAKE_MAX_PHONY 128
#define MAKE_NAME_CAPACITY 256
#define MAKE_VALUE_CAPACITY 4096
#define MAKE_COMMAND_CAPACITY 4096
#define MAKE_LINE_CAPACITY 4096
#define MAKE_SCRIPT_CAPACITY (MAKE_COMMAND_CAPACITY * MAKE_MAX_COMMANDS)

typedef enum {
    MAKE_ORIGIN_FILE = 1,
    MAKE_ORIGIN_COMMAND_LINE = 2,
    MAKE_ORIGIN_ENVIRONMENT = 3
} MakeVariableOrigin;

typedef struct {
    char name[MAKE_NAME_CAPACITY];
    char value[MAKE_VALUE_CAPACITY];
    MakeVariableOrigin origin;
    int exported;
} MakeVariable;

typedef struct {
    char target[MAKE_NAME_CAPACITY];
    char stem[MAKE_NAME_CAPACITY];
    char deps[MAKE_MAX_DEPS][MAKE_NAME_CAPACITY];
    size_t dep_count;
    char commands[MAKE_MAX_COMMANDS][MAKE_COMMAND_CAPACITY];
    size_t command_count;
    int is_pattern;
    int building;
    int built;
} MakeRule;

typedef struct {
    MakeVariable vars[MAKE_MAX_VARS];
    size_t var_count;
    MakeRule rules[MAKE_MAX_RULES];
    size_t rule_count;
    char phony[MAKE_MAX_PHONY][MAKE_NAME_CAPACITY];
    size_t phony_count;
    char first_target[MAKE_NAME_CAPACITY];
    char active_targets[MAKE_MAX_RULES][MAKE_NAME_CAPACITY];
    size_t active_count;
    int dry_run;
    int silent;
    int always_make;
    int jobs_flag_present;
    unsigned long long requested_jobs;
    int oneshell;
    char program_name[MAKE_NAME_CAPACITY];
    char makefile_path[MAKE_LINE_CAPACITY];
} MakeProgram;

/* ── make_parse.c ── */
char *trim_leading_whitespace(char *text);
int set_variable(MakeProgram *program, const char *name, const char *value);
int set_variable_with_origin(MakeProgram *program, const char *name, const char *value, MakeVariableOrigin origin);
const char *get_variable_value(const MakeProgram *program, const char *name);
int mark_variable_exported(MakeProgram *program, const char *name, int exported);
int sync_program_environment(const MakeProgram *program);
int expand_text(const MakeProgram *program, const MakeRule *rule, const char *text, char *out, size_t out_size);
MakeRule *find_rule(MakeProgram *program, const char *target);
int is_phony_target(const MakeProgram *program, const char *name);
int is_target_active(const MakeProgram *program, const char *name);
int push_active_target(MakeProgram *program, const char *name);
void pop_active_target(MakeProgram *program);
MakeRule *find_pattern_rule(MakeProgram *program, const char *target, char *stem_out, size_t stem_size);
int instantiate_pattern_rule(const MakeRule *pattern_rule, const char *target, const char *stem, MakeRule *out_rule);
int parse_makefile(MakeProgram *program, const char *path);

/* ── make_exec.c ── */
int path_exists_and_mtime(const char *path, long long *mtime_out);
unsigned long long effective_job_count(const MakeProgram *program);
int build_targets(MakeProgram *program, const char **targets, size_t target_count);
int build_target(MakeProgram *program, const char *target);

#endif /* MAKE_IMPL_H */
