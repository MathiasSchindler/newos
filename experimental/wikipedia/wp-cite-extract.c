#include "compression/bzip2.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define WP_CITE_BUFFER_SIZE 32768U
#define WP_CITE_PATH_SIZE 1024U
#define WP_CITE_NAME_SIZE 256U
#define WP_CITE_SNIPPET_SIZE 512U
#define WP_CITE_SNIPPET_BEFORE 192U
#define WP_CITE_SNIPPET_AFTER 320U
#define WP_CITE_SNIPPET_CONTEXT_SEARCH 512U
#define WP_CITE_MAX_INPUTS 128U
#define WP_CITE_LINE_INITIAL 4096U

typedef struct {
    const char *data_dir;
    const char *input_path;
    const char *output_path;
    int quiet;
} WpCiteOptions;

typedef struct {
    char path[WP_CITE_PATH_SIZE];
    char name[WP_CITE_NAME_SIZE];
    char wiki[64];
    char date[16];
    unsigned long long size;
} WpDumpInput;

typedef struct {
    WpDumpInput inputs[WP_CITE_MAX_INPUTS];
    size_t count;
    char wiki[64];
    char date[16];
} WpDumpSet;

typedef struct {
    int fd;
    unsigned char buffer[WP_CITE_BUFFER_SIZE];
    size_t offset;
    size_t available;
} WpInput;

typedef struct {
    WpInput *input;
    char *data;
    size_t size;
    size_t capacity;
    int eof;
} WpLineReader;

typedef struct {
    const char *source_name;
    const char *wiki;
    const char *date;
    ToolOutputBuffer *output;
    unsigned long long pages_seen;
    unsigned long long article_pages;
    unsigned long long records_written;
} WpExtractContext;

typedef struct {
    WpExtractContext *ctx;
    char *line;
    size_t line_size;
    size_t line_capacity;
    char *page;
    size_t page_size;
    size_t page_capacity;
    int in_page;
} WpParser;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-q] [-d DIR] [-i FILE] [-o FILE]");
}

static void write_status_prefix(void) {
    char timestamp[32];

    if (platform_format_time(platform_get_epoch_time(), 1, "%Y-%m-%d %H:%M:%S", timestamp, sizeof(timestamp)) != 0) {
        rt_copy_string(timestamp, sizeof(timestamp), "0000-00-00 00:00:00");
    }
    rt_write_cstr(2, timestamp);
    rt_write_char(2, ' ');
}

static void write_info(const char *message, const char *detail) {
    write_status_prefix();
    rt_write_cstr(2, message);
    if (detail != 0) rt_write_cstr(2, detail);
    rt_write_char(2, '\n');
}

static int text_ends_with(const char *text, const char *suffix) {
    size_t text_length = rt_strlen(text);
    size_t suffix_length = rt_strlen(suffix);

    if (suffix_length > text_length) return 0;
    return rt_strcmp(text + text_length - suffix_length, suffix) == 0;
}

static int is_input_dump_name(const char *name) {
    return text_ends_with(name, ".xml.bz2") || text_ends_with(name, ".xml");
}

static int parse_wiki_date_from_name(const char *name, char *wiki_out, size_t wiki_size, char *date_out, size_t date_size) {
    size_t index = 0U;
    size_t wiki_end = 0U;

    while (name[wiki_end] != '\0' && name[wiki_end] != '-') wiki_end += 1U;
    if (wiki_end == 0U || wiki_end + 12U >= rt_strlen(name) || name[wiki_end] != '-') return -1;
    if (wiki_end + 11U >= rt_strlen(name)) return -1;
    for (index = 0U; index < 10U; ++index) {
        char ch = name[wiki_end + 1U + index];
        if ((index == 4U || index == 7U) ? ch != '-' : !tool_ascii_is_digit(ch)) return -1;
    }
    if (name[wiki_end + 11U] != '-') return -1;
    if (wiki_end + 1U > wiki_size || 11U > date_size) return -1;
    memcpy(wiki_out, name, wiki_end);
    wiki_out[wiki_end] = '\0';
    memcpy(date_out, name + wiki_end + 1U, 10U);
    date_out[10] = '\0';
    return 0;
}

static const char *default_data_dir(void) {
    int is_dir = 0;

    if (platform_path_is_directory("experimental/wikipedia/data", &is_dir) == 0 && is_dir) {
        return "experimental/wikipedia/data";
    }
    return "data";
}

static int join_path(const char *dir, const char *name, char *out, size_t out_size) {
    if (dir == 0 || dir[0] == '\0' || (dir[0] == '.' && dir[1] == '\0')) {
        rt_copy_string(out, out_size, name);
        return rt_strlen(out) == rt_strlen(name) ? 0 : -1;
    }
    return rt_join_path(dir, name, out, out_size);
}

