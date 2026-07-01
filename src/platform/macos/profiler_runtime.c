#include <stddef.h>

#include "../../arch/aarch64/macos/syscall.h"

#if defined(__GNUC__) || defined(__clang__)
#define NEWOS_PROFILER_NOINSTR __attribute__((no_instrument_function))
#else
#define NEWOS_PROFILER_NOINSTR
#endif

#define NEWOS_PROFILE_BUFFER_SIZE 65536U
#define NEWOS_PROFILE_HOOK_THREADS 128U
#define NEWOS_PROFILE_O_WRONLY 0x0001
#define NEWOS_PROFILE_O_CREAT  0x0200
#define NEWOS_PROFILE_O_TRUNC  0x0400

extern char **environ;

static int newos_profile_fd = -2;
static int newos_profile_initialized;
static int newos_profile_disabled;
static int newos_profile_limit_reached;
static unsigned int newos_profile_depth;
static unsigned long long newos_profile_event_count;
static unsigned long long newos_profile_skip_events;
static unsigned long long newos_profile_max_events;
static unsigned long long newos_profile_initial_thread_id;
static int newos_profile_worker_only;
static volatile int newos_profile_write_lock;
static volatile int newos_profile_hook_lock;
static unsigned long long newos_profile_hook_thread_ids[NEWOS_PROFILE_HOOK_THREADS];
static unsigned char newos_profile_hook_thread_used[NEWOS_PROFILE_HOOK_THREADS];
static unsigned char newos_profile_hook_thread_active[NEWOS_PROFILE_HOOK_THREADS];
static size_t newos_profile_buffer_length;
static char newos_profile_buffer[NEWOS_PROFILE_BUFFER_SIZE];

static int newos_profile_streq(const char *left, const char *right) NEWOS_PROFILER_NOINSTR;
static int newos_profile_text_is_disabled(const char *text) NEWOS_PROFILER_NOINSTR;
static size_t newos_profile_strlen(const char *text) NEWOS_PROFILER_NOINSTR;
static unsigned long long newos_profile_parse_uint(const char *text) NEWOS_PROFILER_NOINSTR;
static int newos_profile_env_name_matches(const char *entry, const char *name) NEWOS_PROFILER_NOINSTR;
static const char *newos_profile_getenv(const char *name) NEWOS_PROFILER_NOINSTR;
static int newos_profile_open_write(const char *path) NEWOS_PROFILER_NOINSTR;
static long newos_profile_write(int fd, const void *buffer, size_t size) NEWOS_PROFILER_NOINSTR;
static unsigned long long newos_profile_counter_ticks(void) NEWOS_PROFILER_NOINSTR;
static unsigned long long newos_profile_counter_frequency(void) NEWOS_PROFILER_NOINSTR;
static unsigned long long newos_profile_time_ns(void) NEWOS_PROFILER_NOINSTR;
static unsigned long long newos_profile_thread_id(void) NEWOS_PROFILER_NOINSTR;
static unsigned long long newos_profile_event_limit(void) NEWOS_PROFILER_NOINSTR;
static void newos_profile_lock(void) NEWOS_PROFILER_NOINSTR;
static void newos_profile_unlock(void) NEWOS_PROFILER_NOINSTR;
static void newos_profile_hook_table_lock(void) NEWOS_PROFILER_NOINSTR;
static void newos_profile_hook_table_unlock(void) NEWOS_PROFILER_NOINSTR;
static int newos_profile_skip_hook(void) NEWOS_PROFILER_NOINSTR;
static int newos_profile_hook_enter(void) NEWOS_PROFILER_NOINSTR;
static void newos_profile_hook_exit(void) NEWOS_PROFILER_NOINSTR;
static void newos_profile_initialize(void) NEWOS_PROFILER_NOINSTR;
static void newos_profile_flush_locked(void) NEWOS_PROFILER_NOINSTR;
static void newos_profile_flush(void) NEWOS_PROFILER_NOINSTR;
static void newos_profile_append_char(char ch) NEWOS_PROFILER_NOINSTR;
static void newos_profile_append_cstr(const char *text) NEWOS_PROFILER_NOINSTR;
static void newos_profile_append_uint(unsigned long long value) NEWOS_PROFILER_NOINSTR;
static void newos_profile_append_hex(unsigned long long value) NEWOS_PROFILER_NOINSTR;
static void newos_profile_event(const char *kind, void *function_address) NEWOS_PROFILER_NOINSTR;

