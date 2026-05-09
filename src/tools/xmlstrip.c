#include "runtime.h"
#include "tool_util.h"
#include "xml.h"

typedef struct {
    int comments;
    int pi;
    int doctype;
} XmlStripOptions;

static int should_strip(const XmlStripOptions *options, XmlTokenType type) {
    return (options->comments && type == XML_TOKEN_COMMENT) ||
           (options->pi && type == XML_TOKEN_PI) ||
           (options->doctype && type == XML_TOKEN_DOCTYPE);
}

static int strip_one(const char *path, const XmlStripOptions *options) {
    XmlParser parser;
    XmlToken token;
    char *input_buffer;
    size_t length;
    int result;

    if (xml_read_document(path, &input_buffer, &length, "xmlstrip") != 0) {
        return 1;
    }
    xml_parser_init(&parser, input_buffer, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (!should_strip(options, token.type)) {
            xml_write_raw(1, token.raw, token.raw_length);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlstrip", path, &parser);
        xml_free_document(input_buffer);
        return 1;
    }
    xml_free_document(input_buffer);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    XmlStripOptions options;
    int option_result;
    int exit_code = 0;
    int i;

    options.comments = 0;
    options.pi = 0;
    options.doctype = 0;

    tool_opt_init(&opt, argc, argv, "xmlstrip", "[--comments] [--pi] [--doctype] [--all] [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--comments") == 0) {
            options.comments = 1;
        } else if (rt_strcmp(opt.flag, "--pi") == 0) {
            options.pi = 1;
        } else if (rt_strcmp(opt.flag, "--doctype") == 0) {
            options.doctype = 1;
        } else if (rt_strcmp(opt.flag, "--all") == 0) {
            options.comments = 1;
            options.pi = 1;
            options.doctype = 1;
        } else {
            tool_write_error("xmlstrip", "unknown option: ", opt.flag);
            tool_write_usage("xmlstrip", "[--comments] [--pi] [--doctype] [--all] [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlstrip", "[--comments] [--pi] [--doctype] [--all] [FILE ...]");
        return 0;
    }
    if (!options.comments && !options.pi && !options.doctype) {
        tool_write_error("xmlstrip", "choose what to strip", 0);
        tool_write_usage("xmlstrip", "[--comments] [--pi] [--doctype] [--all] [FILE ...]");
        return 1;
    }
    if (opt.argi >= argc) {
        return strip_one(0, &options);
    }
    for (i = opt.argi; i < argc; ++i) {
        if (strip_one(argv[i], &options) != 0) {
            exit_code = 1;
        }
    }
    return exit_code;
}
