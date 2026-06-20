#include "archive_util.h"
#include "concurrency.h"
#include "compression/zlib.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define ZIP_PATH_CAPACITY 4096U
#define ZIP_IO_BUFFER_SIZE 65536U
#define ZIP_MAX_ENTRIES 4096U
#define ZIP_LOCAL_SIG 0x04034b50U
#define ZIP_CENTRAL_SIG 0x02014b50U
#define ZIP_EOCD_SIG 0x06054b50U
#define ZIP_METHOD_STORE 0U
#define ZIP_METHOD_DEFLATE 8U
#define ZIP_MODE_TYPE_MASK 0170000U
#define ZIP_MAX_INPUT_SIZE 268435456ULL
#define ZIP_DEFLATE_MIN_SIZE 1024ULL
#define ZIP_PREPARE_BATCH_MAX 64U
#define ZIP_PREPARE_BATCH_BYTES (64ULL * 1024ULL * 1024ULL)
#define ZIP_DEFAULT_MAX_WORKERS 8U

typedef struct {
    char name[ZIP_PATH_CAPACITY];
    unsigned int crc32;
    unsigned long long size;
    unsigned long long compressed_size;
    unsigned long long offset;
    unsigned int mode;
    unsigned short method;
    int is_dir;
    unsigned short mod_time;
    unsigned short mod_date;
} ZipWrittenEntry;

typedef struct {
    ZipWrittenEntry entry;
    char path[ZIP_PATH_CAPACITY];
    unsigned char *payload;
    size_t payload_size;
    int prepared;
    int status;
} ZipPendingEntry;

typedef struct {
    ZipWrittenEntry *entries;
    size_t count;
    size_t capacity;
    ZipPendingEntry *pending;
    size_t pending_count;
    size_t pending_capacity;
    int out_fd;
    int recursive;
    int store_only;
    int level;
    int verbose;
    int status;
    RtTaskPool pool;
    int pool_initialized;
} ZipContext;

typedef struct {
    ZipContext *context;
    ZipPendingEntry *entries;
} ZipPrepareBatch;

static void print_usage(void) {
    tool_write_usage("zip", "[-0] [-r] [-v] ARCHIVE FILE ...");
}

static unsigned int zip_worker_count_from_env(void) {
    const char *value_text = platform_getenv("NEWOS_ZIP_WORKERS");
    unsigned long long value;
    unsigned int platform_width;

    if (value_text == 0 || value_text[0] == '\0') {
        platform_width = platform_worker_thread_count();
        if (platform_width == 0U) return 0U;
        return platform_width > ZIP_DEFAULT_MAX_WORKERS ? ZIP_DEFAULT_MAX_WORKERS : platform_width;
    }
    if (rt_parse_uint(value_text, &value) != 0) {
        return ZIP_DEFAULT_MAX_WORKERS;
    }
    if (value > RT_TASK_POOL_MAX_WORKERS) {
        return RT_TASK_POOL_MAX_WORKERS;
    }
    return (unsigned int)value;
}

static int write_u16_le(int fd, unsigned int value) {
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xffU);
    return rt_write_all(fd, bytes, sizeof(bytes));
}

static int write_u32_le(int fd, unsigned int value) {
    unsigned char bytes[4];
    archive_store_u32_le(bytes, value);
    return rt_write_all(fd, bytes, sizeof(bytes));
}

static int write_name(int fd, const char *name) {
    return rt_write_all(fd, name, rt_strlen(name));
}

static unsigned short dos_time_from_entry(const PlatformDirEntry *entry) {
    (void)entry;
    return 0U;
}

static unsigned short dos_date_from_entry(const PlatformDirEntry *entry) {
    (void)entry;
    return 0U;
}

static int zip_safe_archive_name(const char *name) {
    const char *cursor = name;

    if (name == 0 || name[0] == '\0' || name[0] == '/' || name[0] == '\\') return 0;
    if (name[1] == ':') return 0;
    while (*cursor != '\0') {
        if (*cursor == '\\') return 0;
        if ((cursor[0] == '.' && cursor[1] == '.' && (cursor[2] == '/' || cursor[2] == '\0')) ||
            (cursor[0] == '/' && cursor[1] == '.' && cursor[2] == '.' && (cursor[3] == '/' || cursor[3] == '\0'))) return 0;
        cursor += 1;
    }
    return 1;
}

