#ifndef NEWOS_COMPILER_PARSER_H
#define NEWOS_COMPILER_PARSER_H

#include "ir.h"
#include "lexer.h"
#include "semantic.h"

#define COMPILER_MAX_TYPEDEF_NAMES 256
#define COMPILER_TYPEDEF_NAME_CAPACITY 64
#define COMPILER_MAX_LOOP_DEPTH 64
#define COMPILER_MAX_POINTER_DEPTH 64
#define COMPILER_MAX_INITIALIZER_DEPTH 128
#define COMPILER_MAX_AGGREGATE_LAYOUTS 256
#define COMPILER_MAX_AGGREGATE_FIELDS 4096

typedef struct {
    char name[COMPILER_SYMBOL_NAME_CAPACITY];
    CompilerBaseType base;
    int is_union;
    unsigned long long size_bytes;
    unsigned long long align_bytes;
    size_t field_start;
    size_t field_count;
    int emitted;
} CompilerAggregateLayout;

typedef struct {
    char aggregate_name[COMPILER_SYMBOL_NAME_CAPACITY];
    char name[COMPILER_SYMBOL_NAME_CAPACITY];
    CompilerType type;
    unsigned short layout_id;
    unsigned long long offset_bytes;
    unsigned long long size_bytes;
} CompilerAggregateField;

typedef struct {
    const CompilerSource *source;
    CompilerLexer lexer;
    CompilerToken current;
    char error_message[COMPILER_ERROR_CAPACITY];
    unsigned long long error_line;
    unsigned long long error_column;
    int dump_ast;
    int dump_ir;
    int output_fd;
    char typedef_names[COMPILER_MAX_TYPEDEF_NAMES][COMPILER_TYPEDEF_NAME_CAPACITY];
    size_t typedef_count;
    CompilerIr ir;
    CompilerSemantic semantic;
    CompilerType pending_function_type;
    char pending_function_name[COMPILER_TYPEDEF_NAME_CAPACITY];
    char pending_parameter_names[64][COMPILER_TYPEDEF_NAME_CAPACITY];
    CompilerType pending_parameter_types[64];
    size_t pending_parameter_count;
    int pending_function_scope;
    char break_labels[COMPILER_MAX_LOOP_DEPTH][COMPILER_TYPEDEF_NAME_CAPACITY];
    char continue_labels[COMPILER_MAX_LOOP_DEPTH][COMPILER_TYPEDEF_NAME_CAPACITY];
    size_t loop_depth;
    size_t initializer_depth;
    CompilerAggregateLayout aggregate_layouts[COMPILER_MAX_AGGREGATE_LAYOUTS];
    size_t aggregate_layout_count;
    CompilerAggregateField aggregate_fields[COMPILER_MAX_AGGREGATE_FIELDS];
    size_t aggregate_field_count;
} CompilerParser;

void compiler_parser_init(CompilerParser *parser, const CompilerSource *source, int dump_ast, int dump_ir, int output_fd);
int compiler_parse_translation_unit(CompilerParser *parser);
const char *compiler_parser_error_message(const CompilerParser *parser);
unsigned long long compiler_parser_error_line(const CompilerParser *parser);
unsigned long long compiler_parser_error_column(const CompilerParser *parser);

#endif
