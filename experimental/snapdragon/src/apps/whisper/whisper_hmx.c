#include "whisper_hmx.h"
#include "whisper_frontend.h"

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

__declspec(dllimport) void *LoadLibraryA(const char *path);
__declspec(dllimport) void *GetProcAddress(void *module, const char *name);
__declspec(dllimport) int FreeLibrary(void *module);
__declspec(dllimport) int QueryPerformanceCounter(long long *value);
__declspec(dllimport) int QueryPerformanceFrequency(long long *value);
__declspec(dllimport) void *VirtualAlloc(void *address, u64 size,
                                         u32 allocation_type, u32 protect);
__declspec(dllimport) int VirtualFree(void *address, u64 size, u32 free_type);

struct WhisperHmx {
    void *module;
    RemoteControl session;
    RemoteInvoke invoke;
    RemoteClose close;
    RpcFree release;
    void *shared;
    unsigned short *packed_weights;
    u64 handle;
    u32 submissions;
    u64 invoke_ticks;
    u64 width_invoke_ticks[4];
    u32 width_invoke_calls[4];
    u64 frequency;
    int batched;
    int grouped;
    int four_grouped;
    int require_batch;
    int opened;
};

static struct WhisperHmx active;
static u32 last_error;

_Static_assert(sizeof(void *) == 8 && sizeof(RemoteArg) == 16, "ARM64 FastRPC ABI");

int whisper_hmx_close(WhisperHmx *hmx) {
    int result = 1;
    struct { int domain; } request = {3};
    if (!hmx) return 0;
    if (hmx->packed_weights && !VirtualFree(hmx->packed_weights, 0, 0x8000U)) result = 0;
    if (hmx->shared) hmx->release(hmx->shared);
    if (hmx->opened) {
        if (hmx->close(hmx->handle)) result = 0;
    }
    if (hmx->session && (!hmx->opened || !result) &&
        hmx->session(7, &request, sizeof(request))) result = 0;
    if (hmx->module && !FreeLibrary(hmx->module)) result = 0;
    hmx->module = 0;
    hmx->shared = 0;
    hmx->packed_weights = 0;
    hmx->opened = 0;
    hmx->session = 0;
    return result;
}

WhisperHmx *whisper_hmx_open(const char *driver_path) {
    RemoteOpen open;
    RpcAlloc allocate;
    struct { int domain; int enable; } request = {3, 1};
    last_error = 0;
    if (!driver_path || !*driver_path || active.module) { last_error = 1; return 0; }
    active.require_batch = 0;
    active.module = LoadLibraryA(driver_path);
    if (!active.module) { last_error = 2; return 0; }
    active.session = (RemoteControl)GetProcAddress(active.module, "remote_session_control");
    active.invoke = (RemoteInvoke)GetProcAddress(active.module, "remote_handle64_invoke");
    active.close = (RemoteClose)GetProcAddress(active.module, "remote_handle64_close");
    open = (RemoteOpen)GetProcAddress(active.module, "remote_handle64_open");
    allocate = (RpcAlloc)GetProcAddress(active.module, "rpcmem_alloc");
    active.release = (RpcFree)GetProcAddress(active.module, "rpcmem_free");
    if (!active.session || !active.invoke || !active.close || !open ||
        !allocate || !active.release) { last_error = 3; goto fail; }
    if (active.session(2, &request, sizeof(request))) { last_error = 4; goto fail; }
    if (open("file:///fastrpc_probe_skel.so?fastrpc_probe_skel_invoke&_modver=1.0&_dom=cdsp",
             &active.handle)) { last_error = 5; goto fail; }
    active.opened = 1;
    active.shared = allocate(25, 1, 2097152);
    if (!active.shared) { last_error = 6; goto fail; }
    active.packed_weights = VirtualAlloc(0, 2097152, 0x3000U, 0x04U);
    if (!active.packed_weights) { last_error = 14; goto fail; }
    {
        long long frequency;
        if (!QueryPerformanceFrequency(&frequency) || frequency <= 0) {
            last_error = 13;
            goto fail;
        }
        active.frequency = (u64)frequency;
    }
    active.submissions = 0;
    active.invoke_ticks = 0;
    for (u32 index = 0; index < 4; ++index) {
        active.width_invoke_ticks[index] = 0;
        active.width_invoke_calls[index] = 0;
    }
    active.batched = -1;
    active.grouped = -1;
    active.four_grouped = -1;
    return &active;
fail:
    whisper_hmx_close(&active);
    return 0;
}

