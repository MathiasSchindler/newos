#include "whisper_cpu.h"
#include "math.h"

int whisper_cpu_projection(
    const float *weights, const float *bias, const float *input, float *output,
    unsigned int input_width, unsigned int output_width
) {
    if (weights == 0 || bias == 0 || input == 0 || output == 0 ||
        input_width == 0 || output_width == 0 ||
        input_width > 0xffffffffU / output_width) return 0;
    for (unsigned int row = 0; row < output_width; ++row) {
        float sum = bias[row];
        for (unsigned int column = 0; column < input_width; ++column) {
            sum += weights[row * input_width + column] * input[column];
        }
        output[row] = sum;
    }
    return 1;
}

int whisper_cpu_layer_norm(const float *input, const float *scale,
                           const float *bias, float *output, unsigned int width) {
    double mean = 0.0;
    double variance = 0.0;
    if (!input || !scale || !bias || !output || !width) return 0;
    for (unsigned int index = 0; index < width; ++index) mean += input[index];
    mean /= width;
    for (unsigned int index = 0; index < width; ++index) {
        double centered = input[index] - mean;
        variance += centered * centered;
    }
    double inverse = 1.0 / math_sqrt(variance / width + 1.0e-5);
    for (unsigned int index = 0; index < width; ++index) {
        output[index] = (float)(((input[index] - mean) * inverse) * scale[index] + bias[index]);
    }
    return 1;
}

float whisper_cpu_gelu(float input) {
    float absolute = input < 0.0f ? -input : input;
    absolute *= 0.7071067811865475244f;
    float factor = 1.0f / (1.0f + 0.3275911f * absolute);
    float polynomial = 1.061405429f * factor - 1.453152027f;
    polynomial = polynomial * factor + 1.421413741f;
    polynomial = polynomial * factor - 0.284496736f;
    float erf = 1.0f - polynomial * factor * (float)math_exp(-(double)absolute * absolute);
    if (input < 0.0f) erf = -erf;
    return input * 0.5f * (1.0f + erf);
}