static int normalize_name(const char *path, int is_dir, char *buffer, size_t buffer_size) {
    size_t length;
    size_t start = 0U;

    if (path == 0 || path[0] == '\0') return -1;
    while (path[start] == '/' && path[start + 1U] != '\0') start += 1U;
    while (path[start] == '.' && path[start + 1U] == '/' && path[start + 2U] != '\0') start += 2U;
    length = rt_strlen(path + start);
    if (length == 0U || length + (is_dir ? 2U : 1U) > buffer_size) return -1;
    rt_copy_string(buffer, buffer_size, path + start);
    length = rt_strlen(buffer);
    while (length > 1U && buffer[length - 1U] == '/') buffer[--length] = '\0';
    if (is_dir && (length == 0U || buffer[length - 1U] != '/')) {
        if (length + 2U > buffer_size) return -1;
        buffer[length++] = '/';
        buffer[length] = '\0';
    }
    return zip_safe_archive_name(buffer) ? 0 : -1;
}

static int write_local_header(int fd, const ZipWrittenEntry *entry) {
    size_t name_length = rt_strlen(entry->name);

    if (name_length > 0xffffU || entry->size > 0xffffffffULL) return -1;
    return write_u32_le(fd, ZIP_LOCAL_SIG) != 0 ||
           write_u16_le(fd, 20U) != 0 ||
           write_u16_le(fd, 0U) != 0 ||
           write_u16_le(fd, entry->method) != 0 ||
           write_u16_le(fd, entry->mod_time) != 0 ||
           write_u16_le(fd, entry->mod_date) != 0 ||
           write_u32_le(fd, entry->crc32) != 0 ||
           write_u32_le(fd, (unsigned int)entry->compressed_size) != 0 ||
           write_u32_le(fd, (unsigned int)entry->size) != 0 ||
           write_u16_le(fd, (unsigned int)name_length) != 0 ||
           write_u16_le(fd, 0U) != 0 ||
           write_name(fd, entry->name) != 0 ? -1 : 0;
}

static int add_entry_record(ZipContext *context, const ZipWrittenEntry *entry) {
    ZipWrittenEntry *grown;

    if (context->count >= ZIP_MAX_ENTRIES) {
        tool_write_error("zip", "too many entries", 0);
        return -1;
    }
    if (context->count == context->capacity) {
        size_t next_capacity = context->capacity == 0U ? 32U : context->capacity * 2U;
        if (next_capacity > ZIP_MAX_ENTRIES) next_capacity = ZIP_MAX_ENTRIES;
        grown = (ZipWrittenEntry *)rt_realloc_array(context->entries, next_capacity, sizeof(*context->entries));
        if (grown == 0) return -1;
        context->entries = grown;
        context->capacity = next_capacity;
    }
    context->entries[context->count++] = *entry;
    return 0;
}

static int add_pending_entry_record(ZipContext *context, const ZipPendingEntry *entry) {
    ZipPendingEntry *grown;

    if (context->pending_count >= ZIP_MAX_ENTRIES) {
        tool_write_error("zip", "too many entries", 0);
        return -1;
    }
    if (context->pending_count == context->pending_capacity) {
        size_t next_capacity = context->pending_capacity == 0U ? 32U : context->pending_capacity * 2U;
        if (next_capacity > ZIP_MAX_ENTRIES) next_capacity = ZIP_MAX_ENTRIES;
        grown = (ZipPendingEntry *)rt_realloc_array(context->pending, next_capacity, sizeof(*context->pending));
        if (grown == 0) return -1;
        context->pending = grown;
        context->pending_capacity = next_capacity;
    }
    context->pending[context->pending_count++] = *entry;
    return 0;
}

static void release_pending_payload(ZipPendingEntry *pending) {
    rt_free(pending->payload);
    pending->payload = 0;
    pending->payload_size = 0U;
    pending->prepared = 0;
}

