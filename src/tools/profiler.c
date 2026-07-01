#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PROFILER_MAX_FUNCTIONS 8192U
#define PROFILER_MAX_SYMBOLS 8192U
#define PROFILER_MAX_STACK 4096U
#define PROFILER_MAX_SLIDE_CANDIDATES 128U
#define PROFILER_LINE_CAPACITY 1024U
#define PROFILER_NAME_CAPACITY 256U
#define PROFILER_FUNCTION_HASH_SIZE 32768U
#define PROFILER_TRACE_READ_BUFFER_SIZE 65536U

typedef struct {
    unsigned long long address;
    unsigned long long calls;
    unsigned long long total_ns;
    unsigned long long self_ns;
    unsigned long long max_ns;
} ProfileFunction;

typedef struct {
    unsigned long long address;
    int function_symbol;
    char name[PROFILER_NAME_CAPACITY];
} ProfileSymbol;

typedef struct {
    size_t function_index;
    unsigned long long start_ns;
    unsigned long long child_ns;
} ProfileStackEntry;

typedef struct {
    size_t function_index;
} ProfileRow;

typedef struct {
    unsigned long long events;
    unsigned long long unmatched_exits;
    unsigned long long stack_overflows;
    unsigned long long malformed_lines;
    unsigned long long open_frames;
    unsigned long long event_limit;
    int trace_limited;
} ProfileStats;

typedef struct {
    unsigned long long delta;
    unsigned int count;
} ProfileSlideCandidate;

typedef enum {
    PROFILE_SORT_SELF,
    PROFILE_SORT_TOTAL,
    PROFILE_SORT_CALLS,
    PROFILE_SORT_ADDR
} ProfileSortKey;

static ProfileFunction profiler_functions[PROFILER_MAX_FUNCTIONS];
static ProfileSymbol profiler_symbols[PROFILER_MAX_SYMBOLS];
static ProfileStackEntry profiler_stack[PROFILER_MAX_STACK];
static ProfileRow profiler_rows[PROFILER_MAX_FUNCTIONS];
static ProfileSlideCandidate profiler_slide_candidates[PROFILER_MAX_SLIDE_CANDIDATES];
static int profiler_function_hash[PROFILER_FUNCTION_HASH_SIZE];
static int profiler_function_hash_initialized;
static size_t profiler_function_count;
static size_t profiler_symbol_count;
static ProfileSortKey profiler_sort_key = PROFILE_SORT_SELF;
static unsigned long long profiler_min_self_ns;
static unsigned long long profiler_min_total_ns;

static void print_usage(void) {
    tool_write_usage("profiler", "[-m SYMBOLS] [-n COUNT] [--sort self|total|calls|addr] [--min-self-ms N] [--min-total-ms N] [--max-events N] [--csv] TRACE");
}

static void print_instrumentation_help(void) {
    rt_write_cstr(1, "GCC primary instrumentation flags:\n");
    rt_write_cstr(1, "  gcc -finstrument-functions -fno-omit-frame-pointer -fno-inline -g -O2 ...\n");
    rt_write_cstr(1, "\nClang secondary instrumentation flags:\n");
    rt_write_cstr(1, "  clang -finstrument-functions -fno-omit-frame-pointer -fno-inline -g -O2 ...\n");
    rt_write_cstr(1, "\nProject build shortcut:\n");
    rt_write_cstr(1, "  make freestanding PROFILE=1 LINKER_REPORTS=1\n");
    rt_write_cstr(1, "  NEWOS_PROFILE=tool.nprof build/freestanding-linux-x86_64/tool ...\n");
    rt_write_cstr(1, "  NEWOS_PROFILE_MAX_EVENTS=1000000 caps trace generation before running out of disk.\n");
    rt_write_cstr(1, "  build/freestanding-linux-x86_64/profiler -m build/freestanding-linux-x86_64/.maps/tool.map tool.nprof\n");
    rt_write_cstr(1, "  MACOS_NEWLINKER_MAP_DIR=build/profile-maps make freestanding PROFILE=1\n");
    rt_write_cstr(1, "  NEWOS_PROFILE=tool.nprof build/newlinker-macos-aarch64/tool ...\n");
    rt_write_cstr(1, "\nTrace lines accepted by profiler:\n");
    rt_write_cstr(1, "  enter TIME_NS ADDRESS\n");
    rt_write_cstr(1, "  exit  TIME_NS ADDRESS\n");
    rt_write_cstr(1, "Aliases: e/x and +/- are accepted for enter/exit.\n");
    rt_write_cstr(1, "\nSymbol files may use either format:\n");
    rt_write_cstr(1, "  0x401000 function_name\n");
    rt_write_cstr(1, "  0000000000401000 T function_name\n");
    rt_write_cstr(1, "  symbol 0x0000000100001000 64 __TEXT,__text _function_name path.o\n");
}

static int event_kind_from_token(const char *token, int *is_enter_out) {
    if (rt_strcmp(token, "enter") == 0 || rt_strcmp(token, "e") == 0 || rt_strcmp(token, "+") == 0) {
        *is_enter_out = 1;
        return 0;
    }
    if (rt_strcmp(token, "exit") == 0 || rt_strcmp(token, "x") == 0 || rt_strcmp(token, "-") == 0) {
        *is_enter_out = 0;
        return 0;
    }
    return -1;
}

