#ifndef NEWOS_COMPILER_PREPROCESSOR_H
#define NEWOS_COMPILER_PREPROCESSOR_H

#include "compiler.h"
#include "source.h"

#define COMPILER_MAX_INCLUDE_DIRS 16
#define COMPILER_MAX_MACROS 512
#define COMPILER_MACRO_NAME_CAPACITY 64
#define COMPILER_MACRO_VALUE_CAPACITY 256

typedef struct {
    char name[COMPILER_MACRO_NAME_CAPACITY];
    char value[COMPILER_MACRO_VALUE_CAPACITY];
    char parameter_name[COMPILER_MACRO_NAME_CAPACITY];
    int defined;
    int is_function_like;
} CompilerMacro;

typedef struct {
    char include_dirs[COMPILER_MAX_INCLUDE_DIRS][COMPILER_PATH_CAPACITY];
    size_t include_dir_count;
    CompilerMacro macros[COMPILER_MAX_MACROS];
    size_t macro_count;
    char error_path[COMPILER_PATH_CAPACITY];
    unsigned long long error_line;
    char error_message[COMPILER_ERROR_CAPACITY];
} CompilerPreprocessor;

void compiler_preprocessor_init(CompilerPreprocessor *preprocessor);
int compiler_preprocessor_add_include_dir(CompilerPreprocessor *preprocessor, const char *path);
int compiler_preprocessor_define(CompilerPreprocessor *preprocessor, const char *name, const char *value);
int compiler_preprocessor_undefine(CompilerPreprocessor *preprocessor, const char *name);
int compiler_preprocess_file(CompilerPreprocessor *preprocessor, const char *path, CompilerSource *source_out);
const char *compiler_preprocessor_error_message(const CompilerPreprocessor *preprocessor);
const char *compiler_preprocessor_error_path(const CompilerPreprocessor *preprocessor);
unsigned long long compiler_preprocessor_error_line(const CompilerPreprocessor *preprocessor);

#endif
