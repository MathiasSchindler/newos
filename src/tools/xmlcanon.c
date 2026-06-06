#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


typedef struct {
    int strip_comments;
    int sort_attrs;
    int expand_empty;
} XmlCanonOptions;

static int attr_compare(const XmlAttribute *left, const XmlAttribute *right) {
    size_t i = 0U;
    while (i < left->name.length && i < right->name.length) {
        if (left->name.start[i] < right->name.start[i]) return -1;
        if (left->name.start[i] > right->name.start[i]) return 1;
        i += 1U;
    }
    if (left->name.length < right->name.length) return -1;
    if (left->name.length > right->name.length) return 1;
    return 0;
}

static void write_attrs(const XmlToken *token, int sort_attrs) {
    size_t inline_order[XML_INITIAL_ATTRIBUTES];
    size_t *order = token->attribute_count <= XML_INITIAL_ATTRIBUTES ? inline_order : (size_t *)rt_malloc_array(token->attribute_count, sizeof(*order));
    size_t i;
    size_t j;

    if (order == 0) return;

    for (i = 0U; i < token->attribute_count; ++i) {
        order[i] = i;
    }
    if (sort_attrs) {
        for (i = 1U; i < token->attribute_count; ++i) {
            size_t value = order[i];
            j = i;
            while (j > 0U && attr_compare(&token->attributes[value], &token->attributes[order[j - 1U]]) < 0) {
                order[j] = order[j - 1U];
                j -= 1U;
            }
            order[j] = value;
        }
    }
    for (i = 0U; i < token->attribute_count; ++i) {
        const XmlAttribute *attr = &token->attributes[order[i]];
        rt_write_char(1, ' ');
        xml_write_raw(1, attr->name.start, attr->name.length);
        rt_write_cstr(1, "=\"");
        xml_write_raw(1, attr->value, attr->value_length);
        rt_write_char(1, '"');
    }
    if (order != inline_order) rt_free(order);
}

static void write_start(const XmlToken *token, const XmlCanonOptions *options, int empty) {
    rt_write_char(1, '<');
    xml_write_raw(1, token->name.start, token->name.length);
    write_attrs(token, options->sort_attrs);
    if (empty && !options->expand_empty) {
        rt_write_cstr(1, "/>");
    } else {
        rt_write_char(1, '>');
        if (empty) {
            rt_write_cstr(1, "</");
            xml_write_raw(1, token->name.start, token->name.length);
            rt_write_char(1, '>');
        }
    }
}

static int canon_one(const char *path, const XmlCanonOptions *options) {
    XmlParser parser;
    XmlToken token;
    char *input;
    size_t length;
    int result;

    if (xml_read_document(path, &input, &length, "xmlcanon") != 0) return 1;
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_COMMENT && options->strip_comments) {
            continue;
        } else if (token.type == XML_TOKEN_START) {
            write_start(&token, options, 0);
        } else if (token.type == XML_TOKEN_EMPTY) {
            write_start(&token, options, 1);
        } else if (token.type == XML_TOKEN_END) {
            rt_write_cstr(1, "</");
            xml_write_raw(1, token.name.start, token.name.length);
            rt_write_char(1, '>');
        } else {
            xml_write_raw(1, token.raw, token.raw_length);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlcanon", path, &parser);
        xml_free_document(input);
        return 1;
    }
    xml_free_document(input);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    XmlCanonOptions options;
    int option_result;
    int exit_code = 0;
    int i;

    options.strip_comments = 0;
    options.sort_attrs = 0;
    options.expand_empty = 0;
    tool_opt_init(&opt, argc, argv, "xmlcanon", "[--strip-comments] [--sort-attrs] [--expand-empty] [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--strip-comments") == 0) options.strip_comments = 1;
        else if (rt_strcmp(opt.flag, "--sort-attrs") == 0) options.sort_attrs = 1;
        else if (rt_strcmp(opt.flag, "--expand-empty") == 0) options.expand_empty = 1;
        else {
            tool_write_error("xmlcanon", "unknown option: ", opt.flag);
            tool_write_usage("xmlcanon", "[--strip-comments] [--sort-attrs] [--expand-empty] [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlcanon", "[--strip-comments] [--sort-attrs] [--expand-empty] [FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) return canon_one(0, &options);
    for (i = opt.argi; i < argc; ++i) {
        if (canon_one(argv[i], &options) != 0) exit_code = 1;
    }
    return exit_code;
}
