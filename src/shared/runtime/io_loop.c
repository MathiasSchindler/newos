#include "io_loop.h"
#include "platform.h"
#include "runtime.h"

static int rt_io_loop_reserve_registrations(RtIoLoop *loop, size_t needed) {
    RtIoRegistration *next;
    size_t capacity;

    if (needed <= loop->registration_capacity) {
        return 0;
    }
    capacity = loop->registration_capacity == 0U ? 8U : loop->registration_capacity * 2U;
    while (capacity < needed) {
        capacity *= 2U;
    }
    next = (RtIoRegistration *)rt_realloc_array(loop->registrations, capacity, sizeof(loop->registrations[0]));
    if (next == 0) {
        return -1;
    }
    loop->registrations = next;
    loop->registration_capacity = capacity;
    return 0;
}

static int rt_io_loop_reserve_timers(RtIoLoop *loop, size_t needed) {
    RtIoTimerEntry *next;
    size_t capacity;

    if (needed <= loop->timer_capacity) {
        return 0;
    }
    capacity = loop->timer_capacity == 0U ? 4U : loop->timer_capacity * 2U;
    while (capacity < needed) {
        capacity *= 2U;
    }
    next = (RtIoTimerEntry *)rt_realloc_array(loop->timers, capacity, sizeof(loop->timers[0]));
    if (next == 0) {
        return -1;
    }
    loop->timers = next;
    loop->timer_capacity = capacity;
    return 0;
}

static int rt_io_loop_reserve_deferred(RtIoLoop *loop, size_t needed) {
    RtIoDeferredEntry *next;
    size_t capacity;

    if (needed <= loop->deferred_capacity) {
        return 0;
    }
    capacity = loop->deferred_capacity == 0U ? 8U : loop->deferred_capacity * 2U;
    while (capacity < needed) {
        capacity *= 2U;
    }
    next = (RtIoDeferredEntry *)rt_realloc_array(loop->deferred, capacity, sizeof(loop->deferred[0]));
    if (next == 0) {
        return -1;
    }
    loop->deferred = next;
    loop->deferred_capacity = capacity;
    return 0;
}

int rt_io_loop_init(RtIoLoop *loop) {
    if (loop == 0) {
        return -1;
    }
    rt_memset(loop, 0, sizeof(*loop));
    return 0;
}

void rt_io_loop_destroy(RtIoLoop *loop) {
    if (loop == 0) {
        return;
    }
    rt_free(loop->registrations);
    rt_free(loop->timers);
    rt_free(loop->deferred);
    rt_memset(loop, 0, sizeof(*loop));
}

static size_t rt_io_loop_find_fd(const RtIoLoop *loop, int fd) {
    size_t index;

    if (loop == 0) {
        return (size_t)-1;
    }
    for (index = 0U; index < loop->registration_count; ++index) {
        if (loop->registrations[index].active && loop->registrations[index].fd == fd) {
            return index;
        }
    }
    return (size_t)-1;
}

int rt_io_loop_add(RtIoLoop *loop, int fd, unsigned int events, RtIoReady callback, void *arg) {
    RtIoRegistration *registration;

    if (loop == 0 || fd < 0 || callback == 0 || events == 0U) {
        return -1;
    }
    if (rt_io_loop_find_fd(loop, fd) != (size_t)-1) {
        return -1;
    }
    if (rt_io_loop_reserve_registrations(loop, loop->registration_count + 1U) != 0) {
        return -1;
    }
    registration = &loop->registrations[loop->registration_count++];
    registration->fd = fd;
    registration->events = events;
    registration->callback = callback;
    registration->arg = arg;
    registration->active = 1;
    return 0;
}

int rt_io_loop_modify(RtIoLoop *loop, int fd, unsigned int events) {
    size_t index = rt_io_loop_find_fd(loop, fd);

    if (index == (size_t)-1 || events == 0U) {
        return -1;
    }
    loop->registrations[index].events = events;
    return 0;
}

int rt_io_loop_remove(RtIoLoop *loop, int fd) {
    size_t index = rt_io_loop_find_fd(loop, fd);

    if (index == (size_t)-1) {
        return -1;
    }
    loop->registrations[index].active = 0;
    return 0;
}

int rt_io_loop_timer(RtIoLoop *loop, unsigned long long delay_ms, RtIoTimer callback, void *arg) {
    RtIoTimerEntry *entry;
    unsigned long long now;

    if (loop == 0 || callback == 0) {
        return -1;
    }
    if (rt_io_loop_reserve_timers(loop, loop->timer_count + 1U) != 0) {
        return -1;
    }
    now = platform_get_monotonic_time_ns();
    entry = &loop->timers[loop->timer_count++];
    entry->due_time_ns = now + delay_ms * 1000000ULL;
    entry->callback = callback;
    entry->arg = arg;
    entry->active = 1;
    return 0;
}

