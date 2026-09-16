#include "gemma_block.h"
#include "gemma_numeric.h"
#include "crypto/sha256.h"
#ifdef GEMMA_TRANSLATE
#include "gemma_tokenizer.h"
static int translate_arguments(void);
static int translate_run(const QnnInterfaceV2 *, QnnBackendHandle, QnnDeviceHandle, QnnContextHandle, i32);
static int translate_release(const QnnInterfaceV2 *, void (*)(void *));
static int translate_quiet;
static void translate_quiet_finish(void);
#endif

__declspec(dllimport) void *GetStdHandle(u32);
__declspec(dllimport) char *GetCommandLineA(void);
__declspec(dllimport) void *CreateFileA(const char *, u32, u32, void *, u32, u32, void *);
__declspec(dllimport) int SetFilePointerEx(void *, long long, long long *, u32);
__declspec(dllimport) int ReadFile(void *, void *, u32, u32 *, void *);
__declspec(dllimport) int WriteFile(void *, const void *, u32, u32 *, void *);
__declspec(dllimport) int CloseHandle(void *);
__declspec(dllimport) void *VirtualAlloc(void *, u64, u32, u32);
__declspec(dllimport) int VirtualFree(void *, u64, u32);
__declspec(dllimport) void *LoadLibraryA(const char *);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
__declspec(dllimport) int FreeLibrary(void *);
__declspec(dllimport) int QueryPerformanceCounter(long long *);
__declspec(dllimport) int QueryPerformanceFrequency(long long *);
__declspec(dllimport) void ExitProcess(u32);
__declspec(dllimport) void *CreateThread(void *, u64, u32 (*)(void *), void *, u32, u32 *);
__declspec(dllimport) void *CreateSemaphoreA(void *, i32, i32, const char *);
__declspec(dllimport) int ReleaseSemaphore(void *, i32, i32 *);
__declspec(dllimport) u32 WaitForSingleObject(void *, u32);
static int serial_bundle_read;

#ifdef GEMMA_TRANSLATE_PROFILE
enum {
    PROFILE_TOTAL, PROFILE_ARGUMENTS, PROFILE_QNN_INIT, PROFILE_RESTORE,
    PROFILE_CONTEXT_READ, PROFILE_CONTEXT_HASH, PROFILE_CONTEXT_CREATE,
    PROFILE_RPC, PROFILE_EMBEDDING_LOAD, PROFILE_BUFFERS,
    PROFILE_ARTIFACT_READ, PROFILE_ARTIFACT_HASH,
    PROFILE_PREFILL_PREPARE, PROFILE_PREFILL_EXECUTE, PROFILE_PREFILL_KV,
    PROFILE_DECODE_PREPARE, PROFILE_DECODE_EXECUTE, PROFILE_DECODE_KV,
    PROFILE_ARGMAX, PROFILE_OUTPUT, PROFILE_CLEANUP, PROFILE_DECODE_SETUP, PROFILE_CACHE_RESET, PROFILE_CACHE_TRANSFER, PROFILE_COUNT
};
static u64 profile_ticks[PROFILE_COUNT], profile_calls[PROFILE_COUNT];
static u64 profile_first_token, profile_decode_min = ~(u64)0, profile_decode_max;
static u64 profile_clock(void) { long long ticks = 0; QueryPerformanceCounter(&ticks); return (u64)ticks; }
static void profile_add(u32 phase, u64 started) {
    profile_ticks[phase] += profile_clock() - started; ++profile_calls[phase];
}
#define PROFILE_START(name) u64 name = profile_clock()
#define PROFILE_END(phase, name) profile_add(phase, name)
#else
#define PROFILE_START(name) ((void)0)
#define PROFILE_END(phase, name) ((void)0)
#endif

typedef struct Binding { char name[192]; char path[1024]; } Binding;
static Binding bindings[1024];
static u32 binding_count, layer, bits;
static void *allocations[8192];
static u32 allocation_count;
static GemmaBlock block;
static char prefix[96], weight_prefix[96];
static QnnTensor runtime_inputs[16], runtime_outputs[128];
static u32 input_count, output_count;
static QnnMemHandle memory_handles[272];
static u32 memory_count;
static u8 *shared;
static u32 cache_positions[2048];
static long long frequency;
static char failure_point[32];
static u32 prompt_chunk = 128;
static u32 prompt_rows = 128;
static GemmaBlock prompt_blocks[34];
static char binding_path[1024];
static QnnApiVersion runtime_version;
enum { CACHE_BYTES = 4U * 2048U * 256U * 2U, ROW_BYTES = 4U * 3U * 256U * 2U,
    REGION_BYTES = CACHE_BYTES + 4096U, ROW_REGION_BYTES = 12288U,
    SHARED_BYTES = 2U * REGION_BYTES + 2U * ROW_REGION_BYTES };

