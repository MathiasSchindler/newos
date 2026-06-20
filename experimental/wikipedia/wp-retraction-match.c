#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define WP_MATCH_LINE_SIZE 65536U
#define WP_MATCH_PATH_SIZE 1024U
#define WP_MATCH_NAME_SIZE 256U
#define WP_MATCH_KEY_SIZE 256U
#define WP_WEAK_KEY_SIZE 64U
#define WP_WEAK_TEXT_SIZE 2048U
#define WP_WEAK_TITLE_SIZE 512U
#define WP_WEAK_FIELD_SIZE 256U
#define WP_MATCH_PROGRESS_ROWS 5000000ULL

#define WP_RW_COL_RECORD_ID 0U
#define WP_RW_COL_TITLE 1U
#define WP_RW_COL_JOURNAL 4U
#define WP_RW_COL_PUBLISHER 5U
#define WP_RW_COL_AUTHOR 7U
#define WP_RW_COL_RETRACTION_DATE 10U
#define WP_RW_COL_RETRACTION_DOI 11U
#define WP_RW_COL_RETRACTION_PMID 12U
#define WP_RW_COL_ORIGINAL_DATE 13U
#define WP_RW_COL_ORIGINAL_DOI 14U
#define WP_RW_COL_ORIGINAL_PMID 15U
#define WP_RW_COL_NATURE 16U
#define WP_RW_COL_REASON 17U
#define WP_RW_COL_COUNT 21U

#define WP_CITE_COL_WIKI 0U
#define WP_CITE_COL_SNAPSHOT 1U
#define WP_CITE_COL_SOURCE 2U
#define WP_CITE_COL_PAGE_ID 3U
#define WP_CITE_COL_PAGE_TITLE 4U
#define WP_CITE_COL_KIND 5U
#define WP_CITE_COL_VALUE 6U
#define WP_CITE_COL_RAW 7U
#define WP_CITE_COL_COUNT 8U

#define WP_MATCH_KIND_DOI 1
#define WP_MATCH_KIND_PMID 2

#define WP_MATCH_OUTPUT_HEADER "match_kind\tmatch_value\tmatch_field\twiki\tsnapshot\tsource\tpage_id\tpage_title\tcitation_kind\tcitation_value\trw_record_id\trw_original_doi\trw_original_pmid\trw_retraction_doi\trw_retraction_pmid\trw_retraction_date\trw_original_date\trw_retraction_nature\trw_title\trw_journal\trw_publisher\trw_reason\traw\n"

typedef struct {
    const char *data_dir;
    const char *citations_path;
    const char *retractions_path;
    const char *output_path;
    const char *weak_output_path;
    const char *article_dedup_path;
    int quiet;
    int no_weak;
} WpMatchOptions;

typedef struct {
    char *record_id;
    char *title;
    char *journal;
    char *publisher;
    char *author;
    char *retraction_date;
    char *retraction_doi;
    char *retraction_pmid;
    char *original_date;
    char *original_doi;
    char *original_pmid;
    char *nature;
    char *reason;
    char *title_norm;
    char *journal_norm;
    char *author_norm;
    char year[5];
    unsigned long long weak_seen_generation;
} WpRetractionRecord;

typedef struct {
    char key[WP_MATCH_KEY_SIZE];
    int kind;
    const char *field_name;
    size_t record_index;
} WpRetractionKey;

typedef struct {
    size_t start_index;
    size_t end_index;
    unsigned long long hash;
    unsigned char occupied;
} WpRetractionHashEntry;

typedef struct {
    char key[WP_WEAK_KEY_SIZE];
    size_t record_index;
} WpWeakKey;

typedef struct {
    WpRetractionRecord *records;
    size_t record_count;
    size_t record_capacity;
    WpRetractionKey *keys;
    size_t key_count;
    size_t key_capacity;
    WpRetractionHashEntry *hash_entries;
    size_t hash_capacity;
    WpWeakKey *weak_keys;
    size_t weak_key_count;
    size_t weak_key_capacity;
} WpRetractionIndex;

typedef struct {
    char path[WP_MATCH_PATH_SIZE];
    char wiki[64];
    char date[16];
} WpCitationInput;

typedef struct {
    unsigned long long citation_rows;
    unsigned long long identifier_rows;
    unsigned long long lookup_rows;
    unsigned long long matched_citation_rows;
    unsigned long long match_output_rows;
} WpMatchStats;

typedef struct {
    const WpRetractionRecord *record;
    const WpRetractionKey *key;
    char *cite_fields[WP_CITE_COL_COUNT];
    unsigned long long evidence_count;
} WpArticleMatchGroup;

typedef struct {
    WpArticleMatchGroup *groups;
    size_t group_count;
    size_t group_capacity;
    unsigned long long row_count;
    unsigned long long doi_row_count;
    unsigned long long pmid_row_count;
} WpDuplicateAnalysis;

typedef struct {
    unsigned long long doi_groups;
    unsigned long long doi_duplicate_groups;
    unsigned long long doi_duplicate_extra_rows;
    unsigned long long pmid_groups;
    unsigned long long pmid_duplicate_groups;
    unsigned long long pmid_duplicate_extra_rows;
} WpDuplicateSummary;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-q] [-d DIR] [-c CITATIONS.tsv] [-r RETRACTION_WATCH.csv] [-o MATCHES.tsv] [-w WEAK.tsv] [--no-weak] [--article-dedup FILE]");
}

static void write_status_prefix(void) {
    char timestamp[32];

    if (platform_format_time(platform_get_epoch_time(), 1, "%Y-%m-%d %H:%M:%S", timestamp, sizeof(timestamp)) != 0) {
        rt_copy_string(timestamp, sizeof(timestamp), "0000-00-00 00:00:00");
    }
    rt_write_cstr(2, timestamp);
    rt_write_char(2, ' ');
}

static void write_info_u64(const char *prefix, unsigned long long value, const char *suffix) {
    char text[32];

    write_status_prefix();
    rt_write_cstr(2, prefix);
    rt_unsigned_to_string(value, text, sizeof(text));
    rt_write_cstr(2, text);
    if (suffix != 0) rt_write_cstr(2, suffix);
    rt_write_char(2, '\n');
}

static void write_match_progress(const WpMatchStats *stats) {
    char text[32];

    write_status_prefix();
    rt_write_cstr(2, "processed citation rows: ");
    rt_unsigned_to_string(stats->citation_rows, text, sizeof(text));
    rt_write_cstr(2, text);
    rt_write_cstr(2, "; identifier rows: ");
    rt_unsigned_to_string(stats->identifier_rows, text, sizeof(text));
    rt_write_cstr(2, text);
    rt_write_cstr(2, "; hard match rows: ");
    rt_unsigned_to_string(stats->match_output_rows, text, sizeof(text));
    rt_write_cstr(2, text);
    rt_write_char(2, '\n');
}

static int text_ends_with(const char *text, const char *suffix) {
    size_t text_length = rt_strlen(text);
    size_t suffix_length = rt_strlen(suffix);

    if (suffix_length > text_length) return 0;
    return rt_strcmp(text + text_length - suffix_length, suffix) == 0;
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

static char *copy_field(const char *text) {
    size_t length = rt_strlen(text);
    char *copy = (char *)rt_malloc(length + 1U);

    if (copy == 0) return 0;
    memcpy(copy, text, length + 1U);
    return copy;
}

static void trim_record_line(char *line) {
    size_t length = rt_strlen(line);

    while (length > 0U && (line[length - 1U] == '\r' || line[length - 1U] == '\n')) {
        line[--length] = '\0';
    }
}

static int starts_with_case(const char *text, const char *prefix) {
    size_t index;

    for (index = 0U; prefix[index] != '\0'; ++index) {
        if (tool_ascii_tolower(text[index]) != tool_ascii_tolower(prefix[index])) return 0;
    }
    return 1;
}

static int span_equals_cstr(const char *text, size_t length, const char *literal) {
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (literal[index] == '\0' || text[index] != literal[index]) return 0;
    }
    return literal[length] == '\0';
}

