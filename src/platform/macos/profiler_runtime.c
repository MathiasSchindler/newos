#include <stddef.h>

#include "../../arch/aarch64/macos/syscall.h"

#if defined(__GNUC__) || defined(__clang__)
#define NEWOS_PROFILER_NOINSTR __attribute__((no_instrument_function))
#else
#define NEWOS_PROFILER_NOINSTR
#endif

#define NEWOS_PROFILE_BUFFER_SIZE 65536U
#define NEWOS_PROFILE_O_WRONLY 0x0001
#define NEWOS_PROFILE_O_CREAT  0x0200
#define NEWOS_PROFILE_O_TRUNC  0x0400

extern char **environ;

static int newos_profile_fd = -2;
static int newos_profile_initialized;
static int newos_profile_disabled;
static unsigned int newos_profile_depth;
static size_t newos_profile_buffer_length;
static char newos_profile_buffer[NEWOS_PROFILE_BUFFER_SIZE];

static int newos_profile_streq(const char *left, const char *right) NEWOS_PROFILER_NOINSTR;
static int newos_profile_text_is_disabled(const char *text) NEWOS_PROFILER_NOINSTR;
static size_t newos_profile_strlen(const char *text) NEWOS_PROFILER_NOINSTR;
static int newos_profile_env_name_matches(const char *entry, const char *name) NEWOS_PROFILER_NOINSTR;
static const char *newos_profile_getenv(const char *name) NEWOS_PROFILER_NOINSTR;
static int newos_profile_open_write(const char *path) NEWOS_PROFILER_NOINSTR;
static long newos_profile_write(int fd, const void *buffer, size_t size) NEWOS_PROFILER_NOINSTR;
static unsigned long long newos_profile_counter_ticks(void) NEWOS_PROFILER_NOINSTR;
static unsigned long long newos_profile_counter_frequency(void) NEWOS_PROFILER_NOINSTR;
static unsigned long long newos_profile_time_ns(void) NEWOS_PROFILER_NOINSTR;
static void newos_profile_initialize(void) NEWOS_PROFILER_NOINSTR;
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
    newos_profile_fd = newos_profile_open_write(path);
    if (newos_profile_fd < 0) newos_profile_disabled = 1;
}

static void newos_profile_flush(void) {
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

static void newos_profile_append_char(char ch) {
    if (newos_profile_buffer_length >= sizeof(newos_profile_buffer)) newos_profile_flush();
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
    if (!newos_profile_initialized) newos_profile_initialize();
    if (newos_profile_disabled || newos_profile_fd < 0) return;
    newos_profile_append_cstr(kind);
    newos_profile_append_char(' ');
    newos_profile_append_uint(newos_profile_time_ns());
    newos_profile_append_char(' ');
    newos_profile_append_hex((unsigned long long)(size_t)function_address);
    newos_profile_append_char('\n');
}

void __cyg_profile_func_enter(void *this_fn, void *call_site) NEWOS_PROFILER_NOINSTR;
void __cyg_profile_func_exit(void *this_fn, void *call_site) NEWOS_PROFILER_NOINSTR;

void __cyg_profile_func_enter(void *this_fn, void *call_site) {
    (void)call_site;
    newos_profile_depth += 1U;
    newos_profile_event("enter", this_fn);
}

void __cyg_profile_func_exit(void *this_fn, void *call_site) {
    (void)call_site;
    newos_profile_event("exit", this_fn);
    if (newos_profile_depth > 0U) newos_profile_depth -= 1U;
    if (newos_profile_depth == 0U) newos_profile_flush();
}
