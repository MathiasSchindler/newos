#include "image/image.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define IMGCHECK_INITIAL_CAPACITY (64U * 1024U)
#define IMGCHECK_ENTRY_CAPACITY 512U
#define IMGCHECK_PATH_CAPACITY 2048U

typedef struct {
    int quiet;
    int verbose;
    int plain;
    int json;
    int recursive;
    int strict;
    int c2pa_trust_validation;
} ImgcheckOptions;

static void print_usage(void) {
    tool_write_usage("imgcheck", "[-q|--quiet] [-v|--verbose] [-p|--plain] [--json] [--strict] [--c2pa-trust] [-R|--recursive] [file ...]");
}

static int read_all_input(const char *path, unsigned char **data_out, size_t *size_out) {
    int fd;
    int should_close;
    unsigned char *buffer;
    size_t capacity = IMGCHECK_INITIAL_CAPACITY;
    size_t used = 0U;

    *data_out = 0;
    *size_out = 0U;
    if (tool_open_input(path, &fd, &should_close) != 0) {
        return -1;
    }
    buffer = (unsigned char *)rt_malloc(capacity);
    if (buffer == 0) {
        tool_close_input(fd, should_close);
        tool_write_error("imgcheck", "out of memory: ", path ? path : "stdin");
        return -1;
    }
    while (1) {
        long bytes_read;

        if (used == capacity) {
            unsigned char *resized;
            size_t next_capacity = capacity * 2U;

            if (next_capacity <= capacity) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                tool_write_error("imgcheck", "input too large: ", path ? path : "stdin");
                return -1;
            }
            resized = (unsigned char *)rt_realloc(buffer, next_capacity);
            if (resized == 0) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                tool_write_error("imgcheck", "out of memory: ", path ? path : "stdin");
                return -1;
            }
            buffer = resized;
            capacity = next_capacity;
        }
        bytes_read = platform_read(fd, buffer + used, capacity - used);
        if (bytes_read < 0) {
            rt_free(buffer);
            tool_close_input(fd, should_close);
            tool_write_error("imgcheck", "read failed: ", path ? path : "stdin");
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }
        used += (size_t)bytes_read;
    }
    tool_close_input(fd, should_close);
    *data_out = buffer;
    *size_out = used;
    return 0;
}

static void write_plain_result(const char *label, const ImageValidation *validation, const ImageC2paInfo *c2pa) {
    rt_write_cstr(1, label);
    rt_write_char(1, '\t');
    rt_write_cstr(1, image_format_extension(validation->format));
    rt_write_char(1, '\t');
    rt_write_cstr(1, validation->valid ? "ok" : "fail");
    rt_write_char(1, '\t');
    if (validation->has_failure_offset) {
        rt_write_uint(1, (unsigned long long)validation->failure_offset);
    } else {
        rt_write_char(1, '-');
    }
    rt_write_char(1, '\t');
    rt_write_cstr(1, validation->message);
    if (c2pa != 0 && c2pa->present) {
        rt_write_char(1, '\t');
        rt_write_cstr(1, c2pa->status);
    }
    rt_write_char(1, '\n');
}

