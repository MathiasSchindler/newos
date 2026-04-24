#include "sshd.h"

#include "../ssh/ssh_transport.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define SSHD_MAX_AUTH_ATTEMPTS 3U
#define SSHD_LOCAL_WINDOW (1024U * 1024U)
#define SSHD_MAX_PACKET 32768U
#define SSHD_MAX_COMMAND SSHD_COMMAND_CAPACITY

typedef struct {
    int fd;
    unsigned int send_seq;
    unsigned int recv_seq;
    unsigned char session_id[32];
    SshTransportKeys keys;
} SshdTransport;

typedef struct {
    unsigned int local_id;
    unsigned int remote_id;
    unsigned int remote_window;
    unsigned int remote_max_packet;
} SshdChannel;

static void sshd_sha256_u32(CryptoSha256Context *ctx, unsigned int value) {
    unsigned char tmp[4];
    ssh_store_be32(tmp, value);
    crypto_sha256_update(ctx, tmp, sizeof(tmp));
}

static void sshd_sha256_string(CryptoSha256Context *ctx, const unsigned char *data, size_t length) {
    sshd_sha256_u32(ctx, (unsigned int)length);
    if (length != 0U) {
        crypto_sha256_update(ctx, data, length);
    }
}

static void sshd_sha256_cstring(CryptoSha256Context *ctx, const char *text) {
    sshd_sha256_string(ctx, (const unsigned char *)text, rt_strlen(text));
}

static void sshd_sha256_mpint(CryptoSha256Context *ctx, const unsigned char *bytes, size_t length) {
    size_t start = 0U;
    size_t used;
    unsigned char zero = 0U;

    while (start < length && bytes[start] == 0U) {
        start += 1U;
    }
    used = length - start;
    if (used == 0U) {
        sshd_sha256_u32(ctx, 0U);
        return;
    }
    if ((bytes[start] & 0x80U) != 0U) {
        sshd_sha256_u32(ctx, (unsigned int)(used + 1U));
        crypto_sha256_update(ctx, &zero, 1U);
    } else {
        sshd_sha256_u32(ctx, (unsigned int)used);
    }
    crypto_sha256_update(ctx, bytes + start, used);
}

static int sshd_compute_hash(
    const char *client_banner,
    const unsigned char *client_kex,
    size_t client_kex_len,
    const unsigned char *server_kex,
    size_t server_kex_len,
    const unsigned char *host_key_blob,
    size_t host_key_blob_len,
    const unsigned char client_public[32],
    const unsigned char server_public[32],
    const unsigned char shared_secret[32],
    unsigned char out[32]
) {
    CryptoSha256Context ctx;

    if (client_banner == 0 || client_kex == 0 || server_kex == 0 || host_key_blob == 0 ||
        client_public == 0 || server_public == 0 || shared_secret == 0 || out == 0) {
        return -1;
    }
    crypto_sha256_init(&ctx);
    sshd_sha256_cstring(&ctx, client_banner);
    sshd_sha256_cstring(&ctx, SSH_SERVER_BANNER_TEXT);
    sshd_sha256_string(&ctx, client_kex, client_kex_len);
    sshd_sha256_string(&ctx, server_kex, server_kex_len);
    sshd_sha256_string(&ctx, host_key_blob, host_key_blob_len);
    sshd_sha256_string(&ctx, client_public, 32U);
    sshd_sha256_string(&ctx, server_public, 32U);
    sshd_sha256_mpint(&ctx, shared_secret, 32U);
    crypto_sha256_final(&ctx, out);
    crypto_secure_bzero(&ctx, sizeof(ctx));
    return 0;
}

static int sshd_build_host_key_blob(const unsigned char public_key[32], unsigned char *buffer, size_t cap, size_t *len_out) {
    SshBuilder b;

    ssh_builder_init(&b, buffer, cap);
    if (ssh_builder_put_cstring(&b, "ssh-ed25519") != 0 ||
        ssh_builder_put_string(&b, public_key, 32U) != 0) {
        return -1;
    }
    *len_out = b.length;
    return 0;
}

