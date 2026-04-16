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
