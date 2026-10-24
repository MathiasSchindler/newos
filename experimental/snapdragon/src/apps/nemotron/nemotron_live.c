#include "nemotron_live.h"

__declspec(dllimport) void Sleep(u32 milliseconds);
__declspec(dllimport) u32 GetCurrentThreadId(void);
typedef int (*NmControlHandler)(u32);
__declspec(dllimport) int SetConsoleCtrlHandler(NmControlHandler, int);
static NmLive *active_live;

static int console_control(u32 code) {
    NmLive *live = __atomic_load_n(&active_live, __ATOMIC_ACQUIRE);
    if (!live || (code != 0 && code != 1 && code != 2 && code != 5 && code != 6)) return 0;
    __atomic_store_n(&live->canceled, 1, __ATOMIC_RELEASE);
    nm_audio_queue_stop(&live->queue); return 1;
}
static int replay(NmLive *live) {
    NmWindowsFile file = {0}; NmInput input; NmWav wav;
    float frame[160] = {0}; u32 count = 0, gap = 0; u64 sent = 0; int ok = 0;
    if (!nm_file_open(&file, &input, live->replay, 0)) goto done;
    if (!nm_wav_open(&wav, &input)) { live->error = wav.error; goto done; }
    live->capture.resampler.rate = 16000; live->capture.resampler.channels = 1;
    live->capture.resampler.format = 1;
    live->capture.thread_id = GetCurrentThreadId(); live->capture.started = nm_clock();
    while (!__atomic_load_n(&live->queue.stop, __ATOMIC_ACQUIRE)) {
        u64 elapsed = nm_clock() - live->capture.started;
        if (live->duration_s && elapsed >= (u64)live->duration_s * live->frequency) break;
        if (elapsed < sent * live->frequency / 16000) { Sleep(1); continue; }
        if (gap) {
            count = gap > 160 ? 160 : gap; gap -= count; memset(frame, 0, sizeof(frame));
        } else {
            if (!nm_wav_read(&wav, frame, 160, &count)) { live->error = wav.error; goto done; }
            if (!count) {
                if (!nm_wav_reset(&wav)) { live->error = wav.error; goto done; }
                gap = live->replay_gap_ms * 16;
                continue;
            }
        }
        if (!nm_audio_queue_push(&live->queue, frame, count)) {
            if (live->queue.failure) live->error = "paced replay queue overflow";
            else ok = 1;
            goto done;
        }
        sent += count;
    }
    ok = 1;
done:
    live->capture.elapsed_ticks = nm_clock() - live->capture.started;
    live->capture.resampler.output_samples = sent;
    if (!nm_file_close(&file)) ok = 0;
    if (!ok) nm_audio_queue_stop(&live->queue);
    nm_audio_queue_end(&live->queue, ok ? 0 : 2); return ok;
}
static int infer(NmLive *live) {
    float frame[160]; u64 next_stats = nm_clock() + live->frequency * 10;
    live->inference_thread = GetCurrentThreadId();
    for (;;) {
        u32 observed = __atomic_load_n(&live->queue.wake, __ATOMIC_ACQUIRE);
        u32 count = nm_audio_queue_pop(&live->queue, frame, 160);
        if (count) {
            if (!nm_endpoint_feed(&live->endpoint, frame, count)) {
                live->error = live->endpoint.error; nm_audio_queue_end(&live->queue, 3);
                nm_audio_queue_stop(&live->queue); return 0;
            }
        } else if (__atomic_load_n(&live->queue.ended, __ATOMIC_ACQUIRE)) break;
        else platform_wait_word(&live->queue.wake, observed);
        if (nm_clock() >= next_stats) {
            u64 memory = nm_working_set(); u32 queued = nm_audio_queue_count(&live->queue);
            if (!memory) { live->error = "live working-set query failed"; nm_audio_queue_stop(&live->queue); return 0; }
            if (!live->min_memory || memory < live->min_memory) live->min_memory = memory;
            if (memory > live->max_memory) live->max_memory = memory;
            if (!live->steady_memory && nm_clock() - live->started >= live->frequency * 60) live->steady_memory = memory;
            live->queue_sum += queued; live->queue_last = queued; ++live->measurements;
            nm_text("LIVE_STATS elapsed_s="); nm_number((nm_clock() - live->started) / live->frequency);
            nm_text(" queue_samples="); nm_number(queued); nm_text(" queue_peak=");
            nm_number(__atomic_load_n(&live->queue.peak, __ATOMIC_RELAXED));
            nm_text(" working_set="); nm_number(memory); nm_text(" utterances="); nm_number(live->endpoint.utterances);
            nm_text("\n"); next_stats = nm_clock() + live->frequency * 10;
        }
    }
    if (live->queue.failure) { live->error = live->capture.error ? live->capture.error : "live capture/queue error"; return 0; }
    if (!nm_endpoint_finish(&live->endpoint)) { live->error = live->endpoint.error; return 0; }
    return 1;
}
static int task(size_t begin, size_t end, unsigned int worker, void *context) {
    NmLive *live = context;
    if (end - begin != 1 || worker > 1) return -1;
    /* Participant0 keeps inference on the driver-owning thread; participant1 captures. */
    if (!worker) return infer(live) ? 0 : -1;
    return (live->replay ? replay(live) : nm_capture_run(&live->capture)) ? 0 : -1;
}
static int stats(NmLive *live, int passed) {
    char output[2048], value[32]; size_t used = 0;
    const char *keys[] = {"duration_s","elapsed_ms","capture_ms","accepted_samples","consumed_samples",
        "queue_capacity","queue_peak","queue_last","measurements","mean_queue_samples","start_working_set",
        "min_working_set","max_working_set","end_working_set","steady_working_set","utterances","idle_samples",
        "input_rate","input_channels","input_format","clipped_samples","capture_thread","inference_thread",
        "failure","canceled","model_samples","endpoint_trimmed_samples","capture_mmcss","source_gap_frames",
        "source_packets","startup_discontinuities"};
    u64 values[] = {live->duration_s, live->elapsed_ticks * 1000 / live->frequency,
        live->capture.elapsed_ticks * 1000 / live->frequency,live->queue.accepted,live->queue.consumed,
        live->queue.capacity,live->queue.peak,nm_audio_queue_count(&live->queue),live->measurements,
        live->measurements ? live->queue_sum / live->measurements : 0,live->start_memory,live->min_memory,
        live->max_memory,live->end_memory,live->steady_memory,live->endpoint.utterances,live->endpoint.idle_samples,
        live->capture.resampler.rate,live->capture.resampler.channels,live->capture.resampler.format,
        live->capture.resampler.clipped,live->capture.thread_id,live->inference_thread,live->queue.failure,live->canceled,
        live->endpoint.model_samples,live->endpoint.trimmed_samples,live->capture.mmcss,live->capture.gap_frames,
        live->capture.packets,live->capture.startup_discontinuities};
    u32 index;
    const char *prefix = passed ? "{\"schema\":\"newos.nemotron.live.stats.v1\",\"passed\":true" :
                                  "{\"schema\":\"newos.nemotron.live.stats.v1\",\"passed\":false";
    memcpy(output, prefix, rt_strlen(prefix)); used = rt_strlen(prefix);
    for (index = 0; index < sizeof(values) / sizeof(values[0]); ++index) {
        size_t length = rt_strlen(keys[index]), n;
        rt_unsigned_to_string(values[index], value, sizeof(value)); n = rt_strlen(value);
        if (used + length + n + 5 >= sizeof(output)) return nm_error("live stats encoding exceeded bound");
        output[used++] = ','; output[used++] = '"'; memcpy(output + used, keys[index], length); used += length;
        memcpy(output + used, "\":", 2); used += 2; memcpy(output + used, value, n); used += n;
    }
    memcpy(output + used, ",\"raw_audio_recorded\":false}\n", sizeof(",\"raw_audio_recorded\":false}\n") - 1);
    used += sizeof(",\"raw_audio_recorded\":false}\n") - 1; output[used] = 0;
    nm_text(output);
    return !live->stats_path || nm_write_file(live->stats_path, output, used);
}
int nm_live_run(NmLive *live, NmEndpointOps ops, void *context) {
    int ok = 0;
    live->frequency = nm_frequency();
    if (!live->frequency) return nm_error("live QPC frequency unavailable");
    live->samples = nm_alloc(81920 * sizeof(float));
    if (!live->samples) return 0;
    nm_audio_queue_init(&live->queue, live->samples, 81920);
    live->capture.queue = &live->queue; live->capture.duration_s = live->duration_s;
    if (!nm_endpoint_init(&live->endpoint, live->silence_ms, 0.005f, ops, context)) { live->error = live->endpoint.error; goto done; }
    if (rt_task_pool_init(&live->pool, 2) != 0 || rt_task_pool_width(&live->pool) != 2) {
        live->error = "live capture/inference requires two project task-pool participants"; goto done;
    }
    __atomic_store_n(&active_live, live, __ATOMIC_RELEASE);
    if (!SetConsoleCtrlHandler(console_control, 1)) { live->error = "live console cancellation handler failed"; goto done; }
    live->start_memory = nm_working_set(); live->started = nm_clock();
    ok = rt_parallel_for(&live->pool, 2, 1, task, live) == 0;
    live->elapsed_ticks = nm_clock() - live->started; live->end_memory = nm_working_set();
    if (!SetConsoleCtrlHandler(console_control, 0)) { live->error = "live cancellation handler removal failed"; ok = 0; }
    __atomic_store_n(&active_live, 0, __ATOMIC_RELEASE);
    if (live->capture.thread_id == live->inference_thread || live->queue.accepted != live->queue.consumed) {
        live->error = "live task separation/sample accounting failed"; ok = 0;
    }
    if (ok && live->queue.consumed != live->endpoint.model_samples +
        live->endpoint.trimmed_samples + live->endpoint.idle_samples) {
        live->error = "live endpoint sample accounting failed"; ok = 0;
    }
    if (!stats(live, ok)) ok = 0;
done:
    __atomic_store_n(&active_live, 0, __ATOMIC_RELEASE);
    rt_task_pool_destroy(&live->pool); nm_free(live->samples, 81920 * sizeof(float)); live->samples = 0;
    if (!ok) nm_error(live->error ? live->error : live->capture.error ? live->capture.error : "live pipeline failed");
    return ok;
}