static int sshd_build_signature_blob(const unsigned char signature[64], unsigned char *buffer, size_t cap, size_t *len_out) {
    SshBuilder b;

    ssh_builder_init(&b, buffer, cap);
    if (ssh_builder_put_cstring(&b, "ssh-ed25519") != 0 ||
        ssh_builder_put_string(&b, signature, 64U) != 0) {
        return -1;
    }
    *len_out = b.length;
    return 0;
}

static int sshd_send_encrypted(SshdTransport *transport, const unsigned char *payload, size_t payload_len) {
    if (ssh_send_encrypted_packet(transport->fd, transport->keys.key_s_to_c, transport->send_seq, payload, payload_len) != 0) {
        return -1;
    }
    transport->send_seq += 1U;
    return 0;
}

static int sshd_read_encrypted(SshdTransport *transport, unsigned char *payload, size_t cap, size_t *len_out) {
    if (ssh_read_encrypted_packet(transport->fd, transport->keys.key_c_to_s, transport->recv_seq, payload, cap, len_out) != 0) {
        return -1;
    }
    transport->recv_seq += 1U;
    return 0;
}

static int sshd_send_failure_list(SshdTransport *t, const char *methods) {
    unsigned char payload[128];
    SshBuilder b;

    ssh_builder_init(&b, payload, sizeof(payload));
    if (ssh_builder_put_u8(&b, SSH_MSG_USERAUTH_FAILURE) != 0 ||
        ssh_builder_put_cstring(&b, methods) != 0 ||
        ssh_builder_put_u8(&b, 0U) != 0) {
        return -1;
    }
    return sshd_send_encrypted(t, payload, b.length);
}

static int sshd_send_channel_failure(SshdTransport *t, unsigned int remote_channel) {
    unsigned char payload[16];
    SshBuilder b;

    ssh_builder_init(&b, payload, sizeof(payload));
    if (ssh_builder_put_u8(&b, SSH_MSG_CHANNEL_FAILURE) != 0 ||
        ssh_builder_put_u32(&b, remote_channel) != 0) {
        return -1;
    }
    return sshd_send_encrypted(t, payload, b.length);
}

static int sshd_send_channel_success(SshdTransport *t, unsigned int remote_channel) {
    unsigned char payload[16];
    SshBuilder b;

    ssh_builder_init(&b, payload, sizeof(payload));
    if (ssh_builder_put_u8(&b, SSH_MSG_CHANNEL_SUCCESS) != 0 ||
        ssh_builder_put_u32(&b, remote_channel) != 0) {
        return -1;
    }
    return sshd_send_encrypted(t, payload, b.length);
}

