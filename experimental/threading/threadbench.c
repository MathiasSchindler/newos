#include "concurrency.h"
#include "platform.h"
#include "runtime.h"

#define THREADBENCH_DEFAULT_ITEMS 262144U
#define THREADBENCH_DEFAULT_TASKS 4096U
#define THREADBENCH_DEFAULT_ROUNDS 64U
#define THREADBENCH_DEFAULT_REPEAT 3U
#define THREADBENCH_DEFAULT_MAX_WIDTH 8U
#define THREADBENCH_DEFAULT_MIN_CHUNK 1024U
#define THREADBENCH_U64_MAX_DIV_1E9 18446744073ULL

#if defined(__GNUC__) || defined(__clang__)
#define THREADBENCH_NOINLINE __attribute__((noinline))
#else
#define THREADBENCH_NOINLINE
#endif

typedef struct {
    size_t items;
    size_t tasks;
    size_t min_chunk;
    unsigned int rounds;
    unsigned int repeat;
    unsigned int max_width;
    const char *case_name;
} BenchOptions;

typedef struct {
    unsigned long long *slots;
    unsigned long long *buffer;
    unsigned int rounds;
} RangeState;

typedef struct {
    unsigned long long seed;
    unsigned int rounds;
    unsigned long long *slots;
} TaskArg;

typedef struct {
    unsigned long long best_ns;
    unsigned long long checksum;
    unsigned int effective_width;
    int error;
} BenchResult;

#define BENCH_KIND_MIX 1U
#define BENCH_KIND_MEMORY 2U
#define BENCH_KIND_TASKS 3U
#define BENCH_KIND_OVERHEAD 4U

static unsigned long long mix64(unsigned long long value) {
    value ^= value >> 33U;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33U;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33U;
    return value;
}

static int text_equals(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

static int parse_size(const char *text, size_t *value_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value == 0ULL) {
        return -1;
    }
    *value_out = (size_t)value;
    return 0;
}

static int parse_uint_option(const char *text, unsigned int *value_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value == 0ULL || value > 4294967295ULL) {
        return -1;
    }
    *value_out = (unsigned int)value;
    return 0;
}

static void write_decimal_x100(unsigned long long value_x100) {
    rt_write_uint(1, value_x100 / 100ULL);
    rt_write_char(1, '.');
    if (value_x100 % 100ULL < 10ULL) {
        rt_write_char(1, '0');
    }
    rt_write_uint(1, value_x100 % 100ULL);
}

static unsigned long long units_per_second(size_t units, unsigned long long ns) {
    unsigned long long unit_count = (unsigned long long)units;

    if (ns == 0ULL) {
        return 0ULL;
    }
    if (unit_count <= THREADBENCH_U64_MAX_DIV_1E9) {
        return (unit_count * 1000000000ULL) / ns;
    }
    return unit_count / (ns / 1000000000ULL + 1ULL);
}

static unsigned long long ns_per_unit_x100(size_t units, unsigned long long ns) {
    if (units == 0U) {
        return 0ULL;
    }
    return (ns * 100ULL) / (unsigned long long)units;
}

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " [--case all|mix|memory|tasks|overhead] [--items N] [--tasks N] [--rounds N] [--repeat N] [--max-width N] [--min-chunk N]\n");
}

static int parse_options(int argc, char **argv, BenchOptions *options) {
    int index;

    options->items = THREADBENCH_DEFAULT_ITEMS;
    options->tasks = THREADBENCH_DEFAULT_TASKS;
    options->min_chunk = THREADBENCH_DEFAULT_MIN_CHUNK;
    options->rounds = THREADBENCH_DEFAULT_ROUNDS;
    options->repeat = THREADBENCH_DEFAULT_REPEAT;
    options->max_width = THREADBENCH_DEFAULT_MAX_WIDTH;
    options->case_name = "all";
    for (index = 1; index < argc; ++index) {
        const char *arg = argv[index];
        const char *value;

        if (text_equals(arg, "--help") || text_equals(arg, "-h")) {
            write_usage(argc > 0 ? argv[0] : "threadbench");
            return 1;
        }
        if (index + 1 >= argc) {
            return -1;
        }
        value = argv[++index];
        if (text_equals(arg, "--case")) {
            options->case_name = value;
        } else if (text_equals(arg, "--items")) {
            if (parse_size(value, &options->items) != 0) return -1;
        } else if (text_equals(arg, "--tasks")) {
            if (parse_size(value, &options->tasks) != 0) return -1;
        } else if (text_equals(arg, "--min-chunk")) {
            if (parse_size(value, &options->min_chunk) != 0) return -1;
        } else if (text_equals(arg, "--rounds")) {
            if (parse_uint_option(value, &options->rounds) != 0) return -1;
        } else if (text_equals(arg, "--repeat")) {
            if (parse_uint_option(value, &options->repeat) != 0) return -1;
        } else if (text_equals(arg, "--max-width")) {
            if (parse_uint_option(value, &options->max_width) != 0) return -1;
        } else {
            return -1;
        }
    }
    if (options->max_width > RT_TASK_POOL_MAX_WORKERS) {
        options->max_width = RT_TASK_POOL_MAX_WORKERS;
    }
    return 0;
}

