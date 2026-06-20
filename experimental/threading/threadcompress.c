#include "concurrency.h"
#include "platform.h"
#include "runtime.h"

#define THREADCOMPRESS_DEFAULT_BYTES (32U * 1024U * 1024U)
#define THREADCOMPRESS_DEFAULT_CHUNK (1024U * 1024U)
#define THREADCOMPRESS_DEFAULT_REPEAT 3U
#define THREADCOMPRESS_DEFAULT_MAX_WIDTH 8U
#define THREADCOMPRESS_PACKET_LIMIT 128U
#define THREADCOMPRESS_U64_MAX_DIV_1E9 18446744073ULL

typedef struct {
    size_t bytes;
    size_t chunk_bytes;
    unsigned int repeat;
    unsigned int max_width;
    int show_stats;
    int verify;
    const char *case_name;
} CompressOptions;

typedef struct {
    const unsigned char *input;
    size_t input_size;
    unsigned char *output;
    size_t output_capacity;
    size_t output_size;
    int result;
} CompressChunk;

typedef struct {
    CompressChunk *chunks;
    size_t chunk_count;
} ParallelCompressState;

typedef struct {
    unsigned long long min_ns;
    unsigned long long median_ns;
    unsigned long long p90_ns;
    unsigned long long max_ns;
    unsigned long long checksum;
    size_t compressed_size;
    unsigned int effective_width;
    RtTaskPoolStats stats;
    int error;
} CompressResult;

static int text_equals(const char *left, const char *right) {
    return rt_strcmp(left, right) == 0;
}

static int parse_size_option(const char *text, size_t *value_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value == 0ULL) {
        return -1;
    }
    *value_out = (size_t)value;
    return 0;
}

static int parse_uint_option(const char *text, unsigned int *value_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value == 0ULL || value > 4294967295ULL) {
        return -1;
    }
    *value_out = (unsigned int)value;
    return 0;
}

static void write_usage(const char *program) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program);
    rt_write_cstr(2, " [--case mixed|text|runs|random] [--bytes N] [--chunk N] [--repeat N] [--max-width N] [--stats] [--no-verify]\n");
}

static int parse_options(int argc, char **argv, CompressOptions *options) {
    int index;

    options->bytes = THREADCOMPRESS_DEFAULT_BYTES;
    options->chunk_bytes = THREADCOMPRESS_DEFAULT_CHUNK;
    options->repeat = THREADCOMPRESS_DEFAULT_REPEAT;
    options->max_width = THREADCOMPRESS_DEFAULT_MAX_WIDTH;
    options->show_stats = 0;
    options->verify = 1;
    options->case_name = "mixed";
    for (index = 1; index < argc; ++index) {
        const char *arg = argv[index];
        const char *value;

        if (text_equals(arg, "--help") || text_equals(arg, "-h")) {
            write_usage(argc > 0 ? argv[0] : "threadcompress");
            return 1;
        }
        if (text_equals(arg, "--stats")) {
            options->show_stats = 1;
            continue;
        }
        if (text_equals(arg, "--no-verify")) {
            options->verify = 0;
            continue;
        }
        if (index + 1 >= argc) {
            return -1;
        }
        value = argv[++index];
        if (text_equals(arg, "--case")) {
            options->case_name = value;
        } else if (text_equals(arg, "--bytes")) {
            if (parse_size_option(value, &options->bytes) != 0) return -1;
        } else if (text_equals(arg, "--chunk")) {
            if (parse_size_option(value, &options->chunk_bytes) != 0) return -1;
        } else if (text_equals(arg, "--repeat")) {
            if (parse_uint_option(value, &options->repeat) != 0) return -1;
        } else if (text_equals(arg, "--max-width")) {
            if (parse_uint_option(value, &options->max_width) != 0) return -1;
        } else {
            return -1;
        }
    }
    if (options->max_width > RT_TASK_POOL_MAX_WORKERS) {
        options->max_width = RT_TASK_POOL_MAX_WORKERS;
    }
    if (options->chunk_bytes == 0U) {
        options->chunk_bytes = THREADCOMPRESS_DEFAULT_CHUNK;
    }
    return 0;
}

