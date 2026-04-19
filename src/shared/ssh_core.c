#include "ssh_core.h"
#include "crypto/sha256.h"
#include "runtime.h"

static int ssh_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int ssh_string_equals_cstr(const unsigned char *data, size_t length, const char *text) {
    size_t i = 0;

    if (data == 0 || text == 0) {
        return 0;
    }

    while (i < length && text[i] != '\0') {
        if ((char)data[i] != text[i]) {
            return 0;
        }
        i += 1U;
    }

    return i == length && text[i] == '\0';
}

static int ssh_copy_range(char *dst, size_t dst_size, const char *src, size_t src_length) {
    size_t i;

    if (dst == 0 || src == 0 || dst_size == 0 || src_length + 1U > dst_size) {
        return -1;
    }

    for (i = 0; i < src_length; ++i) {
        dst[i] = src[i];
    }
    dst[src_length] = '\0';
    return 0;
}

static int ssh_contains_colon(const char *text) {
    size_t i = 0;

    while (text != 0 && text[i] != '\0') {
        if (text[i] == ':') {
            return 1;
        }
        i += 1U;
    }
    return 0;
}

void ssh_cursor_init(SshCursor *cursor, const void *data, size_t length) {
    if (cursor == 0) {
        return;
    }

    cursor->data = (const unsigned char *)data;
    cursor->length = length;
    cursor->offset = 0U;
}

size_t ssh_cursor_remaining(const SshCursor *cursor) {
    if (cursor == 0 || cursor->offset > cursor->length) {
        return 0U;
    }
    return cursor->length - cursor->offset;
}

int ssh_cursor_take_u8(SshCursor *cursor, unsigned char *value_out) {
    if (cursor == 0 || value_out == 0 || ssh_cursor_remaining(cursor) < 1U) {
        return -1;
    }

    *value_out = cursor->data[cursor->offset];
    cursor->offset += 1U;
    return 0;
}

int ssh_cursor_take_u32(SshCursor *cursor, unsigned int *value_out) {
    const unsigned char *data;

    if (value_out == 0 || ssh_cursor_take_bytes(cursor, &data, 4U) != 0) {
        return -1;
    }

    *value_out = ((unsigned int)data[0] << 24) |
                 ((unsigned int)data[1] << 16) |
                 ((unsigned int)data[2] << 8) |
                 (unsigned int)data[3];
    return 0;
}

int ssh_cursor_take_bytes(SshCursor *cursor, const unsigned char **data_out, size_t length) {
    if (cursor == 0 || data_out == 0 || ssh_cursor_remaining(cursor) < length) {
        return -1;
    }

    *data_out = cursor->data + cursor->offset;
    cursor->offset += length;
    return 0;
}

int ssh_cursor_take_string(SshCursor *cursor, SshStringView *view_out) {
    unsigned int length = 0;
    const unsigned char *data = 0;

    if (view_out == 0 || ssh_cursor_take_u32(cursor, &length) != 0) {
        return -1;
    }
    if (ssh_cursor_take_bytes(cursor, &data, (size_t)length) != 0) {
        return -1;
    }

    view_out->data = data;
    view_out->length = (size_t)length;
    return 0;
}

void ssh_builder_init(SshBuilder *builder, unsigned char *buffer, size_t capacity) {
    if (builder == 0) {
        return;
    }

    builder->data = buffer;
    builder->capacity = capacity;
    builder->length = 0U;
}

int ssh_builder_put_u8(SshBuilder *builder, unsigned char value) {
    if (builder == 0 || builder->data == 0 || builder->length + 1U > builder->capacity) {
        return -1;
    }

    builder->data[builder->length++] = value;
    return 0;
}

int ssh_builder_put_u32(SshBuilder *builder, unsigned int value) {
    if (builder == 0 || builder->data == 0 || builder->length + 4U > builder->capacity) {
        return -1;
    }

    builder->data[builder->length++] = (unsigned char)(value >> 24);
    builder->data[builder->length++] = (unsigned char)(value >> 16);
    builder->data[builder->length++] = (unsigned char)(value >> 8);
    builder->data[builder->length++] = (unsigned char)value;
    return 0;
}