void *memset(void *destination, int value, u64 size) {
    u8 *bytes = destination; u64 index;
    for (index = 0; index < size; ++index) bytes[index] = (u8)value;
    return destination;
}
void *memcpy(void *destination, const void *source, u64 size) {
    u8 *output = destination; const u8 *input = source; u64 index;
    for (index = 0; index < size; ++index) output[index] = input[index];
    return destination;
}
static u32 length(const char *text) { u32 size = 0; while (text[size]) ++size; return size; }
static int equal(const char *left, const char *right) {
    while (*left && *left == *right) { ++left; ++right; } return *left == *right;
}
static void text(const char *message) {
#ifdef GEMMA_TRANSLATE
    if (translate_quiet) return;
#endif
    u32 written; WriteFile(GetStdHandle((u32)
#ifdef GEMMA_TRANSLATE
        -12
#else
        -11
#endif
    ), message, length(message), &written, 0);
}
static void number(u64 value) {
    char output[32], reversed[32]; u32 size = 0, index, written;
    do { reversed[size++] = (char)('0' + value % 10); value /= 10; } while (value);
    for (index = 0; index < size; ++index) output[index] = reversed[size - index - 1];
    output[size] = 0; text(output); (void)written;
}
static void status(const char *name, u64 code) { text(name); text(": "); number(code); text("\n"); }
static void join(char *output, const char *first, const char *second) {
    u32 index = 0; while (*first) output[index++] = *first++; while (*second) output[index++] = *second++; output[index] = 0;
}
static void *allocate(void *user, u64 bytes) {
    void *result; (void)user;
    if (!bytes || bytes > 1073741824U || allocation_count == 8192U) return 0;
    result = VirtualAlloc(0, bytes, 0x3000U, 4U);
    if (result) allocations[allocation_count++] = result;
    return result;
}
static u8 *read_file(const char *path, u32 *size) {
    void *file = CreateFileA(path, 0x80000000U, 1, 0, 3, 0x80U, 0);
    long long size64; u32 received, offset = 0; u8 *data = 0;
    if (file == (void *)(u64)-1) { text("Cannot open "); text(path); text("\n"); return 0; }
    if (!SetFilePointerEx(file, 0, &size64, 2) || size64 < 1 || size64 > 1073741824 || !SetFilePointerEx(file, 0, 0, 0)) goto done;
    data = allocate(0, (u64)size64);
    if (!data) goto done;
    while (offset < (u32)size64) {
        if (!ReadFile(file, data + offset, (u32)size64 - offset, &received, 0) || !received) { data = 0; goto done; }
        offset += received;
    }
    *size = offset;
done:
    CloseHandle(file); return data;
}
static u32 read32(const u8 *data) { return data[0] | (u32)data[1] << 8 | (u32)data[2] << 16 | (u32)data[3] << 24; }
static int read_bindings(void) {
#ifndef GEMMA_TRANSLATE
    char args[3][1024] = {{0}}; const char *command = GetCommandLineA();
    u32 count = 0;
#endif
    u32 size, offset = 20, index; u8 *data;
#ifndef GEMMA_TRANSLATE
    while (*command) {
        u32 used = 0; int quoted = 0;
        while (*command == ' ' || *command == '\t') ++command;
        if (!*command) break;
        if (count == 3) return 0;
        while (*command && (quoted || (*command != ' ' && *command != '\t'))) {
            if (*command == '"') { quoted = !quoted; ++command; continue; }
            if (used >= 1023) return 0;
            args[count][used++] = *command++;
        }
        if (quoted) return 0;
        ++count;
    }
    if (count < 2) return 0;
    memcpy(binding_path, args[1], length(args[1]) + 1);
    if (count == 3) {
        if (length(args[2]) >= sizeof(failure_point)) return 0;
        memcpy(failure_point, args[2], length(args[2]) + 1);
    }
#else
    if (!translate_arguments()) return 0;
#endif
    data = read_file(binding_path, &size);
    if (!data || size < 20 || read32(data) != 0x36424d47U || read32(data + 4) != 2) return 0;
    layer = read32(data + 8); bits = read32(data + 12); binding_count = read32(data + 16);
#ifdef GEMMA_TRANSLATE
    if (layer != 34 || bits != 4) { text("Translator requires the W4 512-token prompt binding\n"); return 0; }
#endif
    if ((layer != 0 && layer != 5 && layer != 34) || (bits != 4 && bits != 8) || binding_count > 1024) return 0;
    for (index = 0; index < binding_count; ++index) {
        u32 name_size, path_size;
        if (size - offset < 8) return 0;
        name_size = read32(data + offset); path_size = read32(data + offset + 4); offset += 8;
        if (!name_size || name_size >= 192 || !path_size || path_size >= 1024 || name_size + path_size > size - offset) return 0;
        memcpy(bindings[index].name, data + offset, name_size); offset += name_size;
        memcpy(bindings[index].path, data + offset, path_size); offset += path_size;
        if (length(bindings[index].name) != name_size || length(bindings[index].path) != path_size) return 0;
    }
    if (offset != size) return 0;
    join(prefix, bits == 8 ? "fixture/w8a16/layer-" : "fixture/w4a16/layer-", layer == 0 ? "0/" : "5/");
    join(weight_prefix, "language_model.model.layers.", layer == 0 ? "0." : "5.");
    return 1;
}
static const void *artifact(const char *name, GemmaArtifactHeader *header) {
    u32 index, size; u8 *data;
    for (index = 0; index < binding_count; ++index) if (equal(name, bindings[index].name)) break;
    if (index == binding_count) return 0;
    PROFILE_START(read_started);
    data = read_file(bindings[index].path, &size);
    PROFILE_END(PROFILE_ARTIFACT_READ, read_started);
    PROFILE_START(hash_started);
    if (!data || size < 256 || !gemma_artifact_decode_header(data, header) ||
        !gemma_artifact_header_valid(header, gemma_model_translategemma_4b()) ||
        !gemma_artifact_header_matches_name(header, name) || header->payload_size != size - 256U ||
        !gemma_artifact_payload_valid(header, data + 256, size - 256U)) return 0;
    PROFILE_END(PROFILE_ARTIFACT_HASH, hash_started);
    return data + 256;
}
static const void *weight(void *user, const char *suffix, GemmaArtifactHeader *header) {
    char name[192]; (void)user; join(name, weight_prefix, suffix); return artifact(name, header);
}
static const void *fixture(const char *suffix, u32 step, u32 *size) {
    char name[192], stem[128]; GemmaArtifactHeader header; const void *data;
    join(stem, prefix, step == 0 ? "step-0/" : step == 1 ? "step-1/" : ""); join(name, stem, suffix);
    data = artifact(name, &header);
    if (!data || header.kind != GEMMA_ARTIFACT_KIND_FIXTURE) return 0;
    *size = (u32)header.payload_size; return data;
}
static GemmaBlockTensor *find_tensor(const char *name) {
    u32 index; for (index = 0; index < block.count; ++index) if (equal(block.tensors[index].name + length(block.prefix), name)) return &block.tensors[index];
    return 0;
}
static u16 half(float value) { union { _Float16 value; u16 bits; } result; result.value = (_Float16)value; return result.bits; }
static int fill_fixture(const char *name, u32 step) {
    GemmaBlockTensor *entry = find_tensor(name); u32 bytes, index;
    const float *source = fixture(name, step, &bytes);
    if (!entry || !source || bytes != entry->bytes * 2U) return 0;
    for (index = 0; index < bytes / 4; ++index) ((u16 *)entry->buffer)[index] = half(source[index]);
    return 1;
}
static int register_tensor(const QnnInterfaceV2 *api, QnnContextHandle context,
                           const char *name, i32 fd, u64 offset) {
    GemmaBlockTensor *entry = find_tensor(name);
    QnnMemDescriptor descriptor = {0}; QnnHtpMemDescriptor custom = {0}; u64 code;
    if (!entry || memory_count == 4 || offset % 4096 || offset + entry->bytes > SHARED_BYTES) return 0;
    descriptor.shape.rank = entry->tensor.data.v1.rank;
    descriptor.shape.dimensions = entry->dimensions;
    descriptor.data_type = QNN_DATATYPE_FLOAT_16; descriptor.memory_type = QNN_MEM_TYPE_CUSTOM;
    descriptor.memory.custom_info = &custom;
    custom.type = QNN_HTP_MEM_SHARED_BUFFER; custom.size = SHARED_BYTES;
    custom.config.shared_buffer.fd = fd; custom.config.shared_buffer.offset = offset;
    code = api->mem_register(context, &descriptor, 1, &memory_handles[memory_count]);
    status(name, code); if (code) return 0;
    entry->buffer = shared + offset;
    entry->tensor.data.v1.memory_type = QNN_TENSORMEMTYPE_MEMHANDLE;
    entry->tensor.data.v1.memory.memory_handle = memory_handles[memory_count++];
    return 1;
}
static int prepare_buffers(void) {
    u32 index;
    for (index = 0; index < block.count; ++index) {
        GemmaBlockTensor *entry = &block.tensors[index];
        u32 type = entry->tensor.data.v1.type;
        if (type != QNN_TENSOR_TYPE_APP_WRITE && type != QNN_TENSOR_TYPE_APP_READ) continue;
        if (!entry->buffer) entry->buffer = allocate(0, entry->bytes);
        if (!entry->buffer) return 0;
        if (entry->tensor.data.v1.memory_type == QNN_TENSORMEMTYPE_RAW) {
            entry->tensor.data.v1.memory.client_buffer.data = entry->buffer;
            entry->tensor.data.v1.memory.client_buffer.data_size = entry->bytes;
        }
        if (type == QNN_TENSOR_TYPE_APP_WRITE) {
            if (input_count == 16) return 0; runtime_inputs[input_count++] = entry->tensor;
        } else {
            if (output_count == 128) return 0; runtime_outputs[output_count++] = entry->tensor;
        }
    }
    return fill_fixture("cos-table", 2) && fill_fixture("sin-table", 2);
}
static int prepare_step(u32 step) {
    u32 size, head, query, key; const u32 *positions = fixture("positions", step, &size);
    u16 *mask = find_tensor("mask")->buffer;
    if (!positions || size != 12 || !fill_fixture("input", step)) return 0;
    for (query = 0; query < 3; ++query)
        if (positions[query] >= GEMMA_BLOCK_CACHE || (query && positions[query] <= positions[query - 1])) return 0;
    for (query = 0; query < 3; ++query) ((float *)find_tensor("positions")->buffer)[query] = (float)positions[query];
    for (head = 0; head < 8; ++head) for (query = 0; query < 3; ++query) for (key = 0; key < 2051; ++key) {
        u32 key_position = key < 2048 ? cache_positions[key] : positions[key - 2048];
        int visible = key_position != 0xffffffffU && gemma_numeric_visible(layer, positions[query], key_position);
        mask[(head * 3 + query) * 2051 + key] = visible ? 0 : 0xfbffU;
    }
    return 1;
}
static void cache_new_rows(void) {
    const u16 *key = find_tensor("k-rope")->buffer, *value = find_tensor("v-projection")->buffer;
    const float *positions = find_tensor("positions")->buffer;
    u16 *past_key = find_tensor("past-key")->buffer, *past_value = find_tensor("past-value")->buffer;
    u32 token, head, capacity = layer == 0 ? 1024 : 2048;
    for (token = 0; token < 3; ++token) {
        u32 slot = (u32)positions[token] % capacity;
        for (head = 0; head < 4; ++head) {
            memcpy(past_key + (head * 2048 + slot) * 256, key + (head * 3 + token) * 256, 512);
            memcpy(past_value + (head * 2048 + slot) * 256, value + (head * 3 + token) * 256, 512);
        }
        cache_positions[slot] = positions[token];
    }
}
static int guards_valid(void) {
    u32 region, index;
    const u32 starts[4] = {CACHE_BYTES, REGION_BYTES + CACHE_BYTES,
        2U * REGION_BYTES + ROW_BYTES, 2U * REGION_BYTES + ROW_REGION_BYTES + ROW_BYTES};
    for (region = 0; region < 4; ++region) for (index = 0; index < (region < 2 ? 4096U : ROW_REGION_BYTES - ROW_BYTES); ++index)
        if (shared[starts[region] + index] != 0xa5) return 0;
    return 1;
}
static int compare(u32 step) {
    static const char *names[] = {"input-rmsnorm", "q-projection", "k-projection", "v-projection",
        "q-rmsnorm", "k-rmsnorm", "q-rope", "k-rope", "gqa", "attention-output", "attention-residual",
        "pre-mlp-rmsnorm", "gate-projection", "up-projection", "gelu", "gated-gelu", "mlp", "output"};
    u32 tap, failed = 0;
    for (tap = 0; tap < sizeof(names) / sizeof(names[0]); ++tap) {
        GemmaBlockTensor *entry = find_tensor(names[tap]); u32 bytes, index;
        const float *expected = fixture(names[tap], step, &bytes);
        double error_square = 0, reference_square = 0; float max_error = 0, max_reference = 0;
        if (!entry || !expected || bytes != 2U * entry->bytes) { text("Missing tap "); text(names[tap]); text("\n"); return 0; }
        for (index = 0; index < entry->bytes / 2; ++index) {
            float actual = gemma_numeric_f16(((u16 *)entry->buffer)[index]);
            float error = actual - expected[index], reference = expected[index];
            if (!(actual <= 65504 && actual >= -65504) || !(reference <= 65504 && reference >= -65504)) return 0;
            error_square += (double)error * error; reference_square += (double)reference * reference;
            if (error < 0) error = -error; if (reference < 0) reference = -reference;
            if (error > max_error) max_error = error; if (reference > max_reference) max_reference = reference;
        }
        text("tap "); text(names[tap]); text(" max_abs_micro="); number((u64)(max_error * 1e6f));
        text(" relative_mse_ppm="); number((u64)(1e6 * error_square / (reference_square + 1e-20))); text("\n");
        if (error_square > 0.000625 * reference_square + 0.000001 * (bytes / 4) || max_error > 0.15f * max_reference + 0.02f) {
            text("FAIL numerical tolerance "); text(names[tap]); text("\n"); ++failed;
        }
    }
    return failed == 0;
}
static int compare_attention(u32 step) {
    u32 bytes, head, query, column, tap, keys = step ? 6U : 3U;
    const u32 *old_positions = fixture("positions", 0, &bytes);
    const float *positions = find_tensor("positions")->buffer;
    const u16 *mask = find_tensor("mask")->buffer;
    static const char *names[2] = {"attention-scores", "attention-softmax"};
    if (!old_positions || bytes != 12) return 0;
    for (tap = 0; tap < 2; ++tap) {
        const float *expected = fixture(names[tap], step, &bytes);
        const u16 *actual = find_tensor(names[tap])->buffer;
        double squared_error = 0, squared_reference = 0;
        float maximum = 0, reference_maximum = 0;
        if (!expected || bytes != 8U * 3U * keys * 4U) return 0;
        for (head = 0; head < 8; ++head) for (query = 0; query < 3; ++query) {
            for (column = 0; column < keys; ++column) {
                u32 past = step && column < 3;
                u32 absolute = past ? old_positions[column] : positions[column - (step ? 3 : 0)];
                u32 slot = past ? absolute % (layer == 0 ? 1024 : 2048) : 2048 + column - (step ? 3 : 0);
                u32 offset = (head * 3 + query) * 2051 + slot;
                float reference = expected[(head * 3 + query) * keys + column], observed, error;
                if (!gemma_numeric_visible(layer, positions[query], absolute)) continue;
                if (past && cache_positions[slot] != absolute) return 0;
                observed = gemma_numeric_f16(actual[offset]);
                if (!(observed <= 65504 && observed >= -65504) || !(reference <= 65504 && reference >= -65504)) return 0;
                error = observed - reference;
                squared_error += (double)error * error; squared_reference += (double)reference * reference;
                if (error < 0) error = -error;
                if (reference < 0) reference = -reference;
                if (error > maximum) maximum = error;
                if (reference > reference_maximum) reference_maximum = reference;
            }
            if (tap) for (column = 0; column < 2051; ++column) {
                u32 offset = (head * 3 + query) * 2051 + column;
                if (mask[offset] && gemma_numeric_f16(actual[offset]) != 0) return 0;
            }
        }
        text("tap "); text(names[tap]); text(" relative_mse_ppm=");
        number((u64)(1e6 * squared_error / (squared_reference + 1e-20))); text("\n");
        if (squared_error > 0.000625 * squared_reference + 0.000001 * 24 * keys ||
            maximum > 0.15f * reference_maximum + 0.02f) return 0;
    }
    return 1;
}
static u64 fingerprint(const void *buffer, u32 size) {
    const u8 *data = buffer; u32 index; u64 hash = 14695981039346656037ULL;
    for (index = 0; index < size; ++index) { hash ^= data[index]; hash *= 1099511628211ULL; }
    return hash;
}
static u64 cache_fingerprint(void) {
    return fingerprint(find_tensor("past-key")->buffer, CACHE_BYTES) ^
        fingerprint(find_tensor("past-value")->buffer, CACHE_BYTES);
}
static u64 now(void) { long long ticks; QueryPerformanceCounter(&ticks); return (u64)ticks; }
static int fail_at(const char *point) {
    if (!equal(failure_point, point)) return 0;
    text("Injected failure: "); text(point); text("\n"); return 1;
}
static void timing(const char *name, u64 start) {
    if (frequency > 0) status(name, (now() - start) * 1000000U / (u64)frequency);
}
static void log_callback(const char *format, u32 level, u64 timestamp, va_list args) {
    (void)timestamp;
    if (level <= QNN_LOG_LEVEL_ERROR) {
        text("QNN: ");
        while (*format) {
            char character[2] = {*format++, 0};
            if (character[0] == '%') {
                u32 wide = 0;
                if (*format == 'l') { ++wide; ++format; if (*format == 'l') { ++wide; ++format; } }
                if (*format == 'd' || *format == 'i') {
                    long long value = wide == 2 ? va_arg(args, long long) : va_arg(args, int);
                    if (value < 0) { text("-"); number(0ULL - (u64)value); } else number((u64)value);
                } else if (*format == 'u' || *format == 'x') number(wide == 2 ? va_arg(args, u64) : va_arg(args, u32));
                else if (*format == 's') { const char *value = va_arg(args, const char *); text(value ? value : "(null)"); }
                else if (*format == 'c') { character[0] = (char)va_arg(args, int); text(character); }
                else { text("%"); if (*format) { character[0] = *format; text(character); } }
                if (*format) ++format;
            } else text(character);
        }
        text("\n");
    }
}

