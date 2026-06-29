#include "hash_util.h"
#include "concurrency.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define HASH_STREAM_BUFFER_SIZE 65536
#define HASH_CHECKSUM_RECORD_CAPACITY 4096
#define HASH_DEFAULT_MAX_WORKERS 8U

#define HASH_SUM_STATUS_OK 0
#define HASH_SUM_STATUS_OPEN_ERROR 1
#define HASH_SUM_STATUS_HASH_ERROR 2

typedef struct {
    unsigned char digest[HASH_SHA512_SIZE];
    int status;
} HashSumResult;

typedef struct {
    char **paths;
    HashSumResult *results;
    HashStreamFunction hash_stream;
} HashSumBatch;

void hash_to_hex(const unsigned char *digest, size_t digest_size, char *hex_out) {
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i < digest_size; ++i) {
        hex_out[i * 2U] = digits[(digest[i] >> 4) & 0x0fU];
        hex_out[i * 2U + 1U] = digits[digest[i] & 0x0fU];
    }

    hex_out[digest_size * 2U] = '\0';
}

int hash_print_digest_line(const unsigned char *digest, size_t digest_size, const char *label, int binary_mode, int zero_terminated) {
    char hex[HASH_SHA512_SIZE * 2 + 1];

    hash_to_hex(digest, digest_size, hex);
    if (rt_write_cstr(1, hex) != 0 ||
        rt_write_char(1, ' ') != 0 ||
        rt_write_char(1, binary_mode ? '*' : ' ') != 0 ||
        rt_write_cstr(1, label) != 0 ||
        rt_write_char(1, zero_terminated ? '\0' : '\n') != 0) {
        return -1;
    }
    return 0;
}

static int hash_path_is_stdin(const char *path) {
    return path != 0 && path[0] == '-' && path[1] == '\0';
}

static int hash_hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

int hash_compare_digest(const unsigned char *lhs, const unsigned char *rhs, size_t digest_size) {
    size_t i;
    for (i = 0; i < digest_size; ++i) {
        if (lhs[i] != rhs[i]) {
            return 0;
        }
    }
    return 1;
}

int hash_read_record(int fd, int zero_terminated, char *buffer, size_t buffer_size, int *has_record_out) {
    size_t used = 0;
    char ch;
    long bytes_read;

    *has_record_out = 0;
    while ((bytes_read = platform_read(fd, &ch, 1U)) > 0) {
        *has_record_out = 1;
        if ((zero_terminated && ch == '\0') || (!zero_terminated && ch == '\n')) {
            break;
        }
        if (used + 1U >= buffer_size) {
            return -1;
        }
        buffer[used++] = ch;
    }

    if (bytes_read < 0) {
        return -1;
    }
    if (!*has_record_out && used == 0U) {
        buffer[0] = '\0';
        return 0;
    }

    if (!zero_terminated && used > 0U && buffer[used - 1U] == '\r') {
        used -= 1U;
    }
    buffer[used] = '\0';
    return 1;
}

int hash_parse_check_record(const char *record, size_t digest_size, unsigned char *expected_digest, const char **path_out) {
    size_t index = 0;
    size_t i;

    if (record[0] == '\0') {
        return 1;
    }

    for (i = 0; i < digest_size; ++i) {
        int high;
        int low;

        if (record[index] == '\0' || record[index + 1U] == '\0') {
            return -1;
        }
        high = hash_hex_value(record[index++]);
        low = hash_hex_value(record[index++]);
        if (high < 0 || low < 0) {
            return -1;
        }
        expected_digest[i] = (unsigned char)((high << 4) | low);
    }

    if (record[index] != ' ' && record[index] != '*') {
        return -1;
    }
    while (record[index] == ' ') {
        index += 1U;
    }
    if (record[index] == '*') {
        index += 1U;
    }
    while (record[index] == ' ') {
        index += 1U;
    }
    if (record[index] == '\0') {
        return -1;
    }

    *path_out = record + index;
    return 0;
}

int hash_verify_manifest(const char *tool_name, int fd, size_t digest_size, HashStreamFunction hash_stream, int zero_terminated, int quiet, int status_only) {
    char record[HASH_CHECKSUM_RECORD_CAPACITY];
    int exit_code = 0;

    for (;;) {
        int has_record = 0;
        int read_status = hash_read_record(fd, zero_terminated, record, sizeof(record), &has_record);

        if (read_status < 0) {
            tool_write_error(tool_name, "failed to read checksum list", 0);
            return 1;
        }
        if (read_status == 0) {
            break;
        }
        if (!has_record || record[0] == '\0') {
            continue;
        }

        {
            unsigned char expected[HASH_SHA512_SIZE];
            unsigned char actual[HASH_SHA512_SIZE];
            const char *path = 0;
            int target_fd = -1;
            int should_close = 0;
            int ok = 0;

            if (digest_size > HASH_SHA512_SIZE || hash_parse_check_record(record, digest_size, expected, &path) != 0) {
                if (!status_only) {
                    tool_write_error(tool_name, "invalid checksum line", 0);
                }
                exit_code = 1;
                continue;
            }

            if (tool_open_input(path, &target_fd, &should_close) == 0 &&
                hash_stream(target_fd, actual) == 0 &&
                hash_compare_digest(expected, actual, digest_size)) {
                ok = 1;
            }

            if (should_close) {
                tool_close_input(target_fd, should_close);
            }

            if (!status_only) {
                if (ok) {
                    if (!quiet) {
                        rt_write_cstr(1, path);
                        rt_write_line(1, ": OK");
                    }
                } else {
                    rt_write_cstr(1, path);
                    rt_write_line(1, ": FAILED");
                }
            }

            if (!ok) {
                exit_code = 1;
            }
        }
    }

    return exit_code;
}

