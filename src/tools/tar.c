#include "archive_util.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define TAR_BLOCK_SIZE 512
#define TAR_PATH_CAPACITY 1024
#define TAR_ENTRY_CAPACITY 1024

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
} TarHeader;

static int read_exact(int fd, unsigned char *buffer, size_t count) {
    size_t offset = 0;

    while (offset < count) {
        long bytes = platform_read(fd, buffer + offset, count - offset);
        if (bytes <= 0) {
            return -1;
        }
        offset += (size_t)bytes;
    }

    return 0;
}

static int skip_exact(int fd, unsigned long long count) {
    unsigned char buffer[256];

    while (count > 0ULL) {
        size_t chunk = (count > (unsigned long long)sizeof(buffer)) ? sizeof(buffer) : (size_t)count;
        if (read_exact(fd, buffer, chunk) != 0) {
            return -1;
        }
        count -= (unsigned long long)chunk;
    }

    return 0;
}

static int is_zero_block(const unsigned char *block) {
    size_t i;

    for (i = 0; i < TAR_BLOCK_SIZE; ++i) {
        if (block[i] != 0) {
            return 0;
        }
    }

    return 1;
}

static void tar_copy_name(char *dst, size_t dst_size, const char *src) {
    size_t i = 0;
    while (i + 1 < dst_size && src[i] != '\0') {
        dst[i] = src[i];
        i += 1;
    }
    if (dst_size > 0) {
        dst[i < dst_size ? i : (dst_size - 1)] = '\0';
    }
}

static unsigned int header_checksum(const TarHeader *header) {
    const unsigned char *bytes = (const unsigned char *)header;
    unsigned int sum = 0;
    size_t i;

    for (i = 0; i < TAR_BLOCK_SIZE; ++i) {
        if (i >= 148 && i < 156) {
            sum += (unsigned int)' ';
        } else {
            sum += bytes[i];
        }
    }

    return sum;
}

