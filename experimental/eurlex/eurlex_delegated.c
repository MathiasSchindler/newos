#include "archive_zip.h"
#include "concurrency.h"
#include "platform.h"
#include "runtime.h"

#define EURLEX_DEFAULT_MTD_ARCHIVE "experimental/eurlex/data/LEG_MTD_20260628_01_00.zip"
#define EURLEX_LOCAL_MTD_ARCHIVE "data/LEG_MTD_20260628_01_00.zip"
#define EURLEX_MAX_ENTRY_SIZE (64ULL * 1024ULL * 1024ULL)
#define EURLEX_MAX_TARGETS 64
#define EURLEX_MAX_BASIS 64
#define EURLEX_CELEX_CAPACITY 96
#define EURLEX_TYPE_CAPACITY 64
#define EURLEX_TITLE_CAPACITY 1024
#define EURLEX_INDEX_LINE_CAPACITY 4096
#define EURLEX_DEFAULT_MAX_WORKERS 8U

typedef struct {
    char celex[EURLEX_CELEX_CAPACITY];
} EurlexTarget;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
    int failed;
} EurlexOutputBuffer;

typedef struct {
    ArchiveZipEntry entry;
    char *name;
    EurlexOutputBuffer output;
    unsigned long long match_count;
    int failed;
} EurlexWorkEntry;

typedef struct {
    EurlexWorkEntry *entries;
    size_t count;
    size_t capacity;
    int failed;
} EurlexEntryList;

typedef struct {
    char celex[EURLEX_CELEX_CAPACITY];
    char resource_type[EURLEX_TYPE_CAPACITY];
    char title[EURLEX_TITLE_CAPACITY];
    int title_priority;
    int delegated;
    char basis_celexes[EURLEX_MAX_BASIS][EURLEX_CELEX_CAPACITY];
    int basis_count;
    int basis_matches[EURLEX_MAX_TARGETS];
    int reported_matches[EURLEX_MAX_TARGETS];
} EurlexLegalResource;

typedef struct {
    const EurlexTarget *targets;
    int target_count;
    const char *entry_name;
    EurlexLegalResource current;
    unsigned long long *match_count;
    int output_fd;
    EurlexOutputBuffer *output_buffer;
} EurlexParseContext;

typedef struct {
    int fd;
    const char *archive_path;
    const ArchiveZipInfo *info;
    const EurlexTarget *targets;
    int target_count;
    unsigned long long match_count;
    int failed;
    int build_index;
    int output_fd;
    EurlexOutputBuffer *output_buffer;
} EurlexScanContext;

typedef struct {
    const char *archive_path;
    const ArchiveZipInfo *info;
    const EurlexTarget *targets;
    int target_count;
    int build_index;
    EurlexWorkEntry *entries;
    int worker_fds[RT_TASK_POOL_MAX_WORKERS];
} EurlexParallelScan;

static void print_usage(void) {
    rt_write_line(2, "usage: eurlex-delegated [-a METADATA.zip] CELEX|REGULATION-CITATION ...");
    rt_write_line(2, "       eurlex-delegated -i INDEX.tsv CELEX|REGULATION-CITATION ...");
    rt_write_line(2, "       eurlex-delegated [-a METADATA.zip] --build-index INDEX.tsv [CELEX|REGULATION-CITATION ...]");
}

static int ascii_hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

static char ascii_upper_char(char ch) {
    if (ch >= 'a' && ch <= 'z') return (char)(ch - 'a' + 'A');
    return ch;
}

static int ascii_is_digit(char ch) {
    return ch >= '0' && ch <= '9';
}

static size_t slice_find_last(const char *text, size_t length, const char *needle) {
    size_t needle_length = rt_strlen(needle);
    size_t i;

    if (needle_length == 0U || needle_length > length) return length;
    i = length - needle_length + 1U;
    while (i > 0U) {
        size_t j;

        i -= 1U;
        if (text[i] != needle[0]) continue;
        for (j = 1U; j < needle_length; ++j) {
            if (text[i + j] != needle[j]) break;
        }
        if (j == needle_length) return i;
    }
    return length;
}

