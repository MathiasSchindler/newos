#include "nemotron_windows.h"
#include "crypto/sha256.h"

__declspec(dllimport) void *GetStdHandle(u32);
__declspec(dllimport) int WriteFile(void *, const void *, u32, u32 *, void *);
__declspec(dllimport) int ReadFile(void *, void *, u32, u32 *, void *);
__declspec(dllimport) void *CreateFileW(const u16 *, u32, u32, void *, u32, u32, void *);
__declspec(dllimport) int GetFileSizeEx(void *, long long *);
__declspec(dllimport) int SetFilePointerEx(void *, long long, long long *, u32);
__declspec(dllimport) int DeleteFileW(const u16 *);
__declspec(dllimport) u32 GetFullPathNameW(const u16 *, u32, u16 *, u16 **);
__declspec(dllimport) int SetEnvironmentVariableW(const u16 *, const u16 *);
__declspec(dllimport) u32 GetTempPathW(u32, u16 *);
__declspec(dllimport) u32 GetTempFileNameW(const u16 *, const u16 *, u32, u16 *);
__declspec(dllimport) int CreateDirectoryW(const u16 *, void *);
__declspec(dllimport) int RemoveDirectoryW(const u16 *);
__declspec(dllimport) int CloseHandle(void *);
__declspec(dllimport) u32 GetLastError(void);
__declspec(dllimport) void *LoadLibraryExW(const u16 *, void *, u32);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
__declspec(dllimport) int FreeLibrary(void *);
__declspec(dllimport) u32 GetModuleFileNameW(void *, u16 *, u32);
__declspec(dllimport) int MultiByteToWideChar(u32, u32, const char *, int, u16 *, int);
__declspec(dllimport) int WideCharToMultiByte(u32, u32, const u16 *, int, char *, int, const char *, int *);
__declspec(dllimport) u16 *GetCommandLineW(void);
__declspec(dllimport) int QueryPerformanceCounter(long long *);
__declspec(dllimport) int QueryPerformanceFrequency(long long *);
__declspec(dllimport) void *GetCurrentProcess(void);
typedef struct NmFileTime { u32 low, high; } NmFileTime;
__declspec(dllimport) int GetProcessTimes(void *, NmFileTime *, NmFileTime *, NmFileTime *, NmFileTime *);
__declspec(dllimport) int K32GetProcessMemoryInfo(void *, void *, u32);
__declspec(dllimport) u32 GetFileType(void *);
__declspec(dllimport) int FlushFileBuffers(void *);
__declspec(dllimport) int GetModuleHandleExW(u32, const u16 *, void **);
__declspec(dllimport) u32 GetEnvironmentVariableW(const u16 *, u16 *, u32);
typedef struct NmExceptionRecord {
    u32 code, flags;
    void *nested, *address;
    u32 count, reserved;
    u64 information[15];
} NmExceptionRecord;
typedef struct NmExceptionPointers { NmExceptionRecord *record; void *context; } NmExceptionPointers;
typedef long (*NmExceptionFilter)(NmExceptionPointers *);
__declspec(dllimport) NmExceptionFilter SetUnhandledExceptionFilter(NmExceptionFilter);
__declspec(dllimport) void *AddVectoredExceptionHandler(u32, NmExceptionFilter);