static void profiler_function_hash_init(void) {
    size_t i;

    if (profiler_function_hash_initialized) {
        return;
    }
    for (i = 0U; i < PROFILER_FUNCTION_HASH_SIZE; ++i) {
        profiler_function_hash[i] = -1;
    }
    profiler_function_hash_initialized = 1;
}

static size_t profiler_function_hash_slot(unsigned long long address) {
    unsigned long long mixed = address;

    mixed ^= mixed >> 33U;
    mixed *= 0xff51afd7ed558ccdULL;
    mixed ^= mixed >> 33U;
    return (size_t)(mixed & (PROFILER_FUNCTION_HASH_SIZE - 1U));
}

static int find_function(unsigned long long address) {
    size_t slot;

    profiler_function_hash_init();
    slot = profiler_function_hash_slot(address);

    while (profiler_function_hash[slot] >= 0) {
        int index = profiler_function_hash[slot];

        if (profiler_functions[(size_t)index].address == address) {
            return index;
        }
        slot = (slot + 1U) & (PROFILER_FUNCTION_HASH_SIZE - 1U);
    }
    return -1;
}

static void add_function_hash_entry(unsigned long long address, size_t function_index) {
    size_t slot;

    profiler_function_hash_init();
    slot = profiler_function_hash_slot(address);
    while (profiler_function_hash[slot] >= 0) {
        slot = (slot + 1U) & (PROFILER_FUNCTION_HASH_SIZE - 1U);
    }
    profiler_function_hash[slot] = (int)function_index;
}

static int find_or_add_function(unsigned long long address, size_t *index_out) {
    int existing = find_function(address);

    if (existing >= 0) {
        *index_out = (size_t)existing;
        return 0;
    }
    if (profiler_function_count >= PROFILER_MAX_FUNCTIONS) {
        return -1;
    }
    profiler_functions[profiler_function_count].address = address;
    profiler_functions[profiler_function_count].calls = 0ULL;
    profiler_functions[profiler_function_count].total_ns = 0ULL;
    profiler_functions[profiler_function_count].self_ns = 0ULL;
    profiler_functions[profiler_function_count].max_ns = 0ULL;
    add_function_hash_entry(address, profiler_function_count);
    *index_out = profiler_function_count++;
    return 0;
}

static void add_symbol(unsigned long long address, const char *name, int function_symbol) {
    size_t i;

    if (name == 0 || name[0] == '\0') {
        return;
    }
    for (i = 0U; i < profiler_symbol_count; ++i) {
        if (profiler_symbols[i].address == address) {
            if (function_symbol || !profiler_symbols[i].function_symbol) {
                profiler_symbols[i].function_symbol = function_symbol;
                rt_copy_string(profiler_symbols[i].name, sizeof(profiler_symbols[i].name), name);
            }
            return;
        }
    }
    if (profiler_symbol_count >= PROFILER_MAX_SYMBOLS) {
        return;
    }
    profiler_symbols[profiler_symbol_count].address = address;
    profiler_symbols[profiler_symbol_count].function_symbol = function_symbol;
    rt_copy_string(profiler_symbols[profiler_symbol_count].name, sizeof(profiler_symbols[profiler_symbol_count].name), name);
    profiler_symbol_count += 1U;
}

static int parse_linker_map_symbol_line(const char *line) {
    const char *cursor = line;
    char keyword[32];
    char address_token[64];
    char size_token[64];
    char section[64];
    char name[PROFILER_NAME_CAPACITY];
    unsigned long long address;

    if (!tool_next_token(&cursor, keyword, sizeof(keyword)) || rt_strcmp(keyword, "symbol") != 0) {
        return 0;
    }
    if (!tool_next_token(&cursor, address_token, sizeof(address_token)) ||
        !tool_next_token(&cursor, size_token, sizeof(size_token)) ||
        !tool_next_token(&cursor, section, sizeof(section)) ||
        !tool_next_token(&cursor, name, sizeof(name))) {
        return 0;
    }
    if (rt_strcmp(section, "__TEXT,__text") != 0 || tool_parse_address_token(address_token, &address) != 0) {
        return 0;
    }
    add_symbol(address, tool_display_symbol_name(name), 1);
    return 1;
}

static int parse_linux_newlinker_map_line(const char *line) {
    const char *cursor = line;
    char address_token[64];
    char size_token[64];
    char section[PROFILER_NAME_CAPACITY];
    unsigned long long address;
    unsigned long long size;
    const char *name;

    if (!tool_next_token(&cursor, address_token, sizeof(address_token)) ||
        tool_parse_address_token(address_token, &address) != 0 ||
        !tool_next_token(&cursor, size_token, sizeof(size_token)) ||
        tool_parse_unsigned_auto(size_token, &size) != 0 ||
        !tool_next_token(&cursor, section, sizeof(section)) ||
        section[0] != '.') {
        return 0;
    }
    (void)size;
    name = tool_newlinker_text_symbol_name(section);
    if (name != 0) {
        add_symbol(address, name, 1);
    }
    return 1;
}

