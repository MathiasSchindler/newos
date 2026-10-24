#ifndef NEWOS_NEMOTRON_LIVE_H
#define NEWOS_NEMOTRON_LIVE_H
#include "nemotron_capture.h"
#include "concurrency.h"
typedef struct NmLive {
    NmAudioQueue queue;
    NmCapture capture;
    NmEndpoint endpoint;
    RtTaskPool pool;
    float *samples;
    const char *replay, *stats_path;
    uint32_t duration_s, replay_gap_ms, silence_ms, inference_thread, canceled;
    uint64_t started, elapsed_ticks, frequency, start_memory, min_memory, max_memory, end_memory;
    uint64_t measurements, queue_sum, queue_last, steady_memory;
    const char *error;
} NmLive;
int nm_live_run(NmLive *live, NmEndpointOps ops, void *context);
#endif
