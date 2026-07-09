#include "platform.h"
#include "runtime.h"

#define WASM_MAX_NODES 8192U
#define WASM_MAX_FDS 128U
#define WASM_PATH_MAX 2048U
#define WASM_STDIO_CAPACITY (1024U * 1024U)
#define WASM_PAGE_SIZE 65536U

#define WASM_MODE_DIR 0040755U
#define WASM_MODE_FILE 0100644U
#define WASM_MODE_FILE_EXEC 0100755U

extern unsigned char __heap_base;

static unsigned long wasm_heap_offset;

void *platform_allocate_pages(size_t size) {
    unsigned long start;
    unsigned long end;
    unsigned long current_pages;
    unsigned long needed_pages;

    if (size == 0U) return 0;
    if (wasm_heap_offset == 0UL) {
        wasm_heap_offset = ((unsigned long)&__heap_base + (WASM_PAGE_SIZE - 1U)) & ~(unsigned long)(WASM_PAGE_SIZE - 1U);
    }
    start = wasm_heap_offset;
    end = (start + (unsigned long)size + (WASM_PAGE_SIZE - 1U)) & ~(unsigned long)(WASM_PAGE_SIZE - 1U);
    if (end < start) return 0;
    current_pages = (unsigned long)__builtin_wasm_memory_size(0);
    needed_pages = (end + (WASM_PAGE_SIZE - 1U)) / WASM_PAGE_SIZE;
    if (needed_pages > current_pages) {
        unsigned long grow_pages = needed_pages - current_pages;
        if (__builtin_wasm_memory_grow(0, grow_pages) == (size_t)-1) return 0;
    }
    wasm_heap_offset = end;
    return (void *)start;
}

size_t platform_page_size(void) {
    return WASM_PAGE_SIZE;
}

int platform_free_pages(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
    return 0;
}

typedef struct {
    int used;
    int is_dir;
    unsigned int mode;
    unsigned long long inode;
    long long mtime;
    char path[WASM_PATH_MAX];
    unsigned char *data;
    size_t size;
    size_t capacity;
} WasmNode;

typedef struct {
    int used;
    int node_index;
    size_t offset;
    int readable;
    int writable;
    int append;
} WasmFd;

static WasmNode wasm_nodes[WASM_MAX_NODES];
static size_t wasm_node_count;
static WasmFd wasm_fds[WASM_MAX_FDS];
static char wasm_cwd[WASM_PATH_MAX];
static unsigned long long wasm_next_inode = 1ULL;
static long long wasm_now = 1782000000LL;
static char wasm_stdout_buffer[WASM_STDIO_CAPACITY];
static char wasm_stderr_buffer[WASM_STDIO_CAPACITY];
static size_t wasm_stdout_size;
static size_t wasm_stderr_size;
static int wasm_initialized;

static size_t wasm_strlen(const char *text) {
    size_t length = 0U;
    if (text == 0) return 0U;
    while (text[length] != '\0') length += 1U;
    return length;
}

static int wasm_streq(const char *left, const char *right) {
    size_t index = 0U;
    if (left == 0 || right == 0) return 0;
    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) return 0;
        index += 1U;
    }
    return left[index] == right[index];
}

static int wasm_starts_with_path(const char *path, const char *prefix) {
    size_t prefix_length = wasm_strlen(prefix);
    if (prefix_length == 1U && prefix[0] == '/') return path[0] == '/';
    if (rt_strncmp(path, prefix, prefix_length) != 0) return 0;
    return path[prefix_length] == '\0' || path[prefix_length] == '/';
}

static void wasm_copy_string(char *dst, size_t dst_size, const char *src) {
    size_t index = 0U;
    if (dst_size == 0U) return;
    if (src == 0) src = "";
    while (index + 1U < dst_size && src[index] != '\0') {
        dst[index] = src[index];
        index += 1U;
    }
    dst[index] = '\0';
}

static int wasm_append_char(char *buffer, size_t buffer_size, size_t *length_io, char ch) {
    if (*length_io + 1U >= buffer_size) return -1;
    buffer[*length_io] = ch;
    *length_io += 1U;
    buffer[*length_io] = '\0';
    return 0;
}

static int wasm_append_text(char *buffer, size_t buffer_size, size_t *length_io, const char *text, size_t length) {
    size_t index;
    for (index = 0U; index < length; ++index) {
        if (wasm_append_char(buffer, buffer_size, length_io, text[index]) != 0) return -1;
    }
    return 0;
}

static int wasm_pop_path_component(char *buffer, size_t *length_io) {
    if (*length_io <= 1U) {
        *length_io = 1U;
        buffer[0] = '/';
        buffer[1] = '\0';
        return 0;
    }
    while (*length_io > 1U && buffer[*length_io - 1U] == '/') *length_io -= 1U;
    while (*length_io > 1U && buffer[*length_io - 1U] != '/') *length_io -= 1U;
    if (*length_io > 1U) *length_io -= 1U;
    buffer[*length_io] = '\0';
    return 0;
}

static int wasm_normalize_path(const char *path, char *out, size_t out_size) {
    size_t out_length = 0U;
    size_t index = 0U;
    const char *input = path;

    if (out_size < 2U || path == 0) return -1;
    out[0] = '\0';
    if (input[0] == '/') {
        if (wasm_append_char(out, out_size, &out_length, '/') != 0) return -1;
        index = 1U;
    } else {
        size_t cwd_length = wasm_strlen(wasm_cwd[0] != '\0' ? wasm_cwd : "/repo");
        if (wasm_append_text(out, out_size, &out_length, wasm_cwd[0] != '\0' ? wasm_cwd : "/repo", cwd_length) != 0) return -1;
        if (out_length == 0U || out[out_length - 1U] != '/') {
            if (wasm_append_char(out, out_size, &out_length, '/') != 0) return -1;
        }
    }

    while (1) {
        size_t start;
        size_t length;
        while (input[index] == '/') index += 1U;
        start = index;
        while (input[index] != '\0' && input[index] != '/') index += 1U;
        length = index - start;
        if (length == 0U) break;
        if (length == 1U && input[start] == '.') {
            continue;
        }
        if (length == 2U && input[start] == '.' && input[start + 1U] == '.') {
            wasm_pop_path_component(out, &out_length);
            continue;
        }
        if (out_length > 1U && out[out_length - 1U] != '/') {
            if (wasm_append_char(out, out_size, &out_length, '/') != 0) return -1;
        }
        if (wasm_append_text(out, out_size, &out_length, input + start, length) != 0) return -1;
    }
    if (out_length > 1U && out[out_length - 1U] == '/') {
        out_length -= 1U;
        out[out_length] = '\0';
    }
    return 0;
}

