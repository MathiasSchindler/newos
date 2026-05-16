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

static const ExpackLzssProfile expack_lzss_profiles[] = {
    {COMPRESSION_LZSS_PROFILE_WIDE_WINDOW, 3U, 0x07U},
    {COMPRESSION_LZSS_PROFILE_WIDE_MATCH, 4U, 0x0fU},
    {COMPRESSION_LZSS_PROFILE_MEDIUM_MATCH, 5U, 0x1fU},
    {COMPRESSION_LZSS_PROFILE_LONG_MATCH, 6U, 0x3fU}
};

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
    unsigned int kind;
    const char *name;
    const char *preprocess_error;
    int allow_x86_bcj;
    int can_write_packed_output;
    unsigned int macho_cputype;
    unsigned int macho_code_signature_offset;
    unsigned int macho_code_signature_size;
} ExpackInputFormat;

static int expack_compress_lzss_profile(const ExpackLzssProfile *profile, const unsigned char *input_data, size_t input_size, unsigned char **payload_out, size_t *payload_size_out);
static void expack_write_candidate_name(const ExpackCandidate *candidate);

static const unsigned char expack_stub_x86_64[] = {
    0x31, 0xed, 0x49, 0x89, 0xe4, 0x4c, 0x8b, 0x35, 0x64, 0x01, 0x00, 0x00,
    0x31, 0xff, 0x4c, 0x89, 0xf6, 0xba, 0x03, 0x00, 0x00, 0x00, 0x41, 0xba,
    0x22, 0x00, 0x00, 0x00, 0x41, 0xb8, 0xff, 0xff, 0xff, 0xff, 0x45, 0x31,
    0xc9, 0xb8, 0x09, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x0f,
    0x88, 0x2a, 0x01, 0x00, 0x00, 0x49, 0x89, 0xc5, 0x4f, 0x8d, 0x7c, 0x35,
    0x00, 0x48, 0x8d, 0x1d, 0x44, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x2d, 0x2d,
    0x01, 0x00, 0x00, 0x48, 0x01, 0xdd, 0x4c, 0x89, 0xef, 0x45, 0x31, 0xc0,
    0x45, 0x31, 0xd2, 0x4c, 0x39, 0xff, 0x0f, 0x83, 0x9d, 0x00, 0x00, 0x00,
    0x45, 0x85, 0xc0, 0x75, 0x16, 0x48, 0x39, 0xeb, 0x0f, 0x83, 0xf1, 0x00,
    0x00, 0x00, 0x44, 0x0f, 0xb6, 0x13, 0x48, 0xff, 0xc3, 0x41, 0xb8, 0x01,
    0x00, 0x00, 0x00, 0x45, 0x84, 0xc2, 0x75, 0x15, 0x48, 0x39, 0xeb, 0x0f,
    0x83, 0xd6, 0x00, 0x00, 0x00, 0x8a, 0x03, 0x48, 0xff, 0xc3, 0x88, 0x07,
    0x48, 0xff, 0xc7, 0xeb, 0x50, 0x48, 0x8d, 0x43, 0x02, 0x48, 0x39, 0xe8,
    0x0f, 0x87, 0xbd, 0x00, 0x00, 0x00, 0x0f, 0xb6, 0x03, 0x0f, 0xb6, 0x4b,
    0x01, 0x48, 0x83, 0xc3, 0x02, 0x89, 0xca, 0xc1, 0xea, 0x03, 0xc1, 0xe2,
    0x08, 0x09, 0xc2, 0xff, 0xc2, 0x83, 0xe1, 0x07, 0x83, 0xc1, 0x03, 0x48,
    0x89, 0xfe, 0x48, 0x29, 0xd6, 0x4c, 0x39, 0xee, 0x0f, 0x82, 0x91, 0x00,
    0x00, 0x00, 0x4c, 0x39, 0xff, 0x0f, 0x83, 0x88, 0x00, 0x00, 0x00, 0x8a,
    0x06, 0x48, 0xff, 0xc6, 0x88, 0x07, 0x48, 0xff, 0xc7, 0xff, 0xc9, 0x75,
    0xe9, 0x41, 0xd1, 0xe0, 0x41, 0x81, 0xf8, 0x00, 0x01, 0x00, 0x00, 0x0f,
    0x85, 0x62, 0xff, 0xff, 0xff, 0x45, 0x31, 0xc0, 0xe9, 0x5a, 0xff, 0xff,
    0xff, 0x48, 0x39, 0xeb, 0x75, 0x5d, 0xb8, 0x3f, 0x01, 0x00, 0x00, 0x48,
    0x8d, 0x3d, 0x72, 0x00, 0x00, 0x00, 0x31, 0xf6, 0x0f, 0x05, 0x48, 0x85,
    0xc0, 0x78, 0x48, 0x48, 0x89, 0xc3, 0x4c, 0x89, 0xee, 0x4c, 0x89, 0xf2,
    0x48, 0x85, 0xd2, 0x74, 0x16, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x89, 0xdf,
    0x0f, 0x05, 0x48, 0x85, 0xc0, 0x7e, 0x2c, 0x48, 0x01, 0xc6, 0x48, 0x29,
    0xc2, 0xeb, 0xe5, 0xb8, 0x42, 0x01, 0x00, 0x00, 0x89, 0xdf, 0x48, 0x8d,
    0x35, 0x3e, 0x00, 0x00, 0x00, 0x49, 0x8d, 0x54, 0x24, 0x08, 0x4d, 0x8b,
    0x14, 0x24, 0x4f, 0x8d, 0x54, 0xd4, 0x10, 0x41, 0xb8, 0x00, 0x10, 0x00,
    0x00, 0x0f, 0x05, 0xb8, 0x3c, 0x00, 0x00, 0x00, 0xbf, 0x7f, 0x00, 0x00,
    0x00, 0x0f, 0x05, 0x0f, 0x1f, 0x44, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44,
    0x55, 0x66, 0x77, 0x88, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
    0x65, 0x78, 0x70, 0x61, 0x63, 0x6b, 0x00, 0x00
};

static const unsigned char expack_zero_stub_x86_64[] = {
    0x31, 0xed, 0x49, 0x89, 0xe4, 0x4c, 0x8b, 0x35, 0x06, 0x01, 0x00, 0x00,
    0x31, 0xff, 0x4c, 0x89, 0xf6, 0xba, 0x03, 0x00, 0x00, 0x00, 0x41, 0xba,
    0x22, 0x00, 0x00, 0x00, 0x41, 0xb8, 0xff, 0xff, 0xff, 0xff, 0x45, 0x31,
    0xc9, 0xb8, 0x09, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x0f,
    0x88, 0xd0, 0x00, 0x00, 0x00, 0x49, 0x89, 0xc5, 0x4f, 0x8d, 0x7c, 0x35,
    0x00, 0x48, 0x8d, 0x1d, 0xe6, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x2d, 0xcf,
    0x00, 0x00, 0x00, 0x48, 0x01, 0xdd, 0x4c, 0x89, 0xef, 0x48, 0x39, 0xeb,
    0x74, 0x4e, 0x48, 0x8d, 0x43, 0x05, 0x48, 0x39, 0xe8, 0x0f, 0x87, 0xa2,
    0x00, 0x00, 0x00, 0x0f, 0xb6, 0x03, 0x8b, 0x53, 0x01, 0x48, 0x83, 0xc3,
    0x05, 0x4c, 0x89, 0xf9, 0x48, 0x29, 0xf9, 0x48, 0x39, 0xd1, 0x0f, 0x82,
    0x89, 0x00, 0x00, 0x00, 0x85, 0xc0, 0x75, 0x17, 0x48, 0x89, 0xe9, 0x48,
    0x29, 0xd9, 0x48, 0x39, 0xd1, 0x72, 0x7a, 0x48, 0x89, 0xde, 0x89, 0xd1,
    0xf3, 0xa4, 0x48, 0x89, 0xf3, 0xeb, 0xba, 0x83, 0xf8, 0x01, 0x75, 0x69,
    0x31, 0xc0, 0x89, 0xd1, 0xf3, 0xaa, 0xeb, 0xad, 0x4c, 0x39, 0xff, 0x75,
    0x5c, 0xb8, 0x3f, 0x01, 0x00, 0x00, 0x48, 0x8d, 0x3d, 0x6d, 0x00, 0x00,
    0x00, 0x31, 0xf6, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x78, 0x47, 0x89, 0xc3,
    0x4c, 0x89, 0xee, 0x4c, 0x89, 0xf2, 0x48, 0x85, 0xd2, 0x74, 0x16, 0xb8,
    0x01, 0x00, 0x00, 0x00, 0x89, 0xdf, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x7e,
    0x2c, 0x48, 0x01, 0xc6, 0x48, 0x29, 0xc2, 0xeb, 0xe5, 0xb8, 0x42, 0x01,
    0x00, 0x00, 0x89, 0xdf, 0x48, 0x8d, 0x35, 0x3a, 0x00, 0x00, 0x00, 0x49,
    0x8d, 0x54, 0x24, 0x08, 0x4d, 0x8b, 0x14, 0x24, 0x4f, 0x8d, 0x54, 0xd4,
    0x10, 0x41, 0xb8, 0x00, 0x10, 0x00, 0x00, 0x0f, 0x05, 0xb8, 0x3c, 0x00,
    0x00, 0x00, 0xbf, 0x7f, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x90, 0x11, 0x22,
    0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33,
    0x22, 0x11, 0x65, 0x78, 0x70, 0x61, 0x63, 0x6b, 0x00, 0x00
};

static const unsigned char expack_byte_run_stub_x86_64[] = {
    0x31, 0xed, 0x49, 0x89, 0xe4, 0x4c, 0x8b, 0x35, 0x12, 0x01, 0x00, 0x00,
    0x31, 0xff, 0x4c, 0x89, 0xf6, 0xba, 0x03, 0x00, 0x00, 0x00, 0x41, 0xba,
    0x22, 0x00, 0x00, 0x00, 0x41, 0xb8, 0xff, 0xff, 0xff, 0xff, 0x45, 0x31,
    0xc9, 0xb8, 0x09, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x0f,
    0x88, 0xdc, 0x00, 0x00, 0x00, 0x49, 0x89, 0xc5, 0x4f, 0x8d, 0x7c, 0x35,
    0x00, 0x48, 0x8d, 0x1d, 0xf2, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x2d, 0xdb,
    0x00, 0x00, 0x00, 0x48, 0x01, 0xdd, 0x4c, 0x89, 0xef, 0x48, 0x39, 0xeb,
    0x74, 0x5a, 0x48, 0x8d, 0x43, 0x05, 0x48, 0x39, 0xe8, 0x0f, 0x87, 0xae,
    0x00, 0x00, 0x00, 0x0f, 0xb6, 0x03, 0x8b, 0x53, 0x01, 0x48, 0x83, 0xc3,
    0x05, 0x4c, 0x89, 0xf9, 0x48, 0x29, 0xf9, 0x48, 0x39, 0xd1, 0x0f, 0x82,
    0x95, 0x00, 0x00, 0x00, 0x85, 0xc0, 0x75, 0x1b, 0x48, 0x89, 0xe9, 0x48,
    0x29, 0xd9, 0x48, 0x39, 0xd1, 0x0f, 0x82, 0x82, 0x00, 0x00, 0x00, 0x48,
    0x89, 0xde, 0x89, 0xd1, 0xf3, 0xa4, 0x48, 0x89, 0xf3, 0xeb, 0xb6, 0x83,
    0xf8, 0x01, 0x75, 0x71, 0x48, 0x39, 0xeb, 0x73, 0x6c, 0x8a, 0x03, 0x48,
    0xff, 0xc3, 0x89, 0xd1, 0xf3, 0xaa, 0xeb, 0xa1, 0x4c, 0x39, 0xff, 0x75,
    0x5c, 0xb8, 0x3f, 0x01, 0x00, 0x00, 0x48, 0x8d, 0x3d, 0x6d, 0x00, 0x00,
    0x00, 0x31, 0xf6, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x78, 0x47, 0x89, 0xc3,
    0x4c, 0x89, 0xee, 0x4c, 0x89, 0xf2, 0x48, 0x85, 0xd2, 0x74, 0x16, 0xb8,
    0x01, 0x00, 0x00, 0x00, 0x89, 0xdf, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x7e,
    0x2c, 0x48, 0x01, 0xc6, 0x48, 0x29, 0xc2, 0xeb, 0xe5, 0xb8, 0x42, 0x01,
    0x00, 0x00, 0x89, 0xdf, 0x48, 0x8d, 0x35, 0x3a, 0x00, 0x00, 0x00, 0x49,
    0x8d, 0x54, 0x24, 0x08, 0x4d, 0x8b, 0x14, 0x24, 0x4f, 0x8d, 0x54, 0xd4,
    0x10, 0x41, 0xb8, 0x00, 0x10, 0x00, 0x00, 0x0f, 0x05, 0xb8, 0x3c, 0x00,
    0x00, 0x00, 0xbf, 0x7f, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x90, 0x11, 0x22,
    0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33,
    0x22, 0x11, 0x65, 0x78, 0x70, 0x61, 0x63, 0x6b, 0x00, 0x00
};

