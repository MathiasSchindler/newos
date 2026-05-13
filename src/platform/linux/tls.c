#include "platform.h"
#include "runtime.h"

#include "crypto/x509.h"
#include "tls/tls12_client.h"
#include "tls/tls13_client.h"

#include <stddef.h>

static const char *linux_tls_error = "none";
static char linux_tls_peer_status_storage[160] = "not-verified-native";
static const char *linux_tls_peer_status = linux_tls_peer_status_storage;

const char *platform_tls_last_error(void) {
    return linux_tls_error;
}

const char *platform_tls_peer_verification_status(void) {
    return linux_tls_peer_status;
}

static void linux_tls_set_error(const char *message) {
    linux_tls_error = message != 0 ? message : "unknown tls error";
}

static void linux_tls_set_peer_status(const char *message) {
    rt_copy_string(linux_tls_peer_status_storage, sizeof(linux_tls_peer_status_storage), message != 0 ? message : "unknown");
}

static unsigned char *linux_tls_read_file(const char *path, size_t *length_out) {
    unsigned char *buffer;
    size_t used = 0U;
    size_t cap = 1024U * 1024U;
    int fd;

    if (path == 0 || length_out == 0) {
        return 0;
    }
    fd = platform_open_read(path);
    if (fd < 0) {
        return 0;
    }
    buffer = (unsigned char *)rt_malloc(cap);
    if (buffer == 0) {
        (void)platform_close(fd);
        return 0;
    }
    while (used < cap) {
        long result = platform_read(fd, buffer + used, cap - used);
        if (result < 0) {
            rt_free(buffer);
            (void)platform_close(fd);
            return 0;
        }
        if (result == 0) {
            break;
        }
        used += (size_t)result;
    }
    (void)platform_close(fd);
    if (used == cap) {
        rt_free(buffer);
        return 0;
    }
    *length_out = used;
    return buffer;
}

static unsigned char *linux_tls_load_trust_pem(size_t *length_out) {
    static const char *paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/ssl/cert.pem",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem"
    };
    size_t i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        unsigned char *buffer = linux_tls_read_file(paths[i], length_out);
        if (buffer != 0) return buffer;
    }
    return 0;
}

static int linux_tls_verify_peer_certs(const CryptoX509DerCert *certs, size_t cert_count, const char *host) {
    unsigned char *trust_pem;
    size_t trust_pem_len = 0U;
    char status[160];
    int result;

    if (cert_count == 0U) {
        linux_tls_set_peer_status("no peer certificate");
        return -1;
    }
    trust_pem = linux_tls_load_trust_pem(&trust_pem_len);
    if (trust_pem == 0) {
        linux_tls_set_peer_status("no trust anchor bundle found");
        return -1;
    }
    status[0] = '\0';
    result = crypto_x509_verify_chain(certs, cert_count, host, platform_get_epoch_time(), trust_pem, trust_pem_len, status, sizeof(status));
    rt_free(trust_pem);
    linux_tls_set_peer_status(result == 0 ? "trusted" : status);
    return result;
}

static int linux_tls_verify_native_peer(Tls13Client *native, const char *host) {
    CryptoX509DerCert certs[TLS13_MAX_PEER_CERTS];
    size_t cert_count;

    cert_count = tls13_client_peer_certificates(native, certs, TLS13_MAX_PEER_CERTS);
    return linux_tls_verify_peer_certs(certs, cert_count, host);
}

static int linux_tls_verify_native12_peer(Tls12Client *native, const char *host) {
    CryptoX509DerCert certs[TLS12_MAX_PEER_CERTS];
    size_t cert_count;

    cert_count = tls12_client_peer_certificates(native, certs, TLS12_MAX_PEER_CERTS);
    return linux_tls_verify_peer_certs(certs, cert_count, host);
}

static Tls13Client *linux_native_tls_client(PlatformTlsClient *client) {
    return (Tls13Client *)client->opaque[0];
}

static Tls12Client *linux_native_tls12_client(PlatformTlsClient *client) {
    return (Tls12Client *)client->opaque[0];
}

static int linux_native_tls_version(PlatformTlsClient *client) {
    return client->opaque[1] == (void *)12 ? 12 : 13;
}

