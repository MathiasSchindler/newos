#include "runtime.h"
#include "tool_util.h"
#include "xml.h"


#define XMLSTATS_INITIAL_NAMES 256
#define XMLSTATS_INITIAL_BUCKETS 512

typedef struct {
    char *name;
    size_t length;
    unsigned long long count;
    size_t hash;
    size_t next_index;
} NameCount;

typedef struct {
    unsigned long long elements;
    unsigned long long attributes;
    unsigned long long text_nodes;
    unsigned long long text_bytes;
    unsigned long long comments;
    unsigned long long cdata;
    unsigned long long pi;
    unsigned long long doctype;
    unsigned int max_depth;
    NameCount inline_element_names[XMLSTATS_INITIAL_NAMES];
    NameCount inline_attribute_names[XMLSTATS_INITIAL_NAMES];
    size_t inline_element_buckets[XMLSTATS_INITIAL_BUCKETS];
    size_t inline_attribute_buckets[XMLSTATS_INITIAL_BUCKETS];
    NameCount *element_names;
    NameCount *attribute_names;
    size_t *element_buckets;
    size_t *attribute_buckets;
    size_t element_name_count;
    size_t attribute_name_count;
    size_t element_name_capacity;
    size_t attribute_name_capacity;
    size_t element_bucket_count;
    size_t attribute_bucket_count;
} XmlStats;

static void stats_init(XmlStats *stats) {
    rt_memset(stats, 0, sizeof(*stats));
    stats->element_names = stats->inline_element_names;
    stats->attribute_names = stats->inline_attribute_names;
    stats->element_buckets = stats->inline_element_buckets;
    stats->attribute_buckets = stats->inline_attribute_buckets;
    stats->element_name_capacity = XMLSTATS_INITIAL_NAMES;
    stats->attribute_name_capacity = XMLSTATS_INITIAL_NAMES;
    stats->element_bucket_count = XMLSTATS_INITIAL_BUCKETS;
    stats->attribute_bucket_count = XMLSTATS_INITIAL_BUCKETS;
}

static size_t hash_name(const char *text, size_t length) {
    size_t hash = (size_t)1469598103934665603ULL;
    size_t i;
    for (i = 0U; i < length; ++i) {
        hash ^= (unsigned char)text[i];
        hash *= (size_t)1099511628211ULL;
    }
    return hash == 0U ? 1U : hash;
}

static int ensure_name_capacity(NameCount **names_io, size_t count, size_t *capacity, NameCount *inline_names, size_t needed) {
    NameCount *names;
    size_t next_capacity;
    size_t i;
    if (needed <= *capacity) return 0;
    next_capacity = *capacity == 0U ? XMLSTATS_INITIAL_NAMES : *capacity;
    while (next_capacity < needed) {
        if (next_capacity > (size_t)(~(size_t)0 / 2U)) return -1;
        next_capacity *= 2U;
    }
    names = (NameCount *)rt_malloc_array(next_capacity, sizeof(*names));
    if (names == 0) return -1;
    for (i = 0U; i < count; ++i) names[i] = (*names_io)[i];
    if (*names_io != inline_names) rt_free(*names_io);
    *names_io = names;
    *capacity = next_capacity;
    return 0;
}

static int ensure_bucket_capacity(NameCount *names, size_t count, size_t **buckets_io, size_t *bucket_count, size_t *inline_buckets, size_t needed) {
    size_t next_count;
    size_t *buckets;
    size_t i;

    if (needed <= *bucket_count / 2U) return 0;
    next_count = *bucket_count == 0U ? XMLSTATS_INITIAL_BUCKETS : *bucket_count;
    while (needed > next_count / 2U) {
        if (next_count > (size_t)(~(size_t)0 / 2U)) return -1;
        next_count *= 2U;
    }
    buckets = (size_t *)rt_malloc_array(next_count, sizeof(*buckets));
    if (buckets == 0) return -1;
    rt_memset(buckets, 0, next_count * sizeof(*buckets));
    for (i = 0U; i < count; ++i) {
        size_t bucket = names[i].hash & (next_count - 1U);
        names[i].next_index = buckets[bucket];
        buckets[bucket] = i + 1U;
    }
    if (*buckets_io != inline_buckets) rt_free(*buckets_io);
    *buckets_io = buckets;
    *bucket_count = next_count;
    return 0;
}

static int add_name(NameCount **names_io,
                    size_t *count,
                    size_t *capacity,
                    NameCount *inline_names,
                    size_t **buckets_io,
                    size_t *bucket_count,
                    size_t *inline_buckets,
                    const XmlName *name) {
    NameCount *names = *names_io;
    size_t hash = hash_name(name->start, name->length);
    size_t bucket = hash & (*bucket_count - 1U);
    size_t index = (*buckets_io)[bucket];
    while (index != 0U) {
        NameCount *entry = &names[index - 1U];
        if (entry->hash == hash && entry->length == name->length && rt_strncmp(entry->name, name->start, name->length) == 0) {
            entry->count += 1ULL;
            return 0;
        }
        index = entry->next_index;
    }
    if (ensure_name_capacity(names_io, *count, capacity, inline_names, *count + 1U) != 0) return -1;
    names = *names_io;
    if (ensure_bucket_capacity(names, *count, buckets_io, bucket_count, inline_buckets, *count + 1U) != 0) return -1;
    bucket = hash & (*bucket_count - 1U);
    names[*count].name = xml_slice_dup(name->start, name->length);
    if (names[*count].name == 0) return -1;
    names[*count].length = name->length;
    names[*count].count = 1ULL;
    names[*count].hash = hash;
    names[*count].next_index = (*buckets_io)[bucket];
    (*buckets_io)[bucket] = *count + 1U;
    *count += 1U;
    return 0;
}

