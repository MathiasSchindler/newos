#include "../nemotron_live.h"
#include "platform.h"

__declspec(dllimport) void Sleep(u32 milliseconds);
static NmLive live;
static int begin(void *context, uint64_t sample) { (void)context; (void)sample; return 1; }
static int feed(void *context, const float *samples, uint32_t count) { (void)context; (void)samples; (void)count; return 1; }
static int end(void *context, uint64_t sample, uint64_t speech) { (void)context; (void)sample; (void)speech; return 1; }
static int cancel(void *context) {
    NmLive *pipeline = context;
    Sleep(2000);
    __atomic_store_n(&pipeline->canceled, 1, __ATOMIC_RELEASE);
    nm_audio_queue_stop(&pipeline->queue); return 0;
}
void mainCRTStartup(void) {
    static char args[32][4096]; PlatformThread thread; NmEndpointOps ops = {begin, feed, end};
    int result = 0, ok;
    if (nm_arguments(args) != 2) { nm_error("usage: test-live-cancel PCM16_WAV"); ExitProcess(1); }
    live.replay = args[1]; live.silence_ms = 800; live.replay_gap_ms = 0;
    if (platform_thread_start(&thread, cancel, &live, 65536) != 0) { nm_error("cancel test watchdog start failed"); ExitProcess(1); }
    ok = nm_live_run(&live, ops, 0);
    if (platform_thread_join(&thread, &result) != 0 || result || !live.canceled ||
        live.queue.failure || live.queue.accepted != live.queue.consumed ||
        live.elapsed_ticks > live.frequency * 4) ok = 0;
    nm_text(ok ? "PASS live cancellation/drain/task-pool shutdown\n" : "FAIL live cancellation\n");
    ExitProcess(ok ? 0 : 1);
}
