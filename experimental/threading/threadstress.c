#include "concurrency.h"
#include "platform.h"
#include "runtime.h"

#define STRESS_DEFAULT_ITERATIONS 100U
#define STRESS_DEFAULT_ITEMS 65536U
#define STRESS_DEFAULT_ROUNDS 8U
#define STRESS_SLOT_STRIDE 8U
#define STRESS_ERROR_RESULT 17

#if defined(__GNUC__) || defined(__clang__)
#define STRESS_NOINLINE __attribute__((noinline))
#else
#define STRESS_NOINLINE
#endif

typedef struct {
    unsigned int iterations;
    unsigned int max_width;
    size_t items;
    unsigned int rounds;
    unsigned long long seed;
    int quiet;
} StressOptions;

typedef struct {
    unsigned long long *counts;
    unsigned long long *hashes;
    size_t fail_index;
    unsigned int rounds;
    int fail_enabled;
} RangeStress;

typedef struct {
    unsigned long long *counts;
    unsigned long long *hashes;
    unsigned long long seed;
    unsigned int rounds;
} TaskStress;

typedef struct {
    unsigned int iterations;
    unsigned int failures;
    unsigned int forced_errors_seen;
    unsigned long long total_parallel_ns;
    unsigned long long total_group_ns;
    unsigned long long total_pool_ns;
    unsigned long long total_pool_destroy_ns;
    unsigned long long task_submit_ns;
    unsigned long long task_execute_ns;
    unsigned long long allocation_count;
    unsigned long long allocation_bytes;
    unsigned long long chunks_claimed;
    unsigned long long chunk_attempts;
    unsigned long long group_tasks_claimed;
    unsigned long long worker_waits;
    unsigned long long worker_wakes;
    unsigned long long join_waits;
    unsigned long long workers_woken;
    unsigned long long workers_ran;
    unsigned long long idle_worker_completions;
    unsigned long long dispatch_ns;
    unsigned long long join_ns;
    unsigned long long body_ns;
    unsigned long long futex_wait_calls;
    unsigned long long futex_wake_calls;
    unsigned long long futex_wait_eagain;
    unsigned long long futex_wait_eintr;
    unsigned long long cpu_user_ns;
    unsigned long long cpu_system_ns;
    unsigned long long minor_faults;
    unsigned long long major_faults;
    unsigned long long voluntary_context_switches;
    unsigned long long involuntary_context_switches;
    unsigned long long migrations;
} StressSummary;

static unsigned long long stress_mix64(unsigned long long value) {
    value ^= value >> 33U;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33U;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33U;
    return value;
}

static unsigned long long stress_next(unsigned long long *state) {
    *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
    return *state;
}

static int text_equals(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

static int parse_uint_option(const char *text, unsigned int *value_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value == 0ULL || value > 4294967295ULL) {
        return -1;
    }
    *value_out = (unsigned int)value;
    return 0;
}

static int parse_size_option(const char *text, size_t *value_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value == 0ULL) {
        return -1;
    }
    *value_out = (size_t)value;
    return 0;
}

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " [--iterations N] [--max-width N] [--items N] [--rounds N] [--seed N] [--quiet]\n");
}

