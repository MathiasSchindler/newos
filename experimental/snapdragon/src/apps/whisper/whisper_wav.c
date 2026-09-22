#include "whisper_wav.h"
#include "platform.h"

static unsigned int wav_u16(const unsigned char *bytes) {
    return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8);
}

static unsigned int wav_u32(const unsigned char *bytes) {
    return wav_u16(bytes) | (wav_u16(bytes + 2) << 16);
}

static int wav_read(int fd, void *buffer, unsigned int size) {
    unsigned char *bytes = (unsigned char *)buffer;
    while (size != 0) {
        long count = platform_read(fd, bytes, size);
        if (count <= 0 || (unsigned long)count > size) return 0;
        bytes += count;
        size -= (unsigned int)count;
    }
    return 1;
}

int whisper_wav_open(WhisperWav *wav, const char *path) {
    unsigned char header[12];
    unsigned char chunk[8];
    unsigned char format[16];
    unsigned long long end;
    unsigned long long cursor = 12;
    int valid_format = 0;
    int found_data = 0;
    wav->fd = -1;
    wav->sample_count = 0;
    wav->data_offset = 0;
    if (path == 0) return 0;
    wav->fd = platform_open_read(path);
    if (wav->fd < 0) return 0;
    if (!wav_read(wav->fd, header, sizeof(header)) ||
        header[0] != 'R' || header[1] != 'I' || header[2] != 'F' || header[3] != 'F' ||
        header[8] != 'W' || header[9] != 'A' || header[10] != 'V' || header[11] != 'E' ||
        wav_u32(header + 4) < 4) goto fail;
    end = (unsigned long long)wav_u32(header + 4) + 8;
    while (cursor + 8 <= end) {
        unsigned int size;
        unsigned long long next;
        if (!wav_read(wav->fd, chunk, sizeof(chunk))) goto fail;
        size = wav_u32(chunk + 4);
        cursor += 8;
        next = cursor + (unsigned long long)size + (size & 1U);
        if (next > end) goto fail;
        if (chunk[0] == 'f' && chunk[1] == 'm' && chunk[2] == 't' && chunk[3] == ' ') {
            if (size < sizeof(format) || !wav_read(wav->fd, format, sizeof(format))) goto fail;
            valid_format = wav_u16(format) == 3 && wav_u16(format + 2) == 1 &&
                wav_u32(format + 4) == WHISPER_WAV_RATE &&
                wav_u32(format + 8) == WHISPER_WAV_RATE * sizeof(float) &&
                wav_u16(format + 12) == sizeof(float) && wav_u16(format + 14) == 32;
        } else if (chunk[0] == 'd' && chunk[1] == 'a' &&
                   chunk[2] == 't' && chunk[3] == 'a') {
            if (!valid_format || size % sizeof(float) != 0) goto fail;
            wav->data_offset = cursor;
            wav->sample_count = size / sizeof(float);
            found_data = 1;
            break;
        }
        if (platform_seek(wav->fd, (long long)next, 0) != (long long)next) goto fail;
        cursor = next;
    }
    if (!valid_format || !found_data || wav->sample_count == 0 ||
        platform_seek(wav->fd, 0, 2) < (long long)(wav->data_offset + wav->sample_count * sizeof(float))) goto fail;
    return 1;
fail:
    whisper_wav_close(wav);
    return 0;
}

void whisper_wav_close(WhisperWav *wav) {
    if (wav->fd >= 0) platform_close(wav->fd);
    wav->fd = -1;
    wav->sample_count = 0;
    wav->data_offset = 0;
}

unsigned long long whisper_wav_window_count(const WhisperWav *wav) {
    if (wav->sample_count == 0) return 0;
    if (wav->sample_count <= WHISPER_WAV_WINDOW_SAMPLES) return 1;
    return 1 + (wav->sample_count - WHISPER_WAV_WINDOW_SAMPLES +
                WHISPER_WAV_HOP_SAMPLES - 1) / WHISPER_WAV_HOP_SAMPLES;
}

int whisper_wav_read_window(const WhisperWav *wav, unsigned long long index, float *samples) {
    if (index >= whisper_wav_window_count(wav)) return 0;
    return whisper_wav_read_at(wav, index * WHISPER_WAV_HOP_SAMPLES, samples);
}

int whisper_wav_read_at(const WhisperWav *wav, unsigned long long start, float *samples) {
    unsigned long long available;
    unsigned int count;
    if (samples == 0 || wav->fd < 0 || start >= wav->sample_count) return 0;
    available = wav->sample_count - start;
    count = (unsigned int)(available < WHISPER_WAV_WINDOW_SAMPLES ?
        available : WHISPER_WAV_WINDOW_SAMPLES);
    if (platform_seek(wav->fd, (long long)(wav->data_offset + start * sizeof(float)), 0) !=
            (long long)(wav->data_offset + start * sizeof(float)) ||
        !wav_read(wav->fd, samples, count * sizeof(float))) return 0;
    for (unsigned int position = count; position < WHISPER_WAV_WINDOW_SAMPLES; ++position) {
        samples[position] = 0.0f;
    }
    return 1;
}