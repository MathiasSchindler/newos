#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
#include <stdlib.h>
#define SORT_HOSTED_DYNAMIC 1
#else
#define SORT_HOSTED_DYNAMIC 0
#endif

#include <limits.h>

#define SORT_FREESTANDING_MAX_LINES 8192U
#define SORT_FREESTANDING_MAX_INPUTS 8U
#define SORT_FREESTANDING_STORAGE_CAPACITY (2U * 1024U * 1024U)
#define SORT_FREESTANDING_LINE_CAPACITY (64U * 1024U)

#define SORT_COLLECT_READ_ERROR (-1)
#define SORT_COLLECT_MEMORY_ERROR (-2)

typedef struct {
    int check_only;
    int quiet_check;
    int ignore_case;
    int ignore_leading_blanks;
    int numeric;
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
#if !SORT_HOSTED_DYNAMIC
    char fixed_data[SORT_FREESTANDING_LINE_CAPACITY];
#endif
} SortLineBuilder;

typedef struct {
    int fd;
    int should_close;
    int finished;
    int have_current;
    char buffer[2048];
    size_t buffer_pos;
    size_t buffer_len;
    SortLineBuilder builder;
    SortLine current;
} SortInput;

typedef struct {
    SortLine *lines;
    SortLine **order;
    SortLine **scratch;
    size_t count;
    size_t capacity;
#if !SORT_HOSTED_DYNAMIC
    SortLine fixed_lines[SORT_FREESTANDING_MAX_LINES];
    SortLine *fixed_order[SORT_FREESTANDING_MAX_LINES];
    SortLine *fixed_scratch[SORT_FREESTANDING_MAX_LINES];
    char fixed_storage[SORT_FREESTANDING_STORAGE_CAPACITY];
    size_t storage_used;
#endif
} SortCollection;

typedef struct {
    int valid;
    int negative;
    const char *int_digits;
    size_t int_len;
    const char *frac_digits;
    size_t frac_len;
} SortNumericKey;

static void write_usage(int fd) {
    rt_write_line(fd, "Usage: sort [-bCcfmnrsu] [-o FILE] [-t CHAR] [-k FIELD[,FIELD]] [file ...]");
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
#if !SORT_HOSTED_DYNAMIC
    builder->data = builder->fixed_data;
    builder->capacity = sizeof(builder->fixed_data);
#endif
}

static void line_builder_reset(SortLineBuilder *builder) {
    builder->length = 0U;
}

static int line_builder_ensure_capacity(SortLineBuilder *builder, size_t needed) {
#if SORT_HOSTED_DYNAMIC
    size_t new_capacity;
    char *resized;

    if (needed <= builder->capacity) {
        return 0;
    }

    new_capacity = builder->capacity > 0U ? builder->capacity : 256U;
    while (new_capacity < needed) {
        if (new_capacity > ((size_t)-1) / 2U) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2U;
    }

    resized = (char *)realloc(builder->data, new_capacity);
    if (resized == 0) {
        return -1;
    }

    builder->data = resized;
    builder->capacity = new_capacity;
    return 0;
#else
    (void)builder;
    return needed <= SORT_FREESTANDING_LINE_CAPACITY ? 0 : -1;
#endif
}

static int line_builder_append_char(SortLineBuilder *builder, char ch) {
    if (line_builder_ensure_capacity(builder, builder->length + 2U) != 0) {
        return -1;
    }

    builder->data[builder->length++] = ch;
    builder->data[builder->length] = '\0';
    return 0;
}

static void line_builder_free(SortLineBuilder *builder) {
#if SORT_HOSTED_DYNAMIC
    free(builder->data);
#else
    (void)builder;
#endif
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
            char ch = input->buffer[input->buffer_pos++];

            if (ch == '\n') {
                input->current.text = input->builder.data != 0 ? input->builder.data : "";
                input->current.length = input->builder.length;
                input->have_current = 1;
                return 1;
            }
            if (line_builder_append_char(&input->builder, ch) != 0) {
                input->have_current = 0;
                return SORT_COLLECT_MEMORY_ERROR;
            }
        }
    }
}

static void sort_collection_init(SortCollection *collection) {
    rt_memset(collection, 0, sizeof(*collection));
#if !SORT_HOSTED_DYNAMIC
    collection->lines = collection->fixed_lines;
    collection->order = collection->fixed_order;
    collection->scratch = collection->fixed_scratch;
    collection->capacity = SORT_FREESTANDING_MAX_LINES;
#endif
}

