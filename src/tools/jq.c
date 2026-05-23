#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define JQ_MAX_INPUT (1024U * 1024U)

static char jq_input[JQ_MAX_INPUT + 1U];
static int raw_output;

static void print_usage(void) {
    tool_write_usage("jq", "[-r] FILTER [FILE]");
}

static int read_all(int fd, char *buffer, size_t capacity, size_t *size_out) {
    size_t used = 0U;
    long bytes;

    while (used < capacity && (bytes = platform_read(fd, buffer + used, capacity - used)) > 0) {
        used += (size_t)bytes;
    }
    if (used == capacity) return -1;
    buffer[used] = '\0';
    *size_out = used;
    return 0;
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *skip_string(const char *p) {
    if (*p != '"') return 0;
    p++;
    while (*p != '\0') {
        if (*p == '\\' && p[1] != '\0') {
            p += 2;
        } else if (*p == '"') {
            return p + 1;
        } else {
            p++;
        }
    }
    return 0;
}

static const char *skip_value(const char *p) {
    int depth = 0;

    p = skip_ws(p);
    if (*p == '"') return skip_string(p);
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = open == '{' ? '}' : ']';
        (void)close;
        while (*p != '\0') {
            if (*p == '"') {
                p = skip_string(p);
                if (p == 0) return 0;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            if (*p == '}' || *p == ']') {
                depth--;
                if (depth == 0) return p + 1;
            }
            p++;
        }
        return 0;
    }
    while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') p++;
    return p;
}

static int string_key_equals(const char *start, const char *key, const char **after_out) {
    const char *p = start;
    size_t i = 0U;

    if (*p != '"') return 0;
    p++;
    while (key[i] != '\0') {
        if (*p == '\\') p++;
        if (*p != key[i]) return 0;
        p++;
        i++;
    }
    if (*p != '"') return 0;
    *after_out = p + 1;
    return 1;
}

static int find_key(const char *object, const char *key, const char **value_start, const char **value_end) {
    const char *p = skip_ws(object);

    if (*p != '{') return -1;
    p++;
    for (;;) {
        const char *after_key;
        const char *end;

        p = skip_ws(p);
        if (*p == '}') return -1;
        if (*p != '"') return -1;
        if (!string_key_equals(p, key, &after_key)) {
            p = skip_string(p);
            if (p == 0) return -1;
            p = skip_ws(p);
            if (*p != ':') return -1;
            p = skip_value(p + 1);
            if (p == 0) return -1;
        } else {
            p = skip_ws(after_key);
            if (*p != ':') return -1;
            *value_start = skip_ws(p + 1);
            end = skip_value(*value_start);
            if (end == 0) return -1;
            *value_end = end;
            return 0;
        }
        p = skip_ws(p);
        if (*p == ',') p++;
    }
}

static int emit_value(const char *start, const char *end) {
    if (raw_output && *start == '"' && end > start + 1 && end[-1] == '"') {
        if (rt_write_all(1, start + 1, (size_t)(end - start - 2)) != 0) return -1;
        return rt_write_char(1, '\n');
    }
    if (rt_write_all(1, start, (size_t)(end - start)) != 0) return -1;
    return rt_write_char(1, '\n');
}

static int run_filter(const char *filter, const char *input) {
    const char *current_start = skip_ws(input);
    const char *current_end = skip_value(current_start);
    size_t pos = 0U;

    if (current_end == 0) return -1;
    if (filter[0] == '.' && filter[1] == '\0') {
        return emit_value(current_start, current_end);
    }
    if (filter[0] != '.') return -1;
    pos = 1U;
    while (filter[pos] != '\0') {
        char key[128];
        size_t len = 0U;
        const char *next_start;
        const char *next_end;

        while (filter[pos] != '\0' && filter[pos] != '.') {
            if (len + 1U >= sizeof(key)) return -1;
            key[len++] = filter[pos++];
        }
        key[len] = '\0';
        if (len == 0U) return -1;
        if (find_key(current_start, key, &next_start, &next_end) != 0) return -1;
        current_start = next_start;
        current_end = next_end;
        if (filter[pos] == '.') pos++;
    }
    return emit_value(current_start, current_end);
}

int main(int argc, char **argv) {
    const char *filter = 0;
    const char *path = "-";
    int argi = 1;
    int fd, should_close;
    size_t size;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-r") == 0 || rt_strcmp(argv[argi], "--raw-output") == 0) {
            raw_output = 1;
        } else if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) {
            print_usage();
            return 0;
        } else {
            break;
        }
        argi++;
    }
    if (argi >= argc) {
        print_usage();
        return 1;
    }
    filter = argv[argi++];
    if (argi < argc) path = argv[argi++];
    if (argi != argc) {
        print_usage();
        return 1;
    }
    if (tool_open_input(path, &fd, &should_close) != 0) {
        tool_write_error("jq", "cannot open ", path);
        return 1;
    }
    if (read_all(fd, jq_input, JQ_MAX_INPUT, &size) != 0) {
        tool_close_input(fd, should_close);
        tool_write_error("jq", "input too large", 0);
        return 1;
    }
    tool_close_input(fd, should_close);
    (void)size;
    if (run_filter(filter, jq_input) != 0) {
        tool_write_error("jq", "filter failed", filter);
        return 1;
    }
    return 0;
}
