#include "platform.h"
#include "runtime.h"

#include "tls/tls12_client.h"
#include "tls/tls13_client.h"

#define WIN_BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002UL

__declspec(dllimport) long __stdcall BCryptGenRandom(void *algorithm, unsigned char *buffer, unsigned long count, unsigned long flags);

static const char *windows_tls_error = "none";
static const char *windows_tls_peer_status = "not-verified-windows-freestanding";

const char *platform_tls_last_error(void) {
    return windows_tls_error;
}

const char *platform_tls_peer_verification_status(void) {
    return windows_tls_peer_status;
}

static void windows_tls_set_error(const char *message) {
    windows_tls_error = message != 0 ? message : "unknown tls error";
}

static Tls13Client *windows_native_tls_client(PlatformTlsClient *client) {
    return (Tls13Client *)client->opaque[0];
}

static Tls12Client *windows_native_tls12_client(PlatformTlsClient *client) {
    return (Tls12Client *)client->opaque[0];
}

static int windows_native_tls_version(PlatformTlsClient *client) {
    return client->opaque[1] == (void *)12 ? 12 : 13;
}

int platform_random_bytes(unsigned char *buffer, size_t count) {
    while (count > 0U) {
        unsigned long chunk = count > 0xffffffffUL ? 0xffffffffUL : (unsigned long)count;
        if (BCryptGenRandom(0, buffer, chunk, WIN_BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return -1;
        buffer += chunk;
        count -= chunk;
    }
    return 0;
}

int platform_tls_connect(PlatformTlsClient *client, const char *host, unsigned int port) {
    Tls13Client *native;
    Tls12Client *native12;
    int socket_fd = -1;

    windows_tls_set_error("none");
    windows_tls_peer_status = "not-verified-windows-freestanding";
    if (client == 0 || host == 0 || host[0] == '\0') {
        windows_tls_set_error("invalid tls connect arguments");
        return -1;
    }
    memset(client, 0, sizeof(*client));
    client->socket_fd = -1;
    if (platform_connect_tcp(host, port, &socket_fd) != 0) {
        windows_tls_set_error("tcp connect failed");
        return -1;
    }
    native = (Tls13Client *)rt_malloc(sizeof(*native));
    if (native == 0) {
        windows_tls_set_error("native tls state allocation failed");
        (void)platform_close(socket_fd);
        return -1;
    }
    tls13_client_init(native, socket_fd, 30000U);
    if (tls13_client_handshake(native, host, rt_strlen(host)) != 0) {
        windows_tls_set_error(tls13_client_last_error(native));
        rt_free(native);
        (void)platform_close(socket_fd);
        socket_fd = -1;
        if (platform_connect_tcp(host, port, &socket_fd) != 0) {
            windows_tls_set_error("tcp reconnect failed for tls12 fallback");
            return -1;
        }
        native12 = (Tls12Client *)rt_malloc(sizeof(*native12));
        if (native12 == 0) {
            windows_tls_set_error("native tls12 state allocation failed");
            (void)platform_close(socket_fd);
            return -1;
        }
        tls12_client_init(native12, socket_fd, 30000U);
        if (tls12_client_handshake(native12, host, rt_strlen(host)) != 0) {
            windows_tls_set_error(tls12_client_last_error(native12));
            rt_free(native12);
            (void)platform_close(socket_fd);
            return -1;
        }
        client->opaque[0] = native12;
        client->opaque[1] = (void *)12;
        client->socket_fd = socket_fd;
        client->active = 1;
        windows_tls_set_error("none");
        return 0;
    }
    client->opaque[0] = native;
    client->opaque[1] = (void *)13;
    client->socket_fd = socket_fd;
    client->active = 1;
    windows_tls_set_error("none");
    return 0;
}

long platform_tls_read(PlatformTlsClient *client, void *buffer, size_t count) {
    long result;

    if (client == 0 || buffer == 0 || count == 0U || !client->active) {
        windows_tls_set_error("invalid tls read arguments");
        return -1;
    }
    if (windows_native_tls_version(client) == 12) {
        result = tls12_client_read_app(windows_native_tls12_client(client), (unsigned char *)buffer, count);
        if (result < 0) windows_tls_set_error(tls12_client_last_error(windows_native_tls12_client(client)));
        return result;
    }
    result = tls13_client_read_app(windows_native_tls_client(client), (unsigned char *)buffer, count);
    if (result < 0) windows_tls_set_error(tls13_client_last_error(windows_native_tls_client(client)));
    return result;
}

long platform_tls_write(PlatformTlsClient *client, const void *buffer, size_t count) {
    if (client == 0 || buffer == 0 || count == 0U || !client->active) {
        windows_tls_set_error("invalid tls write arguments");
        return -1;
    }
    if (windows_native_tls_version(client) == 12) {
        return tls12_client_write_app(windows_native_tls12_client(client), (const unsigned char *)buffer, count);
    }
    return tls13_client_write_app(windows_native_tls_client(client), (const unsigned char *)buffer, count);
}

void platform_tls_close(PlatformTlsClient *client) {
    Tls13Client *native;

    if (client != 0) {
        native = windows_native_tls_client(client);
        if (native != 0 && windows_native_tls_version(client) == 12) {
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