#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#if defined(__linux__)
#include "../platform/linux/common.h"
#endif

#define PERF_MAX_SYMBOLS 8192U
#define PERF_MAX_ROWS 8192U
#define PERF_NAME_CAPACITY 256U
#define PERF_LINE_CAPACITY 1024U
#define PERF_ROW_HASH_SIZE 32768U
#define PERF_DEFAULT_FREQ 997ULL
#define PERF_DEFAULT_COUNT 20U
#define PERF_DEFAULT_RING_PAGES 64U
#define PERF_PAGE_SIZE 4096U

#if defined(__linux__)
#define PERF_TYPE_SOFTWARE 1U
#define PERF_COUNT_SW_CPU_CLOCK 0ULL
#define PERF_SAMPLE_IP 1ULL
#define PERF_RECORD_LOST 2U
#define PERF_RECORD_SAMPLE 9U
#define PERF_EVENT_IOC_ENABLE 0x2400UL
#define PERF_EVENT_IOC_DISABLE 0x2401UL
#define PERF_EVENT_IOC_RESET 0x2403UL
#define PERF_ATTR_DISABLED (1ULL << 0)
#define PERF_ATTR_EXCLUDE_KERNEL (1ULL << 5)
#define PERF_ATTR_EXCLUDE_HV (1ULL << 6)
#endif

#if defined(__linux__) && defined(__x86_64__)
static long perf_linux_syscall2(long number, long arg0, long arg1) {
    register long rax __asm__("rax") = number;
    register long rdi __asm__("rdi") = arg0;
    register long rsi __asm__("rsi") = arg1;

    __asm__ volatile("syscall" : "+a"(rax) : "r"(rdi), "r"(rsi) : "rcx", "r11", "memory");
    return rax;
}

static long perf_linux_syscall3(long number, long arg0, long arg1, long arg2) {
    register long rax __asm__("rax") = number;
    register long rdi __asm__("rdi") = arg0;
    register long rsi __asm__("rsi") = arg1;
    register long rdx __asm__("rdx") = arg2;

    __asm__ volatile("syscall" : "+a"(rax) : "r"(rdi), "r"(rsi), "r"(rdx) : "rcx", "r11", "memory");
    return rax;
}

static long perf_linux_syscall5(long number, long arg0, long arg1, long arg2, long arg3, long arg4) {
    register long rax __asm__("rax") = number;
    register long rdi __asm__("rdi") = arg0;
    register long rsi __asm__("rsi") = arg1;
    register long rdx __asm__("rdx") = arg2;
    register long r10 __asm__("r10") = arg3;
    register long r8 __asm__("r8") = arg4;

    __asm__ volatile("syscall" : "+a"(rax) : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    return rax;
}

static long perf_linux_syscall6(long number, long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) {
    register long rax __asm__("rax") = number;
    register long rdi __asm__("rdi") = arg0;
    register long rsi __asm__("rsi") = arg1;
    register long rdx __asm__("rdx") = arg2;
    register long r10 __asm__("r10") = arg3;
    register long r8 __asm__("r8") = arg4;
    register long r9 __asm__("r9") = arg5;

    __asm__ volatile("syscall" : "+a"(rax) : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return rax;
}
#elif defined(__linux__) && defined(__aarch64__)
static long perf_linux_syscall2(long number, long arg0, long arg1) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;

    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return x0;
}

static long perf_linux_syscall3(long number, long arg0, long arg1, long arg2) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;

    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}

static long perf_linux_syscall5(long number, long arg0, long arg1, long arg2, long arg3, long arg4) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;
    register long x3 __asm__("x3") = arg3;
    register long x4 __asm__("x4") = arg4;

    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4) : "memory");
    return x0;
}

static long perf_linux_syscall6(long number, long arg0, long arg1, long arg2, long arg3, long arg4, long arg5) {
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = arg0;
    register long x1 __asm__("x1") = arg1;
    register long x2 __asm__("x2") = arg2;
    register long x3 __asm__("x3") = arg3;
    register long x4 __asm__("x4") = arg4;
    register long x5 __asm__("x5") = arg5;

    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}
#endif

typedef struct {
    unsigned long long address;
    unsigned long long samples;
    unsigned long long lost;
} PerfRow;

typedef struct {
    unsigned long long address;
    int function_symbol;
    char name[PERF_NAME_CAPACITY];
} PerfSymbol;