static const unsigned char expack_lzrep_stub_x86_64[] = {
    0x31, 0xed, 0x49, 0x89, 0xe4, 0x4c, 0x8b, 0x35, 0x6f, 0x01, 0x00, 0x00,
    0x31, 0xff, 0x4c, 0x89, 0xf6, 0xba, 0x03, 0x00, 0x00, 0x00, 0x41, 0xba,
    0x22, 0x00, 0x00, 0x00, 0x41, 0xb8, 0xff, 0xff, 0xff, 0xff, 0x45, 0x31,
    0xc9, 0xb8, 0x09, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x0f,
    0x88, 0x39, 0x01, 0x00, 0x00, 0x49, 0x89, 0xc5, 0x4f, 0x8d, 0x7c, 0x35,
    0x00, 0x48, 0x8d, 0x1d, 0x4f, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x2d, 0x38,
    0x01, 0x00, 0x00, 0x48, 0x01, 0xdd, 0x4c, 0x89, 0xef, 0x45, 0x31, 0xc0,
    0x45, 0x31, 0xc9, 0x41, 0xbb, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x39, 0xff,
    0x0f, 0x83, 0xa7, 0x00, 0x00, 0x00, 0x45, 0x85, 0xc9, 0x75, 0x16, 0x48,
    0x39, 0xeb, 0x0f, 0x83, 0xfa, 0x00, 0x00, 0x00, 0x44, 0x0f, 0xb6, 0x03,
    0x48, 0xff, 0xc3, 0x41, 0xb9, 0x08, 0x00, 0x00, 0x00, 0x44, 0x89, 0xc0,
    0x83, 0xe0, 0x01, 0x41, 0xd1, 0xe8, 0x41, 0xff, 0xc9, 0x85, 0xc0, 0x75,
    0x15, 0x48, 0x39, 0xeb, 0x0f, 0x83, 0xd4, 0x00, 0x00, 0x00, 0x8a, 0x03,
    0x48, 0xff, 0xc3, 0x88, 0x07, 0x48, 0xff, 0xc7, 0xeb, 0xb7, 0x48, 0x39,
    0xeb, 0x0f, 0x83, 0xbf, 0x00, 0x00, 0x00, 0x0f, 0xb6, 0x03, 0x48, 0xff,
    0xc3, 0xa8, 0x80, 0x74, 0x0d, 0x89, 0xc1, 0x83, 0xe1, 0x7f, 0x83, 0xc1,
    0x03, 0x44, 0x89, 0xda, 0xeb, 0x24, 0x48, 0x39, 0xeb, 0x0f, 0x83, 0x9f,
    0x00, 0x00, 0x00, 0x0f, 0xb6, 0x13, 0x48, 0xff, 0xc3, 0x89, 0xc1, 0x83,
    0xe1, 0x0f, 0x83, 0xc1, 0x03, 0xc1, 0xe8, 0x04, 0xc1, 0xe0, 0x08, 0x09,
    0xc2, 0xff, 0xc2, 0x41, 0x89, 0xd3, 0x48, 0x89, 0xfe, 0x48, 0x29, 0xd6,
    0x4c, 0x39, 0xee, 0x72, 0x79, 0x4c, 0x39, 0xff, 0x73, 0x74, 0x8a, 0x06,
    0x48, 0xff, 0xc6, 0x88, 0x07, 0x48, 0xff, 0xc7, 0xff, 0xc9, 0x75, 0xed,
    0xe9, 0x50, 0xff, 0xff, 0xff, 0x48, 0x39, 0xeb, 0x75, 0x5c, 0xb8, 0x3f,
    0x01, 0x00, 0x00, 0x48, 0x8d, 0x3d, 0x6d, 0x00, 0x00, 0x00, 0x31, 0xf6,
    0x0f, 0x05, 0x48, 0x85, 0xc0, 0x78, 0x47, 0x89, 0xc3, 0x4c, 0x89, 0xee,
    0x4c, 0x89, 0xf2, 0x48, 0x85, 0xd2, 0x74, 0x16, 0xb8, 0x01, 0x00, 0x00,
    0x00, 0x89, 0xdf, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x7e, 0x2c, 0x48, 0x01,
    0xc6, 0x48, 0x29, 0xc2, 0xeb, 0xe5, 0xb8, 0x42, 0x01, 0x00, 0x00, 0x89,
    0xdf, 0x48, 0x8d, 0x35, 0x3a, 0x00, 0x00, 0x00, 0x49, 0x8d, 0x54, 0x24,
    0x08, 0x4d, 0x8b, 0x14, 0x24, 0x4f, 0x8d, 0x54, 0xd4, 0x10, 0x41, 0xb8,
    0x00, 0x10, 0x00, 0x00, 0x0f, 0x05, 0xb8, 0x3c, 0x00, 0x00, 0x00, 0xbf,
    0x7f, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x90, 0x11, 0x22, 0x33, 0x44, 0x55,
    0x66, 0x77, 0x88, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x65,
    0x78, 0x70, 0x61, 0x63, 0x6b, 0x00, 0x00
};

static const unsigned char expack_lzss_bcj_stub_x86_64[] = {
    0x31, 0xed, 0x49, 0x89, 0xe4, 0x4c, 0x8b, 0x35, 0xb4, 0x01, 0x00, 0x00,
    0x31, 0xff, 0x4c, 0x89, 0xf6, 0xba, 0x03, 0x00, 0x00, 0x00, 0x41, 0xba,
    0x22, 0x00, 0x00, 0x00, 0x41, 0xb8, 0xff, 0xff, 0xff, 0xff, 0x45, 0x31,
    0xc9, 0xb8, 0x09, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x0f,
    0x88, 0x7e, 0x01, 0x00, 0x00, 0x49, 0x89, 0xc5, 0x4f, 0x8d, 0x7c, 0x35,
    0x00, 0x48, 0x8d, 0x1d, 0x94, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x2d, 0x7d,
    0x01, 0x00, 0x00, 0x48, 0x01, 0xdd, 0x4c, 0x89, 0xef, 0x45, 0x31, 0xc0,
    0x45, 0x31, 0xc9, 0x4c, 0x39, 0xff, 0x0f, 0x83, 0x95, 0x00, 0x00, 0x00,
    0x45, 0x85, 0xc9, 0x75, 0x16, 0x48, 0x39, 0xeb, 0x0f, 0x83, 0x45, 0x01,
    0x00, 0x00, 0x44, 0x0f, 0xb6, 0x03, 0x48, 0xff, 0xc3, 0x41, 0xb9, 0x08,
    0x00, 0x00, 0x00, 0x44, 0x89, 0xc0, 0x83, 0xe0, 0x01, 0x41, 0xd1, 0xe8,
    0x41, 0xff, 0xc9, 0x85, 0xc0, 0x75, 0x15, 0x48, 0x39, 0xeb, 0x0f, 0x83,
    0x1f, 0x01, 0x00, 0x00, 0x8a, 0x03, 0x48, 0xff, 0xc3, 0x88, 0x07, 0x48,
    0xff, 0xc7, 0xeb, 0xb7, 0x48, 0x8d, 0x43, 0x02, 0x48, 0x39, 0xe8, 0x0f,
    0x87, 0x06, 0x01, 0x00, 0x00, 0x0f, 0xb6, 0x03, 0x0f, 0xb6, 0x4b, 0x01,
    0x48, 0x83, 0xc3, 0x02, 0x89, 0xca, 0xc1, 0xea, 0x03, 0xc1, 0xe2, 0x08,
    0x09, 0xc2, 0xff, 0xc2, 0x83, 0xe1, 0x07, 0x83, 0xc1, 0x03, 0x48, 0x89,
    0xfe, 0x48, 0x29, 0xd6, 0x4c, 0x39, 0xee, 0x0f, 0x82, 0xda, 0x00, 0x00,
    0x00, 0x4c, 0x39, 0xff, 0x0f, 0x83, 0xd1, 0x00, 0x00, 0x00, 0x8a, 0x06,
    0x48, 0xff, 0xc6, 0x88, 0x07, 0x48, 0xff, 0xc7, 0xff, 0xc9, 0x75, 0xe9,
    0xe9, 0x62, 0xff, 0xff, 0xff, 0x48, 0x39, 0xeb, 0x0f, 0x85, 0xb5, 0x00,
    0x00, 0x00, 0x4c, 0x89, 0xee, 0x49, 0x8d, 0x7f, 0xfa, 0x48, 0x39, 0xfe,
    0x77, 0x4d, 0x8a, 0x06, 0x3c, 0xe8, 0x74, 0x28, 0x3c, 0xe9, 0x74, 0x24,
    0x3c, 0x0f, 0x75, 0x37, 0x8a, 0x46, 0x01, 0x24, 0xf0, 0x3c, 0x80, 0x75,
    0x2e, 0x8b, 0x46, 0x02, 0x48, 0x89, 0xf2, 0x4c, 0x29, 0xea, 0x83, 0xc2,
    0x06, 0x29, 0xd0, 0x89, 0x46, 0x02, 0x48, 0x83, 0xc6, 0x06, 0xeb, 0x1a,
    0x8b, 0x46, 0x01, 0x48, 0x89, 0xf2, 0x4c, 0x29, 0xea, 0x83, 0xc2, 0x05,
    0x29, 0xd0, 0x89, 0x46, 0x01, 0x48, 0x83, 0xc6, 0x05, 0xeb, 0x03, 0x48,
    0xff, 0xc6, 0x48, 0x39, 0xfe, 0x76, 0xb3, 0xb8, 0x3f, 0x01, 0x00, 0x00,
    0x48, 0x8d, 0x3d, 0x6d, 0x00, 0x00, 0x00, 0x31, 0xf6, 0x0f, 0x05, 0x48,
    0x85, 0xc0, 0x78, 0x47, 0x89, 0xc3, 0x4c, 0x89, 0xee, 0x4c, 0x89, 0xf2,
    0x48, 0x85, 0xd2, 0x74, 0x16, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x89, 0xdf,
    0x0f, 0x05, 0x48, 0x85, 0xc0, 0x7e, 0x2c, 0x48, 0x01, 0xc6, 0x48, 0x29,
    0xc2, 0xeb, 0xe5, 0xb8, 0x42, 0x01, 0x00, 0x00, 0x89, 0xdf, 0x48, 0x8d,
    0x35, 0x3a, 0x00, 0x00, 0x00, 0x49, 0x8d, 0x54, 0x24, 0x08, 0x4d, 0x8b,
    0x14, 0x24, 0x4f, 0x8d, 0x54, 0xd4, 0x10, 0x41, 0xb8, 0x00, 0x10, 0x00,
    0x00, 0x0f, 0x05, 0xb8, 0x3c, 0x00, 0x00, 0x00, 0xbf, 0x7f, 0x00, 0x00,
    0x00, 0x0f, 0x05, 0x90, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x65, 0x78, 0x70, 0x61,
    0x63, 0x6b, 0x00, 0x00
};

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

