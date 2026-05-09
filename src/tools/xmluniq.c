#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


#define XMLUNIQ_INITIAL_KEYS 128

typedef struct {
    char *text;
    size_t length;
} Key;

typedef struct {
    Key inline_keys[XMLUNIQ_INITIAL_KEYS];
    Key *keys;
    size_t count;
    size_t capacity;
} KeySet;

static void key_set_init(KeySet *set) {
    rt_memset(set, 0, sizeof(*set));
    set->keys = set->inline_keys;
    set->capacity = XMLUNIQ_INITIAL_KEYS;
}

static void key_set_free(KeySet *set) {
    size_t i;
    for (i = 0U; i < set->count; ++i) rt_free(set->keys[i].text);
    if (set->keys != set->inline_keys) rt_free(set->keys);
}

static int key_set_ensure(KeySet *set, size_t needed) {
    Key *keys;
    size_t next_capacity;
    size_t i;
    if (needed <= set->capacity) return 0;
    next_capacity = set->capacity == 0U ? XMLUNIQ_INITIAL_KEYS : set->capacity;
    while (next_capacity < needed) {
        if (next_capacity > (size_t)(~(size_t)0 / 2U)) return -1;
        next_capacity *= 2U;
    }
    keys = (Key *)rt_malloc(next_capacity * sizeof(*keys));
    if (keys == 0) return -1;
    for (i = 0U; i < set->count; ++i) keys[i] = set->keys[i];
    if (set->keys != set->inline_keys) rt_free(set->keys);
    set->keys = keys;
    set->capacity = next_capacity;
    return 0;
}

static int seen_or_add(KeySet *set, const char *key, size_t key_length) {
    size_t i;
    for (i = 0U; i < set->count; ++i) {
        if (set->keys[i].length == key_length && rt_strncmp(set->keys[i].text, key, key_length) == 0) return 1;
    }
    if (key_set_ensure(set, set->count + 1U) != 0) return -1;
    set->keys[set->count].text = xml_slice_dup(key, key_length);
    if (set->keys[set->count].text == 0) return -1;
    set->keys[set->count].length = key_length;
    set->count += 1U;
    return 0;
}

static int xmluniq_seen_or_report(KeySet *seen, const char *key, size_t key_length, int *duplicate_out) {
    int duplicate = seen_or_add(seen, key, key_length);
    if (duplicate < 0) {
        tool_write_error("xmluniq", "out of memory", 0);
        return 1;
    }
    *duplicate_out = duplicate;
    return 0;
}

static int uniq_one(const char *selector, const ToolXmlKeySpec *key_spec, const char *path, KeySet *seen) {
    XmlParser parser;
    XmlToken token;
    XmlNameStack stack;
    XmlSelector compiled_selector;
    char *input;
    const char *capture_start = 0;
    ToolXmlKeyState key_state;
    unsigned int capture_depth = 0U;
    int capturing = 0;
    size_t length;
    int result;

    xml_name_stack_init(&stack);
    if (tool_xml_selector_compile(&compiled_selector, selector, "xmluniq") != 0) {
        xml_name_stack_free(&stack);
        return 1;
    }

    if (xml_read_document(path, &input, &length, "xmluniq") != 0) { xml_selector_free(&compiled_selector); xml_name_stack_free(&stack); return 1; }
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) {
            if (tool_xml_name_stack_push(&stack, token.name, "xmluniq") != 0) {
                xml_free_document(input);
                xml_selector_free(&compiled_selector);
                xml_name_stack_free(&stack);
                return 1;
            }
            if (!capturing && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                tool_xml_key_state_init(&key_state);
                tool_xml_key_start(key_spec, &token, stack.count, stack.count, &key_state);
                if (token.type == XML_TOKEN_EMPTY) {
                    int duplicate;
                    if (xmluniq_seen_or_report(seen, key_state.key, key_state.key_length, &duplicate) != 0) {
                        xml_free_document(input);
                        xml_selector_free(&compiled_selector);
                        xml_name_stack_free(&stack);
                        return 1;
                    }
                    if (!duplicate) xml_write_raw(1, token.raw, token.raw_length);
                    xml_name_stack_pop(&stack);
                    continue;
                }
                capturing = 1;
                capture_depth = stack.count;
                capture_start = token.raw;
                continue;
            }
            if (capturing) {
                tool_xml_key_start(key_spec, &token, stack.count, capture_depth, &key_state);
                if (token.type == XML_TOKEN_EMPTY) xml_name_stack_pop(&stack);
                continue;
            }
            xml_write_raw(1, token.raw, token.raw_length);
            if (token.type == XML_TOKEN_EMPTY) xml_name_stack_pop(&stack);
        } else if (token.type == XML_TOKEN_TEXT || token.type == XML_TOKEN_CDATA) {
            if (capturing) {
                tool_xml_key_text(key_spec, &token, stack.count, capture_depth, &key_state);
            } else {
                xml_write_raw(1, token.raw, token.raw_length);
            }
        } else if (token.type == XML_TOKEN_END) {
            if (capturing && stack.count == capture_depth) {
                int duplicate;
                if (xmluniq_seen_or_report(seen, key_state.key, key_state.key_length, &duplicate) != 0) {
                    xml_free_document(input);
                    xml_selector_free(&compiled_selector);
                    xml_name_stack_free(&stack);
                    return 1;
                }
                if (!duplicate) xml_write_raw(1, capture_start, (size_t)((token.raw + token.raw_length) - capture_start));
                capturing = 0;
                xml_name_stack_pop(&stack);
                continue;
            }
            if (capturing) {
                tool_xml_key_end(key_spec, stack.count, &key_state);
            } else {
                xml_write_raw(1, token.raw, token.raw_length);
            }
            xml_name_stack_pop(&stack);
        } else if (!capturing) xml_write_raw(1, token.raw, token.raw_length);
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmluniq", path, &parser);
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
    KeySet seen;
    ToolXmlKeySpec key_spec;
    int option_result;
    const char *selector;
    const char *key;
    int exit_code = 0;
    int i;

    key_set_init(&seen);
    tool_opt_init(&opt, argc, argv, "xmluniq", "SELECTOR KEY [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        tool_write_error("xmluniq", "unknown option: ", opt.flag);
        tool_write_usage("xmluniq", "SELECTOR KEY [FILE ...]");
        return 1;
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmluniq", "SELECTOR KEY [FILE ...]");
        return 0;
    }
    if (opt.argi + 1 >= argc) {
        tool_write_usage("xmluniq", "SELECTOR KEY [FILE ...]");
        return 1;
    }
    selector = argv[opt.argi++];
    key = argv[opt.argi++];
    if (tool_xml_key_parse(key, &key_spec, "xmluniq") != 0) return 1;
    if (opt.argi >= argc) {
        exit_code = uniq_one(selector, &key_spec, 0, &seen);
        key_set_free(&seen);
        return exit_code;
    }
    for (i = opt.argi; i < argc; ++i) {
        if (uniq_one(selector, &key_spec, argv[i], &seen) != 0) exit_code = 1;
    }
    key_set_free(&seen);
    return exit_code;
}
