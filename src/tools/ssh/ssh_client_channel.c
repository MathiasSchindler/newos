/* ssh_client_channel.c - SSH channel management and interactive shell session */

#include "ssh_client_internal.h"

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

int ssh_start_interactive_shell(
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
