#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


typedef struct {
    const char *value;
    size_t value_length;
} XmlQueryTextPredicate;

typedef struct {
    XmlQueryTextPredicate inline_items[4];
    XmlQueryTextPredicate *items;
    unsigned int count;
    unsigned int capacity;
} XmlQueryTextPredicates;

typedef struct {
    char inline_text[256];
    char *text;
    size_t length;
    size_t capacity;
} XmlQueryTextCapture;

static void text_predicates_init(XmlQueryTextPredicates *predicates) {
    predicates->items = predicates->inline_items;
    predicates->count = 0U;
    predicates->capacity = 4U;
}

static void text_predicates_free(XmlQueryTextPredicates *predicates) {
    if (predicates->items != predicates->inline_items) rt_free(predicates->items);
    predicates->items = predicates->inline_items;
    predicates->count = 0U;
    predicates->capacity = 4U;
}

static int text_predicates_add(XmlQueryTextPredicates *predicates, const char *value, size_t value_length) {
    XmlQueryTextPredicate *resized;
    unsigned int next_capacity;
    if (predicates->count == predicates->capacity) {
        next_capacity = predicates->capacity * 2U;
        resized = (XmlQueryTextPredicate *)rt_malloc((size_t)next_capacity * sizeof(*resized));
        if (resized == 0) return -1;
        memcpy(resized, predicates->items, (size_t)predicates->count * sizeof(*resized));
        if (predicates->items != predicates->inline_items) rt_free(predicates->items);
        predicates->items = resized;
        predicates->capacity = next_capacity;
    }
    predicates->items[predicates->count].value = value;
    predicates->items[predicates->count].value_length = value_length;
    predicates->count += 1U;
    return 0;
}

static void text_capture_init(XmlQueryTextCapture *capture) {
    capture->text = capture->inline_text;
    capture->length = 0U;
    capture->capacity = sizeof(capture->inline_text);
}

static void text_capture_reset(XmlQueryTextCapture *capture) {
    capture->length = 0U;
}

static void text_capture_free(XmlQueryTextCapture *capture) {
    if (capture->text != capture->inline_text) rt_free(capture->text);
    capture->text = capture->inline_text;
    capture->length = 0U;
    capture->capacity = sizeof(capture->inline_text);
}

static int text_capture_append(XmlQueryTextCapture *capture, const char *text, size_t length) {
    char *resized;
    size_t next_capacity;
    if (length == 0U) return 0;
    if (capture->length + length < capture->length) return -1;
    if (capture->length + length > capture->capacity) {
        next_capacity = capture->capacity;
        while (next_capacity < capture->length + length) {
            if (next_capacity > (size_t)(~(size_t)0 / 2U)) return -1;
            next_capacity *= 2U;
        }
        resized = (char *)rt_malloc(next_capacity);
        if (resized == 0) return -1;
        if (capture->length > 0U) memcpy(resized, capture->text, capture->length);
        if (capture->text != capture->inline_text) rt_free(capture->text);
        capture->text = resized;
        capture->capacity = next_capacity;
    }
    memcpy(capture->text + capture->length, text, length);
    capture->length += length;
    return 0;
}

static int text_predicates_match(const XmlQueryTextPredicates *predicates, const XmlQueryTextCapture *capture) {
    unsigned int i;
    for (i = 0U; i < predicates->count; ++i) {
        if (capture->length != predicates->items[i].value_length) return 0;
        if (capture->length > 0U && rt_strncmp(capture->text, predicates->items[i].value, capture->length) != 0) return 0;
    }
    return 1;
}

static int find_final_component_start(const char *selector, size_t *start_out) {
    size_t i = 0U;
    size_t start = 0U;
    int bracket_depth = 0;
    char quote = '\0';
    while (selector[i] != '\0') {
        char ch = selector[i];
        if (quote != '\0') {
            if (ch == quote) quote = '\0';
        } else if (ch == '\'' || ch == '"') quote = ch;
        else if (ch == '[') bracket_depth += 1;
        else if (ch == ']') {
            if (bracket_depth > 0) bracket_depth -= 1;
        } else if (ch == '/' && bracket_depth == 0) start = i + 1U;
        i += 1U;
    }
    *start_out = start;
    return quote == '\0' && bracket_depth == 0 ? 0 : -1;
}

