#include "compression/crc32.h"
#include "compression/zlib.h"
#include "crypto/sha1.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define GIT_PATH_CAPACITY 2048U
#define GIT_REF_CAPACITY 512U
#define GIT_OBJECT_HEX_SIZE 40U
#define GIT_INDEX_SIGNATURE 0x44495243U
#define GIT_MODE_TYPE_MASK 0170000U
#define GIT_MODE_REGULAR_FILE 0100644U
#define GIT_MODE_REGULAR_EXECUTABLE 0100755U
#define GIT_MODE_REGULAR_TYPE 0100000U
#define GIT_MODE_TREE 0040000U
#define GIT_MODE_SYMLINK 0120000U
#define GIT_MODE_GITLINK 0160000U
#define GIT_MODE_EXEC_BITS 0111U
#define GIT_SCHEME_HTTP 1
#define GIT_SCHEME_HTTPS 2
#define GIT_PACKET_FLUSH 0
#define GIT_OBJECT_COMMIT 1
#define GIT_OBJECT_TREE 2
#define GIT_OBJECT_BLOB 3
#define GIT_OBJECT_TAG 4
#define GIT_OBJECT_OFS_DELTA 6
#define GIT_OBJECT_REF_DELTA 7
#define GIT_MAX_OBJECT_SIZE (128U * 1024U * 1024U)
#define GIT_PACK_PROGRESS_STEP (8U * 1024U * 1024U)
#define GIT_INDEX_FLAG_EXTENDED 0x4000U
#define GIT_INDEX_EXTENDED_INTENT_TO_ADD 0x2000U

typedef struct {
    char work_tree[GIT_PATH_CAPACITY];
    char git_dir[GIT_PATH_CAPACITY];
    char head_ref[GIT_REF_CAPACITY];
    char head_oid[GIT_OBJECT_HEX_SIZE + 1U];
    int head_is_branch;
} GitRepo;

typedef struct {
    char *path;
    unsigned int mode;
    int intent_to_add;
    unsigned long long size;
    unsigned long long mtime_seconds;
    unsigned int mtime_nanos;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
} GitIndexEntry;

typedef struct {
    GitIndexEntry *entries;
    size_t count;
    size_t capacity;
} GitIndex;

typedef struct {
    char *pattern;
    char *base;
    int directory_only;
    int negated;
    int has_slash;
    int has_wildcard;
} GitIgnorePattern;

typedef struct {
    GitIgnorePattern *patterns;
    size_t count;
    size_t capacity;
} GitIgnoreList;

static int git_path_has_slash(const char *path);
static int git_path_has_wildcard(const char *path);

static void git_progress_line(const char *text) {
    (void)rt_write_line(2, text);
}

static void git_progress_pair_line(const char *prefix, const char *value) {
    (void)rt_write_cstr(2, prefix);
    (void)rt_write_line(2, value);
}

static void git_progress_count_line(const char *prefix, size_t count) {
    char digits[32];

    rt_unsigned_to_string((unsigned long long)count, digits, sizeof(digits));
    (void)rt_write_cstr(2, prefix);
    (void)rt_write_line(2, digits);
}

static void git_progress_pack_bytes(const char *prefix, size_t bytes) {
    char whole[32];
    char fraction[2];
    unsigned long long remainder;
    unsigned long long tenths;

    rt_unsigned_to_string((unsigned long long)(bytes / (1024U * 1024U)), whole, sizeof(whole));
    remainder = (unsigned long long)(bytes % (1024U * 1024U));
    tenths = (remainder * 10ULL) / (1024ULL * 1024ULL);
    fraction[0] = (char)('0' + tenths);
    fraction[1] = '\0';
    (void)rt_write_cstr(2, prefix);
    (void)rt_write_cstr(2, whole);
    (void)rt_write_char(2, '.');
    (void)rt_write_cstr(2, fraction);
    (void)rt_write_line(2, " MiB");
}

