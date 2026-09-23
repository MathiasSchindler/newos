#include "whisper_wav.h"
#include "whisper_indexed.h"
#include "whisper_cpu.h"
#include "whisper_artifact.h"
#include "whisper_frontend.h"
#include "whisper_cpu_encoder.h"
#include "whisper_hmx.h"
#include "whisper_decoder.h"
#include "math.h"
#include "platform.h"

__declspec(dllimport) const char *__stdcall GetCommandLineA(void);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int status);
__declspec(dllimport) unsigned int __stdcall GetConsoleOutputCP(void);
__declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int code_page);

static void write_text(const char *text) {
    const char *end = text;
    while (*end != 0) ++end;
    while (text != end) {
        long written = platform_write(1, text, (unsigned long long)(end - text));
        if (written <= 0) ExitProcess(3);
        text += written;
    }
}

static void write_token_bytes(const char *data, unsigned int size) {
    while (size) {
        long count = platform_write(1, data, size);
        if (count <= 0 || (unsigned long)count > size) ExitProcess(3);
        data += count;
        size -= (unsigned int)count;
    }
}

static void write_number(unsigned long long value) {
    char digits[21];
    unsigned int position = sizeof(digits) - 1;
    digits[position] = 0;
    do {
        digits[--position] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    write_text(digits + position);
}

static void write_hex(unsigned long long value) {
    static const char alphabet[] = "0123456789abcdef";
    char digits[17];
    digits[16] = 0;
    for (unsigned int index = 0; index < 16; ++index) {
        digits[15 - index] = alphabet[value & 15U];
        value >>= 4;
    }
    write_text(digits);
}

static const char *next_argument(const char *cursor, char *output, unsigned int capacity) {
    unsigned int length = 0;
    int quoted = 0;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (*cursor == 0) return 0;
    while (*cursor != 0) {
        char letter = *cursor++;
        if (letter == '"') quoted = !quoted;
        else if (!quoted && (letter == ' ' || letter == '\t')) break;
        else {
            if (length + 1 >= capacity) return 0;
            output[length++] = letter;
        }
    }
    if (quoted || length == 0) return 0;
    output[length] = 0;
    return cursor;
}

static int same(const char *left, const char *right) {
    while (*left != 0 && *left == *right) { ++left; ++right; }
    return *left == *right;
}

static int read_exact_file(const char *path, void *buffer, unsigned int size) {
    int fd;
    int valid;
    fd = platform_open_read(path);
    if (fd < 0) return 0;
    valid = platform_seek(fd, 0, PLATFORM_SEEK_END) == size &&
        platform_seek(fd, 0, PLATFORM_SEEK_SET) == 0;
    for (unsigned int offset = 0; valid && offset < size;) {
        long count = platform_read(fd, (unsigned char *)buffer + offset, size - offset);
        if (count <= 0 || (unsigned long)count > size - offset) valid = 0;
        else offset += (unsigned int)count;
    }
    platform_close(fd);
    return valid;
}

static int read_frontend_constant(const char *directory, const char *name,
                                  void *buffer, unsigned int size) {
    char path[1024];
    unsigned int length = 0;
    while (directory[length] && length + 1 < sizeof(path)) {
        path[length] = directory[length];
        ++length;
    }
    if (directory[length] || length == 0 || length + 1 >= sizeof(path)) return 0;
    path[length++] = '/';
    while (*name) {
        if (length + 1 >= sizeof(path)) return 0;
        path[length++] = *name++;
    }
    path[length] = 0;
    return read_exact_file(path, buffer, size);
}

static int probe_mel(const char *path, const char *directory, const char *reference) {
    static float samples[WHISPER_WAV_WINDOW_SAMPLES];
    static float log_mel[WHISPER_MEL_BINS * WHISPER_FRAME_COUNT];
    static float expected[WHISPER_MEL_BINS * WHISPER_FRAME_COUNT];
    static double window[WHISPER_FFT_SIZE];
    static double roots[WHISPER_FFT_SIZE * 2U];
    static double filters[(WHISPER_FFT_SIZE / 2U + 1U) * WHISPER_MEL_BINS];
    WhisperWav wav;
    if (!read_frontend_constant(directory, "hann-window-f64.bin", window, sizeof(window)) ||
        !read_frontend_constant(directory, "dft-roots-f64.bin", roots, sizeof(roots)) ||
        !read_frontend_constant(directory, "mel-filters-f64.bin", filters, sizeof(filters)) ||
        (reference != 0 && !read_exact_file(reference, expected, sizeof(expected))) ||
        !whisper_wav_open(&wav, path)) return 0;
    for (unsigned int index = 0; index < WHISPER_FFT_SIZE; ++index) {
        if (!math_is_finite(window[index])) { whisper_wav_close(&wav); return 0; }
    }
    for (unsigned int index = 0; index < WHISPER_FFT_SIZE * 2U; ++index) {
        if (!math_is_finite(roots[index])) { whisper_wav_close(&wav); return 0; }
    }
    for (unsigned int index = 0; index < (WHISPER_FFT_SIZE / 2U + 1U) * WHISPER_MEL_BINS; ++index) {
        if (!math_is_finite(filters[index])) { whisper_wav_close(&wav); return 0; }
    }
    if (reference != 0 && whisper_wav_window_count(&wav) != 1) {
        whisper_wav_close(&wav);
        return 0;
    }
    for (unsigned long long index = 0; index < whisper_wav_window_count(&wav); ++index) {
        if (!whisper_wav_read_window(&wav, index, samples) ||
            !whisper_frontend_log_mel_samples(samples, window, roots, filters, log_mel)) {
            whisper_wav_close(&wav);
            return 0;
        }
        if (reference != 0) {
            unsigned int outside_tolerance = 0;
            unsigned long long maximum_millionths = 0;
            for (unsigned int element = 0; element < WHISPER_MEL_BINS * WHISPER_FRAME_COUNT; ++element) {
                double difference = (double)log_mel[element] - expected[element];
                if (difference < 0.0) difference = -difference;
                if (!(difference <= 0.0001)) ++outside_tolerance;
                if (difference < 1.0 &&
                    (unsigned long long)(difference * 1000000.0) > maximum_millionths)
                    maximum_millionths = (unsigned long long)(difference * 1000000.0);
            }
            if (outside_tolerance) {
                whisper_wav_close(&wav);
                return 0;
            }
            write_text("max_delta_millionths=");
            write_number(maximum_millionths);
            write_text("\n");
        }
        write_text("window=");
        write_number(index);
        write_text(" log_mel_fnv64=");
        write_hex(whisper_artifact_hash_update(
            WHISPER_ARTIFACT_HASH_OFFSET_BASIS, log_mel, sizeof(log_mel)));
        write_text("\n");
    }
    whisper_wav_close(&wav);
    return 1;
}

static int transcribe(const char *path, const char *model_path, const char *directory,
                      const char *driver_path, int full_encoder,
                      const WhisperModelConfig *config) {
    static WhisperIndexed model;
    static float samples[WHISPER_WAV_WINDOW_SAMPLES];
    static float mel[WHISPER_MEL_BINS * WHISPER_FRAME_COUNT];
    static unsigned short encoded[WHISPER_BASE_ENCODER_FRAMES * WHISPER_BASE_WIDTH];
    static double window[WHISPER_FFT_SIZE];
    static double roots[WHISPER_FFT_SIZE * 2U];
    static double filters[(WHISPER_FFT_SIZE / 2U + 1U) * WHISPER_MEL_BINS];
    WhisperCpuEncoder *encoder;
    WhisperDecoder *decoder;
    WhisperHmx *hmx = 0;
    WhisperWav wav;
    if (!read_frontend_constant(directory, "hann-window-f64.bin", window, sizeof(window)) ||
        !read_frontend_constant(directory, "dft-roots-f64.bin", roots, sizeof(roots)) ||
        !read_frontend_constant(directory, "mel-filters-f64.bin", filters, sizeof(filters)) ||
        !whisper_wav_open(&wav, path)) return 0;
    if (!whisper_indexed_open(&model, model_path, config)) {
        whisper_wav_close(&wav);
        return 0;
    }
    encoder = whisper_cpu_encoder_create(config);
    decoder = whisper_decoder_load(config);
    if (encoder && decoder && driver_path) hmx = whisper_hmx_open(driver_path);
    if (!encoder || !decoder || (driver_path && !hmx)) {
        if (driver_path && !hmx) {
            write_text("hmx.error=");
            write_number(whisper_hmx_last_error());
            write_text("\n");
        }
        whisper_hmx_close(hmx);
        whisper_cpu_encoder_destroy(encoder);
        whisper_decoder_shutdown(decoder);
        whisper_indexed_close(&model);
        whisper_wav_close(&wav);
        return 0;
    }
    if (hmx) {
        if (full_encoder) whisper_hmx_require_batch(hmx);
        whisper_cpu_encoder_set_projection(encoder, whisper_hmx_projection, hmx, full_encoder);
    }
    for (unsigned long long index = 0;
         index < 1 + (wav.sample_count - 1) / WHISPER_WAV_WINDOW_SAMPLES; ++index) {
        if (!whisper_wav_read_at(&wav, index * WHISPER_WAV_WINDOW_SAMPLES, samples) ||
            !whisper_frontend_log_mel_samples(samples, window, roots, filters, mel) ||
            !whisper_cpu_encode(encoder, &model, mel, encoded) ||
            whisper_decoder_transcribe(decoder, encoded, 224, write_token_bytes) < 0) {
            if (hmx) {
                write_text("hmx.error=");
                write_number(whisper_hmx_last_error());
                write_text("\n");
            }
            whisper_hmx_close(hmx);
            whisper_decoder_shutdown(decoder);
            whisper_cpu_encoder_destroy(encoder);
            whisper_indexed_close(&model);
            whisper_wav_close(&wav);
            return 0;
        }
        write_text("\n");
    }
    if (hmx) {
        unsigned int submissions = whisper_hmx_submissions(hmx);
        write_text("hmx.projection.submissions=");
        write_number(submissions);
        write_text("\n");
        write_text("hmx.projection.batched=");
        write_number((unsigned int)whisper_hmx_batched(hmx));
        write_text("\n");
        write_text("hmx.projection.grouped=");
        write_number((unsigned int)whisper_hmx_grouped(hmx));
        write_text("\n");
        write_text("hmx.invoke.milliseconds=");
        write_number(whisper_hmx_invoke_milliseconds(hmx));
        write_text("\n");
        {
            static const unsigned int widths[] = {384, 512, 1536, 2048};
            for (unsigned int index = 0; index < 4; ++index) {
                write_text("hmx.invoke.width");
                write_number(widths[index]);
                write_text(".calls=");
                write_number(whisper_hmx_width_invoke_calls(hmx, widths[index]));
                write_text("\n");
                write_text("hmx.invoke.width");
                write_number(widths[index]);
                write_text(".milliseconds=");
                write_number(whisper_hmx_width_invoke_milliseconds(hmx, widths[index]));
                write_text("\n");
            }
        }
        if (!submissions || !whisper_hmx_close(hmx)) {
            whisper_decoder_shutdown(decoder);
            whisper_cpu_encoder_destroy(encoder);
            whisper_indexed_close(&model);
            whisper_wav_close(&wav);
            return 0;
        }
    }
    whisper_decoder_shutdown(decoder);
    whisper_cpu_encoder_destroy(encoder);
    whisper_indexed_close(&model);
    whisper_wav_close(&wav);
    return 1;
}

static int probe_projection(const WhisperIndexed *indexed) {
    static float weights[WHISPER_TINY_WIDTH * WHISPER_TINY_WIDTH];
    static float bias[WHISPER_TINY_WIDTH];
    static float input[WHISPER_TINY_WIDTH];
    static float output[WHISPER_TINY_WIDTH];
    const WhisperTensorIndex *matrix = whisper_indexed_find(
        indexed, "model.encoder.layers.0.self_attn.q_proj.weight"
    );
    const WhisperTensorIndex *offset = whisper_indexed_find(
        indexed, "model.encoder.layers.0.self_attn.q_proj.bias"
    );
    if (matrix == 0 || offset == 0 || matrix->type != WHISPER_TENSOR_F32 ||
        matrix->rank != 2 || matrix->shape[0] != WHISPER_TINY_WIDTH ||
        matrix->shape[1] != WHISPER_TINY_WIDTH ||
        offset->type != WHISPER_TENSOR_F32 || offset->rank != 1 ||
        offset->shape[0] != WHISPER_TINY_WIDTH ||
        !whisper_indexed_read(indexed, matrix, weights, sizeof(weights)) ||
        !whisper_indexed_read(indexed, offset, bias, sizeof(bias))) return 0;
    for (unsigned int index = 0; index < WHISPER_TINY_WIDTH; ++index) {
        input[index] = (float)((int)(index % 17) - 8) * 0.0625f;
    }
    if (!whisper_cpu_projection(weights, bias, input, output,
            WHISPER_TINY_WIDTH, WHISPER_TINY_WIDTH)) return 0;
    write_text("projection_first_bits=");
    for (unsigned int index = 0; index < 4; ++index) {
        union { float value; unsigned int bits; } element;
        element.value = output[index];
        if (index) write_text(",");
        write_hex(element.bits);
    }
    write_text("\n");
    write_text("projection_fnv64=");
    write_hex(whisper_artifact_hash_update(
        WHISPER_ARTIFACT_HASH_OFFSET_BASIS, output, sizeof(output)
    ));
    write_text("\n");
    return 1;
}

static int run(void) {
    char executable[1024];
    char option[1024];
    char path[1024];
    char extra[1024];
    char reference[1024];
    char driver[1024];
    const char *cursor = GetCommandLineA();
    const char *remaining;
    const char *after_reference;
    const char *after_driver;
    WhisperWav wav;
    static WhisperIndexed indexed;
    if (cursor == 0 || (cursor = next_argument(cursor, executable, sizeof(executable))) == 0 ||
        (cursor = next_argument(cursor, option, sizeof(option))) == 0 ||
        (cursor = next_argument(cursor, path, sizeof(path))) == 0 ||
        ((remaining = next_argument(cursor, extra, sizeof(extra))) != 0) !=
            (same(option, "--probe-mel") || same(option, "--verify-mel") ||
             same(option, "--transcribe") || same(option, "--transcribe-base") ||
             same(option, "--transcribe-hmx") || same(option, "--transcribe-hmx-encoder") ||
             same(option, "--transcribe-base-hmx-encoder")) ||
        ((after_reference = remaining != 0 ?
            next_argument(remaining, reference, sizeof(reference)) : 0) != 0) !=
            (same(option, "--verify-mel") || same(option, "--transcribe") ||
             same(option, "--transcribe-base") || same(option, "--transcribe-hmx") ||
             same(option, "--transcribe-hmx-encoder") ||
             same(option, "--transcribe-base-hmx-encoder")) ||
        ((after_driver = after_reference != 0 ?
            next_argument(after_reference, driver, sizeof(driver)) : 0) != 0) !=
            (same(option, "--transcribe-hmx") || same(option, "--transcribe-hmx-encoder") ||
             same(option, "--transcribe-base-hmx-encoder")) ||
        (after_driver != 0 && next_argument(after_driver, executable, sizeof(executable)) != 0) ||
        (!same(option, "--inspect-wav") && !same(option, "--inspect-model") &&
         !same(option, "--probe-projection") && !same(option, "--probe-mel") &&
         !same(option, "--verify-mel") && !same(option, "--transcribe") &&
         !same(option, "--transcribe-base") && !same(option, "--transcribe-hmx") &&
         !same(option, "--transcribe-hmx-encoder") &&
         !same(option, "--transcribe-base-hmx-encoder"))) {
        write_text("Usage: whisper-cli.exe --transcribe <wav> <tiny.wti> <frontend-dir> | --transcribe-hmx <wav> <tiny.wti> <frontend-dir> <driver-dll> | --transcribe-hmx-encoder <wav> <tiny.wti> <frontend-dir> <driver-dll> | --inspect-wav <wav> | --inspect-model <tiny.wti> | --probe-projection <tiny.wti> | --probe-mel <wav> <frontend-dir> | --verify-mel <wav> <frontend-dir> <reference-f32>\n");
        write_text("Base: --transcribe-base <wav> <base.wti> <frontend-dir> | --transcribe-base-hmx-encoder <wav> <base.wti> <frontend-dir> <driver-dll>\n");
        return 2;
    }
    if (same(option, "--transcribe") || same(option, "--transcribe-base") ||
        same(option, "--transcribe-hmx") || same(option, "--transcribe-hmx-encoder") ||
        same(option, "--transcribe-base-hmx-encoder")) {
        if (!transcribe(path, extra, reference,
                        (same(option, "--transcribe") || same(option, "--transcribe-base")) ?
                            0 : driver,
                        same(option, "--transcribe-hmx-encoder") ||
                            same(option, "--transcribe-base-hmx-encoder"),
                        (same(option, "--transcribe-base") ||
                         same(option, "--transcribe-base-hmx-encoder")) ?
                            whisper_model_base() : whisper_model_tiny())) {
            write_text("Cannot transcribe audio.\n");
            return 1;
        }
        return 0;
    }
    if (same(option, "--probe-mel") || same(option, "--verify-mel")) {
        if (!probe_mel(path, extra, same(option, "--verify-mel") ? reference : 0)) {
            write_text("Cannot compute log-mel windows.\n");
            return 1;
        }
        return 0;
    }
    if (same(option, "--inspect-model") || same(option, "--probe-projection")) {
        if (!whisper_indexed_open(&indexed, path, whisper_model_tiny())) {
            write_text("Cannot verify a Tiny indexed checkpoint.\n");
            return 1;
        }
        if (same(option, "--probe-projection")) {
            if (!probe_projection(&indexed)) {
                whisper_indexed_close(&indexed);
                write_text("Cannot execute Tiny projection.\n");
                return 1;
            }
        } else {
            write_text("model=tiny\ntensors=");
            write_number(indexed.count);
            write_text("\n");
        }
        whisper_indexed_close(&indexed);
        return 0;
    }
    if (!whisper_wav_open(&wav, path)) {
        write_text("Cannot read a 16 kHz mono float32 RIFF WAV file.\n");
        return 1;
    }
    write_text("samples=");
    write_number(wav.sample_count);
    write_text("\nwindows=");
    write_number(whisper_wav_window_count(&wav));
    write_text("\n");
    whisper_wav_close(&wav);
    return 0;
}

void mainCRTStartup(void) {
    unsigned int original_code_page = GetConsoleOutputCP();
    unsigned int status;
    if (original_code_page) (void)SetConsoleOutputCP(65001U);
    status = (unsigned int)run();
    if (original_code_page) (void)SetConsoleOutputCP(original_code_page);
    ExitProcess(status);
}