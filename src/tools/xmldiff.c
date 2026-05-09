#include "runtime.h"
#include "tool_util.h"
#include "xml.h"

static int next_significant(XmlParser *parser, XmlToken *token) {
    int result;
    while ((result = xml_next_token(parser, token)) > 0) {
        if (token->type == XML_TOKEN_TEXT && token->text_is_blank) continue;
        return result;
    }
    return result;
}

static int slice_equal(const char *left, size_t left_length, const char *right, size_t right_length) {
    if (left_length != right_length) return 0;
    return rt_strncmp(left, right, left_length) == 0;
}

static int attrs_equal(const XmlToken *left, const XmlToken *right) {
    size_t i;
    if (left->attribute_count != right->attribute_count) return 0;
    for (i = 0U; i < left->attribute_count; ++i) {
        size_t j;
        int found = 0;
        for (j = 0U; j < right->attribute_count; ++j) {
            if (xml_names_equal(&left->attributes[i].name, &right->attributes[j].name) &&
                slice_equal(left->attributes[i].value, left->attributes[i].value_length, right->attributes[j].value, right->attributes[j].value_length)) {
                found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

static int tokens_equal(const XmlToken *left, const XmlToken *right) {
    if (left->type != right->type) return 0;
    if (left->type == XML_TOKEN_START || left->type == XML_TOKEN_END || left->type == XML_TOKEN_EMPTY || left->type == XML_TOKEN_PI) {
        if (!xml_names_equal(&left->name, &right->name)) return 0;
    }
    if (left->type == XML_TOKEN_START || left->type == XML_TOKEN_EMPTY) {
        if (!attrs_equal(left, right)) return 0;
    }
    if (left->type == XML_TOKEN_TEXT || left->type == XML_TOKEN_CDATA || left->type == XML_TOKEN_COMMENT || left->type == XML_TOKEN_DOCTYPE || left->type == XML_TOKEN_PI) {
        if (!slice_equal(left->text, left->text_length, right->text, right->text_length)) return 0;
    }
    return 1;
}

static int diff_docs(const char *left_path, const char *right_path) {
    XmlParser left_parser;
    XmlParser right_parser;
    XmlToken left_token;
    XmlToken right_token;
    char *left_input;
    char *right_input;
    size_t left_length;
    size_t right_length;
    unsigned long long index = 0ULL;

    if (xml_read_document(left_path, &left_input, &left_length, "xmldiff") != 0) return 2;
    if (xml_read_document(right_path, &right_input, &right_length, "xmldiff") != 0) {
        xml_free_document(left_input);
        return 2;
    }
    xml_parser_init(&left_parser, left_input, left_length);
    xml_parser_init(&right_parser, right_input, right_length);
    for (;;) {
        int left_result = next_significant(&left_parser, &left_token);
        int right_result = next_significant(&right_parser, &right_token);
        index += 1ULL;
        if (left_result < 0 || (left_result == 0 && xml_parse_complete(&left_parser) != 0)) {
            xml_report_error("xmldiff", left_path, &left_parser);
            xml_free_document(left_input);
            xml_free_document(right_input);
            return 2;
        }
        if (right_result < 0 || (right_result == 0 && xml_parse_complete(&right_parser) != 0)) {
            xml_report_error("xmldiff", right_path, &right_parser);
            xml_free_document(left_input);
            xml_free_document(right_input);
            return 2;
        }
        if (left_result == 0 && right_result == 0) break;
        if (left_result != right_result || !tokens_equal(&left_token, &right_token)) {
            rt_write_cstr(1, "different at token ");
            rt_write_uint(1, index);
            rt_write_char(1, '\n');
            xml_free_document(left_input);
            xml_free_document(right_input);
            return 1;
        }
    }
    rt_write_cstr(1, "equal\n");
    xml_free_document(left_input);
    xml_free_document(right_input);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    int option_result;

    tool_opt_init(&opt, argc, argv, "xmldiff", "LEFT.xml RIGHT.xml");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        tool_write_error("xmldiff", "unknown option: ", opt.flag);
        tool_write_usage("xmldiff", "LEFT.xml RIGHT.xml");
        return 2;
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmldiff", "LEFT.xml RIGHT.xml");
        return 0;
    }
    if (opt.argi + 2 != argc) {
        tool_write_usage("xmldiff", "LEFT.xml RIGHT.xml");
        return 2;
    }
    return diff_docs(argv[opt.argi], argv[opt.argi + 1]);
}
