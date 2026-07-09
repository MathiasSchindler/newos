#ifndef NEWOS_CONCURRENCY_H
#define NEWOS_CONCURRENCY_H

#include <stddef.h>
#include "platform.h"
#include "runtime.h"

#define RT_TASK_POOL_MAX_WORKERS 32U
#define RT_TASK_POOL_DEFAULT_STACK_SIZE (512U * 1024U)

#ifndef NEWOS_RUNTIME_TASK_STATS
#define NEWOS_RUNTIME_TASK_STATS 0
#endif

typedef int (*RtParallelBody)(size_t begin, size_t end, unsigned int worker_index, void *arg);
typedef int (*RtTaskFn)(unsigned int worker_index, void *arg);

typedef struct RtTaskGroup RtTaskGroup;

typedef struct {
    RtTaskFn function;
    void *arg;
} RtTaskRecord;

typedef struct {
    unsigned long long dispatches;
    unsigned long long parallel_dispatches;
    unsigned long long group_dispatches;
    unsigned long long serial_parallel_calls;
    unsigned long long serial_group_calls;
    unsigned long long chunk_claim_attempts;
    unsigned long long chunks_claimed;
    unsigned long long group_batches_claimed;
    unsigned long long group_tasks_claimed;
    unsigned long long worker_waits;
    unsigned long long worker_wakes;
    unsigned long long join_waits;
    unsigned long long worker_completions;
    unsigned long long workers_woken;
    unsigned long long workers_ran;
    unsigned long long idle_worker_completions;
    unsigned long long dispatch_ns;
    unsigned long long join_ns;
    unsigned long long body_ns;
    unsigned long long worker_spin_loops;
    unsigned long long join_spin_loops;
    unsigned long long task_submit_ns;
    unsigned long long task_execute_ns;
    unsigned long long allocation_count;
    unsigned long long allocation_bytes;
    unsigned int last_requested_width;
    unsigned int last_effective_width;
    unsigned int last_active_workers;
    size_t last_count;
    size_t last_requested_min_chunk;
    size_t last_effective_min_chunk;
    size_t last_group_batch_size;
    unsigned long long worker_chunks[RT_TASK_POOL_MAX_WORKERS];
    unsigned long long worker_group_tasks[RT_TASK_POOL_MAX_WORKERS];
} RtTaskPoolStats;

typedef struct {
    unsigned int index;
    struct RtTaskPool *pool;
    PlatformWorkerThread thread;
    RtArena arena;
} RtTaskWorker;

typedef struct RtTaskPool {
    unsigned int width;
    unsigned int native_width;
    unsigned int stop;
    unsigned int generation;
    unsigned int active_workers;
    unsigned int claimed_workers;
    unsigned int finished_workers;
    unsigned int work_kind;
    int first_error;
    size_t next_index;
    size_t count;
    size_t min_chunk;
    RtParallelBody parallel_body;
    RtTaskFn task_body;
    void *work_arg;
    RtTaskGroup *group;
#if NEWOS_RUNTIME_TASK_STATS
    RtTaskPoolStats stats;
#endif
    RtTaskWorker workers[RT_TASK_POOL_MAX_WORKERS];
} RtTaskPool;

struct RtTaskGroup {
    RtTaskPool *pool;
    RtTaskRecord *tasks;
    size_t count;
    size_t capacity;
    int submit_error;
};

int rt_task_pool_init(RtTaskPool *pool, unsigned int worker_count);
void rt_task_pool_destroy(RtTaskPool *pool);
unsigned int rt_task_pool_width(const RtTaskPool *pool);
RtArena *rt_task_pool_worker_arena(RtTaskPool *pool, unsigned int worker_index);
void rt_task_pool_reset_stats(RtTaskPool *pool);
void rt_task_pool_get_stats(const RtTaskPool *pool, RtTaskPoolStats *stats_out);
int rt_parallel_for(RtTaskPool *pool, size_t count, size_t min_chunk, RtParallelBody body, void *arg);
int rt_task_group_begin(RtTaskPool *pool, RtTaskGroup *group);
int rt_task_group_reserve(RtTaskGroup *group, size_t capacity);
int rt_task_group_submit(RtTaskGroup *group, RtTaskFn fn, void *arg);
int rt_task_group_wait(RtTaskGroup *group);

#endif