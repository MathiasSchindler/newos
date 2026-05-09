#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


#define XMLGREP_INLINE_TEXT 4096

typedef struct {
    int ignore_case;
    int invert_match;
    int count_only;
    int files_with_matches;
    int files_without_matches;
    int only_matching;
    int quiet;
    int fixed_string;
    int multiple_files;
} XmlGrepOptions;

typedef struct {
    unsigned long long selected_count;
    int matched;
} XmlGrepResult;

static int make_value_buffer(const char *text, size_t length, char **buffer_out, char inline_buffer[XMLGREP_INLINE_TEXT]) {
    char *buffer = inline_buffer;
    size_t i;

    if (length >= XMLGREP_INLINE_TEXT) {
        if (length == (size_t)~(size_t)0) return -1;
        buffer = (char *)rt_malloc(length + 1U);
        if (buffer == 0) return -1;
    }
    for (i = 0U; i < length; ++i) {
        buffer[i] = text[i];
    }
    buffer[length] = '\0';
    *buffer_out = buffer;
    return 0;
}

static char fold_ascii(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? (char)(ch - 'A' + 'a') : ch;
}

static int chars_equal(char left, char right, int ignore_case) {
    if (ignore_case) {
        left = fold_ascii(left);
        right = fold_ascii(right);
    }
    return left == right;
}

static int fixed_search(const char *needle, const char *text, size_t start, int ignore_case, size_t *start_out, size_t *end_out) {
    size_t needle_length = rt_strlen(needle);
    size_t text_length = rt_strlen(text);
    size_t i;

    if (needle_length == 0U) {
        if (start > text_length) return 0;
        *start_out = start;
        *end_out = start;
        return 1;
    }
    if (needle_length > text_length || start > text_length - needle_length) return 0;
    for (i = start; i <= text_length - needle_length; ++i) {
        size_t j;
        int matched = 1;
        for (j = 0U; j < needle_length; ++j) {
            if (!chars_equal(needle[j], text[i + j], ignore_case)) {
                matched = 0;
                break;
            }
        }
        if (matched) {
            *start_out = i;
            *end_out = i + needle_length;
            return 1;
        }
    }
    return 0;
}

static int find_match(const char *pattern, const char *text, size_t length, const XmlGrepOptions *options, size_t search_start, size_t *start_out, size_t *end_out) {
    char inline_buffer[XMLGREP_INLINE_TEXT];
    char *buffer = inline_buffer;
    int matched;

    if (make_value_buffer(text, length, &buffer, inline_buffer) != 0) {
        return 0;
    }
    matched = options->fixed_string ?
        fixed_search(pattern, buffer, search_start, options->ignore_case, start_out, end_out) :
        tool_regex_search(pattern, buffer, options->ignore_case, search_start, start_out, end_out);
    if (buffer != inline_buffer) rt_free(buffer);
    return matched;
}

static int value_matches(const char *pattern, const char *text, size_t length, const XmlGrepOptions *options) {
    size_t start;
    size_t end;
    return find_match(pattern, text, length, options, 0U, &start, &end);
}

static void write_match_prefix(const char *path, const XmlNameStack *stack) {
    if (path != 0) {
        rt_write_cstr(1, path);
        rt_write_char(1, ':');
    }
    xml_write_name_stack_path(1, stack);
}

static void write_text_match(const char *file_path, const XmlNameStack *stack, const char *text, size_t length) {
    write_match_prefix(file_path, stack);
    rt_write_cstr(1, ": ");
    tool_write_visible(1, text, length);
    rt_write_char(1, '\n');
}

static void write_only_match(const char *file_path, const XmlNameStack *stack, const char *suffix, const char *text, size_t start, size_t end) {
    write_match_prefix(file_path, stack);
    if (suffix != 0) rt_write_cstr(1, suffix);
    rt_write_cstr(1, ": ");
    tool_write_visible(1, text + start, end - start);
    rt_write_char(1, '\n');
}

static void write_count(const char *path, const XmlGrepOptions *options, unsigned long long count) {
    if (options->multiple_files && path != 0) {
        rt_write_cstr(1, path);
        rt_write_char(1, ':');
    }
    rt_write_uint(1, count);
    rt_write_char(1, '\n');
}

static void write_file_result(const char *path) {
    rt_write_cstr(1, path == 0 ? "-" : path);
    rt_write_char(1, '\n');
}

static int set_short_option(XmlGrepOptions *options, char option) {
    if (option == 'i') options->ignore_case = 1;
    else if (option == 'F') options->fixed_string = 1;
    else if (option == 'v') options->invert_match = 1;
    else if (option == 'c') options->count_only = 1;
    else if (option == 'l') options->files_with_matches = 1;
    else if (option == 'L') options->files_without_matches = 1;
    else if (option == 'o') options->only_matching = 1;
    else if (option == 'q') options->quiet = 1;
    else return -1;
    return 0;
}

static int parse_grep_option(XmlGrepOptions *options, const char *flag) {
    size_t i;
    if (rt_strcmp(flag, "--ignore-case") == 0) options->ignore_case = 1;
    else if (rt_strcmp(flag, "--fixed-strings") == 0) options->fixed_string = 1;
    else if (rt_strcmp(flag, "--invert-match") == 0) options->invert_match = 1;
    else if (rt_strcmp(flag, "--count") == 0) options->count_only = 1;
    else if (rt_strcmp(flag, "--files-with-matches") == 0) options->files_with_matches = 1;
    else if (rt_strcmp(flag, "--files-without-match") == 0) options->files_without_matches = 1;
    else if (rt_strcmp(flag, "--only-matching") == 0) options->only_matching = 1;
    else if (rt_strcmp(flag, "--quiet") == 0 || rt_strcmp(flag, "--silent") == 0) options->quiet = 1;
    else if (flag[0] == '-' && flag[1] != '-' && flag[1] != '\0') {
        for (i = 1U; flag[i] != '\0'; ++i) {
            if (set_short_option(options, flag[i]) != 0) return -1;
        }
    } else return -1;
    return 0;
}

