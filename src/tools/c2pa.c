#include "compression/crc32.h"
#include "crypto/p256.h"
#include "crypto/sha256.h"
#include "image/image.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define C2PA_APP11_MAX_PAYLOAD 65533U

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} ByteBuffer;

static const unsigned char DEV_PRIVATE_KEY[32] = {
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U
};

static const unsigned char DEV_CERT_DER[] = {
    0x30U, 0x81U, 0x81U, 0x30U, 0x70U, 0x02U, 0x01U, 0x01U,
    0x30U, 0x0aU, 0x06U, 0x08U, 0x2aU, 0x86U, 0x48U, 0xceU,
    0x3dU, 0x04U, 0x03U, 0x02U, 0x30U, 0x00U, 0x30U, 0x00U,
    0x30U, 0x00U, 0x30U, 0x59U, 0x30U, 0x13U, 0x06U, 0x07U,
    0x2aU, 0x86U, 0x48U, 0xceU, 0x3dU, 0x02U, 0x01U, 0x06U,
    0x08U, 0x2aU, 0x86U, 0x48U, 0xceU, 0x3dU, 0x03U, 0x01U,
    0x07U, 0x03U, 0x42U, 0x00U, 0x04U, 0x6bU, 0x17U, 0xd1U,
    0xf2U, 0xe1U, 0x2cU, 0x42U, 0x47U, 0xf8U, 0xbcU, 0xe6U,
    0xe5U, 0x63U, 0xa4U, 0x40U, 0xf2U, 0x77U, 0x03U, 0x7dU,
    0x81U, 0x2dU, 0xebU, 0x33U, 0xa0U, 0xf4U, 0xa1U, 0x39U,
    0x45U, 0xd8U, 0x98U, 0xc2U, 0x96U, 0x4fU, 0xe3U, 0x42U,
    0xe2U, 0xfeU, 0x1aU, 0x7fU, 0x9bU, 0x8eU, 0xe7U, 0xebU,
    0x4aU, 0x7cU, 0x0fU, 0x9eU, 0x16U, 0x2bU, 0xceU, 0x33U,
    0x57U, 0x6bU, 0x31U, 0x5eU, 0xceU, 0xcbU, 0xb6U, 0x40U,
    0x68U, 0x37U, 0xbfU, 0x51U, 0xf5U, 0x30U, 0x0aU, 0x06U,
    0x08U, 0x2aU, 0x86U, 0x48U, 0xceU, 0x3dU, 0x04U, 0x03U,
    0x02U, 0x03U, 0x01U, 0x00U
};

static void print_usage(void) {
    tool_write_usage("c2pa", "add --dev-key -o OUTPUT [--claim-generator TEXT] [--action ACTION] FILE");
}

static int bytes_equal(const unsigned char *bytes, const char *text, size_t length) {
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (bytes[index] != (unsigned char)text[index]) return 0;
    }
    return 1;
}

static int byte_arrays_equal(const unsigned char *left, const unsigned char *right, size_t length) {
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (left[index] != right[index]) return 0;
    }
    return 1;
}

static unsigned int read_u16_be(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 8U) | (unsigned int)bytes[1];
}

static unsigned int read_u32_be(const unsigned char *bytes) {
    return ((unsigned int)bytes[0] << 24U) | ((unsigned int)bytes[1] << 16U) |
           ((unsigned int)bytes[2] << 8U) | (unsigned int)bytes[3];
}

static void buf_init(ByteBuffer *buf) {
    buf->data = 0;
    buf->size = 0U;
    buf->capacity = 0U;
}

static void buf_free(ByteBuffer *buf) {
    rt_free(buf->data);
    buf_init(buf);
}

static int buf_reserve(ByteBuffer *buf, size_t extra) {
    size_t needed = buf->size + extra;
    if (needed < buf->size) return -1;
    if (needed > buf->capacity) {
        size_t next = buf->capacity == 0U ? 256U : buf->capacity;
        unsigned char *resized;
        while (next < needed) {
            size_t doubled = next * 2U;
            if (doubled <= next) return -1;
            next = doubled;
        }
        resized = (unsigned char *)rt_realloc(buf->data, next);
        if (resized == 0) return -1;
        buf->data = resized;
        buf->capacity = next;
    }
    return 0;
}

static int buf_append(ByteBuffer *buf, const void *data, size_t size) {
    if (buf_reserve(buf, size) != 0) return -1;
    if (size > 0U) memcpy(buf->data + buf->size, data, size);
    buf->size += size;
    return 0;
}

