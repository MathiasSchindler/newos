/*
 * awk_impl.h - internal shared types and declarations for the awk tool.
 *
 * Included by awk.c, awk_parse.c, and awk_exec.c only.
 */

#ifndef AWK_IMPL_H
#define AWK_IMPL_H

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define AWK_LINE_CAPACITY 8192
#define AWK_MAX_CLAUSES 16
#define AWK_MAX_STATEMENTS 16
#define AWK_MAX_EXPRESSIONS 16
#define AWK_MAX_TEXT 256
#define AWK_MAX_FIELDS 256

typedef enum {
    AWK_CLAUSE_MAIN = 0,
    AWK_CLAUSE_BEGIN = 1,
    AWK_CLAUSE_END = 2
} AwkClauseKind;

typedef enum {
    AWK_PATTERN_NONE = 0,
    AWK_PATTERN_REGEX = 1,
    AWK_PATTERN_NR = 2,
    AWK_PATTERN_NF = 3,
    AWK_PATTERN_EXPR_REGEX = 4
} AwkPatternType;

typedef enum {
    AWK_COMPARE_EQ = 0,
    AWK_COMPARE_NE = 1,
    AWK_COMPARE_LT = 2,
    AWK_COMPARE_LE = 3,
    AWK_COMPARE_GT = 4,
    AWK_COMPARE_GE = 5
} AwkCompareOp;

typedef enum {
    AWK_EXPR_WHOLE_LINE = 0,
    AWK_EXPR_FIELD = 1,
    AWK_EXPR_NR = 2,
    AWK_EXPR_NF = 3,
    AWK_EXPR_FS = 4,
    AWK_EXPR_OFS = 5,
    AWK_EXPR_STRING = 6,
    AWK_EXPR_NUMBER = 7
} AwkExprType;

typedef enum {
    AWK_STATEMENT_PRINT = 0,
    AWK_STATEMENT_PRINTF = 1,
    AWK_STATEMENT_ASSIGN = 2
} AwkStatementKind;

typedef enum {
    AWK_VARIABLE_NONE = 0,
    AWK_VARIABLE_FS = 1,
    AWK_VARIABLE_OFS = 2
} AwkVariableName;

typedef struct {
    AwkExprType type;
    unsigned long long number;
    char text[AWK_MAX_TEXT];
} AwkExpression;

typedef struct {
    size_t expression_count;
    AwkExpression expressions[AWK_MAX_EXPRESSIONS];
    AwkStatementKind kind;
    AwkVariableName variable;
} AwkStatement;

typedef struct {
    AwkClauseKind kind;
    AwkPatternType pattern_type;
    AwkCompareOp compare_op;
    unsigned long long compare_value;
    int match_negated;
    char pattern_text[AWK_MAX_TEXT];
    AwkExpression match_expression;
    size_t statement_count;
    AwkStatement statements[AWK_MAX_STATEMENTS];
} AwkClause;

typedef struct {
    size_t clause_count;
    AwkClause clauses[AWK_MAX_CLAUSES];
} AwkProgram;

typedef struct {
    const char *line;
    unsigned long long nr;
    unsigned long long nf;
    const char *field_starts[AWK_MAX_FIELDS];
    size_t field_lengths[AWK_MAX_FIELDS];
} AwkRecord;

typedef struct {
    char fs[AWK_MAX_TEXT];
    char ofs[AWK_MAX_TEXT];
} AwkState;

/* ── awk_parse.c ── */
int parse_program(const char *program_text, AwkProgram *program);

/* ── awk_exec.c ── */
void init_state(AwkState *state);
void init_record(AwkRecord *record, const char *line, unsigned long long nr, const AwkState *state);
int execute_clauses(const AwkProgram *program, AwkClauseKind kind, const AwkRecord *record, AwkState *state);
int awk_stream(int fd, const AwkProgram *program, AwkState *state, unsigned long long *line_number, unsigned long long *last_nf);

#endif /* AWK_IMPL_H */
