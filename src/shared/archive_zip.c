#include "archive_zip.h"
#include "archive_util.h"
#include "platform.h"
#include "runtime.h"

#define ZIP_EOCD_SIG 0x06054b50U
#define ZIP64_EOCD_SIG 0x06064b50U
#define ZIP64_LOCATOR_SIG 0x07064b50U
#define ZIP_CENTRAL_SIG 0x02014b50U

#define ZIP_EOCD_MIN_SIZE 22ULL
#define ZIP_EOCD_MAX_SEARCH (65535ULL + ZIP_EOCD_MIN_SIZE)
#define ZIP_CENTRAL_HEADER_SIZE 46U

static unsigned long long zip_read_u64_or_u32(const unsigned char *bytes, size_t *offset_io) {
    unsigned long long value = archive_read_u64_le(bytes + *offset_io);
    *offset_io += 8U;
    return value;
}

static int zip_read_at(int fd, unsigned long long offset, unsigned char *buffer, size_t count) {
    if (offset > 0x7fffffffffffffffULL) {
        return -1;
    }
    if (platform_seek(fd, (long long)offset, PLATFORM_SEEK_SET) < 0) {
        return -1;
    }
    return archive_read_exact(fd, buffer, count);
}

static int zip_find_eocd(int fd, unsigned long long file_size, unsigned long long *offset_out, unsigned char **tail_out, size_t *tail_size_out) {
    unsigned long long search_size = file_size < ZIP_EOCD_MAX_SEARCH ? file_size : ZIP_EOCD_MAX_SEARCH;
    unsigned long long search_start = file_size - search_size;
    unsigned char *tail;
    size_t tail_size;
    size_t index;

    if (file_size < ZIP_EOCD_MIN_SIZE) {
        return -1;
    }
    tail_size = (size_t)search_size;
    tail = (unsigned char *)rt_malloc(tail_size);
    if (tail == 0) {
        return -1;
    }
    if (zip_read_at(fd, search_start, tail, tail_size) != 0) {
        rt_free(tail);
        return -1;
    }

    index = tail_size - (size_t)ZIP_EOCD_MIN_SIZE;
    for (;;) {
        if (archive_read_u32_le(tail + index) == ZIP_EOCD_SIG) {
            unsigned short comment_length = archive_read_u16_le(tail + index + 20U);
            if (index + (size_t)ZIP_EOCD_MIN_SIZE + (size_t)comment_length == tail_size) {
                *offset_out = search_start + (unsigned long long)index;
                *tail_out = tail;
                *tail_size_out = tail_size;
                return 0;
            }
        }
        if (index == 0U) {
            break;
        }
        index -= 1U;
    }

    rt_free(tail);
    return -1;
}

static int zip_read_zip64_info(int fd, unsigned long long eocd_offset, ArchiveZipInfo *info) {
    unsigned char locator[20];
    unsigned char header[56];
    unsigned long long zip64_offset;

    if (eocd_offset < 20ULL) {
        return -1;
    }
    if (zip_read_at(fd, eocd_offset - 20ULL, locator, sizeof(locator)) != 0 || archive_read_u32_le(locator) != ZIP64_LOCATOR_SIG) {
        return -1;
    }
    zip64_offset = archive_read_u64_le(locator + 8U);
    if (zip_read_at(fd, zip64_offset, header, sizeof(header)) != 0 || archive_read_u32_le(header) != ZIP64_EOCD_SIG) {
        return -1;
    }

    info->entry_count = archive_read_u64_le(header + 32U);
    info->central_directory_size = archive_read_u64_le(header + 40U);
    info->central_directory_offset = archive_read_u64_le(header + 48U);
    info->zip64 = 1;
    return 0;
}

