#include "parser.h"

#include "runtime.h"

typedef struct {
    char name[COMPILER_TYPEDEF_NAME_CAPACITY];
    int is_function;
    int is_array;
    int pointer_depth;
    char parameter_names[64][COMPILER_TYPEDEF_NAME_CAPACITY];
    size_t parameter_count;
} CompilerDeclarator;

static int parse_expression(CompilerParser *parser);
static int parse_assignment_expression(CompilerParser *parser);
static int parse_statement(CompilerParser *parser);
static int parse_compound_statement(CompilerParser *parser);
static int parse_declaration_or_function(CompilerParser *parser, int allow_function_body, int emit_summary);
static int parse_declarator(CompilerParser *parser, CompilerDeclarator *declarator, int allow_abstract);
static int parse_constant_expression(CompilerParser *parser, long long *value_out);
static int parse_constant_unary(CompilerParser *parser, long long *value_out);
static int parse_constant_multiplicative(CompilerParser *parser, long long *value_out);
static int parse_constant_additive(CompilerParser *parser, long long *value_out);
static int parse_constant_shift(CompilerParser *parser, long long *value_out);
static int parse_constant_relational(CompilerParser *parser, long long *value_out);
static int parse_constant_equality(CompilerParser *parser, long long *value_out);
static int parse_constant_bitand(CompilerParser *parser, long long *value_out);
static int parse_constant_bitxor(CompilerParser *parser, long long *value_out);
static int parse_constant_bitor(CompilerParser *parser, long long *value_out);
static int parse_constant_logical_and(CompilerParser *parser, long long *value_out);
static int parse_constant_logical_or(CompilerParser *parser, long long *value_out);
static int parse_cast_expression(CompilerParser *parser);
static int parse_unary_expression(CompilerParser *parser);
static int parse_multiplicative_expression(CompilerParser *parser);
static int parse_additive_expression(CompilerParser *parser);
static int parse_shift_expression(CompilerParser *parser);
static int parse_relational_expression(CompilerParser *parser);
static int parse_equality_expression(CompilerParser *parser);
static int parse_bitand_expression(CompilerParser *parser);
static int parse_bitxor_expression(CompilerParser *parser);
static int parse_bitor_expression(CompilerParser *parser);
static int parse_logical_and_expression(CompilerParser *parser);
static int parse_logical_or_expression(CompilerParser *parser);
static int parse_enum_specifier(CompilerParser *parser);

static int token_text_equals(const CompilerToken *token, const char *text) {
    size_t i = 0;

    while (i < token->length && text[i] != '\0') {
        if (token->start[i] != text[i]) {
            return 0;
        }
        i += 1;
    }

    return i == token->length && text[i] == '\0';
}

static int current_is_punct(const CompilerParser *parser, const char *text) {
    return parser->current.kind == COMPILER_TOKEN_PUNCTUATOR && token_text_equals(&parser->current, text);
}

static int current_is_keyword(const CompilerParser *parser, const char *text) {
    return parser->current.kind == COMPILER_TOKEN_KEYWORD && token_text_equals(&parser->current, text);
}

static int current_is_identifier(const CompilerParser *parser) {
    return parser->current.kind == COMPILER_TOKEN_IDENTIFIER;
}

static int current_is_storage_class_keyword(const CompilerParser *parser) {
    if (current_is_keyword(parser, "typedef") ||
        current_is_keyword(parser, "extern") ||
        current_is_keyword(parser, "static") ||
        current_is_keyword(parser, "auto") ||
        current_is_keyword(parser, "register") ||
        current_is_keyword(parser, "inline")) {
        return 1;
    }
    return 0;
}

static int current_is_type_qualifier_keyword(const CompilerParser *parser) {
    if (current_is_keyword(parser, "const") ||
        current_is_keyword(parser, "volatile") ||
        current_is_keyword(parser, "restrict")) {
        return 1;
    }
    return 0;
}

static int current_is_aggregate_type_keyword(const CompilerParser *parser) {
    if (current_is_keyword(parser, "struct") ||
        current_is_keyword(parser, "union") ||
        current_is_keyword(parser, "enum")) {
        return 1;
    }
    return 0;
}

static int current_is_arithmetic_type_keyword(const CompilerParser *parser) {
    if (current_is_keyword(parser, "void") ||
        current_is_keyword(parser, "char") ||
        current_is_keyword(parser, "short") ||
        current_is_keyword(parser, "int") ||
        current_is_keyword(parser, "long") ||
        current_is_keyword(parser, "signed") ||
        current_is_keyword(parser, "unsigned") ||
        current_is_keyword(parser, "float") ||
        current_is_keyword(parser, "double")) {
        return 1;
    }
    return 0;
}

static int current_is_int_family_keyword(const CompilerParser *parser) {
    if (current_is_keyword(parser, "short") ||
        current_is_keyword(parser, "int") ||
        current_is_keyword(parser, "long") ||
        current_is_keyword(parser, "signed") ||
        current_is_keyword(parser, "float") ||
        current_is_keyword(parser, "double")) {
        return 1;
    }
    return 0;
}

