#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define FIND_MAX_ENTRIES 1024
#define FIND_PATH_CAPACITY 1024
#define FIND_MAX_START_PATHS 64
#define FIND_MAX_EXEC_ARGS 64
#define FIND_MAX_NODES 256
#define FIND_MAX_EXEC_STORAGE 256

enum {
    FIND_NODE_TRUE = 1,
    FIND_NODE_AND,
    FIND_NODE_OR,
    FIND_NODE_NOT,
    FIND_NODE_NAME,
    FIND_NODE_INAME,
    FIND_NODE_PATH,
    FIND_NODE_TYPE,
    FIND_NODE_MTIME,
    FIND_NODE_SIZE,
    FIND_NODE_EMPTY,
    FIND_NODE_PRINT,
    FIND_NODE_PRINT0,
    FIND_NODE_PRUNE,
    FIND_NODE_EXEC
};

typedef struct {
    int type;
    int left;
    int right;
    const char *text;
    char type_filter;
    int relation;
    unsigned long long value;
    int exec_start;
    int exec_count;
} FindNode;

typedef struct {
    int has_mindepth;
    int mindepth;
    int has_maxdepth;
    int maxdepth;
    int node_count;
    int root_node;
    int has_output_action;
    const char *exec_storage[FIND_MAX_EXEC_STORAGE];
    int exec_storage_count;
    FindNode nodes[FIND_MAX_NODES];
} FindOptions;

typedef struct {
    int argc;
    char **argv;
    int index;
    FindOptions *options;
} FindParser;

typedef struct {
    int allow_action;
    int prune;
    int error;
} FindEvalState;

static void print_usage(void) {
    rt_write_line(
        2,
        "Usage: find [path ...] [expression]\n"
        "  predicates: -name/-iname PATTERN -path PATTERN -type f|d|l -mtime N -size N[c|k|M] -empty\n"
        "  operators: ! -not -a -and -o -or ( ... )\n"
        "  actions: -print -print0 -prune -exec CMD {} ;"
    );
}

static int parse_relation_prefix(const char **text, int *relation_out) {
    *relation_out = 0;

    if (**text == '+') {
        *relation_out = 1;
        *text += 1;
    } else if (**text == '-') {
        *relation_out = -1;
        *text += 1;
    }

    return 0;
}

static int parse_size_filter(const char *text, int *relation_out, unsigned long long *size_out) {
    char digits[32];
    size_t length = 0;
    unsigned long long value = 0;
    unsigned long long scale = 1;

    parse_relation_prefix(&text, relation_out);

    while (text[length] != '\0' && text[length] >= '0' && text[length] <= '9') {
        if (length + 1 >= sizeof(digits)) {
            return -1;
        }
        digits[length] = text[length];
        length += 1;
    }

    if (length == 0) {
        return -1;
    }

    digits[length] = '\0';
    if (rt_parse_uint(digits, &value) != 0) {
        return -1;
    }

    if (text[length] == 'k' || text[length] == 'K') {
        scale = 1024ULL;
        length += 1;
    } else if (text[length] == 'm' || text[length] == 'M') {
        scale = 1024ULL * 1024ULL;
        length += 1;
    } else if (text[length] == 'c') {
        scale = 1ULL;
        length += 1;
    }

    if (text[length] != '\0') {
        return -1;
    }

    *size_out = value * scale;
    return 0;
}

static int parse_mtime_filter(const char *text, int *relation_out, unsigned long long *days_out) {
    parse_relation_prefix(&text, relation_out);
    return rt_parse_uint(text, days_out);
}

static int matches_numeric_filter(unsigned long long actual, int relation, unsigned long long expected) {
    if (relation < 0) {
        return actual < expected;
    }
    if (relation > 0) {
        return actual > expected;
    }
    return actual == expected;
}