static int sshd_transport_kex(SshdTransport *t, const SshdConfig *config) {
    unsigned char server_seed[32];
    unsigned char host_public[32];
    unsigned char server_private[32];
    unsigned char server_public[32];
    unsigned char shared_secret[32];
    unsigned char exchange_hash[32];
    unsigned char signature[64];
    unsigned char host_key_blob[128];
    unsigned char signature_blob[128];
    unsigned char client_kex[SSH_PACKET_BUFFER_CAPACITY];
    unsigned char server_kex[SSH_PACKET_BUFFER_CAPACITY];
    unsigned char payload[SSH_PACKET_BUFFER_CAPACITY];
    char client_banner[SSH_BANNER_CAPACITY];
    size_t client_kex_len = 0U;
    size_t server_kex_len = 0U;
    size_t payload_len = 0U;
    size_t host_key_blob_len = 0U;
    size_t signature_blob_len = 0U;
    SshAlgorithmProfile profile;
    SshKexInitMessage client_kex_view;
    SshCursor cursor;
    SshStringView client_public_view;
    SshBuilder b;
    unsigned char msg = 0U;
    int status = -1;

    if (config->host_seed_set) {
        memcpy(server_seed, config->host_seed, sizeof(server_seed));
    } else if (crypto_random_bytes(server_seed, sizeof(server_seed)) != 0) {
        return -1;
    }
    if (crypto_ed25519_public_key_from_seed(host_public, server_seed) != 0) {
        return -1;
    }

    if (ssh_write_all(t->fd, SSH_SERVER_BANNER_WIRE, sizeof(SSH_SERVER_BANNER_WIRE) - 1U) != 0 ||
        ssh_read_banner(t->fd, client_banner, sizeof(client_banner)) != 0) {
        return -1;
    }
    if (config->verbose) {
        rt_write_cstr(1, "sshd: client banner ");
        rt_write_line(1, client_banner);
    }

    ssh_profile_init_default_server(&profile);
    if (crypto_random_bytes(payload, SSH_KEX_COOKIE_SIZE) != 0 ||
        ssh_build_kexinit_payload(&profile, payload, server_kex, sizeof(server_kex), &server_kex_len) != 0 ||
        ssh_send_packet(t->fd, server_kex, server_kex_len) != 0) {
        goto cleanup;
    }
    t->send_seq += 1U;

    if (ssh_read_packet(t->fd, client_kex, sizeof(client_kex), &client_kex_len) != 0 ||
        ssh_parse_kexinit_payload(client_kex, client_kex_len, &client_kex_view) != 0 ||
        !ssh_name_list_contains(&client_kex_view.kex_algorithms, "curve25519-sha256") ||
        !ssh_name_list_contains(&client_kex_view.host_key_algorithms, "ssh-ed25519") ||
        !ssh_name_list_contains(&client_kex_view.ciphers_c_to_s, "chacha20-poly1305@openssh.com") ||
        !ssh_name_list_contains(&client_kex_view.ciphers_s_to_c, "chacha20-poly1305@openssh.com")) {
        goto cleanup;
    }
    t->recv_seq += 1U;

    if (ssh_read_packet(t->fd, payload, sizeof(payload), &payload_len) != 0 ||
        payload_len < 1U || payload[0] != SSH_MSG_KEX_ECDH_INIT) {
        goto cleanup;
    }
    t->recv_seq += 1U;
    ssh_cursor_init(&cursor, payload + 1U, payload_len - 1U);
    if (ssh_cursor_take_string(&cursor, &client_public_view) != 0 || client_public_view.length != 32U) {
        goto cleanup;
    }
    if (crypto_random_bytes(server_private, sizeof(server_private)) != 0 ||
        crypto_x25519_scalarmult_base(server_public, server_private) != 0 ||
        crypto_x25519_scalarmult(shared_secret, server_private, client_public_view.data) != 0 ||
        sshd_build_host_key_blob(host_public, host_key_blob, sizeof(host_key_blob), &host_key_blob_len) != 0 ||
        sshd_compute_hash(client_banner, client_kex, client_kex_len, server_kex, server_kex_len,
                          host_key_blob, host_key_blob_len, client_public_view.data, server_public,
                          shared_secret, exchange_hash) != 0 ||
        crypto_ed25519_sign(signature, exchange_hash, sizeof(exchange_hash), server_seed, host_public) != 0 ||
        sshd_build_signature_blob(signature, signature_blob, sizeof(signature_blob), &signature_blob_len) != 0) {
        goto cleanup;
    }

    ssh_builder_init(&b, payload, sizeof(payload));
    if (ssh_builder_put_u8(&b, SSH_MSG_KEX_ECDH_REPLY) != 0 ||
        ssh_builder_put_string(&b, host_key_blob, host_key_blob_len) != 0 ||
        ssh_builder_put_string(&b, server_public, 32U) != 0 ||
        ssh_builder_put_string(&b, signature_blob, signature_blob_len) != 0 ||
        ssh_send_packet(t->fd, payload, b.length) != 0) {
        goto cleanup;
    }
    t->send_seq += 1U;

    if (crypto_ssh_kdf_derive_sha256(shared_secret, sizeof(shared_secret), exchange_hash, sizeof(exchange_hash), 'C',
                                     exchange_hash, sizeof(exchange_hash), t->keys.key_c_to_s, sizeof(t->keys.key_c_to_s)) != 0 ||
        crypto_ssh_kdf_derive_sha256(shared_secret, sizeof(shared_secret), exchange_hash, sizeof(exchange_hash), 'D',
                                     exchange_hash, sizeof(exchange_hash), t->keys.key_s_to_c, sizeof(t->keys.key_s_to_c)) != 0) {
        goto cleanup;
    }
    memcpy(t->session_id, exchange_hash, sizeof(t->session_id));

    if (ssh_read_packet(t->fd, payload, sizeof(payload), &payload_len) != 0 ||
        payload_len != 1U || payload[0] != SSH_MSG_NEWKEYS) {
        goto cleanup;
    }
    t->recv_seq += 1U;
    msg = SSH_MSG_NEWKEYS;
    if (ssh_send_packet(t->fd, &msg, 1U) != 0) {
        goto cleanup;
    }
    t->send_seq += 1U;
    status = 0;

cleanup:
    crypto_secure_bzero(server_seed, sizeof(server_seed));
    crypto_secure_bzero(server_private, sizeof(server_private));
    crypto_secure_bzero(shared_secret, sizeof(shared_secret));
    crypto_secure_bzero(exchange_hash, sizeof(exchange_hash));
    crypto_secure_bzero(signature, sizeof(signature));
    return status;
}

