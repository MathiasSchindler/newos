#include "runtime.h"
#include "tool_util.h"
#include "xml.h"

typedef struct {
    int force;
    unsigned long long max_count;
} XmlSplitOptions;

static int file_exists(const char *path) {
    int fd = platform_open_read(path);
    if (fd < 0) return 0;
    platform_close(fd);
    return 1;
}

static int make_part_path(char *path, size_t path_size, const char *prefix, unsigned long long index) {
    char digits[32];
    size_t prefix_length = rt_strlen(prefix);
    size_t digit_length;
    size_t zero_count;
    size_t pos = 0U;
    size_t i;
    rt_unsigned_to_string(index, digits, sizeof(digits));
    digit_length = rt_strlen(digits);
    zero_count = digit_length < 6U ? 6U - digit_length : 0U;
    if (prefix_length + zero_count + digit_length + 4U >= path_size) return -1;
    memcpy(path + pos, prefix, prefix_length);
    pos += prefix_length;
    for (i = 0U; i < zero_count; ++i) path[pos++] = '0';
    memcpy(path + pos, digits, digit_length);
    pos += digit_length;
    memcpy(path + pos, ".xml", 5U);
    return 0;
}

static int write_part(const char *prefix, unsigned long long index, const char *start, size_t length, const XmlSplitOptions *options) {
    char path[512];
    int fd;
    if (make_part_path(path, sizeof(path), prefix, index) != 0) {
        tool_write_error("xmlsplit", "output path too long", 0);
        return 1;
    }
    if (!options->force && file_exists(path)) {
        tool_write_error("xmlsplit", "output exists; use --force: ", path);
        return 1;
    }
    fd = options->force ? platform_open_write_mode(path, 0644U, 1) : platform_open_create_exclusive(path, 0644U);
    if (fd < 0) {
        tool_write_error("xmlsplit", "cannot create output: ", path);
        return 1;
    }
    if (rt_write_all(fd, start, length) != 0) {
        platform_close(fd);
        tool_write_error("xmlsplit", "write failed: ", path);
        return 1;
    }
    if (platform_close(fd) != 0) {
        tool_write_error("xmlsplit", "close failed: ", path);
        return 1;
    }
    return 0;
}

static int split_one(const char *selector, const char *prefix, const char *path, const XmlSplitOptions *options) {
    XmlParser parser;
    XmlToken token;
    XmlNameStack stack;
    XmlSelector compiled_selector;
    char *input;
    const char *capture_start = 0;
    unsigned int capture_depth = 0U;
    unsigned long long index = 0ULL;
    int capturing = 0;
    size_t length;
    int result;

    xml_name_stack_init(&stack);
    if (tool_xml_selector_compile(&compiled_selector, selector, "xmlsplit") != 0) { xml_name_stack_free(&stack); return 1; }

    if (xml_read_document(path, &input, &length, "xmlsplit") != 0) { xml_selector_free(&compiled_selector); xml_name_stack_free(&stack); return 1; }
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START) {
            if (tool_xml_name_stack_push(&stack, token.name, "xmlsplit") != 0) {
                xml_free_document(input);
                xml_selector_free(&compiled_selector);
                xml_name_stack_free(&stack);
                return 1;
            }
            if (!capturing && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                if (options->max_count == 0ULL || index < options->max_count) {
                    capturing = 1;
                    capture_depth = stack.count;
                    capture_start = token.raw;
                }
            }
        } else if (token.type == XML_TOKEN_EMPTY) {
            if (tool_xml_name_stack_push(&stack, token.name, "xmlsplit") != 0) {
                xml_free_document(input);
                xml_selector_free(&compiled_selector);
                xml_name_stack_free(&stack);
                return 1;
            }
            if (!capturing && xml_name_stack_matches_token(&stack, &token, &compiled_selector)) {
                if (options->max_count > 0ULL && index >= options->max_count) {
                    xml_name_stack_pop(&stack);
                    continue;
                }
                index += 1ULL;
                if (write_part(prefix, index, token.raw, token.raw_length, options) != 0) {
                    xml_free_document(input);
                    xml_selector_free(&compiled_selector);
                    xml_name_stack_free(&stack);
                    return 1;
                }
            }
            xml_name_stack_pop(&stack);
        } else if (token.type == XML_TOKEN_END) {
            if (capturing && stack.count == capture_depth) {
                index += 1ULL;
                if (write_part(prefix, index, capture_start, (size_t)((token.raw + token.raw_length) - capture_start), options) != 0) {
                    xml_free_document(input);
                    xml_selector_free(&compiled_selector);
                    xml_name_stack_free(&stack);
                    return 1;
                }
                capturing = 0;
            }
            xml_name_stack_pop(&stack);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlsplit", path, &parser);
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
    XmlSplitOptions options;
    int option_result;
    const char *selector;
    const char *prefix;
    const char *path = 0;

    options.force = 0;
    options.max_count = 0ULL;

    tool_opt_init(&opt, argc, argv, "xmlsplit", "[--force] [--max N] SELECTOR PREFIX [FILE]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--force") == 0) {
            options.force = 1;
        } else if (rt_strcmp(opt.flag, "--max") == 0) {
            if (tool_opt_require_value(&opt) != 0 || tool_parse_uint_arg(opt.value, &options.max_count, "xmlsplit", "max") != 0) {
                return 1;
            }
        } else {
            tool_write_error("xmlsplit", "unknown option: ", opt.flag);
            tool_write_usage("xmlsplit", "[--force] [--max N] SELECTOR PREFIX [FILE]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlsplit", "[--force] [--max N] SELECTOR PREFIX [FILE]");
        return 0;
    }
    if (opt.argi + 2 > argc || opt.argi + 3 < argc) {
        tool_write_usage("xmlsplit", "[--force] [--max N] SELECTOR PREFIX [FILE]");
        return 1;
    }
    selector = argv[opt.argi++];
    prefix = argv[opt.argi++];
    if (opt.argi < argc) path = argv[opt.argi];
    return split_one(selector, prefix, path, &options);
}
