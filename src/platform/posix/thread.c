#include "platform.h"

int platform_worker_threads_supported(void) {
    return 0;
}

unsigned int platform_worker_thread_count(void) {
    return 1U;
}

int platform_worker_thread_start(PlatformWorkerThread *thread, PlatformWorkerMain entry, void *arg, size_t stack_size) {
    (void)thread;
    (void)entry;
    (void)arg;
    (void)stack_size;
    return -1;
}

int platform_worker_thread_join(PlatformWorkerThread *thread, int *result_out) {
    (void)thread;
    (void)result_out;
    return -1;
}

void platform_wait_word(volatile unsigned int *word, unsigned int expected) {
    while (__atomic_load_n(word, __ATOMIC_ACQUIRE) == expected) {
        break;
    }
}

void platform_wake_word_one(volatile unsigned int *word) {
    (void)word;
}

void platform_wake_word_all(volatile unsigned int *word) {
    (void)word;
}

void platform_wait_wake_stats_reset(void) {
}

void platform_wait_wake_stats_get(PlatformWaitWakeStats *stats_out) {
    if (stats_out != 0) {
        stats_out->wait_calls = 0ULL;
        stats_out->wake_calls = 0ULL;
        stats_out->wait_eagain = 0ULL;
        stats_out->wait_eintr = 0ULL;
    }
}

int platform_thread_start(PlatformThread *thread, PlatformThreadMain entry, void *arg, size_t stack_size) {
    return platform_worker_thread_start((PlatformWorkerThread *)thread, (PlatformWorkerMain)entry, arg, stack_size);
}

int platform_thread_join(PlatformThread *thread, int *result_out) {
    return platform_worker_thread_join((PlatformWorkerThread *)thread, result_out);
}

void platform_mutex_init(PlatformMutex *mutex) {
    if (mutex != 0) {
        __atomic_store_n(&mutex->state, 0, __ATOMIC_RELEASE);
    }
}

void platform_mutex_lock(PlatformMutex *mutex) {
    int expected = 0;

    while (mutex != 0 && !__atomic_compare_exchange_n(&mutex->state, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        expected = 0;
    }
}

void platform_mutex_unlock(PlatformMutex *mutex) {
    if (mutex != 0) {
        __atomic_store_n(&mutex->state, 0, __ATOMIC_RELEASE);
    }
}

void platform_semaphore_init(PlatformSemaphore *semaphore, int value) {
    if (semaphore != 0) {
        __atomic_store_n(&semaphore->count, value, __ATOMIC_RELEASE);
    }
}

void platform_semaphore_wait(PlatformSemaphore *semaphore) {
    int value;

    if (semaphore == 0) {
        return;
    }
    for (;;) {
        value = __atomic_load_n(&semaphore->count, __ATOMIC_ACQUIRE);
        if (value > 0 && __atomic_compare_exchange_n(&semaphore->count, &value, value - 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return;
        }
    }
}

void platform_semaphore_post(PlatformSemaphore *semaphore) {
    if (semaphore != 0) {
        (void)__atomic_fetch_add(&semaphore->count, 1, __ATOMIC_RELEASE);
    }
}