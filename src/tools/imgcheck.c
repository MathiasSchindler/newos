#include "image/image.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "crypto/sha256.h"

#define IMGCHECK_INITIAL_CAPACITY (64U * 1024U)
#define IMGCHECK_ENTRY_CAPACITY 512U
#define IMGCHECK_PATH_CAPACITY 2048U

#define IMGCHECK_MACHO_MAGIC_64 0xfeedfacfU
#define IMGCHECK_MACHO_TYPE_EXECUTE 2U
#define IMGCHECK_MACHO_FLAG_PIE 0x200000U
#define IMGCHECK_MACHO_LC_SEGMENT_64 0x19U
#define IMGCHECK_MACHO_LC_LOAD_DYLIB 0xcU
#define IMGCHECK_MACHO_LC_LOAD_WEAK_DYLIB 0x80000018U
#define IMGCHECK_MACHO_LC_REEXPORT_DYLIB 0x8000001fU
#define IMGCHECK_MACHO_LC_LOAD_UPWARD_DYLIB 0x80000023U
#define IMGCHECK_MACHO_LC_DYLD_INFO 0x22U
#define IMGCHECK_MACHO_LC_DYLD_INFO_ONLY 0x80000022U
#define IMGCHECK_MACHO_LC_CODE_SIGNATURE 0x1dU
#define IMGCHECK_MACHO_LC_MAIN 0x80000028U
#define IMGCHECK_MACHO_LC_DYLD_CHAINED_FIXUPS 0x80000034U
#define IMGCHECK_MACHO_S_ZEROFILL 1U
#define IMGCHECK_MACHO_S_GB_ZEROFILL 12U
#define IMGCHECK_MACHO_S_THREAD_LOCAL_ZEROFILL 18U
#define IMGCHECK_MACHO_CSMAGIC_EMBEDDED_SIGNATURE 0xfade0cc0U
#define IMGCHECK_MACHO_CSMAGIC_CODEDIRECTORY 0xfade0c02U
#define IMGCHECK_MACHO_CSSLOT_CODEDIRECTORY 0U
#define IMGCHECK_MACHO_HASH_SHA256 2U

typedef struct {
    int valid;
    unsigned int warning_count;
    unsigned int code_signature_checked_slots;
    unsigned int code_signature_mismatches;
    size_t failure_offset;
    int has_failure_offset;
    int code_signature_present;
    int code_signature_verified;
    char message[256];
} ImgcheckMachoValidation;

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

static unsigned int imgcheck_read_u32_le(const unsigned char *data) {
    return (unsigned int)data[0] |
           ((unsigned int)data[1] << 8U) |
           ((unsigned int)data[2] << 16U) |
           ((unsigned int)data[3] << 24U);
}

static unsigned long long imgcheck_read_u64_le(const unsigned char *data) {
    return (unsigned long long)imgcheck_read_u32_le(data) |
           ((unsigned long long)imgcheck_read_u32_le(data + 4) << 32U);
}

static unsigned int imgcheck_read_u32_be(const unsigned char *data) {
    return ((unsigned int)data[0] << 24U) |
           ((unsigned int)data[1] << 16U) |
           ((unsigned int)data[2] << 8U) |
           (unsigned int)data[3];
}

static int imgcheck_is_macho64(const unsigned char *data, size_t size) {
    return size >= 32U && imgcheck_read_u32_le(data) == IMGCHECK_MACHO_MAGIC_64;
}

static void imgcheck_macho_fail(ImgcheckMachoValidation *validation, const char *message, size_t offset) {
    validation->valid = 0;
    validation->has_failure_offset = 1;
    validation->failure_offset = offset;
    rt_copy_string(validation->message, sizeof(validation->message), message);
}

static int imgcheck_macho_section_is_zerofill(unsigned int flags) {
    unsigned int type = flags & 0xffU;
    return type == IMGCHECK_MACHO_S_ZEROFILL || type == IMGCHECK_MACHO_S_GB_ZEROFILL || type == IMGCHECK_MACHO_S_THREAD_LOCAL_ZEROFILL;
}

