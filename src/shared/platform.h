#ifndef NEWOS_PLATFORM_H
#define NEWOS_PLATFORM_H

#include <stddef.h>

#define PLATFORM_NAME_CAPACITY 256
#define PLATFORM_OWNER_CAPACITY 32
#define PLATFORM_GROUP_CAPACITY 32
#define PLATFORM_TERMINAL_STATE_CAPACITY 128
#define PLATFORM_PING_DEFAULT_COUNT 4U
#define PLATFORM_PING_DEFAULT_INTERVAL_SECONDS 1U
#define PLATFORM_PING_DEFAULT_TIMEOUT_SECONDS 1U
#define PLATFORM_PING_DEFAULT_PAYLOAD_SIZE 56U
#define PLATFORM_PING_MAX_PAYLOAD_SIZE 1400U
#define PLATFORM_PING_MAX_TTL 255U
#define PLATFORM_ACCESS_EXISTS 0
#define PLATFORM_ACCESS_EXECUTE 1
#define PLATFORM_ACCESS_WRITE 2
#define PLATFORM_ACCESS_READ 4

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
    int ppid;
    unsigned int uid;
    unsigned long long rss_kb;
    char state[16];
    char user[PLATFORM_NAME_CAPACITY];
    char name[PLATFORM_NAME_CAPACITY];
} PlatformProcessEntry;

typedef struct {
    unsigned int uid;
    unsigned int gid;
    char username[PLATFORM_NAME_CAPACITY];
    char groupname[PLATFORM_NAME_CAPACITY];
} PlatformIdentity;

typedef struct {
    unsigned long long total_bytes;
    unsigned long long free_bytes;
    unsigned long long available_bytes;
} PlatformMemoryInfo;

typedef struct {
    unsigned long long uptime_seconds;
    char load_average[64];
} PlatformUptimeInfo;

typedef struct {
    unsigned int gid;
    char name[PLATFORM_NAME_CAPACITY];
} PlatformGroupEntry;

typedef struct {
    char username[PLATFORM_NAME_CAPACITY];
    char terminal[PLATFORM_NAME_CAPACITY];
    char host[PLATFORM_NAME_CAPACITY];
    long long login_time;
} PlatformSessionEntry;

typedef struct {
    unsigned int count;
    unsigned int interval_seconds;
    unsigned int timeout_seconds;
    unsigned int payload_size;
    unsigned int ttl;
} PlatformPingOptions;

typedef struct {
    unsigned char bytes[PLATFORM_TERMINAL_STATE_CAPACITY];
} PlatformTerminalState;

long platform_write(int fd, const void *buffer, size_t count);
long platform_read(int fd, void *buffer, size_t count);
int platform_open_read(const char *path);
int platform_open_write(const char *path, unsigned int mode);
int platform_open_append(const char *path, unsigned int mode);
int platform_create_temp_file(char *path_buffer, size_t buffer_size, const char *prefix, unsigned int mode);
int platform_close(int fd);
int platform_make_directory(const char *path, unsigned int mode);
int platform_remove_file(const char *path);
int platform_remove_directory(const char *path);
int platform_rename_path(const char *old_path, const char *new_path);
int platform_create_hard_link(const char *target_path, const char *link_path);
int platform_create_symbolic_link(const char *target_path, const char *link_path);
int platform_change_mode(const char *path, unsigned int mode);
int platform_change_owner(const char *path, unsigned int uid, unsigned int gid);
int platform_touch_path(const char *path);
int platform_path_access(const char *path, int mode);
int platform_change_directory(const char *path);
const char *platform_getenv(const char *name);
const char *platform_getenv_entry(size_t index);
int platform_setenv(const char *name, const char *value, int overwrite);
int platform_unsetenv(const char *name);
int platform_clearenv(void);
int platform_isatty(int fd);
int platform_get_process_id(void);
int platform_terminal_enable_raw_mode(int fd, PlatformTerminalState *state_out);
int platform_terminal_restore_mode(int fd, const PlatformTerminalState *state);
int platform_sleep_milliseconds(unsigned long long milliseconds);
int platform_sleep_seconds(unsigned int seconds);
long long platform_get_epoch_time(void);
int platform_format_time(long long epoch_seconds, int use_local_time, const char *format, char *buffer, size_t buffer_size);
int platform_send_signal(int pid, int signal_number);
int platform_parse_signal_name(const char *text, int *signal_out);
const char *platform_signal_name(int signal_number);
void platform_write_signal_list(int fd);
int platform_get_hostname(char *buffer, size_t buffer_size);
int platform_set_hostname(const char *name);
int platform_netcat_tcp(const char *host, unsigned int port, int listen_mode);
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
int platform_wait_process_timeout(
    int pid,
    unsigned int timeout_seconds,
    unsigned int kill_after_seconds,
    int signal_number,
    int preserve_status,
    int *exit_status_out
);
int platform_list_processes(PlatformProcessEntry *entries_out, size_t entry_capacity, size_t *count_out);
int platform_get_identity(PlatformIdentity *identity_out);
int platform_lookup_identity(const char *username, PlatformIdentity *identity_out);
int platform_list_groups_for_identity(
    const PlatformIdentity *identity,
    PlatformGroupEntry *entries_out,
    size_t entry_capacity,
    size_t *count_out
);
int platform_list_sessions(PlatformSessionEntry *entries_out, size_t entry_capacity, size_t *count_out);
int platform_get_memory_info(PlatformMemoryInfo *info_out);
int platform_get_uptime_info(PlatformUptimeInfo *info_out);
int platform_get_uname(char *sysname, size_t sysname_size, char *nodename, size_t nodename_size, char *release, size_t release_size, char *machine, size_t machine_size);
int platform_ping_host(const char *host, const PlatformPingOptions *options);

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
int platform_read_symlink(const char *path, char *buffer, size_t buffer_size);
int platform_get_filesystem_usage(const char *path, unsigned long long *total_bytes_out, unsigned long long *free_bytes_out, unsigned long long *available_bytes_out);
int platform_truncate_path(const char *path, unsigned long long size);
int platform_sync_all(void);
int platform_sync_path(const char *path);

void platform_free_entries(PlatformDirEntry *entries, size_t count);
void platform_format_mode(unsigned int mode, char out[11]);

#endif
