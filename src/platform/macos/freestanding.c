#include <sys/stat.h>

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

extern char **environ;
extern char *getcwd(char *buffer, size_t size);
extern long readlink(const char *path, char *buffer, unsigned long buffer_size);

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

int platform_ignore_signal(int signal_number) {
    (void)signal_number;
    return -1;
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
