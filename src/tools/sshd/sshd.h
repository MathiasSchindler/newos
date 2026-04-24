#ifndef NEWOS_SSHD_H
#define NEWOS_SSHD_H

#include <stddef.h>

#define SSHD_ADDRESS_CAPACITY 64
#define SSHD_PASSWORD_CAPACITY 256
#define SSHD_COMMAND_CAPACITY 512
#define SSHD_SHELL_CAPACITY 128

typedef struct {
    char address[SSHD_ADDRESS_CAPACITY];
    char user[128];
    char password[SSHD_PASSWORD_CAPACITY];
    char shell_path[SSHD_SHELL_CAPACITY];
    unsigned int port;
    int verbose;
    int single_client;
    int password_set;
    unsigned char host_seed[32];
    int host_seed_set;
} SshdConfig;

int sshd_run(const SshdConfig *config);

#endif