static int citation_kind_from_row_prefix(const char *line, int *hard_kind_out, int *weak_candidate_out) {
    size_t column = 0U;
    size_t start = 0U;
    size_t index = 0U;
    const char *kind = 0;
    size_t kind_length = 0U;

    *hard_kind_out = 0;
    *weak_candidate_out = 0;
    while (line[index] != '\0') {
        if (line[index] == '\t') {
            if (column == WP_CITE_COL_KIND) {
                kind = line + start;
                kind_length = index - start;
            } else if (column == WP_CITE_COL_VALUE) {
                if (kind == 0) return -1;
                if (span_equals_cstr(kind, kind_length, "doi")) *hard_kind_out = WP_MATCH_KIND_DOI;
                else if (span_equals_cstr(kind, kind_length, "pmid")) *hard_kind_out = WP_MATCH_KIND_PMID;
                else if (span_equals_cstr(kind, kind_length, "template") || span_equals_cstr(kind, kind_length, "ref")) *weak_candidate_out = 1;
                return 0;
            }
            column += 1U;
            start = index + 1U;
        }
        index += 1U;
    }
    return -1;
}

static int is_doi_trailing_punctuation(char ch) {
    return ch == '.' || ch == ',' || ch == ')' || ch == ']' || ch == '}' || ch == '>' || ch == '"' || ch == '\'';
}

static int normalize_doi(const char *input, char *out, size_t out_size) {
    const char *start = input;
    size_t length;
    size_t index;
    size_t out_index = 0U;

    while (*start == ' ' || *start == '\t') start += 1U;
    if (starts_with_case(start, "https://doi.org/")) start += 16U;
    else if (starts_with_case(start, "http://doi.org/")) start += 15U;
    else if (starts_with_case(start, "https://dx.doi.org/")) start += 19U;
    else if (starts_with_case(start, "http://dx.doi.org/")) start += 18U;
    else if (starts_with_case(start, "doi.org/")) start += 8U;
    else if (starts_with_case(start, "dx.doi.org/")) start += 11U;
    else if (starts_with_case(start, "doi:")) start += 4U;

    length = rt_strlen(start);
    while (length > 0U && (start[length - 1U] == ' ' || start[length - 1U] == '\t' || is_doi_trailing_punctuation(start[length - 1U]))) {
        length -= 1U;
    }
    if (length < 6U || length + 1U > out_size) return -1;

    for (index = 0U; index < length; ++index) {
        unsigned char ch = (unsigned char)start[index];

        if (ch <= ' ' || ch == '\t') return -1;
        out[out_index++] = tool_ascii_tolower((char)ch);
    }
    out[out_index] = '\0';
    return starts_with_case(out, "10.") ? 0 : -1;
}

static int normalize_pmid(const char *input, char *out, size_t out_size) {
    size_t index = 0U;
    size_t out_index = 0U;
    int non_zero = 0;

    while (input[index] != '\0') {
        if (tool_ascii_is_digit(input[index])) {
            if (out_index + 1U >= out_size) return -1;
            if (input[index] != '0') non_zero = 1;
            out[out_index++] = input[index];
        } else if (input[index] != ' ' && input[index] != '\t') {
            return -1;
        }
        index += 1U;
    }
    out[out_index] = '\0';
    return out_index > 0U && non_zero ? 0 : -1;
}

static int normalize_match_text(const char *input, char *out, size_t out_size) {
    size_t in_index = 0U;
    size_t out_index = 0U;
    int pending_space = 0;

    if (out_size == 0U) return -1;
    while (input[in_index] != '\0') {
        unsigned char ch = (unsigned char)input[in_index++];

        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            if (pending_space && out_index > 0U) {
                if (out_index + 1U >= out_size) break;
                out[out_index++] = ' ';
            }
            if (out_index + 1U >= out_size) break;
            out[out_index++] = tool_ascii_tolower((char)ch);
            pending_space = 0;
        } else {
            pending_space = out_index > 0U;
        }
    }
    out[out_index] = '\0';
    return 0;
}

static size_t normalized_word_count(const char *text) {
    size_t count = 0U;
    size_t index = 0U;
    int in_word = 0;

    while (text[index] != '\0') {
        if (text[index] == ' ') {
            in_word = 0;
        } else if (!in_word) {
            count += 1U;
            in_word = 1;
        }
        index += 1U;
    }
    return count;
}

static int weak_title_is_usable(const char *title_norm) {
    return rt_strlen(title_norm) >= 16U && normalized_word_count(title_norm) >= 3U;
}

static int weak_field_is_usable(const char *field_norm) {
    return field_norm != 0 && rt_strlen(field_norm) >= 4U;
}

static int weak_word_is_stopword(const char *word, size_t length) {
    static const char *stopwords[] = {
        "about", "after", "analysis", "article", "based", "between", "case", "clinical",
        "data", "during", "effect", "effects", "from", "into", "journal", "method",
        "methods", "model", "paper", "patients", "research", "results", "study",
        "system", "using", "with", "without"
    };
    size_t i;

    if (length <= 5U) return 1;
    for (i = 0U; i < sizeof(stopwords) / sizeof(stopwords[0]); ++i) {
        if (rt_strlen(stopwords[i]) == length && rt_strncmp(word, stopwords[i], length) == 0) return 1;
    }
    return 0;
}

static int choose_title_anchor(const char *title_norm, char *out, size_t out_size) {
    size_t index = 0U;
    const char *best = 0;
    size_t best_length = 0U;

    while (title_norm[index] != '\0') {
        size_t start;
        size_t length;

        while (title_norm[index] == ' ') index += 1U;
        start = index;
        while (title_norm[index] != '\0' && title_norm[index] != ' ') index += 1U;
        length = index - start;
        if (length > best_length && !weak_word_is_stopword(title_norm + start, length)) {
            best = title_norm + start;
            best_length = length;
        }
    }
    if (best == 0 || best_length + 1U > out_size) return -1;
    memcpy(out, best, best_length);
    out[best_length] = '\0';
    return 0;
}

static int normalized_contains_phrase(const char *text, const char *phrase) {
    size_t text_length = rt_strlen(text);
    size_t phrase_length = rt_strlen(phrase);
    size_t index;

    if (phrase_length == 0U || phrase_length > text_length) return 0;
    for (index = 0U; index + phrase_length <= text_length; ++index) {
        if (index > 0U && text[index - 1U] != ' ') continue;
        if (index + phrase_length < text_length && text[index + phrase_length] != ' ') continue;
        if (rt_strncmp(text + index, phrase, phrase_length) == 0) return 1;
    }
    return 0;
}

static int normalized_contains_significant_author(const char *text, const char *author_norm) {
    size_t index = 0U;

    if (author_norm == 0) return 0;
    while (author_norm[index] != '\0') {
        size_t start;
        size_t length;
        char word[WP_WEAK_KEY_SIZE];

        while (author_norm[index] == ' ') index += 1U;
        start = index;
        while (author_norm[index] != '\0' && author_norm[index] != ' ') index += 1U;
        length = index - start;
        if (length >= 4U && length < sizeof(word) && !weak_word_is_stopword(author_norm + start, length)) {
            memcpy(word, author_norm + start, length);
            word[length] = '\0';
            if (normalized_contains_phrase(text, word)) return 1;
        }
    }
    return 0;
}

static int extract_year(const char *input, char out[5]) {
    size_t index;
    size_t length = rt_strlen(input);

    out[0] = '\0';
    for (index = 0U; index + 4U <= length; ++index) {
        if ((input[index] == '1' || input[index] == '2') &&
            tool_ascii_is_digit(input[index + 1U]) &&
            tool_ascii_is_digit(input[index + 2U]) &&
            tool_ascii_is_digit(input[index + 3U])) {
            unsigned int year = (unsigned int)(input[index] - '0') * 1000U +
                (unsigned int)(input[index + 1U] - '0') * 100U +
                (unsigned int)(input[index + 2U] - '0') * 10U +
                (unsigned int)(input[index + 3U] - '0');

            if (year >= 1800U && year <= 2099U) {
                memcpy(out, input + index, 4U);
                out[4] = '\0';
                return 0;
            }
        }
    }
    return -1;
}

