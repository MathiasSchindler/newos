#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"
#include "runtime.h"
#include "syscall.h"

#define DARWIN_PROT_READ 1
#define DARWIN_PROT_WRITE 2
#define DARWIN_MAP_PRIVATE 2
#define DARWIN_MAP_ANONYMOUS 0x1000
#define DARWIN_O_WRONLY 0x0001
#define DARWIN_O_APPEND 0x0008
#define DARWIN_O_CREAT 0x0200
#define DARWIN_O_TRUNC 0x0400
#define DARWIN_O_EXCL 0x0800
#define DARWIN_ACCESS_EXECUTE 1
#define DARWIN_ACCESS_WRITE 2
#define DARWIN_ACCESS_READ 4
#define DARWIN_S_IFMT 0170000U
#define DARWIN_S_IFDIR 0040000U

typedef struct {
    long tv_sec;
    int tv_usec;
} DarwinTimeval;

typedef struct {
    const char *name;
    int value;
} DarwinSignalEntry;

static const DarwinSignalEntry DARWIN_SIGNAL_TABLE[] = {
    { "HUP", SIGHUP },
    { "INT", SIGINT },
    { "QUIT", SIGQUIT },
    { "ILL", SIGILL },
    { "TRAP", SIGTRAP },
    { "ABRT", SIGABRT },
    { "BUS", SIGBUS },
    { "FPE", SIGFPE },
    { "KILL", SIGKILL },
    { "USR1", SIGUSR1 },
    { "SEGV", SIGSEGV },
    { "USR2", SIGUSR2 },
    { "PIPE", SIGPIPE },
    { "ALRM", SIGALRM },
    { "TERM", SIGTERM },
    { "CHLD", SIGCHLD },
    { "CONT", SIGCONT },
    { "STOP", SIGSTOP },
    { "TSTP", SIGTSTP },
    { "TTIN", SIGTTIN },
    { "TTOU", SIGTTOU }
};

extern char **environ;
int rename(const char *old_path, const char *new_path);

#if defined(__APPLE__)
int sethostname(const char *name, int namelen);
#endif

static int env_name_matches(const char *entry, const char *name) {
    size_t index = 0;

    if (entry == 0 || name == 0 || name[0] == '\0') {
        return 0;
    }

    while (name[index] != '\0') {
        if (entry[index] != name[index]) {
            return 0;
        }
        index += 1U;
    }

    return entry[index] == '=';
}

static void fill_entry_from_stat(const char *display_name, const struct stat *stat_info, PlatformDirEntry *entry) {
    memset(entry, 0, sizeof(*entry));
    rt_copy_string(entry->name, sizeof(entry->name), display_name != 0 ? display_name : "");
    entry->device = (unsigned long long)stat_info->st_dev;
    entry->mode = (unsigned int)stat_info->st_mode;
    entry->uid = (unsigned int)stat_info->st_uid;
    entry->gid = (unsigned int)stat_info->st_gid;
    entry->size = (unsigned long long)stat_info->st_size;
    entry->inode = (unsigned long long)stat_info->st_ino;
    entry->nlink = (unsigned long)stat_info->st_nlink;
    entry->atime = (long long)stat_info->st_atime;
    entry->mtime = (long long)stat_info->st_mtime;
    entry->ctime = (long long)stat_info->st_ctime;
    entry->is_dir = ((unsigned int)stat_info->st_mode & DARWIN_S_IFMT) == DARWIN_S_IFDIR;
    entry->is_hidden = display_name != 0 && display_name[0] == '.';
    rt_unsigned_to_string((unsigned long long)stat_info->st_uid, entry->owner, sizeof(entry->owner));
    rt_unsigned_to_string((unsigned long long)stat_info->st_gid, entry->group, sizeof(entry->group));
}

static int fill_entry_mode(const char *display_name, const char *path, PlatformDirEntry *entry, int follow_symlinks) {
    struct stat stat_info;
    long result;

    if (path == 0 || entry == 0) {
        return -1;
    }

    result = darwin_syscall2(follow_symlinks ? DARWIN_SYS_STAT64 : DARWIN_SYS_LSTAT64, (long)path, (long)&stat_info);
    if (result < 0) {
        return -1;
    }

    fill_entry_from_stat(display_name, &stat_info, entry);
    return 0;
}