static int imgcheck_macho_command_is_dylib(unsigned int command) {
    return command == IMGCHECK_MACHO_LC_LOAD_DYLIB ||
           command == IMGCHECK_MACHO_LC_LOAD_WEAK_DYLIB ||
           command == IMGCHECK_MACHO_LC_REEXPORT_DYLIB ||
           command == IMGCHECK_MACHO_LC_LOAD_UPWARD_DYLIB;
}

static int imgcheck_bytes_equal(const unsigned char *left, const unsigned char *right, size_t size) {
    size_t index;
    for (index = 0U; index < size; ++index) {
        if (left[index] != right[index]) return 0;
    }
    return 1;
}

static int imgcheck_verify_macho_code_signature(const unsigned char *data, size_t size, unsigned int dataoff, unsigned int datasize, ImgcheckMachoValidation *validation) {
    unsigned int super_magic;
    unsigned int super_length;
    unsigned int super_count;
    unsigned int code_directory_offset = 0U;
    unsigned int index;
    unsigned int code_directory_length;
    unsigned int hash_offset;
    unsigned int n_code_slots;
    unsigned int code_limit;
    unsigned int hash_size;
    unsigned int hash_type;
    unsigned int page_size_log2;

    if ((unsigned long long)dataoff + (unsigned long long)datasize > (unsigned long long)size || datasize < 12U) {
        return -1;
    }
    super_magic = imgcheck_read_u32_be(data + dataoff + 0U);
    super_length = imgcheck_read_u32_be(data + dataoff + 4U);
    super_count = imgcheck_read_u32_be(data + dataoff + 8U);
    if (super_magic != IMGCHECK_MACHO_CSMAGIC_EMBEDDED_SIGNATURE || super_length > datasize || super_length < 12U) {
        return -1;
    }
    for (index = 0U; index < super_count; ++index) {
        unsigned int entry_offset = 12U + index * 8U;
        unsigned int slot_type;
        unsigned int blob_offset;
        if (entry_offset + 8U > super_length) return -1;
        slot_type = imgcheck_read_u32_be(data + dataoff + entry_offset + 0U);
        blob_offset = imgcheck_read_u32_be(data + dataoff + entry_offset + 4U);
        if (slot_type == IMGCHECK_MACHO_CSSLOT_CODEDIRECTORY) {
            code_directory_offset = blob_offset;
            break;
        }
    }
    if (code_directory_offset == 0U || code_directory_offset + 44U > super_length) return -1;
    if (imgcheck_read_u32_be(data + dataoff + code_directory_offset) != IMGCHECK_MACHO_CSMAGIC_CODEDIRECTORY) return -1;
    code_directory_length = imgcheck_read_u32_be(data + dataoff + code_directory_offset + 4U);
    hash_offset = imgcheck_read_u32_be(data + dataoff + code_directory_offset + 16U);
    n_code_slots = imgcheck_read_u32_be(data + dataoff + code_directory_offset + 28U);
    code_limit = imgcheck_read_u32_be(data + dataoff + code_directory_offset + 32U);
    hash_size = (unsigned int)data[dataoff + code_directory_offset + 36U];
    hash_type = (unsigned int)data[dataoff + code_directory_offset + 37U];
    page_size_log2 = (unsigned int)data[dataoff + code_directory_offset + 39U];
    if (code_directory_length == 0U || code_directory_offset + code_directory_length > super_length || hash_type != IMGCHECK_MACHO_HASH_SHA256 || hash_size != CRYPTO_SHA256_DIGEST_SIZE ||
        page_size_log2 >= 31U || code_limit > dataoff || hash_offset + n_code_slots * hash_size > code_directory_length) {
        return -1;
    }
    for (index = 0U; index < n_code_slots; ++index) {
        unsigned long long page_size = 1ULL << page_size_log2;
        unsigned long long page_offset = (unsigned long long)index * page_size;
        unsigned long long remaining;
        CryptoSha256Context sha;
        unsigned char digest[CRYPTO_SHA256_DIGEST_SIZE];
        const unsigned char *expected;
        if (page_offset >= code_limit) return -1;
        remaining = (unsigned long long)code_limit - page_offset;
        if (remaining > page_size) remaining = page_size;
        crypto_sha256_init(&sha);
        crypto_sha256_update(&sha, data + page_offset, (size_t)remaining);
        crypto_sha256_final(&sha, digest);
        expected = data + dataoff + code_directory_offset + hash_offset + index * hash_size;
        validation->code_signature_checked_slots += 1U;
        if (!imgcheck_bytes_equal(expected, digest, sizeof(digest))) {
            validation->code_signature_mismatches += 1U;
        }
    }
    validation->code_signature_verified = validation->code_signature_checked_slots == n_code_slots && validation->code_signature_mismatches == 0U;
    return validation->code_signature_verified ? 0 : -1;
}

