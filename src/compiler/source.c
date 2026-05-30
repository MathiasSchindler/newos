#include "source.h"

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define COMPILER_SOURCE_CACHE_CAPACITY 256U

typedef struct {
    char path[COMPILER_PATH_CAPACITY];
    char *data;
    size_t size;
} CompilerSourceCacheEntry;

static CompilerSourceCacheEntry source_cache[COMPILER_SOURCE_CACHE_CAPACITY];
static size_t source_cache_count;
static int source_cache_enabled;

static int source_cache_find(const char *path) {
    size_t i;

    if (path == 0 || path[0] == '\0' || (path[0] == '-' && path[1] == '\0')) {
        return -1;
    }
    for (i = 0; i < source_cache_count; ++i) {
        if (rt_strcmp(source_cache[i].path, path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void source_cache_store(const char *path, const CompilerSource *source) {
    CompilerSourceCacheEntry *entry;
    char *copy;

    if (!source_cache_enabled || path == 0 || path[0] == '\0' || (path[0] == '-' && path[1] == '\0') || source_cache_count >= COMPILER_SOURCE_CACHE_CAPACITY) {
        return;
    }
    if (source_cache_find(path) >= 0) {
        return;
    }
    copy = (char *)rt_malloc(source->size + 1U);
    if (copy == 0) {
        return;
    }
    memcpy(copy, source->data, source->size + 1U);
    entry = &source_cache[source_cache_count++];
    rt_copy_string(entry->path, sizeof(entry->path), path);
    entry->data = copy;
    entry->size = source->size;
}

void compiler_source_cache_set_enabled(int enabled) {
    source_cache_enabled = enabled ? 1 : 0;
}

void compiler_source_cache_clear(void) {
    size_t i;

    for (i = 0; i < source_cache_count; ++i) {
        if (source_cache[i].data != 0) {
            rt_free(source_cache[i].data);
        }
        source_cache[i].data = 0;
        source_cache[i].path[0] = '\0';
        source_cache[i].size = 0U;
    }
    source_cache_count = 0U;
}

int compiler_load_source(const char *path, CompilerSource *source_out) {
    int fd;
    int should_close;
    size_t total = 0;
    int cache_index;

    if (source_out == 0) {
        return -1;
    }

    rt_memset(source_out, 0, sizeof(*source_out));
    rt_copy_string(source_out->path, sizeof(source_out->path), (path != 0) ? path : "-");

    if (source_cache_enabled) {
        cache_index = source_cache_find(path);
        if (cache_index >= 0) {
            memcpy(source_out->data, source_cache[cache_index].data, source_cache[cache_index].size + 1U);
            source_out->size = source_cache[cache_index].size;
            return 0;
        }
    }

    if (tool_open_input(path, &fd, &should_close) != 0) {
        return -1;
    }

    for (;;) {
        size_t remaining = sizeof(source_out->data) - total - 1U;
        long bytes_read;

        if (remaining == 0) {
            tool_close_input(fd, should_close);
            return -2;
        }

        bytes_read = platform_read(fd, source_out->data + total, remaining);
        if (bytes_read == 0) {
            break;
        }
        if (bytes_read < 0) {
            tool_close_input(fd, should_close);
            return -1;
        }

        total += (size_t)bytes_read;
    }

    source_out->data[total] = '\0';
    source_out->size = total;
    tool_close_input(fd, should_close);
    source_cache_store(path, source_out);
    return 0;
}