static int collect_latest_inputs(const char *data_dir, WpDumpSet *set) {
    PlatformDirEntry entries[256];
    size_t count = 0U;
    int is_dir = 0;
    char best_date[16];
    char best_wiki[64];
    size_t index;

    rt_memset(set, 0, sizeof(*set));
    best_date[0] = '\0';
    best_wiki[0] = '\0';
    if (platform_collect_entries(data_dir, 0, entries, sizeof(entries) / sizeof(entries[0]), &count, &is_dir) != 0 || !is_dir) {
        return -1;
    }
    for (index = 0U; index < count; ++index) {
        char wiki[64];
        char date[16];

        if (entries[index].is_dir || !is_input_dump_name(entries[index].name)) continue;
        if (parse_wiki_date_from_name(entries[index].name, wiki, sizeof(wiki), date, sizeof(date)) != 0) continue;
        if (best_date[0] == '\0' || rt_strcmp(date, best_date) > 0 || (rt_strcmp(date, best_date) == 0 && rt_strcmp(wiki, best_wiki) > 0)) {
            rt_copy_string(best_date, sizeof(best_date), date);
            rt_copy_string(best_wiki, sizeof(best_wiki), wiki);
        }
    }
    if (best_date[0] == '\0') return -1;
    rt_copy_string(set->date, sizeof(set->date), best_date);
    rt_copy_string(set->wiki, sizeof(set->wiki), best_wiki);
    for (index = 0U; index < count; ++index) {
        char wiki[64];
        char date[16];
        WpDumpInput *input;

        if (entries[index].is_dir || !is_input_dump_name(entries[index].name)) continue;
        if (parse_wiki_date_from_name(entries[index].name, wiki, sizeof(wiki), date, sizeof(date)) != 0) continue;
        if (rt_strcmp(wiki, best_wiki) != 0 || rt_strcmp(date, best_date) != 0) continue;
        if (set->count >= WP_CITE_MAX_INPUTS) return -1;
        input = &set->inputs[set->count++];
        rt_copy_string(input->name, sizeof(input->name), entries[index].name);
        rt_copy_string(input->wiki, sizeof(input->wiki), wiki);
        rt_copy_string(input->date, sizeof(input->date), date);
        input->size = entries[index].size;
        if (join_path(data_dir, entries[index].name, input->path, sizeof(input->path)) != 0) return -1;
    }
    return set->count == 0U ? -1 : 0;
}

static int set_single_input(const char *path, WpDumpSet *set) {
    const char *name = path;
    size_t index;

    rt_memset(set, 0, sizeof(*set));
    for (index = 0U; path[index] != '\0'; ++index) {
        if (path[index] == '/') name = path + index + 1U;
    }
    rt_copy_string(set->inputs[0].path, sizeof(set->inputs[0].path), path);
    rt_copy_string(set->inputs[0].name, sizeof(set->inputs[0].name), name);
    if (parse_wiki_date_from_name(name, set->inputs[0].wiki, sizeof(set->inputs[0].wiki), set->inputs[0].date, sizeof(set->inputs[0].date)) == 0) {
        rt_copy_string(set->wiki, sizeof(set->wiki), set->inputs[0].wiki);
        rt_copy_string(set->date, sizeof(set->date), set->inputs[0].date);
    } else {
        rt_copy_string(set->wiki, sizeof(set->wiki), "unknownwiki");
        rt_copy_string(set->date, sizeof(set->date), "unknown-date");
        rt_copy_string(set->inputs[0].wiki, sizeof(set->inputs[0].wiki), set->wiki);
        rt_copy_string(set->inputs[0].date, sizeof(set->inputs[0].date), set->date);
    }
    set->count = 1U;
    return 0;
}

static int make_default_output_path(const char *data_dir, const WpDumpSet *set, char *out, size_t out_size) {
    char name[WP_CITE_NAME_SIZE];
    size_t length = 0U;

    length = tool_buffer_append_cstr(name, sizeof(name), length, set->wiki[0] != '\0' ? set->wiki : "wiki");
    length = tool_buffer_append_char(name, sizeof(name), length, '-');
    length = tool_buffer_append_cstr(name, sizeof(name), length, set->date[0] != '\0' ? set->date : "snapshot");
    length = tool_buffer_append_cstr(name, sizeof(name), length, "-citations.tsv");
    if (rt_strlen(name) != length) return -1;
    return join_path(data_dir, name, out, out_size);
}

static int input_open(WpInput *input, const char *path) {
    rt_memset(input, 0, sizeof(*input));
    input->fd = -1;
    input->fd = platform_open_read(path);
    return input->fd < 0 ? -1 : 0;
}

static int input_close(WpInput *input) {
    int result = 0;

    if (input->fd >= 0) {
        if (platform_close(input->fd) != 0) result = -1;
    }
    input->fd = -1;
    return result;
}

static long input_read(WpInput *input, void *buffer, size_t size) {
    return platform_read(input->fd, buffer, size);
}

static void line_reader_init(WpLineReader *reader, WpInput *input) {
    rt_memset(reader, 0, sizeof(*reader));
    reader->input = input;
}

static void line_reader_destroy(WpLineReader *reader) {
    rt_free(reader->data);
    rt_memset(reader, 0, sizeof(*reader));
}

static int line_reader_append(WpLineReader *reader, char ch) {
    if (reader->size + 1U >= reader->capacity) {
        size_t next_capacity = reader->capacity == 0U ? WP_CITE_LINE_INITIAL : reader->capacity * 2U;
        char *next = (char *)rt_realloc(reader->data, next_capacity);
        if (next == 0) return -1;
        reader->data = next;
        reader->capacity = next_capacity;
    }
    reader->data[reader->size++] = ch;
    return 0;
}

