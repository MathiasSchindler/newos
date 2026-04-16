#include "platform.h"
#include "common.h"
#include "syscall.h"

static int fill_entry(const char *display_name, const char *full_path, PlatformDirEntry *entry) {
    struct linux_stat st;
    long result;

    result = linux_syscall4(
        LINUX_SYS_NEWFSTATAT,
        LINUX_AT_FDCWD,
        (long)full_path,
        (long)&st,
        LINUX_AT_SYMLINK_NOFOLLOW
    );
    if (result < 0) {
        return -1;
    }

    copy_string(entry->name, sizeof(entry->name), display_name);
    entry->mode = st.st_mode;
    entry->size = (unsigned long long)st.st_size;
    entry->inode = (unsigned long long)st.st_ino;
    entry->nlink = (unsigned long)st.st_nlink;
    entry->mtime = (long long)st.st_mtime;
    entry->is_dir = ((st.st_mode & LINUX_S_IFMT) == LINUX_S_IFDIR) ? 1 : 0;
    entry->is_hidden = (display_name[0] == '.') ? 1 : 0;
    unsigned_to_string((unsigned long long)st.st_uid, entry->owner, sizeof(entry->owner));
    unsigned_to_string((unsigned long long)st.st_gid, entry->group, sizeof(entry->group));
    return 0;
}

long platform_write(int fd, const void *buffer, size_t count) {
    return linux_syscall3(LINUX_SYS_WRITE, fd, (long)buffer, (long)count);
}

long platform_read(int fd, void *buffer, size_t count) {
    return linux_syscall3(LINUX_SYS_READ, fd, (long)buffer, (long)count);
}

int platform_open_read(const char *path) {
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return 0;
    }

    fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)path, LINUX_O_RDONLY, 0);
    return fd < 0 ? -1 : (int)fd;
}

int platform_open_write(const char *path, unsigned int mode) {
    long fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        return 1;
    }

    fd = linux_syscall4(
        LINUX_SYS_OPENAT,
        LINUX_AT_FDCWD,
        (long)path,
        LINUX_O_WRONLY | LINUX_O_CREAT | LINUX_O_TRUNC,
        (long)mode
    );
    return fd < 0 ? -1 : (int)fd;
}

int platform_close(int fd) {
    if (fd == 0 || fd == 1) {
        return 0;
    }

    return linux_syscall1(LINUX_SYS_CLOSE, fd) < 0 ? -1 : 0;
}

int platform_make_directory(const char *path, unsigned int mode) {
    return linux_syscall3(LINUX_SYS_MKDIRAT, LINUX_AT_FDCWD, (long)path, (long)mode) < 0 ? -1 : 0;
}

int platform_remove_file(const char *path) {
    return linux_syscall3(LINUX_SYS_UNLINKAT, LINUX_AT_FDCWD, (long)path, 0) < 0 ? -1 : 0;
}

int platform_remove_directory(const char *path) {
    return linux_syscall3(LINUX_SYS_UNLINKAT, LINUX_AT_FDCWD, (long)path, LINUX_AT_REMOVEDIR) < 0 ? -1 : 0;
}

int platform_rename_path(const char *old_path, const char *new_path) {
    return linux_syscall4(LINUX_SYS_RENAMEAT, LINUX_AT_FDCWD, (long)old_path, LINUX_AT_FDCWD, (long)new_path) < 0 ? -1 : 0;
}

int platform_create_hard_link(const char *target_path, const char *link_path) {
    return linux_syscall5(LINUX_SYS_LINKAT, LINUX_AT_FDCWD, (long)target_path, LINUX_AT_FDCWD, (long)link_path, 0) < 0 ? -1 : 0;
}

int platform_create_symbolic_link(const char *target_path, const char *link_path) {
    return linux_syscall3(LINUX_SYS_SYMLINKAT, (long)target_path, LINUX_AT_FDCWD, (long)link_path) < 0 ? -1 : 0;
}

