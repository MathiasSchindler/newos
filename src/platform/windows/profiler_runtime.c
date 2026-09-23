#include <stddef.h>

#if !defined(_WIN32) && !defined(__CYGWIN__)
#ifndef __declspec
#define __declspec(attribute)
#endif
#ifndef __stdcall
#define __stdcall
#endif
#endif

#define PROFILE_NOINSTR __attribute__((no_instrument_function))
#define PROFILE_BUFFER_SIZE 65536U
#define PROFILE_GENERIC_WRITE 0x40000000UL
#define PROFILE_CREATE_ALWAYS 2UL
#define PROFILE_FILE_ATTRIBUTE_NORMAL 0x80UL
#define PROFILE_INVALID_HANDLE ((void *)(size_t)-1)

__declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char *name, char *buffer, unsigned long size);
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
__declspec(dllimport) void *__stdcall CreateFileA(const char *path, unsigned long access, unsigned long share,
                                                  void *security, unsigned long disposition, unsigned long attributes, void *template_file);
__declspec(dllimport) int __stdcall WriteFile(void *handle, const void *buffer, unsigned long size, unsigned long *written, void *overlapped);
__declspec(dllimport) int __stdcall CloseHandle(void *handle);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(long long *counter);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(long long *frequency);

static void *profile_file;
static volatile int profile_lock;
static int profile_initialized;
static int profile_disabled;
static int profile_worker_only;
static unsigned long profile_initial_thread;
static unsigned long long profile_skip_events;
static unsigned long long profile_max_events;
static unsigned long long profile_event_count;
static unsigned long long profile_frequency;
static size_t profile_buffer_length;
static char profile_buffer[PROFILE_BUFFER_SIZE];

static unsigned long long profile_parse_uint(const char *text) PROFILE_NOINSTR;
static unsigned long long profile_parse_uint(const char *text) {
    unsigned long long value = 0ULL;
    size_t index = 0U;

    if (text[0] == '\0') return 0ULL;
    while (text[index] >= '0' && text[index] <= '9') {
        unsigned int digit = (unsigned int)(text[index] - '0');
        if (value > (~0ULL - digit) / 10ULL) return 0ULL;
        value = value * 10ULL + digit;
        index++;
    }
    return text[index] == '\0' ? value : 0ULL;
}

static int profile_is_disabled(const char *path) PROFILE_NOINSTR;
static int profile_is_disabled(const char *path) {
    return path[0] == '\0' || (path[0] == '0' && path[1] == '\0') ||
           (path[0] == 'n' && path[1] == 'o' && path[2] == '\0') ||
           (path[0] == 'o' && path[1] == 'f' && path[2] == 'f' && path[3] == '\0') ||
           (path[0] == 'f' && path[1] == 'a' && path[2] == 'l' && path[3] == 's' && path[4] == 'e' && path[5] == '\0');
}

static void profile_flush_locked(void) PROFILE_NOINSTR;
static void profile_flush_locked(void) {
    size_t offset = 0U;

    while (profile_buffer_length > offset && !profile_disabled) {
        unsigned long written = 0UL;
        if (!WriteFile(profile_file, profile_buffer + offset, (unsigned long)(profile_buffer_length - offset), &written, 0) || written == 0UL) {
            profile_disabled = 1;
            break;
        }
        offset += written;
    }
    profile_buffer_length = 0U;
}

static void profile_initialize_locked(void) PROFILE_NOINSTR;
static void profile_initialize_locked(void) {
    char path[512];
    char value[32];
    unsigned long length;
    long long frequency;

    profile_initialized = 1;
    length = GetEnvironmentVariableA("NEWOS_PROFILE", path, sizeof(path));
    if (length == 0UL || length >= sizeof(path) || profile_is_disabled(path)) {
        profile_disabled = 1;
        return;
    }
    length = GetEnvironmentVariableA("NEWOS_PROFILE_SKIP_EVENTS", value, sizeof(value));
    if (length != 0UL && length < sizeof(value)) profile_skip_events = profile_parse_uint(value);
    length = GetEnvironmentVariableA("NEWOS_PROFILE_MAX_EVENTS", value, sizeof(value));
    if (length != 0UL && length < sizeof(value)) profile_max_events = profile_parse_uint(value);
    length = GetEnvironmentVariableA("NEWOS_PROFILE_WORKER_ONLY", value, sizeof(value));
    if (length != 0UL && length < sizeof(value) && !profile_is_disabled(value)) profile_worker_only = 1;
    profile_initial_thread = GetCurrentThreadId();
    if (QueryPerformanceFrequency(&frequency) && frequency > 0) {
        __atomic_store_n(&profile_frequency, (unsigned long long)frequency, __ATOMIC_RELEASE);
    }
    profile_file = CreateFileA(path, PROFILE_GENERIC_WRITE, 0, 0, PROFILE_CREATE_ALWAYS, PROFILE_FILE_ATTRIBUTE_NORMAL, 0);
    if (profile_file == 0 || profile_file == PROFILE_INVALID_HANDLE) profile_disabled = 1;
}

