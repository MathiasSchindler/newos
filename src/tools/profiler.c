#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define PROFILER_MAX_FUNCTIONS 8192U
#define PROFILER_MAX_SYMBOLS 8192U
#define PROFILER_MAX_STACK 4096U
#define PROFILER_LINE_CAPACITY 1024U
#define PROFILER_NAME_CAPACITY 256U

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
} ProfileStats;

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
static size_t profiler_function_count;
static size_t profiler_symbol_count;
static ProfileSortKey profiler_sort_key = PROFILE_SORT_SELF;

static void print_usage(void) {
    tool_write_usage("profiler", "[-m SYMBOLS] [-n COUNT] [--sort self|total|calls|addr] [--csv] TRACE");
}

static void print_instrumentation_help(void) {
    rt_write_cstr(1, "GCC primary instrumentation flags:\n");
    rt_write_cstr(1, "  gcc -finstrument-functions -fno-omit-frame-pointer -fno-inline -g -O2 ...\n");
    rt_write_cstr(1, "\nClang secondary instrumentation flags:\n");
    rt_write_cstr(1, "  clang -finstrument-functions -fno-omit-frame-pointer -fno-inline -g -O2 ...\n");
    rt_write_cstr(1, "\nProject build shortcut:\n");
    rt_write_cstr(1, "  make freestanding PROFILE=1\n");
    rt_write_cstr(1, "  NEWOS_PROFILE=tool.nprof build/freestanding-linux-x86_64/tool ...\n");
    rt_write_cstr(1, "\nTrace lines accepted by profiler:\n");
    rt_write_cstr(1, "  enter TIME_NS ADDRESS\n");
    rt_write_cstr(1, "  exit  TIME_NS ADDRESS\n");
    rt_write_cstr(1, "Aliases: e/x and +/- are accepted for enter/exit.\n");
    rt_write_cstr(1, "\nSymbol files may use either format:\n");
    rt_write_cstr(1, "  0x401000 function_name\n");
    rt_write_cstr(1, "  0000000000401000 T function_name\n");
}

static int parse_unsigned_auto(const char *text, unsigned long long *value_out) {
    unsigned long long value = 0ULL;
    size_t i = 0U;
    int is_hex = 0;
    int saw_digit = 0;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        is_hex = 1;
        i = 2U;
    }
    for (; text[i] != '\0'; ++i) {
        int digit;

        if (is_hex) {
            digit = tool_hex_value(text[i]);
            if (digit < 0) {
                return -1;
            }
            value = (value << 4U) | (unsigned long long)digit;
        } else {
            if (text[i] < '0' || text[i] > '9') {
                return -1;
            }
            value = (value * 10ULL) + (unsigned long long)(text[i] - '0');
        }
        saw_digit = 1;
    }
    if (!saw_digit) {
        return -1;
    }
    *value_out = value;
    return 0;
}

static int parse_address_token(const char *text, unsigned long long *value_out) {
    size_t i;
    int has_hex_letter = 0;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        return parse_unsigned_auto(text, value_out);
    }
    for (i = 0U; text[i] != '\0'; ++i) {
        int digit = tool_hex_value(text[i]);

        if (digit < 0) {
            return -1;
        }
        if ((text[i] >= 'a' && text[i] <= 'f') || (text[i] >= 'A' && text[i] <= 'F')) {
            has_hex_letter = 1;
        }
    }
    if (!has_hex_letter && rt_strlen(text) < 9U) {
        return parse_unsigned_auto(text, value_out);
    }
    *value_out = 0ULL;
    for (i = 0U; text[i] != '\0'; ++i) {
        int digit = tool_hex_value(text[i]);
        if (digit < 0) {
            return -1;
        }
        *value_out = (*value_out << 4U) | (unsigned long long)digit;
    }
    return 0;
}

