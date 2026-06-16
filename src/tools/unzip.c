#include "archive_zip.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define UNZIP_PATH_CAPACITY 4096U
#define UNZIP_MAX_ENTRY_SIZE 268435456ULL
#define UNZIP_MODE_TYPE_MASK 0170000U
#define UNZIP_MODE_SYMLINK 0120000U

typedef struct {
    int list;
    int test;
    int pipe;
    const char *directory;
    const char *archive_path;
    const char **patterns;
    int pattern_count;
} UnzipOptions;

typedef struct {
    int fd;
    const ArchiveZipInfo *info;
    const UnzipOptions *options;
    int matched;
    int failed;
} UnzipContext;

static void print_usage(void) {
    tool_write_usage("unzip", "[-l|-t|-p] [-d DIR] ARCHIVE [ENTRY ...]");
}

static int entry_is_directory_name(const char *name) {
    size_t length = rt_strlen(name);
    return length > 0U && name[length - 1U] == '/';
}

static int entry_matches(const UnzipOptions *options, const char *name) {
    int i;

    if (options->pattern_count == 0) return 1;
    for (i = 0; i < options->pattern_count; ++i) {
        if (tool_wildcard_match(options->patterns[i], name) || rt_strcmp(options->patterns[i], name) == 0) return 1;
    }
    return 0;
}

static int zip_entry_name_is_safe(const char *name) {
    const char *cursor = name;

    if (name == 0 || name[0] == '\0') return 0;
    if (name[0] == '/' || name[0] == '\\') return 0;
    if (name[1] == ':') return 0;
    while (*cursor != '\0') {
        if (*cursor == '\\') return 0;
        if ((cursor[0] == '.' && cursor[1] == '.' && (cursor[2] == '/' || cursor[2] == '\0')) ||
            (cursor[0] == '/' && cursor[1] == '.' && cursor[2] == '.' && (cursor[3] == '/' || cursor[3] == '\0'))) {
            return 0;
        }
        cursor += 1;
    }
    return 1;
}

static int join_output_path(const char *directory, const char *name, char *buffer, size_t buffer_size) {
    if (!zip_entry_name_is_safe(name)) return -1;
    if (directory == 0 || directory[0] == '\0' || rt_strcmp(directory, ".") == 0) {
        if (rt_strlen(name) + 1U > buffer_size) return -1;
        rt_copy_string(buffer, buffer_size, name);
        return 0;
    }
    return tool_join_path(directory, name, buffer, buffer_size);
}

static int ensure_parent_dirs(const char *path) {
    char prefix[UNZIP_PATH_CAPACITY];
    size_t index = 0U;

    if (path == 0 || path[0] == '\0' || rt_strlen(path) >= sizeof(prefix)) return -1;
    while (path[index] != '\0') {
        PlatformDirEntry entry;
        size_t component_end;

        while (path[index] == '/') index += 1U;
        if (path[index] == '\0') break;
        while (path[index] != '\0' && path[index] != '/') index += 1U;
        if (path[index] == '\0') break;
        component_end = index;
        memcpy(prefix, path, component_end);
        prefix[component_end] = '\0';
        if (prefix[0] == '\0') continue;
        if (platform_get_path_info(prefix, &entry) == 0) {
            if ((entry.mode & UNZIP_MODE_TYPE_MASK) == UNZIP_MODE_SYMLINK || !entry.is_dir) return -1;
        } else if (platform_make_directory(prefix, 0755U) != 0) {
            if (platform_get_path_info(prefix, &entry) != 0 || !entry.is_dir) return -1;
        }
    }
    return 0;
}

static int ensure_directory(const char *path) {
    PlatformDirEntry entry;

    if (ensure_parent_dirs(path) != 0) return -1;
    if (platform_get_path_info(path, &entry) == 0) {
        return entry.is_dir && (entry.mode & UNZIP_MODE_TYPE_MASK) != UNZIP_MODE_SYMLINK ? 0 : -1;
    }
    return platform_make_directory(path, 0755U);
}

static int write_entry_file(const char *path, const unsigned char *data, size_t size) {
    PlatformDirEntry entry;
    int fd;
    size_t written = 0U;

    if (ensure_parent_dirs(path) != 0) return -1;
    if (platform_get_path_info(path, &entry) == 0 && (entry.is_dir || (entry.mode & UNZIP_MODE_TYPE_MASK) == UNZIP_MODE_SYMLINK)) return -1;
    fd = platform_open_write(path, 0644U);
    if (fd < 0) return -1;
    while (written < size) {
        long chunk = platform_write(fd, data + written, size - written);
        if (chunk <= 0) {
            platform_close(fd);
            return -1;
        }
        written += (size_t)chunk;
    }
    return platform_close(fd);
}

static int list_entry(const ArchiveZipEntry *entry, void *user_data) {
    UnzipContext *context = (UnzipContext *)user_data;

    if (!entry_matches(context->options, entry->name)) return 0;
    context->matched = 1;
    if (rt_write_uint(1, entry->uncompressed_size) != 0 || rt_write_char(1, '\t') != 0 ||
        rt_write_cstr(1, archive_zip_method_name(entry->method)) != 0 || rt_write_char(1, '\t') != 0 ||
        rt_write_line(1, entry->name) != 0) {
        context->failed = 1;
        return -1;
    }
    return 0;
}