static int access_mode_to_darwin(int mode) {
    int darwin_mode = 0;

    if ((mode & PLATFORM_ACCESS_READ) != 0) {
        darwin_mode |= DARWIN_ACCESS_READ;
    }
    if ((mode & PLATFORM_ACCESS_WRITE) != 0) {
        darwin_mode |= DARWIN_ACCESS_WRITE;
    }
    if ((mode & PLATFORM_ACCESS_EXECUTE) != 0) {
        darwin_mode |= DARWIN_ACCESS_EXECUTE;
    }

    return darwin_mode;
}

static int signal_name_matches(const char *text, const char *name) {
    size_t offset = 0;

    if (text[0] == 'S' && text[1] == 'I' && text[2] == 'G') {
        text += 3;
    }

    while (text[offset] != '\0' && name[offset] != '\0') {
        char left = text[offset];
        char right = name[offset];
        if (left >= 'a' && left <= 'z') {
            left = (char)(left - 'a' + 'A');
        }
        if (right >= 'a' && right <= 'z') {
            right = (char)(right - 'a' + 'A');
        }
        if (left != right) {
            return 0;
        }
        offset += 1U;
    }

    return text[offset] == '\0' && name[offset] == '\0';
}

static void fill_group_name(char *buffer, size_t buffer_size, unsigned int gid) {
    struct group *group_info;

    group_info = getgrgid((gid_t)gid);
    if (group_info != 0 && group_info->gr_name != 0 && group_info->gr_name[0] != '\0') {
        rt_copy_string(buffer, buffer_size, group_info->gr_name);
    } else {
        rt_unsigned_to_string((unsigned long long)gid, buffer, buffer_size);
    }
}

static void fill_identity_from_passwd(struct passwd *passwd_info, PlatformIdentity *identity_out) {
    identity_out->uid = (unsigned int)passwd_info->pw_uid;
    identity_out->gid = (unsigned int)passwd_info->pw_gid;

    if (passwd_info->pw_name != 0 && passwd_info->pw_name[0] != '\0') {
        rt_copy_string(identity_out->username, sizeof(identity_out->username), passwd_info->pw_name);
    } else {
        rt_unsigned_to_string((unsigned long long)identity_out->uid, identity_out->username, sizeof(identity_out->username));
    }
    fill_group_name(identity_out->groupname, sizeof(identity_out->groupname), identity_out->gid);
}

static int join_path_local(const char *base, const char *name, char *buffer, size_t buffer_size) {
    size_t base_len;
    size_t name_len;
    int need_slash;

    if (base == 0 || name == 0 || buffer == 0 || buffer_size == 0) {
        return -1;
    }

    base_len = rt_strlen(base);
    name_len = rt_strlen(name);
    need_slash = base_len > 0U && base[base_len - 1U] != '/';

    if (base_len + (need_slash ? 1U : 0U) + name_len + 1U > buffer_size) {
        return -1;
    }

    rt_copy_string(buffer, buffer_size, base);
    if (need_slash) {
        buffer[base_len] = '/';
        buffer[base_len + 1U] = '\0';
    }
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), name);
    return 0;
}

long platform_write(int fd, const void *buffer, size_t count) {
    return darwin_syscall3(DARWIN_SYS_WRITE, (long)fd, (long)buffer, (long)count);
}

long platform_read(int fd, void *buffer, size_t count) {
    return darwin_syscall3(DARWIN_SYS_READ, (long)fd, (long)buffer, (long)count);
}

void *platform_allocate_pages(size_t size) {
    long mapped = darwin_syscall6(
        DARWIN_SYS_MMAP,
        0,
        (long)size,
        DARWIN_PROT_READ | DARWIN_PROT_WRITE,
        DARWIN_MAP_PRIVATE | DARWIN_MAP_ANONYMOUS,
        -1,
        0
    );
    return mapped < 0 ? 0 : (void *)mapped;
}

