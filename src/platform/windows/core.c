#include "platform.h"
#include "runtime.h"

#define WIN_STD_INPUT_HANDLE  ((unsigned long)-10L)
#define WIN_STD_OUTPUT_HANDLE ((unsigned long)-11L)
#define WIN_STD_ERROR_HANDLE  ((unsigned long)-12L)
#define WIN_INVALID_HANDLE_VALUE ((void *)(long long)-1)
#define WIN_GENERIC_READ 0x80000000UL
#define WIN_GENERIC_WRITE 0x40000000UL
#define WIN_FILE_SHARE_READ 0x00000001UL
#define WIN_FILE_SHARE_WRITE 0x00000002UL
#define WIN_CREATE_ALWAYS 2UL
#define WIN_OPEN_ALWAYS 4UL
#define WIN_OPEN_EXISTING 3UL
#define WIN_FILE_ATTRIBUTE_NORMAL 0x00000080UL
#define WIN_FILE_ATTRIBUTE_DIRECTORY 0x00000010UL
#define WIN_INVALID_FILE_ATTRIBUTES 0xffffffffUL
#define WIN_FD_TABLE_CAPACITY 64
#define WIN_FD_KIND_NONE 0
#define WIN_FD_KIND_FILE 1
#define WIN_FD_KIND_SOCKET 2
#define WIN_AF_UNSPEC 0
#define WIN_SOCK_STREAM 1
#define WIN_IPPROTO_TCP 6
#define WIN_INVALID_SOCKET (~(unsigned long long)0ULL)
#define WIN_SOCKET_ERROR (-1)
#define WIN_FD_SETSIZE 64

typedef unsigned long long WinSocket;

typedef struct WinSockAddr WinSockAddr;

typedef struct WinAddrInfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    size_t ai_addrlen;
    char *ai_canonname;
    WinSockAddr *ai_addr;
    struct WinAddrInfo *ai_next;
} WinAddrInfo;

typedef struct {
    unsigned int fd_count;
    WinSocket fd_array[WIN_FD_SETSIZE];
} WinFdSet;

typedef struct {
    long tv_sec;
    long tv_usec;
} WinTimeval;

__declspec(dllimport) void __stdcall ExitProcess(unsigned int status);
__declspec(dllimport) char *__stdcall GetCommandLineA(void);
__declspec(dllimport) void *__stdcall GetStdHandle(unsigned long handle_id);
__declspec(dllimport) unsigned long __stdcall GetCurrentDirectoryA(unsigned long buffer_length, char *buffer);
__declspec(dllimport) int __stdcall GetComputerNameA(char *buffer, unsigned long *size_io);
__declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableA(const char *name, char *buffer, unsigned long size);
__declspec(dllimport) unsigned long __stdcall GetFileAttributesA(const char *file_name);
__declspec(dllimport) int __stdcall GetFileSizeEx(void *handle, long long *size_out);
__declspec(dllimport) int __stdcall SetFilePointerEx(void *handle, long long distance_to_move,
                                                     long long *new_file_pointer, unsigned long move_method);
__declspec(dllimport) int __stdcall SetEndOfFile(void *handle);
__declspec(dllimport) void __stdcall Sleep(unsigned long milliseconds);
__declspec(dllimport) void *__stdcall CreateFileA(const char *file_name, unsigned long desired_access,
                                                   unsigned long share_mode, void *security_attributes,
                                                   unsigned long creation_disposition,
                                                   unsigned long flags_and_attributes, void *template_file);
__declspec(dllimport) int __stdcall CreateDirectoryA(const char *path_name, void *security_attributes);
__declspec(dllimport) int __stdcall DeleteFileA(const char *file_name);
__declspec(dllimport) int __stdcall FlushFileBuffers(void *handle);
__declspec(dllimport) int __stdcall RemoveDirectoryA(const char *path_name);
__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, size_t size, unsigned long allocation_type, unsigned long protect);
__declspec(dllimport) int __stdcall CloseHandle(void *handle);
__declspec(dllimport) int __stdcall ReadFile(void *handle, void *buffer, unsigned long count, unsigned long *read_out, void *overlapped);
__declspec(dllimport) int __stdcall WriteFile(void *handle, const void *buffer, unsigned long count, unsigned long *written_out, void *overlapped);
__declspec(dllimport) int __stdcall WSAStartup(unsigned short version_requested, void *wsa_data);
__declspec(dllimport) int __stdcall getaddrinfo(const char *node_name, const char *service_name, const WinAddrInfo *hints, WinAddrInfo **result);
__declspec(dllimport) void __stdcall freeaddrinfo(WinAddrInfo *ai);
__declspec(dllimport) WinSocket __stdcall socket(int address_family, int type, int protocol);
__declspec(dllimport) int __stdcall connect(WinSocket s, const WinSockAddr *name, int name_length);
__declspec(dllimport) int __stdcall closesocket(WinSocket s);
__declspec(dllimport) int __stdcall recv(WinSocket s, char *buffer, int length, int flags);
__declspec(dllimport) int __stdcall send(WinSocket s, const char *buffer, int length, int flags);
__declspec(dllimport) int __stdcall select(int nfds, WinFdSet *readfds, WinFdSet *writefds, WinFdSet *exceptfds, WinTimeval *timeout);

