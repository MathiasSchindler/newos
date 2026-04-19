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
#define MAKE_MAX_DEPS 32
#define MAKE_MAX_COMMANDS 32
#define MAKE_MAX_VARS 64
#define MAKE_MAX_PHONY 64
#define MAKE_NAME_CAPACITY 128
#define MAKE_VALUE_CAPACITY 512
#define MAKE_COMMAND_CAPACITY 512
#define MAKE_LINE_CAPACITY 1024

typedef struct {
    char name[MAKE_NAME_CAPACITY];
    char value[MAKE_VALUE_CAPACITY];
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
} MakeProgram;

/* ── make_parse.c ── */
char *trim_leading_whitespace(char *text);
int set_variable(MakeProgram *program, const char *name, const char *value);
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
int build_target(MakeProgram *program, const char *target);

#endif /* MAKE_IMPL_H */
