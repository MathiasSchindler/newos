#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int source;
    unsigned long long field_no;
} JoinOutputField;

typedef struct {
    unsigned long long left_field;
    unsigned long long right_field;
    char delimiter;
    int ignore_case;
    int print_unpaired_left;
    int print_unpaired_right;
    int only_unpaired_left;
    int only_unpaired_right;
    int use_output_list;
    const char *empty_replacement;
    ToolArray output_fields;
} JoinOptions;

typedef struct {
    char *line;
    char *key;
} JoinRecord;

static ToolOutputBuffer join_output;

static int collect_lines_from_fd(int fd, ToolArray *records) {
    ToolRecordReader reader;
    ToolByteBuffer line;
    int result = 0;

    tool_record_reader_init(&reader, fd, '\n');
    tool_byte_buffer_init(&line);
    for (;;) {
        int has_line = 0;
        JoinRecord *record;
        if (tool_record_reader_next_buffer(&reader, &line, &has_line) != 0) {
            result = -1;
            break;
        }
        if (!has_line) break;
        record = (JoinRecord *)tool_array_append(records);
        if (record == 0) {
            result = -1;
            break;
        }
        record->line = (char *)rt_malloc(line.size + 1U);
        if (record->line == 0) {
            records->count -= 1U;
            result = -1;
            break;
        }
        memcpy(record->line, line.data, line.size + 1U);
    }
    tool_byte_buffer_free(&line);
    return result;
}

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-i] [-a1|-a 1] [-a2|-a 2] [-v1|-v 1] [-v2|-v 2] [-1 FIELD] [-2 FIELD] [-j FIELD] [-t CHAR] [-e EMPTY] [-o LIST] file1 file2");
}

static size_t join_decode_codepoint(const char *text, size_t length, size_t start, unsigned int *codepoint_out) {
    size_t index = start;

    if (start >= length) {
        if (codepoint_out != 0) {
            *codepoint_out = 0U;
        }
        return 0U;
    }

    if (rt_utf8_decode(text, length, &index, codepoint_out) != 0 || index <= start) {
        if (codepoint_out != 0) {
            *codepoint_out = (unsigned char)text[start];
        }
        return 1U;
    }

    return index - start;
}


static int compare_keys(const char *left, const char *right, int ignore_case) {
    size_t left_index = 0U;
    size_t right_index = 0U;
    size_t left_length = rt_strlen(left);
    size_t right_length = rt_strlen(right);

    while (left_index < left_length || right_index < right_length) {
        unsigned int lhs = 0U;
        unsigned int rhs = 0U;
        size_t left_advance;
        size_t right_advance;

        if (left_index >= left_length) {
            return -1;
        }
        if (right_index >= right_length) {
            return 1;
        }

        left_advance = join_decode_codepoint(left, left_length, left_index, &lhs);
        right_advance = join_decode_codepoint(right, right_length, right_index, &rhs);

        if (ignore_case) {
            lhs = rt_unicode_simple_fold(lhs);
            rhs = rt_unicode_simple_fold(rhs);
        }

        if (lhs != rhs) {
            return lhs < rhs ? -1 : 1;
        }

        left_index += left_advance;
        right_index += right_advance;
    }

    return 0;
}

static int parse_delimiter(const char *text, char *delimiter_out) {
    if (text[0] == '\0') {
        return -1;
    }
    if (text[0] == '\\' && text[1] == 't' && text[2] == '\0') {
        *delimiter_out = '\t';
        return 0;
    }
    if (text[1] != '\0') {
        return -1;
    }
    *delimiter_out = text[0];
    return 0;
}

static int parse_join_source(const char *text, int *source_out) {
    if (rt_strcmp(text, "1") == 0) {
        *source_out = 1;
        return 0;
    }
    if (rt_strcmp(text, "2") == 0) {
        *source_out = 2;
        return 0;
    }

    return -1;
}

static int extract_field(const char *line, unsigned long long field_no, char delimiter, char *out, size_t out_size) {
    size_t i = 0;
    size_t line_length = rt_strlen(line);
    unsigned long long current_field = 1ULL;
    size_t out_len = 0;

    if (delimiter == '\0') {
        while (line[i] != '\0') {
            size_t advance = 0U;

            while (line[i] != '\0' && tool_unicode_space_at(line, line_length, i, &advance)) {
                i += advance;
            }

            if (line[i] == '\0') {
                break;
            }

            if (current_field == field_no) {
                while (line[i] != '\0' && !tool_unicode_space_at(line, line_length, i, &advance)) {
                    if (out_len + advance < out_size) {
                        size_t j;
                        for (j = 0; j < advance; ++j) {
                            out[out_len++] = line[i + j];
                        }
                    }
                    i += advance;
                }
                out[out_len] = '\0';
                return 0;
            }

            while (line[i] != '\0' && !tool_unicode_space_at(line, line_length, i, &advance)) {
                i += advance;
            }
            current_field += 1ULL;
        }
    } else {
        size_t start = 0;

        while (1) {
            if (current_field == field_no) {
                while (line[start] != '\0' && line[start] != delimiter) {
                    if (out_len + 1U < out_size) {
                        out[out_len++] = line[start];
                    }
                    start += 1;
                }
                out[out_len] = '\0';
                return 0;
            }

            while (line[start] != '\0' && line[start] != delimiter) {
                start += 1;
            }

            if (line[start] == '\0') {
                break;
            }

            start += 1;
            current_field += 1ULL;
        }
    }

    out[0] = '\0';
    return -1;
}

