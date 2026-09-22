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

static int is_option(const u16 *value, const char *expected) {
    while (*expected && *value == (u16)*expected) { ++value; ++expected; }
    return !*expected && !*value;
}

static u16 integer_half(int value) {
    u32 magnitude = (u32)(value < 0 ? -value : value);
    u32 exponent = 0, shifted = magnitude;
    if (!magnitude) return 0;
    while (shifted > 1) { shifted >>= 1; ++exponent; }
    return (u16)((value < 0 ? 0x8000U : 0) | ((exponent + 15) << 10) |
                 (((magnitude << 10) >> exponent) & 1023));
}

static int matrix_trials(RemoteInvoke invoke, u64 handle, u32 *buffer) {
    static int left[1024], right[1024];
    static const char *names[] = {"left-identity", "right-identity", "dense-1", "dense-2", "zero", "signed-permutation"};
    static const char *details[] = {"marker", "stage", "status", "resource", "vtcm_aligned", "unlock", "release", "power_down", "power_destroy", "executed", "vtcm_guard", "reserved"};
    u16 *input = (u16 *)buffer;
    u32 *metadata = buffer + 1024;
    u16 *product = (u16 *)(metadata + 12);
    RemoteArg arguments[2];
    int status;
    arguments[0].buffer.data = input;
    arguments[0].buffer.size = 4096;
    arguments[1].buffer.data = metadata;
    arguments[1].buffer.size = 2096;
    for (u32 trial = 0; trial < 6; ++trial) {
        u32 mismatches = 0, guards = 0;
        for (u32 row = 0; row < 32; ++row) {
            for (u32 column = 0; column < 32; ++column) {
                u32 index = row * 32 + column;
                left[index] = (int)((row * 17 + column * 11 + row * column * 3 + trial * 7) % 5) - 2;
                right[index] = (int)((row * 13 + column * 7 + row * column + trial * 11) % 5) - 2;
                if (trial == 0) {
                    left[index] = row == column;
                    right[index] = (int)((row * 17 + column * 7) % 31) - 15;
                }
                if (trial == 1) right[index] = row == column;
                if (trial == 4) left[index] = 0;
                if (trial == 5) left[index] = column == (row * 7 + 3) % 32 ? (row % 2 ? -1 : 1) : 0;
                input[index] = integer_half(left[index]);
                input[1024 + index] = integer_half(right[index]);
            }
        }
        for (u32 index = 4096; index < 8192; ++index) ((u8 *)buffer)[index] = 0xa5;
        text("hmx.case="); text(names[trial]); text("\n");
        status = invoke(handle, 0x03010100U, arguments);
        field("hmx.invoke.status", (u32)status);
        if (status) return 10;
        for (u32 index = 0; index < 12; ++index) { text("hmx."); field(details[index], metadata[index]); }
        if (metadata[0] != 0x484d5831U || metadata[1] != 9 || metadata[2] || metadata[3] != 1 ||
            metadata[4] != 1 || metadata[9] != 1 || metadata[11]) return 13;
        if (metadata[5] || metadata[6] || metadata[7] || metadata[8]) return 12;
        for (u32 row = 0; row < 32; ++row) {
            for (u32 column = 0; column < 32; ++column) {
                int expected = 0;
                u32 index = row * 32 + column;
                for (u32 inner = 0; inner < 32; ++inner) expected += left[row * 32 + inner] * right[inner * 32 + column];
                u16 actual = product[index] == 0x8000 ? 0 : product[index];
                if (actual != integer_half(expected)) {
                    if (!mismatches) {
                        field("hmx.first_mismatch.index", index);
                        field("hmx.first_mismatch.expected_bits", integer_half(expected));
                        field("hmx.first_mismatch.actual_bits", actual);
                    }
                    ++mismatches;
                }
                guards += input[index] != integer_half(left[index]);
                guards += input[1024 + index] != integer_half(right[index]);
            }
        }
        for (u32 index = 6192; index < 8192; ++index) guards += ((u8 *)buffer)[index] != 0xa5;
        field("hmx.mismatches", mismatches);
        field("hmx.host_guards", guards);
        if (mismatches || guards || metadata[10]) return 11;
    }
    for (u32 index = 4096; index < 8192; ++index) ((u8 *)buffer)[index] = 0xa5;
    arguments[0].buffer.size = 2048;
    status = invoke(handle, 0x03010100U, arguments);
    field("hmx.bad_size.transport_status", (u32)status);
    field("hmx.bad_size.status", metadata[2]);
    if (status || metadata[0] != 0x484d5831U || metadata[1] || metadata[2] != 14) return 10;
    for (u32 index = 3; index < 12; ++index) if (metadata[index]) return 10;
    for (u32 index = 4144; index < 8192; ++index) if (((u8 *)buffer)[index] != 0xa5) return 11;
    text("hmx.elements_verified=6144\n");
    return 0;
}

static int invoke_dsp(void *module, RpcAlloc allocate, RpcFree release, int hmx) {
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
    buffer = (u32 *)allocate(25, 1, hmx ? 8192 : 4096);
    if (!buffer) { result = 6; goto cleanup; }
    if (hmx) { result = matrix_trials(invoke, handle, buffer); goto cleanup; }
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
        if (mode_argument != 1) goto usage;
        invoke_mode = is_option(module_path, "--invoke") ? 1 : is_option(module_path, "--hmx") ? 2 : 0;
        if (!invoke_mode || argument(&cursor, module_path, 2048)) goto usage;
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
        result = invoke_dsp(module, allocate, release, invoke_mode == 2);
        text(result ? "custom_dsp_execution=failed\n" : "custom_dsp_execution=verified\n");
    } else text("custom_dsp_execution=not_tested\n");
    text(invoke_mode == 2 ? (result ? "hmx_execution=failed\n" : "hmx_execution=verified\n") : "hmx_execution=not_tested\n");
cleanup:
    if (!FreeLibrary(module)) {
        field("unload.win32_error", GetLastError());
        if (!result) result = 7;
    }
    return result;
usage:
    text("usage: fastrpc_probe.exe \"C:\\absolute\\path\\libcdsprpc.dll\" [--invoke|--hmx]\n");
    return 2;
}

void mainCRTStartup(void) {
    output = GetStdHandle((u32)-11);
    ExitProcess((u32)run());
}