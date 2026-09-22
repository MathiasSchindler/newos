#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_FRONTEND_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_FRONTEND_H

#include "whisper_model.h"

#define WHISPER_SAMPLE_RATE 16000U
#define WHISPER_SAMPLE_COUNT 480000U
#define WHISPER_FFT_SIZE 400U
#define WHISPER_FRAME_COUNT 3000U
#define WHISPER_MEL_BINS WHISPER_TINY_MEL_BINS
#define WHISPER_HIDDEN_SIZE WHISPER_TINY_WIDTH
#define WHISPER_ENCODER_FRAMES WHISPER_TINY_ENCODER_FRAMES

int whisper_frontend_log_mel(
    const char *primary_wav,
    const char *fallback_wav,
    const double *window,
    const double *roots,
    const double *mel_filters,
    float *output
);

int whisper_frontend_log_mel_window(
    const char *primary_wav,
    const char *fallback_wav,
    unsigned long long start_sample,
    unsigned long long *total_samples,
    const double *window,
    const double *roots,
    const double *mel_filters,
    float *output
);

int whisper_frontend_log_mel_samples(
    const float *samples,
    const double *window,
    const double *roots,
    const double *mel_filters,
    float *output
);

void whisper_frontend_pack_conv1(const float *log_mel, unsigned short *output);
void whisper_frontend_pack_conv2_width(
    const unsigned short *conv1,
    unsigned short *output,
    unsigned int width
);
void whisper_frontend_pack_conv2(const unsigned short *conv1, unsigned short *output);
float whisper_frontend_half_to_float(unsigned short value);
unsigned short whisper_frontend_float_to_half(float value);

#endif
