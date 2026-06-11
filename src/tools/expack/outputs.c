#include "internal.h"

#include "outputs_elf.c"
#include "outputs_pe.c"

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

typedef struct {
    const unsigned char *stub;
    size_t stub_size;
    unsigned int payload_address_offset;
    unsigned int payload_size_offset;
    unsigned int original_size_offset;
} ExpackMachoRunnerDescriptor;

static int expack_macho_container_runner(unsigned int cputype, unsigned int codec, ExpackMachoRunnerDescriptor *runner_out) {
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
#include "macho_arm64_lzrep_runner.inc"
    };
    static const unsigned char arm64_lzss_runner_stub[] = {
#include "macho_arm64_lzss_runner.inc"
    };
    static const unsigned char arm64_lz4_runner_stub[] = {
#include "macho_arm64_lz4_runner.inc"
    };
    static const unsigned char x86_64_exit_stub[] = {
        0xb8, 0x01, 0x00, 0x00, 0x02,
        0xbf, 0x7f, 0x00, 0x00, 0x00,
        0x0f, 0x05
    };

    runner_out->stub = 0;
    runner_out->stub_size = 0U;
    runner_out->payload_address_offset = (unsigned int)-1;
    runner_out->payload_size_offset = (unsigned int)-1;
    runner_out->original_size_offset = (unsigned int)-1;
    if (cputype == EXPACK_MACHO_CPU_ARM64 && codec == EXPACK_CODEC_RAW) {
        runner_out->stub = arm64_raw_runner_stub;
        runner_out->stub_size = sizeof(arm64_raw_runner_stub);
        runner_out->payload_address_offset = EXPACK_MACHO_ARM64_RUNNER_PAYLOAD_ADDRESS_OFFSET;
        runner_out->payload_size_offset = EXPACK_MACHO_ARM64_RUNNER_PAYLOAD_SIZE_OFFSET;
        return 0;
    }
    if (cputype == EXPACK_MACHO_CPU_ARM64 && codec == EXPACK_CODEC_LZREP) {
        runner_out->stub = arm64_lzrep_runner_stub;
        runner_out->stub_size = sizeof(arm64_lzrep_runner_stub);
        runner_out->payload_address_offset = EXPACK_MACHO_ARM64_LZREP_RUNNER_PAYLOAD_ADDRESS_OFFSET;
        runner_out->payload_size_offset = EXPACK_MACHO_ARM64_LZREP_RUNNER_PAYLOAD_SIZE_OFFSET;
        runner_out->original_size_offset = EXPACK_MACHO_ARM64_LZREP_RUNNER_ORIGINAL_SIZE_OFFSET;
        return 0;
    }
    if (cputype == EXPACK_MACHO_CPU_ARM64 && codec == EXPACK_CODEC_LZSS) {
        runner_out->stub = arm64_lzss_runner_stub;
        runner_out->stub_size = sizeof(arm64_lzss_runner_stub);
        runner_out->payload_address_offset = EXPACK_MACHO_ARM64_LZSS_RUNNER_PAYLOAD_ADDRESS_OFFSET;
        runner_out->payload_size_offset = EXPACK_MACHO_ARM64_LZSS_RUNNER_PAYLOAD_SIZE_OFFSET;
        runner_out->original_size_offset = EXPACK_MACHO_ARM64_LZSS_RUNNER_ORIGINAL_SIZE_OFFSET;
        return 0;
    }
    if (cputype == EXPACK_MACHO_CPU_ARM64 && codec == EXPACK_CODEC_LZ4) {
        runner_out->stub = arm64_lz4_runner_stub;
        runner_out->stub_size = sizeof(arm64_lz4_runner_stub);
        runner_out->payload_address_offset = EXPACK_MACHO_ARM64_LZ4_RUNNER_PAYLOAD_ADDRESS_OFFSET;
        runner_out->payload_size_offset = EXPACK_MACHO_ARM64_LZ4_RUNNER_PAYLOAD_SIZE_OFFSET;
        runner_out->original_size_offset = EXPACK_MACHO_ARM64_LZ4_RUNNER_ORIGINAL_SIZE_OFFSET;
        return 0;
    }
    if (cputype == EXPACK_MACHO_CPU_X86_64) {
        runner_out->stub = x86_64_exit_stub;
        runner_out->stub_size = sizeof(x86_64_exit_stub);
        return 0;
    }
    return -1;
}