static unsigned long long expack_zero_run_length(const unsigned char *data, size_t size, size_t offset) {
    size_t position = offset;

    while (position < size && data[position] == 0U) {
        position += 1U;
    }
    return (unsigned long long)(position - offset);
}

static unsigned long long expack_byte_run_length(const unsigned char *data, size_t size, size_t offset) {
    size_t position = offset;
    unsigned char value;

    if (offset >= size) {
        return 0ULL;
    }
    value = data[offset];
    while (position < size && data[position] == value) {
        position += 1U;
    }
    return (unsigned long long)(position - offset);
}

static int expack_should_start_zero_record(const unsigned char *data, size_t size, size_t offset) {
    return expack_zero_run_length(data, size, offset) >= EXPACK_ZERO_RUN_MIN;
}

static int expack_should_start_byte_run_record(const unsigned char *data, size_t size, size_t offset) {
    return expack_byte_run_length(data, size, offset) >= EXPACK_BYTE_RUN_MIN;
}

static unsigned long long expack_zero_encoded_size(const unsigned char *data, size_t size) {
    size_t position = 0U;
    unsigned long long encoded_size = 0ULL;

    while (position < size) {
        unsigned long long zero_count = expack_zero_run_length(data, size, position);
        if (zero_count >= EXPACK_ZERO_RUN_MIN) {
            while (zero_count > 0ULL) {
                unsigned int chunk = zero_count > 0xffffffffULL ? 0xffffffffU : (unsigned int)zero_count;
                encoded_size += 5ULL;
                position += (size_t)chunk;
                zero_count -= (unsigned long long)chunk;
            }
        } else {
            size_t literal_start = position;

            position += 1U;
            while (position < size && position - literal_start < EXPACK_LITERAL_MAX && !expack_should_start_zero_record(data, size, position)) {
                position += 1U;
            }
            encoded_size += 5ULL + (unsigned long long)(position - literal_start);
        }
    }
    return encoded_size;
}

static int expack_zero_store_record(unsigned char *payload, size_t payload_capacity, size_t *payload_offset, unsigned char type, unsigned int length) {
    if (*payload_offset + 5U > payload_capacity) {
        return -1;
    }
    payload[*payload_offset] = type;
    archive_store_u32_le(payload + *payload_offset + 1U, length);
    *payload_offset += 5U;
    return 0;
}

static int expack_compress_zero_run(const unsigned char *data, size_t size, unsigned char **payload_out, size_t *payload_size_out) {
    unsigned long long encoded_bound = expack_zero_encoded_size(data, size);
    unsigned char *payload;
    size_t position = 0U;
    size_t payload_offset = 0U;

    if (encoded_bound > (unsigned long long)((size_t)-1)) {
        return -1;
    }
    payload = (unsigned char *)rt_malloc((size_t)encoded_bound == 0U ? 1U : (size_t)encoded_bound);
    if (payload == 0) {
        return -1;
    }

    while (position < size) {
        unsigned long long zero_count = expack_zero_run_length(data, size, position);
        if (zero_count >= EXPACK_ZERO_RUN_MIN) {
            while (zero_count > 0ULL) {
                unsigned int chunk = zero_count > 0xffffffffULL ? 0xffffffffU : (unsigned int)zero_count;
                if (expack_zero_store_record(payload, (size_t)encoded_bound, &payload_offset, 1U, chunk) != 0) {
                    rt_free(payload);
                    return -1;
                }
                position += (size_t)chunk;
                zero_count -= (unsigned long long)chunk;
            }
        } else {
            size_t literal_start = position;

            position += 1U;
            while (position < size && position - literal_start < EXPACK_LITERAL_MAX && !expack_should_start_zero_record(data, size, position)) {
                position += 1U;
            }
            if (expack_zero_store_record(payload, (size_t)encoded_bound, &payload_offset, 0U, (unsigned int)(position - literal_start)) != 0 ||
                payload_offset + (position - literal_start) > (size_t)encoded_bound) {
                rt_free(payload);
                return -1;
            }
            memcpy(payload + payload_offset, data + literal_start, position - literal_start);
            payload_offset += position - literal_start;
        }
    }

    *payload_out = payload;
    *payload_size_out = payload_offset;
    return 0;
}

static unsigned long long expack_byte_run_encoded_size(const unsigned char *data, size_t size) {
    size_t position = 0U;
    unsigned long long encoded_size = 0ULL;

    while (position < size) {
        unsigned long long run_count = expack_byte_run_length(data, size, position);
        if (run_count >= EXPACK_BYTE_RUN_MIN) {
            while (run_count > 0ULL) {
                unsigned int chunk = run_count > 0xffffffffULL ? 0xffffffffU : (unsigned int)run_count;
                encoded_size += 6ULL;
                position += (size_t)chunk;
                run_count -= (unsigned long long)chunk;
            }
        } else {
            size_t literal_start = position;

            position += 1U;
            while (position < size && position - literal_start < EXPACK_LITERAL_MAX && !expack_should_start_byte_run_record(data, size, position)) {
                position += 1U;
            }
            encoded_size += 5ULL + (unsigned long long)(position - literal_start);
        }
    }
    return encoded_size;
}

static int expack_compress_byte_run(const unsigned char *data, size_t size, unsigned char **payload_out, size_t *payload_size_out) {
    unsigned long long encoded_bound = expack_byte_run_encoded_size(data, size);
    unsigned char *payload;
    size_t position = 0U;
    size_t payload_offset = 0U;

    if (encoded_bound > (unsigned long long)((size_t)-1)) {
        return -1;
    }
    payload = (unsigned char *)rt_malloc((size_t)encoded_bound == 0U ? 1U : (size_t)encoded_bound);
    if (payload == 0) {
        return -1;
    }

    while (position < size) {
        unsigned long long run_count = expack_byte_run_length(data, size, position);
        if (run_count >= EXPACK_BYTE_RUN_MIN) {
            unsigned char value = data[position];
            while (run_count > 0ULL) {
                unsigned int chunk = run_count > 0xffffffffULL ? 0xffffffffU : (unsigned int)run_count;
                if (expack_zero_store_record(payload, (size_t)encoded_bound, &payload_offset, 1U, chunk) != 0 || payload_offset >= (size_t)encoded_bound) {
                    rt_free(payload);
                    return -1;
                }
                payload[payload_offset++] = value;
                position += (size_t)chunk;
                run_count -= (unsigned long long)chunk;
            }
        } else {
            size_t literal_start = position;

            position += 1U;
            while (position < size && position - literal_start < EXPACK_LITERAL_MAX && !expack_should_start_byte_run_record(data, size, position)) {
                position += 1U;
            }
            if (expack_zero_store_record(payload, (size_t)encoded_bound, &payload_offset, 0U, (unsigned int)(position - literal_start)) != 0 ||
                payload_offset + (position - literal_start) > (size_t)encoded_bound) {
                rt_free(payload);
                return -1;
            }
            memcpy(payload + payload_offset, data + literal_start, position - literal_start);
            payload_offset += position - literal_start;
        }
    }

    *payload_out = payload;
    *payload_size_out = payload_offset;
    return 0;
}

static size_t expack_match_length_at(const unsigned char *data, size_t size, size_t position, size_t distance, size_t max_length) {
    size_t length = 0U;

    if (distance == 0U || distance > position) {
        return 0U;
    }
    if (max_length > size - position) {
        max_length = size - position;
    }
    while (length < max_length && data[position + length] == data[position - distance + length]) {
        length += 1U;
    }
    return length;
}

static size_t expack_find_lzrep_match(const unsigned char *data, size_t size, size_t position, unsigned int *distance_out) {
    size_t window_start = position > EXPACK_LZREP_WINDOW_SIZE ? position - EXPACK_LZREP_WINDOW_SIZE : 0U;
    size_t best_length = 0U;
    unsigned int best_distance = 0U;
    size_t candidate;

    for (candidate = window_start; candidate < position; ++candidate) {
        size_t distance = position - candidate;
        size_t length = expack_match_length_at(data, size, position, distance, EXPACK_LZREP_MAX_EXPLICIT_MATCH);
        if (length > best_length && length >= COMPRESSION_LZSS_MIN_MATCH) {
            best_length = length;
            best_distance = (unsigned int)distance;
            if (best_length == EXPACK_LZREP_MAX_EXPLICIT_MATCH) {
                break;
            }
        }
    }
    *distance_out = best_distance;
    return best_length;
}

static int expack_compress_lzrep(const unsigned char *data, size_t size, unsigned char **payload_out, size_t *payload_size_out) {
    size_t bound = compression_lzss_bound(size);
    unsigned char *payload;
    size_t input_offset = 0U;
    size_t output_offset = 0U;
    unsigned int last_distance = 1U;

    if (bound == 0U && size != 0U) {
        return -1;
    }
    payload = (unsigned char *)rt_malloc(bound == 0U ? 1U : bound);
    if (payload == 0) {
        return -1;
    }

    while (input_offset < size) {
        size_t flag_offset;
        unsigned char flags = 0U;
        unsigned int bit;

        if (output_offset >= bound) {
            rt_free(payload);
            return -1;
        }
        flag_offset = output_offset++;

        for (bit = 0U; bit < 8U && input_offset < size; ++bit) {
            unsigned int explicit_distance = 0U;
            size_t repeat_length = expack_match_length_at(data, size, input_offset, last_distance, EXPACK_LZREP_MAX_REPEAT_MATCH);
            size_t explicit_length = expack_find_lzrep_match(data, size, input_offset, &explicit_distance);

            if (repeat_length >= COMPRESSION_LZSS_MIN_MATCH && repeat_length + 1U >= explicit_length) {
                if (output_offset >= bound) {
                    rt_free(payload);
                    return -1;
                }
                flags |= (unsigned char)(1U << bit);
                payload[output_offset++] = (unsigned char)(0x80U | (unsigned int)(repeat_length - COMPRESSION_LZSS_MIN_MATCH));
                input_offset += repeat_length;
            } else if (explicit_length >= COMPRESSION_LZSS_MIN_MATCH) {
                unsigned int token_distance = explicit_distance - 1U;
                unsigned int token_length = (unsigned int)(explicit_length - COMPRESSION_LZSS_MIN_MATCH);

                if (output_offset + 2U > bound) {
                    rt_free(payload);
                    return -1;
                }
                flags |= (unsigned char)(1U << bit);
                payload[output_offset++] = (unsigned char)(((token_distance >> 8U) << 4U) | token_length);
                payload[output_offset++] = (unsigned char)(token_distance & 0xffU);
                last_distance = explicit_distance;
                input_offset += explicit_length;
            } else {
                if (output_offset >= bound) {
                    rt_free(payload);
                    return -1;
                }
                payload[output_offset++] = data[input_offset++];
            }
        }
        payload[flag_offset] = flags;
    }

    *payload_out = payload;
    *payload_size_out = output_offset;
    return 0;
}

static void expack_x86_bcj_transform(unsigned char *data, size_t size) {
    size_t position = 0U;

    while (position + 4U < size) {
        if (data[position] == 0xe8U || data[position] == 0xe9U) {
            unsigned int value = archive_read_u32_le(data + position + 1U);
            value += (unsigned int)(position + 5U);
            archive_store_u32_le(data + position + 1U, value);
            position += 5U;
        } else if (position + 5U < size && data[position] == 0x0fU && (data[position + 1U] & 0xf0U) == 0x80U) {
            unsigned int value = archive_read_u32_le(data + position + 2U);
            value += (unsigned int)(position + 6U);
            archive_store_u32_le(data + position + 2U, value);
            position += 6U;
        } else {
            position += 1U;
        }
    }
}

