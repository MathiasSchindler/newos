#ifndef NEWOS_COMPILER_IR_H
#define NEWOS_COMPILER_IR_H

#include <stddef.h>

#include "semantic.h"

#define COMPILER_MAX_IR_LINES 8192
#define COMPILER_IR_LINE_CAPACITY 8192
#define COMPILER_IR_NAME_CAPACITY 64

typedef struct {
    char lines[COMPILER_MAX_IR_LINES][COMPILER_IR_LINE_CAPACITY];
    size_t count;
    unsigned int temp_counter;
    unsigned int label_counter;
    char error_message[COMPILER_ERROR_CAPACITY];
} CompilerIr;

void compiler_ir_init(CompilerIr *ir);
int compiler_ir_make_label(CompilerIr *ir, const char *prefix, char *buffer, size_t buffer_size);
int compiler_ir_optimize(CompilerIr *ir);
int compiler_ir_emit_function_begin(CompilerIr *ir, const char *name, const CompilerType *type);
int compiler_ir_emit_function_end(CompilerIr *ir, const char *name);
int compiler_ir_emit_constant(CompilerIr *ir, const char *name, long long value);
int compiler_ir_emit_decl(CompilerIr *ir, const char *storage, int is_function, const CompilerType *type, const char *name);
int compiler_ir_emit_assign(CompilerIr *ir, const char *name, const char *expr);
int compiler_ir_emit_eval(CompilerIr *ir, const char *expr);
int compiler_ir_emit_return(CompilerIr *ir, const char *expr);
int compiler_ir_emit_branch_zero(CompilerIr *ir, const char *expr, const char *label);
int compiler_ir_emit_jump(CompilerIr *ir, const char *label);
int compiler_ir_emit_label(CompilerIr *ir, const char *label);
int compiler_ir_emit_case(CompilerIr *ir, const char *expr);
int compiler_ir_emit_default(CompilerIr *ir);
int compiler_ir_emit_note(CompilerIr *ir, const char *keyword, const char *detail);
int compiler_ir_write_dump(const CompilerIr *ir, int fd);
const char *compiler_ir_error_message(const CompilerIr *ir);

#endif