static int prepare_file_payload(ZipContext *context, ZipPendingEntry *pending) {
    unsigned char *input = 0;
    unsigned char *zlib_data = 0;
    size_t input_size = 0U;
    size_t zlib_size = 0U;
    size_t bound;

    if (pending->entry.is_dir) {
        pending->prepared = 1;
        pending->status = 0;
        return 0;
    }

    release_pending_payload(pending);
    if (tool_read_all_input(pending->path, &input, &input_size) != 0 || (unsigned long long)input_size > ZIP_MAX_INPUT_SIZE) {
        rt_free(input);
        pending->status = -1;
        return -1;
    }
    pending->entry.crc32 = archive_crc32_finish(archive_crc32_update(0xffffffffU, input, input_size));
    pending->entry.size = (unsigned long long)input_size;
    pending->entry.compressed_size = pending->entry.size;
    pending->entry.method = ZIP_METHOD_STORE;
    pending->payload = input;
    pending->payload_size = input_size;
    pending->prepared = 1;

    if (!context->store_only && input_size >= ZIP_DEFLATE_MIN_SIZE) {
        bound = compression_zlib_deflate_bound(input_size);
        if (bound != 0U) {
            zlib_data = (unsigned char *)rt_malloc(bound);
            if (zlib_data != 0 &&
                compression_zlib_deflate_level(input, input_size, zlib_data, bound, &zlib_size, context->level) == 0 &&
                zlib_size > 6U && zlib_size - 6U < input_size) {
                size_t raw_deflate_size = zlib_size - 6U;
                memmove(zlib_data, zlib_data + 2U, raw_deflate_size);
                pending->entry.method = ZIP_METHOD_DEFLATE;
                pending->entry.compressed_size = (unsigned long long)raw_deflate_size;
                pending->payload = zlib_data;
                pending->payload_size = raw_deflate_size;
                rt_free(input);
                pending->status = 0;
                return 0;
            }
        }
    }

    rt_free(zlib_data);
    pending->status = 0;
    return 0;
}

