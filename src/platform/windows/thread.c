#include "platform.h"

#if !defined(_WIN32) && !defined(__CYGWIN__)
#ifndef __declspec
#define __declspec(attribute)
#endif
#ifndef __stdcall
#define __stdcall
#endif
#endif

#define WIN_ALL_PROCESSOR_GROUPS 0xffffU
#define WIN_INFINITE 0xffffffffUL
#define WIN_MEM_RELEASE 0x8000UL

typedef unsigned long (__stdcall *WindowsThreadEntry)(void *arg);
typedef int (__stdcall *WindowsWaitOnAddress)(
    volatile void *address, void *compare_address, size_t address_size,
    unsigned long milliseconds
);
typedef void (__stdcall *WindowsWakeByAddress)(void *address);

typedef struct {
    PlatformWorkerMain entry;
    void *arg;
    PlatformWorkerThread *thread;
    int result;
} WindowsThreadStart;

__declspec(dllimport) int __stdcall CloseHandle(void *handle);
__declspec(dllimport) void *__stdcall CreateThread(
    void *security, size_t stack_size, WindowsThreadEntry entry, void *arg,
    unsigned long flags, unsigned long *thread_id
);
__declspec(dllimport) unsigned int __stdcall GetActiveProcessorCount(unsigned short group);
__declspec(dllimport) void *__stdcall GetProcAddress(void *module, const char *name);
__declspec(dllimport) void *__stdcall LoadLibraryA(const char *name);
__declspec(dllimport) void *__stdcall VirtualAlloc(
    void *address, size_t size, unsigned long allocation_type, unsigned long protect
);
__declspec(dllimport) int __stdcall VirtualFree(void *address, size_t size, unsigned long free_type);
static PlatformWaitWakeStats windows_wait_wake_stats;
static WindowsWaitOnAddress windows_wait_on_address;
static WindowsWakeByAddress windows_wake_by_address_all;
static WindowsWakeByAddress windows_wake_by_address_single;

static int windows_thread_api_init(void) {
    void *module;
    if (windows_wait_on_address != 0 && windows_wake_by_address_all != 0 &&
        windows_wake_by_address_single != 0) {
        return 1;
    }
    module = LoadLibraryA("KernelBase.dll");
    if (module == 0) return 0;
    windows_wait_on_address = (WindowsWaitOnAddress)GetProcAddress(module, "WaitOnAddress");
    windows_wake_by_address_all =
        (WindowsWakeByAddress)GetProcAddress(module, "WakeByAddressAll");
    windows_wake_by_address_single =
        (WindowsWakeByAddress)GetProcAddress(module, "WakeByAddressSingle");
    return windows_wait_on_address != 0 && windows_wake_by_address_all != 0 &&
        windows_wake_by_address_single != 0;
}

static unsigned long __stdcall windows_thread_entry(void *arg) {
    WindowsThreadStart *start = (WindowsThreadStart *)arg;
    PlatformWorkerThread *thread = start->thread;

    start->result = start->entry(start->arg);
    __atomic_store_n(&thread->clear_tid, 0, __ATOMIC_RELEASE);
    windows_wake_by_address_all((void *)&thread->clear_tid);
    return 0UL;
}

size_t platform_page_size(void) {
    return 4096U;
}

void *platform_allocate_pages(size_t size) {
    return VirtualAlloc(0, size, 0x3000UL, 0x04UL);
}

int platform_free_pages(void *ptr, size_t size) {
    (void)size;
    if (ptr == 0) return 0;
    return VirtualFree(ptr, 0, WIN_MEM_RELEASE) ? 0 : -1;
}

int platform_worker_threads_supported(void) {
    return windows_thread_api_init();
}

unsigned int platform_worker_thread_count(void) {
    unsigned int count = GetActiveProcessorCount(WIN_ALL_PROCESSOR_GROUPS);
    return count == 0U ? 1U : count;
}