static unsigned long long mix64(unsigned long long value) {
    value ^= value >> 33U;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33U;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33U;
    return value ^ (value >> 33U);
}

static void fill_input(unsigned char *input, size_t size, const char *case_name) {
    static const char text[] = "newos freestanding threading compression benchmark data ";
    size_t index;
    unsigned long long seed = 0xa0761d6478bd642fULL;

    if (text_equals(case_name, "text")) {
        for (index = 0U; index < size; ++index) {
            input[index] = (unsigned char)text[index % (sizeof(text) - 1U)];
        }
        return;
    }
    if (text_equals(case_name, "runs")) {
        for (index = 0U; index < size; ++index) {
            input[index] = (unsigned char)('A' + ((index / 4096U) % 8U));
        }
        return;
    }
    if (text_equals(case_name, "random")) {
        for (index = 0U; index < size; ++index) {
            if ((index & 7U) == 0U) {
                seed = mix64(seed + index);
            }
            input[index] = (unsigned char)(seed >> ((index & 7U) * 8U));
        }
        return;
    }
    for (index = 0U; index < size; ++index) {
        if ((index & 8191U) < 2048U) {
            input[index] = (unsigned char)('a' + ((index >> 13U) % 12U));
        } else if ((index & 255U) < 192U) {
            input[index] = (unsigned char)text[(index + (index >> 8U)) % (sizeof(text) - 1U)];
        } else {
            seed = mix64(seed + index);
            input[index] = (unsigned char)seed;
        }
    }
}

static size_t bzip_rle_bound(size_t input_size) {
    return input_size + ((input_size + THREADCOMPRESS_PACKET_LIMIT - 1U) / THREADCOMPRESS_PACKET_LIMIT) + 16U;
}

static int emit_byte(unsigned char *output, size_t capacity, size_t *offset_io, unsigned char value) {
    if (*offset_io >= capacity) {
        return -1;
    }
    output[*offset_io] = value;
    *offset_io += 1U;
    return 0;
}

static int emit_data(unsigned char *output, size_t capacity, size_t *offset_io, const unsigned char *data, size_t size) {
    size_t index;

    if (size > capacity || *offset_io > capacity - size) {
        return -1;
    }
    for (index = 0U; index < size; ++index) {
        output[*offset_io + index] = data[index];
    }
    *offset_io += size;
    return 0;
}

static int flush_literal(unsigned char *output, size_t capacity, size_t *offset_io, unsigned char *literal, size_t *literal_size_io) {
    if (*literal_size_io == 0U) {
        return 0;
    }
    if (emit_byte(output, capacity, offset_io, (unsigned char)(*literal_size_io - 1U)) != 0 ||
        emit_data(output, capacity, offset_io, literal, *literal_size_io) != 0) {
        return -1;
    }
    *literal_size_io = 0U;
    return 0;
}

static int emit_run(unsigned char *output, size_t capacity, size_t *offset_io, unsigned char value, size_t run_size) {
    while (run_size != 0U) {
        size_t chunk = run_size > THREADCOMPRESS_PACKET_LIMIT ? THREADCOMPRESS_PACKET_LIMIT : run_size;

        if (emit_byte(output, capacity, offset_io, (unsigned char)(0x80U | (unsigned char)(chunk - 1U))) != 0 ||
            emit_byte(output, capacity, offset_io, value) != 0) {
            return -1;
        }
        run_size -= chunk;
    }
    return 0;
}

