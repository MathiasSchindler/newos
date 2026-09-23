#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_HMX_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_HMX_H

typedef struct WhisperHmx WhisperHmx;
WhisperHmx *whisper_hmx_open(const char *driver_path);
int whisper_hmx_close(WhisperHmx *hmx);
int whisper_hmx_projection(void *context, const float *weights, const float *bias,
                           const float *input, float *output, unsigned int frames,
                           unsigned int input_width, unsigned int output_width);
unsigned int whisper_hmx_submissions(const WhisperHmx *hmx);
unsigned int whisper_hmx_invoke_milliseconds(const WhisperHmx *hmx);
unsigned int whisper_hmx_width_invoke_calls(const WhisperHmx *hmx, unsigned int input_width);
unsigned int whisper_hmx_width_invoke_milliseconds(const WhisperHmx *hmx, unsigned int input_width);
int whisper_hmx_batched(const WhisperHmx *hmx);
int whisper_hmx_grouped(const WhisperHmx *hmx);
void whisper_hmx_require_batch(WhisperHmx *hmx);
unsigned int whisper_hmx_last_error(void);

#endif