#include "../nemotron_frontend.h"
#include "../nemotron_wav.h"
#include "../nemotron_reader.h"
#include "../nemotron_tokenizer.h"
#include <stdlib.h>

static int fail_allocation;
void *rt_malloc(size_t bytes) { return fail_allocation ? 0 : malloc(bytes); }
void rt_free(void *data) { free(data); }
void *rt_realloc(void *data, size_t bytes) { return fail_allocation ? 0 : realloc(data, bytes); }
void *rt_realloc_array(void *data, size_t count, size_t bytes) {
    return bytes && count > SIZE_MAX / bytes ? 0 : rt_realloc(data, count * bytes);
}
void rt_memset(void *data, int value, size_t bytes) { memset(data, value, bytes); }
__declspec(dllexport) void NmFailAllocation(int fail) { fail_allocation = fail; }
__declspec(dllexport) size_t NmArtifactSize(void) { return sizeof(NmArtifact); }
__declspec(dllexport) const char *NmArtifactError(const NmArtifact *artifact) { return artifact->error; }
__declspec(dllexport) size_t NmTokenizerSize(void) { return sizeof(NmTokenizer); }
__declspec(dllexport) const char *NmTokenizerError(const NmTokenizer *tokenizer) { return tokenizer->error; }

__declspec(dllexport) size_t NmFrontendSize(void) { return sizeof(NmFrontend); }
__declspec(dllexport) const char *NmFrontendError(const NmFrontend *state) { return state->error; }
__declspec(dllexport) void NmFrontendInfo(const NmFrontend *state, uint64_t info[5]) {
    info[0] = state->samples; info[1] = state->frames; info[2] = state->chunks;
    info[3] = state->count; info[4] = sizeof(*state);
}
__declspec(dllexport) size_t NmWavSize(void) { return sizeof(NmWav); }
__declspec(dllexport) const char *NmWavError(const NmWav *wav) { return wav->error; }
