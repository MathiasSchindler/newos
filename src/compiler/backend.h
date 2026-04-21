#ifndef NEWOS_COMPILER_BACKEND_H
#define NEWOS_COMPILER_BACKEND_H

#include "compiler.h"
#include "ir.h"
#include "targets/target_info.h"

typedef struct {
    CompilerTarget target;
    int function_sections;
    int data_sections;
    char error_message[COMPILER_ERROR_CAPACITY];
} CompilerBackend;

void compiler_backend_init(CompilerBackend *backend, CompilerTarget target, int function_sections, int data_sections);
int compiler_backend_emit_assembly(CompilerBackend *backend, const CompilerIr *ir, int fd);
const char *compiler_backend_error_message(const CompilerBackend *backend);

#endif