static int split_tsv_fields(char *line, char **fields, size_t max_fields, size_t *count_out) {
    size_t count = 0U;
    char *cursor = line;

    while (count < max_fields) {
        fields[count++] = cursor;
        while (*cursor != '\0' && *cursor != '\t') cursor += 1U;
        if (*cursor == '\0') {
            *count_out = count;
            return 0;
        }
        *cursor++ = '\0';
    }
    while (*cursor != '\0') {
        if (*cursor == '\t') return -1;
        cursor += 1U;
    }
    *count_out = count;
    return 0;
}

static int split_csv_fields(char *line, char **fields, size_t max_fields, size_t *count_out) {
    char *cursor = line;
    size_t count = 0U;
    int ended_with_comma = 0;

    while (*cursor != '\0' && count < max_fields) {
        char *field = cursor;
        char *out = cursor;

        ended_with_comma = 0;
        if (*cursor == '"') {
            cursor += 1U;
            field = out = cursor;
            for (;;) {
                if (*cursor == '\0') return -1;
                if (*cursor == '"') {
                    if (cursor[1] == '"') {
                        *out++ = '"';
                        cursor += 2U;
                        continue;
                    }
                    cursor += 1U;
                    break;
                }
                *out++ = *cursor++;
            }
            if (*cursor != ',' && *cursor != '\0') return -1;
        } else {
            while (*cursor != '\0' && *cursor != ',') {
                *out++ = *cursor++;
            }
        }
        if (*cursor == ',') {
            cursor += 1U;
            ended_with_comma = 1;
        }
        *out = '\0';
        fields[count++] = field;
    }
    if (*cursor == '\0' && ended_with_comma && count < max_fields) {
        fields[count++] = cursor;
    }
    if (*cursor != '\0') return -1;
    *count_out = count;
    return 0;
}

static void retraction_index_destroy(WpRetractionIndex *index) {
    size_t i;

    for (i = 0U; i < index->record_count; ++i) {
        rt_free(index->records[i].record_id);
        rt_free(index->records[i].title);
        rt_free(index->records[i].journal);
        rt_free(index->records[i].publisher);
        rt_free(index->records[i].author);
        rt_free(index->records[i].retraction_date);
        rt_free(index->records[i].retraction_doi);
        rt_free(index->records[i].retraction_pmid);
        rt_free(index->records[i].original_date);
        rt_free(index->records[i].original_doi);
        rt_free(index->records[i].original_pmid);
        rt_free(index->records[i].nature);
        rt_free(index->records[i].reason);
        rt_free(index->records[i].title_norm);
        rt_free(index->records[i].journal_norm);
        rt_free(index->records[i].author_norm);
    }
    rt_free(index->records);
    rt_free(index->keys);
    rt_free(index->hash_entries);
    rt_free(index->weak_keys);
    rt_memset(index, 0, sizeof(*index));
}

static int ensure_record_capacity(WpRetractionIndex *index) {
    if (index->record_count == index->record_capacity) {
        size_t next_capacity = index->record_capacity == 0U ? 1024U : index->record_capacity * 2U;
        WpRetractionRecord *next = (WpRetractionRecord *)rt_realloc_array(index->records, next_capacity, sizeof(index->records[0]));

        if (next == 0) return -1;
        index->records = next;
        index->record_capacity = next_capacity;
    }
    return 0;
}

static int ensure_key_capacity(WpRetractionIndex *index) {
    if (index->key_count == index->key_capacity) {
        size_t next_capacity = index->key_capacity == 0U ? 2048U : index->key_capacity * 2U;
        WpRetractionKey *next = (WpRetractionKey *)rt_realloc_array(index->keys, next_capacity, sizeof(index->keys[0]));

        if (next == 0) return -1;
        index->keys = next;
        index->key_capacity = next_capacity;
    }
    return 0;
}

static int ensure_weak_key_capacity(WpRetractionIndex *index) {
    if (index->weak_key_count == index->weak_key_capacity) {
        size_t next_capacity = index->weak_key_capacity == 0U ? 2048U : index->weak_key_capacity * 2U;
        WpWeakKey *next = (WpWeakKey *)rt_realloc_array(index->weak_keys, next_capacity, sizeof(index->weak_keys[0]));

        if (next == 0) return -1;
        index->weak_keys = next;
        index->weak_key_capacity = next_capacity;
    }
    return 0;
}

static char *copy_normalized_field(const char *text, size_t buffer_size) {
    char *copy = (char *)rt_malloc(buffer_size);

    if (copy == 0) return 0;
    if (normalize_match_text(text, copy, buffer_size) != 0) {
        rt_free(copy);
        return 0;
    }
    return copy;
}

static int copy_record_fields(WpRetractionRecord *record, char **fields) {
    record->record_id = copy_field(fields[WP_RW_COL_RECORD_ID]);
    record->title = copy_field(fields[WP_RW_COL_TITLE]);
    record->journal = copy_field(fields[WP_RW_COL_JOURNAL]);
    record->publisher = copy_field(fields[WP_RW_COL_PUBLISHER]);
    record->author = copy_field(fields[WP_RW_COL_AUTHOR]);
    record->retraction_date = copy_field(fields[WP_RW_COL_RETRACTION_DATE]);
    record->retraction_doi = copy_field(fields[WP_RW_COL_RETRACTION_DOI]);
    record->retraction_pmid = copy_field(fields[WP_RW_COL_RETRACTION_PMID]);
    record->original_date = copy_field(fields[WP_RW_COL_ORIGINAL_DATE]);
    record->original_doi = copy_field(fields[WP_RW_COL_ORIGINAL_DOI]);
    record->original_pmid = copy_field(fields[WP_RW_COL_ORIGINAL_PMID]);
    record->nature = copy_field(fields[WP_RW_COL_NATURE]);
    record->reason = copy_field(fields[WP_RW_COL_REASON]);
    record->title_norm = copy_normalized_field(fields[WP_RW_COL_TITLE], WP_WEAK_TITLE_SIZE);
    record->journal_norm = copy_normalized_field(fields[WP_RW_COL_JOURNAL], WP_WEAK_FIELD_SIZE);
    record->author_norm = copy_normalized_field(fields[WP_RW_COL_AUTHOR], WP_WEAK_FIELD_SIZE);
    (void)extract_year(fields[WP_RW_COL_ORIGINAL_DATE], record->year);
    return record->record_id != 0 && record->title != 0 && record->journal != 0 && record->publisher != 0 &&
        record->author != 0 && record->retraction_date != 0 && record->retraction_doi != 0 && record->retraction_pmid != 0 &&
        record->original_date != 0 && record->original_doi != 0 && record->original_pmid != 0 &&
        record->nature != 0 && record->reason != 0 && record->title_norm != 0 &&
        record->journal_norm != 0 && record->author_norm != 0 ? 0 : -1;
}

static int add_key(WpRetractionIndex *index, int kind, const char *field_name, const char *key, size_t record_index) {
    WpRetractionKey *entry;

    if (ensure_key_capacity(index) != 0) return -1;
    entry = &index->keys[index->key_count++];
    rt_copy_string(entry->key, sizeof(entry->key), key);
    if (rt_strlen(entry->key) != rt_strlen(key)) return -1;
    entry->kind = kind;
    entry->field_name = field_name;
    entry->record_index = record_index;
    return 0;
}

static int add_weak_key(WpRetractionIndex *index, const char *key, size_t record_index) {
    WpWeakKey *entry;

    if (ensure_weak_key_capacity(index) != 0) return -1;
    entry = &index->weak_keys[index->weak_key_count++];
    rt_copy_string(entry->key, sizeof(entry->key), key);
    if (rt_strlen(entry->key) != rt_strlen(key)) return -1;
    entry->record_index = record_index;
    return 0;
}