static const char *symbol_for_address(unsigned long long address) {
    size_t i;
    size_t best_index = PROFILER_MAX_SYMBOLS;
    unsigned long long best_address = 0ULL;

    for (i = 0U; i < profiler_symbol_count; ++i) {
        if (profiler_symbols[i].address == address) {
            return profiler_symbols[i].name;
        }
        if (profiler_symbols[i].address < address && (best_index == PROFILER_MAX_SYMBOLS || profiler_symbols[i].address > best_address)) {
            best_index = i;
            best_address = profiler_symbols[i].address;
        }
    }
    if (best_index != PROFILER_MAX_SYMBOLS) {
        return profiler_symbols[best_index].name;
    }
    return 0;
}

static void write_padded_3(unsigned long long value) {
    rt_write_char(1, (char)('0' + ((value / 100ULL) % 10ULL)));
    rt_write_char(1, (char)('0' + ((value / 10ULL) % 10ULL)));
    rt_write_char(1, (char)('0' + (value % 10ULL)));
}

static void write_ns_as_ms(unsigned long long ns) {
    rt_write_uint(1, ns / 1000000ULL);
    rt_write_char(1, '.');
    write_padded_3((ns % 1000000ULL) / 1000ULL);
}

static int read_symbols(const char *path) {
    int fd;
    int should_close;
    ToolRecordReader reader;
    char line[PROFILER_LINE_CAPACITY];
    int has_record;

    if (tool_open_input(path, &fd, &should_close) != 0) {
        tool_write_error("profiler", "failed to open symbols: ", path);
        return -1;
    }
    tool_record_reader_init(&reader, fd, '\n');
    while (tool_record_reader_next(&reader, line, sizeof(line), &has_record) == 0 && has_record) {
        const char *cursor = line;
        char first[128];
        char second[PROFILER_NAME_CAPACITY];
        char third[PROFILER_NAME_CAPACITY];
        unsigned long long address;

        if (parse_linker_map_symbol_line(line) || parse_linux_newlinker_map_line(line)) {
            continue;
        }
        if (!tool_next_token(&cursor, first, sizeof(first))) {
            continue;
        }
        if (tool_parse_address_token(first, &address) != 0) {
            continue;
        }
        if (!tool_next_token(&cursor, second, sizeof(second))) {
            continue;
        }
        if (rt_strlen(second) == 1U && tool_next_token(&cursor, third, sizeof(third))) {
            if (tool_symbol_type_is_function(second)) {
                add_symbol(address, third, 1);
            }
        } else {
            add_symbol(address, second, 1);
        }
    }
    tool_close_input(fd, should_close);
    return 0;
}

static int compare_rows_by_address(const void *left_ptr, const void *right_ptr) {
    const ProfileRow *left = (const ProfileRow *)left_ptr;
    const ProfileRow *right = (const ProfileRow *)right_ptr;
    const ProfileFunction *lf = &profiler_functions[left->function_index];
    const ProfileFunction *rf = &profiler_functions[right->function_index];

    if (lf->address < rf->address) return -1;
    if (lf->address > rf->address) return 1;
    return 0;
}

static int function_address_exists_sorted(unsigned long long address) {
    size_t low = 0U;
    size_t high = profiler_function_count;

    while (low < high) {
        size_t mid = low + ((high - low) / 2U);
        unsigned long long mid_address = profiler_functions[profiler_rows[mid].function_index].address;

        if (mid_address == address) return 1;
        if (mid_address < address) {
            low = mid + 1U;
        } else {
            high = mid;
        }
    }
    return 0;
}

static void remember_slide_candidate(unsigned long long delta) {
    size_t i;

    for (i = 0U; i < PROFILER_MAX_SLIDE_CANDIDATES; ++i) {
        if (profiler_slide_candidates[i].count != 0U && profiler_slide_candidates[i].delta == delta) {
            profiler_slide_candidates[i].count += 1U;
            return;
        }
    }
    for (i = 0U; i < PROFILER_MAX_SLIDE_CANDIDATES; ++i) {
        if (profiler_slide_candidates[i].count == 0U) {
            profiler_slide_candidates[i].delta = delta;
            profiler_slide_candidates[i].count = 1U;
            return;
        }
    }
    for (i = 0U; i < PROFILER_MAX_SLIDE_CANDIDATES; ++i) {
        profiler_slide_candidates[i].count -= 1U;
    }
}

static unsigned int count_symbol_matches_for_slide(unsigned long long delta) {
    size_t i;
    unsigned int matches = 0U;

    for (i = 0U; i < profiler_symbol_count; ++i) {
        if (function_address_exists_sorted(profiler_symbols[i].address + delta)) {
            matches += 1U;
        }
    }
    return matches;
}

