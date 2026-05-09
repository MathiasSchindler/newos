#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


static void write_tag_without_attr(const XmlToken *token, const char *attr_name) {
    size_t i;
    rt_write_char(1, '<');
    xml_write_raw(1, token->name.start, token->name.length);
    for (i = 0U; i < token->attribute_count; ++i) {
        if (xml_name_equals(&token->attributes[i].name, attr_name)) continue;
        rt_write_char(1, ' ');
        xml_write_raw(1, token->attributes[i].name.start, token->attributes[i].name.length);
        rt_write_cstr(1, "=\"");
        xml_write_raw(1, token->attributes[i].value, token->attributes[i].value_length);
        rt_write_char(1, '"');
    }
    rt_write_cstr(1, token->type == XML_TOKEN_EMPTY ? "/>" : ">");
}

static int del_one(const char *selector, const char *path) {
    XmlParser parser;
    XmlToken token;
    XmlNameStack stack;
    XmlSelector compiled_selector;
    char *attr_name = 0;
    char *element_selector = 0;
    char *input;
    unsigned int skip_depth = 0U;
    int deleting = 0;
    int want_attr;
    size_t length;
    int result;

    xml_name_stack_init(&stack);

    want_attr = xml_selector_attribute_dup(selector, &attr_name, &element_selector);
    if (want_attr < 0) {
        tool_write_error("xmldel", "invalid selector: ", selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    if (tool_xml_selector_compile(&compiled_selector, want_attr ? element_selector : selector, "xmldel") != 0) { xml_name_stack_free(&stack); rt_free(attr_name); rt_free(element_selector); return 1; }
    if (xml_read_document(path, &input, &length, "xmldel") != 0) { xml_selector_free(&compiled_selector); xml_name_stack_free(&stack); rt_free(attr_name); rt_free(element_selector); return 1; }
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) {
            if (tool_xml_name_stack_push(&stack, token.name, "xmldel") != 0) {
                xml_free_document(input);
                xml_selector_free(&compiled_selector);
                xml_name_stack_free(&stack);
                rt_free(attr_name);
                rt_free(element_selector);
                return 1;
            }
            if (!want_attr && !deleting && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                if (token.type == XML_TOKEN_START) {
                    deleting = 1;
                    skip_depth = stack.count;
                }
                if (token.type == XML_TOKEN_EMPTY) xml_name_stack_pop(&stack);
                continue;
            }
            if (!deleting) {
                if (want_attr && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) write_tag_without_attr(&token, attr_name);
                else xml_write_raw(1, token.raw, token.raw_length);
            }
            if (token.type == XML_TOKEN_EMPTY) xml_name_stack_pop(&stack);
        } else if (token.type == XML_TOKEN_END) {
            if (deleting && stack.count == skip_depth) {
                deleting = 0;
                xml_name_stack_pop(&stack);
                continue;
            }
            if (!deleting) xml_write_raw(1, token.raw, token.raw_length);
            xml_name_stack_pop(&stack);
        } else if (!deleting) {
            xml_write_raw(1, token.raw, token.raw_length);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmldel", path, &parser);
        xml_free_document(input);
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        rt_free(attr_name);
        rt_free(element_selector);
        return 1;
    }
    xml_free_document(input);
    xml_selector_free(&compiled_selector);
    xml_name_stack_free(&stack);
    rt_free(attr_name);
    rt_free(element_selector);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    int option_result;
    int exit_code = 0;
    const char *selector;
    int i;

    tool_opt_init(&opt, argc, argv, "xmldel", "SELECTOR [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        tool_write_error("xmldel", "unknown option: ", opt.flag);
        tool_write_usage("xmldel", "SELECTOR [FILE ...]");
        return 1;
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmldel", "SELECTOR [FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) {
        tool_write_usage("xmldel", "SELECTOR [FILE ...]");
        return 1;
    }
    selector = argv[opt.argi++];
    if (opt.argi >= argc) return del_one(selector, 0);
    for (i = opt.argi; i < argc; ++i) {
        if (del_one(selector, argv[i]) != 0) exit_code = 1;
    }
    return exit_code;
}