static int write_separator(char delimiter, int *first_out) {
    if (*first_out) {
        *first_out = 0;
        return 0;
    }

    return tool_output_buffer_write_char(&join_output, delimiter == '\0' ? ' ' : delimiter);
}

static int emit_text_field(const char *text, size_t len, char delimiter, int *first_out) {
    if (write_separator(delimiter, first_out) != 0) {
        return -1;
    }

    return tool_output_buffer_write(&join_output, text, len);
}

static int emit_fields_except(const char *line, unsigned long long skip_field, char delimiter, int *first_out) {
    size_t i = 0;
    size_t line_length;
    unsigned long long field_no = 1ULL;

    if (line == 0) {
        return 0;
    }
    line_length = rt_strlen(line);

    if (delimiter == '\0') {
        while (line[i] != '\0') {
            size_t start;
            size_t len = 0;
            size_t advance = 0U;

            while (line[i] != '\0' && tool_unicode_space_at(line, line_length, i, &advance)) {
                i += advance;
            }

            if (line[i] == '\0') {
                break;
            }

            start = i;
            while (line[i] != '\0' && !tool_unicode_space_at(line, line_length, i, &advance)) {
                i += advance;
                len += advance;
            }

            if (field_no != skip_field) {
                if (emit_text_field(line + start, len, delimiter, first_out) != 0) {
                    return -1;
                }
            }

            field_no += 1ULL;
        }
    } else {
        size_t start = 0;

        while (1) {
            size_t len = 0;
            while (line[start + len] != '\0' && line[start + len] != delimiter) {
                len += 1;
            }

            if (field_no != skip_field) {
                if (emit_text_field(line + start, len, delimiter, first_out) != 0) {
                    return -1;
                }
            }

            if (line[start + len] == '\0') {
                break;
            }

            start += len + 1U;
            field_no += 1ULL;
        }
    }

    return 0;
}

static int parse_output_list(const char *text, JoinOptions *options) {
    size_t index = 0;

    options->output_fields.count = 0U;
    while (text[index] != '\0') {
        char token[32];
        size_t token_len = 0;
        JoinOutputField field;

        while (text[index] == ',' || rt_is_space(text[index])) {
            index += 1;
        }

        if (text[index] == '\0') {
            break;
        }

        while (text[index] != '\0' && text[index] != ',' && !rt_is_space(text[index])) {
            if (token_len + 1U >= sizeof(token)) {
                return -1;
            }
            token[token_len++] = text[index++];
        }
        token[token_len] = '\0';

        if (rt_strcmp(token, "0") == 0) {
            field.source = 0;
            field.field_no = 0ULL;
        } else if ((token[0] == '1' || token[0] == '2') && token[1] == '.') {
            field.source = token[0] - '0';
            if (rt_parse_uint(token + 2, &field.field_no) != 0 || field.field_no == 0ULL) {
                return -1;
            }
        } else {
            return -1;
        }

        {
            JoinOutputField *stored = (JoinOutputField *)tool_array_append(&options->output_fields);
            if (stored == 0) return -1;
            *stored = field;
        }
    }

    options->use_output_list = options->output_fields.count > 0U ? 1 : 0;
    return options->use_output_list ? 0 : -1;
}

static int emit_optional_value(const char *text, int found, const JoinOptions *options, int *first_out) {
    if (emit_text_field(found ? text : options->empty_replacement, found ? rt_strlen(text) : rt_strlen(options->empty_replacement), options->delimiter, first_out) != 0) {
        return -1;
    }
    return 0;
}