static int line_reader_next(WpLineReader *reader, char **line_out, size_t *length_out, int *has_line_out) {
    *line_out = 0;
    *length_out = 0U;
    *has_line_out = 0;
    reader->size = 0U;
    for (;;) {
        char ch;

        if (reader->input->offset == reader->input->available) {
            long amount = input_read(reader->input, reader->input->buffer, sizeof(reader->input->buffer));
            if (amount < 0) return -1;
            reader->input->offset = 0U;
            reader->input->available = (size_t)amount;
            if (amount == 0) {
                if (reader->size == 0U) return 0;
                break;
            }
        }
        ch = (char)reader->input->buffer[reader->input->offset++];
        if (line_reader_append(reader, ch) != 0) return -1;
        if (ch == '\n') break;
    }
    if (line_reader_append(reader, '\0') != 0) return -1;
    reader->size -= 1U;
    *line_out = reader->data;
    *length_out = reader->size;
    *has_line_out = 1;
    return 0;
}

static int buffer_append(char **data_io, size_t *size_io, size_t *capacity_io, const char *data, size_t size) {
    if (*size_io + size + 1U > *capacity_io) {
        size_t next_capacity = *capacity_io == 0U ? 65536U : *capacity_io * 2U;
        char *next;
        while (next_capacity < *size_io + size + 1U) next_capacity *= 2U;
        next = (char *)rt_realloc(*data_io, next_capacity);
        if (next == 0) return -1;
        *data_io = next;
        *capacity_io = next_capacity;
    }
    memcpy(*data_io + *size_io, data, size);
    *size_io += size;
    (*data_io)[*size_io] = '\0';
    return 0;
}

static const char *find_substr(const char *text, const char *needle) {
    size_t needle_length = rt_strlen(needle);
    size_t index;

    if (needle_length == 0U) return text;
    for (index = 0U; text[index] != '\0'; ++index) {
        if (rt_strncmp(text + index, needle, needle_length) == 0) return text + index;
    }
    return 0;
}

static int contains_case_n(const char *text, size_t text_size, const char *needle) {
    size_t needle_size = rt_strlen(needle);
    size_t index;

    if (needle_size == 0U) return 1;
    if (needle_size > text_size) return 0;
    for (index = 0U; index + needle_size <= text_size; ++index) {
        size_t needle_index;
        int matched = 1;

        for (needle_index = 0U; needle_index < needle_size; ++needle_index) {
            if (tool_ascii_tolower(text[index + needle_index]) != tool_ascii_tolower(needle[needle_index])) {
                matched = 0;
                break;
            }
        }
        if (matched) return 1;
    }
    return 0;
}

static int extract_xml_field(const char *page, const char *start_tag, const char *end_tag, char *out, size_t out_size) {
    const char *start = find_substr(page, start_tag);
    const char *end;
    size_t length;

    if (start == 0) return -1;
    start += rt_strlen(start_tag);
    end = find_substr(start, end_tag);
    if (end == 0) return -1;
    length = (size_t)(end - start);
    if (length + 1U > out_size) length = out_size - 1U;
    memcpy(out, start, length);
    out[length] = '\0';
    return 0;
}

static int extract_page_id(const char *page, char *out, size_t out_size) {
    const char *title = find_substr(page, "<title>");
    const char *revision = find_substr(page, "<revision>");
    const char *id;
    const char *end;
    size_t length;

    if (title == 0) title = page;
    id = find_substr(title, "<id>");
    if (id == 0 || (revision != 0 && id > revision)) return -1;
    id += 4;
    end = find_substr(id, "</id>");
    if (end == 0 || (revision != 0 && end > revision)) return -1;
    length = (size_t)(end - id);
    if (length + 1U > out_size) length = out_size - 1U;
    memcpy(out, id, length);
    out[length] = '\0';
    return 0;
}

static int extract_text_xml(const char *page, const char **text_start_out, size_t *text_size_out) {
    const char *tag = find_substr(page, "<text");
    const char *start;
    const char *end;

    if (tag == 0) return -1;
    start = tag;
    while (*start != '\0' && *start != '>') start += 1U;
    if (*start != '>') return -1;
    start += 1U;
    end = find_substr(start, "</text>");
    if (end == 0) return -1;
    *text_start_out = start;
    *text_size_out = (size_t)(end - start);
    return 0;
}

static int xml_decode(const char *input, size_t input_size, char **out_data, size_t *out_size) {
    char *out = (char *)rt_malloc(input_size + 1U);
    size_t in_index = 0U;
    size_t out_index = 0U;

    if (out == 0) return -1;
    while (in_index < input_size) {
        if (input[in_index] == '&') {
            if (in_index + 4U <= input_size && rt_strncmp(input + in_index, "&lt;", 4U) == 0) {
                out[out_index++] = '<';
                in_index += 4U;
                continue;
            }
            if (in_index + 4U <= input_size && rt_strncmp(input + in_index, "&gt;", 4U) == 0) {
                out[out_index++] = '>';
                in_index += 4U;
                continue;
            }
            if (in_index + 5U <= input_size && rt_strncmp(input + in_index, "&amp;", 5U) == 0) {
                out[out_index++] = '&';
                in_index += 5U;
                continue;
            }
            if (in_index + 6U <= input_size && rt_strncmp(input + in_index, "&quot;", 6U) == 0) {
                out[out_index++] = '"';
                in_index += 6U;
                continue;
            }
            if (in_index + 6U <= input_size && rt_strncmp(input + in_index, "&apos;", 6U) == 0) {
                out[out_index++] = '\'';
                in_index += 6U;
                continue;
            }
        }
        out[out_index++] = input[in_index++];
    }
    out[out_index] = '\0';
    *out_data = out;
    *out_size = out_index;
    return 0;
}

