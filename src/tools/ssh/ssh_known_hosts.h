#ifndef NEWOS_SSH_KNOWN_HOSTS_H
#define NEWOS_SSH_KNOWN_HOSTS_H

#include <stddef.h>

#define SSH_KNOWN_HOSTS_PATH_CAPACITY 1024

typedef enum {
    SSH_KNOWN_HOST_UNKNOWN = 0,
    SSH_KNOWN_HOST_MATCH = 1,
    SSH_KNOWN_HOST_MISMATCH = 2
} SshKnownHostStatus;

int ssh_known_hosts_default_path(char *buffer, size_t buffer_size);
int ssh_known_hosts_lookup(
    const char *path,
    const char *host,
    unsigned int port,
    const char *algorithm,
    const unsigned char *key_blob,
    size_t key_blob_length,
    SshKnownHostStatus *status_out
);
int ssh_known_hosts_append(
    const char *path,
    const char *host,
    unsigned int port,
    const char *algorithm,
    const unsigned char *key_blob,
    size_t key_blob_length
);

#endif