static int parse_options(int argc, char **argv, StressOptions *options) {
    int index;

    options->iterations = STRESS_DEFAULT_ITERATIONS;
    options->max_width = platform_worker_thread_count();
    options->items = STRESS_DEFAULT_ITEMS;
    options->rounds = STRESS_DEFAULT_ROUNDS;
    options->seed = 0xa0761d6478bd642fULL;
    options->quiet = 0;
    if (options->max_width == 0U || options->max_width > RT_TASK_POOL_MAX_WORKERS) {
        options->max_width = RT_TASK_POOL_MAX_WORKERS;
    }
    for (index = 1; index < argc; ++index) {
        const char *arg = argv[index];
        const char *value;

        if (text_equals(arg, "--help") || text_equals(arg, "-h")) {
            write_usage(argc > 0 ? argv[0] : "threadstress");
            return 1;
        }
        if (text_equals(arg, "--quiet")) {
            options->quiet = 1;
            continue;
        }
        if (index + 1 >= argc) {
            return -1;
        }
        value = argv[++index];
        if (text_equals(arg, "--iterations")) {
            if (parse_uint_option(value, &options->iterations) != 0) return -1;
        } else if (text_equals(arg, "--max-width")) {
            if (parse_uint_option(value, &options->max_width) != 0) return -1;
        } else if (text_equals(arg, "--items")) {
            if (parse_size_option(value, &options->items) != 0) return -1;
        } else if (text_equals(arg, "--rounds")) {
            if (parse_uint_option(value, &options->rounds) != 0) return -1;
        } else if (text_equals(arg, "--seed")) {
            if (rt_parse_uint(value, &options->seed) != 0 || options->seed == 0ULL) return -1;
        } else {
            return -1;
        }
    }
    if (options->max_width == 0U) {
        options->max_width = 1U;
    }
    if (options->max_width > RT_TASK_POOL_MAX_WORKERS) {
        options->max_width = RT_TASK_POOL_MAX_WORKERS;
    }
    return 0;
}

static unsigned long long *slot_for_worker(unsigned long long *slots, unsigned int worker_index) {
    return &slots[(worker_index % RT_TASK_POOL_MAX_WORKERS) * STRESS_SLOT_STRIDE];
}

static const unsigned long long *const_slot_for_worker(const unsigned long long *slots, unsigned int worker_index) {
    return &slots[(worker_index % RT_TASK_POOL_MAX_WORKERS) * STRESS_SLOT_STRIDE];
}

static void clear_slots(unsigned long long *slots) {
    unsigned int index;

    for (index = 0U; index < RT_TASK_POOL_MAX_WORKERS * STRESS_SLOT_STRIDE; ++index) {
        slots[index] = 0ULL;
    }
}

static unsigned long long stress_item_hash(size_t index, unsigned int rounds) {
    unsigned long long value = (unsigned long long)index + 0x9e3779b97f4a7c15ULL;
    unsigned int round;

    for (round = 0U; round < rounds; ++round) {
        value = stress_mix64(value + (unsigned long long)round);
    }
    return value;
}

static unsigned long long expected_range_hash(size_t items, unsigned int rounds) {
    unsigned long long hash = 0ULL;
    size_t index;

    for (index = 0U; index < items; ++index) {
        hash ^= stress_item_hash(index, rounds);
    }
    return hash;
}

static unsigned long long combine_slots(const unsigned long long *slots) {
    unsigned long long value = 0ULL;
    unsigned int index;

    for (index = 0U; index < RT_TASK_POOL_MAX_WORKERS; ++index) {
        value ^= *const_slot_for_worker(slots, index);
    }
    return value;
}

static unsigned long long sum_slots(const unsigned long long *slots) {
    unsigned long long value = 0ULL;
    unsigned int index;

    for (index = 0U; index < RT_TASK_POOL_MAX_WORKERS; ++index) {
        value += *const_slot_for_worker(slots, index);
    }
    return value;
}

static STRESS_NOINLINE int range_body(size_t begin, size_t end, unsigned int worker_index, void *arg) {
    RangeStress *stress = (RangeStress *)arg;
    unsigned long long *count_slot = slot_for_worker(stress->counts, worker_index);
    unsigned long long *hash_slot = slot_for_worker(stress->hashes, worker_index);
    unsigned long long hash = *hash_slot;
    size_t index;

    for (index = begin; index < end; ++index) {
        hash ^= stress_item_hash(index, stress->rounds);
    }
    *count_slot += (unsigned long long)(end - begin);
    *hash_slot = hash;
    if (stress->fail_enabled && stress->fail_index >= begin && stress->fail_index < end) {
        return STRESS_ERROR_RESULT;
    }
    return 0;
}

