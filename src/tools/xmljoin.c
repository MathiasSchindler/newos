#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


#define XMLJOIN_INITIAL_ITEMS 128

typedef struct {
    const char *start;
    size_t length;
    const char *key;
    size_t key_length;
    size_t sequence;
} JoinItem;

typedef struct {
    JoinItem inline_items[XMLJOIN_INITIAL_ITEMS];
    JoinItem *items;
    size_t count;
    size_t capacity;
} JoinItems;

static void join_items_init(JoinItems *items) {
    rt_memset(items, 0, sizeof(*items));
    items->items = items->inline_items;
    items->capacity = XMLJOIN_INITIAL_ITEMS;
}

static void join_items_free(JoinItems *items) {
    if (items->items != items->inline_items) rt_free(items->items);
}

static int join_items_ensure(JoinItems *items, size_t needed) {
    JoinItem *next_items;
    size_t next_capacity;
    size_t i;
    if (needed <= items->capacity) return 0;
    next_capacity = items->capacity == 0U ? XMLJOIN_INITIAL_ITEMS : items->capacity;
    while (next_capacity < needed) {
        if (next_capacity > (size_t)(~(size_t)0 / 2U)) return -1;
        next_capacity *= 2U;
    }
    next_items = (JoinItem *)rt_malloc_array(next_capacity, sizeof(next_items[0]));
    if (next_items == 0) return -1;
    for (i = 0U; i < items->count; ++i) next_items[i] = items->items[i];
    if (items->items != items->inline_items) rt_free(items->items);
    items->items = next_items;
    items->capacity = next_capacity;
    return 0;
}

static int join_item_compare(const void *left, const void *right) {
    const JoinItem *left_item = (const JoinItem *)left;
    const JoinItem *right_item = (const JoinItem *)right;
    int key_result = tool_compare_text_slices(left_item->key, left_item->key_length, right_item->key, right_item->key_length);
    if (key_result != 0) return key_result;
    if (left_item->sequence < right_item->sequence) return -1;
    if (left_item->sequence > right_item->sequence) return 1;
    return 0;
}

static int key_equals(const JoinItem *item, const char *key, size_t key_length) {
    return item->key_length == key_length && rt_strncmp(item->key, key, key_length) == 0;
}

static int add_item(JoinItems *items, const char *start, size_t length, const char *key, size_t key_length) {
    if (join_items_ensure(items, items->count + 1U) != 0) {
        tool_write_error("xmljoin", "out of memory", 0);
        return 1;
    }
    items->items[items->count].start = start;
    items->items[items->count].length = length;
    items->items[items->count].key = key;
    items->items[items->count].key_length = key_length;
    items->items[items->count].sequence = items->count;
    items->count += 1U;
    return 0;
}

static int collect(const char *selector, const ToolXmlKeySpec *key_spec, const char *path, JoinItems *items, char **input_out) {
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
    if (tool_xml_selector_compile(&compiled_selector, selector, "xmljoin") != 0) {
        xml_name_stack_free(&stack);
        return 1;
    }

    if (xml_read_document(path, &input, &length, "xmljoin") != 0) { xml_selector_free(&compiled_selector); xml_name_stack_free(&stack); return 1; }
    *input_out = input;
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START) {
            if (tool_xml_name_stack_push(&stack, token.name, "xmljoin") != 0) {
                xml_selector_free(&compiled_selector);
                xml_name_stack_free(&stack);
                return 1;
            }
            if (!capturing && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                capturing = 1;
                capture_depth = stack.count;
                capture_start = token.raw;
                tool_xml_key_state_init(&key_state);
                tool_xml_key_start(key_spec, &token, stack.count, capture_depth, &key_state);
            } else if (capturing) {
                tool_xml_key_start(key_spec, &token, stack.count, capture_depth, &key_state);
            }
        } else if (token.type == XML_TOKEN_EMPTY) {
            if (tool_xml_name_stack_push(&stack, token.name, "xmljoin") != 0) {
                xml_selector_free(&compiled_selector);
                xml_name_stack_free(&stack);
                return 1;
            }
            if (!capturing && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                tool_xml_key_state_init(&key_state);
                tool_xml_key_start(key_spec, &token, stack.count, stack.count, &key_state);
                if (add_item(items, token.raw, token.raw_length, key_state.key, key_state.key_length) != 0) { xml_selector_free(&compiled_selector); xml_name_stack_free(&stack); return 1; }
            } else if (capturing) {
                tool_xml_key_start(key_spec, &token, stack.count, capture_depth, &key_state);
            }
            xml_name_stack_pop(&stack);
        } else if (token.type == XML_TOKEN_TEXT || token.type == XML_TOKEN_CDATA) {
            if (capturing) tool_xml_key_text(key_spec, &token, stack.count, capture_depth, &key_state);
        } else if (token.type == XML_TOKEN_END) {
            if (capturing && stack.count == capture_depth) {
                if (add_item(items, capture_start, (size_t)((token.raw + token.raw_length) - capture_start), key_state.key, key_state.key_length) != 0) { xml_selector_free(&compiled_selector); xml_name_stack_free(&stack); return 1; }
                capturing = 0;
            } else if (capturing) {
                tool_xml_key_end(key_spec, stack.count, &key_state);
            }
            xml_name_stack_pop(&stack);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmljoin", path, &parser);
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    xml_selector_free(&compiled_selector);
    xml_name_stack_free(&stack);
    return 0;
}

