#include "nemotron_runtime.h"
#include "crypto/sha256.h"

int nm_runtime_open(NmRuntime *runtime, const char *path) {
    NmWindowsFile file = {0};
    int ok = 0;
    memset(runtime, 0, sizeof(*runtime));
    if (!nm_absolute_path(path, runtime->path) || !nm_file_open(&file, 0, runtime->path, 0)) return 0;
    runtime->bytes = file.bytes;
    if (file.bytes < NM_HEADER_BYTES + NM_RUNTIME_PREFIX || file.bytes > 8388608) {
        nm_error("runtime index size outside bounded limits"); goto done;
    }
    runtime->data = nm_alloc(file.bytes);
    if (!runtime->data || !nm_file_read(&file, 0, runtime->data, (u32)file.bytes)) goto done;
    if (!nm_validate(&runtime->artifact, runtime->data, file.bytes) || runtime->artifact.kind != NM_RUNTIME) {
        nm_error(runtime->artifact.error ? runtime->artifact.error : "expected runtime index"); goto done;
    }
    ok = 1;
done:
    if (!nm_file_close(&file)) ok = 0;
    if (!ok) nm_runtime_close(runtime);
    return ok;
}
void nm_runtime_close(NmRuntime *runtime) {
    if (!runtime) return;
    nm_free(runtime->data, runtime->bytes); memset(runtime, 0, sizeof(*runtime));
}
int nm_runtime_path(const NmRuntime *runtime, u32 role, char path[4096]) {
    const u8 *entry; size_t end = 0, i, size, length;
    if (!runtime || !runtime->data || !role || role > NM_RUNTIME_FILES)
        return nm_error("runtime resource role outside index");
    entry = nm_runtime_entry(&runtime->artifact, role - 1);
    if (nm_u32(entry + 260)) { rt_copy_string(path, 4096, (const char *)entry); return 1; }
    size = rt_strlen(runtime->path); length = rt_strlen((const char *)entry);
    for (i = 0; i < size; ++i) if (runtime->path[i] == '\\' || runtime->path[i] == '/') end = i + 1;
    if (end + length >= 4096) return nm_error("runtime resource path exceeds bound");
    memcpy(path, runtime->path, end); memcpy(path + end, entry, length + 1); return 1;
}
int nm_runtime_verify(const NmRuntime *runtime, u32 role) {
    char path[4096]; NmWindowsFile file = {0}; CryptoSha256Context hash;
    u8 buffer[65536], digest[32]; u64 cursor = 0; const u8 *entry; int ok = 0;
    if (!nm_runtime_path(runtime, role, path) || !nm_file_open(&file, 0, path, 0)) return 0;
    entry = nm_runtime_entry(&runtime->artifact, role - 1);
    if (file.bytes != nm_u64(entry + 264)) goto done;
    crypto_sha256_init(&hash);
    while (cursor < file.bytes) {
        u32 bytes = file.bytes - cursor > sizeof(buffer) ? sizeof(buffer) : (u32)(file.bytes - cursor);
        if (!nm_file_read(&file, cursor, buffer, bytes)) goto done;
        crypto_sha256_update(&hash, buffer, bytes); cursor += bytes;
    }
    crypto_sha256_final(&hash, digest); ok = !memcmp(digest, entry + 272, 32);
done:
    if (!nm_file_close(&file)) ok = 0;
    if (!ok) { nm_text("Runtime resource mismatch: "); nm_text(path); nm_text("\n"); }
    return ok;
}
u8 *nm_runtime_read(const NmRuntime *runtime, u32 role, u64 *bytes) {
    char path[4096]; u8 *data, hash[32]; const u8 *entry;
    *bytes = 0;
    if (!nm_runtime_path(runtime, role, path)) return 0;
    data = nm_read_file(path, bytes);
    if (!data) return 0;
    entry = nm_runtime_entry(&runtime->artifact, role - 1);
    crypto_sha256_hash(data, (size_t)*bytes, hash);
    if (*bytes != nm_u64(entry + 264) || memcmp(hash, entry + 272, 32)) {
        nm_error("runtime resource bytes/hash mismatch"); nm_free(data, *bytes); *bytes = 0; return 0;
    }
    return data;
}
int nm_runtime_check(const NmRuntime *runtime) {
    u32 role;
    for (role = 1; role <= NM_RUNTIME_FILES; ++role) {
        if (role >= 6 && role <= 10) {
            NmArtifact graph; u64 bytes = 0; u8 *data = nm_runtime_read(runtime, role, &bytes);
            int ok = data && nm_validate(&graph, data, bytes) && nm_runtime_graph_contract(&graph, role);
            if (data && !ok) nm_error(graph.error);
            nm_free(data, bytes);
            if (!ok) return 0;
        } else if (!nm_runtime_verify(runtime, role)) return 0;
    }
    nm_text("PASS runtime package: 16 hash-verified resources and streaming graph contracts\n"); return 1;
}