static int write_tsv_text(ToolOutputBuffer *output, const char *text, size_t size) {
    size_t index;
    size_t start = 0U;

    for (index = 0U; index < size; ++index) {
        char ch = text[index];
        if (ch == '\t' || ch == '\n' || ch == '\r') {
            if (index > start && tool_output_buffer_write(output, text + start, index - start) != 0) return -1;
            if (tool_output_buffer_write_char(output, ' ') != 0) return -1;
            start = index + 1U;
        }
    }
    if (index > start && tool_output_buffer_write(output, text + start, index - start) != 0) return -1;
    return 0;
}

static int write_tsv_cstr(ToolOutputBuffer *output, const char *text) {
    return write_tsv_text(output, text, rt_strlen(text));
}

static int write_record(WpExtractContext *ctx, const char *page_id, const char *title, const char *kind, const char *value, const char *raw, size_t raw_size) {
    if (write_tsv_cstr(ctx->output, ctx->wiki) != 0 || tool_output_buffer_write_char(ctx->output, '\t') != 0 ||
        write_tsv_cstr(ctx->output, ctx->date) != 0 || tool_output_buffer_write_char(ctx->output, '\t') != 0 ||
        write_tsv_cstr(ctx->output, ctx->source_name) != 0 || tool_output_buffer_write_char(ctx->output, '\t') != 0 ||
        write_tsv_cstr(ctx->output, page_id) != 0 || tool_output_buffer_write_char(ctx->output, '\t') != 0 ||
        write_tsv_cstr(ctx->output, title) != 0 || tool_output_buffer_write_char(ctx->output, '\t') != 0 ||
        write_tsv_cstr(ctx->output, kind) != 0 || tool_output_buffer_write_char(ctx->output, '\t') != 0 ||
        write_tsv_cstr(ctx->output, value) != 0 || tool_output_buffer_write_char(ctx->output, '\t') != 0 ||
        write_tsv_text(ctx->output, raw, raw_size) != 0 || tool_output_buffer_write_char(ctx->output, '\n') != 0) {
        return -1;
    }
    ctx->records_written += 1ULL;
    return 0;
}

static int snippet_match_case_at(const char *text, size_t size, size_t position, const char *needle) {
    size_t needle_size = rt_strlen(needle);
    size_t index;

    if (position + needle_size > size) return 0;
    for (index = 0U; index < needle_size; ++index) {
        if (tool_ascii_tolower(text[position + index]) != tool_ascii_tolower(needle[index])) return 0;
    }
    return 1;
}

static int snippet_find_last_case_before(const char *text, size_t size, size_t begin, size_t end, const char *needle, size_t *position_out) {
    size_t index;

    if (begin > end) begin = end;
    if (end > size) end = size;
    for (index = end; index > begin; --index) {
        size_t position = index - 1U;
        if (snippet_match_case_at(text, size, position, needle)) {
            *position_out = position;
            return 1;
        }
    }
    return 0;
}

static int snippet_find_last_boundary_before(const char *text, size_t begin, size_t end, size_t *position_out) {
    size_t index;

    if (begin > end) begin = end;
    for (index = end; index > begin; --index) {
        size_t position = index - 1U;
        if (text[position] == '\n' || text[position] == '*' || text[position] == '#') {
            *position_out = position + 1U;
            return 1;
        }
    }
    return 0;
}

static int snippet_find_first_case_after(const char *text, size_t size, size_t begin, size_t end, const char *needle, size_t *position_out) {
    size_t index;

    if (end > size) end = size;
    for (index = begin; index < end; ++index) {
        if (snippet_match_case_at(text, size, index, needle)) {
            *position_out = index;
            return 1;
        }
    }
    return 0;
}

static int snippet_boundary_char(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '<' || ch == '{' || ch == '[';
}

static size_t snippet_snap_start(const char *text, size_t start) {
    size_t limit = start > 64U ? start - 64U : 0U;

    while (start > limit && start > 0U && !snippet_boundary_char(text[start - 1U])) start -= 1U;
    if (start > 0U && (text[start - 1U] == '<' || text[start - 1U] == '{' || text[start - 1U] == '[')) start -= 1U;
    return start;
}

static size_t snippet_snap_end(const char *text, size_t size, size_t end) {
    size_t limit = end + 64U;

    if (limit > size) limit = size;
    while (end < limit && end < size && !snippet_boundary_char(text[end])) end += 1U;
    return end;
}

static size_t snippet_bounds(const char *text, size_t size, size_t position, size_t *start_out) {
    size_t search_start = position > WP_CITE_SNIPPET_CONTEXT_SEARCH ? position - WP_CITE_SNIPPET_CONTEXT_SEARCH : 0U;
    size_t start = position > WP_CITE_SNIPPET_BEFORE ? position - WP_CITE_SNIPPET_BEFORE : 0U;
    size_t end = position + WP_CITE_SNIPPET_AFTER;
    size_t marker;
    size_t closing;

    if (snippet_find_last_case_before(text, size, search_start, position, "<ref", &marker)) {
        size_t close_marker;
        if (snippet_find_last_case_before(text, size, marker, position, "</ref>", &close_marker) || position - marker > WP_CITE_SNIPPET_BEFORE) marker = size;
    } else {
        marker = size;
    }
    if (marker != size) {
        start = marker;
    } else if (snippet_find_last_case_before(text, size, search_start, position, "{{", &marker) && position - marker <= WP_CITE_SNIPPET_BEFORE) {
        start = marker;
    } else if (snippet_find_last_boundary_before(text, search_start, position, &marker)) {
        start = marker;
    } else {
        start = snippet_snap_start(text, start);
    }
    if (end > size) end = size;
    if (start < position && snippet_match_case_at(text, size, start, "<ref") && snippet_find_first_case_after(text, size, position, size, "</ref>", &closing) && closing + 6U - start <= WP_CITE_SNIPPET_SIZE) {
        end = closing + 6U;
    } else if (start < position && snippet_match_case_at(text, size, start, "{{") && snippet_find_first_case_after(text, size, position, size, "}}", &closing) && closing + 2U - start <= WP_CITE_SNIPPET_SIZE) {
        end = closing + 2U;
    } else {
        end = snippet_snap_end(text, size, end);
        if (end > start + WP_CITE_SNIPPET_SIZE) end = start + WP_CITE_SNIPPET_SIZE;
    }
    if (end <= position) end = position + 1U < size ? position + 1U : size;
    *start_out = start;
    return end - start;
}