static int position_regression(const QnnInterfaceV2 *api, QnnContextHandle context, GemmaBlockHost host) {
    QnnTensor inputs[16], outputs[6];
    GemmaBlock *blocks[2] = {&block, &prompt_blocks[1]};
    GemmaArtifactHeader header;
    u32 inputs_used = 0, outputs_used = 0, block_index, index, step;
    u64 code, previous_key = 0;
    const float *embedding = artifact("fixture/prompt/input", &header);
    if (!embedding || header.payload_size != 256U * 2560U * 4U) return 0;
    join(weight_prefix, "language_model.model.layers.0.", "");
    code = api->graph_create(context, "gemma_position_regression", 0, &block.graph);
    if (code) return 0;
    for (block_index = 0; block_index < 2; ++block_index) {
        GemmaBlock *current = blocks[block_index];
        current->graph = block.graph; current->internal = 2;
        join(current->prefix, block_index ? "second." : "first.", "");
        if (block_index) current->shared_inputs[0] = &block.tensors[1].tensor;
        if (!gemma_block_build_shape(current, api, context, host, 4, 128, 512)) return 0;
    }
    code = api->graph_finalize(block.graph, 0, 0);
    if (code) { status("position regression finalize", code); return 0; }
    for (block_index = 0; block_index < 2; ++block_index) {
        GemmaBlock *current = blocks[block_index];
        for (index = 0; index < current->count; ++index) {
            GemmaBlockTensor *entry = &current->tensors[index];
            const char *name = entry->name + length(current->prefix);
            u32 element;
            if (entry->borrowed || (entry->tensor.data.v1.type != QNN_TENSOR_TYPE_APP_WRITE &&
                entry->tensor.data.v1.type != QNN_TENSOR_TYPE_APP_READ)) continue;
            entry->buffer = allocate(0, entry->bytes); if (!entry->buffer) return 0;
            entry->tensor.data.v1.memory.client_buffer.data = entry->buffer;
            entry->tensor.data.v1.memory.client_buffer.data_size = entry->bytes;
            if (equal(name, "input")) for (element = 0; element < 128 * 2560; ++element)
                ((u16 *)entry->buffer)[element] = half(embedding[element]);
            if (equal(name, "cos-table") || equal(name, "sin-table")) {
                const float *table = artifact(equal(name, "cos-table") ? "fixture/prompt/local-cos" : "fixture/prompt/local-sin", &header);
                if (!table || header.payload_size != 2048U * 128U * 4U) return 0;
                for (element = 0; element < 512 * 128; ++element) ((u16 *)entry->buffer)[element] = half(table[element]);
            }
            if (entry->tensor.data.v1.type == QNN_TENSOR_TYPE_APP_WRITE) {
                if (inputs_used == 16) return 0;
                inputs[inputs_used++] = entry->tensor;
            } else {
                if (outputs_used == 6) return 0;
                outputs[outputs_used++] = entry->tensor;
            }
        }
    }
    if (inputs_used != 13 || outputs_used != 6 || !blocks[1]->tensors[1].borrowed ||
        blocks[1]->tensors[1].tensor.data.v1.id != block.tensors[1].tensor.data.v1.id) return 0;
    for (step = 0; step < 2; ++step) {
        float *positions = find_tensor("positions")->buffer;
        u64 key_hash;
        for (index = 0; index < 128; ++index) positions[index] = step * 128 + index;
        code = api->graph_execute(block.graph, inputs, inputs_used, outputs, outputs_used, 0, 0);
        if (code) { status("position regression execute", code); return 0; }
        for (index = 0; index < 3; ++index) {
            QnnClientBuffer *first = &outputs[index].data.v1.memory.client_buffer;
            QnnClientBuffer *second = &outputs[index + 3].data.v1.memory.client_buffer;
            u32 element;
            if (first->data_size != second->data_size) return 0;
            for (element = 0; element < first->data_size / 2; ++element) {
                u16 actual = ((const u16 *)first->data)[element];
                if ((actual & 0x7c00U) == 0x7c00U || actual != ((const u16 *)second->data)[element]) return 0;
            }
        }
        key_hash = fingerprint(find_tensor("k-rope")->buffer, 4 * 128 * 256 * 2);
        {
            const u16 *normalized = find_tensor("k-rmsnorm")->buffer;
            const u16 *rotated = find_tensor("k-rope")->buffer;
            const u16 *cosine = find_tensor("cos-table")->buffer, *sine = find_tensor("sin-table")->buffer;
            double error_sum = 0, reference_sum = 0;
            for (index = 0; index < 4 * 128 * 256; ++index) {
                u32 column = index % 256, token = (index / 256) % 128;
                float other = gemma_numeric_f16(normalized[index - column + (column + 128) % 256]);
                float expected = gemma_numeric_f16(half(gemma_numeric_f16(normalized[index]) * gemma_numeric_f16(cosine[(u32)positions[token] * 128 + column % 128]))) +
                    gemma_numeric_f16(half((column < 128 ? -other : other) * gemma_numeric_f16(sine[(u32)positions[token] * 128 + column % 128])));
                float error;
                expected = gemma_numeric_f16(half(expected));
                error = gemma_numeric_f16(rotated[index]) - expected;
                error_sum += (double)error * error; reference_sum += (double)expected * expected;
            }
            status("RoPE primitive relative MSE ppm", (u64)(1e6 * error_sum / (reference_sum + 1e-20)));
            if (error_sum > 0.000625 * reference_sum + 0.000001 * 4 * 128 * 256) return 0;
        }
        if (step && key_hash == previous_key) return 0;
        previous_key = key_hash;
    }
    text("PASS shared-position two-block regression\n");
    return 1;
}

static const char *prompt_graph_name = "gemma_prompt";
static int prompt_bundle;

static int prompt_build(const QnnInterfaceV2 *api, QnnContextHandle context, GemmaBlockHost host, u32 bucket) {
    QnnGraphHandle graph = 0; u32 index; u64 started = now(), code;
    code = api->graph_create(context, prompt_graph_name, 0, &graph);
    status("prompt graphCreate", code); if (code) return 0;
    for (index = 0; index < 34; ++index) {
        GemmaBlock *current = &prompt_blocks[index];
        char suffix[8] = {(char)('0' + index / 10), (char)('0' + index % 10), '.', 0};
        char layer_text[8];
        join(layer_text, index < 10 ? suffix + 1 : suffix, "");
        join(weight_prefix, "language_model.model.layers.", layer_text);
        join(current->prefix, "layer-", suffix);
        current->graph = graph; current->internal = 1;
        if (index && index != 5) {
            u32 owner = (index + 1) % 6 ? 0 : 5, tensor_index;
            static const char *names[4] = {"positions", "cos-table", "sin-table", "mask"};
            for (tensor_index = 0; tensor_index < prompt_blocks[owner].count; ++tensor_index) {
                GemmaBlockTensor *entry = &prompt_blocks[owner].tensors[tensor_index]; u32 shared_index;
                for (shared_index = 0; shared_index < 4; ++shared_index)
                    if (equal(entry->name + 9, names[shared_index])) current->shared_inputs[shared_index] = &entry->tensor;
            }
        }
        if (index) {
            current->shared_inputs[0] = &prompt_blocks[0].tensors[1].tensor;
            current->hidden_input = &prompt_blocks[index - 1].tensors[prompt_blocks[index - 1].count - 1].tensor;
        }
        if (!gemma_block_build_shape(current, api, context, host, bits, prompt_rows, bucket)) return 0;
        status("prompt layer built", index);
    }
    join(weight_prefix, "language_model.model.", "");
    if (!gemma_block_logits(&prompt_blocks[33])) return 0;
    timing("prompt construction us", started);
    started = now(); code = api->graph_finalize(graph, 0, 0);
    status("prompt graphFinalize", code); timing("prompt finalization us", started);
    return code == 0;
}

typedef struct PromptIo {
    u32 layer, id, type, dtype, rank, dimensions[4];
    char name[80];
} PromptIo;
typedef struct PromptHeader {
    u32 magic, version, bucket, tokens, bits, count;
    QnnApiVersion qnn;
    u64 binary_size;
    char repository[64], revision[48], graph[32];
    PromptIo io[320];
    u8 digest[32];
} PromptHeader;
static PromptHeader prompt_header;
static QnnTensor prompt_inputs[256], prompt_outputs[80];
static u32 prompt_input_count, prompt_output_count;
static const float *prompt_expected_key[34], *prompt_expected_value[34], *prompt_embedding, *prompt_logits[2];
static u64 prompt_shared_bytes;

static GemmaBlockTensor *prompt_tensor(u32 layer_index, const char *name) {
    if (equal(name, "positions")) layer_index = 0;
    else if (equal(name, "cos-table") || equal(name, "sin-table") || equal(name, "mask"))
        layer_index = (layer_index + 1) % 6 ? 0 : 5;
    GemmaBlock *current = &prompt_blocks[layer_index]; u32 index;
    for (index = 0; index < current->count; ++index)
        if (equal(current->tensors[index].name + 9, name)) return &current->tensors[index];
    return 0;
}

static int transfer_file(void *file, void *buffer, u64 bytes, int write) {
    u64 offset = 0;
    while (offset < bytes) {
        u32 count = bytes - offset > 1048576 ? 1048576 : (u32)(bytes - offset), transferred = 0;
        int ok = write ? WriteFile(file, (u8 *)buffer + offset, count, &transferred, 0) :
                         ReadFile(file, (u8 *)buffer + offset, count, &transferred, 0);
        if (!ok || !transferred) return 0;
        offset += transferred;
    }
    return 1;
}

static void prompt_digest(PromptHeader *header, void *binary, u8 digest[32]) {
    CryptoSha256Context hash;
    crypto_sha256_init(&hash);
    crypto_sha256_update(&hash, (const u8 *)header, __builtin_offsetof(PromptHeader, digest));
    crypto_sha256_update(&hash, binary, header->binary_size);
    crypto_sha256_final(&hash, digest);
}

static int prompt_io_schema(u32 layer_index, u32 kind, u32 bucket, PromptIo *entry) {
    static const char *names[] = {"input", "positions", "cos-table", "sin-table", "mask",
        "past-key", "past-value", "k-rope", "v-projection", "last-token", "logits", "selected-token", "finite-logits"};
    char prefix[] = "layer-00.";
    if ((kind < 2 && layer_index != 0) || (kind >= 2 && kind <= 4 && layer_index != 0 && layer_index != 5) ||
        (kind >= 9 && layer_index != 33)) return 0;
    memset(entry, 0, sizeof(*entry));
    prefix[6] = (char)('0' + layer_index / 10); prefix[7] = (char)('0' + layer_index % 10);
    join(entry->name, prefix, names[kind]); entry->layer = layer_index;
    entry->type = kind == 7 || kind == 8 || kind >= 10 ? QNN_TENSOR_TYPE_APP_READ : QNN_TENSOR_TYPE_APP_WRITE;
    entry->dtype = kind == 1 || kind == 9 ? QNN_DATATYPE_FLOAT_32 : QNN_DATATYPE_FLOAT_16;
    entry->rank = 2;
    if (kind == 0) { entry->dimensions[0] = 128; entry->dimensions[1] = 2560; }
    else if (kind == 1) { entry->dimensions[0] = 1; entry->dimensions[1] = 128; }
    else if (kind <= 3) { entry->dimensions[0] = bucket; entry->dimensions[1] = 128; }
    else if (kind == 4) { entry->rank = 3; entry->dimensions[0] = 8; entry->dimensions[1] = 128; entry->dimensions[2] = bucket + 128; }
    else if (kind <= 6) { entry->rank = 3; entry->dimensions[0] = 4; entry->dimensions[1] = bucket; entry->dimensions[2] = 256; }
    else if (kind == 7) { entry->rank = 4; entry->dimensions[0] = 1; entry->dimensions[1] = 4; entry->dimensions[2] = 128; entry->dimensions[3] = 256; }
    else if (kind == 8) { entry->rank = 3; entry->dimensions[0] = 4; entry->dimensions[1] = 128; entry->dimensions[2] = 256; }
    else if (kind == 9) { entry->rank = 1; entry->dimensions[0] = 1; }
    else { entry->dimensions[0] = 1; entry->dimensions[1] = 262208; }
    if (kind == 0) entry->dimensions[0] = prompt_rows;
    if (kind == 1 || kind == 4 || kind == 8) entry->dimensions[1] = prompt_rows;
    if (kind == 4) entry->dimensions[2] = bucket + prompt_rows;
    if (kind == 7) entry->dimensions[2] = prompt_rows;
    if (kind >= 11) {
        memset(entry->dimensions, 0, sizeof(entry->dimensions));
        entry->rank = 1; entry->dimensions[0] = 1;
        if (kind == 11) entry->dtype = QNN_DATATYPE_INT_32;
    }
    return 1;
}