static void write_json_result(const char *label, const ImageValidation *validation, const ImageC2paInfo *c2pa) {
    tool_json_begin_event(1, "imgcheck", "stdout", "image_check");
    rt_write_cstr(1, ",\"data\":{\"path\":");
    tool_json_write_string(1, label);
    rt_write_cstr(1, ",\"format\":");
    tool_json_write_string(1, image_format_extension(validation->format));
    rt_write_cstr(1, ",\"valid\":");
    rt_write_cstr(1, validation->valid ? "true" : "false");
    rt_write_cstr(1, ",\"status\":");
    tool_json_write_string(1, validation->valid ? "ok" : "fail");
    rt_write_cstr(1, ",\"message\":");
    tool_json_write_string(1, validation->message);
    rt_write_cstr(1, ",\"failure_offset\":");
    if (validation->has_failure_offset) {
        rt_write_uint(1, (unsigned long long)validation->failure_offset);
    } else {
        rt_write_cstr(1, "null");
    }
    rt_write_cstr(1, ",\"c2pa\":");
    if (c2pa != 0 && c2pa->present) {
        rt_write_cstr(1, "{\"present\":true,\"status\":");
        tool_json_write_string(1, c2pa->status);
        rt_write_cstr(1, ",\"carrier\":");
        tool_json_write_string(1, c2pa->carrier);
        rt_write_cstr(1, ",\"signature_algorithm\":");
        if (c2pa->signature_algorithm != 0) tool_json_write_string(1, c2pa->signature_algorithm); else rt_write_cstr(1, "null");
        rt_write_cstr(1, ",\"jumbf_valid\":");
        rt_write_cstr(1, c2pa->jumbf_valid ? "true" : "false");
        rt_write_cstr(1, ",\"cbor_valid\":");
        rt_write_cstr(1, c2pa->cbor_valid ? "true" : "false");
        rt_write_cstr(1, ",\"cose_valid\":");
        rt_write_cstr(1, c2pa->cose_valid ? "true" : "false");
        rt_write_cstr(1, ",\"manifest_count\":");
        rt_write_uint(1, (unsigned long long)c2pa->manifest_count);
        rt_write_cstr(1, ",\"claim_count\":");
        rt_write_uint(1, (unsigned long long)c2pa->claim_count);
        rt_write_cstr(1, ",\"signature_count\":");
        rt_write_uint(1, (unsigned long long)c2pa->signature_count);
        rt_write_cstr(1, ",\"cose_signature_count\":");
        rt_write_uint(1, (unsigned long long)c2pa->cose_signature_count);
        rt_write_cstr(1, ",\"signature_verified_count\":");
        rt_write_uint(1, (unsigned long long)c2pa->signature_verified_count);
        rt_write_cstr(1, ",\"signature_invalid_count\":");
        rt_write_uint(1, (unsigned long long)c2pa->signature_invalid_count);
        rt_write_cstr(1, ",\"x509_certificate_count\":");
        rt_write_uint(1, (unsigned long long)c2pa->x509_cert_count);
        rt_write_cstr(1, ",\"content_hash_checked\":");
        rt_write_cstr(1, c2pa->content_hash_checked ? "true" : "false");
        rt_write_cstr(1, ",\"content_hash_matched\":");
        rt_write_cstr(1, c2pa->content_hash_matched ? "true" : "false");
        rt_write_cstr(1, ",\"content_hash_mismatched\":");
        rt_write_cstr(1, c2pa->content_hash_mismatched ? "true" : "false");
        rt_write_cstr(1, ",\"validation_failure_count\":");
        rt_write_uint(1, (unsigned long long)c2pa->validation_failure_count);
        rt_write_cstr(1, ",\"signature_verification_supported\":");
        rt_write_cstr(1, c2pa->signature_supported ? "true" : "false");
        rt_write_cstr(1, ",\"trust_validation_supported\":");
        rt_write_cstr(1, c2pa->trust_supported ? "true" : "false");
        rt_write_cstr(1, ",\"trust_validation_valid\":");
        rt_write_cstr(1, c2pa->trust_valid ? "true" : "false");
        rt_write_char(1, '}');
    } else {
        rt_write_cstr(1, "{\"present\":false}");
    }
    rt_write_char(1, '}');
    tool_json_end_event(1);
}

static void write_human_result(const char *label, const ImageValidation *validation, const ImgcheckOptions *options, const ImageC2paInfo *c2pa) {
    rt_write_cstr(1, label);
    rt_write_cstr(1, validation->valid ? ": OK" : ": FAIL");
    rt_write_cstr(1, " (");
    rt_write_cstr(1, image_format_extension(validation->format));
    rt_write_char(1, ')');
    if (options->verbose || !validation->valid) {
        rt_write_cstr(1, ": ");
        rt_write_cstr(1, validation->message);
        if (validation->has_failure_offset) {
            rt_write_cstr(1, " at offset ");
            rt_write_uint(1, (unsigned long long)validation->failure_offset);
        }
    }
    if (c2pa != 0 && c2pa->present && (options->verbose || !validation->valid)) {
        rt_write_cstr(1, "; C2PA: ");
        rt_write_cstr(1, c2pa->status);
    }
    rt_write_char(1, '\n');
}

static int check_path(const char *path, const ImgcheckOptions *options) {
    unsigned char *data;
    size_t size;
    ImageValidation validation;
    ImageValidationOptions validation_options;
    ImageC2paOptions c2pa_options;
    ImageC2paInfo c2pa;
    const char *label = path ? path : "stdin";
    int result;

    if (read_all_input(path, &data, &size) != 0) {
        return -1;
    }
    validation_options.strict = options->strict;
    c2pa_options.trust_validation = options->c2pa_trust_validation;
    result = image_validate_ex(data, size, &validation_options, &validation);
    (void)image_c2pa_analyze_ex(data, size, &c2pa_options, &c2pa);
    rt_free(data);
    if (!options->quiet) {
        if (options->json) {
            write_json_result(label, &validation, &c2pa);
        } else if (options->plain) {
            write_plain_result(label, &validation, &c2pa);
        } else {
            write_human_result(label, &validation, options, &c2pa);
        }
    }
    if (c2pa.present && (c2pa.content_hash_mismatched || c2pa.signature_invalid_count > 0U || c2pa.validation_failure_count > 0U)) {
        return -1;
    }
    return result == 0 && validation.valid ? 0 : -1;
}