static int add_record(WpRetractionIndex *index, char **fields) {
    WpRetractionRecord *record;
    char key[WP_MATCH_KEY_SIZE];
    char weak_key[WP_WEAK_KEY_SIZE];
    size_t record_index;

    if (ensure_record_capacity(index) != 0) return -1;
    record_index = index->record_count;
    record = &index->records[index->record_count++];
    rt_memset(record, 0, sizeof(*record));
    if (copy_record_fields(record, fields) != 0) return -1;

    if (normalize_doi(fields[WP_RW_COL_ORIGINAL_DOI], key, sizeof(key)) == 0) {
        if (add_key(index, WP_MATCH_KIND_DOI, "OriginalPaperDOI", key, record_index) != 0) return -1;
    }
    if (normalize_pmid(fields[WP_RW_COL_ORIGINAL_PMID], key, sizeof(key)) == 0) {
        if (add_key(index, WP_MATCH_KIND_PMID, "OriginalPaperPubMedID", key, record_index) != 0) return -1;
    }
    if (weak_title_is_usable(record->title_norm) && choose_title_anchor(record->title_norm, weak_key, sizeof(weak_key)) == 0) {
        if (add_weak_key(index, weak_key, record_index) != 0) return -1;
    }
    return 0;
}

static int compare_keys(const void *left_ptr, const void *right_ptr) {
    const WpRetractionKey *left = (const WpRetractionKey *)left_ptr;
    const WpRetractionKey *right = (const WpRetractionKey *)right_ptr;
    int cmp;

    if (left->kind != right->kind) return left->kind < right->kind ? -1 : 1;
    cmp = rt_strcmp(left->key, right->key);
    if (cmp != 0) return cmp;
    if (left->record_index == right->record_index) return 0;
    return left->record_index < right->record_index ? -1 : 1;
}

static unsigned long long hash_retraction_key(int kind, const char *key) {
    unsigned long long hash = 1469598103934665603ULL;
    size_t index;

    hash ^= (unsigned long long)(unsigned int)kind;
    hash *= 1099511628211ULL;
    for (index = 0U; key[index] != '\0'; ++index) {
        hash ^= (unsigned long long)(unsigned char)key[index];
        hash *= 1099511628211ULL;
    }
    return hash != 0ULL ? hash : 1ULL;
}

static int build_key_hash(WpRetractionIndex *index) {
    size_t unique_count = 0U;
    size_t capacity = 1U;
    size_t pos;
    WpRetractionHashEntry *entries;

    rt_free(index->hash_entries);
    index->hash_entries = 0;
    index->hash_capacity = 0U;
    if (index->key_count == 0U) return 0;

    for (pos = 0U; pos < index->key_count;) {
        size_t end = pos + 1U;
        while (end < index->key_count &&
               index->keys[end].kind == index->keys[pos].kind &&
               rt_strcmp(index->keys[end].key, index->keys[pos].key) == 0) {
            end += 1U;
        }
        unique_count += 1U;
        pos = end;
    }
    if (unique_count > ((size_t)-1) / 2U) return -1;
    while (capacity < unique_count * 2U) {
        if (capacity > ((size_t)-1) / 2U) return -1;
        capacity *= 2U;
    }
    if (capacity > ((size_t)-1) / sizeof(entries[0])) return -1;

    entries = (WpRetractionHashEntry *)rt_malloc_array(capacity, sizeof(entries[0]));
    if (entries == 0) return -1;
    rt_memset(entries, 0, capacity * sizeof(entries[0]));

    for (pos = 0U; pos < index->key_count;) {
        size_t end = pos + 1U;
        unsigned long long hash = hash_retraction_key(index->keys[pos].kind, index->keys[pos].key);
        size_t slot = (size_t)(hash & (unsigned long long)(capacity - 1U));

        while (end < index->key_count &&
               index->keys[end].kind == index->keys[pos].kind &&
               rt_strcmp(index->keys[end].key, index->keys[pos].key) == 0) {
            end += 1U;
        }
        while (entries[slot].occupied) slot = (slot + 1U) & (capacity - 1U);
        entries[slot].start_index = pos;
        entries[slot].end_index = end;
        entries[slot].hash = hash;
        entries[slot].occupied = 1U;
        pos = end;
    }

    index->hash_entries = entries;
    index->hash_capacity = capacity;
    return 0;
}

static int compare_weak_keys(const void *left_ptr, const void *right_ptr) {
    const WpWeakKey *left = (const WpWeakKey *)left_ptr;
    const WpWeakKey *right = (const WpWeakKey *)right_ptr;
    int cmp = rt_strcmp(left->key, right->key);

    if (cmp != 0) return cmp;
    if (left->record_index == right->record_index) return 0;
    return left->record_index < right->record_index ? -1 : 1;
}

static int load_retractions(const char *path, WpRetractionIndex *index) {
    int fd = platform_open_read(path);
    ToolRecordReader reader;
    char line[WP_MATCH_LINE_SIZE];
    int has_record = 0;
    int saw_header = 0;

    rt_memset(index, 0, sizeof(*index));
    if (fd < 0) {
        tool_write_error("wp-retraction-match", "cannot open Retraction Watch CSV: ", path);
        return -1;
    }
    tool_record_reader_init(&reader, fd, '\n');
    while (tool_record_reader_next(&reader, line, sizeof(line), &has_record) == 0 && has_record) {
        char *fields[WP_RW_COL_COUNT + 4U];
        size_t field_count = 0U;

        trim_record_line(line);
        if (!saw_header) {
            saw_header = 1;
            continue;
        }
        if (split_csv_fields(line, fields, sizeof(fields) / sizeof(fields[0]), &field_count) != 0 || field_count < WP_RW_COL_COUNT) {
            platform_close(fd);
            tool_write_error("wp-retraction-match", "invalid Retraction Watch CSV row", 0);
            return -1;
        }
        if (add_record(index, fields) != 0) {
            platform_close(fd);
            tool_write_error("wp-retraction-match", "out of memory while indexing Retraction Watch CSV", 0);
            return -1;
        }
    }
    if (platform_close(fd) != 0 || !saw_header) return -1;
    rt_sort(index->keys, index->key_count, sizeof(index->keys[0]), compare_keys);
    if (build_key_hash(index) != 0) return -1;
    rt_sort(index->weak_keys, index->weak_key_count, sizeof(index->weak_keys[0]), compare_weak_keys);
    return 0;
}

static int compare_search_key(const WpRetractionKey *entry, int kind, const char *key) {
    int cmp;

    if (entry->kind != kind) return entry->kind < kind ? -1 : 1;
    cmp = rt_strcmp(entry->key, key);
    if (cmp < 0) return -1;
    if (cmp > 0) return 1;
    return 0;
}

static size_t lower_bound_key(const WpRetractionIndex *index, int kind, const char *key) {
    size_t left = 0U;
    size_t right = index->key_count;

    while (left < right) {
        size_t mid = left + (right - left) / 2U;
        if (compare_search_key(&index->keys[mid], kind, key) < 0) left = mid + 1U;
        else right = mid;
    }
    return left;
}

static int find_key_range(const WpRetractionIndex *index, int kind, const char *key, size_t *start_out, size_t *end_out) {
    if (index->hash_entries != 0 && index->hash_capacity != 0U) {
        unsigned long long hash = hash_retraction_key(kind, key);
        size_t slot = (size_t)(hash & (unsigned long long)(index->hash_capacity - 1U));

        while (index->hash_entries[slot].occupied) {
            const WpRetractionHashEntry *entry = &index->hash_entries[slot];
            if (entry->hash == hash && compare_search_key(&index->keys[entry->start_index], kind, key) == 0) {
                *start_out = entry->start_index;
                *end_out = entry->end_index;
                return 1;
            }
            slot = (slot + 1U) & (index->hash_capacity - 1U);
        }
        return 0;
    }

    *start_out = lower_bound_key(index, kind, key);
    *end_out = *start_out;
    while (*end_out < index->key_count && compare_search_key(&index->keys[*end_out], kind, key) == 0) {
        *end_out += 1U;
    }
    return *start_out != *end_out;
}

static int compare_weak_search_key(const WpWeakKey *entry, const char *key) {
    int cmp = rt_strcmp(entry->key, key);

    if (cmp < 0) return -1;
    if (cmp > 0) return 1;
    return 0;
}

