static int gitd_pem_marker_matches(const unsigned char *data, size_t offset, size_t size, const char *marker) {
    size_t length = rt_strlen(marker);

    return offset + length <= size && memcmp(data + offset, marker, length) == 0;
}

static int gitd_decode_pem_block(const unsigned char *data, size_t size, const char *label, unsigned char **der_out, size_t *der_size_out) {
    char begin_marker[96];
    char end_marker[96];
    size_t begin_length;
    size_t end_length;
    size_t begin = size;
    size_t end = size;
    size_t index;
    unsigned char *der;
    size_t der_capacity;
    size_t der_size = 0U;
    unsigned int value = 0U;
    unsigned int bits = 0U;

    if (data == 0 || label == 0 || der_out == 0 || der_size_out == 0) return -1;
    if (rt_strlen(label) + 32U >= sizeof(begin_marker)) return -1;
    rt_copy_string(begin_marker, sizeof(begin_marker), "-----BEGIN ");
    rt_copy_string(begin_marker + rt_strlen(begin_marker), sizeof(begin_marker) - rt_strlen(begin_marker), label);
    rt_copy_string(begin_marker + rt_strlen(begin_marker), sizeof(begin_marker) - rt_strlen(begin_marker), "-----");
    rt_copy_string(end_marker, sizeof(end_marker), "-----END ");
    rt_copy_string(end_marker + rt_strlen(end_marker), sizeof(end_marker) - rt_strlen(end_marker), label);
    rt_copy_string(end_marker + rt_strlen(end_marker), sizeof(end_marker) - rt_strlen(end_marker), "-----");
    begin_length = rt_strlen(begin_marker);
    end_length = rt_strlen(end_marker);
    for (index = 0U; index + begin_length <= size; ++index) {
        if (gitd_pem_marker_matches(data, index, size, begin_marker)) {
            begin = index + begin_length;
            break;
        }
    }
    if (begin == size) return -1;
    for (index = begin; index + end_length <= size; ++index) {
        if (gitd_pem_marker_matches(data, index, size, end_marker)) {
            end = index;
            break;
        }
    }
    if (end == size || end <= begin) return -1;
    der_capacity = ((end - begin) / 4U + 1U) * 3U;
    der = (unsigned char *)rt_malloc(der_capacity == 0U ? 1U : der_capacity);
    if (der == 0) return -1;
    for (index = begin; index < end; ++index) {
        int digit;
        unsigned char ch = data[index];

        if (ch == '=' || ch == '-' || ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') continue;
        digit = tool_base64_value((char)ch);
        if (digit < 0) {
            rt_free(der);
            return -1;
        }
        value = (value << 6U) | (unsigned int)digit;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            if (der_size >= der_capacity) {
                rt_free(der);
                return -1;
            }
            der[der_size++] = (unsigned char)((value >> bits) & 0xffU);
        }
    }
    *der_out = der;
    *der_size_out = der_size;
    return der_size > 0U ? 0 : -1;
}

static int gitd_load_pem_or_der_file(const char *path, const char *label, unsigned char **der_out, size_t *der_size_out) {
    unsigned char *data = 0;
    size_t size = 0U;

    if (git_read_file(path, &data, &size) != 0) return -1;
    if (gitd_decode_pem_block(data, size, label, der_out, der_size_out) == 0) {
        rt_free(data);
        return 0;
    }
    *der_out = data;
    *der_size_out = size;
    return size > 0U ? 0 : -1;
}

static int gitd_load_tls_config(const GitdOptions *options, GitdTlsConfig *config) {
    unsigned char *key_der = 0;
    size_t key_der_size = 0U;
    int result = -1;

    rt_memset(config, 0, sizeof(*config));
    if (options->tls_cert_path[0] == '\0' && options->tls_key_path[0] == '\0') return 0;
    if (options->tls_cert_path[0] == '\0' || options->tls_key_path[0] == '\0') return -1;
    if (gitd_load_pem_or_der_file(options->tls_cert_path, "CERTIFICATE", &config->cert_der, &config->cert_der_len) != 0) goto done;
    if (gitd_load_pem_or_der_file(options->tls_key_path, "RSA PRIVATE KEY", &key_der, &key_der_size) != 0) goto done;
    if (crypto_rsa2048_parse_private_key_der(&config->rsa_key, key_der, key_der_size) != 0) goto done;
    config->enabled = 1;
    result = 0;
done:
    rt_free(key_der);
    if (result != 0) {
        rt_free(config->cert_der);
        rt_memset(config, 0, sizeof(*config));
    }
    return result;
}

static void gitd_tls_config_destroy(GitdTlsConfig *config) {
    if (config == 0) return;
    rt_free(config->cert_der);
    crypto_secure_bzero(&config->rsa_key, sizeof(config->rsa_key));
    rt_memset(config, 0, sizeof(*config));
}