static int is_doi_char(char ch) {
    return ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == ';' || ch == '(' || ch == ')' || ch == '/' || ch == ':' || ch == '-');
}

static int trim_identifier(char *value) {
    size_t length = rt_strlen(value);

    while (length > 0U) {
        char ch = value[length - 1U];
        if (ch == '.' || ch == ',' || ch == ';' || ch == ':' || ch == ')' || ch == ']' || ch == '}') {
            value[--length] = '\0';
        } else {
            break;
        }
    }
    return length > 0U ? 0 : -1;
}

static int emit_identifier(WpExtractContext *ctx, const char *page_id, const char *title, const char *text, size_t text_size, const char *kind, const char *value, size_t position) {
    size_t raw_start = 0U;
    size_t raw_size = snippet_bounds(text, text_size, position, &raw_start);

    return write_record(ctx, page_id, title, kind, value, text + raw_start, raw_size);
}

static int scan_dois(WpExtractContext *ctx, const char *page_id, const char *title, const char *text, size_t text_size) {
    size_t index;

    for (index = 0U; index + 3U < text_size; ++index) {
        char value[160];
        size_t start;
        size_t out = 0U;

        if (text[index] != '1' || text[index + 1U] != '0' || text[index + 2U] != '.' || !tool_ascii_is_digit(text[index + 3U])) continue;
        start = index;
        while (index < text_size && is_doi_char(text[index]) && out + 1U < sizeof(value)) value[out++] = text[index++];
        value[out] = '\0';
        if (out >= 6U && trim_identifier(value) == 0) {
            if (emit_identifier(ctx, page_id, title, text, text_size, "doi", value, start) != 0) return -1;
        }
    }
    return 0;
}

static int collect_digits_after_label(const char *text, size_t text_size, size_t label_pos, char *value, size_t value_size) {
    size_t index = label_pos;
    size_t out = 0U;

    while (index < text_size && !tool_ascii_is_digit(text[index]) && index < label_pos + 32U) index += 1U;
    while (index < text_size && tool_ascii_is_digit(text[index]) && out + 1U < value_size) value[out++] = text[index++];
    value[out] = '\0';
    return out > 0U ? 0 : -1;
}

static int scan_pmids(WpExtractContext *ctx, const char *page_id, const char *title, const char *text, size_t text_size) {
    size_t index;

    for (index = 0U; index + 4U < text_size; ++index) {
        char value[32];
        if ((tool_ascii_tolower(text[index]) == 'p') && (tool_ascii_tolower(text[index + 1U]) == 'm') && (tool_ascii_tolower(text[index + 2U]) == 'i') && (tool_ascii_tolower(text[index + 3U]) == 'd')) {
            if (collect_digits_after_label(text, text_size, index + 4U, value, sizeof(value)) == 0) {
                if (emit_identifier(ctx, page_id, title, text, text_size, "pmid", value, index) != 0) return -1;
            }
        }
    }
    return 0;
}

static int is_isbn_char(char ch) {
    return tool_ascii_is_digit(ch) || ch == 'X' || ch == 'x' || ch == '-' || ch == ' ';
}

static int emit_isbn_at(WpExtractContext *ctx, const char *page_id, const char *title, const char *text, size_t text_size, size_t start, size_t value_start) {
    char value[64];
    size_t index = value_start;
    size_t out = 0U;
    size_t digits = 0U;

    while (index < text_size && (text[index] == ' ' || text[index] == '\t' || text[index] == ':' || text[index] == '=')) index += 1U;

    while (index < text_size && is_isbn_char(text[index]) && out + 1U < sizeof(value)) {
        if (tool_ascii_is_digit(text[index]) || text[index] == 'X' || text[index] == 'x') digits += 1U;
        value[out++] = text[index++];
    }
    value[out] = '\0';
    if (digits < 10U) return 0;
    return emit_identifier(ctx, page_id, title, text, text_size, "isbn", value, start);
}

static int isbn_signal_has_near_label(const char *text, size_t index) {
    size_t start = index > 16U ? index - 16U : 0U;
    size_t pos;

    for (pos = start; pos + 4U <= index; ++pos) {
        if (tool_ascii_tolower(text[pos]) == 'i' && tool_ascii_tolower(text[pos + 1U]) == 's' && tool_ascii_tolower(text[pos + 2U]) == 'b' && tool_ascii_tolower(text[pos + 3U]) == 'n') return 1;
    }
    return 0;
}