static STRESS_NOINLINE int task_body(unsigned int worker_index, void *arg) {
    TaskStress *task = (TaskStress *)arg;
    unsigned long long value = task->seed;
    unsigned int round;

    for (round = 0U; round < task->rounds; ++round) {
        value = stress_mix64(value + (unsigned long long)round);
    }
    *slot_for_worker(task->counts, worker_index) += 1ULL;
    *slot_for_worker(task->hashes, worker_index) ^= value;
    return 0;
}

static unsigned long long expected_task_hash(size_t tasks, unsigned int rounds) {
    unsigned long long hash = 0ULL;
    size_t index;

    for (index = 0U; index < tasks; ++index) {
        unsigned long long value = (unsigned long long)index + 0xd6e8feb86659fd93ULL;
        unsigned int round;

        for (round = 0U; round < rounds; ++round) {
            value = stress_mix64(value + (unsigned long long)round);
        }
        hash ^= value;
    }
    return hash;
}

static void accumulate_stats(StressSummary *summary, const RtTaskPoolStats *stats) {
    summary->chunks_claimed += stats->chunks_claimed;
    summary->chunk_attempts += stats->chunk_claim_attempts;
    summary->group_tasks_claimed += stats->group_tasks_claimed;
    summary->worker_waits += stats->worker_waits;
    summary->worker_wakes += stats->worker_wakes;
    summary->join_waits += stats->join_waits;
    summary->workers_woken += stats->workers_woken;
    summary->workers_ran += stats->workers_ran;
    summary->idle_worker_completions += stats->idle_worker_completions;
    summary->dispatch_ns += stats->dispatch_ns;
    summary->join_ns += stats->join_ns;
    summary->body_ns += stats->body_ns;
    summary->task_submit_ns += stats->task_submit_ns;
    summary->task_execute_ns += stats->task_execute_ns;
    summary->allocation_count += stats->allocation_count;
    summary->allocation_bytes += stats->allocation_bytes;
}

static int run_range_check(RtTaskPool *pool, size_t items, size_t min_chunk, unsigned int rounds, int forced_error, StressSummary *summary) {
    RangeStress stress;
    unsigned long long counts[RT_TASK_POOL_MAX_WORKERS * STRESS_SLOT_STRIDE];
    unsigned long long hashes[RT_TASK_POOL_MAX_WORKERS * STRESS_SLOT_STRIDE];
    RtTaskPoolStats stats;
    unsigned long long start;
    int result;

    clear_slots(counts);
    clear_slots(hashes);
    stress.counts = counts;
    stress.hashes = hashes;
    stress.rounds = rounds;
    stress.fail_enabled = forced_error;
    stress.fail_index = items == 0U ? 0U : items / 2U;
    rt_task_pool_reset_stats(pool);
    start = platform_get_monotonic_time_ns();
    result = rt_parallel_for(pool, items, min_chunk, range_body, &stress);
    summary->total_parallel_ns += platform_get_monotonic_time_ns() - start;
    rt_task_pool_get_stats(pool, &stats);
    accumulate_stats(summary, &stats);
    if (forced_error) {
        if (result == STRESS_ERROR_RESULT) {
            summary->forced_errors_seen += 1U;
            return 0;
        }
        return -1;
    }
    if (result != 0) {
        return -1;
    }
    if (sum_slots(counts) != (unsigned long long)items) {
        return -1;
    }
    if (combine_slots(hashes) != expected_range_hash(items, rounds)) {
        return -1;
    }
    return 0;
}