static void infer_symbol_slide(void) {
    size_t i;
    size_t j;
    unsigned int exact_matches;
    unsigned int best_matches;
    unsigned long long best_delta = 0ULL;

    if (profiler_symbol_count == 0U || profiler_function_count == 0U) return;
    for (i = 0U; i < profiler_function_count; ++i) {
        profiler_rows[i].function_index = i;
    }
    rt_sort(profiler_rows, profiler_function_count, sizeof(profiler_rows[0]), compare_rows_by_address);
    exact_matches = count_symbol_matches_for_slide(0ULL);
    rt_memset(profiler_slide_candidates, 0, sizeof(profiler_slide_candidates));
    for (i = 0U; i < profiler_function_count; ++i) {
        unsigned long long function_address = profiler_functions[i].address;

        for (j = 0U; j < profiler_symbol_count; ++j) {
            unsigned long long symbol_address = profiler_symbols[j].address;
            unsigned long long delta;

            if (function_address < symbol_address) continue;
            delta = function_address - symbol_address;
            if ((delta & 0xfffULL) != 0ULL) continue;
            remember_slide_candidate(delta);
        }
    }
    best_matches = exact_matches;
    for (i = 0U; i < PROFILER_MAX_SLIDE_CANDIDATES; ++i) {
        unsigned int matches;

        if (profiler_slide_candidates[i].count == 0U) continue;
        matches = count_symbol_matches_for_slide(profiler_slide_candidates[i].delta);
        if (matches > best_matches) {
            best_matches = matches;
            best_delta = profiler_slide_candidates[i].delta;
        }
    }
    if (best_delta == 0ULL || best_matches < 2U || best_matches <= exact_matches) return;
    for (i = 0U; i < profiler_symbol_count; ++i) {
        profiler_symbols[i].address += best_delta;
    }
}

static int process_enter(unsigned long long timestamp_ns, unsigned long long address, size_t *stack_depth_io, ProfileStats *stats) {
    size_t function_index;
    size_t depth = *stack_depth_io;

    if (find_or_add_function(address, &function_index) != 0) {
        stats->malformed_lines += 1ULL;
        return 0;
    }
    profiler_functions[function_index].calls += 1ULL;
    if (depth >= PROFILER_MAX_STACK) {
        stats->stack_overflows += 1ULL;
        return 0;
    }
    profiler_stack[depth].function_index = function_index;
    profiler_stack[depth].start_ns = timestamp_ns;
    profiler_stack[depth].child_ns = 0ULL;
    *stack_depth_io = depth + 1U;
    return 0;
}

static int process_exit(unsigned long long timestamp_ns, unsigned long long address, size_t *stack_depth_io, ProfileStats *stats) {
    size_t depth = *stack_depth_io;
    size_t function_index;
    ProfileStackEntry frame;
    unsigned long long elapsed;
    unsigned long long self;

    if (depth == 0U) {
        stats->unmatched_exits += 1ULL;
        return 0;
    }
    function_index = profiler_stack[depth - 1U].function_index;
    if (profiler_functions[function_index].address != address) {
        stats->unmatched_exits += 1ULL;
        return 0;
    }
    frame = profiler_stack[depth - 1U];
    *stack_depth_io = depth - 1U;
    elapsed = timestamp_ns >= frame.start_ns ? timestamp_ns - frame.start_ns : 0ULL;
    self = elapsed >= frame.child_ns ? elapsed - frame.child_ns : 0ULL;
    profiler_functions[function_index].total_ns += elapsed;
    profiler_functions[function_index].self_ns += self;
    if (elapsed > profiler_functions[function_index].max_ns) {
        profiler_functions[function_index].max_ns = elapsed;
    }
    if (*stack_depth_io > 0U) {
        profiler_stack[*stack_depth_io - 1U].child_ns += elapsed;
    }
    return 0;
}

static int parse_decimal_at(const char **cursor_io, unsigned long long *value_out) {
    const char *cursor = *cursor_io;
    unsigned long long value = 0ULL;
    int saw_digit = 0;

    while (*cursor >= '0' && *cursor <= '9') {
        value = value * 10ULL + (unsigned long long)(*cursor - '0');
        saw_digit = 1;
        cursor += 1;
    }
    if (!saw_digit) {
        return -1;
    }
    *cursor_io = cursor;
    *value_out = value;
    return 0;
}

static int parse_hex_address_at(const char **cursor_io, unsigned long long *value_out) {
    const char *cursor = *cursor_io;
    unsigned long long value = 0ULL;
    int saw_digit = 0;

    if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
        cursor += 2;
    }
    while (*cursor != '\0' && !tool_ascii_is_token_space(*cursor)) {
        int digit = tool_hex_value(*cursor);

        if (digit < 0) {
            return -1;
        }
        value = (value << 4U) | (unsigned long long)digit;
        saw_digit = 1;
        cursor += 1;
    }
    if (!saw_digit) {
        return -1;
    }
    *cursor_io = cursor;
    *value_out = value;
    return 0;
}

static int parse_decimal_span(const char **cursor_io, const char *end, unsigned long long *value_out) {
    const char *cursor = *cursor_io;
    unsigned long long value = 0ULL;
    int saw_digit = 0;

    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        value = value * 10ULL + (unsigned long long)(*cursor - '0');
        saw_digit = 1;
        cursor += 1;
    }
    if (!saw_digit) {
        return -1;
    }
    *cursor_io = cursor;
    *value_out = value;
    return 0;
}