typedef struct {
    size_t row_index;
} PerfPrintRow;

static PerfRow perf_rows[PERF_MAX_ROWS];
static PerfSymbol perf_symbols[PERF_MAX_SYMBOLS];
static PerfPrintRow perf_print_rows[PERF_MAX_ROWS];
static int perf_row_hash[PERF_ROW_HASH_SIZE];
static int perf_row_hash_initialized;
static size_t perf_row_count;
static size_t perf_symbol_count;
static unsigned long long perf_total_samples;
static unsigned long long perf_lost_samples;

static void print_usage(void) {
    tool_write_usage("perf", "[-m MAP] [-F HZ] [-n COUNT] [--csv] [--kernel] -- COMMAND [ARG ...]");
}

static void print_open_failure(long err) {
    char value[64];
    int fd;
    long bytes;

    rt_write_cstr(2, "perf: perf_event_open failed");
    if (err != 0) {
        rt_write_cstr(2, " errno=");
        rt_write_uint(2, (unsigned long long)err);
    }
    rt_write_char(2, '\n');
    if (err == 7) {
        rt_write_line(2, "perf: the kernel rejected the perf_event_attr layout or size");
        return;
    }
    if (err != 1 && err != 13) {
        return;
    }
    rt_write_line(2, "perf: Linux may restrict profiling through kernel.perf_event_paranoid");
    fd = platform_open_read("/proc/sys/kernel/perf_event_paranoid");
    if (fd >= 0) {
        bytes = platform_read(fd, value, sizeof(value) - 1U);
        platform_close(fd);
        if (bytes > 0) {
            value[bytes] = '\0';
            rt_trim_newline(value);
            rt_write_cstr(2, "perf: current perf_event_paranoid=");
            rt_write_line(2, value);
        }
    }
    rt_write_line(2, "perf: for user-space sampling, try: sudo sysctl kernel.perf_event_paranoid=1");
    rt_write_line(2, "perf: for broader kernel/hardware access, try: sudo sysctl kernel.perf_event_paranoid=0");
}

static int parse_unsigned_auto(const char *text, unsigned long long *value_out) {
    unsigned long long value = 0ULL;
    size_t i = 0U;
    int is_hex = 0;
    int saw_digit = 0;

    if (text == 0 || text[0] == '\0') return -1;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        is_hex = 1;
        i = 2U;
    }
    for (; text[i] != '\0'; ++i) {
        int digit;

        if (is_hex) {
            digit = tool_hex_value(text[i]);
            if (digit < 0) return -1;
            value = (value << 4U) | (unsigned long long)digit;
        } else {
            if (text[i] < '0' || text[i] > '9') return -1;
            value = value * 10ULL + (unsigned long long)(text[i] - '0');
        }
        saw_digit = 1;
    }
    if (!saw_digit) return -1;
    *value_out = value;
    return 0;
}

static int parse_address_token(const char *text, unsigned long long *value_out) {
    size_t i;
    int has_hex_letter = 0;

    if (text == 0 || text[0] == '\0') return -1;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) return parse_unsigned_auto(text, value_out);
    for (i = 0U; text[i] != '\0'; ++i) {
        int digit = tool_hex_value(text[i]);

        if (digit < 0) return -1;
        if ((text[i] >= 'a' && text[i] <= 'f') || (text[i] >= 'A' && text[i] <= 'F')) has_hex_letter = 1;
    }
    if (!has_hex_letter && rt_strlen(text) < 9U) return parse_unsigned_auto(text, value_out);
    *value_out = 0ULL;
    for (i = 0U; text[i] != '\0'; ++i) {
        int digit = tool_hex_value(text[i]);
        if (digit < 0) return -1;
        *value_out = (*value_out << 4U) | (unsigned long long)digit;
    }
    return 0;
}

static int next_token(const char **cursor_io, char *token, size_t token_size) {
    const char *cursor = *cursor_io;
    size_t length = 0U;

    while (*cursor != '\0' && tool_ascii_is_token_space(*cursor)) cursor++;
    if (*cursor == '\0' || *cursor == '#') {
        *cursor_io = cursor;
        return 0;
    }
    while (*cursor != '\0' && !tool_ascii_is_token_space(*cursor)) {
        if (length + 1U < token_size) token[length++] = *cursor;
        cursor++;
    }
    token[length] = '\0';
    *cursor_io = cursor;
    return length == 0U ? 0 : 1;
}