static const JoinItem *find_key(const JoinItems *items, const char *key, size_t key_length) {
    size_t left = 0U;
    size_t right = items->count;
    while (left < right) {
        size_t middle = left + ((right - left) / 2U);
        if (tool_compare_text_slices(items->items[middle].key, items->items[middle].key_length, key, key_length) < 0) left = middle + 1U;
        else right = middle;
    }
    if (left < items->count && key_equals(&items->items[left], key, key_length)) return &items->items[left];
    return 0;
}

static void write_joined(const JoinItems *left, const JoinItems *right) {
    size_t i;
    rt_write_cstr(1, "<joined>\n");
    for (i = 0U; i < left->count; ++i) {
        const JoinItem *left_item = &left->items[i];
        const JoinItem *match = find_key(right, left_item->key, left_item->key_length);
        if (match != 0) {
            rt_write_cstr(1, "<join key=\"");
            xml_write_escaped_attr(1, left_item->key, left_item->key_length);
            rt_write_cstr(1, "\">");
            xml_write_raw(1, left_item->start, left_item->length);
            xml_write_raw(1, match->start, match->length);
            rt_write_cstr(1, "</join>\n");
        }
    }
    rt_write_cstr(1, "</joined>\n");
}

int main(int argc, char **argv) {
    ToolOptState opt;
    JoinItems left;
    JoinItems right;
    ToolXmlKeySpec key_spec;
    char *left_input = 0;
    char *right_input = 0;
    int option_result;
    const char *selector;
    const char *key;
    int result;

    join_items_init(&left);
    join_items_init(&right);
    tool_opt_init(&opt, argc, argv, "xmljoin", "SELECTOR KEY LEFT.xml RIGHT.xml");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        tool_write_error("xmljoin", "unknown option: ", opt.flag);
        tool_write_usage("xmljoin", "SELECTOR KEY LEFT.xml RIGHT.xml");
        return 1;
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmljoin", "SELECTOR KEY LEFT.xml RIGHT.xml");
        return 0;
    }
    if (opt.argi + 4 != argc) {
        tool_write_usage("xmljoin", "SELECTOR KEY LEFT.xml RIGHT.xml");
        return 1;
    }
    selector = argv[opt.argi++];
    key = argv[opt.argi++];
    if (tool_xml_key_parse(key, &key_spec, "xmljoin") != 0) return 1;
    result = collect(selector, &key_spec, argv[opt.argi], &left, &left_input);
    if (result == 0) result = collect(selector, &key_spec, argv[opt.argi + 1], &right, &right_input);
    if (result == 0) {
        rt_sort(right.items, right.count, sizeof(right.items[0]), join_item_compare);
        write_joined(&left, &right);
    }
    if (left_input != 0) xml_free_document(left_input);
    if (right_input != 0) xml_free_document(right_input);
    join_items_free(&left);
    join_items_free(&right);
    return result;
}
