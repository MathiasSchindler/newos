#ifndef NEWOS_PLATFORM_H
#define NEWOS_PLATFORM_H

#include <stddef.h>

#define PLATFORM_NAME_CAPACITY 256
#define PLATFORM_OWNER_CAPACITY 32
#define PLATFORM_GROUP_CAPACITY 32

typedef struct {
    char name[PLATFORM_NAME_CAPACITY];
    unsigned int mode;
    unsigned long long size;
    unsigned long long inode;
    unsigned long nlink;
    long long mtime;
    char owner[PLATFORM_OWNER_CAPACITY];
    char group[PLATFORM_GROUP_CAPACITY];
    int is_dir;
    int is_hidden;
} PlatformDirEntry;

typedef struct {
    int pid;
    char name[PLATFORM_NAME_CAPACITY];
} PlatformProcessEntry;

typedef struct {
    unsigned int uid;
    unsigned int gid;
    char username[PLATFORM_NAME_CAPACITY];
    char groupname[PLATFORM_NAME_CAPACITY];
} PlatformIdentity;

long platform_write(int fd, const void *buffer, size_t count);
long platform_read(int fd, void *buffer, size_t count);
int platform_open_read(const char *path);
int platform_open_write(const char *path, unsigned int mode);
int platform_close(int fd);
int platform_make_directory(const char *path, unsigned int mode);
int platform_remove_file(const char *path);
int platform_remove_directory(const char *path);
int platform_rename_path(const char *old_path, const char *new_path);
int platform_create_hard_link(const char *target_path, const char *link_path);
int platform_create_symbolic_link(const char *target_path, const char *link_path);
int platform_change_mode(const char *path, unsigned int mode);
int platform_touch_path(const char *path);
int platform_change_directory(const char *path);
int platform_sleep_seconds(unsigned int seconds);
long long platform_get_epoch_time(void);
int platform_create_pipe(int pipe_fds[2]);
int platform_spawn_process(
    char *const argv[],
    int stdin_fd,
    int stdout_fd,
    const char *input_path,
    const char *output_path,
    int output_append,
    int *pid_out
);
int platform_wait_process(int pid, int *exit_status_out);
int platform_list_processes(PlatformProcessEntry *entries_out, size_t entry_capacity, size_t *count_out);
int platform_get_identity(PlatformIdentity *identity_out);
int platform_get_uname(char *sysname, size_t sysname_size, char *nodename, size_t nodename_size, char *release, size_t release_size, char *machine, size_t machine_size);
int platform_ping_host(const char *host, unsigned int count);

int platform_collect_entries(
    const char *path,
    int include_hidden,
    PlatformDirEntry *entries_out,
    size_t entry_capacity,
    size_t *count_out,
    int *path_is_directory
);
int platform_path_is_directory(const char *path, int *is_directory_out);

int platform_stream_file_to_stdout(const char *path);
int platform_get_current_directory(char *buffer, size_t buffer_size);
int platform_get_path_info(const char *path, PlatformDirEntry *entry_out);

void platform_free_entries(PlatformDirEntry *entries, size_t count);
void platform_format_mode(unsigned int mode, char out[11]);

#endif
