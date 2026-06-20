#include "concurrency.h"

#define RT_TASK_WORK_NONE 0U
#define RT_TASK_WORK_PARALLEL 1U
#define RT_TASK_WORK_GROUP 2U

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

static size_t rt_task_pool_next_chunk(RtTaskPool *pool, size_t *end_out) {
    size_t begin;
    size_t end;
    size_t min_chunk = pool->min_chunk == 0U ? 1U : pool->min_chunk;

    for (;;) {
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
            *end_out = end;
            return begin;
        }
    }
}

static void rt_task_pool_run_parallel_worker(RtTaskPool *pool, unsigned int worker_index) {
    for (;;) {
        size_t end = 0U;
        size_t begin = rt_task_pool_next_chunk(pool, &end);

        if (begin >= end) {
            break;
        }
        rt_task_pool_record_error(pool, pool->parallel_body(begin, end, worker_index, pool->work_arg));
    }
}

static void rt_task_pool_run_group_worker(RtTaskPool *pool, unsigned int worker_index) {
    RtTaskGroup *group = pool->group;

    for (;;) {
        size_t index;

        index = __atomic_fetch_add(&pool->next_index, 1U, __ATOMIC_ACQ_REL);
        if (group == 0 || index >= group->count) {
            break;
        }
        rt_task_pool_record_error(pool, group->functions[index](worker_index, group->args[index]));
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
    unsigned int worker_index = worker != 0 ? worker->index : 0U;
    unsigned int observed_generation = 0U;

    if (pool == 0) {
        return -1;
    }

    for (;;) {
        unsigned int generation;

        generation = __atomic_load_n(&pool->generation, __ATOMIC_ACQUIRE);
        while (generation == observed_generation && __atomic_load_n(&pool->stop, __ATOMIC_ACQUIRE) == 0U) {
            platform_wait_word(&pool->generation, observed_generation);
            generation = __atomic_load_n(&pool->generation, __ATOMIC_ACQUIRE);
        }
        if (__atomic_load_n(&pool->stop, __ATOMIC_ACQUIRE) != 0U) {
            break;
        }
        observed_generation = generation;
        rt_task_pool_run_work(pool, worker_index);
        if (__atomic_fetch_add(&pool->finished_workers, 1U, __ATOMIC_ACQ_REL) + 1U >= pool->active_workers) {
            platform_wake_word_all(&pool->finished_workers);
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

    pool->work_kind = work_kind;
    pool->first_error = 0;
    pool->next_index = 0U;
    pool->active_workers = pool->native_width;
    pool->finished_workers = 0U;
    __atomic_fetch_add(&pool->generation, 1U, __ATOMIC_ACQ_REL);
    platform_wake_word_all(&pool->generation);
    rt_task_pool_run_work(pool, 0U);
    if (__atomic_fetch_add(&pool->finished_workers, 1U, __ATOMIC_ACQ_REL) + 1U >= pool->active_workers) {
        platform_wake_word_all(&pool->finished_workers);
    }
    for (;;) {
        expected = __atomic_load_n(&pool->finished_workers, __ATOMIC_ACQUIRE);
        if (expected >= pool->active_workers) {
            break;
        }
        platform_wait_word(&pool->finished_workers, expected);
    }
    __atomic_store_n(&pool->work_kind, RT_TASK_WORK_NONE, __ATOMIC_RELEASE);
    return pool->first_error;
}

int rt_parallel_for(RtTaskPool *pool, size_t count, size_t min_chunk, RtParallelBody body, void *arg) {
    if (body == 0) {
        return -1;
    }
    if (count == 0U) {
        return 0;
    }
    if (pool == 0 || pool->native_width <= 1U || count <= min_chunk || count == 1U) {
        return rt_task_pool_run_serial_parallel(count, min_chunk, body, arg);
    }
    pool->count = count;
    pool->min_chunk = min_chunk == 0U ? 1U : min_chunk;
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

int rt_task_group_submit(RtTaskGroup *group, RtTaskFn fn, void *arg) {
    RtTaskFn *next_functions;
    void **next_args;
    size_t next_capacity;

    if (group == 0 || fn == 0) {
        return -1;
    }
    if (group->submit_error != 0) {
        return group->submit_error;
    }
    if (group->count >= group->capacity) {
        next_capacity = group->capacity == 0U ? 8U : group->capacity * 2U;
        next_functions = (RtTaskFn *)rt_realloc_array(group->functions, next_capacity, sizeof(group->functions[0]));
        next_args = (void **)rt_realloc_array(group->args, next_capacity, sizeof(group->args[0]));
        if (next_functions == 0 || next_args == 0) {
            if (next_functions != 0) {
                group->functions = next_functions;
            }
            if (next_args != 0) {
                group->args = next_args;
            }
            group->submit_error = -1;
            return -1;
        }
        group->functions = next_functions;
        group->args = next_args;
        group->capacity = next_capacity;
    }
    group->functions[group->count] = fn;
    group->args[group->count] = arg;
    group->count += 1U;
    return 0;
}

static int rt_task_group_run_serial(RtTaskGroup *group) {
    size_t index;
    int first_error = group->submit_error;

    for (index = 0U; index < group->count; ++index) {
        int result = group->functions[index](0U, group->args[index]);
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
        result = rt_task_group_run_serial(group);
    } else {
        pool->group = group;
        result = rt_task_pool_dispatch(pool, RT_TASK_WORK_GROUP);
        if (result == 0 && group->submit_error != 0) {
            result = group->submit_error;
        }
    }
    rt_free(group->functions);
    rt_free(group->args);
    rt_memset(group, 0, sizeof(*group));
    return result;
}