int ssh_builder_put_bytes(SshBuilder *builder, const unsigned char *data, size_t length) {
    if (builder == 0 || builder->data == 0 || builder->length + length > builder->capacity) {
        return -1;
    }
    if (length != 0U && data == 0) {
        return -1;
    }

    if (length != 0U) {
        memcpy(builder->data + builder->length, data, length);
        builder->length += length;
    }
    return 0;
}

int ssh_builder_put_string(SshBuilder *builder, const unsigned char *data, size_t length) {
    if (length > 0xffffffffU) {
        return -1;
    }
    if (ssh_builder_put_u32(builder, (unsigned int)length) != 0) {
        return -1;
    }
    return ssh_builder_put_bytes(builder, data, length);
}

int ssh_builder_put_cstring(SshBuilder *builder, const char *text) {
    if (text == 0) {
        return ssh_builder_put_string(builder, (const unsigned char *)"", 0U);
    }
    return ssh_builder_put_string(builder, (const unsigned char *)text, rt_strlen(text));
}

void ssh_profile_init_default_client(SshAlgorithmProfile *profile) {
    if (profile == 0) {
        return;
    }

    rt_copy_string(profile->kex_algorithms, sizeof(profile->kex_algorithms), "curve25519-sha256,ext-info-c,kex-strict-c-v00@openssh.com");
    rt_copy_string(profile->host_key_algorithms, sizeof(profile->host_key_algorithms), "ssh-ed25519");
    rt_copy_string(profile->ciphers_c_to_s, sizeof(profile->ciphers_c_to_s), "chacha20-poly1305@openssh.com");
    rt_copy_string(profile->ciphers_s_to_c, sizeof(profile->ciphers_s_to_c), "chacha20-poly1305@openssh.com");
    rt_copy_string(profile->macs_c_to_s, sizeof(profile->macs_c_to_s), "hmac-sha2-256");
    rt_copy_string(profile->macs_s_to_c, sizeof(profile->macs_s_to_c), "hmac-sha2-256");
    rt_copy_string(profile->compression_c_to_s, sizeof(profile->compression_c_to_s), "none");
    rt_copy_string(profile->compression_s_to_c, sizeof(profile->compression_s_to_c), "none");
}

void ssh_profile_init_default_server(SshAlgorithmProfile *profile) {
    if (profile == 0) {
        return;
    }

    rt_copy_string(profile->kex_algorithms, sizeof(profile->kex_algorithms), "curve25519-sha256,kex-strict-s-v00@openssh.com");
    rt_copy_string(profile->host_key_algorithms, sizeof(profile->host_key_algorithms), "ssh-ed25519");
    rt_copy_string(profile->ciphers_c_to_s, sizeof(profile->ciphers_c_to_s), "chacha20-poly1305@openssh.com");
    rt_copy_string(profile->ciphers_s_to_c, sizeof(profile->ciphers_s_to_c), "chacha20-poly1305@openssh.com");
    rt_copy_string(profile->macs_c_to_s, sizeof(profile->macs_c_to_s), "hmac-sha2-256");
    rt_copy_string(profile->macs_s_to_c, sizeof(profile->macs_s_to_c), "hmac-sha2-256");
    rt_copy_string(profile->compression_c_to_s, sizeof(profile->compression_c_to_s), "none");
    rt_copy_string(profile->compression_s_to_c, sizeof(profile->compression_s_to_c), "none");
}

int ssh_build_kexinit_payload(
    const SshAlgorithmProfile *profile,
    const unsigned char cookie[SSH_KEX_COOKIE_SIZE],
    unsigned char *buffer,
    size_t buffer_size,
    size_t *length_out
) {
    SshBuilder builder;

    if (profile == 0 || cookie == 0 || buffer == 0 || length_out == 0) {
        return -1;
    }

    ssh_builder_init(&builder, buffer, buffer_size);
    if (ssh_builder_put_u8(&builder, SSH_MSG_KEXINIT) != 0 ||
        ssh_builder_put_bytes(&builder, cookie, SSH_KEX_COOKIE_SIZE) != 0 ||
        ssh_builder_put_cstring(&builder, profile->kex_algorithms) != 0 ||
        ssh_builder_put_cstring(&builder, profile->host_key_algorithms) != 0 ||
        ssh_builder_put_cstring(&builder, profile->ciphers_c_to_s) != 0 ||
        ssh_builder_put_cstring(&builder, profile->ciphers_s_to_c) != 0 ||
        ssh_builder_put_cstring(&builder, profile->macs_c_to_s) != 0 ||
        ssh_builder_put_cstring(&builder, profile->macs_s_to_c) != 0 ||
        ssh_builder_put_cstring(&builder, profile->compression_c_to_s) != 0 ||
        ssh_builder_put_cstring(&builder, profile->compression_s_to_c) != 0 ||
        ssh_builder_put_cstring(&builder, "") != 0 ||
        ssh_builder_put_cstring(&builder, "") != 0 ||
        ssh_builder_put_u8(&builder, 0U) != 0 ||
        ssh_builder_put_u32(&builder, 0U) != 0) {
        return -1;
    }

    *length_out = builder.length;
    return 0;
}