static int run_group_check(RtTaskPool *pool, size_t tasks, unsigned int rounds, StressSummary *summary) {
    RtTaskGroup group;
    TaskStress *task_args;
    unsigned long long counts[RT_TASK_POOL_MAX_WORKERS * STRESS_SLOT_STRIDE];
    unsigned long long hashes[RT_TASK_POOL_MAX_WORKERS * STRESS_SLOT_STRIDE];
    RtTaskPoolStats stats;
    unsigned long long start;
    unsigned long long submit_start;
    size_t index;
    int result;

    rt_task_pool_reset_stats(pool);
    task_args = (TaskStress *)rt_malloc_array(tasks, sizeof(task_args[0]));
    if (task_args == 0) {
        return -1;
    }
    (void)__atomic_fetch_add(&pool->stats.allocation_count, 1ULL, __ATOMIC_RELAXED);
    (void)__atomic_fetch_add(&pool->stats.allocation_bytes, (unsigned long long)(tasks * sizeof(task_args[0])), __ATOMIC_RELAXED);
    clear_slots(counts);
    clear_slots(hashes);
    if (rt_task_group_begin(pool, &group) != 0) {
        rt_free(task_args);
        return -1;
    }
    submit_start = platform_get_monotonic_time_ns();
    for (index = 0U; index < tasks; ++index) {
        task_args[index].counts = counts;
        task_args[index].hashes = hashes;
        task_args[index].seed = (unsigned long long)index + 0xd6e8feb86659fd93ULL;
        task_args[index].rounds = rounds;
        if (rt_task_group_submit(&group, task_body, &task_args[index]) != 0) {
            (void)rt_task_group_wait(&group);
            rt_free(task_args);
            return -1;
        }
    }
    (void)__atomic_fetch_add(&pool->stats.task_submit_ns, platform_get_monotonic_time_ns() - submit_start, __ATOMIC_RELAXED);
    start = platform_get_monotonic_time_ns();
    result = rt_task_group_wait(&group);
    {
        unsigned long long execute_ns = platform_get_monotonic_time_ns() - start;

        summary->total_group_ns += execute_ns;
        (void)__atomic_fetch_add(&pool->stats.task_execute_ns, execute_ns, __ATOMIC_RELAXED);
    }
    rt_task_pool_get_stats(pool, &stats);
    accumulate_stats(summary, &stats);
    rt_free(task_args);
    if (result != 0) {
        return -1;
    }
    if (sum_slots(counts) != (unsigned long long)tasks) {
        return -1;
    }
    if (combine_slots(hashes) != expected_task_hash(tasks, rounds)) {
        return -1;
    }
    return 0;
}

