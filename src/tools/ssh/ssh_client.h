#ifndef NEWOS_SSH_CLIENT_H
#define NEWOS_SSH_CLIENT_H

typedef struct {
    const char *host;
    const char *user;
    unsigned int port;
    const char *password;
    const char *identity_path;
    int verbose;
} SshClientConfig;

int ssh_client_connect_and_run(const SshClientConfig *config);

#endif
