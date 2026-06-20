#include "concurrency.h"
#include "platform.h"
#include "runtime.h"

typedef struct {
    const char **paths;
    size_t count;
    unsigned long long checksum;
    unsigned int watchdog_ms;
    volatile unsigned int done;
    volatile unsigned int entered[RT_TASK_POOL_MAX_WORKERS];
    volatile unsigned int opened[RT_TASK_POOL_MAX_WORKERS];
    volatile unsigned int malloced[RT_TASK_POOL_MAX_WORKERS];
    volatile unsigned int reallocs[RT_TASK_POOL_MAX_WORKERS];
    volatile unsigned int read_calls[RT_TASK_POOL_MAX_WORKERS];
    volatile unsigned int read_done[RT_TASK_POOL_MAX_WORKERS];
    volatile unsigned int closed[RT_TASK_POOL_MAX_WORKERS];
    volatile unsigned int freed[RT_TASK_POOL_MAX_WORKERS];
    volatile unsigned int errors[RT_TASK_POOL_MAX_WORKERS];
} ThreadReadState;

typedef struct {
    RtTaskPool *pool;
    ThreadReadState *state;
} ThreadReadWatchdog;

static int text_equals(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

static int parse_uint(const char *text, unsigned int *value_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value == 0ULL || value > 4294967295ULL) {
        return -1;
    }
    *value_out = (unsigned int)value;
    return 0;
}

static unsigned long long mix64(unsigned long long value) {
    value ^= value >> 33U;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33U;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33U;
    return value ^ (value >> 33U);
}

static unsigned long long checksum_bytes(const unsigned char *data, size_t size) {
    unsigned long long value = 0x9e3779b97f4a7c15ULL ^ (unsigned long long)size;
    size_t index;

    for (index = 0U; index < size; ++index) {
        value = mix64(value ^ data[index] ^ ((unsigned long long)index << 8U));
    }
    return value;
}

static void bump_counter(volatile unsigned int *counters, unsigned int worker_index) {
    if (worker_index < RT_TASK_POOL_MAX_WORKERS) {
        (void)__atomic_fetch_add(&counters[worker_index], 1U, __ATOMIC_RELAXED);
    }
}

static int read_all_file(ThreadReadState *state, unsigned int worker_index, const char *path, unsigned char **data_out, size_t *size_out) {
    int fd;
    unsigned char *buffer;
    size_t capacity = 65536U;
    size_t used = 0U;

    *data_out = 0;
    *size_out = 0U;
    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }
    bump_counter(state->opened, worker_index);
    buffer = (unsigned char *)rt_malloc(capacity);
    if (buffer == 0) {
        (void)platform_close(fd);
        return -1;
    }
    bump_counter(state->malloced, worker_index);
    for (;;) {
        long bytes_read;

        if (used == capacity) {
            unsigned char *next;
            size_t next_capacity = capacity * 2U;

            if (next_capacity <= capacity) {
                rt_free(buffer);
                (void)platform_close(fd);
                return -1;
            }
            next = (unsigned char *)rt_realloc(buffer, next_capacity);
            if (next == 0) {
                rt_free(buffer);
                (void)platform_close(fd);
                return -1;
            }
            bump_counter(state->reallocs, worker_index);
            buffer = next;
            capacity = next_capacity;
        }
        bytes_read = platform_read(fd, buffer + used, capacity - used);
        bump_counter(state->read_calls, worker_index);
        if (bytes_read < 0) {
            rt_free(buffer);
            (void)platform_close(fd);
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }
        used += (size_t)bytes_read;
    }
    if (platform_close(fd) != 0) {
        rt_free(buffer);
        return -1;
    }
    bump_counter(state->closed, worker_index);
    *data_out = buffer;
    *size_out = used;
    return 0;
}