static void wasm_dirname(const char *path, char *out, size_t out_size) {
    size_t length = wasm_strlen(path);
    while (length > 1U && path[length - 1U] != '/') length -= 1U;
    if (length <= 1U) {
        wasm_copy_string(out, out_size, "/");
        return;
    }
    length -= 1U;
    if (length >= out_size) length = out_size - 1U;
    memcpy(out, path, length);
    out[length] = '\0';
}

static const char *wasm_basename(const char *path) {
    size_t length = wasm_strlen(path);
    while (length > 0U && path[length - 1U] != '/') length -= 1U;
    return path + length;
}

static int wasm_find_node_normalized(const char *path) {
    size_t index;
    for (index = 0U; index < wasm_node_count; ++index) {
        if (wasm_nodes[index].used && wasm_streq(wasm_nodes[index].path, path)) return (int)index;
    }
    return -1;
}

static int wasm_find_node(const char *path) {
    char normalized[WASM_PATH_MAX];
    if (wasm_normalize_path(path, normalized, sizeof(normalized)) != 0) return -1;
    return wasm_find_node_normalized(normalized);
}

static int wasm_create_node_normalized(const char *path, int is_dir, unsigned int mode) {
    WasmNode *node;
    if (wasm_node_count >= WASM_MAX_NODES) return -1;
    if (wasm_find_node_normalized(path) >= 0) return -1;
    node = &wasm_nodes[wasm_node_count];
    rt_memset(node, 0, sizeof(*node));
    node->used = 1;
    node->is_dir = is_dir;
    node->mode = is_dir ? WASM_MODE_DIR : (0100000U | (mode & 07777U));
    if (!is_dir && (node->mode & 07777U) == 0U) node->mode = WASM_MODE_FILE;
    node->inode = wasm_next_inode++;
    node->mtime = wasm_now++;
    wasm_copy_string(node->path, sizeof(node->path), path);
    wasm_node_count += 1U;
    return (int)(wasm_node_count - 1U);
}

static int wasm_make_directory_normalized(const char *path) {
    char parent[WASM_PATH_MAX];
    int existing = wasm_find_node_normalized(path);
    if (existing >= 0) return wasm_nodes[existing].is_dir ? 0 : -1;
    if (!wasm_streq(path, "/")) {
        int parent_index;
        wasm_dirname(path, parent, sizeof(parent));
        parent_index = wasm_find_node_normalized(parent);
        if (parent_index < 0 || !wasm_nodes[parent_index].is_dir) return -1;
    }
    return wasm_create_node_normalized(path, 1, 0755U) >= 0 ? 0 : -1;
}

static int wasm_ensure_parent_dirs_normalized(const char *path) {
    char scratch[WASM_PATH_MAX];
    size_t index;
    size_t length;

    wasm_copy_string(scratch, sizeof(scratch), path);
    length = wasm_strlen(scratch);
    for (index = 1U; index < length; ++index) {
        if (scratch[index] == '/') {
            scratch[index] = '\0';
            if (wasm_make_directory_normalized(scratch) != 0) return -1;
            scratch[index] = '/';
        }
    }
    return 0;
}

static void wasm_fill_entry(const WasmNode *node, PlatformDirEntry *entry) {
    rt_memset(entry, 0, sizeof(*entry));
    wasm_copy_string(entry->name, sizeof(entry->name), wasm_basename(node->path));
    entry->mode = node->mode;
    entry->uid = 1000U;
    entry->gid = 1000U;
    entry->size = (unsigned long long)node->size;
    entry->inode = node->inode;
    entry->nlink = 1UL;
    entry->atime = node->mtime;
    entry->mtime = node->mtime;
    entry->ctime = node->mtime;
    entry->is_dir = node->is_dir;
    entry->is_hidden = entry->name[0] == '.';
    wasm_copy_string(entry->owner, sizeof(entry->owner), "web");
    wasm_copy_string(entry->group, sizeof(entry->group), "web");
}

static void wasm_reset_nodes(void) {
    rt_memset(wasm_nodes, 0, sizeof(wasm_nodes));
    rt_memset(wasm_fds, 0, sizeof(wasm_fds));
    wasm_node_count = 0U;
    wasm_next_inode = 1ULL;
    wasm_now = 1782000000LL;
    (void)wasm_create_node_normalized("/", 1, 0755U);
    (void)wasm_create_node_normalized("/repo", 1, 0755U);
    wasm_copy_string(wasm_cwd, sizeof(wasm_cwd), "/repo");
}

static void wasm_init_once(void) {
    if (!wasm_initialized) {
        wasm_initialized = 1;
        wasm_reset_nodes();
    }
}

void newos_wasm_begin_command(void) {
    wasm_init_once();
    wasm_stdout_size = 0U;
    wasm_stderr_size = 0U;
}

__attribute__((export_name("newos_wasm_reset")))
void newos_wasm_reset(void) {
    wasm_initialized = 1;
    wasm_stdout_size = 0U;
    wasm_stderr_size = 0U;
    wasm_reset_nodes();
}

__attribute__((export_name("newos_wasm_alloc")))
void *newos_wasm_alloc(size_t size) {
    return rt_malloc(size == 0U ? 1U : size);
}

__attribute__((export_name("newos_wasm_free")))
void newos_wasm_free(void *ptr) {
    rt_free(ptr);
}

__attribute__((export_name("newos_wasm_stdout_ptr")))
const char *newos_wasm_stdout_ptr(void) {
    return wasm_stdout_buffer;
}

__attribute__((export_name("newos_wasm_stdout_size")))
size_t newos_wasm_stdout_size(void) {
    return wasm_stdout_size;
}

__attribute__((export_name("newos_wasm_stderr_ptr")))
const char *newos_wasm_stderr_ptr(void) {
    return wasm_stderr_buffer;
}

__attribute__((export_name("newos_wasm_stderr_size")))
size_t newos_wasm_stderr_size(void) {
    return wasm_stderr_size;
}

__attribute__((export_name("newos_wasm_write_file")))
int newos_wasm_write_file(const char *path, const unsigned char *data, size_t size) {
    char normalized[WASM_PATH_MAX];
    int node_index;
    WasmNode *node;

    wasm_init_once();
    if (wasm_normalize_path(path, normalized, sizeof(normalized)) != 0) return -1;
    if (wasm_ensure_parent_dirs_normalized(normalized) != 0) return -1;
    node_index = wasm_find_node_normalized(normalized);
    if (node_index < 0) node_index = wasm_create_node_normalized(normalized, 0, 0644U);
    if (node_index < 0 || wasm_nodes[node_index].is_dir) return -1;
    node = &wasm_nodes[node_index];
    if (node->capacity < size) {
        unsigned char *next = (unsigned char *)rt_realloc(node->data, size == 0U ? 1U : size);
        if (next == 0) return -1;
        node->data = next;
        node->capacity = size;
    }
    if (size != 0U) memcpy(node->data, data, size);
    node->size = size;
    node->mtime = wasm_now++;
    return 0;
}

