#include "runtime.h"
#include "tool_util.h"
#include "xml.h"

static int cut_one(const char *selector, const char *path) {
    XmlParser parser;
    XmlToken token;
    XmlNameStack stack;
    XmlSelector compiled_selector;
    ToolOutputBuffer output;
    char *input_buffer;
    unsigned int cut_depth = 0U;
    int cutting = 0;
    size_t length;
    int result;

    xml_name_stack_init(&stack);
    if (tool_xml_selector_compile(&compiled_selector, selector, "xmlcut") != 0) { xml_name_stack_free(&stack); return 1; }

    if (xml_read_document(path, &input_buffer, &length, "xmlcut") != 0) {
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    tool_output_buffer_init(&output, 1);
    xml_parser_init(&parser, input_buffer, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START) {
            if (cutting) {
                tool_output_buffer_write(&output, token.raw, token.raw_length);
            }
            TOOL_XML_NAME_STACK_PUSH_OR_RETURN(&stack, token.name, "xmlcut", xml_free_document(input_buffer); xml_selector_free(&compiled_selector); xml_name_stack_free(&stack);)
            if (!cutting && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                cutting = 1;
                cut_depth = stack.count;
                tool_output_buffer_write(&output, token.raw, token.raw_length);
            }
        } else if (token.type == XML_TOKEN_EMPTY) {
            TOOL_XML_NAME_STACK_PUSH_OR_RETURN(&stack, token.name, "xmlcut", xml_free_document(input_buffer); xml_selector_free(&compiled_selector); xml_name_stack_free(&stack);)
            if (cutting || xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                tool_output_buffer_write(&output, token.raw, token.raw_length);
            }
            xml_name_stack_pop(&stack);
        } else if (token.type == XML_TOKEN_END) {
            if (cutting) {
                tool_output_buffer_write(&output, token.raw, token.raw_length);
                if (stack.count == cut_depth) {
                    tool_output_buffer_write_char(&output, '\n');
                    cutting = 0;
                }
            }
            xml_name_stack_pop(&stack);
        } else if (cutting) {
            tool_output_buffer_write(&output, token.raw, token.raw_length);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlcut", path, &parser);
        xml_free_document(input_buffer);
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    if (tool_output_buffer_flush(&output) != 0) {
        tool_write_error("xmlcut", "write failed", 0);
        xml_free_document(input_buffer);
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    xml_free_document(input_buffer);
    xml_selector_free(&compiled_selector);
    xml_name_stack_free(&stack);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    int option_result;
    int exit_code = 0;
    const char *selector;
    const char *wrap_name = 0;
    int i;

    tool_opt_init(&opt, argc, argv, "xmlcut", "[--wrap NAME] SELECTOR [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--wrap") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            wrap_name = opt.value;
        } else {
            tool_write_error("xmlcut", "unknown option: ", opt.flag);
            tool_write_usage("xmlcut", "[--wrap NAME] SELECTOR [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlcut", "[--wrap NAME] SELECTOR [FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) {
        tool_write_usage("xmlcut", "[--wrap NAME] SELECTOR [FILE ...]");
        return 1;
    }
    if (wrap_name != 0 && !xml_is_name(wrap_name)) {
        tool_write_error("xmlcut", "invalid wrapper name: ", wrap_name);
        return 1;
    }
    selector = argv[opt.argi++];
    if (wrap_name != 0) { rt_write_char(1, '<'); rt_write_cstr(1, wrap_name); rt_write_cstr(1, ">\n"); }
    if (opt.argi >= argc) {
        exit_code = cut_one(selector, 0);
    } else {
        for (i = opt.argi; i < argc; ++i) {
            if (cut_one(selector, argv[i]) != 0) {
                exit_code = 1;
            }
        }
    }
    if (wrap_name != 0) { rt_write_cstr(1, "</"); rt_write_cstr(1, wrap_name); rt_write_cstr(1, ">\n"); }
    return exit_code;
}
