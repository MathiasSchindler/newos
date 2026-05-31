#ifndef NEWOS_COMPILER_IR_INTERNAL_H
#define NEWOS_COMPILER_IR_INTERNAL_H

#include "ir.h"
#include "runtime.h"

#define COMPILER_IR_TRACKED_VALUE_CAPACITY 256
#define COMPILER_IR_INITIAL_LINE_CAPACITY 1024U

typedef struct {
    char name[COMPILER_IR_NAME_CAPACITY];
    long long value;
} IrTrackedValue;

typedef struct {
    IrTrackedValue global_constants[COMPILER_IR_TRACKED_VALUE_CAPACITY];
    size_t global_constant_count;
    IrTrackedValue local_values[COMPILER_IR_TRACKED_VALUE_CAPACITY];
    size_t local_value_count;
} IrOptimizerState;

typedef enum {
    IR_CONST_TOKEN_EOF = 0,
    IR_CONST_TOKEN_IDENTIFIER,
    IR_CONST_TOKEN_NUMBER,
    IR_CONST_TOKEN_CHAR,
    IR_CONST_TOKEN_PUNCT,
    IR_CONST_TOKEN_INVALID
} IrConstTokenKind;

typedef struct {
    IrConstTokenKind kind;
    char text[COMPILER_IR_NAME_CAPACITY];
    long long number_value;
} IrConstToken;

typedef struct {
    const char *cursor;
    IrConstToken current;
    const IrOptimizerState *state;
} IrConstParser;

int ir_text_equals(const char *lhs, const char *rhs);
int ir_starts_with(const char *text, const char *prefix);
const char *ir_skip_spaces(const char *text);
int ir_is_identifier_char(char ch);
void ir_const_next(IrConstParser *parser);
int ir_lookup_value(const IrOptimizerState *state, const char *name, long long *value_out);
int ir_apply_binary_op(const char *op, long long lhs, long long rhs, long long *value_out);
int ir_evaluate_constant_expression(const char *expr, const IrOptimizerState *state, long long *value_out);

#endif