static int write_padding(int fd, unsigned long long size) {
    unsigned char zeros[TAR_BLOCK_SIZE];
    unsigned long long padding = (TAR_BLOCK_SIZE - (size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;

    rt_memset(zeros, 0, sizeof(zeros));
    if (padding == 0ULL) {
        return 0;
    }

    return rt_write_all(fd, zeros, (size_t)padding);
}

static int fill_header(TarHeader *header, const char *stored_name, const PlatformDirEntry *entry, char typeflag) {
    rt_memset(header, 0, sizeof(*header));

    if (rt_strlen(stored_name) >= sizeof(header->name)) {
        return -1;
    }

    tar_copy_name(header->name, sizeof(header->name), stored_name);
    archive_write_octal(header->mode, sizeof(header->mode), (unsigned long long)(entry->mode & 0777U));
    archive_write_octal(header->uid, sizeof(header->uid), 0);
    archive_write_octal(header->gid, sizeof(header->gid), 0);
    archive_write_octal(header->size, sizeof(header->size), (typeflag == '5') ? 0ULL : entry->size);
    archive_write_octal(header->mtime, sizeof(header->mtime), (unsigned long long)entry->mtime);
    header->typeflag = typeflag;
    tar_copy_name(header->magic, sizeof(header->magic), "ustar");
    tar_copy_name(header->version, sizeof(header->version), "00");
    tar_copy_name(header->uname, sizeof(header->uname), entry->owner);
    tar_copy_name(header->gname, sizeof(header->gname), entry->group);
    archive_write_octal(header->checksum, sizeof(header->checksum), header_checksum(header));
    return 0;
}

static int archive_path(int archive_fd, const char *path, const char *stored_name) {
    PlatformDirEntry entries[TAR_ENTRY_CAPACITY];
    size_t count = 0;
    int is_directory = 0;
    size_t i;

    if (platform_collect_entries(path, 1, entries, TAR_ENTRY_CAPACITY, &count, &is_directory) != 0 || count == 0) {
        return -1;
    }

    if (is_directory) {
        TarHeader header;
        char dir_name[TAR_PATH_CAPACITY];
        size_t len = rt_strlen(stored_name);

        if (len + 2 > sizeof(dir_name)) {
            return -1;
        }

        memcpy(dir_name, stored_name, len);
        if (len == 0 || dir_name[len - 1] != '/') {
            dir_name[len] = '/';
            dir_name[len + 1] = '\0';
        } else {
            dir_name[len] = '\0';
        }

        if (fill_header(&header, dir_name, &entries[0], '5') != 0 || rt_write_all(archive_fd, &header, sizeof(header)) != 0) {
            return -1;
        }

        for (i = 0; i < count; ++i) {
            char child_path[TAR_PATH_CAPACITY];
            char child_name[TAR_PATH_CAPACITY];

            if (rt_strcmp(entries[i].name, ".") == 0 || rt_strcmp(entries[i].name, "..") == 0) {
                continue;
            }

            if (tool_join_path(path, entries[i].name, child_path, sizeof(child_path)) != 0 ||
                tool_join_path(stored_name, entries[i].name, child_name, sizeof(child_name)) != 0) {
                return -1;
            }

            if (archive_path(archive_fd, child_path, child_name) != 0) {
                return -1;
            }
        }

        return 0;
    }

    {
        TarHeader header;
        int file_fd;
        unsigned char buffer[4096];

        if (fill_header(&header, stored_name, &entries[0], '0') != 0 || rt_write_all(archive_fd, &header, sizeof(header)) != 0) {
            return -1;
        }

        file_fd = platform_open_read(path);
        if (file_fd < 0) {
            return -1;
        }

        for (;;) {
            long bytes = platform_read(file_fd, buffer, sizeof(buffer));
            if (bytes < 0) {
                platform_close(file_fd);
                return -1;
            }
            if (bytes == 0) {
                break;
            }
            if (rt_write_all(archive_fd, buffer, (size_t)bytes) != 0) {
                platform_close(file_fd);
                return -1;
            }
        }

        platform_close(file_fd);
        return write_padding(archive_fd, entries[0].size);
    }
}

static int ensure_parent_dirs(char *path) {
    size_t i;

    for (i = 1; path[i] != '\0'; ++i) {
        if (path[i] == '/') {
            path[i] = '\0';
            (void)platform_make_directory(path, 0755U);
            path[i] = '/';
        }
    }

    return 0;
}

static int extract_archive(int archive_fd, int list_only) {
    unsigned char block[TAR_BLOCK_SIZE];

    for (;;) {
        TarHeader *header;
        unsigned long long size;
        unsigned long long padding;
        char path[TAR_PATH_CAPACITY];

        if (read_exact(archive_fd, block, sizeof(block)) != 0) {
            return -1;
        }

        if (is_zero_block(block)) {
            return 0;
        }

        header = (TarHeader *)block;
        tar_copy_name(path, sizeof(path), header->name);
        size = archive_parse_octal(header->size, sizeof(header->size));
        padding = (TAR_BLOCK_SIZE - (size % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;

        if (list_only) {
            if (rt_write_line(1, path) != 0) {
                return -1;
            }
            if (skip_exact(archive_fd, size + padding) != 0) {
                return -1;
            }
            continue;
        }

        ensure_parent_dirs(path);

        if (header->typeflag == '5') {
            (void)platform_make_directory(path, 0755U);
            continue;
        }

        {
            int out_fd = platform_open_write(path, 0644U);
            unsigned char data[4096];
            unsigned long long remaining = size;

            if (out_fd < 0) {
                return -1;
            }

            while (remaining > 0ULL) {
                size_t chunk = (remaining > (unsigned long long)sizeof(data)) ? sizeof(data) : (size_t)remaining;
                if (read_exact(archive_fd, data, chunk) != 0 || rt_write_all(out_fd, data, chunk) != 0) {
                    platform_close(out_fd);
                    return -1;
                }
                remaining -= (unsigned long long)chunk;
            }

            platform_close(out_fd);
            (void)platform_change_mode(path, (unsigned int)archive_parse_octal(header->mode, sizeof(header->mode)));
        }

        if (padding > 0ULL && skip_exact(archive_fd, padding) != 0) {
            return -1;
        }
    }
}

int main(int argc, char **argv) {
    int create_mode = 0;
    int extract_mode = 0;
    int list_mode = 0;
    const char *archive_path_name = 0;
    int i;

    if (argc < 3) {
        rt_write_line(2, "Usage: tar -cf archive.tar paths... | tar -xf archive.tar | tar -tf archive.tar");
        return 1;
    }

    for (i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') {
            const char *opt = argv[i] + 1;
            while (*opt != '\0') {
                if (*opt == 'c') create_mode = 1;
                else if (*opt == 'x') extract_mode = 1;
                else if (*opt == 't') list_mode = 1;
                else if (*opt == 'f' && i + 1 < argc) {
                    archive_path_name = argv[++i];
                    break;
                }
                opt += 1;
            }
        }
    }

    if (archive_path_name == 0) {
        rt_write_line(2, "tar: archive path required");
        return 1;
    }

    if (create_mode) {
        int archive_fd = platform_open_write(archive_path_name, 0644U);
        unsigned char zeros[TAR_BLOCK_SIZE * 2];

        if (archive_fd < 0) {
            return 1;
        }

        rt_memset(zeros, 0, sizeof(zeros));
        for (i = 3; i < argc; ++i) {
            if (rt_strcmp(argv[i], archive_path_name) == 0) {
                continue;
            }
            if (archive_path(archive_fd, argv[i], argv[i]) != 0) {
                platform_close(archive_fd);
                return 1;
            }
        }

        if (rt_write_all(archive_fd, zeros, sizeof(zeros)) != 0) {
            platform_close(archive_fd);
            return 1;
        }

        platform_close(archive_fd);
        return 0;
    }

    if (extract_mode || list_mode) {
        int archive_fd = platform_open_read(archive_path_name);
        int result;

        if (archive_fd < 0) {
            return 1;
        }

        result = extract_archive(archive_fd, list_mode);
        platform_close(archive_fd);
        return (result == 0) ? 0 : 1;
    }

    rt_write_line(2, "tar: choose one of -c, -x, or -t");
    return 1;
}