static int emit_selected_line(const char *left, const char *right, const JoinOptions *options) {
    int first = 1;
    size_t i;
    size_t value_size = 1U;
    char *value;

    if (left != 0 && rt_strlen(left) + 1U > value_size) value_size = rt_strlen(left) + 1U;
    if (right != 0 && rt_strlen(right) + 1U > value_size) value_size = rt_strlen(right) + 1U;
    value = (char *)rt_malloc(value_size);
    if (value == 0) return -1;

    for (i = 0; i < options->output_fields.count; ++i) {
        const JoinOutputField *field = (const JoinOutputField *)tool_array_get_const(&options->output_fields, i);
        int found = 0;

        if (field->source == 0) {
            if (left != 0 && extract_field(left, options->left_field, options->delimiter, value, value_size) == 0) {
                found = 1;
            } else if (right != 0 && extract_field(right, options->right_field, options->delimiter, value, value_size) == 0) {
                found = 1;
            }
        } else if (field->source == 1) {
            if (left != 0 && extract_field(left, field->field_no, options->delimiter, value, value_size) == 0) {
                found = 1;
            }
        } else if (field->source == 2) {
            if (right != 0 && extract_field(right, field->field_no, options->delimiter, value, value_size) == 0) {
                found = 1;
            }
        }

        if (emit_optional_value(value, found, options, &first) != 0) {
            rt_free(value);
            return -1;
        }
    }

    rt_free(value);
    return tool_output_buffer_write_char(&join_output, '\n');
}

static int emit_default_line(const char *left, const char *right, const JoinOptions *options) {
    size_t key_size = 1U;
    char *key;
    int first = 1;
    int found = 0;

    if (left != 0 && rt_strlen(left) + 1U > key_size) key_size = rt_strlen(left) + 1U;
    if (right != 0 && rt_strlen(right) + 1U > key_size) key_size = rt_strlen(right) + 1U;
    key = (char *)rt_malloc(key_size);
    if (key == 0) return -1;

    if (left != 0 && extract_field(left, options->left_field, options->delimiter, key, key_size) == 0) {
        found = 1;
    } else if (right != 0 && extract_field(right, options->right_field, options->delimiter, key, key_size) == 0) {
        found = 1;
    } else {
        key[0] = '\0';
    }

    if (emit_optional_value(key, found, options, &first) != 0 ||
        emit_fields_except(left, options->left_field, options->delimiter, &first) != 0 ||
        emit_fields_except(right, options->right_field, options->delimiter, &first) != 0 ||
        tool_output_buffer_write_char(&join_output, '\n') != 0) {
        rt_free(key);
        return -1;
    }

    rt_free(key);
    return 0;
}

static int emit_output_line(const char *left, const char *right, const JoinOptions *options) {
    if (options->use_output_list) {
        return emit_selected_line(left, right, options);
    }

    return emit_default_line(left, right, options);
}

static JoinRecord *join_record(ToolArray *records, size_t index) {
    return (JoinRecord *)tool_array_get(records, index);
}

static void free_records(ToolArray *records) {
    size_t i;
    for (i = 0U; i < records->count; ++i) {
        JoinRecord *record = join_record(records, i);
        rt_free(record->line);
        rt_free(record->key);
    }
    tool_array_free(records);
}

static int build_keys(ToolArray *records, unsigned long long field, char delimiter) {
    size_t i;
    for (i = 0U; i < records->count; ++i) {
        JoinRecord *record = join_record(records, i);
        size_t size = rt_strlen(record->line) + 1U;
        record->key = (char *)rt_malloc(size);
        if (record->key == 0) return -1;
        if (extract_field(record->line, field, delimiter, record->key, size) != 0) record->key[0] = '\0';
    }
    return 0;
}

