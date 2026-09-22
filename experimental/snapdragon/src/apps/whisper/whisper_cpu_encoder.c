#include "whisper_cpu_encoder.h"
#include "whisper_cpu.h"
#include "whisper_frontend.h"
#include "math.h"

enum { WIDTH = WHISPER_TINY_WIDTH, FRAMES = WHISPER_TINY_ENCODER_FRAMES,
       MEL_FRAMES = WHISPER_FRAME_COUNT, HEADS = WHISPER_TINY_ATTENTION_HEADS,
       HEAD_WIDTH = WIDTH / HEADS, FFN = WHISPER_TINY_FFN_WIDTH };

__declspec(dllimport) void *VirtualAlloc(void *address, unsigned long long size,
                                         unsigned int allocation_type, unsigned int protect);
__declspec(dllimport) int VirtualFree(void *address, unsigned long long size,
                                      unsigned int free_type);

struct WhisperCpuEncoder {
    float hidden[FRAMES * WIDTH];
    float normalized[FRAMES * WIDTH];
    float query[FRAMES * WIDTH];
    float key[FRAMES * WIDTH];
    float value[FRAMES * WIDTH];
    float attended[FRAMES * WIDTH];
    float conv1[MEL_FRAMES * WIDTH];
    float matrix[FFN * WIDTH];
    float scale[FFN];
    float bias[FFN];
    float vector[WIDTH];
    float feed_forward[FRAMES * FFN];
    float scores[FRAMES];
};

WhisperCpuEncoder *whisper_cpu_encoder_create(void) {
    return VirtualAlloc(0, sizeof(WhisperCpuEncoder), 0x3000U, 0x04U);
}

void whisper_cpu_encoder_destroy(WhisperCpuEncoder *encoder) {
    if (encoder) VirtualFree(encoder, 0, 0x8000U);
}

static int load(const WhisperIndexed *model, const char *name,
                unsigned int rows, unsigned int columns, float *target) {
    const WhisperTensorIndex *tensor = whisper_indexed_find(model, name);
    unsigned int rank = columns ? 2U : 1U;
    if (!tensor || tensor->type != WHISPER_TENSOR_F32 || tensor->rank != rank ||
        tensor->shape[0] != rows || (columns && tensor->shape[1] != columns)) return 0;
    return whisper_indexed_read(model, tensor, target,
                                (unsigned long long)rows * (columns ? columns : 1U) * sizeof(float));
}

static int load_conv(const WhisperIndexed *model, const char *name,
                     unsigned int input_channels, float *target) {
    const WhisperTensorIndex *tensor = whisper_indexed_find(model, name);
    if (!tensor || tensor->type != WHISPER_TENSOR_F32 || tensor->rank != 3 ||
        tensor->shape[0] != WIDTH || tensor->shape[1] != input_channels ||
        tensor->shape[2] != 3) return 0;
    return whisper_indexed_read(model, tensor, target,
                                (unsigned long long)WIDTH * input_channels * 3U * sizeof(float));
}

static int layer_name(char *name, const char *suffix, unsigned int layer) {
    const char *prefix = "model.encoder.layers.";
    unsigned int position = 0;
    while (*prefix) name[position++] = *prefix++;
    name[position++] = (char)('0' + layer);
    name[position++] = '.';
    while (*suffix) {
        if (position + 1 >= WHISPER_TENSOR_NAME_SIZE) return 0;
        name[position++] = *suffix++;
    }
    name[position] = 0;
    return 1;
}

static int load_layer(const WhisperIndexed *model, char *name,
                      unsigned int layer, const char *suffix,
                      unsigned int rows, unsigned int columns, float *target) {
    return layer_name(name, suffix, layer) && load(model, name, rows, columns, target);
}

static void project_frames(WhisperCpuEncoder *encoder, const float *input,
                           float *output, unsigned int count, unsigned int input_width,
                           unsigned int output_width) {
    for (unsigned int frame = 0; frame < count; ++frame) {
        whisper_cpu_projection(encoder->matrix, encoder->bias,
            input + frame * input_width, output + frame * output_width,
            input_width, output_width);
    }
}