__attribute__((export_name("newos_wasm_file_size")))
int newos_wasm_file_size(const char *path) {
    int node_index;
    wasm_init_once();
    node_index = wasm_find_node(path);
    if (node_index < 0 || wasm_nodes[node_index].is_dir || wasm_nodes[node_index].size > 0x7fffffffU) return -1;
    return (int)wasm_nodes[node_index].size;
}

__attribute__((export_name("newos_wasm_read_file")))
int newos_wasm_read_file(const char *path, unsigned char *out, size_t capacity) {
    int node_index;
    wasm_init_once();
    node_index = wasm_find_node(path);
    if (node_index < 0 || wasm_nodes[node_index].is_dir || wasm_nodes[node_index].size > capacity || wasm_nodes[node_index].size > 0x7fffffffU) return -1;
    if (wasm_nodes[node_index].size != 0U) memcpy(out, wasm_nodes[node_index].data, wasm_nodes[node_index].size);
    return (int)wasm_nodes[node_index].size;
}

__attribute__((export_name("newos_wasm_list_files")))
int newos_wasm_list_files(char *out, size_t capacity, int include_git) {
    size_t used = 0U;
    size_t index;

    wasm_init_once();
    if (out == 0 || capacity == 0U) return -1;
    out[0] = '\0';
    for (index = 0U; index < wasm_node_count; ++index) {
        const WasmNode *node = &wasm_nodes[index];
        const char *relative;
        size_t relative_length;

        if (!node->used || rt_strncmp(node->path, "/repo", 5U) != 0) continue;
        if (node->path[5] == '\0') continue;
        if (node->path[5] != '/') continue;
        relative = node->path + 6U;
        if (!include_git && (rt_strncmp(relative, ".git", 4U) == 0 && (relative[4] == '\0' || relative[4] == '/'))) continue;
        relative_length = wasm_strlen(relative);
        if (used + 3U + relative_length >= capacity) return -1;
        out[used++] = node->is_dir ? 'd' : 'f';
        out[used++] = '\t';
        memcpy(out + used, relative, relative_length);
        used += relative_length;
        out[used++] = '\n';
        out[used] = '\0';
    }
    return (int)used;
}

__attribute__((export_name("newos_wasm_remove_path")))
int newos_wasm_remove_path(const char *path) {
    char normalized[WASM_PATH_MAX];
    size_t index;
    int node_index;

    wasm_init_once();
    if (wasm_normalize_path(path, normalized, sizeof(normalized)) != 0) return -1;
    if (wasm_streq(normalized, "/") || wasm_streq(normalized, "/repo")) return -1;
    node_index = wasm_find_node_normalized(normalized);
    if (node_index < 0) return -1;
    if (wasm_nodes[node_index].is_dir) {
        for (index = 0U; index < wasm_node_count; ++index) {
            if (wasm_nodes[index].used && wasm_starts_with_path(wasm_nodes[index].path, normalized)) {
                wasm_nodes[index].used = 0;
            }
        }
    } else {
        wasm_nodes[node_index].used = 0;
    }
    return 0;
}

long platform_write(int fd, const void *buffer, size_t count) {
    char *target = 0;
    size_t *target_size = 0;

    wasm_init_once();
    if (fd == 1 || fd == 2) {
        target = fd == 1 ? wasm_stdout_buffer : wasm_stderr_buffer;
        target_size = fd == 1 ? &wasm_stdout_size : &wasm_stderr_size;
        if (*target_size < WASM_STDIO_CAPACITY) {
            size_t available = WASM_STDIO_CAPACITY - *target_size;
            size_t chunk = count < available ? count : available;
            if (chunk != 0U) memcpy(target + *target_size, buffer, chunk);
            *target_size += chunk;
        }
        return (long)count;
    }
    if (fd >= 3 && (size_t)fd < WASM_MAX_FDS && wasm_fds[fd].used && wasm_fds[fd].writable) {
        WasmFd *handle = &wasm_fds[fd];
        WasmNode *node = &wasm_nodes[handle->node_index];
        size_t end = handle->offset + count;
        if (end < handle->offset) return -1;
        if (end > node->capacity) {
            size_t next_capacity = node->capacity == 0U ? 64U : node->capacity;
            unsigned char *next;
            while (next_capacity < end) {
                size_t doubled = next_capacity * 2U;
                if (doubled <= next_capacity) {
                    next_capacity = end;
                    break;
                }
                next_capacity = doubled;
            }
            next = (unsigned char *)rt_realloc(node->data, next_capacity);
            if (next == 0) return -1;
            if (next_capacity > node->capacity) rt_memset(next + node->capacity, 0, next_capacity - node->capacity);
            node->data = next;
            node->capacity = next_capacity;
        }
        if (count != 0U) memcpy(node->data + handle->offset, buffer, count);
        handle->offset = end;
        if (end > node->size) node->size = end;
        node->mtime = wasm_now++;
        return (long)count;
    }
    return -1;
}

long platform_read(int fd, void *buffer, size_t count) {
    wasm_init_once();
    if (fd == 0) return 0;
    if (fd >= 3 && (size_t)fd < WASM_MAX_FDS && wasm_fds[fd].used && wasm_fds[fd].readable) {
        WasmFd *handle = &wasm_fds[fd];
        WasmNode *node = &wasm_nodes[handle->node_index];
        size_t remaining;
        size_t chunk;
        if (handle->offset >= node->size) return 0;
        remaining = node->size - handle->offset;
        chunk = count < remaining ? count : remaining;
        if (chunk != 0U) memcpy(buffer, node->data + handle->offset, chunk);
        handle->offset += chunk;
        return (long)chunk;
    }
    return -1;
}

static int wasm_open_fd(int node_index, int readable, int writable, int append) {
    size_t fd;
    for (fd = 3U; fd < WASM_MAX_FDS; ++fd) {
        if (!wasm_fds[fd].used) {
            wasm_fds[fd].used = 1;
            wasm_fds[fd].node_index = node_index;
            wasm_fds[fd].readable = readable;
            wasm_fds[fd].writable = writable;
            wasm_fds[fd].append = append;
            wasm_fds[fd].offset = append ? wasm_nodes[node_index].size : 0U;
            return (int)fd;
        }
    }
    return -1;
}

