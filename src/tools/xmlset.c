#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


static void write_start_with_attr(const XmlToken *token, const char *attr_name, const char *value, int set_attr) {
    size_t i;
    int found = 0;
    rt_write_char(1, '<');
    xml_write_raw(1, token->name.start, token->name.length);
    for (i = 0U; i < token->attribute_count; ++i) {
        rt_write_char(1, ' ');
        xml_write_raw(1, token->attributes[i].name.start, token->attributes[i].name.length);
        rt_write_cstr(1, "=\"");
        if (set_attr && xml_name_equals(&token->attributes[i].name, attr_name)) {
            xml_write_escaped_attr(1, value, rt_strlen(value));
            found = 1;
        } else {
            xml_write_raw(1, token->attributes[i].value, token->attributes[i].value_length);
        }
        rt_write_char(1, '"');
    }
    if (set_attr && !found) {
        rt_write_char(1, ' ');
        rt_write_cstr(1, attr_name);
        rt_write_cstr(1, "=\"");
        xml_write_escaped_attr(1, value, rt_strlen(value));
        rt_write_char(1, '"');
    }
    rt_write_cstr(1, token->type == XML_TOKEN_EMPTY ? "/>" : ">");
}

static void write_start_open(const XmlToken *token) {
    size_t i;
    rt_write_char(1, '<');
    xml_write_raw(1, token->name.start, token->name.length);
    for (i = 0U; i < token->attribute_count; ++i) {
        rt_write_char(1, ' ');
        xml_write_raw(1, token->attributes[i].name.start, token->attributes[i].name.length);
        rt_write_cstr(1, "=\"");
        xml_write_raw(1, token->attributes[i].value, token->attributes[i].value_length);
        rt_write_char(1, '"');
    }
    rt_write_char(1, '>');
}

static int check_element_replacement_safe(const char *input, size_t length, const XmlSelector *compiled_selector, const char *path) {
    XmlParser parser;
    XmlToken token;
    XmlNameStack stack;
    unsigned int replacement_depth = 0U;
    int result;

    xml_name_stack_init(&stack);
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) {
            if (tool_xml_name_stack_push(&stack, token.name, "xmlset") != 0) {
                xml_name_stack_free(&stack);
                return 1;
            }
            if (replacement_depth > 0U && stack.count > replacement_depth) {
                tool_write_error("xmlset", "selected element contains child elements; use --force: ", path == 0 ? "-" : path);
                xml_name_stack_free(&stack);
                return 1;
            }
            if (replacement_depth == 0U && xml_name_stack_matches_token(&stack, &token, compiled_selector)) {
                if (token.type == XML_TOKEN_START) replacement_depth = stack.count;
            }
            if (token.type == XML_TOKEN_EMPTY) xml_name_stack_pop(&stack);
        } else if (token.type == XML_TOKEN_END) {
            if (replacement_depth > 0U && stack.count == replacement_depth) replacement_depth = 0U;
            xml_name_stack_pop(&stack);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlset", path, &parser);
        xml_name_stack_free(&stack);
        return 1;
    }
    xml_name_stack_free(&stack);
    return 0;
}

static int set_one(const char *selector, const char *value, const char *path, int force) {
    XmlParser parser;
    XmlToken token;
    XmlNameStack stack;
    XmlSelector compiled_selector;
    char *attr_name = 0;
    char *element_selector = 0;
    char *input;
    unsigned int skip_depth = 0U;
    int replacing = 0;
    int want_attr;
    size_t length;
    int result;

    xml_name_stack_init(&stack);

    want_attr = xml_selector_attribute_dup(selector, &attr_name, &element_selector);
    if (want_attr < 0) {
        tool_write_error("xmlset", "invalid selector: ", selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    if (want_attr && !xml_is_name(attr_name)) {
        tool_write_error("xmlset", "invalid XML name: ", attr_name);
        xml_name_stack_free(&stack);
        rt_free(attr_name);
        rt_free(element_selector);
        return 1;
    }
    if (tool_xml_selector_compile(&compiled_selector, want_attr ? element_selector : selector, "xmlset") != 0) { xml_name_stack_free(&stack); rt_free(attr_name); rt_free(element_selector); return 1; }
    if (xml_read_document(path, &input, &length, "xmlset") != 0) { xml_selector_free(&compiled_selector); xml_name_stack_free(&stack); rt_free(attr_name); rt_free(element_selector); return 1; }
    if (!want_attr && !force && check_element_replacement_safe(input, length, &compiled_selector, path) != 0) {
        xml_free_document(input);
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        rt_free(attr_name);
        rt_free(element_selector);
        return 1;
    }
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) {
            int match;
            if (tool_xml_name_stack_push(&stack, token.name, "xmlset") != 0) {
                xml_free_document(input);
                xml_selector_free(&compiled_selector);
                xml_name_stack_free(&stack);
                rt_free(attr_name);
                rt_free(element_selector);
                return 1;
            }
            match = xml_name_stack_matches_token(&stack, &token, &compiled_selector);
            if (want_attr && match) {
                write_start_with_attr(&token, attr_name, value, 1);
                if (token.type == XML_TOKEN_EMPTY) xml_name_stack_pop(&stack);
                continue;
            }
            if (!want_attr && match) {
                write_start_open(&token);
                xml_write_escaped_text(1, value, rt_strlen(value));
                if (token.type == XML_TOKEN_EMPTY) {
                    rt_write_cstr(1, "</");
                    xml_write_raw(1, token.name.start, token.name.length);
                    rt_write_char(1, '>');
                    xml_name_stack_pop(&stack);
                } else {
                    replacing = 1;
                    skip_depth = stack.count;
                }
                continue;
            }
            if (!replacing) xml_write_raw(1, token.raw, token.raw_length);
            if (token.type == XML_TOKEN_EMPTY) xml_name_stack_pop(&stack);
        } else if (token.type == XML_TOKEN_END) {
            if (replacing && stack.count == skip_depth) {
                xml_write_raw(1, token.raw, token.raw_length);
                replacing = 0;
                xml_name_stack_pop(&stack);
                continue;
            }
            if (!replacing) xml_write_raw(1, token.raw, token.raw_length);
            xml_name_stack_pop(&stack);
        } else if (!replacing) {
            xml_write_raw(1, token.raw, token.raw_length);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlset", path, &parser);
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
    const char *value;
    int force = 0;
    int i;

    tool_opt_init(&opt, argc, argv, "xmlset", "[--force] SELECTOR VALUE [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--force") == 0) {
            force = 1;
        } else {
            tool_write_error("xmlset", "unknown option: ", opt.flag);
            tool_write_usage("xmlset", "[--force] SELECTOR VALUE [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlset", "[--force] SELECTOR VALUE [FILE ...]");
        return 0;
    }
    if (opt.argi + 1 >= argc) {
        tool_write_usage("xmlset", "[--force] SELECTOR VALUE [FILE ...]");
        return 1;
    }
    selector = argv[opt.argi++];
    value = argv[opt.argi++];
    if (opt.argi >= argc) return set_one(selector, value, 0, force);
    for (i = opt.argi; i < argc; ++i) {
        if (set_one(selector, value, argv[i], force) != 0) exit_code = 1;
    }
    return exit_code;
}