static int scan_isbns(WpExtractContext *ctx, const char *page_id, const char *title, const char *text, size_t text_size) {
    size_t index;

    for (index = 0U; index + 4U < text_size; ++index) {
        if (tool_ascii_tolower(text[index]) == 'i' && tool_ascii_tolower(text[index + 1U]) == 's' && tool_ascii_tolower(text[index + 2U]) == 'b' && tool_ascii_tolower(text[index + 3U]) == 'n') {
            if (emit_isbn_at(ctx, page_id, title, text, text_size, index, index + 4U) != 0) return -1;
        } else if (index + 5U < text_size && rt_strncmp(text + index, "978-3", 5U) == 0 && !isbn_signal_has_near_label(text, index)) {
            if (emit_isbn_at(ctx, page_id, title, text, text_size, index, index) != 0) return -1;
        } else if (index + 4U < text_size && rt_strncmp(text + index, "9783", 4U) == 0 && !isbn_signal_has_near_label(text, index)) {
            if (emit_isbn_at(ctx, page_id, title, text, text_size, index, index) != 0) return -1;
        }
    }
    return 0;
}

static int scan_issns(WpExtractContext *ctx, const char *page_id, const char *title, const char *text, size_t text_size) {
    size_t index;

    for (index = 0U; index + 4U < text_size; ++index) {
        char value[32];
        size_t pos;
        size_t out = 0U;
        size_t digits = 0U;

        if (!(tool_ascii_tolower(text[index]) == 'i' && tool_ascii_tolower(text[index + 1U]) == 's' && tool_ascii_tolower(text[index + 2U]) == 's' && tool_ascii_tolower(text[index + 3U]) == 'n')) continue;
        pos = index + 4U;
        while (pos < text_size && !tool_ascii_is_digit(text[pos]) && pos < index + 24U) pos += 1U;
        while (pos < text_size && (tool_ascii_is_digit(text[pos]) || text[pos] == '-' || text[pos] == 'X' || text[pos] == 'x') && out + 1U < sizeof(value)) {
            if (tool_ascii_is_digit(text[pos]) || text[pos] == 'X' || text[pos] == 'x') digits += 1U;
            value[out++] = text[pos++];
        }
        value[out] = '\0';
        if (digits == 8U) {
            if (emit_identifier(ctx, page_id, title, text, text_size, "issn", value, index) != 0) return -1;
        }
    }
    return 0;
}

static int scan_templates(WpExtractContext *ctx, const char *page_id, const char *title, const char *text, size_t text_size) {
    size_t index;

    for (index = 0U; index + 3U < text_size; ++index) {
        if (text[index] == '{' && text[index + 1U] == '{') {
            size_t end = index + 2U;
            size_t candidate_size;
            size_t raw_size;
            while (end + 1U < text_size && !(text[end] == '}' && text[end + 1U] == '}') && end < index + 4096U) end += 1U;
            candidate_size = end + 2U <= text_size ? end + 2U - index : text_size - index;
            raw_size = candidate_size;
            if (raw_size > WP_CITE_SNIPPET_SIZE) raw_size = WP_CITE_SNIPPET_SIZE;
            if (contains_case_n(text + index, candidate_size, "{{Literatur") || contains_case_n(text + index, candidate_size, "{{Cite") || contains_case_n(text + index, candidate_size, "{{Internetquelle")) {
                if (write_record(ctx, page_id, title, "template", "", text + index, raw_size) != 0) return -1;
            }
        } else if (text[index] == '<' && tool_ascii_tolower(text[index + 1U]) == 'r' && tool_ascii_tolower(text[index + 2U]) == 'e' && tool_ascii_tolower(text[index + 3U]) == 'f') {
            size_t end = index;
            size_t candidate_size;
            size_t raw_size;
            while (end + 5U < text_size && !(text[end] == '<' && text[end + 1U] == '/' && tool_ascii_tolower(text[end + 2U]) == 'r' && tool_ascii_tolower(text[end + 3U]) == 'e' && tool_ascii_tolower(text[end + 4U]) == 'f') && end < index + 4096U) end += 1U;
            candidate_size = end > index ? end - index : 0U;
            raw_size = candidate_size;
            if (raw_size > WP_CITE_SNIPPET_SIZE) raw_size = WP_CITE_SNIPPET_SIZE;
            if (candidate_size > 0U && (contains_case_n(text + index, candidate_size, "doi") || contains_case_n(text + index, candidate_size, "pmid") || contains_case_n(text + index, candidate_size, "isbn") || contains_case_n(text + index, candidate_size, "issn") || contains_case_n(text + index, candidate_size, "Literatur") || contains_case_n(text + index, candidate_size, "Cite"))) {
                if (write_record(ctx, page_id, title, "ref", "", text + index, raw_size) != 0) return -1;
            }
        }
    }
    return 0;
}

static int page_is_article(const char *page) {
    return find_substr(page, "<ns>0</ns>") != 0;
}

static int raw_text_has_doi_signal(const char *text, size_t text_size) {
    size_t index;

    for (index = 0U; index + 3U < text_size; ++index) {
        if (text[index] == '1' && text[index + 1U] == '0' && text[index + 2U] == '.' && tool_ascii_is_digit(text[index + 3U])) return 1;
    }
    return 0;
}

