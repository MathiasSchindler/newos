#ifndef NEWOS_COMPILER_SEMANTIC_H
#define NEWOS_COMPILER_SEMANTIC_H

#include <stddef.h>

#include "compiler.h"

#define COMPILER_MAX_SYMBOLS 1024
#define COMPILER_MAX_SCOPES 128
#define COMPILER_SYMBOL_NAME_CAPACITY 64

typedef enum {
    COMPILER_BASE_UNKNOWN = 0,
    COMPILER_BASE_VOID,
    COMPILER_BASE_CHAR,
    COMPILER_BASE_INT,
    COMPILER_BASE_STRUCT,
    COMPILER_BASE_UNION,
    COMPILER_BASE_ENUM
} CompilerBaseType;

typedef struct {
    CompilerBaseType base;
    int pointer_depth;
    int is_function;
    int is_array;
    int is_unsigned;
} CompilerType;

typedef enum {
    COMPILER_SYMBOL_OBJECT = 0,
    COMPILER_SYMBOL_FUNCTION,
    COMPILER_SYMBOL_TYPEDEF,
    COMPILER_SYMBOL_ENUM_CONSTANT
} CompilerSymbolKind;

typedef struct {
    char name[COMPILER_SYMBOL_NAME_CAPACITY];
    CompilerSymbolKind kind;
    CompilerType type;
    size_t scope_level;
    int defined;
    long long constant_value;
    int has_constant_value;
} CompilerSymbol;

typedef struct {
    CompilerSymbol symbols[COMPILER_MAX_SYMBOLS];
    size_t symbol_count;
    size_t scope_markers[COMPILER_MAX_SCOPES];
    size_t scope_count;
    CompilerType current_function_type;
    int in_function;
    char error_message[COMPILER_ERROR_CAPACITY];
} CompilerSemantic;

void compiler_type_init(CompilerType *type);
void compiler_semantic_init(CompilerSemantic *semantic);
int compiler_semantic_enter_scope(CompilerSemantic *semantic);
void compiler_semantic_exit_scope(CompilerSemantic *semantic);
int compiler_semantic_declare(
    CompilerSemantic *semantic,
    const char *name,
    CompilerSymbolKind kind,
    const CompilerType *type,
    int is_definition
);
int compiler_semantic_declare_constant(CompilerSemantic *semantic, const char *name, long long value);
int compiler_semantic_use_identifier(CompilerSemantic *semantic, const char *name, int as_function_call);
int compiler_semantic_lookup_constant(const CompilerSemantic *semantic, const char *name, long long *value_out);
void compiler_semantic_begin_function(CompilerSemantic *semantic, const CompilerType *type);
void compiler_semantic_end_function(CompilerSemantic *semantic);
int compiler_semantic_check_return(CompilerSemantic *semantic, int has_value);
const char *compiler_semantic_error_message(const CompilerSemantic *semantic);

#endif
