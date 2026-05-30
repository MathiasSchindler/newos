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

static void update_max_depth(XmlCountStats *stats, unsigned int token_depth) {
    unsigned long long depth = (unsigned long long)token_depth + 1ULL;
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

static int count_event(const XmlStreamEvent *event, void *user_data) {
    XmlCountStats *stats = (XmlCountStats *)user_data;
    if (event->type == XML_TOKEN_START || event->type == XML_TOKEN_EMPTY) {
        stats->elements += 1ULL;
        stats->attributes += (unsigned long long)event->attribute_count;
        update_max_depth(stats, event->depth);
    } else if (event->type == XML_TOKEN_TEXT) {
        if (!event->text_is_blank) {
            stats->text_nodes += 1ULL;
        }
    } else if (event->type == XML_TOKEN_CDATA) {
        stats->cdata += 1ULL;
        if (event->text_length > 0U) {
            stats->text_nodes += 1ULL;
        }
    } else if (event->type == XML_TOKEN_COMMENT) {
        stats->comments += 1ULL;
    } else if (event->type == XML_TOKEN_PI) {
        stats->processing_instructions += 1ULL;
    } else if (event->type == XML_TOKEN_DOCTYPE) {
        stats->doctypes += 1ULL;
    }
    return 0;
}

static int count_one(const char *path) {
    XmlCountStats stats;

    rt_memset(&stats, 0, sizeof(stats));
    if (xml_stream_visit_document(path, "xmlcount", count_event, &stats) != 0) {
        return 1;
    }
    write_stats(&stats);
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
