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
#define PLATFORM_SEEK_SET 0
#define PLATFORM_SEEK_CUR 1
#define PLATFORM_SEEK_END 2
#define PLATFORM_MOUNT_RDONLY     (1ULL << 0)
#define PLATFORM_MOUNT_NOSUID     (1ULL << 1)
#define PLATFORM_MOUNT_NODEV      (1ULL << 2)
#define PLATFORM_MOUNT_NOEXEC     (1ULL << 3)
#define PLATFORM_MOUNT_SYNC       (1ULL << 4)
#define PLATFORM_MOUNT_REMOUNT    (1ULL << 5)
#define PLATFORM_MOUNT_MANDLOCK   (1ULL << 6)
#define PLATFORM_MOUNT_DIRSYNC    (1ULL << 7)
#define PLATFORM_MOUNT_NOATIME    (1ULL << 8)
#define PLATFORM_MOUNT_NODIRATIME (1ULL << 9)
#define PLATFORM_MOUNT_BIND       (1ULL << 10)
#define PLATFORM_MOUNT_REC        (1ULL << 11)
#define PLATFORM_MOUNT_SILENT     (1ULL << 12)
#define PLATFORM_MOUNT_RELATIME   (1ULL << 13)
#define PLATFORM_MOUNT_STRICTATIME (1ULL << 14)
#define PLATFORM_MOUNT_LAZYTIME   (1ULL << 15)

#define PLATFORM_SHUTDOWN_HALT 0
#define PLATFORM_SHUTDOWN_POWEROFF 1
#define PLATFORM_SHUTDOWN_REBOOT 2

typedef struct {
    char name[PLATFORM_NAME_CAPACITY];
    unsigned long long device;
    unsigned int mode;
    unsigned long long size;
    unsigned long long inode;
    unsigned long nlink;
    long long atime;
    long long mtime;
    long long ctime;
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
    unsigned long long shared_bytes;
    unsigned long long buffer_bytes;
    unsigned long long cache_bytes;
    unsigned long long swap_total_bytes;
    unsigned long long swap_free_bytes;
} PlatformMemoryInfo;

typedef struct {
    unsigned long long total_bytes;
    unsigned long long free_bytes;
    unsigned long long available_bytes;
    unsigned long long total_inodes;
    unsigned long long free_inodes;
    unsigned long long available_inodes;
    char type_name[PLATFORM_NAME_CAPACITY];
} PlatformFilesystemInfo;

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
    int listen_mode;
    int use_udp;
    int scan_mode;
    unsigned int timeout_milliseconds;
} PlatformNetcatOptions;

typedef struct {
    unsigned char bytes[PLATFORM_TERMINAL_STATE_CAPACITY];
} PlatformTerminalState;

