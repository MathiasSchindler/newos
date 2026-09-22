#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_WAV_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_WHISPER_WAV_H

enum {
    WHISPER_WAV_RATE = 16000,
    WHISPER_WAV_WINDOW_SAMPLES = 30 * WHISPER_WAV_RATE,
    WHISPER_WAV_HOP_SAMPLES = 25 * WHISPER_WAV_RATE
};

typedef struct WhisperWav {
    int fd;
    unsigned long long data_offset;
    unsigned long long sample_count;
} WhisperWav;

int whisper_wav_open(WhisperWav *wav, const char *path);
void whisper_wav_close(WhisperWav *wav);
unsigned long long whisper_wav_window_count(const WhisperWav *wav);
int whisper_wav_read_window(
    const WhisperWav *wav, unsigned long long index, float *samples
);
int whisper_wav_read_at(
    const WhisperWav *wav, unsigned long long start_sample, float *samples
);

#endif