static const char *display_symbol_name(const char *name) {
    if (name != 0 && name[0] == '_' && name[1] != '\0') return name + 1;
    return name;
}

static void add_symbol(unsigned long long address, const char *name, int function_symbol) {
    size_t i;

    if (name == 0 || name[0] == '\0') return;
    for (i = 0U; i < perf_symbol_count; ++i) {
        if (perf_symbols[i].address == address) {
            if (function_symbol || !perf_symbols[i].function_symbol) {
                perf_symbols[i].function_symbol = function_symbol;
                rt_copy_string(perf_symbols[i].name, sizeof(perf_symbols[i].name), name);
            }
            return;
        }
    }
    if (perf_symbol_count >= PERF_MAX_SYMBOLS) return;
    perf_symbols[perf_symbol_count].address = address;
    perf_symbols[perf_symbol_count].function_symbol = function_symbol;
    rt_copy_string(perf_symbols[perf_symbol_count].name, sizeof(perf_symbols[perf_symbol_count].name), name);
    perf_symbol_count += 1U;
}

static int parse_linker_map_symbol_line(const char *line) {
    const char *cursor = line;
    char keyword[32];
    char address_token[64];
    char size_token[64];
    char section[64];
    char name[PERF_NAME_CAPACITY];
    unsigned long long address;

    if (!next_token(&cursor, keyword, sizeof(keyword)) || rt_strcmp(keyword, "symbol") != 0) return 0;
    if (!next_token(&cursor, address_token, sizeof(address_token)) ||
        !next_token(&cursor, size_token, sizeof(size_token)) ||
        !next_token(&cursor, section, sizeof(section)) ||
        !next_token(&cursor, name, sizeof(name))) return 0;
    (void)size_token;
    if (rt_strcmp(section, "__TEXT,__text") != 0 || parse_address_token(address_token, &address) != 0) return 0;
    add_symbol(address, display_symbol_name(name), 1);
    return 1;
}

static const char *linux_newlinker_text_symbol_name(const char *section) {
    const char *name;

    if (!tool_starts_with(section, ".text.")) return 0;
    name = section + 6;
    if (tool_starts_with(name, "startup.")) name += 8;
    else if (tool_starts_with(name, "unlikely.")) name += 9;
    else if (tool_starts_with(name, "hot.")) name += 4;
    return name[0] == '\0' ? 0 : name;
}

static int parse_linux_newlinker_map_line(const char *line) {
    const char *cursor = line;
    char address_token[64];
    char size_token[64];
    char section[PERF_NAME_CAPACITY];
    unsigned long long address;
    unsigned long long size;
    const char *name;

    if (!next_token(&cursor, address_token, sizeof(address_token)) ||
        parse_address_token(address_token, &address) != 0 ||
        !next_token(&cursor, size_token, sizeof(size_token)) ||
        parse_unsigned_auto(size_token, &size) != 0 ||
        !next_token(&cursor, section, sizeof(section)) ||
        section[0] != '.') return 0;
    (void)size;
    name = linux_newlinker_text_symbol_name(section);
    if (name != 0) add_symbol(address, name, 1);
    return 1;
}

static int read_symbols(const char *path) {
    int fd;
    int should_close;
    ToolRecordReader reader;
    char line[PERF_LINE_CAPACITY];
    int has_record;

    if (tool_open_input(path, &fd, &should_close) != 0) {
        tool_write_error("perf", "failed to open symbols: ", path);
        return -1;
    }
    tool_record_reader_init(&reader, fd, '\n');
    while (tool_record_reader_next(&reader, line, sizeof(line), &has_record) == 0 && has_record) {
        const char *cursor = line;
        char first[128];
        char second[PERF_NAME_CAPACITY];
        char third[PERF_NAME_CAPACITY];
        unsigned long long address;

        if (parse_linker_map_symbol_line(line) || parse_linux_newlinker_map_line(line)) continue;
        if (!next_token(&cursor, first, sizeof(first))) continue;
        if (parse_address_token(first, &address) != 0) continue;
        if (!next_token(&cursor, second, sizeof(second))) continue;
        if (rt_strlen(second) == 1U && next_token(&cursor, third, sizeof(third))) {
            if (tool_symbol_type_is_function(second)) add_symbol(address, third, 1);
        } else {
            add_symbol(address, second, 1);
        }
    }
    tool_close_input(fd, should_close);
    return 0;
}