static THREADBENCH_NOINLINE int mix_body(size_t begin, size_t end, unsigned int worker_index, void *arg) {
    RangeState *state = (RangeState *)arg;
    unsigned long long acc = state->slots[worker_index];
    size_t index;

    for (index = begin; index < end; ++index) {
        unsigned long long value = (unsigned long long)index + 0x9e3779b97f4a7c15ULL;
        unsigned int round;

        for (round = 0U; round < state->rounds; ++round) {
            value = mix64(value + (unsigned long long)round);
        }
        acc ^= value;
    }
    state->slots[worker_index] = acc;
    return 0;
}

static THREADBENCH_NOINLINE int memory_body(size_t begin, size_t end, unsigned int worker_index, void *arg) {
    RangeState *state = (RangeState *)arg;
    unsigned long long acc = state->slots[worker_index];
    size_t index;

    for (index = begin; index < end; ++index) {
        unsigned long long value = mix64((unsigned long long)index ^ 0xd6e8feb86659fd93ULL);
        unsigned int round;

        for (round = 0U; round < state->rounds; ++round) {
            value = value * 2862933555777941757ULL + 3037000493ULL;
        }
        state->buffer[index] = value;
        acc += value;
    }
    state->slots[worker_index] = acc;
    return 0;
}

static THREADBENCH_NOINLINE int overhead_body(size_t begin, size_t end, unsigned int worker_index, void *arg) {
    RangeState *state = (RangeState *)arg;

    state->slots[worker_index] += (unsigned long long)(end - begin);
    return 0;
}

static THREADBENCH_NOINLINE int task_body(unsigned int worker_index, void *arg) {
    TaskArg *task = (TaskArg *)arg;
    unsigned long long value = task->seed;
    unsigned int round;

    for (round = 0U; round < task->rounds; ++round) {
        value = mix64(value + (unsigned long long)round);
    }
    task->slots[worker_index] ^= value;
    return 0;
}

static const char *case_name_for_kind(unsigned int kind) {
    if (kind == BENCH_KIND_MIX) {
        return "mix";
    }
    if (kind == BENCH_KIND_MEMORY) {
        return "memory";
    }
    if (kind == BENCH_KIND_TASKS) {
        return "tasks";
    }
    if (kind == BENCH_KIND_OVERHEAD) {
        return "overhead";
    }
    return "unknown";
}

static int kind_selected(const BenchOptions *options, unsigned int kind) {
    return text_equals(options->case_name, "all") || text_equals(options->case_name, case_name_for_kind(kind));
}

static size_t units_for_kind(const BenchOptions *options, unsigned int kind) {
    if (kind == BENCH_KIND_TASKS) {
        return options->tasks;
    }
    return options->items;
}

static void clear_slots(unsigned long long *slots) {
    unsigned int index;

    for (index = 0U; index < RT_TASK_POOL_MAX_WORKERS; ++index) {
        slots[index] = 0ULL;
    }
}

static unsigned long long combine_slots(const unsigned long long *slots, unsigned int width) {
    unsigned long long value = 0ULL;
    unsigned int index;

    if (width == 0U || width > RT_TASK_POOL_MAX_WORKERS) {
        width = RT_TASK_POOL_MAX_WORKERS;
    }
    for (index = 0U; index < width; ++index) {
        value ^= mix64(slots[index] + (unsigned long long)index);
    }
    return value;
}