static int expack_compress_lzss_bcj(const unsigned char *input_data, size_t input_size, unsigned char **payload_out, size_t *payload_size_out) {
    unsigned char *transformed;
    int result;

    transformed = (unsigned char *)rt_malloc(input_size == 0U ? 1U : input_size);
    if (transformed == 0) {
        return -1;
    }
    memcpy(transformed, input_data, input_size);
    expack_x86_bcj_transform(transformed, input_size);
    result = expack_compress_lzss_profile(&expack_lzss_profiles[0], transformed, input_size, payload_out, payload_size_out);
    rt_free(transformed);
    return result;
}

static int expack_validate_elf64_x86_64(const unsigned char *data, size_t size, const char **message_out) {
    unsigned short type;
    unsigned short machine;

    if (size < EXPACK_ELF_HEADER_SIZE) {
        *message_out = "input is too small for an ELF header";
        return -1;
    }
    if (!(data[0] == 0x7fU && data[1] == 'E' && data[2] == 'L' && data[3] == 'F')) {
        *message_out = "input is not an ELF file";
        return -1;
    }
    if (data[4] != 2U || data[5] != 1U || data[6] != 1U) {
        *message_out = "only ELF64 little-endian version-1 files are supported";
        return -1;
    }

    type = expack_read_u16_le(data + 16);
    machine = expack_read_u16_le(data + 18);
    if (!(type == 2U || type == 3U)) {
        *message_out = "only executable and PIE ELF files are supported";
        return -1;
    }
    if (machine != 62U) {
        *message_out = "only x86-64 ELF inputs are supported by these stubs";
        return -1;
    }

    return 0;
}

static int expack_has_macho_magic(const unsigned char *data, size_t size) {
    if (size < 4U) {
        return 0;
    }
    return (data[0] == 0xfeU && data[1] == 0xedU && data[2] == 0xfaU && (data[3] == 0xceU || data[3] == 0xcfU)) ||
           ((data[0] == 0xceU || data[0] == 0xcfU) && data[1] == 0xfaU && data[2] == 0xedU && data[3] == 0xfeU);
}

static int expack_has_pe_coff_magic(const unsigned char *data, size_t size) {
    return size >= 2U && data[0] == 'M' && data[1] == 'Z';
}

static const char *expack_macho_machine_name(unsigned int cputype) {
    if (cputype == EXPACK_MACHO_CPU_X86_64) return "Mach-O 64-bit x86-64";
    if (cputype == EXPACK_MACHO_CPU_ARM64) return "Mach-O 64-bit arm64";
    return "Mach-O 64-bit";
}

static int expack_validate_macho64_le(const unsigned char *data, size_t size, ExpackInputFormat *format_out, const char **message_out) {
    unsigned int cputype;
    unsigned int filetype;
    unsigned int ncmds;
    unsigned int sizeofcmds;
    unsigned int command_index;
    size_t command_offset = EXPACK_MACHO_HEADER64_SIZE;

    if (size < EXPACK_MACHO_HEADER64_SIZE) {
        *message_out = "input is too small for a Mach-O 64-bit header";
        return -1;
    }
    if (archive_read_u32_le(data) != 0xfeedfacfU) {
        *message_out = "only Mach-O 64-bit little-endian files are supported";
        return -1;
    }
    cputype = archive_read_u32_le(data + 4U);
    filetype = archive_read_u32_le(data + 12U);
    ncmds = archive_read_u32_le(data + 16U);
    sizeofcmds = archive_read_u32_le(data + 20U);
    if (filetype != EXPACK_MACHO_TYPE_EXECUTE) {
        *message_out = "only Mach-O executable files are supported";
        return -1;
    }
    if (!(cputype == EXPACK_MACHO_CPU_X86_64 || cputype == EXPACK_MACHO_CPU_ARM64)) {
        *message_out = "only Mach-O x86-64 and arm64 inputs are supported";
        return -1;
    }
    if (ncmds > 1024U || sizeofcmds > size - EXPACK_MACHO_HEADER64_SIZE) {
        *message_out = "invalid Mach-O load-command table";
        return -1;
    }

    format_out->macho_code_signature_offset = 0U;
    format_out->macho_code_signature_size = 0U;
    for (command_index = 0U; command_index < ncmds; ++command_index) {
        unsigned int command;
        unsigned int command_size;

        if (command_offset > size || 8U > size - command_offset) {
            *message_out = "invalid Mach-O load-command range";
            return -1;
        }
        command = archive_read_u32_le(data + command_offset);
        command_size = archive_read_u32_le(data + command_offset + 4U);
        if (command_size < 8U || command_size > size - command_offset) {
            *message_out = "invalid Mach-O load-command range";
            return -1;
        }
        if (command == EXPACK_MACHO_LC_CODE_SIGNATURE && command_size >= 16U) {
            format_out->macho_code_signature_offset = archive_read_u32_le(data + command_offset + 8U);
            format_out->macho_code_signature_size = archive_read_u32_le(data + command_offset + 12U);
            if (format_out->macho_code_signature_offset > size || format_out->macho_code_signature_size > size - format_out->macho_code_signature_offset) {
                *message_out = "invalid Mach-O code-signature range";
                return -1;
            }
        }
        command_offset += (size_t)command_size;
    }
    if (command_offset > EXPACK_MACHO_HEADER64_SIZE + (size_t)sizeofcmds) {
        *message_out = "invalid Mach-O load-command table";
        return -1;
    }

    format_out->kind = EXPACK_FORMAT_MACHO;
    format_out->name = expack_macho_machine_name(cputype);
    format_out->preprocess_error = "Mach-O preprocessing failed";
    format_out->allow_x86_bcj = cputype == EXPACK_MACHO_CPU_X86_64;
    format_out->can_write_packed_output = 0;
    format_out->macho_cputype = cputype;
    return 0;
}

static int expack_detect_input_format(const unsigned char *data, size_t size, ExpackInputFormat *format_out, const char **message_out) {
    if (size >= 4U && data[0] == 0x7fU && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        if (expack_validate_elf64_x86_64(data, size, message_out) != 0) {
            return -1;
        }
        format_out->kind = EXPACK_FORMAT_ELF64_X86_64;
        format_out->name = "ELF64 x86-64";
        format_out->preprocess_error = "ELF preprocessing failed";
        format_out->allow_x86_bcj = 1;
        format_out->can_write_packed_output = 1;
        format_out->macho_cputype = 0U;
        return 0;
    }
    if (expack_has_macho_magic(data, size)) {
        return expack_validate_macho64_le(data, size, format_out, message_out);
    }
    if (expack_has_pe_coff_magic(data, size)) {
        format_out->kind = EXPACK_FORMAT_PE_COFF;
        format_out->name = "PE/COFF";
        format_out->preprocess_error = "PE/COFF preprocessing failed";
        format_out->allow_x86_bcj = 0;
        format_out->can_write_packed_output = 0;
        format_out->macho_cputype = 0U;
        *message_out = "PE/COFF input is recognized, but expack does not have a PE/COFF image backend today";
        return -1;
    }
    *message_out = "input is not a supported executable format";
    return -1;
}

static int expack_range_end(size_t offset, size_t length, size_t limit, size_t *end_out) {
    if (offset > limit || length > limit - offset) {
        return -1;
    }
    *end_out = offset + length;
    return 0;
}

static void expack_clear_section_header_fields(unsigned char *data, size_t size) {
    if (size >= EXPACK_ELF_HEADER_SIZE) {
        archive_store_u64_le(data + 40U, 0ULL);
        expack_store_u16_le(data + 58U, 0U);
        expack_store_u16_le(data + 60U, 0U);
        expack_store_u16_le(data + 62U, 0U);
    }
}

static unsigned long long expack_align_to_segment_mod(unsigned long long value, unsigned long long vaddr, unsigned long long align) {
    unsigned long long target;
    unsigned long long current;

    if (align <= 1ULL) {
        return value;
    }
    target = vaddr % align;
    current = value % align;
    if (current <= target) {
        return value + (target - current);
    }
    return value + (align - current) + target;
}

static int expack_find_canonical_load_offset(const ExpackLoadRange *loads, unsigned int load_count, unsigned long long old_offset, unsigned long long size, unsigned long long *new_offset_out) {
    unsigned int index;

    for (index = 0U; index < load_count; ++index) {
        unsigned long long load_end = loads[index].old_offset + loads[index].file_size;
        if (old_offset >= loads[index].old_offset && size <= load_end - old_offset) {
            *new_offset_out = loads[index].new_offset + (old_offset - loads[index].old_offset);
            return 0;
        }
    }
    return -1;
}

static int expack_make_elf_exec_image(const unsigned char *input_data, size_t input_size, ExpackImage *image_out) {
    unsigned long long phoff64 = archive_read_u64_le(input_data + 32U);
    unsigned short phentsize = expack_read_u16_le(input_data + 54U);
    unsigned short phnum = expack_read_u16_le(input_data + 56U);
    size_t phoff;
    size_t phdr_size;
    unsigned long long needed_size64 = EXPACK_ELF_HEADER_SIZE;
    unsigned long long canonical_cursor = 0ULL;
    unsigned short index;
    unsigned int load_count = 0U;
    ExpackLoadRange *loads;
    unsigned char *image_data;

    image_out->data = 0;
    image_out->size = 0U;
    image_out->changed = 0;
    if (phoff64 > (unsigned long long)((size_t)-1)) {
        return -1;
    }
    phoff = (size_t)phoff64;
    if (phentsize < EXPACK_ELF_PHDR_SIZE || phnum == 0U || phnum > ((size_t)-1) / (size_t)phentsize) {
        return -1;
    }
    phdr_size = (size_t)phentsize * (size_t)phnum;
    if (phoff > input_size || phdr_size > input_size - phoff) {
        return -1;
    }
    loads = (ExpackLoadRange *)rt_malloc((size_t)phnum * sizeof(*loads));
    if (loads == 0) {
        return -1;
    }

    for (index = 0U; index < phnum; ++index) {
        size_t phdr_offset = phoff + (size_t)index * (size_t)phentsize;
        const unsigned char *phdr = input_data + phdr_offset;
        unsigned int type = archive_read_u32_le(phdr);
        unsigned long long file_offset64 = archive_read_u64_le(phdr + 8U);
        unsigned long long vaddr64 = archive_read_u64_le(phdr + 16U);
        unsigned long long file_size64 = archive_read_u64_le(phdr + 32U);
        unsigned long long align64 = archive_read_u64_le(phdr + 48U);
        unsigned long long new_offset64;

        if (file_offset64 > (unsigned long long)((size_t)-1) || file_size64 > (unsigned long long)((size_t)-1) ||
            (size_t)file_offset64 > input_size || (size_t)file_size64 > input_size - (size_t)file_offset64) {
            rt_free(loads);
            return -1;
        }
        if (type != 1U || file_size64 == 0ULL) {
            continue;
        }
        new_offset64 = expack_align_to_segment_mod(canonical_cursor, vaddr64, align64);
        if (file_size64 > 0xffffffffffffffffULL - new_offset64) {
            rt_free(loads);
            return -1;
        }
        loads[load_count].old_offset = file_offset64;
        loads[load_count].new_offset = new_offset64;
        loads[load_count].file_size = file_size64;
        load_count += 1U;
        canonical_cursor = new_offset64 + file_size64;
        if (canonical_cursor > needed_size64) {
            needed_size64 = canonical_cursor;
        }
    }

    if (needed_size64 > (unsigned long long)((size_t)-1)) {
        rt_free(loads);
        return -1;
    }
    if (phoff > (size_t)needed_size64 || phdr_size > (size_t)needed_size64 - phoff) {
        rt_free(loads);
        return -1;
    }
    image_data = (unsigned char *)rt_malloc((size_t)needed_size64 == 0U ? 1U : (size_t)needed_size64);
    if (image_data == 0) {
        rt_free(loads);
        return -1;
    }
    memset(image_data, 0, (size_t)needed_size64);
    memcpy(image_data, input_data, EXPACK_ELF_HEADER_SIZE);
    memcpy(image_data + phoff, input_data + phoff, phdr_size);
    for (index = 0U; index < phnum; ++index) {
        size_t phdr_offset = phoff + (size_t)index * (size_t)phentsize;
        const unsigned char *input_phdr = input_data + phdr_offset;
        unsigned int type = archive_read_u32_le(input_phdr);
        unsigned long long file_offset64 = archive_read_u64_le(input_phdr + 8U);
        unsigned long long file_size64 = archive_read_u64_le(input_phdr + 32U);
        unsigned long long new_offset64;
        size_t end;

        if (type != 1U || file_size64 == 0ULL) {
            continue;
        }
        if (expack_find_canonical_load_offset(loads, load_count, file_offset64, file_size64, &new_offset64) != 0) {
            continue;
        }
        if (new_offset64 > needed_size64 || file_size64 > needed_size64 - new_offset64 || expack_range_end((size_t)file_offset64, (size_t)file_size64, input_size, &end) != 0) {
            rt_free(loads);
            rt_free(image_data);
            return -1;
        }
        memcpy(image_data + (size_t)new_offset64, input_data + (size_t)file_offset64, (size_t)file_size64);
    }
    for (index = 0U; index < phnum; ++index) {
        size_t phdr_offset = phoff + (size_t)index * (size_t)phentsize;
        unsigned char *image_phdr = image_data + phdr_offset;
        const unsigned char *input_phdr = input_data + phdr_offset;
        unsigned long long file_offset64 = archive_read_u64_le(input_phdr + 8U);
        unsigned long long file_size64 = archive_read_u64_le(input_phdr + 32U);
        unsigned long long new_offset64;

        if (file_size64 != 0ULL && expack_find_canonical_load_offset(loads, load_count, file_offset64, file_size64, &new_offset64) == 0) {
            archive_store_u64_le(image_phdr + 8U, new_offset64);
        }
    }
    expack_clear_section_header_fields(image_data, (size_t)needed_size64);

    image_out->data = image_data;
    image_out->size = (size_t)needed_size64;
    image_out->changed = (size_t)needed_size64 != input_size || memcmp(image_data, input_data, input_size) != 0;
    rt_free(loads);
    return 0;
}

