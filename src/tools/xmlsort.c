#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


#define XMLSORT_INITIAL_ITEMS 128

typedef struct {
    const char *start;
    size_t length;
    const char *key;
    size_t key_length;
    size_t sequence;
} SortItem;

typedef struct {
    SortItem inline_items[XMLSORT_INITIAL_ITEMS];
    SortItem *items;
    size_t count;
    size_t capacity;
} SortItems;

static int sort_reverse = 0;
static int sort_numeric = 0;

static void sort_items_init(SortItems *items) {
    rt_memset(items, 0, sizeof(*items));
    items->items = items->inline_items;
    items->capacity = XMLSORT_INITIAL_ITEMS;
}

static void sort_items_free(SortItems *items) {
    if (items->items != items->inline_items) rt_free(items->items);
}

static int sort_items_ensure(SortItems *items, size_t needed) {
    SortItem *next_items;
    size_t next_capacity;
    size_t i;
    if (needed <= items->capacity) return 0;
    next_capacity = items->capacity == 0U ? XMLSORT_INITIAL_ITEMS : items->capacity;
    while (next_capacity < needed) {
        if (next_capacity > (size_t)(~(size_t)0 / 2U)) return -1;
        next_capacity *= 2U;
    }
    if (next_capacity > (size_t)(~(size_t)0 / sizeof(items->items[0]))) return -1;
    next_items = (SortItem *)rt_malloc(next_capacity * sizeof(next_items[0]));
    if (next_items == 0) return -1;
    for (i = 0U; i < items->count; ++i) next_items[i] = items->items[i];
    if (items->items != items->inline_items) rt_free(items->items);
    items->items = next_items;
    items->capacity = next_capacity;
    return 0;
}

static int key_compare(const char *left, size_t left_length, const char *right, size_t right_length) {
    size_t i;
    size_t shared = left_length < right_length ? left_length : right_length;
    for (i = 0U; i < shared; ++i) {
        if (left[i] < right[i]) return -1;
        if (left[i] > right[i]) return 1;
    }
    if (left_length < right_length) return -1;
    if (left_length > right_length) return 1;
    return 0;
}

static int key_numeric_parts(const char *key, size_t length, int *negative_out, const char **digits_out, size_t *digit_count_out) {
    size_t start = 0U;
    size_t end = length;
    const char *digits;
    size_t digit_count;

    while (start < end && (key[start] == ' ' || key[start] == '\t' || key[start] == '\n' || key[start] == '\r')) start += 1U;
    while (end > start && (key[end - 1U] == ' ' || key[end - 1U] == '\t' || key[end - 1U] == '\n' || key[end - 1U] == '\r')) end -= 1U;
    *negative_out = 0;
    if (start < end && (key[start] == '-' || key[start] == '+')) {
        *negative_out = key[start] == '-';
        start += 1U;
    }
    if (start >= end || key[start] < '0' || key[start] > '9') return 0;
    while (start < end && key[start] == '0') start += 1U;
    digits = key + start;
    while (start < end && key[start] >= '0' && key[start] <= '9') start += 1U;
    if (start != end) return 0;
    digit_count = (size_t)(key + start - digits);
    if (digit_count == 0U) {
        *negative_out = 0;
        digits = "0";
        digit_count = 1U;
    }
    *digits_out = digits;
    *digit_count_out = digit_count;
    return 1;
}

static int key_compare_numeric(const char *left, size_t left_length, const char *right, size_t right_length) {
    int left_negative = 0;
    int right_negative = 0;
    const char *left_digits = 0;
    const char *right_digits = 0;
    size_t left_digit_count = 0U;
    size_t right_digit_count = 0U;
    int left_numeric = key_numeric_parts(left, left_length, &left_negative, &left_digits, &left_digit_count);
    int right_numeric = key_numeric_parts(right, right_length, &right_negative, &right_digits, &right_digit_count);
    int text_result;

    if (!left_numeric || !right_numeric) {
        if (left_numeric != right_numeric) return left_numeric ? -1 : 1;
        return key_compare(left, left_length, right, right_length);
    }
    if (left_negative != right_negative) return left_negative ? -1 : 1;
    if (left_digit_count != right_digit_count) {
        int result = left_digit_count < right_digit_count ? -1 : 1;
        return left_negative ? -result : result;
    }
    text_result = key_compare(left_digits, left_digit_count, right_digits, right_digit_count);
    return left_negative ? -text_result : text_result;
}