static int prepare_payload_range(size_t begin, size_t end, unsigned int worker_index, void *arg) {
    ZipPrepareBatch *batch = (ZipPrepareBatch *)arg;
    size_t index;

    (void)worker_index;
    for (index = begin; index < end; ++index) {
        if (prepare_file_payload(batch->context, &batch->entries[index]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int prepare_pending_batch(ZipContext *context, size_t begin, size_t end) {
    ZipPrepareBatch batch;

    if (begin >= end) return 0;
    batch.context = context;
    batch.entries = context->pending + begin;
    if (context->pool_initialized && end - begin > 1U) {
        return rt_parallel_for(&context->pool, end - begin, 1U, prepare_payload_range, &batch);
    }
    return prepare_payload_range(0U, end - begin, 0U, &batch);
}

static int write_prepared_entry(ZipContext *context, ZipPendingEntry *pending) {
    ZipWrittenEntry *entry = &pending->entry;
    long long start_offset;

    start_offset = platform_seek(context->out_fd, 0, PLATFORM_SEEK_CUR);
    if (start_offset < 0) return -1;
    entry->offset = (unsigned long long)start_offset;

    if (entry->is_dir) {
        entry->crc32 = 0U;
        entry->size = 0ULL;
        entry->compressed_size = 0ULL;
        if (write_local_header(context->out_fd, entry) != 0) return -1;
    } else {
        if (!pending->prepared || pending->status != 0 || entry->offset > 0xffffffffULL || entry->size > 0xffffffffULL || entry->compressed_size > 0xffffffffULL) return -1;
        if (write_local_header(context->out_fd, entry) != 0) return -1;
        if (pending->payload_size != 0U && rt_write_all(context->out_fd, pending->payload, pending->payload_size) != 0) return -1;
    }
    if (context->verbose) {
        rt_write_cstr(1, entry->is_dir ? "adding directory: " : "adding: ");
        rt_write_line(1, entry->name);
    }
    return add_entry_record(context, entry);
}

static unsigned long long pending_memory_estimate(const ZipPendingEntry *pending) {
    if (pending->entry.is_dir) return 0ULL;
    return pending->entry.size > ZIP_MAX_INPUT_SIZE ? ZIP_MAX_INPUT_SIZE : pending->entry.size;
}

static int write_pending_entries(ZipContext *context) {
    size_t index = 0U;

    while (index < context->pending_count) {
        size_t begin = index;
        size_t end = begin;
        unsigned long long bytes = 0ULL;

        while (end < context->pending_count && end - begin < ZIP_PREPARE_BATCH_MAX) {
            unsigned long long entry_bytes = pending_memory_estimate(&context->pending[end]);
            if (end > begin && bytes + entry_bytes > ZIP_PREPARE_BATCH_BYTES) {
                break;
            }
            bytes += entry_bytes;
            end += 1U;
        }
        if (prepare_pending_batch(context, begin, end) != 0) return -1;
        for (index = begin; index < end; ++index) {
            if (write_prepared_entry(context, &context->pending[index]) != 0) return -1;
            release_pending_payload(&context->pending[index]);
        }
    }
    return 0;
}

static int collect_path(ZipContext *context, const char *path, const char *name_override) {
    PlatformDirEntry info;
    ZipPendingEntry pending;
    char name[ZIP_PATH_CAPACITY];

    if (platform_get_path_info(path, &info) != 0) {
        tool_write_error("zip", "cannot stat: ", path);
        return -1;
    }
    if (normalize_name(name_override != 0 ? name_override : path, info.is_dir, name, sizeof(name)) != 0) {
        tool_write_error("zip", "unsafe or too long path: ", path);
        return -1;
    }
    if (!info.is_dir && (info.size > 0xffffffffULL || info.size > ZIP_MAX_INPUT_SIZE)) {
        tool_write_error("zip", "file too large: ", path);
        return -1;
    }
    if (rt_strlen(path) + 1U > sizeof(pending.path)) {
        tool_write_error("zip", "path too long: ", path);
        return -1;
    }
    rt_memset(&pending, 0, sizeof(pending));
    rt_copy_string(pending.path, sizeof(pending.path), path);
    rt_copy_string(pending.entry.name, sizeof(pending.entry.name), name);
    pending.entry.mode = info.mode;
    pending.entry.is_dir = info.is_dir;
    pending.entry.method = ZIP_METHOD_STORE;
    pending.entry.mod_time = dos_time_from_entry(&info);
    pending.entry.mod_date = dos_date_from_entry(&info);
    pending.entry.size = info.is_dir ? 0ULL : info.size;
    pending.entry.compressed_size = pending.entry.size;
    if (add_pending_entry_record(context, &pending) != 0) {
        tool_write_error("zip", "cannot add: ", path);
        return -1;
    }
    if (info.is_dir) {
        PlatformDirEntry entries[1024];
        size_t count = 0U;
        size_t i;
        int is_dir = 0;

        if (!context->recursive) return 0;
        if (platform_collect_entries(path, 1, entries, sizeof(entries) / sizeof(entries[0]), &count, &is_dir) != 0 || !is_dir) return -1;
        for (i = 0U; i < count; ++i) {
            char child_path[ZIP_PATH_CAPACITY];
            char child_name[ZIP_PATH_CAPACITY];
            if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) continue;
            if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0 ||
                tool_join_path(name, entries[i].name, child_name, sizeof(child_name)) != 0 ||
                collect_path(context, child_path, child_name) != 0) {
                platform_free_entries(entries, count);
                return -1;
            }
        }
        platform_free_entries(entries, count);
    }
    return 0;
}

static int write_central_header(int fd, const ZipWrittenEntry *entry) {
    size_t name_length = rt_strlen(entry->name);
    unsigned int external_attrs = (entry->mode & 0xffffU) << 16U;
    if (entry->is_dir) external_attrs |= 0x10U;
    if (name_length > 0xffffU || entry->compressed_size > 0xffffffffULL || entry->size > 0xffffffffULL || entry->offset > 0xffffffffULL) return -1;
    return write_u32_le(fd, ZIP_CENTRAL_SIG) != 0 ||
           write_u16_le(fd, 0x031eU) != 0 ||
           write_u16_le(fd, 20U) != 0 ||
           write_u16_le(fd, 0U) != 0 ||
           write_u16_le(fd, entry->method) != 0 ||
           write_u16_le(fd, entry->mod_time) != 0 ||
           write_u16_le(fd, entry->mod_date) != 0 ||
           write_u32_le(fd, entry->crc32) != 0 ||
           write_u32_le(fd, (unsigned int)entry->compressed_size) != 0 ||
           write_u32_le(fd, (unsigned int)entry->size) != 0 ||
           write_u16_le(fd, (unsigned int)name_length) != 0 ||
           write_u16_le(fd, 0U) != 0 ||
           write_u16_le(fd, 0U) != 0 ||
           write_u16_le(fd, 0U) != 0 ||
           write_u16_le(fd, 0U) != 0 ||
           write_u32_le(fd, external_attrs) != 0 ||
           write_u32_le(fd, (unsigned int)entry->offset) != 0 ||
           write_name(fd, entry->name) != 0 ? -1 : 0;
}

static int finish_archive(ZipContext *context) {
    long long central_start;
    long long central_end;
    size_t i;
    unsigned long long central_size;

    central_start = platform_seek(context->out_fd, 0, PLATFORM_SEEK_CUR);
    if (central_start < 0) return -1;
    for (i = 0U; i < context->count; ++i) {
        if (write_central_header(context->out_fd, &context->entries[i]) != 0) return -1;
    }
    central_end = platform_seek(context->out_fd, 0, PLATFORM_SEEK_CUR);
    if (central_end < 0) return -1;
    central_size = (unsigned long long)(central_end - central_start);
    if (context->count > 0xffffU || (unsigned long long)central_start > 0xffffffffULL || central_size > 0xffffffffULL) return -1;
    return write_u32_le(context->out_fd, ZIP_EOCD_SIG) != 0 ||
           write_u16_le(context->out_fd, 0U) != 0 ||
           write_u16_le(context->out_fd, 0U) != 0 ||
           write_u16_le(context->out_fd, (unsigned int)context->count) != 0 ||
           write_u16_le(context->out_fd, (unsigned int)context->count) != 0 ||
           write_u32_le(context->out_fd, (unsigned int)central_size) != 0 ||
           write_u32_le(context->out_fd, (unsigned int)central_start) != 0 ||
           write_u16_le(context->out_fd, 0U) != 0 ? -1 : 0;
}

int main(int argc, char **argv) {
    ZipContext context;
    const char *archive_path;
    int argi = 1;
    int i;

    rt_memset(&context, 0, sizeof(context));
    context.level = 6;
    while (argi < argc) {
        const char *arg = argv[argi];
        if (rt_strcmp(arg, "--help") == 0 || rt_strcmp(arg, "-h") == 0) {
            print_usage();
            return 0;
        }
        if (rt_strcmp(arg, "-r") == 0) {
            context.recursive = 1;
        } else if (rt_strcmp(arg, "-v") == 0) {
            context.verbose = 1;
        } else if (rt_strcmp(arg, "-0") == 0) {
            context.store_only = 1;
        } else if (arg[0] == '-' && arg[1] >= '1' && arg[1] <= '9' && arg[2] == '\0') {
            context.store_only = 0;
            context.level = arg[1] - '0';
        } else if (arg[0] == '-' && arg[1] != '\0') {
            tool_write_error("zip", "unsupported option ", arg);
            return 1;
        } else {
            break;
        }
        argi += 1;
    }
    if (argi >= argc || argi + 1 >= argc) {
        print_usage();
        return 1;
    }
    archive_path = argv[argi++];
    context.out_fd = platform_open_write(archive_path, 0644U);
    if (context.out_fd < 0) {
        tool_write_error("zip", "cannot open archive: ", archive_path);
        return 1;
    }
    for (i = argi; i < argc; ++i) {
        if (collect_path(&context, argv[i], 0) != 0) context.status = 1;
    }
    if (context.status == 0 && context.pending_count > 1U) {
        if (rt_task_pool_init(&context.pool, zip_worker_count_from_env()) == 0) {
            context.pool_initialized = 1;
        } else {
            rt_task_pool_destroy(&context.pool);
        }
    }
    if (context.status == 0 && write_pending_entries(&context) != 0) {
        context.status = 1;
        tool_write_error("zip", "cannot write archive: ", archive_path);
    }
    if (finish_archive(&context) != 0) {
        context.status = 1;
        tool_write_error("zip", "cannot finish archive: ", archive_path);
    }
    if (platform_close(context.out_fd) != 0) context.status = 1;
    if (context.status != 0) (void)platform_remove_file(archive_path);
    if (context.pool_initialized) rt_task_pool_destroy(&context.pool);
    for (i = 0; i < (int)context.pending_count; ++i) release_pending_payload(&context.pending[i]);
    rt_free(context.pending);
    rt_free(context.entries);
    return context.status;
}