static int current_is_assignment_op(const CompilerParser *parser) {
    return current_is_punct(parser, "=") ||
           current_is_punct(parser, "+=") ||
           current_is_punct(parser, "-=") ||
           current_is_punct(parser, "*=") ||
           current_is_punct(parser, "/=") ||
           current_is_punct(parser, "%=") ||
           current_is_punct(parser, "<<=") ||
           current_is_punct(parser, ">>=") ||
           current_is_punct(parser, "&=") ||
           current_is_punct(parser, "^=") ||
           current_is_punct(parser, "|=");
}

static void copy_token_text(const CompilerToken *token, char *buffer, size_t buffer_size) {
    size_t count = token->length;

    if (buffer_size == 0) {
        return;
    }

    if (count + 1 > buffer_size) {
        count = buffer_size - 1;
    }

    memcpy(buffer, token->start, count);
    buffer[count] = '\0';
}

static void set_error(CompilerParser *parser, const char *message) {
    rt_copy_string(parser->error_message, sizeof(parser->error_message), message);
    parser->error_line = parser->current.line;
    parser->error_column = parser->current.column;
}

static int semantic_error(CompilerParser *parser) {
    set_error(parser, compiler_semantic_error_message(&parser->semantic));
    return -1;
}

static int ir_error(CompilerParser *parser) {
    set_error(parser, compiler_ir_error_message(&parser->ir));
    return -1;
}

static int emit_ir_status(CompilerParser *parser, int status) {
    return status == 0 ? 0 : ir_error(parser);
}

static void copy_normalized_span(const char *start, const char *end, char *buffer, size_t buffer_size, const char *fallback) {
    size_t out = 0;
    int in_space = 1;

    if (buffer_size == 0) {
        return;
    }

    if (start == 0 || end == 0 || end < start) {
        rt_copy_string(buffer, buffer_size, fallback != 0 ? fallback : "");
        return;
    }

    while (start < end && rt_is_space(*start)) {
        start += 1;
    }
    while (end > start && rt_is_space(end[-1])) {
        end -= 1;
    }

    while (start < end && out + 1 < buffer_size) {
        char ch = *start++;

        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == '\v' || ch == '\f') {
            ch = ' ';
        }

        if (ch == ' ') {
            if (in_space) {
                continue;
            }
            in_space = 1;
        } else {
            in_space = 0;
        }

        buffer[out++] = ch;
    }

    buffer[out] = '\0';
    if (out == 0 && fallback != 0) {
        rt_copy_string(buffer, buffer_size, fallback);
    }
}

static int advance(CompilerParser *parser) {
    if (compiler_lexer_next(&parser->lexer, &parser->current) != 0) {
        rt_copy_string(parser->error_message, sizeof(parser->error_message), compiler_lexer_error_message(&parser->lexer));
        parser->error_line = parser->lexer.line;
        parser->error_column = parser->lexer.column;
        return -1;
    }

    return 0;
}

static int peek_token(const CompilerParser *parser, CompilerToken *token_out) {
    CompilerLexer snapshot = parser->lexer;
    return compiler_lexer_next(&snapshot, token_out);
}

static int emit_ast_line(CompilerParser *parser, const char *kind, const char *name) {
    if (!parser->dump_ast || name == 0 || name[0] == '\0') {
        return 0;
    }

    if (rt_write_cstr(parser->output_fd, kind) != 0 ||
        rt_write_char(parser->output_fd, ' ') != 0 ||
        rt_write_line(parser->output_fd, name) != 0) {
        set_error(parser, "failed while writing AST output");
        return -1;
    }

    return 0;
}

static int add_typedef_name(CompilerParser *parser, const char *name) {
    size_t i;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }

    for (i = 0; i < parser->typedef_count; ++i) {
        if (rt_strcmp(parser->typedef_names[i], name) == 0) {
            return 0;
        }
    }

    if (parser->typedef_count >= COMPILER_MAX_TYPEDEF_NAMES) {
        return -1;
    }

    rt_copy_string(parser->typedef_names[parser->typedef_count], sizeof(parser->typedef_names[parser->typedef_count]), name);
    parser->typedef_count += 1U;
    return 0;
}

static int is_typedef_name(const CompilerParser *parser, const CompilerToken *token) {
    char name[COMPILER_TYPEDEF_NAME_CAPACITY];
    size_t i;

    if (token->kind != COMPILER_TOKEN_IDENTIFIER) {
        return 0;
    }

    copy_token_text(token, name, sizeof(name));
    for (i = 0; i < parser->typedef_count; ++i) {
        if (rt_strcmp(parser->typedef_names[i], name) == 0) {
            return 1;
        }
    }

    return 0;
}