static int buf_u8(ByteBuffer *buf, unsigned int value) {
    unsigned char byte = (unsigned char)value;
    return buf_append(buf, &byte, 1U);
}

static int buf_u16be(ByteBuffer *buf, unsigned int value) {
    unsigned char bytes[2];
    bytes[0] = (unsigned char)(value >> 8U);
    bytes[1] = (unsigned char)value;
    return buf_append(buf, bytes, 2U);
}

static int buf_u32be(ByteBuffer *buf, unsigned int value) {
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(value >> 24U);
    bytes[1] = (unsigned char)(value >> 16U);
    bytes[2] = (unsigned char)(value >> 8U);
    bytes[3] = (unsigned char)value;
    return buf_append(buf, bytes, 4U);
}

static int cbor_type(ByteBuffer *buf, unsigned int major, size_t value) {
    if (value < 24U) return buf_u8(buf, (major << 5U) | (unsigned int)value);
    if (value <= 0xffU) return buf_u8(buf, (major << 5U) | 24U) || buf_u8(buf, (unsigned int)value);
    if (value <= 0xffffU) return buf_u8(buf, (major << 5U) | 25U) || buf_u16be(buf, (unsigned int)value);
    return buf_u8(buf, (major << 5U) | 26U) || buf_u32be(buf, (unsigned int)value);
}

static int cbor_text(ByteBuffer *buf, const char *text) {
    size_t len = rt_strlen(text);
    return cbor_type(buf, 3U, len) || buf_append(buf, text, len);
}

static int cbor_bstr(ByteBuffer *buf, const unsigned char *data, size_t size) {
    return cbor_type(buf, 2U, size) || buf_append(buf, data, size);
}

static int cbor_int(ByteBuffer *buf, long long value) {
    if (value >= 0LL) return cbor_type(buf, 0U, (size_t)value);
    return cbor_type(buf, 1U, (size_t)(-1LL - value));
}

static int make_box(ByteBuffer *out, const char type[4], const unsigned char *payload, size_t payload_size) {
    if (payload_size > 0xfffffff0U) return -1;
    return buf_u32be(out, (unsigned int)payload_size + 8U) || buf_append(out, type, 4U) || buf_append(out, payload, payload_size);
}

static int make_jumb(ByteBuffer *out, const char *label, const unsigned char *children, size_t children_size) {
    ByteBuffer payload;
    ByteBuffer jumd_payload;
    ByteBuffer jumd;
    int result;

    buf_init(&payload);
    buf_init(&jumd_payload);
    buf_init(&jumd);
    result = buf_append(&jumd_payload, label, rt_strlen(label) + 1U) ||
             make_box(&jumd, "jumd", jumd_payload.data, jumd_payload.size) ||
             buf_append(&payload, jumd.data, jumd.size) ||
             buf_append(&payload, children, children_size) ||
             make_box(out, "jumb", payload.data, payload.size);
    buf_free(&payload);
    buf_free(&jumd_payload);
    buf_free(&jumd);
    return result;
}

static int make_cbor_jumb(ByteBuffer *out, const char *label, const unsigned char *cbor, size_t cbor_size) {
    ByteBuffer child;
    int result;

    buf_init(&child);
    result = make_box(&child, "cbor", cbor, cbor_size) || make_jumb(out, label, child.data, child.size);
    buf_free(&child);
    return result;
}

static int read_all_input(const char *path, unsigned char **data_out, size_t *size_out) {
    int fd = platform_open_read(path);
    unsigned char *buffer;
    size_t capacity = 65536U;
    size_t used = 0U;

    *data_out = 0;
    *size_out = 0U;
    if (fd < 0) {
        tool_write_error("c2pa", "cannot read: ", path);
        return -1;
    }
    buffer = (unsigned char *)rt_malloc(capacity);
    if (buffer == 0) {
        platform_close(fd);
        return -1;
    }
    while (1) {
        long bytes_read;
        if (used == capacity) {
            unsigned char *resized = (unsigned char *)rt_realloc(buffer, capacity * 2U);
            if (resized == 0) {
                rt_free(buffer);
                platform_close(fd);
                return -1;
            }
            buffer = resized;
            capacity *= 2U;
        }
        bytes_read = platform_read(fd, buffer + used, capacity - used);
        if (bytes_read < 0) {
            rt_free(buffer);
            platform_close(fd);
            tool_write_error("c2pa", "read failed: ", path);
            return -1;
        }
        if (bytes_read == 0) break;
        used += (size_t)bytes_read;
    }
    platform_close(fd);
    *data_out = buffer;
    *size_out = used;
    return 0;
}

