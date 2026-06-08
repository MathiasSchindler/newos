#include "archive_zip.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

typedef struct {
    int show_summary;
    int show_entries;
    int json;
} ReadApkOptions;

typedef struct {
    unsigned long long manifest_count;
    unsigned long long dex_count;
    unsigned long long resources_count;
    unsigned long long native_library_count;
    unsigned long long asset_count;
    unsigned long long res_count;
    unsigned long long meta_inf_count;
    unsigned long long v1_signature_count;
    unsigned long long v1_signature_manifest_count;
    unsigned long long encrypted_count;
    unsigned long long data_descriptor_count;
    unsigned long long zip64_entry_count;
    unsigned long long deflated_count;
    unsigned long long stored_count;
    unsigned long long other_method_count;
} ReadApkStats;

typedef struct {
    const ReadApkOptions *options;
    ReadApkStats *stats;
    const char *path;
} ReadApkEntryContext;

static void print_usage(void) {
    tool_write_usage("readapk", "[-a] [-s] [-l] [--json] FILE ...");
}

static int text_starts_with(const char *text, const char *prefix) {
    return rt_strncmp(text, prefix, rt_strlen(prefix)) == 0;
}

static char ascii_lower(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (char)(ch - 'A' + 'a');
    }
    return ch;
}

static int text_ends_with_ignore_case(const char *text, const char *suffix) {
    size_t text_length = rt_strlen(text);
    size_t suffix_length = rt_strlen(suffix);
    size_t index;

    if (suffix_length > text_length) {
        return 0;
    }
    for (index = 0U; index < suffix_length; ++index) {
        if (ascii_lower(text[text_length - suffix_length + index]) != ascii_lower(suffix[index])) {
            return 0;
        }
    }
    return 1;
}

static int is_probable_apk(const ReadApkStats *stats, const ArchiveZipSigningBlock *signing_block) {
    return stats->manifest_count > 0ULL || stats->dex_count > 0ULL || stats->resources_count > 0ULL ||
           stats->native_library_count > 0ULL || signing_block->present;
}

static void classify_entry(const ArchiveZipEntry *entry, ReadApkStats *stats) {
    const char *name = entry->name;

    if (rt_strcmp(name, "AndroidManifest.xml") == 0) {
        stats->manifest_count += 1ULL;
    }
    if (rt_strcmp(name, "resources.arsc") == 0) {
        stats->resources_count += 1ULL;
    }
    if (text_ends_with_ignore_case(name, ".dex")) {
        stats->dex_count += 1ULL;
    }
    if (text_starts_with(name, "lib/") && text_ends_with_ignore_case(name, ".so")) {
        stats->native_library_count += 1ULL;
    }
    if (text_starts_with(name, "assets/")) {
        stats->asset_count += 1ULL;
    }
    if (text_starts_with(name, "res/")) {
        stats->res_count += 1ULL;
    }
    if (text_starts_with(name, "META-INF/")) {
        stats->meta_inf_count += 1ULL;
        if (text_ends_with_ignore_case(name, ".RSA") || text_ends_with_ignore_case(name, ".DSA") || text_ends_with_ignore_case(name, ".EC")) {
            stats->v1_signature_count += 1ULL;
        }
        if (text_ends_with_ignore_case(name, ".SF")) {
            stats->v1_signature_manifest_count += 1ULL;
        }
    }
    if ((entry->flags & ARCHIVE_ZIP_FLAG_ENCRYPTED) != 0U) {
        stats->encrypted_count += 1ULL;
    }
    if ((entry->flags & ARCHIVE_ZIP_FLAG_DATA_DESCRIPTOR) != 0U) {
        stats->data_descriptor_count += 1ULL;
    }
    if (entry->zip64) {
        stats->zip64_entry_count += 1ULL;
    }
    if (entry->method == ARCHIVE_ZIP_METHOD_STORE) {
        stats->stored_count += 1ULL;
    } else if (entry->method == ARCHIVE_ZIP_METHOD_DEFLATE) {
        stats->deflated_count += 1ULL;
    } else {
        stats->other_method_count += 1ULL;
    }
}

static int write_bool_text(int fd, int value) {
    return rt_write_cstr(fd, value ? "yes" : "no");
}

static void format_hex32(unsigned int value, char *digits);

static int write_hex32(int fd, unsigned int value) {
    char digits[9];

    format_hex32(value, digits);
    return rt_write_cstr(fd, digits);
}