static int parse_hex_address_span(const char **cursor_io, const char *end, unsigned long long *value_out) {
    const char *cursor = *cursor_io;
    unsigned long long value = 0ULL;
    int saw_digit = 0;

    if ((size_t)(end - cursor) >= 2U && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
        cursor += 2;
    }
    while (cursor < end && !tool_ascii_is_token_space(*cursor)) {
        int digit = tool_hex_value(*cursor);

        if (digit < 0) {
            return -1;
        }
        value = (value << 4U) | (unsigned long long)digit;
        saw_digit = 1;
        cursor += 1;
    }
    if (!saw_digit) {
        return -1;
    }
    *cursor_io = cursor;
    *value_out = value;
    return 0;
}

static int process_standard_trace_span(const char *line,
                                       size_t length,
                                       size_t *stack_depth_io,
                                       ProfileStats *stats,
                                       int *handled_out) {
    const char *cursor;
    const char *end = line + length;
    int is_enter;
    unsigned long long timestamp_ns;
    unsigned long long address;

    *handled_out = 0;
    if (length >= 6U && line[0] == 'e' && line[1] == 'n' && line[2] == 't' && line[3] == 'e' && line[4] == 'r' && tool_ascii_is_token_space(line[5])) {
        is_enter = 1;
        cursor = line + 6;
    } else if (length >= 5U && line[0] == 'e' && line[1] == 'x' && line[2] == 'i' && line[3] == 't' && tool_ascii_is_token_space(line[4])) {
        is_enter = 0;
        cursor = line + 5;
    } else {
        return 0;
    }

    while (cursor < end && tool_ascii_is_token_space(*cursor)) {
        cursor += 1;
    }
    if (parse_decimal_span(&cursor, end, &timestamp_ns) != 0 || cursor >= end || !tool_ascii_is_token_space(*cursor)) {
        return 0;
    }
    while (cursor < end && tool_ascii_is_token_space(*cursor)) {
        cursor += 1;
    }
    if (parse_hex_address_span(&cursor, end, &address) != 0) {
        return 0;
    }

    *handled_out = 1;
    stats->events += 1ULL;
    if (is_enter) {
        return process_enter(timestamp_ns, address, stack_depth_io, stats);
    }
    return process_exit(timestamp_ns, address, stack_depth_io, stats);
}

static int process_standard_trace_line(const char *line, size_t *stack_depth_io, ProfileStats *stats, int *handled_out) {
    const char *cursor;
    int is_enter;
    unsigned long long timestamp_ns;
    unsigned long long address;

    *handled_out = 0;
    if (line[0] == 'e' && line[1] == 'n' && line[2] == 't' && line[3] == 'e' && line[4] == 'r' && tool_ascii_is_token_space(line[5])) {
        is_enter = 1;
        cursor = line + 6;
    } else if (line[0] == 'e' && line[1] == 'x' && line[2] == 'i' && line[3] == 't' && tool_ascii_is_token_space(line[4])) {
        is_enter = 0;
        cursor = line + 5;
    } else {
        return 0;
    }

    while (tool_ascii_is_token_space(*cursor)) {
        cursor += 1;
    }
    if (parse_decimal_at(&cursor, &timestamp_ns) != 0 || !tool_ascii_is_token_space(*cursor)) {
        return 0;
    }
    while (tool_ascii_is_token_space(*cursor)) {
        cursor += 1;
    }
    if (parse_hex_address_at(&cursor, &address) != 0) {
        return 0;
    }

    *handled_out = 1;
    stats->events += 1ULL;
    if (is_enter) {
        return process_enter(timestamp_ns, address, stack_depth_io, stats);
    }
    return process_exit(timestamp_ns, address, stack_depth_io, stats);
}

static int process_trace_line(const char *line, size_t *stack_depth_io, ProfileStats *stats);

static int process_trace_record(const char *record, size_t length, size_t *stack_depth_io, ProfileStats *stats) {
    int handled;

    if (process_standard_trace_span(record, length, stack_depth_io, stats, &handled) != 0) {
        return -1;
    }
    if (handled) {
        return 0;
    }
    if (length + 1U < PROFILER_LINE_CAPACITY) {
        char line[PROFILER_LINE_CAPACITY];

        if (length > 0U) {
            memcpy(line, record, length);
        }
        line[length] = '\0';
        return process_trace_line(line, stack_depth_io, stats);
    }
    stats->malformed_lines += 1ULL;
    return 0;
}

static int profile_event_limit_reached(ProfileStats *stats) {
    if (stats->event_limit != 0ULL && stats->events >= stats->event_limit) {
        stats->trace_limited = 1;
        return 1;
    }
    return 0;
}

