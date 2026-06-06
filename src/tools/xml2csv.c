#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


#define XMLCSV_INITIAL_COLS 16

typedef struct {
    const char *row_selector;
    const char *inline_cols[XMLCSV_INITIAL_COLS];
    const char **cols;
    size_t col_count;
    size_t col_capacity;
    char separator;
    int header;
} XmlCsvOptions;

typedef struct {
    const char *text;
    size_t length;
} CsvValue;

static void csv_options_init(XmlCsvOptions *options) {
    rt_memset(options, 0, sizeof(*options));
    options->cols = options->inline_cols;
    options->col_capacity = XMLCSV_INITIAL_COLS;
    options->separator = ',';
}

static void csv_options_free(XmlCsvOptions *options) {
    if (options->cols != options->inline_cols) rt_free(options->cols);
}

static int csv_options_add_col(XmlCsvOptions *options, const char *col) {
    const char **cols;
    size_t next_capacity;
    size_t i;
    if (options->col_count >= options->col_capacity) {
        next_capacity = options->col_capacity == 0U ? XMLCSV_INITIAL_COLS : options->col_capacity;
        if (next_capacity > (size_t)(~(size_t)0 / 2U)) return -1;
        next_capacity *= 2U;
        cols = (const char **)rt_malloc_array(next_capacity, sizeof(cols[0]));
        if (cols == 0) return -1;
        for (i = 0U; i < options->col_count; ++i) cols[i] = options->cols[i];
        if (options->cols != options->inline_cols) rt_free(options->cols);
        options->cols = cols;
        options->col_capacity = next_capacity;
    }
    options->cols[options->col_count++] = col;
    return 0;
}

static void set_value(CsvValue *value, const char *text, size_t length) {
    value->text = text;
    value->length = length;
}

static void clear_values(CsvValue *values, size_t count) {
    size_t i;
    for (i = 0U; i < count; ++i) {
        values[i].text = "";
        values[i].length = 0U;
    }
}

static void write_csv_cell(const char *text, size_t length, char separator) {
    size_t i;
    int quote = 0;
    for (i = 0U; i < length; ++i) {
        if (text[i] == '"' || text[i] == separator || text[i] == '\n' || text[i] == '\r') quote = 1;
    }
    if (quote) rt_write_char(1, '"');
    for (i = 0U; i < length; ++i) {
        if (text[i] == '"') rt_write_cstr(1, "\"\"");
        else rt_write_char(1, text[i]);
    }
    if (quote) rt_write_char(1, '"');
}

static void write_row(const CsvValue *values, size_t count, char separator) {
    size_t i;
    for (i = 0U; i < count; ++i) {
        if (i > 0U) rt_write_char(1, separator);
        write_csv_cell(values[i].text, values[i].length, separator);
    }
    rt_write_char(1, '\n');
}

static int col_attr_name(const char *col) {
    return col[0] == '@' && col[1] != '\0';
}

static void write_header(const XmlCsvOptions *options) {
    size_t i;
    for (i = 0U; i < options->col_count; ++i) {
        const char *name = options->cols[i];
        if (i > 0U) rt_write_char(1, options->separator);
        if (col_attr_name(name)) name += 1U;
        write_csv_cell(name, rt_strlen(name), options->separator);
    }
    rt_write_char(1, '\n');
}

