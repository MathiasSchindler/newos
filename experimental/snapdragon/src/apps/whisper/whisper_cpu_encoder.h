#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_CPU_ENCODER_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_CPU_ENCODER_H

#include "whisper_indexed.h"

typedef struct WhisperCpuEncoder WhisperCpuEncoder;
typedef int (*WhisperEncoderProjection)(void *context, const float *weights,
    const float *bias, const float *input, float *output,
    unsigned int frames, unsigned int input_width, unsigned int output_width);
WhisperCpuEncoder *whisper_cpu_encoder_create(const WhisperModelConfig *config);
void whisper_cpu_encoder_destroy(WhisperCpuEncoder *encoder);
void whisper_cpu_encoder_set_projection(WhisperCpuEncoder *encoder,
    WhisperEncoderProjection projection, void *context, int all_layers);
int whisper_cpu_encode(WhisperCpuEncoder *encoder, const WhisperIndexed *model,
                       const float *mel, unsigned short *output);

#endif