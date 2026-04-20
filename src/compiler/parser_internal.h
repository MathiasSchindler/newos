#ifndef NEWOS_COMPILER_PARSER_INTERNAL_H
#define NEWOS_COMPILER_PARSER_INTERNAL_H

#include "parser.h"
#include "runtime.h"

typedef struct {
    char name[COMPILER_TYPEDEF_NAME_CAPACITY];
    int is_function;
    int is_array;
    int pointer_depth;
    unsigned long long array_length;
    char parameter_names[64][COMPILER_TYPEDEF_NAME_CAPACITY];
    CompilerType parameter_types[64];
    size_t parameter_count;
} CompilerDeclarator;

#define token_text_equals parser_token_text_equals
#define current_is_punct parser_current_is_punct
#define current_is_keyword parser_current_is_keyword
#define current_is_identifier parser_current_is_identifier
#define current_is_storage_class_keyword parser_current_is_storage_class_keyword
#define current_is_type_qualifier_keyword parser_current_is_type_qualifier_keyword
#define current_is_aggregate_type_keyword parser_current_is_aggregate_type_keyword
#define current_is_arithmetic_type_keyword parser_current_is_arithmetic_type_keyword
#define current_is_int_family_keyword parser_current_is_int_family_keyword
#define current_is_assignment_op parser_current_is_assignment_op
#define copy_token_text parser_copy_token_text
#define set_error parser_set_error
#define semantic_error parser_semantic_error
#define ir_error parser_ir_error
#define emit_ir_status parser_emit_ir_status
#define copy_normalized_span parser_copy_normalized_span
#define advance parser_advance
#define peek_token parser_peek_token
#define emit_ast_line parser_emit_ast_line
#define add_typedef_name parser_add_typedef_name
#define is_typedef_name parser_is_typedef_name
#define maybe_type_identifier parser_maybe_type_identifier
#define token_starts_decl_specifier parser_token_starts_decl_specifier
#define looks_like_declaration parser_looks_like_declaration
#define consume_punct parser_consume_punct
#define expect_punct parser_expect_punct
#define expect_identifier parser_expect_identifier
#define skip_balanced_group parser_skip_balanced_group

int parser_token_text_equals(const CompilerToken *token, const char *text);
int parser_current_is_punct(const CompilerParser *parser, const char *text);
int parser_current_is_keyword(const CompilerParser *parser, const char *text);
int parser_current_is_identifier(const CompilerParser *parser);
int parser_current_is_storage_class_keyword(const CompilerParser *parser);
int parser_current_is_type_qualifier_keyword(const CompilerParser *parser);
int parser_current_is_aggregate_type_keyword(const CompilerParser *parser);
int parser_current_is_arithmetic_type_keyword(const CompilerParser *parser);
int parser_current_is_int_family_keyword(const CompilerParser *parser);
int parser_current_is_assignment_op(const CompilerParser *parser);
void parser_copy_token_text(const CompilerToken *token, char *buffer, size_t buffer_size);
void parser_set_error(CompilerParser *parser, const char *message);
int parser_semantic_error(CompilerParser *parser);
int parser_ir_error(CompilerParser *parser);
int parser_emit_ir_status(CompilerParser *parser, int status);
void parser_copy_normalized_span(const char *start, const char *end, char *buffer, size_t buffer_size, const char *fallback);
int parser_advance(CompilerParser *parser);
int parser_peek_token(const CompilerParser *parser, CompilerToken *token_out);
int parser_emit_ast_line(CompilerParser *parser, const char *kind, const char *name);
int parser_add_typedef_name(CompilerParser *parser, const char *name);
int parser_is_typedef_name(const CompilerParser *parser, const CompilerToken *token);
int parser_maybe_type_identifier(const CompilerParser *parser, int allow_unknown_identifiers);
int parser_token_starts_decl_specifier(const CompilerParser *parser);
int parser_looks_like_declaration(const CompilerParser *parser);
int parser_consume_punct(CompilerParser *parser, const char *text);
int parser_expect_punct(CompilerParser *parser, const char *text);
int parser_expect_identifier(CompilerParser *parser, char *name_out, size_t name_size, int allow_missing);
int parser_skip_balanced_group(CompilerParser *parser, const char *open_text, const char *close_text);

int parse_expression(CompilerParser *parser);
int parse_assignment_expression(CompilerParser *parser);
int parse_initializer(CompilerParser *parser);
int parse_statement(CompilerParser *parser);
int parse_compound_statement(CompilerParser *parser);
int parse_declaration_or_function(CompilerParser *parser, int allow_function_body, int emit_summary);
int parse_declarator(CompilerParser *parser, CompilerDeclarator *declarator, int allow_abstract);
int parse_declaration_specifiers(CompilerParser *parser, int *is_typedef_out, int *is_extern_out, int *is_static_out, CompilerType *type_out);
int looks_like_type_name_after_lparen(const CompilerParser *parser);
int looks_like_compound_literal_after_lparen(const CompilerParser *parser);
int parse_type_name(CompilerParser *parser);
int parse_enum_specifier(CompilerParser *parser);

#endif