static int raw_text_has_citation_signal(const char *text, size_t text_size) {
    return raw_text_has_doi_signal(text, text_size) ||
        contains_case_n(text, text_size, "doi") ||
        contains_case_n(text, text_size, "pmid") ||
        contains_case_n(text, text_size, "pubmed") ||
        contains_case_n(text, text_size, "isbn") ||
        contains_case_n(text, text_size, "issn") ||
        contains_case_n(text, text_size, "978-3") ||
        contains_case_n(text, text_size, "9783") ||
        contains_case_n(text, text_size, "{{Literatur") ||
        contains_case_n(text, text_size, "{{Internetquelle") ||
        contains_case_n(text, text_size, "{{Cite");
}

static int process_page(WpExtractContext *ctx, const char *page) {
    char title[512];
    char page_id[64];
    const char *xml_text = 0;
    size_t xml_text_size = 0U;
    char *text = 0;
    size_t text_size = 0U;
    int result = 0;

    ctx->pages_seen += 1ULL;
    if (!page_is_article(page)) return 0;
    ctx->article_pages += 1ULL;
    if (extract_text_xml(page, &xml_text, &xml_text_size) != 0) return 0;
    if (!raw_text_has_citation_signal(xml_text, xml_text_size)) return 0;
    if (extract_xml_field(page, "<title>", "</title>", title, sizeof(title)) != 0) rt_copy_string(title, sizeof(title), "");
    if (extract_page_id(page, page_id, sizeof(page_id)) != 0) rt_copy_string(page_id, sizeof(page_id), "");
    if (xml_decode(xml_text, xml_text_size, &text, &text_size) != 0) return -1;
    if (scan_dois(ctx, page_id, title, text, text_size) != 0 ||
        scan_pmids(ctx, page_id, title, text, text_size) != 0 ||
        scan_isbns(ctx, page_id, title, text, text_size) != 0 ||
        scan_issns(ctx, page_id, title, text, text_size) != 0 ||
        scan_templates(ctx, page_id, title, text, text_size) != 0) {
        result = -1;
    }
    rt_free(text);
    return result;
}

static void parser_init(WpParser *parser, WpExtractContext *ctx) {
    rt_memset(parser, 0, sizeof(*parser));
    parser->ctx = ctx;
}

static void parser_destroy(WpParser *parser) {
    rt_free(parser->line);
    rt_free(parser->page);
    rt_memset(parser, 0, sizeof(*parser));
}

static int parser_process_line(WpParser *parser, const char *line, size_t line_size) {
    if (!parser->in_page && find_substr(line, "<page>") != 0) {
        parser->in_page = 1;
        parser->page_size = 0U;
    }
    if (parser->in_page && buffer_append(&parser->page, &parser->page_size, &parser->page_capacity, line, line_size) != 0) return -1;
    if (parser->in_page && find_substr(line, "</page>") != 0) {
        if (process_page(parser->ctx, parser->page) != 0) return -1;
        parser->in_page = 0;
    }
    return 0;
}

static int parser_feed(WpParser *parser, const unsigned char *data, size_t size) {
    size_t offset = 0U;

    while (offset < size) {
        size_t next = offset;

        while (next < size && data[next] != '\n') next += 1U;
        if (next < size) next += 1U;
        if (buffer_append(&parser->line, &parser->line_size, &parser->line_capacity, (const char *)(data + offset), next - offset) != 0) return -1;
        if (next > offset && data[next - 1U] == '\n') {
            if (parser_process_line(parser, parser->line, parser->line_size) != 0) return -1;
            parser->line_size = 0U;
            if (parser->line != 0) parser->line[0] = '\0';
        }
        offset = next;
    }
    return 0;
}

static int parser_finish(WpParser *parser) {
    if (parser->line_size != 0U) {
        if (parser_process_line(parser, parser->line, parser->line_size) != 0) return -1;
        parser->line_size = 0U;
    }
    return parser->in_page ? -1 : 0;
}

static int wp_bzip2_read_callback(void *context, unsigned char *buffer, size_t capacity, size_t *size_out) {
    WpInput *input = (WpInput *)context;
    long amount = platform_read(input->fd, buffer, capacity);

    if (amount < 0) return -1;
    *size_out = (size_t)amount;
    return 0;
}

static int wp_bzip2_write_callback(void *context, const unsigned char *data, size_t size) {
    return parser_feed((WpParser *)context, data, size);
}

