#include "archive_util.h"
#include "compression/lzss.h"
#include "crypto/sha256.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define EXPACK_TOOL_NAME "expack"
#define EXPACK_ELF_BASE_VADDR 0x400000ULL
#define EXPACK_ELF_HEADER_SIZE 64U
#define EXPACK_ELF_PHDR_SIZE 56U
#define EXPACK_ELF_CODE_OFFSET (EXPACK_ELF_HEADER_SIZE + EXPACK_ELF_PHDR_SIZE)
#define EXPACK_ZERO_RUN_MIN 8U
#define EXPACK_BYTE_RUN_MIN 8U
#define EXPACK_LITERAL_MAX 65536U
#define EXPACK_CODEC_LZSS 0U
#define EXPACK_CODEC_ZERO_RUN 1U
#define EXPACK_CODEC_BYTE_RUN 2U
#define EXPACK_CODEC_LZREP 3U
#define EXPACK_CODEC_LZSS_BCJ 4U
#define EXPACK_CODEC_RAW 5U
#define EXPACK_FORMAT_ELF64_X86_64 1U
#define EXPACK_FORMAT_MACHO 2U
#define EXPACK_FORMAT_PE_COFF 3U
#define EXPACK_OUTPUT_KIND_ELF_PACKED 1U
#define EXPACK_OUTPUT_KIND_MACHO_CONTAINER 2U
#define EXPACK_OUTPUT_KIND_PE_CONTAINER 3U
#define EXPACK_MACHO_HEADER64_SIZE 32U
#define EXPACK_MACHO_CPU_X86_64 0x01000007U
#define EXPACK_MACHO_CPU_ARM64 0x0100000cU
#define EXPACK_MACHO_TYPE_EXECUTE 2U
#define EXPACK_MACHO_LC_SEGMENT_64 0x19U
#define EXPACK_MACHO_LC_LOAD_DYLINKER 0xeU
#define EXPACK_MACHO_LC_BUILD_VERSION 0x32U
#define EXPACK_MACHO_LC_MAIN 0x80000028U
#define EXPACK_MACHO_LC_CODE_SIGNATURE 0x1dU
#define EXPACK_MACHO_FLAG_NOUNDEFS 0x1U
#define EXPACK_MACHO_FLAG_DYLDLINK 0x4U
#define EXPACK_MACHO_FLAG_TWOLEVEL 0x80U
#define EXPACK_MACHO_FLAG_PIE 0x200000U
#define EXPACK_MACHO_VM_PROT_READ 1U
#define EXPACK_MACHO_VM_PROT_EXECUTE 4U
#define EXPACK_MACHO_CONTAINER_BASE 0x100000000ULL
#define EXPACK_MACHO_CONTAINER_METADATA_SIZE 48U
#define EXPACK_MACHO_CONTAINER_VERSION 1U
#define EXPACK_PE_CONTAINER_METADATA_SIZE 48U
#define EXPACK_PE_CONTAINER_VERSION 1U
#define EXPACK_PE_CONTAINER_IMAGE_BASE 0x140000000ULL
#define EXPACK_PE_CONTAINER_SECTION_ALIGNMENT 0x1000U
#define EXPACK_PE_CONTAINER_FILE_ALIGNMENT 0x200U
#define EXPACK_PE_CONTAINER_HEADERS_SIZE 0x400U
#define EXPACK_PE_CONTAINER_TEXT_RVA 0x1000U
#define EXPACK_PE_CONTAINER_EXPACK_RVA 0x2000U
#define EXPACK_PE_CONTAINER_TEXT_RAW_OFFSET EXPACK_PE_CONTAINER_HEADERS_SIZE
#define EXPACK_PE_CONTAINER_TEXT_RAW_SIZE 0x200U
#define EXPACK_MACHO_ARM64_RUNNER_PAYLOAD_ADDRESS_OFFSET 232U
#define EXPACK_MACHO_ARM64_RUNNER_PAYLOAD_SIZE_OFFSET 240U
#define EXPACK_MACHO_ARM64_LZREP_RUNNER_PAYLOAD_ADDRESS_OFFSET 520U
#define EXPACK_MACHO_ARM64_LZREP_RUNNER_PAYLOAD_SIZE_OFFSET 528U
#define EXPACK_MACHO_ARM64_LZREP_RUNNER_ORIGINAL_SIZE_OFFSET 536U
#define EXPACK_MACHO_CODE_SIGNATURE_PAGE_SIZE 0x4000U
#define EXPACK_MACHO_CODE_DIRECTORY_IDENT "expack"
#define EXPACK_MACHO_CODE_DIRECTORY_IDENT_SIZE 7U
#define EXPACK_LZSS_STUB_LENGTH_SHIFT_OFFSET 177U
#define EXPACK_LZSS_STUB_LENGTH_MASK_OFFSET 187U
#define EXPACK_LZSS_ORIGINAL_SIZE_OFFSET 368U
#define EXPACK_LZSS_PAYLOAD_SIZE_OFFSET 376U
#define EXPACK_ZERO_ORIGINAL_SIZE_OFFSET 274U
#define EXPACK_ZERO_PAYLOAD_SIZE_OFFSET 282U
#define EXPACK_BYTE_RUN_ORIGINAL_SIZE_OFFSET 286U
#define EXPACK_BYTE_RUN_PAYLOAD_SIZE_OFFSET 294U
#define EXPACK_LZREP_ORIGINAL_SIZE_OFFSET 379U
#define EXPACK_LZREP_PAYLOAD_SIZE_OFFSET 387U
#define EXPACK_LZSS_BCJ_ORIGINAL_SIZE_OFFSET 448U
#define EXPACK_LZSS_BCJ_PAYLOAD_SIZE_OFFSET 456U
#define EXPACK_LZREP_WINDOW_SIZE 2048U
#define EXPACK_LZREP_MAX_EXPLICIT_MATCH 18U
#define EXPACK_LZREP_MAX_REPEAT_MATCH 130U

