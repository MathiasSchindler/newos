#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#if defined(__linux__)
#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/mount.h>
#endif

static void copy_identity_names(uid_t uid, gid_t gid, PlatformDirEntry *entry) {
    struct passwd *pw = getpwuid(uid);
    struct group *gr = getgrgid(gid);

    if (pw != NULL) {
        posix_copy_string(entry->owner, sizeof(entry->owner), pw->pw_name);
    } else {
        snprintf(entry->owner, sizeof(entry->owner), "%u", (unsigned)uid);
    }

    if (gr != NULL) {
        posix_copy_string(entry->group, sizeof(entry->group), gr->gr_name);
    } else {
        snprintf(entry->group, sizeof(entry->group), "%u", (unsigned)gid);
    }
}

static int fill_entry_mode(const char *display_name, const char *full_path, PlatformDirEntry *entry, int follow_symlinks) {
    struct stat st;

    if ((follow_symlinks ? stat(full_path, &st) : lstat(full_path, &st)) != 0) {
        return -1;
    }

    memset(entry, 0, sizeof(*entry));
    posix_copy_string(entry->name, sizeof(entry->name), display_name);
    entry->device = (unsigned long long)st.st_dev;
    entry->mode = (unsigned int)st.st_mode;
    entry->size = (unsigned long long)st.st_size;
    entry->inode = (unsigned long long)st.st_ino;
    entry->nlink = (unsigned long)st.st_nlink;
    entry->atime = (long long)st.st_atime;
    entry->mtime = (long long)st.st_mtime;
    entry->ctime = (long long)st.st_ctime;
    entry->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    entry->is_hidden = (display_name[0] == '.') ? 1 : 0;

    copy_identity_names(st.st_uid, st.st_gid, entry);
    return 0;
}

static int fill_entry(const char *display_name, const char *full_path, PlatformDirEntry *entry) {
    return fill_entry_mode(display_name, full_path, entry, 0);
}

long platform_write(int fd, const void *buffer, size_t count) {
    return (long)write(fd, buffer, count);
}

long platform_read(int fd, void *buffer, size_t count) {
    return (long)read(fd, buffer, count);
}

int platform_open_read(const char *path) {
    if (path == NULL || strcmp(path, "-") == 0) {
        return STDIN_FILENO;
    }

    return open(path, O_RDONLY);
}

int platform_open_write_mode(const char *path, unsigned int mode, int truncate_existing) {
    int flags = O_WRONLY | O_CREAT;

    if (path == NULL || strcmp(path, "-") == 0) {
        return STDOUT_FILENO;
    }

    if (truncate_existing) {
        flags |= O_TRUNC;
    }

    return open(path, flags, (mode_t)mode);
}

int platform_open_write(const char *path, unsigned int mode) {
    return platform_open_write_mode(path, mode, 1);
}

int platform_open_append(const char *path, unsigned int mode) {
    if (path == NULL || strcmp(path, "-") == 0) {
        return STDOUT_FILENO;
    }

    return open(path, O_WRONLY | O_CREAT | O_APPEND, (mode_t)mode);
}

int platform_create_temp_file(char *path_buffer, size_t buffer_size, const char *prefix, unsigned int mode) {
    char templ[1024];
    const char *base = (prefix != NULL && prefix[0] != '\0') ? prefix : "/tmp/newos-tmp-";
    size_t base_len = strlen(base);
    int fd;

    if (path_buffer == NULL || buffer_size == 0 || base_len + 7 > sizeof(templ) || base_len + 7 > buffer_size) {
        errno = EINVAL;
        return -1;
    }

    memcpy(templ, base, base_len);
    memcpy(templ + base_len, "XXXXXX", 7);

    fd = mkstemp(templ);
    if (fd < 0) {
        return -1;
    }

    if (fchmod(fd, (mode_t)mode) != 0) {
        int saved_errno = errno;
        close(fd);
        unlink(templ);
        errno = saved_errno;
        return -1;
    }

    posix_copy_string(path_buffer, buffer_size, templ);
    return fd;
}

long long platform_seek(int fd, long long offset, int whence) {
    off_t result = lseek(fd, (off_t)offset, whence);
    return result < 0 ? -1 : (long long)result;
}

int platform_close(int fd) {
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO) {
        return 0;
    }

    return close(fd);
}