static int extract_from_input(const WpDumpInput *input, const WpCiteOptions *options, ToolOutputBuffer *output, unsigned long long *records_out) {
    WpInput source;
    WpLineReader reader;
    WpExtractContext ctx;
    char *page = 0;
    size_t page_size = 0U;
    size_t page_capacity = 0U;
    int in_page = 0;
    int result = 0;

    if (!options->quiet) write_info("extracting ", input->name);
    if (input_open(&source, input->path) != 0) {
        tool_write_error("wp-cite-extract", "cannot open input: ", input->path);
        return -1;
    }
    line_reader_init(&reader, &source);
    rt_memset(&ctx, 0, sizeof(ctx));
    ctx.source_name = input->name;
    ctx.wiki = input->wiki;
    ctx.date = input->date;
    ctx.output = output;
    if (text_ends_with(input->path, ".bz2")) {
        WpParser parser;

        parser_init(&parser, &ctx);
        if (compression_bzip2_decompress_stream(wp_bzip2_read_callback, &source, wp_bzip2_write_callback, &parser) != 0 || parser_finish(&parser) != 0) {
            result = -1;
        }
        parser_destroy(&parser);
    } else for (;;) {
        char *line = 0;
        size_t line_size = 0U;
        int has_line = 0;

        if (line_reader_next(&reader, &line, &line_size, &has_line) != 0) {
            result = -1;
            break;
        }
        if (!has_line) break;
        if (!in_page && find_substr(line, "<page>") != 0) {
            in_page = 1;
            page_size = 0U;
        }
        if (in_page && buffer_append(&page, &page_size, &page_capacity, line, line_size) != 0) {
            result = -1;
            break;
        }
        if (in_page && find_substr(line, "</page>") != 0) {
            if (process_page(&ctx, page) != 0) {
                result = -1;
                break;
            }
            in_page = 0;
        }
    }
    rt_free(page);
    line_reader_destroy(&reader);
    if (input_close(&source) != 0) result = -1;
    if (!options->quiet) {
        char count_text[32];
        write_status_prefix();
        rt_write_cstr(2, "processed ");
        rt_unsigned_to_string(ctx.article_pages, count_text, sizeof(count_text));
        rt_write_cstr(2, count_text);
        rt_write_cstr(2, " article pages from ");
        rt_write_cstr(2, input->name);
        rt_write_cstr(2, ", records ");
        rt_unsigned_to_string(ctx.records_written, count_text, sizeof(count_text));
        rt_write_cstr(2, count_text);
        rt_write_char(2, '\n');
    }
    *records_out += ctx.records_written;
    return result;
}

static int write_header(ToolOutputBuffer *output) {
    return tool_output_buffer_write_cstr(output, "wiki\tsnapshot\tsource\tpage_id\tpage_title\tkind\tvalue\traw\n");
}

int main(int argc, char **argv) {
    ToolOptState state;
    WpCiteOptions options;
    WpDumpSet set;
    ToolOutputBuffer output;
    char output_path[WP_CITE_PATH_SIZE];
    int output_fd;
    int parse_result;
    size_t index;
    unsigned long long total_records = 0ULL;

    rt_memset(&options, 0, sizeof(options));
    options.data_dir = default_data_dir();
    tool_opt_init(&state, argc, argv, argv[0], "[-q] [-d DIR] [-i FILE] [-o FILE]");
    while ((parse_result = tool_opt_next(&state)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(state.flag, "-q") == 0 || rt_strcmp(state.flag, "--quiet") == 0) {
            options.quiet = 1;
        } else if (rt_strcmp(state.flag, "-d") == 0 || rt_strcmp(state.flag, "--data-dir") == 0) {
            if (tool_opt_require_value(&state) != 0) return 1;
            options.data_dir = state.value;
        } else if (tool_starts_with(state.flag, "--data-dir=")) {
            options.data_dir = state.flag + 11;
        } else if (rt_strcmp(state.flag, "-i") == 0 || rt_strcmp(state.flag, "--input") == 0) {
            if (tool_opt_require_value(&state) != 0) return 1;
            options.input_path = state.value;
        } else if (tool_starts_with(state.flag, "--input=")) {
            options.input_path = state.flag + 8;
        } else if (rt_strcmp(state.flag, "-o") == 0 || rt_strcmp(state.flag, "--output") == 0) {
            if (tool_opt_require_value(&state) != 0) return 1;
            options.output_path = state.value;
        } else if (tool_starts_with(state.flag, "--output=")) {
            options.output_path = state.flag + 9;
        } else {
            tool_write_error("wp-cite-extract", "unknown option: ", state.flag);
            print_usage(argv[0]);
            return 1;
        }
    }
    if (parse_result == TOOL_OPT_HELP) {
        print_usage(argv[0]);
        return 0;
    }
    if (parse_result == TOOL_OPT_ERROR || state.argi != argc) {
        print_usage(argv[0]);
        return 1;
    }
    if (options.input_path != 0) {
        if (set_single_input(options.input_path, &set) != 0) return 1;
    } else if (collect_latest_inputs(options.data_dir, &set) != 0) {
        tool_write_error("wp-cite-extract", "cannot find dump XML/XML.BZ2 in ", options.data_dir);
        return 1;
    }
    if (options.output_path != 0) {
        rt_copy_string(output_path, sizeof(output_path), options.output_path);
    } else if (make_default_output_path(options.data_dir, &set, output_path, sizeof(output_path)) != 0) {
        tool_write_error("wp-cite-extract", "output path too long", 0);
        return 1;
    }
    output_fd = platform_open_write(output_path, 0644U);
    if (output_fd < 0) {
        tool_write_error("wp-cite-extract", "cannot open output: ", output_path);
        return 1;
    }
    tool_output_buffer_init(&output, output_fd);
    if (write_header(&output) != 0) {
        platform_close(output_fd);
        return 1;
    }
    if (!options.quiet) {
        write_info("writing ", output_path);
    }
    for (index = 0U; index < set.count; ++index) {
        if (extract_from_input(&set.inputs[index], &options, &output, &total_records) != 0) {
            platform_close(output_fd);
            tool_write_error("wp-cite-extract", "extract failed for ", set.inputs[index].name);
            return 1;
        }
    }
    if (tool_output_buffer_flush(&output) != 0 || platform_close(output_fd) != 0) return 1;
    if (!options.quiet) {
        char count_text[32];
        write_status_prefix();
        rt_write_cstr(2, "wrote ");
        rt_unsigned_to_string(total_records, count_text, sizeof(count_text));
        rt_write_cstr(2, count_text);
        rt_write_cstr(2, " citation records\n");
    }
    return 0;
}