static int test_entry(const ArchiveZipEntry *entry, void *user_data) {
    UnzipContext *context = (UnzipContext *)user_data;
    unsigned char *data = 0;
    size_t data_size = 0U;

    if (!entry_matches(context->options, entry->name)) return 0;
    context->matched = 1;
    if (entry_is_directory_name(entry->name)) return 0;
    if (archive_zip_read_entry_data(context->fd, context->info, entry, UNZIP_MAX_ENTRY_SIZE, &data, &data_size) != 0) {
        tool_write_error("unzip", "cannot test entry: ", entry->name);
        context->failed = 1;
        return 0;
    }
    rt_free(data);
    (void)data_size;
    return 0;
}

static int extract_entry(const ArchiveZipEntry *entry, void *user_data) {
    UnzipContext *context = (UnzipContext *)user_data;
    const UnzipOptions *options = context->options;
    unsigned char *data = 0;
    size_t data_size = 0U;
    char output_path[UNZIP_PATH_CAPACITY];
    int result;

    if (!entry_matches(options, entry->name)) return 0;
    context->matched = 1;
    if (join_output_path(options->directory, entry->name, output_path, sizeof(output_path)) != 0) {
        tool_write_error("unzip", "unsafe or too long entry path: ", entry->name);
        context->failed = 1;
        return 0;
    }
    if (entry_is_directory_name(entry->name)) {
        if (!options->pipe && ensure_directory(output_path) != 0) {
            tool_write_error("unzip", "cannot create directory: ", output_path);
            context->failed = 1;
        }
        return 0;
    }
    if (archive_zip_read_entry_data(context->fd, context->info, entry, UNZIP_MAX_ENTRY_SIZE, &data, &data_size) != 0) {
        tool_write_error("unzip", "cannot read entry: ", entry->name);
        context->failed = 1;
        return 0;
    }
    if (options->pipe) {
        result = rt_write_all(1, data, data_size);
    } else {
        result = write_entry_file(output_path, data, data_size);
        if (result == 0 && rt_write_cstr(1, "inflating: ") == 0) (void)rt_write_line(1, output_path);
    }
    rt_free(data);
    if (result != 0) {
        tool_write_error("unzip", options->pipe ? "cannot write entry: " : "cannot extract entry: ", entry->name);
        context->failed = 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    UnzipOptions options;
    ArchiveZipInfo info;
    UnzipContext context;
    int argi = 1;
    int fd;
    int status;

    rt_memset(&options, 0, sizeof(options));
    options.directory = ".";
    while (argi < argc) {
        const char *arg = argv[argi];
        if (rt_strcmp(arg, "--help") == 0 || rt_strcmp(arg, "-h") == 0) {
            print_usage();
            return 0;
        }
        if (rt_strcmp(arg, "-l") == 0) {
            options.list = 1;
        } else if (rt_strcmp(arg, "-t") == 0) {
            options.test = 1;
        } else if (rt_strcmp(arg, "-p") == 0) {
            options.pipe = 1;
        } else if (rt_strcmp(arg, "-d") == 0) {
            if (argi + 1 >= argc) { print_usage(); return 1; }
            options.directory = argv[++argi];
        } else if (arg[0] == '-' && arg[1] != '\0') {
            tool_write_error("unzip", "unsupported option ", arg);
            return 1;
        } else {
            break;
        }
        argi += 1;
    }
    if (argi >= argc || (options.list + options.test + options.pipe) > 1) {
        print_usage();
        return 1;
    }
    options.archive_path = argv[argi++];
    options.patterns = (const char **)(argv + argi);
    options.pattern_count = argc - argi;

    fd = platform_open_read(options.archive_path);
    if (fd < 0) {
        tool_write_error("unzip", "cannot open archive: ", options.archive_path);
        return 1;
    }
    if (archive_zip_read_info(fd, &info) != 0 || info.multi_disk) {
        platform_close(fd);
        tool_write_error("unzip", "not a supported ZIP archive: ", options.archive_path);
        return 1;
    }
    rt_memset(&context, 0, sizeof(context));
    context.fd = fd;
    context.info = &info;
    context.options = &options;
    if (options.list) {
        status = archive_zip_iterate_entries(fd, &info, list_entry, &context);
    } else if (options.test) {
        status = archive_zip_iterate_entries(fd, &info, test_entry, &context);
        if (status == 0 && !context.failed && rt_write_line(1, "No errors detected") != 0) status = -1;
    } else {
        status = archive_zip_iterate_entries(fd, &info, extract_entry, &context);
    }
    platform_close(fd);
    if (status != 0 || context.failed || !context.matched) {
        if (!context.matched) tool_write_error("unzip", "no matching entries in ", options.archive_path);
        return 1;
    }
    return 0;
}