static int prompt_header_valid(const PromptHeader *header, u32 bucket, u64 file_size) {
    u32 index, previous, layer_index, kind, dimension, expected_count = 0;
    PromptIo expected;
    if ((bucket != 512 && bucket != 1024 && bucket != 2048) || header->magic != 0x37504d47 ||
        (header->version != 4 && header->version != 5) || header->bucket != bucket || header->tokens != prompt_rows || header->bits != 4 ||
        header->count != (header->version == 5 ? 148U : 146U) || !header->binary_size || header->binary_size > 8589934592ULL ||
        file_size != sizeof(*header) + header->binary_size || header->repository[63] || header->revision[47] || header->graph[31] ||
        !equal(header->repository, gemma_model_translategemma_4b()->repository) ||
        !equal(header->revision, gemma_model_translategemma_4b()->revision) || !equal(header->graph, prompt_graph_name)) return 0;
    for (index = 0; index < sizeof(runtime_version); ++index)
        if (((const u8 *)&header->qnn)[index] != ((const u8 *)&runtime_version)[index]) return 0;
    for (index = 0; index < header->count; ++index) {
        if (header->io[index].name[79]) return 0;
        for (previous = 0; previous < index; ++previous)
            if (header->io[index].id == header->io[previous].id || equal(header->io[index].name, header->io[previous].name)) return 0;
    }
    for (layer_index = 0; layer_index < 34; ++layer_index) for (kind = 0; kind < (header->version == 5 ? 13U : 11U); ++kind) {
        const PromptIo *actual = 0;
        if (!prompt_io_schema(layer_index, kind, bucket, &expected)) continue;
        ++expected_count;
        for (index = 0; index < header->count; ++index)
            if (equal(header->io[index].name, expected.name)) { actual = &header->io[index]; break; }
        if (!actual || actual->layer != expected.layer || actual->type != expected.type ||
            actual->dtype != expected.dtype || actual->rank != expected.rank) return 0;
        for (dimension = 0; dimension < 4; ++dimension)
            if (actual->dimensions[dimension] != expected.dimensions[dimension]) return 0;
    }
    return expected_count == header->count;
}

static int prompt_payload_valid(PromptHeader *header, void *binary) {
    u8 digest[32]; u32 index;
    prompt_digest(header, binary, digest);
    for (index = 0; index < 32; ++index) if (digest[index] != header->digest[index]) return 0;
    return 1;
}

static int prompt_envelope_regression(void) {
    PromptHeader *header = &prompt_header, *original = allocate(0, sizeof(*original));
    u8 payload[16] = {0}; u32 bucket, layer_index, kind, test, passed = 0;
    if (!original) return 0;
    for (prompt_rows = 1; prompt_rows <= 128; prompt_rows *= 128) for (bucket = 512; bucket <= 2048; bucket *= 2) {
        memset(header, 0, sizeof(*header));
        header->magic = 0x37504d47; header->version = 4; header->bucket = bucket;
        header->tokens = prompt_rows; header->bits = 4; header->qnn = runtime_version; header->binary_size = sizeof(payload);
        join(header->repository, gemma_model_translategemma_4b()->repository, "");
        join(header->revision, gemma_model_translategemma_4b()->revision, ""); join(header->graph, "gemma_prompt", "");
        for (layer_index = 0; layer_index < 34; ++layer_index) for (kind = 0; kind < 11; ++kind)
            if (prompt_io_schema(layer_index, kind, bucket, &header->io[header->count])) {
                header->io[header->count].id = header->count + 1; ++header->count;
            }
        prompt_digest(header, payload, header->digest);
        if (!prompt_header_valid(header, bucket, sizeof(*header) + sizeof(payload)) || !prompt_payload_valid(header, payload)) return 0;
        memcpy(original, header, sizeof(*header));
        for (test = 0; test < 23; ++test) {
            u64 file_size = sizeof(*header) + sizeof(payload);
            memcpy(header, original, sizeof(*header));
            switch (test) {
                case 0: header->magic ^= 1; break;
                case 1: header->version = 3; break;
                case 2: header->bucket = bucket == 512 ? 1024 : 512; break;
                case 3: header->tokens = 127; break;
                case 4: header->bits = 8; break;
                case 5: header->count = 321; break;
                case 6: header->binary_size = 0; break;
                case 7: header->binary_size = ~(u64)0; break;
                case 8: --file_size; break;
                case 9: ++file_size; break;
                case 10: memset(header->repository, 'x', sizeof(header->repository)); break;
                case 11: header->revision[0] ^= 1; break;
                case 12: memset(header->graph, 'x', sizeof(header->graph)); break;
                case 13: ((u8 *)&header->qnn)[0] ^= 1; break;
                case 14: memset(header->io[0].name, 'x', sizeof(header->io[0].name)); break;
                case 15: header->io[0].layer = 34; break;
                case 16: header->io[0].rank = 5; break;
                case 17: header->io[0].dimensions[0] = 0xffffffffU; break;
                case 18: header->io[0].dtype = QNN_DATATYPE_INT_32; break;
                case 19: header->io[0].type = QNN_TENSOR_TYPE_NATIVE; break;
                case 20: header->io[1].id = header->io[0].id; break;
                case 21: memcpy(header->io[1].name, header->io[0].name, sizeof(header->io[0].name)); break;
                default: header->io[0].dimensions[3] = 1; break;
            }
            if (test != 6 && test != 7) prompt_digest(header, payload, header->digest);
            if (prompt_header_valid(header, bucket, file_size)) return 0;
            ++passed;
        }
        memcpy(header, original, sizeof(*header)); header->digest[0] ^= 1;
        if (prompt_payload_valid(header, payload)) return 0;
        ++passed;
        memcpy(header, original, sizeof(*header)); payload[0] ^= 1;
        if (prompt_payload_valid(header, payload)) return 0;
        payload[0] ^= 1; ++passed;
    }
    prompt_rows = 128;
    memset(header, 0, sizeof(*header));
    status("PASS prompt envelope corruption cases", passed);
    return 1;
}

static int prompt_bind(const QnnInterfaceV2 *api, QnnContextHandle context, const PromptHeader *header) {
    memset(prompt_blocks, 0, sizeof(prompt_blocks));
    if (api->graph_retrieve(context, header->graph, &prompt_blocks[0].graph)) return 0;
    for (u32 index = 0; index < header->count; ++index) {
        const PromptIo *descriptor = &header->io[index];
        GemmaBlock *current = &prompt_blocks[descriptor->layer];
        if (current->count == GEMMA_BLOCK_TENSORS) return 0;
        GemmaBlockTensor *entry = &current->tensors[current->count++];
        u64 size = descriptor->dtype == QNN_DATATYPE_FLOAT_16 ? 2 : 4;
        memcpy(entry->name, descriptor->name, sizeof(entry->name));
        memcpy(entry->dimensions, descriptor->dimensions, sizeof(entry->dimensions));
        entry->tensor.version = QNN_TENSOR_VERSION_1;
        entry->tensor.data.v1.id = descriptor->id; entry->tensor.data.v1.name = entry->name;
        entry->tensor.data.v1.type = descriptor->type; entry->tensor.data.v1.data_type = descriptor->dtype;
        entry->tensor.data.v1.rank = descriptor->rank; entry->tensor.data.v1.dimensions = entry->dimensions;
        entry->tensor.data.v1.quantize_params.encoding_definition = QNN_DEFINITION_UNDEFINED;
        entry->tensor.data.v1.quantize_params.quantization_encoding = QNN_QUANTIZATION_ENCODING_UNDEFINED;
        for (u32 dimension = 0; dimension < descriptor->rank; ++dimension) size *= descriptor->dimensions[dimension];
        if (!size || size > 0xffffffffU) return 0;
        entry->bytes = (u32)size;
    }
    return 1;
}

static int prompt_restore(const QnnInterfaceV2 *api, QnnBackendHandle backend, QnnDeviceHandle device,
                           QnnContextHandle *context, u32 bucket) {
    u32 layer_index, index; u64 bytes = 0, written = 0, code, started;
    void *binary = 0, *file; char path[1100]; int ok = 0;
    PromptHeader *header = &prompt_header;
    join(path, binding_path, prompt_rows == 1 ? ".decode.context" : ".context");
    if (prompt_blocks[0].count) {
    header->magic = 0x37504d47; header->version = 4; header->bucket = bucket; header->tokens = prompt_rows; header->bits = 4;
    header->qnn = runtime_version;
    join(header->repository, gemma_model_translategemma_4b()->repository, "");
    join(header->revision, gemma_model_translategemma_4b()->revision, "");
    join(header->graph, "gemma_prompt", "");
    for (layer_index = 0; layer_index < 34; ++layer_index) for (index = 0; index < prompt_blocks[layer_index].count; ++index) {
        GemmaBlockTensor *entry = &prompt_blocks[layer_index].tensors[index];
        PromptIo *descriptor;
        if (entry->borrowed || (entry->tensor.data.v1.type != QNN_TENSOR_TYPE_APP_WRITE && entry->tensor.data.v1.type != QNN_TENSOR_TYPE_APP_READ)) continue;
        if (header->count == 320) return 0;
        descriptor = &header->io[header->count++];
        descriptor->layer = layer_index; descriptor->id = entry->tensor.data.v1.id;
        descriptor->type = entry->tensor.data.v1.type; descriptor->dtype = entry->tensor.data.v1.data_type;
        descriptor->rank = entry->tensor.data.v1.rank;
        memcpy(descriptor->dimensions, entry->dimensions, sizeof(descriptor->dimensions));
        memcpy(descriptor->name, entry->name, sizeof(descriptor->name));
    }
    code = api->context_get_binary_size(*context, &bytes); status("prompt binary size status", code);
    status("prompt binary bytes", bytes);
    if (code || !bytes || bytes > 8589934592ULL) return 0;
    binary = VirtualAlloc(0, bytes, 0x3000U, 4U); if (!binary) return 0;
    code = api->context_get_binary(*context, binary, bytes, &written);
    if (code || bytes != written) goto done;
    header->binary_size = bytes;
    prompt_digest(header, binary, header->digest);
    file = CreateFileA(path, 0x40000000U, 0, 0, 2, 0x80U, 0);
    if (file == (void *)(u64)-1) goto done;
    ok = transfer_file(file, header, sizeof(*header), 1) && transfer_file(file, binary, bytes, 1);
    if (!CloseHandle(file)) ok = 0;
    if (!ok) goto done;
    code = api->context_free(*context, 0); if (code) { ok = 0; goto done; } *context = 0;
    VirtualFree(binary, 0, 0x8000U); binary = 0;
    } else {
        long long file_size;
        file = CreateFileA(path, 0x80000000U, 1, 0, 3, 0x80U, 0);
        if (file == (void *)(u64)-1) return 0;
        ok = transfer_file(file, header, sizeof(*header), 0) && SetFilePointerEx(file, 0, &file_size, 2);
        if (!CloseHandle(file)) ok = 0;
        bytes = header->binary_size;
        if (!ok || !prompt_header_valid(header, bucket, (u64)file_size)) return 0;
        if (api->context_free(*context, 0)) return 0;
        *context = 0;
    }
    PROFILE_START(context_read_started);
    binary = VirtualAlloc(0, bytes, 0x3000U, 4U); if (!binary) { ok = 0; goto done; }
    file = CreateFileA(path, 0x80000000U, 1, 0, 3, 0x80U, 0);
    if (file == (void *)(u64)-1) { ok = 0; goto done; }
    ok = transfer_file(file, header, sizeof(*header), 0) && transfer_file(file, binary, bytes, 0);
    if (!CloseHandle(file)) ok = 0;
    PROFILE_END(PROFILE_CONTEXT_READ, context_read_started);
    PROFILE_START(context_hash_started);
    if (!ok || !prompt_header_valid(header, bucket, sizeof(*header) + bytes) ||
        !prompt_payload_valid(header, binary)) { ok = 0; goto done; }
    PROFILE_END(PROFILE_CONTEXT_HASH, context_hash_started);
    PROFILE_START(context_create_started);
    started = now(); code = api->context_create_from_binary(backend, device, 0, binary, bytes, context, 0);
    PROFILE_END(PROFILE_CONTEXT_CREATE, context_create_started);
    status("prompt restore", code); timing("prompt restore us", started);
    if (code) { ok = 0; goto done; }
    ok = prompt_bind(api, *context, header);
done:
    if (binary) VirtualFree(binary, 0, 0x8000U);
    return ok;
}