unsigned long __stack_chk_guard;

void __main(void) {
}

__attribute__((naked))
void ___chkstk_ms(void) {
    __asm__(
        "pushq %rcx\n"
        "pushq %r10\n"
        "movq %rax, %r10\n"
        "leaq 24(%rsp), %rcx\n"
        "cmpq $0x1000, %rax\n"
        "jb 2f\n"
        "1:\n"
        "subq $0x1000, %rcx\n"
        "testq %rax, (%rcx)\n"
        "subq $0x1000, %rax\n"
        "cmpq $0x1000, %rax\n"
        "jae 1b\n"
        "2:\n"
        "subq %rax, %rcx\n"
        "testq %rax, (%rcx)\n"
        "movq %r10, %rax\n"
        "popq %r10\n"
        "popq %rcx\n"
        "ret\n"
    );
}

static char windows_command_line[4096];
static char *windows_argv[64];
static void *windows_fd_table[WIN_FD_TABLE_CAPACITY];
static unsigned char windows_fd_kind[WIN_FD_TABLE_CAPACITY];
static int windows_winsock_started;
static char windows_env_buffer[4096];

static int windows_copy_string(char *buffer, size_t buffer_size, const char *text) {
    if (buffer == 0 || buffer_size == 0U || text == 0 || rt_strlen(text) + 1U > buffer_size) return -1;
    rt_copy_string(buffer, buffer_size, text);
    return 0;
}

static int windows_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int windows_parse_command_line(char *text, char **argv, int argv_capacity) {
    int argc = 0;
    char *cursor = text;

    while (*cursor != '\0' && argc + 1 < argv_capacity) {
        char *out;
        int in_quotes = 0;

        while (windows_is_space(*cursor)) cursor += 1;
        if (*cursor == '\0') break;

        argv[argc] = cursor;
        out = cursor;

        while (*cursor != '\0') {
            if (*cursor == '"') {
                in_quotes = !in_quotes;
                cursor += 1;
                continue;
            }
            if (!in_quotes && windows_is_space(*cursor)) {
                cursor += 1;
                break;
            }
            *out = *cursor;
            out += 1;
            cursor += 1;
        }

        *out = '\0';
        argc += 1;
    }

    argv[argc] = 0;
    return argc;
}

static int windows_startup_args(char ***argv_out) {
    char *command_line = GetCommandLineA();
    size_t length = rt_strlen(command_line);

    if (length >= sizeof(windows_command_line)) {
        length = sizeof(windows_command_line) - 1U;
    }
    memcpy(windows_command_line, command_line, length);
    windows_command_line[length] = '\0';

    *argv_out = windows_argv;
    return windows_parse_command_line(windows_command_line, windows_argv, (int)(sizeof(windows_argv) / sizeof(windows_argv[0])));
}

static void *windows_handle_for_fd(int fd) {
    if (fd == 0) return GetStdHandle(WIN_STD_INPUT_HANDLE);
    if (fd == 1) return GetStdHandle(WIN_STD_OUTPUT_HANDLE);
    if (fd == 2) return GetStdHandle(WIN_STD_ERROR_HANDLE);
    if (fd >= 0 && fd < WIN_FD_TABLE_CAPACITY && windows_fd_kind[fd] == WIN_FD_KIND_FILE) return windows_fd_table[fd];
    return 0;
}

static int windows_fd_is_socket(int fd) {
    return fd >= 0 && fd < WIN_FD_TABLE_CAPACITY && windows_fd_kind[fd] == WIN_FD_KIND_SOCKET;
}

static WinSocket windows_socket_for_fd(int fd) {
    return windows_fd_is_socket(fd) ? (WinSocket)(size_t)windows_fd_table[fd] : WIN_INVALID_SOCKET;
}