int platform_close(int fd) {
    if (fd >= 0 && fd <= 2) {
        return 0;
    }
    return darwin_syscall1(DARWIN_SYS_CLOSE, (long)fd) < 0 ? -1 : 0;
}

int platform_open_read(const char *path) {
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return 0;
    }

    fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)path, 0, 0);
    return fd < 0 ? -1 : (int)fd;
}

int platform_open_write_mode(const char *path, unsigned int mode, int truncate_existing) {
    long flags = DARWIN_O_WRONLY | DARWIN_O_CREAT;
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return 1;
    }

    if (truncate_existing) {
        flags |= DARWIN_O_TRUNC;
    }

    fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)path, flags, (long)mode);
    return fd < 0 ? -1 : (int)fd;
}

int platform_open_write(const char *path, unsigned int mode) {
    return platform_open_write_mode(path, mode, 1);
}

int platform_open_create_exclusive(const char *path, unsigned int mode) {
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return -1;
    }

    fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)path, DARWIN_O_WRONLY | DARWIN_O_CREAT | DARWIN_O_EXCL, (long)mode);
    return fd < 0 ? -1 : (int)fd;
}

int platform_open_append(const char *path, unsigned int mode) {
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return 1;
    }

    fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)path, DARWIN_O_WRONLY | DARWIN_O_CREAT | DARWIN_O_APPEND, (long)mode);
    return fd < 0 ? -1 : (int)fd;
}

int platform_open_append_existing(const char *path) {
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return 1;
    }

    fd = darwin_syscall3(DARWIN_SYS_OPEN, (long)path, DARWIN_O_WRONLY | DARWIN_O_APPEND, 0);
    return fd < 0 ? -1 : (int)fd;
}

long long platform_seek(int fd, long long offset, int whence) {
    long result = darwin_syscall3(DARWIN_SYS_LSEEK, (long)fd, (long)offset, (long)whence);
    return result < 0 ? -1 : (long long)result;
}

const char *platform_getenv(const char *name) {
    size_t index = 0;

    if (environ == 0) {
        return 0;
    }

    while (environ[index] != 0) {
        if (env_name_matches(environ[index], name)) {
            return environ[index] + rt_strlen(name) + 1U;
        }
        index += 1U;
    }

    return 0;
}

const char *platform_getenv_entry(size_t index) {
    if (environ == 0) {
        return 0;
    }

    return environ[index];
}

int platform_isatty(int fd) {
    (void)fd;
    return 0;
}

int platform_make_directory(const char *path, unsigned int mode) {
    return darwin_syscall2(DARWIN_SYS_MKDIR, (long)path, (long)mode) < 0 ? -1 : 0;
}

int platform_remove_directory(const char *path) {
    return darwin_syscall1(DARWIN_SYS_RMDIR, (long)path) < 0 ? -1 : 0;
}

int platform_remove_file(const char *path) {
    return darwin_syscall1(DARWIN_SYS_UNLINK, (long)path) < 0 ? -1 : 0;
}

int platform_rename_path(const char *old_path, const char *new_path) {
    return rename(old_path, new_path);
}

int platform_create_hard_link(const char *target_path, const char *link_path) {
    return link(target_path, link_path);
}

int platform_create_symbolic_link(const char *target_path, const char *link_path) {
    return symlink(target_path, link_path);
}

int platform_change_mode(const char *path, unsigned int mode) {
    return chmod(path, (mode_t)mode);
}

int platform_change_owner_ex(const char *path, unsigned int uid, unsigned int gid, int follow_symlinks) {
    if (follow_symlinks) {
        return chown(path, (uid_t)uid, (gid_t)gid);
    }
    return lchown(path, (uid_t)uid, (gid_t)gid);
}

int platform_change_owner(const char *path, unsigned int uid, unsigned int gid) {
    return platform_change_owner_ex(path, uid, gid, 1);
}

