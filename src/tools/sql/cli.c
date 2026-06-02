#include "internal.h"

static int sql_execute_statement(SqlDatabase *db, const char *statement) {
    SqlParser parser;

    sql_parser_init(&parser, statement);
    if (sql_next_token(&parser) != 0 || parser.token_type != SQL_TOKEN_WORD) {
        return -1;
    }
    if (sql_equal_ignore_case(parser.token, "create")) {
        return sql_execute_create(db, &parser);
    }
    if (sql_equal_ignore_case(parser.token, "insert")) {
        return sql_execute_insert(db, &parser);
    }
    if (sql_equal_ignore_case(parser.token, "select")) {
        return sql_execute_select(db, &parser);
    }
    if (sql_equal_ignore_case(parser.token, "update")) {
        return sql_execute_update(db, &parser);
    }
    if (sql_equal_ignore_case(parser.token, "delete")) {
        return sql_execute_delete(db, &parser);
    }
    if (sql_equal_ignore_case(parser.token, "drop")) {
        return sql_execute_drop(db, &parser);
    }
    if (sql_equal_ignore_case(parser.token, "schema")) {
        return sql_execute_schema(db, &parser);
    }
    if (sql_equal_ignore_case(parser.token, "alter")) {
        return sql_execute_alter(db, &parser);
    }
    if (sql_equal_ignore_case(parser.token, "import")) {
        return sql_execute_import(db, &parser);
    }
    if (sql_equal_ignore_case(parser.token, "export")) {
        return sql_execute_export(db, &parser);
    }
    return -1;
}

static int sql_execute_script(SqlDatabase *db, const char *script, int *changed_out) {
    SqlTextBuffer statement;
    size_t start = 0U;
    size_t pos = 0U;
    char quote = '\0';
    int changed_any = 0;
    int result = -1;

    rt_memset(&statement, 0, sizeof(statement));

    for (;;) {
        char ch = script[pos];
        if (quote != '\0') {
            if (ch == '\0') {
                goto out;
            }
            if (ch == '\\' && script[pos + 1U] != '\0') {
                pos += 2U;
                continue;
            }
            if (ch == quote) {
                quote = '\0';
            }
            pos += 1U;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            pos += 1U;
            continue;
        }
        if (ch == ';' || ch == '\0') {
            size_t length = pos - start;
            int changed;

            if (length + 1U > SQL_MAX_STATEMENT_BYTES || sql_ensure_text_capacity(&statement, (unsigned int)(length + 1U), SQL_MAX_STATEMENT_BYTES) != 0) {
                goto out;
            }
            memcpy(statement.data, script + start, length);
            statement.data[length] = '\0';
            sql_trim_whitespace(statement.data);
            if (statement.data[0] != '\0') {
                changed = sql_execute_statement(db, statement.data);
                if (changed < 0) {
                    goto out;
                }
                changed_any = changed_any || changed;
            }
            if (ch == '\0') {
                break;
            }
            start = pos + 1U;
        }
        pos += 1U;
    }
    *changed_out = changed_any;
    result = 0;

out:
    sql_free_text_buffer(&statement);
    return result;
}

static int sql_append_arg(SqlTextBuffer *buffer, unsigned int *used_io, const char *text, int add_space) {
    unsigned int used;
    size_t len = rt_strlen(text);

    if (buffer == 0 || used_io == 0 || len > (size_t)SQL_MAX_STATEMENT_BYTES) {
        return -1;
    }
    used = *used_io;
    if ((size_t)used + (size_t)add_space + len + 1U > (size_t)SQL_MAX_STATEMENT_BYTES ||
        sql_ensure_text_capacity(buffer, (unsigned int)((size_t)used + (size_t)add_space + len + 1U), SQL_MAX_STATEMENT_BYTES) != 0) {
        return -1;
    }
    if (add_space) {
        buffer->data[used++] = ' ';
    }
    memcpy(buffer->data + used, text, len + 1U);
    *used_io = (unsigned int)((size_t)used + len);
    return 0;
}

static int sql_read_stdin(SqlTextBuffer *buffer, unsigned int *used_out) {
    unsigned int used = 0U;

    if (buffer == 0 || used_out == 0) {
        return -1;
    }
    for (;;) {
        long bytes;
        if (used + 1U >= buffer->capacity && sql_ensure_text_capacity(buffer, used + SQL_INITIAL_TEXT_CAPACITY + 1U, SQL_MAX_STATEMENT_BYTES) != 0) {
            return -1;
        }
        bytes = platform_read(0, buffer->data + used, buffer->capacity - used - 1U);
        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            break;
        }
        used += (unsigned int)bytes;
    }
    buffer->data[used] = '\0';
    *used_out = used;
    return 0;
}

static void sql_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " DBFILE [SQL]");
    rt_write_line(2, "SQL: CREATE TABLE, INSERT INTO, SELECT, UPDATE, DELETE FROM, DROP TABLE, IMPORT, EXPORT, SCHEMA");
}

int main(int argc, char **argv) {
    const char *program_name = argc > 0 ? argv[0] : "sql";
    SqlTextBuffer input;
    const char *path;
    int arg_index;
    int changed;
    int exit_code = 1;
    unsigned int input_used = 0U;

    if (argc < 2 || (argc == 2 && (rt_strcmp(argv[1], "-h") == 0 || rt_strcmp(argv[1], "--help") == 0))) {
        sql_usage(program_name);
        return argc < 2 ? 1 : 0;
    }

    rt_memset(&input, 0, sizeof(input));
    path = argv[1];
    if (argc > 2) {
        for (arg_index = 2; arg_index < argc; ++arg_index) {
            if (sql_append_arg(&input, &input_used, argv[arg_index], arg_index > 2) != 0) {
                sql_write_error("statement too large", 0);
                goto out;
            }
        }
    } else if (sql_read_stdin(&input, &input_used) != 0) {
        sql_write_error("unable to read statement", 0);
        goto out;
    }
    if (input.data == 0) {
        sql_write_error("empty statement", 0);
        goto out;
    }
    sql_trim_whitespace(input.data);
    if (input.data[0] == '\0') {
        sql_write_error("empty statement", 0);
        goto out;
    }

    if (sql_load_database(path, &sql_database) != 0) {
        sql_write_error("unable to load database: ", path);
        goto out;
    }
    if (sql_execute_script(&sql_database, input.data, &changed) != 0) {
        sql_write_error("invalid or unsupported SQL statement", 0);
        goto out;
    }
    if (changed && sql_save_database(path, &sql_database) != 0) {
        sql_write_error("unable to save database: ", path);
        goto out;
    }
    exit_code = 0;

out:
    sql_free_text_buffer(&input);
    return exit_code;
}