static int bzip_rle_compress(const unsigned char *input, size_t input_size, unsigned char *output, size_t output_capacity, size_t *output_size_out) {
    unsigned char literal[THREADCOMPRESS_PACKET_LIMIT];
    size_t literal_size = 0U;
    size_t output_size = 0U;
    unsigned char run_value = 0U;
    size_t run_size = 0U;
    int have_run = 0;
    size_t index;

    for (index = 0U; index < input_size; ++index) {
        unsigned char value = input[index];

        if (!have_run) {
            run_value = value;
            run_size = 1U;
            have_run = 1;
            continue;
        }
        if (value == run_value && run_size < THREADCOMPRESS_PACKET_LIMIT) {
            run_size += 1U;
            continue;
        }
        if (run_size >= 4U) {
            if (flush_literal(output, output_capacity, &output_size, literal, &literal_size) != 0 ||
                emit_run(output, output_capacity, &output_size, run_value, run_size) != 0) {
                return -1;
            }
        } else {
            size_t run_index;

            for (run_index = 0U; run_index < run_size; ++run_index) {
                literal[literal_size++] = run_value;
                if (literal_size == THREADCOMPRESS_PACKET_LIMIT && flush_literal(output, output_capacity, &output_size, literal, &literal_size) != 0) {
                    return -1;
                }
            }
        }
        run_value = value;
        run_size = 1U;
    }
    if (have_run) {
        if (run_size >= 4U) {
            if (flush_literal(output, output_capacity, &output_size, literal, &literal_size) != 0 ||
                emit_run(output, output_capacity, &output_size, run_value, run_size) != 0) {
                return -1;
            }
        } else {
            size_t run_index;

            for (run_index = 0U; run_index < run_size; ++run_index) {
                literal[literal_size++] = run_value;
                if (literal_size == THREADCOMPRESS_PACKET_LIMIT && flush_literal(output, output_capacity, &output_size, literal, &literal_size) != 0) {
                    return -1;
                }
            }
        }
    }
    if (flush_literal(output, output_capacity, &output_size, literal, &literal_size) != 0) {
        return -1;
    }
    *output_size_out = output_size;
    return 0;
}

static int verify_bzip_rle_chunk(const unsigned char *input, size_t input_size, const unsigned char *compressed, size_t compressed_size) {
    size_t input_offset = 0U;
    size_t compressed_offset = 0U;

    while (compressed_offset < compressed_size) {
        unsigned char tag = compressed[compressed_offset++];
        size_t count = (size_t)(tag & 0x7fU) + 1U;

        if ((tag & 0x80U) != 0U) {
            unsigned char value;
            size_t index;

            if (compressed_offset >= compressed_size || count > input_size || input_offset > input_size - count) {
                return -1;
            }
            value = compressed[compressed_offset++];
            for (index = 0U; index < count; ++index) {
                if (input[input_offset++] != value) {
                    return -1;
                }
            }
        } else {
            size_t index;

            if (count > compressed_size || compressed_offset > compressed_size - count || count > input_size || input_offset > input_size - count) {
                return -1;
            }
            for (index = 0U; index < count; ++index) {
                if (input[input_offset + index] != compressed[compressed_offset + index]) {
                    return -1;
                }
            }
            input_offset += count;
            compressed_offset += count;
        }
    }
    return input_offset == input_size ? 0 : -1;
}

static unsigned long long checksum_bytes(const unsigned char *data, size_t size) {
    unsigned long long value = 0x9e3779b97f4a7c15ULL ^ (unsigned long long)size;
    size_t index;

    for (index = 0U; index < size; ++index) {
        value = mix64(value ^ data[index] ^ ((unsigned long long)index << 8U));
    }
    return value;
}

static int parallel_compress_body(size_t begin, size_t end, unsigned int worker_index, void *arg) {
    ParallelCompressState *state = (ParallelCompressState *)arg;
    size_t index;

    (void)worker_index;
    for (index = begin; index < end; ++index) {
        CompressChunk *chunk = &state->chunks[index];

        chunk->output_size = 0U;
        chunk->result = bzip_rle_compress(chunk->input, chunk->input_size, chunk->output, chunk->output_capacity, &chunk->output_size);
        if (chunk->result != 0) {
            return -1;
        }
    }
    return 0;
}