static int run_range_case(RtTaskPool *pool, const BenchOptions *options, unsigned int kind, unsigned long long *checksum_out) {
    RangeState state;
    unsigned long long slots[RT_TASK_POOL_MAX_WORKERS];
    int result;

    clear_slots(slots);
    state.slots = slots;
    state.buffer = 0;
    state.rounds = kind == BENCH_KIND_OVERHEAD ? 1U : options->rounds;
    if (kind == BENCH_KIND_MEMORY) {
        state.buffer = (unsigned long long *)rt_malloc_array(options->items, sizeof(state.buffer[0]));
        if (state.buffer == 0) {
            return -1;
        }
        result = rt_parallel_for(pool, options->items, options->min_chunk, memory_body, &state);
        rt_free(state.buffer);
    } else if (kind == BENCH_KIND_OVERHEAD) {
        result = rt_parallel_for(pool, options->items, 1U, overhead_body, &state);
    } else {
        result = rt_parallel_for(pool, options->items, options->min_chunk, mix_body, &state);
    }
    *checksum_out = combine_slots(slots, rt_task_pool_width(pool));
    return result;
}

static int run_task_case(RtTaskPool *pool, const BenchOptions *options, unsigned long long *checksum_out) {
    RtTaskGroup group;
    TaskArg *tasks;
    unsigned long long slots[RT_TASK_POOL_MAX_WORKERS];
    size_t index;
    int result;

    tasks = (TaskArg *)rt_malloc_array(options->tasks, sizeof(tasks[0]));
    if (tasks == 0) {
        return -1;
    }
    clear_slots(slots);
    if (rt_task_group_begin(pool, &group) != 0) {
        rt_free(tasks);
        return -1;
    }
    for (index = 0U; index < options->tasks; ++index) {
        tasks[index].seed = (unsigned long long)index + 0xa0761d6478bd642fULL;
        tasks[index].rounds = options->rounds;
        tasks[index].slots = slots;
        if (rt_task_group_submit(&group, task_body, &tasks[index]) != 0) {
            (void)rt_task_group_wait(&group);
            rt_free(tasks);
            return -1;
        }
    }
    result = rt_task_group_wait(&group);
    *checksum_out = combine_slots(slots, rt_task_pool_width(pool));
    rt_free(tasks);
    return result;
}

static int run_case_once(RtTaskPool *pool, const BenchOptions *options, unsigned int kind, unsigned long long *checksum_out) {
    if (kind == BENCH_KIND_TASKS) {
        return run_task_case(pool, options, checksum_out);
    }
    return run_range_case(pool, options, kind, checksum_out);
}

static BenchResult run_case_width(const BenchOptions *options, unsigned int kind, unsigned int requested_width) {
    BenchResult result;
    RtTaskPool pool;
    unsigned int repeat;

    result.best_ns = 0ULL;
    result.checksum = 0ULL;
    result.effective_width = 1U;
    result.error = 0;
    if (rt_task_pool_init(&pool, requested_width) != 0) {
        rt_task_pool_destroy(&pool);
        if (rt_task_pool_init(&pool, 1U) != 0) {
            result.error = -1;
            return result;
        }
    }
    result.effective_width = rt_task_pool_width(&pool);
    for (repeat = 0U; repeat < options->repeat; ++repeat) {
        unsigned long long checksum = 0ULL;
        unsigned long long start = platform_get_monotonic_time_ns();
        unsigned long long elapsed;

        if (run_case_once(&pool, options, kind, &checksum) != 0) {
            result.error = -1;
            break;
        }
        elapsed = platform_get_monotonic_time_ns() - start;
        if (result.best_ns == 0ULL || elapsed < result.best_ns) {
            result.best_ns = elapsed;
        }
        result.checksum ^= checksum + (unsigned long long)repeat;
    }
    rt_task_pool_destroy(&pool);
    return result;
}