static int write_file(const char *path, const unsigned char *data, size_t size) {
    int fd = platform_open_write(path, 0644U);
    if (fd < 0) {
        tool_write_error("c2pa", "cannot write: ", path);
        return -1;
    }
    if (rt_write_all(fd, data, size) != 0) {
        platform_close(fd);
        tool_write_error("c2pa", "write failed: ", path);
        return -1;
    }
    platform_close(fd);
    return 0;
}

static int has_existing_c2pa(const unsigned char *data, size_t size) {
    ImageC2paInfo info;
    return image_c2pa_analyze(data, size, &info) == 0 && info.present;
}

static int make_hash_assertion(ByteBuffer *out, const unsigned char digest[CRYPTO_SHA256_DIGEST_SIZE]) {
    return cbor_type(out, 5U, 3U) ||
           cbor_text(out, "alg") || cbor_text(out, "sha256") ||
           cbor_text(out, "hash") || cbor_bstr(out, digest, CRYPTO_SHA256_DIGEST_SIZE) ||
           cbor_text(out, "exclusions") || cbor_type(out, 4U, 0U);
}

static int make_actions_assertion(ByteBuffer *out, const char *action) {
    return cbor_type(out, 5U, 1U) ||
        cbor_text(out, "actions") || cbor_type(out, 4U, 1U) ||
        cbor_type(out, 5U, 1U) ||
        cbor_text(out, "action") || cbor_text(out, action);
}

static int append_assertion_ref(ByteBuffer *out, const char *url, const unsigned char digest[CRYPTO_SHA256_DIGEST_SIZE]) {
    return cbor_type(out, 5U, 2U) ||
        cbor_text(out, "url") || cbor_text(out, url) ||
        cbor_text(out, "hash") || cbor_bstr(out, digest, CRYPTO_SHA256_DIGEST_SIZE);
}

static int make_claim(ByteBuffer *out,
                      const char *generator,
             const unsigned char hash_assertion_digest[CRYPTO_SHA256_DIGEST_SIZE],
             const unsigned char actions_assertion_digest[CRYPTO_SHA256_DIGEST_SIZE]) {
    return cbor_type(out, 5U, 5U) ||
           cbor_text(out, "claim_generator") || cbor_text(out, generator) ||
           cbor_text(out, "instanceID") || cbor_text(out, "xmp:iid:newos-dev-c2pa") ||
           cbor_text(out, "signature") || cbor_text(out, "self#jumbf=c2pa.signature") ||
        cbor_text(out, "created_assertions") || cbor_type(out, 4U, 2U) ||
        append_assertion_ref(out, "self#jumbf=c2pa.assertions/c2pa.hash.data", hash_assertion_digest) ||
        append_assertion_ref(out, "self#jumbf=c2pa.assertions/c2pa.actions.v2", actions_assertion_digest) ||
        cbor_text(out, "assertions") || cbor_type(out, 4U, 2U) ||
        cbor_text(out, "c2pa.hash.data") || cbor_text(out, "c2pa.actions.v2");
}

static int make_protected_header(ByteBuffer *out) {
    return cbor_type(out, 5U, 2U) ||
           cbor_int(out, 1LL) || cbor_int(out, -7LL) ||
           cbor_int(out, 33LL) || cbor_bstr(out, DEV_CERT_DER, sizeof(DEV_CERT_DER));
}

static int make_sig_structure(ByteBuffer *out, const unsigned char *protected_bytes, size_t protected_size, const unsigned char *claim, size_t claim_size) {
    return cbor_type(out, 4U, 4U) ||
           cbor_text(out, "Signature1") ||
           cbor_bstr(out, protected_bytes, protected_size) ||
           cbor_bstr(out, (const unsigned char *)"", 0U) ||
           cbor_bstr(out, claim, claim_size);
}

