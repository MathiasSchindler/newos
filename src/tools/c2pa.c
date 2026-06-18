#include "compression/crc32.h"
#include "crypto/p256.h"
#include "crypto/sha256.h"
#include "image/image.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define C2PA_APP11_MAX_PAYLOAD 65533U
#define C2PA_MAX_ACTIONS 16U

static const unsigned char JUMBF_TYPE_C2PA_STORE[16] = {
    0x63U, 0x32U, 0x70U, 0x61U, 0x00U, 0x11U, 0x00U, 0x10U,
    0x80U, 0x00U, 0x00U, 0xaaU, 0x00U, 0x38U, 0x9bU, 0x71U
};
static const unsigned char JUMBF_TYPE_C2PA_MANIFEST[16] = {
    0x63U, 0x32U, 0x6dU, 0x61U, 0x00U, 0x11U, 0x00U, 0x10U,
    0x80U, 0x00U, 0x00U, 0xaaU, 0x00U, 0x38U, 0x9bU, 0x71U
};
static const unsigned char JUMBF_TYPE_C2PA_ASSERTION_STORE[16] = {
    0x63U, 0x32U, 0x61U, 0x73U, 0x00U, 0x11U, 0x00U, 0x10U,
    0x80U, 0x00U, 0x00U, 0xaaU, 0x00U, 0x38U, 0x9bU, 0x71U
};
static const unsigned char JUMBF_TYPE_C2PA_CLAIM[16] = {
    0x63U, 0x32U, 0x63U, 0x6cU, 0x00U, 0x11U, 0x00U, 0x10U,
    0x80U, 0x00U, 0x00U, 0xaaU, 0x00U, 0x38U, 0x9bU, 0x71U
};
static const unsigned char JUMBF_TYPE_C2PA_SIGNATURE[16] = {
    0x63U, 0x32U, 0x63U, 0x73U, 0x00U, 0x11U, 0x00U, 0x10U,
    0x80U, 0x00U, 0x00U, 0xaaU, 0x00U, 0x38U, 0x9bU, 0x71U
};
static const unsigned char JUMBF_TYPE_CBOR[16] = {
    0x63U, 0x62U, 0x6fU, 0x72U, 0x00U, 0x11U, 0x00U, 0x10U,
    0x80U, 0x00U, 0x00U, 0xaaU, 0x00U, 0x38U, 0x9bU, 0x71U
};

typedef ToolByteBuffer ByteBuffer;

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
    tool_write_usage("c2pa", "add --dev-key -o OUTPUT [--claim-generator TEXT] [--action ACTION]... FILE");
}



static int action_can_start_manifest(const char *action) {
    return rt_strcmp(action, "c2pa.created") == 0 || rt_strcmp(action, "c2pa.opened") == 0;
}

static int prepend_default_created_action(const char **actions, unsigned int *action_count) {
    unsigned int action_index;

    if (*action_count == 0U) {
        actions[(*action_count)++] = "c2pa.created";
        return 0;
    }
    if (action_can_start_manifest(actions[0])) return 0;
    if (*action_count >= C2PA_MAX_ACTIONS) return -1;
    action_index = *action_count;
    while (action_index > 0U) {
        actions[action_index] = actions[action_index - 1U];
        action_index -= 1U;
    }
    actions[0] = "c2pa.created";
    *action_count += 1U;
    return 0;
}


static int cbor_type(ByteBuffer *buf, unsigned int major, size_t value) {
    if (value < 24U) return tool_byte_buffer_append_byte(buf, (major << 5U) | (unsigned int)value);
    if (value <= 0xffU) return tool_byte_buffer_append_byte(buf, (major << 5U) | 24U) || tool_byte_buffer_append_byte(buf, (unsigned int)value);
    if (value <= 0xffffU) return tool_byte_buffer_append_byte(buf, (major << 5U) | 25U) || tool_byte_buffer_append_u16_be(buf, (unsigned int)value);
    return tool_byte_buffer_append_byte(buf, (major << 5U) | 26U) || tool_byte_buffer_append_u32_be(buf, (unsigned int)value);
}

static int cbor_text(ByteBuffer *buf, const char *text) {
    size_t len = rt_strlen(text);
    return cbor_type(buf, 3U, len) || tool_byte_buffer_append(buf, text, len);
}

