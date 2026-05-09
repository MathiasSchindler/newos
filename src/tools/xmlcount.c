#include "runtime.h"
#include "tool_util.h"
#include "xml.h"

typedef struct {
    unsigned long long elements;
    unsigned long long attributes;
    unsigned long long text_nodes;
    unsigned long long comments;
    unsigned long long cdata;
    unsigned long long processing_instructions;
    unsigned long long doctypes;
    unsigned long long max_depth;
} XmlCountStats;

static void update_max_depth(XmlCountStats *stats, const XmlToken *token) {
    unsigned long long depth = (unsigned long long)token->depth + 1ULL;
    if (depth > stats->max_depth) {
        stats->max_depth = depth;
    }
}

static void write_stat(const char *name, unsigned long long value) {
    rt_write_cstr(1, name);
    rt_write_char(1, ' ');
    rt_write_uint(1, value);
    rt_write_char(1, '\n');
}

static void write_stats(const XmlCountStats *stats) {
    write_stat("elements", stats->elements);
    write_stat("attributes", stats->attributes);
    write_stat("text", stats->text_nodes);
    write_stat("comments", stats->comments);
    write_stat("cdata", stats->cdata);
    write_stat("pi", stats->processing_instructions);
    write_stat("doctype", stats->doctypes);
    write_stat("max-depth", stats->max_depth);
}

static int count_one(const char *path) {
    XmlParser parser;
    XmlToken token;
    XmlCountStats stats;
    char *input_buffer;
    size_t length;
    int result;

    rt_memset(&stats, 0, sizeof(stats));
    if (xml_read_document(path, &input_buffer, &length, "xmlcount") != 0) {
        return 1;
    }
    xml_parser_init(&parser, input_buffer, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) {
            stats.elements += 1ULL;
            stats.attributes += (unsigned long long)token.attribute_count;
            update_max_depth(&stats, &token);
        } else if (token.type == XML_TOKEN_TEXT) {
            if (!token.text_is_blank) {
                stats.text_nodes += 1ULL;
            }
        } else if (token.type == XML_TOKEN_CDATA) {
            stats.cdata += 1ULL;
            if (token.text_length > 0U) {
                stats.text_nodes += 1ULL;
            }
        } else if (token.type == XML_TOKEN_COMMENT) {
            stats.comments += 1ULL;
        } else if (token.type == XML_TOKEN_PI) {
            stats.processing_instructions += 1ULL;
        } else if (token.type == XML_TOKEN_DOCTYPE) {
            stats.doctypes += 1ULL;
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlcount", path, &parser);
        xml_free_document(input_buffer);
        return 1;
    }
    write_stats(&stats);
    xml_free_document(input_buffer);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    int option_result;
    int exit_code = 0;
    int i;

    tool_opt_init(&opt, argc, argv, "xmlcount", "[FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        tool_write_error("xmlcount", "unknown option: ", opt.flag);
        tool_write_usage("xmlcount", "[FILE ...]");
        return 1;
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlcount", "[FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) {
        return count_one(0);
    }
    for (i = opt.argi; i < argc; ++i) {
        if (count_one(argv[i]) != 0) {
            exit_code = 1;
        }
    }
    return exit_code;
}