static void record_value_match(const char *pattern, const char *path, const XmlNameStack *stack, const char *suffix, const char *text, size_t length, const XmlGrepOptions *options, XmlGrepResult *grep_result) {
    int matched = value_matches(pattern, text, length, options);
    int selected = options->invert_match ? !matched : matched;

    if (!selected) return;
    grep_result->selected_count += 1ULL;
    grep_result->matched = 1;
    if (options->quiet || options->count_only || options->files_with_matches || options->files_without_matches) return;

    if (options->only_matching && !options->invert_match) {
        size_t start = 0U;
        size_t end = 0U;
        size_t search_start = 0U;
        while (find_match(pattern, text, length, options, search_start, &start, &end)) {
            write_only_match(path, stack, suffix, text, start, end);
            if (end <= start) {
                search_start = start + 1U;
            } else {
                search_start = end;
            }
            if (search_start > length) break;
        }
    } else if (suffix != 0) {
        write_match_prefix(path, stack);
        rt_write_cstr(1, suffix);
        rt_write_cstr(1, ": ");
        tool_write_visible(1, text, length);
        rt_write_char(1, '\n');
    } else {
        write_text_match(path, stack, text, length);
    }
}

static int result_counts_for_status(const XmlGrepOptions *options, const XmlGrepResult *grep_result) {
    if (options->files_without_matches) return !grep_result->matched;
    return grep_result->matched;
}

static int grep_one(const char *pattern, const char *path, const XmlGrepOptions *options, XmlGrepResult *grep_result) {
    XmlParser parser;
    XmlToken token;
    XmlNameStack stack;
    char *input_buffer;
    size_t length;
    int result;
    char attr_suffix[XML_NAME_BUFFER_SIZE + 3U];

    grep_result->selected_count = 0ULL;
    grep_result->matched = 0;
    xml_name_stack_init(&stack);

    if (xml_read_document(path, &input_buffer, &length, "xmlgrep") != 0) {
        xml_name_stack_free(&stack);
        return 1;
    }
    xml_parser_init(&parser, input_buffer, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) {
            size_t i;
            if (tool_xml_name_stack_push(&stack, token.name, "xmlgrep") != 0) {
                xml_free_document(input_buffer);
                xml_name_stack_free(&stack);
                return 1;
            }
            for (i = 0U; i < token.attribute_count; ++i) {
                attr_suffix[0] = '/';
                attr_suffix[1] = '@';
                xml_copy_name(&token.attributes[i].name, attr_suffix + 2U, sizeof(attr_suffix) - 2U);
                record_value_match(pattern, path, &stack, attr_suffix, token.attributes[i].value, token.attributes[i].value_length, options, grep_result);
            }
            if (token.type == XML_TOKEN_EMPTY) {
                xml_name_stack_pop(&stack);
            }
        } else if (token.type == XML_TOKEN_TEXT || token.type == XML_TOKEN_CDATA) {
            if (!token.text_is_blank) {
                record_value_match(pattern, path, &stack, 0, token.text, token.text_length, options, grep_result);
            }
        } else if (token.type == XML_TOKEN_END) {
            xml_name_stack_pop(&stack);
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlgrep", path, &parser);
        xml_free_document(input_buffer);
        xml_name_stack_free(&stack);
        return 1;
    }
    xml_free_document(input_buffer);
    xml_name_stack_free(&stack);
    if (options->count_only) {
        write_count(path, options, grep_result->selected_count);
    } else if (options->files_with_matches && grep_result->matched) {
        write_file_result(path);
    } else if (options->files_without_matches && !grep_result->matched) {
        write_file_result(path);
    }
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    XmlGrepOptions options;
    XmlGrepResult grep_result;
    int option_result;
    int had_match = 0;
    int had_error = 0;
    const char *pattern;
    int i;

    rt_memset(&options, 0, sizeof(options));
    tool_opt_init(&opt, argc, argv, "xmlgrep", "[-iFvclLoq] PATTERN [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (parse_grep_option(&options, opt.flag) != 0) {
            tool_write_error("xmlgrep", "unknown option: ", opt.flag);
            tool_write_usage("xmlgrep", "[-iFvclLoq] PATTERN [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlgrep", "[-iFvclLoq] PATTERN [FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) {
        tool_write_usage("xmlgrep", "[-iFvclLoq] PATTERN [FILE ...]");
        return 1;
    }
    pattern = argv[opt.argi++];
    if (options.files_with_matches && options.files_without_matches) {
        tool_write_error("xmlgrep", "choose only one of -l and -L", 0);
        return 1;
    }
    options.multiple_files = argc - opt.argi > 1;
    if (opt.argi >= argc) {
        if (grep_one(pattern, 0, &options, &grep_result) != 0) return 2;
        return result_counts_for_status(&options, &grep_result) ? 0 : 1;
    }
    for (i = opt.argi; i < argc; ++i) {
        if (grep_one(pattern, argv[i], &options, &grep_result) != 0) {
            had_error = 1;
        } else if (result_counts_for_status(&options, &grep_result)) {
            had_match = 1;
        }
    }
    if (had_error) return 2;
    return had_match ? 0 : 1;
}