static void format_hex32(unsigned int value, char *digits) {
    unsigned int index;

    for (index = 0U; index < 8U; ++index) {
        unsigned int nibble = (value >> ((7U - index) * 4U)) & 0xfU;
        digits[index] = (char)(nibble < 10U ? ('0' + nibble) : ('a' + nibble - 10U));
    }
    digits[8] = '\0';
}

static int write_entry_flags(int fd, const ArchiveZipEntry *entry) {
    if (rt_write_char(fd, (entry->flags & ARCHIVE_ZIP_FLAG_ENCRYPTED) != 0U ? 'E' : '-') != 0) return -1;
    if (rt_write_char(fd, (entry->flags & ARCHIVE_ZIP_FLAG_DATA_DESCRIPTOR) != 0U ? 'D' : '-') != 0) return -1;
    if (rt_write_char(fd, (entry->flags & ARCHIVE_ZIP_FLAG_UTF8) != 0U ? 'U' : '-') != 0) return -1;
    if (rt_write_char(fd, entry->zip64 ? 'Z' : '-') != 0) return -1;
    return 0;
}

static int write_json_summary(const char *path, const ArchiveZipInfo *info, const ArchiveZipSigningBlock *signing_block, const ReadApkStats *stats) {
    if (tool_json_begin_event(1, "readapk", "stdout", "apk_summary") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{\"file\":") != 0) return -1;
    if (tool_json_write_string(1, path) != 0) return -1;
    if (rt_write_cstr(1, ",\"probable_apk\":") != 0) return -1;
    if (rt_write_cstr(1, is_probable_apk(stats, signing_block) ? "true" : "false") != 0) return -1;
    if (rt_write_cstr(1, ",\"entries\":") != 0) return -1;
    if (rt_write_uint(1, info->entry_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"central_directory_offset\":") != 0) return -1;
    if (rt_write_uint(1, info->central_directory_offset) != 0) return -1;
    if (rt_write_cstr(1, ",\"central_directory_size\":") != 0) return -1;
    if (rt_write_uint(1, info->central_directory_size) != 0) return -1;
    if (rt_write_cstr(1, ",\"zip64\":") != 0) return -1;
    if (rt_write_cstr(1, info->zip64 ? "true" : "false") != 0) return -1;
    if (rt_write_cstr(1, ",\"manifest\":") != 0) return -1;
    if (rt_write_uint(1, stats->manifest_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"dex_files\":") != 0) return -1;
    if (rt_write_uint(1, stats->dex_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"resources_arsc\":") != 0) return -1;
    if (rt_write_uint(1, stats->resources_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"native_libraries\":") != 0) return -1;
    if (rt_write_uint(1, stats->native_library_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"assets\":") != 0) return -1;
    if (rt_write_uint(1, stats->asset_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"res_files\":") != 0) return -1;
    if (rt_write_uint(1, stats->res_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"v1_signature_files\":") != 0) return -1;
    if (rt_write_uint(1, stats->v1_signature_count) != 0) return -1;
    if (rt_write_cstr(1, ",\"apk_signing_block\":") != 0) return -1;
    if (rt_write_cstr(1, signing_block->present ? "true" : "false") != 0) return -1;
    if (rt_write_cstr(1, ",\"apk_signature_v2\":") != 0) return -1;
    if (rt_write_cstr(1, signing_block->v2 ? "true" : "false") != 0) return -1;
    if (rt_write_cstr(1, ",\"apk_signature_v3\":") != 0) return -1;
    if (rt_write_cstr(1, signing_block->v3 ? "true" : "false") != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int write_json_entry(const char *path, const ArchiveZipEntry *entry) {
    char crc_text[9];

    format_hex32(entry->crc32, crc_text);
    if (tool_json_begin_event(1, "readapk", "stdout", "apk_entry") != 0) return -1;
    if (rt_write_cstr(1, ",\"data\":{\"file\":") != 0) return -1;
    if (tool_json_write_string(1, path) != 0) return -1;
    if (rt_write_cstr(1, ",\"name\":") != 0) return -1;
    if (tool_json_write_string(1, entry->name) != 0) return -1;
    if (rt_write_cstr(1, ",\"method\":") != 0) return -1;
    if (tool_json_write_string(1, archive_zip_method_name(entry->method)) != 0) return -1;
    if (rt_write_cstr(1, ",\"method_id\":") != 0) return -1;
    if (rt_write_uint(1, entry->method) != 0) return -1;
    if (rt_write_cstr(1, ",\"compressed_size\":") != 0) return -1;
    if (rt_write_uint(1, entry->compressed_size) != 0) return -1;
    if (rt_write_cstr(1, ",\"uncompressed_size\":") != 0) return -1;
    if (rt_write_uint(1, entry->uncompressed_size) != 0) return -1;
    if (rt_write_cstr(1, ",\"crc32\":") != 0) return -1;
    if (tool_json_write_string(1, crc_text) != 0) return -1;
    if (rt_write_cstr(1, ",\"local_header_offset\":") != 0) return -1;
    if (rt_write_uint(1, entry->local_header_offset) != 0) return -1;
    if (rt_write_cstr(1, ",\"encrypted\":") != 0) return -1;
    if (rt_write_cstr(1, (entry->flags & ARCHIVE_ZIP_FLAG_ENCRYPTED) != 0U ? "true" : "false") != 0) return -1;
    if (rt_write_cstr(1, ",\"utf8\":") != 0) return -1;
    if (rt_write_cstr(1, (entry->flags & ARCHIVE_ZIP_FLAG_UTF8) != 0U ? "true" : "false") != 0) return -1;
    if (rt_write_cstr(1, ",\"zip64\":") != 0) return -1;
    if (rt_write_cstr(1, entry->zip64 ? "true" : "false") != 0) return -1;
    if (rt_write_char(1, '}') != 0) return -1;
    return tool_json_end_event(1);
}

static int print_summary(const char *path, const ArchiveZipInfo *info, const ArchiveZipSigningBlock *signing_block, const ReadApkStats *stats) {
    if (rt_write_cstr(1, "File: ") != 0 || rt_write_line(1, path) != 0) return -1;
    if (rt_write_cstr(1, "Type: ") != 0 || rt_write_line(1, is_probable_apk(stats, signing_block) ? "Android APK / ZIP archive" : "ZIP archive (APK features not detected)") != 0) return -1;
    if (rt_write_cstr(1, "Entries: ") != 0 || rt_write_uint(1, info->entry_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "Central directory: offset ") != 0 || rt_write_uint(1, info->central_directory_offset) != 0) return -1;
    if (rt_write_cstr(1, ", size ") != 0 || rt_write_uint(1, info->central_directory_size) != 0) return -1;
    if (rt_write_cstr(1, ", comment ") != 0 || rt_write_uint(1, info->comment_length) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "Zip64: ") != 0 || write_bool_text(1, info->zip64) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "Multi-disk: ") != 0 || write_bool_text(1, info->multi_disk) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "AndroidManifest.xml: ") != 0 || rt_write_uint(1, stats->manifest_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "resources.arsc: ") != 0 || rt_write_uint(1, stats->resources_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "DEX files: ") != 0 || rt_write_uint(1, stats->dex_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "Native libraries: ") != 0 || rt_write_uint(1, stats->native_library_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "res/ files: ") != 0 || rt_write_uint(1, stats->res_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "assets/ files: ") != 0 || rt_write_uint(1, stats->asset_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "META-INF files: ") != 0 || rt_write_uint(1, stats->meta_inf_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    if (rt_write_cstr(1, "APK v1 signature files: ") != 0 || rt_write_uint(1, stats->v1_signature_count) != 0) return -1;
    if (rt_write_cstr(1, " cert, ") != 0 || rt_write_uint(1, stats->v1_signature_manifest_count) != 0 || rt_write_line(1, " manifest") != 0) return -1;
    if (rt_write_cstr(1, "APK Signing Block: ") != 0 || write_bool_text(1, signing_block->present) != 0) return -1;
    if (signing_block->present) {
        if (rt_write_cstr(1, " offset ") != 0 || rt_write_uint(1, signing_block->offset) != 0) return -1;
        if (rt_write_cstr(1, ", size ") != 0 || rt_write_uint(1, signing_block->size) != 0) return -1;
        if (rt_write_cstr(1, ", v2 ") != 0 || write_bool_text(1, signing_block->v2) != 0) return -1;
        if (rt_write_cstr(1, ", v3 ") != 0 || write_bool_text(1, signing_block->v3) != 0) return -1;
        if (rt_write_cstr(1, ", v3.1 ") != 0 || write_bool_text(1, signing_block->v31) != 0) return -1;
    }
    if (rt_write_char(1, '\n') != 0) return -1;
    if (stats->encrypted_count > 0ULL || stats->data_descriptor_count > 0ULL || stats->zip64_entry_count > 0ULL) {
        if (rt_write_cstr(1, "ZIP flags: encrypted ") != 0 || rt_write_uint(1, stats->encrypted_count) != 0) return -1;
        if (rt_write_cstr(1, ", data-descriptor ") != 0 || rt_write_uint(1, stats->data_descriptor_count) != 0) return -1;
        if (rt_write_cstr(1, ", zip64 entries ") != 0 || rt_write_uint(1, stats->zip64_entry_count) != 0 || rt_write_char(1, '\n') != 0) return -1;
    }
    return 0;
}

static int print_entry(const ArchiveZipEntry *entry, void *user_data) {
    ReadApkEntryContext *context = (ReadApkEntryContext *)user_data;

    classify_entry(entry, context->stats);
    if (context->options->json && context->options->show_entries) {
        return write_json_entry(context->path, entry);
    }
    if (!context->options->show_entries || context->options->json) {
        return 0;
    }
    if (rt_write_uint(1, entry->local_header_offset) != 0 || rt_write_char(1, '\t') != 0) return -1;
    if (rt_write_cstr(1, archive_zip_method_name(entry->method)) != 0 || rt_write_char(1, '\t') != 0) return -1;
    if (rt_write_uint(1, entry->compressed_size) != 0 || rt_write_char(1, '\t') != 0) return -1;
    if (rt_write_uint(1, entry->uncompressed_size) != 0 || rt_write_char(1, '\t') != 0) return -1;
    if (write_hex32(1, entry->crc32) != 0 || rt_write_char(1, '\t') != 0) return -1;
    if (write_entry_flags(1, entry) != 0 || rt_write_char(1, '\t') != 0) return -1;
    return rt_write_line(1, entry->name);
}

static int inspect_apk(const char *path, const ReadApkOptions *options) {
    int fd = platform_open_read(path);
    ArchiveZipInfo info;
    ArchiveZipSigningBlock signing_block;
    ReadApkStats stats;
    ReadApkEntryContext context;
    int result;

    if (fd < 0) {
        tool_write_error("readapk", "cannot open: ", path);
        return 1;
    }
    if (archive_zip_read_info(fd, &info) != 0) {
        platform_close(fd);
        tool_write_error("readapk", "not a readable ZIP/APK: ", path);
        return 1;
    }
    if (archive_zip_read_signing_block(fd, &info, &signing_block) != 0) {
        platform_close(fd);
        tool_write_error("readapk", "invalid APK Signing Block: ", path);
        return 1;
    }

    rt_memset(&stats, 0, sizeof(stats));
    context.options = options;
    context.stats = &stats;
    context.path = path;

    if (options->show_entries && !options->json) {
        if (rt_write_line(1, "offset\tmethod\tcompressed\tuncompressed\tcrc32\tflags\tname") != 0) {
            platform_close(fd);
            return 1;
        }
    }
    result = archive_zip_iterate_entries(fd, &info, print_entry, &context);
    platform_close(fd);
    if (result != 0) {
        tool_write_error("readapk", "cannot read central directory: ", path);
        return 1;
    }

    if (options->json) {
        if (options->show_summary && write_json_summary(path, &info, &signing_block, &stats) != 0) {
            return 1;
        }
    } else if (options->show_summary) {
        if (options->show_entries && rt_write_char(1, '\n') != 0) return 1;
        if (print_summary(path, &info, &signing_block, &stats) != 0) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    ReadApkOptions options;
    ToolOptState opt;
    int opt_result;
    int exit_status = 0;
    int index;

    rt_memset(&options, 0, sizeof(options));
    tool_opt_init(&opt, argc, argv, "readapk", "[-a] [-s] [-l] [--json] FILE ...");
    while ((opt_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "-a") == 0 || rt_strcmp(opt.flag, "--all") == 0) {
            options.show_summary = 1;
            options.show_entries = 1;
        } else if (rt_strcmp(opt.flag, "-s") == 0 || rt_strcmp(opt.flag, "--summary") == 0) {
            options.show_summary = 1;
        } else if (rt_strcmp(opt.flag, "-l") == 0 || rt_strcmp(opt.flag, "--list") == 0) {
            options.show_entries = 1;
        } else if (rt_strcmp(opt.flag, "--json") == 0) {
            options.json = 1;
        } else {
            tool_write_error("readapk", "unknown option: ", opt.flag);
            print_usage();
            return 1;
        }
    }
    if (opt_result == TOOL_OPT_HELP) {
        print_usage();
        return 0;
    }
    if (opt_result == TOOL_OPT_ERROR) {
        return 1;
    }
    options.json = tool_json_is_enabled();
    if (!options.show_summary && !options.show_entries) {
        options.show_summary = 1;
    }
    if (opt.argi >= argc) {
        print_usage();
        return 1;
    }

    for (index = opt.argi; index < argc; ++index) {
        if (index > opt.argi && !options.json && rt_write_char(1, '\n') != 0) {
            return 1;
        }
        if (inspect_apk(argv[index], &options) != 0) {
            exit_status = 1;
        }
    }
    return exit_status;
}