int ssh_parse_kexinit_payload(const unsigned char *payload, size_t payload_length, SshKexInitMessage *message_out) {
    SshCursor cursor;
    unsigned char message_id;
    const unsigned char *cookie;

    if (payload == 0 || message_out == 0 || payload_length < 17U) {
        return -1;
    }

    ssh_cursor_init(&cursor, payload, payload_length);
    if (ssh_cursor_take_u8(&cursor, &message_id) != 0 || message_id != SSH_MSG_KEXINIT) {
        return -1;
    }
    if (ssh_cursor_take_bytes(&cursor, &cookie, SSH_KEX_COOKIE_SIZE) != 0) {
        return -1;
    }

    memcpy(message_out->cookie, cookie, SSH_KEX_COOKIE_SIZE);
    if (ssh_cursor_take_string(&cursor, &message_out->kex_algorithms) != 0 ||
        ssh_cursor_take_string(&cursor, &message_out->host_key_algorithms) != 0 ||
        ssh_cursor_take_string(&cursor, &message_out->ciphers_c_to_s) != 0 ||
        ssh_cursor_take_string(&cursor, &message_out->ciphers_s_to_c) != 0 ||
        ssh_cursor_take_string(&cursor, &message_out->macs_c_to_s) != 0 ||
        ssh_cursor_take_string(&cursor, &message_out->macs_s_to_c) != 0 ||
        ssh_cursor_take_string(&cursor, &message_out->compression_c_to_s) != 0 ||
        ssh_cursor_take_string(&cursor, &message_out->compression_s_to_c) != 0 ||
        ssh_cursor_take_string(&cursor, &message_out->languages_c_to_s) != 0 ||
        ssh_cursor_take_string(&cursor, &message_out->languages_s_to_c) != 0 ||
        ssh_cursor_take_u8(&cursor, &message_out->first_kex_packet_follows) != 0 ||
        ssh_cursor_take_u32(&cursor, &message_out->reserved) != 0) {
        return -1;
    }

    return 0;
}

int ssh_validate_banner_line(const char *line) {
    size_t length;

    if (line == 0) {
        return 0;
    }

    length = rt_strlen(line);
    if (length == 0U || length >= SSH_BANNER_CAPACITY) {
        return 0;
    }

    if (length >= 8U &&
        line[0] == 'S' && line[1] == 'S' && line[2] == 'H' && line[3] == '-' &&
        line[4] == '2' && line[5] == '.' && line[6] == '0' && line[7] == '-') {
        return 1;
    }
    if (length >= 9U &&
        line[0] == 'S' && line[1] == 'S' && line[2] == 'H' && line[3] == '-' &&
        line[4] == '1' && line[5] == '.' && line[6] == '9' && line[7] == '9' && line[8] == '-') {
        return 1;
    }
    return 0;
}