static int cbor_bstr(ByteBuffer *buf, const unsigned char *data, size_t size) {
    return cbor_type(buf, 2U, size) || tool_byte_buffer_append(buf, data, size);
}

static int cbor_int(ByteBuffer *buf, long long value) {
    if (value >= 0LL) return cbor_type(buf, 0U, (size_t)value);
    return cbor_type(buf, 1U, (size_t)(-1LL - value));
}

static int make_box(ByteBuffer *out, const char type[4], const unsigned char *payload, size_t payload_size) {
    if (payload_size > 0xfffffff0U) return -1;
    return tool_byte_buffer_append_u32_be(out, (unsigned int)payload_size + 8U) || tool_byte_buffer_append(out, type, 4U) || tool_byte_buffer_append(out, payload, payload_size);
}

static int make_jumb(ByteBuffer *out, const char *label, const unsigned char type_uuid[16], const unsigned char *children, size_t children_size) {
    ByteBuffer payload;
    ByteBuffer jumd_payload;
    ByteBuffer jumd;
    int result;

    tool_byte_buffer_init(&payload);
    tool_byte_buffer_init(&jumd_payload);
    tool_byte_buffer_init(&jumd);
    result = tool_byte_buffer_append(&jumd_payload, type_uuid, 16U) ||
             tool_byte_buffer_append_byte(&jumd_payload, 0x03U) ||
             tool_byte_buffer_append(&jumd_payload, label, rt_strlen(label) + 1U) ||
             make_box(&jumd, "jumd", jumd_payload.data, jumd_payload.size) ||
             tool_byte_buffer_append(&payload, jumd.data, jumd.size) ||
             tool_byte_buffer_append(&payload, children, children_size) ||
             make_box(out, "jumb", payload.data, payload.size);
    tool_byte_buffer_free(&payload);
    tool_byte_buffer_free(&jumd_payload);
    tool_byte_buffer_free(&jumd);
    return result;
}

