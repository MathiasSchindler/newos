#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


static void write_value_line(const char *text, size_t length) {
    xml_write_raw(1, text, length);
    rt_write_char(1, '\n');
}

static int get_one(const char *selector, const char *path) {
    XmlParser parser;
    XmlToken token;
    XmlNameStack stack;
    XmlSelector compiled_selector;
    char *input_buffer;
    char *attr_name = 0;
    char *element_selector = 0;
    unsigned int text_match_depth = 0U;
    int want_attr;
    size_t length;
    int result;

    xml_name_stack_init(&stack);

    want_attr = xml_selector_attribute_dup(selector, &attr_name, &element_selector);
    if (want_attr < 0) {
        tool_write_error("xmlget", "invalid selector: ", selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    if (tool_xml_selector_compile(&compiled_selector, want_attr ? element_selector : selector, "xmlget") != 0) {
        xml_name_stack_free(&stack);
        rt_free(attr_name);
        rt_free(element_selector);
        return 1;
    }
    if (xml_read_document(path, &input_buffer, &length, "xmlget") != 0) {
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        rt_free(attr_name);
        rt_free(element_selector);
        return 1;
    }
    xml_parser_init(&parser, input_buffer, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) {
            size_t i;
            if (tool_xml_name_stack_push(&stack, token.name, "xmlget") != 0) {
                xml_free_document(input_buffer);
                xml_selector_free(&compiled_selector);
                xml_name_stack_free(&stack);
                rt_free(attr_name);
                rt_free(element_selector);
                return 1;
            }
            if (want_attr && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                for (i = 0U; i < token.attribute_count; ++i) {
                    if (xml_name_equals(&token.attributes[i].name, attr_name)) {
                        write_value_line(token.attributes[i].value, token.attributes[i].value_length);
                    }
                }
            } else if (!want_attr && token.type == XML_TOKEN_START && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                text_match_depth = stack.count;
            }
            if (token.type == XML_TOKEN_EMPTY) {
                xml_name_stack_pop(&stack);
            }
        } else if (token.type == XML_TOKEN_TEXT || token.type == XML_TOKEN_CDATA) {
            if (!want_attr && !token.text_is_blank && (compiled_selector.has_predicates ? stack.count == text_match_depth : xml_name_stack_matches_compiled(&stack, &compiled_selector))) {
                write_value_line(token.text, token.text_length);
            }
        } else if (token.type == XML_TOKEN_END) {
            if (stack.count == text_match_depth) text_match_depth = 0U;
            xml_name_stack_pop(&stack);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlget", path, &parser);
        xml_free_document(input_buffer);
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        rt_free(attr_name);
        rt_free(element_selector);
        return 1;
    }
    xml_free_document(input_buffer);
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

    tool_opt_init(&opt, argc, argv, "xmlget", "SELECTOR [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        tool_write_error("xmlget", "unknown option: ", opt.flag);
        tool_write_usage("xmlget", "SELECTOR [FILE ...]");
        return 1;
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlget", "SELECTOR [FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) {
        tool_write_usage("xmlget", "SELECTOR [FILE ...]");
        return 1;
    }
    selector = argv[opt.argi++];
    if (opt.argi >= argc) {
        return get_one(selector, 0);
    }
    for (i = opt.argi; i < argc; ++i) {
        if (get_one(selector, argv[i]) != 0) {
            exit_code = 1;
        }
    }
    return exit_code;
}
