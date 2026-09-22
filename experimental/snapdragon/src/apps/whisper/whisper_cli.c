#include "whisper_wav.h"
#include "platform.h"

__declspec(dllimport) const char *__stdcall GetCommandLineA(void);
__declspec(dllimport) void __stdcall ExitProcess(unsigned int status);

static void write_text(const char *text) {
    const char *end = text;
    while (*end != 0) ++end;
    while (text != end) {
        long written = platform_write(1, text, (unsigned long long)(end - text));
        if (written <= 0) ExitProcess(3);
        text += written;
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

static int run(void) {
    char executable[1024];
    char option[1024];
    char path[1024];
    char extra[1024];
    const char *cursor = GetCommandLineA();
    WhisperWav wav;
    if (cursor == 0 || (cursor = next_argument(cursor, executable, sizeof(executable))) == 0 ||
        (cursor = next_argument(cursor, option, sizeof(option))) == 0 ||
        (cursor = next_argument(cursor, path, sizeof(path))) == 0 ||
        next_argument(cursor, extra, sizeof(extra)) != 0 ||
        !same(option, "--inspect-wav")) {
        write_text("Usage: whisper-cli.exe --inspect-wav <16k-mono-f32.wav>\n");
        return 2;
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
    ExitProcess((unsigned int)run());
}