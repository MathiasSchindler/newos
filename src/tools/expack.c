#include "archive_util.h"
#include "concurrency.h"
#include "compression/lzss.h"
#include "compression/zlib.h"
#include "crypto/sha256.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#include "expack/internal.h"

static unsigned short expack_read_u16_le(const unsigned char *bytes) {
    return (unsigned short)bytes[0] | (unsigned short)((unsigned short)bytes[1] << 8);
}

static unsigned long long expack_align_up_u64(unsigned long long value, unsigned long long alignment) {
    unsigned long long remainder;

    if (alignment <= 1ULL) {
        return value;
    }
    remainder = value % alignment;
    return remainder == 0ULL ? value : value + (alignment - remainder);
}

static void expack_store_fixed_name(unsigned char *field, const char *name) {
    size_t index = 0U;

    memset(field, 0, 16U);
    while (index < 16U && name[index] != '\0') {
        field[index] = (unsigned char)name[index];
        index += 1U;
    }
}

#include "expack/codecs.c"
#include "expack/formats.c"
#include "expack/outputs.c"

static int expack_load_file(const char *path, unsigned char **data_out, size_t *size_out) {
    int fd;
    long long file_size;
    unsigned char *data;

    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }
    file_size = platform_seek(fd, 0, PLATFORM_SEEK_END);
    if (file_size < 0 || platform_seek(fd, 0, PLATFORM_SEEK_SET) < 0) {
        platform_close(fd);
        return -1;
    }
    data = (unsigned char *)rt_malloc((size_t)file_size == 0U ? 1U : (size_t)file_size);
    if (data == 0) {
        platform_close(fd);
        return -1;
    }
    if ((size_t)file_size != 0U && archive_read_exact(fd, data, (size_t)file_size) != 0) {
        rt_free(data);
        platform_close(fd);
        return -1;
    }
    platform_close(fd);
    *data_out = data;
    *size_out = (size_t)file_size;
    return 0;
}

static int expack_write_exact_file(const char *path, const unsigned char *data, size_t size) {
    int fd = platform_open_write(path, 0755U);
    if (fd < 0) {
        return -1;
    }
    if (size != 0U && rt_write_all(fd, data, size) != 0) {
        platform_close(fd);
        return -1;
    }
    if (platform_close(fd) != 0) {
        return -1;
    }
    return 0;
}

static char *expack_make_default_output_path(const char *input_path) {
    const char suffix[] = "-pack";
    size_t input_length = rt_strlen(input_path);
    size_t suffix_length = sizeof(suffix) - 1U;
    char *output_path;

    if (input_length > ((size_t)-1) - suffix_length - 1U) {
        return 0;
    }
    output_path = (char *)rt_malloc(input_length + suffix_length + 1U);
    if (output_path == 0) {
        return 0;
    }
    memcpy(output_path, input_path, input_length);
    memcpy(output_path + input_length, suffix, suffix_length + 1U);
    return output_path;
}

static unsigned long long expack_output_file_size(const char *output_path) {
    PlatformDirEntry entry;

    if (platform_get_path_info(output_path, &entry) == 0) {
        return entry.size;
    }
    return 0ULL;
}

static void expack_write_summary(size_t input_size, size_t image_size, int image_changed, const ExpackCandidate *candidate, unsigned long long output_size) {
    rt_write_cstr(1, "expack: input ");
    rt_write_uint(1, (unsigned long long)input_size);
    if (image_changed) {
        rt_write_cstr(1, " bytes, exec image ");
        rt_write_uint(1, (unsigned long long)image_size);
    }
    rt_write_cstr(1, " bytes");
    if (output_size != 0ULL) {
        rt_write_cstr(1, ", output ");
        rt_write_uint(1, output_size);
        rt_write_cstr(1, " bytes");
    }
    rt_write_cstr(1, ", payload ");
    rt_write_uint(1, (unsigned long long)candidate->payload_size);
    rt_write_cstr(1, " bytes, codec ");
    expack_write_candidate_name(candidate);
    rt_write_cstr(1, ", packed estimate ");
    rt_write_uint(1, candidate->packed_size);
    rt_write_cstr(1, " bytes\n");
}