static int csv_one(const XmlCsvOptions *options, const char *path) {
    XmlParser parser;
    XmlToken token;
    XmlNameStack stack;
    XmlSelector row_selector;
    CsvValue *values;
    char *input;
    unsigned int row_depth = 0U;
    int in_row = 0;
    int active_col = -1;
    size_t length;
    int result;

    xml_name_stack_init(&stack);
    if (tool_xml_selector_compile(&row_selector, options->row_selector, "xml2csv") != 0) { xml_name_stack_free(&stack); return 1; }

    if (options->col_count > (size_t)(~(size_t)0 / sizeof(values[0]))) {
        xml_selector_free(&row_selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    values = (CsvValue *)rt_malloc_array(options->col_count, sizeof(values[0]));
    if (values == 0) {
        xml_selector_free(&row_selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    clear_values(values, options->col_count);

    if (xml_read_document(path, &input, &length, "xml2csv") != 0) { rt_free(values); xml_selector_free(&row_selector); xml_name_stack_free(&stack); return 1; }
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) {
            size_t i;
            if (tool_xml_name_stack_push(&stack, token.name, "xml2csv") != 0) {
                xml_free_document(input);
                rt_free(values);
                xml_selector_free(&row_selector);
                xml_name_stack_free(&stack);
                return 1;
            }
            if (!in_row && xml_name_stack_matches_token(&stack, &token, &row_selector)) {
                clear_values(values, options->col_count);
                in_row = 1;
                row_depth = stack.count;
                active_col = -1;
                for (i = 0U; i < token.attribute_count; ++i) {
                    size_t c;
                    for (c = 0U; c < options->col_count; ++c) {
                        if (col_attr_name(options->cols[c]) && xml_name_equals(&token.attributes[i].name, options->cols[c] + 1)) {
                            set_value(&values[c], token.attributes[i].value, token.attributes[i].value_length);
                        }
                    }
                }
            } else if (in_row && stack.count == row_depth + 1U) {
                for (i = 0U; i < options->col_count; ++i) {
                    if (!col_attr_name(options->cols[i]) && rt_strcmp(options->cols[i], ".") != 0 && xml_name_equals(&token.name, options->cols[i])) {
                        active_col = (int)i;
                    }
                }
            }
            if (token.type == XML_TOKEN_EMPTY) {
                if (in_row && stack.count == row_depth) {
                    write_row(values, options->col_count, options->separator);
                    in_row = 0;
                }
                xml_name_stack_pop(&stack);
            }
        } else if (token.type == XML_TOKEN_TEXT || token.type == XML_TOKEN_CDATA) {
            if (in_row && !token.text_is_blank) {
                size_t c;
                for (c = 0U; c < options->col_count; ++c) {
                    if (rt_strcmp(options->cols[c], ".") == 0 && stack.count == row_depth) {
                        set_value(&values[c], token.text, token.text_length);
                    }
                }
                if (active_col >= 0) set_value(&values[active_col], token.text, token.text_length);
            }
        } else if (token.type == XML_TOKEN_END) {
            if (in_row && stack.count == row_depth + 1U) active_col = -1;
            if (in_row && stack.count == row_depth) {
                write_row(values, options->col_count, options->separator);
                in_row = 0;
            }
            xml_name_stack_pop(&stack);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xml2csv", path, &parser);
        xml_free_document(input);
        rt_free(values);
        xml_selector_free(&row_selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    xml_free_document(input);
    rt_free(values);
    xml_selector_free(&row_selector);
    xml_name_stack_free(&stack);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    XmlCsvOptions options;
    int option_result;
    int exit_code = 0;
    int i;

    csv_options_init(&options);
    tool_opt_init(&opt, argc, argv, "xml2csv", "[--header] [--sep CHAR] --row SELECTOR --col NAME [--col NAME ...] [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--row") == 0) {
            if (tool_opt_require_value(&opt) != 0) { csv_options_free(&options); return 1; }
            options.row_selector = opt.value;
        } else if (rt_strcmp(opt.flag, "--col") == 0) {
            if (tool_opt_require_value(&opt) != 0) { csv_options_free(&options); return 1; }
            if (csv_options_add_col(&options, opt.value) != 0) {
                tool_write_error("xml2csv", "out of memory", 0);
                csv_options_free(&options);
                return 1;
            }
        } else if (rt_strcmp(opt.flag, "--header") == 0) {
            options.header = 1;
        } else if (rt_strcmp(opt.flag, "--sep") == 0) {
            char parsed[8];
            size_t parsed_length;
            if (tool_opt_require_value(&opt) != 0) { csv_options_free(&options); return 1; }
            if (tool_parse_escaped_string(opt.value, parsed, sizeof(parsed), &parsed_length) != 0 || parsed_length != 1U) {
                tool_write_error("xml2csv", "separator must be one character: ", opt.value);
                csv_options_free(&options);
                return 1;
            }
            options.separator = parsed[0];
        } else {
            tool_write_error("xml2csv", "unknown option: ", opt.flag);
            tool_write_usage("xml2csv", "[--header] [--sep CHAR] --row SELECTOR --col NAME [--col NAME ...] [FILE ...]");
            csv_options_free(&options);
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xml2csv", "[--header] [--sep CHAR] --row SELECTOR --col NAME [--col NAME ...] [FILE ...]");
        csv_options_free(&options);
        return 0;
    }
    if (options.row_selector == 0 || options.col_count == 0U) {
        tool_write_usage("xml2csv", "[--header] [--sep CHAR] --row SELECTOR --col NAME [--col NAME ...] [FILE ...]");
        csv_options_free(&options);
        return 1;
    }
    if (options.header) write_header(&options);
    if (opt.argi >= argc) {
        exit_code = csv_one(&options, 0);
        csv_options_free(&options);
        return exit_code;
    }
    for (i = opt.argi; i < argc; ++i) {
        if (csv_one(&options, argv[i]) != 0) exit_code = 1;
    }
    csv_options_free(&options);
    return exit_code;
}