static int read_body(size_t begin, size_t end, unsigned int worker_index, void *arg) {
    ThreadReadState *state = (ThreadReadState *)arg;
    unsigned long long local = (unsigned long long)worker_index;
    size_t index;

    for (index = begin; index < end; ++index) {
        unsigned char *data = 0;
        size_t size = 0U;

        bump_counter(state->entered, worker_index);
        if (read_all_file(state, worker_index, state->paths[index], &data, &size) != 0) {
            bump_counter(state->errors, worker_index);
            return -1;
        }
        bump_counter(state->read_done, worker_index);
        local ^= mix64(checksum_bytes(data, size) + index);
        rt_free(data);
        bump_counter(state->freed, worker_index);
    }
    (void)__atomic_fetch_xor(&state->checksum, local, __ATOMIC_RELAXED);
    return 0;
}

static void write_watchdog_snapshot(const ThreadReadWatchdog *watchdog) {
    RtTaskPool *pool = watchdog->pool;
    ThreadReadState *state = watchdog->state;
    unsigned int index;

    rt_write_cstr(2, "# threadread watchdog generation=");
    rt_write_uint(2, __atomic_load_n(&pool->generation, __ATOMIC_ACQUIRE));
    rt_write_cstr(2, " active=");
    rt_write_uint(2, __atomic_load_n(&pool->active_workers, __ATOMIC_ACQUIRE));
    rt_write_cstr(2, " claimed=");
    rt_write_uint(2, __atomic_load_n(&pool->claimed_workers, __ATOMIC_ACQUIRE));
    rt_write_cstr(2, " finished=");
    rt_write_uint(2, __atomic_load_n(&pool->finished_workers, __ATOMIC_ACQUIRE));
    rt_write_cstr(2, " next=");
    rt_write_uint(2, (unsigned long long)__atomic_load_n(&pool->next_index, __ATOMIC_ACQUIRE));
    rt_write_cstr(2, " error=");
    rt_write_uint(2, (unsigned int)__atomic_load_n(&pool->first_error, __ATOMIC_ACQUIRE));
    rt_write_char(2, '\n');
    for (index = 0U; index < RT_TASK_POOL_MAX_WORKERS; ++index) {
        unsigned int entered = __atomic_load_n(&state->entered[index], __ATOMIC_ACQUIRE);
        unsigned int opened = __atomic_load_n(&state->opened[index], __ATOMIC_ACQUIRE);
        unsigned int malloced = __atomic_load_n(&state->malloced[index], __ATOMIC_ACQUIRE);
        unsigned int reallocs = __atomic_load_n(&state->reallocs[index], __ATOMIC_ACQUIRE);
        unsigned int read_calls = __atomic_load_n(&state->read_calls[index], __ATOMIC_ACQUIRE);
        unsigned int read_done = __atomic_load_n(&state->read_done[index], __ATOMIC_ACQUIRE);
        unsigned int closed = __atomic_load_n(&state->closed[index], __ATOMIC_ACQUIRE);
        unsigned int freed = __atomic_load_n(&state->freed[index], __ATOMIC_ACQUIRE);
        unsigned int errors = __atomic_load_n(&state->errors[index], __ATOMIC_ACQUIRE);

        if (entered == 0U && opened == 0U && malloced == 0U && reallocs == 0U && read_calls == 0U && read_done == 0U && closed == 0U && freed == 0U && errors == 0U) {
            continue;
        }
        rt_write_cstr(2, "# worker ");
        rt_write_uint(2, index);
        rt_write_cstr(2, " entered=");
        rt_write_uint(2, entered);
        rt_write_cstr(2, " opened=");
        rt_write_uint(2, opened);
        rt_write_cstr(2, " malloced=");
        rt_write_uint(2, malloced);
        rt_write_cstr(2, " reallocs=");
        rt_write_uint(2, reallocs);
        rt_write_cstr(2, " read_calls=");
        rt_write_uint(2, read_calls);
        rt_write_cstr(2, " read_done=");
        rt_write_uint(2, read_done);
        rt_write_cstr(2, " closed=");
        rt_write_uint(2, closed);
        rt_write_cstr(2, " freed=");
        rt_write_uint(2, freed);
        rt_write_cstr(2, " errors=");
        rt_write_uint(2, errors);
        rt_write_char(2, '\n');
    }
}