static int newos_profile_streq(const char *left, const char *right) {
    size_t index = 0U;

    if (left == 0 || right == 0) return left == right;
    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) return 0;
        index += 1U;
    }
    return left[index] == right[index];
}

static int newos_profile_text_is_disabled(const char *text) {
    return text == 0 || text[0] == '\0' ||
           newos_profile_streq(text, "0") ||
           newos_profile_streq(text, "off") ||
           newos_profile_streq(text, "false") ||
           newos_profile_streq(text, "no");
}

static size_t newos_profile_strlen(const char *text) {
    size_t length = 0U;
    while (text != 0 && text[length] != '\0') length += 1U;
    return length;
}

static unsigned long long newos_profile_parse_uint(const char *text) {
    unsigned long long value = 0ULL;
    size_t index = 0U;

    if (text == 0 || text[0] == '\0') return 0ULL;
    while (text[index] >= '0' && text[index] <= '9') {
        value = (value * 10ULL) + (unsigned long long)(text[index] - '0');
        index += 1U;
    }
    return text[index] == '\0' ? value : 0ULL;
}

static int newos_profile_env_name_matches(const char *entry, const char *name) {
    size_t index = 0U;

    if (entry == 0 || name == 0) return 0;
    while (name[index] != '\0') {
        if (entry[index] != name[index]) return 0;
        index += 1U;
    }
    return entry[index] == '=';
}

static const char *newos_profile_getenv(const char *name) {
    size_t index;
    size_t name_length;

    if (environ == 0 || name == 0) return 0;
    name_length = newos_profile_strlen(name);
    for (index = 0U; environ[index] != 0; ++index) {
        if (newos_profile_env_name_matches(environ[index], name)) {
            return environ[index] + name_length + 1U;
        }
    }
    return 0;
}

static int newos_profile_open_write(const char *path) {
    return (int)darwin_syscall3(DARWIN_SYS_OPEN,
                                (long)path,
                                NEWOS_PROFILE_O_WRONLY | NEWOS_PROFILE_O_CREAT | NEWOS_PROFILE_O_TRUNC,
                                0644);
}

static long newos_profile_write(int fd, const void *buffer, size_t size) {
    return darwin_syscall3(DARWIN_SYS_WRITE, (long)fd, (long)buffer, (long)size);
}

static unsigned long long newos_profile_counter_ticks(void) {
#if defined(__aarch64__) || defined(__arm64__)
    unsigned long long ticks;

    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
#else
    return 0ULL;
#endif
}

static unsigned long long newos_profile_counter_frequency(void) {
#if defined(__aarch64__) || defined(__arm64__)
    unsigned long long frequency;

    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    return frequency;
#else
    return 0ULL;
#endif
}

static unsigned long long newos_profile_time_ns(void) {
    unsigned long long ticks = newos_profile_counter_ticks();
    unsigned long long frequency = newos_profile_counter_frequency();
    unsigned long long seconds;
    unsigned long long remainder;

    if (frequency == 0ULL) return ticks;
    seconds = ticks / frequency;
    remainder = ticks % frequency;
    return (seconds * 1000000000ULL) + ((remainder * 1000000000ULL) / frequency);
}

static unsigned long long newos_profile_thread_id(void) {
#if defined(__aarch64__) || defined(__arm64__)
    unsigned long value;

    __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(value));
    if (value != 0UL) return (unsigned long long)value;
#endif
    return 0ULL;
}

static unsigned long long newos_profile_event_limit(void) {
    unsigned long long limit;

    if (newos_profile_max_events == 0ULL) return 0ULL;
    limit = newos_profile_skip_events + newos_profile_max_events;
    if (limit < newos_profile_skip_events) return ~0ULL;
    return limit;
}

static void newos_profile_lock(void) {
    while (__atomic_exchange_n(&newos_profile_write_lock, 1, __ATOMIC_ACQUIRE) != 0) {
    }
}

