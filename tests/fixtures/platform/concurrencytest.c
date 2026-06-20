#include "concurrency.h"
#include "io_loop.h"
#include "runtime.h"

typedef struct {
    unsigned int visits[16];
    unsigned int worker_seen;
} ParallelState;

typedef struct {
    unsigned int total;
} TaskState;

typedef struct {
    RtIoLoop *loop;
    unsigned int deferred_count;
    unsigned int timer_count;
} LoopState;

#if defined(CONCURRENCYTEST_STUB_POLL)
int platform_poll_fds(const int *fds, size_t fd_count, size_t *ready_index_out, int timeout_milliseconds) {
    (void)fds;
    (void)fd_count;
    (void)ready_index_out;
    (void)timeout_milliseconds;
    return -1;
}
#endif

static int mark_range(size_t begin, size_t end, unsigned int worker_index, void *arg) {
    ParallelState *state = (ParallelState *)arg;
    size_t index;

    state->worker_seen |= worker_index + 1U;
    for (index = begin; index < end; ++index) {
        state->visits[index] += 1U;
    }
    return 0;
}

static int add_task(unsigned int worker_index, void *arg) {
    TaskState *state = (TaskState *)arg;

    state->total += worker_index + 1U;
    return 0;
}

static void deferred_callback(void *arg) {
    LoopState *state = (LoopState *)arg;

    state->deferred_count += 1U;
}

static void timer_callback(void *arg) {
    LoopState *state = (LoopState *)arg;

    state->timer_count += 1U;
    rt_io_loop_stop(state->loop);
}

static void fail(const char *message) {
    rt_write_cstr(2, "concurrencytest: ");
    rt_write_line(2, message);
}

int main(void) {
    RtTaskPool pool;
    RtTaskGroup group;
    ParallelState parallel_state;
    TaskState task_state;
    RtIoLoop loop;
    LoopState loop_state;
    size_t index;

    rt_memset(&parallel_state, 0, sizeof(parallel_state));
    rt_memset(&task_state, 0, sizeof(task_state));
    rt_memset(&loop_state, 0, sizeof(loop_state));

    if (rt_task_pool_init(&pool, 1U) != 0) {
        fail("pool init failed");
        return 1;
    }
    if (rt_parallel_for(&pool, 16U, 3U, mark_range, &parallel_state) != 0) {
        fail("parallel_for failed");
        return 1;
    }
    for (index = 0U; index < 16U; ++index) {
        if (parallel_state.visits[index] != 1U) {
            fail("parallel visit count mismatch");
            return 1;
        }
    }
    if (parallel_state.worker_seen != 1U) {
        fail("serial worker index mismatch");
        return 1;
    }

    if (rt_task_group_begin(&pool, &group) != 0 ||
        rt_task_group_submit(&group, add_task, &task_state) != 0 ||
        rt_task_group_submit(&group, add_task, &task_state) != 0 ||
        rt_task_group_wait(&group) != 0) {
        fail("task group failed");
        return 1;
    }
    if (task_state.total != 2U) {
        fail("task group result mismatch");
        return 1;
    }
    rt_task_pool_destroy(&pool);

    loop_state.loop = &loop;
    if (rt_io_loop_init(&loop) != 0 ||
        rt_io_loop_defer(&loop, deferred_callback, &loop_state) != 0 ||
        rt_io_loop_timer(&loop, 0ULL, timer_callback, &loop_state) != 0 ||
        rt_io_loop_run(&loop) != 0) {
        fail("io loop failed");
        return 1;
    }
    rt_io_loop_destroy(&loop);
    if (loop_state.deferred_count != 1U || loop_state.timer_count != 1U) {
        fail("io loop callback mismatch");
        return 1;
    }

    rt_write_line(1, "status: ok");
    return 0;
}