static u64 allocated;
static int diagnostics_stderr;
static const char *boundary_operation = "startup", *boundary_name = "", *boundary_type = "";
static long unhandled_exception(NmExceptionPointers *exception);
static void flush_output(void) {
    void *handle = GetStdHandle((u32)-11);
    if (GetFileType(handle) == 1) FlushFileBuffers(handle);
}
static void hexadecimal(u64 value, u32 digits) {
    static const char hex[] = "0123456789abcdef";
    char output[19]; u32 i;
    output[0] = '0'; output[1] = 'x';
    for (i = 0; i < digits; ++i) output[i + 2] = hex[(value >> ((digits - i - 1) * 4)) & 15];
    output[digits + 2] = 0; nm_text(output);
}
void nm_text(const char *s) {
    u32 written; WriteFile(GetStdHandle(diagnostics_stderr ? (u32)-12 : (u32)-11),
                          s, (u32)rt_strlen(s), &written, 0);
}
void nm_diagnostics_to_stderr(int enabled) { diagnostics_stderr = enabled != 0; }
void nm_number(u64 n) { char s[32]; rt_unsigned_to_string(n, s, sizeof(s)); nm_text(s); }
void nm_boundary(const char *operation, const char *name, const char *type) {
    boundary_operation = operation; boundary_name = name; boundary_type = type;
    SetUnhandledExceptionFilter(unhandled_exception);
    nm_text("QNN boundary: "); nm_text(operation); nm_text(" name="); nm_text(name);
    nm_text(" type="); nm_text(type); nm_text("\n"); flush_output();
}
static void report_exception(NmExceptionPointers *exception, const char *classification) {
    static u16 path[32768]; static char utf8[131072]; void *module = 0;
    NmExceptionRecord *r = exception ? exception->record : 0;
    nm_text("nemotron: "); nm_text(classification); nm_text(" native/backend exception code=");
    hexadecimal(r ? r->code : 0, 8);
    nm_text(" address="); hexadecimal(r ? (u64)r->address : 0, 16);
    nm_text(" boundary="); nm_text(boundary_operation); nm_text(" name="); nm_text(boundary_name);
    nm_text(" type="); nm_text(boundary_type); nm_text("\n");
    if (r && GetModuleHandleExW(6, (const u16 *)r->address, &module) &&
        GetModuleFileNameW(module, path, 32768) &&
        WideCharToMultiByte(65001, 0, path, -1, utf8, sizeof(utf8), 0, 0)) {
        nm_text("fault module: "); nm_text(utf8); nm_text(" offset=");
        hexadecimal((u64)r->address - (u64)module, 16); nm_text("\n");
    }
    flush_output();
}
static long unhandled_exception(NmExceptionPointers *exception) {
    report_exception(exception, "unhandled"); return 1;
}
static long first_chance_exception(NmExceptionPointers *exception) {
    if (exception && exception->record && exception->record->code == 0xc0000094U)
        report_exception(exception, "first-chance integer-divide-by-zero");
    return 0;
}
void nm_debug_init(void) {
    AddVectoredExceptionHandler(1, first_chance_exception);
    SetUnhandledExceptionFilter(unhandled_exception);
}
int nm_error(const char *s) { nm_text("nemotron: "); nm_text(s); nm_text("\n"); return 0; }
void *nm_alloc(u64 n) {
    void *p;
    if (!n || n > NM_MAX_PAYLOAD || allocated > NM_MAX_PAYLOAD - n) {
        nm_error("allocation exceeds 8 GiB working-set bound"); return 0;
    }
    p = rt_malloc((size_t)n);
    if (!p) { nm_error("allocation failed"); return 0; }
    allocated += n; memset(p, 0, (size_t)n); return p;
}
void nm_free(void *p, u64 n) { if (p) { rt_free(p); allocated -= n; } }
static int wide(const char *s, u16 *w) {
    return MultiByteToWideChar(65001, 8, s, -1, w, 32768) != 0;
}
static int transfer(void *f, void *data, u64 bytes, int writing) {
    u8 *p = data;
    while (bytes) {
        u32 done = 0, n = bytes > 0x4000000 ? 0x4000000 : (u32)bytes;
        if (!(writing ? WriteFile(f, p, n, &done, 0) : ReadFile(f, p, n, &done, 0)) || !done || done > n) {
            nm_text("file transfer Win32 error "); nm_number(GetLastError()); nm_text("\n"); return 0;
        }
        p += done; bytes -= done;
    }
    return 1;
}
static int input_at(void *context, uint64_t offset, void *data, uint32_t bytes) {
    return nm_file_read(context, offset, data, bytes);
}
int nm_stdout(const void *data, u32 bytes) { return transfer(GetStdHandle((u32)-11), (void *)data, bytes, 1); }
int nm_absolute_path(const char *path, char absolute[4096]) {
    u16 input[32768], output[32768]; u32 count;
    if (!wide(path, input)) return nm_error("invalid UTF8 absolute path input");
    count = GetFullPathNameW(input, 32768, output, 0);
    if (!count || count >= 32768 ||
        !WideCharToMultiByte(65001, 0, output, -1, absolute, 4096, 0, 0))
        return nm_error("absolute path resolution exceeds bounds or failed");
    return 1;
}
int nm_environment_set(const char *name, const char *value) {
    u16 key[128], data[32768];
    if (!MultiByteToWideChar(65001, 8, name, -1, key, 128) ||
        !MultiByteToWideChar(65001, 8, value, -1, data, 32768) ||
        !SetEnvironmentVariableW(key, data)) return nm_error("process environment configuration failed");
    return 1;
}
int nm_file_open(NmWindowsFile *file, NmInput *input, const char *path, int writing) {
    u16 w[32768]; long long size = 0;
    memset(file, 0, sizeof(*file));
    if (!wide(path, w)) return nm_error("invalid UTF8 streaming path");
    file->handle = CreateFileW(w, writing ? 0xc0000000U : 0x80000000U,
                              writing ? 0 : 1, 0, writing ? 1 : 3, 0x80, 0);
    if (file->handle == (void *)(u64)-1) {
        file->handle = 0; nm_text("Cannot open streaming file "); nm_text(path);
        nm_text("; Win32 error "); nm_number(GetLastError()); nm_text("\n"); return 0;
    }
    if (!GetFileSizeEx(file->handle, &size) || size < 0 || (u64)size > NM_MAX_PAYLOAD) {
        nm_error("streaming file size outside limits"); nm_file_close(file); return 0;
    }
    file->bytes = (u64)size;
    if (input) { input->context = file; input->bytes = file->bytes; input->read_at = input_at; }
    return 1;
}
int nm_file_read(NmWindowsFile *file, u64 offset, void *data, u32 bytes) {
    if (!file || !file->handle || offset > file->bytes || bytes > file->bytes - offset)
        return nm_error("streaming read outside file bounds");
    if (!SetFilePointerEx(file->handle, (long long)offset, 0, 0))
        return nm_error("streaming read seek failed");
    return transfer(file->handle, data, bytes, 0);
}
int nm_file_write(NmWindowsFile *file, u64 offset, const void *data, u32 bytes) {
    if (!file || !file->handle || offset > NM_MAX_PAYLOAD || bytes > NM_MAX_PAYLOAD - offset)
        return nm_error("streaming write outside bounds");
    if (!SetFilePointerEx(file->handle, (long long)offset, 0, 0))
        return nm_error("streaming write seek failed");
    if (!transfer(file->handle, (void *)data, bytes, 1)) return 0;
    if (offset + bytes > file->bytes) file->bytes = offset + bytes;
    return 1;
}
int nm_file_close(NmWindowsFile *file) {
    if (file && file->handle) {
        int ok = CloseHandle(file->handle); file->handle = 0;
        if (!ok) return nm_error("streaming file close failed");
    }
    return 1;
}
int nm_file_remove(const char *path) {
    u16 w[32768];
    if (!wide(path, w) || !DeleteFileW(w)) return nm_error("partial output removal failed");
    return 1;
}
static int module_ascii(const char *path) {
    const u8 *text = (const u8 *)path;
    while (*text) {
        if (*text >= 128 || *text == ' ' || *text == '%' || *text == '?' || *text == '#') return 0;
        ++text;
    }
    return 1;
}
static int stage_copy(const char *source, const char *destination, const u8 expected[32], int *created) {
    NmWindowsFile input = {0}, output = {0};
    u8 buffer[65536], hash[32]; u64 position = 0, bytes = 0; u8 *verified = 0;
    int ok = 0;
    if (!nm_file_open(&input, 0, source, 0)) return 0;
    if (input.bytes > 16777216) { nm_error("signed module/catalog staging exceeds16MiB bound"); goto done; }
    if (!nm_file_open(&output, 0, destination, 1)) goto done;
    *created = 1;
    while (position < input.bytes) {
        u32 count = input.bytes - position > sizeof(buffer) ? sizeof(buffer) : (u32)(input.bytes - position);
        if (!nm_file_read(&input, position, buffer, count) || !nm_file_write(&output, position, buffer, count)) goto done;
        position += count;
    }
    if (!nm_file_close(&output)) goto done;
    verified = nm_read_file(destination, &bytes);
    if (!verified) goto done;
    crypto_sha256_hash(verified, (size_t)bytes, hash);
    if (bytes != input.bytes || memcmp(hash, expected, 32)) { nm_error("staged signed bytes/hash mismatch"); goto done; }
    ok = 1;
done:
    if (!nm_file_close(&input)) ok = 0;
    if (!nm_file_close(&output)) ok = 0;
    nm_free(verified, bytes); return ok;
}
int nm_module_stage_close(NmModuleStage *stage) {
    u16 path[32768]; int ok = 1;
    if (!stage) return 1;
    if (stage->module_created && !nm_file_remove(stage->module)) ok = 0;
    if (stage->catalog_created && !nm_file_remove(stage->catalog)) ok = 0;
    if (stage->created && (!wide(stage->directory, path) || !RemoveDirectoryW(path))) {
        nm_error("owned vendor staging directory cleanup failed"); ok = 0;
    }
    if (ok) memset(stage, 0, sizeof(*stage));
    return ok;
}
int nm_module_stage(NmModuleStage *stage, const char *module, const char *catalog,
                    const u8 module_sha[32], const u8 catalog_sha[32]) {
    u16 temporary[32768], path[32768], prefix[4] = {'n','m','a',0};
    u32 count; size_t length;
    memset(stage, 0, sizeof(*stage));
    if (module_ascii(module) && module_ascii(catalog)) {
        rt_copy_string(stage->module, sizeof(stage->module), module);
        rt_copy_string(stage->catalog, sizeof(stage->catalog), catalog); return 1;
    }
    count = GetTempPathW(32768, temporary);
    if (!count || count >= 32768 ||
        !WideCharToMultiByte(65001, 0, temporary, -1, stage->directory, sizeof(stage->directory), 0, 0) ||
        !module_ascii(stage->directory))
        return nm_error("OEM RPC requires an ASCII TEMP path without URI-reserved characters for verified staging");
    if (!GetTempFileNameW(temporary, prefix, 0, path)) return nm_error("owned vendor staging name creation failed");
    if (!WideCharToMultiByte(65001, 0, path, -1, stage->directory, sizeof(stage->directory), 0, 0)) {
        if (!DeleteFileW(path)) nm_error("owned temporary name cleanup failed");
        return nm_error("vendor staging path exceeds UTF8 bound");
    }
    if (!DeleteFileW(path) || !CreateDirectoryW(path, 0)) return nm_error("owned vendor staging directory creation failed");
    stage->created = 1; length = rt_strlen(stage->directory);
    if (length + sizeof("\\nm_fastrpc_skel.cat") > sizeof(stage->catalog)) {
        nm_error("vendor staging file path exceeds bound"); goto fail;
    }
    memcpy(stage->module, stage->directory, length); memcpy(stage->module + length, "\\nm_fastrpc_skel.so", sizeof("\\nm_fastrpc_skel.so"));
    memcpy(stage->catalog, stage->directory, length); memcpy(stage->catalog + length, "\\nm_fastrpc_skel.cat", sizeof("\\nm_fastrpc_skel.cat"));
    if (!stage_copy(module, stage->module, module_sha, &stage->module_created) ||
        !stage_copy(catalog, stage->catalog, catalog_sha, &stage->catalog_created)) goto fail;
    nm_text("OEM module-path boundary: approved ASCII staging of unchanged hash-verified signed module/catalog\nsource=");
    nm_text(module); nm_text("\nstaged="); nm_text(stage->module); nm_text("\n");
    return 1;
fail:
    nm_module_stage_close(stage); return 0;
}
u8 *nm_read_file(const char *path, u64 *bytes) {
    u16 w[32768]; void *f; long long size = 0; u8 *p = 0;
    *bytes = 0;
    if (!wide(path, w)) { nm_error("invalid UTF8 file path"); return 0; }
    f = CreateFileW(w, 0x80000000, 1, 0, 3, 0x80, 0);
    if (f == (void *)(u64)-1) {
        nm_text("Cannot read "); nm_text(path); nm_text("; Win32 error "); nm_number(GetLastError()); nm_text("\n"); return 0;
    }
    if (!GetFileSizeEx(f, &size) || size < 1 || (u64)size > NM_MAX_PAYLOAD) nm_error("file size outside bounded limits");
    else {
        p = nm_alloc((u64)size);
        if (p && !transfer(f, p, (u64)size, 0)) { nm_free(p, (u64)size); p = 0; }
    }
    CloseHandle(f); if (p) *bytes = (u64)size; return p;
}
int nm_write_file(const char *path, const void *p, u64 bytes) {
    u16 w[32768]; void *f; int ok;
    if (!wide(path, w)) return nm_error("invalid output path");
    /* CREATE_NEW prevents overwriting graph, model, fixture or context data. */
    f = CreateFileW(w, 0x40000000, 0, 0, 1, 0x80, 0);
    if (f == (void *)(u64)-1) {
        nm_text("Cannot create output (must not exist): "); nm_text(path); nm_text("; Win32 error ");
        nm_number(GetLastError()); nm_text("\n"); return 0;
    }
    ok = transfer(f, (void *)p, bytes, 1); CloseHandle(f); return ok;
}
u64 nm_clock(void) { long long t = 0; QueryPerformanceCounter(&t); return (u64)t; }
u64 nm_frequency(void) { long long t = 0; QueryPerformanceFrequency(&t); return (u64)t; }
int nm_process_cpu(u64 *user_100ns, u64 *kernel_100ns) {
    NmFileTime creation, exit_time, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user)) {
        nm_text("HOST_PROCESS_CPU GetProcessTimes failed Win32 error="); nm_number(GetLastError()); nm_text("\n");
        return 0;
    }
    *user_100ns = ((u64)user.high << 32) | user.low;
    *kernel_100ns = ((u64)kernel.high << 32) | kernel.low;
    return 1;
}
u64 nm_working_set(void) {
    struct { u32 cb, faults; u64 peak, working, peak_paged, paged, peak_nonpaged, nonpaged, pagefile, peak_pagefile; } counters;
    memset(&counters, 0, sizeof(counters)); counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        nm_error("working-set query failed"); return 0;
    }
    return counters.working;
}
u64 nm_peak_rss(void) {
    struct { u32 cb, faults; u64 peak, working, peak_paged, paged, peak_nonpaged, nonpaged, pagefile, peak_pagefile; } counters;
    memset(&counters, 0, sizeof(counters)); counters.cb = sizeof(counters);
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? counters.peak : 0;
}
void *nm_symbol(void *m, const char *s) { return GetProcAddress(m, s); }
void nm_unload(void *m) { if (m) FreeLibrary(m); }
static int environment(const char *name, char value[4096]) {
    u16 key[128], wide_value[4096]; u32 n;
    if (rt_strlen(name) >= 128 || !MultiByteToWideChar(65001, 8, name, -1, key, 128)) return -1;
    n = GetEnvironmentVariableW(key, wide_value, 4096);
    if (!n) { value[0] = 0; return 0; }
    if (n >= 4096 || !WideCharToMultiByte(65001, 0, wide_value, -1, value, 4096, 0, 0)) return -1;
    return 1;
}
int nm_package_path(const char *path_variable, const char *hash_variable, char path[4096]) {
    char expected[4096]; u8 hash[32], *data; u64 bytes; u32 i;
    int p = environment(path_variable, path), h = environment(hash_variable, expected);
    if (!p && !h) return 0;
    if (p != 1 || h != 1 || rt_strlen(expected) != 64 ||
        !(((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
          path[1] == ':' && path[2] == '\\')) {
        nm_error("FP32 package requires an absolute drive path and explicit SHA256"); return -1;
    }
    data = nm_read_file(path, &bytes);
    if (!data) return -1;
    crypto_sha256_hash(data, (size_t)bytes, hash); nm_free(data, bytes);
    for (i = 0; i < 64; ++i) {
        u32 c = (u8)expected[i], digit = c >= '0' && c <= '9' ? c - '0' :
            c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (digit > 15 || digit != ((hash[i / 2] >> (i & 1 ? 0 : 4)) & 15)) {
            nm_error("FP32 package SHA256 mismatch"); return -1;
        }
    }
    nm_text("FP32 package SHA256 verified: "); nm_text(path); nm_text("\n"); return 1;
}
void *nm_load_qnn(void) {
    u16 path[32768]; char utf8[131072]; u32 n, i;
    u64 bytes; u8 *data, hash[32], header[256]; void *module;
    n = GetModuleFileNameW(0, path, 32768);
    if (!n || n >= 32768) { nm_error("cannot locate executable directory"); return 0; }
    for (i = n; i && path[i - 1] != '\\'; --i) {}
    if (!i || i + 11 >= 32768) return 0;
    for (n = 0; n < 11; ++n) path[i + n] = (u16)"QnnHtp.dll"[n];
    if (!WideCharToMultiByte(65001, 0, path, -1, utf8, sizeof(utf8), 0, 0)) return 0;
    data = nm_read_file(utf8, &bytes); if (!data) return 0;
    crypto_sha256_hash(data, (size_t)bytes, hash);
    nm_header(header, NM_CONTEXT, 16, 1, 0, 0, data, bytes);
    nm_free(data, bytes);
    if (memcmp(hash, header + 168, 32)) { nm_error("pinned QnnHtp.dll SHA256 mismatch"); return 0; }
    /* Absolute path and restricted DLL search exclude current-directory/PATH injection. */
    module = LoadLibraryExW(path, 0, 0x1100);
    if (!module) { nm_text("QnnHtp load failed Win32 error "); nm_number(GetLastError()); nm_text("\n"); }
    return module;
}
int nm_modules(void) {
    typedef int (*EnumModules)(void *, void **, u32, u32 *);
    u16 base[] = {'K','e','r','n','e','l','3','2','.','d','l','l',0};
    void *kernel = LoadLibraryExW(base, 0, 0x800), *modules[1024]; u32 needed = 0, i;
    EnumModules enumerate = kernel ? (EnumModules)GetProcAddress(kernel, "K32EnumProcessModules") : 0;
    if (!enumerate || !enumerate(GetCurrentProcess(), modules, sizeof(modules), &needed) || needed > sizeof(modules)) {
        nm_error("loaded DLL enumeration failed"); nm_unload(kernel); return 0;
    }
    for (i = 0; i < needed / sizeof(void *); ++i) {
        u16 path[32768]; char utf8[131072];
        u32 n = GetModuleFileNameW(modules[i], path, 32768);
        if (n && n < 32768 && WideCharToMultiByte(65001, 0, path, -1, utf8, sizeof(utf8), 0, 0)) {
            nm_text("module: "); nm_text(utf8); nm_text("\n");
        } else { nm_error("loaded module absolute path query failed"); nm_unload(kernel); return 0; }
    }
    nm_unload(kernel); return 1;
}
int nm_arguments_n(char (*args)[4096], u32 capacity) {
    const u16 *command = GetCommandLineW(); u16 word[4096]; u32 count = 0;
    while (*command) {
        u32 used = 0; int quoted = 0;
        while (*command == ' ' || *command == '\t') ++command;
        if (!*command) break;
        if (count == capacity) return 0;
        while (*command && (quoted || (*command != ' ' && *command != '\t'))) {
            u32 slashes = 0;
            while (*command == '\\') { ++slashes; ++command; }
            if (*command == '"') {
                u32 literal = slashes / 2;
                while (literal--) { if (used == 4095) return 0; word[used++] = '\\'; }
                if (slashes & 1) { if (used == 4095) return 0; word[used++] = '"'; }
                else quoted = !quoted;
                ++command;
            } else {
                while (slashes--) { if (used == 4095) return 0; word[used++] = '\\'; }
                if (!*command || (!quoted && (*command == ' ' || *command == '\t'))) break;
                if (used == 4095) return 0;
                word[used++] = *command++;
            }
        }
        if (quoted) return 0;
        word[used] = 0;
        if (!WideCharToMultiByte(65001, 0, word, -1, args[count], 4096, 0, 0)) return 0;
        ++count;
    }
    return (int)count;
}
int nm_arguments(char args[8][4096]) { return nm_arguments_n(args, 8); }