static size_t perf_row_hash_slot(unsigned long long address) {
    unsigned long long mixed = address;

    mixed ^= mixed >> 33U;
    mixed *= 0xff51afd7ed558ccdULL;
    mixed ^= mixed >> 33U;
    return (size_t)(mixed & (PERF_ROW_HASH_SIZE - 1U));
}

static void perf_row_hash_init(void) {
    size_t i;

    if (perf_row_hash_initialized) return;
    for (i = 0U; i < PERF_ROW_HASH_SIZE; ++i) perf_row_hash[i] = -1;
    perf_row_hash_initialized = 1;
}

static int symbol_index_for_address(unsigned long long address) {
    size_t i;
    size_t best_index = PERF_MAX_SYMBOLS;
    unsigned long long best_address = 0ULL;

    for (i = 0U; i < perf_symbol_count; ++i) {
        if (perf_symbols[i].address == address) return (int)i;
        if (perf_symbols[i].address < address && (best_index == PERF_MAX_SYMBOLS || perf_symbols[i].address > best_address)) {
            best_index = i;
            best_address = perf_symbols[i].address;
        }
    }
    return best_index == PERF_MAX_SYMBOLS ? -1 : (int)best_index;
}

static const char *symbol_for_row(const PerfRow *row) {
    int symbol_index = symbol_index_for_address(row->address);

    return symbol_index >= 0 ? perf_symbols[(size_t)symbol_index].name : 0;
}

static unsigned long long aggregate_address(unsigned long long ip) {
    int symbol_index = symbol_index_for_address(ip);

    if (symbol_index >= 0) return perf_symbols[(size_t)symbol_index].address;
    return ip;
}

static void add_sample(unsigned long long ip) {
    unsigned long long address = aggregate_address(ip);
    size_t slot;

    perf_row_hash_init();
    slot = perf_row_hash_slot(address);
    while (perf_row_hash[slot] >= 0) {
        size_t row_index = (size_t)perf_row_hash[slot];

        if (perf_rows[row_index].address == address) {
            perf_rows[row_index].samples += 1ULL;
            perf_total_samples += 1ULL;
            return;
        }
        slot = (slot + 1U) & (PERF_ROW_HASH_SIZE - 1U);
    }
    if (perf_row_count >= PERF_MAX_ROWS) {
        perf_lost_samples += 1ULL;
        return;
    }
    perf_rows[perf_row_count].address = address;
    perf_rows[perf_row_count].samples = 1ULL;
    perf_rows[perf_row_count].lost = 0ULL;
    perf_row_hash[slot] = (int)perf_row_count;
    perf_row_count += 1U;
    perf_total_samples += 1ULL;
}

static int compare_print_rows(const void *left_ptr, const void *right_ptr) {
    const PerfPrintRow *left = (const PerfPrintRow *)left_ptr;
    const PerfPrintRow *right = (const PerfPrintRow *)right_ptr;
    const PerfRow *left_row = &perf_rows[left->row_index];
    const PerfRow *right_row = &perf_rows[right->row_index];

    if (left_row->samples > right_row->samples) return -1;
    if (left_row->samples < right_row->samples) return 1;
    if (left_row->address < right_row->address) return -1;
    if (left_row->address > right_row->address) return 1;
    return 0;
}

static void write_hex_address(unsigned long long value) {
    char digits[32];
    size_t count = 0U;

    rt_write_cstr(1, "0x");
    if (value == 0ULL) {
        rt_write_char(1, '0');
        return;
    }
    while (value != 0ULL && count < sizeof(digits)) {
        unsigned int nibble = (unsigned int)(value & 0xfULL);
        digits[count++] = (char)(nibble < 10U ? ('0' + nibble) : ('a' + (nibble - 10U)));
        value >>= 4U;
    }
    while (count > 0U) rt_write_char(1, digits[--count]);
}

