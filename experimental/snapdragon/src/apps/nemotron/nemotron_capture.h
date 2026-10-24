#ifndef NEWOS_NEMOTRON_CAPTURE_H
#define NEWOS_NEMOTRON_CAPTURE_H
#include "nemotron_windows.h"
#include "nemotron_audio.h"
typedef struct NmCapture {
    NmAudioQueue *queue;
    NmResampler resampler;
    u32 duration_s, thread_id, packets, startup_discontinuities, has_position, mmcss;
    u64 started, elapsed_ticks, source_frames, next_position, gap_frames;
    int32_t hresult;
    const char *error;
} NmCapture;
int nm_capture_run(NmCapture *capture);
int nm_capture_hresult(NmCapture *capture, const char *operation, int32_t code);
int nm_capture_flags(NmCapture *capture, uint32_t flags);
int nm_capture_position(NmCapture *capture, uint64_t position, uint32_t frames);
#endif