static size_t slice_find_from(const char *text, size_t length, const char *needle, size_t offset) {
    size_t needle_length = rt_strlen(needle);
    size_t i;

    if (needle_length == 0U || needle_length > length || offset > length - needle_length) return length;
    for (i = offset; i + needle_length <= length; ++i) {
        size_t j;

        if (text[i] != needle[0]) continue;
        for (j = 1U; j < needle_length; ++j) {
            if (text[i + j] != needle[j]) break;
        }
        if (j == needle_length) return i;
    }
    return length;
}

static int normalize_celex_slice(const char *text, size_t length, char *buffer, size_t buffer_size) {
    size_t start = 0U;
    size_t end = length;
    size_t pos;
    size_t out = 0U;

    while (start < end && (text[start] == ' ' || text[start] == '\t' || text[start] == '\n' || text[start] == '\r')) start += 1U;
    while (end > start && (text[end - 1U] == ' ' || text[end - 1U] == '\t' || text[end - 1U] == '\n' || text[end - 1U] == '\r')) end -= 1U;

    pos = slice_find_last(text + start, end - start, "/celex/");
    if (pos != end - start) {
        start += pos + 7U;
    } else {
        pos = slice_find_last(text + start, end - start, "celex:");
        if (pos != end - start) start += pos + 6U;
    }

    if (start >= end || buffer_size == 0U) return -1;
    while (start < end) {
        char ch = text[start];
        if (ch == '?' || ch == '#') break;
        if (ch == '%' && start + 2U < end) {
            int hi = ascii_hex_value(text[start + 1U]);
            int lo = ascii_hex_value(text[start + 2U]);
            if (hi >= 0 && lo >= 0) {
                ch = (char)((hi << 4) | lo);
                start += 3U;
            } else {
                start += 1U;
            }
        } else {
            start += 1U;
        }
        if (out + 1U >= buffer_size) return -1;
        buffer[out++] = ascii_upper_char(ch);
    }
    if (out == 0U) return -1;
    buffer[out] = '\0';
    return 0;
}

static int normalize_celex_cstr(const char *text, char *buffer, size_t buffer_size) {
    return normalize_celex_slice(text, rt_strlen(text), buffer, buffer_size);
}

static int normalize_regulation_citation_slice(const char *text, size_t length, char *buffer, size_t buffer_size) {
    size_t i;

    if (buffer_size < 11U) return -1;
    for (i = 0U; i + 5U < length; ++i) {
        size_t slash;
        size_t number_start;
        size_t number_end;
        size_t digit_count;
        size_t out = 0U;

        if (!ascii_is_digit(text[i]) || !ascii_is_digit(text[i + 1U]) || !ascii_is_digit(text[i + 2U]) || !ascii_is_digit(text[i + 3U])) continue;
        slash = i + 4U;
        while (slash < length && (text[slash] == ' ' || text[slash] == '\t')) slash += 1U;
        if (slash >= length || text[slash] != '/') continue;
        number_start = slash + 1U;
        while (number_start < length && (text[number_start] == ' ' || text[number_start] == '\t')) number_start += 1U;
        number_end = number_start;
        while (number_end < length && ascii_is_digit(text[number_end])) number_end += 1U;
        digit_count = number_end - number_start;
        if (digit_count == 0U || digit_count > 5U || digit_count + 7U >= buffer_size) continue;

        buffer[out++] = '3';
        buffer[out++] = text[i];
        buffer[out++] = text[i + 1U];
        buffer[out++] = text[i + 2U];
        buffer[out++] = text[i + 3U];
        buffer[out++] = 'R';
        if (digit_count < 4U) {
            size_t pad;
            for (pad = digit_count; pad < 4U; ++pad) buffer[out++] = '0';
        }
        while (number_start < number_end) buffer[out++] = text[number_start++];
        buffer[out] = '\0';
        return 0;
    }
    return -1;
}

static int normalize_target_cstr(const char *text, char *buffer, size_t buffer_size) {
    if (normalize_regulation_citation_slice(text, rt_strlen(text), buffer, buffer_size) == 0) return 0;
    return normalize_celex_cstr(text, buffer, buffer_size);
}

static int slice_ends_with(const char *text, size_t length, const char *suffix) {
    size_t suffix_length = rt_strlen(suffix);
    size_t offset;
    size_t i;

    if (suffix_length > length) return 0;
    offset = length - suffix_length;
    for (i = 0U; i < suffix_length; ++i) {
        if (text[offset + i] != suffix[i]) return 0;
    }
    return 1;
}

static int cstr_ends_with(const char *text, const char *suffix) {
    return slice_ends_with(text, rt_strlen(text), suffix);
}

