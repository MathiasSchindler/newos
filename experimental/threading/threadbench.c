#include "concurrency.h"
#include "platform.h"
#include "runtime.h"

#define THREADBENCH_DEFAULT_ITEMS 262144U
#define THREADBENCH_DEFAULT_TASKS 4096U
#define THREADBENCH_DEFAULT_ROUNDS 64U
#define THREADBENCH_DEFAULT_REPEAT 3U
#define THREADBENCH_DEFAULT_MAX_WIDTH 8U
#define THREADBENCH_DEFAULT_MIN_CHUNK 1024U
#define THREADBENCH_SLOT_STRIDE 8U
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
    int show_stats;
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
    unsigned long long min_ns;
    unsigned long long median_ns;
    unsigned long long p90_ns;
    unsigned long long max_ns;
    unsigned long long checksum;
    RtTaskPoolStats stats;
    PlatformWaitWakeStats wait_wake_stats;
    unsigned long long pool_init_ns;
    unsigned long long pool_destroy_ns;
    unsigned long long cpu_user_ns;
    unsigned long long cpu_system_ns;
    unsigned long long minor_faults;
    unsigned long long major_faults;
    unsigned long long voluntary_context_switches;
    unsigned long long involuntary_context_switches;
    unsigned long long migrations;
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

static unsigned long long usage_field_delta(unsigned long long after, unsigned long long before) {
    return after >= before ? after - before : 0ULL;
}

static void usage_delta(const PlatformProcessUsage *before, const PlatformProcessUsage *after, PlatformProcessUsage *delta_out) {
    delta_out->user_time_ns = usage_field_delta(after->user_time_ns, before->user_time_ns);
    delta_out->system_time_ns = usage_field_delta(after->system_time_ns, before->system_time_ns);
    delta_out->minor_faults = usage_field_delta(after->minor_faults, before->minor_faults);
    delta_out->major_faults = usage_field_delta(after->major_faults, before->major_faults);
    delta_out->voluntary_context_switches = usage_field_delta(after->voluntary_context_switches, before->voluntary_context_switches);
    delta_out->involuntary_context_switches = usage_field_delta(after->involuntary_context_switches, before->involuntary_context_switches);
    delta_out->migrations = usage_field_delta(after->migrations, before->migrations);
}

static void bench_record_allocation(RtTaskPool *pool, size_t bytes) {
    if (pool != 0) {
        (void)__atomic_fetch_add(&pool->stats.allocation_count, 1ULL, __ATOMIC_RELAXED);
        (void)__atomic_fetch_add(&pool->stats.allocation_bytes, (unsigned long long)bytes, __ATOMIC_RELAXED);
    }
}

static void sort_u64(unsigned long long *values, unsigned int count) {
    unsigned int index;

    for (index = 1U; index < count; ++index) {
        unsigned long long value = values[index];
        unsigned int cursor = index;

        while (cursor > 0U && values[cursor - 1U] > value) {
            values[cursor] = values[cursor - 1U];
            cursor -= 1U;
        }
        values[cursor] = value;
    }
}

static unsigned long long *slot_for_worker(unsigned long long *slots, unsigned int worker_index);
static const unsigned long long *const_slot_for_worker(const unsigned long long *slots, unsigned int worker_index);

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " [--case all|mix|memory|tasks|overhead] [--items N] [--tasks N] [--rounds N] [--repeat N] [--max-width N] [--min-chunk N] [--stats]\n");
}