static void sort_u64(unsigned long long *values, unsigned int count) {
    unsigned int index;

    for (index = 1U; index < count; ++index) {
        unsigned long long value = values[index];
        unsigned int cursor = index;

        while (cursor > 0U && values[cursor - 1U] > value) {
            values[cursor] = values[cursor - 1U];
            cursor -= 1U;
        }
        values[cursor] = value;
    }
}

static unsigned long long units_per_second(size_t units, unsigned long long ns) {
    unsigned long long unit_count = (unsigned long long)units;

    if (ns == 0ULL) {
        return 0ULL;
    }
    if (unit_count <= THREADCOMPRESS_U64_MAX_DIV_1E9) {
        return (unit_count * 1000000000ULL) / ns;
    }
    return unit_count / (ns / 1000000000ULL + 1ULL);
}

static void write_decimal_x100(unsigned long long value_x100) {
    rt_write_uint(1, value_x100 / 100ULL);
    rt_write_char(1, '.');
    if (value_x100 % 100ULL < 10ULL) {
        rt_write_char(1, '0');
    }
    rt_write_uint(1, value_x100 % 100ULL);
}

static size_t chunk_count_for(size_t input_size, size_t chunk_size) {
    if (input_size == 0U) {
        return 1U;
    }
    return (input_size + chunk_size - 1U) / chunk_size;
}

static int prepare_chunks(CompressChunk *chunks, size_t chunk_count, const unsigned char *input, size_t input_size, size_t chunk_size) {
    size_t index;

    for (index = 0U; index < chunk_count; ++index) {
        size_t offset = index * chunk_size;
        size_t size = offset < input_size ? input_size - offset : 0U;

        if (size > chunk_size) {
            size = chunk_size;
        }
        chunks[index].input = input + offset;
        chunks[index].input_size = size;
        chunks[index].output_capacity = bzip_rle_bound(size);
        chunks[index].output = (unsigned char *)rt_malloc(chunks[index].output_capacity);
        chunks[index].output_size = 0U;
        chunks[index].result = 0;
        if (chunks[index].output == 0) {
            return -1;
        }
    }
    return 0;
}

static void free_chunks(CompressChunk *chunks, size_t chunk_count) {
    size_t index;

    for (index = 0U; index < chunk_count; ++index) {
        rt_free(chunks[index].output);
    }
}

static int verify_chunks(const CompressChunk *chunks, size_t chunk_count) {
    size_t index;

    for (index = 0U; index < chunk_count; ++index) {
        if (verify_bzip_rle_chunk(chunks[index].input, chunks[index].input_size, chunks[index].output, chunks[index].output_size) != 0) {
            return -1;
        }
    }
    return 0;
}

static size_t chunks_compressed_size(const CompressChunk *chunks, size_t chunk_count) {
    size_t total = 0U;
    size_t index;

    for (index = 0U; index < chunk_count; ++index) {
        total += chunks[index].output_size;
    }
    return total;
}

static unsigned long long chunks_checksum(const CompressChunk *chunks, size_t chunk_count) {
    unsigned long long checksum = 0ULL;
    size_t index;

    for (index = 0U; index < chunk_count; ++index) {
        checksum ^= mix64(checksum_bytes(chunks[index].output, chunks[index].output_size) + index);
    }
    return checksum;
}