static int process_trace_line(const char *line, size_t *stack_depth_io, ProfileStats *stats) {
    const char *cursor = line;
    char kind[32];
    char time_token[64];
    char address_token[64];
    int is_enter;
    unsigned long long timestamp_ns;
    unsigned long long address;
    int handled;

    if (process_standard_trace_line(line, stack_depth_io, stats, &handled) != 0) {
        return -1;
    }
    if (handled) {
        return 0;
    }

    if (!tool_next_token(&cursor, kind, sizeof(kind))) {
        return 0;
    }
    if (event_kind_from_token(kind, &is_enter) != 0 ||
        !tool_next_token(&cursor, time_token, sizeof(time_token)) ||
        !tool_next_token(&cursor, address_token, sizeof(address_token)) ||
        tool_parse_unsigned_auto(time_token, &timestamp_ns) != 0 ||
        tool_parse_address_token(address_token, &address) != 0) {
        stats->malformed_lines += 1ULL;
        return 0;
    }
    stats->events += 1ULL;
    if (is_enter) {
        return process_enter(timestamp_ns, address, stack_depth_io, stats);
    }
    return process_exit(timestamp_ns, address, stack_depth_io, stats);
}

static int read_trace(const char *path, ProfileStats *stats) {
    int fd;
    int should_close;
    char buffer[PROFILER_TRACE_READ_BUFFER_SIZE];
    char partial[PROFILER_LINE_CAPACITY];
    size_t partial_len = 0U;
    size_t stack_depth = 0U;

    if (tool_open_input(path, &fd, &should_close) != 0) {
        tool_write_error("profiler", "failed to open trace: ", path);
        return -1;
    }
    while (1) {
        long bytes_read = platform_read(fd, buffer, sizeof(buffer));
        size_t start = 0U;
        size_t index;

        if (bytes_read < 0) {
            tool_close_input(fd, should_close);
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }
        for (index = 0U; index < (size_t)bytes_read; ++index) {
            if (buffer[index] == '\n') {
                size_t span_len = index - start;

                if (span_len > 0U && buffer[index - 1U] == '\r') {
                    span_len -= 1U;
                }
                if (partial_len == 0U) {
                    if (process_trace_record(buffer + start, span_len, &stack_depth, stats) != 0) {
                        tool_close_input(fd, should_close);
                        return -1;
                    }
                    if (profile_event_limit_reached(stats)) goto done;
                } else {
                    if (partial_len + span_len >= sizeof(partial)) {
                        stats->malformed_lines += 1ULL;
                        partial_len = 0U;
                    } else {
                        if (span_len > 0U) {
                            memcpy(partial + partial_len, buffer + start, span_len);
                        }
                        partial_len += span_len;
                        if (partial_len > 0U && partial[partial_len - 1U] == '\r') {
                            partial_len -= 1U;
                        }
                        if (process_trace_record(partial, partial_len, &stack_depth, stats) != 0) {
                            tool_close_input(fd, should_close);
                            return -1;
                        }
                        if (profile_event_limit_reached(stats)) goto done;
                        partial_len = 0U;
                    }
                }
                start = index + 1U;
            }
        }
        if (start < (size_t)bytes_read) {
            size_t remaining = (size_t)bytes_read - start;

            if (partial_len + remaining >= sizeof(partial)) {
                stats->malformed_lines += 1ULL;
                partial_len = 0U;
            } else {
                memcpy(partial + partial_len, buffer + start, remaining);
                partial_len += remaining;
            }
        }
    }
done:
    if (partial_len > 0U) {
        if (partial[partial_len - 1U] == '\r') {
            partial_len -= 1U;
        }
        if (!stats->trace_limited && process_trace_record(partial, partial_len, &stack_depth, stats) != 0) {
            tool_close_input(fd, should_close);
            return -1;
        }
    }
    stats->open_frames = (unsigned long long)stack_depth;
    tool_close_input(fd, should_close);
    return 0;
}

static int compare_rows(const void *left_ptr, const void *right_ptr) {
    const ProfileRow *left = (const ProfileRow *)left_ptr;
    const ProfileRow *right = (const ProfileRow *)right_ptr;
    const ProfileFunction *lf = &profiler_functions[left->function_index];
    const ProfileFunction *rf = &profiler_functions[right->function_index];
    unsigned long long lv;
    unsigned long long rv;

    if (profiler_sort_key == PROFILE_SORT_TOTAL) {
        lv = lf->total_ns;
        rv = rf->total_ns;
    } else if (profiler_sort_key == PROFILE_SORT_CALLS) {
        lv = lf->calls;
        rv = rf->calls;
    } else if (profiler_sort_key == PROFILE_SORT_ADDR) {
        return lf->address < rf->address ? -1 : (lf->address > rf->address ? 1 : 0);
    } else {
        lv = lf->self_ns;
        rv = rf->self_ns;
    }
    if (lv != rv) {
        return lv > rv ? -1 : 1;
    }
    if (lf->address < rf->address) return -1;
    if (lf->address > rf->address) return 1;
    return 0;
}