int platform_path_is_directory(const char *path, int *is_directory_out) {
    PlatformDirEntry entry;

    if (platform_get_path_info_follow(path, &entry) != 0) {
        return -1;
    }

    if (is_directory_out != 0) {
        *is_directory_out = entry.is_dir;
    }
    return 0;
}

int platform_get_current_directory(char *buffer, size_t buffer_size) {
    return getcwd(buffer, buffer_size) == 0 ? -1 : 0;
}

int platform_get_path_info(const char *path, PlatformDirEntry *entry_out) {
    return fill_entry_mode(path, path, entry_out, 0);
}

int platform_get_path_info_follow(const char *path, PlatformDirEntry *entry_out) {
    return fill_entry_mode(path, path, entry_out, 1);
}

int platform_read_symlink(const char *path, char *buffer, size_t buffer_size) {
    long result;

    if (buffer == 0 || buffer_size == 0) {
        return -1;
    }

    result = readlink(path, buffer, buffer_size - 1U);
    if (result < 0) {
        return -1;
    }

    buffer[result] = '\0';
    return 0;
}

int platform_path_access(const char *path, int mode) {
    return darwin_syscall2(DARWIN_SYS_ACCESS, (long)path, (long)access_mode_to_darwin(mode)) < 0 ? -1 : 0;
}

int platform_collect_entries(
    const char *path,
    int include_hidden,
    PlatformDirEntry *entries_out,
    size_t entry_capacity,
    size_t *count_out,
    int *path_is_directory
) {
    PlatformDirEntry current;
    size_t count = 0;
    DIR *directory;
    struct dirent *entry;

    if (path == 0 || entries_out == 0 || count_out == 0 || path_is_directory == 0) {
        return -1;
    }

    *count_out = 0;
    *path_is_directory = 0;

    if (platform_get_path_info(path, &current) != 0) {
        return -1;
    }
    if (!current.is_dir) {
        if (entry_capacity == 0) {
            return -1;
        }
        entries_out[0] = current;
        *count_out = 1;
        return 0;
    }

    directory = opendir(path);
    if (directory == 0) {
        return -1;
    }

    while ((entry = readdir(directory)) != 0) {
        char full_path[1024];

        if (!include_hidden && entry->d_name[0] == '.') {
            continue;
        }
        if (count >= entry_capacity) {
            closedir(directory);
            return -1;
        }
        if (join_path_local(path, entry->d_name, full_path, sizeof(full_path)) != 0) {
            closedir(directory);
            return -1;
        }
        if (fill_entry_mode(entry->d_name, full_path, &entries_out[count], 0) != 0) {
            closedir(directory);
            return -1;
        }
        count += 1U;
    }

    closedir(directory);
    *count_out = count;
    *path_is_directory = 1;
    return 0;
}

void platform_free_entries(PlatformDirEntry *entries, size_t count) {
    (void)entries;
    (void)count;
}

int platform_get_filesystem_info(const char *path, PlatformFilesystemInfo *info_out) {
    struct statfs info;
    unsigned long long block_size;

    if (path == 0 || info_out == 0) {
        return -1;
    }

    memset(info_out, 0, sizeof(*info_out));
    if (statfs(path, &info) != 0) {
        return -1;
    }

    block_size = info.f_bsize > 0 ? (unsigned long long)info.f_bsize : 512ULL;
    info_out->total_bytes = (unsigned long long)info.f_blocks * block_size;
    info_out->free_bytes = (unsigned long long)info.f_bfree * block_size;
    info_out->available_bytes = (unsigned long long)info.f_bavail * block_size;
    info_out->total_inodes = (unsigned long long)info.f_files;
    info_out->free_inodes = (unsigned long long)info.f_ffree;
    info_out->available_inodes = (unsigned long long)info.f_ffree;
    rt_copy_string(info_out->type_name, sizeof(info_out->type_name), info.f_fstypename[0] != '\0' ? info.f_fstypename : "darwin");
    return 0;
}

