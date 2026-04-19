/* ssh_client_io.c - low-level I/O helpers and binary packet transport */

#include "ssh_client_internal.h"

#include <string.h>

int ssh_write_all(int fd, const void *buffer, size_t count) {
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

int ssh_read_exact(int fd, void *buffer, size_t count) {
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

void ssh_store_be32(unsigned char out[4], unsigned int value) {
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

unsigned int ssh_read_be32(const unsigned char in[4]) {
    return ((unsigned int)in[0] << 24) |
           ((unsigned int)in[1] << 16) |
           ((unsigned int)in[2] << 8) |
           (unsigned int)in[3];
}

int ssh_copy_view_text(const SshStringView *view, char *buffer, size_t buffer_size) {
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

int ssh_view_equals_text(const SshStringView *view, const char *text) {
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

int ssh_find_text_span(const char *text, size_t text_len, const char *needle, size_t *pos_out) {
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

int ssh_send_packet(int fd, const unsigned char *payload, size_t payload_len) {
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

int ssh_read_packet(
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

int ssh_send_encrypted_packet(int fd, const unsigned char key[64], unsigned int seqnr, const unsigned char *payload, size_t payload_len) {
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

int ssh_read_encrypted_packet(
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

int ssh_read_banner(int fd, char *buffer, size_t buffer_size) {
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