int ssh_parse_destination(const char *text, const char *default_user, unsigned int default_port, SshDestination *out) {
    const char *host_text;
    const char *at;
    const char *closing;
    const char *port_text = 0;
    const char *last_colon = 0;
    size_t host_length;
    unsigned long long parsed_port = default_port == 0U ? SSH_DEFAULT_PORT : default_port;
    size_t i;

    if (text == 0 || text[0] == '\0' || out == 0) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    if (default_user != 0 && default_user[0] != '\0') {
        rt_copy_string(out->user, sizeof(out->user), default_user);
        out->has_user = 1;
    }

    at = 0;
    for (i = 0; text[i] != '\0'; ++i) {
        if (text[i] == '@') {
            at = text + i;
            break;
        }
    }

    host_text = text;
    if (at != 0) {
        if (ssh_copy_range(out->user, sizeof(out->user), text, (size_t)(at - text)) != 0) {
            return -1;
        }
        out->has_user = 1;
        host_text = at + 1;
    }

    if (host_text[0] == '[') {
        closing = host_text + 1;
        while (*closing != '\0' && *closing != ']') {
            closing += 1;
        }
        if (*closing != ']') {
            return -1;
        }
        host_length = (size_t)(closing - (host_text + 1));
        if (ssh_copy_range(out->host, sizeof(out->host), host_text + 1, host_length) != 0) {
            return -1;
        }
        if (closing[1] == ':') {
            port_text = closing + 2;
        } else if (closing[1] != '\0') {
            return -1;
        }
    } else {
        for (i = 0; host_text[i] != '\0'; ++i) {
            if (host_text[i] == ':') {
                if (last_colon != 0) {
                    last_colon = 0;
                    break;
                }
                last_colon = host_text + i;
            }
        }

        if (last_colon != 0) {
            host_length = (size_t)(last_colon - host_text);
            port_text = last_colon + 1;
        } else {
            host_length = rt_strlen(host_text);
        }

        if (host_length == 0U || ssh_copy_range(out->host, sizeof(out->host), host_text, host_length) != 0) {
            return -1;
        }
    }

    if (port_text != 0 && port_text[0] != '\0') {
        if (rt_parse_uint(port_text, &parsed_port) != 0 || parsed_port == 0ULL || parsed_port > 65535ULL) {
            return -1;
        }
    }

    out->port = (unsigned int)parsed_port;
    return 0;
}

int ssh_format_destination(const SshDestination *destination, int include_user, char *buffer, size_t buffer_size) {
    char port_text[32];
    size_t used = 0;
    int needs_brackets;

    if (destination == 0 || buffer == 0 || buffer_size == 0) {
        return -1;
    }

    buffer[0] = '\0';
    if (include_user && destination->has_user) {
        size_t user_length = rt_strlen(destination->user);
        if (user_length + 1U >= buffer_size) {
            return -1;
        }
        memcpy(buffer, destination->user, user_length);
        buffer[user_length] = '@';
        used = user_length + 1U;
        buffer[used] = '\0';
    }

    needs_brackets = ssh_contains_colon(destination->host);
    if (needs_brackets) {
        if (used + 1U >= buffer_size) {
            return -1;
        }
        buffer[used++] = '[';
        buffer[used] = '\0';
    }

    if (used + rt_strlen(destination->host) + 1U >= buffer_size) {
        return -1;
    }
    rt_copy_string(buffer + used, buffer_size - used, destination->host);
    used += rt_strlen(destination->host);

    if (needs_brackets) {
        if (used + 1U >= buffer_size) {
            return -1;
        }
        buffer[used++] = ']';
        buffer[used] = '\0';
    }

    if (destination->port != 0U && destination->port != SSH_DEFAULT_PORT) {
        rt_unsigned_to_string((unsigned long long)destination->port, port_text, sizeof(port_text));
        if (used + 1U + rt_strlen(port_text) + 1U >= buffer_size) {
            return -1;
        }
        buffer[used++] = ':';
        buffer[used] = '\0';
        rt_copy_string(buffer + used, buffer_size - used, port_text);
    }

    return 0;
}

int ssh_name_list_contains(const SshStringView *list, const char *name) {
    size_t start = 0;
    size_t i;

    if (list == 0 || name == 0) {
        return 0;
    }

    for (i = 0; i <= list->length; ++i) {
        if (i == list->length || list->data[i] == ',') {
            if (ssh_string_equals_cstr(list->data + start, i - start, name)) {
                return 1;
            }
            start = i + 1U;
        }
    }

    return 0;
}

