#include "archive_util.h"
#include "compression/lzss.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define EXPACK_TOOL_NAME "expack"
#define EXPACK_ELF_BASE_VADDR 0x400000ULL
#define EXPACK_ELF_HEADER_SIZE 64U
#define EXPACK_ELF_PHDR_SIZE 56U
#define EXPACK_ELF_CODE_OFFSET (EXPACK_ELF_HEADER_SIZE + EXPACK_ELF_PHDR_SIZE)
#define EXPACK_ZERO_RUN_MIN 8U
#define EXPACK_LITERAL_MAX 65536U
#define EXPACK_CODEC_LZSS 0U
#define EXPACK_CODEC_ZERO_RUN 1U
#define EXPACK_LZSS_STUB_LENGTH_SHIFT_OFFSET 177U
#define EXPACK_LZSS_STUB_LENGTH_MASK_OFFSET 187U
#define EXPACK_LZSS_ORIGINAL_SIZE_OFFSET 368U
#define EXPACK_LZSS_PAYLOAD_SIZE_OFFSET 376U
#define EXPACK_ZERO_ORIGINAL_SIZE_OFFSET 274U
#define EXPACK_ZERO_PAYLOAD_SIZE_OFFSET 282U

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

static unsigned short expack_read_u16_le(const unsigned char *bytes) {
    return (unsigned short)bytes[0] | (unsigned short)((unsigned short)bytes[1] << 8);
}

static void expack_store_u16_le(unsigned char *bytes, unsigned short value) {
    bytes[0] = (unsigned char)(value & 0xffU);
    bytes[1] = (unsigned char)((value >> 8) & 0xffU);
}

static unsigned long long expack_zero_run_length(const unsigned char *data, size_t size, size_t offset) {
    size_t position = offset;

    while (position < size && data[position] == 0U) {
        position += 1U;
    }
    return (unsigned long long)(position - offset);
}

static int expack_should_start_zero_record(const unsigned char *data, size_t size, size_t offset) {
    return expack_zero_run_length(data, size, offset) >= EXPACK_ZERO_RUN_MIN;
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

static int expack_validate_input_elf(const unsigned char *data, size_t size, const char **message_out) {
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
        *message_out = "only x86-64 ELF inputs are supported by this first stub";
        return -1;
    }

    return 0;
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

static int expack_select_best_payload(const unsigned char *input_data, size_t input_size, ExpackCandidate *selected_out) {
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

static int expack_write_packed_elf(const char *output_path, const ExpackCandidate *candidate, size_t original_size) {
    unsigned char header[EXPACK_ELF_CODE_OFFSET];
    unsigned char stub[sizeof(expack_stub_x86_64) > sizeof(expack_zero_stub_x86_64) ? sizeof(expack_stub_x86_64) : sizeof(expack_zero_stub_x86_64)];
    unsigned long long file_size = (unsigned long long)sizeof(header) + (unsigned long long)candidate->stub_size + (unsigned long long)candidate->payload_size;
    int output_fd;

    expack_write_elf_header(header, file_size);
    memcpy(stub, candidate->stub, candidate->stub_size);
    if (candidate->codec == EXPACK_CODEC_LZSS) {
        stub[EXPACK_LZSS_STUB_LENGTH_SHIFT_OFFSET] = candidate->lzss_profile->length_shift;
        stub[EXPACK_LZSS_STUB_LENGTH_MASK_OFFSET] = candidate->lzss_profile->length_mask;
        archive_store_u64_le(stub + EXPACK_LZSS_ORIGINAL_SIZE_OFFSET, (unsigned long long)original_size);
        archive_store_u64_le(stub + EXPACK_LZSS_PAYLOAD_SIZE_OFFSET, (unsigned long long)candidate->payload_size);
    } else if (candidate->codec == EXPACK_CODEC_ZERO_RUN) {
        archive_store_u64_le(stub + EXPACK_ZERO_ORIGINAL_SIZE_OFFSET, (unsigned long long)original_size);
        archive_store_u64_le(stub + EXPACK_ZERO_PAYLOAD_SIZE_OFFSET, (unsigned long long)candidate->payload_size);
    } else {
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
    } else {
        rt_write_cstr(1, "unknown");
    }
}

static void expack_write_summary(size_t input_size, const ExpackCandidate *candidate) {
    rt_write_cstr(1, "expack: input ");
    rt_write_uint(1, (unsigned long long)input_size);
    rt_write_cstr(1, " bytes, payload ");
    rt_write_uint(1, (unsigned long long)candidate->payload_size);
    rt_write_cstr(1, " bytes, codec ");
    expack_write_candidate_name(candidate);
    rt_write_cstr(1, ", packed ");
    rt_write_uint(1, candidate->packed_size);
    rt_write_cstr(1, " bytes\n");
}

int main(int argc, char **argv) {
    const char *input_path = 0;
    const char *output_path = 0;
    const char *error_message = 0;
    unsigned char *input_data = 0;
    ExpackCandidate selected;
    size_t input_size = 0U;
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

    tool_opt_init(&options, argc, argv, EXPACK_TOOL_NAME, "[-q] INPUT OUTPUT");
    while ((opt_result = tool_opt_next(&options)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(options.flag, "-q") == 0 || rt_strcmp(options.flag, "--quiet") == 0) {
            quiet = 1;
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
    if (argc - options.argi != 2) {
        tool_write_usage(EXPACK_TOOL_NAME, options.usage_suffix);
        return 1;
    }

    input_path = argv[options.argi];
    output_path = argv[options.argi + 1];
    if (expack_load_file(input_path, &input_data, &input_size) != 0) {
        tool_write_error(EXPACK_TOOL_NAME, "cannot read input: ", input_path);
        return 1;
    }
    if (expack_validate_input_elf(input_data, input_size, &error_message) != 0) {
        tool_write_error(EXPACK_TOOL_NAME, error_message, 0);
        rt_free(input_data);
        return 1;
    }

    if (expack_select_best_payload(input_data, input_size, &selected) != 0) {
        tool_write_error(EXPACK_TOOL_NAME, "compression failed", 0);
        rt_free(input_data);
        return 1;
    }
    if (expack_write_packed_elf(output_path, &selected, input_size) != 0) {
        tool_write_error(EXPACK_TOOL_NAME, "cannot write packed output: ", output_path);
        expack_candidate_release(&selected);
        rt_free(input_data);
        return 1;
    }
    if (!quiet) {
        expack_write_summary(input_size, &selected);
    }

    expack_candidate_release(&selected);
    rt_free(input_data);
    return 0;
}
