#ifndef NEWOS_SSH_CLIENT_H
#define NEWOS_SSH_CLIENT_H

#include <stddef.h>

typedef struct {
    const char *host;
    const char *user;
    unsigned int port;
    const char *password;
    const char *identity_path;
    int verbose;
} SshClientConfig;

typedef int (*SshClientDataCallback)(const unsigned char *data, size_t size, int extended, void *user_data);

typedef struct {
    SshClientConfig client;
    const char *command;
    const unsigned char *input;
    size_t input_size;
    SshClientDataCallback output_callback;
    void *output_user_data;
} SshClientExecConfig;

int ssh_client_connect_and_run(const SshClientConfig *config);
int ssh_client_exec(const SshClientExecConfig *config, int *exit_status_out);

#endif