int platform_open_read(const char *path) {
    int node_index;
    wasm_init_once();
    if (path == 0 || wasm_streq(path, "-")) return 0;
    node_index = wasm_find_node(path);
    if (node_index < 0 || wasm_nodes[node_index].is_dir) return -1;
    return wasm_open_fd(node_index, 1, 0, 0);
}

int platform_open_read_secure(const char *path, PlatformDirEntry *entry_out) {
    int fd = platform_open_read(path);
    if (fd >= 0 && entry_out != 0) wasm_fill_entry(&wasm_nodes[wasm_fds[fd].node_index], entry_out);
    return fd;
}

int platform_open_write_mode(const char *path, unsigned int mode, int truncate_existing) {
    char normalized[WASM_PATH_MAX];
    int node_index;
    wasm_init_once();
    if (path == 0 || wasm_streq(path, "-")) return 1;
    if (wasm_normalize_path(path, normalized, sizeof(normalized)) != 0) return -1;
    if (wasm_ensure_parent_dirs_normalized(normalized) != 0) return -1;
    node_index = wasm_find_node_normalized(normalized);
    if (node_index < 0) node_index = wasm_create_node_normalized(normalized, 0, mode);
    if (node_index < 0 || wasm_nodes[node_index].is_dir) return -1;
    wasm_nodes[node_index].mode = 0100000U | (mode & 07777U);
    if (truncate_existing) wasm_nodes[node_index].size = 0U;
    wasm_nodes[node_index].mtime = wasm_now++;
    return wasm_open_fd(node_index, 0, 1, 0);
}

int platform_open_write_untraced(const char *path, unsigned int mode, int truncate_existing) {
    return platform_open_write_mode(path, mode, truncate_existing);
}

int platform_open_write(const char *path, unsigned int mode) {
    return platform_open_write_mode(path, mode, 1);
}

int platform_open_create_exclusive(const char *path, unsigned int mode) {
    char normalized[WASM_PATH_MAX];
    wasm_init_once();
    if (wasm_normalize_path(path, normalized, sizeof(normalized)) != 0) return -1;
    if (wasm_find_node_normalized(normalized) >= 0) return -1;
    return platform_open_write_mode(path, mode, 1);
}

int platform_open_append(const char *path, unsigned int mode) {
    int fd = platform_open_write_mode(path, mode, 0);
    if (fd >= 0) {
        wasm_fds[fd].append = 1;
        wasm_fds[fd].offset = wasm_nodes[wasm_fds[fd].node_index].size;
    }
    return fd;
}

int platform_open_append_existing(const char *path) {
    int node_index;
    wasm_init_once();
    node_index = wasm_find_node(path);
    if (node_index < 0 || wasm_nodes[node_index].is_dir) return -1;
    return wasm_open_fd(node_index, 0, 1, 1);
}

long long platform_seek(int fd, long long offset, int whence) {
    WasmFd *handle;
    long long base;
    long long next;
    wasm_init_once();
    if (fd < 3 || (size_t)fd >= WASM_MAX_FDS || !wasm_fds[fd].used) return -1;
    handle = &wasm_fds[fd];
    if (whence == PLATFORM_SEEK_SET) base = 0;
    else if (whence == PLATFORM_SEEK_CUR) base = (long long)handle->offset;
    else if (whence == PLATFORM_SEEK_END) base = (long long)wasm_nodes[handle->node_index].size;
    else return -1;
    next = base + offset;
    if (next < 0) return -1;
    handle->offset = (size_t)next;
    return next;
}

int platform_close(int fd) {
    if (fd >= 3 && (size_t)fd < WASM_MAX_FDS) rt_memset(&wasm_fds[fd], 0, sizeof(wasm_fds[fd]));
    return 0;
}

int platform_make_directory(const char *path, unsigned int mode) {
    char normalized[WASM_PATH_MAX];
    (void)mode;
    wasm_init_once();
    if (wasm_normalize_path(path, normalized, sizeof(normalized)) != 0) return -1;
    return wasm_make_directory_normalized(normalized);
}

int platform_remove_file(const char *path) {
    int node_index;
    wasm_init_once();
    node_index = wasm_find_node(path);
    if (node_index < 0 || wasm_nodes[node_index].is_dir) return -1;
    wasm_nodes[node_index].used = 0;
    return 0;
}

int platform_remove_directory(const char *path) {
    char normalized[WASM_PATH_MAX];
    size_t index;
    int node_index;
    wasm_init_once();
    if (wasm_normalize_path(path, normalized, sizeof(normalized)) != 0) return -1;
    node_index = wasm_find_node_normalized(normalized);
    if (node_index < 0 || !wasm_nodes[node_index].is_dir || wasm_streq(normalized, "/")) return -1;
    for (index = 0U; index < wasm_node_count; ++index) {
        if (wasm_nodes[index].used && index != (size_t)node_index && wasm_starts_with_path(wasm_nodes[index].path, normalized)) return -1;
    }
    wasm_nodes[node_index].used = 0;
    return 0;
}

int platform_rename_path(const char *old_path, const char *new_path) {
    char old_normalized[WASM_PATH_MAX];
    char new_normalized[WASM_PATH_MAX];
    size_t old_length;
    size_t index;
    int old_index;
    wasm_init_once();
    if (wasm_normalize_path(old_path, old_normalized, sizeof(old_normalized)) != 0 || wasm_normalize_path(new_path, new_normalized, sizeof(new_normalized)) != 0) return -1;
    if (wasm_find_node_normalized(new_normalized) >= 0 || wasm_ensure_parent_dirs_normalized(new_normalized) != 0) return -1;
    old_index = wasm_find_node_normalized(old_normalized);
    if (old_index < 0) return -1;
    old_length = wasm_strlen(old_normalized);
    for (index = 0U; index < wasm_node_count; ++index) {
        if (wasm_nodes[index].used && wasm_starts_with_path(wasm_nodes[index].path, old_normalized)) {
            char suffix[WASM_PATH_MAX];
            char replaced[WASM_PATH_MAX];
            size_t replaced_length = 0U;
            wasm_copy_string(suffix, sizeof(suffix), wasm_nodes[index].path + old_length);
            if (wasm_append_text(replaced, sizeof(replaced), &replaced_length, new_normalized, wasm_strlen(new_normalized)) != 0 || wasm_append_text(replaced, sizeof(replaced), &replaced_length, suffix, wasm_strlen(suffix)) != 0) return -1;
            wasm_copy_string(wasm_nodes[index].path, sizeof(wasm_nodes[index].path), replaced);
            wasm_nodes[index].mtime = wasm_now++;
        }
    }
    return 0;
}

