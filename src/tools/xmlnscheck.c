#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


typedef struct {
    char **prefixes;
    unsigned int *depths;
    int *used;
    unsigned long long *lines;
    unsigned long long *columns;
    size_t count;
    size_t capacity;
} NamespaceState;

static char *name_prefix_dup(const XmlName *name) {
    size_t i;
    for (i = 0U; i < name->length; ++i) {
        if (name->start[i] == ':') {
            return xml_slice_dup(name->start, i);
        }
    }
    return 0;
}

static int name_equals_slice(const XmlName *name, const char *text, size_t length) {
    return name->length == length && rt_strncmp(name->start, text, length) == 0;
}

static int attr_is_xmlns(const XmlAttribute *attr) {
    return name_equals_slice(&attr->name, "xmlns", 5U) ||
           (attr->name.length > 6U && rt_strncmp(attr->name.start, "xmlns:", 6U) == 0);
}

static char *attr_decl_prefix_dup(const XmlAttribute *attr, int *decl_out) {
    size_t length;
    *decl_out = 0;
    if (name_equals_slice(&attr->name, "xmlns", 5U)) {
        *decl_out = 1;
        return xml_slice_dup("", 0U);
    }
    if (!(attr->name.length > 6U && rt_strncmp(attr->name.start, "xmlns:", 6U) == 0)) return 0;
    length = attr->name.length - 6U;
    *decl_out = 1;
    return xml_slice_dup(attr->name.start + 6U, length);
}

static int namespace_state_push(NamespaceState *state, const char *prefix, unsigned int depth, const XmlToken *token) {
    char **prefixes;
    unsigned int *depths;
    int *used;
    unsigned long long *lines;
    unsigned long long *columns;
    size_t new_capacity;
    size_t i;
    if (state->count == state->capacity) {
        new_capacity = state->capacity == 0U ? 64U : state->capacity * 2U;
        if (new_capacity <= state->capacity) return -1;
        prefixes = (char **)rt_malloc_array(new_capacity, sizeof(*prefixes));
        depths = (unsigned int *)rt_malloc_array(new_capacity, sizeof(*depths));
        used = (int *)rt_malloc_array(new_capacity, sizeof(*used));
        lines = (unsigned long long *)rt_malloc_array(new_capacity, sizeof(*lines));
        columns = (unsigned long long *)rt_malloc_array(new_capacity, sizeof(*columns));
        if (prefixes == 0 || depths == 0 || used == 0 || lines == 0 || columns == 0) {
            rt_free(prefixes);
            rt_free(depths);
            rt_free(used);
            rt_free(lines);
            rt_free(columns);
            return -1;
        }
        for (i = 0U; i < state->count; ++i) {
            prefixes[i] = state->prefixes[i];
            depths[i] = state->depths[i];
            used[i] = state->used[i];
            lines[i] = state->lines[i];
            columns[i] = state->columns[i];
        }
        rt_free(state->prefixes);
        rt_free(state->depths);
        rt_free(state->used);
        rt_free(state->lines);
        rt_free(state->columns);
        state->prefixes = prefixes;
        state->depths = depths;
        state->used = used;
        state->lines = lines;
        state->columns = columns;
        state->capacity = new_capacity;
    }
    state->prefixes[state->count] = xml_slice_dup(prefix, rt_strlen(prefix));
    if (state->prefixes[state->count] == 0) return -1;
    state->depths[state->count] = depth;
    state->used[state->count] = 0;
    state->lines[state->count] = token->line;
    state->columns[state->count] = token->column;
    state->count += 1U;
    return 0;
}

static void namespace_state_free(NamespaceState *state) {
    size_t i;
    for (i = 0U; i < state->count; ++i) rt_free(state->prefixes[i]);
    rt_free(state->prefixes);
    rt_free(state->depths);
    rt_free(state->used);
    rt_free(state->lines);
    rt_free(state->columns);
}

static int prefix_bound(NamespaceState *state, const char *prefix, int mark_used) {
    size_t i;
    if (prefix[0] == '\0' || rt_strcmp(prefix, "xml") == 0) return 1;
    for (i = state->count; i > 0U; --i) {
        if (rt_strcmp(state->prefixes[i - 1U], prefix) == 0) {
            if (mark_used) state->used[i - 1U] = 1;
            return 1;
        }
    }
    return 0;
}

static void report_position_issue(const char *path, unsigned long long line, unsigned long long column, const char *message, const char *detail) {
    rt_write_cstr(2, "xmlnscheck: ");
    if (path != 0) {
        rt_write_cstr(2, path);
        rt_write_char(2, ':');
    }
    rt_write_uint(2, line);
    rt_write_char(2, ':');
    rt_write_uint(2, column);
    rt_write_cstr(2, ": ");
    rt_write_cstr(2, message);
    if (detail != 0) rt_write_cstr(2, detail);
    rt_write_char(2, '\n');
}

static int pop_depth(const char *path, NamespaceState *state, unsigned int depth, int report_unused) {
    while (state->count > 0U && state->depths[state->count - 1U] >= depth) {
        if (report_unused && !state->used[state->count - 1U]) {
            report_position_issue(path, state->lines[state->count - 1U], state->columns[state->count - 1U], "unused namespace declaration: ", state->prefixes[state->count - 1U]);
            return 1;
        }
        rt_free(state->prefixes[state->count - 1U]);
        state->count -= 1U;
    }
    return 0;
}