static int windows_allocate_fd(void *value, int kind) {
    int fd;
    for (fd = 3; fd < WIN_FD_TABLE_CAPACITY; ++fd) {
        if (windows_fd_kind[fd] == WIN_FD_KIND_NONE) {
            windows_fd_table[fd] = value;
            windows_fd_kind[fd] = (unsigned char)kind;
            return fd;
        }
    }
    return -1;
}

static int windows_winsock_start(void) {
    unsigned char wsa_data[512];
    if (windows_winsock_started) return 0;
    if (WSAStartup(0x0202U, wsa_data) != 0) return -1;
    windows_winsock_started = 1;
    return 0;
}

void *platform_allocate_pages(size_t size) {
    return VirtualAlloc(0, size, 0x3000UL, 0x04UL);
}

long platform_write(int fd, const void *buffer, size_t count) {
    void *handle = windows_handle_for_fd(fd);
    unsigned long chunk;
    unsigned long written = 0;

    if (windows_fd_is_socket(fd)) {
        int socket_chunk;
        int result;
        if (buffer == 0) return -1;
        socket_chunk = count > 0x7fffffffU ? 0x7fffffff : (int)count;
        result = send(windows_socket_for_fd(fd), (const char *)buffer, socket_chunk, 0);
        return result == WIN_SOCKET_ERROR ? -1 : (long)result;
    }

    if (handle == 0) return -1;
    chunk = count > 0xffffffffUL ? 0xffffffffUL : (unsigned long)count;
    if (!WriteFile(handle, buffer, chunk, &written, 0)) return -1;
    return (long)written;
}

long platform_read(int fd, void *buffer, size_t count) {
    void *handle = windows_handle_for_fd(fd);
    unsigned long chunk;
    unsigned long bytes_read = 0;

    if (windows_fd_is_socket(fd)) {
        int socket_chunk;
        int result;
        if (buffer == 0) return -1;
        socket_chunk = count > 0x7fffffffU ? 0x7fffffff : (int)count;
        result = recv(windows_socket_for_fd(fd), (char *)buffer, socket_chunk, 0);
        return result == WIN_SOCKET_ERROR ? -1 : (long)result;
    }

    if (handle == 0) return -1;
    chunk = count > 0xffffffffUL ? 0xffffffffUL : (unsigned long)count;
    if (!ReadFile(handle, buffer, chunk, &bytes_read, 0)) return -1;
    return (long)bytes_read;
}

int platform_open_read(const char *path) {
    void *handle;
    int fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) return 0;

    handle = CreateFileA(path, WIN_GENERIC_READ, WIN_FILE_SHARE_READ, 0,
                         WIN_OPEN_EXISTING, WIN_FILE_ATTRIBUTE_NORMAL, 0);
    if (handle == 0 || handle == WIN_INVALID_HANDLE_VALUE) return -1;

    fd = windows_allocate_fd(handle, WIN_FD_KIND_FILE);
    if (fd >= 0) return fd;

    (void)CloseHandle(handle);
    return -1;
}

int platform_open_write_mode(const char *path, unsigned int mode, int truncate_existing) {
    void *handle;
    unsigned long creation_disposition;
    int fd;

    (void)mode;
    if (path == 0 || (path[0] == '-' && path[1] == '\0')) return 1;

    creation_disposition = truncate_existing ? WIN_CREATE_ALWAYS : WIN_OPEN_ALWAYS;
    handle = CreateFileA(path, WIN_GENERIC_WRITE, WIN_FILE_SHARE_READ | WIN_FILE_SHARE_WRITE, 0,
                         creation_disposition, WIN_FILE_ATTRIBUTE_NORMAL, 0);
    if (handle == 0 || handle == WIN_INVALID_HANDLE_VALUE) return -1;

    fd = windows_allocate_fd(handle, WIN_FD_KIND_FILE);
    if (fd >= 0) return fd;

    (void)CloseHandle(handle);
    return -1;
}

int platform_open_write(const char *path, unsigned int mode) {
    return platform_open_write_mode(path, mode, 1);
}

int platform_open_append(const char *path, unsigned int mode) {
    int fd = platform_open_write_mode(path, mode, 0);
    if (fd >= 0 && platform_seek(fd, 0, PLATFORM_SEEK_END) < 0) {
        (void)platform_close(fd);
        return -1;
    }
    return fd;
}