static CompressResult run_serial_case(const CompressOptions *options, const unsigned char *input) {
    CompressResult result;
    unsigned long long *samples;
    unsigned char *output;
    size_t output_capacity;
    unsigned int repeat;

    rt_memset(&result, 0, sizeof(result));
    samples = (unsigned long long *)rt_malloc_array(options->repeat, sizeof(samples[0]));
    output_capacity = bzip_rle_bound(options->bytes);
    output = (unsigned char *)rt_malloc(output_capacity);
    if (samples == 0 || output == 0) {
        rt_free(samples);
        rt_free(output);
        result.error = -1;
        return result;
    }
    result.effective_width = 1U;
    for (repeat = 0U; repeat < options->repeat; ++repeat) {
        size_t output_size = 0U;
        unsigned long long start = platform_get_monotonic_time_ns();
        unsigned long long elapsed;

        if (bzip_rle_compress(input, options->bytes, output, output_capacity, &output_size) != 0) {
            result.error = -1;
            break;
        }
        elapsed = platform_get_monotonic_time_ns() - start;
        samples[repeat] = elapsed;
        if (result.min_ns == 0ULL || elapsed < result.min_ns) {
            result.min_ns = elapsed;
            result.compressed_size = output_size;
        }
        result.checksum ^= checksum_bytes(output, output_size) + repeat;
        if (options->verify && verify_bzip_rle_chunk(input, options->bytes, output, output_size) != 0) {
            result.error = -1;
            break;
        }
    }
    if (result.error == 0) {
        unsigned int p90_index;

        sort_u64(samples, options->repeat);
        result.min_ns = samples[0];
        result.median_ns = samples[options->repeat / 2U];
        p90_index = ((options->repeat * 9U) + 9U) / 10U;
        if (p90_index == 0U) p90_index = 1U;
        p90_index -= 1U;
        if (p90_index >= options->repeat) p90_index = options->repeat - 1U;
        result.p90_ns = samples[p90_index];
        result.max_ns = samples[options->repeat - 1U];
    }
    rt_free(output);
    rt_free(samples);
    return result;
}

static CompressResult run_parallel_case(const CompressOptions *options, const unsigned char *input, unsigned int requested_width) {
    CompressResult result;
    RtTaskPool pool;
    ParallelCompressState state;
    CompressChunk *chunks;
    unsigned long long *samples;
    size_t chunk_count = chunk_count_for(options->bytes, options->chunk_bytes);
    unsigned int repeat;

    rt_memset(&result, 0, sizeof(result));
    rt_memset(&pool, 0, sizeof(pool));
    samples = (unsigned long long *)rt_malloc_array(options->repeat, sizeof(samples[0]));
    chunks = (CompressChunk *)rt_malloc_array(chunk_count, sizeof(chunks[0]));
    if (samples == 0 || chunks == 0) {
        rt_free(samples);
        rt_free(chunks);
        result.error = -1;
        return result;
    }
    rt_memset(chunks, 0, chunk_count * sizeof(chunks[0]));
    if (prepare_chunks(chunks, chunk_count, input, options->bytes, options->chunk_bytes) != 0 || rt_task_pool_init(&pool, requested_width) != 0) {
        free_chunks(chunks, chunk_count);
        rt_free(chunks);
        rt_free(samples);
        rt_task_pool_destroy(&pool);
        result.error = -1;
        return result;
    }
    result.effective_width = rt_task_pool_width(&pool);
    state.chunks = chunks;
    state.chunk_count = chunk_count;
    for (repeat = 0U; repeat < options->repeat; ++repeat) {
        RtTaskPoolStats stats;
        unsigned long long start;
        unsigned long long elapsed;

        rt_task_pool_reset_stats(&pool);
        start = platform_get_monotonic_time_ns();
        if (rt_parallel_for(&pool, chunk_count, 1U, parallel_compress_body, &state) != 0) {
            result.error = -1;
            break;
        }
        elapsed = platform_get_monotonic_time_ns() - start;
        rt_task_pool_get_stats(&pool, &stats);
        samples[repeat] = elapsed;
        if (result.min_ns == 0ULL || elapsed < result.min_ns) {
            result.min_ns = elapsed;
            result.compressed_size = chunks_compressed_size(chunks, chunk_count);
            result.stats = stats;
        }
        result.checksum ^= chunks_checksum(chunks, chunk_count) + repeat;
        if (options->verify && verify_chunks(chunks, chunk_count) != 0) {
            result.error = -1;
            break;
        }
    }
    if (result.error == 0) {
        unsigned int p90_index;

        sort_u64(samples, options->repeat);
        result.min_ns = samples[0];
        result.median_ns = samples[options->repeat / 2U];
        p90_index = ((options->repeat * 9U) + 9U) / 10U;
        if (p90_index == 0U) p90_index = 1U;
        p90_index -= 1U;
        if (p90_index >= options->repeat) p90_index = options->repeat - 1U;
        result.p90_ns = samples[p90_index];
        result.max_ns = samples[options->repeat - 1U];
    }
    rt_task_pool_destroy(&pool);
    free_chunks(chunks, chunk_count);
    rt_free(chunks);
    rt_free(samples);
    return result;
}