static int make_cbor_jumb(ByteBuffer *out, const char *label, const unsigned char type_uuid[16], const unsigned char *cbor, size_t cbor_size) {
    ByteBuffer child;
    int result;

    tool_byte_buffer_init(&child);
    result = make_box(&child, "cbor", cbor, cbor_size) || make_jumb(out, label, type_uuid, child.data, child.size);
    tool_byte_buffer_free(&child);
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

static int c2pa_write_file(const char *path, const unsigned char *data, size_t size) {
    return tool_write_file_all_report(path, data, size, "c2pa");
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

static int make_actions_assertion(ByteBuffer *out, const char *const *actions, unsigned int action_count) {
    unsigned int action_index;

    if (action_count == 0U) return -1;
    if (cbor_type(out, 5U, 1U) || cbor_text(out, "actions") || cbor_type(out, 4U, action_count)) return -1;
    for (action_index = 0U; action_index < action_count; ++action_index) {
        if (cbor_type(out, 5U, 1U) || cbor_text(out, "action") || cbor_text(out, actions[action_index])) return -1;
    }
    return 0;
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
       cbor_text(out, "alg") || cbor_text(out, "sha256") ||
       cbor_text(out, "signature") || cbor_text(out, "self#jumbf=c2pa.signature") ||
       cbor_text(out, "instanceID") || cbor_text(out, "xmp:iid:newos-dev-c2pa") ||
        cbor_text(out, "created_assertions") || cbor_type(out, 4U, 2U) ||
        append_assertion_ref(out, "self#jumbf=c2pa.assertions/c2pa.hash.data", hash_assertion_digest) ||
    append_assertion_ref(out, "self#jumbf=c2pa.assertions/c2pa.actions.v2", actions_assertion_digest) ||
    cbor_text(out, "claim_generator_info") || cbor_type(out, 5U, 1U) ||
    cbor_text(out, "name") || cbor_text(out, generator);
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

    tool_byte_buffer_init(&protected_header);
    tool_byte_buffer_init(&sig_structure);
    result = make_protected_header(&protected_header) ||
             make_sig_structure(&sig_structure, protected_header.data, protected_header.size, claim, claim_size);
    if (result == 0) {
        crypto_sha256_hash(sig_structure.data, sig_structure.size, digest);
        if (!crypto_p256_ecdsa_sha256_sign(DEV_PRIVATE_KEY, digest, signature)) result = -1;
    }
    if (result == 0) {
        result = tool_byte_buffer_append_byte(out, 0xd2U) || cbor_type(out, 4U, 4U) ||
                 cbor_bstr(out, protected_header.data, protected_header.size) ||
                 cbor_type(out, 5U, 0U) ||
                 tool_byte_buffer_append_byte(out, 0xf6U) ||
                 cbor_bstr(out, signature, sizeof(signature));
    }
    tool_byte_buffer_free(&protected_header);
    tool_byte_buffer_free(&sig_structure);
    return result;
}

static int make_manifest(const unsigned char *input, size_t input_size, const char *generator, const char *const *actions, unsigned int action_count, ByteBuffer *out) {
    unsigned char content_digest[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned char hash_assertion_digest[CRYPTO_SHA256_DIGEST_SIZE];
    unsigned char actions_assertion_digest[CRYPTO_SHA256_DIGEST_SIZE];
    ByteBuffer hash_assertion;
    ByteBuffer actions_assertion;
    ByteBuffer hash_assertion_box;
    ByteBuffer actions_assertion_box;
    ByteBuffer claim;
    ByteBuffer signature;
    ByteBuffer assertions_children;
    ByteBuffer manifest_children;
    int result;

    tool_byte_buffer_init(&hash_assertion);
    tool_byte_buffer_init(&actions_assertion);
    tool_byte_buffer_init(&hash_assertion_box);
    tool_byte_buffer_init(&actions_assertion_box);
    tool_byte_buffer_init(&claim);
    tool_byte_buffer_init(&signature);
    tool_byte_buffer_init(&assertions_children);
    tool_byte_buffer_init(&manifest_children);
    crypto_sha256_hash(input, input_size, content_digest);
    result = make_hash_assertion(&hash_assertion, content_digest) || make_actions_assertion(&actions_assertion, actions, action_count);
    result = result ||
             make_cbor_jumb(&hash_assertion_box, "c2pa.hash.data", JUMBF_TYPE_CBOR, hash_assertion.data, hash_assertion.size) ||
             make_cbor_jumb(&actions_assertion_box, "c2pa.actions.v2", JUMBF_TYPE_CBOR, actions_assertion.data, actions_assertion.size);
    if (result == 0) {
        crypto_sha256_hash(hash_assertion_box.data, hash_assertion_box.size, hash_assertion_digest);
        crypto_sha256_hash(actions_assertion_box.data, actions_assertion_box.size, actions_assertion_digest);
    }
    result = result || make_claim(&claim, generator, hash_assertion_digest, actions_assertion_digest) ||
             make_cose_signature(&signature, claim.data, claim.size) ||
             tool_byte_buffer_append(&assertions_children, hash_assertion_box.data, hash_assertion_box.size) ||
             tool_byte_buffer_append(&assertions_children, actions_assertion_box.data, actions_assertion_box.size) ||
             make_cbor_jumb(&manifest_children, "c2pa.claim.v2", JUMBF_TYPE_C2PA_CLAIM, claim.data, claim.size) ||
             make_jumb(&manifest_children, "c2pa.assertions", JUMBF_TYPE_C2PA_ASSERTION_STORE, assertions_children.data, assertions_children.size) ||
             make_cbor_jumb(&manifest_children, "c2pa.signature", JUMBF_TYPE_C2PA_SIGNATURE, signature.data, signature.size) ||
             make_jumb(out, "urn:c2pa:newos-dev", JUMBF_TYPE_C2PA_MANIFEST, manifest_children.data, manifest_children.size);
    tool_byte_buffer_free(&hash_assertion);
    tool_byte_buffer_free(&actions_assertion);
    tool_byte_buffer_free(&hash_assertion_box);
    tool_byte_buffer_free(&actions_assertion_box);
    tool_byte_buffer_free(&claim);
    tool_byte_buffer_free(&signature);
    tool_byte_buffer_free(&assertions_children);
    tool_byte_buffer_free(&manifest_children);
    return result;
}

static int make_store(const unsigned char *input, size_t input_size, const char *generator, const char *const *actions, unsigned int action_count, ByteBuffer *out) {
    ByteBuffer manifest;
    int result;
    tool_byte_buffer_init(&manifest);
    result = make_manifest(input, input_size, generator, actions, action_count, &manifest) || make_jumb(out, "c2pa", JUMBF_TYPE_C2PA_STORE, manifest.data, manifest.size);
    tool_byte_buffer_free(&manifest);
    return result;
}

static int insert_png_cabx(const unsigned char *data, size_t size, const unsigned char *payload, size_t payload_size, ByteBuffer *out) {
    static const unsigned char sig[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    size_t offset = 8U;
    if (size < 8U || !tool_bytes_equal(data, sig, sizeof(sig))) return -1;
    while (offset + 12U <= size) {
        unsigned int length = tool_read_u32_be(data + offset);
        const unsigned char *type = data + offset + 4U;
        if ((size_t)length > size - offset - 12U) return -1;
        if (tool_bytes_equal_text(type, "IEND", 4U)) {
            unsigned int crc;
            if (tool_byte_buffer_append(out, data, offset) != 0) return -1;
            if (tool_byte_buffer_append_u32_be(out, (unsigned int)payload_size) || tool_byte_buffer_append(out, "caBX", 4U) || tool_byte_buffer_append(out, payload, payload_size)) return -1;
            crc = compression_crc32(out->data + out->size - payload_size - 4U, payload_size + 4U);
            if (tool_byte_buffer_append_u32_be(out, crc) || tool_byte_buffer_append(out, data + offset, size - offset)) return -1;
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
        segment_size = tool_read_u16_be(data + offset);
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
    return tool_byte_buffer_append(out, data, insert_at) ||
           tool_byte_buffer_append_byte(out, 0xffU) || tool_byte_buffer_append_byte(out, 0xebU) ||
           tool_byte_buffer_append_u16_be(out, segment_payload_size + 2U) ||
           tool_byte_buffer_append(out, "JP", 2U) || tool_byte_buffer_append_byte(out, 0U) || tool_byte_buffer_append_byte(out, 1U) ||
           tool_byte_buffer_append(out, jumbf, jumbf_size) ||
           tool_byte_buffer_append(out, data + insert_at, size - insert_at);
}

static int run_add(int argc, char **argv, int arg_index) {
    const char *output_path = 0;
    const char *input_path = 0;
    const char *generator = "newos c2pa dev signer";
    const char *actions[C2PA_MAX_ACTIONS];
    unsigned int action_count = 0U;
    int dev_key = 0;
    unsigned char *input = 0;
    size_t input_size = 0U;
    ImageInfo info;
    ByteBuffer store;
    ByteBuffer output;
    int result = -1;

    tool_byte_buffer_init(&store);
    tool_byte_buffer_init(&output);
    while (arg_index < argc) {
        const char *arg = argv[arg_index];
        if (rt_strcmp(arg, "--dev-key") == 0) { dev_key = 1; arg_index += 1; continue; }
        if ((rt_strcmp(arg, "-o") == 0 || rt_strcmp(arg, "--output") == 0) && arg_index + 1 < argc) { output_path = argv[arg_index + 1]; arg_index += 2; continue; }
        if (rt_strcmp(arg, "--claim-generator") == 0 && arg_index + 1 < argc) { generator = argv[arg_index + 1]; arg_index += 2; continue; }
        if (rt_strcmp(arg, "--action") == 0 && arg_index + 1 < argc) {
            if (action_count >= C2PA_MAX_ACTIONS) {
                tool_write_error("c2pa", "too many actions", "");
                goto done;
            }
            actions[action_count++] = argv[arg_index + 1];
            arg_index += 2;
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0') { tool_write_error("c2pa", "unknown option: ", arg); goto done; }
        input_path = arg;
        arg_index += 1;
    }
    if (!dev_key || output_path == 0 || input_path == 0) {
        print_usage();
        goto done;
    }
    if (prepend_default_created_action(actions, &action_count) != 0) {
        tool_write_error("c2pa", "too many actions", "");
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
    if (make_store(input, input_size, generator, actions, action_count, &store) != 0) goto done;
    if (info.format == IMAGE_FORMAT_PNG) result = insert_png_cabx(input, input_size, store.data, store.size, &output);
    else result = insert_jpeg_app11(input, input_size, store.data, store.size, &output);
    if (result == 0) result = c2pa_write_file(output_path, output.data, output.size);
done:
    rt_free(input);
    tool_byte_buffer_free(&store);
    tool_byte_buffer_free(&output);
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