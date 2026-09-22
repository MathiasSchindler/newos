#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_CPU_ENCODER_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_CPU_ENCODER_H

#include "whisper_indexed.h"

typedef struct WhisperCpuEncoder WhisperCpuEncoder;
WhisperCpuEncoder *whisper_cpu_encoder_create(void);
void whisper_cpu_encoder_destroy(WhisperCpuEncoder *encoder);
int whisper_cpu_encode(WhisperCpuEncoder *encoder, const WhisperIndexed *model,
                       const float *mel, unsigned short *output);

#endif