int platform_get_filesystem_usage(
    const char *path,
    unsigned long long *total_bytes_out,
    unsigned long long *free_bytes_out,
    unsigned long long *available_bytes_out
) {
    PlatformFilesystemInfo info;

    if (total_bytes_out == 0 || free_bytes_out == 0 || available_bytes_out == 0) {
        return -1;
    }
    if (platform_get_filesystem_info(path, &info) != 0) {
        return -1;
    }
    *total_bytes_out = info.total_bytes;
    *free_bytes_out = info.free_bytes;
    *available_bytes_out = info.available_bytes;
    return 0;
}

void platform_format_mode(unsigned int mode, char out[11]) {
    out[0] = S_ISDIR(mode) ? 'd' :
             S_ISLNK(mode) ? 'l' :
             S_ISCHR(mode) ? 'c' :
             S_ISBLK(mode) ? 'b' :
             S_ISFIFO(mode) ? 'p' :
             S_ISSOCK(mode) ? 's' : '-';

    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_IXUSR) ? 'x' : '-';
    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_IXGRP) ? 'x' : '-';
    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}

int platform_ignore_signal(int signal_number) {
    (void)signal_number;
    return -1;
}

int platform_send_signal(int pid, int signal_number) {
    return kill((pid_t)pid, signal_number);
}

int platform_parse_signal_name(const char *text, int *signal_out) {
    unsigned long long numeric;
    size_t index;

    if (text == 0 || text[0] == '\0' || signal_out == 0) {
        return -1;
    }
    if (rt_parse_uint(text, &numeric) == 0) {
        *signal_out = (int)numeric;
        return 0;
    }

    for (index = 0; index < sizeof(DARWIN_SIGNAL_TABLE) / sizeof(DARWIN_SIGNAL_TABLE[0]); ++index) {
        if (signal_name_matches(text, DARWIN_SIGNAL_TABLE[index].name)) {
            *signal_out = DARWIN_SIGNAL_TABLE[index].value;
            return 0;
        }
    }

    return -1;
}

const char *platform_signal_name(int signal_number) {
    size_t index;

    for (index = 0; index < sizeof(DARWIN_SIGNAL_TABLE) / sizeof(DARWIN_SIGNAL_TABLE[0]); ++index) {
        if (DARWIN_SIGNAL_TABLE[index].value == signal_number) {
            return DARWIN_SIGNAL_TABLE[index].name;
        }
    }

    return "UNKNOWN";
}

void platform_write_signal_list(int fd) {
    size_t index;

    for (index = 0; index < sizeof(DARWIN_SIGNAL_TABLE) / sizeof(DARWIN_SIGNAL_TABLE[0]); ++index) {
        if (index > 0) {
            (void)platform_write(fd, " ", 1U);
        }
        (void)platform_write(fd, DARWIN_SIGNAL_TABLE[index].name, rt_strlen(DARWIN_SIGNAL_TABLE[index].name));
    }
    (void)platform_write(fd, "\n", 1U);
}

int platform_sleep_milliseconds(unsigned long long milliseconds) {
    DarwinTimeval timeout;

    timeout.tv_sec = (long)(milliseconds / 1000ULL);
    timeout.tv_usec = (int)((milliseconds % 1000ULL) * 1000ULL);

    return darwin_syscall5(DARWIN_SYS_SELECT, 0, 0, 0, 0, (long)&timeout) < 0 ? -1 : 0;
}

int platform_sleep_seconds(unsigned int seconds) {
    return platform_sleep_milliseconds((unsigned long long)seconds * 1000ULL);
}

long long platform_get_epoch_time(void) {
    DarwinTimeval now;

    if (darwin_syscall2(DARWIN_SYS_GETTIMEOFDAY, (long)&now, 0) < 0) {
        return 0;
    }

    return (long long)now.tv_sec;
}

int platform_format_time(long long epoch_seconds, int use_local_time, const char *format, char *buffer, size_t buffer_size) {
    time_t when;
    struct tm time_value;
    struct tm *time_ptr;
    const char *actual_format;

    if (buffer == 0 || buffer_size == 0) {
        return -1;
    }

    when = (time_t)epoch_seconds;
    actual_format = (format != 0 && format[0] != '\0') ? format : "%Y-%m-%d %H:%M:%S";
    time_ptr = use_local_time ? localtime_r(&when, &time_value) : gmtime_r(&when, &time_value);
    if (time_ptr == 0) {
        buffer[0] = '\0';
        return -1;
    }

    return strftime(buffer, buffer_size, actual_format, time_ptr) == 0 ? -1 : 0;
}