static void copy_resource_leaf(const char *text, size_t length, char *buffer, size_t buffer_size) {
    size_t start = length;
    size_t out = 0U;
    size_t i;

    while (start > 0U && text[start - 1U] != '/' && text[start - 1U] != '#') start -= 1U;
    if (buffer_size == 0U) return;
    for (i = start; i < length && out + 1U < buffer_size; ++i) {
        buffer[out++] = text[i];
    }
    buffer[out] = '\0';
}

static int target_index_for_resource(const EurlexParseContext *context, const char *text, size_t length) {
    char celex[EURLEX_CELEX_CAPACITY];
    int i;

    if (normalize_celex_slice(text, length, celex, sizeof(celex)) != 0) return -1;
    for (i = 0; i < context->target_count; ++i) {
        if (rt_strcmp(celex, context->targets[i].celex) == 0) return i;
    }
    return -1;
}

static int add_basis_celex(EurlexLegalResource *resource, const char *text, size_t length) {
    char celex[EURLEX_CELEX_CAPACITY];
    int i;

    if (normalize_celex_slice(text, length, celex, sizeof(celex)) != 0) return -1;
    for (i = 0; i < resource->basis_count; ++i) {
        if (rt_strcmp(resource->basis_celexes[i], celex) == 0) return 0;
    }
    if (resource->basis_count >= EURLEX_MAX_BASIS) return -1;
    rt_copy_string(resource->basis_celexes[resource->basis_count], sizeof(resource->basis_celexes[resource->basis_count]), celex);
    resource->basis_count += 1;
    return 0;
}

static int entry_mentions_target(const unsigned char *data, size_t size, const EurlexScanContext *context) {
    const char *text = (const char *)data;
    int i;

    for (i = 0; i < context->target_count; ++i) {
        if (slice_find_from(text, size, context->targets[i].celex, 0U) != size) return 1;
    }
    return 0;
}

static void append_normalized_text(char *buffer, size_t buffer_size, const char *text, size_t length) {
    size_t out = rt_strlen(buffer);
    size_t i;
    int pending_space = out > 0U && buffer[out - 1U] != ' ';

    if (buffer_size == 0U) return;
    for (i = 0U; i < length && out + 1U < buffer_size; ++i) {
        char ch = text[i];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            pending_space = out > 0U;
            continue;
        }
        if (pending_space && out + 1U < buffer_size) {
            buffer[out++] = ' ';
        }
        pending_space = 0;
        buffer[out++] = ch;
    }
    while (out > 0U && buffer[out - 1U] == ' ') out -= 1U;
    buffer[out] = '\0';
}

static void reset_resource(EurlexLegalResource *resource) {
    rt_memset(resource, 0, sizeof(*resource));
}