static void print_results(unsigned int count, int csv, unsigned long long elapsed_ns, int exit_status) {
    size_t i;
    size_t limit;

    for (i = 0U; i < perf_row_count; ++i) perf_print_rows[i].row_index = i;
    rt_sort(perf_print_rows, perf_row_count, sizeof(perf_print_rows[0]), compare_print_rows);
    limit = count < perf_row_count ? count : perf_row_count;
    if (csv) {
        rt_write_line(1, "rank,samples,pct,address,function");
    } else {
        rt_write_cstr(1, "samples=");
        rt_write_uint(1, perf_total_samples);
        rt_write_cstr(1, " lost=");
        rt_write_uint(1, perf_lost_samples);
        rt_write_cstr(1, " elapsed_ms=");
        rt_write_uint(1, elapsed_ns / 1000000ULL);
        rt_write_cstr(1, " exit=");
        rt_write_uint(1, (unsigned long long)exit_status);
        rt_write_char(1, '\n');
        rt_write_line(1, "rank samples pct address function");
    }
    for (i = 0U; i < limit; ++i) {
        const PerfRow *row = &perf_rows[perf_print_rows[i].row_index];
        const char *symbol = symbol_for_row(row);

        if (csv) {
            rt_write_uint(1, (unsigned long long)(i + 1U));
            rt_write_char(1, ',');
            rt_write_uint(1, row->samples);
            rt_write_char(1, ',');
            tool_write_percent_2(1, row->samples, perf_total_samples);
            rt_write_char(1, ',');
            write_hex_address(row->address);
            rt_write_char(1, ',');
            rt_write_line(1, symbol != 0 ? symbol : "");
        } else {
            rt_write_uint(1, (unsigned long long)(i + 1U));
            rt_write_char(1, ' ');
            rt_write_uint(1, row->samples);
            rt_write_char(1, ' ');
            tool_write_percent_2(1, row->samples, perf_total_samples);
            rt_write_char(1, ' ');
            write_hex_address(row->address);
            rt_write_char(1, ' ');
            rt_write_line(1, symbol != 0 ? symbol : "?");
        }
    }
}

#if defined(__linux__)
typedef struct {
    unsigned int type;
    unsigned int size;
    unsigned long long config;
    unsigned long long sample_period;
    unsigned long long sample_type;
    unsigned long long read_format;
    unsigned long long flags;
    unsigned int wakeup_events;
    unsigned int bp_type;
    unsigned long long config1;
} PerfEventAttr;

typedef struct {
    unsigned char reserved[1024];
    volatile unsigned long long data_head;
    volatile unsigned long long data_tail;
    unsigned long long data_offset;
    unsigned long long data_size;
} PerfMmapPage;

typedef struct {
    PerfMmapPage *meta;
    unsigned char *data;
    unsigned long long data_size;
} PerfRing;

static unsigned short ring_u16(const PerfRing *ring, unsigned long long offset) {
    unsigned long long index = offset & (ring->data_size - 1ULL);
    unsigned short value = 0U;
    unsigned int i;

    for (i = 0U; i < 2U; ++i) value |= (unsigned short)ring->data[(index + i) & (ring->data_size - 1ULL)] << (i * 8U);
    return value;
}

static unsigned int ring_u32(const PerfRing *ring, unsigned long long offset) {
    unsigned long long index = offset & (ring->data_size - 1ULL);
    unsigned int value = 0U;
    unsigned int i;

    for (i = 0U; i < 4U; ++i) value |= (unsigned int)ring->data[(index + i) & (ring->data_size - 1ULL)] << (i * 8U);
    return value;
}

static unsigned long long ring_u64(const PerfRing *ring, unsigned long long offset) {
    unsigned long long index = offset & (ring->data_size - 1ULL);
    unsigned long long value = 0ULL;
    unsigned int i;

    for (i = 0U; i < 8U; ++i) value |= (unsigned long long)ring->data[(index + i) & (ring->data_size - 1ULL)] << (i * 8U);
    return value;
}

static void consume_ring(PerfRing *ring) {
    unsigned long long head = ring->meta->data_head;
    unsigned long long tail = ring->meta->data_tail;

    __sync_synchronize();
    if (head < tail) tail = head;
    if (head - tail > ring->data_size) {
        perf_lost_samples += (head - tail - ring->data_size) / 8ULL;
        tail = head - ring->data_size;
    }
    while (head - tail >= 8ULL) {
        unsigned int type = ring_u32(ring, tail);
        unsigned short size = ring_u16(ring, tail + 6ULL);

        if (size < 8U || (unsigned long long)size > ring->data_size || head - tail < (unsigned long long)size) break;
        if (type == PERF_RECORD_SAMPLE && size >= 16U) {
            unsigned long long ip = ring_u64(ring, tail + 8ULL);
            add_sample(ip);
        } else if (type == PERF_RECORD_LOST && size >= 24U) {
            perf_lost_samples += ring_u64(ring, tail + 16ULL);
        }
        tail += (unsigned long long)size;
    }
    __sync_synchronize();
    ring->meta->data_tail = tail;
}

