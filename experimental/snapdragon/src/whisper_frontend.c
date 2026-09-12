#include "whisper_frontend.h"
#include "../../../src/shared/math.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long long usize;

__declspec(dllimport) int CloseHandle(void *handle);
__declspec(dllimport) void *CreateFileA(
    const char *name,
    u32 access,
    u32 sharing,
    void *security,
    u32 creation,
    u32 attributes,
    void *template_file
);
__declspec(dllimport) int ReadFile(
    void *handle,
    void *buffer,
    u32 size,
    u32 *read,
    void *overlapped
);
__declspec(dllimport) int SetFilePointerEx(
    void *handle,
    long long distance,
    long long *new_position,
    u32 move_method
);

static float waveform[WHISPER_SAMPLE_COUNT];
static double stage_real[20][20];
static double stage_imaginary[20][20];
static double frame_samples[WHISPER_FFT_SIZE];
static double magnitudes[WHISPER_FFT_SIZE / 2U + 1U];

static u16 read_u16_le(const u8 *bytes) {
    return (u16)((u16)bytes[0] | ((u16)bytes[1] << 8U));
}

static u32 read_u32_le(const u8 *bytes) {
    return (u32)bytes[0] | ((u32)bytes[1] << 8U) |
        ((u32)bytes[2] << 16U) | ((u32)bytes[3] << 24U);
}

static int read_exact(void *handle, void *buffer, u32 size) {
    u8 *bytes = buffer;
    while (size != 0U) {
        u32 count = 0U;
        if (!ReadFile(handle, bytes, size, &count, 0) || count == 0U) return 0;
        bytes += count;
        size -= count;
    }
    return 1;
}

static int skip_exact(void *handle, u32 size) {
    u8 scratch[256];
    while (size != 0U) {
        u32 chunk = size > sizeof(scratch) ? sizeof(scratch) : size;
        if (!read_exact(handle, scratch, chunk)) return 0;
        size -= chunk;
    }
    return 1;
}

