#include "whisper_cpu.h"
#include "whisper_frontend.h"

__declspec(dllimport) void __stdcall ExitProcess(unsigned int status);

void mainCRTStartup(void) {
    const float weights[] = {1.0f, -2.0f, 3.0f, -4.0f, 5.0f, 6.0f};
    const float bias[] = {0.5f, -0.5f};
    const float input[] = {2.0f, 3.0f, -1.0f};
    float output[2] = {0.0f, 0.0f};
    const float norm_input[] = {1.0f, 1.0f};
    const float norm_scale[] = {2.0f, 3.0f};
    const float norm_bias[] = {4.0f, 5.0f};
    float normalized[2];
    static float silence[WHISPER_SAMPLE_COUNT];
    static float mel[WHISPER_MEL_BINS * WHISPER_FRAME_COUNT];
    static double window[WHISPER_FFT_SIZE];
    static double roots[WHISPER_FFT_SIZE * 2U];
    static double filters[(WHISPER_FFT_SIZE / 2U + 1U) * WHISPER_MEL_BINS];
    int passed = whisper_cpu_projection(weights, bias, input, output, 3, 2) &&
        output[0] == -6.5f && output[1] == 0.5f &&
        !whisper_cpu_projection(weights, bias, input, output, 0, 2) &&
        !whisper_cpu_projection(weights, bias, input, output, 0xffffffffU, 2) &&
        whisper_cpu_layer_norm(norm_input, norm_scale, norm_bias, normalized, 2) &&
        normalized[0] == 4.0f && normalized[1] == 5.0f &&
        !whisper_cpu_layer_norm(norm_input, norm_scale, norm_bias, normalized, 0) &&
        whisper_cpu_gelu(0.0f) == 0.0f;
    if (passed) {
        passed = whisper_frontend_log_mel_samples(silence, window, roots, filters, mel) &&
            mel[0] == -1.5f && mel[WHISPER_MEL_BINS * WHISPER_FRAME_COUNT - 1] == -1.5f;
        union { unsigned int bits; float value; } invalid = { .bits = 0x7fc00000U };
        silence[0] = invalid.value;
        passed = passed && !whisper_frontend_log_mel_samples(silence, window, roots, filters, mel);
    }
    ExitProcess(passed ? 0 : 1);
}