static int output_buffer_append(EurlexOutputBuffer *buffer, const char *data, size_t size) {
    char *grown;
    size_t next_capacity;

    if (buffer == 0 || size == 0U) return 0;
    if (buffer->failed) return -1;
    if (size > (size_t)-1 - buffer->size) {
        buffer->failed = 1;
        return -1;
    }
    if (buffer->size + size > buffer->capacity) {
        next_capacity = buffer->capacity == 0U ? 1024U : buffer->capacity;
        while (next_capacity < buffer->size + size) {
            if (next_capacity > (size_t)-1 / 2U) {
                next_capacity = buffer->size + size;
                break;
            }
            next_capacity *= 2U;
        }
        grown = (char *)rt_realloc_array(buffer->data, next_capacity, sizeof(*buffer->data));
        if (grown == 0) {
            buffer->failed = 1;
            return -1;
        }
        buffer->data = grown;
        buffer->capacity = next_capacity;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 0;
}

static void output_buffer_release(EurlexOutputBuffer *buffer) {
    if (buffer == 0) return;
    rt_free(buffer->data);
    rt_memset(buffer, 0, sizeof(*buffer));
}

static void context_write_char(EurlexParseContext *context, char ch) {
    if (context->output_buffer != 0) {
        (void)output_buffer_append(context->output_buffer, &ch, 1U);
    } else {
        rt_write_char(context->output_fd, ch);
    }
}

static void context_write_tsv_field(EurlexParseContext *context, const char *text) {
    size_t i;
    for (i = 0U; text[i] != '\0'; ++i) {
        char ch = text[i];
        if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
        context_write_char(context, ch);
    }
}

static void write_index_header(int fd) {
    rt_write_line(fd, "target_celex\tdelegated_celex\tresource_type\ttitle\tmetadata_entry");
}

static void report_resource(EurlexParseContext *context) {
    int i;

    if (!context->current.delegated || context->current.celex[0] == '\0') return;
    if (context->target_count == 0) {
        for (i = 0; i < context->current.basis_count; ++i) {
            context_write_tsv_field(context, context->current.basis_celexes[i]);
            context_write_char(context, '\t');
            context_write_tsv_field(context, context->current.celex);
            context_write_char(context, '\t');
            context_write_tsv_field(context, context->current.resource_type[0] == '\0' ? "-" : context->current.resource_type);
            context_write_char(context, '\t');
            context_write_tsv_field(context, context->current.title[0] == '\0' ? "-" : context->current.title);
            context_write_char(context, '\t');
            context_write_tsv_field(context, context->entry_name);
            context_write_char(context, '\n');
            *context->match_count += 1ULL;
        }
        return;
    }
    for (i = 0; i < context->target_count; ++i) {
        if (!context->current.basis_matches[i]) continue;
        if (context->current.reported_matches[i]) continue;
        context_write_tsv_field(context, context->targets[i].celex);
        context_write_char(context, '\t');
        context_write_tsv_field(context, context->current.celex);
        context_write_char(context, '\t');
        context_write_tsv_field(context, context->current.resource_type[0] == '\0' ? "-" : context->current.resource_type);
        context_write_char(context, '\t');
        context_write_tsv_field(context, context->current.title[0] == '\0' ? "-" : context->current.title);
        context_write_char(context, '\t');
        context_write_tsv_field(context, context->entry_name);
        context_write_char(context, '\n');
        context->current.reported_matches[i] = 1;
        *context->match_count += 1ULL;
    }
}

static int line_matches_target(const char *line, size_t length, const EurlexTarget *targets, int target_count) {
    size_t field_length = 0U;
    int i;

    while (field_length < length && line[field_length] != '\t') field_length += 1U;
    if (field_length == length) return 0;
    for (i = 0; i < target_count; ++i) {
        size_t target_length = rt_strlen(targets[i].celex);
        size_t j;

        if (target_length != field_length) continue;
        for (j = 0U; j < field_length; ++j) {
            if (line[j] != targets[i].celex[j]) break;
        }
        if (j == field_length) return 1;
    }
    return 0;
}

static int query_index_file(const char *path, const EurlexTarget *targets, int target_count, unsigned long long *match_count_out) {
    int fd = platform_open_read(path);
    char read_buffer[8192];
    char line_buffer[EURLEX_INDEX_LINE_CAPACITY];
    size_t line_length = 0U;
    int truncated = 0;
    long bytes;

    *match_count_out = 0ULL;
    if (fd < 0) {
        rt_write_cstr(2, "eurlex-delegated: cannot open index: ");
        rt_write_line(2, path);
        return -1;
    }

    write_index_header(1);
    while ((bytes = platform_read(fd, read_buffer, sizeof(read_buffer))) > 0) {
        long i;

        for (i = 0; i < bytes; ++i) {
            char ch = read_buffer[i];
            if (ch == '\n') {
                if (!truncated && line_matches_target(line_buffer, line_length, targets, target_count)) {
                    (void)platform_write(1, line_buffer, line_length);
                    rt_write_char(1, '\n');
                    *match_count_out += 1ULL;
                }
                line_length = 0U;
                truncated = 0;
                continue;
            }
            if (line_length + 1U < sizeof(line_buffer)) {
                line_buffer[line_length++] = ch;
            } else {
                truncated = 1;
            }
        }
    }
    if (bytes < 0) {
        platform_close(fd);
        rt_write_cstr(2, "eurlex-delegated: cannot read index: ");
        rt_write_line(2, path);
        return -1;
    }
    if (line_length > 0U && !truncated && line_matches_target(line_buffer, line_length, targets, target_count)) {
        (void)platform_write(1, line_buffer, line_length);
        rt_write_char(1, '\n');
        *match_count_out += 1ULL;
    }
    platform_close(fd);
    return 0;
}

static int find_rdf_resource_attr(const char *text, size_t length, size_t start, size_t limit, const char **value_out, size_t *length_out) {
    size_t attr;
    size_t value_start;
    size_t value_end;

    if (limit > length) limit = length;
    attr = slice_find_from(text, limit, "rdf:resource=\"", start);
    if (attr == limit) return -1;
    value_start = attr + 14U;
    value_end = value_start;
    while (value_end < limit && text[value_end] != '\"') value_end += 1U;
    if (value_end >= limit) return -1;
    *value_out = text + value_start;
    *length_out = value_end - value_start;
    return 0;
}

static int direct_extract_element_text(const char *block, size_t block_length, const char *element, char *buffer, size_t buffer_size, int celex_only) {
    size_t pos = 0U;
    int best_priority = 0;

    while (pos < block_length) {
        size_t name = slice_find_from(block, block_length, element, pos);
        size_t tag_end;
        size_t text_start;
        size_t text_end;
        int priority = 1;

        if (name == block_length) break;
        tag_end = slice_find_from(block, block_length, ">", name);
        if (tag_end == block_length) break;
        text_start = tag_end + 1U;
        text_end = slice_find_from(block, block_length, "<", text_start);
        if (text_end == block_length) break;
        if (slice_find_from(block, tag_end, "xml:lang=\"en\"", name) != tag_end) priority = 3;
        else if (slice_find_from(block, tag_end, "xml:lang=", name) == tag_end) priority = 2;
        if ((!celex_only || slice_find_last(block + text_start, text_end - text_start, "celex:") != text_end - text_start) && priority > best_priority) {
            buffer[0] = '\0';
            if (celex_only) {
                normalize_celex_slice(block + text_start, text_end - text_start, buffer, buffer_size);
            } else {
                append_normalized_text(buffer, buffer_size, block + text_start, text_end - text_start);
            }
            best_priority = priority;
        }
        pos = text_end + 1U;
    }
    return buffer[0] == '\0' ? -1 : 0;
}

static void direct_scan_description(const char *block, size_t block_length, const char *entry_title, EurlexParseContext *context) {
    size_t pos = 0U;

    reset_resource(&context->current);
    while (pos < block_length) {
        size_t type_pos = slice_find_from(block, block_length, "work_has_resource-type", pos);
        size_t tag_end;
        const char *resource;
        size_t resource_length;
        char leaf[EURLEX_TYPE_CAPACITY];

        if (type_pos == block_length) break;
        tag_end = slice_find_from(block, block_length, ">", type_pos);
        if (tag_end == block_length) break;
        if (find_rdf_resource_attr(block, block_length, type_pos, tag_end, &resource, &resource_length) == 0) {
            copy_resource_leaf(resource, resource_length, leaf, sizeof(leaf));
            if (leaf[0] != '\0') rt_copy_string(context->current.resource_type, sizeof(context->current.resource_type), leaf);
            if (cstr_ends_with(leaf, "_DEL")) context->current.delegated = 1;
        }
        pos = tag_end + 1U;
    }
    if (!context->current.delegated) return;

    if (direct_extract_element_text(block, block_length, "resource_legal_id_celex", context->current.celex, sizeof(context->current.celex), 0) != 0) {
        direct_extract_element_text(block, block_length, "work_id_document", context->current.celex, sizeof(context->current.celex), 1);
    }
    direct_extract_element_text(block, block_length, "expression_title", context->current.title, sizeof(context->current.title), 0);
    if (context->current.title[0] == '\0') {
        direct_extract_element_text(block, block_length, "work_title", context->current.title, sizeof(context->current.title), 0);
    }
    if (context->current.title[0] == '\0' && entry_title[0] != '\0') {
        rt_copy_string(context->current.title, sizeof(context->current.title), entry_title);
    }

    pos = 0U;
    while (pos < block_length) {
        size_t rel_pos = slice_find_from(block, block_length, "resource_legal_based_on_resource_legal", pos);
        size_t amend_pos = slice_find_from(block, block_length, "resource_legal_amends_resource_legal", pos);
        size_t tag_end;
        const char *resource;
        size_t resource_length;
        int target_index;

        if (amend_pos < rel_pos) rel_pos = amend_pos;
        if (rel_pos == block_length) break;
        tag_end = slice_find_from(block, block_length, ">", rel_pos);
        if (tag_end == block_length) break;
        if (find_rdf_resource_attr(block, block_length, rel_pos, tag_end, &resource, &resource_length) == 0) {
            if (context->target_count == 0) {
                add_basis_celex(&context->current, resource, resource_length);
            } else {
                target_index = target_index_for_resource(context, resource, resource_length);
                if (target_index >= 0) context->current.basis_matches[target_index] = 1;
            }
        }
        pos = tag_end + 1U;
    }
    report_resource(context);
}

static void direct_scan_rdf_entry(const unsigned char *data, size_t size, const char *entry_name, EurlexScanContext *scan) {
    const char *text = (const char *)data;
    size_t pos = 0U;
    char entry_title[EURLEX_TITLE_CAPACITY];
    EurlexParseContext context;

    entry_title[0] = '\0';
    direct_extract_element_text(text, size, "expression_title", entry_title, sizeof(entry_title), 0);
    if (entry_title[0] == '\0') {
        direct_extract_element_text(text, size, "work_title", entry_title, sizeof(entry_title), 0);
    }

    rt_memset(&context, 0, sizeof(context));
    context.targets = scan->targets;
    context.target_count = scan->target_count;
    context.entry_name = entry_name;
    context.match_count = &scan->match_count;
    context.output_fd = scan->output_fd;
    context.output_buffer = scan->output_buffer;

    while (pos < size) {
        size_t begin = slice_find_from(text, size, "<rdf:Description", pos);
        size_t end;
        if (begin == size) break;
        end = slice_find_from(text, size, "</rdf:Description>", begin);
        if (end == size) break;
        end += 18U;
        direct_scan_description(text + begin, end - begin, entry_title, &context);
        pos = end;
    }
}

static int scan_zip_entry(const ArchiveZipEntry *entry, void *user_data) {
    EurlexScanContext *context = (EurlexScanContext *)user_data;
    unsigned char *data = 0;
    size_t data_size = 0U;

    if (!cstr_ends_with(entry->name, ".rdf")) return 0;
    if (archive_zip_read_entry_data(context->fd, context->info, entry, EURLEX_MAX_ENTRY_SIZE, &data, &data_size) != 0) {
        rt_write_cstr(2, "eurlex-delegated: cannot read ZIP entry: ");
        rt_write_line(2, entry->name);
        context->failed = 1;
        return 0;
    }
    if (context->build_index || entry_mentions_target(data, data_size, context)) {
        direct_scan_rdf_entry(data, data_size, entry->name, context);
    }
    rt_free(data);
    return 0;
}

static int collect_zip_entry(const ArchiveZipEntry *entry, void *user_data) {
    EurlexEntryList *list = (EurlexEntryList *)user_data;
    EurlexWorkEntry *grown;
    EurlexWorkEntry *work;
    size_t name_length;
    size_t next_capacity;

    if (!cstr_ends_with(entry->name, ".rdf")) return 0;
    if (list->count == list->capacity) {
        next_capacity = list->capacity == 0U ? 256U : list->capacity * 2U;
        grown = (EurlexWorkEntry *)rt_realloc_array(list->entries, next_capacity, sizeof(*list->entries));
        if (grown == 0) {
            list->failed = 1;
            return -1;
        }
        list->entries = grown;
        list->capacity = next_capacity;
    }
    work = &list->entries[list->count];
    rt_memset(work, 0, sizeof(*work));
    name_length = rt_strlen(entry->name);
    work->name = (char *)rt_malloc(name_length + 1U);
    if (work->name == 0) {
        list->failed = 1;
        return -1;
    }
    memcpy(work->name, entry->name, name_length + 1U);
    work->entry = *entry;
    work->entry.name = work->name;
    list->count += 1U;
    return 0;
}

static void release_entry_list(EurlexEntryList *list) {
    size_t index;

    if (list == 0) return;
    for (index = 0U; index < list->count; ++index) {
        output_buffer_release(&list->entries[index].output);
        rt_free(list->entries[index].name);
    }
    rt_free(list->entries);
    rt_memset(list, 0, sizeof(*list));
}

static int parallel_entry_mentions_target(const unsigned char *data, size_t size, const EurlexParallelScan *parallel) {
    const char *text = (const char *)data;
    int i;

    for (i = 0; i < parallel->target_count; ++i) {
        if (slice_find_from(text, size, parallel->targets[i].celex, 0U) != size) return 1;
    }
    return 0;
}

static int parallel_scan_range(size_t begin, size_t end, unsigned int worker_index, void *arg) {
    EurlexParallelScan *parallel = (EurlexParallelScan *)arg;
    int fd;
    size_t index;

    if (worker_index >= RT_TASK_POOL_MAX_WORKERS) return -1;
    fd = parallel->worker_fds[worker_index];
    if (fd < 0) {
        fd = platform_open_read(parallel->archive_path);
        if (fd < 0) return -1;
        parallel->worker_fds[worker_index] = fd;
    }
    for (index = begin; index < end; ++index) {
        EurlexWorkEntry *work = &parallel->entries[index];
        EurlexScanContext scan;
        unsigned char *data = 0;
        size_t data_size = 0U;

        if (archive_zip_read_entry_data(fd, parallel->info, &work->entry, EURLEX_MAX_ENTRY_SIZE, &data, &data_size) != 0) {
            work->failed = 1;
            return -1;
        }
        if (parallel->build_index || parallel_entry_mentions_target(data, data_size, parallel)) {
            rt_memset(&scan, 0, sizeof(scan));
            scan.fd = fd;
            scan.archive_path = parallel->archive_path;
            scan.info = parallel->info;
            scan.targets = parallel->targets;
            scan.target_count = parallel->target_count;
            scan.build_index = parallel->build_index;
            scan.output_buffer = &work->output;
            direct_scan_rdf_entry(data, data_size, work->name, &scan);
            work->match_count += scan.match_count;
            if (work->output.failed) {
                work->failed = 1;
                rt_free(data);
                return -1;
            }
        }
        rt_free(data);
    }
    return 0;
}

static unsigned int eurlex_worker_count_from_env(void) {
    const char *value_text = platform_getenv("NEWOS_EURLEX_WORKERS");
    unsigned long long value;
    unsigned int platform_width;

    if (value_text == 0 || value_text[0] == '\0') {
        platform_width = platform_worker_thread_count();
        if (platform_width == 0U) return 0U;
        return platform_width > EURLEX_DEFAULT_MAX_WORKERS ? EURLEX_DEFAULT_MAX_WORKERS : platform_width;
    }
    if (rt_parse_uint(value_text, &value) != 0) {
        return EURLEX_DEFAULT_MAX_WORKERS;
    }
    if (value > RT_TASK_POOL_MAX_WORKERS) {
        return RT_TASK_POOL_MAX_WORKERS;
    }
    return (unsigned int)value;
}

static int scan_entries_parallel(const char *archive_path, const ArchiveZipInfo *info, EurlexEntryList *list, EurlexScanContext *context) {
    EurlexParallelScan parallel;
    RtTaskPool pool;
    unsigned int worker_count;
    size_t index;
    int result;

    if (list->count == 0U) return 0;
    rt_memset(&parallel, 0, sizeof(parallel));
    parallel.archive_path = archive_path;
    parallel.info = info;
    parallel.targets = context->targets;
    parallel.target_count = context->target_count;
    parallel.build_index = context->build_index;
    parallel.entries = list->entries;
    for (index = 0U; index < RT_TASK_POOL_MAX_WORKERS; ++index) parallel.worker_fds[index] = -1;

    worker_count = eurlex_worker_count_from_env();
    if (rt_task_pool_init(&pool, worker_count) != 0) {
        rt_task_pool_destroy(&pool);
        if (rt_task_pool_init(&pool, 1U) != 0) return -1;
    }
    result = rt_parallel_for(&pool, list->count, 1U, parallel_scan_range, &parallel);
    rt_task_pool_destroy(&pool);
    for (index = 0U; index < RT_TASK_POOL_MAX_WORKERS; ++index) {
        if (parallel.worker_fds[index] >= 0) platform_close(parallel.worker_fds[index]);
    }
    if (result != 0) return -1;
    for (index = 0U; index < list->count; ++index) {
        EurlexWorkEntry *work = &list->entries[index];
        if (work->failed) return -1;
        if (work->output.size != 0U && rt_write_all(context->output_fd, work->output.data, work->output.size) != 0) return -1;
        context->match_count += work->match_count;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *archive_path = EURLEX_DEFAULT_MTD_ARCHIVE;
    const char *index_path = 0;
    const char *build_index_path = 0;
    int using_default_archive = 1;
    EurlexTarget targets[EURLEX_MAX_TARGETS];
    int target_count = 0;
    int argi = 1;
    int fd;
    ArchiveZipInfo info;
    EurlexScanContext context;
    EurlexEntryList entries;

    while (argi < argc) {
        if (rt_strcmp(argv[argi], "--help") == 0 || rt_strcmp(argv[argi], "-h") == 0) {
            print_usage();
            return 0;
        }
        if (rt_strcmp(argv[argi], "-a") == 0) {
            if (argi + 1 >= argc) {
                print_usage();
                return 1;
            }
            archive_path = argv[argi + 1];
            using_default_archive = 0;
            argi += 2;
            continue;
        }
        if (rt_strcmp(argv[argi], "-i") == 0 || rt_strcmp(argv[argi], "--index") == 0) {
            if (argi + 1 >= argc) {
                print_usage();
                return 1;
            }
            index_path = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (rt_strcmp(argv[argi], "--build-index") == 0) {
            if (argi + 1 >= argc) {
                print_usage();
                return 1;
            }
            build_index_path = argv[argi + 1];
            argi += 2;
            continue;
        }
        break;
    }

    if (index_path != 0 && build_index_path != 0) {
        rt_write_line(2, "eurlex-delegated: choose either --index or --build-index");
        return 1;
    }

    while (argi < argc) {
        if (target_count >= EURLEX_MAX_TARGETS) {
            rt_write_line(2, "eurlex-delegated: too many CELEX targets");
            return 1;
        }
        if (normalize_target_cstr(argv[argi], targets[target_count].celex, sizeof(targets[target_count].celex)) != 0) {
            rt_write_cstr(2, "eurlex-delegated: invalid target: ");
            rt_write_line(2, argv[argi]);
            return 1;
        }
        target_count += 1;
        argi += 1;
    }
    if (build_index_path == 0 && target_count == 0) {
        print_usage();
        return 1;
    }
    if (index_path != 0) {
        unsigned long long index_matches;

        if (query_index_file(index_path, targets, target_count, &index_matches) != 0) return 1;
        if (index_matches == 0ULL) {
            rt_write_line(2, "eurlex-delegated: no delegated acts found for requested CELEX target(s)");
        }
        return 0;
    }

    fd = platform_open_read(archive_path);
    if (fd < 0 && using_default_archive) {
        archive_path = EURLEX_LOCAL_MTD_ARCHIVE;
        fd = platform_open_read(archive_path);
    }
    if (fd < 0) {
        rt_write_cstr(2, "eurlex-delegated: cannot open metadata ZIP: ");
        rt_write_line(2, archive_path);
        return 1;
    }
    if (archive_zip_read_info(fd, &info) != 0 || info.multi_disk) {
        platform_close(fd);
        rt_write_cstr(2, "eurlex-delegated: unsupported metadata ZIP: ");
        rt_write_line(2, archive_path);
        return 1;
    }

    rt_memset(&context, 0, sizeof(context));
    context.fd = fd;
    context.archive_path = archive_path;
    context.info = &info;
    context.targets = targets;
    context.target_count = target_count;
    context.output_fd = 1;
    if (build_index_path != 0) {
        context.output_fd = platform_open_write(build_index_path, 0644U);
        if (context.output_fd < 0) {
            platform_close(fd);
            rt_write_cstr(2, "eurlex-delegated: cannot write index: ");
            rt_write_line(2, build_index_path);
            return 1;
        }
        context.build_index = target_count == 0;
    }
    write_index_header(context.output_fd);
    rt_memset(&entries, 0, sizeof(entries));
    if (target_count > 0) {
        if (archive_zip_iterate_entries(fd, &info, collect_zip_entry, &entries) != 0 || entries.failed || scan_entries_parallel(archive_path, &info, &entries, &context) != 0) {
            release_entry_list(&entries);
            if (context.output_fd != 1) platform_close(context.output_fd);
            platform_close(fd);
            return 1;
        }
        release_entry_list(&entries);
    } else if (archive_zip_iterate_entries(fd, &info, scan_zip_entry, &context) != 0 || context.failed) {
        if (context.output_fd != 1) platform_close(context.output_fd);
        platform_close(fd);
        return 1;
    }
    if (context.output_fd != 1) platform_close(context.output_fd);
    platform_close(fd);

    if (build_index_path == 0 && context.match_count == 0ULL) {
        rt_write_line(2, "eurlex-delegated: no delegated acts found for requested CELEX target(s)");
    }
    return 0;
}