static int conv(WhisperCpuEncoder *encoder, const WhisperIndexed *model,
                const float *input, float *output, unsigned int input_frames,
                unsigned int input_channels, unsigned int output_frames,
                unsigned int stride, const char *weight_name, const char *bias_name,
                int channel_major) {
    if (!load_conv(model, weight_name, input_channels, encoder->matrix) ||
        !load(model, bias_name, WIDTH, 0, encoder->bias)) return 0;
    for (unsigned int frame = 0; frame < output_frames; ++frame) {
        for (unsigned int channel = 0; channel < WIDTH; ++channel) {
            const float *weights = encoder->matrix + channel * input_channels * 3U;
            float sum = encoder->bias[channel];
            for (unsigned int source = 0; source < input_channels; ++source) {
                for (unsigned int tap = 0; tap < 3U; ++tap) {
                    int position = (int)(frame * stride + tap) - 1;
                    if (position >= 0 && (unsigned int)position < input_frames) {
                        float sample = channel_major ?
                            input[source * input_frames + (unsigned int)position] :
                            input[(unsigned int)position * input_channels + source];
                        sum += weights[source * 3U + tap] * sample;
                    }
                }
            }
            output[frame * WIDTH + channel] = whisper_cpu_gelu(sum);
        }
    }
    return 1;
}

static void attend(WhisperCpuEncoder *encoder) {
    for (unsigned int head = 0; head < HEADS; ++head) {
        for (unsigned int frame = 0; frame < FRAMES; ++frame) {
            const float *query = encoder->query + frame * WIDTH + head * HEAD_WIDTH;
            float maximum = -1.0e30f;
            double denominator = 0.0;
            for (unsigned int key_frame = 0; key_frame < FRAMES; ++key_frame) {
                const float *key = encoder->key + key_frame * WIDTH + head * HEAD_WIDTH;
                float score = 0.0f;
                for (unsigned int lane = 0; lane < HEAD_WIDTH; ++lane)
                    score += query[lane] * key[lane];
                score *= 0.125f;
                encoder->scores[key_frame] = score;
                if (score > maximum) maximum = score;
            }
            for (unsigned int key_frame = 0; key_frame < FRAMES; ++key_frame) {
                float score = (float)math_exp((double)encoder->scores[key_frame] - maximum);
                encoder->scores[key_frame] = score;
                denominator += score;
            }
            for (unsigned int lane = 0; lane < HEAD_WIDTH; ++lane) {
                double sum = 0.0;
                for (unsigned int key_frame = 0; key_frame < FRAMES; ++key_frame)
                    sum += (double)encoder->scores[key_frame] *
                        encoder->value[key_frame * WIDTH + head * HEAD_WIDTH + lane];
                encoder->attended[frame * WIDTH + head * HEAD_WIDTH + lane] =
                    (float)(sum / denominator);
            }
        }
    }
}