static void imgcheck_validate_macho(const unsigned char *data, size_t size, const ImgcheckOptions *options, ImgcheckMachoValidation *validation) {
    unsigned int filetype;
    unsigned int ncmds;
    unsigned int sizeofcmds;
    unsigned int flags;
    unsigned long long command_offset = 32ULL;
    unsigned int command_index;
    int has_main = 0;
    int has_text_segment = 0;
    int has_code_signature = 0;
    int has_dylib_import = 0;
    int has_rebase_metadata = 0;
    int pie_without_rebase = 0;
    int entry_in_file_backed_segment = 0;
    unsigned long long entryoff = 0ULL;

    validation->valid = 1;
    validation->warning_count = 0U;
    validation->code_signature_checked_slots = 0U;
    validation->code_signature_mismatches = 0U;
    validation->failure_offset = 0U;
    validation->has_failure_offset = 0;
    validation->code_signature_present = 0;
    validation->code_signature_verified = 0;
    rt_copy_string(validation->message, sizeof(validation->message), "valid Mach-O");

    if (!imgcheck_is_macho64(data, size)) {
        imgcheck_macho_fail(validation, "not a Mach-O 64-bit little-endian file", 0U);
        return;
    }
    filetype = imgcheck_read_u32_le(data + 12);
    ncmds = imgcheck_read_u32_le(data + 16);
    sizeofcmds = imgcheck_read_u32_le(data + 20);
    flags = imgcheck_read_u32_le(data + 24);
    if (ncmds > 1024U || 32ULL + (unsigned long long)sizeofcmds > (unsigned long long)size) {
        imgcheck_macho_fail(validation, "invalid Mach-O load-command table", 32U);
        return;
    }

    for (command_index = 0U; command_index < ncmds; ++command_index) {
        unsigned int command;
        unsigned int command_size;

        if (command_offset + 8ULL > (unsigned long long)size) {
            imgcheck_macho_fail(validation, "truncated Mach-O load command", (size_t)command_offset);
            return;
        }
        command = imgcheck_read_u32_le(data + command_offset);
        command_size = imgcheck_read_u32_le(data + command_offset + 4ULL);
        if (command_size < 8U || command_offset + (unsigned long long)command_size > (unsigned long long)size) {
            imgcheck_macho_fail(validation, "invalid Mach-O load-command size", (size_t)command_offset);
            return;
        }
        if (command == IMGCHECK_MACHO_LC_SEGMENT_64 && command_size >= 72U) {
            unsigned long long vmaddr = imgcheck_read_u64_le(data + command_offset + 24ULL);
            unsigned long long vmsize = imgcheck_read_u64_le(data + command_offset + 32ULL);
            unsigned long long fileoff = imgcheck_read_u64_le(data + command_offset + 40ULL);
            unsigned long long filesize = imgcheck_read_u64_le(data + command_offset + 48ULL);
            unsigned int nsects = imgcheck_read_u32_le(data + command_offset + 64ULL);
            unsigned int section_index;

            if (fileoff + filesize < fileoff || fileoff + filesize > (unsigned long long)size) {
                imgcheck_macho_fail(validation, "Mach-O segment file range exceeds input size", (size_t)command_offset);
                return;
            }
            if (72ULL + ((unsigned long long)nsects * 80ULL) > (unsigned long long)command_size) {
                imgcheck_macho_fail(validation, "Mach-O segment section table exceeds command size", (size_t)command_offset);
                return;
            }
            if (data[command_offset + 8ULL] == '_' && data[command_offset + 9ULL] == '_' && data[command_offset + 10ULL] == 'T' && data[command_offset + 11ULL] == 'E') {
                has_text_segment = 1;
            }
            if (has_main && entryoff >= fileoff && entryoff < fileoff + filesize && vmsize != 0ULL && vmaddr != 0ULL) {
                entry_in_file_backed_segment = 1;
            }
            for (section_index = 0U; section_index < nsects; ++section_index) {
                unsigned long long section_offset = command_offset + 72ULL + ((unsigned long long)section_index * 80ULL);
                unsigned long long section_size = imgcheck_read_u64_le(data + section_offset + 40ULL);
                unsigned int section_fileoff = imgcheck_read_u32_le(data + section_offset + 48ULL);
                unsigned int section_flags = imgcheck_read_u32_le(data + section_offset + 64ULL);
                if (!imgcheck_macho_section_is_zerofill(section_flags) && section_size != 0ULL &&
                    ((unsigned long long)section_fileoff + section_size < (unsigned long long)section_fileoff ||
                     (unsigned long long)section_fileoff + section_size > (unsigned long long)size)) {
                    imgcheck_macho_fail(validation, "Mach-O section file range exceeds input size", (size_t)section_offset);
                    return;
                }
            }
        } else if (command == IMGCHECK_MACHO_LC_MAIN && command_size >= 24U) {
            has_main = 1;
            entryoff = imgcheck_read_u64_le(data + command_offset + 8ULL);
        } else if (command == IMGCHECK_MACHO_LC_CODE_SIGNATURE && command_size >= 16U) {
            unsigned int dataoff = imgcheck_read_u32_le(data + command_offset + 8ULL);
            unsigned int datasize = imgcheck_read_u32_le(data + command_offset + 12ULL);
            has_code_signature = 1;
            validation->code_signature_present = 1;
            if ((unsigned long long)dataoff + (unsigned long long)datasize > (unsigned long long)size) {
                imgcheck_macho_fail(validation, "Mach-O code-signature range exceeds input size", (size_t)command_offset);
                return;
            }
            if (imgcheck_verify_macho_code_signature(data, size, dataoff, datasize, validation) != 0) {
                validation->warning_count += 1U;
            }
        } else if (command == IMGCHECK_MACHO_LC_DYLD_INFO || command == IMGCHECK_MACHO_LC_DYLD_INFO_ONLY || command == IMGCHECK_MACHO_LC_DYLD_CHAINED_FIXUPS) {
            has_rebase_metadata = 1;
        } else if (imgcheck_macho_command_is_dylib(command)) {
            has_dylib_import = 1;
        }
        command_offset += (unsigned long long)command_size;
    }

    if (filetype == IMGCHECK_MACHO_TYPE_EXECUTE && !has_text_segment) {
        imgcheck_macho_fail(validation, "Mach-O executable has no __TEXT segment", 0U);
        return;
    }
    if (has_main && entryoff < (unsigned long long)size) {
        entry_in_file_backed_segment = 1;
    }
    if (filetype == IMGCHECK_MACHO_TYPE_EXECUTE && !has_main) {
        validation->warning_count += 1U;
    }
    if (has_main && !entry_in_file_backed_segment) {
        validation->warning_count += 1U;
    }
    if ((flags & IMGCHECK_MACHO_FLAG_PIE) != 0U && !has_rebase_metadata) {
        pie_without_rebase = 1;
        validation->warning_count += 1U;
    }
    if (!has_code_signature) {
        validation->warning_count += 1U;
    }
    if (has_dylib_import) {
        validation->warning_count += 1U;
    }
    if (validation->warning_count != 0U) {
        if (pie_without_rebase) {
            rt_copy_string(validation->message, sizeof(validation->message), "valid Mach-O with warnings: PIE has no dyld rebase metadata");
        } else if (has_dylib_import) {
            rt_copy_string(validation->message, sizeof(validation->message), "valid Mach-O with warnings: dylib imports present");
        } else if (!has_code_signature) {
            rt_copy_string(validation->message, sizeof(validation->message), "valid Mach-O with warnings: no code signature");
        } else if (!validation->code_signature_verified) {
            rt_copy_string(validation->message, sizeof(validation->message), "valid Mach-O with warnings: code signature hashes not verified");
        } else {
            rt_copy_string(validation->message, sizeof(validation->message), "valid Mach-O with warnings");
        }
        if (options->strict) {
            validation->valid = 0;
        }
    }
}

