#ifndef NEWOS_CONCURRENCY_H
#define NEWOS_CONCURRENCY_H

#include <stddef.h>
#include "platform.h"
#include "runtime.h"

#define RT_TASK_POOL_MAX_WORKERS 32U
#define RT_TASK_POOL_DEFAULT_STACK_SIZE (512U * 1024U)

typedef int (*RtParallelBody)(size_t begin, size_t end, unsigned int worker_index, void *arg);
typedef int (*RtTaskFn)(unsigned int worker_index, void *arg);

typedef struct RtTaskGroup RtTaskGroup;

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
    RtTaskWorker workers[RT_TASK_POOL_MAX_WORKERS];
} RtTaskPool;

struct RtTaskGroup {
    RtTaskPool *pool;
    RtTaskFn *functions;
    void **args;
    size_t count;
    size_t capacity;
    int submit_error;
};

int rt_task_pool_init(RtTaskPool *pool, unsigned int worker_count);
void rt_task_pool_destroy(RtTaskPool *pool);
unsigned int rt_task_pool_width(const RtTaskPool *pool);
RtArena *rt_task_pool_worker_arena(RtTaskPool *pool, unsigned int worker_index);
int rt_parallel_for(RtTaskPool *pool, size_t count, size_t min_chunk, RtParallelBody body, void *arg);
int rt_task_group_begin(RtTaskPool *pool, RtTaskGroup *group);
int rt_task_group_submit(RtTaskGroup *group, RtTaskFn fn, void *arg);
int rt_task_group_wait(RtTaskGroup *group);

#endif