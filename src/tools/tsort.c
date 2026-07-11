#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    char *name;
    ToolArray outgoing;
    size_t indegree;
    int emitted;
} TsortNode;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[file]");
}

static int find_node(const ToolArray *nodes, const char *name) {
    size_t i;

    for (i = 0U; i < nodes->count; ++i) {
        const TsortNode *node = (const TsortNode *)tool_array_get_const(nodes, i);
        if (rt_strcmp(node->name, name) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int get_or_add_node(ToolArray *nodes, const char *name) {
    int existing = find_node(nodes, name);
    TsortNode *node;
    size_t length;

    if (existing >= 0) {
        return existing;
    }

    node = (TsortNode *)tool_array_append(nodes);
    if (node == 0) return -1;
    length = rt_strlen(name);
    node->name = (char *)rt_malloc(length + 1U);
    if (node->name == 0) {
        nodes->count -= 1U;
        return -1;
    }
    memcpy(node->name, name, length + 1U);
    tool_array_init(&node->outgoing, sizeof(size_t));
    return (int)(nodes->count - 1U);
}

static int add_edge(ToolArray *nodes, size_t from, size_t to) {
    TsortNode *from_node = (TsortNode *)tool_array_get(nodes, from);
    TsortNode *to_node = (TsortNode *)tool_array_get(nodes, to);
    size_t i;
    size_t *edge;

    if (from_node == 0 || to_node == 0) return -1;
    for (i = 0U; i < from_node->outgoing.count; ++i) {
        const size_t *existing = (const size_t *)tool_array_get_const(&from_node->outgoing, i);
        if (*existing == to) return 0;
    }
    edge = (size_t *)tool_array_append(&from_node->outgoing);
    if (edge == 0) return -1;
    *edge = to;
    to_node->indegree += 1U;
    return 0;
}

static int consume_token(ToolArray *nodes, ToolByteBuffer *token, size_t *pending, int *have_pending) {
    int index;

    if (tool_byte_buffer_terminate(token) != 0) return -1;
    index = get_or_add_node(nodes, (const char *)token->data);
    if (index < 0) return -1;
    if (*have_pending) {
        if (add_edge(nodes, *pending, (size_t)index) != 0) return -1;
        *have_pending = 0;
    } else {
        *pending = (size_t)index;
        *have_pending = 1;
    }
    token->size = 0U;
    return 0;
}

static int parse_pairs_from_fd(int fd, ToolArray *nodes) {
    char chunk[2048];
    ToolByteBuffer token;
    size_t pending = 0U;
    int have_pending = 0;
    int result = 0;

    tool_byte_buffer_init(&token);

    for (;;) {
        long bytes_read = platform_read(fd, chunk, sizeof(chunk));
        long i;

        if (bytes_read < 0) {
            result = -1;
            break;
        }
        if (bytes_read == 0) {
            break;
        }

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (rt_is_space(ch)) {
                if (token.size > 0U && consume_token(nodes, &token, &pending, &have_pending) != 0) {
                    result = -1;
                    break;
                }
            } else if (tool_byte_buffer_append_char(&token, ch) != 0) {
                result = -1;
                break;
            }
        }
        if (result != 0) break;
    }

    if (result == 0 && token.size > 0U && consume_token(nodes, &token, &pending, &have_pending) != 0) {
        result = -1;
    }
    tool_byte_buffer_free(&token);
    return result;
}

static void free_nodes(ToolArray *nodes) {
    size_t i;

    for (i = 0U; i < nodes->count; ++i) {
        TsortNode *node = (TsortNode *)tool_array_get(nodes, i);
        rt_free(node->name);
        tool_array_free(&node->outgoing);
    }
    tool_array_free(nodes);
}

int main(int argc, char **argv) {
    ToolArray nodes;
    int fd;
    int should_close = 0;
    size_t emitted_count = 0U;
    size_t i;

    if (argc > 2) {
        print_usage(argv[0]);
        return 1;
    }

    tool_array_init(&nodes, sizeof(TsortNode));

    if (tool_open_input(argc == 2 ? argv[1] : 0, &fd, &should_close) != 0) {
        tool_write_error("tsort", "cannot open ", argc == 2 ? argv[1] : "stdin");
        free_nodes(&nodes);
        return 1;
    }

    if (parse_pairs_from_fd(fd, &nodes) != 0) {
        tool_close_input(fd, should_close);
        tool_write_error("tsort", "failed to parse input", 0);
        free_nodes(&nodes);
        return 1;
    }
    tool_close_input(fd, should_close);

    while (emitted_count < nodes.count) {
        int found = -1;
        TsortNode *node;

        for (i = 0U; i < nodes.count; ++i) {
            node = (TsortNode *)tool_array_get(&nodes, i);
            if (!node->emitted && node->indegree == 0U) {
                found = (int)i;
                break;
            }
        }

        if (found < 0) {
            tool_write_error("tsort", "cycle detected", 0);
            free_nodes(&nodes);
            return 1;
        }

        node = (TsortNode *)tool_array_get(&nodes, (size_t)found);
        node->emitted = 1;
        emitted_count += 1U;
        if (rt_write_line(1, node->name) != 0) {
            free_nodes(&nodes);
            return 1;
        }

        for (i = 0U; i < node->outgoing.count; ++i) {
            const size_t *target_index = (const size_t *)tool_array_get_const(&node->outgoing, i);
            TsortNode *target = (TsortNode *)tool_array_get(&nodes, *target_index);
            if (target->indegree > 0U) {
                target->indegree -= 1U;
            }
        }
    }

    free_nodes(&nodes);
    return 0;
}
