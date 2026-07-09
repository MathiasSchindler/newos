#include "concurrency.h"

#define RT_TASK_WORK_NONE 0U
#define RT_TASK_WORK_PARALLEL 1U
#define RT_TASK_WORK_GROUP 2U
#define RT_TASK_POOL_CHUNKS_PER_WORKER 8U
#ifndef RT_TASK_WORKER_SPIN_LIMIT
#define RT_TASK_WORKER_SPIN_LIMIT 0U
#endif
#ifndef RT_TASK_JOIN_SPIN_LIMIT
#define RT_TASK_JOIN_SPIN_LIMIT 0U
#endif

#if RT_TASK_WORKER_SPIN_LIMIT > 0U || RT_TASK_JOIN_SPIN_LIMIT > 0U
static void rt_task_pool_cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}
#endif

static void rt_task_pool_clear(RtTaskPool *pool) {
    rt_memset(pool, 0, sizeof(*pool));
}

static void rt_task_pool_record_error(RtTaskPool *pool, int error) {
    int expected = 0;

    if (error == 0) {
        return;
    }
    if (__atomic_load_n(&pool->first_error, __ATOMIC_ACQUIRE) == 0) {
        __atomic_compare_exchange_n(&pool->first_error, &expected, error, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
}

#if NEWOS_RUNTIME_TASK_STATS
static void rt_task_pool_stat_add(unsigned long long *field, unsigned long long value) {
    (void)__atomic_fetch_add(field, value, __ATOMIC_RELAXED);
}

static void rt_task_pool_stat_store_uint(unsigned int *field, unsigned int value) {
    __atomic_store_n(field, value, __ATOMIC_RELAXED);
}

static void rt_task_pool_stat_store_size(size_t *field, size_t value) {
    __atomic_store_n(field, value, __ATOMIC_RELAXED);
}

static unsigned long long rt_task_pool_now_ns(void) {
    return platform_get_monotonic_time_ns();
}

static void rt_task_pool_stat_add_worker(unsigned long long *fields, unsigned int worker_index, unsigned long long value) {
    if (worker_index < RT_TASK_POOL_MAX_WORKERS) {
        rt_task_pool_stat_add(&fields[worker_index], value);
    }
}
#else
#define rt_task_pool_stat_add(field, value) ((void)0)
#define rt_task_pool_stat_store_uint(field, value) ((void)0)
#define rt_task_pool_stat_store_size(field, value) ((void)0)
#define rt_task_pool_stat_add_worker(fields, worker_index, value) ((void)0)
#endif

static size_t rt_task_pool_next_chunk(RtTaskPool *pool, size_t *end_out) {
    size_t begin;
    size_t end;
    size_t min_chunk = pool->min_chunk == 0U ? 1U : pool->min_chunk;

    for (;;) {
        rt_task_pool_stat_add(&pool->stats.chunk_claim_attempts, 1ULL);
        begin = __atomic_load_n(&pool->next_index, __ATOMIC_ACQUIRE);
        if (begin >= pool->count) {
            *end_out = begin;
            return begin;
        }
        end = begin + min_chunk;
        if (end < begin || end > pool->count) {
            end = pool->count;
        }
        if (__atomic_compare_exchange_n(&pool->next_index, &begin, end, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            rt_task_pool_stat_add(&pool->stats.chunks_claimed, 1ULL);
            *end_out = end;
            return begin;
        }
    }
}

static size_t rt_task_pool_effective_min_chunk(const RtTaskPool *pool, size_t count, size_t min_chunk) {
    unsigned int width = pool != 0 && pool->native_width != 0U ? pool->native_width : 1U;
    size_t target_chunks;
    size_t auto_chunk;

    if (min_chunk == 0U) {
        min_chunk = 1U;
    }
    target_chunks = (size_t)width * RT_TASK_POOL_CHUNKS_PER_WORKER;
    if (target_chunks == 0U || count <= target_chunks) {
        return min_chunk;
    }
    auto_chunk = (count + target_chunks - 1U) / target_chunks;
    return auto_chunk > min_chunk ? auto_chunk : min_chunk;
}

static unsigned int rt_task_pool_active_for_units(const RtTaskPool *pool, size_t count, size_t unit_size) {
    unsigned int native_width = pool != 0 && pool->native_width != 0U ? pool->native_width : 1U;
    size_t units;

    if (native_width <= 1U || count <= 1U) {
        return 1U;
    }
    if (unit_size == 0U) {
        unit_size = 1U;
    }
    units = (count + unit_size - 1U) / unit_size;
    if (units == 0U) {
        return 1U;
    }
    return units < (size_t)native_width ? (unsigned int)units : native_width;
}

static void rt_task_pool_run_parallel_worker(RtTaskPool *pool, unsigned int worker_index) {
    unsigned int claimed_any = 0U;

    for (;;) {
        size_t end = 0U;
        size_t begin = rt_task_pool_next_chunk(pool, &end);

        if (begin >= end) {
            break;
        }
        claimed_any = 1U;
        rt_task_pool_stat_add_worker(pool->stats.worker_chunks, worker_index, 1ULL);
        int worker_error = pool->parallel_body(begin, end, worker_index, pool->work_arg);
        rt_task_pool_record_error(pool, worker_error);
    }
    if (claimed_any != 0U) {
        rt_task_pool_stat_add(&pool->stats.workers_ran, 1ULL);
    }
}

static void rt_task_pool_run_group_worker(RtTaskPool *pool, unsigned int worker_index) {
    RtTaskGroup *group = pool->group;
    size_t batch_size = pool->min_chunk == 0U ? 1U : pool->min_chunk;
    unsigned int claimed_any = 0U;

    for (;;) {
        size_t begin;
        size_t end;
        size_t index;

        begin = __atomic_fetch_add(&pool->next_index, batch_size, __ATOMIC_ACQ_REL);
        if (group == 0 || begin >= group->count) {
            break;
        }
        claimed_any = 1U;
        end = begin + batch_size;
        if (end < begin || end > group->count) {
            end = group->count;
        }
        rt_task_pool_stat_add(&pool->stats.group_batches_claimed, 1ULL);
        rt_task_pool_stat_add(&pool->stats.group_tasks_claimed, (unsigned long long)(end - begin));
        rt_task_pool_stat_add_worker(pool->stats.worker_group_tasks, worker_index, (unsigned long long)(end - begin));
        for (index = begin; index < end; ++index) {
            int task_error = group->tasks[index].function(worker_index, group->tasks[index].arg);
            rt_task_pool_record_error(pool, task_error);
        }
    }
    if (claimed_any != 0U) {
        rt_task_pool_stat_add(&pool->stats.workers_ran, 1ULL);
    }
}

static void rt_task_pool_run_work(RtTaskPool *pool, unsigned int worker_index) {
    unsigned int kind = __atomic_load_n(&pool->work_kind, __ATOMIC_ACQUIRE);

    if (kind == RT_TASK_WORK_PARALLEL) {
        rt_task_pool_run_parallel_worker(pool, worker_index);
    } else if (kind == RT_TASK_WORK_GROUP) {
        rt_task_pool_run_group_worker(pool, worker_index);
    }
}

static int rt_task_worker_main(void *arg) {
    RtTaskWorker *worker = (RtTaskWorker *)arg;
    RtTaskPool *pool = worker != 0 ? worker->pool : 0;
    unsigned int observed_generation = 0U;

    (void)worker;
    if (pool == 0) {
        return -1;
    }

    for (;;) {
        unsigned int generation;

        generation = __atomic_load_n(&pool->generation, __ATOMIC_ACQUIRE);
        while (generation == observed_generation && __atomic_load_n(&pool->stop, __ATOMIC_ACQUIRE) == 0U) {
#if RT_TASK_WORKER_SPIN_LIMIT > 0U
            unsigned int spins;

            for (spins = 0U; spins < RT_TASK_WORKER_SPIN_LIMIT; ++spins) {
                rt_task_pool_cpu_relax();
                generation = __atomic_load_n(&pool->generation, __ATOMIC_ACQUIRE);
                if (generation != observed_generation || __atomic_load_n(&pool->stop, __ATOMIC_ACQUIRE) != 0U) {
                    break;
                }
            }
            rt_task_pool_stat_add(&pool->stats.worker_spin_loops, (unsigned long long)spins);
            if (generation != observed_generation || __atomic_load_n(&pool->stop, __ATOMIC_ACQUIRE) != 0U) {
                break;
            }
#endif
            rt_task_pool_stat_add(&pool->stats.worker_waits, 1ULL);
            platform_wait_word(&pool->generation, observed_generation);
            generation = __atomic_load_n(&pool->generation, __ATOMIC_ACQUIRE);
        }
        if (__atomic_load_n(&pool->stop, __ATOMIC_ACQUIRE) != 0U) {
            break;
        }
        observed_generation = generation;
        for (;;) {
            unsigned int participant_index;
            unsigned int active_workers = __atomic_load_n(&pool->active_workers, __ATOMIC_ACQUIRE);

            participant_index = __atomic_load_n(&pool->claimed_workers, __ATOMIC_ACQUIRE);
            if (participant_index >= active_workers) {
                break;
            }
            if (__atomic_compare_exchange_n(&pool->claimed_workers, &participant_index, participant_index + 1U, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                rt_task_pool_run_work(pool, participant_index);
                rt_task_pool_stat_add(&pool->stats.worker_completions, 1ULL);
                if (__atomic_fetch_add(&pool->finished_workers, 1U, __ATOMIC_ACQ_REL) + 1U >= active_workers) {
                    rt_task_pool_stat_add(&pool->stats.worker_wakes, 1ULL);
                    platform_wake_word_all(&pool->finished_workers);
                }
                break;
            }
        }
    }
    return 0;
}

static int rt_task_pool_start_workers(RtTaskPool *pool) {
    unsigned int index;

    for (index = 1U; index < pool->width; ++index) {
        pool->workers[index].index = index;
        pool->workers[index].pool = pool;
        rt_arena_init(&pool->workers[index].arena, 0U);
        if (platform_worker_thread_start(&pool->workers[index].thread, rt_task_worker_main, &pool->workers[index], RT_TASK_POOL_DEFAULT_STACK_SIZE) != 0) {
            pool->native_width = index;
            return -1;
        }
    }
    pool->native_width = pool->width;
    return 0;
}

int rt_task_pool_init(RtTaskPool *pool, unsigned int worker_count) {
    unsigned int platform_width;

    if (pool == 0) {
        return -1;
    }
    rt_task_pool_clear(pool);
    platform_width = platform_worker_thread_count();
    if (worker_count == 0U) {
        worker_count = platform_width;
    }
    if (worker_count == 0U) {
        worker_count = 1U;
    }
    if (worker_count > RT_TASK_POOL_MAX_WORKERS) {
        worker_count = RT_TASK_POOL_MAX_WORKERS;
    }
    if (!platform_worker_threads_supported()) {
        worker_count = 1U;
    }
    pool->width = worker_count;
    pool->native_width = 1U;
    pool->workers[0].index = 0U;
    pool->workers[0].pool = pool;
    rt_arena_init(&pool->workers[0].arena, 0U);
    if (worker_count <= 1U) {
        return 0;
    }
    if (rt_task_pool_start_workers(pool) != 0) {
        rt_task_pool_destroy(pool);
        rt_task_pool_clear(pool);
        pool->width = 1U;
        pool->native_width = 1U;
        pool->workers[0].index = 0U;
        pool->workers[0].pool = pool;
        rt_arena_init(&pool->workers[0].arena, 0U);
        return -1;
    }
    return 0;
}

void rt_task_pool_destroy(RtTaskPool *pool) {
    unsigned int index;

    if (pool == 0 || pool->width == 0U) {
        return;
    }
    __atomic_store_n(&pool->stop, 1U, __ATOMIC_RELEASE);
    __atomic_fetch_add(&pool->generation, 1U, __ATOMIC_ACQ_REL);
    platform_wake_word_all(&pool->generation);
    for (index = 1U; index < pool->native_width; ++index) {
        (void)platform_worker_thread_join(&pool->workers[index].thread, 0);
    }
    for (index = 0U; index < pool->width; ++index) {
        rt_arena_destroy(&pool->workers[index].arena);
    }
    rt_task_pool_clear(pool);
}

unsigned int rt_task_pool_width(const RtTaskPool *pool) {
    return pool == 0 || pool->width == 0U ? 1U : pool->width;
}

RtArena *rt_task_pool_worker_arena(RtTaskPool *pool, unsigned int worker_index) {
    if (pool == 0 || worker_index >= pool->width) {
        return 0;
    }
    return &pool->workers[worker_index].arena;
}

void rt_task_pool_reset_stats(RtTaskPool *pool) {
#if NEWOS_RUNTIME_TASK_STATS
    unsigned int index;
#endif

    if (pool == 0) {
        return;
    }
#if NEWOS_RUNTIME_TASK_STATS
    __atomic_store_n(&pool->stats.dispatches, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.parallel_dispatches, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.group_dispatches, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.serial_parallel_calls, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.serial_group_calls, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.chunk_claim_attempts, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.chunks_claimed, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.group_batches_claimed, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.group_tasks_claimed, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.worker_waits, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.worker_wakes, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.join_waits, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.worker_completions, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.workers_woken, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.workers_ran, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.idle_worker_completions, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.dispatch_ns, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.join_ns, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.body_ns, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.worker_spin_loops, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.join_spin_loops, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.task_submit_ns, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.task_execute_ns, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.allocation_count, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&pool->stats.allocation_bytes, 0ULL, __ATOMIC_RELAXED);
    rt_task_pool_stat_store_uint(&pool->stats.last_requested_width, 0U);
    rt_task_pool_stat_store_uint(&pool->stats.last_effective_width, 0U);
    rt_task_pool_stat_store_uint(&pool->stats.last_active_workers, 0U);
    rt_task_pool_stat_store_size(&pool->stats.last_count, 0U);
    rt_task_pool_stat_store_size(&pool->stats.last_requested_min_chunk, 0U);
    rt_task_pool_stat_store_size(&pool->stats.last_effective_min_chunk, 0U);
    rt_task_pool_stat_store_size(&pool->stats.last_group_batch_size, 0U);
    for (index = 0U; index < RT_TASK_POOL_MAX_WORKERS; ++index) {
        __atomic_store_n(&pool->stats.worker_chunks[index], 0ULL, __ATOMIC_RELAXED);
        __atomic_store_n(&pool->stats.worker_group_tasks[index], 0ULL, __ATOMIC_RELAXED);
    }
#endif
}

void rt_task_pool_get_stats(const RtTaskPool *pool, RtTaskPoolStats *stats_out) {
    if (stats_out == 0) {
        return;
    }
    if (pool == 0) {
        rt_memset(stats_out, 0, sizeof(*stats_out));
        return;
    }
#if NEWOS_RUNTIME_TASK_STATS
    *stats_out = pool->stats;
#else
    rt_memset(stats_out, 0, sizeof(*stats_out));
#endif
}

static int rt_task_pool_run_serial_parallel(size_t count, size_t min_chunk, RtParallelBody body, void *arg) {
    size_t begin = 0U;
    int first_error = 0;

    if (min_chunk == 0U) {
        min_chunk = count == 0U ? 1U : count;
    }
    while (begin < count) {
        size_t end = begin + min_chunk;
        int result;

        if (end < begin || end > count) {
            end = count;
        }
        result = body(begin, end, 0U, arg);
        if (first_error == 0 && result != 0) {
            first_error = result;
        }
        begin = end;
    }
    return first_error;
}

static int rt_task_pool_dispatch(RtTaskPool *pool, unsigned int work_kind) {
    unsigned int expected;
#if NEWOS_RUNTIME_TASK_STATS
    unsigned long long dispatch_start_ns;
    unsigned long long body_start_ns;
    unsigned long long body_end_ns;
    unsigned long long join_start_ns;
    unsigned long long workers_ran_before;
    unsigned long long workers_ran;

    dispatch_start_ns = rt_task_pool_now_ns();
    workers_ran_before = __atomic_load_n(&pool->stats.workers_ran, __ATOMIC_RELAXED);
#endif
    rt_task_pool_stat_add(&pool->stats.dispatches, 1ULL);
    if (work_kind == RT_TASK_WORK_PARALLEL) {
        rt_task_pool_stat_add(&pool->stats.parallel_dispatches, 1ULL);
    } else if (work_kind == RT_TASK_WORK_GROUP) {
        rt_task_pool_stat_add(&pool->stats.group_dispatches, 1ULL);
    }
    pool->work_kind = work_kind;
    pool->first_error = 0;
    pool->next_index = 0U;
    if (work_kind == RT_TASK_WORK_PARALLEL) {
        pool->active_workers = rt_task_pool_active_for_units(pool, pool->count, pool->min_chunk);
    } else if (work_kind == RT_TASK_WORK_GROUP && pool->group != 0) {
        pool->active_workers = rt_task_pool_active_for_units(pool, pool->group->count, pool->min_chunk);
    } else {
        pool->active_workers = pool->native_width;
    }
    if (pool->active_workers == 0U) {
        pool->active_workers = 1U;
    }
    pool->claimed_workers = 1U;
    pool->finished_workers = 0U;
    rt_task_pool_stat_add(&pool->stats.workers_woken, pool->active_workers > 1U ? (unsigned long long)(pool->active_workers - 1U) : 0ULL);
    __atomic_fetch_add(&pool->generation, 1U, __ATOMIC_ACQ_REL);
    if (pool->active_workers > 1U) {
        rt_task_pool_stat_add(&pool->stats.worker_wakes, 1ULL);
        platform_wake_word_count(&pool->generation, pool->active_workers - 1U);
    }
#if NEWOS_RUNTIME_TASK_STATS
    body_start_ns = rt_task_pool_now_ns();
#endif
    rt_task_pool_run_work(pool, 0U);
#if NEWOS_RUNTIME_TASK_STATS
    body_end_ns = rt_task_pool_now_ns();
    rt_task_pool_stat_add(&pool->stats.body_ns, body_end_ns - body_start_ns);
#endif
    rt_task_pool_stat_add(&pool->stats.worker_completions, 1ULL);
    if (__atomic_fetch_add(&pool->finished_workers, 1U, __ATOMIC_ACQ_REL) + 1U >= pool->active_workers) {
        rt_task_pool_stat_add(&pool->stats.worker_wakes, 1ULL);
        platform_wake_word_all(&pool->finished_workers);
    }
#if NEWOS_RUNTIME_TASK_STATS
    rt_task_pool_stat_add(&pool->stats.dispatch_ns, body_start_ns - dispatch_start_ns);
    join_start_ns = rt_task_pool_now_ns();
#endif
    for (;;) {
        expected = __atomic_load_n(&pool->finished_workers, __ATOMIC_ACQUIRE);
        if (expected >= pool->active_workers) {
            break;
        }
#if RT_TASK_JOIN_SPIN_LIMIT > 0U
        {
            unsigned int spins;

        for (spins = 0U; spins < RT_TASK_JOIN_SPIN_LIMIT; ++spins) {
            rt_task_pool_cpu_relax();
            expected = __atomic_load_n(&pool->finished_workers, __ATOMIC_ACQUIRE);
            if (expected >= pool->active_workers) {
                break;
            }
        }
        rt_task_pool_stat_add(&pool->stats.join_spin_loops, (unsigned long long)spins);
        if (expected >= pool->active_workers) {
            break;
        }
        }
    #endif
        rt_task_pool_stat_add(&pool->stats.join_waits, 1ULL);
        platform_wait_word(&pool->finished_workers, expected);
    }
#if NEWOS_RUNTIME_TASK_STATS
    rt_task_pool_stat_add(&pool->stats.join_ns, rt_task_pool_now_ns() - join_start_ns);
    workers_ran = __atomic_load_n(&pool->stats.workers_ran, __ATOMIC_RELAXED) - workers_ran_before;
    if ((unsigned long long)pool->active_workers > workers_ran) {
        rt_task_pool_stat_add(&pool->stats.idle_worker_completions, (unsigned long long)pool->active_workers - workers_ran);
    }
#endif
    __atomic_store_n(&pool->work_kind, RT_TASK_WORK_NONE, __ATOMIC_RELEASE);
    return pool->first_error;
}

int rt_parallel_for(RtTaskPool *pool, size_t count, size_t min_chunk, RtParallelBody body, void *arg) {
    size_t effective_min_chunk = min_chunk == 0U ? 1U : min_chunk;

    if (body == 0) {
        return -1;
    }
    if (count == 0U) {
        return 0;
    }
    if (pool != 0) {
        effective_min_chunk = rt_task_pool_effective_min_chunk(pool, count, min_chunk);

        rt_task_pool_stat_store_uint(&pool->stats.last_requested_width, pool->width);
        rt_task_pool_stat_store_uint(&pool->stats.last_effective_width, pool->native_width);
        rt_task_pool_stat_store_size(&pool->stats.last_count, count);
        rt_task_pool_stat_store_size(&pool->stats.last_requested_min_chunk, min_chunk);
        rt_task_pool_stat_store_size(&pool->stats.last_effective_min_chunk, effective_min_chunk);
        rt_task_pool_stat_store_size(&pool->stats.last_group_batch_size, 0U);
        if (pool->native_width > 1U && count > effective_min_chunk && count != 1U) {
            rt_task_pool_stat_store_uint(&pool->stats.last_active_workers, rt_task_pool_active_for_units(pool, count, effective_min_chunk));
        } else {
            rt_task_pool_stat_store_uint(&pool->stats.last_active_workers, 1U);
        }
    }
    if (pool == 0 || pool->native_width <= 1U || count <= effective_min_chunk || count == 1U) {
        if (pool != 0) {
            rt_task_pool_stat_add(&pool->stats.serial_parallel_calls, 1ULL);
        }
        return rt_task_pool_run_serial_parallel(count, effective_min_chunk, body, arg);
    }
    pool->count = count;
    pool->min_chunk = effective_min_chunk;
    pool->parallel_body = body;
    pool->work_arg = arg;
    pool->group = 0;
    return rt_task_pool_dispatch(pool, RT_TASK_WORK_PARALLEL);
}

int rt_task_group_begin(RtTaskPool *pool, RtTaskGroup *group) {
    if (group == 0) {
        return -1;
    }
    rt_memset(group, 0, sizeof(*group));
    group->pool = pool;
    return 0;
}

int rt_task_group_reserve(RtTaskGroup *group, size_t capacity) {
    RtTaskRecord *next_tasks;

    if (group == 0) {
        return -1;
    }
    if (group->submit_error != 0) {
        return group->submit_error;
    }
    if (capacity <= group->capacity) {
        return 0;
    }
    next_tasks = (RtTaskRecord *)rt_realloc_array(group->tasks, capacity, sizeof(group->tasks[0]));
    if (next_tasks == 0) {
        group->submit_error = -1;
        return -1;
    }
    group->tasks = next_tasks;
    group->capacity = capacity;
    if (group->pool != 0) {
        rt_task_pool_stat_add(&group->pool->stats.allocation_count, 1ULL);
        rt_task_pool_stat_add(&group->pool->stats.allocation_bytes, (unsigned long long)(capacity * sizeof(group->tasks[0])));
    }
    return 0;
}

int rt_task_group_submit(RtTaskGroup *group, RtTaskFn fn, void *arg) {
    size_t next_capacity;

    if (group == 0 || fn == 0) {
        return -1;
    }
    if (group->submit_error != 0) {
        return group->submit_error;
    }
    if (group->count >= group->capacity) {
        next_capacity = group->capacity == 0U ? 8U : group->capacity * 2U;
        if (rt_task_group_reserve(group, next_capacity) != 0) {
            return -1;
        }
    }
    group->tasks[group->count].function = fn;
    group->tasks[group->count].arg = arg;
    group->count += 1U;
    return 0;
}

static int rt_task_group_run_serial(RtTaskGroup *group) {
    size_t index;
    int first_error = group->submit_error;

    for (index = 0U; index < group->count; ++index) {
        int result = group->tasks[index].function(0U, group->tasks[index].arg);
        if (first_error == 0 && result != 0) {
            first_error = result;
        }
    }
    return first_error;
}

int rt_task_group_wait(RtTaskGroup *group) {
    RtTaskPool *pool;
    int result;

    if (group == 0) {
        return -1;
    }
    pool = group->pool;
    if (pool == 0 || pool->native_width <= 1U || group->count <= 1U) {
        if (pool != 0) {
            rt_task_pool_stat_add(&pool->stats.serial_group_calls, 1ULL);
            rt_task_pool_stat_store_uint(&pool->stats.last_requested_width, pool->width);
            rt_task_pool_stat_store_uint(&pool->stats.last_effective_width, pool->native_width);
            rt_task_pool_stat_store_uint(&pool->stats.last_active_workers, 1U);
            rt_task_pool_stat_store_size(&pool->stats.last_count, group->count);
            rt_task_pool_stat_store_size(&pool->stats.last_requested_min_chunk, 0U);
            rt_task_pool_stat_store_size(&pool->stats.last_effective_min_chunk, 0U);
            rt_task_pool_stat_store_size(&pool->stats.last_group_batch_size, 0U);
        }
        result = rt_task_group_run_serial(group);
    } else {
        size_t group_batch_size = rt_task_pool_effective_min_chunk(pool, group->count, 1U);

        rt_task_pool_stat_store_uint(&pool->stats.last_requested_width, pool->width);
        rt_task_pool_stat_store_uint(&pool->stats.last_effective_width, pool->native_width);
        rt_task_pool_stat_store_uint(&pool->stats.last_active_workers, rt_task_pool_active_for_units(pool, group->count, group_batch_size));
        rt_task_pool_stat_store_size(&pool->stats.last_count, group->count);
        rt_task_pool_stat_store_size(&pool->stats.last_requested_min_chunk, 0U);
        rt_task_pool_stat_store_size(&pool->stats.last_effective_min_chunk, 0U);
        rt_task_pool_stat_store_size(&pool->stats.last_group_batch_size, group_batch_size);
        pool->group = group;
        pool->count = group->count;
        pool->min_chunk = group_batch_size;
        result = rt_task_pool_dispatch(pool, RT_TASK_WORK_GROUP);
        if (result == 0 && group->submit_error != 0) {
            result = group->submit_error;
        }
    }
    rt_free(group->tasks);
    rt_memset(group, 0, sizeof(*group));
    return result;
}