int platform_get_hostname(char *buffer, size_t buffer_size) {
    if (buffer == 0 || buffer_size == 0) {
        return -1;
    }
    buffer[0] = '\0';
    if (gethostname(buffer, buffer_size) != 0) {
        return -1;
    }
    buffer[buffer_size - 1U] = '\0';
    return 0;
}

int platform_set_hostname(const char *name) {
    if (name == 0) {
        return -1;
    }
    return sethostname(name, (int)rt_strlen(name));
}

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
) {
    struct utsname info;

    if (uname(&info) != 0) {
        return -1;
    }

    rt_copy_string(sysname, sysname_size, info.sysname);
    rt_copy_string(nodename, nodename_size, info.nodename);
    rt_copy_string(release, release_size, info.release);
    rt_copy_string(version, version_size, info.version);
    rt_copy_string(machine, machine_size, info.machine);
    return 0;
}

int platform_lookup_group(const char *groupname, unsigned int *gid_out) {
    struct group *group_info;
    unsigned long long value;

    if (groupname == 0 || groupname[0] == '\0' || gid_out == 0) {
        return -1;
    }

    if (rt_parse_uint(groupname, &value) == 0) {
        *gid_out = (unsigned int)value;
        return 0;
    }

    group_info = getgrnam(groupname);
    if (group_info == 0) {
        return -1;
    }

    *gid_out = (unsigned int)group_info->gr_gid;
    return 0;
}

int platform_lookup_identity(const char *username, PlatformIdentity *identity_out) {
    struct passwd *passwd_info;
    unsigned int uid;

    if (identity_out == 0) {
        return -1;
    }

    if (username != 0 && username[0] != '\0') {
        unsigned long long value;
        if (rt_parse_uint(username, &value) == 0) {
            passwd_info = getpwuid((uid_t)value);
        } else {
            passwd_info = getpwnam(username);
        }
        if (passwd_info == 0) {
            return -1;
        }
        fill_identity_from_passwd(passwd_info, identity_out);
        return 0;
    }

    uid = (unsigned int)darwin_syscall0(DARWIN_SYS_GETUID);
    passwd_info = getpwuid((uid_t)uid);
    if (passwd_info != 0) {
        fill_identity_from_passwd(passwd_info, identity_out);
        return 0;
    }

    identity_out->uid = uid;
    identity_out->gid = (unsigned int)darwin_syscall0(DARWIN_SYS_GETGID);
    rt_unsigned_to_string((unsigned long long)identity_out->uid, identity_out->username, sizeof(identity_out->username));
    fill_group_name(identity_out->groupname, sizeof(identity_out->groupname), identity_out->gid);
    return 0;
}

int platform_get_identity(PlatformIdentity *identity_out) {
    return platform_lookup_identity(0, identity_out);
}

