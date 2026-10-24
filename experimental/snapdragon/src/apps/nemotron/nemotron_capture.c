#include "nemotron_capture.h"

typedef struct NmGuid { u32 a; u16 b, c; u8 d[8]; } NmGuid;
typedef struct NmCom { void **vtable; } NmCom;
typedef int32_t Hr;
typedef Hr (*CoInit)(void *, u32);
typedef void (*CoExit)(void);
typedef Hr (*CoCreate)(const NmGuid *, void *, u32, const NmGuid *, void **);
typedef void (*CoFree)(void *);
typedef void *(*AvRegister)(const u16 *, u32 *);
typedef int (*AvPriority)(void *, int);
typedef int (*AvRevert)(void *);
__declspec(dllimport) void *LoadLibraryExW(const u16 *, void *, u32);
__declspec(dllimport) void *CreateEventW(void *, int, int, const u16 *);
__declspec(dllimport) u32 WaitForSingleObject(void *, u32);
__declspec(dllimport) u32 GetCurrentThreadId(void);
__declspec(dllimport) int CloseHandle(void *);
__declspec(dllimport) u32 GetLastError(void);

static const NmGuid class_device = {0xbcde0395,0xe52f,0x467c,{0x8e,0x3d,0xc4,0x57,0x92,0x91,0x69,0x2e}};
static const NmGuid iid_enumerator = {0xa95664d2,0x9614,0x4f35,{0xa7,0x46,0xde,0x8d,0xb6,0x36,0x17,0xe6}};
static const NmGuid iid_audio = {0x1cb9ad4c,0xdbfa,0x4c32,{0xb1,0x78,0xc2,0xf5,0x68,0xa7,0x03,0xb2}};
static const NmGuid iid_capture = {0xc8adbd64,0xe71e,0x48a0,{0xa4,0xde,0x18,0x5c,0x39,0x5c,0xd3,0x17}};
static const NmGuid format_float = {3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
static const NmGuid format_pcm = {1,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
static void release(NmCom *object) {
    if (object) ((u32 (*)(void *))object->vtable[2])(object);
}
static int capture_error(NmCapture *capture, const char *operation, Hr code) {
    capture->error = operation; capture->hresult = code;
    nm_text("microphone: "); nm_text(operation); nm_text(" HRESULT="); nm_number((u32)code); nm_text("\n");
    if ((u32)code == 0x80070005U) nm_error("microphone access denied; check Windows desktop microphone privacy permission");
    if ((u32)code == 0x88890004U) nm_error("microphone device invalidated/lost");
    return 0;
}
int nm_capture_hresult(NmCapture *capture, const char *operation, int32_t code) {
    return code >= 0 || capture_error(capture, operation, code);
}
int nm_capture_flags(NmCapture *capture, uint32_t flags) {
    if (flags & ~7U) return capture_error(capture, "unsupported microphone buffer flags", (Hr)0x80070057U);
    if ((flags & 1) && capture->packets)
        return capture_error(capture, "microphone data discontinuity; samples not silently discarded", (Hr)0x80004005U);
    if (flags & 4) return capture_error(capture, "microphone timestamp error", (Hr)0x80004005U);
    if (flags & 1) ++capture->startup_discontinuities;
    return 1;
}
int nm_capture_position(NmCapture *capture, uint64_t position, uint32_t frames) {
    if (!frames || position > UINT64_MAX - frames)
        return capture_error(capture, "invalid microphone source-frame position", (Hr)0x80004005U);
    if (capture->has_position && position != capture->next_position) {
        if (position > capture->next_position) capture->gap_frames += position - capture->next_position;
        nm_text("microphone source discontinuity: expected="); nm_number(capture->next_position);
        nm_text(" actual="); nm_number(position); nm_text(" gap_frames="); nm_number(capture->gap_frames); nm_text("\n");
        return capture_error(capture, "microphone source-frame gap or reversal", (Hr)0x80004005U);
    }
    capture->next_position = position + frames; capture->has_position = 1; return 1;
}
static int capture_win32_error(NmCapture *capture, const char *operation) {
    u32 error = GetLastError();
    return capture_error(capture, operation, error ? (Hr)(0x80070000U | (error & 65535)) : (Hr)0x80004005U);
}
static u32 short_word(const u8 *p) { return p[0] | (u32)p[1] << 8; }
int nm_capture_run(NmCapture *capture) {
    static const u16 ole_name[] = {'o','l','e','3','2','.','d','l','l',0};
    static const u16 avrt_name[] = {'a','v','r','t','.','d','l','l',0};
    static const u16 audio_task[] = {'A','u','d','i','o',0};
    void *ole = 0, *event = 0, *avrt = 0, *av_task = 0; NmCom *enumerator = 0, *device = 0, *audio = 0, *client = 0;
    CoInit initialize = 0; CoExit uninitialize = 0; CoCreate create = 0; CoFree free_format = 0;
    AvRegister register_task = 0; AvPriority set_priority = 0; AvRevert revert_task = 0;
    u8 *format = 0; Hr code; int initialized = 0, started = 0, ok = 0;
    u32 rate, channels, encoding, align, bits, task_index = 0; u64 frequency;
    capture->thread_id = GetCurrentThreadId();
    ole = LoadLibraryExW(ole_name, 0, 0x800);
    if (!ole) { capture_error(capture, "load Windows Ole32", (Hr)0x80004005U); goto done; }
    initialize = (CoInit)nm_symbol(ole, "CoInitializeEx");
    uninitialize = (CoExit)nm_symbol(ole, "CoUninitialize");
    create = (CoCreate)nm_symbol(ole, "CoCreateInstance");
    free_format = (CoFree)nm_symbol(ole, "CoTaskMemFree");
    if (!initialize || !uninitialize || !create || !free_format) { capture_error(capture, "Windows COM API missing", (Hr)0x80004005U); goto done; }
    code = initialize(0, 0);
    if (code < 0) { capture_error(capture, "initialize capture COM apartment", code); goto done; }
    initialized = 1;
    code = create(&class_device, 0, 1, &iid_enumerator, (void **)&enumerator);
    if (code < 0 || !enumerator) { capture_error(capture, "create MMDeviceEnumerator", code); goto done; }
    code = ((Hr (*)(void *, int, int, void **))enumerator->vtable[4])(enumerator, 1, 0, (void **)&device);
    if (code < 0 || !device) { capture_error(capture, "open Windows default microphone", code); goto done; }
    code = ((Hr (*)(void *, const NmGuid *, u32, void *, void **))device->vtable[3])(device, &iid_audio, 1, 0, (void **)&audio);
    if (code < 0 || !audio) { capture_error(capture, "activate IAudioClient", code); goto done; }
    code = ((Hr (*)(void *, void **))audio->vtable[8])(audio, (void **)&format);
    if (code < 0 || !format) { capture_error(capture, "get microphone mix format", code); goto done; }
    encoding = short_word(format); channels = short_word(format + 2); rate = nm_u32(format + 4);
    align = short_word(format + 12); bits = short_word(format + 14);
    if (encoding == 0xfffe) {
        if (short_word(format + 16) < 22) { capture_error(capture, "truncated extensible mix format", (Hr)0x80070057U); goto done; }
        if (!memcmp(format + 24, &format_float, sizeof(format_float))) encoding = 3;
        else if (!memcmp(format + 24, &format_pcm, sizeof(format_pcm))) encoding = 1;
        else encoding = 0;
    }
    if ((encoding == 1 ? bits != 16 : encoding != 3 || bits != 32) ||
        align != channels * (bits / 8) || nm_u32(format + 8) != rate * align ||
        !nm_resampler_init(&capture->resampler, rate, channels, encoding)) {
        capture_error(capture, "unsupported microphone PCM16/float32 mix format", (Hr)0x80070057U); goto done;
    }
    nm_text("microphone default capture: rate="); nm_number(rate); nm_text(" channels="); nm_number(channels);
    nm_text(" encoding="); nm_number(encoding); nm_text(" target=16000 mono; raw audio is not recorded\n");
    code = ((Hr (*)(void *, int, u32, long long, long long, const void *, const NmGuid *))audio->vtable[3])(
        audio, 0, 0x40000, 1000000, 0, format, 0);
    if (code < 0) { capture_error(capture, "initialize WASAPI shared event capture", code); goto done; }
    event = CreateEventW(0, 0, 0, 0);
    if (!event) { capture_error(capture, "create WASAPI event", (Hr)0x80004005U); goto done; }
    code = ((Hr (*)(void *, void *))audio->vtable[13])(audio, event);
    if (code < 0) { capture_error(capture, "set WASAPI event handle", code); goto done; }
    code = ((Hr (*)(void *, const NmGuid *, void **))audio->vtable[14])(audio, &iid_capture, (void **)&client);
    if (code < 0 || !client) { capture_error(capture, "get IAudioCaptureClient", code); goto done; }
    frequency = nm_frequency();
    if (!frequency) { capture_error(capture, "capture QPC frequency unavailable", (Hr)0x80004005U); goto done; }
    avrt = LoadLibraryExW(avrt_name, 0, 0x800);
    if (!avrt) { capture_win32_error(capture, "load Windows Avrt"); goto done; }
    register_task = (AvRegister)nm_symbol(avrt, "AvSetMmThreadCharacteristicsW");
    set_priority = (AvPriority)nm_symbol(avrt, "AvSetMmThreadPriority");
    revert_task = (AvRevert)nm_symbol(avrt, "AvRevertMmThreadCharacteristics");
    if (!register_task || !set_priority || !revert_task) {
        capture_error(capture, "Windows MMCSS API missing", (Hr)0x80004005U); goto done;
    }
    av_task = register_task(audio_task, &task_index);
    if (!av_task) { capture_win32_error(capture, "register MMCSS Audio capture thread"); goto done; }
    if (!set_priority(av_task, 1)) { capture_win32_error(capture, "set MMCSS Audio HIGH capture priority"); goto done; }
    capture->mmcss = 1; nm_text("microphone: MMCSS Audio/HIGH registered; thread-only, restored at shutdown\n");
    code = ((Hr (*)(void *))audio->vtable[10])(audio);
    if (code < 0) { capture_error(capture, "start microphone", code); goto done; }
    started = 1; capture->started = nm_clock();
    while (!__atomic_load_n(&capture->queue->stop, __ATOMIC_ACQUIRE)) {
        u32 waiting, available = 0;
        if (capture->duration_s && nm_clock() - capture->started >= (u64)capture->duration_s * frequency) break;
        waiting = WaitForSingleObject(event, 100);
        if (waiting != 0 && waiting != 258) { capture_error(capture, "WASAPI event wait failed", (Hr)0x80004005U); goto done; }
        code = ((Hr (*)(void *, u32 *))client->vtable[5])(client, &available);
        if (!nm_capture_hresult(capture, "get capture packet size/device lost", code)) goto done;
        while (available) {
            u8 *data = 0; u32 frames = 0, flags = 0; u64 device_position = 0, timestamp = 0;
            int consumed;
            code = ((Hr (*)(void *, void **, u32 *, u32 *, u64 *, u64 *))client->vtable[3])(
                client, (void **)&data, &frames, &flags, &device_position, &timestamp);
            if (!nm_capture_hresult(capture, "get microphone buffer", code)) goto done;
            if (!frames) break;
            consumed = (flags & 4) || nm_capture_position(capture, device_position, frames);
            if (consumed) consumed = nm_capture_flags(capture, flags);
            if (consumed) {
                consumed = nm_resampler_feed(&capture->resampler, data, frames, (flags & 2) != 0,
                                             nm_audio_queue_push, capture->queue);
                if (!consumed && !__atomic_load_n(&capture->queue->stop, __ATOMIC_ACQUIRE))
                    capture_error(capture, capture->resampler.error, (Hr)0x80004005U);
            }
            code = ((Hr (*)(void *, u32))client->vtable[4])(client, frames);
            if (!nm_capture_hresult(capture, "release microphone buffer", code)) goto done;
            ++capture->packets; capture->source_frames += frames;
            if (!consumed) {
                if (__atomic_load_n(&capture->queue->stop, __ATOMIC_ACQUIRE) && !capture->queue->failure) break;
                if (!capture->error) capture_error(capture, "capture queue overflow or resampler failure", (Hr)0x80004005U);
                goto done;
            }
            code = ((Hr (*)(void *, u32 *))client->vtable[5])(client, &available);
            if (!nm_capture_hresult(capture, "capture device invalidated", code)) goto done;
        }
    }
    ok = 1;
done:
    if (started) {
        capture->elapsed_ticks = nm_clock() - capture->started;
        code = ((Hr (*)(void *))audio->vtable[11])(audio);
        if (code < 0) { capture_error(capture, "stop microphone", code); ok = 0; }
    }
    release(client); release(audio); release(device); release(enumerator);
    if (format && free_format) free_format(format);
    if (event && !CloseHandle(event)) { capture_error(capture, "close microphone event", (Hr)0x80004005U); ok = 0; }
    if (initialized) uninitialize();
    if (av_task && !revert_task(av_task)) { capture_win32_error(capture, "restore capture thread MMCSS registration"); ok = 0; }
    nm_unload(avrt);
    nm_unload(ole);
    if (!ok) nm_audio_queue_stop(capture->queue);
    nm_audio_queue_end(capture->queue, ok ? 0 : 2);
    return ok;
}