static int parse_text_predicate(const char *text, size_t start, size_t end, const char **value_out, size_t *value_length_out) {
    size_t value_start;
    size_t value_end;
    if (end <= start + 2U) return 0;
    if (text[start] == '.' && text[start + 1U] == '=') value_start = start + 2U;
    else if (end > start + 7U && rt_strncmp(text + start, "text()=", 7U) == 0) value_start = start + 7U;
    else return 0;
    if (value_start >= end) return -1;
    if (text[value_start] == '\'' || text[value_start] == '"') {
        char quote = text[value_start];
        value_start += 1U;
        value_end = value_start;
        while (value_end < end && text[value_end] != quote) value_end += 1U;
        if (value_end >= end || value_end + 1U != end) return -1;
    } else {
        value_end = end;
    }
    *value_out = text + value_start;
    *value_length_out = value_end - value_start;
    return 1;
}

static int find_predicate_end(const char *text, size_t length, size_t start, size_t *end_out) {
    size_t pos = start;
    char quote = '\0';
    while (pos < length) {
        if (quote != '\0') {
            if (text[pos] == quote) quote = '\0';
        } else if (text[pos] == '\'' || text[pos] == '"') quote = text[pos];
        else if (text[pos] == ']') {
            *end_out = pos;
            return 0;
        }
        pos += 1U;
    }
    return -1;
}

static int prepare_selector(const char *selector, char **base_selector_out, XmlQueryTextPredicates *text_predicates) {
    char *base_selector;
    size_t selector_length;
    size_t final_start;
    size_t read_pos;
    size_t write_pos;
    int saw_text_predicate = 0;

    if (find_final_component_start(selector, &final_start) != 0) return -1;
    selector_length = rt_strlen(selector);
    base_selector = (char *)rt_malloc(selector_length + 1U);
    if (base_selector == 0) return -1;
    memcpy(base_selector, selector, final_start);
    write_pos = final_start;
    read_pos = final_start;
    while (read_pos < selector_length && selector[read_pos] != '[') {
        base_selector[write_pos++] = selector[read_pos++];
    }
    while (read_pos < selector_length) {
        size_t predicate_start;
        size_t predicate_end;
        const char *value = 0;
        size_t value_length = 0U;
        int predicate_result;
        if (selector[read_pos] != '[') { rt_free(base_selector); return -1; }
        predicate_start = read_pos + 1U;
        if (find_predicate_end(selector, selector_length, predicate_start, &predicate_end) != 0) { rt_free(base_selector); return -1; }
        predicate_result = parse_text_predicate(selector, predicate_start, predicate_end, &value, &value_length);
        if (predicate_result < 0) { rt_free(base_selector); return -1; }
        if (predicate_result > 0) {
            if (text_predicates_add(text_predicates, value, value_length) != 0) { rt_free(base_selector); return -1; }
            saw_text_predicate = 1;
        } else {
            size_t i;
            for (i = read_pos; i <= predicate_end; ++i) base_selector[write_pos++] = selector[i];
        }
        read_pos = predicate_end + 1U;
    }
    base_selector[write_pos] = '\0';
    if (!saw_text_predicate) {
        rt_free(base_selector);
        base_selector = xml_slice_dup(selector, selector_length);
        if (base_selector == 0) return -1;
    }
    *base_selector_out = base_selector;
    return 0;
}