static int sshd_authenticate(SshdTransport *t, const SshdConfig *config) {
    unsigned char payload[SSH_PACKET_BUFFER_CAPACITY];
    size_t len = 0U;
    SshCursor cursor;
    SshStringView service;
    SshStringView user;
    SshStringView method;
    SshStringView password;
    unsigned int attempts;
    unsigned char change = 0U;
    SshBuilder b;

    if (sshd_read_encrypted(t, payload, sizeof(payload), &len) != 0 || len < 1U || payload[0] != SSH_MSG_SERVICE_REQUEST) {
        return -1;
    }
    ssh_cursor_init(&cursor, payload + 1U, len - 1U);
    if (ssh_cursor_take_string(&cursor, &service) != 0 || !ssh_view_equals_text(&service, "ssh-userauth")) {
        return -1;
    }
    ssh_builder_init(&b, payload, sizeof(payload));
    if (ssh_builder_put_u8(&b, SSH_MSG_SERVICE_ACCEPT) != 0 ||
        ssh_builder_put_cstring(&b, "ssh-userauth") != 0 ||
        sshd_send_encrypted(t, payload, b.length) != 0) {
        return -1;
    }

    for (attempts = 0U; attempts < SSHD_MAX_AUTH_ATTEMPTS; ++attempts) {
        if (sshd_read_encrypted(t, payload, sizeof(payload), &len) != 0 || len < 1U || payload[0] != SSH_MSG_USERAUTH_REQUEST) {
            return -1;
        }
        ssh_cursor_init(&cursor, payload + 1U, len - 1U);
        if (ssh_cursor_take_string(&cursor, &user) != 0 ||
            ssh_cursor_take_string(&cursor, &service) != 0 ||
            ssh_cursor_take_string(&cursor, &method) != 0 ||
            !ssh_view_equals_text(&service, "ssh-connection")) {
            return -1;
        }
        if (ssh_view_equals_text(&method, "password")) {
            if (ssh_cursor_take_u8(&cursor, &change) != 0 ||
                ssh_cursor_take_string(&cursor, &password) != 0 ||
                change != 0U) {
                return -1;
            }
            if (ssh_view_equals_text(&user, config->user) &&
                password.length == rt_strlen(config->password) &&
                crypto_constant_time_equal(password.data, (const unsigned char *)config->password, password.length)) {
                payload[0] = SSH_MSG_USERAUTH_SUCCESS;
                return sshd_send_encrypted(t, payload, 1U);
            }
        }
        if (sshd_send_failure_list(t, "password") != 0) {
            return -1;
        }
    }
    return -1;
}