int platform_worker_thread_start(
    PlatformWorkerThread *thread,
    PlatformWorkerMain entry,
    void *arg,
    size_t stack_size
) {
    WindowsThreadStart *start;
    void *handle;
    unsigned long thread_id = 0UL;

    if (thread == 0 || entry == 0 || !windows_thread_api_init()) return -1;
    start = (WindowsThreadStart *)platform_allocate_pages(4096U);
    if (start == 0) return -1;
    start->entry = entry;
    start->arg = arg;
    start->thread = thread;
    start->result = 0;
    thread->tid = 0;
    thread->clear_tid = 1;
    thread->stack = start;
    thread->stack_size = 4096U;
    handle = CreateThread(0, stack_size, windows_thread_entry, start, 0UL, &thread_id);
    if (handle == 0) {
        (void)platform_free_pages(start, 4096U);
        thread->clear_tid = 0;
        thread->stack = 0;
        thread->stack_size = 0U;
        return -1;
    }
    thread->tid = (int)thread_id;
    (void)CloseHandle(handle);
    return 0;
}

int platform_worker_thread_join(PlatformWorkerThread *thread, int *result_out) {
    WindowsThreadStart *start;

    if (thread == 0 || thread->stack == 0) return -1;
    while (__atomic_load_n(&thread->clear_tid, __ATOMIC_ACQUIRE) != 0) {
        int expected = 1;
        (void)windows_wait_on_address(
            &thread->clear_tid, &expected, sizeof(expected), WIN_INFINITE
        );
    }
    start = (WindowsThreadStart *)thread->stack;
    if (result_out != 0) *result_out = start->result;
    (void)platform_free_pages(start, thread->stack_size);
    thread->tid = 0;
    thread->stack = 0;
    thread->stack_size = 0U;
    return 0;
}

int platform_thread_start(
    PlatformThread *thread,
    PlatformThreadMain entry,
    void *arg,
    size_t stack_size
) {
    return platform_worker_thread_start(
        (PlatformWorkerThread *)thread, (PlatformWorkerMain)entry, arg, stack_size
    );
}

int platform_thread_join(PlatformThread *thread, int *result_out) {
    return platform_worker_thread_join((PlatformWorkerThread *)thread, result_out);
}

void platform_wait_word(volatile unsigned int *word, unsigned int expected) {
    if (!windows_thread_api_init()) return;
    (void)__atomic_fetch_add(&windows_wait_wake_stats.wait_calls, 1ULL, __ATOMIC_RELAXED);
    (void)windows_wait_on_address(
        (volatile void *)word, &expected, sizeof(expected), WIN_INFINITE
    );
}

void platform_wake_word_one(volatile unsigned int *word) {
    if (!windows_thread_api_init()) return;
    (void)__atomic_fetch_add(&windows_wait_wake_stats.wake_calls, 1ULL, __ATOMIC_RELAXED);
    windows_wake_by_address_single((void *)word);
}

void platform_wake_word_count(volatile unsigned int *word, unsigned int count) {
    if (count == 0U || !windows_thread_api_init()) return;
    (void)__atomic_fetch_add(&windows_wait_wake_stats.wake_calls, 1ULL, __ATOMIC_RELAXED);
    if (count == 1U) {
        windows_wake_by_address_single((void *)word);
    } else {
        windows_wake_by_address_all((void *)word);
    }
}

void platform_wake_word_all(volatile unsigned int *word) {
    if (!windows_thread_api_init()) return;
    (void)__atomic_fetch_add(&windows_wait_wake_stats.wake_calls, 1ULL, __ATOMIC_RELAXED);
    windows_wake_by_address_all((void *)word);
}

void platform_wait_wake_stats_reset(void) {
    __atomic_store_n(&windows_wait_wake_stats.wait_calls, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&windows_wait_wake_stats.wake_calls, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&windows_wait_wake_stats.wait_eagain, 0ULL, __ATOMIC_RELAXED);
    __atomic_store_n(&windows_wait_wake_stats.wait_eintr, 0ULL, __ATOMIC_RELAXED);
}

void platform_wait_wake_stats_get(PlatformWaitWakeStats *stats_out) {
    if (stats_out == 0) return;
    stats_out->wait_calls = __atomic_load_n(
        &windows_wait_wake_stats.wait_calls, __ATOMIC_RELAXED
    );
    stats_out->wake_calls = __atomic_load_n(
        &windows_wait_wake_stats.wake_calls, __ATOMIC_RELAXED
    );
    stats_out->wait_eagain = 0ULL;
    stats_out->wait_eintr = 0ULL;
}