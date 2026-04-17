#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define POSIX_ICMP_ECHO 8

typedef struct {
    unsigned char type;
    unsigned char code;
    unsigned short checksum;
    unsigned short identifier;
    unsigned short sequence;
} PosixIcmpPacket;

static unsigned short compute_icmp_checksum(const void *data, size_t length) {
    const unsigned short *words = (const unsigned short *)data;
    unsigned int sum = 0;
    size_t remaining = length;

    while (remaining > 1) {
        sum += *words++;
        remaining -= 2;
    }

    if (remaining == 1) {
        sum += *(const unsigned char *)words;
    }

    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffU) + (sum >> 16);
    }

    return (unsigned short)~sum;
}

static int write_socket_all(int fd, const char *buffer, size_t count) {
    size_t offset = 0;

    while (offset < count) {
        ssize_t written = send(fd, buffer + offset, count - offset, 0);
        if (written <= 0) {
            return -1;
        }
        offset += (size_t)written;
    }

    return 0;
}

static int stream_fd_to_socket(int input_fd, int sock) {
    char buffer[4096];
    ssize_t bytes_read;

    while ((bytes_read = read(input_fd, buffer, sizeof(buffer))) > 0) {
        if (write_socket_all(sock, buffer, (size_t)bytes_read) != 0) {
            return -1;
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

static int stream_socket_to_stdout(int sock) {
    char buffer[4096];
    ssize_t bytes_read;

    while ((bytes_read = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
        if (rt_write_all(1, buffer, (size_t)bytes_read) != 0) {
            return -1;
        }
    }

    return bytes_read < 0 ? -1 : 0;
}

int platform_netcat_tcp(const char *host, unsigned int port, int listen_mode) {
    struct sockaddr_in addr;
    int sock;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    if (listen_mode) {
        int client;
        int reuse = 1;
        (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(sock, 1) != 0) {
            close(sock);
            return -1;
        }
        client = accept(sock, NULL, NULL);
        close(sock);
        if (client < 0) {
            return -1;
        }
        if (stream_socket_to_stdout(client) != 0) {
            close(client);
            return -1;
        }
        close(client);
        return 0;
    }

    if (host == NULL || inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(sock);
        errno = EINVAL;
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }

#if __STDC_HOSTED__
    if (!isatty(0)) {
        if (stream_fd_to_socket(0, sock) != 0) {
            close(sock);
            return -1;
        }
        (void)shutdown(sock, SHUT_WR);
    }
#endif

    if (stream_socket_to_stdout(sock) != 0) {
        close(sock);
        return -1;
    }

    close(sock);
    return 0;
}

int platform_ping_host(const char *host, unsigned int count) {
    struct sockaddr_in addr;
    struct timeval timeout;
    int sock;
    unsigned int i;

    if (host == NULL || count == 0) {
        errno = EINVAL;
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (sock < 0) {
        sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    }
    if (sock < 0) {
        return -1;
    }

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    for (i = 0; i < count; ++i) {
        PosixIcmpPacket packet;
        unsigned char reply[256];
        ssize_t received;

        memset(&packet, 0, sizeof(packet));
        packet.type = POSIX_ICMP_ECHO;
        packet.code = 0;
        packet.identifier = (unsigned short)getpid();
        packet.sequence = (unsigned short)(i + 1);
        packet.checksum = compute_icmp_checksum(&packet, sizeof(packet));

        if (sendto(sock, &packet, sizeof(packet), 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(sock);
            return -1;
        }

        received = recvfrom(sock, reply, sizeof(reply), 0, NULL, NULL);
        if (received < 0) {
            close(sock);
            return -1;
        }
    }

    close(sock);
    return 0;
}