int platform_open_append_existing(const char *path) {
    void *handle;
    int fd;

    if (path == 0 || (path[0] == '-' && path[1] == '\0')) return 1;

    handle = CreateFileA(path, WIN_GENERIC_WRITE, WIN_FILE_SHARE_READ | WIN_FILE_SHARE_WRITE, 0,
                         WIN_OPEN_EXISTING, WIN_FILE_ATTRIBUTE_NORMAL, 0);
    if (handle == 0 || handle == WIN_INVALID_HANDLE_VALUE) return -1;

    fd = windows_allocate_fd(handle, WIN_FD_KIND_FILE);
    if (fd < 0 || platform_seek(fd, 0, PLATFORM_SEEK_END) < 0) {
        if (fd >= 0) (void)platform_close(fd);
        else (void)CloseHandle(handle);
        return -1;
    }
    return fd;
}

int platform_close(int fd) {
    void *handle;

    if (fd >= 0 && fd <= 2) return 0;
    if (fd < 0 || fd >= WIN_FD_TABLE_CAPACITY || windows_fd_kind[fd] == WIN_FD_KIND_NONE) return -1;

    handle = windows_fd_table[fd];
    windows_fd_table[fd] = 0;
    if (windows_fd_kind[fd] == WIN_FD_KIND_SOCKET) {
        windows_fd_kind[fd] = WIN_FD_KIND_NONE;
        return closesocket((WinSocket)(size_t)handle) == 0 ? 0 : -1;
    }
    windows_fd_kind[fd] = WIN_FD_KIND_NONE;
    return CloseHandle(handle) != 0 ? 0 : -1;
}

long long platform_seek(int fd, long long offset, int whence) {
    void *handle = windows_handle_for_fd(fd);
    unsigned long method;
    long long new_position = 0;

    if (handle == 0) return -1;
    if (whence == PLATFORM_SEEK_SET) {
        method = 0UL;
    } else if (whence == PLATFORM_SEEK_CUR) {
        method = 1UL;
    } else if (whence == PLATFORM_SEEK_END) {
        method = 2UL;
    } else {
        return -1;
    }

    if (!SetFilePointerEx(handle, offset, &new_position, method)) return -1;
    return new_position;
}

const char *platform_getenv(const char *name) {
    unsigned long length;

    if (name == 0 || name[0] == '\0') return 0;
    length = GetEnvironmentVariableA(name, windows_env_buffer, (unsigned long)sizeof(windows_env_buffer));
    if (length == 0UL || length >= (unsigned long)sizeof(windows_env_buffer)) return 0;
    return windows_env_buffer;
}

const char *platform_getenv_entry(size_t index) {
    (void)index;
    return 0;
}

int platform_isatty(int fd) {
    (void)fd;
    return 0;
}

int platform_parse_signal_name(const char *text, int *signal_out) {
    (void)text;
    (void)signal_out;
    return -1;
}

const char *platform_signal_name(int signal_number) {
    (void)signal_number;
    return 0;
}

void platform_write_signal_list(int fd) {
    (void)fd;
}

int platform_ignore_signal(int signal_number) {
    (void)signal_number;
    return 0;
}

int platform_poll_fds(const int *fds, size_t fd_count, size_t *ready_index_out, int timeout_milliseconds) {
    WinFdSet readfds;
    WinTimeval timeout;
    WinTimeval *timeout_ptr = &timeout;
    size_t indexes[WIN_FD_SETSIZE];
    size_t i;
    int result;

    if (fds == 0 || fd_count == 0U || ready_index_out == 0) return -1;
    if (windows_winsock_start() != 0) return -1;

    readfds.fd_count = 0U;
    for (i = 0U; i < fd_count && readfds.fd_count < WIN_FD_SETSIZE; ++i) {
        if (windows_fd_is_socket(fds[i])) {
            indexes[readfds.fd_count] = i;
            readfds.fd_array[readfds.fd_count] = windows_socket_for_fd(fds[i]);
            readfds.fd_count += 1U;
        } else if (fds[i] >= 0) {
            *ready_index_out = i;
            return 1;
        }
    }
    if (readfds.fd_count == 0U) return -1;

    if (timeout_milliseconds < 0) {
        timeout_ptr = 0;
    } else {
        timeout.tv_sec = timeout_milliseconds / 1000;
        timeout.tv_usec = (long)(timeout_milliseconds % 1000) * 1000L;
    }

    result = select(0, &readfds, 0, 0, timeout_ptr);
    if (result <= 0) return result;
    if (readfds.fd_count > 0U) {
        WinSocket ready_socket = readfds.fd_array[0];
        for (i = 0U; i < fd_count; ++i) {
            if (windows_fd_is_socket(fds[i]) && windows_socket_for_fd(fds[i]) == ready_socket) {
                *ready_index_out = i;
                return 1;
            }
        }
        *ready_index_out = indexes[0];
        return 1;
    }
    return 0;
}

