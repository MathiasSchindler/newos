#include "runtime.h"
#include "tool_util.h"
#include "xml.h"

static int format_one(const char *path, unsigned int indent_width) {
    XmlParser parser;
    XmlToken token;
    char *input_buffer;
    size_t length;
    int result;
    int previous_text = 0;
    int wrote_any = 0;

    if (xml_read_document(path, &input_buffer, &length, "xmlfmt") != 0) {
        return 1;
    }
    xml_parser_init(&parser, input_buffer, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_TEXT && token.text_is_blank) {
            continue;
        }
        if (token.type == XML_TOKEN_TEXT || token.type == XML_TOKEN_CDATA) {
            xml_write_raw(1, token.raw, token.raw_length);
            previous_text = 1;
            wrote_any = 1;
            continue;
        }
        if (wrote_any && !previous_text) {
            rt_write_char(1, '\n');
        }
        if (token.type == XML_TOKEN_END) {
            if (!previous_text) {
                xml_write_indent(1, token.depth - 1U, indent_width);
            }
        } else {
            xml_write_indent(1, token.depth, indent_width);
        }
        xml_write_raw(1, token.raw, token.raw_length);
        previous_text = token.type == XML_TOKEN_START ? 0 : 0;
        wrote_any = 1;
    }
    if (wrote_any) {
        rt_write_char(1, '\n');
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlfmt", path, &parser);
        xml_free_document(input_buffer);
        return 1;
    }
    xml_free_document(input_buffer);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    int option_result;
    unsigned int indent_width = 2U;
    int exit_code = 0;
    int i;

    tool_opt_init(&opt, argc, argv, "xmlfmt", "[-i WIDTH] [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-i") == 0 || rt_strcmp(opt.flag, "--indent") == 0) {
            unsigned long long parsed;
            if (tool_opt_require_value(&opt) != 0 || rt_parse_uint(opt.value, &parsed) != 0 || parsed > 16ULL) {
                tool_write_error("xmlfmt", "invalid indent width: ", opt.value);
                return 1;
            }
            indent_width = (unsigned int)parsed;
        } else {
            tool_write_error("xmlfmt", "unknown option: ", opt.flag);
            tool_write_usage("xmlfmt", "[-i WIDTH] [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlfmt", "[-i WIDTH] [FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) {
        return format_one(0, indent_width);
    }
    for (i = opt.argi; i < argc; ++i) {
        if (format_one(argv[i], indent_width) != 0) {
            exit_code = 1;
        }
    }
    return exit_code;
}