static void write_macho_plain_result(const char *label, const ImgcheckMachoValidation *validation) {
    rt_write_cstr(1, label);
    rt_write_cstr(1, "\tmacho\t");
    rt_write_cstr(1, validation->valid ? (validation->warning_count != 0U ? "warn" : "ok") : "fail");
    rt_write_char(1, '\t');
    if (validation->has_failure_offset) rt_write_uint(1, (unsigned long long)validation->failure_offset); else rt_write_char(1, '-');
    rt_write_char(1, '\t');
    rt_write_cstr(1, validation->message);
    rt_write_char(1, '\n');
}

static void write_macho_json_result(const char *label, const ImgcheckMachoValidation *validation) {
    tool_json_begin_event(1, "imgcheck", "stdout", "macho_check");
    rt_write_cstr(1, ",\"data\":{\"path\":");
    tool_json_write_string(1, label);
    rt_write_cstr(1, ",\"format\":\"macho\",\"valid\":");
    rt_write_cstr(1, validation->valid ? "true" : "false");
    rt_write_cstr(1, ",\"warning_count\":");
    rt_write_uint(1, (unsigned long long)validation->warning_count);
    rt_write_cstr(1, ",\"code_signature_present\":");
    rt_write_cstr(1, validation->code_signature_present ? "true" : "false");
    rt_write_cstr(1, ",\"code_signature_verified\":");
    rt_write_cstr(1, validation->code_signature_verified ? "true" : "false");
    rt_write_cstr(1, ",\"code_signature_checked_slots\":");
    rt_write_uint(1, (unsigned long long)validation->code_signature_checked_slots);
    rt_write_cstr(1, ",\"code_signature_mismatches\":");
    rt_write_uint(1, (unsigned long long)validation->code_signature_mismatches);
    rt_write_cstr(1, ",\"message\":");
    tool_json_write_string(1, validation->message);
    rt_write_cstr(1, ",\"failure_offset\":");
    if (validation->has_failure_offset) rt_write_uint(1, (unsigned long long)validation->failure_offset); else rt_write_cstr(1, "null");
    rt_write_char(1, '}');
    tool_json_end_event(1);
}

