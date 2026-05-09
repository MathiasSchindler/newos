#include "runtime.h"
#include "tool_util.h"
#include "xml.h"

static void write_location(const XmlToken *token) {
    rt_write_uint(1, token->line);
    rt_write_char(1, ':');
    rt_write_uint(1, token->column);
}

static void write_token(const XmlToken *token) {
    size_t i;
    write_location(token);
    rt_write_char(1, ' ');
    rt_write_cstr(1, xml_token_type_name(token->type));
    rt_write_cstr(1, " depth=");
    rt_write_uint(1, token->depth);
    if (token->name.start != 0) {
        rt_write_cstr(1, " name=");
        xml_write_raw(1, token->name.start, token->name.length);
    }
    for (i = 0; i < token->attribute_count; ++i) {
        rt_write_cstr(1, " ");
        xml_write_raw(1, token->attributes[i].name.start, token->attributes[i].name.length);
        rt_write_cstr(1, "=\"");
        tool_write_visible(1, token->attributes[i].value, token->attributes[i].value_length);
        rt_write_char(1, '"');
    }
    if (token->text != 0 && token->text_length > 0U) {
        rt_write_cstr(1, " text=\"");
        tool_write_visible(1, token->text, token->text_length);
        rt_write_char(1, '"');
    }
    rt_write_char(1, '\n');
}

static int tokens_one(const char *path) {
    XmlParser parser;
    XmlToken token;
    char *input_buffer;
    size_t length;
    int result;

    if (xml_read_document(path, &input_buffer, &length, "xmltokens") != 0) {
        return 1;
    }
    xml_parser_init(&parser, input_buffer, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        write_token(&token);
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmltokens", path, &parser);
        xml_free_document(input_buffer);
        return 1;
    }
    xml_free_document(input_buffer);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    int option_result;
    int exit_code = 0;
    int i;

    tool_opt_init(&opt, argc, argv, "xmltokens", "[FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        tool_write_error("xmltokens", "unknown option: ", opt.flag);
        tool_write_usage("xmltokens", "[FILE ...]");
        return 1;
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmltokens", "[FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) {
        return tokens_one(0);
    }
    for (i = opt.argi; i < argc; ++i) {
        if (tokens_one(argv[i]) != 0) {
            exit_code = 1;
        }
    }
    return exit_code;
}