static void report_issue(const char *path, const XmlToken *token, const char *message, const char *detail) {
    rt_write_cstr(2, "xmlnscheck: ");
    if (path != 0) {
        rt_write_cstr(2, path);
        rt_write_char(2, ':');
    }
    rt_write_uint(2, token->line);
    rt_write_char(2, ':');
    rt_write_uint(2, token->column);
    rt_write_cstr(2, ": ");
    rt_write_cstr(2, message);
    if (detail != 0) rt_write_cstr(2, detail);
    rt_write_char(2, '\n');
}

static void free_seen_prefixes(char **seen, size_t seen_count) {
    while (seen_count > 0U) rt_free(seen[--seen_count]);
    rt_free(seen);
}

static int check_start(const char *path, const XmlToken *token, NamespaceState *state) {
    char **seen;
    size_t seen_count = 0U;
    size_t i;

    seen = (char **)rt_malloc_array(token->attribute_count, sizeof(*seen));
    if (token->attribute_count > 0U && seen == 0) {
        report_issue(path, token, "out of memory", 0);
        return 1;
    }

    for (i = 0U; i < token->attribute_count; ++i) {
        int decl;
        char *prefix = attr_decl_prefix_dup(&token->attributes[i], &decl);
        size_t j;
        if (decl && prefix == 0) {
            report_issue(path, token, "out of memory", 0);
            free_seen_prefixes(seen, seen_count);
            return 1;
        }
        if (!decl) continue;
        if (rt_strcmp(prefix, "xmlns") == 0) {
            report_issue(path, token, "reserved namespace prefix declared: ", prefix);
            rt_free(prefix);
            free_seen_prefixes(seen, seen_count);
            return 1;
        }
        if (rt_strcmp(prefix, "xml") == 0 && !(token->attributes[i].value_length == 36U && rt_strncmp(token->attributes[i].value, "http://www.w3.org/XML/1998/namespace", 36U) == 0)) {
            report_issue(path, token, "xml prefix must use its reserved namespace", 0);
            rt_free(prefix);
            free_seen_prefixes(seen, seen_count);
            return 1;
        }
        for (j = 0U; j < seen_count; ++j) {
            if (rt_strcmp(seen[j], prefix) == 0) {
                report_issue(path, token, "duplicate namespace declaration: ", prefix[0] == '\0' ? "default" : prefix);
                rt_free(prefix);
                free_seen_prefixes(seen, seen_count);
                return 1;
            }
        }
        seen[seen_count++] = prefix;
        if (prefix[0] != '\0' && namespace_state_push(state, prefix, token->depth + 1U, token) != 0) {
            report_issue(path, token, "out of memory", 0);
            free_seen_prefixes(seen, seen_count);
            return 1;
        }
    }

    {
        char *prefix = name_prefix_dup(&token->name);
        if (prefix != 0 && !prefix_bound(state, prefix, 1)) {
        report_issue(path, token, "unbound element prefix: ", prefix);
        rt_free(prefix);
        free_seen_prefixes(seen, seen_count);
        return 1;
    }
        if (prefix != 0 && rt_strcmp(prefix, "xmlns") == 0) {
        report_issue(path, token, "element uses reserved prefix: ", prefix);
        rt_free(prefix);
        free_seen_prefixes(seen, seen_count);
        return 1;
    }
        rt_free(prefix);
    }
    for (i = 0U; i < token->attribute_count; ++i) {
        if (attr_is_xmlns(&token->attributes[i])) continue;
        {
            char *prefix = name_prefix_dup(&token->attributes[i].name);
        if (prefix != 0) {
            if (rt_strcmp(prefix, "xmlns") == 0 || !prefix_bound(state, prefix, 1)) {
                report_issue(path, token, "unbound attribute prefix: ", prefix);
                rt_free(prefix);
                free_seen_prefixes(seen, seen_count);
                return 1;
            }
        }
            rt_free(prefix);
        }
    }
    free_seen_prefixes(seen, seen_count);
    return 0;
}

static int check_one(const char *path, int report_unused) {
    XmlParser parser;
    XmlToken token;
    NamespaceState state;
    char *input;
    size_t length;
    int result;

    rt_memset(&state, 0, sizeof(state));
    if (xml_read_document(path, &input, &length, "xmlnscheck") != 0) return 1;
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) {
            if (check_start(path, &token, &state) != 0) {
                xml_free_document(input);
                namespace_state_free(&state);
                return 1;
            }
            if (token.type == XML_TOKEN_EMPTY && pop_depth(path, &state, token.depth + 1U, report_unused) != 0) {
                xml_free_document(input);
                namespace_state_free(&state);
                return 1;
            }
        } else if (token.type == XML_TOKEN_END) {
            if (pop_depth(path, &state, token.depth, report_unused) != 0) {
                xml_free_document(input);
                namespace_state_free(&state);
                return 1;
            }
        }
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlnscheck", path, &parser);
        xml_free_document(input);
        namespace_state_free(&state);
        return 1;
    }
    xml_free_document(input);
    namespace_state_free(&state);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    int option_result;
    int exit_code = 0;
    int report_unused = 0;
    int i;

    tool_opt_init(&opt, argc, argv, "xmlnscheck", "[--unused] [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--unused") == 0) report_unused = 1;
        else {
            tool_write_error("xmlnscheck", "unknown option: ", opt.flag);
            tool_write_usage("xmlnscheck", "[--unused] [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlnscheck", "[--unused] [FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) return check_one(0, report_unused);
    for (i = opt.argi; i < argc; ++i) {
        if (check_one(argv[i], report_unused) != 0) exit_code = 1;
    }
    return exit_code;
}