static int hash_sum_file(const char *path, HashStreamFunction hash_stream, unsigned char digest[HASH_SHA512_SIZE]) {
    int fd;
    int should_close;
    int result;

    if (tool_open_input(path, &fd, &should_close) != 0) {
        return HASH_SUM_STATUS_OPEN_ERROR;
    }
    result = hash_stream(fd, digest) == 0 ? HASH_SUM_STATUS_OK : HASH_SUM_STATUS_HASH_ERROR;
    tool_close_input(fd, should_close);
    return result;
}

static int hash_sum_file_range(size_t begin, size_t end, unsigned int worker_index, void *arg) {
    HashSumBatch *batch = (HashSumBatch *)arg;
    size_t index;

    (void)worker_index;
    for (index = begin; index < end; ++index) {
        batch->results[index].status = hash_sum_file(batch->paths[index], batch->hash_stream, batch->results[index].digest);
    }
    return 0;
}

static int hash_sum_print_results(const char *tool_name, size_t digest_size, char **paths, HashSumResult *results, size_t count, int binary_mode, int zero_terminated) {
    int exit_code = 0;
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (results[index].status == HASH_SUM_STATUS_OPEN_ERROR) {
            rt_write_cstr(2, tool_name);
            rt_write_cstr(2, ": cannot open ");
            rt_write_line(2, paths[index]);
            exit_code = 1;
            continue;
        }
        if (results[index].status != HASH_SUM_STATUS_OK ||
            hash_print_digest_line(results[index].digest, digest_size, paths[index], binary_mode, zero_terminated) != 0) {
            rt_write_cstr(2, tool_name);
            rt_write_cstr(2, ": failed on ");
            rt_write_line(2, paths[index]);
            exit_code = 1;
        }
    }
    return exit_code;
}

static int hash_sum_files_parallel(const char *tool_name, size_t digest_size, HashStreamFunction hash_stream, int binary_mode, int zero_terminated, int file_count, char **paths) {
    RtTaskPool pool;
    HashSumBatch batch;
    HashSumResult *results;
    int result;

    results = (HashSumResult *)rt_malloc_array((size_t)file_count, sizeof(*results));
    if (results == 0) {
        return -1;
    }
    rt_memset(results, 0, (size_t)file_count * sizeof(*results));
    rt_memset(&pool, 0, sizeof(pool));
    batch.paths = paths;
    batch.results = results;
    batch.hash_stream = hash_stream;

    if (rt_task_pool_init(&pool, tool_worker_count_from_env("NEWOS_HASH_WORKERS", HASH_DEFAULT_MAX_WORKERS)) != 0) {
        result = hash_sum_file_range(0U, (size_t)file_count, 0U, &batch);
    } else {
        result = rt_parallel_for(&pool, (size_t)file_count, 1U, hash_sum_file_range, &batch);
        rt_task_pool_destroy(&pool);
    }
    if (result == 0) {
        result = hash_sum_print_results(tool_name, digest_size, paths, results, (size_t)file_count, binary_mode, zero_terminated);
    } else {
        result = 1;
    }
    rt_free(results);
    return result;
}

