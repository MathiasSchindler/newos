#ifndef NEWOS_ARCHIVE_ZIP_H
#define NEWOS_ARCHIVE_ZIP_H

#include <stddef.h>

#define ARCHIVE_ZIP_METHOD_STORE 0U
#define ARCHIVE_ZIP_METHOD_DEFLATE 8U

#define ARCHIVE_ZIP_FLAG_ENCRYPTED       0x0001U
#define ARCHIVE_ZIP_FLAG_DATA_DESCRIPTOR 0x0008U
#define ARCHIVE_ZIP_FLAG_UTF8            0x0800U

#define ARCHIVE_ZIP_SIG_V2  0x7109871aU
#define ARCHIVE_ZIP_SIG_V3  0xf05368c0U
#define ARCHIVE_ZIP_SIG_V31 0x1b93ad61U

typedef struct {
    unsigned long long file_size;
    unsigned long long central_directory_offset;
    unsigned long long central_directory_size;
    unsigned long long entry_count;
    unsigned int comment_length;
    int zip64;
    int multi_disk;
} ArchiveZipInfo;

typedef struct {
    char *name;
    unsigned short version_made_by;
    unsigned short version_needed;
    unsigned short flags;
    unsigned short method;
    unsigned short mod_time;
    unsigned short mod_date;
    unsigned int crc32;
    unsigned long long compressed_size;
    unsigned long long uncompressed_size;
    unsigned long long local_header_offset;
    unsigned short extra_length;
    unsigned short comment_length;
    int zip64;
} ArchiveZipEntry;

typedef struct {
    int present;
    int v2;
    int v3;
    int v31;
    int source_stamp;
    unsigned long long offset;
    unsigned long long size;
} ArchiveZipSigningBlock;

typedef int (*ArchiveZipEntryCallback)(const ArchiveZipEntry *entry, void *user_data);

int archive_zip_read_info(int fd, ArchiveZipInfo *info_out);
int archive_zip_iterate_entries(int fd, const ArchiveZipInfo *info, ArchiveZipEntryCallback callback, void *user_data);
int archive_zip_read_signing_block(int fd, const ArchiveZipInfo *info, ArchiveZipSigningBlock *block_out);
const char *archive_zip_method_name(unsigned int method);

#endif