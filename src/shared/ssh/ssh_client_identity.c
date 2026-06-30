/* ssh_client_identity.c - Ed25519 identity loading (raw and OpenSSH PEM) */

#include "ssh_client_internal.h"

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

int ssh_parse_ed25519_public_key_blob(const SshStringView *blob, unsigned char public_key[32]) {
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

int ssh_parse_ed25519_signature_blob(const SshStringView *blob, unsigned char signature[64]) {
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

int ssh_try_load_identity(const char *identity_path, SshIdentity *identity) {
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
