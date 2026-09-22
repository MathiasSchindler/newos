#include "whisper_wav.h"

__declspec(dllimport) void __stdcall ExitProcess(unsigned int status);

static unsigned char header[44];
static unsigned long long file_position;
static unsigned long long file_size;
static unsigned long long physical_size;
static float window[WHISPER_WAV_WINDOW_SAMPLES];
static int active;

static void set_u16(unsigned char *bytes, unsigned int value) {
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8);
}

static void set_u32(unsigned char *bytes, unsigned int value) {
    set_u16(bytes, value);
    set_u16(bytes + 2, value >> 16);
}

static void make_wav(unsigned int samples) {
    for (unsigned int index = 0; index < sizeof(header); ++index) header[index] = 0;
    header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
    header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';
    header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
    header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
    set_u32(header + 4, 36 + samples * 4);
    set_u32(header + 16, 16);
    set_u16(header + 20, 3);
    set_u16(header + 22, 1);
    set_u32(header + 24, WHISPER_WAV_RATE);
    set_u32(header + 28, WHISPER_WAV_RATE * 4);
    set_u16(header + 32, 4);
    set_u16(header + 34, 32);
    set_u32(header + 40, samples * 4);
    file_size = 44ULL + samples * 4ULL;
    physical_size = file_size;
    file_position = 0;
    active = 0;
}

int platform_open_read(const char *path) {
    if (path == 0 || path[0] != 'w' || path[1] != 0) return -1;
    file_position = 0;
    active = 1;
    return 7;
}

long platform_read(int fd, void *buffer, unsigned long long count) {
    unsigned char *bytes = (unsigned char *)buffer;
    if (fd != 7 || !active || file_position >= physical_size) return 0;
    if (count > 7) count = 7;
    if (count > physical_size - file_position) count = physical_size - file_position;
    for (unsigned long long index = 0; index < count; ++index) {
        unsigned long long position = file_position + index;
        if (position < sizeof(header)) bytes[index] = header[position];
        else {
            union { float number; unsigned char bytes[4]; } value;
            value.number = (float)(((position - 44) / 4) % 256);
            bytes[index] = value.bytes[(position - 44) % 4];
        }
    }
    file_position += count;
    return (long)count;
}

long long platform_seek(int fd, long long offset, int whence) {
    long long result = offset + (whence == 2 ? (long long)physical_size : 0);
    if (fd != 7 || !active || result < 0 || (whence != 0 && whence != 2)) return -1;
    file_position = (unsigned long long)result;
    return result;
}

int platform_close(int fd) {
    if (fd != 7 || !active) return -1;
    active = 0;
    return 0;
}

static int check_window(unsigned int samples, unsigned long long windows) {
    WhisperWav wav;
    make_wav(samples);
    if (!whisper_wav_open(&wav, "w")) return 0;
    if (wav.sample_count != samples || whisper_wav_window_count(&wav) != windows) return 0;
    for (unsigned long long index = 0; index < windows; ++index) {
        unsigned long long start = index * WHISPER_WAV_HOP_SAMPLES;
        unsigned long long remaining = samples - start;
        unsigned int read_count = (unsigned int)(remaining < WHISPER_WAV_WINDOW_SAMPLES ?
            remaining : WHISPER_WAV_WINDOW_SAMPLES);
        if (!whisper_wav_read_window(&wav, index, window) ||
            window[0] != (float)(start % 256) ||
            window[read_count - 1] != (float)((start + read_count - 1) % 256) ||
            (read_count < WHISPER_WAV_WINDOW_SAMPLES && window[read_count] != 0.0f)) return 0;
    }
    if (whisper_wav_read_window(&wav, windows, window)) return 0;
    whisper_wav_close(&wav);
    return !active;
}

static int check_invalid(void) {
    WhisperWav wav;
    make_wav(100);
    physical_size -= 1;
    if (whisper_wav_open(&wav, "w") || active) return 0;
    make_wav(100);
    header[22] = 2;
    if (whisper_wav_open(&wav, "w") || active) return 0;
    make_wav(100);
    header[0] = 'X';
    if (whisper_wav_open(&wav, "w") || active) return 0;
    make_wav(0);
    if (whisper_wav_open(&wav, "w") || active) return 0;
    return 1;
}

void mainCRTStartup(void) {
    unsigned int status = !(check_window(1, 1) &&
        check_window(WHISPER_WAV_WINDOW_SAMPLES - 1, 1) &&
        check_window(WHISPER_WAV_WINDOW_SAMPLES, 1) &&
        check_window(WHISPER_WAV_WINDOW_SAMPLES + 1, 2) &&
        check_window(WHISPER_WAV_WINDOW_SAMPLES + WHISPER_WAV_HOP_SAMPLES, 2) &&
        check_window(WHISPER_WAV_WINDOW_SAMPLES + WHISPER_WAV_HOP_SAMPLES + 1, 3) &&
        check_invalid());
    ExitProcess(status);
}