static int next_token(const char **cursor_io, char *token, size_t token_size) {
    const char *cursor = *cursor_io;
    size_t length = 0U;

    while (*cursor != '\0' && tool_ascii_is_token_space(*cursor)) {
        cursor++;
    }
    if (*cursor == '\0' || *cursor == '#') {
        *cursor_io = cursor;
        return 0;
    }
    while (*cursor != '\0' && !tool_ascii_is_token_space(*cursor)) {
        if (length + 1U < token_size) {
            token[length++] = *cursor;
        }
        cursor++;
    }
    token[length] = '\0';
    *cursor_io = cursor;
    return length == 0U ? 0 : 1;
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

static int find_function(unsigned long long address) {
    size_t i;

    for (i = 0U; i < profiler_function_count; ++i) {
        if (profiler_functions[i].address == address) {
            return (int)i;
        }
    }
    return -1;
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
    *index_out = profiler_function_count++;
    return 0;
}

static int symbol_type_is_function(const char *type) {
    return rt_strcmp(type, "T") == 0 || rt_strcmp(type, "t") == 0 ||
           rt_strcmp(type, "W") == 0 || rt_strcmp(type, "w") == 0;
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
    while (count > 0U) {
        count -= 1U;
        rt_write_char(1, digits[count]);
    }
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

static void write_percent_2(unsigned long long value, unsigned long long total) {
    unsigned long long scaled;

    if (total == 0ULL) {
        rt_write_cstr(1, "0.00");
        return;
    }
    scaled = (value * 10000ULL) / total;
    rt_write_uint(1, scaled / 100ULL);
    rt_write_char(1, '.');
    rt_write_char(1, (char)('0' + ((scaled / 10ULL) % 10ULL)));
    rt_write_char(1, (char)('0' + (scaled % 10ULL)));
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

        if (!next_token(&cursor, first, sizeof(first))) {
            continue;
        }
        if (parse_address_token(first, &address) != 0) {
            continue;
        }
        if (!next_token(&cursor, second, sizeof(second))) {
            continue;
        }
        if (rt_strlen(second) == 1U && next_token(&cursor, third, sizeof(third))) {
            if (symbol_type_is_function(second)) {
                add_symbol(address, third, 1);
            }
        } else {
            add_symbol(address, second, 1);
        }
    }
    tool_close_input(fd, should_close);
    return 0;
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

static int process_trace_line(const char *line, size_t *stack_depth_io, ProfileStats *stats) {
    const char *cursor = line;
    char kind[32];
    char time_token[64];
    char address_token[64];
    int is_enter;
    unsigned long long timestamp_ns;
    unsigned long long address;

    if (!next_token(&cursor, kind, sizeof(kind))) {
        return 0;
    }
    if (event_kind_from_token(kind, &is_enter) != 0 ||
        !next_token(&cursor, time_token, sizeof(time_token)) ||
        !next_token(&cursor, address_token, sizeof(address_token)) ||
        parse_unsigned_auto(time_token, &timestamp_ns) != 0 ||
        parse_address_token(address_token, &address) != 0) {
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
    ToolRecordReader reader;
    char line[PROFILER_LINE_CAPACITY];
    int has_record;
    size_t stack_depth = 0U;

    if (tool_open_input(path, &fd, &should_close) != 0) {
        tool_write_error("profiler", "failed to open trace: ", path);
        return -1;
    }
    tool_record_reader_init(&reader, fd, '\n');
    while (tool_record_reader_next(&reader, line, sizeof(line), &has_record) == 0 && has_record) {
        if (process_trace_line(line, &stack_depth, stats) != 0) {
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
        write_percent_2(function->self_ns, program_total_ns);
        rt_write_char(1, ',');
        write_percent_2(function->total_ns, program_total_ns);
        rt_write_char(1, ',');
        rt_write_uint(1, avg_self);
        rt_write_char(1, ',');
        rt_write_uint(1, avg_total);
        rt_write_char(1, ',');
        write_hex_address(function->address);
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
    write_percent_2(function->self_ns, program_total_ns);
    rt_write_char(1, '\t');
    write_percent_2(function->total_ns, program_total_ns);
    rt_write_char(1, '\t');
    write_ns_as_ms(avg_self);
    rt_write_char(1, '\t');
    write_ns_as_ms(avg_total);
    rt_write_char(1, '\t');
    write_hex_address(function->address);
    rt_write_char(1, '\t');
    rt_write_line(1, symbol != 0 ? symbol : "?");
}

static void write_report(unsigned long long limit, int csv, const ProfileStats *stats) {
    size_t i;
    size_t count = profiler_function_count;
    unsigned long long program_total_ns = 0ULL;

    for (i = 0U; i < profiler_function_count; ++i) {
        profiler_rows[i].function_index = i;
        if (profiler_functions[i].total_ns > program_total_ns) {
            program_total_ns = profiler_functions[i].total_ns;
        }
    }
    rt_sort(profiler_rows, profiler_function_count, sizeof(profiler_rows[0]), compare_rows);
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
        rt_write_char(2, '\n');
    }
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
    write_report(limit, csv, &stats);
    return 0;
}