static int sshd_send_channel_open_confirmation(SshdTransport *t, const SshdChannel *ch) {
    unsigned char payload[64];
    SshBuilder b;

    ssh_builder_init(&b, payload, sizeof(payload));
    if (ssh_builder_put_u8(&b, SSH_MSG_CHANNEL_OPEN_CONFIRMATION) != 0 ||
        ssh_builder_put_u32(&b, ch->remote_id) != 0 ||
        ssh_builder_put_u32(&b, ch->local_id) != 0 ||
        ssh_builder_put_u32(&b, SSHD_LOCAL_WINDOW) != 0 ||
        ssh_builder_put_u32(&b, SSHD_MAX_PACKET) != 0) {
        return -1;
    }
    return sshd_send_encrypted(t, payload, b.length);
}

static int sshd_send_channel_data(SshdTransport *t, unsigned int recipient, const unsigned char *data, size_t len) {
    unsigned char payload[1152];
    SshBuilder b;

    if (len > 1024U) {
        return -1;
    }
    ssh_builder_init(&b, payload, sizeof(payload));
    if (ssh_builder_put_u8(&b, SSH_MSG_CHANNEL_DATA) != 0 ||
        ssh_builder_put_u32(&b, recipient) != 0 ||
        ssh_builder_put_string(&b, data, len) != 0) {
        return -1;
    }
    return sshd_send_encrypted(t, payload, b.length);
}

static int sshd_send_exit_and_close(SshdTransport *t, unsigned int recipient, int exit_status) {
    unsigned char payload[128];
    SshBuilder b;

    ssh_builder_init(&b, payload, sizeof(payload));
    if (ssh_builder_put_u8(&b, SSH_MSG_CHANNEL_REQUEST) != 0 ||
        ssh_builder_put_u32(&b, recipient) != 0 ||
        ssh_builder_put_cstring(&b, "exit-status") != 0 ||
        ssh_builder_put_u8(&b, 0U) != 0 ||
        ssh_builder_put_u32(&b, (unsigned int)(exit_status < 0 ? 255 : exit_status)) != 0 ||
        sshd_send_encrypted(t, payload, b.length) != 0) {
        return -1;
    }
    ssh_builder_init(&b, payload, sizeof(payload));
    if (ssh_builder_put_u8(&b, SSH_MSG_CHANNEL_EOF) != 0 ||
        ssh_builder_put_u32(&b, recipient) != 0 ||
        sshd_send_encrypted(t, payload, b.length) != 0) {
        return -1;
    }
    ssh_builder_init(&b, payload, sizeof(payload));
    if (ssh_builder_put_u8(&b, SSH_MSG_CHANNEL_CLOSE) != 0 ||
        ssh_builder_put_u32(&b, recipient) != 0 ||
        sshd_send_encrypted(t, payload, b.length) != 0) {
        return -1;
    }
    return 0;
}

static int sshd_copy_command(const SshStringView *view, char *buffer, size_t buffer_size) {
    size_t i;

    if (view == 0 || buffer == 0 || buffer_size == 0U || view->length == 0U || view->length + 1U > buffer_size) {
        return -1;
    }
    for (i = 0U; i < view->length; ++i) {
        unsigned char ch = view->data[i];
        if (ch == 0U || ch == '\r' || ch == '\n') {
            return -1;
        }
        buffer[i] = (char)ch;
    }
    buffer[view->length] = '\0';
    return 0;
}