static int matches_type_filter(const PlatformDirEntry *entry, char type_filter) {
    if (type_filter == '\0') {
        return 1;
    }
    if (type_filter == 'f') {
        return !entry->is_dir;
    }
    if (type_filter == 'd') {
        return entry->is_dir;
    }
    if (type_filter == 'l') {
        return (entry->mode & 0170000U) == 0120000U;
    }
    return 0;
}

static int add_node(FindOptions *options, int type) {
    FindNode *node;
    int index;

    if (options->node_count >= FIND_MAX_NODES) {
        return -1;
    }

    index = options->node_count;
    node = &options->nodes[options->node_count++];
    rt_memset(node, 0, sizeof(*node));
    node->type = type;
    node->left = -1;
    node->right = -1;
    node->exec_start = -1;
    return index;
}

static int is_operator_token(const char *arg) {
    return rt_strcmp(arg, "!") == 0 || rt_strcmp(arg, "-not") == 0 || rt_strcmp(arg, "-a") == 0 ||
           rt_strcmp(arg, "-and") == 0 || rt_strcmp(arg, "-o") == 0 || rt_strcmp(arg, "-or") == 0 ||
           rt_strcmp(arg, "(") == 0 || rt_strcmp(arg, ")") == 0;
}

static int is_primary_token(const char *arg) {
    return is_operator_token(arg) || rt_strcmp(arg, "-name") == 0 || rt_strcmp(arg, "-iname") == 0 ||
           rt_strcmp(arg, "-path") == 0 || rt_strcmp(arg, "-type") == 0 || rt_strcmp(arg, "-mtime") == 0 ||
           rt_strcmp(arg, "-size") == 0 || rt_strcmp(arg, "-empty") == 0 || rt_strcmp(arg, "-mindepth") == 0 ||
           rt_strcmp(arg, "-maxdepth") == 0 || rt_strcmp(arg, "-print") == 0 || rt_strcmp(arg, "-print0") == 0 ||
           rt_strcmp(arg, "-prune") == 0 || rt_strcmp(arg, "-exec") == 0;
}

