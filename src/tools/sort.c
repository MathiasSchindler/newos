#include "concurrency.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#include <limits.h>

#define SORT_MAX_LINES 131072U
#define SORT_MAX_INPUTS 8U
#define SORT_MAX_RUNS 128U
#define SORT_IO_BUFFER_SIZE 8192U
#define SORT_OUTPUT_BUFFER_SIZE 16384U
#define SORT_STORAGE_CAPACITY (4U * 1024U * 1024U)
#define SORT_LINE_CAPACITY (64U * 1024U)
#define SORT_TEMP_PATH_CAPACITY 256U
#define SORT_DEFAULT_MAX_WORKERS 8U
#define SORT_PARALLEL_MIN_LINES 8192U

#define SORT_COLLECT_READ_ERROR (-1)
#define SORT_COLLECT_MEMORY_ERROR (-2)

#define SORT_MODE_TEXT 0
#define SORT_MODE_NUMERIC 1
#define SORT_MODE_HUMAN_NUMERIC 2
#define SORT_MODE_MONTH 3
#define SORT_MODE_VERSION 4

typedef struct {
    int check_only;
    int quiet_check;
    int ignore_case;
    int ignore_leading_blanks;
    int dictionary_order;
    int ignore_nonprinting;
    int sort_mode;
    int reverse;
    int unique;
    int merge_mode;
    int have_key;
    unsigned long long key_start;
    unsigned long long key_end;
    int have_separator;
    char separator;
    const char *output_path;
} SortOptions;

typedef struct {
    char *text;
    size_t length;
} SortLine;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    char fixed_data[SORT_LINE_CAPACITY];
} SortLineBuilder;

typedef struct {
    int fd;
    int should_close;
    int finished;
    int have_current;
    char buffer[SORT_IO_BUFFER_SIZE];
    size_t buffer_pos;
    size_t buffer_len;
    SortLineBuilder builder;
    SortLine current;
} SortInput;

typedef struct {
    size_t count;
    SortLine *lines;
    SortLine **order;
    SortLine **scratch;
    char *storage;
    size_t storage_used;
} SortCollection;

typedef struct {
    size_t count;
    char paths[SORT_MAX_RUNS][SORT_TEMP_PATH_CAPACITY];
} SortRunSet;

typedef ToolOutputBuffer SortOutput;

typedef struct {
    SortLine **order;
    SortLine **scratch;
    size_t begin;
    size_t end;
    const SortOptions *options;
} SortChunkJob;

typedef struct {
    int valid;
    int negative;
    const char *int_digits;
    size_t int_len;
    const char *frac_digits;
    size_t frac_len;
} SortNumericKey;

typedef struct {
    int valid;
    int negative;
    unsigned long long magnitude;
} SortHumanKey;

static int flush_sort_run(SortCollection *collection, const SortOptions *options, SortRunSet *runs);
static void sort_collection_free(SortCollection *collection);

static void write_usage(int fd) {
    rt_write_line(fd, "Usage: sort [-bCcdfiMmnrsuV] [--human-numeric-sort] [-o FILE] [-t CHAR] [-k FIELD[,FIELD]] [file ...]");
}

static int parse_key_spec(const char *text, SortOptions *options) {
    unsigned long long start = 0ULL;
    unsigned long long end = 0ULL;
    size_t index = 0U;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }

    while (text[index] >= '0' && text[index] <= '9') {
        unsigned long long digit = (unsigned long long)(text[index] - '0');
        if (start > (ULLONG_MAX - digit) / 10ULL) {
            return -1;
        }
        start = start * 10ULL + digit;
        index += 1U;
    }

    if (start == 0ULL) {
        return -1;
    }

    end = start;
    if (text[index] == ',') {
        index += 1U;
        if (text[index] == '\0') {
            return -1;
        }

        end = 0ULL;
        while (text[index] >= '0' && text[index] <= '9') {
            unsigned long long digit = (unsigned long long)(text[index] - '0');
            if (end > (ULLONG_MAX - digit) / 10ULL) {
                return -1;
            }
            end = end * 10ULL + digit;
            index += 1U;
        }

        if (end == 0ULL || end < start) {
            return -1;
        }
    }

    if (text[index] != '\0') {
        return -1;
    }

    options->have_key = 1;
    options->key_start = start;
    options->key_end = end;
    return 0;
}

static void line_builder_init(SortLineBuilder *builder) {
    rt_memset(builder, 0, sizeof(*builder));
    builder->data = builder->fixed_data;
    builder->capacity = sizeof(builder->fixed_data);
}

static void line_builder_reset(SortLineBuilder *builder) {
    builder->length = 0U;
}

static int line_builder_ensure_capacity(SortLineBuilder *builder, size_t needed) {
    return needed <= builder->capacity ? 0 : -1;
}

static int line_builder_append_span(SortLineBuilder *builder, const char *text, size_t length) {
    if (length == 0U) {
        return 0;
    }
    if (line_builder_ensure_capacity(builder, builder->length + length + 1U) != 0) {
        return -1;
    }

    memcpy(builder->data + builder->length, text, length);
    builder->length += length;
    builder->data[builder->length] = '\0';
    return 0;
}

static void line_builder_free(SortLineBuilder *builder) {
    (void)builder;
}

static int line_builder_copy_text(SortLineBuilder *builder, const char *text, size_t length) {
    if (line_builder_ensure_capacity(builder, length + 1U) != 0) {
        return -1;
    }

    if (length > 0U && text != 0) {
        memcpy(builder->data, text, length);
    }
    builder->length = length;
    builder->data[length] = '\0';
    return 0;
}

static void sort_input_init(SortInput *input) {
    rt_memset(input, 0, sizeof(*input));
    line_builder_init(&input->builder);
}

static void sort_input_close(SortInput *input) {
    if (input->should_close) {
        (void)platform_close(input->fd);
    }
    line_builder_free(&input->builder);
}

static int sort_input_open(SortInput *input, const char *path) {
    sort_input_init(input);
    return tool_open_input(path, &input->fd, &input->should_close);
}

static int sort_input_read_next(SortInput *input) {
    line_builder_reset(&input->builder);

    while (1) {
        if (input->buffer_pos >= input->buffer_len) {
            long bytes_read;

            if (input->finished) {
                input->have_current = 0;
                return 0;
            }

            bytes_read = platform_read(input->fd, input->buffer, sizeof(input->buffer));
            if (bytes_read < 0) {
                input->have_current = 0;
                return SORT_COLLECT_READ_ERROR;
            }
            if (bytes_read == 0) {
                input->finished = 1;
                if (input->builder.length > 0U) {
                    input->current.text = input->builder.data != 0 ? input->builder.data : "";
                    input->current.length = input->builder.length;
                    input->have_current = 1;
                    return 1;
                }
                input->have_current = 0;
                return 0;
            }

            input->buffer_pos = 0U;
            input->buffer_len = (size_t)bytes_read;
        }

        while (input->buffer_pos < input->buffer_len) {
            size_t start = input->buffer_pos;

            while (input->buffer_pos < input->buffer_len && input->buffer[input->buffer_pos] != '\n') {
                input->buffer_pos += 1U;
            }

            if (line_builder_append_span(&input->builder, input->buffer + start, input->buffer_pos - start) != 0) {
                input->have_current = 0;
                return SORT_COLLECT_MEMORY_ERROR;
            }
            if (input->buffer_pos < input->buffer_len && input->buffer[input->buffer_pos] == '\n') {
                input->buffer_pos += 1U;
                input->current.text = input->builder.data != 0 ? input->builder.data : "";
                input->current.length = input->builder.length;
                input->have_current = 1;
                return 1;
            }
        }
    }
}

