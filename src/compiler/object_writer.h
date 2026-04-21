#ifndef NEWOS_COMPILER_OBJECT_WRITER_H
#define NEWOS_COMPILER_OBJECT_WRITER_H

#include "backend.h"
#include "compiler.h"
#include "ir.h"

typedef struct {
    char error_message[COMPILER_ERROR_CAPACITY];
} CompilerObjectWriter;

void compiler_object_writer_init(CompilerObjectWriter *writer);
int compiler_object_write_target(CompilerObjectWriter *writer, CompilerTarget target, const CompilerIr *ir, int fd, int function_sections, int data_sections);
int compiler_object_write_elf64_x86_64(CompilerObjectWriter *writer, const CompilerIr *ir, int fd, int function_sections, int data_sections);
int compiler_object_write_macho64_aarch64(CompilerObjectWriter *writer, const CompilerIr *ir, int fd, int function_sections, int data_sections);
const char *compiler_object_writer_error_message(const CompilerObjectWriter *writer);

#endif