static char ascii_tolower_char(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static void lowercase_copy(char *buffer, size_t buffer_size, const char *text) {
    size_t i = 0U;

    if (buffer_size == 0U) {
        return;
    }

    while (text[i] != '\0' && i + 1U < buffer_size) {
        buffer[i] = ascii_tolower_char(text[i]);
        i += 1U;
    }
    buffer[i] = '\0';
}

static int wildcard_match_casefold(const char *pattern, const char *text) {
    char lowered_pattern[FIND_PATH_CAPACITY];
    char lowered_text[FIND_PATH_CAPACITY];

    lowercase_copy(lowered_pattern, sizeof(lowered_pattern), pattern);
    lowercase_copy(lowered_text, sizeof(lowered_text), text);
    return tool_wildcard_match(lowered_pattern, lowered_text);
}

static int matches_empty_filter(const char *path, const PlatformDirEntry *entry) {
    if (entry->is_dir) {
        PlatformDirEntry entries[FIND_MAX_ENTRIES];
        size_t count = 0U;
        int is_directory = 0;
        size_t i;

        if (platform_collect_entries(path, 1, entries, FIND_MAX_ENTRIES, &count, &is_directory) != 0 || !is_directory) {
            return 0;
        }

        for (i = 0U; i < count; ++i) {
            if (rt_strcmp(entries[i].name, ".") != 0 && rt_strcmp(entries[i].name, "..") != 0) {
                platform_free_entries(entries, count);
                return 0;
            }
        }
        platform_free_entries(entries, count);
        return 1;
    }

    return entry->size == 0ULL;
}

static int emit_path(const char *path, int nul_terminated) {
    if (rt_write_cstr(1, path) != 0) {
        return -1;
    }
    return rt_write_char(1, nul_terminated ? '\0' : '\n');
}

static int run_exec_action(const char *path, const FindOptions *options, const FindNode *node) {
    char *spawn_argv[FIND_MAX_EXEC_ARGS + 1];
    int pid;
    int status;
    int i;

    if (node->exec_count <= 0) {
        return 0;
    }

    for (i = 0; i < node->exec_count; ++i) {
        const char *value = options->exec_storage[node->exec_start + i];
        spawn_argv[i] = (char *)(rt_strcmp(value, "{}") == 0 ? path : value);
    }
    spawn_argv[node->exec_count] = 0;

    if (platform_spawn_process(spawn_argv, -1, -1, 0, 0, 0, &pid) != 0) {
        tool_write_error("find", "failed to execute ", spawn_argv[0]);
        return -1;
    }
    if (platform_wait_process(pid, &status) != 0) {
        tool_write_error("find", "wait failed for ", spawn_argv[0]);
        return -1;
    }
    if (status != 0) {
        tool_write_error("find", "command failed: ", spawn_argv[0]);
        return -1;
    }
    return 0;
}

static int evaluate_node(
    int node_index,
    const char *path,
    const PlatformDirEntry *entry,
    const FindOptions *options,
    long long now,
    FindEvalState *state
) {
    const FindNode *node = &options->nodes[node_index];

    if (state->error) {
        return 0;
    }

    switch (node->type) {
        case FIND_NODE_TRUE:
            return 1;
        case FIND_NODE_AND: {
            int left_value = evaluate_node(node->left, path, entry, options, now, state);
            if (!left_value || state->error) {
                return 0;
            }
            return evaluate_node(node->right, path, entry, options, now, state);
        }
        case FIND_NODE_OR: {
            int left_value = evaluate_node(node->left, path, entry, options, now, state);
            if (left_value || state->error) {
                return left_value;
            }
            return evaluate_node(node->right, path, entry, options, now, state);
        }
        case FIND_NODE_NOT:
            return !evaluate_node(node->left, path, entry, options, now, state);
        case FIND_NODE_NAME:
            return tool_wildcard_match(node->text, tool_base_name(path));
        case FIND_NODE_INAME:
            return wildcard_match_casefold(node->text, tool_base_name(path));
        case FIND_NODE_PATH:
            return tool_wildcard_match(node->text, path);
        case FIND_NODE_TYPE:
            return matches_type_filter(entry, node->type_filter);
        case FIND_NODE_MTIME: {
            unsigned long long age_days = 0ULL;
            if (now > entry->mtime) {
                age_days = (unsigned long long)((now - entry->mtime) / 86400);
            }
            return matches_numeric_filter(age_days, node->relation, node->value);
        }
        case FIND_NODE_SIZE:
            return matches_numeric_filter(entry->size, node->relation, node->value);
        case FIND_NODE_EMPTY:
            return matches_empty_filter(path, entry);
        case FIND_NODE_PRINT:
            if (state->allow_action && emit_path(path, 0) != 0) {
                state->error = 1;
            }
            return 1;
        case FIND_NODE_PRINT0:
            if (state->allow_action && emit_path(path, 1) != 0) {
                state->error = 1;
            }
            return 1;
        case FIND_NODE_PRUNE:
            if (entry->is_dir) {
                state->prune = 1;
            }
            return 1;
        case FIND_NODE_EXEC:
            if (state->allow_action && run_exec_action(path, options, node) != 0) {
                state->error = 1;
                return 0;
            }
            return 1;
        default:
            return 0;
    }
}

static int parse_expression(FindParser *parser, int *node_out);

static int parse_primary(FindParser *parser, int *node_out) {
    const char *token;
    int node_index;

    if (parser->index >= parser->argc) {
        rt_write_line(2, "find: missing expression");
        return -1;
    }

    token = parser->argv[parser->index];
    if (rt_strcmp(token, "!") == 0 || rt_strcmp(token, "-not") == 0) {
        parser->index += 1;
        if (parse_primary(parser, node_out) != 0) {
            return -1;
        }
        node_index = add_node(parser->options, FIND_NODE_NOT);
        if (node_index < 0) {
            rt_write_line(2, "find: expression too large");
            return -1;
        }
        parser->options->nodes[node_index].left = *node_out;
        *node_out = node_index;
        return 0;
    }

    if (rt_strcmp(token, "(") == 0) {
        parser->index += 1;
        if (parse_expression(parser, node_out) != 0) {
            return -1;
        }
        if (parser->index >= parser->argc || rt_strcmp(parser->argv[parser->index], ")") != 0) {
            rt_write_line(2, "find: missing ')'");
            return -1;
        }
        parser->index += 1;
        return 0;
    }

    if (rt_strcmp(token, "-name") == 0 || rt_strcmp(token, "-iname") == 0 || rt_strcmp(token, "-path") == 0 ||
        rt_strcmp(token, "-type") == 0 || rt_strcmp(token, "-mtime") == 0 || rt_strcmp(token, "-size") == 0 ||
        rt_strcmp(token, "-mindepth") == 0 || rt_strcmp(token, "-maxdepth") == 0) {
        if (parser->index + 1 >= parser->argc) {
            rt_write_line(2, "find: missing argument");
            return -1;
        }
    }

    if (rt_strcmp(token, "-name") == 0) {
        node_index = add_node(parser->options, FIND_NODE_NAME);
        parser->options->nodes[node_index].text = parser->argv[parser->index + 1];
        parser->index += 2;
        *node_out = node_index;
        return 0;
    }
    if (rt_strcmp(token, "-iname") == 0) {
        node_index = add_node(parser->options, FIND_NODE_INAME);
        parser->options->nodes[node_index].text = parser->argv[parser->index + 1];
        parser->index += 2;
        *node_out = node_index;
        return 0;
    }
    if (rt_strcmp(token, "-path") == 0) {
        node_index = add_node(parser->options, FIND_NODE_PATH);
        parser->options->nodes[node_index].text = parser->argv[parser->index + 1];
        parser->index += 2;
        *node_out = node_index;
        return 0;
    }
    if (rt_strcmp(token, "-type") == 0) {
        char type_filter = parser->argv[parser->index + 1][0];
        if (type_filter != 'f' && type_filter != 'd' && type_filter != 'l') {
            rt_write_line(2, "find: unsupported -type value");
            return -1;
        }
        node_index = add_node(parser->options, FIND_NODE_TYPE);
        parser->options->nodes[node_index].type_filter = type_filter;
        parser->index += 2;
        *node_out = node_index;
        return 0;
    }
    if (rt_strcmp(token, "-mtime") == 0) {
        node_index = add_node(parser->options, FIND_NODE_MTIME);
        if (parse_mtime_filter(
                parser->argv[parser->index + 1],
                &parser->options->nodes[node_index].relation,
                &parser->options->nodes[node_index].value
            ) != 0) {
            rt_write_line(2, "find: invalid -mtime value");
            return -1;
        }
        parser->index += 2;
        *node_out = node_index;
        return 0;
    }
    if (rt_strcmp(token, "-size") == 0) {
        node_index = add_node(parser->options, FIND_NODE_SIZE);
        if (parse_size_filter(
                parser->argv[parser->index + 1],
                &parser->options->nodes[node_index].relation,
                &parser->options->nodes[node_index].value
            ) != 0) {
            rt_write_line(2, "find: invalid -size value");
            return -1;
        }
        parser->index += 2;
        *node_out = node_index;
        return 0;
    }
    if (rt_strcmp(token, "-empty") == 0) {
        node_index = add_node(parser->options, FIND_NODE_EMPTY);
        parser->index += 1;
        *node_out = node_index;
        return 0;
    }
    if (rt_strcmp(token, "-mindepth") == 0) {
        long long depth = 0;
        if (tool_parse_int_arg(parser->argv[parser->index + 1], &depth, "find", "mindepth") != 0 || depth < 0) {
            return -1;
        }
        parser->options->has_mindepth = 1;
        parser->options->mindepth = (int)depth;
        node_index = add_node(parser->options, FIND_NODE_TRUE);
        parser->index += 2;
        *node_out = node_index;
        return 0;
    }
    if (rt_strcmp(token, "-maxdepth") == 0) {
        long long depth = 0;
        if (tool_parse_int_arg(parser->argv[parser->index + 1], &depth, "find", "maxdepth") != 0 || depth < 0) {
            return -1;
        }
        parser->options->has_maxdepth = 1;
        parser->options->maxdepth = (int)depth;
        node_index = add_node(parser->options, FIND_NODE_TRUE);
        parser->index += 2;
        *node_out = node_index;
        return 0;
    }
    if (rt_strcmp(token, "-print") == 0) {
        parser->options->has_output_action = 1;
        node_index = add_node(parser->options, FIND_NODE_PRINT);
        parser->index += 1;
        *node_out = node_index;
        return 0;
    }
    if (rt_strcmp(token, "-print0") == 0) {
        parser->options->has_output_action = 1;
        node_index = add_node(parser->options, FIND_NODE_PRINT0);
        parser->index += 1;
        *node_out = node_index;
        return 0;
    }
    if (rt_strcmp(token, "-prune") == 0) {
        node_index = add_node(parser->options, FIND_NODE_PRUNE);
        parser->index += 1;
        *node_out = node_index;
        return 0;
    }
    if (rt_strcmp(token, "-exec") == 0) {
        int exec_count = 0;
        int exec_start = parser->options->exec_storage_count;

        parser->index += 1;
        while (parser->index < parser->argc && exec_count < FIND_MAX_EXEC_ARGS) {
            const char *arg = parser->argv[parser->index++];
            if (rt_strcmp(arg, ";") == 0 || rt_strcmp(arg, "+") == 0) {
                break;
            }
            if (parser->options->exec_storage_count >= FIND_MAX_EXEC_STORAGE) {
                rt_write_line(2, "find: too many -exec arguments");
                return -1;
            }
            parser->options->exec_storage[parser->options->exec_storage_count++] = arg;
            exec_count += 1;
        }

        if (exec_count == 0 || parser->index > parser->argc ||
            (parser->index > 0 &&
             rt_strcmp(parser->argv[parser->index - 1], ";") != 0 &&
             rt_strcmp(parser->argv[parser->index - 1], "+") != 0)) {
            rt_write_line(2, "find: -exec requires a command terminated by ';' or '+'");
            return -1;
        }

        parser->options->has_output_action = 1;
        node_index = add_node(parser->options, FIND_NODE_EXEC);
        parser->options->nodes[node_index].exec_start = exec_start;
        parser->options->nodes[node_index].exec_count = exec_count;
        *node_out = node_index;
        return 0;
    }

    rt_write_cstr(2, "find: unknown predicate ");
    rt_write_line(2, token);
    return -1;
}

static int parse_and_expression(FindParser *parser, int *node_out) {
    int left = -1;

    if (parse_primary(parser, &left) != 0) {
        return -1;
    }

    while (parser->index < parser->argc) {
        const char *token = parser->argv[parser->index];
        int right;
        int node_index;

        if (rt_strcmp(token, ")") == 0 || rt_strcmp(token, "-o") == 0 || rt_strcmp(token, "-or") == 0) {
            break;
        }
        if (rt_strcmp(token, "-a") == 0 || rt_strcmp(token, "-and") == 0) {
            parser->index += 1;
        } else if (!is_primary_token(token)) {
            break;
        }

        if (parse_primary(parser, &right) != 0) {
            return -1;
        }

        node_index = add_node(parser->options, FIND_NODE_AND);
        if (node_index < 0) {
            rt_write_line(2, "find: expression too large");
            return -1;
        }
        parser->options->nodes[node_index].left = left;
        parser->options->nodes[node_index].right = right;
        left = node_index;
    }

    *node_out = left;
    return 0;
}

static int parse_expression(FindParser *parser, int *node_out) {
    int left = -1;

    if (parse_and_expression(parser, &left) != 0) {
        return -1;
    }

    while (parser->index < parser->argc) {
        const char *token = parser->argv[parser->index];
        int right;
        int node_index;

        if (rt_strcmp(token, "-o") != 0 && rt_strcmp(token, "-or") != 0) {
            break;
        }
        parser->index += 1;

        if (parse_and_expression(parser, &right) != 0) {
            return -1;
        }

        node_index = add_node(parser->options, FIND_NODE_OR);
        if (node_index < 0) {
            rt_write_line(2, "find: expression too large");
            return -1;
        }
        parser->options->nodes[node_index].left = left;
        parser->options->nodes[node_index].right = right;
        left = node_index;
    }

    *node_out = left;
    return 0;
}

static int find_walk(const char *path, const FindOptions *options, long long now, int depth) {
    PlatformDirEntry current;
    PlatformDirEntry entries[FIND_MAX_ENTRIES];
    size_t count = 0;
    int is_directory = 0;
    FindEvalState state;
    size_t i;

    if (platform_get_path_info(path, &current) != 0) {
        rt_write_cstr(2, "find: cannot access ");
        rt_write_line(2, path);
        return -1;
    }

    state.allow_action = (!options->has_mindepth || depth >= options->mindepth) &&
                         (!options->has_maxdepth || depth <= options->maxdepth);
    state.prune = 0;
    state.error = 0;

    if (evaluate_node(options->root_node, path, &current, options, now, &state) &&
        !options->has_output_action && state.allow_action) {
        if (emit_path(path, 0) != 0) {
            return -1;
        }
    }
    if (state.error) {
        return -1;
    }

    if (!current.is_dir) {
        return 0;
    }
    if ((options->has_maxdepth && depth >= options->maxdepth) || state.prune) {
        return 0;
    }
    if (platform_collect_entries(path, 1, entries, FIND_MAX_ENTRIES, &count, &is_directory) != 0) {
        rt_write_cstr(2, "find: cannot access ");
        rt_write_line(2, path);
        return -1;
    }

    for (i = 0; i < count; ++i) {
        char child_path[FIND_PATH_CAPACITY];

        if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
            continue;
        }

        if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0) {
            rt_write_line(2, "find: path too long");
            platform_free_entries(entries, count);
            return -1;
        }

        if (find_walk(child_path, options, now, depth + 1) != 0) {
            platform_free_entries(entries, count);
            return -1;
        }
    }

    platform_free_entries(entries, count);
    return 0;
}

int main(int argc, char **argv) {
    const char *start_paths[FIND_MAX_START_PATHS];
    int start_count = 0;
    FindOptions options;
    FindParser parser;
    long long now;
    int i;

    rt_memset(&options, 0, sizeof(options));
    options.root_node = -1;

    i = 1;
    while (i < argc && !is_primary_token(argv[i])) {
        if (start_count >= FIND_MAX_START_PATHS) {
            rt_write_line(2, "find: too many start paths");
            return 1;
        }
        start_paths[start_count++] = argv[i];
        i += 1;
    }

    if (i < argc) {
        parser.argc = argc;
        parser.argv = argv;
        parser.index = i;
        parser.options = &options;

        if (parse_expression(&parser, &options.root_node) != 0) {
            print_usage();
            return 1;
        }
        if (parser.index != argc) {
            print_usage();
            return 1;
        }
    }

    if (start_count == 0) {
        start_paths[start_count++] = ".";
    }
    if (options.root_node < 0) {
        options.root_node = add_node(&options, FIND_NODE_TRUE);
    }

    now = platform_get_epoch_time();
    for (i = 0; i < start_count; ++i) {
        if (find_walk(start_paths[i], &options, now, 0) != 0) {
            return 1;
        }
    }
    return 0;
}