typedef struct PromptBundle {
    u32 magic, version;
    PromptHeader graphs[2];
    u8 digest[32];
} PromptBundle;
static PromptBundle prompt_bundle_header;
static GemmaBlockTensor *bundle_past[68];

static void bundle_digest(const PromptBundle *header, const void *binary, u8 digest[32]) {
    CryptoSha256Context hash;
    crypto_sha256_init(&hash);
    crypto_sha256_update(&hash, (const u8 *)header, __builtin_offsetof(PromptBundle, digest));
    crypto_sha256_update(&hash, binary, header->graphs[0].binary_size);
    crypto_sha256_final(&hash, digest);
}

static int bundle_valid(const PromptBundle *header, u64 file_size) {
    int ok = header->magic == 0x38424d47 && header->version == 1 &&
        header->graphs[0].binary_size == header->graphs[1].binary_size &&
        header->graphs[0].binary_size <= 8589934592ULL &&
        file_size == sizeof(*header) + header->graphs[0].binary_size;
    for (u32 graph = 0; ok && graph < 2; ++graph) {
        prompt_rows = graph ? 1 : 128;
        prompt_graph_name = graph ? "gemma_decode" : "gemma_prompt";
        ok = prompt_header_valid(&header->graphs[graph], 512, sizeof(PromptHeader) + header->graphs[graph].binary_size);
    }
    prompt_rows = 128; prompt_graph_name = "gemma_prompt";
    return ok;
}

static void bundle_capture(PromptHeader *header) {
    memset(header, 0, sizeof(*header));
    header->magic = 0x37504d47; header->version = 4; header->bucket = 512;
    header->tokens = prompt_rows; header->bits = 4; header->qnn = runtime_version;
#ifdef GEMMA_NPU_SELECTION
    header->version = 5;
#endif
    join(header->repository, gemma_model_translategemma_4b()->repository, "");
    join(header->revision, gemma_model_translategemma_4b()->revision, "");
    join(header->graph, prompt_graph_name, "");
    for (u32 layer_index = 0; layer_index < 34; ++layer_index) {
        for (u32 index = 0; index < prompt_blocks[layer_index].count; ++index) {
            GemmaBlockTensor *entry = &prompt_blocks[layer_index].tensors[index];
            if (entry->borrowed || (entry->tensor.data.v1.type != QNN_TENSOR_TYPE_APP_WRITE &&
                entry->tensor.data.v1.type != QNN_TENSOR_TYPE_APP_READ)) continue;
            if (header->count == 320) return;
            PromptIo *descriptor = &header->io[header->count++];
            descriptor->layer = layer_index; descriptor->id = entry->tensor.data.v1.id;
            descriptor->type = entry->tensor.data.v1.type; descriptor->dtype = entry->tensor.data.v1.data_type;
            descriptor->rank = entry->tensor.data.v1.rank;
            memcpy(descriptor->dimensions, entry->dimensions, sizeof(descriptor->dimensions));
            memcpy(descriptor->name, entry->name, sizeof(descriptor->name));
        }
    }
}

typedef struct SelectionHeader {
    u32 magic, version;
    QnnApiVersion qnn;
    u64 binary_size;
    u32 ids[3];
    u8 digest[32];
} SelectionHeader;
_Static_assert(sizeof(SelectionHeader) == 88 && __builtin_offsetof(SelectionHeader, digest) == 52 &&
               __builtin_offsetof(SelectionHeader, binary_size) == 32, "Selection cache ABI");

static void selection_digest(const SelectionHeader *header, const void *binary, u8 digest[32]) {
    CryptoSha256Context hash;
    crypto_sha256_init(&hash);
    crypto_sha256_update(&hash, (const u8 *)header, __builtin_offsetof(SelectionHeader, digest));
    crypto_sha256_update(&hash, binary, header->binary_size);
    crypto_sha256_final(&hash, digest);
}

static int selection_write(const QnnInterfaceV2 *api, QnnContextHandle context, u32 input, const QnnTensor outputs[2]) {
    SelectionHeader header = {0};
    header.magic = 0x31534d47; header.version = 1; header.qnn = runtime_version;
    header.ids[0] = block.tensors[input].tensor.data.v1.id;
    header.ids[1] = outputs[0].data.v1.id; header.ids[2] = outputs[1].data.v1.id;
    if (api->context_get_binary_size(context, &header.binary_size) || !header.binary_size ||
        header.binary_size > 16777216) return 0;
    void *binary = allocate(0, header.binary_size); u64 written = 0;
    if (!binary || api->context_get_binary(context, binary, header.binary_size, &written) ||
        written != header.binary_size) return 0;
    selection_digest(&header, binary, header.digest);
    char path[1100]; join(path, binding_path, ".selection.context");
    void *file = CreateFileA(path, 0x40000000U, 0, 0, 2, 0x80U, 0);
    if (file == (void *)(u64)-1) return 0;
    int ok = transfer_file(file, &header, sizeof(header), 1) && transfer_file(file, binary, header.binary_size, 1);
    if (!CloseHandle(file)) ok = 0;
    status("selection binary bytes", header.binary_size);
    return ok;
}

static int selection_regression(const QnnInterfaceV2 *api, QnnContextHandle context, GemmaBlockHost host) {
    memset(&block, 0, sizeof(block)); block.api = api; block.host = host; block.internal = 1;
    if (api->graph_create(context, "selection_test", 0, &block.graph)) return 0;
    const u32 shape[] = {1, 262208};
    u16 *values = allocate(0, 262208 * sizeof(u16));
    if (!values) return 0;
    u32 input = gemma_block_tensor(&block, "test-logits", QNN_TENSOR_TYPE_APP_WRITE, QNN_DATATYPE_FLOAT_16, shape, 2, 0);
    if (!gemma_block_select(&block, input) || api->graph_finalize(block.graph, 0, 0)) return 0;
    QnnTensor outputs[2]; u32 count = 0;
    for (u32 index = 0; index < block.count; ++index) {
        GemmaBlockTensor *entry = &block.tensors[index];
        if (entry->tensor.data.v1.type != QNN_TENSOR_TYPE_APP_READ) continue;
        if (count == 2) return 0;
        entry->buffer = allocate(0, entry->bytes);
        if (!entry->buffer) return 0;
        entry->tensor.data.v1.memory.client_buffer.data = entry->buffer;
        entry->tensor.data.v1.memory.client_buffer.data_size = entry->bytes;
        outputs[count++] = entry->tensor;
    }
    block.tensors[input].tensor.data.v1.memory.client_buffer.data = values;
    block.tensors[input].tensor.data.v1.memory.client_buffer.data_size = 262208 * sizeof(u16);
    if (count != 2) return 0;
    for (u32 test = 0; test < 6; ++test) {
        memset(values, 0, 262208 * sizeof(u16));
        u32 expected = test == 1 ? 262207 : test == 2 ? 106 : 0;
        if (test == 1 || test == 2) values[expected] = 0x3c00;
        if (test == 2) values[262207] = 0x3c00;
        if (test >= 3) values[99] = test == 3 ? 0x7c00 : test == 4 ? 0xfc00 : 0x7e00;
        if (api->graph_execute(block.graph, &block.tensors[input].tensor, 1, outputs, 2, 0, 0)) return 0;
        u16 finite = *(u16 *)outputs[1].data.v1.memory.client_buffer.data;
        u32 selected = *(u32 *)outputs[0].data.v1.memory.client_buffer.data;
        if (finite != (test < 3 ? 0x3c00 : 0) || (test < 3 && selected != expected)) {
            status("selection failed case", test); status("selected", selected); status("finite", finite); return 0;
        }
    }
    memset(values, 0, 262208 * sizeof(u16)); values[106] = 0x3c00;
    u64 started = now();
    for (u32 repeat = 0; repeat < 100; ++repeat) {
        if (api->graph_execute(block.graph, &block.tensors[input].tensor, 1, outputs, 2, 0, 0) ||
            *(u32 *)outputs[0].data.v1.memory.client_buffer.data != 106 ||
            *(u16 *)outputs[1].data.v1.memory.client_buffer.data != 0x3c00) return 0;
    }
    timing("selection 100 executions us", started);
    if (equal(failure_point, "build-selection") && !selection_write(api, context, input, outputs)) return 0;
    text("PASS NPU selection ties, boundary IDs and nonfinite rejection\n");
    return 1;
}

static int bundle_regression(void) {
    PromptBundle *header = &prompt_bundle_header;
    PromptBundle *original = allocate(0, sizeof(*original));
    u8 payload[16] = {0}, digest[32];
    if (!original) return 0;
    memset(header, 0, sizeof(*header)); header->magic = 0x38424d47; header->version = 1;
    for (u32 graph = 0; graph < 2; ++graph) {
        PromptHeader *schema = &header->graphs[graph];
        prompt_rows = graph ? 1 : 128;
        schema->magic = 0x37504d47; schema->version = 5; schema->bucket = 512;
        schema->tokens = prompt_rows; schema->bits = 4; schema->qnn = runtime_version;
        schema->binary_size = sizeof(payload);
        join(schema->repository, gemma_model_translategemma_4b()->repository, "");
        join(schema->revision, gemma_model_translategemma_4b()->revision, "");
        join(schema->graph, graph ? "gemma_decode" : "gemma_prompt", "");
        for (u32 layer_index = 0; layer_index < 34; ++layer_index) for (u32 kind = 0; kind < 13; ++kind) {
            if (prompt_io_schema(layer_index, kind, 512, &schema->io[schema->count])) {
                schema->io[schema->count].id = schema->count + 1; ++schema->count;
            }
        }
    }
    bundle_digest(header, payload, header->digest);
    if (!bundle_valid(header, sizeof(*header) + sizeof(payload))) return 0;
    memcpy(original, header, sizeof(*header));
    for (u32 test = 0; test < 12; ++test) {
        memcpy(header, original, sizeof(*header));
        u64 size = sizeof(*header) + sizeof(payload);
        switch (test) {
            case 0: header->magic ^= 1; break;
            case 1: header->version = 2; break;
            case 2: header->graphs[1].binary_size++; break;
            case 3: header->graphs[0].binary_size = ~(u64)0; break;
            case 4: header->graphs[1].tokens = 128; break;
            case 5: header->graphs[1].graph[0] ^= 1; break;
            case 6: header->graphs[0].count--; break;
            case 7: header->graphs[1].io[147].dtype = QNN_DATATYPE_INT_32; break;
            case 8: header->graphs[1].io[146].dimensions[0] = 2; break;
            case 9: header->graphs[1].io[1].id = header->graphs[1].io[0].id; break;
            case 10: --size; break;
            default: ++size; break;
        }
        if (bundle_valid(header, size)) { status("bundle rejection failed case", test); return 0; }
    }
    memcpy(header, original, sizeof(*header)); payload[0] = 1;
    bundle_digest(header, payload, digest);
    int changed = 0;
    for (u32 index = 0; index < 32; ++index) if (digest[index] != header->digest[index]) changed = 1;
    if (!changed) return 0;
    payload[0] = 0; header->graphs[1].io[0].id++;
    bundle_digest(header, payload, digest); changed = 0;
    for (u32 index = 0; index < 32; ++index) if (digest[index] != header->digest[index]) changed = 1;
    if (!changed) return 0;
    memset(header, 0, sizeof(*header));
    text("PASS bundle schema and integrity corruption cases: 14\n");
    return 1;
}

