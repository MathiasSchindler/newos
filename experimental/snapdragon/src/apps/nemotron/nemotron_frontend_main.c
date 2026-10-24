#include "nemotron_frontend.h"
#include "nemotron_windows.h"
#include "nemotron_runtime.h"
#include "crypto/sha256.h"

typedef struct NmFeatureOutput {
    NmWindowsFile file;
    u64 position, last, frames;
    u32 chunks;
} NmFeatureOutput;
static u8 scratch[65536];

static int file_hash(NmWindowsFile *file, u64 start, u8 digest[32]) {
    CryptoSha256Context hash;
    u64 cursor = start;
    crypto_sha256_init(&hash);
    while (cursor < file->bytes) {
        u32 bytes = file->bytes - cursor > sizeof(scratch) ? sizeof(scratch) : (u32)(file->bytes - cursor);
        if (!nm_file_read(file, cursor, scratch, bytes)) return 0;
        crypto_sha256_update(&hash, scratch, bytes); cursor += bytes;
    }
    crypto_sha256_final(&hash, digest); return 1;
}
static int output_chunk(void *context, const float *features, uint32_t width,
                        uint32_t valid, uint32_t flags, uint64_t index) {
    NmFeatureOutput *output = context;
    u8 record[16];
    if (flags == NM_AUDIO_END_FLAG) {
        if (!output->chunks || index != output->chunks || width || valid) return nm_error("invalid frontend end event");
        if (!nm_file_read(&output->file, output->last + 4, record, 4)) return 0;
        nm_put32(record, nm_u32(record) | NM_AUDIO_FINAL_FLAG);
        return nm_file_write(&output->file, output->last + 4, record, 4);
    }
    if (index != output->chunks || index > 0xffffffffU ||
        width != (index ? NM_AUDIO_STEADY : NM_AUDIO_FIRST) || !valid || valid > width)
        return nm_error("invalid feature chunk contract");
    nm_put32(record, (u32)index); nm_put32(record + 4, flags);
    nm_put32(record + 8, width); nm_put32(record + 12, valid);
    output->last = output->position;
    if (!nm_file_write(&output->file, output->position, record, sizeof(record)) ||
        !nm_file_write(&output->file, output->position + sizeof(record), features, width * NM_AUDIO_MELS * 4)) return 0;
    output->position += sizeof(record) + width * NM_AUDIO_MELS * 4;
    output->frames += valid; ++output->chunks; return 1;
}
static int partition(const char *text, u32 sizes[16], u32 *count) {
    const char *start = text;
    *count = 0;
    while (*start) {
        char item[16]; size_t n = 0; unsigned long long value;
        while (start[n] && start[n] != ',') { if (n == sizeof(item) - 1) return 0; item[n] = start[n]; ++n; }
        item[n] = 0;
        if (*count == 16 || rt_parse_uint(item, &value) != 0 || !value || value > 8192) return 0;
        sizes[(*count)++] = (u32)value; start += n;
        if (*start == ',') { ++start; if (!*start) return 0; }
    }
    return *count != 0;
}
static int features(const NmArtifact *bundle, const char *wav_path, const char *output_path, const char *supply) {
    NmWindowsFile input_file = {0}; NmInput input; NmWav wav;
    NmFeatureOutput output; NmFrontend *frontend = 0;
    u32 sizes[16], partitions, next = 0, count; float samples[8192];
    u8 header[128] = {0}; int ok = 0, created = 0;
    memset(&output, 0, sizeof(output)); output.position = sizeof(header);
    if (!partition(supply, sizes, &partitions)) return nm_error("invalid sample partition (1..8192, at most16 entries)");
    if (!nm_file_open(&input_file, &input, wav_path, 0)) return 0;
    if (!nm_wav_open(&wav, &input)) { nm_error(wav.error); goto done; }
    if (wav.samples < NM_AUDIO_HOP) { nm_error("audio shorter than one feature frame"); goto done; }
    frontend = nm_alloc(sizeof(*frontend));
    if (!frontend) goto done;
    if (!nm_frontend_init(frontend, nm_runtime_window(bundle), nm_runtime_mel(bundle), output_chunk, &output)) {
        nm_error(frontend->error); goto done;
    }
    if (!nm_file_open(&output.file, 0, output_path, 1)) goto done;
    created = 1;
    if (!nm_file_write(&output.file, 0, header, sizeof(header))) goto done;
    do {
        if (!nm_wav_read(&wav, samples, sizes[next++ % partitions], &count)) { nm_error(wav.error); goto done; }
        if (!nm_frontend_feed(frontend, samples, count)) { nm_error(frontend->error); goto done; }
    } while (count);
    if (!nm_frontend_finish(frontend)) { nm_error(frontend->error); goto done; }
    if (frontend->frames != wav.samples / NM_AUDIO_HOP || output.frames != frontend->frames) {
        nm_error("frontend frame accounting mismatch"); goto done;
    }
    memcpy(header, "NM2FEAT1", 8);
    nm_put32(header + 8, 1); nm_put32(header + 12, NM_AUDIO_RATE); nm_put32(header + 16, NM_AUDIO_MELS);
    nm_put32(header + 20, output.chunks); nm_put64(header + 24, wav.samples);
    nm_put64(header + 32, output.frames); nm_put64(header + 40, output.position - sizeof(header));
    memcpy(header + 80, bundle->header + 104, 32);
    if (!file_hash(&output.file, sizeof(header), header + 48) ||
        !nm_file_write(&output.file, 0, header, sizeof(header))) goto done;
    nm_text("PASS frontend: samples="); nm_number(wav.samples); nm_text(" frames="); nm_number(output.frames);
    nm_text(" chunks="); nm_number(output.chunks); nm_text(" ring_capacity=512 state_bytes=");
    nm_number(sizeof(*frontend)); nm_text(" CPU-frontend-only; no model inference\n");
    ok = 1;
done:
    if (!nm_file_close(&input_file)) ok = 0;
    if (!nm_file_close(&output.file)) ok = 0;
    nm_free(frontend, sizeof(*frontend));
    if (!ok && created) nm_file_remove(output_path);
    return ok;
}
void mainCRTStartup(void) {
    static char args[8][4096]; NmRuntime runtime = {0};
    int count = nm_arguments(args), ok = 0;
    if (count != 3 && count != 6) {
        nm_error("usage: nemotron-frontend check PACKAGE | features PACKAGE WAV OUTPUT PARTITION"); goto done;
    }
    if (!nm_runtime_open(&runtime, args[2])) goto done;
    if (count == 3 && !rt_strcmp(args[1], "check")) ok = nm_runtime_check(&runtime);
    else if (count == 6 && !rt_strcmp(args[1], "features")) ok = features(&runtime.artifact, args[3], args[4], args[5]);
    else nm_error("unknown frontend command/argument count");
done:
    nm_runtime_close(&runtime); ExitProcess(ok ? 0 : 1);
}