static int maybe_type_identifier(const CompilerParser *parser, int allow_unknown_identifiers) {
    CompilerToken next;

    if (!current_is_identifier(parser)) {
        return 0;
    }

    if (is_typedef_name(parser, &parser->current)) {
        return 1;
    }

    if (!allow_unknown_identifiers) {
        return 0;
    }

    if (peek_token(parser, &next) != 0) {
        return 0;
    }

    if (next.kind == COMPILER_TOKEN_IDENTIFIER) {
        return 1;
    }

    if (next.kind == COMPILER_TOKEN_PUNCTUATOR && token_text_equals(&next, "*")) {
        return 1;
    }

    return 0;
}

static int token_starts_decl_specifier(const CompilerParser *parser) {
    if (current_is_storage_class_keyword(parser) ||
        current_is_type_qualifier_keyword(parser) ||
        current_is_arithmetic_type_keyword(parser) ||
        current_is_aggregate_type_keyword(parser)) {
        return 1;
    }

    return maybe_type_identifier(parser, 1);
}

static int token_starts_known_type_specifier(const CompilerParser *parser) {
    if (current_is_type_qualifier_keyword(parser) ||
        current_is_arithmetic_type_keyword(parser) ||
        current_is_aggregate_type_keyword(parser)) {
        return 1;
    }

    return maybe_type_identifier(parser, 0);
}

static int looks_like_declaration(const CompilerParser *parser) {
    return token_starts_decl_specifier(parser);
}

static int consume_punct(CompilerParser *parser, const char *text) {
    if (!current_is_punct(parser, text)) {
        return 0;
    }

    return advance(parser) == 0 ? 1 : -1;
}

static int expect_punct(CompilerParser *parser, const char *text) {
    int consumed = consume_punct(parser, text);
    if (consumed == 1) {
        return 0;
    }
    if (consumed < 0) {
        return -1;
    }
    set_error(parser, "expected punctuation");
    return -1;
}

static int expect_identifier(CompilerParser *parser, char *name_out, size_t name_size, int allow_missing) {
    if (current_is_identifier(parser)) {
        if (name_out != 0) {
            copy_token_text(&parser->current, name_out, name_size);
        }
        return advance(parser);
    }

    if (allow_missing) {
        if (name_out != 0 && name_size > 0) {
            name_out[0] = '\0';
        }
        return 0;
    }

    set_error(parser, "expected identifier");
    return -1;
}

static int skip_balanced_group(CompilerParser *parser, const char *open_text, const char *close_text) {
    int depth = 1;

    if (expect_punct(parser, open_text) != 0) {
        return -1;
    }

    while (parser->current.kind != COMPILER_TOKEN_EOF && depth > 0) {
        if (current_is_punct(parser, open_text)) {
            depth += 1;
        } else if (current_is_punct(parser, close_text)) {
            depth -= 1;
        }

        if (advance(parser) != 0) {
            return -1;
        }
    }

    if (depth != 0) {
        set_error(parser, "unterminated balanced token group");
        return -1;
    }

    return 0;
}


#include "parser_types.inc"

#include "parser_expressions.inc"

#include "parser_declarations.inc"

#include "parser_statements.inc"

        "nfds_t", "speed_t", "tcflag_t", "sigset_t"
    };
    static const char *const builtin_objects[] = {
        "errno", "stdin", "stdout", "stderr", "environ"
    };
    size_t i;

    rt_memset(parser, 0, sizeof(*parser));
    parser->source = source;
    parser->dump_ast = dump_ast;
    parser->dump_ir = dump_ir;
    parser->output_fd = output_fd;
    compiler_lexer_init(&parser->lexer, source);
    compiler_ir_init(&parser->ir);
    compiler_semantic_init(&parser->semantic);

    for (i = 0; i < sizeof(builtin_types) / sizeof(builtin_types[0]); ++i) {
        CompilerType type;
        (void)add_typedef_name(parser, builtin_types[i]);
        compiler_type_init(&type);
        (void)compiler_semantic_declare(&parser->semantic, builtin_types[i], COMPILER_SYMBOL_TYPEDEF, &type, 1);
    }

    for (i = 0; i < sizeof(builtin_objects) / sizeof(builtin_objects[0]); ++i) {
        CompilerType type;
        rt_memset(&type, 0, sizeof(type));
        (void)compiler_semantic_declare(&parser->semantic, builtin_objects[i], COMPILER_SYMBOL_OBJECT, &type, 0);
    }

    (void)advance(parser);
}

int compiler_parse_translation_unit(CompilerParser *parser) {
    while (parser->current.kind != COMPILER_TOKEN_EOF) {
        if (parse_declaration_or_function(parser, 1, 1) != 0) {
            return -1;
        }
    }

    return 0;
}

const char *compiler_parser_error_message(const CompilerParser *parser) {
    return parser->error_message;
}

unsigned long long compiler_parser_error_line(const CompilerParser *parser) {
    return parser->error_line;
}

unsigned long long compiler_parser_error_column(const CompilerParser *parser) {
    return parser->error_column;
}