static int add_item(SortItems *items, const char *start, size_t length, const char *key, size_t key_length) {
    if (sort_items_ensure(items, items->count + 1U) != 0) {
        tool_write_error("xmlsort", "out of memory", 0);
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

static int collect_one(const char *selector, const ToolXmlKeySpec *key_spec, const char *path, SortItems *items) {
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
    if (xml_selector_compile(&compiled_selector, selector) != 0) {
        xml_name_stack_free(&stack);
        return 1;
    }

    if (xml_read_document(path, &input, &length, "xmlsort") != 0) { xml_selector_free(&compiled_selector); xml_name_stack_free(&stack); return 1; }
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START) {
            TOOL_XML_NAME_STACK_PUSH_OR_RETURN(&stack, token.name, "xmlsort", xml_free_document(input); xml_selector_free(&compiled_selector); xml_name_stack_free(&stack);)
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
            TOOL_XML_NAME_STACK_PUSH_OR_RETURN(&stack, token.name, "xmlsort", xml_free_document(input); xml_selector_free(&compiled_selector); xml_name_stack_free(&stack);)
            if (!capturing && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                tool_xml_key_state_init(&key_state);
                tool_xml_key_start(key_spec, &token, stack.count, stack.count, &key_state);
                if (add_item(items, token.raw, token.raw_length, key_state.key, key_state.key_length) != 0) {
                    xml_free_document(input);
                    xml_selector_free(&compiled_selector);
                    xml_name_stack_free(&stack);
                    return 1;
                }
            } else if (capturing) {
                tool_xml_key_start(key_spec, &token, stack.count, capture_depth, &key_state);
            }
            xml_name_stack_pop(&stack);
        } else if (token.type == XML_TOKEN_TEXT || token.type == XML_TOKEN_CDATA) {
            if (capturing) tool_xml_key_text(key_spec, &token, stack.count, capture_depth, &key_state);
        } else if (token.type == XML_TOKEN_END) {
            if (capturing && stack.count == capture_depth) {
                if (add_item(items, capture_start, (size_t)((token.raw + token.raw_length) - capture_start), key_state.key, key_state.key_length) != 0) {
                    xml_free_document(input);
                    xml_selector_free(&compiled_selector);
                    xml_name_stack_free(&stack);
                    return 1;
                }
                capturing = 0;
            } else if (capturing) {
                tool_xml_key_end(key_spec, stack.count, &key_state);
            }
            xml_name_stack_pop(&stack);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlsort", path, &parser);
        xml_free_document(input);
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    xml_selector_free(&compiled_selector);
    xml_name_stack_free(&stack);
    return 0;
}

static int sort_item_compare(const void *left, const void *right) {
    const SortItem *left_item = (const SortItem *)left;
    const SortItem *right_item = (const SortItem *)right;
    int key_result = sort_numeric ?
        key_compare_numeric(left_item->key, left_item->key_length, right_item->key, right_item->key_length) :
        key_compare(left_item->key, left_item->key_length, right_item->key, right_item->key_length);
    if (sort_reverse) key_result = -key_result;
    if (key_result != 0) return key_result;
    if (left_item->sequence < right_item->sequence) return -1;
    if (left_item->sequence > right_item->sequence) return 1;
    return 0;
}

static void sort_selected_items(SortItems *items) {
    rt_sort(items->items, items->count, sizeof(items->items[0]), sort_item_compare);
}

int main(int argc, char **argv) {
    ToolOptState opt;
    SortItems items;
    ToolXmlKeySpec key_spec;
    int option_result;
    const char *selector;
    const char *key;
    int exit_code = 0;
    int i;

    sort_items_init(&items);
    tool_opt_init(&opt, argc, argv, "xmlsort", "[-n] [-r] SELECTOR KEY [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-n") == 0 || rt_strcmp(opt.flag, "--numeric") == 0) {
            sort_numeric = 1;
        } else if (rt_strcmp(opt.flag, "-r") == 0 || rt_strcmp(opt.flag, "--reverse") == 0) {
            sort_reverse = 1;
        } else {
            tool_write_error("xmlsort", "unknown option: ", opt.flag);
            tool_write_usage("xmlsort", "[-n] [-r] SELECTOR KEY [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlsort", "[-n] [-r] SELECTOR KEY [FILE ...]");
        return 0;
    }
    if (opt.argi + 1 >= argc) {
        tool_write_usage("xmlsort", "[-n] [-r] SELECTOR KEY [FILE ...]");
        return 1;
    }
    selector = argv[opt.argi++];
    key = argv[opt.argi++];
    if (tool_xml_key_parse(key, &key_spec, "xmlsort") != 0) return 1;
    if (opt.argi >= argc) exit_code = collect_one(selector, &key_spec, 0, &items);
    else {
        for (i = opt.argi; i < argc; ++i) {
            if (collect_one(selector, &key_spec, argv[i], &items) != 0) exit_code = 1;
        }
    }
    if (exit_code == 0) {
        sort_selected_items(&items);
        for (i = 0; i < (int)items.count; ++i) {
            xml_write_raw(1, items.items[i].start, items.items[i].length);
            rt_write_char(1, '\n');
        }
    }
    sort_items_free(&items);
    return exit_code;
}