static int sshd_run_exec(SshdTransport *t, const SshdConfig *config, unsigned int remote_channel, const char *command) {
    int pipe_fds[2];
    int pid = -1;
    int exit_status = 255;
    int finished = 0;
    int pipe_open = 1;
    unsigned int idle_ticks = 0U;
    char *argv_exec[4];

    if (platform_create_pipe(pipe_fds) != 0) {
        return -1;
    }
    argv_exec[0] = (char *)config->shell_path;
    argv_exec[1] = (char *)"-c";
    argv_exec[2] = (char *)command;
    argv_exec[3] = 0;
    if (platform_spawn_process(argv_exec, -1, pipe_fds[1], 0, 0, 0, &pid) != 0) {
        platform_close(pipe_fds[0]);
        platform_close(pipe_fds[1]);
        return -1;
    }
    platform_close(pipe_fds[1]);

    while ((pipe_open || !finished) && idle_ticks < 6000U) {
        if (pipe_open) {
            size_t ready = 0U;
            int poll_status = platform_poll_fds(&pipe_fds[0], 1U, &ready, 50);
            if (poll_status > 0) {
                unsigned char chunk[1024];
                long n = platform_read(pipe_fds[0], chunk, sizeof(chunk));
                (void)ready;
                idle_ticks = 0U;
                if (n < 0) {
                    platform_close(pipe_fds[0]);
                    pipe_open = 0;
                } else if (n == 0) {
                    platform_close(pipe_fds[0]);
                    pipe_open = 0;
                } else if (sshd_send_channel_data(t, remote_channel, chunk, (size_t)n) != 0) {
                    platform_close(pipe_fds[0]);
                    return -1;
                }
            } else if (poll_status == 0) {
                idle_ticks += 1U;
            } else {
                platform_close(pipe_fds[0]);
                pipe_open = 0;
            }
        }
        if (!finished) {
            if (platform_poll_process_exit(pid, &finished, &exit_status) != 0) {
                finished = 1;
                exit_status = 255;
            }
        }
    }
    if (pipe_open) {
        platform_close(pipe_fds[0]);
    }
    if (!finished) {
        (void)platform_wait_process_timeout(pid, 0ULL, 0ULL, 15, 0, &exit_status);
    } else {
        (void)platform_wait_process(pid, &exit_status);
    }
    return sshd_send_exit_and_close(t, remote_channel, exit_status);
}