unsigned int whisper_hmx_submissions(const WhisperHmx *hmx) {
    return hmx ? hmx->submissions : 0;
}

unsigned int whisper_hmx_invoke_milliseconds(const WhisperHmx *hmx) {
    return hmx && hmx->frequency ?
        (unsigned int)(hmx->invoke_ticks * 1000U / hmx->frequency) : 0;
}

static u32 width_index(u32 input_width) {
    return input_width == 384U ? 0U : input_width == 512U ? 1U :
           input_width == 1536U ? 2U : 3U;
}

unsigned int whisper_hmx_width_invoke_calls(const WhisperHmx *hmx, u32 input_width) {
    return hmx ? hmx->width_invoke_calls[width_index(input_width)] : 0;
}

unsigned int whisper_hmx_width_invoke_milliseconds(const WhisperHmx *hmx, u32 input_width) {
    return hmx && hmx->frequency ?
        (unsigned int)(hmx->width_invoke_ticks[width_index(input_width)] * 1000U /
                       hmx->frequency) : 0;
}

unsigned int whisper_hmx_last_error(void) {
    return last_error;
}

int whisper_hmx_batched(const WhisperHmx *hmx) {
    return hmx && hmx->batched == 1;
}

int whisper_hmx_grouped(const WhisperHmx *hmx) {
    return hmx && (hmx->grouped == 1 || hmx->four_grouped == 1);
}

void whisper_hmx_require_batch(WhisperHmx *hmx) {
    if (hmx) hmx->require_batch = 1;
}

static int timed_invoke(WhisperHmx *hmx, u32 method, RemoteArg *arguments, u32 input_width) {
    long long start, end;
    u32 index = width_index(input_width);
    int timed = QueryPerformanceCounter(&start);
    int result = hmx->invoke(hmx->handle, method, arguments);
    ++hmx->width_invoke_calls[index];
    if (timed && QueryPerformanceCounter(&end) && end >= start) {
        u64 ticks = (u64)(end - start);
        hmx->invoke_ticks += ticks;
        hmx->width_invoke_ticks[index] += ticks;
    }
    return result;
}

static int batched_projection(WhisperHmx *hmx, const float *weights, const float *bias,
                              const float *input, float *output, u32 frames,
                              u32 input_width, u32 output_width, u32 groups) {
    unsigned short *tiles = (unsigned short *)hmx->shared;
    u32 *detail = (u32 *)(tiles + (32U + 32U * groups) * input_width);
    const unsigned short *product = (const unsigned short *)(detail + 12);
    RemoteArg arguments[2];
    arguments[0].buffer.data = tiles;
    arguments[0].buffer.size = (32U + 32U * groups) * input_width * sizeof(*tiles);
    arguments[1].buffer.data = detail;
    arguments[1].buffer.size = 48U + groups * (input_width / 32U) * 2048U;
    for (u32 column = 0; column < output_width; column += 32)
        for (u32 inner = 0; inner < input_width; ++inner)
            for (u32 lane = 0; lane < 32; ++lane)
                hmx->packed_weights[column * input_width + inner * 32U + lane] =
                    whisper_frontend_float_to_half(
                        weights[(column + lane) * input_width + inner]);
    for (u32 batch = 0; batch < frames; batch += 32) {
        for (u32 row = 0; row < 32; ++row) {
            for (u32 inner = 0; inner < input_width; ++inner) {
                tiles[row * input_width + inner] = whisper_frontend_float_to_half(
                    batch + row < frames ? input[(batch + row) * input_width + inner] : 0.0f);
            }
        }
        for (u32 column = 0; column < output_width; column += 32U * groups) {
            for (u32 row = 0; row < 32 && batch + row < frames; ++row)
                for (u32 lane = 0; lane < 32U * groups; ++lane)
                    output[(batch + row) * output_width + column + lane] = bias[column + lane];
            for (u32 index = 0; index < groups * input_width * 32U; ++index)
                tiles[32U * input_width + index] =
                    hmx->packed_weights[column * input_width + index];
            detail[0] = 0xa5a5a5a5U;
              int status = timed_invoke(hmx, groups == 4U ? 0x06010100U :
                                      groups == 2U ? 0x05010100U : 0x04010100U,
                                      arguments, input_width);
              if ((groups == 4U ? hmx->four_grouped :
                  groups == 2U ? hmx->grouped : hmx->batched) == -1 && (status == 14 ||
                (!status && detail[0] == 0xa5a5a5a5U))) return 2;
            if (status || detail[0] != 0x484d5831U || detail[1] != 9 || detail[2] ||
                detail[3] != 1 || detail[4] != 1 || detail[5] || detail[6] ||
                detail[7] || detail[8] || detail[9] != 1 || detail[10] || detail[11]) {
                last_error = status ? 7 : 8;
                return 0;
            }
            hmx->batched = 1;
            if (groups == 2U) hmx->grouped = 1;
            if (groups == 4U) hmx->four_grouped = 1;
            ++hmx->submissions;
            for (u32 group = 0; group < groups; ++group)
                for (u32 tile = 0; tile < input_width / 32U; ++tile)
                    for (u32 row = 0; row < 32 && batch + row < frames; ++row)
                        for (u32 lane = 0; lane < 32; ++lane)
                            output[(batch + row) * output_width + column + group * 32U + lane] +=
                                whisper_frontend_half_to_float(product[
                                    (group * (input_width / 32U) + tile) * 1024U + row * 32U + lane]);
        }
    }
    return 1;
}