typedef struct {
    unsigned int profile_id;
    unsigned char length_shift;
    unsigned char length_mask;
} ExpackLzssProfile;

typedef struct {
    unsigned int codec;
    const ExpackLzssProfile *lzss_profile;
    const unsigned char *stub;
    size_t stub_size;
    unsigned char *payload;
    size_t payload_size;
    unsigned long long packed_size;
} ExpackCandidate;

typedef struct {
    unsigned char *data;
    size_t size;
    int changed;
} ExpackImage;

typedef struct {
    unsigned long long old_offset;
    unsigned long long new_offset;
    unsigned long long file_size;
} ExpackLoadRange;

typedef struct {
    unsigned int cputype;
    unsigned int code_signature_offset;
    unsigned int code_signature_size;
} ExpackMachoInfo;

typedef struct {
    unsigned int machine;
    unsigned int section_count;
    unsigned int entry_rva;
    unsigned int section_alignment;
    unsigned int file_alignment;
    unsigned int size_of_image;
    unsigned int size_of_headers;
    unsigned int subsystem;
    unsigned long long image_base;
} ExpackPeInfo;

typedef union {
    ExpackMachoInfo macho;
    ExpackPeInfo pe;
} ExpackFormatInfo;

typedef struct {
    unsigned int kind;
    const char *name;
    const char *preprocess_error;
    int allow_x86_bcj;
    ExpackFormatInfo info;
} ExpackInputFormat;

typedef struct ExpackOutputBackend ExpackOutputBackend;

struct ExpackOutputBackend {
    unsigned int format_kind;
    unsigned int default_output_kind;
    const char *packed_unsupported_message;
    int (*can_write_packed)(const ExpackInputFormat *format);
    int (*can_write_container)(const ExpackInputFormat *format, unsigned int output_kind);
    int (*prepare_container_candidate)(const ExpackInputFormat *format, const ExpackImage *image, ExpackCandidate *candidate);
    int (*write_packed)(const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size);
    int (*write_container)(const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size);
    void (*write_container_success)(const char *output_path, const ExpackCandidate *candidate);
};

static int expack_compress_lzss_profile(const ExpackLzssProfile *profile, const unsigned char *input_data, size_t input_size, unsigned char **payload_out, size_t *payload_size_out);
static void expack_write_candidate_name(const ExpackCandidate *candidate);

static unsigned short expack_read_u16_le(const unsigned char *bytes) {
    return (unsigned short)bytes[0] | (unsigned short)((unsigned short)bytes[1] << 8);
}

static void expack_store_u16_le(unsigned char *bytes, unsigned short value) {
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8) & 0xffU);
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

    tool_opt_init(&options, argc, argv, EXPACK_TOOL_NAME, "[-q] INPUT [OUTPUT]\n       expack --analyze INPUT\n       expack --macho-container INPUT [OUTPUT]");
    while ((opt_result = tool_opt_next(&options)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(options.flag, "-q") == 0 || rt_strcmp(options.flag, "--quiet") == 0) {
            quiet = 1;
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
    if (expack_select_best_payload(image.data, image.size, &selected, analyze || !quiet, input_format.allow_x86_bcj) != 0) {
        tool_write_error(EXPACK_TOOL_NAME, "compression failed", 0);
        expack_release_exec_image(&image, input_data);
        rt_free(input_data);
        return 1;
    }
    if (analyze) {
        rt_write_cstr(1, "selected: ");
        expack_write_candidate_name(&selected);
        rt_write_cstr(1, " (packed ");
        rt_write_uint(1, selected.packed_size);
        rt_write_cstr(1, " bytes)\n");
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