static size_t lower_bound_weak_key(const WpRetractionIndex *index, const char *key) {
    size_t left = 0U;
    size_t right = index->weak_key_count;

    while (left < right) {
        size_t mid = left + (right - left) / 2U;
        if (compare_weak_search_key(&index->weak_keys[mid], key) < 0) left = mid + 1U;
        else right = mid;
    }
    return left;
}

static int write_tsv_text(ToolOutputBuffer *output, const char *text) {
    size_t index;
    size_t start = 0U;

    for (index = 0U; text[index] != '\0'; ++index) {
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

static int write_tsv_field(ToolOutputBuffer *output, const char *text) {
    return write_tsv_text(output, text != 0 ? text : "");
}

static int write_tsv_sep(ToolOutputBuffer *output) {
    return tool_output_buffer_write_char(output, '\t');
}

static int write_tsv_u64(ToolOutputBuffer *output, unsigned long long value) {
    char text[32];

    rt_unsigned_to_string(value, text, sizeof(text));
    return write_tsv_field(output, text);
}

static int write_match_record(
    ToolOutputBuffer *output,
    const WpRetractionRecord *record,
    const WpRetractionKey *key,
    char **cite_fields
) {
    const char *match_kind = key->kind == WP_MATCH_KIND_DOI ? "doi" : "pmid";

    if (write_tsv_field(output, match_kind) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, key->key) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, key->field_name) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_WIKI]) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_SNAPSHOT]) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_SOURCE]) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_PAGE_ID]) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_PAGE_TITLE]) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_KIND]) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_VALUE]) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->record_id) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->original_doi) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->original_pmid) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->retraction_doi) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->retraction_pmid) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->retraction_date) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->original_date) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->nature) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->title) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->journal) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->publisher) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->reason) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_RAW]) != 0 ||
        tool_output_buffer_write_char(output, '\n') != 0) {
        return -1;
    }
    return 0;
}

static size_t append_signal(char *signals, size_t signals_size, size_t length, const char *signal) {
    if (length > 0U) length = tool_buffer_append_char(signals, signals_size, length, '+');
    return tool_buffer_append_cstr(signals, signals_size, length, signal);
}

static int weak_match_signals(const WpRetractionRecord *record, const char *raw_norm, char *signals, size_t signals_size) {
    size_t length = 0U;
    int has_extra = 0;

    if (!weak_title_is_usable(record->title_norm) || !normalized_contains_phrase(raw_norm, record->title_norm)) return 0;
    length = append_signal(signals, signals_size, length, "title");
    if (weak_field_is_usable(record->journal_norm) && normalized_contains_phrase(raw_norm, record->journal_norm)) {
        length = append_signal(signals, signals_size, length, "journal");
        has_extra = 1;
    }
    if (record->year[0] != '\0' && normalized_contains_phrase(raw_norm, record->year)) {
        length = append_signal(signals, signals_size, length, "year");
        has_extra = 1;
    }
    if (normalized_contains_significant_author(raw_norm, record->author_norm)) {
        length = append_signal(signals, signals_size, length, "author");
        has_extra = 1;
    }
    if (rt_strlen(signals) != length) return 0;
    return has_extra;
}

static int write_weak_match_record(
    ToolOutputBuffer *output,
    const WpRetractionRecord *record,
    const char *signals,
    char **cite_fields
) {
    if (write_tsv_field(output, "weak-title") != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->title) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, signals) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_WIKI]) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_SNAPSHOT]) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_SOURCE]) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_PAGE_ID]) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_PAGE_TITLE]) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_KIND]) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_VALUE]) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->record_id) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->original_doi) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->original_pmid) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->retraction_doi) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->retraction_pmid) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->retraction_date) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->original_date) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->nature) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->title) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->journal) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->author) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->publisher) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, record->reason) != 0 || write_tsv_sep(output) != 0 ||
        write_tsv_field(output, cite_fields[WP_CITE_COL_RAW]) != 0 ||
        tool_output_buffer_write_char(output, '\n') != 0) {
        return -1;
    }
    return 0;
}

static int write_output_header(ToolOutputBuffer *output) {
    return tool_output_buffer_write_cstr(output, WP_MATCH_OUTPUT_HEADER);
}

static int write_weak_output_header(ToolOutputBuffer *output) {
    return tool_output_buffer_write_cstr(output, "match_kind\tmatch_value\tmatch_signals\twiki\tsnapshot\tsource\tpage_id\tpage_title\tcitation_kind\tcitation_value\trw_record_id\trw_original_doi\trw_original_pmid\trw_retraction_doi\trw_retraction_pmid\trw_retraction_date\trw_original_date\trw_retraction_nature\trw_title\trw_journal\trw_author\trw_publisher\trw_reason\traw\n");
}

static void article_match_group_free_fields(WpArticleMatchGroup *group) {
    size_t index;

    for (index = 0U; index < WP_CITE_COL_COUNT; ++index) {
        rt_free(group->cite_fields[index]);
        group->cite_fields[index] = 0;
    }
}

static void duplicate_analysis_destroy(WpDuplicateAnalysis *analysis) {
    size_t index;

    for (index = 0U; index < analysis->group_count; ++index) {
        article_match_group_free_fields(&analysis->groups[index]);
    }
    rt_free(analysis->groups);
    rt_memset(analysis, 0, sizeof(*analysis));
}

static int ensure_article_group_capacity(WpDuplicateAnalysis *analysis) {
    if (analysis->group_count == analysis->group_capacity) {
        size_t next_capacity = analysis->group_capacity == 0U ? 128U : analysis->group_capacity * 2U;
        WpArticleMatchGroup *next = (WpArticleMatchGroup *)rt_realloc_array(analysis->groups, next_capacity, sizeof(analysis->groups[0]));

        if (next == 0) return -1;
        analysis->groups = next;
        analysis->group_capacity = next_capacity;
    }
    return 0;
}

static int article_group_matches(const WpArticleMatchGroup *group, const WpRetractionRecord *record, const WpRetractionKey *key, char **cite_fields) {
    return group->record == record &&
        group->key->kind == key->kind &&
        rt_strcmp(group->key->key, key->key) == 0 &&
        rt_strcmp(group->cite_fields[WP_CITE_COL_WIKI], cite_fields[WP_CITE_COL_WIKI]) == 0 &&
        rt_strcmp(group->cite_fields[WP_CITE_COL_SNAPSHOT], cite_fields[WP_CITE_COL_SNAPSHOT]) == 0 &&
        rt_strcmp(group->cite_fields[WP_CITE_COL_PAGE_ID], cite_fields[WP_CITE_COL_PAGE_ID]) == 0;
}

static WpArticleMatchGroup *find_article_group(WpDuplicateAnalysis *analysis, const WpRetractionRecord *record, const WpRetractionKey *key, char **cite_fields) {
    size_t index;

    for (index = 0U; index < analysis->group_count; ++index) {
        if (article_group_matches(&analysis->groups[index], record, key, cite_fields)) return &analysis->groups[index];
    }
    return 0;
}

static int copy_cite_fields(WpArticleMatchGroup *group, char **cite_fields) {
    size_t index;

    for (index = 0U; index < WP_CITE_COL_COUNT; ++index) {
        group->cite_fields[index] = copy_field(cite_fields[index]);
        if (group->cite_fields[index] == 0) {
            article_match_group_free_fields(group);
            return -1;
        }
    }
    return 0;
}

static int duplicate_analysis_note_match(WpDuplicateAnalysis *analysis, const WpRetractionRecord *record, const WpRetractionKey *key, char **cite_fields) {
    WpArticleMatchGroup *group = find_article_group(analysis, record, key, cite_fields);

    if (group == 0) {
        if (ensure_article_group_capacity(analysis) != 0) return -1;
        group = &analysis->groups[analysis->group_count];
        rt_memset(group, 0, sizeof(*group));
        group->record = record;
        group->key = key;
        if (copy_cite_fields(group, cite_fields) != 0) return -1;
        analysis->group_count += 1U;
    }

    group->evidence_count += 1ULL;
    analysis->row_count += 1ULL;
    if (key->kind == WP_MATCH_KIND_DOI) analysis->doi_row_count += 1ULL;
    else if (key->kind == WP_MATCH_KIND_PMID) analysis->pmid_row_count += 1ULL;
    return 0;
}

