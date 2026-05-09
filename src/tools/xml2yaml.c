#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


static void write_indent(unsigned int depth) {
    unsigned int i;
    for (i = 0U; i < depth; ++i) rt_write_cstr(1, "  ");
}

static void write_yaml_string(const char *text, size_t length) {
    size_t i;
    rt_write_char(1, '"');
    for (i = 0U; i < length; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '"') rt_write_cstr(1, "\\\"");
        else if (ch == '\\') rt_write_cstr(1, "\\\\");
        else if (ch == '\n') rt_write_cstr(1, "\\n");
        else if (ch == '\r') rt_write_cstr(1, "\\r");
        else if (ch == '\t') rt_write_cstr(1, "\\t");
        else if (ch < 0x20U || ch == 0x7fU) rt_write_cstr(1, " ");
        else rt_write_char(1, (char)ch);
    }
    rt_write_char(1, '"');
}

static void write_attrs(const XmlToken *token, unsigned int indent) {
    size_t i;
    if (token->attribute_count == 0U) {
        rt_write_cstr(1, "attributes: {}\n");
        return;
    }
    rt_write_cstr(1, "attributes:\n");
    for (i = 0U; i < token->attribute_count; ++i) {
        write_indent(indent + 1U);
        write_yaml_string(token->attributes[i].name.start, token->attributes[i].name.length);
        rt_write_cstr(1, ": ");
        write_yaml_string(token->attributes[i].value, token->attributes[i].value_length);
        rt_write_char(1, '\n');
    }
}

static void write_element(const XmlToken *token, unsigned int depth, int list_item) {
    unsigned int indent = list_item ? depth + 1U : depth;
    write_indent(depth);
    if (list_item) rt_write_cstr(1, "- ");
    rt_write_cstr(1, "name: ");
    write_yaml_string(token->name.start, token->name.length);
    rt_write_char(1, '\n');
    write_indent(indent);
    write_attrs(token, indent);
    write_indent(indent);
    rt_write_cstr(1, token->type == XML_TOKEN_EMPTY ? "children: []\n" : "children:\n");
}

static int yaml_one(const char *path) {
    XmlParser parser;
    XmlToken token;
    char *input;
    size_t length;
    int result;

    if (xml_read_document(path, &input, &length, "xml2yaml") != 0) return 1;
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) {
            write_element(&token, token.depth, token.depth > 0U);
        } else if ((token.type == XML_TOKEN_TEXT || token.type == XML_TOKEN_CDATA) && !token.text_is_blank) {
            write_indent(token.depth + 1U);
            rt_write_cstr(1, "- text: ");
            write_yaml_string(token.text, token.text_length);
            rt_write_char(1, '\n');
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xml2yaml", path, &parser);
        xml_free_document(input);
        return 1;
    }
    xml_free_document(input);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    int option_result;
    int exit_code = 0;
    int i;

    tool_opt_init(&opt, argc, argv, "xml2yaml", "[FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        tool_write_error("xml2yaml", "unknown option: ", opt.flag);
        tool_write_usage("xml2yaml", "[FILE ...]");
        return 1;
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xml2yaml", "[FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) return yaml_one(0);
    for (i = opt.argi; i < argc; ++i) if (yaml_one(argv[i]) != 0) exit_code = 1;
    return exit_code;
}