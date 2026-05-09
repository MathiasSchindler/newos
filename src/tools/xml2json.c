#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


static void write_json_string(const char *text, size_t length) {
    size_t i;
    rt_write_char(1, '"');
    for (i = 0U; i < length; ++i) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '"') rt_write_cstr(1, "\\\"");
        else if (ch == '\\') rt_write_cstr(1, "\\\\");
        else if (ch == '\n') rt_write_cstr(1, "\\n");
        else if (ch == '\r') rt_write_cstr(1, "\\r");
        else if (ch == '\t') rt_write_cstr(1, "\\t");
        else if (ch < 0x20U) rt_write_cstr(1, " ");
        else rt_write_char(1, (char)ch);
    }
    rt_write_char(1, '"');
}

static void write_attrs(const XmlToken *token) {
    size_t i;
    rt_write_cstr(1, "\"attributes\":{");
    for (i = 0U; i < token->attribute_count; ++i) {
        if (i > 0U) rt_write_char(1, ',');
        write_json_string(token->attributes[i].name.start, token->attributes[i].name.length);
        rt_write_char(1, ':');
        write_json_string(token->attributes[i].value, token->attributes[i].value_length);
    }
    rt_write_char(1, '}');
}

static void write_element_start(const XmlToken *token) {
    rt_write_cstr(1, "{\"name\":");
    write_json_string(token->name.start, token->name.length);
    rt_write_char(1, ',');
    write_attrs(token);
    rt_write_cstr(1, ",\"children\":[");
}

static int ensure_child_counts(unsigned int **counts_io, size_t *capacity, unsigned int *inline_counts, size_t needed) {
    unsigned int *counts;
    size_t next_capacity;
    size_t i;
    if (needed <= *capacity) return 0;
    next_capacity = *capacity == 0U ? XML_INITIAL_DEPTH : *capacity;
    while (next_capacity < needed) {
        if (next_capacity > (size_t)(~(size_t)0 / 2U)) return -1;
        next_capacity *= 2U;
    }
    counts = (unsigned int *)rt_malloc(next_capacity * sizeof(*counts));
    if (counts == 0) return -1;
    for (i = 0U; i < *capacity; ++i) counts[i] = (*counts_io)[i];
    for (; i < next_capacity; ++i) counts[i] = 0U;
    if (*counts_io != inline_counts) rt_free(*counts_io);
    *counts_io = counts;
    *capacity = next_capacity;
    return 0;
}

static int json_one(const char *path) {
    XmlParser parser;
    XmlToken token;
    char *input;
    unsigned int inline_child_counts[XML_INITIAL_DEPTH];
    unsigned int *child_counts = inline_child_counts;
    size_t child_count_capacity = XML_INITIAL_DEPTH;
    unsigned int depth = 0U;
    size_t length;
    int result;

    rt_memset(inline_child_counts, 0, sizeof(inline_child_counts));
    if (xml_read_document(path, &input, &length, "xml2json") != 0) return 1;
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) {
            if (ensure_child_counts(&child_counts, &child_count_capacity, inline_child_counts, (size_t)depth + 1U) != 0) {
                xml_free_document(input);
                if (child_counts != inline_child_counts) rt_free(child_counts);
                return 1;
            }
            if (depth > 0U) {
                if (child_counts[depth - 1U] > 0U) rt_write_char(1, ',');
                child_counts[depth - 1U] += 1U;
            }
            write_element_start(&token);
            if (token.type == XML_TOKEN_EMPTY) {
                rt_write_cstr(1, "]}");
            } else {
                child_counts[depth] = 0U;
                depth += 1U;
            }
        } else if (token.type == XML_TOKEN_TEXT || token.type == XML_TOKEN_CDATA) {
            if (!token.text_is_blank && depth > 0U) {
                if (child_counts[depth - 1U] > 0U) rt_write_char(1, ',');
                child_counts[depth - 1U] += 1U;
                rt_write_cstr(1, "{\"text\":");
                write_json_string(token.text, token.text_length);
                rt_write_char(1, '}');
            }
        } else if (token.type == XML_TOKEN_END) {
            if (depth > 0U) depth -= 1U;
            rt_write_cstr(1, "]}");
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xml2json", path, &parser);
        xml_free_document(input);
        if (child_counts != inline_child_counts) rt_free(child_counts);
        return 1;
    }
    rt_write_char(1, '\n');
    xml_free_document(input);
    if (child_counts != inline_child_counts) rt_free(child_counts);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    int option_result;
    int exit_code = 0;
    int i;

    tool_opt_init(&opt, argc, argv, "xml2json", "[FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        tool_write_error("xml2json", "unknown option: ", opt.flag);
        tool_write_usage("xml2json", "[FILE ...]");
        return 1;
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xml2json", "[FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) return json_one(0);
    for (i = opt.argi; i < argc; ++i) {
        if (json_one(argv[i]) != 0) exit_code = 1;
    }
    return exit_code;
}
