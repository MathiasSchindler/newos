#ifndef NEWOS_TOOLS_EXPACK_INTERNAL_H
#define NEWOS_TOOLS_EXPACK_INTERNAL_H

#include <stddef.h>

#include "../../shared/compression/lzss.h"
#include "../../shared/crypto/sha256.h"

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
#define EXPACK_CODEC_LZSS_BCJ_RIP 6U
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
#define EXPACK_PE_CONTAINER_METADATA_SIZE 24U
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
#define EXPACK_LZSS_BCJ_RIP_ORIGINAL_SIZE_OFFSET 566U
#define EXPACK_LZSS_BCJ_RIP_PAYLOAD_SIZE_OFFSET 574U
#define EXPACK_LZREP_WINDOW_SIZE 2048U
#define EXPACK_LZREP_MAX_EXPLICIT_MATCH 18U
#define EXPACK_LZREP_MAX_REPEAT_MATCH 130U
#define EXPACK_CANDIDATE_UNSUPPORTED_SIZE (~0ULL)

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
    unsigned long long vaddr;
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
    unsigned long long (*score_candidate)(const ExpackInputFormat *format, const ExpackCandidate *candidate);
    int (*can_write_packed)(const ExpackInputFormat *format);
    int (*can_write_container)(const ExpackInputFormat *format, unsigned int output_kind);
    int (*prepare_container_candidate)(const ExpackInputFormat *format, const ExpackImage *image, ExpackCandidate *candidate);
    int (*write_packed)(const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size);
    int (*write_container)(const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size);
    void (*write_container_success)(const char *output_path, const ExpackCandidate *candidate);
};

static int expack_compress_lzss_profile(const ExpackLzssProfile *profile, const unsigned char *input_data, size_t input_size, unsigned char **payload_out, size_t *payload_size_out);
static void expack_write_candidate_name(const ExpackCandidate *candidate);

#endif