static int load_wav(
    const char *primary,
    const char *fallback,
    u64 start_sample,
    u64 *total_samples
) {
    void *invalid_handle = (void *)(usize)-1;
    void *handle = CreateFileA(primary, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
    u8 header[12];
    u8 chunk_header[8];
    u8 format[40];
    u32 sample_count = 0U;
    u32 index;
    int format_valid = 0;
    int data_found = 0;

    if (handle == invalid_handle && fallback != 0) {
        handle = CreateFileA(fallback, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
    }
    if (handle == invalid_handle) return 0;
    for (index = 0U; index < WHISPER_SAMPLE_COUNT; ++index) waveform[index] = 0.0f;
    if (!read_exact(handle, header, sizeof(header)) ||
        header[0] != 'R' || header[1] != 'I' || header[2] != 'F' || header[3] != 'F' ||
        header[8] != 'W' || header[9] != 'A' || header[10] != 'V' || header[11] != 'E') {
        CloseHandle(handle);
        return -1;
    }
    while (read_exact(handle, chunk_header, sizeof(chunk_header))) {
        u32 chunk_size = read_u32_le(chunk_header + 4U);
        if (chunk_header[0] == 'f' && chunk_header[1] == 'm' &&
            chunk_header[2] == 't' && chunk_header[3] == ' ') {
            u32 kept = chunk_size > sizeof(format) ? sizeof(format) : chunk_size;
            if (!read_exact(handle, format, kept) || !skip_exact(handle, chunk_size - kept)) {
                CloseHandle(handle);
                return -1;
            }
            format_valid = kept >= 16U && read_u16_le(format) == 3U &&
                read_u16_le(format + 2U) == 1U && read_u32_le(format + 4U) == WHISPER_SAMPLE_RATE &&
                read_u16_le(format + 12U) == 4U && read_u16_le(format + 14U) == 32U;
        } else if (chunk_header[0] == 'd' && chunk_header[1] == 'a' &&
            chunk_header[2] == 't' && chunk_header[3] == 'a') {
            u64 available_samples = chunk_size / sizeof(float);
            u32 start_bytes;
            u32 kept;
            if (total_samples != 0) *total_samples = available_samples;
            if (start_sample > available_samples ||
                start_sample > 0xffffffffULL / sizeof(float)) {
                CloseHandle(handle);
                return -1;
            }
            start_bytes = (u32)(start_sample * sizeof(float));
            if (start_bytes != 0U && !SetFilePointerEx(
                    handle, (long long)start_bytes, 0, 1U)) {
                CloseHandle(handle);
                return -1;
            }
            kept = chunk_size - start_bytes;
            if (kept > sizeof(waveform)) kept = sizeof(waveform);
            kept -= kept % sizeof(float);
            if (!read_exact(handle, waveform, kept)) {
                CloseHandle(handle);
                return -1;
            }
            sample_count = kept / sizeof(float);
            data_found = 1;
            if (format_valid) break;
            if (!skip_exact(handle, chunk_size - start_bytes - kept)) {
                CloseHandle(handle);
                return -1;
            }
        } else if (!skip_exact(handle, chunk_size)) {
            CloseHandle(handle);
            return -1;
        }
        if ((chunk_size & 1U) != 0U && !skip_exact(handle, 1U)) {
            CloseHandle(handle);
            return -1;
        }
        if (format_valid && data_found) break;
    }
    CloseHandle(handle);
    if (!format_valid || !data_found) return -1;
    for (index = 0U; index < sample_count; ++index) {
        union { float value; u32 bits; } sample;
        sample.value = waveform[index];
        if ((sample.bits & 0x7f800000U) == 0x7f800000U) return -1;
    }
    return 1;
}

static double padded_sample(u32 index) {
    if (index < WHISPER_FFT_SIZE / 2U) {
        return waveform[WHISPER_FFT_SIZE / 2U - index];
    }
    if (index < WHISPER_SAMPLE_COUNT + WHISPER_FFT_SIZE / 2U) {
        return waveform[index - WHISPER_FFT_SIZE / 2U];
    }
    return waveform[2U * WHISPER_SAMPLE_COUNT + WHISPER_FFT_SIZE / 2U - 2U - index];
}

float whisper_frontend_half_to_float(u16 value) {
    union { u32 bits; float value; } converted;
    u32 sign = (u32)(value & 0x8000U) << 16U;
    u32 exponent = (value >> 10U) & 31U;
    u32 fraction = value & 1023U;
    if (exponent == 0U) {
        if (fraction == 0U) {
            converted.bits = sign;
            return converted.value;
        }
        exponent = 113U;
        while ((fraction & 1024U) == 0U) {
            fraction <<= 1U;
            exponent -= 1U;
        }
        fraction &= 1023U;
    } else if (exponent == 31U) {
        converted.bits = sign | 0x7f800000U | (fraction << 13U);
        return converted.value;
    } else {
        exponent += 112U;
    }
    converted.bits = sign | (exponent << 23U) | (fraction << 13U);
    return converted.value;
}

u16 whisper_frontend_float_to_half(float value) {
    union { float value; u32 bits; } source;
    u32 sign;
    u32 exponent;
    u32 fraction;
    source.value = value;
    sign = (source.bits >> 16U) & 0x8000U;
    exponent = (source.bits >> 23U) & 0xffU;
    fraction = source.bits & 0x7fffffU;
    if (exponent == 0xffU) {
        return (u16)(sign | 0x7c00U | (fraction != 0U ? 0x0200U : 0U));
    }
    if (exponent > 142U) return (u16)(sign | 0x7c00U);
    if (exponent < 113U) {
        u32 shift;
        u32 mantissa;
        u32 remainder;
        u32 halfway;
        if (exponent < 102U) return (u16)sign;
        mantissa = fraction | 0x800000U;
        shift = 126U - exponent;
        remainder = mantissa & ((1U << shift) - 1U);
        halfway = 1U << (shift - 1U);
        mantissa >>= shift;
        if (remainder > halfway || (remainder == halfway && (mantissa & 1U))) mantissa += 1U;
        return (u16)(sign | mantissa);
    }
    fraction += 0x00000fffU + ((fraction >> 13U) & 1U);
    if ((fraction & 0x00800000U) != 0U) {
        fraction = 0U;
        exponent += 1U;
        if (exponent > 142U) return (u16)(sign | 0x7c00U);
    }
    return (u16)(sign | ((exponent - 112U) << 10U) | (fraction >> 13U));
}

int whisper_frontend_log_mel_window(
    const char *primary_wav,
    const char *fallback_wav,
    u64 start_sample,
    u64 *total_samples,
    const double *window,
    const double *roots,
    const double *mel_filters,
    float *output
) {
    double maximum = -1.0e30;
    u32 frame;
    int loaded = load_wav(primary_wav, fallback_wav, start_sample, total_samples);
    if (loaded != 1) return loaded;

    for (frame = 0U; frame < WHISPER_FRAME_COUNT; ++frame) {
        u32 n1;
        u32 k2;
        u32 bin;
        u32 mel;
        for (n1 = 0U; n1 < WHISPER_FFT_SIZE; ++n1) {
            frame_samples[n1] = padded_sample(frame * 160U + n1) * window[n1];
        }
        for (n1 = 0U; n1 < 20U; ++n1) {
            for (k2 = 0U; k2 < 20U; ++k2) {
                double real = 0.0;
                double imaginary = 0.0;
                u32 n2;
                for (n2 = 0U; n2 < 20U; ++n2) {
                    u32 root = (n2 * k2 * 20U) % WHISPER_FFT_SIZE;
                    double sample = frame_samples[n1 + 20U * n2];
                    real += sample * roots[root * 2U];
                    imaginary += sample * roots[root * 2U + 1U];
                }
                stage_real[n1][k2] = real;
                stage_imaginary[n1][k2] = imaginary;
            }
        }
        for (bin = 0U; bin <= WHISPER_FFT_SIZE / 2U; ++bin) {
            double real = 0.0;
            double imaginary = 0.0;
            u32 inner_bin = bin % 20U;
            for (n1 = 0U; n1 < 20U; ++n1) {
                u32 root = (n1 * bin) % WHISPER_FFT_SIZE;
                double root_real = roots[root * 2U];
                double root_imaginary = roots[root * 2U + 1U];
                real += stage_real[n1][inner_bin] * root_real -
                    stage_imaginary[n1][inner_bin] * root_imaginary;
                imaginary += stage_real[n1][inner_bin] * root_imaginary +
                    stage_imaginary[n1][inner_bin] * root_real;
            }
            {
                float rounded_real = (float)real;
                float rounded_imaginary = (float)imaginary;
                magnitudes[bin] = (double)rounded_real * rounded_real +
                    (double)rounded_imaginary * rounded_imaginary;
            }
        }
        for (mel = 0U; mel < WHISPER_MEL_BINS; ++mel) {
            double sum = 0.0;
            for (bin = 0U; bin <= WHISPER_FFT_SIZE / 2U; ++bin) {
                sum += mel_filters[bin * WHISPER_MEL_BINS + mel] * magnitudes[bin];
            }
            if (sum < 1.0e-10) sum = 1.0e-10;
            sum = math_log10(sum);
            output[mel * WHISPER_FRAME_COUNT + frame] = (float)sum;
            if (sum > maximum) maximum = sum;
        }
    }
    maximum -= 8.0;
    for (frame = 0U; frame < WHISPER_MEL_BINS * WHISPER_FRAME_COUNT; ++frame) {
        double value = output[frame];
        if (value < maximum) value = maximum;
        output[frame] = (float)((value + 4.0) / 4.0);
    }
    return 1;
}

int whisper_frontend_log_mel(
    const char *primary_wav,
    const char *fallback_wav,
    const double *window,
    const double *roots,
    const double *mel_filters,
    float *output
) {
    return whisper_frontend_log_mel_window(
        primary_wav, fallback_wav, 0U, 0,
        window, roots, mel_filters, output
    );
}

void whisper_frontend_pack_conv1(const float *log_mel, u16 *output) {
    u32 frame;
    for (frame = 0U; frame < WHISPER_FRAME_COUNT; ++frame) {
        u32 channel;
        for (channel = 0U; channel < WHISPER_MEL_BINS; ++channel) {
            u32 tap;
            for (tap = 0U; tap < 3U; ++tap) {
                int source_frame = (int)frame + (int)tap - 1;
                float value = source_frame < 0 || source_frame >= (int)WHISPER_FRAME_COUNT
                    ? 0.0f
                    : log_mel[channel * WHISPER_FRAME_COUNT + (u32)source_frame];
                output[(frame * WHISPER_MEL_BINS + channel) * 3U + tap] =
                    whisper_frontend_float_to_half(value);
            }
        }
    }
}

void whisper_frontend_pack_conv2_width(const u16 *conv1, u16 *output, u32 width) {
    u32 frame;
    for (frame = 0U; frame < WHISPER_ENCODER_FRAMES; ++frame) {
        u32 center = frame * 2U;
        u32 channel;
        for (channel = 0U; channel < width; ++channel) {
            u32 tap;
            for (tap = 0U; tap < 3U; ++tap) {
                int source_frame = (int)center + (int)tap - 1;
                u16 value = source_frame < 0 || source_frame >= (int)WHISPER_FRAME_COUNT
                    ? 0U
                    : conv1[(u32)source_frame * width + channel];
                output[(frame * width + channel) * 3U + tap] = value;
            }
        }
    }
}

void whisper_frontend_pack_conv2(const u16 *conv1, u16 *output) {
    whisper_frontend_pack_conv2_width(conv1, output, WHISPER_HIDDEN_SIZE);
}
