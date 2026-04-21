#include "ssh_client.h"

#include "crypto/chacha20_poly1305.h"
#include "crypto/crypto_util.h"
#include "crypto/curve25519.h"
#include "crypto/ed25519.h"
#include "crypto/sha256.h"
#include "crypto/ssh_kdf.h"
#include "platform.h"
#include "runtime.h"
#include "ssh_core.h"
#include "ssh_known_hosts.h"
#include "tool_util.h"

#include <stddef.h>

#define SSH_CLIENT_BANNER_TEXT "SSH-2.0-newos_ssh_0.1"
#define SSH_CLIENT_BANNER_WIRE "SSH-2.0-newos_ssh_0.1\r\n"
#define SSH_PACKET_BUFFER_CAPACITY 8192U
#define SSH_PASSWORD_CAPACITY 256U
#define SSH_IDENTITY_PATH_CAPACITY 1024U

typedef struct {
    unsigned char key_c_to_s[64];
    unsigned char key_s_to_c[64];
} SshTransportKeys;

typedef struct {
    SshStringView host_key_blob;
    SshStringView server_public_key;
    SshStringView signature_blob;
} SshEcdhReply;

typedef struct {
    unsigned int local_id;
    unsigned int remote_id;
    unsigned int local_window;
    unsigned int remote_window;
    unsigned int max_packet;
    int eof_sent;
    int close_sent;
} SshChannelState;

typedef struct {
    unsigned char seed[32];
    unsigned char public_key[32];
    char source_path[SSH_IDENTITY_PATH_CAPACITY];
    int loaded;
} SshIdentity;