static int sort_collection_reserve(SortCollection *collection, size_t needed) {
#if SORT_HOSTED_DYNAMIC
    size_t new_capacity;
    SortLine *new_lines;
    SortLine **new_order;
    SortLine **new_scratch;

    if (needed <= collection->capacity) {
        return 0;
    }

    new_capacity = collection->capacity > 0U ? collection->capacity : 256U;
    while (new_capacity < needed) {
        if (new_capacity > ((size_t)-1) / 2U) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2U;
    }

    new_lines = (SortLine *)realloc(collection->lines, new_capacity * sizeof(SortLine));
    if (new_lines == 0) {
        return -1;
    }

    new_order = (SortLine **)realloc(collection->order, new_capacity * sizeof(SortLine *));
    if (new_order == 0) {
        collection->lines = new_lines;
        return -1;
    }

    new_scratch = (SortLine **)realloc(collection->scratch, new_capacity * sizeof(SortLine *));
    if (new_scratch == 0) {
        collection->lines = new_lines;
        collection->order = new_order;
        return -1;
    }

    collection->lines = new_lines;
    collection->order = new_order;
    collection->scratch = new_scratch;
    collection->capacity = new_capacity;
    return 0;
#else
    return needed <= collection->capacity ? 0 : -1;
#endif
}

static int sort_collection_store_line(SortCollection *collection, const char *text, size_t length) {
    char *stored_text;

    if (sort_collection_reserve(collection, collection->count + 1U) != 0) {
        return -1;
    }

#if SORT_HOSTED_DYNAMIC
    stored_text = (char *)malloc(length + 1U);
    if (stored_text == 0) {
        return -1;
    }
#else
    if (collection->storage_used + length + 1U > sizeof(collection->fixed_storage)) {
        return -1;
    }
    stored_text = collection->fixed_storage + collection->storage_used;
    collection->storage_used += length + 1U;
#endif

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
#if SORT_HOSTED_DYNAMIC
    size_t i;

    for (i = 0; i < collection->count; ++i) {
        free(collection->lines[i].text);
    }
    free(collection->lines);
    free(collection->order);
    free(collection->scratch);
#else
    (void)collection;
#endif
}

