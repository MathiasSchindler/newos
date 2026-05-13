#include "image_internal.h"

static int c2pa_token_at(const unsigned char *data, size_t size, size_t offset, const char *token) {
    size_t index = 0U;

    while (token[index] != '\0') {
        if (offset + index >= size || data[offset + index] != (unsigned char)token[index]) {
            return 0;
        }
        index += 1U;
    }
    return 1;
}

static unsigned int c2pa_count_token(const unsigned char *data, size_t size, const char *token) {
    size_t offset;
    unsigned int count = 0U;
    size_t token_length = 0U;

    while (token[token_length] != '\0') {
        token_length += 1U;
    }
    if (token_length == 0U || size < token_length) {
        return 0U;
    }
    for (offset = 0U; offset + token_length <= size; ++offset) {
        if (c2pa_token_at(data, size, offset, token)) {
            count += 1U;
            offset += token_length - 1U;
        }
    }
    return count;
}

static int c2pa_payload_has_jumbf(const unsigned char *data, size_t size) {
    return c2pa_count_token(data, size, "jumb") > 0U &&
           c2pa_count_token(data, size, "jumd") > 0U &&
           c2pa_count_token(data, size, "c2pa") > 0U;
}

static void c2pa_scan_common(const unsigned char *data, size_t size, ImageC2paInfo *info) {
    info->jumbf_box_count = c2pa_count_token(data, size, "jumb");
    info->manifest_count = c2pa_count_token(data, size, "urn:c2pa:");
    info->claim_count = c2pa_count_token(data, size, "c2pa.claim");
    info->assertion_store_count = c2pa_count_token(data, size, "c2pa.assertions");
    info->assertion_count = c2pa_count_token(data, size, "c2pa.");
    info->signature_count = c2pa_count_token(data, size, "c2pa.signature");
    info->ingredient_count = c2pa_count_token(data, size, "c2pa.ingredient");
    info->has_manifest_store = c2pa_payload_has_jumbf(data, size);
}

static int c2pa_scan_png_carriers(const unsigned char *data, size_t size, ImageC2paInfo *info) {
    static const unsigned char signature[8] = {0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    size_t offset = 8U;

    if (size < 8U || !image_byte_arrays_equal(data, signature, sizeof(signature))) {
        return 0;
    }
    while (offset + 12U <= size) {
        unsigned int length = image_read_u32_be(data + offset);
        const unsigned char *type = data + offset + 4U;
        size_t payload = offset + 8U;

        if ((size_t)length > size - offset - 12U) {
            break;
        }
        if (image_bytes_equal(type, "caBX", 4U)) {
            info->carrier_count += 1U;
            info->carrier = info->carrier != 0 ? "mixed" : "PNG caBX";
            if (c2pa_payload_has_jumbf(data + payload, (size_t)length)) {
                info->recognized_carrier_count += 1U;
            }
        }
        offset += 12U + (size_t)length;
        if (image_bytes_equal(type, "IEND", 4U)) {
            break;
        }
    }
    return info->carrier_count > 0U;
}

static int c2pa_scan_jpeg_carriers(const unsigned char *data, size_t size, ImageC2paInfo *info) {
    size_t offset = 2U;

    if (size < 3U || data[0] != 0xffU || data[1] != 0xd8U || data[2] != 0xffU) {
        return 0;
    }
    while (offset + 3U < size) {
        unsigned char marker;
        unsigned int segment_size;
        size_t payload;
        size_t payload_size;

        while (offset < size && data[offset] != 0xffU) {
            offset += 1U;
        }
        while (offset < size && data[offset] == 0xffU) {
            offset += 1U;
        }
        if (offset >= size) {
            break;
        }
        marker = data[offset++];
        if (marker == 0xd9U || marker == 0xdaU) {
            break;
        }
        if (marker == 0x01U || (marker >= 0xd0U && marker <= 0xd7U)) {
            continue;
        }
        if (offset + 2U > size) {
            break;
        }
        segment_size = image_read_u16_be(data + offset);
        if (segment_size < 2U || (size_t)segment_size > size - offset) {
            break;
        }
        payload = offset + 2U;
        payload_size = (size_t)segment_size - 2U;
        if (marker == 0xebU && payload_size >= 12U &&
            image_bytes_equal(data + payload, "JP", 2U) &&
            data[payload + 2U] == 0U && data[payload + 3U] == 1U &&
            c2pa_payload_has_jumbf(data + payload, payload_size)) {
            info->carrier_count += 1U;
            info->recognized_carrier_count += 1U;
            info->carrier = info->carrier != 0 ? "mixed" : "JPEG APP11 JUMBF";
        }
        offset += (size_t)segment_size;
    }
    return info->carrier_count > 0U;
}

void image_c2pa_info_init(ImageC2paInfo *info) {
    if (info == 0) {
        return;
    }
    info->present = 0;
    info->has_manifest_store = 0;
    info->carrier = 0;
    info->carrier_count = 0U;
    info->recognized_carrier_count = 0U;
    info->jumbf_box_count = 0U;
    info->manifest_count = 0U;
    info->claim_count = 0U;
    info->assertion_store_count = 0U;
    info->assertion_count = 0U;
    info->signature_count = 0U;
    info->ingredient_count = 0U;
    info->status = "absent";
}

int image_c2pa_analyze(const unsigned char *data, size_t size, ImageC2paInfo *info) {
    int carrier_found;

    if (info == 0) {
        return -1;
    }
    image_c2pa_info_init(info);
    if (data == 0 || size == 0U) {
        return -1;
    }

    carrier_found = c2pa_scan_png_carriers(data, size, info);
    carrier_found = c2pa_scan_jpeg_carriers(data, size, info) || carrier_found;
    c2pa_scan_common(data, size, info);

    if (!carrier_found && info->jumbf_box_count > 0U && c2pa_count_token(data, size, "c2pa") > 0U) {
        info->carrier = "embedded JUMBF";
        info->carrier_count = 1U;
        info->recognized_carrier_count = info->has_manifest_store ? 1U : 0U;
        carrier_found = 1;
    }

    if (!carrier_found && info->jumbf_box_count == 0U && c2pa_count_token(data, size, "c2pa") == 0U) {
        return 0;
    }

    info->present = 1;
    if (info->carrier == 0) {
        info->carrier = "unknown";
    }
    if (info->has_manifest_store && info->claim_count > 0U && info->signature_count > 0U) {
        info->status = "recognized C2PA manifest store";
    } else if (info->has_manifest_store) {
        info->status = "recognized C2PA JUMBF data";
    } else {
        info->status = "C2PA markers found; manifest store not recognized";
    }
    return 0;
}