int platform_connect_tcp(const char *host, unsigned int port, int *socket_fd_out) {
    WinAddrInfo hints;
    WinAddrInfo *results = 0;
    WinAddrInfo *current;
    WinSocket sock = WIN_INVALID_SOCKET;
    char port_text[16];
    int fd;

    if (host == 0 || host[0] == '\0' || socket_fd_out == 0 || port == 0U || port > 65535U) return -1;
    if (windows_winsock_start() != 0) return -1;

    rt_memset(&hints, 0, sizeof(hints));
    hints.ai_family = WIN_AF_UNSPEC;
    hints.ai_socktype = WIN_SOCK_STREAM;
    hints.ai_protocol = WIN_IPPROTO_TCP;
    rt_unsigned_to_string(port, port_text, sizeof(port_text));

    if (getaddrinfo(host, port_text, &hints, &results) != 0) return -1;
    for (current = results; current != 0; current = current->ai_next) {
        sock = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (sock == WIN_INVALID_SOCKET) continue;
        if (connect(sock, current->ai_addr, (int)current->ai_addrlen) == 0) break;
        (void)closesocket(sock);
        sock = WIN_INVALID_SOCKET;
    }

    freeaddrinfo(results);
    if (sock == WIN_INVALID_SOCKET) return -1;
    fd = windows_allocate_fd((void *)(size_t)sock, WIN_FD_KIND_SOCKET);
    if (fd < 0) {
        (void)closesocket(sock);
        return -1;
    }

    *socket_fd_out = fd;
    return 0;
}

int platform_sleep_milliseconds(unsigned long long milliseconds) {
    while (milliseconds > 0xffffffffULL) {
        Sleep(0xffffffffUL);
        milliseconds -= 0xffffffffULL;
    }
    Sleep((unsigned long)milliseconds);
    return 0;
}

int platform_get_current_directory(char *buffer, size_t buffer_size) {
    unsigned long length;

    if (buffer == 0 || buffer_size == 0U) return -1;
    if (buffer_size > 0xffffffffUL) buffer_size = 0xffffffffUL;

    length = GetCurrentDirectoryA((unsigned long)buffer_size, buffer);
    if (length == 0UL || length >= (unsigned long)buffer_size) return -1;
    return 0;
}

int platform_get_hostname(char *buffer, size_t buffer_size) {
    unsigned long length;

    if (buffer == 0 || buffer_size == 0U) return -1;
    if (buffer_size > 0xffffffffUL) buffer_size = 0xffffffffUL;

    length = (unsigned long)buffer_size;
    if (!GetComputerNameA(buffer, &length)) return -1;
    if (length >= (unsigned long)buffer_size) return -1;
    buffer[length] = '\0';
    return 0;
}

int platform_set_hostname(const char *name) {
    (void)name;
    return -1;
}

int platform_path_access(const char *path, int mode) {
    unsigned long attributes;

    (void)mode;
    if (path == 0 || path[0] == '\0') return -1;
    attributes = GetFileAttributesA(path);
    return attributes == WIN_INVALID_FILE_ATTRIBUTES ? -1 : 0;
}

int platform_make_directory(const char *path, unsigned int mode) {
    (void)mode;
    if (path == 0 || path[0] == '\0') return -1;
    return CreateDirectoryA(path, 0) != 0 ? 0 : -1;
}

int platform_remove_file(const char *path) {
    if (path == 0 || path[0] == '\0') return -1;
    return DeleteFileA(path) != 0 ? 0 : -1;
}

int platform_remove_directory(const char *path) {
    if (path == 0 || path[0] == '\0') return -1;
    return RemoveDirectoryA(path) != 0 ? 0 : -1;
}

int platform_truncate_path(const char *path, unsigned long long size) {
    int fd;
    void *handle;
    int ok;

    fd = platform_open_append_existing(path);
    if (fd < 0) return -1;
    handle = windows_handle_for_fd(fd);
    ok = handle != 0 && platform_seek(fd, (long long)size, PLATFORM_SEEK_SET) >= 0 && SetEndOfFile(handle) != 0;
    (void)platform_close(fd);
    return ok ? 0 : -1;
}

