#include "nemotron_asr_runtime.h"
#include "nemotron_frontend.h"
#include "nemotron_rnnt.h"
#include "nemotron_tokenizer.h"
#include "nemotron_live.h"

#define NM_ASR_TOKEN_LIMIT 65536U
#define NM_ASR_TEXT_LIMIT 1048576U
typedef struct NmEvents {
    NmWindowsFile file;
    u64 offset;
    u32 used;
    int standard;
    char buffer[4096];
} NmEvents;
typedef struct NmAsrApp {
    NmAsrModel model;
    NmRnnt decoder;
    NmTokenizer tokenizer;
    NmFrontend *frontend;
    NmEvents events;
    char *text;
    u64 sequence, utterance, started, frequency, cpu_user, cpu_kernel;
    const char *error;
    int diagnostics;
    int copied_io;
    int listening, quiet_transcripts;
    u64 audio_offset;
    NmLive live;
} NmAsrApp;
static NmAsrApp app;
static char arguments[32][4096];

static int flush(NmEvents *events) {
    int ok;
    if (!events->used) return 1;
    ok = events->standard ? nm_stdout(events->buffer, events->used) :
         nm_file_write(&events->file, events->offset, events->buffer, events->used);
    if (!ok) return 0;
    events->offset += events->used; events->used = 0; return 1;
}
static int write_bytes(NmEvents *events, const char *data, size_t bytes) {
    while (bytes) {
        size_t take = sizeof(events->buffer) - events->used;
        if (take > bytes) take = bytes;
        memcpy(events->buffer + events->used, data, take);
        events->used += (u32)take; data += take; bytes -= take;
        if (events->used == sizeof(events->buffer) && !flush(events)) return 0;
    }
    return 1;
}
static int literal(const char *text) { return write_bytes(&app.events, text, rt_strlen(text)); }
static int number(u64 value) {
    char text[32]; rt_unsigned_to_string(value, text, sizeof(text)); return literal(text);
}
static int quoted(const char *text, size_t bytes) {
    static const char hex[] = "0123456789abcdef";
    size_t index;
    if (!literal("\"")) return 0;
    for (index = 0; index < bytes; ++index) {
        unsigned char c = (unsigned char)text[index];
        if (c == '"' || c == '\\') {
            char escaped[2] = {'\\', (char)c};
            if (!write_bytes(&app.events, escaped, sizeof(escaped))) return 0;
        } else if (c < 32) {
            char escaped[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            if (!write_bytes(&app.events, escaped, sizeof(escaped))) return 0;
        } else if (!write_bytes(&app.events, text + index, 1)) return 0;
    }
    return literal("\"");
}
static int event(const char *kind) {
    size_t bytes = 0, index;
    u64 user = 0, kernel = 0, copied = 0, shared = 0;
    nm_rpc_io_stats(&copied, &shared);
    if (!rt_strcmp(kind, "final") &&
        (!nm_process_cpu(&user, &kernel) || user < app.cpu_user || kernel < app.cpu_kernel))
        return nm_error("ASR host process CPU counters invalid");
    if (app.quiet_transcripts) {
        return literal("{\"schema\":\"newos.nemotron.live.activity.v1\",\"event\":") &&
            quoted(kind, rt_strlen(kind)) && literal(",\"utterance\":") && number(app.utterance) &&
            literal(",\"audio_samples\":") && number(app.audio_offset + app.frontend->samples) &&
            literal(",\"capture_samples\":") && number(app.live.endpoint.samples) &&
            literal(",\"endpoint_trimmed_samples\":") && number(app.live.endpoint.trimmed_samples) &&
            literal(",\"token_count\":") && number(app.decoder.count) && literal(",\"transcript_retained\":false}\n") &&
            flush(&app.events);
    }
    if (!nm_tokenizer_decode(&app.tokenizer, app.decoder.tokens, app.decoder.count, 1,
                             app.text, NM_ASR_TEXT_LIMIT, &bytes)) {
        app.error = app.tokenizer.error; return 0;
    }
    if (!literal("{\"schema\":\"newos.nemotron.asr.v1\",\"event\":") ||
        !quoted(kind, rt_strlen(kind)) || !literal(",\"seq\":") || !number(app.sequence++) ||
        !literal(",\"utterance\":") || !number(app.utterance) ||
        !literal(",\"language\":\"de-DE\",\"chunk_ms\":320,\"audio_samples\":") ||
        !number(app.audio_offset + app.frontend->samples) || !literal(",\"audio_ms\":") ||
        !number((app.audio_offset + app.frontend->samples) / 16) ||
        !literal(",\"encoder_frames\":") || !number(app.decoder.frames) ||
        !literal(",\"tokens\":[")) return 0;
    for (index = 0; index < app.decoder.count; ++index)
        if ((index && !literal(",")) || !number(app.decoder.tokens[index])) return 0;
    if (!literal("],\"text\":") || !quoted(app.text, bytes) ||
        !literal(",\"prediction_calls\":") || !number(app.decoder.prediction_calls) ||
        !literal(",\"joiner_calls\":") || !number(app.decoder.joiner_calls) ||
        !literal(",\"backend\":\"OEMFastRPC-HVX-FP32\",\"model_cpu\":false,\"hmx\":false,\"vendor_ascii_module_staging\":") ||
        !literal(app.model.module_stage.created ? "true" : "false") ||
        !literal(",\"shared_rpc_io\":") || !literal(app.model.shared_io ? "true" : "false") ||
        !literal(",\"fused_decoder\":") || !literal(app.model.fused_decoder ? "true" : "false") ||
        !literal(",\"rpc_graph_calls\":") || !number(app.model.graph_calls) ||
        !literal(",\"fused_joiner_reuses\":") || !number(app.model.fused_joiner_reuses) ||
        !literal(",\"host_rpc_copy_bytes\":") || !number(copied) ||
        !literal(",\"shared_rpc_binding_bytes\":") || !number(shared)) return 0;
    if ((app.listening || app.live.replay) &&
        (!literal(",\"capture_samples\":") || !number(app.live.endpoint.samples) ||
         !literal(",\"endpoint_trimmed_samples\":") || !number(app.live.endpoint.trimmed_samples))) return 0;
    if (app.diagnostics) {
        if (!nm_tokenizer_decode(&app.tokenizer, app.decoder.tokens, app.decoder.count, 0,
                                 app.text, NM_ASR_TEXT_LIMIT, &bytes) ||
            !literal(",\"raw_text\":") || !quoted(app.text, bytes)) return 0;
    }
    if (!rt_strcmp(kind, "final")) {
        if (!literal(",\"wall_ms\":") || !number((nm_clock() - app.started) * 1000 / app.frequency) ||
            !literal(",\"host_cpu_user_ms\":") || !number((user - app.cpu_user) / 10000) ||
            !literal(",\"host_cpu_kernel_ms\":") || !number((kernel - app.cpu_kernel) / 10000) ||
            !literal(",\"timing_scope\":\"WAV frontend/model/IO/events; excludes package staging\"")) return 0;
    }
    return literal("}\n") && flush(&app.events);
}
static int chunk(void *context, const float *features, uint32_t width,
                 uint32_t valid, uint32_t flags, uint64_t index) {
    const float *projected = 0; u32 frame;
    (void)context;
    if (flags == NM_AUDIO_END_FLAG) {
        if (features || width || valid || index != app.model.chunks) {
            app.error = "invalid ASR frontend end event"; return 0;
        }
        return event("final");
    }
    if (index != app.model.chunks || !valid || valid > width ||
        ((flags & NM_AUDIO_FIRST_FLAG) != (index ? 0U : NM_AUDIO_FIRST_FLAG))) {
        app.error = "ASR feature chunk order/valid length mismatch"; return 0;
    }
    if (!nm_asr_model_encode(&app.model, features, width, &projected)) {
        app.error = "DSP encoder execution failed"; return 0;
    }
    for (frame = 0; frame < 4; ++frame)
        if (!nm_rnnt_frame(&app.decoder, projected + frame * 640)) {
            app.error = app.decoder.error; return 0;
        }
    return event("partial");
}
static int validate_wav(const char *path) {
    NmWindowsFile file = {0}; NmInput input; NmWav wav; int ok;
    if (!nm_file_open(&file, &input, path, 0)) return 0;
    ok = nm_wav_open(&wav, &input);
    if (!ok) nm_error(wav.error);
    else if (wav.samples < 160) ok = nm_error("ASR WAV shorter than one valid feature frame");
    if (!nm_file_close(&file)) ok = 0;
    return ok;
}
static int live_begin(void *context, uint64_t sample) {
    NmRnntOps ops = {nm_asr_model_reset, nm_asr_model_predict, nm_asr_model_join};
    (void)context;
    app.audio_offset = sample; app.started = nm_clock();
    if (!nm_process_cpu(&app.cpu_user, &app.cpu_kernel) || !nm_frontend_reset(app.frontend) ||
        !(app.utterance ? nm_rnnt_reset(&app.decoder) :
          nm_rnnt_init(&app.decoder, ops, &app.model, app.decoder.tokens, NM_ASR_TOKEN_LIMIT))) {
        app.error = app.decoder.error ? app.decoder.error : app.frontend->error; return 0;
    }
    return 1;
}
static int live_feed(void *context, const float *samples, uint32_t count) {
    (void)context;
    if (!nm_frontend_feed(app.frontend, samples, count)) {
        if (!app.error) app.error = app.frontend->error;
        return 0;
    }
    return 1;
}
static int live_end(void *context, uint64_t sample, uint64_t speech) {
    (void)context; (void)speech;
    if (app.audio_offset + app.frontend->samples != sample || !nm_frontend_finish(app.frontend)) {
        if (!app.error) app.error = app.frontend->error ? app.frontend->error : "endpoint sample accounting mismatch";
        return 0;
    }
    ++app.utterance; return 1;
}
static int recognize(const char *path) {
    NmWindowsFile file = {0}; NmInput input; NmWav wav;
    float samples[4096]; u32 count; int ok = 0;
    NmRnntOps ops = {nm_asr_model_reset, nm_asr_model_predict, nm_asr_model_join};
    app.error = 0;
    if (!nm_file_open(&file, &input, path, 0)) return 0;
    if (!nm_wav_open(&wav, &input)) { app.error = wav.error; goto done; }
    app.started = nm_clock();
    if (!app.started || !nm_process_cpu(&app.cpu_user, &app.cpu_kernel)) {
        app.error = "ASR timing initialization failed"; goto done;
    }
    if (!nm_frontend_reset(app.frontend) ||
        !(app.utterance ? nm_rnnt_reset(&app.decoder) :
          nm_rnnt_init(&app.decoder, ops, &app.model, app.decoder.tokens, NM_ASR_TOKEN_LIMIT))) {
        app.error = app.decoder.error ? app.decoder.error : app.frontend->error; goto done;
    }
    do {
        if (!nm_wav_read(&wav, samples, 4096, &count)) { app.error = wav.error; goto done; }
        if (!nm_frontend_feed(app.frontend, samples, count)) {
            if (!app.error) app.error = app.frontend->error;
            goto done;
        }
    } while (count);
    if (!nm_frontend_finish(app.frontend)) {
        if (!app.error) app.error = app.frontend->error;
        goto done;
    }
    ok = 1;
done:
    if (!nm_file_close(&file)) ok = 0;
    if (!ok) nm_error(app.error ? app.error : "native WAV recognition failed");
    return ok;
}
void mainCRTStartup(void) {
    const char *package = 0, *wav = 0, *second = 0, *output = "-";
    NmRuntime runtime = {0};
    const u8 *tokenizer; u64 tokenizer_bytes = 0;
    u32 seen = 0; int count, index, ok = 0, output_open = 0, model_open = 0;
    nm_diagnostics_to_stderr(1);
    app.live.silence_ms = 800; app.live.replay_gap_ms = 10000;
    count = nm_arguments_n(arguments, 32);
    if (!count) { nm_error("ASR command line exceeds bounds"); goto done; }
    for (index = 1; index < count; ++index) {
        const char *name = arguments[index], *value = 0; u32 flag = 0;
        if (!rt_strcmp(name, "--diagnostics")) { flag = 64; app.diagnostics = 1; }
        else if (!rt_strcmp(name, "--copied-io")) { flag = 128; app.copied_io = 1; }
        else if (!rt_strcmp(name, "--listen")) { flag = 256; app.listening = 1; }
        else if (!rt_strcmp(name, "--quiet-transcripts")) { flag = 8192; app.quiet_transcripts = 1; }
        else {
            if (++index == count) { nm_error("missing ASR option value"); goto done; }
            value = arguments[index];
            if (!rt_strcmp(name, "--package")) { flag = 1; package = value; }
            else if (!rt_strcmp(name, "--wav")) { flag = 2; wav = value; }
            else if (!rt_strcmp(name, "--output")) { flag = 4; output = value; }
            else if (!rt_strcmp(name, "--reset-wav")) { flag = 8; second = value; }
            else if (!rt_strcmp(name, "--replay-live")) { flag = 512; app.live.replay = value; }
            else if (!rt_strcmp(name, "--stats")) { flag = 16384; app.live.stats_path = value; }
            else if (!rt_strcmp(name, "--duration-s") || !rt_strcmp(name, "--endpoint-ms") ||
                     !rt_strcmp(name, "--replay-gap-ms")) {
                unsigned long long number;
                if (rt_parse_uint(value, &number) != 0) { nm_error("invalid live duration/endpoint/gap"); goto done; }
                if (!rt_strcmp(name, "--duration-s")) {
                    flag = 1024; if (!number || number > 86400) { nm_error("live duration must be1..86400seconds"); goto done; }
                    app.live.duration_s = (u32)number;
                } else if (!rt_strcmp(name, "--endpoint-ms")) {
                    flag = 2048; if (number < 100 || number > 10000) { nm_error("endpoint silence must be100..10000ms"); goto done; }
                    app.live.silence_ms = (u32)number;
                } else {
                    flag = 4096; if (number > 60000) { nm_error("replay gap exceeds60seconds"); goto done; }
                    app.live.replay_gap_ms = (u32)number;
                }
            }
            else if (!rt_strcmp(name, "--language")) {
                flag = 16; if (rt_strcmp(value, "de-DE")) { nm_error("only pinned de-DE profile is supported"); goto done; }
            } else if (!rt_strcmp(name, "--chunk-ms")) {
                unsigned long long chunk_ms; flag = 32;
                if (rt_parse_uint(value, &chunk_ms) != 0 || chunk_ms != 320) {
                    nm_error("only pinned 320-ms [56,3] profile is supported"); goto done;
                }
            } else { nm_error("unknown ASR option"); goto done; }
        }
        if (seen & flag) { nm_error("duplicate ASR option"); goto done; }
        seen |= flag;
    }
    if (!package || ((app.listening || app.live.replay) ? (wav || second) : !wav) ||
        (app.listening && app.live.replay) || (!(app.listening || app.live.replay) && (seen & (1024|2048|4096|8192|16384)))) {
        nm_error("usage: --package INDEX with --wav WAV | --listen | --replay-live WAV; optional --duration-s N --endpoint-ms N --stats NEWFILE --quiet-transcripts");
        goto done;
    }
    if ((wav && !validate_wav(wav)) || (second && !validate_wav(second)) ||
        (app.live.replay && !validate_wav(app.live.replay)) || !nm_runtime_open(&runtime, package)) goto done;
    tokenizer = nm_runtime_tokenizer(&runtime.artifact, &tokenizer_bytes);
    if (!tokenizer || !nm_tokenizer_open(&app.tokenizer, tokenizer, tokenizer_bytes)) {
        nm_error("ASR requires schema2 runtime index with pinned decode table; run M3 export"); goto done;
    }
    app.frequency = nm_frequency();
    if (!app.frequency) { nm_error("ASR QPC frequency unavailable"); goto done; }
    app.frontend = nm_alloc(sizeof(*app.frontend)); app.text = nm_alloc(NM_ASR_TEXT_LIMIT);
    app.decoder.tokens = nm_alloc((u64)NM_ASR_TOKEN_LIMIT * sizeof(u32));
    if (!app.frontend || !app.text || !app.decoder.tokens) goto done;
    if (!nm_frontend_init(app.frontend, nm_runtime_window(&runtime.artifact),
                         nm_runtime_mel(&runtime.artifact), chunk, &app)) {
        nm_error(app.frontend->error); goto done;
    }
    app.events.standard = !rt_strcmp(output, "-");
    if (!app.events.standard && !nm_file_open(&app.events.file, 0, output, 1)) goto done;
    output_open = 1;
    if (!nm_asr_model_open(&app.model, &runtime, app.diagnostics, !app.copied_io)) goto done;
    model_open = 1;
    if (app.listening || app.live.replay) {
        NmEndpointOps ops = {live_begin, live_feed, live_end};
        if (!nm_live_run(&app.live, ops, &app)) goto done;
    } else {
        if (!recognize(wav)) goto done;
        if (second) { app.utterance = 1; if (!recognize(second)) goto done; }
    }
    ok = 1;
done:
    if (!ok && output_open) {
        app.events.used = 0;
        literal("{\"schema\":\"newos.nemotron.asr.v1\",\"event\":\"error\",\"message\":");
        quoted(app.error ? app.error : "native ASR failed; see diagnostics", rt_strlen(app.error ? app.error : "native ASR failed; see diagnostics"));
        literal("}\n"); flush(&app.events);
    }
    if (model_open && !nm_asr_model_close(&app.model)) ok = 0;
    if (!nm_file_close(&app.events.file)) ok = 0;
    nm_free(app.frontend, sizeof(*app.frontend)); nm_free(app.text, NM_ASR_TEXT_LIMIT);
    nm_free(app.decoder.tokens, (u64)NM_ASR_TOKEN_LIMIT * sizeof(u32));
    nm_runtime_close(&runtime); ExitProcess(ok ? 0 : 1);
}
