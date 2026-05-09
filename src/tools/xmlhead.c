#include "runtime.h"
#include "tool_util.h"
#include "xml.h"

static int head_one(const char *selector, unsigned long long limit, const char *path, unsigned long long *written) {
    XmlParser parser;
    XmlToken token;
    XmlNameStack stack;
    XmlSelector compiled_selector;
    ToolOutputBuffer output;
    char *input;
    unsigned int copy_depth = 0U;
    int copying = 0;
    size_t length;
    int result = 0;

    xml_name_stack_init(&stack);
    if (xml_selector_compile(&compiled_selector, selector) != 0) { xml_name_stack_free(&stack); return 1; }

    if (xml_read_document(path, &input, &length, "xmlhead") != 0) { xml_selector_free(&compiled_selector); xml_name_stack_free(&stack); return 1; }
    tool_output_buffer_init(&output, 1);
    xml_parser_init(&parser, input, length);
    while ((copying || *written < limit) && (result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START) {
            if (copying) tool_output_buffer_write(&output, token.raw, token.raw_length);
            TOOL_XML_NAME_STACK_PUSH_OR_RETURN(&stack, token.name, "xmlhead", xml_free_document(input); xml_selector_free(&compiled_selector); xml_name_stack_free(&stack);)
            if (!copying && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                copying = 1;
                copy_depth = stack.count;
                *written += 1ULL;
                tool_output_buffer_write(&output, token.raw, token.raw_length);
            }
        } else if (token.type == XML_TOKEN_EMPTY) {
            TOOL_XML_NAME_STACK_PUSH_OR_RETURN(&stack, token.name, "xmlhead", xml_free_document(input); xml_selector_free(&compiled_selector); xml_name_stack_free(&stack);)
            if (copying || xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                tool_output_buffer_write(&output, token.raw, token.raw_length);
                if (!copying) {
                    *written += 1ULL;
                    tool_output_buffer_write_char(&output, '\n');
                }
            }
            xml_name_stack_pop(&stack);
        } else if (token.type == XML_TOKEN_END) {
            if (copying) {
                tool_output_buffer_write(&output, token.raw, token.raw_length);
                if (stack.count == copy_depth) {
                    tool_output_buffer_write_char(&output, '\n');
                    copying = 0;
                }
            }
            xml_name_stack_pop(&stack);
        } else if (copying) tool_output_buffer_write(&output, token.raw, token.raw_length);
    }
    if (result < 0 || (!copying && *written < limit && xml_parse_complete(&parser) != 0)) {
        xml_report_error("xmlhead", path, &parser);
        xml_free_document(input);
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    if (tool_output_buffer_flush(&output) != 0) {
        tool_write_error("xmlhead", "write failed", 0);
        xml_free_document(input);
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    xml_free_document(input);
    xml_selector_free(&compiled_selector);
    xml_name_stack_free(&stack);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    int option_result;
    unsigned long long limit = 10ULL;
    unsigned long long written = 0ULL;
    const char *selector;
    const char *wrap_name = 0;
    int exit_code = 0;
    int i;

    tool_opt_init(&opt, argc, argv, "xmlhead", "[-n COUNT] [--wrap NAME] SELECTOR [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-n") == 0 || rt_strcmp(opt.flag, "--count") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            if (rt_parse_uint(opt.value, &limit) != 0) return 1;
        } else if (rt_strcmp(opt.flag, "--wrap") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            wrap_name = opt.value;
        } else {
            tool_write_error("xmlhead", "unknown option: ", opt.flag);
            tool_write_usage("xmlhead", "[-n COUNT] [--wrap NAME] SELECTOR [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlhead", "[-n COUNT] [--wrap NAME] SELECTOR [FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) {
        tool_write_usage("xmlhead", "[-n COUNT] [--wrap NAME] SELECTOR [FILE ...]");
        return 1;
    }
    if (wrap_name != 0 && !xml_is_name(wrap_name)) {
        tool_write_error("xmlhead", "invalid wrapper name: ", wrap_name);
        return 1;
    }
    selector = argv[opt.argi++];
    if (wrap_name != 0) { rt_write_char(1, '<'); rt_write_cstr(1, wrap_name); rt_write_cstr(1, ">\n"); }
    if (opt.argi >= argc) {
        exit_code = head_one(selector, limit, 0, &written);
    } else {
        for (i = opt.argi; i < argc && written < limit; ++i) {
            if (head_one(selector, limit, argv[i], &written) != 0) exit_code = 1;
        }
    }
    if (wrap_name != 0) { rt_write_cstr(1, "</"); rt_write_cstr(1, wrap_name); rt_write_cstr(1, ">\n"); }
    return exit_code;
}
