#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef IP_TTL
#define IP_TTL 4
#endif

#define POSIX_ICMP_ECHO 8
#define POSIX_ICMP_REPLY 0

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

static int resolve_ping_host(const char *host, struct sockaddr_in *addr_out, char *ip_out, size_t ip_out_size) {
    struct addrinfo hints;
    struct addrinfo *results = 0;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    rc = getaddrinfo(host, 0, &hints, &results);
    if (rc != 0 || results == 0) {
        return -1;
    }

    memcpy(addr_out, results->ai_addr, sizeof(*addr_out));
    if (inet_ntop(AF_INET, &addr_out->sin_addr, ip_out, (socklen_t)ip_out_size) == 0) {
        freeaddrinfo(results);
        return -1;
    }

    freeaddrinfo(results);
    return 0;
}

static double elapsed_milliseconds(const struct timeval *start, const struct timeval *end) {
    long seconds = (long)(end->tv_sec - start->tv_sec);
    long usec = (long)(end->tv_usec - start->tv_usec);
    return (double)(seconds * 1000L) + (double)usec / 1000.0;
}

static void write_milliseconds(double milliseconds) {
    unsigned long long whole_ms = (unsigned long long)milliseconds;
    unsigned long long frac_ms = (unsigned long long)((milliseconds - (double)whole_ms) * 1000.0);

    rt_write_uint(1, whole_ms);
    rt_write_char(1, '.');
    if (frac_ms < 100ULL) {
        rt_write_char(1, '0');
    }
    if (frac_ms < 10ULL) {
        rt_write_char(1, '0');
    }
    rt_write_uint(1, frac_ms);
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
    int sock = -1;

    memset(&addr, 0, sizeof(addr));

    if (listen_mode) {
        int client;
        int reuse = 1;
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            return -1;
        }
        addr.sin_family = AF_INET;
        addr.sin_port = htons((unsigned short)port);
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

    if (host == NULL) {
        errno = EINVAL;
        return -1;
    }

    {
        struct addrinfo hints;
        struct addrinfo *results = 0;
        struct addrinfo *current;
        char port_text[16];

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned int)port);

        if (getaddrinfo(host, port_text, &hints, &results) != 0) {
            errno = EINVAL;
            return -1;
        }

        for (current = results; current != 0; current = current->ai_next) {
            sock = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
            if (sock < 0) {
                continue;
            }
            if (connect(sock, current->ai_addr, current->ai_addrlen) == 0) {
                break;
            }
            close(sock);
            sock = -1;
        }

        freeaddrinfo(results);
    }

    if (sock < 0) {
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

int platform_ping_host(const char *host, const PlatformPingOptions *options) {
    struct sockaddr_in addr;
    char ip_text[INET_ADDRSTRLEN];
    struct timeval timeout;
    PlatformPingOptions effective_options;
    int sock;
    unsigned int seq;
    unsigned int transmitted = 0;
    unsigned int received_count = 0;
    unsigned short identifier;
    double min_ms = 0.0;
    double max_ms = 0.0;
    double total_ms = 0.0;

    if (options == NULL) {
        effective_options.count = PLATFORM_PING_DEFAULT_COUNT;
        effective_options.interval_seconds = PLATFORM_PING_DEFAULT_INTERVAL_SECONDS;
        effective_options.timeout_seconds = PLATFORM_PING_DEFAULT_TIMEOUT_SECONDS;
        effective_options.payload_size = PLATFORM_PING_DEFAULT_PAYLOAD_SIZE;
        effective_options.ttl = 0;
        options = &effective_options;
    }

    if (host == NULL || options->count == 0 || options->timeout_seconds == 0 ||
        options->payload_size > PLATFORM_PING_MAX_PAYLOAD_SIZE || options->ttl > PLATFORM_PING_MAX_TTL) {
        errno = EINVAL;
        return 1;
    }

    if (resolve_ping_host(host, &addr, ip_text, sizeof(ip_text)) != 0) {
        tool_write_error("ping", "unknown host ", host);
        return 1;
    }

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (sock < 0) {
        sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    }
    if (sock < 0) {
        tool_write_error("ping", "unable to create ICMP socket", 0);
        return 1;
    }

    timeout.tv_sec = (time_t)options->timeout_seconds;
    timeout.tv_usec = 0;
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (options->ttl > 0U) {
        int ttl_value = (int)options->ttl;
        (void)setsockopt(sock, IPPROTO_IP, IP_TTL, &ttl_value, sizeof(ttl_value));
    }

    identifier = (unsigned short)getpid();

    rt_write_cstr(1, "PING ");
    rt_write_cstr(1, host);
    rt_write_cstr(1, " (");
    rt_write_cstr(1, ip_text);
    rt_write_cstr(1, ") ");
    rt_write_uint(1, options->payload_size);
    rt_write_line(1, " data bytes");

    for (seq = 1U; seq <= options->count; ++seq) {
        unsigned char packet[sizeof(PosixIcmpPacket) + PLATFORM_PING_MAX_PAYLOAD_SIZE];
        unsigned char reply[sizeof(packet) + 128];
        PosixIcmpPacket *header = (PosixIcmpPacket *)packet;
        struct timeval start_time;
        struct timeval end_time;
        ssize_t reply_size;
        size_t packet_size = sizeof(PosixIcmpPacket) + options->payload_size;
        int matched = 0;

        memset(packet, 0, packet_size);
        header->type = POSIX_ICMP_ECHO;
        header->code = 0;
        header->identifier = htons(identifier);
        header->sequence = htons((unsigned short)seq);
        memset(packet + sizeof(PosixIcmpPacket), 0x42, options->payload_size);
        header->checksum = compute_icmp_checksum(packet, packet_size);

        (void)gettimeofday(&start_time, 0);
        if (sendto(sock, packet, packet_size, 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            close(sock);
            tool_write_error("ping", "send failed to ", host);
            return 1;
        }
        transmitted += 1U;

        for (;;) {
            reply_size = recvfrom(sock, reply, sizeof(reply), 0, 0, 0);
            if (reply_size < 0) {
                break;
            }

            {
                size_t offset = 0;
                const PosixIcmpPacket *reply_header;

                if ((size_t)reply_size >= 20U && (reply[0] >> 4) == 4U) {
                    offset = (size_t)(reply[0] & 0x0fU) * 4U;
                }
                if ((size_t)reply_size < offset + sizeof(PosixIcmpPacket)) {
                    continue;
                }

                reply_header = (const PosixIcmpPacket *)(reply + offset);
                if (reply_header->type == POSIX_ICMP_REPLY &&
                    ntohs(reply_header->identifier) == identifier &&
                    ntohs(reply_header->sequence) == (unsigned short)seq) {
                    double rtt_ms = 0.0;

                    (void)gettimeofday(&end_time, 0);
                    rtt_ms = elapsed_milliseconds(&start_time, &end_time);
                    if (received_count == 0U || rtt_ms < min_ms) {
                        min_ms = rtt_ms;
                    }
                    if (received_count == 0U || rtt_ms > max_ms) {
                        max_ms = rtt_ms;
                    }
                    total_ms += rtt_ms;
                    received_count += 1U;

                    rt_write_uint(1, (unsigned long long)((reply_size > (ssize_t)offset) ? (reply_size - (ssize_t)offset) : reply_size));
                    rt_write_cstr(1, " bytes from ");
                    rt_write_cstr(1, ip_text);
                    rt_write_cstr(1, ": icmp_seq=");
                    rt_write_uint(1, seq);
                    rt_write_cstr(1, " time=");
                    write_milliseconds(rtt_ms);
                    rt_write_line(1, " ms");

                    matched = 1;
                    break;
                }
            }
        }

        if (!matched) {
            rt_write_cstr(1, "Request timeout for icmp_seq ");
            rt_write_uint(1, seq);
            rt_write_char(1, '\n');
        }

        if (seq < options->count && options->interval_seconds > 0U) {
            (void)platform_sleep_seconds(options->interval_seconds);
        }
    }

    close(sock);

    rt_write_cstr(1, "--- ");
    rt_write_cstr(1, host);
    rt_write_line(1, " ping statistics ---");
    rt_write_uint(1, transmitted);
    rt_write_cstr(1, " packets transmitted, ");
    rt_write_uint(1, received_count);
    rt_write_cstr(1, " packets received, ");
    if (transmitted > 0U) {
        unsigned long long loss = ((unsigned long long)(transmitted - received_count) * 100ULL) / (unsigned long long)transmitted;
        rt_write_uint(1, loss);
    } else {
        rt_write_uint(1, 0ULL);
    }
    rt_write_line(1, "% packet loss");

    if (received_count > 0U) {
        rt_write_cstr(1, "round-trip min/avg/max = ");
        write_milliseconds(min_ms);
        rt_write_char(1, '/');
        write_milliseconds(total_ms / (double)received_count);
        rt_write_char(1, '/');
        write_milliseconds(max_ms);
        rt_write_line(1, " ms");
    }

    return received_count > 0U ? 0 : 1;
}