static int sort_collection_init(SortCollection *collection) {
    rt_memset(collection, 0, sizeof(*collection));

    collection->lines = (SortLine *)rt_malloc_array(SORT_MAX_LINES, sizeof(collection->lines[0]));
    collection->order = (SortLine **)rt_malloc_array(SORT_MAX_LINES, sizeof(collection->order[0]));
    collection->scratch = (SortLine **)rt_malloc_array(SORT_MAX_LINES, sizeof(collection->scratch[0]));
    collection->storage = (char *)rt_malloc(SORT_STORAGE_CAPACITY);
    if (collection->lines == 0 || collection->order == 0 || collection->scratch == 0 || collection->storage == 0) {
        sort_collection_free(collection);
        return -1;
    }
    return 0;
}

static void sort_collection_reset(SortCollection *collection) {
    collection->count = 0U;
    collection->storage_used = 0U;
}

static int sort_collection_reserve(SortCollection *collection, size_t needed) {
    (void)collection;
    return needed <= SORT_MAX_LINES ? 0 : -1;
}

static int sort_collection_store_line(SortCollection *collection, const char *text, size_t length) {
    char *stored_text;
    size_t remaining;

    if (collection->count >= SORT_MAX_LINES ||
        sort_collection_reserve(collection, collection->count + 1U) != 0) {
        return -1;
    }

    remaining = SORT_STORAGE_CAPACITY - collection->storage_used;
    if (length >= remaining) {
        return -1;
    }
    stored_text = collection->storage + collection->storage_used;
    collection->storage_used += length + 1U;

    if (length > 0U) {
        memcpy(stored_text, text, length);
    }
    stored_text[length] = '\0';

    collection->lines[collection->count].text = stored_text;
    collection->lines[collection->count].length = length;
    collection->count += 1U;
    return 0;
}

static void sort_collection_free(SortCollection *collection) {
    rt_free(collection->lines);
    rt_free(collection->order);
    rt_free(collection->scratch);
    rt_free(collection->storage);
    rt_memset(collection, 0, sizeof(*collection));
}

static void sort_collection_prepare_order(SortCollection *collection) {
    size_t i;

    for (i = 0U; i < collection->count; ++i) {
        collection->order[i] = &collection->lines[i];
    }
}

static void sort_run_set_init(SortRunSet *runs) {
    rt_memset(runs, 0, sizeof(*runs));
}

static void sort_run_set_cleanup(SortRunSet *runs) {
    size_t i;

    for (i = 0U; i < runs->count; ++i) {
        if (runs->paths[i][0] != '\0') {
            (void)platform_remove_file(runs->paths[i]);
            runs->paths[i][0] = '\0';
        }
    }
    runs->count = 0U;
}

static int collect_external_line(SortCollection *collection,
                                 const SortOptions *options,
                                 SortRunSet *runs,
                                 const char *text,
                                 size_t length) {
    if (sort_collection_store_line(collection, text, length) == 0) {
        return 0;
    }

    if (collection->count == 0U || flush_sort_run(collection, options, runs) != 0) {
        return SORT_COLLECT_MEMORY_ERROR;
    }
    if (sort_collection_store_line(collection, text, length) != 0) {
        return SORT_COLLECT_MEMORY_ERROR;
    }
    return 0;
}

static int collect_external_from_fd(int fd,
                                    SortCollection *collection,
                                    const SortOptions *options,
                                    SortRunSet *runs) {
    char buffer[SORT_IO_BUFFER_SIZE];
    SortLineBuilder builder;
    long bytes_read;

    line_builder_init(&builder);

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        size_t index = 0U;

        while (index < (size_t)bytes_read) {
            size_t start = index;

            while (index < (size_t)bytes_read && buffer[index] != '\n') {
                index += 1U;
            }
            if (index < (size_t)bytes_read && buffer[index] == '\n') {
                const char *line_text;
                size_t line_length;

                if (builder.length == 0U) {
                    line_text = buffer + start;
                    line_length = index - start;
                } else {
                    if (line_builder_append_span(&builder, buffer + start, index - start) != 0) {
                        line_builder_free(&builder);
                        return SORT_COLLECT_MEMORY_ERROR;
                    }
                    line_text = builder.data != 0 ? builder.data : "";
                    line_length = builder.length;
                }
                if (collect_external_line(collection, options, runs, line_text, line_length) != 0) {
                    line_builder_free(&builder);
                    return SORT_COLLECT_MEMORY_ERROR;
                }
                line_builder_reset(&builder);
                index += 1U;
            } else if (line_builder_append_span(&builder, buffer + start, index - start) != 0) {
                line_builder_free(&builder);
                return SORT_COLLECT_MEMORY_ERROR;
            }
        }
    }

    if (bytes_read < 0) {
        line_builder_free(&builder);
        return SORT_COLLECT_READ_ERROR;
    }

    if (builder.length > 0U) {
        if (collect_external_line(collection, options, runs, builder.data != 0 ? builder.data : "", builder.length) != 0) {
            line_builder_free(&builder);
            return SORT_COLLECT_MEMORY_ERROR;
        }
    }

    line_builder_free(&builder);
    return 0;
}



