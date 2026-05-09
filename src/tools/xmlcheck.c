#include "runtime.h"
#include "tool_util.h"
#include "xml.h"

static int check_one(const char *path, int stream) {
    XmlParser parser;
    XmlToken token;
    char *input_buffer;
    size_t length;
    int result;

    if (stream) return xml_stream_validate_document(path, "xmlcheck");
    if (xml_read_document(path, &input_buffer, &length, "xmlcheck") != 0) {
        return 1;
    }
    xml_parser_init(&parser, input_buffer, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlcheck", path, &parser);
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
    int stream = 0;
    int i;

    tool_opt_init(&opt, argc, argv, "xmlcheck", "[--stream] [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--stream") == 0) {
            stream = 1;
        } else {
            tool_write_error("xmlcheck", "unknown option: ", opt.flag);
            tool_write_usage("xmlcheck", "[--stream] [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlcheck", "[--stream] [FILE ...]");
        return 0;
    }

    if (opt.argi >= argc) {
        return check_one(0, stream);
    }
    for (i = opt.argi; i < argc; ++i) {
        if (check_one(argv[i], stream) != 0) {
            exit_code = 1;
        }
    }
    return exit_code;
}