static int expack_make_exact_exec_image(const unsigned char *input_data, size_t input_size, ExpackImage *image_out) {
    image_out->data = (unsigned char *)input_data;
    image_out->size = input_size;
    image_out->changed = 0;
    return 0;
}

static void expack_release_exec_image(ExpackImage *image, const unsigned char *input_data) {
    if (image->data != 0 && image->data != input_data) {
        rt_free(image->data);
    }
    image->data = 0;
    image->size = 0U;
    image->changed = 0;
}

static int expack_make_exec_image(const ExpackInputFormat *format, const unsigned char *input_data, size_t input_size, ExpackImage *image_out) {
    if (format->kind == EXPACK_FORMAT_ELF64_X86_64) {
        return expack_make_elf_exec_image(input_data, input_size, image_out);
    }
    if (format->kind == EXPACK_FORMAT_MACHO) {
        return expack_make_exact_exec_image(input_data, input_size, image_out);
    }
    image_out->data = 0;
    image_out->size = 0U;
    image_out->changed = 0;
    return -1;
}

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

static int expack_compress_lzss_profile(const ExpackLzssProfile *profile, const unsigned char *input_data, size_t input_size, unsigned char **payload_out, size_t *payload_size_out) {
    size_t bound = compression_lzss_bound(input_size);
    unsigned char *payload;

    if (bound == 0U && input_size != 0U) {
        return -1;
    }
    payload = (unsigned char *)rt_malloc(bound == 0U ? 1U : bound);
    if (payload == 0) {
        return -1;
    }
    if (compression_lzss_compress_profile(profile->profile_id, input_data, input_size, payload, bound, payload_size_out) != 0) {
        rt_free(payload);
        return -1;
    }
    *payload_out = payload;
    return 0;
}

static unsigned long long expack_packed_size(size_t stub_size, size_t payload_size) {
    return (unsigned long long)EXPACK_ELF_CODE_OFFSET + (unsigned long long)stub_size + (unsigned long long)payload_size;
}

static void expack_candidate_release(ExpackCandidate *candidate) {
    if (candidate->payload != 0) {
        rt_free(candidate->payload);
    }
    candidate->codec = 0U;
    candidate->lzss_profile = 0;
    candidate->stub = 0;
    candidate->stub_size = 0U;
    candidate->payload = 0;
    candidate->payload_size = 0U;
    candidate->packed_size = 0ULL;
}

static void expack_candidate_take(ExpackCandidate *selected, ExpackCandidate *candidate) {
    expack_candidate_release(selected);
    *selected = *candidate;
    candidate->payload = 0;
}

static int expack_consider_candidate(ExpackCandidate *selected, int *have_selected, ExpackCandidate *candidate) {
    if (!*have_selected || candidate->packed_size < selected->packed_size) {
        expack_candidate_take(selected, candidate);
        *have_selected = 1;
    }
    expack_candidate_release(candidate);
    return 0;
}

static int expack_make_raw_candidate(const unsigned char *input_data, size_t input_size, ExpackCandidate *candidate) {
    unsigned char *payload = (unsigned char *)rt_malloc(input_size == 0U ? 1U : input_size);
    if (payload == 0) {
        return -1;
    }
    if (input_size != 0U) {
        memcpy(payload, input_data, input_size);
    }
    expack_candidate_release(candidate);
    candidate->codec = EXPACK_CODEC_RAW;
    candidate->lzss_profile = 0;
    candidate->stub = 0;
    candidate->stub_size = 0U;
    candidate->payload = payload;
    candidate->payload_size = input_size;
    candidate->packed_size = (unsigned long long)input_size;
    return 0;
}

static void expack_report_candidate(const ExpackCandidate *candidate) {
    rt_write_cstr(1, "  ");
    expack_write_candidate_name(candidate);
    rt_write_cstr(1, ": payload ");
    rt_write_uint(1, (unsigned long long)candidate->payload_size);
    rt_write_cstr(1, ", stub ");
    rt_write_uint(1, (unsigned long long)candidate->stub_size);
    rt_write_cstr(1, ", packed ");
    rt_write_uint(1, candidate->packed_size);
    rt_write_cstr(1, "\n");
}

static int expack_select_best_payload(const unsigned char *input_data, size_t input_size, ExpackCandidate *selected_out, int report_candidates, int allow_x86_bcj) {
    unsigned int profile_index;
    int have_selected = 0;

    expack_candidate_release(selected_out);
    for (profile_index = 0U; profile_index < sizeof(expack_lzss_profiles) / sizeof(expack_lzss_profiles[0]); ++profile_index) {
        ExpackCandidate candidate;

        candidate.codec = EXPACK_CODEC_LZSS;
        candidate.lzss_profile = &expack_lzss_profiles[profile_index];
        candidate.stub = expack_stub_x86_64;
        candidate.stub_size = sizeof(expack_stub_x86_64);
        candidate.payload = 0;
        candidate.payload_size = 0U;
        candidate.packed_size = 0ULL;

        if (expack_compress_lzss_profile(candidate.lzss_profile, input_data, input_size, &candidate.payload, &candidate.payload_size) != 0) {
            continue;
        }
        candidate.packed_size = expack_packed_size(candidate.stub_size, candidate.payload_size);
        if (report_candidates) expack_report_candidate(&candidate);
        expack_consider_candidate(selected_out, &have_selected, &candidate);
    }

    {
        ExpackCandidate candidate;

        candidate.codec = EXPACK_CODEC_ZERO_RUN;
        candidate.lzss_profile = 0;
        candidate.stub = expack_zero_stub_x86_64;
        candidate.stub_size = sizeof(expack_zero_stub_x86_64);
        candidate.payload = 0;
        candidate.payload_size = 0U;
        candidate.packed_size = 0ULL;
        if (expack_compress_zero_run(input_data, input_size, &candidate.payload, &candidate.payload_size) == 0) {
            candidate.packed_size = expack_packed_size(candidate.stub_size, candidate.payload_size);
            if (report_candidates) expack_report_candidate(&candidate);
            expack_consider_candidate(selected_out, &have_selected, &candidate);
        }
    }

    {
        ExpackCandidate candidate;

        candidate.codec = EXPACK_CODEC_BYTE_RUN;
        candidate.lzss_profile = 0;
        candidate.stub = expack_byte_run_stub_x86_64;
        candidate.stub_size = sizeof(expack_byte_run_stub_x86_64);
        candidate.payload = 0;
        candidate.payload_size = 0U;
        candidate.packed_size = 0ULL;
        if (expack_compress_byte_run(input_data, input_size, &candidate.payload, &candidate.payload_size) == 0) {
            candidate.packed_size = expack_packed_size(candidate.stub_size, candidate.payload_size);
            if (report_candidates) expack_report_candidate(&candidate);
            expack_consider_candidate(selected_out, &have_selected, &candidate);
        }
    }

    {
        ExpackCandidate candidate;

        candidate.codec = EXPACK_CODEC_LZREP;
        candidate.lzss_profile = 0;
        candidate.stub = expack_lzrep_stub_x86_64;
        candidate.stub_size = sizeof(expack_lzrep_stub_x86_64);
        candidate.payload = 0;
        candidate.payload_size = 0U;
        candidate.packed_size = 0ULL;
        if (expack_compress_lzrep(input_data, input_size, &candidate.payload, &candidate.payload_size) == 0) {
            candidate.packed_size = expack_packed_size(candidate.stub_size, candidate.payload_size);
            if (report_candidates) expack_report_candidate(&candidate);
            expack_consider_candidate(selected_out, &have_selected, &candidate);
        }
    }

    if (allow_x86_bcj) {
        ExpackCandidate candidate;

        candidate.codec = EXPACK_CODEC_LZSS_BCJ;
        candidate.lzss_profile = &expack_lzss_profiles[0];
        candidate.stub = expack_lzss_bcj_stub_x86_64;
        candidate.stub_size = sizeof(expack_lzss_bcj_stub_x86_64);
        candidate.payload = 0;
        candidate.payload_size = 0U;
        candidate.packed_size = 0ULL;
        if (expack_compress_lzss_bcj(input_data, input_size, &candidate.payload, &candidate.payload_size) == 0) {
            candidate.packed_size = expack_packed_size(candidate.stub_size, candidate.payload_size);
            if (report_candidates) expack_report_candidate(&candidate);
            expack_consider_candidate(selected_out, &have_selected, &candidate);
        }
    }

    return have_selected ? 0 : -1;
}

static void expack_write_elf_header(unsigned char *header, unsigned long long file_size) {
    unsigned long long entry = EXPACK_ELF_BASE_VADDR + EXPACK_ELF_CODE_OFFSET;

    memset(header, 0, EXPACK_ELF_CODE_OFFSET);
    header[0] = 0x7fU;
    header[1] = 'E';
    header[2] = 'L';
    header[3] = 'F';
    header[4] = 2U;
    header[5] = 1U;
    header[6] = 1U;
    expack_store_u16_le(header + 16, 2U);
    expack_store_u16_le(header + 18, 62U);
    archive_store_u32_le(header + 20, 1U);
    archive_store_u64_le(header + 24, entry);
    archive_store_u64_le(header + 32, EXPACK_ELF_HEADER_SIZE);
    expack_store_u16_le(header + 52, EXPACK_ELF_HEADER_SIZE);
    expack_store_u16_le(header + 54, EXPACK_ELF_PHDR_SIZE);
    expack_store_u16_le(header + 56, 1U);

    archive_store_u32_le(header + EXPACK_ELF_HEADER_SIZE + 0, 1U);
    archive_store_u32_le(header + EXPACK_ELF_HEADER_SIZE + 4, 5U);
    archive_store_u64_le(header + EXPACK_ELF_HEADER_SIZE + 8, 0ULL);
    archive_store_u64_le(header + EXPACK_ELF_HEADER_SIZE + 16, EXPACK_ELF_BASE_VADDR);
    archive_store_u64_le(header + EXPACK_ELF_HEADER_SIZE + 24, EXPACK_ELF_BASE_VADDR);
    archive_store_u64_le(header + EXPACK_ELF_HEADER_SIZE + 32, file_size);
    archive_store_u64_le(header + EXPACK_ELF_HEADER_SIZE + 40, file_size);
    archive_store_u64_le(header + EXPACK_ELF_HEADER_SIZE + 48, 0x1000ULL);
}