int platform_change_mode(const char *path, unsigned int mode) {
    int node_index;
    wasm_init_once();
    node_index = wasm_find_node(path);
    if (node_index < 0) return -1;
    wasm_nodes[node_index].mode = wasm_nodes[node_index].is_dir ? (0040000U | (mode & 07777U)) : (0100000U | (mode & 07777U));
    return 0;
}

int platform_path_access(const char *path, int mode) {
    int node_index;
    (void)mode;
    wasm_init_once();
    node_index = wasm_find_node(path);
    return node_index >= 0 ? 0 : -1;
}

int platform_path_is_directory(const char *path, int *is_directory_out) {
    int node_index;
    wasm_init_once();
    node_index = wasm_find_node(path);
    if (node_index < 0) return -1;
    if (is_directory_out != 0) *is_directory_out = wasm_nodes[node_index].is_dir;
    return 0;
}

int platform_get_path_info(const char *path, PlatformDirEntry *entry_out) {
    int node_index;
    wasm_init_once();
    node_index = wasm_find_node(path);
    if (node_index < 0) return -1;
    if (entry_out != 0) wasm_fill_entry(&wasm_nodes[node_index], entry_out);
    return 0;
}

int platform_get_path_info_follow(const char *path, PlatformDirEntry *entry_out) {
    return platform_get_path_info(path, entry_out);
}

int platform_collect_entries(const char *path, int include_hidden, PlatformDirEntry *entries_out, size_t entry_capacity, size_t *count_out, int *path_is_directory) {
    char normalized[WASM_PATH_MAX];
    size_t normalized_length;
    size_t count = 0U;
    size_t index;
    int node_index;

    wasm_init_once();
    if (wasm_normalize_path(path, normalized, sizeof(normalized)) != 0) return -1;
    node_index = wasm_find_node_normalized(normalized);
    if (node_index < 0) return -1;
    if (!wasm_nodes[node_index].is_dir) {
        if (count_out != 0) *count_out = 0U;
        if (path_is_directory != 0) *path_is_directory = 0;
        return 0;
    }
    normalized_length = wasm_strlen(normalized);
    for (index = 0U; index < wasm_node_count; ++index) {
        const char *rest;
        if (!wasm_nodes[index].used || index == (size_t)node_index) continue;
        if (wasm_streq(normalized, "/")) {
            if (wasm_nodes[index].path[0] != '/') continue;
            rest = wasm_nodes[index].path + 1U;
        } else {
            if (rt_strncmp(wasm_nodes[index].path, normalized, normalized_length) != 0 || wasm_nodes[index].path[normalized_length] != '/') continue;
            rest = wasm_nodes[index].path + normalized_length + 1U;
        }
        if (rest[0] == '\0') continue;
        if (!include_hidden && rest[0] == '.') continue;
        {
            const char *slash = rest;
            while (*slash != '\0' && *slash != '/') slash += 1U;
            if (*slash == '/') continue;
        }
        if (count < entry_capacity) wasm_fill_entry(&wasm_nodes[index], &entries_out[count]);
        count += 1U;
    }
    if (count_out != 0) *count_out = count <= entry_capacity ? count : entry_capacity;
    if (path_is_directory != 0) *path_is_directory = 1;
    return count <= entry_capacity ? 0 : -1;
}

void platform_free_entries(PlatformDirEntry *entries, size_t count) {
    (void)entries;
    (void)count;
}

int platform_get_current_directory(char *buffer, size_t buffer_size) {
    wasm_init_once();
    if (wasm_strlen(wasm_cwd) + 1U > buffer_size) return -1;
    wasm_copy_string(buffer, buffer_size, wasm_cwd);
    return 0;
}

int platform_change_directory(const char *path) {
    char normalized[WASM_PATH_MAX];
    int node_index;
    wasm_init_once();
    if (wasm_normalize_path(path, normalized, sizeof(normalized)) != 0) return -1;
    node_index = wasm_find_node_normalized(normalized);
    if (node_index < 0 || !wasm_nodes[node_index].is_dir) return -1;
    wasm_copy_string(wasm_cwd, sizeof(wasm_cwd), normalized);
    return 0;
}

const char *platform_getenv(const char *name) {
    if (wasm_streq(name, "USER")) return "web";
    if (wasm_streq(name, "GIT_AUTHOR_NAME")) return "Web User";
    if (wasm_streq(name, "GIT_AUTHOR_EMAIL")) return "web@example.invalid";
    if (wasm_streq(name, "GIT_COMMITTER_NAME")) return "Web User";
    if (wasm_streq(name, "GIT_COMMITTER_EMAIL")) return "web@example.invalid";
    if (wasm_streq(name, "TERM")) return "dumb";
    if (wasm_streq(name, "NO_COLOR")) return "1";
    return 0;
}

