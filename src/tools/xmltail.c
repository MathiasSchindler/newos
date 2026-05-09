#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


#define XMLTAIL_INITIAL_ITEMS 64

typedef struct {
    const char *start;
    size_t length;
} TailItem;

typedef struct {
    TailItem inline_items[XMLTAIL_INITIAL_ITEMS];
    TailItem *items;
    size_t limit;
    size_t count;
} TailBuffer;

static int tail_buffer_init(TailBuffer *buffer, size_t limit) {
    rt_memset(buffer, 0, sizeof(*buffer));
    buffer->limit = limit;
    if (limit <= XMLTAIL_INITIAL_ITEMS) {
        buffer->items = buffer->inline_items;
        return 0;
    }
    if (limit > (size_t)(~(size_t)0 / sizeof(buffer->items[0]))) return -1;
    buffer->items = (TailItem *)rt_malloc(limit * sizeof(buffer->items[0]));
    if (buffer->items == 0) return -1;
    return 0;
}

static void tail_buffer_free(TailBuffer *buffer) {
    if (buffer->items != buffer->inline_items) rt_free(buffer->items);
}

static void add_tail(TailBuffer *buffer, const char *start, size_t length) {
    size_t slot;
    if (buffer->limit == 0U) return;
    slot = buffer->count % buffer->limit;
    buffer->items[slot].start = start;
    buffer->items[slot].length = length;
    buffer->count += 1U;
}

static int tail_one(const char *selector, TailBuffer *buffer, const char *path) {
    XmlParser parser;
    XmlToken token;
    XmlNameStack stack;
    XmlSelector compiled_selector;
    char *input;
    const char *capture_start = 0;
    unsigned int capture_depth = 0U;
    int capturing = 0;
    size_t length;
    int result;

    xml_name_stack_init(&stack);
    if (tool_xml_selector_compile(&compiled_selector, selector, "xmltail") != 0) { xml_name_stack_free(&stack); return 1; }

    if (xml_read_document(path, &input, &length, "xmltail") != 0) { xml_selector_free(&compiled_selector); xml_name_stack_free(&stack); return 1; }
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START) {
            TOOL_XML_NAME_STACK_PUSH_OR_RETURN(&stack, token.name, "xmltail", xml_free_document(input); xml_selector_free(&compiled_selector); xml_name_stack_free(&stack);)
            if (!capturing && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                capturing = 1;
                capture_depth = stack.count;
                capture_start = token.raw;
            }
        } else if (token.type == XML_TOKEN_EMPTY) {
            TOOL_XML_NAME_STACK_PUSH_OR_RETURN(&stack, token.name, "xmltail", xml_free_document(input); xml_selector_free(&compiled_selector); xml_name_stack_free(&stack);)
            if (!capturing && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) add_tail(buffer, token.raw, token.raw_length);
            xml_name_stack_pop(&stack);
        } else if (token.type == XML_TOKEN_END) {
            if (capturing && stack.count == capture_depth) {
                add_tail(buffer, capture_start, (size_t)((token.raw + token.raw_length) - capture_start));
                capturing = 0;
            }
            xml_name_stack_pop(&stack);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmltail", path, &parser);
        xml_free_document(input);
        xml_selector_free(&compiled_selector);
        xml_name_stack_free(&stack);
        return 1;
    }
    xml_selector_free(&compiled_selector);
    xml_name_stack_free(&stack);
    return 0;
}

static void write_tail(const TailBuffer *buffer) {
    size_t available = buffer->count < buffer->limit ? buffer->count : buffer->limit;
    size_t start = buffer->limit > 0U && buffer->count > buffer->limit ? buffer->count % buffer->limit : 0U;
    size_t i;
    for (i = 0U; i < available; ++i) {
        size_t index = (start + i) % buffer->limit;
        xml_write_raw(1, buffer->items[index].start, buffer->items[index].length);
        rt_write_char(1, '\n');
    }
}

int main(int argc, char **argv) {
    ToolOptState opt;
    TailBuffer buffer;
    int option_result;
    size_t limit = 10U;
    const char *selector;
    const char *wrap_name = 0;
    int exit_code = 0;
    int i;

    tool_opt_init(&opt, argc, argv, "xmltail", "[-n COUNT] [--wrap NAME] SELECTOR [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-n") == 0 || rt_strcmp(opt.flag, "--count") == 0) {
            unsigned long long parsed;
            if (tool_opt_require_value(&opt) != 0 || tool_parse_uint_arg(opt.value, &parsed, "xmltail", "count") != 0) return 1;
            limit = (size_t)parsed;
        } else if (rt_strcmp(opt.flag, "--wrap") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            wrap_name = opt.value;
        } else {
            tool_write_error("xmltail", "unknown option: ", opt.flag);
            tool_write_usage("xmltail", "[-n COUNT] [--wrap NAME] SELECTOR [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmltail", "[-n COUNT] [--wrap NAME] SELECTOR [FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) {
        tool_write_usage("xmltail", "[-n COUNT] [--wrap NAME] SELECTOR [FILE ...]");
        return 1;
    }
    if (wrap_name != 0 && !xml_is_name(wrap_name)) {
        tool_write_error("xmltail", "invalid wrapper name: ", wrap_name);
        return 1;
    }
    selector = argv[opt.argi++];
    if (tail_buffer_init(&buffer, limit) != 0) {
        tool_write_error("xmltail", "out of memory", 0);
        return 1;
    }
    if (opt.argi >= argc) exit_code = tail_one(selector, &buffer, 0);
    else {
        for (i = opt.argi; i < argc; ++i) {
            if (tail_one(selector, &buffer, argv[i]) != 0) exit_code = 1;
        }
    }
    if (exit_code == 0) {
        if (wrap_name != 0) { rt_write_char(1, '<'); rt_write_cstr(1, wrap_name); rt_write_cstr(1, ">\n"); }
        write_tail(&buffer);
        if (wrap_name != 0) { rt_write_cstr(1, "</"); rt_write_cstr(1, wrap_name); rt_write_cstr(1, ">\n"); }
    }
    tail_buffer_free(&buffer);
    return exit_code;
}