static int sshd_handle_channel(SshdTransport *t, const SshdConfig *config) {
    unsigned char payload[SSH_PACKET_BUFFER_CAPACITY];
    size_t len = 0U;
    SshCursor cursor;
    SshStringView type;
    SshStringView request;
    SshStringView command_view;
    unsigned int sender = 0U;
    unsigned int window = 0U;
    unsigned int max_packet = 0U;
    unsigned int recipient = 0U;
    unsigned char want_reply = 0U;
    SshdChannel ch;
    char command[SSHD_MAX_COMMAND];

    for (;;) {
        if (sshd_read_encrypted(t, payload, sizeof(payload), &len) != 0 || len < 1U) {
            return -1;
        }
        if (payload[0] == SSH_MSG_GLOBAL_REQUEST) {
            ssh_cursor_init(&cursor, payload + 1U, len - 1U);
            if (ssh_cursor_take_string(&cursor, &request) != 0 ||
                ssh_cursor_take_u8(&cursor, &want_reply) != 0) {
                return -1;
            }
            if (want_reply) {
                payload[0] = SSH_MSG_REQUEST_FAILURE;
                if (sshd_send_encrypted(t, payload, 1U) != 0) return -1;
            }
            continue;
        }
        if (payload[0] != SSH_MSG_CHANNEL_OPEN) {
            return -1;
        }
        break;
    }

    ssh_cursor_init(&cursor, payload + 1U, len - 1U);
    if (ssh_cursor_take_string(&cursor, &type) != 0 ||
        ssh_cursor_take_u32(&cursor, &sender) != 0 ||
        ssh_cursor_take_u32(&cursor, &window) != 0 ||
        ssh_cursor_take_u32(&cursor, &max_packet) != 0) {
        return -1;
    }
    if (!ssh_view_equals_text(&type, "session")) {
        SshBuilder b;
        ssh_builder_init(&b, payload, sizeof(payload));
        if (ssh_builder_put_u8(&b, SSH_MSG_CHANNEL_OPEN_FAILURE) != 0 ||
            ssh_builder_put_u32(&b, sender) != 0 ||
            ssh_builder_put_u32(&b, 1U) != 0 ||
            ssh_builder_put_cstring(&b, "only session channels are supported") != 0 ||
            ssh_builder_put_cstring(&b, "") != 0) {
            return -1;
        }
        return sshd_send_encrypted(t, payload, b.length);
    }
    ch.local_id = 0U;
    ch.remote_id = sender;
    ch.remote_window = window;
    ch.remote_max_packet = max_packet;
    if (sshd_send_channel_open_confirmation(t, &ch) != 0) {
        return -1;
    }

    for (;;) {
        if (sshd_read_encrypted(t, payload, sizeof(payload), &len) != 0 || len < 1U) {
            return -1;
        }
        if (payload[0] == SSH_MSG_CHANNEL_CLOSE) {
            return 0;
        }
        if (payload[0] != SSH_MSG_CHANNEL_REQUEST) {
            continue;
        }
        ssh_cursor_init(&cursor, payload + 1U, len - 1U);
        if (ssh_cursor_take_u32(&cursor, &recipient) != 0 ||
            ssh_cursor_take_string(&cursor, &request) != 0 ||
            ssh_cursor_take_u8(&cursor, &want_reply) != 0 ||
            recipient != ch.local_id) {
            return -1;
        }
        if (ssh_view_equals_text(&request, "exec")) {
            if (ssh_cursor_take_string(&cursor, &command_view) != 0 ||
                sshd_copy_command(&command_view, command, sizeof(command)) != 0) {
                if (want_reply) (void)sshd_send_channel_failure(t, ch.remote_id);
                return -1;
            }
            if (want_reply && sshd_send_channel_success(t, ch.remote_id) != 0) {
                return -1;
            }
            return sshd_run_exec(t, config, ch.remote_id, command);
        }
        if (want_reply) {
            if (sshd_send_channel_failure(t, ch.remote_id) != 0) {
                return -1;
            }
        }
    }
}

static int sshd_handle_client(int fd, const SshdConfig *config) {
    SshdTransport t;
    int status = -1;

    rt_memset(&t, 0, sizeof(t));
    t.fd = fd;
    if (sshd_transport_kex(&t, config) == 0 &&
        sshd_authenticate(&t, config) == 0 &&
        sshd_handle_channel(&t, config) == 0) {
        status = 0;
    }
    crypto_secure_bzero(&t, sizeof(t));
    return status;
}

int sshd_run(const SshdConfig *config) {
    int listener = -1;
    int client = -1;

    if (config == 0 || config->port == 0U || !config->password_set) {
        return -1;
    }
    if (!config->host_seed_set) {
        rt_write_line(2, "sshd: using ephemeral host key; use -k for a persistent identity");
    }
    if (platform_open_tcp_listener(config->address, config->port, &listener) != 0) {
        tool_write_error("sshd", "listen failed on ", config->address);
        return -1;
    }
    if (config->verbose) {
        rt_write_cstr(1, "sshd: listening on ");
        rt_write_cstr(1, config->address);
        rt_write_cstr(1, ":");
        rt_write_uint(1, config->port);
        rt_write_char(1, '\n');
    }

    do {
        if (platform_accept_tcp(listener, &client) != 0) {
            platform_close(listener);
            return -1;
        }
        (void)sshd_handle_client(client, config);
        platform_close(client);
        client = -1;
    } while (!config->single_client);

    platform_close(listener);
    return 0;
}