static long perf_event_open(PerfEventAttr *attr, int pid) {
    return perf_linux_syscall5(LINUX_SYS_PERF_EVENT_OPEN, (long)attr, (long)pid, -1L, -1L, 0L);
}

static int run_perf(char **cmd_argv, const char *map_path, unsigned long long freq, unsigned int count, unsigned int ring_pages, int csv, int include_kernel) {
    PerfEventAttr attr;
    PerfRing ring;
    unsigned long long mmap_size;
    unsigned long long start_ns;
    unsigned long long end_ns;
    long fd;
    long mapped;
    int pid;
    int exit_status = 0;
    int finished = 0;
    int poll_fds[1];
    size_t ready_index;

    if (map_path != 0 && read_symbols(map_path) != 0) return 1;
    if (platform_spawn_process(cmd_argv, -1, -1, 0, 0, 0, &pid) != 0) {
        tool_write_error("perf", "failed to execute ", cmd_argv[0]);
        return 127;
    }
    rt_memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_SOFTWARE;
    attr.size = (unsigned int)sizeof(attr);
    attr.config = PERF_COUNT_SW_CPU_CLOCK;
    attr.sample_period = 1000000000ULL / (freq == 0ULL ? PERF_DEFAULT_FREQ : freq);
    if (attr.sample_period == 0ULL) attr.sample_period = 1ULL;
    attr.sample_type = PERF_SAMPLE_IP;
    attr.flags = PERF_ATTR_DISABLED | PERF_ATTR_EXCLUDE_HV;
    if (!include_kernel) attr.flags |= PERF_ATTR_EXCLUDE_KERNEL;
    attr.wakeup_events = 1U;
    fd = perf_event_open(&attr, pid);
    if (fd < 0) {
        int ignored_status = 0;
        print_open_failure(-fd);
        (void)platform_wait_process_timeout(pid, 0ULL, 1000ULL, LINUX_SIGTERM, 1, &ignored_status);
        return 125;
    }
    if (ring_pages == 0U) ring_pages = PERF_DEFAULT_RING_PAGES;
    mmap_size = ((unsigned long long)ring_pages + 1ULL) * PERF_PAGE_SIZE;
    mapped = perf_linux_syscall6(LINUX_SYS_MMAP, 0L, (long)mmap_size, LINUX_PROT_READ | LINUX_PROT_WRITE, LINUX_MAP_SHARED, fd, 0L);
    if (mapped < 0) {
        long mmap_err = -mapped;
        platform_close((int)fd);
        (void)platform_wait_process_timeout(pid, 0ULL, 1000ULL, LINUX_SIGTERM, 1, &exit_status);
        rt_write_cstr(2, "perf: failed to mmap sample ring errno=");
        rt_write_uint(2, (unsigned long long)mmap_err);
        rt_write_char(2, '\n');
        return 125;
    }
    ring.meta = (PerfMmapPage *)mapped;
    ring.data_size = ring.meta->data_size != 0ULL ? ring.meta->data_size : (unsigned long long)ring_pages * PERF_PAGE_SIZE;
    ring.data = (unsigned char *)mapped + (ring.meta->data_offset != 0ULL ? ring.meta->data_offset : PERF_PAGE_SIZE);
    start_ns = platform_get_monotonic_time_ns();
    (void)perf_linux_syscall3(LINUX_SYS_IOCTL, fd, PERF_EVENT_IOC_RESET, 0L);
    (void)perf_linux_syscall3(LINUX_SYS_IOCTL, fd, PERF_EVENT_IOC_ENABLE, 0L);
    poll_fds[0] = (int)fd;
    while (!finished) {
        if (platform_poll_process_exit(pid, &finished, &exit_status) != 0) break;
        (void)platform_poll_fds(poll_fds, 1U, &ready_index, 50);
        consume_ring(&ring);
    }
    (void)perf_linux_syscall3(LINUX_SYS_IOCTL, fd, PERF_EVENT_IOC_DISABLE, 0L);
    consume_ring(&ring);
    end_ns = platform_get_monotonic_time_ns();
    (void)perf_linux_syscall2(LINUX_SYS_MUNMAP, mapped, (long)mmap_size);
    platform_close((int)fd);
    print_results(count, csv, end_ns >= start_ns ? end_ns - start_ns : 0ULL, exit_status);
    return exit_status;
}
#else
static int run_perf(char **cmd_argv, const char *map_path, unsigned long long freq, unsigned int count, unsigned int ring_pages, int csv, int include_kernel) {
    (void)cmd_argv;
    (void)map_path;
    (void)freq;
    (void)count;
    (void)ring_pages;
    (void)csv;
    (void)include_kernel;
    rt_write_line(2, "perf: Linux perf_event_open support is required");
    return 125;
}
#endif