static int bundle_build(const QnnInterfaceV2 *api, QnnBackendHandle backend, QnnDeviceHandle device,
                         QnnContextHandle *context, GemmaBlockHost host) {
    struct { u32 option; union { u8 enabled; u64 reserved[2]; } value; } sharing = {1, {.enabled = 1}};
    struct { u32 option; const void *custom; } config = {0, &sharing};
    const QnnContextConfig *configs[] = {(const QnnContextConfig *)&config, 0};
    if (api->context_free(*context, 0)) return 0;
    *context = 0;
    if (api->context_create(backend, device, configs, context)) return 0;
    PromptBundle *header = &prompt_bundle_header;
    memset(header, 0, sizeof(*header)); header->magic = 0x38424d47; header->version = 1;
    for (u32 graph = 0; graph < 2; ++graph) {
        prompt_rows = graph ? 1 : 128;
        prompt_graph_name = graph ? "gemma_decode" : "gemma_prompt";
        memset(prompt_blocks, 0, sizeof(prompt_blocks));
        if (!prompt_build(api, *context, host, 512)) return 0;
        bundle_capture(&header->graphs[graph]);
        while (allocation_count) if (!VirtualFree(allocations[--allocation_count], 0, 0x8000U)) return 0;
    }
    u64 bytes = 0, written = 0;
    if (api->context_get_binary_size(*context, &bytes) || !bytes || bytes > 8589934592ULL) return 0;
    header->graphs[0].binary_size = bytes; header->graphs[1].binary_size = bytes;
    if (!bundle_valid(header, sizeof(*header) + bytes)) return 0;
    void *binary = VirtualAlloc(0, bytes, 0x3000U, 4U);
    if (!binary) return 0;
    int ok = !api->context_get_binary(*context, binary, bytes, &written) && written == bytes;
    if (ok) {
        char path[1100]; join(path, binding_path, ".bundle.context");
        bundle_digest(header, binary, header->digest);
        void *file = CreateFileA(path, 0x40000000U, 0, 0, 2, 0x80U, 0);
        if (file == (void *)(u64)-1) ok = 0;
        else {
            ok = transfer_file(file, header, sizeof(*header), 1) && transfer_file(file, binary, bytes, 1);
            if (!CloseHandle(file)) ok = 0;
        }
    }
    if (!VirtualFree(binary, 0, 0x8000U)) ok = 0;
    status("bundle binary bytes", bytes);
    return ok;
}

typedef struct BundleReader {
    void *file, *ready;
    u8 *binary;
    u64 bytes;
    int ok;
} BundleReader;

static u32 bundle_reader(void *argument) {
    BundleReader *reader = argument;
    const u64 chunk = 8388608;
    reader->ok = 1;
    for (u64 offset = 0; offset < reader->bytes; offset += chunk) {
        u64 count = reader->bytes - offset < chunk ? reader->bytes - offset : chunk;
        if (reader->ok && !transfer_file(reader->file, reader->binary + offset, count, 0)) reader->ok = 0;
        if (!ReleaseSemaphore(reader->ready, 1, 0)) ExitProcess(1);
    }
    return 0;
}

static int bundle_read_hash(void *file, const PromptBundle *header, void *binary, u8 digest[32]) {
    BundleReader reader = {file, 0, binary, header->graphs[0].binary_size, 0};
    reader.ready = CreateSemaphoreA(0, 0, 1024, 0);
    if (!reader.ready) return 0;
    void *thread = CreateThread(0, 0, bundle_reader, &reader, 0, 0);
    if (!thread) { CloseHandle(reader.ready); return 0; }
    CryptoSha256Context hash;
    crypto_sha256_init(&hash);
    crypto_sha256_update(&hash, (const u8 *)header, __builtin_offsetof(PromptBundle, digest));
    int ok = 1;
    for (u64 offset = 0; offset < reader.bytes; offset += 8388608) {
        u64 count = reader.bytes - offset < 8388608 ? reader.bytes - offset : 8388608;
        if (WaitForSingleObject(reader.ready, 0xffffffffU) != 0) { ok = 0; break; }
        crypto_sha256_update(&hash, (u8 *)binary + offset, count);
    }
    if (WaitForSingleObject(thread, 0xffffffffU) != 0) ExitProcess(1);
    if (!reader.ok) ok = 0;
    crypto_sha256_final(&hash, digest);
    if (!CloseHandle(thread)) ok = 0;
    if (!CloseHandle(reader.ready)) ok = 0;
    return ok;
}

static int bundle_reader_regression(void) {
    PromptBundle header = {0};
    header.graphs[0].binary_size = 8388608 + 97;
    u8 *binary = allocate(0, header.graphs[0].binary_size + 1), expected[32], actual[32];
    if (!binary) return 0;
    if (bundle_read_hash((void *)(u64)-1, &header, binary, actual)) return 0;
    char path[1100]; join(path, binding_path, ".reader-test");
    void *file = CreateFileA(path, 0xc0000000U, 0, 0, 2, 0x04000100U, 0);
    if (file == (void *)(u64)-1) return 0;
    for (u64 index = 0; index < header.graphs[0].binary_size; ++index) binary[index] = (u8)(index * 31 + 7);
    bundle_digest(&header, binary, expected);
    int ok = transfer_file(file, binary, header.graphs[0].binary_size, 1) && SetFilePointerEx(file, 0, 0, 0);
    memset(binary, 0, header.graphs[0].binary_size);
    if (ok) ok = bundle_read_hash(file, &header, binary, actual);
    for (u32 index = 0; index < 32; ++index) if (actual[index] != expected[index]) ok = 0;
    if (ok) {
        header.graphs[0].binary_size++;
        ok = SetFilePointerEx(file, 0, 0, 0) && !bundle_read_hash(file, &header, binary, actual);
    }
    if (!CloseHandle(file)) ok = 0;
    if (ok) text("PASS overlapped reader multi-chunk digest, invalid handle and short read\n");
    return ok;
}

static int bundle_restore(const QnnInterfaceV2 *api, QnnBackendHandle backend, QnnDeviceHandle device,
                           QnnContextHandle *context) {
    PROFILE_START(context_read_started);
    PromptBundle *header = &prompt_bundle_header;
    char path[1100]; long long file_size = 0; u8 digest[32];
    join(path, binding_path, ".bundle.context");
    void *file = CreateFileA(path, 0x80000000U, 1, 0, 3, 0x80U, 0);
    if (file == (void *)(u64)-1) return 0;
    int ok = SetFilePointerEx(file, 0, &file_size, 2) && SetFilePointerEx(file, 0, 0, 0) &&
        transfer_file(file, header, sizeof(*header), 0) && bundle_valid(header, (u64)file_size);
    u64 bytes = ok ? header->graphs[0].binary_size : 0;
    void *binary = ok ? VirtualAlloc(0, bytes, 0x3000U, 4U) : 0;
    u64 load_started = now();
    if (!binary || (serial_bundle_read ? !transfer_file(file, binary, bytes, 0) : !bundle_read_hash(file, header, binary, digest))) ok = 0;
    if (!CloseHandle(file)) ok = 0;
    PROFILE_END(PROFILE_CONTEXT_READ, context_read_started);
    if (ok) {
        PROFILE_START(context_hash_started);
        if (serial_bundle_read) bundle_digest(header, binary, digest);
        for (u32 index = 0; index < 32; ++index) if (digest[index] != header->digest[index]) ok = 0;
        PROFILE_END(PROFILE_CONTEXT_HASH, context_hash_started);
    }
    if (!serial_bundle_read) text("bundle context_read includes overlapped SHA-256\n");
    timing("bundle read/hash us", load_started);
    if (ok) {
        PROFILE_START(context_create_started);
        if (api->context_free(*context, 0)) ok = 0;
        else {
            *context = 0;
            u64 code = api->context_create_from_binary(backend, device, 0, binary, bytes, context, 0);
            status("bundle restore", code);
            ok = !code && prompt_bind(api, *context, &header->graphs[0]);
        }
        PROFILE_END(PROFILE_CONTEXT_CREATE, context_create_started);
    }
    if (binary && !VirtualFree(binary, 0, 0x8000U)) ok = 0;
    return ok;
}

static const float *prompt_fixture(const char *suffix, u32 elements) {
    char name[192]; GemmaArtifactHeader header; const void *data;
    join(name, "fixture/prompt/", suffix); data = artifact(name, &header);
    if (!data || header.kind != GEMMA_ARTIFACT_KIND_FIXTURE || header.payload_size != (u64)elements * 4) return 0;
    return data;
}

