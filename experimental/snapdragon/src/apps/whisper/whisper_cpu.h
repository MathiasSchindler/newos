#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_CPU_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_CPU_H

int whisper_cpu_projection(
    const float *weights, const float *bias, const float *input, float *output,
    unsigned int input_width, unsigned int output_width
);

int whisper_cpu_layer_norm(const float *input, const float *scale,
                           const float *bias, float *output, unsigned int width);
float whisper_cpu_gelu(float input);

#endif