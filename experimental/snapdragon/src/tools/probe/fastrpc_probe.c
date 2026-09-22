typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef union RemoteArg {
    struct { void *data; u64 size; } buffer;
    u32 handle;
    u64 handle64;
    struct { int fd; u32 offset; } dma;
} RemoteArg;
typedef int (*RemoteControl)(u32, void *, u32);
typedef int (*RemoteOpen)(const char *, u64 *);
typedef int (*RemoteInvoke)(u64, u32, RemoteArg *);
typedef int (*RemoteClose)(u64);
typedef void *(*RpcAlloc)(int, u32, int);
typedef void (*RpcFree)(void *);
typedef int (*RpcFd)(void *);

__declspec(dllimport) void ExitProcess(u32);
__declspec(dllimport) void *GetStdHandle(u32);
__declspec(dllimport) int WriteFile(void *, const void *, u32, u32 *, void *);
__declspec(dllimport) const u16 *GetCommandLineW(void);
__declspec(dllimport) void *LoadLibraryExW(const u16 *, void *, u32);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
__declspec(dllimport) u32 GetModuleFileNameW(void *, u16 *, u32);
__declspec(dllimport) int WideCharToMultiByte(u32, u32, const u16 *, int, char *, int, const char *, int *);
__declspec(dllimport) u32 GetLastError(void);
__declspec(dllimport) int FreeLibrary(void *);

typedef struct DspCapability {
    u32 domain;
    u32 attribute;
    u32 value;
} DspCapability;

_Static_assert(sizeof(void *) == 8, "Windows ARM64 host required");
_Static_assert(sizeof(DspCapability) == 12, "FastRPC capability ABI");
_Static_assert(sizeof(RemoteArg) == 16, "Windows ARM64 remote_arg ABI");

static void *output;
static u16 dll_path[2048];
static u16 module_path[2048];
static char utf8_path[8192];

static void text(const char *value) {
    u32 length = 0;
    while (value[length]) ++length;
    while (length) {
        u32 written = 0;
        if (!WriteFile(output, value, length, &written, 0) || !written) ExitProcess(90);
        value += written;
        length -= written;
    }
}