static void duplicate_analysis_summarize(const WpDuplicateAnalysis *analysis, WpDuplicateSummary *summary) {
    size_t index;

    rt_memset(summary, 0, sizeof(*summary));
    for (index = 0U; index < analysis->group_count; ++index) {
        const WpArticleMatchGroup *group = &analysis->groups[index];
        unsigned long long extra_rows = group->evidence_count > 0ULL ? group->evidence_count - 1ULL : 0ULL;

        if (group->key->kind == WP_MATCH_KIND_DOI) {
            summary->doi_groups += 1ULL;
            if (extra_rows > 0ULL) {
                summary->doi_duplicate_groups += 1ULL;
                summary->doi_duplicate_extra_rows += extra_rows;
            }
        } else if (group->key->kind == WP_MATCH_KIND_PMID) {
            summary->pmid_groups += 1ULL;
            if (extra_rows > 0ULL) {
                summary->pmid_duplicate_groups += 1ULL;
                summary->pmid_duplicate_extra_rows += extra_rows;
            }
        }
    }
}

static void write_duplicate_summary(const WpDuplicateAnalysis *analysis) {
    WpDuplicateSummary summary;

    duplicate_analysis_summarize(analysis, &summary);
    write_info_u64("matched hard DOI rows: ", analysis->doi_row_count, 0);
    write_info_u64("matched hard PMID rows: ", analysis->pmid_row_count, 0);
    write_info_u64("article-level hard DOI groups: ", summary.doi_groups, 0);
    write_info_u64("article-level hard DOI duplicate groups: ", summary.doi_duplicate_groups, 0);
    write_info_u64("article-level hard DOI duplicate extra rows: ", summary.doi_duplicate_extra_rows, 0);
    write_info_u64("article-level hard PMID groups: ", summary.pmid_groups, 0);
    write_info_u64("article-level hard PMID duplicate groups: ", summary.pmid_duplicate_groups, 0);
    write_info_u64("article-level hard PMID duplicate extra rows: ", summary.pmid_duplicate_extra_rows, 0);
}

static int write_article_dedup_header(ToolOutputBuffer *output) {
    return tool_output_buffer_write_cstr(output, "evidence_row_count\t") != 0 ||
        tool_output_buffer_write_cstr(output, WP_MATCH_OUTPUT_HEADER) != 0 ? -1 : 0;
}

static int write_article_group_record(ToolOutputBuffer *output, const WpArticleMatchGroup *group) {
    if (write_tsv_u64(output, group->evidence_count) != 0 || write_tsv_sep(output) != 0) return -1;
    return write_match_record(output, group->record, group->key, (char **)group->cite_fields);
}

static int write_article_dedup_output(const char *path, const WpDuplicateAnalysis *analysis) {
    int fd;
    int should_close = 1;
    ToolOutputBuffer output;
    size_t index;

    if (path[0] == '-' && path[1] == '\0') {
        fd = 1;
        should_close = 0;
    } else {
        fd = platform_open_write(path, 0644U);
        if (fd < 0) {
            tool_write_error("wp-retraction-match", "cannot open article dedup output: ", path);
            return -1;
        }
    }

    tool_output_buffer_init(&output, fd);
    if (write_article_dedup_header(&output) != 0) {
        if (should_close) platform_close(fd);
        return -1;
    }
    for (index = 0U; index < analysis->group_count; ++index) {
        if (write_article_group_record(&output, &analysis->groups[index]) != 0) {
            if (should_close) platform_close(fd);
            return -1;
        }
    }
    if (tool_output_buffer_flush(&output) != 0) {
        if (should_close) platform_close(fd);
        return -1;
    }
    if (should_close && platform_close(fd) != 0) return -1;
    return 0;
}

static int process_weak_citation(WpRetractionIndex *index, ToolOutputBuffer *weak_output, char **fields, unsigned long long row_generation, unsigned long long *matches_out) {
    char raw_norm[WP_WEAK_TEXT_SIZE];
    size_t raw_index = 0U;

    if (normalize_match_text(fields[WP_CITE_COL_RAW], raw_norm, sizeof(raw_norm)) != 0) return -1;
    while (raw_norm[raw_index] != '\0') {
        size_t start;
        size_t length;
        char token[WP_WEAK_KEY_SIZE];
        size_t hit;

        while (raw_norm[raw_index] == ' ') raw_index += 1U;
        start = raw_index;
        while (raw_norm[raw_index] != '\0' && raw_norm[raw_index] != ' ') raw_index += 1U;
        length = raw_index - start;
        if (length < 6U || length >= sizeof(token)) continue;
        memcpy(token, raw_norm + start, length);
        token[length] = '\0';
        hit = lower_bound_weak_key(index, token);
        while (hit < index->weak_key_count && compare_weak_search_key(&index->weak_keys[hit], token) == 0) {
            WpRetractionRecord *record = &index->records[index->weak_keys[hit].record_index];
            char signals[64];

            hit += 1U;
            if (record->weak_seen_generation == row_generation) continue;
            record->weak_seen_generation = row_generation;
            signals[0] = '\0';
            if (!weak_match_signals(record, raw_norm, signals, sizeof(signals))) continue;
            if (write_weak_match_record(weak_output, record, signals, fields) != 0) return -1;
            *matches_out += 1ULL;
        }
    }
    return 0;
}

static int process_citations(
    const char *path,
    WpRetractionIndex *index,
    ToolOutputBuffer *output,
    ToolOutputBuffer *weak_output,
    WpDuplicateAnalysis *analysis,
    unsigned long long *matches_out,
    unsigned long long *weak_matches_out,
    WpMatchStats *stats,
    int quiet
) {
    int fd = platform_open_read(path);
    ToolRecordReader reader;
    char line[WP_MATCH_LINE_SIZE];
    int has_record = 0;
    int saw_header = 0;
    unsigned long long row_generation = 1ULL;
    unsigned long long next_progress = WP_MATCH_PROGRESS_ROWS;

    *matches_out = 0ULL;
    *weak_matches_out = 0ULL;
    rt_memset(stats, 0, sizeof(*stats));
    if (fd < 0) {
        tool_write_error("wp-retraction-match", "cannot open citation TSV: ", path);
        return -1;
    }
    tool_record_reader_init(&reader, fd, '\n');
    while (tool_record_reader_next(&reader, line, sizeof(line), &has_record) == 0 && has_record) {
        char *fields[WP_CITE_COL_COUNT + 2U];
        size_t field_count = 0U;
        char key[WP_MATCH_KEY_SIZE];
        int kind = 0;
        int weak_candidate = 0;
        size_t hit;
        size_t hit_end;
        unsigned long long row_matches = 0ULL;

        trim_record_line(line);
        if (!saw_header) {
            saw_header = 1;
            continue;
        }
        stats->citation_rows += 1ULL;
        if (!quiet && stats->citation_rows >= next_progress) {
            write_match_progress(stats);
            if (next_progress <= ((unsigned long long)-1) - WP_MATCH_PROGRESS_ROWS) next_progress += WP_MATCH_PROGRESS_ROWS;
            else next_progress = (unsigned long long)-1;
        }
        row_generation += 1ULL;
        if (row_generation == 0ULL) row_generation = 1ULL;
        if (citation_kind_from_row_prefix(line, &kind, &weak_candidate) != 0) {
            platform_close(fd);
            tool_write_error("wp-retraction-match", "invalid citation TSV row", 0);
            return -1;
        }
        if (kind == 0 && !(weak_output != 0 && weak_candidate)) continue;
        if (split_tsv_fields(line, fields, sizeof(fields) / sizeof(fields[0]), &field_count) != 0 || field_count < WP_CITE_COL_COUNT) {
            platform_close(fd);
            tool_write_error("wp-retraction-match", "invalid citation TSV row", 0);
            return -1;
        }
        if (kind == WP_MATCH_KIND_DOI && rt_strcmp(fields[WP_CITE_COL_KIND], "doi") == 0) {
            stats->identifier_rows += 1ULL;
            if (normalize_doi(fields[WP_CITE_COL_VALUE], key, sizeof(key)) != 0) continue;
        } else if (kind == WP_MATCH_KIND_PMID && rt_strcmp(fields[WP_CITE_COL_KIND], "pmid") == 0) {
            stats->identifier_rows += 1ULL;
            if (normalize_pmid(fields[WP_CITE_COL_VALUE], key, sizeof(key)) != 0) continue;
        } else {
            if (weak_output != 0 && (rt_strcmp(fields[WP_CITE_COL_KIND], "template") == 0 || rt_strcmp(fields[WP_CITE_COL_KIND], "ref") == 0)) {
                if (process_weak_citation(index, weak_output, fields, row_generation, weak_matches_out) != 0) {
                    platform_close(fd);
                    return -1;
                }
            }
            continue;
        }
        stats->lookup_rows += 1ULL;
        if (!find_key_range(index, kind, key, &hit, &hit_end)) continue;
        while (hit < hit_end) {
            const WpRetractionKey *match_key = &index->keys[hit];
            const WpRetractionRecord *record = &index->records[match_key->record_index];

            if (duplicate_analysis_note_match(analysis, record, match_key, fields) != 0) {
                platform_close(fd);
                tool_write_error("wp-retraction-match", "out of memory while tracking duplicate groups", 0);
                return -1;
            }
            if (write_match_record(output, record, match_key, fields) != 0) {
                platform_close(fd);
                return -1;
            }
            *matches_out += 1ULL;
            stats->match_output_rows += 1ULL;
            row_matches += 1ULL;
            hit += 1U;
        }
        if (row_matches > 0ULL) stats->matched_citation_rows += 1ULL;
    }
    if (platform_close(fd) != 0 || !saw_header) return -1;
    return 0;
}