const char *platform_getenv_entry(size_t index) { (void)index; return 0; }
int platform_setenv(const char *name, const char *value, int overwrite) { (void)name; (void)value; (void)overwrite; return -1; }
int platform_unsetenv(const char *name) { (void)name; return -1; }
int platform_clearenv(void) { return -1; }
int platform_isatty(int fd) { (void)fd; return 0; }
int platform_get_terminal_size(int fd, unsigned int *rows_out, unsigned int *columns_out) { (void)fd; if (rows_out) *rows_out = 24U; if (columns_out) *columns_out = 80U; return 0; }
int platform_get_process_id(void) { return 1; }
long long platform_get_epoch_time(void) { wasm_init_once(); return wasm_now++; }
unsigned long long platform_get_monotonic_time_ns(void) { return (unsigned long long)platform_get_epoch_time() * 1000000000ULL; }
int platform_sleep_milliseconds(unsigned long long milliseconds) { (void)milliseconds; return 0; }
int platform_sleep_seconds(unsigned int seconds) { (void)seconds; return 0; }
int platform_random_bytes(unsigned char *buffer, size_t count) { size_t i; for (i = 0U; i < count; ++i) buffer[i] = (unsigned char)((i * 1103515245U + 12345U) >> 16U); return 0; }
int platform_read_symlink(const char *path, char *buffer, size_t buffer_size) { (void)path; (void)buffer; (void)buffer_size; return -1; }
int platform_create_symbolic_link(const char *target_path, const char *link_path) { (void)target_path; (void)link_path; return -1; }
int platform_create_hard_link(const char *target_path, const char *link_path) { (void)target_path; (void)link_path; return -1; }
int platform_change_owner_ex(const char *path, unsigned int uid, unsigned int gid, int follow_symlinks) { (void)path; (void)uid; (void)gid; (void)follow_symlinks; return 0; }
int platform_change_owner(const char *path, unsigned int uid, unsigned int gid) { return platform_change_owner_ex(path, uid, gid, 1); }
int platform_touch_path(const char *path) { int fd = platform_open_append(path, 0644U); if (fd < 0) return -1; return platform_close(fd); }
int platform_truncate_path(const char *path, unsigned long long size) { int node_index = wasm_find_node(path); WasmNode *node; if (node_index < 0 || wasm_nodes[node_index].is_dir || size > (unsigned long long)((size_t)-1)) return -1; node = &wasm_nodes[node_index]; if ((size_t)size > node->capacity) { unsigned char *next = (unsigned char *)rt_realloc(node->data, (size_t)size); if (next == 0) return -1; rt_memset(next + node->capacity, 0, (size_t)size - node->capacity); node->data = next; node->capacity = (size_t)size; } node->size = (size_t)size; return 0; }
int platform_set_path_times(const char *path, long long atime, long long mtime, int create_if_missing, int update_access, int update_modify) { (void)atime; (void)update_access; (void)update_modify; if (platform_get_path_info(path, 0) != 0) { if (!create_if_missing) return -1; if (platform_open_write(path, 0644U) < 0) return -1; } { int node_index = wasm_find_node(path); if (node_index >= 0) wasm_nodes[node_index].mtime = mtime; } return 0; }
int platform_create_temp_file(char *path_buffer, size_t buffer_size, const char *prefix, unsigned int mode) { (void)mode; if (prefix == 0) prefix = "/repo/tmp"; if (wasm_strlen(prefix) + 16U > buffer_size) return -1; wasm_copy_string(path_buffer, buffer_size, prefix); return platform_open_create_exclusive(path_buffer, 0600U); }
int platform_stream_file_to_stdout(const char *path) { int fd = platform_open_read(path); char buffer[4096]; if (fd < 0) return -1; for (;;) { long got = platform_read(fd, buffer, sizeof(buffer)); if (got < 0) { platform_close(fd); return -1; } if (got == 0) break; if (platform_write(1, buffer, (size_t)got) != got) { platform_close(fd); return -1; } } return platform_close(fd); }
int platform_sync_all(void) { return 0; }
int platform_sync_path(const char *path) { (void)path; return 0; }
int platform_sync_path_data(const char *path) { (void)path; return 0; }
int platform_get_filesystem_info(const char *path, PlatformFilesystemInfo *info_out) { (void)path; rt_memset(info_out, 0, sizeof(*info_out)); info_out->total_bytes = 64ULL * 1024ULL * 1024ULL; info_out->free_bytes = 32ULL * 1024ULL * 1024ULL; info_out->available_bytes = info_out->free_bytes; wasm_copy_string(info_out->type_name, sizeof(info_out->type_name), "wasmfs"); return 0; }
int platform_get_filesystem_usage(const char *path, unsigned long long *total_bytes_out, unsigned long long *free_bytes_out, unsigned long long *available_bytes_out) { (void)path; if (total_bytes_out) *total_bytes_out = 64ULL * 1024ULL * 1024ULL; if (free_bytes_out) *free_bytes_out = 32ULL * 1024ULL * 1024ULL; if (available_bytes_out) *available_bytes_out = 32ULL * 1024ULL * 1024ULL; return 0; }

int platform_connect_tcp(const char *host, unsigned int port, int *socket_fd_out) { (void)host; (void)port; (void)socket_fd_out; return -1; }
int platform_open_tcp_listener(const char *host, unsigned int port, int *socket_fd_out) { (void)host; (void)port; (void)socket_fd_out; return -1; }
int platform_accept_tcp(int listener_fd, int *client_fd_out) { (void)listener_fd; (void)client_fd_out; return -1; }
int platform_tls_connect(PlatformTlsClient *client, const char *host, unsigned int port) { (void)client; (void)host; (void)port; return -1; }
int platform_tls_connect_timeout(PlatformTlsClient *client, const char *host, unsigned int port, unsigned int timeout_milliseconds) { (void)timeout_milliseconds; return platform_tls_connect(client, host, port); }
const char *platform_tls_last_error(void) { return "browser transport not implemented"; }
const char *platform_tls_peer_verification_status(void) { return "unavailable"; }
int platform_tls_peer_info(PlatformTlsClient *client, PlatformTlsPeerInfo *info_out) { (void)client; (void)info_out; return -1; }
long platform_tls_read(PlatformTlsClient *client, void *buffer, size_t count) { (void)client; (void)buffer; (void)count; return -1; }
long platform_tls_write(PlatformTlsClient *client, const void *buffer, size_t count) { (void)client; (void)buffer; (void)count; return -1; }
void platform_tls_close(PlatformTlsClient *client) { (void)client; }
int platform_create_pipe(int pipe_fds[2]) { (void)pipe_fds; return -1; }
int platform_spawn_process(char *const argv[], int stdin_fd, int stdout_fd, const char *input_path, const char *output_path, int output_append, int *pid_out) { (void)argv; (void)stdin_fd; (void)stdout_fd; (void)input_path; (void)output_path; (void)output_append; (void)pid_out; return -1; }
int platform_spawn_process_ex(char *const argv[], int stdin_fd, int stdout_fd, const char *input_path, const char *output_path, int output_append, const char *working_directory, const char *drop_user, const char *drop_group, int *pid_out) { (void)working_directory; (void)drop_user; (void)drop_group; return platform_spawn_process(argv, stdin_fd, stdout_fd, input_path, output_path, output_append, pid_out); }
int platform_wait_process(int pid, int *exit_status_out) { (void)pid; if (exit_status_out) *exit_status_out = 127; return -1; }
int platform_wait_process_usage(int pid, int *exit_status_out, PlatformProcessUsage *usage_out) { (void)usage_out; return platform_wait_process(pid, exit_status_out); }
int platform_wait_process_timeout(int pid, unsigned long long timeout_milliseconds, unsigned long long kill_after_milliseconds, int signal_number, int preserve_status, int *exit_status_out) { (void)timeout_milliseconds; (void)kill_after_milliseconds; (void)signal_number; (void)preserve_status; return platform_wait_process(pid, exit_status_out); }
int platform_poll_process_exit(int pid, int *finished_out, int *exit_status_out) { (void)pid; if (finished_out) *finished_out = 1; if (exit_status_out) *exit_status_out = 127; return 0; }
int platform_get_current_process_usage(PlatformProcessUsage *usage_out) { rt_memset(usage_out, 0, sizeof(*usage_out)); return 0; }
int platform_poll_fds(const int *fds, size_t fd_count, size_t *ready_index_out, int timeout_milliseconds) { (void)fds; (void)fd_count; (void)timeout_milliseconds; if (ready_index_out) *ready_index_out = 0U; return -1; }

