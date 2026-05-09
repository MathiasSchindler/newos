#include "runtime.h"
#include "tool_util.h"
#include "xml.h"
#include "xml_dtd.h"

typedef struct {
    int stream;
    int allow_doctype;
    int allow_pi;
    int allow_comments;
    unsigned int max_depth;
    const char *root_name;
    const char *dtd_path;
} ValidateOptions;

static int name_matches_text(const XmlName *name, const char *text) {
    return text == 0 || xml_name_equals(name, text);
}

static int validate_one(const ValidateOptions *options, const char *path) {
    XmlParser parser;
    XmlToken token;
    char *input;
    size_t length;
    int result;
    int saw_root = 0;

    if (options->stream && options->dtd_path == 0) {
        XmlStreamOptions stream_options;
        stream_options.allow_doctype = options->allow_doctype;
        stream_options.allow_pi = options->allow_pi;
        stream_options.allow_comments = options->allow_comments;
        stream_options.max_depth = options->max_depth;
        stream_options.root_name = options->root_name;
        return xml_stream_validate_document_with_options(path, "xmlvalidate", &stream_options);
    }
    if (xml_read_document(path, &input, &length, "xmlvalidate") != 0) return 1;
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if ((token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) && token.depth == 0U) {
            if (!name_matches_text(&token.name, options->root_name)) {
                tool_write_error("xmlvalidate", "unexpected root element: ", path == 0 ? "-" : path);
                xml_free_document(input);
                return 1;
            }
            saw_root = 1;
        }
        if ((token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) && options->max_depth > 0U && token.depth + 1U > options->max_depth) {
            tool_write_error("xmlvalidate", "maximum depth exceeded: ", path == 0 ? "-" : path);
            xml_free_document(input);
            return 1;
        }
        if (!options->allow_doctype && options->dtd_path == 0 && token.type == XML_TOKEN_DOCTYPE) {
            tool_write_error("xmlvalidate", "doctype is not allowed: ", path == 0 ? "-" : path);
            xml_free_document(input);
            return 1;
        }
        if (!options->allow_pi && token.type == XML_TOKEN_PI) {
            tool_write_error("xmlvalidate", "processing instruction is not allowed: ", path == 0 ? "-" : path);
            xml_free_document(input);
            return 1;
        }
        if (!options->allow_comments && token.type == XML_TOKEN_COMMENT) {
            tool_write_error("xmlvalidate", "comment is not allowed: ", path == 0 ? "-" : path);
            xml_free_document(input);
            return 1;
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlvalidate", path, &parser);
        xml_free_document(input);
        return 1;
    }
    if (options->root_name != 0 && !saw_root) {
        tool_write_error("xmlvalidate", "missing root element: ", path == 0 ? "-" : path);
        xml_free_document(input);
        return 1;
    }
    if (options->dtd_path != 0) {
        XmlDtd dtd;
        int dtd_result;
        xml_dtd_init(&dtd);
        dtd_result = xml_dtd_load(&dtd, options->dtd_path, input, length, "xmlvalidate");
        if (dtd_result == 0) dtd_result = xml_dtd_validate_document(&dtd, path, input, length, "xmlvalidate");
        xml_dtd_free(&dtd);
        if (dtd_result != 0) {
            xml_free_document(input);
            return 1;
        }
    }
    xml_free_document(input);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    ValidateOptions options;
    int option_result;
    int exit_code = 0;
    int i;

    rt_memset(&options, 0, sizeof(options));
    tool_opt_init(&opt, argc, argv, "xmlvalidate", "[--stream] [--dtd FILE|auto] [--allow-doctype] [--allow-pi] [--allow-comments] [--max-depth N] [--root NAME] [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--stream") == 0) options.stream = 1;
        else if (rt_strcmp(opt.flag, "--allow-doctype") == 0) options.allow_doctype = 1;
        else if (rt_strcmp(opt.flag, "--allow-pi") == 0) options.allow_pi = 1;
        else if (rt_strcmp(opt.flag, "--allow-comments") == 0) options.allow_comments = 1;
        else if (rt_strcmp(opt.flag, "--dtd") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            options.dtd_path = opt.value;
        }
        else if (rt_strcmp(opt.flag, "--max-depth") == 0) {
            unsigned long long value;
            if (tool_opt_require_value(&opt) != 0 || rt_parse_uint(opt.value, &value) != 0 || value == 0ULL || value > 4294967295ULL) {
                tool_write_error("xmlvalidate", "invalid max depth: ", opt.value);
                return 1;
            }
            options.max_depth = (unsigned int)value;
        } else if (rt_strcmp(opt.flag, "--root") == 0) {
            if (tool_opt_require_value(&opt) != 0) {
                tool_write_usage("xmlvalidate", "[--stream] [--dtd FILE|auto] [--allow-doctype] [--allow-pi] [--allow-comments] [--max-depth N] [--root NAME] [FILE ...]");
                return 1;
            }
            options.root_name = opt.value;
        } else {
            tool_write_error("xmlvalidate", "unknown option: ", opt.flag);
            tool_write_usage("xmlvalidate", "[--stream] [--dtd FILE|auto] [--allow-doctype] [--allow-pi] [--allow-comments] [--max-depth N] [--root NAME] [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlvalidate", "[--stream] [--dtd FILE|auto] [--allow-doctype] [--allow-pi] [--allow-comments] [--max-depth N] [--root NAME] [FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) return validate_one(&options, 0);
    for (i = opt.argi; i < argc; ++i) {
        if (validate_one(&options, argv[i]) != 0) exit_code = 1;
    }
    return exit_code;
}