int platform_list_groups_for_identity(
    const PlatformIdentity *identity,
    PlatformGroupEntry *entries_out,
    size_t entry_capacity,
    size_t *count_out
) {
    int group_ids[256];
    long group_count;
    size_t index;
    size_t stored = 0;

    if (identity == 0 || count_out == 0 || (entry_capacity > 0 && entries_out == 0)) {
        return -1;
    }

    *count_out = 0;
    if (entry_capacity == 0) {
        return 0;
    }

    group_count = darwin_syscall2(DARWIN_SYS_GETGROUPS, (long)(sizeof(group_ids) / sizeof(group_ids[0])), (long)group_ids);
    if (group_count < 0) {
        group_count = 0;
    }

    entries_out[stored].gid = identity->gid;
    rt_copy_string(entries_out[stored].name, sizeof(entries_out[stored].name), identity->groupname);
    stored += 1U;

    for (index = 0; index < (size_t)group_count && stored < entry_capacity; ++index) {
        size_t existing;
        int duplicate = 0;
        unsigned int gid = (unsigned int)group_ids[index];

        for (existing = 0; existing < stored; ++existing) {
            if (entries_out[existing].gid == gid) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        entries_out[stored].gid = gid;
        fill_group_name(entries_out[stored].name, sizeof(entries_out[stored].name), gid);
        stored += 1U;
    }

    *count_out = stored;
    return 0;
}

int platform_get_memory_info(PlatformMemoryInfo *info_out) {
    unsigned long long total_memory = 0;
    size_t total_size = sizeof(total_memory);

    if (info_out == 0) {
        return -1;
    }

    memset(info_out, 0, sizeof(*info_out));
    if (sysctlbyname("hw.memsize", &total_memory, &total_size, 0, 0) != 0) {
        return -1;
    }

    info_out->total_bytes = total_memory;
    info_out->free_bytes = total_memory / 2ULL;
    info_out->available_bytes = total_memory / 2ULL;
    return 0;
}

int platform_get_uptime_info(PlatformUptimeInfo *info_out) {
    if (info_out == 0) {
        return -1;
    }

    info_out->uptime_seconds = 0;
    rt_copy_string(info_out->load_average, sizeof(info_out->load_average), "0.00 0.00 0.00");
    return 0;
}

int platform_list_sessions(PlatformSessionEntry *entries_out, size_t entry_capacity, size_t *count_out) {
    (void)entries_out;
    (void)entry_capacity;
    (void)count_out;

    return -1;
}

int platform_set_path_times(
    const char *path,
    long long atime,
    long long mtime,
    int create_if_missing,
    int update_access,
    int update_modify
) {
    PlatformDirEntry entry;
    DarwinTimeval times[2];
    int fd;

    if (path == 0) {
        return -1;
    }
    if (!update_access && !update_modify) {
        return 0;
    }

    if (platform_get_path_info(path, &entry) != 0) {
        if (!create_if_missing) {
            return -1;
        }
        fd = platform_open_write_mode(path, 0644U, 0);
        if (fd < 0) {
            return -1;
        }
        (void)platform_close(fd);
        if (platform_get_path_info(path, &entry) != 0) {
            return -1;
        }
    }

    if (!update_access) {
        atime = entry.atime;
    }
    if (!update_modify) {
        mtime = entry.mtime;
    }

    times[0].tv_sec = (long)atime;
    times[0].tv_usec = 0;
    times[1].tv_sec = (long)mtime;
    times[1].tv_usec = 0;
    return darwin_syscall2(DARWIN_SYS_UTIMES, (long)path, (long)times) < 0 ? -1 : 0;
}

int platform_truncate_path(const char *path, unsigned long long size) {
    int fd;
    long result;

    if (path == 0) {
        return -1;
    }

    fd = platform_open_write_mode(path, 0644U, 0);
    if (fd < 0) {
        return -1;
    }

    result = darwin_syscall2(DARWIN_SYS_FTRUNCATE, (long)fd, (long)size);
    (void)platform_close(fd);
    return result < 0 ? -1 : 0;
}

int platform_sync_all(void) {
    return darwin_syscall0(DARWIN_SYS_SYNC) < 0 ? -1 : 0;
}

static int open_sync_fd(const char *path) {
    int fd;

    fd = platform_open_read(path);
    if (fd < 0) {
        fd = platform_open_append_existing(path);
    }

    return fd;
}

int platform_sync_path(const char *path) {
    int fd;
    long result;

    fd = open_sync_fd(path);
    if (fd < 0) {
        return -1;
    }

    result = darwin_syscall1(DARWIN_SYS_FSYNC, (long)fd);
    (void)platform_close(fd);
    return result < 0 ? -1 : 0;
}

int platform_sync_path_data(const char *path) {
    return platform_sync_path(path);
}

int platform_get_process_id(void) {
    long pid = darwin_syscall0(DARWIN_SYS_GETPID);
    return pid < 0 ? -1 : (int)pid;
}
