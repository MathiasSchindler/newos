#include "archive_util.h"
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
    ZipWrittenEntry *entries;
    size_t count;
    size_t capacity;
    int out_fd;
    int recursive;
    int store_only;
    int level;
    int verbose;
    int status;
} ZipContext;

static void print_usage(void) {
    tool_write_usage("zip", "[-0] [-r] [-v] ARCHIVE FILE ...");
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

static int stream_stored_file(ZipContext *context, const char *path, ZipWrittenEntry *entry) {
    unsigned char buffer[ZIP_IO_BUFFER_SIZE];
    unsigned int crc = 0xffffffffU;
    unsigned long long size = 0ULL;
    int input_fd = platform_open_read(path);

    if (input_fd < 0) return -1;
    for (;;) {
        long bytes = platform_read(input_fd, buffer, sizeof(buffer));
        if (bytes < 0) {
            platform_close(input_fd);
            return -1;
        }
        if (bytes == 0) break;
        crc = archive_crc32_update(crc, buffer, (size_t)bytes);
        if (rt_write_all(context->out_fd, buffer, (size_t)bytes) != 0) {
            platform_close(input_fd);
            return -1;
        }
        size += (unsigned long long)bytes;
    }
    platform_close(input_fd);
    entry->crc32 = archive_crc32_finish(crc);
    entry->size = size;
    entry->compressed_size = size;
    entry->method = ZIP_METHOD_STORE;
    return 0;
}

static int write_file_payload(ZipContext *context, const char *path, ZipWrittenEntry *entry) {
    unsigned char *input = 0;
    unsigned char *zlib_data = 0;
    size_t input_size = 0U;
    size_t zlib_size = 0U;
    size_t bound;
    int result = -1;

    if (context->store_only || entry->size < ZIP_DEFLATE_MIN_SIZE) {
        return stream_stored_file(context, path, entry);
    }

    if (tool_read_all_input(path, &input, &input_size) != 0) return -1;
    entry->crc32 = archive_crc32_finish(archive_crc32_update(0xffffffffU, input, input_size));
    entry->size = (unsigned long long)input_size;
    entry->compressed_size = entry->size;
    entry->method = ZIP_METHOD_STORE;

    if (!context->store_only && input_size != 0U) {
        bound = compression_zlib_deflate_bound(input_size);
        if (bound != 0U) {
            zlib_data = (unsigned char *)rt_malloc(bound);
            if (zlib_data != 0 && compression_zlib_deflate_level(input, input_size, zlib_data, bound, &zlib_size, context->level) == 0 && zlib_size > 6U && zlib_size - 6U < input_size) {
                entry->method = ZIP_METHOD_DEFLATE;
                entry->compressed_size = (unsigned long long)(zlib_size - 6U);
                if (rt_write_all(context->out_fd, zlib_data + 2U, zlib_size - 6U) != 0) goto done;
                result = 0;
                goto done;
            }
        }
    }

    if (input_size == 0U || rt_write_all(context->out_fd, input, input_size) == 0) result = 0;

done:
    rt_free(zlib_data);
    rt_free(input);
    return result;
}

static int write_stored_entry(ZipContext *context, const char *path, const PlatformDirEntry *info, const char *name) {
    ZipWrittenEntry entry;
    long long start_offset;
    long long data_offset;
    unsigned char zeros[1] = {0};

    rt_memset(&entry, 0, sizeof(entry));
    rt_copy_string(entry.name, sizeof(entry.name), name);
    entry.mode = info->mode;
    entry.is_dir = info->is_dir;
    entry.method = ZIP_METHOD_STORE;
    entry.mod_time = dos_time_from_entry(info);
    entry.mod_date = dos_date_from_entry(info);
    start_offset = platform_seek(context->out_fd, 0, PLATFORM_SEEK_CUR);
    if (start_offset < 0) return -1;
    entry.offset = (unsigned long long)start_offset;

    if (info->is_dir) {
        entry.crc32 = 0U;
        entry.size = 0ULL;
        entry.compressed_size = 0ULL;
        if (write_local_header(context->out_fd, &entry) != 0) return -1;
    } else {
        if (entry.offset > 0xffffffffULL || info->size > 0xffffffffULL || info->size > ZIP_MAX_INPUT_SIZE) return -1;
        entry.size = info->size;
        entry.compressed_size = info->size;
        entry.crc32 = 0U;
        if (write_local_header(context->out_fd, &entry) != 0) return -1;
        data_offset = platform_seek(context->out_fd, 0, PLATFORM_SEEK_CUR);
        if (data_offset < 0) return -1;
        if (write_file_payload(context, path, &entry) != 0) return -1;
        if (platform_seek(context->out_fd, start_offset, PLATFORM_SEEK_SET) < 0 || write_local_header(context->out_fd, &entry) != 0) return -1;
        if (platform_seek(context->out_fd, data_offset + (long long)entry.compressed_size, PLATFORM_SEEK_SET) < 0) return -1;
        (void)zeros;
    }
    if (context->verbose) {
        rt_write_cstr(1, entry.is_dir ? "adding directory: " : "adding: ");
        rt_write_line(1, entry.name);
    }
    return add_entry_record(context, &entry);
}

static int add_path(ZipContext *context, const char *path, const char *name_override) {
    PlatformDirEntry info;
    char name[ZIP_PATH_CAPACITY];

    if (platform_get_path_info(path, &info) != 0) {
        tool_write_error("zip", "cannot stat: ", path);
        return -1;
    }
    if (normalize_name(name_override != 0 ? name_override : path, info.is_dir, name, sizeof(name)) != 0) {
        tool_write_error("zip", "unsafe or too long path: ", path);
        return -1;
    }
    if (write_stored_entry(context, path, &info, name) != 0) {
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
                add_path(context, child_path, child_name) != 0) {
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
        if (add_path(&context, argv[i], 0) != 0) context.status = 1;
    }
    if (finish_archive(&context) != 0) {
        context.status = 1;
        tool_write_error("zip", "cannot finish archive: ", archive_path);
    }
    if (platform_close(context.out_fd) != 0) context.status = 1;
    if (context.status != 0) (void)platform_remove_file(archive_path);
    rt_free(context.entries);
    return context.status;
}
