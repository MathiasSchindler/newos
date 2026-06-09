#include "runtime.h"
#include "tool_util.h"
#include "xml.h"

static int minify_one(const char *path) {
    XmlParser parser;
    XmlToken token;
    ToolOutputBuffer output;
    char *input_buffer;
    size_t length;
    int result;

    if (xml_read_document(path, &input_buffer, &length, "xmlmin") != 0) {
        return 1;
    }
    xml_parser_init(&parser, input_buffer, length);
    tool_output_buffer_init(&output, 1);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_TEXT && token.text_is_blank) {
            continue;
        }
        if (tool_output_buffer_write(&output, token.raw, token.raw_length) != 0) {
            xml_free_document(input_buffer);
            return 1;
        }
    }
    if (tool_output_buffer_flush(&output) != 0) {
        xml_free_document(input_buffer);
        return 1;
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlmin", path, &parser);
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

    tool_opt_init(&opt, argc, argv, "xmlmin", "[FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        tool_write_error("xmlmin", "unknown option: ", opt.flag);
        tool_write_usage("xmlmin", "[FILE ...]");
        return 1;
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlmin", "[FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) {
        return minify_one(0);
    }
    for (i = opt.argi; i < argc; ++i) {
        if (minify_one(argv[i]) != 0) {
            exit_code = 1;
        }
    }
    return exit_code;
}