static int prompt_buffers(const QnnInterfaceV2 *api, QnnContextHandle context, i32 fd, u32 bucket) {
    u32 layer_index, index, element; u64 offset = 0;
    const float *tables[4];
    tables[0] = prompt_fixture("local-cos", 2048 * 128); tables[1] = prompt_fixture("local-sin", 2048 * 128);
    tables[2] = prompt_fixture("global-cos", 2048 * 128); tables[3] = prompt_fixture("global-sin", 2048 * 128);
    for (index = 0; index < 4; ++index) if (!tables[index]) return 0;
#ifndef GEMMA_TRANSLATE
    prompt_embedding = prompt_fixture("input", 256 * 2560);
    prompt_logits[0] = prompt_fixture("logits-253", 262208); prompt_logits[1] = prompt_fixture("logits-256", 262208);
    if (!prompt_embedding || !prompt_logits[0] || !prompt_logits[1]) return 0;
#endif
    for (layer_index = 0; layer_index < 34; ++layer_index) {
#ifndef GEMMA_TRANSLATE
        char name[80], digits[8] = {(char)('0' + layer_index / 10), (char)('0' + layer_index % 10), '/', 0};
        char stem[32];
        join(stem, "layer-", layer_index < 10 ? digits + 1 : digits);
        join(name, stem, "k-rope"); prompt_expected_key[layer_index] = prompt_fixture(name, 4 * 256 * 256);
        join(name, stem, "v-projection"); prompt_expected_value[layer_index] = prompt_fixture(name, 4 * 256 * 256);
        if (!prompt_expected_key[layer_index] || !prompt_expected_value[layer_index]) return 0;
    #endif
        for (index = 0; index < prompt_blocks[layer_index].count; ++index) {
            GemmaBlockTensor *entry = &prompt_blocks[layer_index].tensors[index]; const char *name_part = entry->name + 9;
            int cache = equal(name_part, "past-key") || equal(name_part, "past-value");
            int current = equal(name_part, "k-rope") || equal(name_part, "v-projection");
            if (cache && prompt_bundle && prompt_rows == 1) {
                GemmaBlockTensor *past = bundle_past[layer_index * 2 + (equal(name_part, "past-value") ? 1 : 0)];
                if (!past || past->bytes != entry->bytes) return 0;
                entry->buffer = past->buffer;
                entry->tensor.data.v1.memory_type = QNN_TENSORMEMTYPE_MEMHANDLE;
                entry->tensor.data.v1.memory.memory_handle = past->tensor.data.v1.memory.memory_handle;
            } else if (cache || current) {
                QnnMemDescriptor descriptor = {0}; QnnHtpMemDescriptor custom = {0}; u64 code;
                if (memory_count == 272 || offset + entry->bytes + 4096 > prompt_shared_bytes) return 0;
                entry->buffer = shared + offset;
                descriptor.shape.rank = entry->tensor.data.v1.rank; descriptor.shape.dimensions = entry->dimensions;
                descriptor.data_type = QNN_DATATYPE_FLOAT_16; descriptor.memory_type = QNN_MEM_TYPE_CUSTOM;
                descriptor.memory.custom_info = &custom; custom.type = QNN_HTP_MEM_SHARED_BUFFER; custom.size = prompt_shared_bytes;
                custom.config.shared_buffer.fd = fd; custom.config.shared_buffer.offset = offset;
                code = api->mem_register(context, &descriptor, 1, &memory_handles[memory_count]);
                if (code) { status("prompt memRegister", code); return 0; }
                entry->tensor.data.v1.memory_type = QNN_TENSORMEMTYPE_MEMHANDLE;
                entry->tensor.data.v1.memory.memory_handle = memory_handles[memory_count++];
                memset(entry->buffer, 0, entry->bytes); offset += ((entry->bytes + 4095U) & ~4095ULL) + 4096;
            } else {
                entry->buffer = allocate(0, entry->bytes); if (!entry->buffer) return 0;
                entry->tensor.data.v1.memory.client_buffer.data = entry->buffer;
                entry->tensor.data.v1.memory.client_buffer.data_size = entry->bytes;
            }
            if (entry->tensor.data.v1.type == QNN_TENSOR_TYPE_APP_WRITE) {
                if (prompt_input_count == 256) return 0; prompt_inputs[prompt_input_count++] = entry->tensor;
            } else {
                if (prompt_output_count == 80) return 0; prompt_outputs[prompt_output_count++] = entry->tensor;
            }
        }
        for (index = 0; index < 2; ++index) {
            u16 *target = prompt_tensor(layer_index, index ? "sin-table" : "cos-table")->buffer;
            const float *source = tables[index + ((layer_index + 1) % 6 ? 0 : 2)];
            for (element = 0; element < bucket * 128; ++element) target[element] = half(source[element]);
        }
    }
    return offset == prompt_shared_bytes;
}

static int prompt_compare(const char *name, const u16 *actual, const float *expected,
                           u32 heads, u32 rows, u32 actual_stride, u32 expected_stride, u32 width) {
    u32 head, row, column; double error_sum = 0, reference_sum = 0; float maximum = 0, reference_maximum = 0;
    for (head = 0; head < heads; ++head) for (row = 0; row < rows; ++row) for (column = 0; column < width; ++column) {
        float observed = gemma_numeric_f16(actual[(head * actual_stride + row) * width + column]);
        float reference = expected[(head * expected_stride + row) * width + column], error = observed - reference;
        if (!(observed >= -65504 && observed <= 65504 && reference >= -65504 && reference <= 65504)) return 0;
        error_sum += (double)error * error; reference_sum += (double)reference * reference;
        if (error < 0) error = -error; if (reference < 0) reference = -reference;
        if (error > maximum) maximum = error; if (reference > reference_maximum) reference_maximum = reference;
    }
    text(name); text(" relative_mse_ppm="); number((u64)(1e6 * error_sum / (reference_sum + 1e-20))); text("\n");
    return error_sum <= 0.000625 * reference_sum + 0.000001 * heads * rows * width && maximum <= 0.15f * reference_maximum + 0.02f;
}

static int prompt_execute(const QnnInterfaceV2 *api, u32 bucket, u32 valid, int verify, u64 *elapsed) {
    u32 layer_index, start, head, row, key, element; u64 started = now(), code;
    for (layer_index = 0; layer_index < 34; ++layer_index) {
        memset(prompt_tensor(layer_index, "past-key")->buffer, 0, 4U * bucket * 512);
        memset(prompt_tensor(layer_index, "past-value")->buffer, 0, 4U * bucket * 512);
    }
    for (start = 0; start < valid; start += prompt_chunk) {
        u32 count = valid - start < prompt_chunk ? valid - start : prompt_chunk;
        u16 *embedding = prompt_tensor(0, "input")->buffer;
        float *positions = prompt_tensor(0, "positions")->buffer;
        for (element = 0; element < 128 * 2560; ++element)
            embedding[element] = element < count * 2560 ? half(prompt_embedding[start * 2560 + element]) : 0;
        for (row = 0; row < 128; ++row) positions[row] = row < count ? start + row : 0;
        for (layer_index = 0; layer_index <= 5; layer_index += 5) {
            u16 *mask = prompt_tensor(layer_index, "mask")->buffer;
            for (head = 0; head < 8; ++head) for (row = 0; row < 128; ++row) for (key = 0; key < bucket + 128; ++key) {
                u32 absolute = key < bucket ? key : start + key - bucket;
                int occupied = key < bucket ? key < start : key < bucket + count;
                int visible = occupied && gemma_numeric_visible(layer_index, positions[row], absolute);
                mask[(head * 128 + row) * (bucket + 128) + key] = visible ? 0 : 0xfbffU;
            }
        }
        *(float *)prompt_tensor(33, "last-token")->buffer = (float)(count - 1);
        code = api->graph_execute(prompt_blocks[0].graph, prompt_inputs, prompt_input_count, prompt_outputs, prompt_output_count, 0, 0);
        if (code) { status("prompt execute", code); return 0; }
        for (layer_index = 0; layer_index < 34; ++layer_index) {
            u32 kind;
            for (kind = 0; kind < 2; ++kind) {
                GemmaBlockTensor *current = prompt_tensor(layer_index, kind ? "v-projection" : "k-rope");
                GemmaBlockTensor *past = prompt_tensor(layer_index, kind ? "past-value" : "past-key");
                for (element = 0; element < 4096; ++element)
                    if (((u8 *)current->buffer)[current->bytes + element] != 0xa5 || ((u8 *)past->buffer)[past->bytes + element] != 0xa5) return 0;
                for (head = 0; head < 4; ++head)
                    memcpy((u16 *)past->buffer + (head * bucket + start) * 256,
                           (const u16 *)current->buffer + head * 128 * 256, count * 512);
            }
        }
    }
    *elapsed = (now() - started) * 1000000U / (u64)frequency;
    if (verify) {
        for (layer_index = 0; layer_index < 34; ++layer_index) {
            status("prompt compare layer", layer_index);
            int key_ok = prompt_compare("key", prompt_tensor(layer_index, "past-key")->buffer, prompt_expected_key[layer_index], 4, valid, bucket, 256, 256);
            int value_ok = prompt_compare("value", prompt_tensor(layer_index, "past-value")->buffer, prompt_expected_value[layer_index], 4, valid, bucket, 256, 256);
            if (!key_ok || !value_ok) return 0;
        }
        if (!prompt_compare("logits", prompt_tensor(33, "logits")->buffer, prompt_logits[valid == 256], 1, 1, 1, 1, 262208)) return 0;
    }
    return 1;
}