static int join_files(const char *left_path, const char *right_path, const JoinOptions *options) {
    ToolArray left_records;
    ToolArray right_records;
    int left_fd;
    int left_close;
    int right_fd;
    int right_close;
    size_t left_index = 0U;
    size_t right_index = 0U;
    int result = -1;

    tool_output_buffer_init(&join_output, 1);
    tool_array_init(&left_records, sizeof(JoinRecord));
    tool_array_init(&right_records, sizeof(JoinRecord));

    if (tool_open_input(left_path, &left_fd, &left_close) != 0) {
        tool_write_error("join", "cannot open ", left_path);
        return -1;
    }

    if (collect_lines_from_fd(left_fd, &left_records) != 0) {
        tool_close_input(left_fd, left_close);
        tool_write_error("join", "read error on ", left_path);
        goto cleanup;
    }
    tool_close_input(left_fd, left_close);

    if (tool_open_input(right_path, &right_fd, &right_close) != 0) {
        tool_write_error("join", "cannot open ", right_path);
        goto cleanup;
    }

    if (collect_lines_from_fd(right_fd, &right_records) != 0) {
        tool_close_input(right_fd, right_close);
        tool_write_error("join", "read error on ", right_path);
        goto cleanup;
    }
    tool_close_input(right_fd, right_close);

    if (build_keys(&left_records, options->left_field, options->delimiter) != 0 ||
        build_keys(&right_records, options->right_field, options->delimiter) != 0) goto cleanup;

    while (left_index < left_records.count && right_index < right_records.count) {
        int comparison = compare_keys(join_record(&left_records, left_index)->key, join_record(&right_records, right_index)->key, options->ignore_case);

        if (comparison < 0) {
            if (options->print_unpaired_left && emit_output_line(join_record(&left_records, left_index)->line, 0, options) != 0) {
                goto cleanup;
            }
            left_index += 1U;
        } else if (comparison > 0) {
            if (options->print_unpaired_right && emit_output_line(0, join_record(&right_records, right_index)->line, options) != 0) {
                goto cleanup;
            }
            right_index += 1U;
        } else {
            size_t left_start = left_index;
            size_t right_start = right_index;
            size_t left_end;
            size_t right_end;

            while (left_index < left_records.count && compare_keys(join_record(&left_records, left_start)->key, join_record(&left_records, left_index)->key, options->ignore_case) == 0) {
                left_index += 1U;
            }
            while (right_index < right_records.count && compare_keys(join_record(&right_records, right_start)->key, join_record(&right_records, right_index)->key, options->ignore_case) == 0) {
                right_index += 1U;
            }
            left_end = left_index;
            right_end = right_index;

            if (!options->only_unpaired_left && !options->only_unpaired_right) {
                size_t left_group;
                for (left_group = left_start; left_group < left_end; ++left_group) {
                    size_t right_group;
                    for (right_group = right_start; right_group < right_end; ++right_group) {
                        if (emit_output_line(join_record(&left_records, left_group)->line, join_record(&right_records, right_group)->line, options) != 0) {
                            goto cleanup;
                        }
                    }
                }
            }
        }
    }

    while (left_index < left_records.count) {
        if (options->print_unpaired_left && emit_output_line(join_record(&left_records, left_index)->line, 0, options) != 0) {
            goto cleanup;
        }
        left_index += 1U;
    }

    while (right_index < right_records.count) {
        if (options->print_unpaired_right && emit_output_line(0, join_record(&right_records, right_index)->line, options) != 0) {
            goto cleanup;
        }
        right_index += 1U;
    }

    result = tool_output_buffer_flush(&join_output);
cleanup:
    free_records(&left_records);
    free_records(&right_records);
    return result;
}

int main(int argc, char **argv) {
    JoinOptions options;
    int argi = 1;

    options.left_field = 1ULL;
    options.right_field = 1ULL;
    options.delimiter = '\0';
    options.ignore_case = 0;
    options.print_unpaired_left = 0;
    options.print_unpaired_right = 0;
    options.only_unpaired_left = 0;
    options.only_unpaired_right = 0;
    options.use_output_list = 0;
    options.empty_replacement = "";
    tool_array_init(&options.output_fields, sizeof(JoinOutputField));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-i") == 0 || rt_strcmp(argv[argi], "--ignore-case") == 0) {
            options.ignore_case = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-a1") == 0) {
            options.print_unpaired_left = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-a2") == 0) {
            options.print_unpaired_right = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-a") == 0) {
            int source = 0;
            if (argi + 1 >= argc || parse_join_source(argv[argi + 1], &source) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            if (source == 1) {
                options.print_unpaired_left = 1;
            } else {
                options.print_unpaired_right = 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-v1") == 0) {
            options.print_unpaired_left = 1;
            options.only_unpaired_left = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-v2") == 0) {
            options.print_unpaired_right = 1;
            options.only_unpaired_right = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-v") == 0) {
            int source = 0;
            if (argi + 1 >= argc || parse_join_source(argv[argi + 1], &source) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            if (source == 1) {
                options.print_unpaired_left = 1;
                options.only_unpaired_left = 1;
            } else {
                options.print_unpaired_right = 1;
                options.only_unpaired_right = 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-1") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.left_field, "join", "field") != 0 || options.left_field == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-2") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &options.right_field, "join", "field") != 0 || options.right_field == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-j") == 0) {
            unsigned long long field = 0ULL;
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &field, "join", "field") != 0 || field == 0ULL) {
                print_usage(argv[0]);
                return 1;
            }
            options.left_field = field;
            options.right_field = field;
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-t") == 0) {
            if (argi + 1 >= argc || parse_delimiter(argv[argi + 1], &options.delimiter) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-e") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            options.empty_replacement = argv[argi + 1];
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-o") == 0) {
            if (argi + 1 >= argc || parse_output_list(argv[argi + 1], &options) != 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (argc - argi != 2) {
        print_usage(argv[0]);
        return 1;
    }

    {
        int result = join_files(argv[argi], argv[argi + 1], &options) == 0 ? 0 : 1;
        tool_array_free(&options.output_fields);
        return result;
    }
}