static int expack_candidate_size_offsets(const ExpackCandidate *candidate, unsigned int *original_size_offset_out, unsigned int *payload_size_offset_out) {
    if (candidate->codec == EXPACK_CODEC_LZSS) {
        *original_size_offset_out = EXPACK_LZSS_ORIGINAL_SIZE_OFFSET;
        *payload_size_offset_out = EXPACK_LZSS_PAYLOAD_SIZE_OFFSET;
        return 0;
    }
    if (candidate->codec == EXPACK_CODEC_ZERO_RUN) {
        *original_size_offset_out = EXPACK_ZERO_ORIGINAL_SIZE_OFFSET;
        *payload_size_offset_out = EXPACK_ZERO_PAYLOAD_SIZE_OFFSET;
        return 0;
    }
    if (candidate->codec == EXPACK_CODEC_BYTE_RUN) {
        *original_size_offset_out = EXPACK_BYTE_RUN_ORIGINAL_SIZE_OFFSET;
        *payload_size_offset_out = EXPACK_BYTE_RUN_PAYLOAD_SIZE_OFFSET;
        return 0;
    }
    if (candidate->codec == EXPACK_CODEC_LZREP) {
        *original_size_offset_out = EXPACK_LZREP_ORIGINAL_SIZE_OFFSET;
        *payload_size_offset_out = EXPACK_LZREP_PAYLOAD_SIZE_OFFSET;
        return 0;
    }
    if (candidate->codec == EXPACK_CODEC_LZSS_BCJ) {
        *original_size_offset_out = EXPACK_LZSS_BCJ_ORIGINAL_SIZE_OFFSET;
        *payload_size_offset_out = EXPACK_LZSS_BCJ_PAYLOAD_SIZE_OFFSET;
        return 0;
    }
    return -1;
}

static int expack_patch_stub(unsigned char *stub, const ExpackCandidate *candidate, size_t original_size) {
    unsigned int original_size_offset;
    unsigned int payload_size_offset;

    if (expack_candidate_size_offsets(candidate, &original_size_offset, &payload_size_offset) != 0) {
        return -1;
    }
    if (candidate->codec == EXPACK_CODEC_LZSS) {
        stub[EXPACK_LZSS_STUB_LENGTH_SHIFT_OFFSET] = candidate->lzss_profile->length_shift;
        stub[EXPACK_LZSS_STUB_LENGTH_MASK_OFFSET] = candidate->lzss_profile->length_mask;
    }
    archive_store_u64_le(stub + original_size_offset, (unsigned long long)original_size);
    archive_store_u64_le(stub + payload_size_offset, (unsigned long long)candidate->payload_size);
    return 0;
}

static int expack_write_packed_elf(const char *output_path, const ExpackCandidate *candidate, size_t original_size) {
    unsigned char header[EXPACK_ELF_CODE_OFFSET];
    unsigned char stub[sizeof(expack_lzss_bcj_stub_x86_64)];
    unsigned long long file_size = (unsigned long long)sizeof(header) + (unsigned long long)candidate->stub_size + (unsigned long long)candidate->payload_size;
    int output_fd;

    expack_write_elf_header(header, file_size);
    memcpy(stub, candidate->stub, candidate->stub_size);
    if (expack_patch_stub(stub, candidate, original_size) != 0) {
        return -1;
    }

    output_fd = platform_open_write(output_path, 0755U);
    if (output_fd < 0) {
        return -1;
    }
    if (rt_write_all(output_fd, header, sizeof(header)) != 0 || rt_write_all(output_fd, stub, candidate->stub_size) != 0 ||
        rt_write_all(output_fd, candidate->payload, candidate->payload_size) != 0) {
        platform_close(output_fd);
        return -1;
    }
    if (platform_close(output_fd) != 0) {
        return -1;
    }
    return 0;
}

static int expack_macho_container_subtype(unsigned int cputype, unsigned int *subtype_out) {
    if (cputype == EXPACK_MACHO_CPU_ARM64) {
        *subtype_out = 0U;
        return 0;
    }
    if (cputype == EXPACK_MACHO_CPU_X86_64) {
        *subtype_out = 3U;
        return 0;
    }
    return -1;
}

static const unsigned char *expack_macho_container_stub(unsigned int cputype, unsigned int codec, size_t *stub_size_out) {
    static const unsigned char arm64_raw_runner_stub[] = {
        0xf5, 0x03, 0x01, 0xaa, 0xf6, 0x03, 0x02, 0xaa, 0xff, 0x03, 0x01, 0xd1,
        0xf7, 0x03, 0x00, 0x91, 0x45, 0x07, 0x00, 0x10, 0x26, 0x03, 0x80, 0xd2,
        0xa7, 0x14, 0x40, 0x38, 0xe7, 0x16, 0x00, 0x38, 0xc6, 0x04, 0x00, 0xf1,
        0xa1, 0xff, 0xff, 0x54, 0xf7, 0x03, 0x00, 0x91, 0x90, 0x02, 0x80, 0xd2,
        0x01, 0x10, 0x00, 0xd4, 0xe1, 0x42, 0x00, 0x91, 0x02, 0x01, 0x80, 0xd2,
        0x03, 0xfc, 0x5c, 0xd3, 0x63, 0x0c, 0x40, 0x92, 0x64, 0x06, 0x00, 0x30,
        0x83, 0x68, 0x63, 0x38, 0x23, 0x14, 0x00, 0x38, 0x00, 0xec, 0x7c, 0xd3,
        0x42, 0x04, 0x00, 0xf1, 0x21, 0xff, 0xff, 0x54, 0xe0, 0x03, 0x17, 0xaa,
        0x21, 0xc0, 0x81, 0xd2, 0x02, 0x38, 0x80, 0xd2, 0xb0, 0x00, 0x80, 0xd2,
        0x01, 0x10, 0x00, 0xd4, 0x62, 0x03, 0x00, 0x54, 0xf3, 0x03, 0x00, 0xaa,
        0x89, 0x03, 0x00, 0x10, 0x38, 0x01, 0x40, 0xf9, 0x38, 0x01, 0x18, 0x8b,
        0x69, 0x03, 0x00, 0x10, 0x39, 0x01, 0x40, 0xf9, 0x99, 0x01, 0x00, 0xb4,
        0xe0, 0x03, 0x13, 0xaa, 0xe1, 0x03, 0x18, 0xaa, 0xe2, 0x03, 0x19, 0xaa,
        0x90, 0x00, 0x80, 0xd2,
        0x01, 0x10, 0x00, 0xd4, 0xc2, 0x01, 0x00, 0x54, 0x1f, 0x04, 0x00, 0xf1,
        0x8b, 0x01, 0x00, 0x54, 0x18, 0x03, 0x00, 0x8b, 0x39, 0x03, 0x00, 0xcb,
        0xf5, 0xff, 0xff, 0x17, 0xe0, 0x03, 0x13, 0xaa, 0xd0, 0x00, 0x80, 0xd2,
        0x01, 0x10, 0x00, 0xd4, 0xe0, 0x03, 0x17, 0xaa, 0xe1, 0x03, 0x15, 0xaa,
        0xe2, 0x03, 0x16, 0xaa, 0x70, 0x07, 0x80, 0xd2, 0x01, 0x10, 0x00, 0xd4,
        0xe0, 0x0f, 0x80, 0xd2, 0x30, 0x00, 0x80, 0xd2, 0x01, 0x10, 0x00, 0xd4,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x2f, 0x74, 0x6d, 0x70,
        0x2f, 0x65, 0x78, 0x70, 0x61, 0x63, 0x6b, 0x2d, 0x72, 0x75, 0x6e, 0x2d,
        0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x00, 0x30, 0x31, 0x32,
        0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x61, 0x62, 0x63, 0x64, 0x65,
        0x66
    };
    static const unsigned char arm64_lzrep_runner_stub[] = {
#include "expack/macho_arm64_lzrep_runner.inc"
    };
    static const unsigned char x86_64_exit_stub[] = {
        0xb8, 0x01, 0x00, 0x00, 0x02,
        0xbf, 0x7f, 0x00, 0x00, 0x00,
        0x0f, 0x05
    };

    if (cputype == EXPACK_MACHO_CPU_ARM64 && codec == EXPACK_CODEC_RAW) {
        *stub_size_out = sizeof(arm64_raw_runner_stub);
        return arm64_raw_runner_stub;
    }
    if (cputype == EXPACK_MACHO_CPU_ARM64 && codec == EXPACK_CODEC_LZREP) {
        *stub_size_out = sizeof(arm64_lzrep_runner_stub);
        return arm64_lzrep_runner_stub;
    }
    if (cputype == EXPACK_MACHO_CPU_X86_64) {
        *stub_size_out = sizeof(x86_64_exit_stub);
        return x86_64_exit_stub;
    }
    *stub_size_out = 0U;
    return 0;
}

static void expack_write_macho_section(unsigned char *section, const char *section_name, const char *segment_name, unsigned long long address, unsigned long long size, unsigned int offset, unsigned int alignment, unsigned int flags) {
    expack_store_fixed_name(section, section_name);
    expack_store_fixed_name(section + 16U, segment_name);
    archive_store_u64_le(section + 32U, address);
    archive_store_u64_le(section + 40U, size);
    archive_store_u32_le(section + 48U, offset);
    archive_store_u32_le(section + 52U, alignment);
    archive_store_u32_le(section + 56U, 0U);
    archive_store_u32_le(section + 60U, 0U);
    archive_store_u32_le(section + 64U, flags);
    archive_store_u32_le(section + 68U, 0U);
    archive_store_u32_le(section + 72U, 0U);
    archive_store_u32_le(section + 76U, 0U);
}

static void expack_write_macho_segment64(unsigned char *segment, const char *segment_name, unsigned long long vmaddr, unsigned long long vmsize, unsigned long long fileoff, unsigned long long filesize, unsigned int maxprot, unsigned int initprot, unsigned int nsects, unsigned int flags) {
    archive_store_u32_le(segment + 0U, EXPACK_MACHO_LC_SEGMENT_64);
    archive_store_u32_le(segment + 4U, 72U + 80U * nsects);
    expack_store_fixed_name(segment + 8U, segment_name);
    archive_store_u64_le(segment + 24U, vmaddr);
    archive_store_u64_le(segment + 32U, vmsize);
    archive_store_u64_le(segment + 40U, fileoff);
    archive_store_u64_le(segment + 48U, filesize);
    archive_store_u32_le(segment + 56U, maxprot);
    archive_store_u32_le(segment + 60U, initprot);
    archive_store_u32_le(segment + 64U, nsects);
    archive_store_u32_le(segment + 68U, flags);
}

static void expack_store_u32_be(unsigned char *out, unsigned int value) {
    out[0] = (unsigned char)((value >> 24U) & 0xffU);
    out[1] = (unsigned char)((value >> 16U) & 0xffU);
    out[2] = (unsigned char)((value >> 8U) & 0xffU);
    out[3] = (unsigned char)(value & 0xffU);
}

typedef struct {
    CryptoSha256Context context;
    unsigned char *hashes;
    size_t hash_count;
    size_t hash_index;
    size_t page_used;
} ExpackMachoPageHasher;

static int expack_macho_page_hasher_init(ExpackMachoPageHasher *hasher, size_t hash_count) {
    hasher->hashes = (unsigned char *)rt_malloc(hash_count * CRYPTO_SHA256_DIGEST_SIZE);
    if (hasher->hashes == 0 && hash_count != 0U) {
        return -1;
    }
    hasher->hash_count = hash_count;
    hasher->hash_index = 0U;
    hasher->page_used = 0U;
    crypto_sha256_init(&hasher->context);
    return 0;
}

