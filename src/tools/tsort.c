#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define TSORT_MAX_NODES 256
#define TSORT_MAX_NAME 128

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[file]");
}

static int find_node(char names[TSORT_MAX_NODES][TSORT_MAX_NAME], size_t count, const char *name) {
    size_t i;

    for (i = 0U; i < count; ++i) {
        if (rt_strcmp(names[i], name) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int get_or_add_node(char names[TSORT_MAX_NODES][TSORT_MAX_NAME], size_t *count, const char *name) {
    int existing = find_node(names, *count, name);

    if (existing >= 0) {
        return existing;
    }

    if (*count >= TSORT_MAX_NODES) {
        return -1;
    }

    rt_copy_string(names[*count], TSORT_MAX_NAME, name);
    *count += 1U;
    return (int)(*count - 1U);
}

static int parse_pairs_from_fd(
    int fd,
    char names[TSORT_MAX_NODES][TSORT_MAX_NAME],
    size_t *node_count,
    unsigned char edges[TSORT_MAX_NODES][TSORT_MAX_NODES],
    unsigned int indegree[TSORT_MAX_NODES]
) {
    char chunk[2048];
    char token[TSORT_MAX_NAME];
    char pending[TSORT_MAX_NAME];
    size_t token_len = 0U;
    int have_pending = 0;

    for (;;) {
        long bytes_read = platform_read(fd, chunk, sizeof(chunk));
        long i;

        if (bytes_read < 0) {
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (rt_is_space(ch)) {
                if (token_len > 0U) {
                    int index;

                    token[token_len] = '\0';
                    index = get_or_add_node(names, node_count, token);
                    if (index < 0) {
                        return -1;
                    }

                    if (!have_pending) {
                        rt_copy_string(pending, sizeof(pending), token);
                        have_pending = 1;
                    } else {
                        int from = find_node(names, *node_count, pending);
                        int to = index;
                        if (from < 0 || to < 0) {
                            return -1;
                        }
                        if (!edges[from][to]) {
                            edges[from][to] = 1U;
                            indegree[to] += 1U;
                        }
                        have_pending = 0;
                    }
                    token_len = 0U;
                }
            } else if (token_len + 1U < sizeof(token)) {
                token[token_len++] = ch;
            }
        }
    }

    if (token_len > 0U) {
        int index;

        token[token_len] = '\0';
        index = get_or_add_node(names, node_count, token);
        if (index < 0) {
            return -1;
        }

        if (have_pending) {
            int from = find_node(names, *node_count, pending);
            int to = index;
            if (from < 0 || to < 0) {
                return -1;
            }
            if (!edges[from][to]) {
                edges[from][to] = 1U;
                indegree[to] += 1U;
            }
        }
    } else if (have_pending) {
        if (get_or_add_node(names, node_count, pending) < 0) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    char names[TSORT_MAX_NODES][TSORT_MAX_NAME];
    unsigned char edges[TSORT_MAX_NODES][TSORT_MAX_NODES];
    unsigned int indegree[TSORT_MAX_NODES];
    unsigned char emitted[TSORT_MAX_NODES];
    size_t node_count = 0U;
    int fd;
    int should_close = 0;
    size_t emitted_count = 0U;
    size_t i;

    if (argc > 2) {
        print_usage(argv[0]);
        return 1;
    }

    rt_memset(edges, 0, sizeof(edges));
    rt_memset(indegree, 0, sizeof(indegree));
    rt_memset(emitted, 0, sizeof(emitted));

    if (tool_open_input(argc == 2 ? argv[1] : 0, &fd, &should_close) != 0) {
        tool_write_error("tsort", "cannot open ", argc == 2 ? argv[1] : "stdin");
        return 1;
    }

    if (parse_pairs_from_fd(fd, names, &node_count, edges, indegree) != 0) {
        tool_close_input(fd, should_close);
        tool_write_error("tsort", "failed to parse input", 0);
        return 1;
    }
    tool_close_input(fd, should_close);

    while (emitted_count < node_count) {
        int found = -1;

        for (i = 0U; i < node_count; ++i) {
            if (!emitted[i] && indegree[i] == 0U) {
                found = (int)i;
                break;
            }
        }

        if (found < 0) {
            tool_write_error("tsort", "cycle detected", 0);
            return 1;
        }

        emitted[found] = 1U;
        emitted_count += 1U;
        rt_write_line(1, names[found]);

        for (i = 0U; i < node_count; ++i) {
            if (edges[found][i]) {
                edges[found][i] = 0U;
                if (indegree[i] > 0U) {
                    indegree[i] -= 1U;
                }
            }
        }
    }

    return 0;
}