static int ssh_write_all(int fd, const void *buffer, size_t count) {
    const unsigned char *data = (const unsigned char *)buffer;
    size_t offset = 0U;

    while (offset < count) {
        long written = platform_write(fd, data + offset, count - offset);
        if (written <= 0) {
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

static int ssh_read_exact(int fd, void *buffer, size_t count) {
    unsigned char *data = (unsigned char *)buffer;
    size_t offset = 0U;

    while (offset < count) {
        long bytes = platform_read(fd, data + offset, count - offset);
        if (bytes <= 0) {
            return -1;
        }
        offset += (size_t)bytes;
    }
    return 0;
}

static int ssh_discard_exact(int fd, size_t count) {
    unsigned char buffer[256];
    size_t remaining = count;

    while (remaining > 0U) {
        size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        if (ssh_read_exact(fd, buffer, chunk) != 0) {
            return -1;
        }
        remaining -= chunk;
    }
    return 0;
}

static void ssh_store_be32(unsigned char out[4], unsigned int value) {
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static unsigned int ssh_read_be32(const unsigned char in[4]) {
    return ((unsigned int)in[0] << 24) |
           ((unsigned int)in[1] << 16) |
           ((unsigned int)in[2] << 8) |
           (unsigned int)in[3];
}

static int ssh_copy_view_text(const SshStringView *view, char *buffer, size_t buffer_size) {
    size_t i;

    if (view == 0 || buffer == 0 || buffer_size == 0U || view->length + 1U > buffer_size) {
        return -1;
    }
    for (i = 0; i < view->length; ++i) {
        buffer[i] = (char)view->data[i];
    }
    buffer[view->length] = '\0';
    return 0;
}

static int ssh_view_equals_text(const SshStringView *view, const char *text) {
    size_t i = 0U;

    if (view == 0 || text == 0) {
        return 0;
    }
    while (i < view->length && text[i] != '\0') {
        if ((char)view->data[i] != text[i]) {
            return 0;
        }
        i += 1U;
    }
    return i == view->length && text[i] == '\0';
}

static int ssh_find_text_span(const char *text, size_t text_len, const char *needle, size_t *pos_out) {
    size_t needle_len = rt_strlen(needle);
    size_t i = 0U;

    if (text == 0 || needle == 0 || pos_out == 0 || needle_len == 0U || needle_len > text_len) {
        return -1;
    }

    while (i + needle_len <= text_len) {
        size_t j = 0U;
        while (j < needle_len && text[i + j] == needle[j]) {
            j += 1U;
        }
        if (j == needle_len) {
            *pos_out = i;
            return 0;
        }
        i += 1U;
    }
    return -1;
}

static int ssh_parse_ed25519_public_key_blob(const SshStringView *blob, unsigned char public_key[32]) {
    SshCursor cursor;
    SshStringView algorithm;
    SshStringView key;
    size_t i;

    if (blob == 0 || public_key == 0) {
        return -1;
    }

    ssh_cursor_init(&cursor, blob->data, blob->length);
    if (ssh_cursor_take_string(&cursor, &algorithm) != 0 ||
        ssh_cursor_take_string(&cursor, &key) != 0 ||
        !ssh_view_equals_text(&algorithm, "ssh-ed25519") ||
        key.length != 32U) {
        return -1;
    }

    for (i = 0; i < 32U; ++i) {
        public_key[i] = key.data[i];
    }
    return 0;
}

static int ssh_parse_ed25519_signature_blob(const SshStringView *blob, unsigned char signature[64]) {
    SshCursor cursor;
    SshStringView algorithm;
    SshStringView sig;
    size_t i;

    if (blob == 0 || signature == 0) {
        return -1;
    }

    ssh_cursor_init(&cursor, blob->data, blob->length);
    if (ssh_cursor_take_string(&cursor, &algorithm) != 0 ||
        ssh_cursor_take_string(&cursor, &sig) != 0 ||
        !ssh_view_equals_text(&algorithm, "ssh-ed25519") ||
        sig.length != 64U) {
        return -1;
    }

    for (i = 0; i < 64U; ++i) {
        signature[i] = sig.data[i];
    }
    return 0;
}

static int ssh_identity_path_is_symlink(const char *path) {
    char target[SSH_IDENTITY_PATH_CAPACITY];
    return path != 0 && platform_read_symlink(path, target, sizeof(target)) == 0;
}

static int ssh_identity_path_is_secure(const char *path) {
    PlatformDirEntry entry;
    PlatformIdentity identity;

    if (path == 0 || path[0] == '\0') {
        return -1;
    }
    if (platform_get_path_info(path, &entry) != 0) {
        return 1;
    }
    if (entry.is_dir || ssh_identity_path_is_symlink(path)) {
        return 0;
    }
    if ((entry.mode & 077U) != 0U) {
        return 0;
    }
    if (platform_get_identity(&identity) == 0 &&
        identity.uid != 0U &&
        entry.uid != identity.uid) {
        return 0;
    }
    return 1;
}

static int ssh_load_file(const char *path, unsigned char *buffer, size_t buffer_size, size_t *length_out) {
    int fd;
    size_t used = 0U;

    if (path == 0 || buffer == 0 || buffer_size == 0U || length_out == 0) {
        return -1;
    }
    if (ssh_identity_path_is_secure(path) == 0) {
        tool_write_error("ssh", "refusing insecure private key file ", path);
        return -1;
    }

    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }

    while (used < buffer_size) {
        long bytes = platform_read(fd, buffer + used, buffer_size - used);
        if (bytes < 0) {
            platform_close(fd);
            return -1;
        }
        if (bytes == 0) {
            break;
        }
        used += (size_t)bytes;
    }

    if (used == buffer_size) {
        unsigned char extra = 0U;
        long extra_bytes = platform_read(fd, &extra, 1U);
        if (extra_bytes < 0) {
            platform_close(fd);
            return -1;
        }
        if (extra_bytes > 0) {
            platform_close(fd);
            return -1;
        }
    }

    platform_close(fd);
    *length_out = used;
    return 0;
}

static int ssh_load_raw_ed25519_identity(const unsigned char *data, size_t data_len, SshIdentity *out) {
    size_t i;

    if (data == 0 || out == 0 || (data_len != 32U && data_len != 64U)) {
        return -1;
    }

    for (i = 0; i < 32U; ++i) {
        out->seed[i] = data[i];
    }
    if (crypto_ed25519_public_key_from_seed(out->public_key, out->seed) != 0) {
        return -1;
    }
    if (data_len == 64U && !crypto_constant_time_equal(out->public_key, data + 32U, 32U)) {
        return -1;
    }
    out->loaded = 1;
    return 0;
}

static int ssh_load_openssh_ed25519_identity(const char *text, size_t text_len, SshIdentity *out) {
    static const char begin_marker[] = "-----BEGIN OPENSSH PRIVATE KEY-----";
    static const char end_marker[] = "-----END OPENSSH PRIVATE KEY-----";
    static const char openssh_magic[] = "openssh-key-v1\0";
    unsigned char decoded[4096];
    unsigned char derived_public_key[32];
    SshCursor outer;
    SshCursor inner;
    SshStringView public_blob;
    SshStringView field;
    SshStringView public_key;
    SshStringView private_key;
    size_t begin_pos = 0U;
    size_t end_rel = 0U;
    size_t decoded_len = 0U;
    unsigned int key_count = 0U;
    unsigned int check_1 = 0U;
    unsigned int check_2 = 0U;
    size_t i;
    int status = -1;

    if (text == 0 || out == 0) {
        return -1;
    }
    if (ssh_find_text_span(text, text_len, begin_marker, &begin_pos) != 0) {
        return -1;
    }
    begin_pos += sizeof(begin_marker) - 1U;
    if (ssh_find_text_span(text + begin_pos, text_len - begin_pos, end_marker, &end_rel) != 0) {
        return -1;
    }
    if (ssh_base64_decode(text + begin_pos, decoded, sizeof(decoded), &decoded_len) != 0) {
        goto cleanup;
    }
    if (decoded_len < sizeof(openssh_magic) - 1U) {
        goto cleanup;
    }
    for (i = 0; i < sizeof(openssh_magic) - 1U; ++i) {
        if (decoded[i] != (unsigned char)openssh_magic[i]) {
            goto cleanup;
        }
    }

    ssh_cursor_init(&outer, decoded + (sizeof(openssh_magic) - 1U), decoded_len - (sizeof(openssh_magic) - 1U));
    if (ssh_cursor_take_string(&outer, &field) != 0 || !ssh_view_equals_text(&field, "none")) {
        goto cleanup;
    }
    if (ssh_cursor_take_string(&outer, &field) != 0 || !ssh_view_equals_text(&field, "none")) {
        goto cleanup;
    }
    if (ssh_cursor_take_string(&outer, &field) != 0 || field.length != 0U) {
        goto cleanup;
    }
    if (ssh_cursor_take_u32(&outer, &key_count) != 0 || key_count == 0U) {
        goto cleanup;
    }
    if (ssh_cursor_take_string(&outer, &public_blob) != 0 ||
        ssh_parse_ed25519_public_key_blob(&public_blob, out->public_key) != 0) {
        goto cleanup;
    }
    for (i = 1U; i < key_count; ++i) {
        if (ssh_cursor_take_string(&outer, &field) != 0) {
            goto cleanup;
        }
    }
    if (ssh_cursor_take_string(&outer, &field) != 0) {
        goto cleanup;
    }

    ssh_cursor_init(&inner, field.data, field.length);
    if (ssh_cursor_take_u32(&inner, &check_1) != 0 ||
        ssh_cursor_take_u32(&inner, &check_2) != 0 ||
        check_1 != check_2) {
        goto cleanup;
    }
    if (ssh_cursor_take_string(&inner, &field) != 0 || !ssh_view_equals_text(&field, "ssh-ed25519")) {
        goto cleanup;
    }
    if (ssh_cursor_take_string(&inner, &public_key) != 0 ||
        public_key.length != 32U ||
        !crypto_constant_time_equal(public_key.data, out->public_key, 32U)) {
        goto cleanup;
    }
    if (ssh_cursor_take_string(&inner, &private_key) != 0 || private_key.length != 64U) {
        goto cleanup;
    }
    for (i = 0; i < 32U; ++i) {
        out->seed[i] = private_key.data[i];
    }
    if (!crypto_constant_time_equal(private_key.data + 32U, out->public_key, 32U)) {
        goto cleanup;
    }
    if (crypto_ed25519_public_key_from_seed(derived_public_key, out->seed) != 0 ||
        !crypto_constant_time_equal(derived_public_key, out->public_key, 32U)) {
        goto cleanup;
    }

    out->loaded = 1;
    status = 0;

cleanup:
    crypto_secure_bzero(derived_public_key, sizeof(derived_public_key));
    crypto_secure_bzero(decoded, sizeof(decoded));
    return status;
}

static int ssh_try_load_identity(const char *identity_path, SshIdentity *identity) {
    unsigned char file_buffer[8192];
    size_t file_length = 0U;
    char default_path[SSH_IDENTITY_PATH_CAPACITY];
    const char *path = identity_path;

    if (identity == 0) {
        return -1;
    }
    rt_memset(identity, 0, sizeof(*identity));

    if ((path == 0 || path[0] == '\0') && platform_getenv("HOME") != 0) {
        char ssh_dir[SSH_IDENTITY_PATH_CAPACITY];
        if (rt_join_path(platform_getenv("HOME"), ".ssh", ssh_dir, sizeof(ssh_dir)) == 0 &&
            rt_join_path(ssh_dir, "id_ed25519", default_path, sizeof(default_path)) == 0) {
            path = default_path;
        }
    }
    if (path == 0 || path[0] == '\0') {
        return 0;
    }

    rt_copy_string(identity->source_path, sizeof(identity->source_path), path);
    if (ssh_load_file(path, file_buffer, sizeof(file_buffer), &file_length) != 0) {
        return identity_path != 0 && identity_path[0] != '\0' ? -1 : 0;
    }

    if ((file_length == 32U || file_length == 64U) &&
        ssh_load_raw_ed25519_identity(file_buffer, file_length, identity) == 0) {
        crypto_secure_bzero(file_buffer, sizeof(file_buffer));
        return 0;
    }

    if (ssh_load_openssh_ed25519_identity((const char *)file_buffer, file_length, identity) != 0) {
        crypto_secure_bzero(file_buffer, sizeof(file_buffer));
        rt_memset(identity, 0, sizeof(*identity));
        return identity_path != 0 && identity_path[0] != '\0' ? -1 : 0;
    }

    crypto_secure_bzero(file_buffer, sizeof(file_buffer));
    return 0;
}

static int ssh_prompt_password(const char *user, const char *host, char *password, size_t password_size) {
    PlatformTerminalState saved;
    int raw_enabled = 0;
    size_t used = 0U;
    char ch;

    if (password == 0 || password_size == 0U) {
        return -1;
    }

    password[0] = '\0';
    rt_write_cstr(2, "ssh: password for ");
    rt_write_cstr(2, user);
    rt_write_cstr(2, "@");
    rt_write_cstr(2, host);
    rt_write_cstr(2, ": ");

    if (platform_isatty(0)) {
        if (platform_terminal_enable_raw_mode(0, &saved) == 0) {
            raw_enabled = 1;
        }
    }

    for (;;) {
        long bytes = platform_read(0, &ch, 1U);
        if (bytes <= 0) {
            if (raw_enabled) {
                (void)platform_terminal_restore_mode(0, &saved);
            }
            rt_write_char(2, '\n');
            return -1;
        }
        if (ch == '\r' || ch == '\n') {
            break;
        }
        if ((ch == 0x7f || ch == '\b') && used > 0U) {
            used -= 1U;
            password[used] = '\0';
            continue;
        }
        if (used + 1U < password_size) {
            password[used++] = ch;
            password[used] = '\0';
        }
    }

    if (raw_enabled) {
        (void)platform_terminal_restore_mode(0, &saved);
    }
    rt_write_char(2, '\n');
    return used == 0U ? -1 : 0;
}

static void ssh_sha256_update_u32be(CryptoSha256Context *ctx, unsigned int value) {
    unsigned char tmp[4];
    ssh_store_be32(tmp, value);
    crypto_sha256_update(ctx, tmp, sizeof(tmp));
}

static void ssh_sha256_update_string(CryptoSha256Context *ctx, const unsigned char *data, size_t len) {
    ssh_sha256_update_u32be(ctx, (unsigned int)len);
    if (len != 0U) {
        crypto_sha256_update(ctx, data, len);
    }
}

static void ssh_sha256_update_cstring(CryptoSha256Context *ctx, const char *text) {
    ssh_sha256_update_string(ctx, (const unsigned char *)text, rt_strlen(text));
}

static void ssh_sha256_update_view(CryptoSha256Context *ctx, const SshStringView *view) {
    if (view == 0) {
        ssh_sha256_update_u32be(ctx, 0U);
        return;
    }
    ssh_sha256_update_string(ctx, view->data, view->length);
}

static void ssh_sha256_update_mpint_bytes(CryptoSha256Context *ctx, const unsigned char *bytes, size_t len) {
    size_t start = 0U;
    size_t used;
    unsigned char zero = 0U;

    while (start < len && bytes[start] == 0U) {
        start += 1U;
    }
    used = len - start;
    if (used == 0U) {
        ssh_sha256_update_u32be(ctx, 0U);
        return;
    }

    if ((bytes[start] & 0x80U) != 0U) {
        ssh_sha256_update_u32be(ctx, (unsigned int)(used + 1U));
        crypto_sha256_update(ctx, &zero, 1U);
    } else {
        ssh_sha256_update_u32be(ctx, (unsigned int)used);
    }
    crypto_sha256_update(ctx, bytes + start, used);
}

static int ssh_compute_curve25519_session_hash(
    const char *server_banner,
    const unsigned char *client_kex_payload,
    size_t client_kex_len,
    const unsigned char *server_kex_payload,
    size_t server_kex_len,
    const SshEcdhReply *reply,
    const unsigned char client_public_key[32],
    const unsigned char shared_secret[32],
    unsigned char out_hash[32]
) {
    CryptoSha256Context ctx;

    if (server_banner == 0 || client_kex_payload == 0 || server_kex_payload == 0 ||
        reply == 0 || client_public_key == 0 || shared_secret == 0 || out_hash == 0) {
        return -1;
    }

    crypto_sha256_init(&ctx);
    ssh_sha256_update_cstring(&ctx, SSH_CLIENT_BANNER_TEXT);
    ssh_sha256_update_cstring(&ctx, server_banner);
    ssh_sha256_update_string(&ctx, client_kex_payload, client_kex_len);
    ssh_sha256_update_string(&ctx, server_kex_payload, server_kex_len);
    ssh_sha256_update_view(&ctx, &reply->host_key_blob);
    ssh_sha256_update_string(&ctx, client_public_key, 32U);
    ssh_sha256_update_view(&ctx, &reply->server_public_key);
    ssh_sha256_update_mpint_bytes(&ctx, shared_secret, 32U);
    crypto_sha256_final(&ctx, out_hash);
    crypto_secure_bzero(&ctx, sizeof(ctx));
    return 0;
}

static int ssh_send_packet(int fd, const unsigned char *payload, size_t payload_len) {
    unsigned char packet[SSH_PACKET_BUFFER_CAPACITY];
    size_t padding_len = 4U;
    unsigned int packet_len;
    size_t total_len;

    while (((4U + 1U + payload_len + padding_len) & 7U) != 0U) {
        padding_len += 1U;
    }

    packet_len = (unsigned int)(1U + payload_len + padding_len);
    total_len = 4U + (size_t)packet_len;
    if (total_len > sizeof(packet)) {
        return -1;
    }

    ssh_store_be32(packet, packet_len);
    packet[4] = (unsigned char)padding_len;
    if (payload_len != 0U) {
        memcpy(packet + 5U, payload, payload_len);
    }
    if (crypto_random_bytes(packet + 5U + payload_len, padding_len) != 0) {
        return -1;
    }
    return ssh_write_all(fd, packet, total_len);
}

static int ssh_read_packet(
    int fd,
    unsigned char *payload,
    size_t payload_capacity,
    size_t *payload_len_out
) {
    unsigned char header[5];
    unsigned int packet_len;
    unsigned char padding_len;
    size_t payload_len;

    if (payload == 0 || payload_len_out == 0) {
        return -1;
    }
    if (ssh_read_exact(fd, header, sizeof(header)) != 0) {
        return -1;
    }

    packet_len = ssh_read_be32(header);
    padding_len = header[4];
    if (packet_len > 35000U || padding_len < 4U || packet_len < (unsigned int)padding_len + 1U) {
        return -1;
    }

    payload_len = (size_t)packet_len - (size_t)padding_len - 1U;
    if (payload_len > payload_capacity) {
        return -2;
    }

    if (ssh_read_exact(fd, payload, payload_len) != 0 ||
        ssh_discard_exact(fd, (size_t)padding_len) != 0) {
        return -1;
    }

    *payload_len_out = payload_len;
    return 0;
}

static int ssh_send_encrypted_packet(int fd, const unsigned char key[64], unsigned int seqnr, const unsigned char *payload, size_t payload_len) {
    unsigned char packet[SSH_PACKET_BUFFER_CAPACITY];
    unsigned char tag[16];
    size_t padding_len = 4U;
    unsigned int packet_len;
    size_t total_len;

    while (((1U + payload_len + padding_len) & 7U) != 0U) {
        padding_len += 1U;
    }

    packet_len = (unsigned int)(1U + payload_len + padding_len);
    total_len = 4U + (size_t)packet_len;
    if (total_len > sizeof(packet)) {
        return -1;
    }

    ssh_store_be32(packet, packet_len);
    packet[4] = (unsigned char)padding_len;
    if (payload_len != 0U) {
        memcpy(packet + 5U, payload, payload_len);
    }
    if (crypto_random_bytes(packet + 5U + payload_len, padding_len) != 0) {
        return -1;
    }

    crypto_ssh_chachapoly_encrypt_packet(key, seqnr, packet, total_len, tag);
    if (ssh_write_all(fd, packet, total_len) != 0 ||
        ssh_write_all(fd, tag, sizeof(tag)) != 0) {
        return -1;
    }
    return 0;
}

static int ssh_read_encrypted_packet(
    int fd,
    const unsigned char key[64],
    unsigned int seqnr,
    unsigned char *payload,
    size_t payload_capacity,
    size_t *payload_len_out
) {
    unsigned char packet[SSH_PACKET_BUFFER_CAPACITY];
    unsigned char plain_len[4];
    unsigned char tag[16];
    unsigned int packet_len;
    unsigned char padding_len;
    size_t payload_len;

    if (payload == 0 || payload_len_out == 0) {
        return -1;
    }
    if (ssh_read_exact(fd, packet, 4U) != 0) {
        return -1;
    }

    crypto_ssh_chachapoly_decrypt_length(key, seqnr, packet, plain_len);
    packet_len = ssh_read_be32(plain_len);
    if (packet_len > 35000U || packet_len < 5U || packet_len + 4U > sizeof(packet)) {
        return -1;
    }
    if (ssh_read_exact(fd, packet + 4U, (size_t)packet_len) != 0 ||
        ssh_read_exact(fd, tag, sizeof(tag)) != 0) {
        return -1;
    }
    if (crypto_ssh_chachapoly_decrypt_packet(key, seqnr, packet, 4U + (size_t)packet_len, tag) != 0) {
        return -1;
    }

    padding_len = packet[4];
    if (padding_len < 4U || packet_len < (unsigned int)padding_len + 1U) {
        return -1;
    }
    payload_len = (size_t)packet_len - (size_t)padding_len - 1U;
    if (payload_len > payload_capacity) {
        return -2;
    }

    if (payload_len != 0U) {
        memcpy(payload, packet + 5U, payload_len);
    }
    *payload_len_out = payload_len;
    return 0;
}

static int ssh_read_banner(int fd, char *buffer, size_t buffer_size) {
    size_t used = 0U;
    char ch;

    if (buffer == 0 || buffer_size == 0U) {
        return -1;
    }

    for (;;) {
        long bytes = platform_read(fd, &ch, 1U);
        if (bytes <= 0) {
            return -1;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            buffer[used] = '\0';
            if (ssh_validate_banner_line(buffer)) {
                return 0;
            }
            used = 0U;
            continue;
        }
        if (used + 1U >= buffer_size) {
            return -1;
        }
        buffer[used++] = ch;
    }
}

static int ssh_parse_ecdh_reply(const unsigned char *payload, size_t payload_len, SshEcdhReply *reply) {
    SshCursor cursor;

    if (payload == 0 || reply == 0 || payload_len < 1U || payload[0] != SSH_MSG_KEX_ECDH_REPLY) {
        return -1;
    }
    ssh_cursor_init(&cursor, payload + 1U, payload_len - 1U);
    if (ssh_cursor_take_string(&cursor, &reply->host_key_blob) != 0 ||
        ssh_cursor_take_string(&cursor, &reply->server_public_key) != 0 ||
        ssh_cursor_take_string(&cursor, &reply->signature_blob) != 0) {
        return -1;
    }
    return 0;
}

static int ssh_parse_service_accept(const unsigned char *payload, size_t payload_len) {
    SshCursor cursor;
    SshStringView service;

    if (payload == 0 || payload_len < 1U || payload[0] != SSH_MSG_SERVICE_ACCEPT) {
        return -1;
    }
    ssh_cursor_init(&cursor, payload + 1U, payload_len - 1U);
    if (ssh_cursor_take_string(&cursor, &service) != 0 || !ssh_view_equals_text(&service, "ssh-userauth")) {
        return -1;
    }
    return 0;
}

static int ssh_parse_userauth_banner(const unsigned char *payload, size_t payload_len) {
    SshCursor cursor;
    SshStringView message;
    SshStringView language;

    if (payload == 0 || payload_len < 1U || payload[0] != SSH_MSG_USERAUTH_BANNER) {
        return -1;
    }
    ssh_cursor_init(&cursor, payload + 1U, payload_len - 1U);
    if (ssh_cursor_take_string(&cursor, &message) != 0 ||
        ssh_cursor_take_string(&cursor, &language) != 0) {
        return -1;
    }

    rt_write_cstr(1, "ssh banner: ");
    if (message.length != 0U) {
        (void)ssh_write_all(1, message.data, message.length);
    }
    rt_write_char(1, '\n');
    (void)language;
    return 0;
}

static int ssh_parse_userauth_failure(
    const unsigned char *payload,
    size_t payload_len,
    char *methods_buffer,
    size_t methods_buffer_size
) {
    SshCursor cursor;
    SshStringView methods;
    unsigned char partial = 0U;

    if (payload == 0 || methods_buffer == 0 || methods_buffer_size == 0U ||
        payload_len < 1U || payload[0] != SSH_MSG_USERAUTH_FAILURE) {
        return -1;
    }
    ssh_cursor_init(&cursor, payload + 1U, payload_len - 1U);
    if (ssh_cursor_take_string(&cursor, &methods) != 0 ||
        ssh_cursor_take_u8(&cursor, &partial) != 0 ||
        ssh_copy_view_text(&methods, methods_buffer, methods_buffer_size) != 0) {
        return -1;
    }
    (void)partial;
    return 0;
}

static int ssh_parse_global_request(const unsigned char *payload, size_t payload_len, int *want_reply_out) {
    SshCursor cursor;
    SshStringView request_name;
    unsigned char want_reply = 0U;

    if (payload == 0 || want_reply_out == 0 || payload_len < 1U || payload[0] != SSH_MSG_GLOBAL_REQUEST) {
        return -1;
    }
    ssh_cursor_init(&cursor, payload + 1U, payload_len - 1U);
    if (ssh_cursor_take_string(&cursor, &request_name) != 0 ||
        ssh_cursor_take_u8(&cursor, &want_reply) != 0) {
        return -1;
    }
    *want_reply_out = want_reply ? 1 : 0;
    (void)request_name;
    return 0;
}

static int ssh_parse_channel_open_confirmation(
    const unsigned char *payload,
    size_t payload_len,
    unsigned int expected_local_channel,
    SshChannelState *channel
) {
    SshCursor cursor;
    unsigned int recipient = 0U;
    unsigned int sender = 0U;
    unsigned int initial_window = 0U;
    unsigned int max_packet = 0U;

    if (payload == 0 || channel == 0 || payload_len < 1U || payload[0] != SSH_MSG_CHANNEL_OPEN_CONFIRMATION) {
        return -1;
    }
    ssh_cursor_init(&cursor, payload + 1U, payload_len - 1U);
    if (ssh_cursor_take_u32(&cursor, &recipient) != 0 ||
        ssh_cursor_take_u32(&cursor, &sender) != 0 ||
        ssh_cursor_take_u32(&cursor, &initial_window) != 0 ||
        ssh_cursor_take_u32(&cursor, &max_packet) != 0 ||
        recipient != expected_local_channel) {
        return -1;
    }

    channel->remote_id = sender;
    channel->remote_window = initial_window;
    channel->max_packet = max_packet;
    return 0;
}

static int ssh_parse_channel_status_reply(
    const unsigned char *payload,
    size_t payload_len,
    unsigned int expected_local_channel
) {
    SshCursor cursor;
    unsigned int recipient = 0U;

    if (payload == 0 || payload_len < 1U ||
        (payload[0] != SSH_MSG_CHANNEL_SUCCESS && payload[0] != SSH_MSG_CHANNEL_FAILURE)) {
        return -1;
    }
    ssh_cursor_init(&cursor, payload + 1U, payload_len - 1U);
    if (ssh_cursor_take_u32(&cursor, &recipient) != 0 || recipient != expected_local_channel) {
        return -1;
    }
    return payload[0] == SSH_MSG_CHANNEL_SUCCESS ? 1 : 0;
}

static int ssh_parse_channel_window_adjust(
    const unsigned char *payload,
    size_t payload_len,
    unsigned int expected_local_channel,
    unsigned int *bytes_to_add_out
) {
    SshCursor cursor;
    unsigned int recipient = 0U;
    unsigned int bytes_to_add = 0U;

    if (payload == 0 || bytes_to_add_out == 0 || payload_len < 1U || payload[0] != SSH_MSG_CHANNEL_WINDOW_ADJUST) {
        return -1;
    }
    ssh_cursor_init(&cursor, payload + 1U, payload_len - 1U);
    if (ssh_cursor_take_u32(&cursor, &recipient) != 0 ||
        ssh_cursor_take_u32(&cursor, &bytes_to_add) != 0 ||
        recipient != expected_local_channel) {
        return -1;
    }
    *bytes_to_add_out = bytes_to_add;
    return 0;
}

static int ssh_parse_channel_data(
    const unsigned char *payload,
    size_t payload_len,
    unsigned int expected_local_channel,
    SshStringView *data_out,
    int extended
) {
    SshCursor cursor;
    unsigned int recipient = 0U;
    unsigned int data_type = 0U;
    SshStringView data;

    if (payload == 0 || data_out == 0 || payload_len < 1U) {
        return -1;
    }
    if ((!extended && payload[0] != SSH_MSG_CHANNEL_DATA) ||
        (extended && payload[0] != SSH_MSG_CHANNEL_EXTENDED_DATA)) {
        return -1;
    }

    ssh_cursor_init(&cursor, payload + 1U, payload_len - 1U);
    if (ssh_cursor_take_u32(&cursor, &recipient) != 0 || recipient != expected_local_channel) {
        return -1;
    }
    if (extended) {
        if (ssh_cursor_take_u32(&cursor, &data_type) != 0 || data_type != 1U) {
            return -1;
        }
    }
    if (ssh_cursor_take_string(&cursor, &data) != 0) {
        return -1;
    }
    *data_out = data;
    return 0;
}

static int ssh_parse_channel_close_or_eof(
    const unsigned char *payload,
    size_t payload_len,
    unsigned char expected_type,
    unsigned int expected_local_channel
) {
    SshCursor cursor;
    unsigned int recipient = 0U;

    if (payload == 0 || payload_len < 1U || payload[0] != expected_type) {
        return -1;
    }
    ssh_cursor_init(&cursor, payload + 1U, payload_len - 1U);
    if (ssh_cursor_take_u32(&cursor, &recipient) != 0 || recipient != expected_local_channel) {
        return -1;
    }
    return 0;
}

static int ssh_send_request_failure(int fd, const unsigned char key[64], unsigned int seqnr) {
    static const unsigned char payload[1] = { SSH_MSG_REQUEST_FAILURE };
    return ssh_send_encrypted_packet(fd, key, seqnr, payload, sizeof(payload));
}

static int ssh_send_service_request(int fd, const unsigned char key[64], unsigned int seqnr, const char *service_name) {
    unsigned char payload[128];
    SshBuilder builder;

    ssh_builder_init(&builder, payload, sizeof(payload));
    if (ssh_builder_put_u8(&builder, SSH_MSG_SERVICE_REQUEST) != 0 ||
        ssh_builder_put_cstring(&builder, service_name) != 0) {
        return -1;
    }
    return ssh_send_encrypted_packet(fd, key, seqnr, payload, builder.length);
}

static int ssh_send_userauth_none(int fd, const unsigned char key[64], unsigned int seqnr, const char *user_name) {
    unsigned char payload[256];
    SshBuilder builder;

    ssh_builder_init(&builder, payload, sizeof(payload));
    if (ssh_builder_put_u8(&builder, SSH_MSG_USERAUTH_REQUEST) != 0 ||
        ssh_builder_put_cstring(&builder, user_name) != 0 ||
        ssh_builder_put_cstring(&builder, "ssh-connection") != 0 ||
        ssh_builder_put_cstring(&builder, "none") != 0) {
        return -1;
    }
    return ssh_send_encrypted_packet(fd, key, seqnr, payload, builder.length);
}

static int ssh_send_userauth_password(
    int fd,
    const unsigned char key[64],
    unsigned int seqnr,
    const char *user_name,
    const char *password
) {
    unsigned char payload[512];
    SshBuilder builder;

    ssh_builder_init(&builder, payload, sizeof(payload));
    if (ssh_builder_put_u8(&builder, SSH_MSG_USERAUTH_REQUEST) != 0 ||
        ssh_builder_put_cstring(&builder, user_name) != 0 ||
        ssh_builder_put_cstring(&builder, "ssh-connection") != 0 ||
        ssh_builder_put_cstring(&builder, "password") != 0 ||
        ssh_builder_put_u8(&builder, 0U) != 0 ||
        ssh_builder_put_cstring(&builder, password) != 0) {
        return -1;
    }
    return ssh_send_encrypted_packet(fd, key, seqnr, payload, builder.length);
}

static int ssh_send_userauth_publickey(
    int fd,
    const unsigned char key[64],
    unsigned int seqnr,
    const char *user_name,
    const unsigned char session_id[32],
    const SshIdentity *identity
) {
    unsigned char public_key_blob[128];
    unsigned char signed_data[512];
    unsigned char signature[64];
    unsigned char signature_blob[128];
    unsigned char payload[768];
    SshBuilder public_builder;
    SshBuilder signed_builder;
    SshBuilder signature_builder;
    SshBuilder payload_builder;
    int status = -1;

    if (identity == 0 || !identity->loaded) {
        return -1;
    }

    ssh_builder_init(&public_builder, public_key_blob, sizeof(public_key_blob));
    if (ssh_builder_put_cstring(&public_builder, "ssh-ed25519") != 0 ||
        ssh_builder_put_string(&public_builder, identity->public_key, 32U) != 0) {
        goto cleanup;
    }

    ssh_builder_init(&signed_builder, signed_data, sizeof(signed_data));
    if (ssh_builder_put_string(&signed_builder, session_id, 32U) != 0 ||
        ssh_builder_put_u8(&signed_builder, SSH_MSG_USERAUTH_REQUEST) != 0 ||
        ssh_builder_put_cstring(&signed_builder, user_name) != 0 ||
        ssh_builder_put_cstring(&signed_builder, "ssh-connection") != 0 ||
        ssh_builder_put_cstring(&signed_builder, "publickey") != 0 ||
        ssh_builder_put_u8(&signed_builder, 1U) != 0 ||
        ssh_builder_put_cstring(&signed_builder, "ssh-ed25519") != 0 ||
        ssh_builder_put_string(&signed_builder, public_key_blob, public_builder.length) != 0) {
        goto cleanup;
    }

    if (crypto_ed25519_sign(signature, signed_data, signed_builder.length, identity->seed, identity->public_key) != 0) {
        goto cleanup;
    }

    ssh_builder_init(&signature_builder, signature_blob, sizeof(signature_blob));
    if (ssh_builder_put_cstring(&signature_builder, "ssh-ed25519") != 0 ||
        ssh_builder_put_string(&signature_builder, signature, sizeof(signature)) != 0) {
        goto cleanup;
    }

    ssh_builder_init(&payload_builder, payload, sizeof(payload));
    if (ssh_builder_put_u8(&payload_builder, SSH_MSG_USERAUTH_REQUEST) != 0 ||
        ssh_builder_put_cstring(&payload_builder, user_name) != 0 ||
        ssh_builder_put_cstring(&payload_builder, "ssh-connection") != 0 ||
        ssh_builder_put_cstring(&payload_builder, "publickey") != 0 ||
        ssh_builder_put_u8(&payload_builder, 1U) != 0 ||
        ssh_builder_put_cstring(&payload_builder, "ssh-ed25519") != 0 ||
        ssh_builder_put_string(&payload_builder, public_key_blob, public_builder.length) != 0 ||
        ssh_builder_put_string(&payload_builder, signature_blob, signature_builder.length) != 0) {
        goto cleanup;
    }

    if (ssh_send_encrypted_packet(fd, key, seqnr, payload, payload_builder.length) != 0) {
        goto cleanup;
    }
    status = 0;

cleanup:
    crypto_secure_bzero(public_key_blob, sizeof(public_key_blob));
    crypto_secure_bzero(signed_data, sizeof(signed_data));
    crypto_secure_bzero(signature, sizeof(signature));
    crypto_secure_bzero(signature_blob, sizeof(signature_blob));
    crypto_secure_bzero(payload, sizeof(payload));
    return status;
}

static int ssh_send_channel_open_session(
    int fd,
    const unsigned char key[64],
    unsigned int seqnr,
    unsigned int local_channel,
    unsigned int initial_window,
    unsigned int max_packet
) {
    unsigned char payload[128];
    SshBuilder builder;

    ssh_builder_init(&builder, payload, sizeof(payload));
    if (ssh_builder_put_u8(&builder, SSH_MSG_CHANNEL_OPEN) != 0 ||
        ssh_builder_put_cstring(&builder, "session") != 0 ||
        ssh_builder_put_u32(&builder, local_channel) != 0 ||
        ssh_builder_put_u32(&builder, initial_window) != 0 ||
        ssh_builder_put_u32(&builder, max_packet) != 0) {
        return -1;
    }
    return ssh_send_encrypted_packet(fd, key, seqnr, payload, builder.length);
}

static int ssh_send_channel_request_pty(int fd, const unsigned char key[64], unsigned int seqnr, unsigned int remote_channel) {
    unsigned char payload[256];
    SshBuilder builder;

    ssh_builder_init(&builder, payload, sizeof(payload));
    if (ssh_builder_put_u8(&builder, SSH_MSG_CHANNEL_REQUEST) != 0 ||
        ssh_builder_put_u32(&builder, remote_channel) != 0 ||
        ssh_builder_put_cstring(&builder, "pty-req") != 0 ||
        ssh_builder_put_u8(&builder, 1U) != 0 ||
        ssh_builder_put_cstring(&builder, "vt100") != 0 ||
        ssh_builder_put_u32(&builder, 80U) != 0 ||
        ssh_builder_put_u32(&builder, 24U) != 0 ||
        ssh_builder_put_u32(&builder, 0U) != 0 ||
        ssh_builder_put_u32(&builder, 0U) != 0 ||
        ssh_builder_put_string(&builder, (const unsigned char *)"", 0U) != 0) {
        return -1;
    }
    return ssh_send_encrypted_packet(fd, key, seqnr, payload, builder.length);
}

static int ssh_send_channel_request_shell(int fd, const unsigned char key[64], unsigned int seqnr, unsigned int remote_channel) {
    unsigned char payload[128];
    SshBuilder builder;

    ssh_builder_init(&builder, payload, sizeof(payload));
    if (ssh_builder_put_u8(&builder, SSH_MSG_CHANNEL_REQUEST) != 0 ||
        ssh_builder_put_u32(&builder, remote_channel) != 0 ||
        ssh_builder_put_cstring(&builder, "shell") != 0 ||
        ssh_builder_put_u8(&builder, 1U) != 0) {
        return -1;
    }
    return ssh_send_encrypted_packet(fd, key, seqnr, payload, builder.length);
}

static int ssh_send_channel_data(
    int fd,
    const unsigned char key[64],
    unsigned int seqnr,
    SshChannelState *channel,
    const unsigned char *data,
    size_t data_len
) {
    unsigned char payload[2048];
    SshBuilder builder;

    if (channel == 0 || data == 0 || data_len == 0U ||
        data_len > channel->max_packet || data_len > channel->remote_window ||
        data_len + 32U > sizeof(payload)) {
        return -1;
    }

    ssh_builder_init(&builder, payload, sizeof(payload));
    if (ssh_builder_put_u8(&builder, SSH_MSG_CHANNEL_DATA) != 0 ||
        ssh_builder_put_u32(&builder, channel->remote_id) != 0 ||
        ssh_builder_put_string(&builder, data, data_len) != 0) {
        return -1;
    }
    if (ssh_send_encrypted_packet(fd, key, seqnr, payload, builder.length) != 0) {
        return -1;
    }
    channel->remote_window -= (unsigned int)data_len;
    return 0;
}

static int ssh_send_channel_window_adjust(
    int fd,
    const unsigned char key[64],
    unsigned int seqnr,
    unsigned int remote_channel,
    unsigned int bytes_to_add
) {
    unsigned char payload[32];
    SshBuilder builder;

    ssh_builder_init(&builder, payload, sizeof(payload));
    if (ssh_builder_put_u8(&builder, SSH_MSG_CHANNEL_WINDOW_ADJUST) != 0 ||
        ssh_builder_put_u32(&builder, remote_channel) != 0 ||
        ssh_builder_put_u32(&builder, bytes_to_add) != 0) {
        return -1;
    }
    return ssh_send_encrypted_packet(fd, key, seqnr, payload, builder.length);
}

static int ssh_send_channel_eof(int fd, const unsigned char key[64], unsigned int seqnr, unsigned int remote_channel) {
    unsigned char payload[16];
    SshBuilder builder;

    ssh_builder_init(&builder, payload, sizeof(payload));
    if (ssh_builder_put_u8(&builder, SSH_MSG_CHANNEL_EOF) != 0 ||
        ssh_builder_put_u32(&builder, remote_channel) != 0) {
        return -1;
    }
    return ssh_send_encrypted_packet(fd, key, seqnr, payload, builder.length);
}

static int ssh_send_channel_close(int fd, const unsigned char key[64], unsigned int seqnr, unsigned int remote_channel) {
    unsigned char payload[16];
    SshBuilder builder;

    ssh_builder_init(&builder, payload, sizeof(payload));
    if (ssh_builder_put_u8(&builder, SSH_MSG_CHANNEL_CLOSE) != 0 ||
        ssh_builder_put_u32(&builder, remote_channel) != 0) {
        return -1;
    }
    return ssh_send_encrypted_packet(fd, key, seqnr, payload, builder.length);
}

static int ssh_confirm_host_key(
    const char *host,
    unsigned int port,
    const SshStringView *host_key_blob
) {
    char fingerprint[SSH_FINGERPRINT_CAPACITY];
    char known_hosts_path[SSH_KNOWN_HOSTS_PATH_CAPACITY];
    SshKnownHostStatus status = SSH_KNOWN_HOST_UNKNOWN;

    if (host == 0 || host_key_blob == 0) {
        return -1;
    }
    if (ssh_known_hosts_default_path(known_hosts_path, sizeof(known_hosts_path)) != 0) {
        return -1;
    }
    if (ssh_known_hosts_lookup(known_hosts_path, host, port, "ssh-ed25519",
                               host_key_blob->data, host_key_blob->length, &status) != 0) {
        return -1;
    }
    if (status == SSH_KNOWN_HOST_MATCH) {
        return 0;
    }
    if (status == SSH_KNOWN_HOST_MISMATCH) {
        rt_write_cstr(2, "ssh: host key mismatch for ");
        rt_write_cstr(2, host);
        rt_write_char(2, '\n');
        return -1;
    }

    if (ssh_format_fingerprint_sha256(host_key_blob->data, host_key_blob->length, fingerprint, sizeof(fingerprint)) != 0) {
        return -1;
    }
    rt_write_cstr(2, "ssh: host key for ");
    rt_write_cstr(2, host);
    rt_write_cstr(2, " is not pinned yet\n");
    rt_write_cstr(2, "ssh: fingerprint ");
    rt_write_cstr(2, fingerprint);
    rt_write_char(2, '\n');

    if (!tool_prompt_yes_no("ssh: trust this host", "")) {
        return -1;
    }
    return ssh_known_hosts_append(known_hosts_path, host, port, "ssh-ed25519",
                                  host_key_blob->data, host_key_blob->length);
}

static int ssh_authenticate_user(
    int sock,
    const char *user,
    const char *host,
    const char *password_in,
    const unsigned char session_id[32],
    const unsigned char key_c_to_s[64],
    const unsigned char key_s_to_c[64],
    unsigned int *client_seq_io,
    unsigned int *server_seq_io,
    const SshIdentity *identity,
    int verbose
) {
    unsigned char payload[SSH_PACKET_BUFFER_CAPACITY];
    size_t payload_len = 0U;
    char methods[128];
    char prompted_password[SSH_PASSWORD_CAPACITY];
    const char *password = password_in;
    int saw_success = 0;
    int rc;

    if (ssh_send_service_request(sock, key_c_to_s, *client_seq_io, "ssh-userauth") != 0) {
        return -1;
    }
    *client_seq_io += 1U;

    for (;;) {
        rc = ssh_read_encrypted_packet(sock, key_s_to_c, *server_seq_io, payload, sizeof(payload), &payload_len);
        if (rc != 0 || payload_len == 0U) {
            return -1;
        }
        *server_seq_io += 1U;
        if (payload[0] == SSH_MSG_EXT_INFO) {
            continue;
        }
        if (payload[0] == SSH_MSG_USERAUTH_BANNER) {
            (void)ssh_parse_userauth_banner(payload, payload_len);
            continue;
        }
        if (ssh_parse_service_accept(payload, payload_len) == 0) {
            break;
        }
        return -1;
    }

    if (ssh_send_userauth_none(sock, key_c_to_s, *client_seq_io, user) != 0) {
        return -1;
    }
    *client_seq_io += 1U;

    methods[0] = '\0';
    for (;;) {
        rc = ssh_read_encrypted_packet(sock, key_s_to_c, *server_seq_io, payload, sizeof(payload), &payload_len);
        if (rc != 0 || payload_len == 0U) {
            return -1;
        }
        *server_seq_io += 1U;
        if (payload[0] == SSH_MSG_USERAUTH_BANNER) {
            (void)ssh_parse_userauth_banner(payload, payload_len);
            continue;
        }
        if (payload[0] == SSH_MSG_EXT_INFO) {
            continue;
        }
        if (payload[0] == SSH_MSG_USERAUTH_SUCCESS) {
            saw_success = 1;
            break;
        }
        if (payload[0] == SSH_MSG_USERAUTH_FAILURE) {
            if (ssh_parse_userauth_failure(payload, payload_len, methods, sizeof(methods)) != 0) {
                return -1;
            }
            break;
        }
        return -1;
    }

    if (!saw_success && identity != 0 && identity->loaded &&
        ssh_name_list_contains(&(SshStringView){ (const unsigned char *)methods, rt_strlen(methods) }, "publickey")) {
        if (verbose) {
            rt_write_cstr(1, "ssh: trying public-key authentication\n");
        }
        if (ssh_send_userauth_publickey(sock, key_c_to_s, *client_seq_io, user, session_id, identity) != 0) {
            return -1;
        }
        *client_seq_io += 1U;

        for (;;) {
            rc = ssh_read_encrypted_packet(sock, key_s_to_c, *server_seq_io, payload, sizeof(payload), &payload_len);
            if (rc != 0 || payload_len == 0U) {
                return -1;
            }
            *server_seq_io += 1U;
            if (payload[0] == SSH_MSG_USERAUTH_BANNER) {
                (void)ssh_parse_userauth_banner(payload, payload_len);
                continue;
            }
            if (payload[0] == SSH_MSG_EXT_INFO) {
                continue;
            }
            if (payload[0] == SSH_MSG_USERAUTH_SUCCESS) {
                saw_success = 1;
                break;
            }
            if (payload[0] == SSH_MSG_USERAUTH_FAILURE) {
                if (ssh_parse_userauth_failure(payload, payload_len, methods, sizeof(methods)) != 0) {
                    return -1;
                }
                break;
            }
            return -1;
        }
    }

    if (!saw_success) {
        if (!ssh_name_list_contains(&(SshStringView){ (const unsigned char *)methods, rt_strlen(methods) }, "password")) {
            rt_write_cstr(2, "ssh: server does not offer a supported authentication method\n");
            return -1;
        }
        if (password == 0 || password[0] == '\0') {
            if (ssh_prompt_password(user, host, prompted_password, sizeof(prompted_password)) != 0) {
                return -1;
            }
            password = prompted_password;
        }
        if (verbose) {
            rt_write_cstr(1, "ssh: trying password authentication\n");
        }
        if (ssh_send_userauth_password(sock, key_c_to_s, *client_seq_io, user, password) != 0) {
            crypto_secure_bzero(prompted_password, sizeof(prompted_password));
            return -1;
        }
        *client_seq_io += 1U;

        for (;;) {
            rc = ssh_read_encrypted_packet(sock, key_s_to_c, *server_seq_io, payload, sizeof(payload), &payload_len);
            if (rc != 0 || payload_len == 0U) {
                crypto_secure_bzero(prompted_password, sizeof(prompted_password));
                return -1;
            }
            *server_seq_io += 1U;
            if (payload[0] == SSH_MSG_USERAUTH_BANNER) {
                (void)ssh_parse_userauth_banner(payload, payload_len);
                continue;
            }
            if (payload[0] == SSH_MSG_EXT_INFO) {
                continue;
            }
            if (payload[0] == SSH_MSG_USERAUTH_SUCCESS) {
                saw_success = 1;
                break;
            }
            if (payload[0] == SSH_MSG_USERAUTH_FAILURE) {
                rt_write_cstr(2, "ssh: authentication failed\n");
                crypto_secure_bzero(prompted_password, sizeof(prompted_password));
                return -1;
            }
            return -1;
        }
    }

    crypto_secure_bzero(prompted_password, sizeof(prompted_password));
    return saw_success ? 0 : -1;
}

static int ssh_start_interactive_shell(
    int sock,
    const SshTransportKeys *keys,
    unsigned int *client_seq_io,
    unsigned int *server_seq_io,
    int verbose
) {
    unsigned char payload[SSH_PACKET_BUFFER_CAPACITY];
    size_t payload_len = 0U;
    unsigned char stdin_buffer[512];
    SshChannelState channel;
    PlatformTerminalState saved;
    int terminal_raw = 0;
    int remote_closed = 0;
    int fds[2];
    size_t ready_index = 0U;
    int poll_rc;

    rt_memset(&channel, 0, sizeof(channel));
    channel.local_id = 0U;
    channel.local_window = 1024U * 1024U;
    channel.max_packet = 32768U;

    if (ssh_send_channel_open_session(sock, keys->key_c_to_s, *client_seq_io,
                                      channel.local_id, channel.local_window, channel.max_packet) != 0) {
        return -1;
    }
    *client_seq_io += 1U;

    for (;;) {
        unsigned int bytes_to_add = 0U;
        int want_reply = 0;

        if (ssh_read_encrypted_packet(sock, keys->key_s_to_c, *server_seq_io, payload, sizeof(payload), &payload_len) != 0 ||
            payload_len == 0U) {
            return -1;
        }
        *server_seq_io += 1U;

        if (payload[0] == SSH_MSG_EXT_INFO) {
            continue;
        }
        if (payload[0] == SSH_MSG_GLOBAL_REQUEST) {
            if (ssh_parse_global_request(payload, payload_len, &want_reply) != 0) {
                return -1;
            }
            if (want_reply) {
                if (ssh_send_request_failure(sock, keys->key_c_to_s, *client_seq_io) != 0) {
                    return -1;
                }
                *client_seq_io += 1U;
            }
            continue;
        }
        if (payload[0] == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
            if (ssh_parse_channel_window_adjust(payload, payload_len, channel.local_id, &bytes_to_add) != 0) {
                return -1;
            }
            channel.remote_window += bytes_to_add;
            continue;
        }
        if (payload[0] == SSH_MSG_CHANNEL_OPEN_CONFIRMATION &&
            ssh_parse_channel_open_confirmation(payload, payload_len, channel.local_id, &channel) == 0) {
            break;
        }
        return -1;
    }

    if (ssh_send_channel_request_pty(sock, keys->key_c_to_s, *client_seq_io, channel.remote_id) != 0) {
        return -1;
    }
    *client_seq_io += 1U;

    for (;;) {
        int want_reply = 0;
        int status_reply;

        if (ssh_read_encrypted_packet(sock, keys->key_s_to_c, *server_seq_io, payload, sizeof(payload), &payload_len) != 0 ||
            payload_len == 0U) {
            return -1;
        }
        *server_seq_io += 1U;

        if (payload[0] == SSH_MSG_EXT_INFO) {
            continue;
        }
        if (payload[0] == SSH_MSG_GLOBAL_REQUEST) {
            if (ssh_parse_global_request(payload, payload_len, &want_reply) != 0) {
                return -1;
            }
            if (want_reply) {
                if (ssh_send_request_failure(sock, keys->key_c_to_s, *client_seq_io) != 0) {
                    return -1;
                }
                *client_seq_io += 1U;
            }
            continue;
        }
        status_reply = ssh_parse_channel_status_reply(payload, payload_len, channel.local_id);
        if (status_reply >= 0) {
            break;
        }
    }

    if (ssh_send_channel_request_shell(sock, keys->key_c_to_s, *client_seq_io, channel.remote_id) != 0) {
        return -1;
    }
    *client_seq_io += 1U;

    for (;;) {
        SshStringView data;
        int want_reply = 0;
        int status_reply;

        if (ssh_read_encrypted_packet(sock, keys->key_s_to_c, *server_seq_io, payload, sizeof(payload), &payload_len) != 0 ||
            payload_len == 0U) {
            return -1;
        }
        *server_seq_io += 1U;

        if (payload[0] == SSH_MSG_EXT_INFO) {
            continue;
        }
        if (payload[0] == SSH_MSG_GLOBAL_REQUEST) {
            if (ssh_parse_global_request(payload, payload_len, &want_reply) != 0) {
                return -1;
            }
            if (want_reply) {
                if (ssh_send_request_failure(sock, keys->key_c_to_s, *client_seq_io) != 0) {
                    return -1;
                }
                *client_seq_io += 1U;
            }
            continue;
        }
        if (ssh_parse_channel_data(payload, payload_len, channel.local_id, &data, 0) == 0) {
            if (data.length != 0U && ssh_write_all(1, data.data, data.length) != 0) {
                return -1;
            }
            if (ssh_send_channel_window_adjust(sock, keys->key_c_to_s, *client_seq_io, channel.remote_id, (unsigned int)data.length) != 0) {
                return -1;
            }
            *client_seq_io += 1U;
            break;
        }
        status_reply = ssh_parse_channel_status_reply(payload, payload_len, channel.local_id);
        if (status_reply > 0) {
            break;
        }
    }

    if (verbose) {
        rt_write_cstr(1, "ssh: interactive shell is ready\n");
    }
    if (platform_isatty(0) && platform_terminal_enable_raw_mode(0, &saved) == 0) {
        terminal_raw = 1;
    }

    while (!remote_closed) {
        fds[0] = sock;
        if (!channel.eof_sent && channel.remote_window != 0U) {
            fds[1] = 0;
            poll_rc = platform_poll_fds(fds, 2U, &ready_index, -1);
        } else {
            poll_rc = platform_poll_fds(fds, 1U, &ready_index, -1);
        }
        if (poll_rc <= 0) {
            if (terminal_raw) {
                (void)platform_terminal_restore_mode(0, &saved);
            }
            return -1;
        }

        if (ready_index == 1U && !channel.eof_sent && channel.remote_window != 0U) {
            size_t limit = sizeof(stdin_buffer);
            long bytes;

            if (limit > channel.remote_window) {
                limit = channel.remote_window;
            }
            if (limit > channel.max_packet) {
                limit = channel.max_packet;
            }
            bytes = platform_read(0, stdin_buffer, limit);
            if (bytes < 0) {
                if (terminal_raw) {
                    (void)platform_terminal_restore_mode(0, &saved);
                }
                return -1;
            }
            if (bytes == 0) {
                if (!channel.eof_sent) {
                    if (ssh_send_channel_eof(sock, keys->key_c_to_s, *client_seq_io, channel.remote_id) != 0) {
                        if (terminal_raw) {
                            (void)platform_terminal_restore_mode(0, &saved);
                        }
                        return -1;
                    }
                    *client_seq_io += 1U;
                    channel.eof_sent = 1;
                }
            } else {
                size_t offset = 0U;
                while (offset < (size_t)bytes) {
                    size_t chunk = (size_t)bytes - offset;
                    if (chunk > 512U) {
                        chunk = 512U;
                    }
                    if (chunk > channel.max_packet) {
                        chunk = channel.max_packet;
                    }
                    if (chunk > channel.remote_window) {
                        chunk = channel.remote_window;
                    }
                    if (chunk == 0U) {
                        break;
                    }
                    if (ssh_send_channel_data(sock, keys->key_c_to_s, *client_seq_io, &channel,
                                              stdin_buffer + offset, chunk) != 0) {
                        if (terminal_raw) {
                            (void)platform_terminal_restore_mode(0, &saved);
                        }
                        return -1;
                    }
                    *client_seq_io += 1U;
                    offset += chunk;
                }
            }
            continue;
        }

        if (ready_index == 0U) {
            unsigned int bytes_to_add = 0U;
            SshStringView data;
            int want_reply = 0;

            if (ssh_read_encrypted_packet(sock, keys->key_s_to_c, *server_seq_io, payload, sizeof(payload), &payload_len) != 0 ||
                payload_len == 0U) {
                if (terminal_raw) {
                    (void)platform_terminal_restore_mode(0, &saved);
                }
                return -1;
            }
            *server_seq_io += 1U;

            if (payload[0] == SSH_MSG_GLOBAL_REQUEST) {
                if (ssh_parse_global_request(payload, payload_len, &want_reply) != 0) {
                    if (terminal_raw) {
                        (void)platform_terminal_restore_mode(0, &saved);
                    }
                    return -1;
                }
                if (want_reply) {
                    if (ssh_send_request_failure(sock, keys->key_c_to_s, *client_seq_io) != 0) {
                        if (terminal_raw) {
                            (void)platform_terminal_restore_mode(0, &saved);
                        }
                        return -1;
                    }
                    *client_seq_io += 1U;
                }
                continue;
            }
            if (payload[0] == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
                if (ssh_parse_channel_window_adjust(payload, payload_len, channel.local_id, &bytes_to_add) != 0) {
                    if (terminal_raw) {
                        (void)platform_terminal_restore_mode(0, &saved);
                    }
                    return -1;
                }
                channel.remote_window += bytes_to_add;
                continue;
            }
            if (ssh_parse_channel_data(payload, payload_len, channel.local_id, &data, 0) == 0) {
                if (data.length != 0U && ssh_write_all(1, data.data, data.length) != 0) {
                    if (terminal_raw) {
                        (void)platform_terminal_restore_mode(0, &saved);
                    }
                    return -1;
                }
                if (ssh_send_channel_window_adjust(sock, keys->key_c_to_s, *client_seq_io, channel.remote_id, (unsigned int)data.length) != 0) {
                    if (terminal_raw) {
                        (void)platform_terminal_restore_mode(0, &saved);
                    }
                    return -1;
                }
                *client_seq_io += 1U;
                continue;
            }
            if (ssh_parse_channel_data(payload, payload_len, channel.local_id, &data, 1) == 0) {
                if (data.length != 0U && ssh_write_all(2, data.data, data.length) != 0) {
                    if (terminal_raw) {
                        (void)platform_terminal_restore_mode(0, &saved);
                    }
                    return -1;
                }
                if (ssh_send_channel_window_adjust(sock, keys->key_c_to_s, *client_seq_io, channel.remote_id, (unsigned int)data.length) != 0) {
                    if (terminal_raw) {
                        (void)platform_terminal_restore_mode(0, &saved);
                    }
                    return -1;
                }
                *client_seq_io += 1U;
                continue;
            }
            if (payload[0] == SSH_MSG_CHANNEL_EOF &&
                ssh_parse_channel_close_or_eof(payload, payload_len, SSH_MSG_CHANNEL_EOF, channel.local_id) == 0) {
                continue;
            }
            if (payload[0] == SSH_MSG_CHANNEL_CLOSE &&
                ssh_parse_channel_close_or_eof(payload, payload_len, SSH_MSG_CHANNEL_CLOSE, channel.local_id) == 0) {
                if (!channel.close_sent) {
                    if (ssh_send_channel_close(sock, keys->key_c_to_s, *client_seq_io, channel.remote_id) != 0) {
                        if (terminal_raw) {
                            (void)platform_terminal_restore_mode(0, &saved);
                        }
                        return -1;
                    }
                    *client_seq_io += 1U;
                    channel.close_sent = 1;
                }
                remote_closed = 1;
                continue;
            }
        }
    }

    if (terminal_raw) {
        (void)platform_terminal_restore_mode(0, &saved);
    }
    return 0;
}

int ssh_client_connect_and_run(const SshClientConfig *config) {
    int sock = -1;
    char server_banner[SSH_BANNER_CAPACITY];
    unsigned char client_cookie[SSH_KEX_COOKIE_SIZE];
    unsigned char client_kex_payload[1024];
    unsigned char server_kex_payload[SSH_PACKET_BUFFER_CAPACITY];
    unsigned char server_payload[SSH_PACKET_BUFFER_CAPACITY];
    unsigned char client_private[32];
    unsigned char client_public[32];
    unsigned char server_host_key[32];
    unsigned char server_signature[64];
    unsigned char shared_secret[32];
    unsigned char exchange_hash[32];
    SshTransportKeys keys;
    SshAlgorithmProfile profile;
    SshKexInitMessage server_kex;
    SshEcdhReply reply;
    SshIdentity identity;
    unsigned int client_seq = 0U;
    unsigned int server_seq = 0U;
    size_t client_kex_len = 0U;
    size_t server_kex_len = 0U;
    size_t server_payload_len = 0U;

    if (config == 0 || config->host == 0 || config->user == 0 || config->port == 0U) {
        return -1;
    }

    (void)platform_ignore_signal(13);

    if (platform_connect_tcp(config->host, config->port, &sock) != 0) {
        tool_write_error("ssh", "connect failed to ", config->host);
        return -1;
    }

    rt_write_cstr(1, "connected to ");
    rt_write_cstr(1, config->host);
    rt_write_cstr(1, ":");
    rt_write_uint(1, config->port);
    rt_write_char(1, '\n');

    if (ssh_write_all(sock, SSH_CLIENT_BANNER_WIRE, sizeof(SSH_CLIENT_BANNER_WIRE) - 1U) != 0 ||
        ssh_read_banner(sock, server_banner, sizeof(server_banner)) != 0) {
        tool_write_error("ssh", "banner exchange failed with ", config->host);
        platform_close(sock);
        return -1;
    }
    if (config->verbose) {
        rt_write_cstr(1, "server banner: ");
        rt_write_cstr(1, server_banner);
        rt_write_char(1, '\n');
    }

    ssh_profile_init_default_client(&profile);
    if (crypto_random_bytes(client_cookie, sizeof(client_cookie)) != 0 ||
        ssh_build_kexinit_payload(&profile, client_cookie, client_kex_payload, sizeof(client_kex_payload), &client_kex_len) != 0 ||
        ssh_send_packet(sock, client_kex_payload, client_kex_len) != 0) {
        platform_close(sock);
        return -1;
    }
    client_seq += 1U;

    if (ssh_read_packet(sock, server_kex_payload, sizeof(server_kex_payload), &server_kex_len) != 0 ||
        ssh_parse_kexinit_payload(server_kex_payload, server_kex_len, &server_kex) != 0 ||
        !ssh_name_list_contains(&server_kex.kex_algorithms, "curve25519-sha256") ||
        !ssh_name_list_contains(&server_kex.host_key_algorithms, "ssh-ed25519") ||
        !ssh_name_list_contains(&server_kex.ciphers_c_to_s, "chacha20-poly1305@openssh.com") ||
        !ssh_name_list_contains(&server_kex.ciphers_s_to_c, "chacha20-poly1305@openssh.com")) {
        tool_write_error("ssh", "unsupported SSH transport profile from ", config->host);
        platform_close(sock);
        return -1;
    }
    server_seq += 1U;

    if (crypto_random_bytes(client_private, sizeof(client_private)) != 0 ||
        crypto_x25519_scalarmult_base(client_public, client_private) != 0) {
        platform_close(sock);
        return -1;
    }

    {
        unsigned char payload[64];
        SshBuilder builder;

        ssh_builder_init(&builder, payload, sizeof(payload));
        if (ssh_builder_put_u8(&builder, SSH_MSG_KEX_ECDH_INIT) != 0 ||
            ssh_builder_put_string(&builder, client_public, 32U) != 0 ||
            ssh_send_packet(sock, payload, builder.length) != 0) {
            platform_close(sock);
            return -1;
        }
    }
    client_seq += 1U;

    if (ssh_read_packet(sock, server_payload, sizeof(server_payload), &server_payload_len) != 0 ||
        ssh_parse_ecdh_reply(server_payload, server_payload_len, &reply) != 0 ||
        reply.server_public_key.length != 32U ||
        crypto_x25519_scalarmult(shared_secret, client_private, reply.server_public_key.data) != 0 ||
        ssh_compute_curve25519_session_hash(server_banner, client_kex_payload, client_kex_len,
                                            server_kex_payload, server_kex_len, &reply, client_public,
                                            shared_secret, exchange_hash) != 0 ||
        ssh_parse_ed25519_public_key_blob(&reply.host_key_blob, server_host_key) != 0 ||
        ssh_parse_ed25519_signature_blob(&reply.signature_blob, server_signature) != 0 ||
        crypto_ed25519_verify(server_signature, exchange_hash, sizeof(exchange_hash), server_host_key) != 0 ||
        ssh_confirm_host_key(config->host, config->port, &reply.host_key_blob) != 0) {
        tool_write_error("ssh", "key exchange failed with ", config->host);
        platform_close(sock);
        crypto_secure_bzero(client_private, sizeof(client_private));
        crypto_secure_bzero(shared_secret, sizeof(shared_secret));
        crypto_secure_bzero(exchange_hash, sizeof(exchange_hash));
        return -1;
    }
    server_seq += 1U;

    if (crypto_ssh_kdf_derive_sha256(shared_secret, sizeof(shared_secret), exchange_hash, sizeof(exchange_hash), 'C',
                                     exchange_hash, sizeof(exchange_hash), keys.key_c_to_s, sizeof(keys.key_c_to_s)) != 0 ||
        crypto_ssh_kdf_derive_sha256(shared_secret, sizeof(shared_secret), exchange_hash, sizeof(exchange_hash), 'D',
                                     exchange_hash, sizeof(exchange_hash), keys.key_s_to_c, sizeof(keys.key_s_to_c)) != 0) {
        platform_close(sock);
        crypto_secure_bzero(client_private, sizeof(client_private));
        crypto_secure_bzero(shared_secret, sizeof(shared_secret));
        crypto_secure_bzero(exchange_hash, sizeof(exchange_hash));
        return -1;
    }

    {
        static const unsigned char newkeys_payload[1] = { SSH_MSG_NEWKEYS };
        if (ssh_send_packet(sock, newkeys_payload, sizeof(newkeys_payload)) != 0 ||
            ssh_read_packet(sock, server_payload, sizeof(server_payload), &server_payload_len) != 0 ||
            server_payload_len < 1U || server_payload[0] != SSH_MSG_NEWKEYS) {
            platform_close(sock);
            crypto_secure_bzero(client_private, sizeof(client_private));
            crypto_secure_bzero(shared_secret, sizeof(shared_secret));
            crypto_secure_bzero(exchange_hash, sizeof(exchange_hash));
            return -1;
        }
    }
    client_seq += 1U;
    server_seq += 1U;
    if (ssh_try_load_identity(config->identity_path, &identity) != 0) {
        tool_write_error("ssh", "failed to load identity ", config->identity_path);
        platform_close(sock);
        return -1;
    }

    if (ssh_authenticate_user(sock, config->user, config->host, config->password, exchange_hash,
                              keys.key_c_to_s, keys.key_s_to_c, &client_seq, &server_seq,
                              &identity, config->verbose) != 0) {
        platform_close(sock);
        crypto_secure_bzero(&identity, sizeof(identity));
        return -1;
    }
    if (config->verbose) {
        rt_write_cstr(1, "ssh: authentication succeeded\n");
    }

    if (ssh_start_interactive_shell(sock, &keys, &client_seq, &server_seq, config->verbose) != 0) {
        tool_write_error("ssh", "session failed for ", config->host);
        platform_close(sock);
        crypto_secure_bzero(&identity, sizeof(identity));
        return -1;
    }

    platform_close(sock);
    crypto_secure_bzero(client_private, sizeof(client_private));
    crypto_secure_bzero(shared_secret, sizeof(shared_secret));
    crypto_secure_bzero(exchange_hash, sizeof(exchange_hash));
    crypto_secure_bzero(&keys, sizeof(keys));
    crypto_secure_bzero(&identity, sizeof(identity));
    return 0;
}