static int watchdog_main(void *arg) {
    ThreadReadWatchdog *watchdog = (ThreadReadWatchdog *)arg;
    unsigned long long start;
    unsigned long long deadline;

    if (watchdog == 0 || watchdog->pool == 0 || watchdog->state == 0 || watchdog->state->watchdog_ms == 0U) {
        return 0;
    }
    start = platform_get_monotonic_time_ns();
    deadline = start + (unsigned long long)watchdog->state->watchdog_ms * 1000000ULL;
    while (__atomic_load_n(&watchdog->state->done, __ATOMIC_ACQUIRE) == 0U && platform_get_monotonic_time_ns() < deadline) {
    }
    if (__atomic_load_n(&watchdog->state->done, __ATOMIC_ACQUIRE) == 0U) {
        write_watchdog_snapshot(watchdog);
    }
    return 0;
}

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " [--workers N] [--watchdog-ms N] FILE ...\n");
}

int main(int argc, char **argv) {
    RtTaskPool pool;
    PlatformThread watchdog_thread;
    ThreadReadWatchdog watchdog;
    ThreadReadState state;
    unsigned int workers = 1U;
    unsigned int watchdog_ms = 0U;
    int argi = 1;
    int result;
    int watchdog_started = 0;

    while (argi < argc) {
        const char *arg = argv[argi];

        if (text_equals(arg, "--help") || text_equals(arg, "-h")) {
            write_usage(argc > 0 ? argv[0] : "threadread");
            return 0;
        }
        if (text_equals(arg, "--workers")) {
            if (argi + 1 >= argc || parse_uint(argv[argi + 1], &workers) != 0) {
                write_usage(argc > 0 ? argv[0] : "threadread");
                return 1;
            }
            argi += 2;
            continue;
        }
        if (text_equals(arg, "--watchdog-ms")) {
            if (argi + 1 >= argc || parse_uint(argv[argi + 1], &watchdog_ms) != 0) {
                write_usage(argc > 0 ? argv[0] : "threadread");
                return 1;
            }
            argi += 2;
            continue;
        }
        break;
    }
    if (argi >= argc) {
        write_usage(argc > 0 ? argv[0] : "threadread");
        return 1;
    }
    rt_memset(&pool, 0, sizeof(pool));
    rt_memset(&state, 0, sizeof(state));
    state.watchdog_ms = watchdog_ms;
    state.paths = (const char **)(argv + argi);
    state.count = (size_t)(argc - argi);
    if (rt_task_pool_init(&pool, workers) != 0) {
        return 1;
    }
    rt_memset(&watchdog_thread, 0, sizeof(watchdog_thread));
    watchdog.pool = &pool;
    watchdog.state = &state;
    if (state.watchdog_ms != 0U && platform_thread_start(&watchdog_thread, watchdog_main, &watchdog, RT_TASK_POOL_DEFAULT_STACK_SIZE) == 0) {
        watchdog_started = 1;
    }
    result = rt_parallel_for(&pool, state.count, 1U, read_body, &state);
    __atomic_store_n(&state.done, 1U, __ATOMIC_RELEASE);
    if (watchdog_started) {
        (void)platform_thread_join(&watchdog_thread, 0);
    }
    rt_task_pool_destroy(&pool);
    if (result != 0) {
        return 1;
    }
    rt_write_cstr(1, "files=");
    rt_write_uint(1, (unsigned long long)state.count);
    rt_write_cstr(1, " workers=");
    rt_write_uint(1, workers);
    rt_write_cstr(1, " checksum=");
    rt_write_uint(1, state.checksum);
    rt_write_char(1, '\n');
    return 0;
}