int whisper_hmx_projection(void *context, const float *weights, const float *bias,
                           const float *input, float *output, unsigned int frames,
                           unsigned int input_width, unsigned int output_width) {
    WhisperHmx *hmx = (WhisperHmx *)context;
    unsigned short *tiles;
    u32 *detail;
    const unsigned short *product;
    RemoteArg arguments[2];
    if (!hmx || !hmx->opened || !hmx->shared || !weights || !bias || !input || !output ||
        !frames || !((input_width == 384 && (output_width == 384 || output_width == 1536)) ||
                 (input_width == 1536 && output_width == 384) ||
                 (input_width == 512 && (output_width == 512 || output_width == 2048)) ||
                 (input_width == 2048 && output_width == 512))) {
        last_error = 9;
        return 0;
    }
    if (hmx->batched != 0) {
        int result;
        if (hmx->four_grouped != 0 && input_width <= 1536U) {
            result = batched_projection(hmx, weights, bias, input, output,
                                        frames, input_width, output_width, 4U);
            if (result != 2) return result;
            hmx->four_grouped = 0;
        }
        if (hmx->grouped != 0) {
            result = batched_projection(hmx, weights, bias, input, output,
                                        frames, input_width, output_width, 2U);
            if (result != 2) return result;
            hmx->grouped = 0;
        }
        result = batched_projection(hmx, weights, bias, input, output,
                                    frames, input_width, output_width, 1U);
        if (result != 2) return result;
        if (hmx->require_batch) { last_error = 12; return 0; }
        hmx->batched = 0;
    }
    tiles = (unsigned short *)hmx->shared;
    detail = (u32 *)hmx->shared + 1024;
    product = (const unsigned short *)(detail + 12);
    arguments[0].buffer.data = tiles;
    arguments[0].buffer.size = 4096;
    arguments[1].buffer.data = detail;
    arguments[1].buffer.size = 2096;
    for (u32 batch = 0; batch < frames; batch += 32) {
        for (u32 column = 0; column < output_width; column += 32) {
            for (u32 row = 0; row < 32 && batch + row < frames; ++row)
                for (u32 lane = 0; lane < 32; ++lane)
                    output[(batch + row) * output_width + column + lane] = bias[column + lane];
            for (u32 inner = 0; inner < input_width; inner += 32) {
                for (u32 row = 0; row < 32; ++row) {
                    for (u32 lane = 0; lane < 32; ++lane) {
                        float value = batch + row < frames ?
                            input[(batch + row) * input_width + inner + lane] : 0.0f;
                        tiles[row * 32 + lane] = whisper_frontend_float_to_half(value);
                        tiles[1024 + row * 32 + lane] = whisper_frontend_float_to_half(
                            weights[(column + lane) * input_width + inner + row]);
                    }
                }
                if (timed_invoke(hmx, 0x03010100U, arguments, input_width)) {
                    last_error = 10;
                    return 0;
                }
                if (
                    detail[0] != 0x484d5831U || detail[1] != 9 || detail[2] ||
                    detail[3] != 1 || detail[4] != 1 || detail[5] || detail[6] ||
                    detail[7] || detail[8] || detail[9] != 1 || detail[10] || detail[11]) {
                    last_error = 11;
                    return 0;
                }
                ++hmx->submissions;
                for (u32 row = 0; row < 32 && batch + row < frames; ++row)
                    for (u32 lane = 0; lane < 32; ++lane)
                        output[(batch + row) * output_width + column + lane] +=
                            whisper_frontend_half_to_float(product[row * 32 + lane]);
            }
        }
    }
    return 1;
}