static int parse_wiki_date_from_citation_name(const char *name, char *wiki_out, size_t wiki_size, char *date_out, size_t date_size) {
    size_t wiki_end = 0U;
    size_t name_length = rt_strlen(name);
    size_t suffix_length = rt_strlen("-citations.tsv");
    size_t index;

    while (name[wiki_end] != '\0' && name[wiki_end] != '-') wiki_end += 1U;
    if (wiki_end == 0U || wiki_end + 1U + 10U + suffix_length != name_length) return -1;
    for (index = 0U; index < 10U; ++index) {
        char ch = name[wiki_end + 1U + index];
        if ((index == 4U || index == 7U) ? ch != '-' : !tool_ascii_is_digit(ch)) return -1;
    }
    if (!text_ends_with(name, "-citations.tsv")) return -1;
    if (wiki_end + 1U > wiki_size || 11U > date_size) return -1;
    memcpy(wiki_out, name, wiki_end);
    wiki_out[wiki_end] = '\0';
    memcpy(date_out, name + wiki_end + 1U, 10U);
    date_out[10] = '\0';
    return 0;
}

static int find_latest_citations(const char *data_dir, WpCitationInput *input) {
    PlatformDirEntry entries[256];
    size_t count = 0U;
    size_t index;
    int is_dir = 0;
    char best_date[16];
    char best_wiki[64];
    char best_name[WP_MATCH_NAME_SIZE];

    best_date[0] = '\0';
    best_wiki[0] = '\0';
    best_name[0] = '\0';
    if (platform_collect_entries(data_dir, 0, entries, sizeof(entries) / sizeof(entries[0]), &count, &is_dir) != 0 || !is_dir) {
        return -1;
    }
    for (index = 0U; index < count; ++index) {
        char wiki[64];
        char date[16];

        if (entries[index].is_dir || parse_wiki_date_from_citation_name(entries[index].name, wiki, sizeof(wiki), date, sizeof(date)) != 0) continue;
        if (best_date[0] == '\0' || rt_strcmp(date, best_date) > 0 || (rt_strcmp(date, best_date) == 0 && rt_strcmp(wiki, best_wiki) > 0)) {
            rt_copy_string(best_date, sizeof(best_date), date);
            rt_copy_string(best_wiki, sizeof(best_wiki), wiki);
            rt_copy_string(best_name, sizeof(best_name), entries[index].name);
        }
    }
    if (best_name[0] == '\0') return -1;
    rt_copy_string(input->wiki, sizeof(input->wiki), best_wiki);
    rt_copy_string(input->date, sizeof(input->date), best_date);
    return join_path(data_dir, best_name, input->path, sizeof(input->path));
}

static int set_citation_input(const char *path, WpCitationInput *input) {
    const char *name = path;
    size_t index;

    rt_copy_string(input->path, sizeof(input->path), path);
    if (rt_strlen(input->path) != rt_strlen(path)) return -1;
    for (index = 0U; path[index] != '\0'; ++index) {
        if (path[index] == '/') name = path + index + 1U;
    }
    if (parse_wiki_date_from_citation_name(name, input->wiki, sizeof(input->wiki), input->date, sizeof(input->date)) != 0) {
        rt_copy_string(input->wiki, sizeof(input->wiki), "wiki");
        rt_copy_string(input->date, sizeof(input->date), "snapshot");
    }
    return 0;
}

static int make_default_output_path(const char *data_dir, const WpCitationInput *input, char *out, size_t out_size) {
    char name[WP_MATCH_NAME_SIZE];
    size_t length = 0U;

    length = tool_buffer_append_cstr(name, sizeof(name), length, input->wiki[0] != '\0' ? input->wiki : "wiki");
    length = tool_buffer_append_char(name, sizeof(name), length, '-');
    length = tool_buffer_append_cstr(name, sizeof(name), length, input->date[0] != '\0' ? input->date : "snapshot");
    length = tool_buffer_append_cstr(name, sizeof(name), length, "-retraction-matches.tsv");
    if (rt_strlen(name) != length) return -1;
    return join_path(data_dir, name, out, out_size);
}

static int make_default_weak_output_path(const char *data_dir, const WpCitationInput *input, char *out, size_t out_size) {
    char name[WP_MATCH_NAME_SIZE];
    size_t length = 0U;

    length = tool_buffer_append_cstr(name, sizeof(name), length, input->wiki[0] != '\0' ? input->wiki : "wiki");
    length = tool_buffer_append_char(name, sizeof(name), length, '-');
    length = tool_buffer_append_cstr(name, sizeof(name), length, input->date[0] != '\0' ? input->date : "snapshot");
    length = tool_buffer_append_cstr(name, sizeof(name), length, "-weak-retraction-matches.tsv");
    if (rt_strlen(name) != length) return -1;
    return join_path(data_dir, name, out, out_size);
}

static int make_default_retractions_path(const char *data_dir, char *out, size_t out_size) {
    return join_path(data_dir, "retraction_watch.csv", out, out_size);
}