static void git_progress_clone_into(const char *destination) {
    (void)rt_write_cstr(2, "Cloning into '");
    (void)rt_write_cstr(2, destination);
    (void)rt_write_line(2, "'...");
}

typedef struct {
    const GitRepo *repo;
    const GitIndex *index;
    const GitIgnoreList *ignores;
    int porcelain;
    int color_mode;
    int saw_change;
} GitStatusWalk;

typedef struct {
    unsigned char *data;
    size_t size;
    size_t capacity;
} GitBuffer;

typedef int (*GitHttpBodyCallback)(const unsigned char *data, size_t size, void *user_data);

typedef struct {
    GitBuffer pending;
    GitBuffer pack_data;
    size_t next_progress_bytes;
    int printed_progress;
    int remote_progress_open;
} GitUploadPackStream;

typedef struct {
    const unsigned char *data;
    size_t length;
} GitDiffLine;

typedef struct {
    const char *path;
    size_t insertions;
    size_t deletions;
} GitDiffStat;

typedef struct {
    GitDiffStat *entries;
    size_t count;
    size_t capacity;
} GitDiffStatList;

typedef struct {
    int scheme;
    unsigned int port;
    char host[256];
    char path[1024];
} GitUrl;

typedef struct {
    int use_tls;
    int socket_fd;
    PlatformTlsClient tls;
} GitHttpConnection;

typedef struct {
    char *name;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
} GitRemoteRef;

typedef struct {
    GitRemoteRef *refs;
    size_t count;
    size_t capacity;
    char head_ref[GIT_REF_CAPACITY];
    unsigned char head_oid[CRYPTO_SHA1_DIGEST_SIZE];
    int has_head;
} GitRemoteRefs;

typedef struct {
    unsigned long long offset;
    unsigned int crc32;
    int type;
    int resolved;
    unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned char base_oid[CRYPTO_SHA1_DIGEST_SIZE];
    unsigned long long base_offset;
    unsigned char *data;
    size_t size;
} GitPackObject;

typedef struct {
    const GitPackObject *object;
} GitPackIndexEntry;

typedef struct {
    GitPackObject *objects;
    size_t count;
    size_t capacity;
} GitPack;

typedef struct {
    GitIndex entries;
    const GitRepo *repo;
} GitCheckoutIndex;

static void git_index_destroy(GitIndex *index) {
    size_t i;

    if (index == 0) {
        return;
    }
    for (i = 0U; i < index->count; ++i) {
        rt_free(index->entries[i].path);
    }
    rt_free(index->entries);
    rt_memset(index, 0, sizeof(*index));
}

static void git_ignore_destroy(GitIgnoreList *ignores) {
    size_t i;

    if (ignores == 0) {
        return;
    }
    for (i = 0U; i < ignores->count; ++i) {
        rt_free(ignores->patterns[i].pattern);
        rt_free(ignores->patterns[i].base);
    }
    rt_free(ignores->patterns);
    rt_memset(ignores, 0, sizeof(*ignores));
}

static void git_buffer_destroy(GitBuffer *buffer) {
    if (buffer == 0) {
        return;
    }
    rt_free(buffer->data);
    rt_memset(buffer, 0, sizeof(*buffer));
}

static void git_remote_refs_destroy(GitRemoteRefs *refs) {
    size_t i;

    if (refs == 0) {
        return;
    }
    for (i = 0U; i < refs->count; ++i) {
        rt_free(refs->refs[i].name);
    }
    rt_free(refs->refs);
    rt_memset(refs, 0, sizeof(*refs));
}

static void git_pack_destroy(GitPack *pack) {
    size_t i;

    if (pack == 0) {
        return;
    }
    for (i = 0U; i < pack->count; ++i) {
        rt_free(pack->objects[i].data);
    }
    rt_free(pack->objects);
    rt_memset(pack, 0, sizeof(*pack));
}