static void write_header(const BenchOptions *options) {
    rt_write_line(1, "# experimental/threading synthetic benchmark");
    rt_write_cstr(1, "# workers_supported=");
    rt_write_uint(1, (unsigned long long)platform_worker_threads_supported());
    rt_write_cstr(1, " detected_width=");
    rt_write_uint(1, (unsigned long long)platform_worker_thread_count());
    rt_write_char(1, '\n');
    rt_write_cstr(1, "# items=");
    rt_write_uint(1, options->items);
    rt_write_cstr(1, " tasks=");
    rt_write_uint(1, options->tasks);
    rt_write_cstr(1, " rounds=");
    rt_write_uint(1, options->rounds);
    rt_write_cstr(1, " repeat=");
    rt_write_uint(1, options->repeat);
    rt_write_cstr(1, " min_chunk=");
    rt_write_uint(1, options->min_chunk);
    rt_write_char(1, '\n');
    rt_write_line(1, "case,requested_width,effective_width,units,min_chunk,best_ns,ns_per_unit,units_per_sec,speedup,checksum");
}

static void write_result_row(const char *name, unsigned int requested_width, size_t units, size_t min_chunk, const BenchResult *result, unsigned long long baseline_ns) {
    unsigned long long speedup_x100 = 0ULL;

    if (result->best_ns != 0ULL) {
        speedup_x100 = (baseline_ns * 100ULL) / result->best_ns;
    }
    rt_write_cstr(1, name);
    rt_write_char(1, ',');
    rt_write_uint(1, requested_width);
    rt_write_char(1, ',');
    rt_write_uint(1, result->effective_width);
    rt_write_char(1, ',');
    rt_write_uint(1, (unsigned long long)units);
    rt_write_char(1, ',');
    rt_write_uint(1, (unsigned long long)min_chunk);
    rt_write_char(1, ',');
    rt_write_uint(1, result->best_ns);
    rt_write_char(1, ',');
    write_decimal_x100(ns_per_unit_x100(units, result->best_ns));
    rt_write_char(1, ',');
    rt_write_uint(1, units_per_second(units, result->best_ns));
    rt_write_char(1, ',');
    write_decimal_x100(speedup_x100);
    rt_write_char(1, ',');
    rt_write_uint(1, result->checksum);
    rt_write_char(1, '\n');
}

static int run_selected_case(const BenchOptions *options, unsigned int kind) {
    BenchResult baseline;
    unsigned int requested_width;
    const char *name = case_name_for_kind(kind);
    size_t units = units_for_kind(options, kind);
    size_t min_chunk = kind == BENCH_KIND_OVERHEAD ? 1U : options->min_chunk;

    baseline = run_case_width(options, kind, 1U);
    if (baseline.error != 0 || baseline.best_ns == 0ULL) {
        rt_write_cstr(2, "threadbench: benchmark failed: ");
        rt_write_line(2, name);
        return -1;
    }
    write_result_row(name, 1U, units, min_chunk, &baseline, baseline.best_ns);
    requested_width = 2U;
    while (requested_width <= options->max_width) {
        BenchResult result = run_case_width(options, kind, requested_width);

        if (result.error != 0 || result.best_ns == 0ULL) {
            rt_write_cstr(2, "threadbench: benchmark failed: ");
            rt_write_line(2, name);
            return -1;
        }
        write_result_row(name, requested_width, units, min_chunk, &result, baseline.best_ns);
        requested_width *= 2U;
    }
    return 0;
}

static int run_benchmarks(const BenchOptions *options) {
    int ran_case = 0;

    write_header(options);
    if (kind_selected(options, BENCH_KIND_MIX)) {
        ran_case = 1;
        if (run_selected_case(options, BENCH_KIND_MIX) != 0) return -1;
    }
    if (kind_selected(options, BENCH_KIND_MEMORY)) {
        ran_case = 1;
        if (run_selected_case(options, BENCH_KIND_MEMORY) != 0) return -1;
    }
    if (kind_selected(options, BENCH_KIND_TASKS)) {
        ran_case = 1;
        if (run_selected_case(options, BENCH_KIND_TASKS) != 0) return -1;
    }
    if (kind_selected(options, BENCH_KIND_OVERHEAD)) {
        ran_case = 1;
        if (run_selected_case(options, BENCH_KIND_OVERHEAD) != 0) return -1;
    }
    if (!ran_case) {
        rt_write_line(2, "threadbench: unknown benchmark case");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    BenchOptions options;
    int parsed = parse_options(argc, argv, &options);

    if (parsed > 0) {
        return 0;
    }
    if (parsed != 0) {
        write_usage(argc > 0 ? argv[0] : "threadbench");
        return 1;
    }
    return run_benchmarks(&options) == 0 ? 0 : 1;
}