static size_t profile_append_uint(char *text, size_t offset, unsigned long long value, unsigned int base) PROFILE_NOINSTR;
static size_t profile_append_uint(char *text, size_t offset, unsigned long long value, unsigned int base) {
    static const char digits[] = "0123456789abcdef";
    char reversed[32];
    size_t count = 0U;

    do {
        reversed[count++] = digits[value % base];
        value /= base;
    } while (value != 0ULL);
    while (count != 0U) text[offset++] = reversed[--count];
    return offset;
}

static void profile_event(const char *kind, void *address) PROFILE_NOINSTR;
static void profile_event(const char *kind, void *address) {
    char line[128];
    size_t length = 0U;
    unsigned long thread_id = GetCurrentThreadId();
    unsigned long long timestamp_ns = 0ULL;
    unsigned long long frequency = __atomic_load_n(&profile_frequency, __ATOMIC_ACQUIRE);
    long long counter;

    if (frequency != 0ULL && QueryPerformanceCounter(&counter)) {
        unsigned long long ticks = (unsigned long long)counter;
        timestamp_ns = (ticks / frequency) * 1000000000ULL +
                       ((ticks % frequency) * 1000000000ULL) / frequency;
    }
    while (__atomic_exchange_n(&profile_lock, 1, __ATOMIC_ACQUIRE)) {
    }
    if (!profile_initialized) profile_initialize_locked();
    if (profile_disabled || (profile_worker_only && thread_id == profile_initial_thread)) goto done;
    if (timestamp_ns == 0ULL && profile_frequency != 0ULL && QueryPerformanceCounter(&counter)) {
        unsigned long long ticks = (unsigned long long)counter;
        timestamp_ns = (ticks / profile_frequency) * 1000000000ULL +
                       ((ticks % profile_frequency) * 1000000000ULL) / profile_frequency;
    }
    if (profile_event_count++ < profile_skip_events) goto done;
    if (profile_max_events != 0ULL && profile_event_count - profile_skip_events > profile_max_events) {
        profile_flush_locked();
        profile_disabled = 1;
        goto done;
    }
    while (kind[length] != '\0') { line[length] = kind[length]; length++; }
    line[length++] = ' ';
    length = profile_append_uint(line, length, timestamp_ns, 10U);
    line[length++] = ' ';
    length = profile_append_uint(line, length, thread_id, 10U);
    line[length++] = ' ';
    line[length++] = '0';
    line[length++] = 'x';
    length = profile_append_uint(line, length, (unsigned long long)(size_t)address, 16U);
    line[length++] = '\n';
    if (profile_buffer_length + length > sizeof(profile_buffer)) profile_flush_locked();
    if (!profile_disabled) {
        size_t index;
        for (index = 0U; index < length; ++index) profile_buffer[profile_buffer_length + index] = line[index];
        profile_buffer_length += length;
        if (profile_max_events != 0ULL && profile_event_count - profile_skip_events >= profile_max_events) {
            profile_flush_locked();
            profile_disabled = 1;
        }
    }
done:
    __atomic_store_n(&profile_lock, 0, __ATOMIC_RELEASE);
}

void __cyg_profile_func_enter(void *function, void *caller) PROFILE_NOINSTR;
void __cyg_profile_func_exit(void *function, void *caller) PROFILE_NOINSTR;
void windows_profile_finish(void) PROFILE_NOINSTR;

void __cyg_profile_func_enter(void *function, void *caller) {
    (void)caller;
    profile_event("enter", function);
}

void __cyg_profile_func_exit(void *function, void *caller) {
    (void)caller;
    profile_event("exit", function);
}

void windows_profile_finish(void) {
    while (__atomic_exchange_n(&profile_lock, 1, __ATOMIC_ACQUIRE)) {
    }
    if (profile_initialized && !profile_disabled) profile_flush_locked();
    if (profile_file != 0 && profile_file != PROFILE_INVALID_HANDLE) (void)CloseHandle(profile_file);
    profile_file = 0;
    profile_disabled = 1;
    __atomic_store_n(&profile_lock, 0, __ATOMIC_RELEASE);
}