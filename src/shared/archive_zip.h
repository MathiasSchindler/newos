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
    unsigned int v2_signer_count;
    unsigned int v3_signer_count;
    unsigned int v2_signature_count;
    unsigned int v3_signature_count;
    unsigned int certificate_count;
    unsigned long long offset;
    unsigned long long size;
} ArchiveZipSigningBlock;

typedef struct {
    unsigned long long local_header_offset;
    unsigned long long data_offset;
    unsigned long long compressed_size;
    unsigned long long uncompressed_size;
    unsigned int crc32;
    unsigned short flags;
    unsigned short method;
    unsigned short name_length;
    unsigned short extra_length;
} ArchiveZipLocalInfo;

typedef struct {
    unsigned long long checked_entries;
    unsigned long long local_header_errors;
    unsigned long long range_errors;
    unsigned long long name_mismatch_errors;
    unsigned long long method_errors;
    unsigned long long crc_errors;
    unsigned long long duplicate_names;
    unsigned long long suspicious_names;
    unsigned long long unsupported_methods;
} ArchiveZipValidation;

typedef int (*ArchiveZipEntryCallback)(const ArchiveZipEntry *entry, void *user_data);

int archive_zip_read_info(int fd, ArchiveZipInfo *info_out);
int archive_zip_iterate_entries(int fd, const ArchiveZipInfo *info, ArchiveZipEntryCallback callback, void *user_data);
int archive_zip_read_local_info(int fd, const ArchiveZipInfo *info, const ArchiveZipEntry *entry, ArchiveZipLocalInfo *local_out);
int archive_zip_read_entry_data(int fd, const ArchiveZipInfo *info, const ArchiveZipEntry *entry, unsigned long long max_size, unsigned char **data_out, size_t *size_out);
int archive_zip_validate(int fd, const ArchiveZipInfo *info, ArchiveZipValidation *validation_out);
int archive_zip_read_signing_block(int fd, const ArchiveZipInfo *info, ArchiveZipSigningBlock *block_out);
const char *archive_zip_method_name(unsigned int method);

#endif