void mainCRTStartup(void) {
    PROFILE_START(total_started);
    PROFILE_START(arguments_started);
    typedef u64 (*Providers)(const QnnInterfaceProviderV2 ***, u32 *);
    typedef void *(*RpcAlloc)(i32, u32, i32);
    typedef void (*RpcFree)(void *);
    typedef i32 (*RpcFd)(void *);
    void *module = 0, *rpc_module = 0; RpcFree rpc_free = 0;
    const QnnInterfaceProviderV2 **providers; const QnnInterfaceV2 *api = 0;
    QnnLogHandle log = 0; QnnBackendHandle backend = 0; QnnDeviceHandle device = 0; QnnContextHandle context = 0;
    Providers get_providers; RpcAlloc rpc_alloc; RpcFd rpc_fd; u32 count, index, step, result = 1, cleanup_errors = 0, prompt_bucket = 0;
    u64 code, started; GemmaBlockHost host = {0, allocate, weight, status};
    if (!read_bindings() || !QueryPerformanceFrequency(&frequency)) {
#ifdef GEMMA_TRANSLATE
        text("Invalid request or assets; run translate.exe --help\n");
#endif
        goto cleanup;
    }
    PROFILE_END(PROFILE_ARGUMENTS, arguments_started);
    PROFILE_START(qnn_init_started);
    if (layer == 34 && equal(failure_point, "envelope-regression")) {
        result = prompt_envelope_regression() && bundle_regression() && bundle_reader_regression() ? 0 : 1;
        goto cleanup;
    }
#ifdef GEMMA_TRANSLATE
    text("TranslateGemma 4B W4 NPU prototype\n");
#else
    text(layer == 34 ? "Gemma Stage 7 W" : "Gemma Stage 6 W"); number(bits); text(" layer "); number(layer); text("\n");
#endif
    module = LoadLibraryA("QnnHtp.dll"); if (!module) goto cleanup;
    get_providers = (Providers)GetProcAddress(module, "QnnInterface_getProviders");
    if (!get_providers || get_providers(&providers, &count)) goto cleanup;
    for (index = 0; index < count; ++index) if (providers[index]->prefix.api_version.core_api_version.major == 2 &&
        providers[index]->prefix.api_version.core_api_version.minor == 39) {
            api = &providers[index]->api; runtime_version = providers[index]->prefix.api_version; break;
        }
    if (!api) goto cleanup;
    code = api->log_create(log_callback, QNN_LOG_LEVEL_ERROR, &log); if (code) goto cleanup;
    code = api->backend_create(log, 0, &backend); if (code) goto cleanup;
    code = api->device_create(log, 0, &device); if (code) goto cleanup;
    code = api->context_create(backend, device, 0, &context); if (code) goto cleanup;
    PROFILE_END(PROFILE_QNN_INIT, qnn_init_started);
    if (layer == 34 && (equal(failure_point, "selection-regression") || equal(failure_point, "build-selection"))) {
        result = selection_regression(api, context, host) ? 0 : 1;
        goto cleanup;
    }
    if (layer == 34 && equal(failure_point, "position-regression")) {
        result = position_regression(api, context, host) ? 0 : 1;
        goto cleanup;
    }
    if (layer == 34 && equal(failure_point, "build-bundle-512")) {
        result = bundle_build(api, backend, device, &context, host) && bundle_restore(api, backend, device, &context) ? 0 : 1;
        if (!result) text("PASS bundle built and restored; execution validation remains required\n");
        goto cleanup;
    }
    if (layer == 34) {
        int restore_only = failure_point[0] == 'r';
        if (equal(failure_point, "build-decode-512")) prompt_rows = 1;
        if (equal(failure_point, "decode-check-512")) { restore_only = 1; prompt_chunk = 1; }
        prompt_bucket = equal(failure_point, "build-decode-512") || equal(failure_point, "prompt-512") || equal(failure_point, "restore-512") || equal(failure_point, "decode-check-512") ? 512 :
            equal(failure_point, "prompt-1024") || equal(failure_point, "restore-1024") ? 1024 :
            equal(failure_point, "prompt-2048") || equal(failure_point, "restore-2048") ? 2048 : 0;
        if (!prompt_bucket || bits != 4 || (!restore_only && !prompt_build(api, context, host, prompt_bucket))) goto cleanup;
#ifndef GEMMA_TRANSLATE
        while (allocation_count) if (!VirtualFree(allocations[--allocation_count], 0, 0x8000U)) ++cleanup_errors;
        text("Released construction weight buffers\n");
#endif
        PROFILE_START(restore_started);
        if (cleanup_errors || !(prompt_bundle ? bundle_restore(api, backend, device, &context) :
            prompt_restore(api, backend, device, &context, prompt_bucket))) goto cleanup;
        PROFILE_END(PROFILE_RESTORE, restore_started);
        if (equal(failure_point, "build-decode-512")) { text("PASS decode context built and restored\n"); result = 0; goto cleanup; }
    } else {
    if (fail_at("context")) goto cleanup;
    started = now(); if (!gemma_block_build(&block, api, context, host, bits)) goto cleanup;
    timing("graph construction us", started); status("tensors", block.count);
    if (fail_at("graph")) goto cleanup;
    started = now(); code = api->graph_finalize(block.graph, 0, 0); status("graphFinalize", code);
    timing("graph finalization us", started); if (code) goto cleanup;
    if (fail_at("finalized")) goto cleanup;
    }
    PROFILE_START(rpc_started);
    rpc_module = LoadLibraryA("libcdsprpc.dll"); if (!rpc_module) rpc_module = LoadLibraryA("libadsprpc.dll");
    if (!rpc_module) goto cleanup;
    rpc_alloc = (RpcAlloc)GetProcAddress(rpc_module, "rpcmem_alloc");
    rpc_free = (RpcFree)GetProcAddress(rpc_module, "rpcmem_free");
    rpc_fd = (RpcFd)GetProcAddress(rpc_module, "rpcmem_to_fd");
    if (!rpc_alloc || !rpc_free || !rpc_fd) goto cleanup;
    if (prompt_bucket) {
        u64 elapsed, runs[3], swap; u32 repeat;
        prompt_shared_bytes = 34ULL * (2U * (4U * prompt_bucket * 512U + 4096U) + 2U * (4U * 128U * 512U + 4096U));
        shared = rpc_alloc(25, 1, (i32)prompt_shared_bytes);
        if (!shared || (u64)shared % 4096) goto cleanup;
        memset(shared, 0xa5, prompt_shared_bytes);
        PROFILE_END(PROFILE_RPC, rpc_started);
    #ifdef GEMMA_TRANSLATE
        result = (u32)translate_run(api, backend, device, context, rpc_fd(shared));
        result = result == 1 ? 0 : result == 2 ? 2 : 1;
        goto cleanup;
    #endif
        if (!prompt_buffers(api, context, rpc_fd(shared), prompt_bucket)) goto cleanup;
        if (!prompt_execute(api, prompt_bucket, 253, 1, &elapsed)) goto cleanup;
        status("padded prompt us", elapsed);
        if (prompt_chunk == 1) { text("PASS incremental KV/logits\n"); result = 0; goto cleanup; }
        if (!prompt_execute(api, prompt_bucket, 256, 1, &elapsed)) goto cleanup;
        status("full prompt us", elapsed);
        for (repeat = 0; repeat < 3; ++repeat) {
            u64 hash = fingerprint(prompt_tensor(33, "logits")->buffer, 262208 * 2);
            if (!prompt_execute(api, prompt_bucket, 256, 0, &runs[repeat]) ||
                hash != fingerprint(prompt_tensor(33, "logits")->buffer, 262208 * 2)) goto cleanup;
            status("warm prompt us", runs[repeat]);
        }
        for (index = 0; index < 3; ++index) for (repeat = index + 1; repeat < 3; ++repeat)
            if (runs[repeat] < runs[index]) { swap = runs[index]; runs[index] = runs[repeat]; runs[repeat] = swap; }
        status("warm prompt tokens per second", 256000000ULL / runs[1]);
        if (runs[1] > 853333ULL) { text("FAIL prompt throughput below 300 tokens/s\n"); goto cleanup; }
        result = 0; goto cleanup;
    }
    shared = rpc_alloc(25, 1, SHARED_BYTES); if (!shared) goto cleanup;
    if ((u64)shared % 4096U) goto cleanup;
    memset(shared, 0xa5, SHARED_BYTES);
    if (!register_tensor(api, context, "past-key", rpc_fd(shared), 0)) goto cleanup;
    if (fail_at("registered")) goto cleanup;
    if (!register_tensor(api, context, "past-value", rpc_fd(shared), REGION_BYTES) ||
        !register_tensor(api, context, "k-rope", rpc_fd(shared), 2U * REGION_BYTES) ||
        !register_tensor(api, context, "v-projection", rpc_fd(shared), 2U * REGION_BYTES + ROW_REGION_BYTES)) goto cleanup;
    memset(find_tensor("past-key")->buffer, 0, CACHE_BYTES); memset(find_tensor("past-value")->buffer, 0, CACHE_BYTES);
    for (index = 0; index < 2048; ++index) cache_positions[index] = 0xffffffffU;
    if (!prepare_buffers()) goto cleanup;
    for (step = 0; step < 2; ++step) {
        u64 hashes[GEMMA_BLOCK_TENSORS], cache_hash;
        if (!prepare_step(step)) goto cleanup;
        cache_hash = cache_fingerprint();
        started = now(); code = api->graph_execute(block.graph, runtime_inputs, input_count, runtime_outputs, output_count, 0, 0);
        status("graphExecute", code); timing(step == 0 ? "first execution us" : "cached execution us", started);
        if (code || !guards_valid() || cache_hash != cache_fingerprint()) goto cleanup;
        if (fail_at("executed")) goto cleanup;
        if (!compare(step) || !compare_attention(step)) goto cleanup;
        for (index = 0; index < block.count; ++index) if (block.tensors[index].tensor.data.v1.type == QNN_TENSOR_TYPE_APP_READ)
            hashes[index] = fingerprint(block.tensors[index].buffer, block.tensors[index].bytes);
        for (index = 0; index < 3; ++index) {
            u32 tap;
            started = now(); code = api->graph_execute(block.graph, runtime_inputs, input_count, runtime_outputs, output_count, 0, 0);
            timing("warm execution us", started);
            if (code || !guards_valid() || cache_hash != cache_fingerprint()) goto cleanup;
            for (tap = 0; tap < block.count; ++tap) if (block.tensors[tap].tensor.data.v1.type == QNN_TENSOR_TYPE_APP_READ &&
                hashes[tap] != fingerprint(block.tensors[tap].buffer, block.tensors[tap].bytes)) goto cleanup;
        }
        if (step) {
            GemmaBlockTensor *attention = find_tensor("gqa");
            u64 original = fingerprint(attention->buffer, attention->bytes);
            u16 *mask = find_tensor("mask")->buffer;
            u32 row, column;
            for (row = 0; row < 24; ++row) for (column = 0; column < 2048; ++column) mask[row * 2051 + column] = 0xfbffU;
            code = api->graph_execute(block.graph, runtime_inputs, input_count, runtime_outputs, output_count, 0, 0);
            if (code || original == fingerprint(attention->buffer, attention->bytes) ||
                !guards_valid() || cache_hash != cache_fingerprint() || !prepare_step(step)) goto cleanup;
            code = api->graph_execute(block.graph, runtime_inputs, input_count, runtime_outputs, output_count, 0, 0);
            if (code || original != fingerprint(attention->buffer, attention->bytes) ||
                !guards_valid() || cache_hash != cache_fingerprint()) goto cleanup;
            text("PASS cached-row influence and restored replay\n");
        }
        cache_new_rows();
    }
    status("host KV copy bytes per step", 2U * ROW_BYTES);
    text("DDR traffic: unavailable (no hardware traffic counter requested)\n");
    if (failure_point[0]) goto cleanup;
    result = 0;
cleanup:
    ; PROFILE_START(cleanup_started);
    u64 cleanup_stage = now();
    if (api) {
        while (memory_count) { --memory_count; code = api->mem_deregister(&memory_handles[memory_count], 1); if (code) ++cleanup_errors; }
        timing("cleanup deregister us", cleanup_stage); cleanup_stage = now();
    #ifdef GEMMA_TRANSLATE
        if (!translate_release(api, rpc_free)) ++cleanup_errors;
    #endif
        if (context && api->context_free(context, 0)) ++cleanup_errors;
        timing("cleanup contexts us", cleanup_stage); cleanup_stage = now();
    }
    if (shared && rpc_free) rpc_free(shared);
    if (rpc_module && !FreeLibrary(rpc_module)) ++cleanup_errors;
    if (api) {
        if (device && api->device_free(device)) ++cleanup_errors;
        if (backend && api->backend_free(backend)) ++cleanup_errors;
        if (log && api->log_free(log)) ++cleanup_errors;
    }
    if (module && !FreeLibrary(module)) ++cleanup_errors;
    timing("cleanup backend us", cleanup_stage); cleanup_stage = now();
    while (allocation_count) if (!VirtualFree(allocations[--allocation_count], 0, 0x8000U)) ++cleanup_errors;
    timing("cleanup allocations us", cleanup_stage);
    PROFILE_END(PROFILE_CLEANUP, cleanup_started);
    PROFILE_END(PROFILE_TOTAL, total_started);
#ifdef GEMMA_TRANSLATE_PROFILE
    if (frequency) {
        static const char *names[PROFILE_COUNT] = {
            "total", "arguments", "qnn_init", "restore_total", "context_read", "context_hash", "context_create",
            "rpc", "embedding_load", "buffers", "artifact_read", "artifact_hash",
            "prefill_prepare", "prefill_execute", "prefill_kv", "decode_prepare", "decode_execute", "decode_kv",
            "argmax", "output", "cleanup", "decode_setup", "cache_reset", "cache_transfer"
        };
        for (u32 phase = 0; phase < PROFILE_COUNT; ++phase) {
            text("PROFILE "); text(names[phase]); text(" us="); number(profile_ticks[phase] * 1000000 / (u64)frequency);
            text(" calls="); number(profile_calls[phase]); text("\n");
        }
        status("PROFILE first_token_us", profile_first_token ? (profile_first_token - total_started) * 1000000 / (u64)frequency : 0);
        status("PROFILE decode_min_us", profile_decode_min == ~(u64)0 ? 0 : profile_decode_min * 1000000 / (u64)frequency);
        status("PROFILE decode_max_us", profile_decode_max * 1000000 / (u64)frequency);
    }
#endif
    status("cleanup errors", cleanup_errors);
    if (cleanup_errors) result = 1;
    if (!result && layer != 34) text("PASS block intermediates, cached rows, guard regions and warm determinism\n");
    if (!result && layer == 34)
#ifndef GEMMA_TRANSLATE
    text(prompt_chunk == 1 ? "PASS incremental decode cleanup\n" : equal(failure_point, "envelope-regression") ? "PASS envelope regression cleanup\n" :
        equal(failure_point, "position-regression") ? "PASS position regression cleanup\n" :
        equal(failure_point, "selection-regression") || equal(failure_point, "build-selection") ? "PASS selection regression cleanup\n" :
        equal(failure_point, "build-bundle-512") || equal(failure_point, "build-decode-512") ? "PASS context build cleanup\n" :
        "PASS prompt restore, KV/logits, padding, determinism and throughput\n");
#else
    text("Translation complete\n");
#endif
#ifdef GEMMA_TRANSLATE
    status("translator result", result);
#else
    status("block runner result", result);
#endif
#ifdef GEMMA_TRANSLATE
    translate_quiet_finish();
#endif
    ExitProcess(result);
}