int main(int argc, char **argv) {
    const char *map_path = 0;
    unsigned long long freq = PERF_DEFAULT_FREQ;
    unsigned long long parsed;
    unsigned int count = PERF_DEFAULT_COUNT;
    unsigned int ring_pages = PERF_DEFAULT_RING_PAGES;
    int csv = 0;
    int include_kernel = 0;
    int argi = 1;

    while (argi < argc) {
        const char *arg = argv[argi];

        if (rt_strcmp(arg, "-h") == 0 || rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 0;
        }
        if (rt_strcmp(arg, "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(arg, "--csv") == 0) {
            csv = 1;
            argi += 1;
        } else if (rt_strcmp(arg, "--kernel") == 0) {
            include_kernel = 1;
            argi += 1;
        } else if (rt_strcmp(arg, "-m") == 0 || rt_strcmp(arg, "--map") == 0 || rt_strcmp(arg, "--symbols") == 0) {
            if (argi + 1 >= argc) {
                tool_write_error("perf", "missing value for ", arg);
                return 1;
            }
            map_path = argv[argi + 1];
            argi += 2;
        } else if (tool_starts_with(arg, "--map=")) {
            map_path = arg + 6;
            argi += 1;
        } else if (tool_starts_with(arg, "--symbols=")) {
            map_path = arg + 10;
            argi += 1;
        } else if (rt_strcmp(arg, "-F") == 0 || rt_strcmp(arg, "--freq") == 0) {
            if (argi + 1 >= argc || parse_unsigned_auto(argv[argi + 1], &freq) != 0 || freq == 0ULL) {
                tool_write_error("perf", "invalid frequency", 0);
                return 1;
            }
            argi += 2;
        } else if (tool_starts_with(arg, "--freq=")) {
            if (parse_unsigned_auto(arg + 7, &freq) != 0 || freq == 0ULL) {
                tool_write_error("perf", "invalid frequency", 0);
                return 1;
            }
            argi += 1;
        } else if (rt_strcmp(arg, "-n") == 0 || rt_strcmp(arg, "--count") == 0) {
            if (argi + 1 >= argc || parse_unsigned_auto(argv[argi + 1], &parsed) != 0 || parsed == 0ULL || parsed > 1000000ULL) {
                tool_write_error("perf", "invalid row count", 0);
                return 1;
            }
            count = (unsigned int)parsed;
            argi += 2;
        } else if (tool_starts_with(arg, "--count=")) {
            if (parse_unsigned_auto(arg + 8, &parsed) != 0 || parsed == 0ULL || parsed > 1000000ULL) {
                tool_write_error("perf", "invalid row count", 0);
                return 1;
            }
            count = (unsigned int)parsed;
            argi += 1;
        } else if (rt_strcmp(arg, "--ring-pages") == 0) {
            if (argi + 1 >= argc || parse_unsigned_auto(argv[argi + 1], &parsed) != 0 || parsed == 0ULL || parsed > 4096ULL) {
                tool_write_error("perf", "invalid ring page count", 0);
                return 1;
            }
            ring_pages = (unsigned int)parsed;
            argi += 2;
        } else if (tool_starts_with(arg, "--ring-pages=")) {
            if (parse_unsigned_auto(arg + 13, &parsed) != 0 || parsed == 0ULL || parsed > 4096ULL) {
                tool_write_error("perf", "invalid ring page count", 0);
                return 1;
            }
            ring_pages = (unsigned int)parsed;
            argi += 1;
        } else if (arg[0] == '-') {
            tool_write_error("perf", "unknown option: ", arg);
            print_usage();
            return 1;
        } else {
            break;
        }
    }
    if (argi >= argc) {
        print_usage();
        return 1;
    }
    return run_perf(&argv[argi], map_path, freq, count, ring_pages, csv, include_kernel);
}
