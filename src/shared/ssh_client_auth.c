/* ssh_client_auth.c - user authentication (none / publickey / password) */

#include "ssh_client_internal.h"

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

int ssh_authenticate_user(
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