static void expack_write_report_header(const char *label, const ExpackInputFormat *format, size_t input_size, size_t image_size, int image_changed) {
    rt_write_cstr(1, label);
    rt_write_cstr(1, ": input ");
    rt_write_uint(1, (unsigned long long)input_size);
    rt_write_cstr(1, " bytes, format ");
    rt_write_cstr(1, format->name);
    rt_write_cstr(1, ", exec image ");
    rt_write_uint(1, (unsigned long long)image_size);
    if (image_changed) {
        rt_write_cstr(1, " bytes (reconstructed)");
    } else {
        rt_write_cstr(1, " bytes");
    }
    if (format->kind == EXPACK_FORMAT_MACHO && format->info.macho.code_signature_size != 0U) {
        rt_write_cstr(1, ", code signature ");
        rt_write_uint(1, (unsigned long long)format->info.macho.code_signature_size);
        rt_write_cstr(1, " bytes at ");
        rt_write_uint(1, (unsigned long long)format->info.macho.code_signature_offset);
    }
    if (format->kind == EXPACK_FORMAT_PE_COFF) {
        rt_write_cstr(1, ", entry RVA ");
        rt_write_uint(1, (unsigned long long)format->info.pe.entry_rva);
        rt_write_cstr(1, ", sections ");
        rt_write_uint(1, (unsigned long long)format->info.pe.section_count);
    }
    rt_write_cstr(1, "\n");
}

static void expack_write_unsupported_output_error(const ExpackOutputBackend *backend, const ExpackInputFormat *format) {
    if (backend != 0 && backend->packed_unsupported_message != 0) {
        tool_write_error(EXPACK_TOOL_NAME, backend->packed_unsupported_message, 0);
    } else {
        (void)format;
        tool_write_error(EXPACK_TOOL_NAME, "cannot write packed output for this executable format", 0);
    }
}

