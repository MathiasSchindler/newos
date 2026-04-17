#ifndef NEWOS_COMPILER_BACKEND_H
#define NEWOS_COMPILER_BACKEND_H

#include "compiler.h"
#include "ir.h"

typedef enum {
    COMPILER_BACKEND_TARGET_LINUX_X86_64 = 0,
    COMPILER_BACKEND_TARGET_LINUX_AARCH64 = 1,
    COMPILER_BACKEND_TARGET_MACOS_AARCH64 = 2
} CompilerBackendTarget;

typedef struct {
    CompilerBackendTarget target;
    char error_message[COMPILER_ERROR_CAPACITY];
} CompilerBackend;

void compiler_backend_init(CompilerBackend *backend, CompilerBackendTarget target);
int compiler_backend_emit_assembly(CompilerBackend *backend, const CompilerIr *ir, int fd);
const char *compiler_backend_error_message(const CompilerBackend *backend);

#endif