static void write_summary(const StressOptions *options, const StressSummary *summary, unsigned long long elapsed_ns) {
    rt_write_line(1, "# experimental/threading stress");
    rt_write_cstr(1, "iterations=");
    rt_write_uint(1, summary->iterations);
    rt_write_cstr(1, " failures=");
    rt_write_uint(1, summary->failures);
    rt_write_cstr(1, " forced_errors_seen=");
    rt_write_uint(1, summary->forced_errors_seen);
    rt_write_cstr(1, " max_width=");
    rt_write_uint(1, options->max_width);
    rt_write_cstr(1, " items=");
    rt_write_uint(1, (unsigned long long)options->items);
    rt_write_cstr(1, " rounds=");
    rt_write_uint(1, options->rounds);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "elapsed_ns=");
    rt_write_uint(1, elapsed_ns);
    rt_write_cstr(1, " total_pool_ns=");
    rt_write_uint(1, summary->total_pool_ns);
    rt_write_cstr(1, " total_pool_destroy_ns=");
    rt_write_uint(1, summary->total_pool_destroy_ns);
    rt_write_cstr(1, " total_parallel_ns=");
    rt_write_uint(1, summary->total_parallel_ns);
    rt_write_cstr(1, " total_group_ns=");
    rt_write_uint(1, summary->total_group_ns);
    rt_write_cstr(1, " task_submit_ns=");
    rt_write_uint(1, summary->task_submit_ns);
    rt_write_cstr(1, " task_execute_ns=");
    rt_write_uint(1, summary->task_execute_ns);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "chunks=");
    rt_write_uint(1, summary->chunks_claimed);
    rt_write_cstr(1, " chunk_attempts=");
    rt_write_uint(1, summary->chunk_attempts);
    rt_write_cstr(1, " group_tasks=");
    rt_write_uint(1, summary->group_tasks_claimed);
    rt_write_cstr(1, " worker_waits=");
    rt_write_uint(1, summary->worker_waits);
    rt_write_cstr(1, " worker_wakes=");
    rt_write_uint(1, summary->worker_wakes);
    rt_write_cstr(1, " join_waits=");
    rt_write_uint(1, summary->join_waits);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "workers_woken=");
    rt_write_uint(1, summary->workers_woken);
    rt_write_cstr(1, " workers_ran=");
    rt_write_uint(1, summary->workers_ran);
    rt_write_cstr(1, " idle_worker_completions=");
    rt_write_uint(1, summary->idle_worker_completions);
    rt_write_cstr(1, " dispatch_ns=");
    rt_write_uint(1, summary->dispatch_ns);
    rt_write_cstr(1, " join_ns=");
    rt_write_uint(1, summary->join_ns);
    rt_write_cstr(1, " body_ns=");
    rt_write_uint(1, summary->body_ns);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "allocation_count=");
    rt_write_uint(1, summary->allocation_count);
    rt_write_cstr(1, " allocation_bytes=");
    rt_write_uint(1, summary->allocation_bytes);
    rt_write_cstr(1, " futex_wait_calls=");
    rt_write_uint(1, summary->futex_wait_calls);
    rt_write_cstr(1, " futex_wake_calls=");
    rt_write_uint(1, summary->futex_wake_calls);
    rt_write_cstr(1, " futex_wait_eagain=");
    rt_write_uint(1, summary->futex_wait_eagain);
    rt_write_cstr(1, " futex_wait_eintr=");
    rt_write_uint(1, summary->futex_wait_eintr);
    rt_write_char(1, '\n');
    rt_write_cstr(1, "cpu_user_ns=");
    rt_write_uint(1, summary->cpu_user_ns);
    rt_write_cstr(1, " cpu_system_ns=");
    rt_write_uint(1, summary->cpu_system_ns);
    rt_write_cstr(1, " cpu_total_ns=");
    rt_write_uint(1, summary->cpu_user_ns + summary->cpu_system_ns);
    rt_write_cstr(1, " minor_faults=");
    rt_write_uint(1, summary->minor_faults);
    rt_write_cstr(1, " major_faults=");
    rt_write_uint(1, summary->major_faults);
    rt_write_cstr(1, " voluntary_context_switches=");
    rt_write_uint(1, summary->voluntary_context_switches);
    rt_write_cstr(1, " involuntary_context_switches=");
    rt_write_uint(1, summary->involuntary_context_switches);
    rt_write_cstr(1, " migrations=");
    rt_write_uint(1, summary->migrations);
    rt_write_char(1, '\n');
}