static unsigned int result_active_workers(const CompressResult *result) {
    if (result->stats.last_active_workers != 0U) {
        return result->stats.last_active_workers;
    }
    return result->effective_width == 0U ? 1U : result->effective_width;
}

static void write_header(const CompressOptions *options) {
    rt_write_line(1, "# experimental/threading compression benchmark");
    rt_write_cstr(1, "# workers_supported=");
    rt_write_uint(1, (unsigned long long)platform_worker_threads_supported());
    rt_write_cstr(1, " detected_width=");
    rt_write_uint(1, (unsigned long long)platform_worker_thread_count());
    rt_write_char(1, '\n');
    rt_write_cstr(1, "# case=");
    rt_write_cstr(1, options->case_name);
    rt_write_cstr(1, " bytes=");
    rt_write_uint(1, (unsigned long long)options->bytes);
    rt_write_cstr(1, " chunk=");
    rt_write_uint(1, (unsigned long long)options->chunk_bytes);
    rt_write_cstr(1, " repeat=");
    rt_write_uint(1, options->repeat);
    rt_write_char(1, '\n');
    rt_write_line(1, "case,mode,requested_width,effective_width,active_workers,input_bytes,chunk_bytes,chunks,compressed_bytes,ratio,median_ns,min_ns,p90_ns,max_ns,bytes_per_sec,speedup,checksum");
}

static void write_result_row(const CompressOptions *options, const char *mode, unsigned int requested_width, size_t chunk_count, const CompressResult *result, unsigned long long baseline_ns) {
    unsigned long long speedup_x100 = 0ULL;
    unsigned long long ratio_x100 = 0ULL;

    if (result->median_ns != 0ULL) {
        speedup_x100 = (baseline_ns * 100ULL) / result->median_ns;
    }
    if (options->bytes != 0U) {
        ratio_x100 = ((unsigned long long)result->compressed_size * 10000ULL) / (unsigned long long)options->bytes;
    }
    rt_write_cstr(1, options->case_name);
    rt_write_char(1, ',');
    rt_write_cstr(1, mode);
    rt_write_char(1, ',');
    rt_write_uint(1, requested_width);
    rt_write_char(1, ',');
    rt_write_uint(1, result->effective_width);
    rt_write_char(1, ',');
    rt_write_uint(1, result_active_workers(result));
    rt_write_char(1, ',');
    rt_write_uint(1, (unsigned long long)options->bytes);
    rt_write_char(1, ',');
    rt_write_uint(1, (unsigned long long)options->chunk_bytes);
    rt_write_char(1, ',');
    rt_write_uint(1, (unsigned long long)chunk_count);
    rt_write_char(1, ',');
    rt_write_uint(1, (unsigned long long)result->compressed_size);
    rt_write_char(1, ',');
    write_decimal_x100(ratio_x100);
    rt_write_char(1, ',');
    rt_write_uint(1, result->median_ns);
    rt_write_char(1, ',');
    rt_write_uint(1, result->min_ns);
    rt_write_char(1, ',');
    rt_write_uint(1, result->p90_ns);
    rt_write_char(1, ',');
    rt_write_uint(1, result->max_ns);
    rt_write_char(1, ',');
    rt_write_uint(1, units_per_second(options->bytes, result->median_ns));
    rt_write_char(1, ',');
    write_decimal_x100(speedup_x100);
    rt_write_char(1, ',');
    rt_write_uint(1, result->checksum);
    rt_write_char(1, '\n');
}