static int is_sort_alpha(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

static int is_sort_alnum(char ch) {
    return is_sort_alpha(ch) || tool_ascii_is_digit(ch);
}

static int is_sort_printable(unsigned char ch) {
    return ch >= 0x20U && ch < 0x7fU;
}


static void sort_trim_leading_blanks(const char **text, size_t *length) {
    while (*length > 0U && tool_ascii_is_blank((*text)[0])) {
        *text += 1;
        *length -= 1U;
    }
}

static int compare_text_spans(const char *left,
                              size_t left_len,
                              const char *right,
                              size_t right_len,
                              const SortOptions *options) {
    size_t left_index = 0U;
    size_t right_index = 0U;
    int ignore_case = options->ignore_case;
    int dictionary_order = options->dictionary_order;
    int ignore_nonprinting = options->ignore_nonprinting;

    if (!dictionary_order && !ignore_nonprinting) {
        if (!ignore_case) {
            size_t limit = left_len < right_len ? left_len : right_len;
            size_t i;

            for (i = 0U; i < limit; ++i) {
                unsigned char lhs = (unsigned char)left[i];
                unsigned char rhs = (unsigned char)right[i];

                if (lhs != rhs) {
                    return lhs < rhs ? -1 : 1;
                }
            }

            if (left_len == right_len) {
                return 0;
            }
            return left_len < right_len ? -1 : 1;
        }

        while (left_index < left_len && right_index < right_len) {
            unsigned char lhs = (unsigned char)tool_ascii_tolower(left[left_index]);
            unsigned char rhs = (unsigned char)tool_ascii_tolower(right[right_index]);

            if (lhs != rhs) {
                return lhs < rhs ? -1 : 1;
            }

            left_index += 1U;
            right_index += 1U;
        }

        if (left_index >= left_len && right_index >= right_len) {
            return 0;
        }
        return left_index >= left_len ? -1 : 1;
    }

    while (1) {
        unsigned char lhs;
        unsigned char rhs;

        while (left_index < left_len) {
            unsigned char ch = (unsigned char)left[left_index];

            if (ignore_nonprinting && !is_sort_printable(ch)) {
                left_index += 1U;
                continue;
            }
            if (dictionary_order && !is_sort_alnum((char)ch) && !tool_ascii_is_blank((char)ch)) {
                left_index += 1U;
                continue;
            }
            break;
        }
        while (right_index < right_len) {
            unsigned char ch = (unsigned char)right[right_index];

            if (ignore_nonprinting && !is_sort_printable(ch)) {
                right_index += 1U;
                continue;
            }
            if (dictionary_order && !is_sort_alnum((char)ch) && !tool_ascii_is_blank((char)ch)) {
                right_index += 1U;
                continue;
            }
            break;
        }

        if (left_index >= left_len || right_index >= right_len) {
            break;
        }

        lhs = (unsigned char)(ignore_case ? tool_ascii_tolower(left[left_index]) : left[left_index]);
        rhs = (unsigned char)(ignore_case ? tool_ascii_tolower(right[right_index]) : right[right_index]);

        if (lhs != rhs) {
            return lhs < rhs ? -1 : 1;
        }
        left_index += 1U;
        right_index += 1U;
    }

    while (left_index < left_len) {
        unsigned char ch = (unsigned char)left[left_index];

        if (ignore_nonprinting && !is_sort_printable(ch)) {
            left_index += 1U;
            continue;
        }
        if (dictionary_order && !is_sort_alnum((char)ch) && !tool_ascii_is_blank((char)ch)) {
            left_index += 1U;
            continue;
        }
        break;
    }
    while (right_index < right_len) {
        unsigned char ch = (unsigned char)right[right_index];

        if (ignore_nonprinting && !is_sort_printable(ch)) {
            right_index += 1U;
            continue;
        }
        if (dictionary_order && !is_sort_alnum((char)ch) && !tool_ascii_is_blank((char)ch)) {
            right_index += 1U;
            continue;
        }
        break;
    }

    if (left_index >= left_len && right_index >= right_len) {
        return 0;
    }
    if (left_index >= left_len) {
        return -1;
    }
    return 1;
}

static int compare_plain_spans(const char *left, size_t left_len, const char *right, size_t right_len) {
    size_t limit = left_len < right_len ? left_len : right_len;
    int result = limit != 0U ? memcmp(left, right, limit) : 0;

    if (result < 0) return -1;
    if (result > 0) return 1;
    if (left_len == right_len) return 0;
    return left_len < right_len ? -1 : 1;
}

static void extract_key_span(const SortLine *line,
                             const SortOptions *options,
                             const char **start_out,
                             size_t *length_out) {
    size_t start_index = 0U;
    size_t end_index = line->length;

    *start_out = line->text;
    *length_out = line->length;

    if (!options->have_key) {
        if (options->ignore_leading_blanks) {
            sort_trim_leading_blanks(start_out, length_out);
        }
        return;
    }

    if (options->have_separator) {
        unsigned long long field = 1ULL;
        size_t i = 0U;
        int have_start = 0;
        int have_end = 0;

        while (1) {
            size_t field_begin = i;

            while (i < line->length && line->text[i] != options->separator) {
                i += 1U;
            }

            if (field == options->key_start) {
                start_index = field_begin;
                have_start = 1;
            }
            if (field == options->key_end) {
                end_index = i;
                have_end = 1;
            }

            if (i >= line->length) {
                break;
            }

            i += 1U;
            field += 1ULL;
        }

        if (!have_start) {
            *start_out = line->text;
            *length_out = 0U;
            return;
        }
        if (!have_end) {
            end_index = line->length;
        }
    } else {
        unsigned long long field = 0ULL;
        size_t i = 0U;
        int have_start = 0;
        int have_end = 0;

        while (i < line->length) {
            while (i < line->length && tool_ascii_is_blank(line->text[i])) {
                i += 1U;
            }
            if (i >= line->length) {
                break;
            }

            field += 1ULL;
            if (field == options->key_start) {
                start_index = i;
                have_start = 1;
            }

            while (i < line->length && !tool_ascii_is_blank(line->text[i])) {
                i += 1U;
            }

            if (field == options->key_end) {
                end_index = i;
                have_end = 1;
                break;
            }
        }

        if (!have_start) {
            *start_out = line->text;
            *length_out = 0U;
            return;
        }
        if (!have_end) {
            end_index = line->length;
        }
    }

    if (end_index < start_index) {
        end_index = start_index;
    }

    *start_out = line->text + start_index;
    *length_out = end_index - start_index;
    if (options->ignore_leading_blanks) {
        sort_trim_leading_blanks(start_out, length_out);
    }
}

static void parse_numeric_key(const char *text, size_t length, SortNumericKey *key) {
    size_t index = 0U;
    size_t integer_start;
    size_t integer_end;
    size_t fraction_start = 0U;
    size_t fraction_end = 0U;

    key->valid = 0;
    key->negative = 0;
    key->int_digits = text;
    key->int_len = 0U;
    key->frac_digits = text;
    key->frac_len = 0U;

    while (index < length && tool_ascii_is_blank(text[index])) {
        index += 1U;
    }

    if (index < length && (text[index] == '-' || text[index] == '+')) {
        key->negative = text[index] == '-';
        index += 1U;
    }

    integer_start = index;
    while (index < length && text[index] >= '0' && text[index] <= '9') {
        index += 1U;
    }
    integer_end = index;

    if (index < length && text[index] == '.') {
        index += 1U;
        fraction_start = index;
        while (index < length && text[index] >= '0' && text[index] <= '9') {
            index += 1U;
        }
        fraction_end = index;
    }

    if (integer_end == integer_start && fraction_end == fraction_start) {
        return;
    }

    while (index < length && tool_ascii_is_blank(text[index])) {
        index += 1U;
    }
    if (index != length) {
        return;
    }

    while (integer_start < integer_end && text[integer_start] == '0') {
        integer_start += 1U;
    }
    while (fraction_end > fraction_start && text[fraction_end - 1U] == '0') {
        fraction_end -= 1U;
    }

    key->valid = 1;
    key->int_digits = text + integer_start;
    key->int_len = integer_end - integer_start;
    key->frac_digits = text + fraction_start;
    key->frac_len = fraction_end - fraction_start;

    if (key->int_len == 0U && key->frac_len == 0U) {
        key->negative = 0;
    }
}

static unsigned long long clamp_multiply_ull(unsigned long long value, unsigned long long factor) {
    if (factor != 0ULL && value > ULLONG_MAX / factor) {
        return ULLONG_MAX;
    }
    return value * factor;
}

static int suffix_scale(char ch) {
    switch (tool_ascii_tolower(ch)) {
        case 'k': return 1;
        case 'm': return 2;
        case 'g': return 3;
        case 't': return 4;
        case 'p': return 5;
        case 'e': return 6;
        default: return -1;
    }
}

static void parse_human_key(const char *text, size_t length, SortHumanKey *key) {
    size_t index = 0U;
    unsigned long long value = 0ULL;
    unsigned int frac_digits = 0U;
    int have_digit = 0;
    int scale = 0;
    int i;

    rt_memset(key, 0, sizeof(*key));

    while (index < length && tool_ascii_is_blank(text[index])) {
        index += 1U;
    }

    if (index < length && (text[index] == '-' || text[index] == '+')) {
        key->negative = text[index] == '-';
        index += 1U;
    }

    while (index < length && tool_ascii_is_digit(text[index])) {
        have_digit = 1;
        if (value <= (ULLONG_MAX - (unsigned long long)(text[index] - '0')) / 10ULL) {
            value = value * 10ULL + (unsigned long long)(text[index] - '0');
        } else {
            value = ULLONG_MAX;
        }
        index += 1U;
    }

    value = clamp_multiply_ull(value, 1000ULL);
    if (index < length && text[index] == '.') {
        index += 1U;
        while (index < length && tool_ascii_is_digit(text[index])) {
            have_digit = 1;
            if (frac_digits < 3U && value != ULLONG_MAX) {
                unsigned long long add = (unsigned long long)(text[index] - '0') *
                                         (frac_digits == 0U ? 100ULL : (frac_digits == 1U ? 10ULL : 1ULL));
                if (value <= ULLONG_MAX - add) {
                    value += add;
                } else {
                    value = ULLONG_MAX;
                }
            }
            frac_digits += 1U;
            index += 1U;
        }
    }

    if (!have_digit) {
        return;
    }

    if (index < length) {
        int parsed_scale = suffix_scale(text[index]);
        if (parsed_scale >= 0) {
            scale = parsed_scale;
            index += 1U;
            if (index < length && (text[index] == 'i' || text[index] == 'I')) {
                index += 1U;
            }
            if (index < length && (text[index] == 'b' || text[index] == 'B')) {
                index += 1U;
            }
        }
    }

    while (index < length && tool_ascii_is_blank(text[index])) {
        index += 1U;
    }
    if (index != length) {
        return;
    }

    for (i = 0; i < scale; ++i) {
        value = clamp_multiply_ull(value, 1024ULL);
    }
    key->valid = 1;
    key->magnitude = value;
    if (value == 0ULL) {
        key->negative = 0;
    }
}

static int compare_numeric_keys(const SortNumericKey *left, const SortNumericKey *right) {
    size_t i;
    int result = 0;

    if (left->negative != right->negative) {
        return left->negative ? -1 : 1;
    }

    if (left->int_len != right->int_len) {
        result = left->int_len < right->int_len ? -1 : 1;
    } else if (left->int_len > 0U) {
        for (i = 0U; i < left->int_len; ++i) {
            if (left->int_digits[i] != right->int_digits[i]) {
                result = left->int_digits[i] < right->int_digits[i] ? -1 : 1;
                break;
            }
        }
    }

    if (result == 0) {
        size_t max_frac = left->frac_len > right->frac_len ? left->frac_len : right->frac_len;

        for (i = 0U; i < max_frac; ++i) {
            char lhs = i < left->frac_len ? left->frac_digits[i] : '0';
            char rhs = i < right->frac_len ? right->frac_digits[i] : '0';

            if (lhs != rhs) {
                result = lhs < rhs ? -1 : 1;
                break;
            }
        }
    }

    return left->negative ? -result : result;
}

static int compare_human_keys(const SortHumanKey *left, const SortHumanKey *right) {
    int result = 0;

    if (left->negative != right->negative) {
        return left->negative ? -1 : 1;
    }
    if (left->magnitude < right->magnitude) {
        result = -1;
    } else if (left->magnitude > right->magnitude) {
        result = 1;
    }
    return left->negative ? -result : result;
}

static int parse_month_value(const char *text, size_t length) {
    char folded[3];
    size_t index = 0U;
    int i;

    while (index < length && tool_ascii_is_blank(text[index])) {
        index += 1U;
    }
    if (index + 3U > length) {
        return 0;
    }
    for (i = 0; i < 3; ++i) {
        folded[i] = tool_ascii_tolower(text[index + (size_t)i]);
        if (!is_sort_alpha(folded[i])) {
            return 0;
        }
    }
    if (folded[0] == 'j') {
        if (folded[1] == 'a' && folded[2] == 'n') return 1;
        if (folded[1] == 'u' && folded[2] == 'n') return 6;
        if (folded[1] == 'u' && folded[2] == 'l') return 7;
    } else if (folded[0] == 'f') {
        if (folded[1] == 'e' && folded[2] == 'b') return 2;
    } else if (folded[0] == 'm') {
        if (folded[1] == 'a' && folded[2] == 'r') return 3;
        if (folded[1] == 'a' && folded[2] == 'y') return 5;
    } else if (folded[0] == 'a') {
        if (folded[1] == 'p' && folded[2] == 'r') return 4;
        if (folded[1] == 'u' && folded[2] == 'g') return 8;
    } else if (folded[0] == 's') {
        if (folded[1] == 'e' && folded[2] == 'p') return 9;
    } else if (folded[0] == 'o') {
        if (folded[1] == 'c' && folded[2] == 't') return 10;
    } else if (folded[0] == 'n') {
        if (folded[1] == 'o' && folded[2] == 'v') return 11;
    } else if (folded[0] == 'd') {
        if (folded[1] == 'e' && folded[2] == 'c') return 12;
    }
    return 0;
}

static int compare_version_spans(const char *left,
                                 size_t left_len,
                                 const char *right,
                                 size_t right_len,
                                 const SortOptions *options) {
    size_t left_index = 0U;
    size_t right_index = 0U;

    while (left_index < left_len || right_index < right_len) {
        if (left_index < left_len && right_index < right_len &&
            tool_ascii_is_digit(left[left_index]) && tool_ascii_is_digit(right[right_index])) {
            size_t left_start = left_index;
            size_t right_start = right_index;
            size_t left_sig;
            size_t right_sig;
            size_t left_sig_len;
            size_t right_sig_len;
            size_t i;

            while (left_index < left_len && tool_ascii_is_digit(left[left_index])) {
                left_index += 1U;
            }
            while (right_index < right_len && tool_ascii_is_digit(right[right_index])) {
                right_index += 1U;
            }

            left_sig = left_start;
            right_sig = right_start;
            while (left_sig < left_index && left[left_sig] == '0') {
                left_sig += 1U;
            }
            while (right_sig < right_index && right[right_sig] == '0') {
                right_sig += 1U;
            }
            left_sig_len = left_index - left_sig;
            right_sig_len = right_index - right_sig;
            if (left_sig_len != right_sig_len) {
                return left_sig_len < right_sig_len ? -1 : 1;
            }
            for (i = 0U; i < left_sig_len; ++i) {
                if (left[left_sig + i] != right[right_sig + i]) {
                    return left[left_sig + i] < right[right_sig + i] ? -1 : 1;
                }
            }
            if ((left_index - left_start) != (right_index - right_start)) {
                return (left_index - left_start) < (right_index - right_start) ? -1 : 1;
            }
            continue;
        }

        if (left_index >= left_len) {
            return -1;
        }
        if (right_index >= right_len) {
            return 1;
        }
        {
            unsigned char lhs = (unsigned char)(options->ignore_case ? tool_ascii_tolower(left[left_index]) : left[left_index]);
            unsigned char rhs = (unsigned char)(options->ignore_case ? tool_ascii_tolower(right[right_index]) : right[right_index]);
            if (lhs != rhs) {
                return lhs < rhs ? -1 : 1;
            }
        }
        left_index += 1U;
        right_index += 1U;
    }

    return 0;
}

static int compare_lines(const SortLine *left, const SortLine *right, const SortOptions *options) {
    const char *left_text = left->text;
    const char *right_text = right->text;
    size_t left_len = left->length;
    size_t right_len = right->length;
    int result = 0;

    if (!options->have_key && !options->ignore_leading_blanks && !options->ignore_case &&
        !options->dictionary_order && !options->ignore_nonprinting && options->sort_mode == SORT_MODE_TEXT) {
        result = compare_plain_spans(left_text, left_len, right_text, right_len);
        return options->reverse ? -result : result;
    }

    extract_key_span(left, options, &left_text, &left_len);
    extract_key_span(right, options, &right_text, &right_len);

    if (options->sort_mode == SORT_MODE_NUMERIC) {
        SortNumericKey left_numeric;
        SortNumericKey right_numeric;

        parse_numeric_key(left_text, left_len, &left_numeric);
        parse_numeric_key(right_text, right_len, &right_numeric);

        if (left_numeric.valid && right_numeric.valid) {
            result = compare_numeric_keys(&left_numeric, &right_numeric);
        }
    } else if (options->sort_mode == SORT_MODE_HUMAN_NUMERIC) {
        SortHumanKey left_human;
        SortHumanKey right_human;

        parse_human_key(left_text, left_len, &left_human);
        parse_human_key(right_text, right_len, &right_human);

        if (left_human.valid && right_human.valid) {
            result = compare_human_keys(&left_human, &right_human);
        }
    } else if (options->sort_mode == SORT_MODE_MONTH) {
        int left_month = parse_month_value(left_text, left_len);
        int right_month = parse_month_value(right_text, right_len);

        if (left_month != 0 && right_month != 0 && left_month != right_month) {
            result = left_month < right_month ? -1 : 1;
        }
    } else if (options->sort_mode == SORT_MODE_VERSION) {
        result = compare_version_spans(left_text, left_len, right_text, right_len, options);
    }

    if (result == 0) {
        result = compare_text_spans(left_text, left_len, right_text, right_len, options);
    }

    return options->reverse ? -result : result;
}

static unsigned int sort_worker_count_from_env(void) {
    const char *value_text = platform_getenv("NEWOS_SORT_WORKERS");
    unsigned long long value;
    unsigned int platform_width;

    if (value_text == 0 || value_text[0] == '\0') {
        platform_width = platform_worker_thread_count();
        if (platform_width == 0U) return 0U;
        return platform_width > SORT_DEFAULT_MAX_WORKERS ? SORT_DEFAULT_MAX_WORKERS : platform_width;
    }
    if (rt_parse_uint(value_text, &value) != 0) {
        return SORT_DEFAULT_MAX_WORKERS;
    }
    if (value > RT_TASK_POOL_MAX_WORKERS) {
        return RT_TASK_POOL_MAX_WORKERS;
    }
    return (unsigned int)value;
}

static void merge_line_order(SortLine **order,
                             SortLine **scratch,
                             size_t left,
                             size_t middle,
                             size_t right,
                             const SortOptions *options) {
    size_t left_index = left;
    size_t right_index = middle;
    size_t out = left;

    while (left_index < middle && right_index < right) {
        if (compare_lines(order[left_index], order[right_index], options) <= 0) {
            scratch[out++] = order[left_index++];
        } else {
            scratch[out++] = order[right_index++];
        }
    }

    while (left_index < middle) {
        scratch[out++] = order[left_index++];
    }
    while (right_index < right) {
        scratch[out++] = order[right_index++];
    }

    for (out = left; out < right; ++out) {
        order[out] = scratch[out];
    }
}

static void merge_sort_lines(SortLine **order,
                             SortLine **scratch,
                             size_t left,
                             size_t right,
                             const SortOptions *options) {
    size_t middle;

    if (right - left < 2U) {
        return;
    }

    middle = left + ((right - left) / 2U);
    merge_sort_lines(order, scratch, left, middle, options);
    merge_sort_lines(order, scratch, middle, right, options);
    merge_line_order(order, scratch, left, middle, right, options);
}

static int sort_chunk_task(unsigned int worker_index, void *arg) {
    SortChunkJob *job = (SortChunkJob *)arg;

    (void)worker_index;
    merge_sort_lines(job->order, job->scratch, job->begin, job->end, job->options);
    return 0;
}

static size_t sort_parallel_chunk_count(size_t line_count, unsigned int worker_count) {
    size_t chunk_count;
    size_t max_chunks;

    if (worker_count < 2U || line_count < SORT_PARALLEL_MIN_LINES * 2U) {
        return 1U;
    }
    chunk_count = (size_t)worker_count;
    max_chunks = line_count / SORT_PARALLEL_MIN_LINES;
    if (max_chunks < 2U) {
        return 1U;
    }
    if (chunk_count > max_chunks) {
        chunk_count = max_chunks;
    }
    if (chunk_count > RT_TASK_POOL_MAX_WORKERS) {
        chunk_count = RT_TASK_POOL_MAX_WORKERS;
    }
    return chunk_count;
}

static void sort_lines_parallel(SortCollection *collection, const SortOptions *options) {
    RtTaskPool pool;
    RtTaskGroup group;
    SortChunkJob jobs[RT_TASK_POOL_MAX_WORKERS];
    size_t begins[RT_TASK_POOL_MAX_WORKERS];
    size_t ends[RT_TASK_POOL_MAX_WORKERS];
    size_t chunk_count;
    size_t chunk_index;
    size_t chunk_size;
    size_t active_count;
    int result;

    rt_memset(&pool, 0, sizeof(pool));
    if (rt_task_pool_init(&pool, sort_worker_count_from_env()) != 0) {
        rt_task_pool_destroy(&pool);
        merge_sort_lines(collection->order, collection->scratch, 0U, collection->count, options);
        return;
    }

    chunk_count = sort_parallel_chunk_count(collection->count, rt_task_pool_width(&pool));
    if (chunk_count < 2U) {
        rt_task_pool_destroy(&pool);
        merge_sort_lines(collection->order, collection->scratch, 0U, collection->count, options);
        return;
    }

    chunk_size = (collection->count + chunk_count - 1U) / chunk_count;
    for (chunk_index = 0U; chunk_index < chunk_count; ++chunk_index) {
        begins[chunk_index] = chunk_index * chunk_size;
        ends[chunk_index] = begins[chunk_index] + chunk_size;
        if (ends[chunk_index] > collection->count) {
            ends[chunk_index] = collection->count;
        }
        jobs[chunk_index].order = collection->order;
        jobs[chunk_index].scratch = collection->scratch;
        jobs[chunk_index].begin = begins[chunk_index];
        jobs[chunk_index].end = ends[chunk_index];
        jobs[chunk_index].options = options;
    }

    result = 0;
    if (rt_task_group_begin(&pool, &group) != 0 || rt_task_group_reserve(&group, chunk_count) != 0) {
        result = -1;
    }
    for (chunk_index = 0U; result == 0 && chunk_index < chunk_count; ++chunk_index) {
        if (rt_task_group_submit(&group, sort_chunk_task, jobs + chunk_index) != 0) {
            result = -1;
        }
    }
    if (result == 0 && rt_task_group_wait(&group) != 0) {
        result = -1;
    }
    rt_task_pool_destroy(&pool);

    if (result != 0) {
        merge_sort_lines(collection->order, collection->scratch, 0U, collection->count, options);
        return;
    }

    active_count = chunk_count;
    while (active_count > 1U) {
        size_t output_count = 0U;

        for (chunk_index = 0U; chunk_index < active_count; chunk_index += 2U) {
            if (chunk_index + 1U < active_count) {
                merge_line_order(collection->order,
                                 collection->scratch,
                                 begins[chunk_index],
                                 begins[chunk_index + 1U],
                                 ends[chunk_index + 1U],
                                 options);
                begins[output_count] = begins[chunk_index];
                ends[output_count] = ends[chunk_index + 1U];
            } else {
                begins[output_count] = begins[chunk_index];
                ends[output_count] = ends[chunk_index];
            }
            output_count += 1U;
        }
        active_count = output_count;
    }
}

static void sort_lines(SortCollection *collection, const SortOptions *options) {
    if (collection->count < 2U) {
        return;
    }

    sort_lines_parallel(collection, options);
}

static int sort_output_write_line(SortOutput *output, const SortLine *line) {
    if (tool_output_buffer_write(output, line->text, line->length) != 0 ||
        tool_output_buffer_write_char(output, '\n') != 0) {
        return -1;
    }
    return 0;
}

static int write_sorted_output(int fd, const SortCollection *collection, const SortOptions *options) {
    size_t i;
    SortLine *previous = 0;
    SortOutput output;

    tool_output_buffer_init(&output, fd);

    for (i = 0U; i < collection->count; ++i) {
        SortLine *line = collection->order[i];

        if (options->unique && previous != 0 && compare_lines(previous, line, options) == 0) {
            continue;
        }

        if (sort_output_write_line(&output, line) != 0) {
            return -1;
        }

        previous = line;
    }

    return tool_output_buffer_flush(&output);
}

static int flush_sort_run(SortCollection *collection, const SortOptions *options, SortRunSet *runs) {
    int fd;

    if (collection->count == 0U) {
        return 0;
    }
    if (runs->count >= SORT_MAX_RUNS) {
        rt_write_line(2, "sort: too many temporary runs");
        return -1;
    }

    fd = platform_create_temp_file(runs->paths[runs->count], sizeof(runs->paths[runs->count]), "/tmp/newos-sort-", 0600U);
    if (fd < 0) {
        rt_write_line(2, "sort: cannot create temporary file");
        runs->paths[runs->count][0] = '\0';
        return -1;
    }

    sort_collection_prepare_order(collection);
    sort_lines(collection, options);
    if (write_sorted_output(fd, collection, options) != 0) {
        rt_write_line(2, "sort: write error");
        (void)platform_close(fd);
        (void)platform_remove_file(runs->paths[runs->count]);
        runs->paths[runs->count][0] = '\0';
        return -1;
    }
    if (platform_close(fd) != 0) {
        rt_write_line(2, "sort: write error");
        (void)platform_remove_file(runs->paths[runs->count]);
        runs->paths[runs->count][0] = '\0';
        return -1;
    }

    runs->count += 1U;
    sort_collection_reset(collection);
    return 0;
}

static int copy_sort_line(SortLineBuilder *builder, SortLine *dest, const SortLine *src) {
    if (line_builder_copy_text(builder, src->text, src->length) != 0) {
        return -1;
    }
    dest->text = builder->data != 0 ? builder->data : "";
    dest->length = src->length;
    return 0;
}

static int report_disorder(const SortOptions *options, const SortLine *line) {
    if (!options->quiet_check) {
        rt_write_cstr(2, "sort: disorder: ");
        rt_write_line(2, line->text);
    }
    return 1;
}

static int check_input_sorted(int argc, char **argv, int argi, const SortOptions *options) {
    SortLineBuilder previous_builder;
    SortLine previous;
    int have_previous = 0;
    int status = 0;
    int i;

    rt_memset(&previous, 0, sizeof(previous));
    line_builder_init(&previous_builder);

    if (argi == argc) {
        SortInput input;
        sort_input_init(&input);
        input.fd = 0;
        input.should_close = 0;

        while ((status = sort_input_read_next(&input)) > 0) {
            if (have_previous) {
                int comparison = compare_lines(&previous, &input.current, options);
                if (comparison > 0 || (options->unique && comparison == 0)) {
                    int rc = report_disorder(options, &input.current);
                    sort_input_close(&input);
                    line_builder_free(&previous_builder);
                    return rc;
                }
            }
            if (copy_sort_line(&previous_builder, &previous, &input.current) != 0) {
                sort_input_close(&input);
                line_builder_free(&previous_builder);
                rt_write_line(2, "sort: input too large for available memory");
                return 1;
            }
            have_previous = 1;
        }

        sort_input_close(&input);
        if (status == SORT_COLLECT_READ_ERROR) {
            rt_write_line(2, "sort: read error");
            line_builder_free(&previous_builder);
            return 1;
        }
        if (status == SORT_COLLECT_MEMORY_ERROR) {
            rt_write_line(2, "sort: input too large for available memory");
            line_builder_free(&previous_builder);
            return 1;
        }
    } else {
        for (i = argi; i < argc; ++i) {
            SortInput input;

            if (sort_input_open(&input, argv[i]) != 0) {
                rt_write_cstr(2, "sort: cannot open ");
                rt_write_line(2, argv[i]);
                line_builder_free(&previous_builder);
                return 1;
            }

            while ((status = sort_input_read_next(&input)) > 0) {
                if (have_previous) {
                    int comparison = compare_lines(&previous, &input.current, options);
                    if (comparison > 0 || (options->unique && comparison == 0)) {
                        int rc = report_disorder(options, &input.current);
                        sort_input_close(&input);
                        line_builder_free(&previous_builder);
                        return rc;
                    }
                }
                if (copy_sort_line(&previous_builder, &previous, &input.current) != 0) {
                    sort_input_close(&input);
                    line_builder_free(&previous_builder);
                    rt_write_line(2, "sort: input too large for available memory");
                    return 1;
                }
                have_previous = 1;
            }

            sort_input_close(&input);
            if (status == SORT_COLLECT_READ_ERROR) {
                rt_write_cstr(2, "sort: read error on ");
                rt_write_line(2, argv[i]);
                line_builder_free(&previous_builder);
                return 1;
            }
            if (status == SORT_COLLECT_MEMORY_ERROR) {
                rt_write_cstr(2, "sort: input too large while reading ");
                rt_write_line(2, argv[i]);
                line_builder_free(&previous_builder);
                return 1;
            }
        }
    }

    line_builder_free(&previous_builder);
    return 0;
}

static int merge_sorted_inputs(int argc, char **argv, int argi, int output_fd, const SortOptions *options, int *exit_code) {
    int input_count = (argi == argc) ? 1 : (argc - argi);
    int active_count = 0;
    int i;
    SortLineBuilder previous_builder;
    SortLine previous_line;
    SortInput fixed_inputs[SORT_MAX_INPUTS];
    SortInput *inputs = fixed_inputs;
    SortOutput output;
    int have_previous = 0;

    rt_memset(&previous_line, 0, sizeof(previous_line));
    line_builder_init(&previous_builder);
    tool_output_buffer_init(&output, output_fd);

    if ((size_t)input_count > SORT_MAX_INPUTS) {
        line_builder_free(&previous_builder);
        rt_write_line(2, "sort: too many inputs for merge mode");
        return 1;
    }

    for (i = 0; i < input_count; ++i) {
        const char *path = (argi == argc) ? "-" : argv[argi + i];
        int status;

        if (sort_input_open(&inputs[i], path) != 0) {
            rt_write_cstr(2, "sort: cannot open ");
            rt_write_line(2, path);
            *exit_code = 1;
            continue;
        }

        status = sort_input_read_next(&inputs[i]);
        if (status > 0) {
            active_count += 1;
            continue;
        }
        if (status == SORT_COLLECT_READ_ERROR) {
            rt_write_cstr(2, "sort: read error on ");
            rt_write_line(2, path);
            sort_input_close(&inputs[i]);
            line_builder_free(&previous_builder);
            return 1;
        }
        if (status == SORT_COLLECT_MEMORY_ERROR) {
            rt_write_cstr(2, "sort: input too large while reading ");
            rt_write_line(2, path);
            sort_input_close(&inputs[i]);
            line_builder_free(&previous_builder);
            return 1;
        }
    }

    while (active_count > 0) {
        int best_index = -1;

        for (i = 0; i < input_count; ++i) {
            if (!inputs[i].have_current) {
                continue;
            }
            if (best_index < 0 || compare_lines(&inputs[i].current, &inputs[best_index].current, options) < 0) {
                best_index = i;
            }
        }

        if (best_index < 0) {
            break;
        }

        if (!options->unique) {
            if (sort_output_write_line(&output, &inputs[best_index].current) != 0) {
                rt_write_line(2, "sort: write error");
                line_builder_free(&previous_builder);
                for (i = 0; i < input_count; ++i) {
                    sort_input_close(&inputs[i]);
                }
                return 1;
            }
        } else if (!have_previous || compare_lines(&previous_line, &inputs[best_index].current, options) != 0) {
            if (sort_output_write_line(&output, &inputs[best_index].current) != 0) {
                rt_write_line(2, "sort: write error");
                line_builder_free(&previous_builder);
                for (i = 0; i < input_count; ++i) {
                    sort_input_close(&inputs[i]);
                }
                return 1;
            }
            if (copy_sort_line(&previous_builder, &previous_line, &inputs[best_index].current) != 0) {
                rt_write_line(2, "sort: input too large for available memory");
                line_builder_free(&previous_builder);
                for (i = 0; i < input_count; ++i) {
                    sort_input_close(&inputs[i]);
                }
                return 1;
            }
            have_previous = 1;
        }

        {
            int status = sort_input_read_next(&inputs[best_index]);
            if (status <= 0) {
                if (status == SORT_COLLECT_READ_ERROR) {
                    rt_write_cstr(2, "sort: read error on ");
                    rt_write_line(2, (argi == argc) ? "-" : argv[argi + best_index]);
                    line_builder_free(&previous_builder);
                    for (i = 0; i < input_count; ++i) {
                        sort_input_close(&inputs[i]);
                    }
                    return 1;
                }
                if (status == SORT_COLLECT_MEMORY_ERROR) {
                    rt_write_cstr(2, "sort: input too large while reading ");
                    rt_write_line(2, (argi == argc) ? "-" : argv[argi + best_index]);
                    line_builder_free(&previous_builder);
                    for (i = 0; i < input_count; ++i) {
                        sort_input_close(&inputs[i]);
                    }
                    return 1;
                }
                inputs[best_index].have_current = 0;
                active_count -= 1;
            }
        }
    }

    if (tool_output_buffer_flush(&output) != 0) {
        rt_write_line(2, "sort: write error");
        line_builder_free(&previous_builder);
        for (i = 0; i < input_count; ++i) {
            sort_input_close(&inputs[i]);
        }
        return 1;
    }

    line_builder_free(&previous_builder);
    for (i = 0; i < input_count; ++i) {
        sort_input_close(&inputs[i]);
    }
    return *exit_code;
}

static int merge_run_paths(char paths[][SORT_TEMP_PATH_CAPACITY],
                           size_t path_count,
                           int output_fd,
                           const SortOptions *options) {
    char *argv[SORT_MAX_INPUTS];
    int exit_code = 0;
    size_t i;

    if (path_count == 0U || path_count > SORT_MAX_INPUTS) {
        return 1;
    }

    for (i = 0U; i < path_count; ++i) {
        argv[i] = paths[i];
    }
    return merge_sorted_inputs((int)path_count, argv, 0, output_fd, options, &exit_code);
}

static int merge_run_set_to_output(SortRunSet *runs, int output_fd, const SortOptions *options) {
    SortRunSet next_runs;

    while (runs->count > SORT_MAX_INPUTS) {
        size_t index = 0U;

        sort_run_set_init(&next_runs);
        while (index < runs->count) {
            size_t group_count = runs->count - index;
            int fd;
            int rc;

            if (group_count > SORT_MAX_INPUTS) {
                group_count = SORT_MAX_INPUTS;
            }
            if (next_runs.count >= SORT_MAX_RUNS) {
                rt_write_line(2, "sort: too many temporary runs");
                sort_run_set_cleanup(&next_runs);
                return 1;
            }

            fd = platform_create_temp_file(next_runs.paths[next_runs.count], sizeof(next_runs.paths[next_runs.count]), "/tmp/newos-sort-", 0600U);
            if (fd < 0) {
                rt_write_line(2, "sort: cannot create temporary file");
                sort_run_set_cleanup(&next_runs);
                return 1;
            }
            rc = merge_run_paths(&runs->paths[index], group_count, fd, options);
            if (platform_close(fd) != 0 && rc == 0) {
                rt_write_line(2, "sort: write error");
                rc = 1;
            }
            if (rc != 0) {
                (void)platform_remove_file(next_runs.paths[next_runs.count]);
                next_runs.paths[next_runs.count][0] = '\0';
                sort_run_set_cleanup(&next_runs);
                return 1;
            }
            next_runs.count += 1U;
            index += group_count;
        }

        sort_run_set_cleanup(runs);
        *runs = next_runs;
    }

    if (runs->count == 0U) {
        return 0;
    }
    return merge_run_paths(runs->paths, runs->count, output_fd, options);
}

static int output_conflicts_with_inputs(const char *output_path, int argc, char **argv, int argi) {
    int i;

    if (output_path == 0) {
        return 0;
    }

    for (i = argi; i < argc; ++i) {
        if (rt_strcmp(output_path, argv[i]) == 0 || tool_paths_equal(output_path, argv[i])) {
            return 1;
        }
    }

    return 0;
}

static int sort_regular_inputs(int argc, char **argv, int argi, const SortOptions *options) {
    SortCollection collection;
    SortRunSet runs;
    int exit_code = 0;
    int output_fd = 1;
    int close_output = 0;
    int i;

    if (sort_collection_init(&collection) != 0) {
        rt_write_line(2, "sort: input too large for available memory");
        return 1;
    }
    sort_run_set_init(&runs);

    if (argi == argc) {
        int collect_status = collect_external_from_fd(0, &collection, options, &runs);
        if (collect_status == SORT_COLLECT_READ_ERROR) {
            rt_write_line(2, "sort: read error");
            sort_run_set_cleanup(&runs);
            sort_collection_free(&collection);
            return 1;
        }
        if (collect_status == SORT_COLLECT_MEMORY_ERROR) {
            rt_write_line(2, "sort: input too large for available memory");
            sort_run_set_cleanup(&runs);
            sort_collection_free(&collection);
            return 1;
        }
    } else {
        for (i = argi; i < argc; ++i) {
            int fd;
            int should_close;
            int collect_status;

            if (tool_open_input(argv[i], &fd, &should_close) != 0) {
                rt_write_cstr(2, "sort: cannot open ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
                continue;
            }

            collect_status = collect_external_from_fd(fd, &collection, options, &runs);
            if (collect_status == SORT_COLLECT_READ_ERROR) {
                rt_write_cstr(2, "sort: read error on ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
            } else if (collect_status == SORT_COLLECT_MEMORY_ERROR) {
                rt_write_cstr(2, "sort: input too large while reading ");
                rt_write_line(2, argv[i]);
                tool_close_input(fd, should_close);
                sort_run_set_cleanup(&runs);
                sort_collection_free(&collection);
                return 1;
            }

            tool_close_input(fd, should_close);
        }
    }

    if (runs.count > 0U) {
        if (flush_sort_run(&collection, options, &runs) != 0) {
            sort_run_set_cleanup(&runs);
            sort_collection_free(&collection);
            return 1;
        }
    }

    if (options->output_path != 0) {
        output_fd = platform_open_write(options->output_path, 0644U);
        if (output_fd < 0) {
            rt_write_cstr(2, "sort: cannot open output ");
            rt_write_line(2, options->output_path);
            sort_run_set_cleanup(&runs);
            sort_collection_free(&collection);
            return 1;
        }
        close_output = 1;
    }

    if (runs.count > 0U) {
        if (merge_run_set_to_output(&runs, output_fd, options) != 0) {
            if (close_output) {
                (void)platform_close(output_fd);
            }
            sort_run_set_cleanup(&runs);
            sort_collection_free(&collection);
            return 1;
        }
    } else {
        sort_collection_prepare_order(&collection);
        sort_lines(&collection, options);
        if (write_sorted_output(output_fd, &collection, options) != 0) {
            rt_write_line(2, "sort: write error");
            if (close_output) {
                (void)platform_close(output_fd);
            }
            sort_collection_free(&collection);
            return 1;
        }
    }

    if (close_output && platform_close(output_fd) != 0) {
        rt_write_line(2, "sort: write error");
        sort_run_set_cleanup(&runs);
        sort_collection_free(&collection);
        return 1;
    }

    sort_run_set_cleanup(&runs);
    sort_collection_free(&collection);
    return exit_code;
}

int main(int argc, char **argv) {
    int exit_code = 0;
    int argi = 1;
    SortOptions options;
    int output_fd = 1;
    int close_output = 0;

    rt_memset(&options, 0, sizeof(options));

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) {
            write_usage(1);
            return 0;
        }
        if (rt_strcmp(argv[argi], "--dictionary-order") == 0) {
            options.dictionary_order = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--ignore-nonprinting") == 0) {
            options.ignore_nonprinting = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--human-numeric-sort") == 0) {
            options.sort_mode = SORT_MODE_HUMAN_NUMERIC;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--month-sort") == 0) {
            options.sort_mode = SORT_MODE_MONTH;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "--version-sort") == 0) {
            options.sort_mode = SORT_MODE_VERSION;
            argi += 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-k") == 0 || (argv[argi][1] == 'k' && argv[argi][2] != '\0')) {
            const char *value = (rt_strcmp(argv[argi], "-k") == 0) ? ((argi + 1 < argc) ? argv[argi + 1] : 0) : argv[argi] + 2;
            if (parse_key_spec(value, &options) != 0) {
                write_usage(2);
                return 1;
            }
            argi += (rt_strcmp(argv[argi], "-k") == 0) ? 2 : 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-t") == 0 || (argv[argi][1] == 't' && argv[argi][2] != '\0')) {
            const char *value = (rt_strcmp(argv[argi], "-t") == 0) ? ((argi + 1 < argc) ? argv[argi + 1] : 0) : argv[argi] + 2;
            if (value == 0 || value[0] == '\0' || value[1] != '\0') {
                write_usage(2);
                return 1;
            }
            options.have_separator = 1;
            options.separator = value[0];
            argi += (rt_strcmp(argv[argi], "-t") == 0) ? 2 : 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-o") == 0 || (argv[argi][1] == 'o' && argv[argi][2] != '\0')) {
            options.output_path = (rt_strcmp(argv[argi], "-o") == 0) ? ((argi + 1 < argc) ? argv[argi + 1] : 0) : argv[argi] + 2;
            if (options.output_path == 0 || options.output_path[0] == '\0') {
                write_usage(2);
                return 1;
            }
            argi += (rt_strcmp(argv[argi], "-o") == 0) ? 2 : 1;
            continue;
        }

        flag = argv[argi] + 1;
        while (*flag != '\0') {
            if (*flag == 'b') {
                options.ignore_leading_blanks = 1;
            } else if (*flag == 'c') {
                options.check_only = 1;
            } else if (*flag == 'C') {
                options.check_only = 1;
                options.quiet_check = 1;
            } else if (*flag == 'd') {
                options.dictionary_order = 1;
            } else if (*flag == 'f') {
                options.ignore_case = 1;
            } else if (*flag == 'i') {
                options.ignore_nonprinting = 1;
            } else if (*flag == 'M') {
                options.sort_mode = SORT_MODE_MONTH;
            } else if (*flag == 'm') {
                options.merge_mode = 1;
            } else if (*flag == 'n') {
                options.sort_mode = SORT_MODE_NUMERIC;
            } else if (*flag == 'r') {
                options.reverse = 1;
            } else if (*flag == 's') {
                /* Stable ordering is already preserved by the current merge sort. */
            } else if (*flag == 'u') {
                options.unique = 1;
            } else if (*flag == 'V') {
                options.sort_mode = SORT_MODE_VERSION;
            } else {
                write_usage(2);
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (options.check_only) {
        return check_input_sorted(argc, argv, argi, &options);
    }

    if (options.merge_mode && !output_conflicts_with_inputs(options.output_path, argc, argv, argi)) {
        if (options.output_path != 0) {
            output_fd = platform_open_write(options.output_path, 0644U);
            if (output_fd < 0) {
                rt_write_cstr(2, "sort: cannot open output ");
                rt_write_line(2, options.output_path);
                return 1;
            }
            close_output = 1;
        }
        exit_code = merge_sorted_inputs(argc, argv, argi, output_fd, &options, &exit_code);
        if (close_output) {
            (void)platform_close(output_fd);
        }
        return exit_code;
    }

    return sort_regular_inputs(argc, argv, argi, &options);
}