static void write_macho_human_result(const char *label, const ImgcheckMachoValidation *validation, const ImgcheckOptions *options) {
    rt_write_cstr(1, label);
    rt_write_cstr(1, validation->valid ? (validation->warning_count != 0U ? ": WARN" : ": OK") : ": FAIL");
    rt_write_cstr(1, " (macho)");
    if (options->verbose || !validation->valid || validation->warning_count != 0U) {
        rt_write_cstr(1, ": ");
        rt_write_cstr(1, validation->message);
        if (validation->warning_count != 0U) {
            rt_write_cstr(1, "; warnings=");
            rt_write_uint(1, (unsigned long long)validation->warning_count);
        }
        if (validation->code_signature_present) {
            rt_write_cstr(1, "; code-signature=");
            rt_write_cstr(1, validation->code_signature_verified ? "verified" : "unverified");
        }
        if (validation->has_failure_offset) {
            rt_write_cstr(1, " at offset ");
            rt_write_uint(1, (unsigned long long)validation->failure_offset);
        }
    }
    rt_write_char(1, '\n');
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
    if (imgcheck_is_macho64(data, size)) {
        ImgcheckMachoValidation macho_validation;
        imgcheck_validate_macho(data, size, options, &macho_validation);
        rt_free(data);
        if (!options->quiet) {
            if (options->json) {
                write_macho_json_result(label, &macho_validation);
            } else if (options->plain) {
                write_macho_plain_result(label, &macho_validation);
            } else {
                write_macho_human_result(label, &macho_validation, options);
            }
        }
        return macho_validation.valid ? 0 : -1;
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
