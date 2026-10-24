#include "nemotron_wav.h"
#include "runtime.h"

static uint32_t word(const unsigned char *data) { return data[0] | (uint32_t)data[1] << 8; }
static uint32_t dword(const unsigned char *data) { return word(data) | word(data + 2) << 16; }
static int wav_error(NmWav *wav, const char *error) {
    if (wav) wav->error = error;
    return 0;
}
int nm_wav_open(NmWav *wav, const NmInput *input) {
    unsigned char header[12], chunk[8], format[18];
    uint64_t cursor = 12;
    int found_format = 0, found_data = 0;
    if (!wav) return 0;
    memset(wav, 0, sizeof(*wav));
    if (!input || !input->read_at || input->bytes < 12)
        return wav_error(wav, "truncated WAV header");
    wav->input = *input;
    if (!input->read_at(input->context, 0, header, sizeof(header)))
        return wav_error(wav, "WAV header read failed");
    if (memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4) ||
        (uint64_t)dword(header + 4) + 8 != input->bytes)
        return wav_error(wav, "invalid RIFF/WAVE size or signature");
    while (cursor < input->bytes) {
        uint32_t bytes;
        uint64_t next;
        if (input->bytes - cursor < 8 || !input->read_at(input->context, cursor, chunk, 8))
            return wav_error(wav, "truncated WAV chunk");
        bytes = dword(chunk + 4); cursor += 8;
        next = cursor + bytes + (bytes & 1U);
        if (next > input->bytes) return wav_error(wav, "WAV chunk outside RIFF");
        if (!memcmp(chunk, "fmt ", 4)) {
            if (found_format || (bytes != 16 && bytes != 18) ||
                !input->read_at(input->context, cursor, format, bytes))
                return wav_error(wav, "invalid or duplicate WAV format");
            if (word(format) != 1 || word(format + 2) != 1 || dword(format + 4) != 16000 ||
                dword(format + 8) != 32000 || word(format + 12) != 2 || word(format + 14) != 16 ||
                (bytes == 18 && word(format + 16)))
                return wav_error(wav, "WAV requires 16-kHz mono PCM16");
            found_format = 1;
        } else if (!memcmp(chunk, "data", 4)) {
            if (!found_format || found_data || !bytes || (bytes & 1U))
                return wav_error(wav, "invalid or duplicate WAV data");
            wav->offset = cursor; wav->samples = bytes / 2; found_data = 1;
        }
        cursor = next;
    }
    if (!found_format || !found_data) return wav_error(wav, "missing WAV format or data");
    return 1;
}
int nm_wav_read(NmWav *wav, float *samples, uint32_t capacity, uint32_t *count) {
    unsigned char data[1024];
    uint32_t done = 0;
    if (!wav || !count) return wav_error(wav, "invalid WAV read arguments");
    *count = 0;
    if (wav->error || !wav->input.read_at || !samples || !capacity)
        return wav_error(wav, "invalid or failed WAV reader");
    while (done < capacity && wav->cursor < wav->samples) {
        uint64_t remaining = wav->samples - wav->cursor;
        uint32_t take = capacity - done, i;
        if (take > sizeof(data) / 2) take = sizeof(data) / 2;
        if (take > remaining) take = (uint32_t)remaining;
        if (!wav->input.read_at(wav->input.context, wav->offset + wav->cursor * 2, data, take * 2))
            return wav_error(wav, "WAV sample read failed");
        for (i = 0; i < take; ++i) {
            uint32_t bits = word(data + i * 2);
            int value = bits & 0x8000U ? (int)bits - 65536 : (int)bits;
            samples[done + i] = (float)value * 0x1p-15f;
        }
        done += take; wav->cursor += take;
    }
    *count = done; return 1;
}
int nm_wav_reset(NmWav *wav) {
    if (!wav || !wav->input.read_at || wav->error) return wav_error(wav, "invalid or failed WAV reader");
    wav->cursor = 0; return 1;
}