static int encode_layer(WhisperCpuEncoder *encoder, const WhisperIndexed *model,
                        unsigned int layer) {
    char name[WHISPER_TENSOR_NAME_SIZE];
    if (!load_layer(model, name, layer, "self_attn_layer_norm.weight", WIDTH, 0,
                    encoder->scale) ||
        !load_layer(model, name, layer, "self_attn_layer_norm.bias", WIDTH, 0,
                    encoder->bias)) return 0;
    for (unsigned int frame = 0; frame < FRAMES; ++frame)
        whisper_cpu_layer_norm(encoder->hidden + frame * WIDTH, encoder->scale,
            encoder->bias, encoder->normalized + frame * WIDTH, WIDTH);

    if (!load_layer(model, name, layer, "self_attn.q_proj.weight", WIDTH, WIDTH,
                    encoder->matrix) ||
        !load_layer(model, name, layer, "self_attn.q_proj.bias", WIDTH, 0,
                    encoder->bias)) return 0;
    project_frames(encoder, encoder->normalized, encoder->query, FRAMES, WIDTH, WIDTH);
    if (!load_layer(model, name, layer, "self_attn.k_proj.weight", WIDTH, WIDTH,
                    encoder->matrix)) return 0;
    for (unsigned int index = 0; index < WIDTH; ++index) encoder->bias[index] = 0.0f;
    project_frames(encoder, encoder->normalized, encoder->key, FRAMES, WIDTH, WIDTH);
    if (!load_layer(model, name, layer, "self_attn.v_proj.weight", WIDTH, WIDTH,
                    encoder->matrix) ||
        !load_layer(model, name, layer, "self_attn.v_proj.bias", WIDTH, 0,
                    encoder->bias)) return 0;
    project_frames(encoder, encoder->normalized, encoder->value, FRAMES, WIDTH, WIDTH);
    attend(encoder);
    if (!load_layer(model, name, layer, "self_attn.out_proj.weight", WIDTH, WIDTH,
                    encoder->matrix) ||
        !load_layer(model, name, layer, "self_attn.out_proj.bias", WIDTH, 0,
                    encoder->bias)) return 0;
    for (unsigned int frame = 0; frame < FRAMES; ++frame) {
        whisper_cpu_projection(encoder->matrix, encoder->bias,
            encoder->attended + frame * WIDTH, encoder->vector, WIDTH, WIDTH);
        for (unsigned int channel = 0; channel < WIDTH; ++channel)
            encoder->hidden[frame * WIDTH + channel] += encoder->vector[channel];
    }

    if (!load_layer(model, name, layer, "final_layer_norm.weight", WIDTH, 0,
                    encoder->scale) ||
        !load_layer(model, name, layer, "final_layer_norm.bias", WIDTH, 0,
                    encoder->bias)) return 0;
    for (unsigned int frame = 0; frame < FRAMES; ++frame)
        whisper_cpu_layer_norm(encoder->hidden + frame * WIDTH, encoder->scale,
            encoder->bias, encoder->normalized + frame * WIDTH, WIDTH);
    if (!load_layer(model, name, layer, "fc1.weight", FFN, WIDTH,
                    encoder->matrix) ||
        !load_layer(model, name, layer, "fc1.bias", FFN, 0, encoder->bias)) return 0;
    for (unsigned int frame = 0; frame < FRAMES; ++frame) {
        whisper_cpu_projection(encoder->matrix, encoder->bias,
            encoder->normalized + frame * WIDTH, encoder->feed_forward + frame * FFN,
            WIDTH, FFN);
    }
    for (unsigned int index = 0; index < FRAMES * FFN; ++index)
        encoder->feed_forward[index] = whisper_cpu_gelu(encoder->feed_forward[index]);
    if (!load_layer(model, name, layer, "fc2.weight", WIDTH, FFN,
                    encoder->matrix) ||
        !load_layer(model, name, layer, "fc2.bias", WIDTH, 0, encoder->bias)) return 0;
    for (unsigned int frame = 0; frame < FRAMES; ++frame) {
        whisper_cpu_projection(encoder->matrix, encoder->bias,
            encoder->feed_forward + frame * FFN, encoder->vector, FFN, WIDTH);
        for (unsigned int channel = 0; channel < WIDTH; ++channel)
            encoder->hidden[frame * WIDTH + channel] += encoder->vector[channel];
    }
    return 1;
}

int whisper_cpu_encode(WhisperCpuEncoder *encoder, const WhisperIndexed *model,
                       const float *mel, unsigned short *output) {
    if (!encoder || !model || !mel || !output) return 0;
    if (!conv(encoder, model, mel, encoder->conv1, MEL_FRAMES, WHISPER_MEL_BINS,
              MEL_FRAMES, 1, "model.encoder.conv1.weight", "model.encoder.conv1.bias", 1) ||
        !conv(encoder, model, encoder->conv1, encoder->hidden, MEL_FRAMES, WIDTH,
              FRAMES, 2, "model.encoder.conv2.weight", "model.encoder.conv2.bias", 0) ||
        !load(model, "model.encoder.embed_positions.weight", FRAMES, WIDTH,
              encoder->normalized)) return 0;
    for (unsigned int index = 0; index < FRAMES * WIDTH; ++index)
        encoder->hidden[index] += encoder->normalized[index];
    for (unsigned int layer = 0; layer < WHISPER_TINY_ENCODER_LAYERS; ++layer)
        if (!encode_layer(encoder, model, layer)) return 0;
    for (unsigned int index = 0; index < FRAMES * WIDTH; ++index)
        output[index] = whisper_frontend_float_to_half(encoder->hidden[index]);
    return 1;
}