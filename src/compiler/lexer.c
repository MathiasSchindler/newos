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

    if (text == 0 || candidate == 0) {
        return 0;
    }

    for (i = 0; i < length; ++i) {
        if (candidate[i] == '\0' || text[i] != candidate[i]) {
            return 0;
        }
    }

    return candidate[length] == '\0';
}

static int is_keyword(const char *text, size_t length) {
    if (text == 0 || length == 0U) {
        return 0;
    }

    switch (length) {
        case 2U:
            return (text[0] == 'd' && same_text_n(text, "do", length)) ||
                   (text[0] == 'i' && same_text_n(text, "if", length));
        case 3U:
            return (text[0] == 'f' && same_text_n(text, "for", length)) ||
                   (text[0] == 'i' && same_text_n(text, "int", length));
        case 4U:
            switch (text[0]) {
                case 'a': return same_text_n(text, "auto", length);
                case 'c': return same_text_n(text, "case", length) || same_text_n(text, "char", length);
                case 'e': return same_text_n(text, "else", length) || same_text_n(text, "enum", length);
                case 'g': return same_text_n(text, "goto", length);
                case 'l': return same_text_n(text, "long", length);
                case 'v': return same_text_n(text, "void", length);
                default: return 0;
            }
        case 5U:
            switch (text[0]) {
                case 'b': return same_text_n(text, "break", length);
                case 'c': return same_text_n(text, "const", length);
                case 'f': return same_text_n(text, "float", length);
                case 's': return same_text_n(text, "short", length);
                case 'u': return same_text_n(text, "union", length);
                case 'w': return same_text_n(text, "while", length);
                default: return 0;
            }
        case 6U:
            switch (text[0]) {
                case 'd': return same_text_n(text, "double", length);
                case 'e': return same_text_n(text, "extern", length);
                case 'i': return same_text_n(text, "inline", length);
                case 'r': return same_text_n(text, "return", length);
                case 's':
                    return same_text_n(text, "signed", length) ||
                           same_text_n(text, "sizeof", length) ||
                           same_text_n(text, "static", length) ||
                           same_text_n(text, "struct", length) ||
                           same_text_n(text, "switch", length);
                default: return 0;
            }
        case 7U:
            return (text[0] == 'd' && same_text_n(text, "default", length)) ||
                   (text[0] == 't' && same_text_n(text, "typedef", length));
        case 8U:
            switch (text[0]) {
                case 'c': return same_text_n(text, "continue", length);
                case 'r':
                    return same_text_n(text, "register", length) ||
                           same_text_n(text, "restrict", length);
                case 'u': return same_text_n(text, "unsigned", length);
                case 'v': return same_text_n(text, "volatile", length);
                default: return 0;
            }
        default:
            return 0;
    }
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
    switch (text[0]) {
        case '{':
        case '}':
        case '[':
        case ']':
        case '(':
        case ')':
        case ';':
        case ':':
        case ',':
        case '?':
        case '~':
            return 1U;
        case '#':
            return text[1] == '#' ? 2U : 1U;
        case '.':
            return (text[1] == '.' && text[2] == '.') ? 3U : 1U;
        case '+':
            if (text[1] == '+' || text[1] == '=') {
                return 2U;
            }
            return 1U;
        case '-':
            if (text[1] == '>' || text[1] == '-' || text[1] == '=') {
                return 2U;
            }
            return 1U;
        case '*':
        case '/':
        case '%':
        case '^':
            return text[1] == '=' ? 2U : 1U;
        case '&':
        case '|':
            if (text[1] == text[0] || text[1] == '=') {
                return 2U;
            }
            return 1U;
        case '!':
        case '=':
            return text[1] == '=' ? 2U : 1U;
        case '<':
            if (text[1] == '<') {
                return text[2] == '=' ? 3U : 2U;
            }
            return text[1] == '=' ? 2U : 1U;
        case '>':
            if (text[1] == '>') {
                return text[2] == '=' ? 3U : 2U;
            }
            return text[1] == '=' ? 2U : 1U;
        default:
            return 0U;
    }
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
