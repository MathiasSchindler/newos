#ifndef NEWOS_COMPILER_LEXER_H
#define NEWOS_COMPILER_LEXER_H

#include "compiler.h"
#include "source.h"

typedef enum {
    COMPILER_TOKEN_EOF = 0,
    COMPILER_TOKEN_IDENTIFIER,
    COMPILER_TOKEN_KEYWORD,
    COMPILER_TOKEN_NUMBER,
    COMPILER_TOKEN_STRING,
    COMPILER_TOKEN_CHAR,
    COMPILER_TOKEN_PUNCTUATOR
} CompilerTokenKind;

typedef struct {
    CompilerTokenKind kind;
    const char *start;
    size_t length;
    unsigned long long line;
    unsigned long long column;
} CompilerToken;

typedef struct {
    const CompilerSource *source;
    const char *cursor;
    unsigned long long line;
    unsigned long long column;
    char error_message[COMPILER_ERROR_CAPACITY];
} CompilerLexer;

void compiler_lexer_init(CompilerLexer *lexer, const CompilerSource *source);
int compiler_lexer_next(CompilerLexer *lexer, CompilerToken *token_out);
const char *compiler_token_kind_name(CompilerTokenKind kind);
const char *compiler_lexer_error_message(const CompilerLexer *lexer);

#endif