static void newos_profile_unlock(void) {
    __atomic_store_n(&newos_profile_write_lock, 0, __ATOMIC_RELEASE);
}

static void newos_profile_hook_table_lock(void) {
    while (__atomic_exchange_n(&newos_profile_hook_lock, 1, __ATOMIC_ACQUIRE) != 0) {
    }
}

static void newos_profile_hook_table_unlock(void) {
    __atomic_store_n(&newos_profile_hook_lock, 0, __ATOMIC_RELEASE);
}

static int newos_profile_skip_hook(void) {
    return newos_profile_initialized && newos_profile_disabled;
}

static int newos_profile_hook_enter(void) {
    unsigned long long thread_id = newos_profile_thread_id();
    size_t i;
    size_t free_slot = NEWOS_PROFILE_HOOK_THREADS;

    newos_profile_hook_table_lock();
    for (i = 0U; i < NEWOS_PROFILE_HOOK_THREADS; ++i) {
        if (newos_profile_hook_thread_used[i]) {
            if (newos_profile_hook_thread_ids[i] == thread_id) {
                if (newos_profile_hook_thread_active[i]) {
                    newos_profile_hook_table_unlock();
                    return 0;
                }
                newos_profile_hook_thread_active[i] = 1U;
                newos_profile_hook_table_unlock();
                return 1;
            }
        } else if (free_slot == NEWOS_PROFILE_HOOK_THREADS) {
            free_slot = i;
        }
    }
    if (free_slot == NEWOS_PROFILE_HOOK_THREADS) {
        newos_profile_hook_table_unlock();
        return 0;
    }
    newos_profile_hook_thread_used[free_slot] = 1U;
    newos_profile_hook_thread_ids[free_slot] = thread_id;
    newos_profile_hook_thread_active[free_slot] = 1U;
    newos_profile_hook_table_unlock();
    return 1;
}

static void newos_profile_hook_exit(void) {
    unsigned long long thread_id = newos_profile_thread_id();
    size_t i;

    newos_profile_hook_table_lock();
    for (i = 0U; i < NEWOS_PROFILE_HOOK_THREADS; ++i) {
        if (newos_profile_hook_thread_used[i] && newos_profile_hook_thread_ids[i] == thread_id) {
            newos_profile_hook_thread_active[i] = 0U;
            break;
        }
    }
    newos_profile_hook_table_unlock();
}

static void newos_profile_initialize(void) {
    const char *path;

    if (newos_profile_initialized) return;
    newos_profile_initialized = 1;
    path = newos_profile_getenv("NEWOS_PROFILE");
    if (newos_profile_text_is_disabled(path)) {
        newos_profile_disabled = 1;
        newos_profile_fd = -1;
        return;
    }
    newos_profile_skip_events = newos_profile_parse_uint(newos_profile_getenv("NEWOS_PROFILE_SKIP_EVENTS"));
    newos_profile_max_events = newos_profile_parse_uint(newos_profile_getenv("NEWOS_PROFILE_MAX_EVENTS"));
    newos_profile_initial_thread_id = newos_profile_thread_id();
    newos_profile_worker_only = !newos_profile_text_is_disabled(newos_profile_getenv("NEWOS_PROFILE_WORKER_ONLY"));
    newos_profile_fd = newos_profile_open_write(path);
    if (newos_profile_fd < 0) newos_profile_disabled = 1;
}

static void newos_profile_flush_locked(void) {
    size_t written = 0U;

    if (newos_profile_fd < 0 || newos_profile_buffer_length == 0U) return;
    while (written < newos_profile_buffer_length) {
        long chunk = newos_profile_write(newos_profile_fd,
                                         newos_profile_buffer + written,
                                         newos_profile_buffer_length - written);
        if (chunk <= 0) {
            newos_profile_disabled = 1;
            newos_profile_buffer_length = 0U;
            return;
        }
        written += (size_t)chunk;
    }
    newos_profile_buffer_length = 0U;
}

static void newos_profile_flush(void) {
    newos_profile_lock();
    newos_profile_flush_locked();
    newos_profile_unlock();
}

