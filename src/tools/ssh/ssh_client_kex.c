/* ssh_client_kex.c - key exchange: session hash, ECDH reply parsing, host key trust */

#include "ssh_client_internal.h"

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

int ssh_compute_curve25519_session_hash(
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

int ssh_parse_ecdh_reply(const unsigned char *payload, size_t payload_len, SshEcdhReply *reply) {
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

int ssh_confirm_host_key(
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