static int parse_options(int argc, char **argv, BenchOptions *options) {
    int index;

    options->items = THREADBENCH_DEFAULT_ITEMS;
    options->tasks = THREADBENCH_DEFAULT_TASKS;
    options->min_chunk = THREADBENCH_DEFAULT_MIN_CHUNK;
    options->rounds = THREADBENCH_DEFAULT_ROUNDS;
    options->repeat = THREADBENCH_DEFAULT_REPEAT;
    options->max_width = THREADBENCH_DEFAULT_MAX_WIDTH;
    options->show_stats = 0;
    options->case_name = "all";
    for (index = 1; index < argc; ++index) {
        const char *arg = argv[index];
        const char *value;

        if (text_equals(arg, "--help") || text_equals(arg, "-h")) {
            write_usage(argc > 0 ? argv[0] : "threadbench");
            return 1;
        }
        if (text_equals(arg, "--stats")) {
            options->show_stats = 1;
            continue;
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
    unsigned long long *slot = slot_for_worker(state->slots, worker_index);
    unsigned long long acc = *slot;
    size_t index;

    for (index = begin; index < end; ++index) {
        unsigned long long value = (unsigned long long)index + 0x9e3779b97f4a7c15ULL;
        unsigned int round;

        for (round = 0U; round < state->rounds; ++round) {
            value = mix64(value + (unsigned long long)round);
        }
        acc ^= value;
    }
    *slot = acc;
    return 0;
}

static THREADBENCH_NOINLINE int memory_body(size_t begin, size_t end, unsigned int worker_index, void *arg) {
    RangeState *state = (RangeState *)arg;
    unsigned long long *slot = slot_for_worker(state->slots, worker_index);
    unsigned long long acc = *slot;
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
    *slot = acc;
    return 0;
}

static THREADBENCH_NOINLINE int overhead_body(size_t begin, size_t end, unsigned int worker_index, void *arg) {
    RangeState *state = (RangeState *)arg;

    *slot_for_worker(state->slots, worker_index) += (unsigned long long)(end - begin);
    return 0;
}

static THREADBENCH_NOINLINE int task_body(unsigned int worker_index, void *arg) {
    TaskArg *task = (TaskArg *)arg;
    unsigned long long value = task->seed;
    unsigned int round;

    for (round = 0U; round < task->rounds; ++round) {
        value = mix64(value + (unsigned long long)round);
    }
    *slot_for_worker(task->slots, worker_index) ^= value;
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

    for (index = 0U; index < RT_TASK_POOL_MAX_WORKERS * THREADBENCH_SLOT_STRIDE; ++index) {
        slots[index] = 0ULL;
    }
}

static unsigned long long *slot_for_worker(unsigned long long *slots, unsigned int worker_index) {
    return &slots[(worker_index % RT_TASK_POOL_MAX_WORKERS) * THREADBENCH_SLOT_STRIDE];
}

static const unsigned long long *const_slot_for_worker(const unsigned long long *slots, unsigned int worker_index) {
    return &slots[(worker_index % RT_TASK_POOL_MAX_WORKERS) * THREADBENCH_SLOT_STRIDE];
}

static unsigned long long combine_slots(const unsigned long long *slots, unsigned int width) {
    unsigned long long value = 0ULL;
    unsigned int index;

    if (width == 0U || width > RT_TASK_POOL_MAX_WORKERS) {
        width = RT_TASK_POOL_MAX_WORKERS;
    }
    for (index = 0U; index < width; ++index) {
        value ^= mix64(*const_slot_for_worker(slots, index) + (unsigned long long)index);
    }
    return value;
}

static int run_range_case(RtTaskPool *pool, const BenchOptions *options, unsigned int kind, unsigned long long *checksum_out) {
    RangeState state;
    unsigned long long slots[RT_TASK_POOL_MAX_WORKERS * THREADBENCH_SLOT_STRIDE];
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
        bench_record_allocation(pool, options->items * sizeof(state.buffer[0]));
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
    unsigned long long slots[RT_TASK_POOL_MAX_WORKERS * THREADBENCH_SLOT_STRIDE];
    size_t index;
    int result;

    tasks = (TaskArg *)rt_malloc_array(options->tasks, sizeof(tasks[0]));
    if (tasks == 0) {
        return -1;
    }
    bench_record_allocation(pool, options->tasks * sizeof(tasks[0]));
    clear_slots(slots);
    if (rt_task_group_begin(pool, &group) != 0) {
        rt_free(tasks);
        return -1;
    }
    {
        unsigned long long submit_start_ns = platform_get_monotonic_time_ns();

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
        (void)__atomic_fetch_add(&pool->stats.task_submit_ns, platform_get_monotonic_time_ns() - submit_start_ns, __ATOMIC_RELAXED);
    }
    {
        unsigned long long execute_start_ns = platform_get_monotonic_time_ns();

        result = rt_task_group_wait(&group);
        (void)__atomic_fetch_add(&pool->stats.task_execute_ns, platform_get_monotonic_time_ns() - execute_start_ns, __ATOMIC_RELAXED);
    }
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
    unsigned long long *samples;
    unsigned int repeat;
    unsigned int sample_count = 0U;

    result.min_ns = 0ULL;
    result.median_ns = 0ULL;
    result.p90_ns = 0ULL;
    result.max_ns = 0ULL;
    result.checksum = 0ULL;
    rt_memset(&result.stats, 0, sizeof(result.stats));
    rt_memset(&result.wait_wake_stats, 0, sizeof(result.wait_wake_stats));
    result.pool_init_ns = 0ULL;
    result.pool_destroy_ns = 0ULL;
    result.cpu_user_ns = 0ULL;
    result.cpu_system_ns = 0ULL;
    result.minor_faults = 0ULL;
    result.major_faults = 0ULL;
    result.voluntary_context_switches = 0ULL;
    result.involuntary_context_switches = 0ULL;
    result.migrations = 0ULL;
    result.effective_width = 1U;
    result.error = 0;
    samples = (unsigned long long *)rt_malloc_array(options->repeat, sizeof(samples[0]));
    if (samples == 0) {
        result.error = -1;
        return result;
    }
    {
        unsigned long long pool_start_ns = platform_get_monotonic_time_ns();

        if (rt_task_pool_init(&pool, requested_width) != 0) {
            rt_task_pool_destroy(&pool);
            if (rt_task_pool_init(&pool, 1U) != 0) {
                result.error = -1;
                rt_free(samples);
                return result;
            }
        }
        result.pool_init_ns = platform_get_monotonic_time_ns() - pool_start_ns;
    }
    result.effective_width = rt_task_pool_width(&pool);
    for (repeat = 0U; repeat < options->repeat; ++repeat) {
        unsigned long long checksum = 0ULL;
        RtTaskPoolStats stats;
        PlatformWaitWakeStats wait_wake_stats;
        PlatformProcessUsage usage_before;
        PlatformProcessUsage usage_after;
        PlatformProcessUsage usage_delta_sample;
        unsigned long long start;
        unsigned long long elapsed;

        rt_task_pool_reset_stats(&pool);
        platform_wait_wake_stats_reset();
        rt_memset(&usage_before, 0, sizeof(usage_before));
        rt_memset(&usage_after, 0, sizeof(usage_after));
        rt_memset(&usage_delta_sample, 0, sizeof(usage_delta_sample));
        (void)platform_get_current_process_usage(&usage_before);
        start = platform_get_monotonic_time_ns();
        if (run_case_once(&pool, options, kind, &checksum) != 0) {
            result.error = -1;
            break;
        }
        elapsed = platform_get_monotonic_time_ns() - start;
        (void)platform_get_current_process_usage(&usage_after);
        usage_delta(&usage_before, &usage_after, &usage_delta_sample);
        rt_task_pool_get_stats(&pool, &stats);
        platform_wait_wake_stats_get(&wait_wake_stats);
        samples[sample_count++] = elapsed;
        if (result.min_ns == 0ULL || elapsed < result.min_ns) {
            result.min_ns = elapsed;
            result.stats = stats;
            result.wait_wake_stats = wait_wake_stats;
            result.cpu_user_ns = usage_delta_sample.user_time_ns;
            result.cpu_system_ns = usage_delta_sample.system_time_ns;
            result.minor_faults = usage_delta_sample.minor_faults;
            result.major_faults = usage_delta_sample.major_faults;
            result.voluntary_context_switches = usage_delta_sample.voluntary_context_switches;
            result.involuntary_context_switches = usage_delta_sample.involuntary_context_switches;
            result.migrations = usage_delta_sample.migrations;
        }
        result.checksum ^= checksum + (unsigned long long)repeat;
    }
    {
        unsigned long long destroy_start_ns = platform_get_monotonic_time_ns();

        rt_task_pool_destroy(&pool);
        result.pool_destroy_ns = platform_get_monotonic_time_ns() - destroy_start_ns;
    }
    if (result.error == 0 && sample_count != 0U) {
        unsigned int p90_index;

        sort_u64(samples, sample_count);
        result.min_ns = samples[0];
        result.median_ns = samples[sample_count / 2U];
        p90_index = ((sample_count * 9U) + 9U) / 10U;
        if (p90_index == 0U) {
            p90_index = 1U;
        }
        p90_index -= 1U;
        if (p90_index >= sample_count) {
            p90_index = sample_count - 1U;
        }
        result.p90_ns = samples[p90_index];
        result.max_ns = samples[sample_count - 1U];
    }
    rt_free(samples);
    return result;
}

static void write_stats_field(const char *name, unsigned long long value) {
    rt_write_char(1, ' ');
    rt_write_cstr(1, name);
    rt_write_char(1, '=');
    rt_write_uint(1, value);
}

static unsigned int stats_active_workers(const RtTaskPoolStats *stats, unsigned int fallback_width) {
    if (stats->last_active_workers != 0U) {
        return stats->last_active_workers;
    }
    return fallback_width == 0U ? 1U : fallback_width;
}

static void write_worker_min_max(const char *prefix, const unsigned long long *values, unsigned int active_workers) {
    unsigned long long min_value = 0ULL;
    unsigned long long max_value = 0ULL;
    unsigned long long total = 0ULL;
    unsigned int zero_count = 0U;
    unsigned long long imbalance_x100 = 0ULL;
    unsigned int index;

    if (active_workers == 0U || active_workers > RT_TASK_POOL_MAX_WORKERS) {
        active_workers = RT_TASK_POOL_MAX_WORKERS;
    }
    for (index = 0U; index < active_workers; ++index) {
        unsigned long long value = values[index];

        if (index == 0U || value < min_value) {
            min_value = value;
        }
        if (index == 0U || value > max_value) {
            max_value = value;
        }
        if (value == 0ULL) {
            zero_count += 1U;
        }
        total += value;
    }
    if (min_value != 0ULL) {
        imbalance_x100 = (max_value * 100ULL) / min_value;
    } else if (max_value != 0ULL) {
        imbalance_x100 = max_value * 100ULL;
    }
    rt_write_char(1, ' ');
    rt_write_cstr(1, prefix);
    rt_write_cstr(1, "_total=");
    rt_write_uint(1, total);
    rt_write_char(1, ' ');
    rt_write_cstr(1, prefix);
    rt_write_cstr(1, "_min=");
    rt_write_uint(1, min_value);
    rt_write_char(1, ' ');
    rt_write_cstr(1, prefix);
    rt_write_cstr(1, "_max=");
    rt_write_uint(1, max_value);
    rt_write_char(1, ' ');
    rt_write_cstr(1, prefix);
    rt_write_cstr(1, "_zero=");
    rt_write_uint(1, zero_count);
    rt_write_char(1, ' ');
    rt_write_cstr(1, prefix);
    rt_write_cstr(1, "_imbalance=");
    write_decimal_x100(imbalance_x100);
}

static void write_result_stats(const char *name, unsigned int requested_width, const BenchResult *result) {
    const RtTaskPoolStats *stats = &result->stats;
    unsigned int active_workers = stats_active_workers(stats, result->effective_width);

    rt_write_cstr(1, "# stats case=");
    rt_write_cstr(1, name);
    write_stats_field("requested_width", requested_width);
    write_stats_field("effective_width", result->effective_width);
    write_stats_field("active_workers", active_workers);
    write_stats_field("dispatches", stats->dispatches);
    write_stats_field("parallel_dispatches", stats->parallel_dispatches);
    write_stats_field("group_dispatches", stats->group_dispatches);
    write_stats_field("serial_parallel", stats->serial_parallel_calls);
    write_stats_field("serial_group", stats->serial_group_calls);
    write_stats_field("chunk_attempts", stats->chunk_claim_attempts);
    write_stats_field("chunks", stats->chunks_claimed);
    write_stats_field("group_batches", stats->group_batches_claimed);
    write_stats_field("group_tasks", stats->group_tasks_claimed);
    write_stats_field("worker_waits", stats->worker_waits);
    write_stats_field("worker_wakes", stats->worker_wakes);
    write_stats_field("join_waits", stats->join_waits);
    write_stats_field("worker_completions", stats->worker_completions);
    write_stats_field("workers_woken", stats->workers_woken);
    write_stats_field("workers_ran", stats->workers_ran);
    write_stats_field("idle_worker_completions", stats->idle_worker_completions);
    write_stats_field("dispatch_ns", stats->dispatch_ns);
    write_stats_field("join_ns", stats->join_ns);
    write_stats_field("body_ns", stats->body_ns);
    write_stats_field("pool_init_ns", result->pool_init_ns);
    write_stats_field("pool_destroy_ns", result->pool_destroy_ns);
    write_stats_field("task_submit_ns", stats->task_submit_ns);
    write_stats_field("task_execute_ns", stats->task_execute_ns);
    write_stats_field("allocation_count", stats->allocation_count);
    write_stats_field("allocation_bytes", stats->allocation_bytes);
    write_stats_field("futex_wait_calls", result->wait_wake_stats.wait_calls);
    write_stats_field("futex_wake_calls", result->wait_wake_stats.wake_calls);
    write_stats_field("futex_wait_eagain", result->wait_wake_stats.wait_eagain);
    write_stats_field("futex_wait_eintr", result->wait_wake_stats.wait_eintr);
    write_stats_field("cpu_user_ns", result->cpu_user_ns);
    write_stats_field("cpu_system_ns", result->cpu_system_ns);
    write_stats_field("cpu_total_ns", result->cpu_user_ns + result->cpu_system_ns);
    write_stats_field("minor_faults", result->minor_faults);
    write_stats_field("major_faults", result->major_faults);
    write_stats_field("voluntary_context_switches", result->voluntary_context_switches);
    write_stats_field("involuntary_context_switches", result->involuntary_context_switches);
    write_stats_field("migrations", result->migrations);
    write_stats_field("count", (unsigned long long)stats->last_count);
    write_stats_field("requested_min_chunk", (unsigned long long)stats->last_requested_min_chunk);
    write_stats_field("effective_min_chunk", (unsigned long long)stats->last_effective_min_chunk);
    write_stats_field("group_batch_size", (unsigned long long)stats->last_group_batch_size);
    write_worker_min_max("worker_chunks", stats->worker_chunks, active_workers);
    write_worker_min_max("worker_group_tasks", stats->worker_group_tasks, active_workers);
    rt_write_char(1, '\n');
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
    rt_write_line(1, "case,requested_width,effective_width,active_workers,units,requested_min_chunk,effective_min_chunk,min_ns,median_ns,p90_ns,max_ns,ns_per_unit,units_per_sec,speedup,checksum");
}

static unsigned int result_active_workers(const BenchResult *result) {
    if (result->stats.last_active_workers != 0U) {
        return result->stats.last_active_workers;
    }
    return result->effective_width == 0U ? 1U : result->effective_width;
}

static size_t result_effective_min_chunk(const BenchResult *result, size_t requested_min_chunk) {
    if (result->stats.last_group_batch_size != 0U) {
        return result->stats.last_group_batch_size;
    }
    if (result->stats.last_effective_min_chunk != 0U) {
        return result->stats.last_effective_min_chunk;
    }
    return requested_min_chunk;
}

static void write_result_row(const char *name, unsigned int requested_width, size_t units, size_t min_chunk, const BenchResult *result, unsigned long long baseline_ns) {
    unsigned long long speedup_x100 = 0ULL;
    unsigned long long timing_ns = result->median_ns != 0ULL ? result->median_ns : result->min_ns;

    if (timing_ns != 0ULL) {
        speedup_x100 = (baseline_ns * 100ULL) / timing_ns;
    }
    rt_write_cstr(1, name);
    rt_write_char(1, ',');
    rt_write_uint(1, requested_width);
    rt_write_char(1, ',');
    rt_write_uint(1, result->effective_width);
    rt_write_char(1, ',');
    rt_write_uint(1, result_active_workers(result));
    rt_write_char(1, ',');
    rt_write_uint(1, (unsigned long long)units);
    rt_write_char(1, ',');
    rt_write_uint(1, (unsigned long long)min_chunk);
    rt_write_char(1, ',');
    rt_write_uint(1, (unsigned long long)result_effective_min_chunk(result, min_chunk));
    rt_write_char(1, ',');
    rt_write_uint(1, result->min_ns);
    rt_write_char(1, ',');
    rt_write_uint(1, result->median_ns);
    rt_write_char(1, ',');
    rt_write_uint(1, result->p90_ns);
    rt_write_char(1, ',');
    rt_write_uint(1, result->max_ns);
    rt_write_char(1, ',');
    write_decimal_x100(ns_per_unit_x100(units, timing_ns));
    rt_write_char(1, ',');
    rt_write_uint(1, units_per_second(units, timing_ns));
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
    if (baseline.error != 0 || baseline.median_ns == 0ULL) {
        rt_write_cstr(2, "threadbench: benchmark failed: ");
        rt_write_line(2, name);
        return -1;
    }
    write_result_row(name, 1U, units, min_chunk, &baseline, baseline.median_ns);
    if (options->show_stats) {
        write_result_stats(name, 1U, &baseline);
    }
    requested_width = 2U;
    while (requested_width <= options->max_width) {
        BenchResult result = run_case_width(options, kind, requested_width);

        if (result.error != 0 || result.median_ns == 0ULL) {
            rt_write_cstr(2, "threadbench: benchmark failed: ");
            rt_write_line(2, name);
            return -1;
        }
        write_result_row(name, requested_width, units, min_chunk, &result, baseline.median_ns);
        if (options->show_stats) {
            write_result_stats(name, requested_width, &result);
        }
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