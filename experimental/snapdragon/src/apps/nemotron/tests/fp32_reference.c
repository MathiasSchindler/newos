#include "fp32/nm_fp32.h"

__declspec(dllexport) int NmFp32Reference(
    const uint32_t *metadata, size_t words, const void *const *inputs,
    const size_t *counts, size_t ninputs, float *output, size_t output_count,
    unsigned int slice, unsigned int slices
) {
    return nm_fp32_execute(metadata, words, inputs, counts, ninputs, output, output_count, slice, slices);
}

int __stdcall DllMainCRTStartup(void *module, unsigned int reason, void *reserved) {
    (void)module;
    (void)reason;
    (void)reserved;
    return 1;
}