static void free_name_counts(NameCount *names, size_t count) {
    size_t i;
    for (i = 0U; i < count; ++i) rt_free(names[i].name);
}

static void stats_free(XmlStats *stats) {
    free_name_counts(stats->element_names, stats->element_name_count);
    free_name_counts(stats->attribute_names, stats->attribute_name_count);
    if (stats->element_names != stats->inline_element_names) rt_free(stats->element_names);
    if (stats->attribute_names != stats->inline_attribute_names) rt_free(stats->attribute_names);
    if (stats->element_buckets != stats->inline_element_buckets) rt_free(stats->element_buckets);
    if (stats->attribute_buckets != stats->inline_attribute_buckets) rt_free(stats->attribute_buckets);
}

static void write_name_counts(const char *title, const NameCount *names, size_t count) {
    size_t i;
    rt_write_cstr(1, title);
    rt_write_char(1, '\n');
    for (i = 0U; i < count; ++i) {
        rt_write_cstr(1, "    ");
        rt_write_cstr(1, names[i].name == 0 ? "" : names[i].name);
        rt_write_char(1, ' ');
        rt_write_uint(1, names[i].count);
        rt_write_char(1, '\n');
    }
}

static void write_stats(const XmlStats *stats) {
    rt_write_cstr(1, "elements "); rt_write_uint(1, stats->elements); rt_write_char(1, '\n');
    rt_write_cstr(1, "attributes "); rt_write_uint(1, stats->attributes); rt_write_char(1, '\n');
    rt_write_cstr(1, "text-nodes "); rt_write_uint(1, stats->text_nodes); rt_write_char(1, '\n');
    rt_write_cstr(1, "text-bytes "); rt_write_uint(1, stats->text_bytes); rt_write_char(1, '\n');
    rt_write_cstr(1, "comments "); rt_write_uint(1, stats->comments); rt_write_char(1, '\n');
    rt_write_cstr(1, "cdata "); rt_write_uint(1, stats->cdata); rt_write_char(1, '\n');
    rt_write_cstr(1, "pi "); rt_write_uint(1, stats->pi); rt_write_char(1, '\n');
    rt_write_cstr(1, "doctype "); rt_write_uint(1, stats->doctype); rt_write_char(1, '\n');
    rt_write_cstr(1, "max-depth "); rt_write_uint(1, stats->max_depth); rt_write_char(1, '\n');
    write_name_counts("element-names", stats->element_names, stats->element_name_count);
    write_name_counts("attribute-names", stats->attribute_names, stats->attribute_name_count);
}

static int stats_one(const char *path, XmlStats *stats) {
    XmlParser parser;
    XmlToken token;
    char *input;
    size_t length;
    int result;

    if (xml_read_document(path, &input, &length, "xmlstats") != 0) return 1;
    xml_parser_init(&parser, input, length);
    while ((result = xml_next_token(&parser, &token)) > 0) {
        if (token.type == XML_TOKEN_START || token.type == XML_TOKEN_EMPTY) {
            size_t i;
            stats->elements += 1ULL;
            stats->attributes += (unsigned long long)token.attribute_count;
            if (token.depth + 1U > stats->max_depth) stats->max_depth = token.depth + 1U;
            if (add_name(&stats->element_names,
                         &stats->element_name_count,
                         &stats->element_name_capacity,
                         stats->inline_element_names,
                         &stats->element_buckets,
                         &stats->element_bucket_count,
                         stats->inline_element_buckets,
                         &token.name) != 0) {
                xml_free_document(input);
                return 1;
            }
            for (i = 0U; i < token.attribute_count; ++i) {
                if (add_name(&stats->attribute_names,
                             &stats->attribute_name_count,
                             &stats->attribute_name_capacity,
                             stats->inline_attribute_names,
                             &stats->attribute_buckets,
                             &stats->attribute_bucket_count,
                             stats->inline_attribute_buckets,
                             &token.attributes[i].name) != 0) {
                    xml_free_document(input);
                    return 1;
                }
            }
        } else if (token.type == XML_TOKEN_TEXT || token.type == XML_TOKEN_CDATA) {
            if (!token.text_is_blank) {
                stats->text_nodes += 1ULL;
                stats->text_bytes += (unsigned long long)token.text_length;
            }
            if (token.type == XML_TOKEN_CDATA) stats->cdata += 1ULL;
        } else if (token.type == XML_TOKEN_COMMENT) stats->comments += 1ULL;
        else if (token.type == XML_TOKEN_PI) stats->pi += 1ULL;
        else if (token.type == XML_TOKEN_DOCTYPE) stats->doctype += 1ULL;
    }
    if (result < 0 || xml_parse_complete(&parser) != 0) {
        xml_report_error("xmlstats", path, &parser);
        xml_free_document(input);
        return 1;
    }
    xml_free_document(input);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    XmlStats stats;
    int option_result;
    int exit_code = 0;
    int i;

    stats_init(&stats);
    tool_opt_init(&opt, argc, argv, "xmlstats", "[FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        tool_write_error("xmlstats", "unknown option: ", opt.flag);
        tool_write_usage("xmlstats", "[FILE ...]");
        return 1;
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlstats", "[FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) exit_code = stats_one(0, &stats);
    else {
        for (i = opt.argi; i < argc; ++i) {
            if (stats_one(argv[i], &stats) != 0) exit_code = 1;
        }
    }
    if (exit_code == 0) write_stats(&stats);
    stats_free(&stats);
    return exit_code;
}
