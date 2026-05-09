#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


static void write_name_or_text(const XmlName *name, const char *replacement, int replace) {
    if (replace) rt_write_cstr(1, replacement);
    else xml_write_raw(1, name->start, name->length);
}

static void write_tag(const XmlToken *token, const char *element_name, int rename_element, const char *attr_name, const char *new_attr, int rename_attr) {
    size_t i;
    rt_write_char(1, '<');
    write_name_or_text(&token->name, element_name, rename_element);
    for (i = 0U; i < token->attribute_count; ++i) {
        rt_write_char(1, ' ');
        if (rename_attr && xml_name_equals(&token->attributes[i].name, attr_name)) rt_write_cstr(1, new_attr);
        else xml_write_raw(1, token->attributes[i].name.start, token->attributes[i].name.length);
        rt_write_cstr(1, "=\"");
        xml_write_raw(1, token->attributes[i].value, token->attributes[i].value_length);
        rt_write_char(1, '"');
    }
    rt_write_cstr(1, token->type == XML_TOKEN_EMPTY ? "/>" : ">");
    if (token->type == XML_TOKEN_EMPTY && rename_element) {
    }
}

static int ensure_renamed_capacity(char ***renamed_io, size_t *capacity_io, size_t needed) {
    char **resized;
    size_t old_capacity = *capacity_io;
    size_t new_capacity = old_capacity == 0U ? XML_INITIAL_DEPTH : old_capacity;
    size_t i;
    while (new_capacity < needed) {
        if (new_capacity > ((size_t)-1) / 2U) return -1;
        new_capacity *= 2U;
    }
    if (new_capacity == old_capacity) return 0;
    resized = (char **)rt_realloc(*renamed_io, new_capacity * sizeof(*resized));
    if (resized == 0) return -1;
    for (i = old_capacity; i < new_capacity; ++i) resized[i] = 0;
    *renamed_io = resized;
    *capacity_io = new_capacity;
    return 0;
}

static void free_renamed(char **renamed, size_t capacity) {
    size_t i;
    if (renamed == 0) return;
    for (i = 0U; i < capacity; ++i) rt_free(renamed[i]);
    rt_free(renamed);
}

static int rename_one(const char *selector, const char *new_name, const char *path) {
    XmlParser parser;
    XmlToken token;
    XmlNameStack stack;
    XmlSelector compiled_selector;
    char **renamed = 0;
    size_t renamed_capacity = 0U;
    char *attr_name = 0;
    char *element_selector = 0;
    char *input;
    int want_attr;
    size_t length;
    int result;

    xml_name_stack_init(&stack);

    want_attr = xml_selector_attribute_dup(selector, &attr_name, &element_selector);
    if (want_attr < 0) {
        tool_write_error("xmlrename", "invalid selector: ", selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    if (want_attr && new_name[0] == '@') new_name += 1;
    if (!xml_is_name(new_name)) {
        tool_write_error("xmlrename", "invalid XML name: ", new_name);
        xml_name_stack_free(&stack);
        rt_free(attr_name);
        rt_free(element_selector);
        return 1;
    }
    if (tool_xml_selector_compile(&compiled_selector, want_attr ? element_selector : selector, "xmlrename") != 0) { xml_name_stack_free(&stack); rt_free(attr_name); rt_free(element_selector); return 1; }
    if (xml_read_document(path, &input, &length, "xmlrename") != 0) { xml_selector_free(&compiled_selector); xml_name_stack_free(&stack); rt_free(attr_name); rt_free(element_selector); return 1; }
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) {
            int match;
            if (tool_xml_name_stack_push(&stack, token.name, "xmlrename") != 0) {
                xml_free_document(input);
                xml_selector_free(&compiled_selector);
                xml_name_stack_free(&stack);
                free_renamed(renamed, renamed_capacity);
                rt_free(attr_name);
                rt_free(element_selector);
                return 1;
            }
            match = xml_name_stack_matches_token(&stack, &token, &compiled_selector);
            if (!want_attr && match) {
                write_tag(&token, new_name, 1, 0, 0, 0);
                if (token.type == XML_TOKEN_START) {
                    if (ensure_renamed_capacity(&renamed, &renamed_capacity, stack.count) != 0) {
                        tool_write_error("xmlrename", "out of memory", 0);
                        xml_free_document(input);
                        xml_selector_free(&compiled_selector);
                        xml_name_stack_free(&stack);
                        free_renamed(renamed, renamed_capacity);
                        rt_free(attr_name);
                        rt_free(element_selector);
                        return 1;
                    }
                    rt_free(renamed[stack.count - 1U]);
                    renamed[stack.count - 1U] = xml_slice_dup(new_name, rt_strlen(new_name));
                    if (renamed[stack.count - 1U] == 0) {
                        tool_write_error("xmlrename", "out of memory", 0);
                        xml_free_document(input);
                        xml_selector_free(&compiled_selector);
                        xml_name_stack_free(&stack);
                        free_renamed(renamed, renamed_capacity);
                        rt_free(attr_name);
                        rt_free(element_selector);
                        return 1;
                    }
                }
            } else if (want_attr && match) {
                write_tag(&token, 0, 0, attr_name, new_name, 1);
            } else {
                xml_write_raw(1, token.raw, token.raw_length);
            }
            if (token.type == XML_TOKEN_EMPTY) xml_name_stack_pop(&stack);
        } else if (token.type == XML_TOKEN_END) {
            if (stack.count > 0U && stack.count <= renamed_capacity && renamed[stack.count - 1U] != 0) {
                rt_write_cstr(1, "</");
                rt_write_cstr(1, renamed[stack.count - 1U]);
                rt_write_char(1, '>');
                rt_free(renamed[stack.count - 1U]);
                renamed[stack.count - 1U] = 0;
            } else {
                xml_write_raw(1, token.raw, token.raw_length);
            }
            xml_name_stack_pop(&stack);
        } else {
            xml_write_raw(1, token.raw, token.raw_length);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlrename", path, &parser);
        xml_free_document(input);
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        free_renamed(renamed, renamed_capacity);
        rt_free(attr_name);
        rt_free(element_selector);
        return 1;
    }
    xml_free_document(input);
    xml_selector_free(&compiled_selector);
    xml_name_stack_free(&stack);
    free_renamed(renamed, renamed_capacity);
    rt_free(attr_name);
    rt_free(element_selector);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    int option_result;
    int exit_code = 0;
    const char *selector;
    const char *new_name;
    int i;

    tool_opt_init(&opt, argc, argv, "xmlrename", "SELECTOR NEW-NAME [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        tool_write_error("xmlrename", "unknown option: ", opt.flag);
        tool_write_usage("xmlrename", "SELECTOR NEW-NAME [FILE ...]");
        return 1;
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlrename", "SELECTOR NEW-NAME [FILE ...]");
        return 0;
    }
    if (opt.argi + 1 >= argc) {
        tool_write_usage("xmlrename", "SELECTOR NEW-NAME [FILE ...]");
        return 1;
    }
    selector = argv[opt.argi++];
    new_name = argv[opt.argi++];
    if (opt.argi >= argc) return rename_one(selector, new_name, 0);
    for (i = opt.argi; i < argc; ++i) {
        if (rename_one(selector, new_name, argv[i]) != 0) exit_code = 1;
    }
    return exit_code;
}