static void number(u32 value) {
    char buffer[11];
    u32 cursor = sizeof(buffer) - 1;
    buffer[cursor] = 0;
    do {
        buffer[--cursor] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    text(buffer + cursor);
}

static void field(const char *name, u32 value) {
    text(name);
    text("=");
    number(value);
    text("\n");
}

static int argument(const u16 **cursor, u16 *destination, u32 capacity) {
    u32 count = 0;
    int quoted = 0;
    while (**cursor == ' ' || **cursor == '\t') ++*cursor;
    if (!**cursor) return 0;
    while (**cursor) {
        u16 value = *(*cursor)++;
        if (value == '"') quoted = !quoted;
        else if (!quoted && (value == ' ' || value == '\t')) break;
        else {
            if (count + 1 >= capacity) return -1;
            destination[count++] = value;
        }
    }
    if (quoted) return -1;
    destination[count] = 0;
    return count ? 1 : -1;
}

static int is_invoke(const u16 *value) {
    const char *expected = "--invoke";
    while (*expected && *value == (u16)*expected) { ++value; ++expected; }
    return !*expected && !*value;
}

static int invoke_scalar(void *module, RpcAlloc allocate, RpcFree release) {
    RemoteControl session = (RemoteControl)GetProcAddress(module, "remote_session_control");
    RemoteOpen open = (RemoteOpen)GetProcAddress(module, "remote_handle64_open");
    RemoteInvoke invoke = (RemoteInvoke)GetProcAddress(module, "remote_handle64_invoke");
    RemoteClose close = (RemoteClose)GetProcAddress(module, "remote_handle64_close");
    struct { int domain; int enable; } unsigned_request = {3, 1};
    struct { int domain; } close_request = {3};
    RemoteArg arguments[2];
    u64 handle = 0;
    u32 *buffer = 0;
    int opened = 0;
    int result = 0;
    int status;
    if (!session || !open || !invoke || !close) return 8;
    status = session(2, &unsigned_request, sizeof(unsigned_request));
    field("unsigned_session.status", (u32)status);
    if (status) return 8;
    status = open("file:///fastrpc_probe_skel.so?fastrpc_probe_skel_invoke&_modver=1.0&_dom=cdsp", &handle);
    field("remote_open.status", (u32)status);
    if (status) { result = 9; goto cleanup; }
    opened = 1;
    buffer = (u32 *)allocate(25, 1, 4096);
    if (!buffer) { result = 6; goto cleanup; }
    arguments[0].buffer.data = buffer;
    arguments[0].buffer.size = 64;
    arguments[1].buffer.data = buffer + 32;
    arguments[1].buffer.size = 68;
    for (u32 trial = 0; trial < 3; ++trial) {
        u32 mismatches = 0;
        for (u32 index = 0; index < 16; ++index) {
            buffer[index] = trial * 65537U + index;
            buffer[32 + index] = ~((buffer[index] * 1664525U + 1013904223U) ^ (0x9e3779b9U + index));
        }
        buffer[48] = ~0x46525043U;
        field("remote_invoke.trial", trial);
        status = invoke(handle, 0x02010100U, arguments);
        field("remote_invoke.status", (u32)status);
        if (status) { result = 10; break; }
        for (u32 index = 0; index < 16; ++index) {
            u32 expected = ((trial * 65537U + index) * 1664525U + 1013904223U) ^ (0x9e3779b9U + index);
            mismatches += buffer[32 + index] != expected;
        }
        mismatches += buffer[48] != 0x46525043U;
        field("remote_invoke.mismatches", mismatches);
        if (mismatches) { result = 11; break; }
    }
cleanup:
    if (buffer) release(buffer);
    if (opened) {
        status = close(handle);
        field("remote_close.status", (u32)status);
        if (status && !result) result = 12;
        if (!status) {
            text("session_close=handled_by_last_handle\n");
            return result;
        }
    }
    status = session(7, &close_request, sizeof(close_request));
    field("session_close.status", (u32)status);
    if (status && !result) result = 12;
    return result;
}

static int run(void) {
    static const char *exports[] = {
        "remote_handle64_open", "remote_handle64_invoke", "remote_handle64_close",
        "remote_handle_control", "remote_session_control", "rpcmem_alloc",
        "rpcmem_free", "rpcmem_to_fd", "remote_mmap64", "remote_munmap64"
    };
    static const char *attributes[] = {
        "domain_support", "unsigned_pd_support", "hvx_64b", "hvx_128b",
        "vtcm_page", "vtcm_count", "arch_version", "hmx_depth", "hmx_spatial"
    };
    const u16 *cursor = GetCommandLineW();
    void *module;
    RemoteControl control;
    RpcAlloc allocate;
    RpcFree release;
    RpcFd descriptor;
    u32 path_length;
    int invoke_mode = 0;
    int mode_argument;
    int result = 0;
    if (argument(&cursor, dll_path, 2048) != 1 ||
        argument(&cursor, dll_path, 2048) != 1 ||
        !((dll_path[0] >= 'A' && dll_path[0] <= 'Z') ||
          (dll_path[0] >= 'a' && dll_path[0] <= 'z')) ||
        dll_path[1] != ':' || dll_path[2] != '\\') goto usage;
    mode_argument = argument(&cursor, module_path, 2048);
    if (mode_argument) {
        if (mode_argument != 1 || !is_invoke(module_path) || argument(&cursor, module_path, 2048)) goto usage;
        invoke_mode = 1;
    }
    text("probe=fastrpc-host-v2\nqnn_api_used=0\n");
    module = LoadLibraryExW(dll_path, 0, 0x00000100U | 0x00000800U);
    if (!module) {
        field("load.win32_error", GetLastError());
        return 3;
    }
    path_length = GetModuleFileNameW(module, module_path, 2048);
    if (!path_length || path_length >= 2048 ||
        !WideCharToMultiByte(65001, 0, module_path, -1, utf8_path, sizeof(utf8_path), 0, 0)) {
        result = 4;
        goto cleanup;
    }
    text("module="); text(utf8_path); text("\n");
    for (u32 index = 0; index < sizeof(exports) / sizeof(exports[0]); ++index) {
        text("export.");
        field(exports[index], GetProcAddress(module, exports[index]) != 0);
    }
    control = (RemoteControl)GetProcAddress(module, "remote_handle_control");
    if (control) {
        for (u32 index = 0; index < sizeof(attributes) / sizeof(attributes[0]); ++index) {
            DspCapability capability = {3, index, 0xffffffffU};
            int status = control(2, &capability, sizeof(capability));
            text("cap."); text(attributes[index]); field(".status", (u32)status);
            if (!status) {
                text("cap."); text(attributes[index]); field(".value", capability.value);
            }
        }
    }
    allocate = (RpcAlloc)GetProcAddress(module, "rpcmem_alloc");
    release = (RpcFree)GetProcAddress(module, "rpcmem_free");
    descriptor = (RpcFd)GetProcAddress(module, "rpcmem_to_fd");
    if (allocate && release && descriptor) {
        u8 *buffer = (u8 *)allocate(25, 1, 4096);
        field("memory.allocated", buffer != 0);
        if (buffer) {
            volatile u8 *host_view = buffer;
            int fd = descriptor(buffer);
            u32 mismatch = 0;
            for (u32 index = 0; index < 4096; ++index) host_view[index] = (u8)(index * 17U + 3U);
            for (u32 index = 0; index < 4096; ++index) mismatch += host_view[index] != (u8)(index * 17U + 3U);
            field("memory.fd_valid", fd != -1);
            field("memory.host_mismatches", mismatch);
            release(buffer);
            text("memory.freed=1\n");
            if (fd == -1 || mismatch) result = 6;
        } else result = 6;
    } else result = 5;
    if (invoke_mode && !result) {
        result = invoke_scalar(module, allocate, release);
        text(result ? "custom_dsp_execution=failed\n" : "custom_dsp_execution=verified\n");
    } else text("custom_dsp_execution=not_tested\n");
    text("hmx_execution=not_tested\n");
cleanup:
    if (!FreeLibrary(module)) {
        field("unload.win32_error", GetLastError());
        if (!result) result = 7;
    }
    return result;
usage:
    text("usage: fastrpc_probe.exe \"C:\\absolute\\path\\libcdsprpc.dll\" [--invoke]\n");
    return 2;
}

void mainCRTStartup(void) {
    output = GetStdHandle((u32)-11);
    ExitProcess((u32)run());
}