static int collect_lines_from_fd(int fd, SortCollection *collection) {
    char buffer[2048];
    SortLineBuilder builder;
    long bytes_read;

    line_builder_init(&builder);

    while ((bytes_read = platform_read(fd, buffer, sizeof(buffer))) > 0) {
        long i;

        for (i = 0; i < bytes_read; ++i) {
            if (buffer[i] == '\n') {
                if (sort_collection_store_line(collection, builder.data != 0 ? builder.data : "", builder.length) != 0) {
                    line_builder_free(&builder);
                    return SORT_COLLECT_MEMORY_ERROR;
                }
                line_builder_reset(&builder);
            } else if (line_builder_append_char(&builder, buffer[i]) != 0) {
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
        if (sort_collection_store_line(collection, builder.data != 0 ? builder.data : "", builder.length) != 0) {
            line_builder_free(&builder);
            return SORT_COLLECT_MEMORY_ERROR;
        }
    }

    line_builder_free(&builder);
    return 0;
}

static int is_sort_space(char ch) {
    return ch == ' ' || ch == '\t';
}

static char sort_fold_ascii(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static void sort_trim_leading_blanks(const char **text, size_t *length) {
    while (*length > 0U && is_sort_space((*text)[0])) {
        *text += 1;
        *length -= 1U;
    }
}

static int compare_text_spans(const char *left,
                              size_t left_len,
                              const char *right,
                              size_t right_len,
                              const SortOptions *options) {
    size_t i = 0U;

    while (i < left_len && i < right_len) {
        unsigned char lhs = (unsigned char)(options->ignore_case ? sort_fold_ascii(left[i]) : left[i]);
        unsigned char rhs = (unsigned char)(options->ignore_case ? sort_fold_ascii(right[i]) : right[i]);

        if (lhs != rhs) {
            return lhs < rhs ? -1 : 1;
        }
        i += 1U;
    }

    if (left_len < right_len) {
        return -1;
    }
    if (left_len > right_len) {
        return 1;
    }
    return 0;
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
            while (i < line->length && is_sort_space(line->text[i])) {
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

            while (i < line->length && !is_sort_space(line->text[i])) {
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

    rt_memset(key, 0, sizeof(*key));

    while (index < length && is_sort_space(text[index])) {
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

    while (index < length && is_sort_space(text[index])) {
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

static int compare_lines(const SortLine *left, const SortLine *right, const SortOptions *options) {
    const char *left_text = left->text;
    const char *right_text = right->text;
    size_t left_len = left->length;
    size_t right_len = right->length;
    int result = 0;

    extract_key_span(left, options, &left_text, &left_len);
    extract_key_span(right, options, &right_text, &right_len);

    if (options->numeric) {
        SortNumericKey left_numeric;
        SortNumericKey right_numeric;

        parse_numeric_key(left_text, left_len, &left_numeric);
        parse_numeric_key(right_text, right_len, &right_numeric);

        if (left_numeric.valid && right_numeric.valid) {
            result = compare_numeric_keys(&left_numeric, &right_numeric);
        }
    }

    if (result == 0) {
        result = compare_text_spans(left_text, left_len, right_text, right_len, options);
    }

    return options->reverse ? -result : result;
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

static void sort_lines(SortCollection *collection, const SortOptions *options) {
    if (collection->count < 2U) {
        return;
    }

    merge_sort_lines(collection->order, collection->scratch, 0U, collection->count, options);
}

static int write_sorted_output(int fd, const SortCollection *collection, const SortOptions *options) {
    size_t i;
    SortLine *previous = 0;

    for (i = 0U; i < collection->count; ++i) {
        SortLine *line = collection->order[i];

        if (options->unique && previous != 0 && compare_lines(previous, line, options) == 0) {
            continue;
        }

        if ((line->length > 0U && rt_write_all(fd, line->text, line->length) != 0) ||
            rt_write_char(fd, '\n') != 0) {
            return -1;
        }

        previous = line;
    }

    return 0;
}

static int write_sort_line(int fd, const SortLine *line) {
    if ((line->length > 0U && rt_write_all(fd, line->text, line->length) != 0) ||
        rt_write_char(fd, '\n') != 0) {
        return -1;
    }
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
#if SORT_HOSTED_DYNAMIC
    SortInput *inputs = 0;
#else
    SortInput fixed_inputs[SORT_FREESTANDING_MAX_INPUTS];
    SortInput *inputs = fixed_inputs;
#endif
    int have_previous = 0;

    rt_memset(&previous_line, 0, sizeof(previous_line));
    line_builder_init(&previous_builder);

#if SORT_HOSTED_DYNAMIC
    inputs = (SortInput *)calloc((size_t)input_count, sizeof(SortInput));
    if (inputs == 0) {
        line_builder_free(&previous_builder);
        rt_write_line(2, "sort: input too large for available memory");
        return 1;
    }
#else
    if ((size_t)input_count > SORT_FREESTANDING_MAX_INPUTS) {
        line_builder_free(&previous_builder);
        rt_write_line(2, "sort: too many inputs for merge mode");
        return 1;
    }
#endif

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
#if SORT_HOSTED_DYNAMIC
            free(inputs);
#endif
            return 1;
        }
        if (status == SORT_COLLECT_MEMORY_ERROR) {
            rt_write_cstr(2, "sort: input too large while reading ");
            rt_write_line(2, path);
            sort_input_close(&inputs[i]);
            line_builder_free(&previous_builder);
#if SORT_HOSTED_DYNAMIC
            free(inputs);
#endif
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

        if (!options->unique || !have_previous || compare_lines(&previous_line, &inputs[best_index].current, options) != 0) {
            if (write_sort_line(output_fd, &inputs[best_index].current) != 0) {
                rt_write_line(2, "sort: write error");
                line_builder_free(&previous_builder);
                for (i = 0; i < input_count; ++i) {
                    sort_input_close(&inputs[i]);
                }
#if SORT_HOSTED_DYNAMIC
                free(inputs);
#endif
                return 1;
            }
            if (copy_sort_line(&previous_builder, &previous_line, &inputs[best_index].current) != 0) {
                rt_write_line(2, "sort: input too large for available memory");
                line_builder_free(&previous_builder);
                for (i = 0; i < input_count; ++i) {
                    sort_input_close(&inputs[i]);
                }
#if SORT_HOSTED_DYNAMIC
                free(inputs);
#endif
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
#if SORT_HOSTED_DYNAMIC
                    free(inputs);
#endif
                    return 1;
                }
                if (status == SORT_COLLECT_MEMORY_ERROR) {
                    rt_write_cstr(2, "sort: input too large while reading ");
                    rt_write_line(2, (argi == argc) ? "-" : argv[argi + best_index]);
                    line_builder_free(&previous_builder);
                    for (i = 0; i < input_count; ++i) {
                        sort_input_close(&inputs[i]);
                    }
#if SORT_HOSTED_DYNAMIC
                    free(inputs);
#endif
                    return 1;
                }
                inputs[best_index].have_current = 0;
                active_count -= 1;
            }
        }
    }

    line_builder_free(&previous_builder);
    for (i = 0; i < input_count; ++i) {
        sort_input_close(&inputs[i]);
    }
#if SORT_HOSTED_DYNAMIC
    free(inputs);
#endif
    return *exit_code;
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

int main(int argc, char **argv) {
    SortCollection collection;
    int exit_code = 0;
    int argi = 1;
    int i;
    SortOptions options;
    int output_fd = 1;
    int close_output = 0;

    rt_memset(&options, 0, sizeof(options));
    sort_collection_init(&collection);

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        const char *flag;

        if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        }
        if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) {
            write_usage(1);
            sort_collection_free(&collection);
            return 0;
        }
        if (rt_strcmp(argv[argi], "-k") == 0 || (argv[argi][1] == 'k' && argv[argi][2] != '\0')) {
            const char *value = (rt_strcmp(argv[argi], "-k") == 0) ? ((argi + 1 < argc) ? argv[argi + 1] : 0) : argv[argi] + 2;
            if (parse_key_spec(value, &options) != 0) {
                write_usage(2);
                sort_collection_free(&collection);
                return 1;
            }
            argi += (rt_strcmp(argv[argi], "-k") == 0) ? 2 : 1;
            continue;
        }
        if (rt_strcmp(argv[argi], "-t") == 0 || (argv[argi][1] == 't' && argv[argi][2] != '\0')) {
            const char *value = (rt_strcmp(argv[argi], "-t") == 0) ? ((argi + 1 < argc) ? argv[argi + 1] : 0) : argv[argi] + 2;
            if (value == 0 || value[0] == '\0' || value[1] != '\0') {
                write_usage(2);
                sort_collection_free(&collection);
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
                sort_collection_free(&collection);
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
            } else if (*flag == 'f') {
                options.ignore_case = 1;
            } else if (*flag == 'm') {
                options.merge_mode = 1;
            } else if (*flag == 'n') {
                options.numeric = 1;
            } else if (*flag == 'r') {
                options.reverse = 1;
            } else if (*flag == 's') {
                /* Stable ordering is already preserved by the current merge sort. */
            } else if (*flag == 'u') {
                options.unique = 1;
            } else {
                write_usage(2);
                sort_collection_free(&collection);
                return 1;
            }
            flag += 1;
        }

        argi += 1;
    }

    if (options.check_only) {
        sort_collection_free(&collection);
        return check_input_sorted(argc, argv, argi, &options);
    }

    if (options.merge_mode && !output_conflicts_with_inputs(options.output_path, argc, argv, argi)) {
        if (options.output_path != 0) {
            output_fd = platform_open_write(options.output_path, 0644U);
            if (output_fd < 0) {
                rt_write_cstr(2, "sort: cannot open output ");
                rt_write_line(2, options.output_path);
                sort_collection_free(&collection);
                return 1;
            }
            close_output = 1;
        }
        exit_code = merge_sorted_inputs(argc, argv, argi, output_fd, &options, &exit_code);
        if (close_output) {
            (void)platform_close(output_fd);
        }
        sort_collection_free(&collection);
        return exit_code;
    }

    if (argi == argc) {
        int collect_status = collect_lines_from_fd(0, &collection);
        if (collect_status == SORT_COLLECT_READ_ERROR) {
            rt_write_line(2, "sort: read error");
            sort_collection_free(&collection);
            return 1;
        }
        if (collect_status == SORT_COLLECT_MEMORY_ERROR) {
            rt_write_line(2, "sort: input too large for available memory");
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

            collect_status = collect_lines_from_fd(fd, &collection);
            if (collect_status == SORT_COLLECT_READ_ERROR) {
                rt_write_cstr(2, "sort: read error on ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
            } else if (collect_status == SORT_COLLECT_MEMORY_ERROR) {
                rt_write_cstr(2, "sort: input too large while reading ");
                rt_write_line(2, argv[i]);
                tool_close_input(fd, should_close);
                sort_collection_free(&collection);
                return 1;
            }

            tool_close_input(fd, should_close);
        }
    }

    for (i = 0; i < (int)collection.count; ++i) {
        collection.order[i] = &collection.lines[i];
    }

    sort_lines(&collection, &options);

    if (options.output_path != 0) {
        output_fd = platform_open_write(options.output_path, 0644U);
        if (output_fd < 0) {
            rt_write_cstr(2, "sort: cannot open output ");
            rt_write_line(2, options.output_path);
            sort_collection_free(&collection);
            return 1;
        }
        close_output = 1;
    }

    if (write_sorted_output(output_fd, &collection, &options) != 0) {
        rt_write_line(2, "sort: write error");
        if (close_output) {
            (void)platform_close(output_fd);
        }
        sort_collection_free(&collection);
        return 1;
    }

    if (close_output) {
        (void)platform_close(output_fd);
    }
    sort_collection_free(&collection);
    return exit_code;
}