int platform_change_mode(const char *path, unsigned int mode) {
    return linux_syscall4(LINUX_SYS_FCHMODAT, LINUX_AT_FDCWD, (long)path, (long)mode, 0) < 0 ? -1 : 0;
}

int platform_touch_path(const char *path) {
    long fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)path, LINUX_O_WRONLY | LINUX_O_CREAT, 0644);
    struct linux_timespec times[2];

    if (fd < 0) {
        return -1;
    }

    linux_syscall1(LINUX_SYS_CLOSE, fd);
    times[0].tv_sec = 0;
    times[0].tv_nsec = (long)0x3fffffff;
    times[1].tv_sec = 0;
    times[1].tv_nsec = (long)0x3fffffff;
    return linux_syscall4(LINUX_SYS_UTIMENSAT, LINUX_AT_FDCWD, (long)path, (long)times, 0) < 0 ? -1 : 0;
}

int platform_change_directory(const char *path) {
    return linux_syscall1(LINUX_SYS_CHDIR, (long)path) < 0 ? -1 : 0;
}

struct linux_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

int platform_get_uname(char *sysname, size_t sysname_size, char *nodename, size_t nodename_size, char *release, size_t release_size, char *machine, size_t machine_size) {
    struct linux_utsname info;

    if (linux_syscall1(LINUX_SYS_UNAME, (long)&info) < 0) {
        return -1;
    }

    linux_copy_string(sysname, (unsigned long)sysname_size, info.sysname);
    linux_copy_string(nodename, (unsigned long)nodename_size, info.nodename);
    linux_copy_string(release, (unsigned long)release_size, info.release);
    linux_copy_string(machine, (unsigned long)machine_size, info.machine);
    return 0;
}

int platform_path_is_directory(const char *path, int *is_directory_out) {
    struct linux_stat st;
    long stat_result;

    if (path == 0 || is_directory_out == 0) {
        return -1;
    }

    stat_result = linux_syscall4(
        LINUX_SYS_NEWFSTATAT,
        LINUX_AT_FDCWD,
        (long)path,
        (long)&st,
        LINUX_AT_SYMLINK_NOFOLLOW
    );
    if (stat_result < 0) {
        return -1;
    }

    *is_directory_out = ((st.st_mode & LINUX_S_IFMT) == LINUX_S_IFDIR) ? 1 : 0;
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
    struct linux_stat st;
    size_t count = 0;
    long stat_result;

    if (path == 0 || entries_out == 0 || count_out == 0 || path_is_directory == 0) {
        return -1;
    }

    *count_out = 0;
    *path_is_directory = 0;

    stat_result = linux_syscall4(
        LINUX_SYS_NEWFSTATAT,
        LINUX_AT_FDCWD,
        (long)path,
        (long)&st,
        LINUX_AT_SYMLINK_NOFOLLOW
    );
    if (stat_result < 0) {
        return -1;
    }

    if ((st.st_mode & LINUX_S_IFMT) != LINUX_S_IFDIR) {
        if (entry_capacity == 0) {
            return -1;
        }

        if (fill_entry(path, path, &entries_out[0]) != 0) {
            return -1;
        }

        *count_out = 1;
        return 0;
    }

    {
        long fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)path, LINUX_O_RDONLY | LINUX_O_DIRECTORY, 0);
        char buffer[4096];

        if (fd < 0) {
            return -1;
        }

        for (;;) {
            long bytes = linux_syscall3(LINUX_SYS_GETDENTS64, fd, (long)buffer, sizeof(buffer));
            unsigned long offset = 0;

            if (bytes == 0) {
                break;
            }

            if (bytes < 0) {
                linux_syscall1(LINUX_SYS_CLOSE, fd);
                return -1;
            }

            while (offset < (unsigned long)bytes) {
                struct linux_dirent64 *entry = (struct linux_dirent64 *)(buffer + offset);
                const char *name = entry->d_name;
                char full_path[1024];

                if (include_hidden || name[0] != '.') {
                    if (count >= entry_capacity) {
                        linux_syscall1(LINUX_SYS_CLOSE, fd);
                        return -1;
                    }

                    if (join_path(path, name, full_path, sizeof(full_path)) != 0) {
                        linux_syscall1(LINUX_SYS_CLOSE, fd);
                        return -1;
                    }

                    if (fill_entry(name, full_path, &entries_out[count]) != 0) {
                        linux_syscall1(LINUX_SYS_CLOSE, fd);
                        return -1;
                    }

                    count += 1;
                }

                offset += entry->d_reclen;
            }
        }

        linux_syscall1(LINUX_SYS_CLOSE, fd);
    }

    *count_out = count;
    *path_is_directory = 1;
    return 0;
}

