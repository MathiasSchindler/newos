#include "lexer.h"

#include "runtime.h"

static int is_alpha_ascii(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

static int is_digit_ascii(char ch) {
    return ch >= '0' && ch <= '9';
}

static int is_identifier_start(char ch) {
    return is_alpha_ascii(ch) || ch == '_';
}

static int is_identifier_continue(char ch) {
    return is_identifier_start(ch) || is_digit_ascii(ch);
}

static int is_number_continue(char ch) {
    return is_identifier_continue(ch) || ch == '.';
}

static int same_text_n(const char *text, const char *candidate, size_t length) {
    size_t i;

    for (i = 0; i < length; ++i) {
        if (candidate[i] == '\0' || text[i] != candidate[i]) {
            return 0;
        }
    }

    return candidate[length] == '\0';
}

static int is_keyword(const char *text, size_t length) {
    static const char *const keywords[] = {
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline",
        "int", "long", "register", "restrict", "return", "short", "signed", "sizeof",
        "static", "struct", "switch", "typedef", "union", "unsigned", "void",
        "volatile", "while"
    };
    size_t i;

    for (i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i) {
        if (same_text_n(text, keywords[i], length)) {
            return 1;
        }
    }

    return 0;
}

static void set_error(CompilerLexer *lexer, const char *message) {
    rt_copy_string(lexer->error_message, sizeof(lexer->error_message), message);
}

static void advance_one(CompilerLexer *lexer) {
    if (*lexer->cursor == '\n') {
        lexer->line += 1ULL;
        lexer->column = 1ULL;
    } else if (*lexer->cursor != '\0') {
        lexer->column += 1ULL;
    }

    if (*lexer->cursor != '\0') {
        lexer->cursor += 1;
    }
}

static void make_token(CompilerToken *token, CompilerTokenKind kind, const char *start, size_t length, unsigned long long line, unsigned long long column) {
    token->kind = kind;
    token->start = start;
    token->length = length;
    token->line = line;
    token->column = column;
}

static int skip_ignored(CompilerLexer *lexer) {
    for (;;) {
        if (rt_is_space(*lexer->cursor)) {
            advance_one(lexer);
            continue;
        }

        if (lexer->cursor[0] == '/' && lexer->cursor[1] == '/') {
            while (*lexer->cursor != '\0' && *lexer->cursor != '\n') {
                advance_one(lexer);
            }
            continue;
        }

        if (lexer->cursor[0] == '/' && lexer->cursor[1] == '*') {
            int closed = 0;
            advance_one(lexer);
            advance_one(lexer);
            while (*lexer->cursor != '\0') {
                if (lexer->cursor[0] == '*' && lexer->cursor[1] == '/') {
                    advance_one(lexer);
                    advance_one(lexer);
                    closed = 1;
                    break;
                }
                advance_one(lexer);
            }

            if (!closed) {
                set_error(lexer, "unterminated block comment");
                return -1;
            }
            continue;
        }

        break;
    }

    return 0;
}

static size_t match_punctuator(const char *text) {
    static const char *const multi_char[] = {
        "<<=", ">>=", "...", "->", "++", "--", "<<", ">>",
        "<=", ">=", "==", "!=", "&&", "||",
        "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "##"
    };
    static const char single_char[] = "{}[]()#;:,?~+-*/%&|^!<>=.";
    size_t i;

    for (i = 0; i < sizeof(multi_char) / sizeof(multi_char[0]); ++i) {
        size_t j = 0;
        while (multi_char[i][j] != '\0' && text[j] == multi_char[i][j]) {
            j += 1;
        }
        if (multi_char[i][j] == '\0') {
            return j;
        }
    }

    for (i = 0; single_char[i] != '\0'; ++i) {
        if (text[0] == single_char[i]) {
            return 1U;
        }
    }

    return 0;
}

void compiler_lexer_init(CompilerLexer *lexer, const CompilerSource *source) {
    lexer->source = source;
    lexer->cursor = source->data;
    lexer->line = 1ULL;
    lexer->column = 1ULL;
    lexer->error_message[0] = '\0';
}

int compiler_lexer_next(CompilerLexer *lexer, CompilerToken *token_out) {
    const char *start;
    unsigned long long line;
    unsigned long long column;
    size_t length;
    size_t i;

    if (skip_ignored(lexer) != 0) {
        return -1;
    }

    start = lexer->cursor;
    line = lexer->line;
    column = lexer->column;

    if (*lexer->cursor == '\0') {
        make_token(token_out, COMPILER_TOKEN_EOF, lexer->cursor, 0, line, column);
        return 0;
    }

    if (is_identifier_start(*lexer->cursor)) {
        advance_one(lexer);
        while (is_identifier_continue(*lexer->cursor)) {
            advance_one(lexer);
        }
        length = (size_t)(lexer->cursor - start);
        make_token(
            token_out,
            is_keyword(start, length) ? COMPILER_TOKEN_KEYWORD : COMPILER_TOKEN_IDENTIFIER,
            start,
            length,
            line,
            column
        );
        return 0;
    }

    if (is_digit_ascii(*lexer->cursor) || (*lexer->cursor == '.' && is_digit_ascii(lexer->cursor[1]))) {
        advance_one(lexer);
        while (is_number_continue(*lexer->cursor)) {
            advance_one(lexer);
        }
        make_token(token_out, COMPILER_TOKEN_NUMBER, start, (size_t)(lexer->cursor - start), line, column);
        return 0;
    }

    if (*lexer->cursor == '"' || *lexer->cursor == '\'') {
        char quote = *lexer->cursor;
        CompilerTokenKind kind = (quote == '"') ? COMPILER_TOKEN_STRING : COMPILER_TOKEN_CHAR;

        advance_one(lexer);
        while (*lexer->cursor != '\0') {
            if (*lexer->cursor == '\n') {
                set_error(lexer, (quote == '"') ? "unterminated string literal" : "unterminated character literal");
                return -1;
            }
            if (*lexer->cursor == '\\' && lexer->cursor[1] != '\0') {
                advance_one(lexer);
                advance_one(lexer);
                continue;
            }
            if (*lexer->cursor == quote) {
                advance_one(lexer);
                make_token(token_out, kind, start, (size_t)(lexer->cursor - start), line, column);
                return 0;
            }
            advance_one(lexer);
        }

        set_error(lexer, (quote == '"') ? "unterminated string literal" : "unterminated character literal");
        return -1;
    }

    length = match_punctuator(lexer->cursor);
    if (length != 0U) {
        for (i = 0; i < length; ++i) {
            advance_one(lexer);
        }
        make_token(token_out, COMPILER_TOKEN_PUNCTUATOR, start, length, line, column);
        return 0;
    }

    set_error(lexer, "unexpected character");
    return -1;
}

const char *compiler_token_kind_name(CompilerTokenKind kind) {
    switch (kind) {
        case COMPILER_TOKEN_EOF:
            return "eof";
        case COMPILER_TOKEN_IDENTIFIER:
            return "identifier";
        case COMPILER_TOKEN_KEYWORD:
            return "keyword";
        case COMPILER_TOKEN_NUMBER:
            return "number";
        case COMPILER_TOKEN_STRING:
            return "string";
        case COMPILER_TOKEN_CHAR:
            return "char";
        case COMPILER_TOKEN_PUNCTUATOR:
            return "punct";
    }

    return "unknown";
}

const char *compiler_lexer_error_message(const CompilerLexer *lexer) {
    return lexer->error_message;
}