int main(int argc, char **argv) {
    const char *input_path = 0;
    const char *output_path = 0;
    char *default_output_path = 0;
    const char *error_message = 0;
    unsigned char *input_data = 0;
    ExpackImage image;
    ExpackCandidate selected;
    ExpackInputFormat input_format;
    const ExpackOutputBackend *output_backend = 0;
    size_t input_size = 0U;
    int analyze = 0;
    int macho_container = 0;
    int positional_count;
    unsigned int output_kind = 0U;
    int try_all_candidates = 0;
    int quiet = 0;
    ToolOptState options;
    int opt_result;

    selected.codec = 0U;
    selected.lzss_profile = 0;
    selected.stub = 0;
    selected.stub_size = 0U;
    selected.payload = 0;
    selected.payload_size = 0U;
    selected.packed_size = 0ULL;
    input_format.kind = 0U;
    input_format.name = "unknown";
    input_format.preprocess_error = "executable preprocessing failed";
    input_format.allow_x86_bcj = 0;
    memset(&input_format.info, 0, sizeof(input_format.info));
    image.data = 0;
    image.size = 0U;
    image.changed = 0;

    tool_opt_init(&options, argc, argv, EXPACK_TOOL_NAME, "[-q] [--all] INPUT [OUTPUT]\n       expack --analyze [--all] INPUT\n       expack --macho-container [--all] INPUT [OUTPUT]");
    while ((opt_result = tool_opt_next(&options)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(options.flag, "-q") == 0 || rt_strcmp(options.flag, "--quiet") == 0) {
            quiet = 1;
        } else if (rt_strcmp(options.flag, "--all") == 0) {
            try_all_candidates = 1;
        } else if (rt_strcmp(options.flag, "--analyze") == 0) {
            analyze = 1;
        } else if (rt_strcmp(options.flag, "--macho-container") == 0) {
            macho_container = 1;
        } else {
            tool_write_error(EXPACK_TOOL_NAME, "unknown option: ", options.flag);
            tool_write_usage(EXPACK_TOOL_NAME, options.usage_suffix);
            return 1;
        }
    }
    if (opt_result == TOOL_OPT_HELP) {
        tool_write_usage(EXPACK_TOOL_NAME, options.usage_suffix);
        return 0;
    }
    if (opt_result == TOOL_OPT_ERROR) {
        return 1;
    }
    positional_count = argc - options.argi;
    if ((analyze && macho_container) || (analyze && positional_count != 1) || (!analyze && !(positional_count == 1 || positional_count == 2))) {
        tool_write_usage(EXPACK_TOOL_NAME, options.usage_suffix);
        return 1;
    }

    input_path = argv[options.argi];
    if (!analyze && positional_count == 2) {
        output_path = argv[options.argi + 1];
    }
    if (expack_load_file(input_path, &input_data, &input_size) != 0) {
        tool_write_error(EXPACK_TOOL_NAME, "cannot read input: ", input_path);
        return 1;
    }
    if (expack_detect_input_format(input_data, input_size, &input_format, &error_message) != 0) {
        tool_write_error(EXPACK_TOOL_NAME, error_message, 0);
        rt_free(input_data);
        return 1;
    }

    if (expack_make_exec_image(&input_format, input_data, input_size, &image) != 0) {
        tool_write_error(EXPACK_TOOL_NAME, input_format.preprocess_error, 0);
        rt_free(input_data);
        return 1;
    }
    output_backend = expack_find_output_backend(&input_format);
    if (analyze || !quiet) {
        expack_write_report_header(analyze ? "expack analyze" : "expack", &input_format, input_size, image.size, image.changed);
    }
    if (expack_select_best_payload(&input_format, output_backend, image.data, image.size, &selected, analyze || !quiet, input_format.allow_x86_bcj, try_all_candidates) != 0) {
        tool_write_error(EXPACK_TOOL_NAME, "compression failed", 0);
        expack_release_exec_image(&image, input_data);
        rt_free(input_data);
        return 1;
    }
    if (analyze) {
        if (selected.packed_size >= (unsigned long long)input_size) {
            rt_write_cstr(1, "selected: raw (kept unpacked: input ");
            rt_write_uint(1, (unsigned long long)input_size);
            rt_write_cstr(1, " bytes, best packer ");
            expack_write_candidate_name(&selected);
            rt_write_cstr(1, " would be ");
            rt_write_uint(1, selected.packed_size);
            rt_write_cstr(1, " bytes)\n");
        } else {
            rt_write_cstr(1, "selected: ");
            expack_write_candidate_name(&selected);
            rt_write_cstr(1, " (packed ");
            rt_write_uint(1, selected.packed_size);
            rt_write_cstr(1, " bytes)\n");
        }
        expack_candidate_release(&selected);
        expack_release_exec_image(&image, input_data);
        rt_free(input_data);
        return 0;
    }
    if (output_path == 0) {
        default_output_path = expack_make_default_output_path(input_path);
        if (default_output_path == 0) {
            tool_write_error(EXPACK_TOOL_NAME, "cannot prepare default output path", 0);
            expack_candidate_release(&selected);
            expack_release_exec_image(&image, input_data);
            rt_free(input_data);
            return 1;
        }
        output_path = default_output_path;
    }
    if (macho_container) {
        output_kind = EXPACK_OUTPUT_KIND_MACHO_CONTAINER;
    } else if (output_backend != 0) {
        output_kind = output_backend->default_output_kind;
    }
    if (output_kind != EXPACK_OUTPUT_KIND_ELF_PACKED) {
        if (output_backend == 0 || output_backend->can_write_container == 0 || !output_backend->can_write_container(&input_format, output_kind)) {
            if (macho_container) {
                tool_write_error(EXPACK_TOOL_NAME, "--macho-container requires a Mach-O input", 0);
            } else {
                expack_write_unsupported_output_error(output_backend, &input_format);
            }
            if (default_output_path != 0) rt_free(default_output_path);
            expack_candidate_release(&selected);
            expack_release_exec_image(&image, input_data);
            rt_free(input_data);
            return 1;
        }
        if (output_backend->prepare_container_candidate != 0 && output_backend->prepare_container_candidate(&input_format, &image, &selected) != 0) {
            tool_write_error(EXPACK_TOOL_NAME, "cannot prepare container payload", 0);
            if (default_output_path != 0) rt_free(default_output_path);
            expack_candidate_release(&selected);
            expack_release_exec_image(&image, input_data);
            rt_free(input_data);
            return 1;
        }
        if (expack_write_container_output(output_backend, &input_format, output_path, &selected, image.size, output_kind) != 0) {
            tool_write_error(EXPACK_TOOL_NAME, "cannot write container output: ", output_path);
            if (default_output_path != 0) rt_free(default_output_path);
            expack_candidate_release(&selected);
            expack_release_exec_image(&image, input_data);
            rt_free(input_data);
            return 1;
        }
        if (!quiet && output_backend->write_container_success != 0) {
            output_backend->write_container_success(output_path, &selected);
            expack_write_summary(input_size, image.size, image.changed, &selected, expack_output_file_size(output_path));
        }
        if (default_output_path != 0) rt_free(default_output_path);
        expack_candidate_release(&selected);
        expack_release_exec_image(&image, input_data);
        rt_free(input_data);
        return 0;
    }
    if (output_kind != EXPACK_OUTPUT_KIND_ELF_PACKED || output_backend == 0 || output_backend->can_write_packed == 0 || !output_backend->can_write_packed(&input_format)) {
        expack_write_unsupported_output_error(output_backend, &input_format);
        if (default_output_path != 0) rt_free(default_output_path);
        expack_candidate_release(&selected);
        expack_release_exec_image(&image, input_data);
        rt_free(input_data);
        return 1;
    }
    if (selected.packed_size >= (unsigned long long)input_size) {
        ExpackCandidate raw;
        raw.payload = 0;
        if (expack_make_raw_candidate(input_data, input_size, &raw) != 0 || expack_write_exact_file(output_path, input_data, input_size) != 0) {
            tool_write_error(EXPACK_TOOL_NAME, "cannot write exact output: ", output_path);
            if (default_output_path != 0) rt_free(default_output_path);
            expack_candidate_release(&raw);
            expack_candidate_release(&selected);
            expack_release_exec_image(&image, input_data);
            rt_free(input_data);
            return 1;
        }
        if (!quiet) {
            expack_write_summary(input_size, image.size, image.changed, &raw, expack_output_file_size(output_path));
        }
        if (default_output_path != 0) rt_free(default_output_path);
        expack_candidate_release(&raw);
        expack_candidate_release(&selected);
        expack_release_exec_image(&image, input_data);
        rt_free(input_data);
        return 0;
    }
    if (expack_write_packed_output(output_backend, &input_format, output_path, &selected, image.size) != 0) {
        tool_write_error(EXPACK_TOOL_NAME, "cannot write packed output: ", output_path);
        if (default_output_path != 0) rt_free(default_output_path);
        expack_candidate_release(&selected);
        expack_release_exec_image(&image, input_data);
        rt_free(input_data);
        return 1;
    }
    if (!quiet) {
        expack_write_summary(input_size, image.size, image.changed, &selected, expack_output_file_size(output_path));
    }

    if (default_output_path != 0) rt_free(default_output_path);
    expack_candidate_release(&selected);
    expack_release_exec_image(&image, input_data);
    rt_free(input_data);
    return 0;
}