int archive_zip_read_info(int fd, ArchiveZipInfo *info_out) {
    unsigned long long eocd_offset;
    unsigned char *tail = 0;
    size_t tail_size = 0U;
    unsigned char eocd[22];
    long long end_offset;
    unsigned long long file_size;
    unsigned int disk_number;
    unsigned int central_disk;
    unsigned int entries_disk;
    unsigned int entries_total;
    unsigned int central_size32;
    unsigned int central_offset32;

    rt_memset(info_out, 0, sizeof(*info_out));
    end_offset = platform_seek(fd, 0, PLATFORM_SEEK_END);
    if (end_offset < 0) {
        return -1;
    }
    file_size = (unsigned long long)end_offset;
    info_out->file_size = file_size;

    if (zip_find_eocd(fd, file_size, &eocd_offset, &tail, &tail_size) != 0) {
        return -1;
    }
    (void)tail_size;
    memcpy(eocd, tail + (size_t)(eocd_offset - (file_size - (file_size < ZIP_EOCD_MAX_SEARCH ? file_size : ZIP_EOCD_MAX_SEARCH))), sizeof(eocd));
    rt_free(tail);

    disk_number = archive_read_u16_le(eocd + 4U);
    central_disk = archive_read_u16_le(eocd + 6U);
    entries_disk = archive_read_u16_le(eocd + 8U);
    entries_total = archive_read_u16_le(eocd + 10U);
    central_size32 = archive_read_u32_le(eocd + 12U);
    central_offset32 = archive_read_u32_le(eocd + 16U);

    info_out->comment_length = archive_read_u16_le(eocd + 20U);
    info_out->multi_disk = disk_number != 0U || central_disk != 0U || entries_disk != entries_total;
    info_out->entry_count = entries_total;
    info_out->central_directory_size = central_size32;
    info_out->central_directory_offset = central_offset32;

    if (entries_total == 0xffffU || central_size32 == 0xffffffffU || central_offset32 == 0xffffffffU) {
        if (zip_read_zip64_info(fd, eocd_offset, info_out) != 0) {
            return -1;
        }
    }

    if (info_out->central_directory_offset > file_size || info_out->central_directory_size > file_size ||
        info_out->central_directory_offset + info_out->central_directory_size > file_size) {
        return -1;
    }
    return 0;
}

static void zip_apply_zip64_extra(ArchiveZipEntry *entry, const unsigned char *extra, size_t extra_length) {
    size_t offset = 0U;

    while (offset + 4U <= extra_length) {
        unsigned int tag = archive_read_u16_le(extra + offset);
        unsigned int length = archive_read_u16_le(extra + offset + 2U);
        size_t data_offset = offset + 4U;
        size_t value_offset = 0U;

        if (data_offset + length > extra_length) {
            return;
        }
        if (tag == 0x0001U) {
            if (entry->uncompressed_size == 0xffffffffULL && value_offset + 8U <= length) {
                entry->uncompressed_size = zip_read_u64_or_u32(extra + data_offset, &value_offset);
                entry->zip64 = 1;
            }
            if (entry->compressed_size == 0xffffffffULL && value_offset + 8U <= length) {
                entry->compressed_size = zip_read_u64_or_u32(extra + data_offset, &value_offset);
                entry->zip64 = 1;
            }
            if (entry->local_header_offset == 0xffffffffULL && value_offset + 8U <= length) {
                entry->local_header_offset = zip_read_u64_or_u32(extra + data_offset, &value_offset);
                entry->zip64 = 1;
            }
        }
        offset = data_offset + length;
    }
}