long platform_write(int fd, const void *buffer, size_t count);
long platform_read(int fd, void *buffer, size_t count);
int platform_open_read(const char *path);
int platform_open_write(const char *path, unsigned int mode);
int platform_open_write_mode(const char *path, unsigned int mode, int truncate_existing);
int platform_open_append(const char *path, unsigned int mode);
long long platform_seek(int fd, long long offset, int whence);
int platform_create_temp_file(char *path_buffer, size_t buffer_size, const char *prefix, unsigned int mode);
int platform_close(int fd);
int platform_make_directory(const char *path, unsigned int mode);
int platform_remove_file(const char *path);
int platform_remove_directory(const char *path);
int platform_mount_filesystem(
    const char *source,
    const char *target,
    const char *filesystem_type,
    unsigned long long flags,
    const char *data
);
int platform_unmount_filesystem(const char *target, int force, int lazy);
int platform_rename_path(const char *old_path, const char *new_path);
int platform_create_hard_link(const char *target_path, const char *link_path);
int platform_create_symbolic_link(const char *target_path, const char *link_path);
int platform_change_mode(const char *path, unsigned int mode);
int platform_change_owner_ex(const char *path, unsigned int uid, unsigned int gid, int follow_symlinks);
int platform_change_owner(const char *path, unsigned int uid, unsigned int gid);
int platform_touch_path(const char *path);
int platform_set_path_times(
    const char *path,
    long long atime,
    long long mtime,
    int create_if_missing,
    int update_access,
    int update_modify
);
int platform_path_access(const char *path, int mode);
int platform_change_directory(const char *path);
const char *platform_getenv(const char *name);
const char *platform_getenv_entry(size_t index);
int platform_setenv(const char *name, const char *value, int overwrite);
int platform_unsetenv(const char *name);
int platform_clearenv(void);
int platform_isatty(int fd);
int platform_get_process_id(void);
long platform_read_kernel_log(char *buffer, size_t buffer_size, int clear_after_read);
int platform_clear_kernel_log(void);
int platform_set_console_log_level(int level);
int platform_random_bytes(unsigned char *buffer, size_t count);
int platform_terminal_enable_raw_mode(int fd, PlatformTerminalState *state_out);
int platform_terminal_restore_mode(int fd, const PlatformTerminalState *state);
int platform_sleep_milliseconds(unsigned long long milliseconds);
int platform_sleep_seconds(unsigned int seconds);
long long platform_get_epoch_time(void);
int platform_format_time(long long epoch_seconds, int use_local_time, const char *format, char *buffer, size_t buffer_size);
int platform_send_signal(int pid, int signal_number);
int platform_ignore_signal(int signal_number);
int platform_shutdown_system(int action);
int platform_parse_signal_name(const char *text, int *signal_out);
const char *platform_signal_name(int signal_number);
void platform_write_signal_list(int fd);
int platform_get_hostname(char *buffer, size_t buffer_size);
int platform_set_hostname(const char *name);
int platform_netcat(const char *host, unsigned int port, const PlatformNetcatOptions *options);
int platform_netcat_tcp(const char *host, unsigned int port, int listen_mode);
int platform_connect_tcp(const char *host, unsigned int port, int *socket_fd_out);
int platform_poll_fds(const int *fds, size_t fd_count, size_t *ready_index_out, int timeout_milliseconds);
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
    unsigned long long timeout_milliseconds,
    unsigned long long kill_after_milliseconds,
    int signal_number,
    int preserve_status,
    int *exit_status_out
);
int platform_list_processes(PlatformProcessEntry *entries_out, size_t entry_capacity, size_t *count_out);
int platform_get_identity(PlatformIdentity *identity_out);
int platform_lookup_identity(const char *username, PlatformIdentity *identity_out);
int platform_lookup_group(const char *groupname, unsigned int *gid_out);
int platform_list_groups_for_identity(
    const PlatformIdentity *identity,
    PlatformGroupEntry *entries_out,
    size_t entry_capacity,
    size_t *count_out
);
int platform_list_sessions(PlatformSessionEntry *entries_out, size_t entry_capacity, size_t *count_out);
int platform_get_memory_info(PlatformMemoryInfo *info_out);
int platform_get_uptime_info(PlatformUptimeInfo *info_out);
int platform_get_uname(
    char *sysname,
    size_t sysname_size,
    char *nodename,
    size_t nodename_size,
    char *release,
    size_t release_size,
    char *version,
    size_t version_size,
    char *machine,
    size_t machine_size
);
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
int platform_get_path_info_follow(const char *path, PlatformDirEntry *entry_out);
int platform_read_symlink(const char *path, char *buffer, size_t buffer_size);
int platform_get_filesystem_info(const char *path, PlatformFilesystemInfo *info_out);
int platform_get_filesystem_usage(const char *path, unsigned long long *total_bytes_out, unsigned long long *free_bytes_out, unsigned long long *available_bytes_out);
int platform_truncate_path(const char *path, unsigned long long size);
int platform_sync_all(void);
int platform_sync_path(const char *path);
int platform_sync_path_data(const char *path);

void platform_free_entries(PlatformDirEntry *entries, size_t count);
void platform_format_mode(unsigned int mode, char out[11]);

#endif
