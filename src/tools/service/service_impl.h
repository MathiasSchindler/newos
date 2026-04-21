#ifndef NEWOS_SERVICE_IMPL_H
#define NEWOS_SERVICE_IMPL_H

#include <stddef.h>

#define SERVICE_PATH_CAPACITY 1024
#define SERVICE_COMMAND_CAPACITY 2048
#define SERVICE_MAX_ARGS 64U

typedef struct {
    char command[SERVICE_COMMAND_CAPACITY];
    char pidfile[SERVICE_PATH_CAPACITY];
    char stdout_path[SERVICE_PATH_CAPACITY];
    char stderr_path[SERVICE_PATH_CAPACITY];
    char workdir[SERVICE_PATH_CAPACITY];
    char drop_user[SERVICE_PATH_CAPACITY];
    char drop_group[SERVICE_PATH_CAPACITY];
    unsigned long long stop_timeout_ms;
} ServiceConfig;

int service_main(int argc, char **argv);
int service_load_config(const char *config_path, ServiceConfig *config_out);
int service_read_pidfile(const char *path, int *pid_out);
int service_write_pidfile(const char *path, int pid);
int service_remove_pidfile(const char *path);
int service_pid_is_running(int pid);
int service_split_command(const char *command, char *storage, size_t storage_size, char *argv_out[], size_t argv_capacity);
int service_start_process(const ServiceConfig *config, int *pid_out);
int service_stop_process(const ServiceConfig *config, int pid);

#endif
