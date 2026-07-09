#ifndef NEWOS_IO_LOOP_H
#define NEWOS_IO_LOOP_H

#include <stddef.h>
#include "platform.h"

#define RT_IO_READ  (1U << 0)
#define RT_IO_WRITE (1U << 1)
#define RT_IO_ERROR (1U << 2)

typedef struct RtIoLoop RtIoLoop;
typedef void (*RtIoReady)(int fd, unsigned int events, void *arg);
typedef void (*RtIoTimer)(void *arg);
typedef void (*RtIoDeferred)(void *arg);

typedef struct {
    int fd;
    unsigned int events;
    RtIoReady callback;
    void *arg;
    int active;
} RtIoRegistration;

typedef struct {
    unsigned long long due_time_ns;
    RtIoTimer callback;
    void *arg;
    int active;
} RtIoTimerEntry;

typedef struct {
    RtIoDeferred callback;
    void *arg;
} RtIoDeferredEntry;

struct RtIoLoop {
    RtIoRegistration *registrations;
    size_t registration_count;
    size_t registration_capacity;
    RtIoTimerEntry *timers;
    size_t timer_count;
    size_t timer_capacity;
    RtIoDeferredEntry *deferred;
    size_t deferred_count;
    size_t deferred_capacity;
    PlatformPollFd *poll_fds;
    size_t poll_capacity;
    int stop;
};

int rt_io_loop_init(RtIoLoop *loop);
void rt_io_loop_destroy(RtIoLoop *loop);
int rt_io_loop_add(RtIoLoop *loop, int fd, unsigned int events, RtIoReady callback, void *arg);
int rt_io_loop_modify(RtIoLoop *loop, int fd, unsigned int events);
int rt_io_loop_remove(RtIoLoop *loop, int fd);
int rt_io_loop_timer(RtIoLoop *loop, unsigned long long delay_ms, RtIoTimer callback, void *arg);
int rt_io_loop_defer(RtIoLoop *loop, RtIoDeferred callback, void *arg);
int rt_io_loop_run(RtIoLoop *loop);
void rt_io_loop_stop(RtIoLoop *loop);

#endif