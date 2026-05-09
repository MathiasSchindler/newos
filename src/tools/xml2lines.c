#include "runtime.h"
#include "tool_util.h"
#include "xml.h"

static void write_assignment_prefix(const XmlNameStack *stack) {
    xml_write_name_stack_path(1, stack);
    rt_write_cstr(1, " = ");
}

static void write_line_value(const char *text, size_t length) {
    size_t i;
    for (i = 0U; i < length; ++i) {
        if (text[i] == '\\') rt_write_cstr(1, "\\\\");
        else if (text[i] == '\n') rt_write_cstr(1, "\\n");
        else if (text[i] == '\r') rt_write_cstr(1, "\\r");
        else if (text[i] == '\t') rt_write_cstr(1, "\\t");
        else if (text[i] == '=') rt_write_cstr(1, "\\=");
        else rt_write_char(1, text[i]);
    }
}

static void write_attr_line(const XmlNameStack *stack, const XmlAttribute *attr) {
    xml_write_name_stack_path(1, stack);
    rt_write_cstr(1, "/@");
    xml_write_raw(1, attr->name.start, attr->name.length);
    rt_write_cstr(1, " = ");
    write_line_value(attr->value, attr->value_length);
    rt_write_char(1, '\n');
}

static int lines_one(const char *path) {
    XmlParser parser;
    XmlToken token;
    XmlNameStack stack;
    char *input_buffer;
    size_t length;
    int result;

    xml_name_stack_init(&stack);

    if (xml_read_document(path, &input_buffer, &length, "xml2lines") != 0) {
        xml_name_stack_free(&stack);
        return 1;
    }
    xml_parser_init(&parser, input_buffer, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) {
            size_t i;
            TOOL_XML_NAME_STACK_PUSH_OR_RETURN(&stack, token.name, "xml2lines", xml_free_document(input_buffer); xml_name_stack_free(&stack);)
            for (i = 0U; i < token.attribute_count; ++i) {
                write_attr_line(&stack, &token.attributes[i]);
            }
            if (token.type == XML_TOKEN_EMPTY) {
                write_assignment_prefix(&stack);
                rt_write_char(1, '\n');
                xml_name_stack_pop(&stack);
            }
        } else if (token.type == XML_TOKEN_TEXT || token.type == XML_TOKEN_CDATA) {
            if (!token.text_is_blank) {
                write_assignment_prefix(&stack);
                write_line_value(token.text, token.text_length);
                rt_write_char(1, '\n');
            }
        } else if (token.type == XML_TOKEN_END) {
            xml_name_stack_pop(&stack);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xml2lines", path, &parser);
        xml_free_document(input_buffer);
        xml_name_stack_free(&stack);
        return 1;
    }
    xml_free_document(input_buffer);
    xml_name_stack_free(&stack);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    int option_result;
    int exit_code = 0;
    int i;

    tool_opt_init(&opt, argc, argv, "xml2lines", "[FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        tool_write_error("xml2lines", "unknown option: ", opt.flag);
        tool_write_usage("xml2lines", "[FILE ...]");
        return 1;
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xml2lines", "[FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) {
        return lines_one(0);
    }
    for (i = opt.argi; i < argc; ++i) {
        if (lines_one(argv[i]) != 0) {
            exit_code = 1;
        }
    }
    return exit_code;
}
