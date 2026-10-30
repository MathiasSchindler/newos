#include "../nemotron_audio.h"
#include "../nemotron_capture.h"
#include "math/math.h"
#include "concurrency.h"

static NmResampler first, second;
static float source[96000], result[32000], partitioned[32000], queue_memory[640];
static u32 used, passed, failed, begins, ends;
static u64 fed, begin_sample, end_sample;
static NmAudioQueue queue;
static int recording;
static void check(int condition, const char *label) {
    if (condition) ++passed;
    else { ++failed; nm_text("FAIL "); nm_text(label); nm_text("\n"); }
}
static int collect(void *context, const float *samples, uint32_t count) {
    float *target = context;
    if (used + count > 32000) return 0;
    memcpy(target + used, samples, count * sizeof(float)); used += count; return 1;
}
static int begin(void *context, uint64_t sample) {
    (void)context; ++begins; begin_sample = sample; return 1;
}
static int feed(void *context, const float *samples, uint32_t count) {
    (void)context; fed += count;
    return !recording || collect(recording == 1 ? result : partitioned, samples, count);
}
static int end(void *context, uint64_t sample, uint64_t speech) {
    (void)context;
    if (sample < speech) return 0;
    ++ends; end_sample = sample; return 1;
}
static int producer_consumer(size_t begin_, size_t end_, unsigned int worker, void *context) {
    u32 index;
    (void)begin_; (void)end_; (void)context;
    if (worker) {
        for (index = 0; index < 20000; ++index) {
            float sample = (float)(index % 100) / 100;
            while (nm_audio_queue_count(&queue) == 640) { /* pacing is producer test-only, not capture backpressure */
                if (__atomic_load_n(&queue.stop, __ATOMIC_ACQUIRE)) return -1;
            }
            if (!nm_audio_queue_push(&queue, &sample, 1)) return -1;
        }
        nm_audio_queue_end(&queue, 0); return 0;
    }
    for (index = 0; index < 20000;) {
        float samples[32]; u32 observed = __atomic_load_n(&queue.wake, __ATOMIC_ACQUIRE);
        u32 count = nm_audio_queue_pop(&queue, samples, 32), i;
        if (!count) { platform_wait_word(&queue.wake, observed); continue; }
        for (i = 0; i < count; ++i, ++index)
            if (samples[i] != (float)(index % 100) / 100) { nm_audio_queue_stop(&queue); return -1; }
    }
    return 0;
}
void mainCRTStartup(void) {
    u32 index, offset, rates[4] = {8000, 16000, 44100, 48000};
    NmEndpoint endpoint; NmEndpointOps ops = {begin, feed, end}; NmCapture capture = {0};
    RtTaskPool pool; int pool_ok;
    for (index = 0; index < 48000; ++index) {
        source[index * 2] = source[index * 2 + 1] = (float)(0.5 * math_sin(2 * MATH_PI * 1000 * index / 48000));
    }
    check(nm_resampler_init(&first, 48000, 2, 3), "48000 float stereo mix");
    used = 0;
    check(nm_resampler_feed(&first, source, 48000, 0, collect, result) && used == 16000, "48k to16k exact output count");
    check(nm_resampler_init(&second, 48000, 2, 3), "partition resampler");
    used = 0;
    for (offset = 0; offset < 48000;) {
        u32 count = offset % 517 + 1;
        if (count > 48000 - offset) count = 48000 - offset;
        if (!nm_resampler_feed(&second, source + offset * 2, count, 0, collect, partitioned)) ++failed;
        offset += count;
    }
    check(used == 16000 && !memcmp(result, partitioned, 16000 * sizeof(float)), "sample supply partition invariant resampling");
    {
        double energy = 0;
        for (index = 200; index < 16000; ++index) energy += result[index] * result[index];
        check(energy / 15800 > 0.12 && energy / 15800 < 0.13, "speech-band pass gain");
    }
    for (index = 0; index < 48000; ++index)
        source[index] = (float)(0.5 * math_sin(2 * MATH_PI * 12000 * index / 48000));
    check(nm_resampler_init(&first, 48000, 1, 3), "anti-alias resampler");
    used = 0;
    check(nm_resampler_feed(&first, source, 48000, 0, collect, result), "anti-alias source");
    {
        double energy = 0;
        for (index = 200; index < 16000; ++index) energy += result[index] * result[index];
        check(energy / 15800 < 1e-6, "out-of-band alias suppressed");
    }
    for (index = 0; index < 4; ++index) {
        used = 0;
        check(nm_resampler_init(&first, rates[index], 1, 3) &&
              nm_resampler_feed(&first, 0, rates[index], 1, collect, result) &&
              used == 16000, "supported rate, explicit silence and output counts");
    }
    check(!nm_resampler_init(&first, 48001, 1, 3), "unsupported rate explicit");
    check(!nm_resampler_init(&first, 16000, 9, 3), "unsupported channel count explicit");
    check(nm_resampler_init(&first, 16000, 1, 3), "nonfinite test setup");
    source[0] = (float)math_nan(); used = 0;
    check(!nm_resampler_feed(&first, source, 1, 0, collect, result) && first.error, "nonfinite microphone input explicit");
    {
        unsigned char pcm[6] = {0,128,0,0,255,127};
        used = 0;
        check(nm_resampler_init(&first, 16000, 1, 1) && nm_resampler_feed(&first, pcm, 3, 0, collect, result) &&
              result[0] == -1 && result[1] == 0 && result[2] == 32767 * 0x1p-15f, "PCM16 exact no-filter path");
    }
    nm_audio_queue_init(&queue, queue_memory, 640); memset(source, 0, 641 * sizeof(float));
    check(!nm_audio_queue_push(&queue, source, 641) && queue.failure == 1 && queue.stop, "bounded queue overflow fatal");
    nm_audio_queue_end(&queue, 2);
    check(queue.failure == 1, "capture completion preserves first queue failure");
    nm_audio_queue_init(&queue, queue_memory, 640);
    queue.read = queue.write = (uint64_t)UINT32_MAX - 320;
    for (index = 0; index < 640; ++index) source[index] = (float)index / 640;
    check(nm_audio_queue_push(&queue, source, 640) &&
          nm_audio_queue_pop(&queue, result, 640) == 640 &&
          !memcmp(source, result, 640 * sizeof(float)) && queue.write > UINT32_MAX,
          "long-running queue counters cross32-bit boundary without ring aliasing");
    nm_audio_queue_init(&queue, queue_memory, 640); nm_audio_queue_stop(&queue);
    check(!nm_audio_queue_push(&queue, source, 1) && !queue.failure, "cancel does not silently accept samples");
    nm_audio_queue_init(&queue, queue_memory, 640);
    pool_ok = rt_task_pool_init(&pool, 2) == 0 && rt_task_pool_width(&pool) == 2;
    check(pool_ok, "project task pool audio separation");
    check(pool_ok && rt_parallel_for(&pool, 2, 1, producer_consumer, 0) == 0 &&
          queue.accepted == queue.consumed && queue.consumed == 20000 && !queue.failure, "concurrent SPSC no loss/order corruption");
    rt_task_pool_destroy(&pool);
    check(nm_endpoint_init(&endpoint, 800, 0.005f, ops, 0), "endpoint configured");
    memset(source, 0, 16000 * sizeof(float));
    check(nm_endpoint_feed(&endpoint, source, 16000) && !begins, "silence does not create model utterance");
    for (index = 0; index < 16000; ++index) source[index] = 0.02f;
    check(nm_endpoint_feed(&endpoint, source, 16000) && begins == 1, "speech onset with bounded preroll");
    memset(source, 0, 4800 * sizeof(float));
    check(nm_endpoint_feed(&endpoint, source, 4800) && !ends, "pause inside sentence remains active");
    check(endpoint.quiet_samples == 4800 && !endpoint.trimmed_samples &&
          fed == endpoint.model_samples, "tentative pause buffered without model work");
    for (index = 0; index < 16000; ++index) source[index] = 0.02f;
    check(nm_endpoint_feed(&endpoint, source, 16000) && !ends, "long speech stays in same utterance");
    check(!endpoint.quiet_samples && fed == 41120, "resumed speech flushes complete internal pause");
    memset(source, 0, 12800 * sizeof(float));
    check(nm_endpoint_feed(&endpoint, source, 12800) && ends == 1 &&
          end_sample - begin_sample == fed, "configured silence endpoint, no sample boundary drift");
    check(endpoint.trimmed_samples == 9600 && endpoint.samples - end_sample == 9600 &&
          end_sample - endpoint.last_speech == 3200, "confirmed endpoint retains200ms and accounts600ms trimming");
    for (index = 0; index < 3200; ++index) source[index] = 0.001f;
    check(nm_endpoint_feed(&endpoint, source, 3200) && begins == 1, "below-threshold background noise");
    for (index = 0; index < 3200; ++index) source[index] = 0.02f;
    check(nm_endpoint_feed(&endpoint, source, 3200) && begins == 2 &&
          nm_endpoint_finish(&endpoint) && ends == 2, "new utterance plus EOF/reset endpoint");
    check(endpoint.samples == endpoint.idle_samples + endpoint.model_samples + endpoint.trimmed_samples &&
          fed == endpoint.model_samples, "every captured sample accounted across reset and EOF");
    for (index = 0; index < 24000; ++index)
        source[index] = index < 3200 ? 0.02f : index < 8000 ? 0.001f : index < 11200 ? 0.03f : 0;
    begins = ends = used = 0; fed = 0; recording = 1;
    check(nm_endpoint_init(&endpoint, 800, 0.005f, ops, 0) &&
          nm_endpoint_feed(&endpoint, source, 24000) && nm_endpoint_finish(&endpoint) &&
          begins == 1 && ends == 1 && used == 14400 &&
          !memcmp(source, result, used * sizeof(float)),
          "live model input preserves speech and complete internal pause byte exactly");
    begins = ends = used = 0; fed = 0; recording = 2;
    check(nm_endpoint_init(&endpoint, 800, 0.005f, ops, 0), "partitioned endpoint configured");
    for (offset = 0; offset < 24000;) {
        u32 count = offset % 517 + 1;
        if (count > 24000 - offset) count = 24000 - offset;
        if (!nm_endpoint_feed(&endpoint, source + offset, count)) ++failed;
        offset += count;
    }
    check(nm_endpoint_finish(&endpoint) && used == 14400 && begins == 1 && ends == 1 &&
          !memcmp(result, partitioned, used * sizeof(float)) && endpoint.trimmed_samples == 9600,
          "endpoint policy partition invariant including event boundaries and trimming");
    begins = ends = used = 0; fed = 0; recording = 1;
    check(nm_endpoint_init(&endpoint, 800, 0.005f, ops, 0) &&
          nm_endpoint_feed(&endpoint, source, 4000) && nm_endpoint_finish(&endpoint) &&
          used == 4000 && !endpoint.trimmed_samples && !memcmp(result, source, used * sizeof(float)),
          "EOF before confirmed endpoint retains all tentative silence");
    recording = 0;
    check(nm_endpoint_init(&endpoint, 100, 0.005f, ops, 0) &&
          nm_endpoint_feed(&endpoint, source + 8000, 4800) && nm_endpoint_finish(&endpoint) &&
          !endpoint.trimmed_samples, "endpoint shorter than retained context never pads or trims");
    check(nm_endpoint_init(&endpoint, 10000, 0.005f, ops, 0) &&
          nm_endpoint_feed(&endpoint, source, 3200), "maximum endpoint bounded buffer setup");
    memset(source, 0, sizeof(source));
    check(nm_endpoint_feed(&endpoint, source, 96000) && nm_endpoint_feed(&endpoint, source, 64000) &&
          !endpoint.active && endpoint.trimmed_samples == 156800 && nm_endpoint_finish(&endpoint),
          "maximum10000ms endpoint bounded, trimmed and fully accounted");
    check(!nm_endpoint_init(&endpoint, 10001, 0.005f, ops, 0) && endpoint.error,
          "unbounded endpoint settings rejected explicitly");
    check(nm_capture_flags(&capture, 1) && capture.startup_discontinuities == 1, "first packet discontinuity explicitly counted");
    capture.packets = 1;
    check(!nm_capture_flags(&capture, 1) && capture.error, "later discontinuity is fatal");
    check(!nm_capture_flags(&capture, 4), "capture timestamp error explicit");
    check(!nm_capture_hresult(&capture, "injected device loss", (int32_t)0x88890004U), "device loss HRESULT path");
    check(!nm_capture_hresult(&capture, "injected privacy denial", (int32_t)0x80070005U), "permission failure path");
    memset(&capture, 0, sizeof(capture));
    check(nm_capture_position(&capture, 100, 480) && nm_capture_position(&capture, 580, 480),
          "capture source positions contiguous across packets");
    check(!nm_capture_position(&capture, 1080, 480) && capture.gap_frames == 20 && capture.error,
          "source gap fatal and quantified without trusting flags");
    check(!nm_capture_position(&capture, 1000, 480), "source position reversal fatal");
    check(!nm_capture_position(&capture, UINT64_MAX, 480), "source position overflow fatal");
    nm_text("Audio/capture/endpoint checks passed "); nm_number(passed); nm_text(" failed "); nm_number(failed); nm_text("\n");
    ExitProcess(failed ? 1 : 0);
}
