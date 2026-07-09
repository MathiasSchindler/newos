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
    unsigned int ready_count;
    unsigned int ready_target;
    unsigned int ready_events;
} LoopState;

#if defined(CONCURRENCYTEST_STUB_POLL)
int platform_poll(PlatformPollFd *fds, size_t fd_count, int timeout_milliseconds) {
    size_t index;
    (void)timeout_milliseconds;
    for (index = 0U; index < fd_count; ++index) {
        fds[index].revents = index < 17U ? fds[index].events | PLATFORM_POLL_ERROR : 0U;
    }
    return fd_count < 17U ? (int)fd_count : 17;
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

static void ready_callback(int fd, unsigned int events, void *arg) {
    LoopState *state = (LoopState *)arg;

    (void)fd;
    state->ready_count += 1U;
    state->ready_events |= events;
    (void)rt_io_loop_remove(state->loop, fd);
    if (state->ready_count == state->ready_target) rt_io_loop_stop(state->loop);
}

static void fail(const char *message) {
    rt_write_cstr(2, "concurrencytest: ");
    rt_write_line(2, message);
}

int main(void) {
    RtTaskPool pool;
    RtTaskGroup group;
    RtArena arena;
    ParallelState parallel_state;
    TaskState task_state;
    RtIoLoop loop;
    LoopState loop_state;
    size_t index;
    void *arena_first;
    void *arena_second;
#if !defined(CONCURRENCYTEST_STUB_POLL)
    int pipe_fds[20][2];
#endif

    rt_memset(&parallel_state, 0, sizeof(parallel_state));
    rt_memset(&task_state, 0, sizeof(task_state));
    rt_memset(&loop_state, 0, sizeof(loop_state));

    rt_arena_init(&arena, 0U);
    arena_first = rt_arena_alloc(&arena, 12000U);
    arena_second = rt_arena_alloc(&arena, 12000U);
    if (arena_first == 0 || arena_second == 0 || arena_first == arena_second) {
        fail("arena multi-block allocation failed");
        return 1;
    }
    rt_arena_reset(&arena);
    if (rt_arena_alloc(&arena, 12000U) != arena_second || rt_arena_alloc(&arena, 12000U) != arena_first) {
        fail("arena reset did not reuse backing blocks");
        return 1;
    }
    rt_arena_destroy(&arena);

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

    rt_memset(&loop_state, 0, sizeof(loop_state));
    loop_state.loop = &loop;
    loop_state.ready_target = 17U;
    if (rt_io_loop_init(&loop) != 0) {
        fail("ready loop init failed");
        return 1;
    }
    for (index = 0U; index < 20U; ++index) {
#if defined(CONCURRENCYTEST_STUB_POLL)
        unsigned int events = (index & 1U) == 0U ? RT_IO_READ : RT_IO_WRITE;
        int fd = (int)index + 10;
#else
        unsigned int events = RT_IO_READ;
        int fd;
        char byte = 'x';

        if (platform_create_pipe(pipe_fds[index]) != 0 ||
            (index < loop_state.ready_target && platform_write(pipe_fds[index][1], &byte, 1U) != 1)) {
            fail("ready loop pipe setup failed");
            return 1;
        }
        fd = pipe_fds[index][0];
#endif
        if (rt_io_loop_add(&loop, fd, events, ready_callback, &loop_state) != 0) {
            fail("ready loop registration failed");
            return 1;
        }
    }
    if (rt_io_loop_run(&loop) != 0 || loop_state.ready_count != loop_state.ready_target ||
#if defined(CONCURRENCYTEST_STUB_POLL)
        loop_state.ready_events != (RT_IO_READ | RT_IO_WRITE | RT_IO_ERROR)) {
#else
        loop_state.ready_events != RT_IO_READ) {
#endif
        fail("io loop did not dispatch all actual readiness events");
        return 1;
    }
    rt_io_loop_destroy(&loop);
#if !defined(CONCURRENCYTEST_STUB_POLL)
    for (index = 0U; index < 20U; ++index) {
        (void)platform_close(pipe_fds[index][0]);
        (void)platform_close(pipe_fds[index][1]);
    }
#endif

    rt_write_line(1, "status: ok");
    return 0;
}