int platform_make_directory(const char *path, unsigned int mode) {
    return mkdir(path, (mode_t)mode);
}

int platform_remove_file(const char *path) {
    return unlink(path);
}

int platform_remove_directory(const char *path) {
    return rmdir(path);
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

int platform_touch_path(const char *path) {
    long long now = platform_get_epoch_time();

    if (now < 0) {
        errno = EINVAL;
        return -1;
    }

    return platform_set_path_times(path, now, now, 1, 1, 1);
}

int platform_set_path_times(
    const char *path,
    long long atime,
    long long mtime,
    int create_if_missing,
    int update_access,
    int update_modify
) {
    struct timespec times[2];
    int fd = -1;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (!update_access && !update_modify) {
        return 0;
    }

    if (create_if_missing) {
        fd = open(path, O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            return -1;
        }
        close(fd);
    }

    times[0].tv_sec = update_access ? (time_t)atime : (time_t)0;
    times[0].tv_nsec = update_access ? 0L : UTIME_OMIT;
    times[1].tv_sec = update_modify ? (time_t)mtime : (time_t)0;
    times[1].tv_nsec = update_modify ? 0L : UTIME_OMIT;
    return utimensat(AT_FDCWD, path, times, 0);
}

int platform_path_access(const char *path, int mode) {
    int access_mode = F_OK;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    if ((mode & PLATFORM_ACCESS_READ) != 0) {
        access_mode |= R_OK;
    }
    if ((mode & PLATFORM_ACCESS_WRITE) != 0) {
        access_mode |= W_OK;
    }
    if ((mode & PLATFORM_ACCESS_EXECUTE) != 0) {
        access_mode |= X_OK;
    }

    return access(path, access_mode);
}

int platform_change_directory(const char *path) {
    return chdir(path);
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

    posix_copy_string(sysname, sysname_size, info.sysname);
    posix_copy_string(nodename, nodename_size, info.nodename);
    posix_copy_string(release, release_size, info.release);
    posix_copy_string(version, version_size, info.version);
    posix_copy_string(machine, machine_size, info.machine);
    return 0;
}

int platform_path_is_directory(const char *path, int *is_directory_out) {
    struct stat st;

    if (path == NULL || is_directory_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (lstat(path, &st) != 0) {
        return -1;
    }

    *is_directory_out = S_ISDIR(st.st_mode) ? 1 : 0;
    return 0;
}

int platform_collect_entries(
    const char *path,
    int include_hidden,
    PlatformDirEntry *entries_out,
    size_t entry_capacity,
    size_t *count_out,
    int *path_is_directory
) {
    struct stat st;
    size_t count = 0;

    if (path == NULL || entries_out == NULL || count_out == NULL || path_is_directory == NULL) {
        errno = EINVAL;
        return -1;
    }

    *count_out = 0;
    *path_is_directory = 0;

    if (lstat(path, &st) != 0) {
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        if (entry_capacity == 0) {
            errno = ENOSPC;
            return -1;
        }

        if (fill_entry(path, path, &entries_out[0]) != 0) {
            return -1;
        }

        *count_out = 1;
        return 0;
    }

    {
        DIR *dir = opendir(path);
        struct dirent *de;

        if (dir == NULL) {
            return -1;
        }

        while ((de = readdir(dir)) != NULL) {
            char full_path[1024];

            if (!include_hidden && de->d_name[0] == '.') {
                continue;
            }

            if (count >= entry_capacity) {
                closedir(dir);
                errno = ENOSPC;
                return -1;
            }

            if (posix_join_path(path, de->d_name, full_path, sizeof(full_path)) != 0) {
                closedir(dir);
                return -1;
            }

            if (fill_entry(de->d_name, full_path, &entries_out[count]) != 0) {
                closedir(dir);
                return -1;
            }

            count += 1;
        }

        closedir(dir);
    }

    *count_out = count;
    *path_is_directory = 1;
    return 0;
}

void platform_free_entries(PlatformDirEntry *entries, size_t count) {
    (void)entries;
    (void)count;
}

int platform_stream_file_to_stdout(const char *path) {
    int fd;
    int should_close;
    char buffer[4096];
    ssize_t bytes_read;

    if (path == NULL || strcmp(path, "-") == 0) {
        fd = STDIN_FILENO;
        should_close = 0;
    } else {
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            return -1;
        }
        should_close = 1;
    }

    while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
        ssize_t offset = 0;

        while (offset < bytes_read) {
            ssize_t bytes_written = write(STDOUT_FILENO, buffer + offset, (size_t)(bytes_read - offset));
            if (bytes_written < 0) {
                int saved_errno = errno;
                if (should_close) {
                    close(fd);
                }
                errno = saved_errno;
                return -1;
            }
            offset += bytes_written;
        }
    }

    if (should_close) {
        close(fd);
    }

    if (bytes_read < 0) {
        return -1;
    }

    return 0;
}