int rt_io_loop_defer(RtIoLoop *loop, RtIoDeferred callback, void *arg) {
    RtIoDeferredEntry *entry;

    if (loop == 0 || callback == 0) {
        return -1;
    }
    if (rt_io_loop_reserve_deferred(loop, loop->deferred_count + 1U) != 0) {
        return -1;
    }
    entry = &loop->deferred[loop->deferred_count++];
    entry->callback = callback;
    entry->arg = arg;
    return 0;
}

static void rt_io_loop_compact_registrations(RtIoLoop *loop) {
    size_t read_index;
    size_t write_index = 0U;

    for (read_index = 0U; read_index < loop->registration_count; ++read_index) {
        if (loop->registrations[read_index].active) {
            if (write_index != read_index) {
                loop->registrations[write_index] = loop->registrations[read_index];
            }
            write_index += 1U;
        }
    }
    loop->registration_count = write_index;
}

static void rt_io_loop_run_deferred(RtIoLoop *loop) {
    RtIoDeferredEntry *entries = loop->deferred;
    size_t count = loop->deferred_count;
    size_t index;

    loop->deferred = 0;
    loop->deferred_count = 0U;
    loop->deferred_capacity = 0U;
    for (index = 0U; index < count; ++index) {
        entries[index].callback(entries[index].arg);
        if (loop->stop) {
            break;
        }
    }
    rt_free(entries);
}

static int rt_io_loop_next_timeout(RtIoLoop *loop) {
    unsigned long long now = platform_get_monotonic_time_ns();
    unsigned long long best = 0ULL;
    int has_timer = 0;
    size_t index;

    for (index = 0U; index < loop->timer_count; ++index) {
        if (!loop->timers[index].active) {
            continue;
        }
        if (loop->timers[index].due_time_ns <= now) {
            return 0;
        }
        if (!has_timer || loop->timers[index].due_time_ns < best) {
            best = loop->timers[index].due_time_ns;
            has_timer = 1;
        }
    }
    if (!has_timer) {
        return -1;
    }
    return (int)((best - now + 999999ULL) / 1000000ULL);
}

static void rt_io_loop_run_timers(RtIoLoop *loop) {
    unsigned long long now = platform_get_monotonic_time_ns();
    size_t index;

    for (index = 0U; index < loop->timer_count; ++index) {
        if (loop->timers[index].active && loop->timers[index].due_time_ns <= now) {
            RtIoTimer callback = loop->timers[index].callback;
            void *arg = loop->timers[index].arg;

            loop->timers[index].active = 0;
            callback(arg);
            if (loop->stop) {
                break;
            }
        }
    }
}

static void rt_io_loop_compact_timers(RtIoLoop *loop) {
    size_t read_index;
    size_t write_index = 0U;

    for (read_index = 0U; read_index < loop->timer_count; ++read_index) {
        if (loop->timers[read_index].active) {
            if (write_index != read_index) {
                loop->timers[write_index] = loop->timers[read_index];
            }
            write_index += 1U;
        }
    }
    loop->timer_count = write_index;
}

static int rt_io_loop_poll_once(RtIoLoop *loop, int timeout_ms) {
    int stack_fds[16];
    int *fds = stack_fds;
    size_t fd_count = 0U;
    size_t index;
    size_t ready_index = 0U;
    int poll_result;

    rt_io_loop_compact_registrations(loop);
    if (loop->registration_count == 0U) {
        if (timeout_ms > 0) {
            (void)platform_sleep_milliseconds((unsigned long long)timeout_ms);
        }
        return 0;
    }
    if (loop->registration_count > sizeof(stack_fds) / sizeof(stack_fds[0])) {
        fds = (int *)rt_malloc_array(loop->registration_count, sizeof(fds[0]));
        if (fds == 0) {
            return -1;
        }
    }
    for (index = 0U; index < loop->registration_count; ++index) {
        fds[fd_count++] = loop->registrations[index].fd;
    }
    poll_result = platform_poll_fds(fds, fd_count, &ready_index, timeout_ms);
    if (fds != stack_fds) {
        rt_free(fds);
    }
    if (poll_result <= 0) {
        return poll_result;
    }
    if (ready_index < loop->registration_count && loop->registrations[ready_index].active) {
        RtIoRegistration registration = loop->registrations[ready_index];
        registration.callback(registration.fd, registration.events, registration.arg);
    }
    return 1;
}

int rt_io_loop_run(RtIoLoop *loop) {
    if (loop == 0) {
        return -1;
    }
    loop->stop = 0;
    while (!loop->stop) {
        int timeout_ms;

        if (loop->deferred_count != 0U) {
            rt_io_loop_run_deferred(loop);
            continue;
        }
        rt_io_loop_run_timers(loop);
        rt_io_loop_compact_timers(loop);
        if (loop->stop) {
            break;
        }
        rt_io_loop_compact_registrations(loop);
        if (loop->registration_count == 0U && loop->timer_count == 0U) {
            break;
        }
        timeout_ms = rt_io_loop_next_timeout(loop);
        if (rt_io_loop_poll_once(loop, timeout_ms) < 0) {
            return -1;
        }
    }
    return 0;
}

void rt_io_loop_stop(RtIoLoop *loop) {
    if (loop != 0) {
        loop->stop = 1;
    }
}