int platform_sync_all(void) {
    return 0;
}

int platform_sync_path(const char *path) {
    int fd;
    void *handle;
    int ok;

    fd = platform_open_append_existing(path);
    if (fd < 0) return -1;
    handle = windows_handle_for_fd(fd);
    ok = handle != 0 && FlushFileBuffers(handle) != 0;
    (void)platform_close(fd);
    return ok ? 0 : -1;
}

int platform_sync_path_data(const char *path) {
    return platform_sync_path(path);
}

int platform_get_path_info(const char *path, PlatformDirEntry *entry_out) {
    unsigned long attributes;
    void *handle;
    long long file_size = 0;
    int is_dir;

    if (path == 0 || path[0] == '\0' || entry_out == 0) return -1;
    attributes = GetFileAttributesA(path);
    if (attributes == WIN_INVALID_FILE_ATTRIBUTES) return -1;

    rt_memset(entry_out, 0, sizeof(*entry_out));
    is_dir = (attributes & WIN_FILE_ATTRIBUTE_DIRECTORY) != 0UL;
    entry_out->is_dir = is_dir;
    entry_out->mode = is_dir ? 0040755U : 0100644U;
    entry_out->nlink = 1UL;

    if (!is_dir) {
        handle = CreateFileA(path, WIN_GENERIC_READ, WIN_FILE_SHARE_READ | WIN_FILE_SHARE_WRITE, 0,
                             WIN_OPEN_EXISTING, WIN_FILE_ATTRIBUTE_NORMAL, 0);
        if (handle != 0 && handle != WIN_INVALID_HANDLE_VALUE) {
            if (GetFileSizeEx(handle, &file_size) != 0 && file_size >= 0) {
                entry_out->size = (unsigned long long)file_size;
            }
            (void)CloseHandle(handle);
        }
    }
    return 0;
}

int platform_get_path_info_follow(const char *path, PlatformDirEntry *entry_out) {
    return platform_get_path_info(path, entry_out);
}

int platform_path_is_directory(const char *path, int *is_directory_out) {
    unsigned long attributes;

    if (path == 0 || path[0] == '\0' || is_directory_out == 0) return -1;
    attributes = GetFileAttributesA(path);
    if (attributes == WIN_INVALID_FILE_ATTRIBUTES) return -1;
    *is_directory_out = (attributes & WIN_FILE_ATTRIBUTE_DIRECTORY) != 0UL;
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
    (void)path;
    (void)include_hidden;
    (void)entries_out;
    (void)entry_capacity;
    if (count_out != 0) *count_out = 0;
    if (path_is_directory != 0) *path_is_directory = 0;
    return -1;
}

void platform_free_entries(PlatformDirEntry *entries, size_t count) {
    (void)entries;
    (void)count;
}

int platform_read_symlink(const char *path, char *buffer, size_t buffer_size) {
    (void)path;
    (void)buffer;
    (void)buffer_size;
    return -1;
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
    if (windows_copy_string(sysname, sysname_size, "Windows") != 0) return -1;
    if (platform_get_hostname(nodename, nodename_size) != 0) return -1;
    if (windows_copy_string(release, release_size, "NT") != 0) return -1;
    if (windows_copy_string(version, version_size, "freestanding") != 0) return -1;
    if (windows_copy_string(machine, machine_size, "x86_64") != 0) return -1;
    return 0;
}

__attribute__((noreturn, no_stack_protector))
void __stack_chk_fail(void) {
    static const char message[] = "newos: stack check failure\r\n";

    (void)platform_write(2, message, sizeof(message) - 1U);
    ExitProcess(127);
    for (;;) {
    }
}

__attribute__((no_stack_protector))
void __newos_stack_guard_init(long argc, char **argv) {
    unsigned long guard = 0xf00dfeedUL;

    guard ^= (unsigned long)(size_t)&guard;
    guard ^= (unsigned long)(size_t)argv;
    guard ^= (unsigned long)argc * 0x9e3779b97f4a7c15UL;
    guard &= ~0xffUL;
    __stack_chk_guard = guard == 0 ? 0xf00dfeedUL : guard;
}

int main(int argc, char **argv);

__attribute__((noreturn, no_stack_protector))
void mainCRTStartup(void) {
    char **argv;
    int argc = windows_startup_args(&argv);
    int status;

    __newos_stack_guard_init(argc, argv);
    status = main(argc, argv);
    ExitProcess((unsigned int)status);
    for (;;) {
    }
}