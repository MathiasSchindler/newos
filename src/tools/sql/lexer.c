#include "internal.h"

static int sql_identifier_char(char ch) {
    return (ch >= 'A' && ch <= 'Z') ||
           (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_';
}

static int sql_valid_identifier(const char *text) {
    size_t i;

    if (text == 0 || text[0] == '\0') {
        return 0;
    }
    for (i = 0U; text[i] != '\0'; ++i) {
        if (!sql_identifier_char(text[i])) {
            return 0;
        }
    }
    return 1;
}

static void sql_parser_init(SqlParser *parser, const char *input) {
    parser->input = input;
    parser->pos = 0U;
    parser->token[0] = '\0';
    parser->token_type = SQL_TOKEN_END;
}

static void sql_skip_space(SqlParser *parser) {
    while (rt_is_space(parser->input[parser->pos])) {
        parser->pos += 1U;
    }
}

static int sql_next_token(SqlParser *parser) {
    size_t out = 0U;
    char ch;

    sql_skip_space(parser);
    ch = parser->input[parser->pos];
    parser->token[0] = '\0';

    if (ch == '\0') {
        parser->token_type = SQL_TOKEN_END;
        return 0;
    }
    if (ch == ';') {
        parser->pos += 1U;
        parser->token_type = SQL_TOKEN_END;
        return 0;
    }
    if (ch == ',' || ch == '(' || ch == ')' || ch == '=' || ch == '*' || ch == '<' || ch == '>' || ch == '!' || ch == '+' || ch == '|') {
        parser->token[0] = ch;
        parser->token[1] = '\0';
        parser->pos += 1U;
        parser->token_type = SQL_TOKEN_SYMBOL;
        return 0;
    }
    if (ch == '\'' || ch == '"') {
        char quote = ch;
        parser->pos += 1U;
        while (parser->input[parser->pos] != '\0' && parser->input[parser->pos] != quote) {
            ch = parser->input[parser->pos];
            if (ch == '\\' && parser->input[parser->pos + 1U] != '\0') {
                parser->pos += 1U;
                ch = parser->input[parser->pos];
            }
            if (out + 1U >= sizeof(parser->token) || ch == '\n' || ch == '\r') {
                return -1;
            }
            parser->token[out++] = ch;
            parser->pos += 1U;
        }
        if (parser->input[parser->pos] != quote) {
            return -1;
        }
        parser->pos += 1U;
        parser->token[out] = '\0';
        parser->token_type = SQL_TOKEN_STRING;
        return 0;
    }

    while (sql_identifier_char(parser->input[parser->pos]) || parser->input[parser->pos] == '-' || parser->input[parser->pos] == '.') {
        if (out + 1U >= sizeof(parser->token)) {
            return -1;
        }
        parser->token[out++] = parser->input[parser->pos++];
    }
    if (out == 0U) {
        return -1;
    }
    parser->token[out] = '\0';
    parser->token_type = SQL_TOKEN_WORD;
    return 0;
}

static int sql_expect_word(SqlParser *parser, const char *word) {
    return sql_next_token(parser) == 0 && parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, word);
}

static int sql_expect_symbol(SqlParser *parser, char symbol) {
    return sql_next_token(parser) == 0 && parser->token_type == SQL_TOKEN_SYMBOL && parser->token[0] == symbol && parser->token[1] == '\0';
}

static int sql_read_identifier(SqlParser *parser, char *buffer, size_t buffer_size) {
    if (sql_next_token(parser) != 0 || parser->token_type != SQL_TOKEN_WORD) {
        return -1;
    }
    if (!sql_valid_identifier(parser->token)) {
        return -1;
    }
    return sql_copy_checked(buffer, buffer_size, parser->token);
}

static int sql_read_value(SqlParser *parser, char *buffer, size_t buffer_size) {
    if (sql_next_token(parser) != 0 || (parser->token_type != SQL_TOKEN_WORD && parser->token_type != SQL_TOKEN_STRING)) {
        return -1;
    }
    return sql_copy_checked(buffer, buffer_size, parser->token);
}

static int sql_at_end(SqlParser *parser) {
    return sql_next_token(parser) == 0 && parser->token_type == SQL_TOKEN_END;
}

static int sql_try_word(SqlParser *parser, const char *word) {
    size_t saved = parser->pos;

    if (sql_next_token(parser) == 0 && parser->token_type == SQL_TOKEN_WORD && tool_str_equal_ignore_case_ascii(parser->token, word)) {
        return 1;
    }
    parser->pos = saved;
    return 0;
}

static int sql_try_symbol(SqlParser *parser, char symbol) {
    size_t saved = parser->pos;

    if (sql_next_token(parser) == 0 && parser->token_type == SQL_TOKEN_SYMBOL && parser->token[0] == symbol && parser->token[1] == '\0') {
        return 1;
    }
    parser->pos = saved;
    return 0;
}
