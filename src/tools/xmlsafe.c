#include "runtime.h"
#include "tool_util.h"
#include "xml.h"

typedef struct {
    int allow_doctype;
    int allow_pi;
    int allow_comments;
    int buffered;
    unsigned int max_depth;
    unsigned long long max_text;
} XmlSafeOptions;

static void write_safe_error(const char *path, const XmlToken *token, const char *message) {
    rt_write_cstr(2, "xmlsafe: ");
    if (path != 0) {
        rt_write_cstr(2, path);
        rt_write_char(2, ':');
    }
    rt_write_uint(2, token->line);
    rt_write_char(2, ':');
    rt_write_uint(2, token->column);
    rt_write_cstr(2, ": ");
    rt_write_cstr(2, message);
    rt_write_char(2, '\n');
}

static int safe_one(const char *path, const XmlSafeOptions *options) {
    XmlStreamOptions stream_options;
    XmlParser parser;
    XmlToken token;
    char *input_buffer;
    size_t length;
    int result;

    if (!options->buffered) {
        stream_options.allow_doctype = options->allow_doctype;
        stream_options.allow_pi = options->allow_pi;
        stream_options.allow_comments = options->allow_comments;
        stream_options.max_depth = options->max_depth;
        stream_options.max_text = options->max_text;
        stream_options.root_name = 0;
        return xml_stream_validate_document_with_options(path, "xmlsafe", &stream_options);
    }

    if (xml_read_document(path, &input_buffer, &length, "xmlsafe") != 0) {
        return 1;
    }
    xml_parser_init(&parser, input_buffer, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (!options->allow_doctype && token.type == XML_TOKEN_DOCTYPE) {
            write_safe_error(path, &token, "DOCTYPE is not allowed");
            xml_free_document(input_buffer);
            return 1;
        }
        if (!options->allow_pi && token.type == XML_TOKEN_PI) {
            write_safe_error(path, &token, "processing instruction is not allowed");
            xml_free_document(input_buffer);
            return 1;
        }
        if (!options->allow_comments && token.type == XML_TOKEN_COMMENT) {
            write_safe_error(path, &token, "comment is not allowed");
            xml_free_document(input_buffer);
            return 1;
        }
        if ((token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) && token.depth + 1U > options->max_depth) {
            write_safe_error(path, &token, "maximum depth exceeded");
            xml_free_document(input_buffer);
            return 1;
        }
        if ((token.type == XML_TOKEN_TEXT || token.type == XML_TOKEN_CDATA) && (unsigned long long)token.text_length > options->max_text) {
            write_safe_error(path, &token, "text node is too large");
            xml_free_document(input_buffer);
            return 1;
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlsafe", path, &parser);
        xml_free_document(input_buffer);
        return 1;
    }
    xml_free_document(input_buffer);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    XmlSafeOptions options;
    int option_result;
    int exit_code = 0;
    int i;

    options.allow_doctype = 0;
    options.allow_pi = 0;
    options.allow_comments = 0;
    options.buffered = 0;
    options.max_depth = 64U;
    options.max_text = 1048576ULL;

    tool_opt_init(&opt, argc, argv, "xmlsafe", "[--stream] [--buffered] [--allow-doctype] [--allow-pi] [--allow-comments] [--max-depth N] [--max-text N] [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--stream") == 0) {
            options.buffered = 0;
        } else if (rt_strcmp(opt.flag, "--buffered") == 0) {
            options.buffered = 1;
        } else if (rt_strcmp(opt.flag, "--allow-doctype") == 0) {
            options.allow_doctype = 1;
        } else if (rt_strcmp(opt.flag, "--allow-pi") == 0) {
            options.allow_pi = 1;
        } else if (rt_strcmp(opt.flag, "--allow-comments") == 0) {
            options.allow_comments = 1;
        } else if (rt_strcmp(opt.flag, "--max-depth") == 0) {
            unsigned long long value;
            if (tool_opt_require_value(&opt) != 0 || rt_parse_uint(opt.value, &value) != 0 || value == 0ULL || value > (unsigned long long)(~0U)) {
                tool_write_error("xmlsafe", "invalid maximum depth: ", opt.value);
                return 1;
            }
            options.max_depth = (unsigned int)value;
        } else if (rt_strcmp(opt.flag, "--max-text") == 0) {
            unsigned long long value;
            if (tool_opt_require_value(&opt) != 0 || rt_parse_uint(opt.value, &value) != 0 || value == 0ULL) {
                tool_write_error("xmlsafe", "invalid maximum text size: ", opt.value);
                return 1;
            }
            options.max_text = value;
        } else {
            tool_write_error("xmlsafe", "unknown option: ", opt.flag);
            tool_write_usage("xmlsafe", "[--stream] [--buffered] [--allow-doctype] [--allow-pi] [--allow-comments] [--max-depth N] [--max-text N] [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlsafe", "[--stream] [--buffered] [--allow-doctype] [--allow-pi] [--allow-comments] [--max-depth N] [--max-text N] [FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) {
        return safe_one(0, &options);
    }
    for (i = opt.argi; i < argc; ++i) {
        if (safe_one(argv[i], &options) != 0) {
            exit_code = 1;
        }
    }
    return exit_code;
}