int ssh_select_algorithm(const char *preferred_csv, const SshStringView *peer_list, char *buffer, size_t buffer_size) {
    const char *start;
    const char *end;
    size_t token_length;
    size_t i;

    if (preferred_csv == 0 || peer_list == 0 || buffer == 0 || buffer_size == 0) {
        return -1;
    }

    start = preferred_csv;
    while (*start != '\0') {
        end = start;
        while (*end != '\0' && *end != ',') {
            end += 1;
        }
        token_length = (size_t)(end - start);
        if (token_length != 0U && token_length + 1U <= buffer_size) {
            for (i = 0; i < token_length; ++i) {
                buffer[i] = start[i];
            }
            buffer[token_length] = '\0';
            if (ssh_name_list_contains(peer_list, buffer)) {
                return 0;
            }
        }
        start = (*end == ',') ? end + 1 : end;
    }

    return -1;
}

int ssh_base64_encode(const unsigned char *data, size_t length, char *buffer, size_t buffer_size, size_t *length_out) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0;
    size_t used = 0;

    if ((data == 0 && length != 0U) || buffer == 0 || buffer_size == 0) {
        return -1;
    }

    while (i < length) {
        unsigned int value = 0U;
        unsigned int remain = (unsigned int)(length - i);

        value |= (unsigned int)data[i] << 16;
        if (remain > 1U) {
            value |= (unsigned int)data[i + 1U] << 8;
        }
        if (remain > 2U) {
            value |= (unsigned int)data[i + 2U];
        }

        if (used + 4U >= buffer_size) {
            return -1;
        }
        buffer[used++] = alphabet[(value >> 18) & 0x3fU];
        buffer[used++] = alphabet[(value >> 12) & 0x3fU];
        buffer[used++] = remain > 1U ? alphabet[(value >> 6) & 0x3fU] : '=';
        buffer[used++] = remain > 2U ? alphabet[value & 0x3fU] : '=';
        i += remain > 2U ? 3U : (size_t)remain;
    }

    if (used >= buffer_size) {
        return -1;
    }
    buffer[used] = '\0';
    if (length_out != 0) {
        *length_out = used;
    }
    return 0;
}

int ssh_base64_decode(const char *text, unsigned char *buffer, size_t buffer_size, size_t *length_out) {
    unsigned int bits = 0U;
    unsigned int bit_count = 0U;
    size_t out_len = 0U;
    size_t i = 0U;

    if (text == 0 || (buffer == 0 && buffer_size != 0U)) {
        return -1;
    }

    while (text[i] != '\0') {
        unsigned int value;
        char ch = text[i++];

        if (ssh_is_space(ch)) {
            continue;
        }
        if (ch == '=') {
            break;
        }
        if (ch >= 'A' && ch <= 'Z') {
            value = (unsigned int)(ch - 'A');
        } else if (ch >= 'a' && ch <= 'z') {
            value = 26U + (unsigned int)(ch - 'a');
        } else if (ch >= '0' && ch <= '9') {
            value = 52U + (unsigned int)(ch - '0');
        } else if (ch == '+') {
            value = 62U;
        } else if (ch == '/') {
            value = 63U;
        } else {
            return -1;
        }

        bits = (bits << 6) | value;
        bit_count += 6U;
        while (bit_count >= 8U) {
            bit_count -= 8U;
            if (out_len >= buffer_size) {
                return -1;
            }
            buffer[out_len++] = (unsigned char)((bits >> bit_count) & 0xffU);
        }
    }

    if (length_out != 0) {
        *length_out = out_len;
    }
    return 0;
}

int ssh_format_fingerprint_sha256(const unsigned char *data, size_t length, char *buffer, size_t buffer_size) {
    unsigned char digest[CRYPTO_SHA256_DIGEST_SIZE];
    char encoded[64];
    size_t encoded_length = 0U;
    size_t i;

    if ((data == 0 && length != 0U) || buffer == 0 || buffer_size == 0U) {
        return -1;
    }

    crypto_sha256_hash(data, length, digest);
    if (ssh_base64_encode(digest, sizeof(digest), encoded, sizeof(encoded), &encoded_length) != 0) {
        return -1;
    }
    if (7U + encoded_length + 1U > buffer_size) {
        return -1;
    }

    memcpy(buffer, "SHA256:", 7U);
    for (i = 0; i < encoded_length; ++i) {
        buffer[7U + i] = encoded[i];
    }
    buffer[7U + encoded_length] = '\0';
    return 0;
}