int platform_get_current_directory(char *buffer, size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        errno = EINVAL;
        return -1;
    }

    if (getcwd(buffer, buffer_size) == NULL) {
        return -1;
    }

    return 0;
}

int platform_get_path_info(const char *path, PlatformDirEntry *entry_out) {
    if (path == NULL || entry_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    return fill_entry(path, path, entry_out);
}

int platform_get_path_info_follow(const char *path, PlatformDirEntry *entry_out) {
    if (path == NULL || entry_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    return fill_entry_mode(path, path, entry_out, 1);
}

int platform_read_symlink(const char *path, char *buffer, size_t buffer_size) {
    ssize_t length;

    if (path == NULL || buffer == NULL || buffer_size == 0) {
        errno = EINVAL;
        return -1;
    }

    length = readlink(path, buffer, buffer_size - 1);
    if (length < 0) {
        return -1;
    }

    buffer[length] = '\0';
    return 0;
}

int platform_get_filesystem_info(const char *path, PlatformFilesystemInfo *info_out) {
    struct statvfs info;

    if (path == NULL || info_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(info_out, 0, sizeof(*info_out));
    if (statvfs(path, &info) != 0) {
        return -1;
    }

    info_out->total_bytes = (unsigned long long)info.f_blocks * (unsigned long long)info.f_frsize;
    info_out->free_bytes = (unsigned long long)info.f_bfree * (unsigned long long)info.f_frsize;
    info_out->available_bytes = (unsigned long long)info.f_bavail * (unsigned long long)info.f_frsize;
    info_out->total_inodes = (unsigned long long)info.f_files;
    info_out->free_inodes = (unsigned long long)info.f_ffree;
    info_out->available_inodes = (unsigned long long)info.f_favail;

#if defined(__APPLE__) || defined(__FreeBSD__)
    {
        struct statfs mount_info;
        if (statfs(path, &mount_info) == 0) {
            posix_copy_string(info_out->type_name, sizeof(info_out->type_name), mount_info.f_fstypename);
        }
    }
#endif

    if (info_out->type_name[0] == '\0') {
        posix_copy_string(info_out->type_name, sizeof(info_out->type_name), "posix");
    }

    return 0;
}

int platform_get_filesystem_usage(const char *path, unsigned long long *total_bytes_out, unsigned long long *free_bytes_out, unsigned long long *available_bytes_out) {
    PlatformFilesystemInfo info;

    if (path == NULL || total_bytes_out == NULL || free_bytes_out == NULL || available_bytes_out == NULL) {
        errno = EINVAL;
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

int platform_truncate_path(const char *path, unsigned long long size) {
    int fd;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        return -1;
    }

    if (ftruncate(fd, (off_t)size) != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    return close(fd);
}

int platform_sync_all(void) {
    sync();
    return 0;
}

static int open_sync_fd(const char *path) {
    int fd;

    if (path == NULL) {
        errno = EINVAL;
        return -1;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fd = open(path, O_WRONLY);
    }

    return fd;
}

int platform_sync_path(const char *path) {
    int fd;
    int result;
    int saved_errno;

    fd = open_sync_fd(path);
    if (fd < 0) {
        return -1;
    }

    result = fsync(fd);
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return result;
}

int platform_sync_path_data(const char *path) {
    int fd;
    int result;
    int saved_errno;

    fd = open_sync_fd(path);
    if (fd < 0) {
        return -1;
    }

#if defined(__APPLE__)
    result = fsync(fd);
#elif defined(_POSIX_SYNCHRONIZED_IO) && (_POSIX_SYNCHRONIZED_IO > 0)
    result = fdatasync(fd);
#else
    result = fsync(fd);
#endif
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return result;
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