static char *git_strdup_n(const char *text, size_t length) {
    char *copy = (char *)rt_malloc(length + 1U);

    if (copy == 0) {
        return 0;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

static int git_copy(char *buffer, size_t buffer_size, const char *text) {
    if (buffer_size == 0U || rt_strlen(text) >= buffer_size) {
        return -1;
    }
    rt_copy_string(buffer, buffer_size, text);
    return 0;
}

static int git_buffer_reserve(GitBuffer *buffer, size_t needed) {
    unsigned char *new_data;
    size_t new_capacity;

    if (needed <= buffer->capacity) {
        return 0;
    }
    new_capacity = buffer->capacity == 0U ? 4096U : buffer->capacity;
    while (new_capacity < needed) {
        if (new_capacity > ((size_t)-1) / 2U) {
            return -1;
        }
        new_capacity *= 2U;
    }
    new_data = (unsigned char *)rt_realloc(buffer->data, new_capacity);
    if (new_data == 0) {
        return -1;
    }
    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return 0;
}

static int git_buffer_append(GitBuffer *buffer, const void *data, size_t size) {
    if (size == 0U) {
        return 0;
    }
    if (data == 0 || buffer->size > ((size_t)-1) - size || git_buffer_reserve(buffer, buffer->size + size) != 0) {
        return -1;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
    return 0;
}

static int git_buffer_append_cstr(GitBuffer *buffer, const char *text) {
    return git_buffer_append(buffer, text, rt_strlen(text));
}

static int git_buffer_append_char(GitBuffer *buffer, char ch) {
    return git_buffer_append(buffer, &ch, 1U);
}

static void git_buffer_discard_prefix(GitBuffer *buffer, size_t count) {
    if (buffer == 0 || count == 0U) {
        return;
    }
    if (count >= buffer->size) {
        buffer->size = 0U;
        return;
    }
    memmove(buffer->data, buffer->data + count, buffer->size - count);
    buffer->size -= count;
}

static int git_hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return (int)(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (int)(ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (int)(ch - 'A');
    }
    return -1;
}

static int git_parse_oid_hex_n(const char *text, size_t length, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    size_t i;

    if (text == 0 || length < GIT_OBJECT_HEX_SIZE) {
        return -1;
    }
    for (i = 0U; i < CRYPTO_SHA1_DIGEST_SIZE; ++i) {
        int hi = git_hex_value(text[i * 2U]);
        int lo = git_hex_value(text[i * 2U + 1U]);

        if (hi < 0 || lo < 0) {
            return -1;
        }
        oid[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

static int git_parse_oid_hex(const char *text, unsigned char oid[CRYPTO_SHA1_DIGEST_SIZE]) {
    return git_parse_oid_hex_n(text, rt_strlen(text), oid);
}

static int git_oid_equal(const unsigned char left[CRYPTO_SHA1_DIGEST_SIZE], const unsigned char right[CRYPTO_SHA1_DIGEST_SIZE]) {
    return memcmp(left, right, CRYPTO_SHA1_DIGEST_SIZE) == 0;
}

static void git_write_u32_be(unsigned char *out, unsigned int value) {
    out[0] = (unsigned char)(value >> 24);
    out[1] = (unsigned char)(value >> 16);
    out[2] = (unsigned char)(value >> 8);
    out[3] = (unsigned char)value;
}

static int git_write_all_file(const char *path, const void *data, size_t size, unsigned int mode) {
    int fd = platform_open_write(path, mode);
    int result;

    if (fd < 0) {
        return -1;
    }
    result = rt_write_all(fd, data, size);
    if (platform_close(fd) != 0) {
        result = -1;
    }
    return result;
}

static int git_join(char *buffer, size_t buffer_size, const char *left, const char *right) {
    return tool_join_path(left, right, buffer, buffer_size);
}

static int git_read_file(const char *path, unsigned char **data_out, size_t *size_out) {
    return tool_read_all_input(path, data_out, size_out);
}

static void git_trim_line(char *text) {
    size_t len;

    if (text == 0) {
        return;
    }
    len = rt_strlen(text);
    while (len > 0U && (text[len - 1U] == '\n' || text[len - 1U] == '\r' || text[len - 1U] == ' ' || text[len - 1U] == '\t')) {
        text[len - 1U] = '\0';
        len -= 1U;
    }
}

static int git_read_text_file(const char *path, char *buffer, size_t buffer_size) {
    unsigned char *data = 0;
    size_t size = 0U;
    size_t copy_size;

    if (git_read_file(path, &data, &size) != 0) {
        return -1;
    }
    if (buffer_size == 0U) {
        rt_free(data);
        return -1;
    }
    copy_size = size < buffer_size - 1U ? size : buffer_size - 1U;
    memcpy(buffer, data, copy_size);
    buffer[copy_size] = '\0';
    rt_free(data);
    git_trim_line(buffer);
    return 0;
}

#include "git/http.c"

#include "git/repo.c"

#include "git/objects.c"

#include "git/remote.c"

#include "git/status_diff.c"

#include "git/commands.c"

static void git_usage(void) {
    rt_write_line(2, "Usage: git [--no-pager] <status|diff|branch|rev-parse|ls-files|add|commit|hash-object|clone|fetch|checkout> [args ...]");
    rt_write_line(2, "       git status [-s|--short|--porcelain] [--color[=WHEN]|--no-color]");
    rt_write_line(2, "       git diff [--stat] [--cached|--staged] [--color[=WHEN]|--no-color] [<rev> <rev>|<rev>..<rev>] [--] [path ...]");
    rt_write_line(2, "       git ls-files [--cached|--others] [--exclude-standard] [--] [path ...]");
    rt_write_line(2, "       git add [-N|--intent-to-add] [--] path ...");
    rt_write_line(2, "       git commit [-m|--message MESSAGE] [--allow-empty]");
}

int main(int argc, char **argv) {
    GitRepo repo;
    const char *cmd;
    int argi = 1;

    while (argi < argc && rt_strcmp(argv[argi], "--no-pager") == 0) {
        argi += 1;
    }
    if (argi >= argc || rt_strcmp(argv[argi], "--help") == 0 || rt_strcmp(argv[argi], "-h") == 0) {
        git_usage();
        return argi >= argc ? 1 : 0;
    }

    cmd = argv[argi];
    if (rt_strcmp(cmd, "hash-object") == 0) {
        return git_cmd_hash_object(argc, argv, argi + 1);
    }
    if (rt_strcmp(cmd, "clone") == 0) {
        return git_cmd_clone(argc, argv, argi + 1);
    }

    if (git_discover(&repo) != 0 || git_load_head(&repo) != 0) {
        tool_write_error("git", "not a git repository", 0);
        return 1;
    }

    if (rt_strcmp(cmd, "status") == 0) {
        return git_cmd_status(&repo, argc, argv, argi + 1);
    }
    if (rt_strcmp(cmd, "diff") == 0) {
        return git_cmd_diff(&repo, argc, argv, argi + 1);
    }
    if (rt_strcmp(cmd, "branch") == 0) {
        return git_cmd_branch(&repo, argc, argv, argi + 1);
    }
    if (rt_strcmp(cmd, "rev-parse") == 0) {
        return git_cmd_rev_parse(&repo, argc, argv, argi + 1);
    }
    if (rt_strcmp(cmd, "ls-files") == 0) {
        return git_cmd_ls_files(&repo, argc, argv, argi + 1);
    }
    if (rt_strcmp(cmd, "add") == 0) {
        return git_cmd_add(&repo, argc, argv, argi + 1);
    }
    if (rt_strcmp(cmd, "commit") == 0) {
        return git_cmd_commit(&repo, argc, argv, argi + 1);
    }
    if (rt_strcmp(cmd, "fetch") == 0) {
        return git_cmd_fetch(&repo, argc, argv, argi + 1);
    }
    if (rt_strcmp(cmd, "checkout") == 0) {
        return git_cmd_checkout(&repo, argc, argv, argi + 1);
    }

    tool_write_error("git", "unsupported command: ", cmd);
    return 1;
}