static int run_stress(const StressOptions *options) {
    StressSummary summary;
    PlatformProcessUsage usage_before;
    PlatformProcessUsage usage_after;
    PlatformWaitWakeStats wait_wake_stats;
    unsigned long long rng = options->seed;
    unsigned long long start_ns;
    unsigned int iteration_limit = options->iterations;
    unsigned int max_width = options->max_width;
    size_t max_items = options->items;
    unsigned int rounds = options->rounds;
    int quiet = options->quiet;
    unsigned int iteration;

    rt_memset(&summary, 0, sizeof(summary));
    rt_memset(&usage_before, 0, sizeof(usage_before));
    rt_memset(&usage_after, 0, sizeof(usage_after));
    rt_memset(&wait_wake_stats, 0, sizeof(wait_wake_stats));
    platform_wait_wake_stats_reset();
    (void)platform_get_current_process_usage(&usage_before);
    start_ns = platform_get_monotonic_time_ns();
    for (iteration = 0U; iteration < iteration_limit; ++iteration) {
        RtTaskPool pool;
        unsigned int width = 1U + (unsigned int)(stress_next(&rng) % max_width);
        size_t items = 1U + (size_t)(stress_next(&rng) % max_items);
        size_t task_count = 1U + (size_t)(stress_next(&rng) % 4096U);
        size_t chunk_options[6];
        size_t min_chunk;
        unsigned long long pool_start;
        int failed = 0;

        chunk_options[0] = 1U;
        chunk_options[1] = 2U;
        chunk_options[2] = 8U;
        chunk_options[3] = 64U;
        chunk_options[4] = 512U;
        chunk_options[5] = 4096U;
        min_chunk = chunk_options[stress_next(&rng) % 6U];
        pool_start = platform_get_monotonic_time_ns();
        if (rt_task_pool_init(&pool, width) != 0) {
            summary.failures += 1U;
            continue;
        }
        summary.total_pool_ns += platform_get_monotonic_time_ns() - pool_start;
        if (run_range_check(&pool, items, min_chunk, rounds, 0, &summary) != 0) {
            failed = 1;
        }
        if (!failed && run_group_check(&pool, task_count, rounds, &summary) != 0) {
            failed = 1;
        }
        if (!failed && (iteration % 11U) == 0U && run_range_check(&pool, items, min_chunk, rounds, 1, &summary) != 0) {
            failed = 1;
        }
        pool_start = platform_get_monotonic_time_ns();
        rt_task_pool_destroy(&pool);
        summary.total_pool_destroy_ns += platform_get_monotonic_time_ns() - pool_start;
        summary.iterations += 1U;
        if (failed) {
            summary.failures += 1U;
            if (!quiet) {
                rt_write_cstr(2, "threadstress: failure at iteration ");
                rt_write_uint(2, iteration);
                rt_write_char(2, '\n');
            }
        }
    }
    (void)platform_get_current_process_usage(&usage_after);
    platform_wait_wake_stats_get(&wait_wake_stats);
    summary.futex_wait_calls = wait_wake_stats.wait_calls;
    summary.futex_wake_calls = wait_wake_stats.wake_calls;
    summary.futex_wait_eagain = wait_wake_stats.wait_eagain;
    summary.futex_wait_eintr = wait_wake_stats.wait_eintr;
    summary.cpu_user_ns = usage_after.user_time_ns >= usage_before.user_time_ns ? usage_after.user_time_ns - usage_before.user_time_ns : 0ULL;
    summary.cpu_system_ns = usage_after.system_time_ns >= usage_before.system_time_ns ? usage_after.system_time_ns - usage_before.system_time_ns : 0ULL;
    summary.minor_faults = usage_after.minor_faults >= usage_before.minor_faults ? usage_after.minor_faults - usage_before.minor_faults : 0ULL;
    summary.major_faults = usage_after.major_faults >= usage_before.major_faults ? usage_after.major_faults - usage_before.major_faults : 0ULL;
    summary.voluntary_context_switches = usage_after.voluntary_context_switches >= usage_before.voluntary_context_switches ? usage_after.voluntary_context_switches - usage_before.voluntary_context_switches : 0ULL;
    summary.involuntary_context_switches = usage_after.involuntary_context_switches >= usage_before.involuntary_context_switches ? usage_after.involuntary_context_switches - usage_before.involuntary_context_switches : 0ULL;
    summary.migrations = usage_after.migrations >= usage_before.migrations ? usage_after.migrations - usage_before.migrations : 0ULL;
    write_summary(options, &summary, platform_get_monotonic_time_ns() - start_ns);
    return summary.failures == 0U ? 0 : 1;
}

int main(int argc, char **argv) {
    StressOptions options;
    int parsed = parse_options(argc, argv, &options);

    if (parsed > 0) {
        return 0;
    }
    if (parsed != 0) {
        write_usage(argc > 0 ? argv[0] : "threadstress");
        return 1;
    }
    return run_stress(&options);
}