int archive_zip_iterate_entries(int fd, const ArchiveZipInfo *info, ArchiveZipEntryCallback callback, void *user_data) {
    unsigned long long offset = info->central_directory_offset;
    unsigned long long index;

    for (index = 0ULL; index < info->entry_count; ++index) {
        unsigned char header[ZIP_CENTRAL_HEADER_SIZE];
        unsigned char *name;
        unsigned char *extra = 0;
        ArchiveZipEntry entry;
        unsigned short name_length;
        unsigned short extra_length;
        unsigned short comment_length;
        int callback_result;

        if (zip_read_at(fd, offset, header, sizeof(header)) != 0 || archive_read_u32_le(header) != ZIP_CENTRAL_SIG) {
            return -1;
        }

        name_length = archive_read_u16_le(header + 28U);
        extra_length = archive_read_u16_le(header + 30U);
        comment_length = archive_read_u16_le(header + 32U);
        name = (unsigned char *)rt_malloc((size_t)name_length + 1U);
        if (name == 0) {
            return -1;
        }
        if (zip_read_at(fd, offset + ZIP_CENTRAL_HEADER_SIZE, name, name_length) != 0) {
            rt_free(name);
            return -1;
        }
        name[name_length] = '\0';

        if (extra_length > 0U) {
            extra = (unsigned char *)rt_malloc(extra_length);
            if (extra == 0) {
                rt_free(name);
                return -1;
            }
            if (zip_read_at(fd, offset + ZIP_CENTRAL_HEADER_SIZE + (unsigned long long)name_length, extra, extra_length) != 0) {
                rt_free(extra);
                rt_free(name);
                return -1;
            }
        }

        rt_memset(&entry, 0, sizeof(entry));
        entry.name = (char *)name;
        entry.version_made_by = archive_read_u16_le(header + 4U);
        entry.version_needed = archive_read_u16_le(header + 6U);
        entry.flags = archive_read_u16_le(header + 8U);
        entry.method = archive_read_u16_le(header + 10U);
        entry.mod_time = archive_read_u16_le(header + 12U);
        entry.mod_date = archive_read_u16_le(header + 14U);
        entry.crc32 = archive_read_u32_le(header + 16U);
        entry.compressed_size = archive_read_u32_le(header + 20U);
        entry.uncompressed_size = archive_read_u32_le(header + 24U);
        entry.extra_length = extra_length;
        entry.comment_length = comment_length;
        entry.local_header_offset = archive_read_u32_le(header + 42U);
        zip_apply_zip64_extra(&entry, extra, extra_length);

        callback_result = callback(&entry, user_data);
        rt_free(extra);
        rt_free(name);
        if (callback_result != 0) {
            return callback_result;
        }

        offset += ZIP_CENTRAL_HEADER_SIZE + (unsigned long long)name_length + (unsigned long long)extra_length + (unsigned long long)comment_length;
    }
    return 0;
}

int archive_zip_read_signing_block(int fd, const ArchiveZipInfo *info, ArchiveZipSigningBlock *block_out) {
    unsigned char footer[24];
    unsigned char start_size_bytes[8];
    unsigned long long footer_offset;
    unsigned long long block_size;
    unsigned long long block_offset;
    unsigned long long cursor;
    unsigned long long pairs_end;

    rt_memset(block_out, 0, sizeof(*block_out));
    if (info->central_directory_offset < 24ULL) {
        return 0;
    }
    footer_offset = info->central_directory_offset - 24ULL;
    if (zip_read_at(fd, footer_offset, footer, sizeof(footer)) != 0) {
        return -1;
    }
    if (memcmp(footer + 8U, "APK Sig Block 42", 16U) != 0) {
        return 0;
    }

    block_size = archive_read_u64_le(footer);
    if (block_size + 8ULL > info->central_directory_offset) {
        return -1;
    }
    block_offset = info->central_directory_offset - block_size - 8ULL;
    if (zip_read_at(fd, block_offset, start_size_bytes, sizeof(start_size_bytes)) != 0 || archive_read_u64_le(start_size_bytes) != block_size) {
        return -1;
    }

    block_out->present = 1;
    block_out->offset = block_offset;
    block_out->size = block_size + 8ULL;
    cursor = block_offset + 8ULL;
    pairs_end = info->central_directory_offset - 24ULL;
    while (cursor + 12ULL <= pairs_end) {
        unsigned char pair_header[12];
        unsigned long long pair_size;
        unsigned int pair_id;

        if (zip_read_at(fd, cursor, pair_header, sizeof(pair_header)) != 0) {
            return -1;
        }
        pair_size = archive_read_u64_le(pair_header);
        pair_id = archive_read_u32_le(pair_header + 8U);
        if (pair_size < 4ULL || cursor + 8ULL + pair_size > pairs_end) {
            return -1;
        }
        if (pair_id == ARCHIVE_ZIP_SIG_V2) block_out->v2 = 1;
        if (pair_id == ARCHIVE_ZIP_SIG_V3) block_out->v3 = 1;
        if (pair_id == ARCHIVE_ZIP_SIG_V31) block_out->v31 = 1;
        if (pair_id == 0x42726577U) block_out->source_stamp = 1;
        cursor += 8ULL + pair_size;
    }
    return 0;
}

const char *archive_zip_method_name(unsigned int method) {
    switch (method) {
        case 0U: return "stored";
        case 1U: return "shrunk";
        case 6U: return "imploded";
        case 8U: return "deflated";
        case 9U: return "deflate64";
        case 12U: return "bzip2";
        case 14U: return "lzma";
        case 93U: return "zstd";
        case 95U: return "xz";
        case 99U: return "aes";
        default: return "unknown";
    }
}