static int make_cose_signature(ByteBuffer *out, const unsigned char *claim, size_t claim_size) {
    ByteBuffer protected_header;
    ByteBuffer sig_structure;
    unsigned char digest[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned char signature[CRYPTO_P256_ECDSA_SIGNATURE_SIZE];
    int result;

    buf_init(&protected_header);
    buf_init(&sig_structure);
    result = make_protected_header(&protected_header) ||
             make_sig_structure(&sig_structure, protected_header.data, protected_header.size, claim, claim_size);
    if (result == 0) {
        crypto_sha256_hash(sig_structure.data, sig_structure.size, digest);
        if (!crypto_p256_ecdsa_sha256_sign(DEV_PRIVATE_KEY, digest, signature)) result = -1;
    }
    if (result == 0) {
        result = buf_u8(out, 0xd2U) || cbor_type(out, 4U, 4U) ||
                 cbor_bstr(out, protected_header.data, protected_header.size) ||
                 cbor_type(out, 5U, 0U) ||
                 buf_u8(out, 0xf6U) ||
                 cbor_bstr(out, signature, sizeof(signature));
    }
    buf_free(&protected_header);
    buf_free(&sig_structure);
    return result;
}

static int make_manifest(const unsigned char *input, size_t input_size, const char *generator, const char *action, ByteBuffer *out) {
    unsigned char content_digest[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned char hash_assertion_digest[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned char actions_assertion_digest[CRYPTO_SHA256_DIGEST_SIZE];
    ByteBuffer hash_assertion;
    ByteBuffer actions_assertion;
    ByteBuffer claim;
    ByteBuffer signature;
    ByteBuffer assertions_children;
    ByteBuffer manifest_children;
    int result;

    buf_init(&hash_assertion);
    buf_init(&actions_assertion);
    buf_init(&claim);
    buf_init(&signature);
    buf_init(&assertions_children);
    buf_init(&manifest_children);
    crypto_sha256_hash(input, input_size, content_digest);
    result = make_hash_assertion(&hash_assertion, content_digest) || make_actions_assertion(&actions_assertion, action);
    if (result == 0) {
        crypto_sha256_hash(hash_assertion.data, hash_assertion.size, hash_assertion_digest);
        crypto_sha256_hash(actions_assertion.data, actions_assertion.size, actions_assertion_digest);
    }
    result = result || make_claim(&claim, generator, hash_assertion_digest, actions_assertion_digest) ||
             make_cose_signature(&signature, claim.data, claim.size) ||
             make_cbor_jumb(&assertions_children, "c2pa.hash.data", hash_assertion.data, hash_assertion.size) ||
             make_cbor_jumb(&assertions_children, "c2pa.actions.v2", actions_assertion.data, actions_assertion.size) ||
             make_cbor_jumb(&manifest_children, "c2pa.claim.v2", claim.data, claim.size) ||
             make_jumb(&manifest_children, "c2pa.assertions", assertions_children.data, assertions_children.size) ||
             make_cbor_jumb(&manifest_children, "c2pa.signature", signature.data, signature.size) ||
             make_jumb(out, "urn:c2pa:newos-dev", manifest_children.data, manifest_children.size);
    buf_free(&hash_assertion);
    buf_free(&actions_assertion);
    buf_free(&claim);
    buf_free(&signature);
    buf_free(&assertions_children);
    buf_free(&manifest_children);
    return result;
}

static int make_store(const unsigned char *input, size_t input_size, const char *generator, const char *action, ByteBuffer *out) {
    ByteBuffer manifest;
    int result;
    buf_init(&manifest);
    result = make_manifest(input, input_size, generator, action, &manifest) || make_jumb(out, "c2pa", manifest.data, manifest.size);
    buf_free(&manifest);
    return result;
}

static int insert_png_cabx(const unsigned char *data, size_t size, const unsigned char *payload, size_t payload_size, ByteBuffer *out) {
    static const unsigned char sig[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    size_t offset = 8U;
    if (size < 8U || !byte_arrays_equal(data, sig, sizeof(sig))) return -1;
    while (offset + 12U <= size) {
        unsigned int length = read_u32_be(data + offset);
        const unsigned char *type = data + offset + 4U;
        if ((size_t)length > size - offset - 12U) return -1;
        if (bytes_equal(type, "IEND", 4U)) {
            unsigned int crc;
            if (buf_append(out, data, offset) != 0) return -1;
            if (buf_u32be(out, (unsigned int)payload_size) || buf_append(out, "caBX", 4U) || buf_append(out, payload, payload_size)) return -1;
            crc = compression_crc32(out->data + out->size - payload_size - 4U, payload_size + 4U);
            if (buf_u32be(out, crc) || buf_append(out, data + offset, size - offset)) return -1;
            return 0;
        }
        offset += 12U + (size_t)length;
    }
    return -1;
}

static int jpeg_insert_offset(const unsigned char *data, size_t size, size_t *offset_out) {
    size_t offset = 2U;
    if (size < 4U || data[0] != 0xffU || data[1] != 0xd8U) return -1;
    while (offset + 3U < size) {
        unsigned char marker;
        unsigned int segment_size;
        while (offset < size && data[offset] != 0xffU) offset += 1U;
        while (offset < size && data[offset] == 0xffU) offset += 1U;
        if (offset >= size) return -1;
        marker = data[offset++];
        if (marker == 0xdaU) {
            *offset_out = offset - 2U;
            return 0;
        }
        if (marker == 0xd9U) return -1;
        if (marker == 0x01U || (marker >= 0xd0U && marker <= 0xd7U)) continue;
        if (offset + 2U > size) return -1;
        segment_size = read_u16_be(data + offset);
        if (segment_size < 2U || (size_t)segment_size > size - offset) return -1;
        offset += (size_t)segment_size;
    }
    return -1;
}

static int insert_jpeg_app11(const unsigned char *data, size_t size, const unsigned char *jumbf, size_t jumbf_size, ByteBuffer *out) {
    size_t insert_at;
    unsigned int segment_payload_size;
    if (jumbf_size + 4U > C2PA_APP11_MAX_PAYLOAD || jpeg_insert_offset(data, size, &insert_at) != 0) return -1;
    segment_payload_size = (unsigned int)jumbf_size + 4U;
    return buf_append(out, data, insert_at) ||
           buf_u8(out, 0xffU) || buf_u8(out, 0xebU) ||
           buf_u16be(out, segment_payload_size + 2U) ||
           buf_append(out, "JP", 2U) || buf_u8(out, 0U) || buf_u8(out, 1U) ||
           buf_append(out, jumbf, jumbf_size) ||
           buf_append(out, data + insert_at, size - insert_at);
}

static int run_add(int argc, char **argv, int arg_index) {
    const char *output_path = 0;
    const char *input_path = 0;
    const char *generator = "newos c2pa dev signer";
    const char *action = "c2pa.created";
    int dev_key = 0;
    unsigned char *input = 0;
    size_t input_size = 0U;
    ImageInfo info;
    ByteBuffer store;
    ByteBuffer output;
    int result = -1;

    buf_init(&store);
    buf_init(&output);
    while (arg_index < argc) {
        const char *arg = argv[arg_index];
        if (rt_strcmp(arg, "--dev-key") == 0) { dev_key = 1; arg_index += 1; continue; }
        if ((rt_strcmp(arg, "-o") == 0 || rt_strcmp(arg, "--output") == 0) && arg_index + 1 < argc) { output_path = argv[arg_index + 1]; arg_index += 2; continue; }
        if (rt_strcmp(arg, "--claim-generator") == 0 && arg_index + 1 < argc) { generator = argv[arg_index + 1]; arg_index += 2; continue; }
        if (rt_strcmp(arg, "--action") == 0 && arg_index + 1 < argc) { action = argv[arg_index + 1]; arg_index += 2; continue; }
        if (arg[0] == '-' && arg[1] != '\0') { tool_write_error("c2pa", "unknown option: ", arg); goto done; }
        input_path = arg;
        arg_index += 1;
    }
    if (!dev_key || output_path == 0 || input_path == 0) {
        print_usage();
        goto done;
    }
    if (read_all_input(input_path, &input, &input_size) != 0) goto done;
    if (image_probe(input, input_size, &info) != 0 || (info.format != IMAGE_FORMAT_PNG && info.format != IMAGE_FORMAT_JPEG)) {
        tool_write_error("c2pa", "expected PNG or JPEG: ", input_path);
        goto done;
    }
    if (has_existing_c2pa(input, input_size)) {
        tool_write_error("c2pa", "input already contains C2PA metadata: ", input_path);
        goto done;
    }
    if (make_store(input, input_size, generator, action, &store) != 0) goto done;
    if (info.format == IMAGE_FORMAT_PNG) result = insert_png_cabx(input, input_size, store.data, store.size, &output);
    else result = insert_jpeg_app11(input, input_size, store.data, store.size, &output);
    if (result == 0) result = write_file(output_path, output.data, output.size);
done:
    rt_free(input);
    buf_free(&store);
    buf_free(&output);
    return result == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }
    if (rt_strcmp(argv[1], "-h") == 0 || rt_strcmp(argv[1], "--help") == 0) {
        print_usage();
        return 0;
    }
    if (rt_strcmp(argv[1], "add") == 0) {
        return run_add(argc, argv, 2);
    }
    tool_write_error("c2pa", "unknown command: ", argv[1]);
    print_usage();
    return 1;
}