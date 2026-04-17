#ifndef NEWOS_PLATFORM_LINUX_COMMON_H
#define NEWOS_PLATFORM_LINUX_COMMON_H

#include "platform.h"
#include "runtime.h"

#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100
#define LINUX_AT_REMOVEDIR 0x200

#define LINUX_O_RDONLY 0
#define LINUX_O_WRONLY 1
#define LINUX_O_CREAT 0100
#define LINUX_O_EXCL 0200
#define LINUX_O_APPEND 02000
#define LINUX_O_TRUNC 01000
#define LINUX_O_DIRECTORY 0200000

#define LINUX_SIGCHLD 17
#define LINUX_TIOCGWINSZ 0x5413

#define LINUX_S_IFMT   0170000U
#define LINUX_S_IFIFO  0010000U
#define LINUX_S_IFCHR  0020000U
#define LINUX_S_IFDIR  0040000U
#define LINUX_S_IFBLK  0060000U
#define LINUX_S_IFREG  0100000U
#define LINUX_S_IFLNK  0120000U
#define LINUX_S_IFSOCK 0140000U

#define LINUX_S_IRUSR 0400U
#define LINUX_S_IWUSR 0200U
#define LINUX_S_IXUSR 0100U
#define LINUX_S_IRGRP 0040U
#define LINUX_S_IWGRP 0020U
#define LINUX_S_IXGRP 0010U
#define LINUX_S_IROTH 0004U
#define LINUX_S_IWOTH 0002U
#define LINUX_S_IXOTH 0001U

struct linux_stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned long st_rdev;
    unsigned long __pad1;
    long st_size;
    int st_blksize;
    int __pad2;
    long st_blocks;
    long st_atime;
    unsigned long st_atime_nsec;
    long st_mtime;
    unsigned long st_mtime_nsec;
    long st_ctime;
    unsigned long st_ctime_nsec;
    unsigned int __unused4;
    unsigned int __unused5;
};

struct linux_dirent64 {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

struct linux_timespec {
    long tv_sec;
    long tv_nsec;
};

struct linux_winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define linux_copy_string rt_copy_string
#define linux_string_length rt_strlen
#define linux_unsigned_to_string rt_unsigned_to_string
#define linux_join_path rt_join_path
#define linux_is_digit_string rt_is_digit_string
#define linux_parse_pid_value rt_parse_pid_value
#define linux_trim_newline rt_trim_newline

#define copy_string rt_copy_string
#define string_length rt_strlen
#define unsigned_to_string rt_unsigned_to_string
#define join_path rt_join_path
#define is_digit_string rt_is_digit_string
#define parse_pid_value rt_parse_pid_value
#define trim_newline rt_trim_newline

#endif