static void newos_profile_append_char(char ch) {
    if (newos_profile_buffer_length >= sizeof(newos_profile_buffer)) newos_profile_flush_locked();
    if (newos_profile_buffer_length < sizeof(newos_profile_buffer)) {
        newos_profile_buffer[newos_profile_buffer_length++] = ch;
    }
}

static void newos_profile_append_cstr(const char *text) {
    while (text != 0 && *text != '\0') newos_profile_append_char(*text++);
}

static void newos_profile_append_uint(unsigned long long value) {
    char digits[32];
    size_t count = 0U;

    if (value == 0ULL) {
        newos_profile_append_char('0');
        return;
    }
    while (value != 0ULL && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (count > 0U) newos_profile_append_char(digits[--count]);
}

static void newos_profile_append_hex(unsigned long long value) {
    static const char hex_digits[] = "0123456789abcdef";
    char digits[32];
    size_t count = 0U;

    newos_profile_append_cstr("0x");
    if (value == 0ULL) {
        newos_profile_append_char('0');
        return;
    }
    while (value != 0ULL && count < sizeof(digits)) {
        digits[count++] = hex_digits[value & 0xfULL];
        value >>= 4U;
    }
    while (count > 0U) newos_profile_append_char(digits[--count]);
}

static void newos_profile_event(const char *kind, void *function_address) {
    unsigned long long event_index;
    unsigned long long event_limit;
    unsigned long long thread_id;

    if (!newos_profile_initialized) newos_profile_initialize();
    if (newos_profile_fd < 0) return;
    if (newos_profile_disabled) return;
    thread_id = newos_profile_thread_id();
    if (newos_profile_worker_only && thread_id == newos_profile_initial_thread_id) return;
    event_index = __atomic_fetch_add(&newos_profile_event_count, 1ULL, __ATOMIC_RELAXED);
    if (event_index < newos_profile_skip_events) return;
    event_limit = newos_profile_event_limit();
    if (event_limit != 0ULL && event_index >= event_limit) {
        newos_profile_flush();
        newos_profile_limit_reached = 1;
        newos_profile_disabled = 1;
        return;
    }
    newos_profile_lock();
    if ((newos_profile_disabled && !newos_profile_limit_reached) || newos_profile_fd < 0) {
        newos_profile_unlock();
        return;
    }
    newos_profile_append_cstr(kind);
    newos_profile_append_char(' ');
    newos_profile_append_uint(newos_profile_time_ns());
    newos_profile_append_char(' ');
    newos_profile_append_uint(thread_id);
    newos_profile_append_char(' ');
    newos_profile_append_hex((unsigned long long)(size_t)function_address);
    newos_profile_append_char('\n');
    if (event_limit != 0ULL && event_index + 1ULL >= event_limit) {
        newos_profile_flush_locked();
        newos_profile_limit_reached = 1;
        newos_profile_disabled = 1;
    }
    newos_profile_unlock();
}

void __cyg_profile_func_enter(void *this_fn, void *call_site) NEWOS_PROFILER_NOINSTR;
void __cyg_profile_func_exit(void *this_fn, void *call_site) NEWOS_PROFILER_NOINSTR;

void __cyg_profile_func_enter(void *this_fn, void *call_site) NEWOS_PROFILER_NOINSTR;
void __cyg_profile_func_enter(void *this_fn, void *call_site) {
    (void)call_site;
    if (newos_profile_skip_hook()) return;
    if (!newos_profile_hook_enter()) return;
    newos_profile_depth += 1U;
    newos_profile_event("enter", this_fn);
    newos_profile_hook_exit();
}

void __cyg_profile_func_exit(void *this_fn, void *call_site) NEWOS_PROFILER_NOINSTR;
void __cyg_profile_func_exit(void *this_fn, void *call_site) {
    (void)call_site;
    if (newos_profile_skip_hook()) return;
    if (!newos_profile_hook_enter()) return;
    newos_profile_event("exit", this_fn);
    if (newos_profile_depth > 0U) newos_profile_depth -= 1U;
    if (newos_profile_depth == 0U) newos_profile_flush();
    newos_profile_hook_exit();
}