static void write_report_line(size_t rank, const ProfileFunction *function, unsigned long long program_total_ns, int csv) {
    const char *symbol = symbol_for_address(function->address);
    unsigned long long avg_self = function->calls == 0ULL ? 0ULL : function->self_ns / function->calls;
    unsigned long long avg_total = function->calls == 0ULL ? 0ULL : function->total_ns / function->calls;

    if (csv) {
        rt_write_uint(1, (unsigned long long)rank);
        rt_write_char(1, ',');
        rt_write_uint(1, function->calls);
        rt_write_char(1, ',');
        rt_write_uint(1, function->self_ns);
        rt_write_char(1, ',');
        rt_write_uint(1, function->total_ns);
        rt_write_char(1, ',');
        rt_write_uint(1, function->max_ns);
        rt_write_char(1, ',');
        tool_write_percent_2(1, function->self_ns, program_total_ns);
        rt_write_char(1, ',');
        tool_write_percent_2(1, function->total_ns, program_total_ns);
        rt_write_char(1, ',');
        rt_write_uint(1, avg_self);
        rt_write_char(1, ',');
        rt_write_uint(1, avg_total);
        rt_write_char(1, ',');
        tool_write_hex_value(1, function->address);
        rt_write_char(1, ',');
        rt_write_line(1, symbol != 0 ? symbol : "");
        return;
    }
    rt_write_uint(1, (unsigned long long)rank);
    rt_write_char(1, '\t');
    rt_write_uint(1, function->calls);
    rt_write_char(1, '\t');
    write_ns_as_ms(function->self_ns);
    rt_write_char(1, '\t');
    write_ns_as_ms(function->total_ns);
    rt_write_char(1, '\t');
    write_ns_as_ms(function->max_ns);
    rt_write_char(1, '\t');
    tool_write_percent_2(1, function->self_ns, program_total_ns);
    rt_write_char(1, '\t');
    tool_write_percent_2(1, function->total_ns, program_total_ns);
    rt_write_char(1, '\t');
    write_ns_as_ms(avg_self);
    rt_write_char(1, '\t');
    write_ns_as_ms(avg_total);
    rt_write_char(1, '\t');
    tool_write_hex_value(1, function->address);
    rt_write_char(1, '\t');
    rt_write_line(1, symbol != 0 ? symbol : "?");
}

static void write_report(unsigned long long limit, int csv, const ProfileStats *stats) {
    size_t i;
    size_t count = 0U;
    size_t matched_count;
    size_t hidden_by_filter;
    unsigned long long program_total_ns = 0ULL;

    for (i = 0U; i < profiler_function_count; ++i) {
        if (profiler_min_self_ns != 0ULL && profiler_functions[i].self_ns < profiler_min_self_ns) continue;
        if (profiler_min_total_ns != 0ULL && profiler_functions[i].total_ns < profiler_min_total_ns) continue;
        profiler_rows[count++].function_index = i;
        if (profiler_functions[i].total_ns > program_total_ns) {
            program_total_ns = profiler_functions[i].total_ns;
        }
    }
    matched_count = count;
    hidden_by_filter = profiler_function_count - matched_count;
    rt_sort(profiler_rows, count, sizeof(profiler_rows[0]), compare_rows);
    if (limit != 0ULL && limit < (unsigned long long)count) {
        count = (size_t)limit;
    }
    if (csv) {
        rt_write_cstr(1, "rank,calls,self_ns,total_ns,max_ns,self_pct,total_pct,avg_self_ns,avg_total_ns,address,function\n");
    } else {
        rt_write_cstr(1, "rank\tcalls\tself_ms\ttotal_ms\tmax_ms\tself%\ttotal%\tavg_self_ms\tavg_total_ms\taddress\tfunction\n");
    }
    for (i = 0U; i < count; ++i) {
        write_report_line(i + 1U, &profiler_functions[profiler_rows[i].function_index], program_total_ns, csv);
    }
    if (!csv) {
        rt_write_cstr(2, "profiler: events=");
        rt_write_uint(2, stats->events);
        rt_write_cstr(2, " functions=");
        rt_write_uint(2, (unsigned long long)profiler_function_count);
        rt_write_cstr(2, " matched=");
        rt_write_uint(2, (unsigned long long)matched_count);
        if (hidden_by_filter != 0U) {
            rt_write_cstr(2, " hidden_by_filter=");
            rt_write_uint(2, (unsigned long long)hidden_by_filter);
        }
        if ((unsigned long long)count != matched_count) {
            rt_write_cstr(2, " reported=");
            rt_write_uint(2, (unsigned long long)count);
        }
        if (stats->malformed_lines != 0ULL || stats->unmatched_exits != 0ULL || stats->stack_overflows != 0ULL || stats->open_frames != 0ULL) {
            rt_write_cstr(2, " malformed=");
            rt_write_uint(2, stats->malformed_lines);
            rt_write_cstr(2, " unmatched_exits=");
            rt_write_uint(2, stats->unmatched_exits);
            rt_write_cstr(2, " stack_overflows=");
            rt_write_uint(2, stats->stack_overflows);
            rt_write_cstr(2, " open_frames=");
            rt_write_uint(2, stats->open_frames);
        }
        if (stats->trace_limited) {
            rt_write_cstr(2, " trace_limited_at=");
            rt_write_uint(2, stats->event_limit);
        }
        rt_write_char(2, '\n');
        if (stats->unmatched_exits != 0ULL || stats->open_frames != 0ULL) {
            rt_write_cstr(2, "profiler: warning: unmatched/open frames can indicate truncated traces or interleaved multi-threaded call stacks; exact threaded attribution needs per-thread trace ids\n");
        }
    }
}