void platform_mutex_init(PlatformMutex *mutex) { if (mutex) mutex->state = 0; }
void platform_mutex_lock(PlatformMutex *mutex) { (void)mutex; }
void platform_mutex_unlock(PlatformMutex *mutex) { (void)mutex; }
void platform_semaphore_init(PlatformSemaphore *semaphore, int value) { if (semaphore) semaphore->count = value; }
void platform_semaphore_wait(PlatformSemaphore *semaphore) { (void)semaphore; }
void platform_semaphore_post(PlatformSemaphore *semaphore) { (void)semaphore; }
int platform_thread_start(PlatformThread *thread, PlatformThreadMain entry, void *arg, size_t stack_size) { (void)thread; (void)entry; (void)arg; (void)stack_size; return -1; }
int platform_thread_join(PlatformThread *thread, int *result_out) { (void)thread; if (result_out) *result_out = -1; return -1; }
int platform_worker_threads_supported(void) { return 0; }
unsigned int platform_worker_thread_count(void) { return 1U; }
int platform_worker_thread_start(PlatformWorkerThread *thread, PlatformWorkerMain entry, void *arg, size_t stack_size) { (void)thread; (void)entry; (void)arg; (void)stack_size; return -1; }
int platform_worker_thread_join(PlatformWorkerThread *thread, int *result_out) { (void)thread; if (result_out) *result_out = -1; return -1; }
void platform_wait_word(volatile unsigned int *word, unsigned int expected) { (void)word; (void)expected; }
void platform_wake_word_one(volatile unsigned int *word) { (void)word; }
void platform_wake_word_count(volatile unsigned int *word, unsigned int count) { (void)word; (void)count; }
void platform_wake_word_all(volatile unsigned int *word) { (void)word; }
void platform_wait_wake_stats_reset(void) {}
void platform_wait_wake_stats_get(PlatformWaitWakeStats *stats_out) { rt_memset(stats_out, 0, sizeof(*stats_out)); }

int platform_format_time(long long epoch_seconds, int use_local_time, const char *format, char *buffer, size_t buffer_size) { (void)epoch_seconds; (void)use_local_time; (void)format; wasm_copy_string(buffer, buffer_size, "2026-06-22 00:00:00"); return 0; }
int platform_send_signal(int pid, int signal_number) { (void)pid; (void)signal_number; return -1; }
int platform_ignore_signal(int signal_number) { (void)signal_number; return 0; }
int platform_shutdown_system(int action) { (void)action; return -1; }
int platform_parse_signal_name(const char *text, int *signal_out) { (void)text; if (signal_out) *signal_out = 0; return -1; }
const char *platform_signal_name(int signal_number) { (void)signal_number; return "UNKNOWN"; }
void platform_write_signal_list(int fd) { (void)fd; }
int platform_get_hostname(char *buffer, size_t buffer_size) { wasm_copy_string(buffer, buffer_size, "wasm"); return 0; }
int platform_set_hostname(const char *name) { (void)name; return -1; }
int platform_drop_privileges(const char *username, const char *groupname) { (void)username; (void)groupname; return -1; }
int platform_trace_syscalls(char *const argv[], PlatformSyscallTraceCallback callback, void *user_data, int *exit_status_out) { (void)argv; (void)callback; (void)user_data; if (exit_status_out) *exit_status_out = 127; return -1; }
int platform_get_identity(PlatformIdentity *identity_out) { rt_memset(identity_out, 0, sizeof(*identity_out)); identity_out->uid = 1000U; identity_out->gid = 1000U; wasm_copy_string(identity_out->username, sizeof(identity_out->username), "web"); wasm_copy_string(identity_out->groupname, sizeof(identity_out->groupname), "web"); return 0; }
int platform_lookup_identity(const char *username, PlatformIdentity *identity_out) { (void)username; return platform_get_identity(identity_out); }
int platform_lookup_group(const char *groupname, unsigned int *gid_out) { (void)groupname; if (gid_out) *gid_out = 1000U; return 0; }
int platform_list_groups_for_identity(const PlatformIdentity *identity, PlatformGroupEntry *entries_out, size_t entry_capacity, size_t *count_out) { (void)identity; if (entry_capacity > 0U && entries_out) { entries_out[0].gid = 1000U; wasm_copy_string(entries_out[0].name, sizeof(entries_out[0].name), "web"); } if (count_out) *count_out = entry_capacity > 0U ? 1U : 0U; return 0; }
int platform_list_processes(PlatformProcessEntry *entries_out, size_t entry_capacity, size_t *count_out) { (void)entries_out; (void)entry_capacity; if (count_out) *count_out = 0U; return 0; }
int platform_list_process_open_files(int pid, PlatformOpenFileEntry *entries_out, size_t entry_capacity, size_t *count_out) { (void)pid; (void)entries_out; (void)entry_capacity; if (count_out) *count_out = 0U; return 0; }
int platform_list_sessions(PlatformSessionEntry *entries_out, size_t entry_capacity, size_t *count_out) { (void)entries_out; (void)entry_capacity; if (count_out) *count_out = 0U; return 0; }
int platform_get_memory_info(PlatformMemoryInfo *info_out) { rt_memset(info_out, 0, sizeof(*info_out)); return 0; }
int platform_get_uptime_info(PlatformUptimeInfo *info_out) { rt_memset(info_out, 0, sizeof(*info_out)); return 0; }
int platform_get_uname(char *sysname, size_t sysname_size, char *nodename, size_t nodename_size, char *release, size_t release_size, char *version, size_t version_size, char *machine, size_t machine_size) { wasm_copy_string(sysname, sysname_size, "newos-wasm"); wasm_copy_string(nodename, nodename_size, "browser"); wasm_copy_string(release, release_size, "0"); wasm_copy_string(version, version_size, "experiment"); wasm_copy_string(machine, machine_size, "wasm32"); return 0; }