static int expack_macho_page_hasher_update(ExpackMachoPageHasher *hasher, const unsigned char *data, size_t size) {
    size_t offset = 0U;

    while (offset < size) {
        size_t space = EXPACK_MACHO_CODE_SIGNATURE_PAGE_SIZE - hasher->page_used;
        size_t chunk = size - offset;
        if (chunk > space) {
            chunk = space;
        }
        crypto_sha256_update(&hasher->context, data + offset, chunk);
        hasher->page_used += chunk;
        offset += chunk;
        if (hasher->page_used == EXPACK_MACHO_CODE_SIGNATURE_PAGE_SIZE) {
            if (hasher->hash_index >= hasher->hash_count) {
                return -1;
            }
            crypto_sha256_final(&hasher->context, hasher->hashes + hasher->hash_index * CRYPTO_SHA256_DIGEST_SIZE);
            ++hasher->hash_index;
            hasher->page_used = 0U;
            crypto_sha256_init(&hasher->context);
        }
    }
    return 0;
}

static int expack_macho_page_hasher_finish(ExpackMachoPageHasher *hasher) {
    if (hasher->page_used != 0U) {
        if (hasher->hash_index >= hasher->hash_count) {
            return -1;
        }
        crypto_sha256_final(&hasher->context, hasher->hashes + hasher->hash_index * CRYPTO_SHA256_DIGEST_SIZE);
        ++hasher->hash_index;
        hasher->page_used = 0U;
    }
    return hasher->hash_index == hasher->hash_count ? 0 : -1;
}

static int expack_macho_page_hasher_update_zeroes(ExpackMachoPageHasher *hasher, unsigned long long size) {
    static const unsigned char zero_pad[64] = {0U};
    while (size != 0ULL) {
        size_t chunk = size > (unsigned long long)sizeof(zero_pad) ? sizeof(zero_pad) : (size_t)size;
        if (expack_macho_page_hasher_update(hasher, zero_pad, chunk) != 0) {
            return -1;
        }
        size -= (unsigned long long)chunk;
    }
    return 0;
}

static unsigned char *expack_make_macho_code_signature(const unsigned char *header, size_t header_size, const unsigned char *stub, size_t stub_size, const unsigned char *metadata, size_t metadata_size, const unsigned char *payload, size_t payload_size, unsigned long long code_limit, size_t *signature_size_out) {
    const unsigned int superblob_header_size = 20U;
    const unsigned int code_directory_fixed_size = 88U;
    const unsigned int hash_offset = code_directory_fixed_size + EXPACK_MACHO_CODE_DIRECTORY_IDENT_SIZE;
    unsigned int code_slots;
    unsigned int code_directory_size;
    unsigned int signature_size;
    unsigned char *signature;
    ExpackMachoPageHasher hasher;
    unsigned long long written_size = (unsigned long long)header_size + (unsigned long long)stub_size + (unsigned long long)metadata_size + (unsigned long long)payload_size;

    if (code_limit == 0ULL || code_limit > 0xffffffffULL || (code_limit % EXPACK_MACHO_CODE_SIGNATURE_PAGE_SIZE) != 0ULL || written_size > code_limit) {
        return 0;
    }
    code_slots = (unsigned int)(code_limit / EXPACK_MACHO_CODE_SIGNATURE_PAGE_SIZE);
    if (code_slots == 0U || code_slots > ((unsigned int)-1 - hash_offset) / CRYPTO_SHA256_DIGEST_SIZE) {
        return 0;
    }
    code_directory_size = hash_offset + code_slots * CRYPTO_SHA256_DIGEST_SIZE;
    signature_size = superblob_header_size + code_directory_size;
    signature = (unsigned char *)rt_malloc(signature_size);
    if (signature == 0) {
        return 0;
    }
    if (expack_macho_page_hasher_init(&hasher, code_slots) != 0) {
        rt_free(signature);
        return 0;
    }
    if (expack_macho_page_hasher_update(&hasher, header, header_size) != 0 ||
        expack_macho_page_hasher_update(&hasher, stub, stub_size) != 0 ||
        expack_macho_page_hasher_update(&hasher, metadata, metadata_size) != 0 ||
        expack_macho_page_hasher_update(&hasher, payload, payload_size) != 0 ||
        expack_macho_page_hasher_update_zeroes(&hasher, code_limit - written_size) != 0 ||
        expack_macho_page_hasher_finish(&hasher) != 0) {
        rt_free(hasher.hashes);
        rt_free(signature);
        return 0;
    }

    memset(signature, 0, signature_size);
    expack_store_u32_be(signature + 0U, 0xfade0cc0U);
    expack_store_u32_be(signature + 4U, signature_size);
    expack_store_u32_be(signature + 8U, 1U);
    expack_store_u32_be(signature + 12U, 0U);
    expack_store_u32_be(signature + 16U, superblob_header_size);

    expack_store_u32_be(signature + superblob_header_size + 0U, 0xfade0c02U);
    expack_store_u32_be(signature + superblob_header_size + 4U, code_directory_size);
    expack_store_u32_be(signature + superblob_header_size + 8U, 0x20400U);
    expack_store_u32_be(signature + superblob_header_size + 12U, 0x2U);
    expack_store_u32_be(signature + superblob_header_size + 16U, hash_offset);
    expack_store_u32_be(signature + superblob_header_size + 20U, code_directory_fixed_size);
    expack_store_u32_be(signature + superblob_header_size + 24U, 0U);
    expack_store_u32_be(signature + superblob_header_size + 28U, code_slots);
    expack_store_u32_be(signature + superblob_header_size + 32U, (unsigned int)code_limit);
    signature[superblob_header_size + 36U] = CRYPTO_SHA256_DIGEST_SIZE;
    signature[superblob_header_size + 37U] = 2U;
    signature[superblob_header_size + 39U] = 14U;
    memcpy(signature + superblob_header_size + code_directory_fixed_size, EXPACK_MACHO_CODE_DIRECTORY_IDENT, EXPACK_MACHO_CODE_DIRECTORY_IDENT_SIZE);
    memcpy(signature + superblob_header_size + hash_offset, hasher.hashes, (size_t)code_slots * CRYPTO_SHA256_DIGEST_SIZE);
    rt_free(hasher.hashes);
    *signature_size_out = signature_size;
    return signature;
}

static void expack_write_macho_container_metadata(unsigned char *metadata, const ExpackCandidate *candidate, size_t original_size) {
    memset(metadata, 0, EXPACK_MACHO_CONTAINER_METADATA_SIZE);
    memcpy(metadata, "EXPACKM1", 8U);
    archive_store_u32_le(metadata + 8U, EXPACK_MACHO_CONTAINER_VERSION);
    archive_store_u32_le(metadata + 12U, candidate->codec);
    archive_store_u64_le(metadata + 16U, (unsigned long long)original_size);
    archive_store_u64_le(metadata + 24U, (unsigned long long)candidate->payload_size);
    archive_store_u32_le(metadata + 32U, candidate->codec == EXPACK_CODEC_LZSS ? candidate->lzss_profile->profile_id : 0xffffffffU);
}

static int expack_write_macho_container(const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size) {
    enum { header_size = 32U, pagezero_command_size = 72U, text_command_size = 72U + 80U + 80U, linkedit_command_size = 72U, dylinker_command_size = 32U, build_version_command_size = 32U, main_command_size = 24U, code_signature_command_size = 16U };
    unsigned char header[header_size + pagezero_command_size + text_command_size + linkedit_command_size + dylinker_command_size + build_version_command_size + main_command_size + code_signature_command_size];
    static const unsigned char zero_pad[64] = {0U};
    unsigned char metadata[EXPACK_MACHO_CONTAINER_METADATA_SIZE];
    unsigned char *signature;
    unsigned char *patched_stub;
    const unsigned char *stub;
    size_t stub_size;
    size_t signature_size;
    unsigned int subtype;
    unsigned int commands_size = pagezero_command_size + text_command_size + linkedit_command_size + dylinker_command_size + build_version_command_size + main_command_size + code_signature_command_size;
    unsigned int code_offset = header_size + commands_size;
    unsigned int payload_offset;
    unsigned long long payload_size = (unsigned long long)EXPACK_MACHO_CONTAINER_METADATA_SIZE + (unsigned long long)candidate->payload_size;
    unsigned long long file_size;
    unsigned long long text_file_size;
    int output_fd;

    if (expack_macho_container_subtype(format->macho_cputype, &subtype) != 0) {
        return -1;
    }
    stub = expack_macho_container_stub(format->macho_cputype, candidate->codec, &stub_size);
    if (stub == 0 || stub_size == 0U || stub_size > (size_t)((unsigned int)-1 - code_offset)) {
        return -1;
    }
    patched_stub = (unsigned char *)rt_malloc(stub_size);
    if (patched_stub == 0) {
        return -1;
    }
    memcpy(patched_stub, stub, stub_size);
    stub = patched_stub;
    payload_offset = code_offset + (unsigned int)stub_size;
    file_size = (unsigned long long)payload_offset + payload_size;
    text_file_size = expack_align_up_u64(file_size, 0x4000ULL);

    memset(header, 0, sizeof(header));
    archive_store_u32_le(header + 0U, 0xfeedfacfU);
    archive_store_u32_le(header + 4U, format->macho_cputype);
    archive_store_u32_le(header + 8U, subtype);
    archive_store_u32_le(header + 12U, EXPACK_MACHO_TYPE_EXECUTE);
    archive_store_u32_le(header + 16U, 7U);
    archive_store_u32_le(header + 20U, commands_size);
    archive_store_u32_le(header + 24U, EXPACK_MACHO_FLAG_NOUNDEFS | EXPACK_MACHO_FLAG_DYLDLINK | EXPACK_MACHO_FLAG_TWOLEVEL | EXPACK_MACHO_FLAG_PIE);

    expack_write_macho_segment64(header + 32U, "__PAGEZERO", 0ULL, EXPACK_MACHO_CONTAINER_BASE, 0ULL, 0ULL, 0U, 0U, 0U, 0U);
    expack_write_macho_segment64(header + 104U, "__TEXT", EXPACK_MACHO_CONTAINER_BASE, text_file_size, 0ULL, text_file_size, EXPACK_MACHO_VM_PROT_READ | EXPACK_MACHO_VM_PROT_EXECUTE, EXPACK_MACHO_VM_PROT_READ | EXPACK_MACHO_VM_PROT_EXECUTE, 2U, 0U);
    expack_write_macho_section(header + 176U, "__text", "__TEXT", EXPACK_MACHO_CONTAINER_BASE + (unsigned long long)code_offset, (unsigned long long)stub_size, code_offset, 2U, 0x80000400U);
    expack_write_macho_section(header + 256U, "__expack", "__TEXT", EXPACK_MACHO_CONTAINER_BASE + (unsigned long long)payload_offset, payload_size, payload_offset, 3U, 0U);
    expack_write_macho_segment64(header + 336U, "__LINKEDIT", EXPACK_MACHO_CONTAINER_BASE + text_file_size, 0x4000ULL, text_file_size, 0ULL, EXPACK_MACHO_VM_PROT_READ, EXPACK_MACHO_VM_PROT_READ, 0U, 0U);

    archive_store_u32_le(header + 408U, EXPACK_MACHO_LC_LOAD_DYLINKER);
    archive_store_u32_le(header + 412U, dylinker_command_size);
    archive_store_u32_le(header + 416U, 12U);
    memcpy(header + 420U, "/usr/lib/dyld", 14U);

    archive_store_u32_le(header + 440U, EXPACK_MACHO_LC_BUILD_VERSION);
    archive_store_u32_le(header + 444U, build_version_command_size);
    archive_store_u32_le(header + 448U, 1U);
    archive_store_u32_le(header + 452U, 0x000b0000U);
    archive_store_u32_le(header + 456U, 0x000b0000U);
    archive_store_u32_le(header + 460U, 1U);
    archive_store_u32_le(header + 464U, 3U);

    archive_store_u32_le(header + 472U, EXPACK_MACHO_LC_MAIN);
    archive_store_u32_le(header + 476U, main_command_size);
    archive_store_u64_le(header + 480U, code_offset);

    archive_store_u32_le(header + 496U, EXPACK_MACHO_LC_CODE_SIGNATURE);
    archive_store_u32_le(header + 500U, code_signature_command_size);
    archive_store_u32_le(header + 504U, (unsigned int)text_file_size);

    expack_write_macho_container_metadata(metadata, candidate, original_size);
    if (format->macho_cputype == EXPACK_MACHO_CPU_ARM64) {
        if (candidate->codec == EXPACK_CODEC_RAW) {
            if (EXPACK_MACHO_ARM64_RUNNER_PAYLOAD_SIZE_OFFSET + 8U > stub_size) {
                rt_free(patched_stub);
                return -1;
            }
            archive_store_u64_le(patched_stub + EXPACK_MACHO_ARM64_RUNNER_PAYLOAD_ADDRESS_OFFSET, (unsigned long long)(stub_size + EXPACK_MACHO_CONTAINER_METADATA_SIZE - EXPACK_MACHO_ARM64_RUNNER_PAYLOAD_ADDRESS_OFFSET));
            archive_store_u64_le(patched_stub + EXPACK_MACHO_ARM64_RUNNER_PAYLOAD_SIZE_OFFSET, (unsigned long long)candidate->payload_size);
        } else if (candidate->codec == EXPACK_CODEC_LZREP) {
            if (EXPACK_MACHO_ARM64_LZREP_RUNNER_ORIGINAL_SIZE_OFFSET + 8U > stub_size) {
                rt_free(patched_stub);
                return -1;
            }
            archive_store_u64_le(patched_stub + EXPACK_MACHO_ARM64_LZREP_RUNNER_PAYLOAD_ADDRESS_OFFSET, (unsigned long long)(stub_size + EXPACK_MACHO_CONTAINER_METADATA_SIZE - EXPACK_MACHO_ARM64_LZREP_RUNNER_PAYLOAD_ADDRESS_OFFSET));
            archive_store_u64_le(patched_stub + EXPACK_MACHO_ARM64_LZREP_RUNNER_PAYLOAD_SIZE_OFFSET, (unsigned long long)candidate->payload_size);
            archive_store_u64_le(patched_stub + EXPACK_MACHO_ARM64_LZREP_RUNNER_ORIGINAL_SIZE_OFFSET, (unsigned long long)original_size);
        } else {
            rt_free(patched_stub);
            return -1;
        }
    }
    signature = expack_make_macho_code_signature(header, sizeof(header), stub, stub_size, metadata, sizeof(metadata), candidate->payload, candidate->payload_size, text_file_size, &signature_size);
    if (signature == 0 || signature_size > (size_t)((unsigned int)-1)) {
        rt_free(patched_stub);
        return -1;
    }
    archive_store_u32_le(header + 384U, (unsigned int)signature_size);
    archive_store_u32_le(header + 508U, (unsigned int)signature_size);
    rt_free(signature);
    signature = expack_make_macho_code_signature(header, sizeof(header), stub, stub_size, metadata, sizeof(metadata), candidate->payload, candidate->payload_size, text_file_size, &signature_size);
    if (signature == 0 || signature_size > (size_t)((unsigned int)-1)) {
        rt_free(patched_stub);
        return -1;
    }

    output_fd = platform_open_write(output_path, 0755U);
    if (output_fd < 0) {
        rt_free(signature);
        rt_free(patched_stub);
        return -1;
    }
    if (rt_write_all(output_fd, header, sizeof(header)) != 0 || rt_write_all(output_fd, stub, stub_size) != 0 ||
        rt_write_all(output_fd, metadata, sizeof(metadata)) != 0 || rt_write_all(output_fd, candidate->payload, candidate->payload_size) != 0) {
        platform_close(output_fd);
        rt_free(signature);
        rt_free(patched_stub);
        return -1;
    }
    while (file_size < text_file_size) {
        size_t chunk = (size_t)(text_file_size - file_size);
        if (chunk > sizeof(zero_pad)) {
            chunk = sizeof(zero_pad);
        }
        if (rt_write_all(output_fd, zero_pad, chunk) != 0) {
            platform_close(output_fd);
            rt_free(signature);
            rt_free(patched_stub);
            return -1;
        }
        file_size += (unsigned long long)chunk;
    }
    if (rt_write_all(output_fd, signature, signature_size) != 0) {
        platform_close(output_fd);
        rt_free(signature);
        rt_free(patched_stub);
        return -1;
    }
    rt_free(signature);
    rt_free(patched_stub);
    if (platform_close(output_fd) != 0) {
        return -1;
    }
    return 0;
}