static int parse_milliseconds_to_ns(const char *text, unsigned long long *ns_out, const char *what) {
    unsigned long long milliseconds;

    if (tool_parse_uint_arg(text, &milliseconds, "profiler", what) != 0) return -1;
    if (milliseconds > 18446744073709ULL) milliseconds = 18446744073709ULL;
    *ns_out = milliseconds * 1000000ULL;
    return 0;
}

static int parse_sort_key(const char *text, ProfileSortKey *sort_key_out) {
    if (rt_strcmp(text, "self") == 0) {
        *sort_key_out = PROFILE_SORT_SELF;
    } else if (rt_strcmp(text, "total") == 0) {
        *sort_key_out = PROFILE_SORT_TOTAL;
    } else if (rt_strcmp(text, "calls") == 0) {
        *sort_key_out = PROFILE_SORT_CALLS;
    } else if (rt_strcmp(text, "addr") == 0 || rt_strcmp(text, "address") == 0) {
        *sort_key_out = PROFILE_SORT_ADDR;
    } else {
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *trace_path = 0;
    const char *symbols_path = 0;
    unsigned long long limit = 30ULL;
    int csv = 0;
    int argi;
    ProfileStats stats;

    rt_memset(&stats, 0, sizeof(stats));
    for (argi = 1; argi < argc; ++argi) {
        const char *arg = argv[argi];

        if (rt_strcmp(arg, "--help") == 0 || rt_strcmp(arg, "-h") == 0) {
            print_usage();
            return 0;
        } else if (rt_strcmp(arg, "--help-instrumentation") == 0) {
            print_instrumentation_help();
            return 0;
        } else if (rt_strcmp(arg, "--csv") == 0) {
            csv = 1;
        } else if (rt_strcmp(arg, "-m") == 0 || rt_strcmp(arg, "--map") == 0 || rt_strcmp(arg, "--symbols") == 0) {
            if (argi + 1 >= argc) {
                tool_write_error("profiler", "missing option value: ", arg);
                return 1;
            }
            symbols_path = argv[++argi];
        } else if (tool_starts_with(arg, "--map=")) {
            symbols_path = arg + 6;
        } else if (tool_starts_with(arg, "--symbols=")) {
            symbols_path = arg + 10;
        } else if (rt_strcmp(arg, "-n") == 0 || rt_strcmp(arg, "--limit") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &limit, "profiler", "limit") != 0) {
                return 1;
            }
            argi += 1;
        } else if (tool_starts_with(arg, "--limit=")) {
            if (tool_parse_uint_arg(arg + 8, &limit, "profiler", "limit") != 0) {
                return 1;
            }
        } else if (rt_strcmp(arg, "--max-events") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &stats.event_limit, "profiler", "max events") != 0) {
                return 1;
            }
            argi += 1;
        } else if (tool_starts_with(arg, "--max-events=")) {
            if (tool_parse_uint_arg(arg + 13, &stats.event_limit, "profiler", "max events") != 0) {
                return 1;
            }
        } else if (rt_strcmp(arg, "--min-self-ms") == 0) {
            if (argi + 1 >= argc || parse_milliseconds_to_ns(argv[argi + 1], &profiler_min_self_ns, "minimum self milliseconds") != 0) {
                return 1;
            }
            argi += 1;
        } else if (tool_starts_with(arg, "--min-self-ms=")) {
            if (parse_milliseconds_to_ns(arg + 14, &profiler_min_self_ns, "minimum self milliseconds") != 0) {
                return 1;
            }
        } else if (rt_strcmp(arg, "--min-total-ms") == 0) {
            if (argi + 1 >= argc || parse_milliseconds_to_ns(argv[argi + 1], &profiler_min_total_ns, "minimum total milliseconds") != 0) {
                return 1;
            }
            argi += 1;
        } else if (tool_starts_with(arg, "--min-total-ms=")) {
            if (parse_milliseconds_to_ns(arg + 15, &profiler_min_total_ns, "minimum total milliseconds") != 0) {
                return 1;
            }
        } else if (rt_strcmp(arg, "--sort") == 0) {
            if (argi + 1 >= argc || parse_sort_key(argv[argi + 1], &profiler_sort_key) != 0) {
                tool_write_error("profiler", "invalid sort key", 0);
                return 1;
            }
            argi += 1;
        } else if (tool_starts_with(arg, "--sort=")) {
            if (parse_sort_key(arg + 7, &profiler_sort_key) != 0) {
                tool_write_error("profiler", "invalid sort key: ", arg + 7);
                return 1;
            }
        } else if (arg[0] == '-' && arg[1] != '\0') {
            tool_write_error("profiler", "unknown option: ", arg);
            return 1;
        } else if (trace_path == 0) {
            trace_path = arg;
        } else {
            tool_write_error("profiler", "unexpected argument: ", arg);
            return 1;
        }
    }
    if (trace_path == 0) {
        print_usage();
        return 1;
    }
    if (symbols_path != 0 && read_symbols(symbols_path) != 0) {
        return 1;
    }
    if (read_trace(trace_path, &stats) != 0) {
        return 1;
    }
    infer_symbol_slide();
    write_report(limit, csv, &stats);
    return 0;
}