int platform_tls_connect(PlatformTlsClient *client, const char *host, unsigned int port) {
    Tls13Client *native;
    Tls12Client *native12;
    int socket_fd = -1;

    linux_tls_set_error("none");
    linux_tls_set_peer_status("not-verified-native");
    if (client == 0 || host == 0 || host[0] == '\0') {
        linux_tls_set_error("invalid tls connect arguments");
        return -1;
    }
    memset(client, 0, sizeof(*client));
    client->socket_fd = -1;
    if (platform_connect_tcp(host, port, &socket_fd) != 0) {
        linux_tls_set_error("tcp connect failed");
        return -1;
    }
    native = (Tls13Client *)rt_malloc(sizeof(*native));
    if (native == 0) {
        linux_tls_set_error("native tls state allocation failed");
        (void)platform_close(socket_fd);
        return -1;
    }
    tls13_client_init(native, socket_fd, 30000U);
    if (tls13_client_handshake(native, host, rt_strlen(host)) != 0) {
        linux_tls_set_error(tls13_client_last_error(native));
        rt_free(native);
        (void)platform_close(socket_fd);
        socket_fd = -1;
        if (platform_connect_tcp(host, port, &socket_fd) != 0) {
            linux_tls_set_error("tcp reconnect failed for tls12 fallback");
            return -1;
        }
        native12 = (Tls12Client *)rt_malloc(sizeof(*native12));
        if (native12 == 0) {
            linux_tls_set_error("native tls12 state allocation failed");
            (void)platform_close(socket_fd);
            return -1;
        }
        tls12_client_init(native12, socket_fd, 30000U);
        if (tls12_client_handshake(native12, host, rt_strlen(host)) != 0) {
            linux_tls_set_error(tls12_client_last_error(native12));
            rt_free(native12);
            (void)platform_close(socket_fd);
            return -1;
        }
        if (linux_tls_verify_native12_peer(native12, host) != 0) {
            linux_tls_set_error("certificate verification failed");
            rt_free(native12);
            (void)platform_close(socket_fd);
            return -1;
        }
        client->opaque[0] = native12;
        client->opaque[1] = (void *)12;
        client->socket_fd = socket_fd;
        client->active = 1;
        linux_tls_set_error("none");
        return 0;
    }
    if (linux_tls_verify_native_peer(native, host) != 0) {
        linux_tls_set_error("certificate verification failed");
        rt_free(native);
        (void)platform_close(socket_fd);
        return -1;
    }
    client->opaque[0] = native;
    client->opaque[1] = (void *)13;
    client->socket_fd = socket_fd;
    client->active = 1;
    linux_tls_set_error("none");
    return 0;
}

long platform_tls_read(PlatformTlsClient *client, void *buffer, size_t count) {
    Tls13Client *native;
    long result;

    if (client == 0 || buffer == 0 || count == 0U || !client->active) {
        linux_tls_set_error("invalid tls read arguments");
        return -1;
    }
    native = linux_native_tls_client(client);
    if (linux_native_tls_version(client) == 12) {
        result = tls12_client_read_app(linux_native_tls12_client(client), (unsigned char *)buffer, count);
        if (result < 0) linux_tls_set_error(tls12_client_last_error(linux_native_tls12_client(client)));
        return result;
    }
    result = tls13_client_read_app(native, (unsigned char *)buffer, count);
    if (result < 0) {
        linux_tls_set_error(tls13_client_last_error(native));
    }
    return result;
}

long platform_tls_write(PlatformTlsClient *client, const void *buffer, size_t count) {
    if (client == 0 || buffer == 0 || count == 0U || !client->active) {
        linux_tls_set_error("invalid tls write arguments");
        return -1;
    }
    if (linux_native_tls_version(client) == 12) {
        return tls12_client_write_app(linux_native_tls12_client(client), (const unsigned char *)buffer, count);
    }
    return tls13_client_write_app(linux_native_tls_client(client), (const unsigned char *)buffer, count);
}

void platform_tls_close(PlatformTlsClient *client) {
    Tls13Client *native;

    if (client != 0) {
        native = linux_native_tls_client(client);
        if (native != 0 && linux_native_tls_version(client) == 12) {
            (void)tls12_client_close_notify((Tls12Client *)native);
            rt_free(native);
        } else if (native != 0) {
            (void)tls13_client_close_notify(native);
            rt_free(native);
        }
        if (client->socket_fd >= 0) {
            (void)platform_close(client->socket_fd);
        }
        memset(client, 0, sizeof(*client));
        client->socket_fd = -1;
    }
}