static int expack_write_packed_output(const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size) {
    if (format->kind == EXPACK_FORMAT_ELF64_X86_64) {
        return expack_write_packed_elf(output_path, candidate, original_size);
    }
    (void)output_path;
    (void)candidate;
    (void)original_size;
    return -1;
}

static void expack_write_lzss_profile_name(unsigned int profile_id) {
    if (profile_id == COMPRESSION_LZSS_PROFILE_WIDE_WINDOW) {
        rt_write_cstr(1, "wide-window");
    } else if (profile_id == COMPRESSION_LZSS_PROFILE_WIDE_MATCH) {
        rt_write_cstr(1, "wide-match");
    } else if (profile_id == COMPRESSION_LZSS_PROFILE_MEDIUM_MATCH) {
        rt_write_cstr(1, "medium-match");
    } else if (profile_id == COMPRESSION_LZSS_PROFILE_LONG_MATCH) {
        rt_write_cstr(1, "long-match");
    } else {
        rt_write_cstr(1, "unknown");
    }
}

static void expack_write_candidate_name(const ExpackCandidate *candidate) {
    if (candidate->codec == EXPACK_CODEC_LZSS) {
        rt_write_cstr(1, "lzss/");
        expack_write_lzss_profile_name(candidate->lzss_profile->profile_id);
    } else if (candidate->codec == EXPACK_CODEC_ZERO_RUN) {
        rt_write_cstr(1, "zero-run");
    } else if (candidate->codec == EXPACK_CODEC_BYTE_RUN) {
        rt_write_cstr(1, "byte-run");
    } else if (candidate->codec == EXPACK_CODEC_LZREP) {
        rt_write_cstr(1, "lzrep");
    } else if (candidate->codec == EXPACK_CODEC_LZSS_BCJ) {
        rt_write_cstr(1, "lzss-bcj/wide-window");
    } else if (candidate->codec == EXPACK_CODEC_RAW) {
        rt_write_cstr(1, "raw");
    } else {
        rt_write_cstr(1, "unknown");
    }
}

static void expack_write_summary(size_t input_size, size_t image_size, int image_changed, const ExpackCandidate *candidate) {
    rt_write_cstr(1, "expack: input ");
    rt_write_uint(1, (unsigned long long)input_size);
    if (image_changed) {
        rt_write_cstr(1, " bytes, exec image ");
        rt_write_uint(1, (unsigned long long)image_size);
    }
    rt_write_cstr(1, " bytes, payload ");
    rt_write_uint(1, (unsigned long long)candidate->payload_size);
    rt_write_cstr(1, " bytes, codec ");
    expack_write_candidate_name(candidate);
    rt_write_cstr(1, ", packed ");
    rt_write_uint(1, candidate->packed_size);
    rt_write_cstr(1, " bytes\n");
}

static void expack_write_analyze_header(const ExpackInputFormat *format, size_t input_size, size_t image_size, int image_changed) {
    rt_write_cstr(1, "expack analyze: input ");
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
    if (format->kind == EXPACK_FORMAT_MACHO && format->macho_code_signature_size != 0U) {
        rt_write_cstr(1, ", code signature ");
        rt_write_uint(1, (unsigned long long)format->macho_code_signature_size);
        rt_write_cstr(1, " bytes at ");
        rt_write_uint(1, (unsigned long long)format->macho_code_signature_offset);
    }
    rt_write_cstr(1, "\n");
}

static void expack_write_unsupported_output_error(const ExpackInputFormat *format) {
    if (format->kind == EXPACK_FORMAT_MACHO) {
        tool_write_error(EXPACK_TOOL_NAME, "Mach-O compression analysis is supported, but writing compressed runnable Mach-O output needs a native decoder backend", 0);
    } else {
        tool_write_error(EXPACK_TOOL_NAME, "cannot write packed output for this executable format", 0);
    }
}

int main(int argc, char **argv) {
    const char *input_path = 0;
    const char *output_path = 0;
    const char *error_message = 0;
    unsigned char *input_data = 0;
    ExpackImage image;
    ExpackCandidate selected;
    ExpackInputFormat input_format;
    size_t input_size = 0U;
    int analyze = 0;
    int macho_container = 0;
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
    input_format.can_write_packed_output = 0;
    input_format.macho_cputype = 0U;
    input_format.macho_code_signature_offset = 0U;
    input_format.macho_code_signature_size = 0U;
    image.data = 0;
    image.size = 0U;
    image.changed = 0;

    tool_opt_init(&options, argc, argv, EXPACK_TOOL_NAME, "[-q] INPUT OUTPUT\n       expack --analyze INPUT\n       expack --macho-container INPUT OUTPUT");
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
    if ((analyze && macho_container) || (!analyze && argc - options.argi != 2) || (analyze && argc - options.argi != 1)) {
        tool_write_usage(EXPACK_TOOL_NAME, options.usage_suffix);
        return 1;
    }

    input_path = argv[options.argi];
    output_path = analyze ? 0 : argv[options.argi + 1];
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
    if (analyze) {
        expack_write_analyze_header(&input_format, input_size, image.size, image.changed);
    }
    if (expack_select_best_payload(image.data, image.size, &selected, analyze, input_format.allow_x86_bcj) != 0) {
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
    if (macho_container) {
        if (input_format.kind != EXPACK_FORMAT_MACHO) {
            tool_write_error(EXPACK_TOOL_NAME, "--macho-container requires a Mach-O input", 0);
            expack_candidate_release(&selected);
            expack_release_exec_image(&image, input_data);
            rt_free(input_data);
            return 1;
        }
        if (input_format.macho_cputype == EXPACK_MACHO_CPU_ARM64 && selected.codec != EXPACK_CODEC_LZREP) {
            if (expack_make_raw_candidate(image.data, image.size, &selected) != 0) {
                tool_write_error(EXPACK_TOOL_NAME, "cannot prepare raw Mach-O runner payload", 0);
                expack_candidate_release(&selected);
                expack_release_exec_image(&image, input_data);
                rt_free(input_data);
                return 1;
            }
        }
        if (expack_write_macho_container(&input_format, output_path, &selected, image.size) != 0) {
            tool_write_error(EXPACK_TOOL_NAME, "cannot write Mach-O prototype container: ", output_path);
            expack_candidate_release(&selected);
            expack_release_exec_image(&image, input_data);
            rt_free(input_data);
            return 1;
        }
        if (!quiet) {
            rt_write_cstr(1, "expack: wrote Mach-O prototype container ");
            rt_write_cstr(1, output_path);
            rt_write_cstr(1, " with codec ");
            expack_write_candidate_name(&selected);
            rt_write_cstr(1, "\n");
        }
        expack_candidate_release(&selected);
        expack_release_exec_image(&image, input_data);
        rt_free(input_data);
        return 0;
    }
    if (!input_format.can_write_packed_output) {
        expack_write_unsupported_output_error(&input_format);
        expack_candidate_release(&selected);
        expack_release_exec_image(&image, input_data);
        rt_free(input_data);
        return 1;
    }
    if (expack_write_packed_output(&input_format, output_path, &selected, image.size) != 0) {
        tool_write_error(EXPACK_TOOL_NAME, "cannot write packed output: ", output_path);
        expack_candidate_release(&selected);
        expack_release_exec_image(&image, input_data);
        rt_free(input_data);
        return 1;
    }
    if (!quiet) {
        expack_write_summary(input_size, image.size, image.changed, &selected);
    }

    expack_candidate_release(&selected);
    expack_release_exec_image(&image, input_data);
    rt_free(input_data);
    return 0;
}