static int query_one(const char *selector, const XmlQueryTextPredicates *text_predicates, const char *path) {
    XmlParser parser;
    XmlToken token;
    XmlNameStack stack;
    XmlSelector compiled_selector;
    ToolOutputBuffer output;
    XmlQueryTextCapture text_capture;
    const char *candidate_start = 0;
    char *input;
    unsigned int copy_depth = 0U;
    int copying = 0;
    size_t length;
    int result;

    xml_name_stack_init(&stack);
    text_capture_init(&text_capture);
    if (tool_xml_selector_compile(&compiled_selector, selector, "xmlquery") != 0) {
        text_capture_free(&text_capture);
        xml_name_stack_free(&stack);
        return 1;
    }

    if (xml_read_document(path, &input, &length, "xmlquery") != 0) { text_capture_free(&text_capture); xml_selector_free(&compiled_selector); xml_name_stack_free(&stack); return 1; }
    tool_output_buffer_init(&output, 1);
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START) {
            if (copying && text_predicates->count == 0U) tool_output_buffer_write(&output, token.raw, token.raw_length);
            if (tool_xml_name_stack_push(&stack, token.name, "xmlquery") != 0) {
                text_capture_free(&text_capture);
                xml_free_document(input);
                xml_selector_free(&compiled_selector);
                xml_name_stack_free(&stack);
                return 1;
            }
            if (!copying && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                copying = 1;
                copy_depth = stack.count;
                if (text_predicates->count == 0U) tool_output_buffer_write(&output, token.raw, token.raw_length);
                else { candidate_start = token.raw; text_capture_reset(&text_capture); }
            }
        } else if (token.type == XML_TOKEN_EMPTY) {
            if (tool_xml_name_stack_push(&stack, token.name, "xmlquery") != 0) {
                text_capture_free(&text_capture);
                xml_free_document(input);
                xml_selector_free(&compiled_selector);
                xml_name_stack_free(&stack);
                return 1;
            }
            if (copying) {
                if (text_predicates->count == 0U) tool_output_buffer_write(&output, token.raw, token.raw_length);
            } else if (xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                if (text_predicates->count > 0U) text_capture_reset(&text_capture);
                if (text_predicates->count == 0U || text_predicates_match(text_predicates, &text_capture)) {
                    tool_output_buffer_write(&output, token.raw, token.raw_length);
                    tool_output_buffer_write_char(&output, '\n');
                }
            }
            xml_name_stack_pop(&stack);
        } else if (token.type == XML_TOKEN_END) {
            if (copying) {
                if (text_predicates->count == 0U) tool_output_buffer_write(&output, token.raw, token.raw_length);
                if (stack.count == copy_depth) {
                    if (text_predicates->count == 0U) tool_output_buffer_write_char(&output, '\n');
                    else if (text_predicates_match(text_predicates, &text_capture)) {
                        tool_output_buffer_write(&output, candidate_start, (size_t)((token.raw + token.raw_length) - candidate_start));
                        tool_output_buffer_write_char(&output, '\n');
                    }
                    copying = 0;
                    candidate_start = 0;
                }
            }
            xml_name_stack_pop(&stack);
        } else if (copying) {
            if (text_predicates->count == 0U) tool_output_buffer_write(&output, token.raw, token.raw_length);
            else if (token.type == XML_TOKEN_TEXT || token.type == XML_TOKEN_CDATA) {
                if (text_capture_append(&text_capture, token.text, token.text_length) != 0) {
                    tool_write_error("xmlquery", "out of memory", 0);
                    xml_free_document(input);
                    text_capture_free(&text_capture);
                    xml_selector_free(&compiled_selector);
                    xml_name_stack_free(&stack);
                    return 1;
                }
            }
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlquery", path, &parser);
        xml_free_document(input);
        text_capture_free(&text_capture);
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    if (tool_output_buffer_flush(&output) != 0) {
        tool_write_error("xmlquery", "write failed", 0);
        xml_free_document(input);
        text_capture_free(&text_capture);
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    xml_free_document(input);
    text_capture_free(&text_capture);
    xml_selector_free(&compiled_selector);
    xml_name_stack_free(&stack);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    const char *selector;
    char *base_selector;
    XmlQueryTextPredicates text_predicates;
    const char *wrap_name = 0;
    int option_result;
    int exit_code = 0;
    int i;

    tool_opt_init(&opt, argc, argv, "xmlquery", "[--wrap NAME] SELECTOR [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--wrap") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            wrap_name = opt.value;
        } else {
            tool_write_error("xmlquery", "unknown option: ", opt.flag);
            tool_write_usage("xmlquery", "[--wrap NAME] SELECTOR [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlquery", "[--wrap NAME] SELECTOR [FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) {
        tool_write_usage("xmlquery", "[--wrap NAME] SELECTOR [FILE ...]");
        return 1;
    }
    if (wrap_name != 0 && !xml_is_name(wrap_name)) {
        tool_write_error("xmlquery", "invalid wrapper name: ", wrap_name);
        return 1;
    }
    selector = argv[opt.argi];
    text_predicates_init(&text_predicates);
    if (prepare_selector(selector, &base_selector, &text_predicates) != 0) {
        text_predicates_free(&text_predicates);
        tool_write_error("xmlquery", "invalid selector: ", selector);
        return 1;
    }
    opt.argi += 1;
    if (wrap_name != 0) { rt_write_char(1, '<'); rt_write_cstr(1, wrap_name); rt_write_cstr(1, ">\n"); }
    if (opt.argi >= argc) {
        exit_code = query_one(base_selector, &text_predicates, 0);
    } else {
        for (i = opt.argi; i < argc; ++i) {
            if (query_one(base_selector, &text_predicates, argv[i]) != 0) exit_code = 1;
        }
    }
    if (wrap_name != 0) { rt_write_cstr(1, "</"); rt_write_cstr(1, wrap_name); rt_write_cstr(1, ">\n"); }
    rt_free(base_selector);
    text_predicates_free(&text_predicates);
    return exit_code;
}