int hash_sum_main(int argc, char **argv, const char *tool_name, size_t digest_size, HashStreamFunction hash_stream, int allow_binary_mode) {
    const char *usage = allow_binary_mode ? "[-bct] [-q] [-s] [-z] [FILE ...]" : "[-c] [-q] [-s] [-z] [FILE ...]";
    int binary_mode = 0;
    int check_mode = 0;
    int quiet = 0;
    int status_only = 0;
    int zero_terminated = 0;
    int exit_code = 0;
    int argi = 1;
    int i;

    if (digest_size > HASH_SHA512_SIZE) return 1;
    while (argi < argc) {
        const char *arg = argv[argi];
        size_t j;

        if (rt_strcmp(arg, "--") == 0) {
            argi += 1;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') {
            break;
        }
        if (rt_strcmp(arg, "--check") == 0) {
            check_mode = 1;
            argi += 1;
            continue;
        }
        if (allow_binary_mode && rt_strcmp(arg, "--binary") == 0) {
            binary_mode = 1;
            argi += 1;
            continue;
        }
        if (allow_binary_mode && rt_strcmp(arg, "--text") == 0) {
            binary_mode = 0;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "--quiet") == 0) {
            quiet = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "--status") == 0) {
            status_only = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "--zero") == 0) {
            zero_terminated = 1;
            argi += 1;
            continue;
        }

        for (j = 1; arg[j] != '\0'; ++j) {
            if (allow_binary_mode && arg[j] == 'b') {
                binary_mode = 1;
            } else if (arg[j] == 'c') {
                check_mode = 1;
            } else if (arg[j] == 'q') {
                quiet = 1;
            } else if (arg[j] == 's') {
                status_only = 1;
            } else if (allow_binary_mode && arg[j] == 't') {
                binary_mode = 0;
            } else if (arg[j] == 'z') {
                zero_terminated = 1;
            } else {
                tool_write_usage(argv[0], usage);
                return 1;
            }
        }
        argi += 1;
    }

    if (status_only) {
        quiet = 1;
    }

    if (check_mode) {
        if (argi >= argc) {
            return hash_verify_manifest(tool_name, 0, digest_size, hash_stream, zero_terminated, quiet, status_only);
        }

        for (i = argi; i < argc; ++i) {
            int fd;
            int should_close;

            if (tool_open_input(argv[i], &fd, &should_close) != 0) {
                rt_write_cstr(2, tool_name);
                rt_write_cstr(2, ": cannot open ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
                continue;
            }
            if (hash_verify_manifest(tool_name, fd, digest_size, hash_stream, zero_terminated, quiet, status_only) != 0) {
                exit_code = 1;
            }
            tool_close_input(fd, should_close);
        }
        return exit_code;
    }

    if (argi >= argc) {
        unsigned char digest[HASH_SHA512_SIZE];
        if (hash_stream(0, digest) != 0 || hash_print_digest_line(digest, digest_size, "-", binary_mode, zero_terminated) != 0) {
            return 1;
        }
        return 0;
    }

    for (i = argi; i < argc; ++i) {
        if (hash_path_is_stdin(argv[i])) {
            break;
        }
    }
    if (i >= argc && argc - argi > 1) {
        int parallel_result = hash_sum_files_parallel(tool_name, digest_size, hash_stream, binary_mode, zero_terminated, argc - argi, argv + argi);
        if (parallel_result >= 0) {
            return parallel_result;
        }
    }

    for (i = argi; i < argc; ++i) {
        int fd;
        int should_close;
        unsigned char digest[HASH_SHA512_SIZE];

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, tool_name);
            rt_write_cstr(2, ": cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (hash_stream(fd, digest) != 0 || hash_print_digest_line(digest, digest_size, argv[i], binary_mode, zero_terminated) != 0) {
            rt_write_cstr(2, tool_name);
            rt_write_cstr(2, ": failed on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}

int hash_md5_stream(int fd, unsigned char out[HASH_MD5_SIZE]) {
    CryptoMd5Context ctx;
    unsigned char buffer[HASH_STREAM_BUFFER_SIZE];

    crypto_md5_init(&ctx);
    for (;;) {
        long bytes = platform_read(fd, buffer, sizeof(buffer));
        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            break;
        }
        crypto_md5_update(&ctx, buffer, (size_t)bytes);
    }
    crypto_md5_final(&ctx, out);
    return 0;
}

int hash_sha1_stream(int fd, unsigned char out[HASH_SHA1_SIZE]) {
    CryptoSha1Context ctx;
    unsigned char buffer[HASH_STREAM_BUFFER_SIZE];

    crypto_sha1_init(&ctx);
    for (;;) {
        long bytes = platform_read(fd, buffer, sizeof(buffer));
        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            break;
        }
        crypto_sha1_update(&ctx, buffer, (size_t)bytes);
    }
    crypto_sha1_final(&ctx, out);
    return 0;
}

int hash_sha256_stream(int fd, unsigned char out[HASH_SHA256_SIZE]) {
    CryptoSha256Context ctx;
    unsigned char buffer[HASH_STREAM_BUFFER_SIZE];

    crypto_sha256_init(&ctx);
    for (;;) {
        long bytes = platform_read(fd, buffer, sizeof(buffer));
        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            break;
        }
        crypto_sha256_update(&ctx, buffer, (size_t)bytes);
    }
    crypto_sha256_final(&ctx, out);
    return 0;
}

int hash_sha512_stream(int fd, unsigned char out[HASH_SHA512_SIZE]) {
    CryptoSha512Context ctx;
    unsigned char buffer[HASH_STREAM_BUFFER_SIZE];

    crypto_sha512_init(&ctx);
    for (;;) {
        long bytes = platform_read(fd, buffer, sizeof(buffer));
        if (bytes < 0) {
            return -1;
        }
        if (bytes == 0) {
            break;
        }
        crypto_sha512_update(&ctx, buffer, (size_t)bytes);
    }
    crypto_sha512_final(&ctx, out);
    return 0;
}