int platform_netcat(const char *host, unsigned int port, const PlatformNetcatOptions *options) { (void)host; (void)port; (void)options; return -1; }
int platform_netcat_tcp(const char *host, unsigned int port, int listen_mode) { (void)host; (void)port; (void)listen_mode; return -1; }
int platform_dns_lookup(const char *server, unsigned int port, const char *name, int family_filter, PlatformDnsEntry *entries_out, size_t entry_capacity, size_t *count_out) { (void)server; (void)port; (void)name; (void)family_filter; (void)entries_out; (void)entry_capacity; if (count_out) *count_out = 0U; return -1; }
int platform_dns_query(const char *server, unsigned int port, const char *name, unsigned short record_type, PlatformDnsEntry *entries_out, size_t entry_capacity, size_t *count_out) { (void)record_type; return platform_dns_lookup(server, port, name, 0, entries_out, entry_capacity, count_out); }
int platform_dhcp_request(const char *ifname, const char *server, unsigned int server_port, unsigned int client_port, unsigned int timeout_milliseconds, PlatformDhcpLease *lease_out) { (void)ifname; (void)server; (void)server_port; (void)client_port; (void)timeout_milliseconds; (void)lease_out; return -1; }
int platform_list_sockets(PlatformSocketEntry *entries_out, size_t entry_capacity, size_t *count_out, int include_tcp, int include_udp, int listening_only) { (void)entries_out; (void)entry_capacity; (void)include_tcp; (void)include_udp; (void)listening_only; if (count_out) *count_out = 0U; return 0; }
int platform_list_network_links(PlatformNetworkLink *entries_out, size_t entry_capacity, size_t *count_out) { (void)entries_out; (void)entry_capacity; if (count_out) *count_out = 0U; return 0; }
int platform_list_network_addresses(PlatformNetworkAddress *entries_out, size_t entry_capacity, size_t *count_out, int family_filter, const char *ifname_filter) { (void)entries_out; (void)entry_capacity; (void)family_filter; (void)ifname_filter; if (count_out) *count_out = 0U; return 0; }
int platform_list_network_routes(PlatformRouteEntry *entries_out, size_t entry_capacity, size_t *count_out, int family_filter, const char *ifname_filter) { (void)entries_out; (void)entry_capacity; (void)family_filter; (void)ifname_filter; if (count_out) *count_out = 0U; return 0; }
int platform_network_link_set(const char *ifname, int want_up, unsigned int mtu_value, int set_mtu) { (void)ifname; (void)want_up; (void)mtu_value; (void)set_mtu; return -1; }
int platform_network_address_change(const char *ifname, const char *cidr, int add) { (void)ifname; (void)cidr; (void)add; return -1; }
int platform_network_route_change(const char *destination, const char *gateway, const char *ifname, int add) { (void)destination; (void)gateway; (void)ifname; (void)add; return -1; }
int platform_ping_host(const char *host, const PlatformPingOptions *options) { (void)host; (void)options; return -1; }
int platform_trace_route(const char *host, const PlatformTracerouteOptions *options, PlatformTracerouteHop *hops_out, size_t hop_capacity, size_t *hop_count_out) { (void)host; (void)options; (void)hops_out; (void)hop_capacity; if (hop_count_out) *hop_count_out = 0U; return -1; }

int platform_usb_list_devices(PlatformUsbDevice *entries_out, size_t entry_capacity, size_t *count_out) { (void)entries_out; (void)entry_capacity; if (count_out) *count_out = 0U; return 0; }
int platform_usb_open(const PlatformUsbDevice *device, PlatformUsbHandle *handle_out) { (void)device; (void)handle_out; return -1; }
int platform_usb_close(PlatformUsbHandle *handle) { (void)handle; return -1; }
int platform_usb_claim_interface(PlatformUsbHandle *handle, unsigned int interface_number) { (void)handle; (void)interface_number; return -1; }
int platform_usb_release_interface(PlatformUsbHandle *handle, unsigned int interface_number) { (void)handle; (void)interface_number; return -1; }
int platform_usb_control_transfer(PlatformUsbHandle *handle, unsigned int request_type, unsigned int request, unsigned int value, unsigned int index, unsigned char *data, size_t length, unsigned int timeout_milliseconds, size_t *transferred_out) { (void)handle; (void)request_type; (void)request; (void)value; (void)index; (void)data; (void)length; (void)timeout_milliseconds; if (transferred_out) *transferred_out = 0U; return -1; }
int platform_usb_bulk_transfer(PlatformUsbHandle *handle, unsigned int endpoint, unsigned char *data, size_t length, unsigned int timeout_milliseconds, size_t *transferred_out) { (void)handle; (void)endpoint; (void)data; (void)length; (void)timeout_milliseconds; if (transferred_out) *transferred_out = 0U; return -1; }
int platform_usb_read_configuration_descriptor(PlatformUsbHandle *handle, unsigned int configuration_index, unsigned char *buffer, size_t buffer_size, size_t *length_out) { (void)handle; (void)configuration_index; (void)buffer; (void)buffer_size; if (length_out) *length_out = 0U; return -1; }
int platform_create_node(const char *path, unsigned int node_type, unsigned int mode, unsigned int major, unsigned int minor) { (void)path; (void)node_type; (void)mode; (void)major; (void)minor; return -1; }
int platform_mount_filesystem(const char *source, const char *target, const char *filesystem_type, unsigned long long flags, const char *data) { (void)source; (void)target; (void)filesystem_type; (void)flags; (void)data; return -1; }
int platform_unmount_filesystem(const char *target, int force, int lazy) { (void)target; (void)force; (void)lazy; return -1; }
long platform_read_kernel_log(char *buffer, size_t buffer_size, int clear_after_read) { (void)buffer; (void)buffer_size; (void)clear_after_read; return -1; }
int platform_open_kernel_log_stream(void) { return -1; }
int platform_open_kernel_log_writer(void) { return -1; }
int platform_clear_kernel_log(void) { return -1; }
int platform_set_console_log_level(int level) { (void)level; return -1; }
int platform_terminal_get_mode(int fd, PlatformTerminalMode *mode_out) { (void)fd; rt_memset(mode_out, 0, sizeof(*mode_out)); return 0; }
int platform_terminal_set_mode(int fd, const PlatformTerminalMode *mode, unsigned int change_mask) { (void)fd; (void)mode; (void)change_mask; return 0; }
int platform_terminal_enable_raw_mode(int fd, PlatformTerminalState *state_out) { (void)fd; rt_memset(state_out, 0, sizeof(*state_out)); return 0; }
int platform_terminal_restore_mode(int fd, const PlatformTerminalState *state) { (void)fd; (void)state; return 0; }

void platform_format_mode(unsigned int mode, char out[11]) {
    const char type = (mode & 0040000U) == 0040000U ? 'd' : '-';
    unsigned int bits = mode & 0777U;
    out[0] = type;
    out[1] = (bits & 0400U) ? 'r' : '-';
    out[2] = (bits & 0200U) ? 'w' : '-';
    out[3] = (bits & 0100U) ? 'x' : '-';
    out[4] = (bits & 0040U) ? 'r' : '-';
    out[5] = (bits & 0020U) ? 'w' : '-';
    out[6] = (bits & 0010U) ? 'x' : '-';
    out[7] = (bits & 0004U) ? 'r' : '-';
    out[8] = (bits & 0002U) ? 'w' : '-';
    out[9] = (bits & 0001U) ? 'x' : '-';
    out[10] = '\0';
}