int platform_stream_file_to_stdout(const char *path) {
    long fd;
    int should_close;
    char buffer[4096];

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) {
        fd = 0;
        should_close = 0;
    } else {
        fd = linux_syscall4(LINUX_SYS_OPENAT, LINUX_AT_FDCWD, (long)path, LINUX_O_RDONLY, 0);
        if (fd < 0) {
            return -1;
        }
        should_close = 1;
    }

    for (;;) {
        long bytes = linux_syscall3(LINUX_SYS_READ, fd, (long)buffer, sizeof(buffer));
        long offset = 0;

        if (bytes == 0) {
            break;
        }

        if (bytes < 0) {
            if (should_close) {
                linux_syscall1(LINUX_SYS_CLOSE, fd);
            }
            return -1;
        }

        while (offset < bytes) {
            long written = linux_syscall3(LINUX_SYS_WRITE, 1, (long)(buffer + offset), (unsigned long)(bytes - offset));
            if (written < 0) {
                if (should_close) {
                    linux_syscall1(LINUX_SYS_CLOSE, fd);
                }
                return -1;
            }
            offset += written;
        }
    }

    if (should_close) {
        linux_syscall1(LINUX_SYS_CLOSE, fd);
    }

    return 0;
}

int platform_get_current_directory(char *buffer, size_t buffer_size) {
    long result = linux_syscall2(LINUX_SYS_GETCWD, (long)buffer, (long)buffer_size);
    return (result < 0) ? -1 : 0;
}

int platform_get_path_info(const char *path, PlatformDirEntry *entry_out) {
    if (path == 0 || entry_out == 0) {
        return -1;
    }

    return fill_entry(path, path, entry_out);
}

void platform_free_entries(PlatformDirEntry *entries, size_t count) {
    (void)entries;
    (void)count;
}

void platform_format_mode(unsigned int mode, char out[11]) {
    unsigned int type = mode & LINUX_S_IFMT;

    out[0] = (type == LINUX_S_IFDIR) ? 'd' :
             (type == LINUX_S_IFLNK) ? 'l' :
             (type == LINUX_S_IFCHR) ? 'c' :
             (type == LINUX_S_IFBLK) ? 'b' :
             (type == LINUX_S_IFIFO) ? 'p' :
             (type == LINUX_S_IFSOCK) ? 's' : '-';

    out[1] = (mode & LINUX_S_IRUSR) ? 'r' : '-';
    out[2] = (mode & LINUX_S_IWUSR) ? 'w' : '-';
    out[3] = (mode & LINUX_S_IXUSR) ? 'x' : '-';
    out[4] = (mode & LINUX_S_IRGRP) ? 'r' : '-';
    out[5] = (mode & LINUX_S_IWGRP) ? 'w' : '-';
    out[6] = (mode & LINUX_S_IXGRP) ? 'x' : '-';
    out[7] = (mode & LINUX_S_IROTH) ? 'r' : '-';
    out[8] = (mode & LINUX_S_IWOTH) ? 'w' : '-';
    out[9] = (mode & LINUX_S_IXOTH) ? 'x' : '-';
    out[10] = '\0';
}
