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
    size_t start = 0U;
    size_t pos = 0U;
    char quote = '\0';
    int changed_any = 0;

    for (;;) {
        char ch = script[pos];
        if (quote != '\0') {
            if (ch == '\0') {
                return -1;
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

            if (length >= sizeof(sql_statement_buffer)) {
                return -1;
            }
            memcpy(sql_statement_buffer, script + start, length);
            sql_statement_buffer[length] = '\0';
            sql_trim_whitespace(sql_statement_buffer);
            if (sql_statement_buffer[0] != '\0') {
                changed = sql_execute_statement(db, sql_statement_buffer);
                if (changed < 0) {
                    return -1;
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
    return 0;
}

static int sql_append_arg(char *buffer, size_t buffer_size, const char *text, int add_space) {
    size_t used = rt_strlen(buffer);
    size_t len = rt_strlen(text);

    if (used + (size_t)add_space + len + 1U > buffer_size) {
        return -1;
    }
    if (add_space) {
        buffer[used++] = ' ';
    }
    memcpy(buffer + used, text, len + 1U);
    return 0;
}

static int sql_read_stdin(char *buffer, size_t buffer_size) {
    size_t used = 0U;

    while (used + 1U < buffer_size) {
        long bytes = platform_read(0, buffer + used, buffer_size - used - 1U);
        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            break;
        }
        used += (size_t)bytes;
    }
    if (used + 1U >= buffer_size) {
        return -1;
    }
    buffer[used] = '\0';
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
    const char *path;
    int arg_index;
    int changed;

    if (argc < 2 || (argc == 2 && (rt_strcmp(argv[1], "-h") == 0 || rt_strcmp(argv[1], "--help") == 0))) {
        sql_usage(program_name);
        return argc < 2 ? 1 : 0;
    }

    path = argv[1];
    sql_input_buffer[0] = '\0';
    if (argc > 2) {
        for (arg_index = 2; arg_index < argc; ++arg_index) {
            if (sql_append_arg(sql_input_buffer, sizeof(sql_input_buffer), argv[arg_index], arg_index > 2) != 0) {
                sql_write_error("statement too large", 0);
                return 1;
            }
        }
    } else if (sql_read_stdin(sql_input_buffer, sizeof(sql_input_buffer)) != 0) {
        sql_write_error("unable to read statement", 0);
        return 1;
    }
    sql_trim_whitespace(sql_input_buffer);
    if (sql_input_buffer[0] == '\0') {
        sql_write_error("empty statement", 0);
        return 1;
    }

    if (sql_load_database(path, &sql_database) != 0) {
        sql_write_error("unable to load database: ", path);
        return 1;
    }
    if (sql_execute_script(&sql_database, sql_input_buffer, &changed) != 0) {
        sql_write_error("invalid or unsupported SQL statement", 0);
        return 1;
    }
    if (changed && sql_save_database(path, &sql_database) != 0) {
        sql_write_error("unable to save database: ", path);
        return 1;
    }
    return 0;
}