static void write_stats_row(const char *mode, unsigned int requested_width, const CompressResult *result) {
    const RtTaskPoolStats *stats = &result->stats;

    rt_write_cstr(1, "# stats mode=");
    rt_write_cstr(1, mode);
    rt_write_cstr(1, " requested_width=");
    rt_write_uint(1, requested_width);
    rt_write_cstr(1, " active_workers=");
    rt_write_uint(1, result_active_workers(result));
    rt_write_cstr(1, " dispatches=");
    rt_write_uint(1, stats->dispatches);
    rt_write_cstr(1, " chunks=");
    rt_write_uint(1, stats->chunks_claimed);
    rt_write_cstr(1, " chunk_attempts=");
    rt_write_uint(1, stats->chunk_claim_attempts);
    rt_write_cstr(1, " workers_woken=");
    rt_write_uint(1, stats->workers_woken);
    rt_write_cstr(1, " workers_ran=");
    rt_write_uint(1, stats->workers_ran);
    rt_write_cstr(1, " idle_worker_completions=");
    rt_write_uint(1, stats->idle_worker_completions);
    rt_write_cstr(1, " dispatch_ns=");
    rt_write_uint(1, stats->dispatch_ns);
    rt_write_cstr(1, " join_ns=");
    rt_write_uint(1, stats->join_ns);
    rt_write_cstr(1, " body_ns=");
    rt_write_uint(1, stats->body_ns);
    rt_write_cstr(1, " effective_min_chunk=");
    rt_write_uint(1, (unsigned long long)stats->last_effective_min_chunk);
    rt_write_char(1, '\n');
}

static int run_benchmark(const CompressOptions *options) {
    unsigned char *input;
    CompressResult baseline;
    unsigned int requested_width;
    size_t chunk_count = chunk_count_for(options->bytes, options->chunk_bytes);

    if (!text_equals(options->case_name, "mixed") && !text_equals(options->case_name, "text") && !text_equals(options->case_name, "runs") && !text_equals(options->case_name, "random")) {
        rt_write_line(2, "threadcompress: unknown input case");
        return -1;
    }
    input = (unsigned char *)rt_malloc(options->bytes == 0U ? 1U : options->bytes);
    if (input == 0) {
        rt_write_line(2, "threadcompress: allocation failed");
        return -1;
    }
    fill_input(input, options->bytes, options->case_name);
    write_header(options);
    baseline = run_serial_case(options, input);
    if (baseline.error != 0 || baseline.median_ns == 0ULL) {
        rt_free(input);
        rt_write_line(2, "threadcompress: serial benchmark failed");
        return -1;
    }
    write_result_row(options, "serial-bzip2-rle", 1U, 1U, &baseline, baseline.median_ns);
    requested_width = 2U;
    while (requested_width <= options->max_width) {
        CompressResult result = run_parallel_case(options, input, requested_width);

        if (result.error != 0 || result.median_ns == 0ULL) {
            rt_free(input);
            rt_write_line(2, "threadcompress: parallel benchmark failed");
            return -1;
        }
        write_result_row(options, "parallel-bzip2-rle", requested_width, chunk_count, &result, baseline.median_ns);
        if (options->show_stats) {
            write_stats_row("parallel-bzip2-rle", requested_width, &result);
        }
        requested_width *= 2U;
    }
    requested_width /= 2U;
    if (requested_width != options->max_width && options->max_width > 1U) {
        CompressResult result = run_parallel_case(options, input, options->max_width);

        if (result.error != 0 || result.median_ns == 0ULL) {
            rt_free(input);
            rt_write_line(2, "threadcompress: parallel benchmark failed");
            return -1;
        }
        write_result_row(options, "parallel-bzip2-rle", options->max_width, chunk_count, &result, baseline.median_ns);
        if (options->show_stats) {
            write_stats_row("parallel-bzip2-rle", options->max_width, &result);
        }
    }
    rt_free(input);
    return 0;
}

int main(int argc, char **argv) {
    CompressOptions options;
    int parsed = parse_options(argc, argv, &options);

    if (parsed > 0) {
        return 0;
    }
    if (parsed != 0) {
        write_usage(argc > 0 ? argv[0] : "threadcompress");
        return 1;
    }
    return run_benchmark(&options) == 0 ? 0 : 1;
}