static int check_path_recursive(const char *path, const ImgcheckOptions *options) {
    PlatformDirEntry entries[IMGCHECK_ENTRY_CAPACITY];
    size_t count = 0U;
    int is_directory = 0;
    int status = 0;
    size_t index;

    if (!options->recursive || platform_collect_entries(path, 1, entries, IMGCHECK_ENTRY_CAPACITY, &count, &is_directory) != 0) {
        return check_path(path, options);
    }
    if (!is_directory) {
        platform_free_entries(entries, count);
        return check_path(path, options);
    }
    for (index = 0U; index < count; ++index) {
        char child_path[IMGCHECK_PATH_CAPACITY];

        if (rt_strcmp(entries[index].name, ".") == 0 || rt_strcmp(entries[index].name, "..") == 0) {
            continue;
        }
        if (tool_join_path(path, entries[index].name, child_path, sizeof(child_path)) != 0) {
            status = 1;
            continue;
        }
        if (entries[index].is_dir) {
            if (check_path_recursive(child_path, options) != 0) {
                status = 1;
            }
        } else if (check_path(child_path, options) != 0) {
            status = 1;
        }
    }
    platform_free_entries(entries, count);
    return status == 0 ? 0 : -1;
}

static int parse_options(int argc, char **argv, ImgcheckOptions *options, int *arg_index_out) {
    int arg_index = 1;

    options->quiet = 0;
    options->verbose = 0;
    options->plain = 0;
    options->json = 0;
    options->recursive = 0;
    options->strict = 0;
    options->c2pa_trust_validation = 0;
    while (arg_index < argc && argv[arg_index][0] == '-' && argv[arg_index][1] != '\0') {
        const char *arg = argv[arg_index];

        if (rt_strcmp(arg, "--") == 0) {
            arg_index += 1;
            break;
        }
        if (rt_strcmp(arg, "-h") == 0 || rt_strcmp(arg, "--help") == 0) {
            print_usage();
            return 1;
        }
        if (rt_strcmp(arg, "-q") == 0 || rt_strcmp(arg, "--quiet") == 0) {
            options->quiet = 1;
            arg_index += 1;
            continue;
        }
        if (rt_strcmp(arg, "-v") == 0 || rt_strcmp(arg, "--verbose") == 0) {
            options->verbose = 1;
            arg_index += 1;
            continue;
        }
        if (rt_strcmp(arg, "-p") == 0 || rt_strcmp(arg, "--plain") == 0) {
            options->plain = 1;
            arg_index += 1;
            continue;
        }
        if (rt_strcmp(arg, "--json") == 0) {
            options->json = 1;
            tool_json_set_enabled(1);
            arg_index += 1;
            continue;
        }
        if (rt_strcmp(arg, "--c2pa-trust") == 0 || rt_strcmp(arg, "--trust") == 0) {
            options->c2pa_trust_validation = 1;
            arg_index += 1;
            continue;
        }
        if (rt_strcmp(arg, "--strict") == 0) {
            options->strict = 1;
            arg_index += 1;
            continue;
        }
        if (rt_strcmp(arg, "-R") == 0 || rt_strcmp(arg, "--recursive") == 0) {
            options->recursive = 1;
            arg_index += 1;
            continue;
        }
        tool_write_error("imgcheck", "unknown option: ", arg);
        print_usage();
        return -1;
    }
    *arg_index_out = arg_index;
    return 0;
}

int main(int argc, char **argv) {
    ImgcheckOptions options;
    int arg_index;
    int parse_result;
    int status = 0;

    parse_result = parse_options(argc, argv, &options, &arg_index);
    if (parse_result > 0) {
        return 0;
    }
    if (parse_result < 0) {
        return 1;
    }
    if (arg_index >= argc) {
        return check_path(0, &options) == 0 ? 0 : 1;
    }
    while (arg_index < argc) {
        if (check_path_recursive(argv[arg_index], &options) != 0) {
            status = 1;
        }
        arg_index += 1;
    }
    return status;
}