int main(int argc, char **argv) {
    ToolOptState state;
    WpMatchOptions options;
    WpRetractionIndex index;
    WpDuplicateAnalysis analysis;
    WpMatchStats stats;
    WpCitationInput citations;
    ToolOutputBuffer output;
    ToolOutputBuffer weak_output;
    char retractions_path[WP_MATCH_PATH_SIZE];
    char output_path[WP_MATCH_PATH_SIZE];
    char weak_output_path[WP_MATCH_PATH_SIZE];
    int output_fd = -1;
    int weak_output_fd = -1;
    int should_close_output = 1;
    int should_close_weak_output = 1;
    int parse_result;
    unsigned long long matches = 0ULL;
    unsigned long long weak_matches = 0ULL;
    int exit_code = 1;

    rt_memset(&options, 0, sizeof(options));
    rt_memset(&index, 0, sizeof(index));
    rt_memset(&analysis, 0, sizeof(analysis));
    rt_memset(&stats, 0, sizeof(stats));
    rt_memset(&citations, 0, sizeof(citations));
    weak_output_path[0] = '\0';
    options.data_dir = default_data_dir();
    tool_opt_init(&state, argc, argv, argv[0], "[-q] [-d DIR] [-c CITATIONS.tsv] [-r RETRACTION_WATCH.csv] [-o MATCHES.tsv] [-w WEAK.tsv] [--no-weak] [--article-dedup FILE]");
    while ((parse_result = tool_opt_next(&state)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(state.flag, "-q") == 0 || rt_strcmp(state.flag, "--quiet") == 0) {
            options.quiet = 1;
        } else if (rt_strcmp(state.flag, "-d") == 0 || rt_strcmp(state.flag, "--data-dir") == 0) {
            if (tool_opt_require_value(&state) != 0) return 1;
            options.data_dir = state.value;
        } else if (tool_starts_with(state.flag, "--data-dir=")) {
            options.data_dir = state.flag + 11;
        } else if (rt_strcmp(state.flag, "-c") == 0 || rt_strcmp(state.flag, "--citations") == 0) {
            if (tool_opt_require_value(&state) != 0) return 1;
            options.citations_path = state.value;
        } else if (tool_starts_with(state.flag, "--citations=")) {
            options.citations_path = state.flag + 12;
        } else if (rt_strcmp(state.flag, "-r") == 0 || rt_strcmp(state.flag, "--retractions") == 0) {
            if (tool_opt_require_value(&state) != 0) return 1;
            options.retractions_path = state.value;
        } else if (tool_starts_with(state.flag, "--retractions=")) {
            options.retractions_path = state.flag + 14;
        } else if (rt_strcmp(state.flag, "-o") == 0 || rt_strcmp(state.flag, "--output") == 0) {
            if (tool_opt_require_value(&state) != 0) return 1;
            options.output_path = state.value;
        } else if (tool_starts_with(state.flag, "--output=")) {
            options.output_path = state.flag + 9;
        } else if (rt_strcmp(state.flag, "-w") == 0 || rt_strcmp(state.flag, "--weak-output") == 0) {
            if (tool_opt_require_value(&state) != 0) return 1;
            options.weak_output_path = state.value;
        } else if (tool_starts_with(state.flag, "--weak-output=")) {
            options.weak_output_path = state.flag + 14;
        } else if (rt_strcmp(state.flag, "--no-weak") == 0) {
            options.no_weak = 1;
        } else if (rt_strcmp(state.flag, "--article-dedup") == 0) {
            if (tool_opt_require_value(&state) != 0) return 1;
            options.article_dedup_path = state.value;
        } else if (tool_starts_with(state.flag, "--article-dedup=")) {
            options.article_dedup_path = state.flag + 16;
        } else {
            tool_write_error("wp-retraction-match", "unknown option: ", state.flag);
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
    if (options.no_weak && options.weak_output_path != 0) {
        tool_write_error("wp-retraction-match", "--no-weak cannot be combined with --weak-output", 0);
        return 1;
    }

    if (options.citations_path != 0) {
        if (set_citation_input(options.citations_path, &citations) != 0) {
            tool_write_error("wp-retraction-match", "citation path too long", 0);
            return 1;
        }
    } else if (find_latest_citations(options.data_dir, &citations) != 0) {
        tool_write_error("wp-retraction-match", "cannot find citation TSV in ", options.data_dir);
        return 1;
    }
    if (options.retractions_path != 0) {
        rt_copy_string(retractions_path, sizeof(retractions_path), options.retractions_path);
        if (rt_strlen(retractions_path) != rt_strlen(options.retractions_path)) {
            tool_write_error("wp-retraction-match", "Retraction Watch CSV path too long", 0);
            return 1;
        }
    } else if (make_default_retractions_path(options.data_dir, retractions_path, sizeof(retractions_path)) != 0) {
        tool_write_error("wp-retraction-match", "Retraction Watch CSV path too long", 0);
        return 1;
    }
    if (options.output_path != 0) {
        rt_copy_string(output_path, sizeof(output_path), options.output_path);
        if (rt_strlen(output_path) != rt_strlen(options.output_path)) {
            tool_write_error("wp-retraction-match", "output path too long", 0);
            return 1;
        }
    } else if (make_default_output_path(options.data_dir, &citations, output_path, sizeof(output_path)) != 0) {
        tool_write_error("wp-retraction-match", "output path too long", 0);
        return 1;
    }
    if (options.weak_output_path != 0) {
        rt_copy_string(weak_output_path, sizeof(weak_output_path), options.weak_output_path);
        if (rt_strlen(weak_output_path) != rt_strlen(options.weak_output_path)) {
            tool_write_error("wp-retraction-match", "weak output path too long", 0);
            return 1;
        }
    } else if (!options.no_weak && make_default_weak_output_path(options.data_dir, &citations, weak_output_path, sizeof(weak_output_path)) != 0) {
        tool_write_error("wp-retraction-match", "weak output path too long", 0);
        return 1;
    }
    if (output_path[0] == '-' && output_path[1] == '\0' &&
        weak_output_path[0] == '-' && weak_output_path[1] == '\0') {
        tool_write_error("wp-retraction-match", "hard and weak outputs cannot both be standard output", 0);
        return 1;
    }
    if (options.article_dedup_path != 0 && options.article_dedup_path[0] == '-' && options.article_dedup_path[1] == '\0' &&
        ((output_path[0] == '-' && output_path[1] == '\0') || (weak_output_path[0] == '-' && weak_output_path[1] == '\0'))) {
        tool_write_error("wp-retraction-match", "article dedup output cannot share standard output", 0);
        return 1;
    }

    if (load_retractions(retractions_path, &index) != 0) goto done;
    if (!options.quiet) {
        write_info_u64("loaded Retraction Watch records: ", (unsigned long long)index.record_count, 0);
        write_info_u64("indexed original paper identifiers: ", (unsigned long long)index.key_count, 0);
        if (!options.no_weak) write_info_u64("indexed weak title anchors: ", (unsigned long long)index.weak_key_count, 0);
    }
    if (output_path[0] == '-' && output_path[1] == '\0') {
        output_fd = 1;
        should_close_output = 0;
    } else {
        output_fd = platform_open_write(output_path, 0644U);
        if (output_fd < 0) {
            tool_write_error("wp-retraction-match", "cannot open output: ", output_path);
            goto done;
        }
    }
    tool_output_buffer_init(&output, output_fd);
    if (weak_output_path[0] != '\0') {
        if (weak_output_path[0] == '-' && weak_output_path[1] == '\0') {
            weak_output_fd = 1;
            should_close_weak_output = 0;
        } else {
            weak_output_fd = platform_open_write(weak_output_path, 0644U);
            if (weak_output_fd < 0) {
                tool_write_error("wp-retraction-match", "cannot open weak output: ", weak_output_path);
                goto done;
            }
        }
        tool_output_buffer_init(&weak_output, weak_output_fd);
    }
    if (write_output_header(&output) != 0) goto done;
    if (weak_output_fd >= 0 && write_weak_output_header(&weak_output) != 0) goto done;
    if (process_citations(citations.path, &index, &output, weak_output_fd >= 0 ? &weak_output : 0, &analysis, &matches, &weak_matches, &stats, options.quiet) != 0) goto done;
    if (tool_output_buffer_flush(&output) != 0) goto done;
    if (weak_output_fd >= 0 && tool_output_buffer_flush(&weak_output) != 0) goto done;
    if (options.article_dedup_path != 0 && write_article_dedup_output(options.article_dedup_path, &analysis) != 0) goto done;
    if (!options.quiet) {
        write_info_u64("processed citation rows: ", stats.citation_rows, 0);
        write_info_u64("identifier citation rows: ", stats.identifier_rows, 0);
        write_info_u64("identifier lookups: ", stats.lookup_rows, 0);
        write_info_u64("hard matched citation rows: ", stats.matched_citation_rows, 0);
        write_info_u64("matched citation rows: ", matches, 0);
        write_duplicate_summary(&analysis);
        if (options.article_dedup_path != 0) write_info_u64("article-level deduplicated output rows: ", (unsigned long long)analysis.group_count, 0);
    }
    if (!options.quiet && weak_output_fd >= 0) write_info_u64("weak matched citation rows: ", weak_matches, 0);
    exit_code = 0;

done:
    if (weak_output_fd >= 0 && should_close_weak_output && platform_close(weak_output_fd) != 0) exit_code = 1;
    if (output_fd >= 0 && should_close_output && platform_close(output_fd) != 0) exit_code = 1;
    duplicate_analysis_destroy(&analysis);
    retraction_index_destroy(&index);
    return exit_code;
}