static int expack_patch_macho_runner(const ExpackMachoRunnerDescriptor *runner, unsigned char *stub, size_t original_size, const ExpackCandidate *candidate) {
    if (runner->payload_address_offset != (unsigned int)-1) {
        if (runner->payload_address_offset + 8U > runner->stub_size || runner->payload_size_offset + 8U > runner->stub_size) {
            return -1;
        }
        archive_store_u64_le(stub + runner->payload_address_offset, (unsigned long long)(runner->stub_size + EXPACK_MACHO_CONTAINER_METADATA_SIZE - runner->payload_address_offset));
        archive_store_u64_le(stub + runner->payload_size_offset, (unsigned long long)candidate->payload_size);
    }
    if (runner->original_size_offset != (unsigned int)-1) {
        if (runner->original_size_offset + 8U > runner->stub_size) {
            return -1;
        }
        archive_store_u64_le(stub + runner->original_size_offset, (unsigned long long)original_size);
    }
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
    ExpackMachoRunnerDescriptor runner;
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

    if (expack_macho_container_subtype(format->info.macho.cputype, &subtype) != 0) {
        return -1;
    }
    if (expack_macho_container_runner(format->info.macho.cputype, candidate->codec, &runner) != 0) {
        return -1;
    }
    stub = runner.stub;
    stub_size = runner.stub_size;
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
    archive_store_u32_le(header + 4U, format->info.macho.cputype);
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
    if (expack_patch_macho_runner(&runner, patched_stub, original_size, candidate) != 0) {
        rt_free(patched_stub);
        return -1;
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

static int expack_backend_can_write_packed(const ExpackInputFormat *format) {
    (void)format;
    return 1;
}

static int expack_backend_cannot_write_packed(const ExpackInputFormat *format) {
    (void)format;
    return 0;
}

static int expack_backend_cannot_write_container(const ExpackInputFormat *format, unsigned int output_kind) {
    (void)format;
    (void)output_kind;
    return 0;
}

static unsigned long long expack_elf_score_candidate(const ExpackInputFormat *format, const ExpackCandidate *candidate) {
    (void)format;
    return (unsigned long long)EXPACK_ELF_CODE_OFFSET + (unsigned long long)candidate->stub_size + (unsigned long long)candidate->payload_size;
}

static unsigned long long expack_macho_score_candidate(const ExpackInputFormat *format, const ExpackCandidate *candidate) {
    enum { header_size = 32U, pagezero_command_size = 72U, text_command_size = 72U + 80U + 80U, linkedit_command_size = 72U, dylinker_command_size = 32U, build_version_command_size = 32U, main_command_size = 24U, code_signature_command_size = 16U };
    ExpackMachoRunnerDescriptor runner;
    unsigned int commands_size = pagezero_command_size + text_command_size + linkedit_command_size + dylinker_command_size + build_version_command_size + main_command_size + code_signature_command_size;
    unsigned int code_offset = header_size + commands_size;
    unsigned long long payload_size;
    unsigned long long file_size;
    unsigned long long text_file_size;

    if (expack_macho_container_runner(format->info.macho.cputype, candidate->codec, &runner) != 0 || runner.stub == 0 || runner.stub_size == 0U) {
        return EXPACK_CANDIDATE_UNSUPPORTED_SIZE;
    }
    payload_size = EXPACK_MACHO_CONTAINER_METADATA_SIZE + (unsigned long long)candidate->payload_size;
    file_size = (unsigned long long)code_offset + (unsigned long long)runner.stub_size + payload_size;
    text_file_size = expack_align_up_u64(file_size, 0x4000ULL);
    return text_file_size;
}

static unsigned long long expack_pe_score_candidate(const ExpackInputFormat *format, const ExpackCandidate *candidate) {
    unsigned long long runner_size;
    (void)format;
    if (candidate->codec == EXPACK_CODEC_RAW) {
        return EXPACK_CANDIDATE_UNSUPPORTED_SIZE;
    }
    if ((candidate->codec == EXPACK_CODEC_LZSS || candidate->codec == EXPACK_CODEC_LZSS_BCJ) &&
        (candidate->lzss_profile == 0 || candidate->lzss_profile->profile_id != COMPRESSION_LZSS_PROFILE_WIDE_WINDOW)) {
        return EXPACK_CANDIDATE_UNSUPPORTED_SIZE;
    }
    runner_size = expack_pe_runner_size_for_codec(candidate->codec);
    if (runner_size == EXPACK_CANDIDATE_UNSUPPORTED_SIZE) {
        return EXPACK_CANDIDATE_UNSUPPORTED_SIZE;
    }
    return runner_size + EXPACK_PE_CONTAINER_METADATA_SIZE + (unsigned long long)candidate->payload_size;
}

static int expack_elf_write_packed(const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size) {
    (void)format;
    return expack_write_packed_elf(output_path, candidate, original_size);
}

static int expack_macho_can_write_container(const ExpackInputFormat *format, unsigned int output_kind) {
    return format->kind == EXPACK_FORMAT_MACHO && output_kind == EXPACK_OUTPUT_KIND_MACHO_CONTAINER;
}

static int expack_macho_prepare_container_candidate(const ExpackInputFormat *format, const ExpackImage *image, ExpackCandidate *candidate) {
    ExpackMachoRunnerDescriptor runner;

    if (format->info.macho.cputype == EXPACK_MACHO_CPU_ARM64 &&
        expack_macho_container_runner(format->info.macho.cputype, candidate->codec, &runner) != 0) {
        if (expack_make_raw_candidate(image->data, image->size, candidate) != 0) {
            return -1;
        }
        candidate->packed_size = expack_macho_score_candidate(format, candidate);
    }
    return 0;
}

static int expack_pe_can_write_container(const ExpackInputFormat *format, unsigned int output_kind) {
    return format->kind == EXPACK_FORMAT_PE_COFF && output_kind == EXPACK_OUTPUT_KIND_PE_CONTAINER;
}

static int expack_pe_prepare_container_candidate(const ExpackInputFormat *format, const ExpackImage *image, ExpackCandidate *candidate) {
    if (candidate->codec == EXPACK_CODEC_LZSS || candidate->codec == EXPACK_CODEC_LZREP || candidate->codec == EXPACK_CODEC_LZSS_BCJ) {
        if (candidate->packed_size < (unsigned long long)image->size) {
            return 0;
        }
        if (expack_make_raw_candidate(image->data, image->size, candidate) != 0) {
            return -1;
        }
        candidate->packed_size = (unsigned long long)image->size;
        return 0;
    }
    if (expack_make_raw_candidate(image->data, image->size, candidate) != 0) {
        return -1;
    }
    (void)format;
    candidate->packed_size = (unsigned long long)image->size;
    return 0;
}

static void expack_macho_write_container_success(const char *output_path, const ExpackCandidate *candidate) {
    rt_write_cstr(1, "expack: wrote Mach-O prototype container ");
    rt_write_cstr(1, output_path);
    rt_write_cstr(1, " with codec ");
    expack_write_candidate_name(candidate);
    rt_write_cstr(1, "\n");
}

static void expack_pe_write_container_success(const char *output_path, const ExpackCandidate *candidate) {
    if (candidate->codec == EXPACK_CODEC_RAW) {
        rt_write_cstr(1, "expack: wrote PE/COFF exact executable copy ");
    } else {
        rt_write_cstr(1, "expack: wrote PE/COFF self-extracting container ");
    }
    rt_write_cstr(1, output_path);
    rt_write_cstr(1, " with codec ");
    expack_write_candidate_name(candidate);
    rt_write_cstr(1, "\n");
}

static const ExpackOutputBackend *expack_find_output_backend(const ExpackInputFormat *format) {
    static ExpackOutputBackend backend;

    memset(&backend, 0, sizeof(backend));
    if (format->kind == EXPACK_FORMAT_ELF64_X86_64) {
        backend.format_kind = EXPACK_FORMAT_ELF64_X86_64;
        backend.default_output_kind = EXPACK_OUTPUT_KIND_ELF_PACKED;
        backend.packed_unsupported_message = "cannot write packed output for this executable format";
        backend.score_candidate = expack_elf_score_candidate;
        backend.can_write_packed = expack_backend_can_write_packed;
        backend.can_write_container = expack_backend_cannot_write_container;
        backend.write_packed = expack_elf_write_packed;
        return &backend;
    }
    if (format->kind == EXPACK_FORMAT_MACHO) {
        backend.format_kind = EXPACK_FORMAT_MACHO;
        backend.default_output_kind = EXPACK_OUTPUT_KIND_MACHO_CONTAINER;
        backend.packed_unsupported_message = "Mach-O compression analysis is supported, but writing compressed runnable Mach-O output needs a native decoder backend";
        backend.score_candidate = expack_macho_score_candidate;
        backend.can_write_packed = expack_backend_cannot_write_packed;
        backend.can_write_container = expack_macho_can_write_container;
        backend.prepare_container_candidate = expack_macho_prepare_container_candidate;
        backend.write_container = expack_write_macho_container;
        backend.write_container_success = expack_macho_write_container_success;
        return &backend;
    }
    if (format->kind == EXPACK_FORMAT_PE_COFF) {
        backend.format_kind = EXPACK_FORMAT_PE_COFF;
        backend.default_output_kind = EXPACK_OUTPUT_KIND_PE_CONTAINER;
        backend.packed_unsupported_message = "PE/COFF compression analysis is supported, but writing packed output needs a PE/COFF container backend";
        backend.score_candidate = expack_pe_score_candidate;
        backend.can_write_packed = expack_backend_cannot_write_packed;
        backend.can_write_container = expack_pe_can_write_container;
        backend.prepare_container_candidate = expack_pe_prepare_container_candidate;
        backend.write_container = expack_write_pe_container;
        backend.write_container_success = expack_pe_write_container_success;
        return &backend;
    }
    return 0;
}

static int expack_write_packed_output(const ExpackOutputBackend *backend, const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size) {
    if (backend == 0 || backend->write_packed == 0) {
        return -1;
    }
    return backend->write_packed(format, output_path, candidate, original_size);
}

static int expack_write_container_output(const ExpackOutputBackend *backend, const ExpackInputFormat *format, const char *output_path, const ExpackCandidate *candidate, size_t original_size, unsigned int output_kind) {
    if (backend == 0 || backend->write_container == 0 || backend->can_write_container == 0 || !backend->can_write_container(format, output_kind)) {
        return -1;
    }
    return backend->write_container(format, output_path, candidate, original_size);
}
