#include "whisper_decoder.h"

typedef unsigned int u32;
typedef unsigned long long usize;

__declspec(dllimport) void ExitProcess(u32 status);
__declspec(dllimport) void *VirtualAlloc(
    void *address, usize size, u32 allocation_type, u32 protect
);
__declspec(dllimport) int VirtualFree(void *address, usize size, u32 free_type);

static u32 allocation_calls;
static u32 failed_call;
static u32 live_allocations;

int whisper_decoder_test_token_exclusions(WhisperDecoder *decoder);
int whisper_decoder_test_prefix_cache(WhisperDecoder *decoder);
int whisper_decoder_test_gumbel_cache(WhisperDecoder *decoder, int allocation_fails);

void *whisper_decoder_test_allocate(
    void *address, usize size, u32 allocation_type, u32 protect
) {
    void *result;
    ++allocation_calls;
    if (allocation_calls == failed_call) return 0;
    result = VirtualAlloc(address, size, allocation_type, protect);
    if (result != 0) ++live_allocations;
    return result;
}

int whisper_decoder_test_free(void *address, usize size, u32 free_type) {
    int result = VirtualFree(address, size, free_type);
    if (result && live_allocations != 0U) --live_allocations;
    return result;
}

static int allocation_failure_leaks(u32 call) {
    WhisperDecoder *decoder;
    allocation_calls = 0U;
    failed_call = call;
    live_allocations = 0U;
    decoder = whisper_decoder_load(whisper_model_tiny());
    if (decoder != 0) {
        whisper_decoder_shutdown(decoder);
        return 1;
    }
    return live_allocations != 0U;
}

void mainCRTStartup(void) {
    WhisperDecoder *decoder;
    u32 call;
    whisper_decoder_shutdown(0);
    for (call = 1U; call <= 4U; ++call) {
        if (allocation_failure_leaks(call)) ExitProcess(10U + call);
    }

    allocation_calls = 0U;
    failed_call = 0U;
    live_allocations = 0U;
    decoder = whisper_decoder_load(whisper_model_tiny());
    if (decoder == 0 || live_allocations != 4U) ExitProcess(20U);
    if (whisper_decoder_test_token_exclusions(decoder) != 0) ExitProcess(22U);
    if (whisper_decoder_test_prefix_cache(decoder) != 0) ExitProcess(23U);
    failed_call = allocation_calls + 1U;
    if (whisper_decoder_test_gumbel_cache(decoder, 1) != 0 || live_allocations != 4U) ExitProcess(24U);
    failed_call = 0U;
    if (whisper_decoder_test_gumbel_cache(decoder, 0) != 0 || live_allocations != 5U) ExitProcess(25U);
    whisper_decoder_shutdown(decoder);
    if (live_allocations != 0U) ExitProcess(21U);
    ExitProcess(0U);
}