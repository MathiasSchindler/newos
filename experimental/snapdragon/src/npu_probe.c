#include "qnn_abi.h"
#include "whisper_artifact.h"
#include "whisper_decoder.h"
#include "whisper_decoder_qnn.h"
#include "whisper_encoder_qnn.h"
#include "whisper_frontend.h"

typedef unsigned long long usize;

enum {
    TINY_WIDTH = WHISPER_TINY_WIDTH,
    TINY_MLP_WIDTH = WHISPER_TINY_FFN_WIDTH,
    TINY_SEQUENCE_LENGTH = WHISPER_TINY_ENCODER_FRAMES,
    TINY_ATTENTION_HEADS = WHISPER_TINY_ATTENTION_HEADS,
    TINY_ATTENTION_HEAD_WIDTH = WHISPER_TINY_WIDTH / WHISPER_TINY_ATTENTION_HEADS,
    ATTENTION_PROJECTION_COUNT = 3,
    BENCHMARK_WARMUPS = 10,
    BENCHMARK_SAMPLES = 100
};

static u8 tiny_input[TINY_SEQUENCE_LENGTH * TINY_MLP_WIDTH];
static u8 tiny_weights[TINY_WIDTH * TINY_MLP_WIDTH];
static u8 tiny_output[TINY_SEQUENCE_LENGTH * TINY_MLP_WIDTH];
static u8 tiny_expected[TINY_SEQUENCE_LENGTH * TINY_MLP_WIDTH];
static u8 model_input[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u8 model_output[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u8 model_expected[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static i8 model_fc1_weights[TINY_MLP_WIDTH * TINY_WIDTH];
static i8 model_fc2_weights[TINY_WIDTH * TINY_MLP_WIDTH];
static i32 model_fc1_bias[TINY_MLP_WIDTH];
static i32 model_fc2_bias[TINY_WIDTH];
static float model_fc1_weight_scales[TINY_MLP_WIDTH];
static float model_fc2_weight_scales[TINY_WIDTH];
static QnnScaleOffset model_fc1_weight_quant[TINY_MLP_WIDTH];
static QnnScaleOffset model_fc2_weight_quant[TINY_WIDTH];
static QnnScaleOffset model_fc1_bias_quant[TINY_MLP_WIDTH];
static QnnScaleOffset model_fc2_bias_quant[TINY_WIDTH];
static QnnScaleOffset model_activation_quant[4];
static u8 model_attention_input[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u8 model_attention_output[ATTENTION_PROJECTION_COUNT][TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u8 model_attention_expected[ATTENTION_PROJECTION_COUNT][TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static i8 model_attention_weights[ATTENTION_PROJECTION_COUNT][TINY_WIDTH * TINY_WIDTH];
static i32 model_attention_bias[ATTENTION_PROJECTION_COUNT][TINY_WIDTH];
static float model_attention_weight_scales[ATTENTION_PROJECTION_COUNT][TINY_WIDTH];
static QnnScaleOffset model_attention_weight_quant[ATTENTION_PROJECTION_COUNT][TINY_WIDTH];
static QnnScaleOffset model_attention_bias_quant[ATTENTION_PROJECTION_COUNT][TINY_WIDTH];
static QnnScaleOffset model_attention_activation_quant[ATTENTION_PROJECTION_COUNT + 1];
static u8 model_attention_core_input[ATTENTION_PROJECTION_COUNT][TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u8 model_attention_core_output[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u8 model_attention_core_expected[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static QnnScaleOffset model_attention_core_quant[6];
static u8 model_block_input[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u8 model_block_attention_input[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u8 model_block_output[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u8 model_layer_norm_scale[TINY_WIDTH];
static i32 model_layer_norm_bias[TINY_WIDTH];
static QnnScaleOffset model_block_quant[15];
static QnnScaleOffset model_layer_norm_scale_quant;
static i8 model_out_proj_weights[TINY_WIDTH * TINY_WIDTH];
static i32 model_out_proj_bias[TINY_WIDTH];
static float model_out_proj_weight_scales[TINY_WIDTH];
static QnnScaleOffset model_out_proj_weight_quant[TINY_WIDTH];
static QnnScaleOffset model_out_proj_bias_quant[TINY_WIDTH];
static u8 model_final_layer_norm_scale[TINY_WIDTH];
static i32 model_final_layer_norm_bias[TINY_WIDTH];
static QnnScaleOffset model_final_layer_norm_scale_quant;
static QnnGraphHandle model_encoder_graphs[4];
static QnnTensor model_encoder_inputs[4];
static QnnTensor model_encoder_outputs[4];
static u32 model_encoder_activation_dimensions[2] = {TINY_SEQUENCE_LENGTH, TINY_WIDTH};
static u8 model_encoder_stack_input[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u8 model_encoder_stack_buffers[2][TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u8 model_encoder_stack_expected[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u16 model_fp16_block_input[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u16 model_fp16_block_output[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u16 model_fp16_block_expected[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u16 model_fp16_attention_weights[4][ATTENTION_PROJECTION_COUNT][TINY_WIDTH * TINY_WIDTH];
static u16 model_fp16_attention_bias[4][ATTENTION_PROJECTION_COUNT][TINY_WIDTH];
static u16 model_fp16_out_proj_weights[4][TINY_WIDTH * TINY_WIDTH];
static u16 model_fp16_out_proj_bias[4][TINY_WIDTH];
static u16 model_fp16_fc1_weights[4][TINY_MLP_WIDTH * TINY_WIDTH];
static u16 model_fp16_fc1_bias[4][TINY_MLP_WIDTH];
static u16 model_fp16_fc2_weights[4][TINY_WIDTH * TINY_MLP_WIDTH];
static u16 model_fp16_fc2_bias[4][TINY_WIDTH];
static u16 model_fp16_layer_norm_scale[4][TINY_WIDTH];
static u16 model_fp16_layer_norm_bias[4][TINY_WIDTH];
static u16 model_fp16_final_layer_norm_scale[4][TINY_WIDTH];
static u16 model_fp16_final_layer_norm_bias[4][TINY_WIDTH];
static QnnGraphHandle model_fp16_encoder_graphs[4];
static QnnTensor model_fp16_encoder_inputs[4];
static QnnTensor model_fp16_encoder_outputs[4];
static u16 model_fp16_encoder_stack_input[TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static u16 model_fp16_encoder_stack_buffers[2][TINY_SEQUENCE_LENGTH * TINY_WIDTH];
static QnnGraphHandle model_fp16_monolithic_graph;
static QnnTensor model_fp16_monolithic_input;
static QnnTensor model_fp16_monolithic_output;
static QnnTensor model_fp16_monolithic_previous_output;
static i32 model_monolithic_name_layer = -1;
static u32 model_monolithic_node_index;
static char model_monolithic_node_names[4][22][64];
static double frontend_hann_window[WHISPER_FFT_SIZE];
static double frontend_dft_roots[WHISPER_FFT_SIZE * 2U];
static double frontend_mel_filters[(WHISPER_FFT_SIZE / 2U + 1U) * WHISPER_MEL_BINS];
static float frontend_log_mel[WHISPER_MEL_BINS * WHISPER_FRAME_COUNT];
static float frontend_log_mel_expected[WHISPER_MEL_BINS * WHISPER_FRAME_COUNT];
static u16 frontend_conv1_input[WHISPER_FRAME_COUNT * WHISPER_MEL_BINS * 3U];
static u16 frontend_conv1_input_expected[WHISPER_FRAME_COUNT * WHISPER_MEL_BINS * 3U];
static u16 frontend_conv1_weight[WHISPER_HIDDEN_SIZE * WHISPER_MEL_BINS * 3U];
static u16 frontend_conv1_bias[WHISPER_HIDDEN_SIZE];
static u16 frontend_conv1_output[WHISPER_FRAME_COUNT * WHISPER_HIDDEN_SIZE];
static u16 frontend_conv1_expected[WHISPER_FRAME_COUNT * WHISPER_HIDDEN_SIZE];
static u16 frontend_conv2_input[WHISPER_ENCODER_FRAMES * WHISPER_HIDDEN_SIZE * 3U];
static u16 frontend_conv2_weight[WHISPER_HIDDEN_SIZE * WHISPER_HIDDEN_SIZE * 3U];
static u16 frontend_conv2_bias[WHISPER_HIDDEN_SIZE];
static u16 frontend_positions[WHISPER_ENCODER_FRAMES * WHISPER_HIDDEN_SIZE];
static u16 frontend_output[WHISPER_ENCODER_FRAMES * WHISPER_HIDDEN_SIZE];
static u16 frontend_expected[WHISPER_ENCODER_FRAMES * WHISPER_HIDDEN_SIZE];
static QnnGraphHandle frontend_graphs[2];
static QnnTensor frontend_inputs[2];
static QnnTensor frontend_outputs[2];
static WhisperEncoderQnnIds encoder_qnn_ids;
static WhisperEncoderQnn *whisper_encoder_qnn;
static WhisperDecoderQnnIds decoder_qnn_ids;
static WhisperDecoderQnn *whisper_decoder_qnn;
static WhisperDecoder *whisper_decoder;
static const WhisperModelConfig *active_model;
static u32 frontend_input_dimensions[2][2] = {
    {WHISPER_FRAME_COUNT, WHISPER_MEL_BINS * 3U},
    {WHISPER_ENCODER_FRAMES, WHISPER_HIDDEN_SIZE * 3U}
};
static u32 frontend_weight_dimensions[2][2] = {
    {WHISPER_HIDDEN_SIZE, WHISPER_MEL_BINS * 3U},
    {WHISPER_HIDDEN_SIZE, WHISPER_HIDDEN_SIZE * 3U}
};
static u32 frontend_output_dimensions[2][2] = {
    {WHISPER_FRAME_COUNT, WHISPER_HIDDEN_SIZE},
    {WHISPER_ENCODER_FRAMES, WHISPER_HIDDEN_SIZE}
};
static u32 frontend_bias_dimensions[1] = {WHISPER_HIDDEN_SIZE};
static char external_wav_path[512];
static int quiet_output;
static int command_argument_error;
static u32 decoder_worker_count;

typedef struct WavManifestReader {
    void *handle;
    u8 buffer[4096];
    u32 position;
    u32 count;
} WavManifestReader;

static WavManifestReader wav_manifest_reader;
static char wav_manifest_path[512];

enum {
    LONG_FORM_STEP_SAMPLES = 25U * WHISPER_SAMPLE_RATE,
    LONG_FORM_OVERLAP_SAMPLES = 5U * WHISPER_SAMPLE_RATE,
    SEGMENT_TRANSCRIPT_CAPACITY = 16384U,
    STITCHED_TRANSCRIPT_CAPACITY = 1048576U,
    OVERLAP_WORD_LIMIT = 50U
};

typedef struct TranscriptWord {
    u32 start;
    u32 end;
} TranscriptWord;

static char segment_transcript[SEGMENT_TRANSCRIPT_CAPACITY];
static u32 segment_transcript_size;
static int segment_transcript_overflow;
static int capture_decoder_output;
static char stitched_transcript[STITCHED_TRANSCRIPT_CAPACITY];
static u32 stitched_transcript_size;

typedef struct ModelContextCacheMetadata {
    u64 binary_size;
    WhisperEncoderQnnIds encoder;
    WhisperDecoderQnnIds decoder;
} ModelContextCacheMetadata;

static char model_context_cache_primary[192];
static char model_context_cache_local[96];

enum {
    MODEL_CONTEXT_CACHE_METADATA_SIZE =
    8U + 11U * 4U +
    (WHISPER_DECODER_QNN_MAX_OUTPUTS +
        WHISPER_DECODER_QNN_MAX_LAYERS * 2U) * 4U +
        (1U + WHISPER_DECODER_QNN_MAX_LAYERS * 4U) * 4U
};

typedef struct ProcessMemoryCounters {
    u32 size;
    u32 page_fault_count;
    usize peak_working_set_size;
    usize working_set_size;
    usize quota_peak_paged_pool_usage;
    usize quota_paged_pool_usage;
    usize quota_peak_nonpaged_pool_usage;
    usize quota_nonpaged_pool_usage;
    usize pagefile_usage;
    usize peak_pagefile_usage;
    usize private_usage;
} ProcessMemoryCounters;

__declspec(dllimport) int CloseHandle(void *handle);
__declspec(dllimport) void *CreateFileA(const char *name, u32 access, u32 sharing, void *security, u32 creation, u32 attributes, void *template_file);
__declspec(dllimport) void ExitProcess(u32 exit_code);
__declspec(dllimport) char *GetCommandLineA(void);
__declspec(dllimport) void *GetCurrentProcess(void);
__declspec(dllimport) int GetConsoleMode(void *console, u32 *mode);
__declspec(dllimport) u32 GetConsoleOutputCP(void);
__declspec(dllimport) void *GetProcAddress(void *module, const char *name);
__declspec(dllimport) u32 GetLastError(void);
__declspec(dllimport) void *GetStdHandle(u32 handle_id);
__declspec(dllimport) void *LoadLibraryA(const char *name);
__declspec(dllimport) int K32GetProcessMemoryInfo(
    void *process, ProcessMemoryCounters *counters, u32 size
);
__declspec(dllimport) int QueryPerformanceCounter(long long *value);
__declspec(dllimport) int QueryPerformanceFrequency(long long *value);
__declspec(dllimport) int ReadFile(void *handle, void *buffer, u32 size, u32 *read, void *overlapped);
__declspec(dllimport) int SetConsoleOutputCP(u32 code_page);
__declspec(dllimport) int SetStdHandle(u32 handle_id, void *handle);
__declspec(dllimport) int FreeLibrary(void *module);
__declspec(dllimport) void *VirtualAlloc(void *address, usize size, u32 allocation_type, u32 protect);
__declspec(dllimport) int VirtualFree(void *address, usize size, u32 free_type);
__declspec(dllimport) int WriteFile(void *handle, const void *buffer, u32 size, u32 *written, void *overlapped);

static void *stdout_handle;
static void *quiet_stderr_handle;
static u32 original_console_output_cp;
static int console_output_cp_changed;

static usize text_length(const char *text) {
    usize length = 0U;
    while (text[length] != '\0') length += 1U;
    return length;
}

static int append_path_text(
    char *path,
    u32 capacity,
    u32 *used,
    const char *text
) {
    while (*text != '\0') {
        if (*used + 1U >= capacity) return 0;
        path[(*used)++] = *text++;
    }
    path[*used] = '\0';
    return 1;
}

static int initialize_model_context_paths(const WhisperModelConfig *model) {
    u32 primary_used = 0U;
    u32 local_used = 0U;
    return append_path_text(
            model_context_cache_primary, sizeof(model_context_cache_primary),
            &primary_used, "experimental/snapdragon/build/whisper-"
        ) && append_path_text(
            model_context_cache_primary, sizeof(model_context_cache_primary),
            &primary_used, model->name
        ) && append_path_text(
            model_context_cache_primary, sizeof(model_context_cache_primary),
            &primary_used, "-encoder-fp16.qnnctx"
        ) && append_path_text(
            model_context_cache_local, sizeof(model_context_cache_local),
            &local_used, "whisper-"
        ) && append_path_text(
            model_context_cache_local, sizeof(model_context_cache_local),
            &local_used, model->name
        ) && append_path_text(
            model_context_cache_local, sizeof(model_context_cache_local),
            &local_used, "-encoder-fp16.qnnctx"
        );
}

static int read_command_argument(const char **command, char *output, u32 capacity) {
    const char *cursor = *command;
    u32 quoted = 0U;
    u32 length = 0U;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (*cursor == '\0') return 0;
    if (*cursor == '"') {
        quoted = 1U;
        ++cursor;
    }
    while (*cursor != '\0') {
        if ((quoted && *cursor == '"') || (!quoted && (*cursor == ' ' || *cursor == '\t'))) break;
        if (length + 1U >= capacity) return -1;
        output[length++] = *cursor++;
    }
    if (quoted && *cursor == '"') ++cursor;
    output[length] = '\0';
    *command = cursor;
    return length == 0U ? 0 : 1;
}

static int argument_equals(const char *left, const char *right) {
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

static int argument_has_prefix(const char *text, const char *prefix) {
    while (*prefix != '\0' && *text == *prefix) {
        ++text;
        ++prefix;
    }
    return *prefix == '\0';
}

static int parse_worker_count(const char *text, u32 *value) {
    u32 parsed = 0U;
    if (*text == '\0') return 0;
    while (*text != '\0') {
        if (*text < '0' || *text > '9' || parsed > 3U) return 0;
        parsed = parsed * 10U + (u32)(*text++ - '0');
    }
    if (parsed == 0U || parsed > 32U) return 0;
    *value = parsed;
    return 1;
}

static const char *first_command_argument(void) {
    const char *cursor = GetCommandLineA();
    int result;
    u32 index;
    if (cursor == 0) return 0;
    if (*cursor == '"') {
        ++cursor;
        while (*cursor != '\0' && *cursor != '"') ++cursor;
        if (*cursor == '"') ++cursor;
    } else {
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') ++cursor;
    }
    for (;;) {
        result = read_command_argument(&cursor, external_wav_path, sizeof(external_wav_path));
        if (result <= 0) {
            command_argument_error = result < 0;
            return 0;
        }
        if (argument_equals(external_wav_path, "--quiet")) {
            quiet_output = 1;
            continue;
        }
        if (argument_equals(external_wav_path, "--model=tiny")) {
            active_model = whisper_model_tiny();
            continue;
        }
        if (argument_equals(external_wav_path, "--model=base")) {
            active_model = whisper_model_base();
            continue;
        }
        if (argument_equals(external_wav_path, "--model=small")) {
            active_model = whisper_model_small();
            continue;
        }
        if (argument_has_prefix(external_wav_path, "--decoder-workers=")) {
            if (!parse_worker_count(
                    external_wav_path + 18U, &decoder_worker_count
                )) {
                command_argument_error = 1;
                return 0;
            }
            continue;
        }
        if (argument_has_prefix(external_wav_path, "--model=")) {
            command_argument_error = 1;
            return 0;
        }
        if (argument_has_prefix(external_wav_path, "--quiet=")) {
            quiet_output = 1;
            index = 8U;
            if (external_wav_path[index] == '\0') {
                command_argument_error = 1;
                return 0;
            }
            do {
                external_wav_path[index - 8U] = external_wav_path[index];
            } while (external_wav_path[index++] != '\0');
        }
        return external_wav_path;
    }
}

static int wav_manifest_open(WavManifestReader *reader, const char *path) {
    void *invalid = (void *)(usize)-1;
    reader->handle = CreateFileA(path, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
    reader->position = 0U;
    reader->count = 0U;
    return reader->handle == invalid ? 0 : 1;
}

static int wav_manifest_next(
    WavManifestReader *reader,
    char *path,
    u32 capacity
) {
    u32 length = 0U;
    for (;;) {
        u8 value;
        if (reader->position == reader->count) {
            u32 count = 0U;
            if (!ReadFile(
                    reader->handle, reader->buffer, sizeof(reader->buffer),
                    &count, 0)) return -1;
            reader->position = 0U;
            reader->count = count;
            if (count == 0U) {
                if (length == 0U) return 0;
                path[length] = '\0';
                return 1;
            }
        }
        value = reader->buffer[reader->position++];
        if (value == '\r') continue;
        if (value == '\n') {
            if (length == 0U) continue;
            path[length] = '\0';
            return 1;
        }
        if (length + 1U >= capacity) return -1;
        path[length++] = (char)value;
    }
}

static void make_layer_name(char *output, u32 capacity, u32 layer, const char *name) {
    u32 used = 0U;
    u32 index = 0U;
    const char prefix[] = "mono_l";
    while (prefix[index] != '\0' && used + 1U < capacity) output[used++] = prefix[index++];
    if (used + 1U < capacity) output[used++] = (char)('0' + layer);
    if (used + 1U < capacity) output[used++] = '_';
    index = 0U;
    while (name[index] != '\0' && used + 1U < capacity) output[used++] = name[index++];
    output[used] = '\0';
}

static void write_raw_bytes(const char *data, usize size) {
    while (size != 0U) {
        u32 chunk = size > 0xffffffffULL ? 0xffffffffU : (u32)size;
        u32 written = 0U;
        if (!WriteFile(stdout_handle, data, chunk, &written, 0) || written == 0U) return;
        data += written;
        size -= written;
    }
}

static void write_bytes(const char *data, usize size) {
    if (!quiet_output) write_raw_bytes(data, size);
}

static void silence_standard_error(void) {
    void *invalid = (void *)(usize)-1;
    void *handle = CreateFileA("NUL", 0x40000000U, 3U, 0, 3U, 0x80U, 0);
    if (handle == invalid) return;
    if (!SetStdHandle(0xfffffff4U, handle)) {
        CloseHandle(handle);
        return;
    }
    quiet_stderr_handle = handle;
}

static void write_decoder_bytes(const char *data, u32 size) {
    u32 index;
    if (capture_decoder_output) {
        if (size > SEGMENT_TRANSCRIPT_CAPACITY - 1U - segment_transcript_size) {
            segment_transcript_overflow = 1;
            return;
        }
        for (index = 0U; index < size; ++index) {
            segment_transcript[segment_transcript_size++] = data[index];
        }
        segment_transcript[segment_transcript_size] = '\0';
        return;
    }
    write_raw_bytes(data, size);
}

static void write_text(const char *text) {
    write_bytes(text, text_length(text));
}

static void write_u32(u32 value) {
    char digits[10];
    usize used = 0U;
    do {
        digits[used++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (used != 0U) write_bytes(&digits[--used], 1U);
}

static void write_u64(u64 value) {
    char digits[20];
    usize used = 0U;
    do {
        digits[used++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (used != 0U) write_bytes(&digits[--used], 1U);
}

static void write_process_memory(void) {
    ProcessMemoryCounters counters;
    counters.size = sizeof(counters);
    if (!K32GetProcessMemoryInfo(
            GetCurrentProcess(), &counters, sizeof(counters)
        )) return;
    write_text("  process peak resident bytes: ");
    write_u64(counters.peak_working_set_size);
    write_text("\n  process resident bytes: ");
    write_u64(counters.working_set_size);
    write_text("\n  process private committed bytes: ");
    write_u64(counters.private_usage);
    write_text("\n");
}

static int text_has_prefix(const char *text, const char *prefix) {
    u32 index = 0U;
    while (prefix[index] != '\0') {
        if (text[index] != prefix[index]) return 0;
        ++index;
    }
    return 1;
}

static int transcript_space(u8 value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static int transcript_word_byte(u8 value) {
    return (value >= '0' && value <= '9') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= 'a' && value <= 'z') || value >= 0x80U;
}

static u8 transcript_fold_byte(u8 value) {
    return value >= 'A' && value <= 'Z' ? (u8)(value + ('a' - 'A')) : value;
}

static u32 collect_first_words(
    const char *text,
    u32 size,
    TranscriptWord *words,
    u32 capacity
) {
    u32 count = 0U;
    u32 index = 0U;
    while (index < size && count < capacity) {
        while (index < size && transcript_space((u8)text[index])) ++index;
        if (index == size) break;
        words[count].start = index;
        while (index < size && !transcript_space((u8)text[index])) ++index;
        words[count].end = index;
        ++count;
    }
    return count;
}

static u32 collect_last_words(
    const char *text,
    u32 size,
    TranscriptWord *words,
    u32 capacity
) {
    u32 count = 0U;
    u32 index = 0U;
    while (index < size) {
        TranscriptWord word;
        u32 move;
        while (index < size && transcript_space((u8)text[index])) ++index;
        if (index == size) break;
        word.start = index;
        while (index < size && !transcript_space((u8)text[index])) ++index;
        word.end = index;
        if (count < capacity) {
            words[count++] = word;
        } else {
            for (move = 1U; move < capacity; ++move) words[move - 1U] = words[move];
            words[capacity - 1U] = word;
        }
    }
    return count;
}

static int transcript_words_equal(
    const char *left,
    TranscriptWord left_word,
    const char *right,
    TranscriptWord right_word
) {
    u32 left_index = left_word.start;
    u32 right_index = right_word.start;
    for (;;) {
        while (left_index < left_word.end &&
               !transcript_word_byte((u8)left[left_index])) ++left_index;
        while (right_index < right_word.end &&
               !transcript_word_byte((u8)right[right_index])) ++right_index;
        if (left_index == left_word.end || right_index == right_word.end) {
            return left_index == left_word.end && right_index == right_word.end;
        }
        if (transcript_fold_byte((u8)left[left_index]) !=
            transcript_fold_byte((u8)right[right_index])) return 0;
        ++left_index;
        ++right_index;
    }
}

static u32 long_form_overlap_words(
    const TranscriptWord *old_words,
    u32 old_count,
    const TranscriptWord *new_words,
    u32 new_count
) {
    u32 maximum = old_count < new_count ? old_count : new_count;
    u32 new_limit = new_count < OVERLAP_WORD_LIMIT ? new_count : OVERLAP_WORD_LIMIT;
    u32 count;
    u32 best_length = 0U;
    u32 best_new_end = 0U;
    u32 old_index;
    if (maximum > OVERLAP_WORD_LIMIT) maximum = OVERLAP_WORD_LIMIT;
    for (count = maximum; count >= 3U; --count) {
        u32 index;
        for (index = 0U; index < count; ++index) {
            if (!transcript_words_equal(
                    stitched_transcript, old_words[old_count - count + index],
                    segment_transcript, new_words[index])) break;
        }
        if (index == count) return count;
    }
    for (old_index = 0U; old_index < old_count; ++old_index) {
        u32 new_index;
        for (new_index = 0U; new_index < new_limit; ++new_index) {
            u32 length = 0U;
            u32 old_words_after;
            while (old_index + length < old_count &&
                     new_index + length < new_limit &&
                   transcript_words_equal(
                       stitched_transcript, old_words[old_index + length],
                       segment_transcript, new_words[new_index + length])) {
                ++length;
            }
            old_words_after = old_count - (old_index + length);
            if (length >= 4U && old_words_after <= 12U &&
                (length > best_length ||
                 (length == best_length && new_index + length > best_new_end))) {
                best_length = length;
                best_new_end = new_index + length;
            }
        }
    }
    return best_length >= 4U ? best_new_end : 0U;
}

static int stitch_segment_transcript(u32 *appended_start) {
    TranscriptWord old_words[OVERLAP_WORD_LIMIT];
    TranscriptWord new_words[OVERLAP_WORD_LIMIT + 1U];
    u32 old_count = collect_last_words(
        stitched_transcript, stitched_transcript_size,
        old_words, OVERLAP_WORD_LIMIT
    );
    u32 new_count = collect_first_words(
        segment_transcript, segment_transcript_size,
        new_words, OVERLAP_WORD_LIMIT + 1U
    );
    u32 overlap = long_form_overlap_words(old_words, old_count, new_words, new_count);
    u32 start = overlap < new_count ? new_words[overlap].start : segment_transcript_size;
    u32 end = segment_transcript_size;
    u32 index;
    *appended_start = stitched_transcript_size;
    while (start < end && transcript_space((u8)segment_transcript[start])) ++start;
    while (end > start && transcript_space((u8)segment_transcript[end - 1U])) --end;
    if (start == end) return 1;
    if (stitched_transcript_size != 0U) {
        if (stitched_transcript_size + 1U >= STITCHED_TRANSCRIPT_CAPACITY) return 0;
        stitched_transcript[stitched_transcript_size++] = ' ';
    }
    if (end - start >= STITCHED_TRANSCRIPT_CAPACITY - stitched_transcript_size) return 0;
    for (index = start; index < end; ++index) {
        stitched_transcript[stitched_transcript_size++] = segment_transcript[index];
    }
    stitched_transcript[stitched_transcript_size] = '\0';
    return 1;
}

static void write_fraction3(u64 value) {
    if (value < 100U) write_text("0");
    if (value < 10U) write_text("0");
    write_u64(value);
}

static void write_duration_us(const char *name, u64 ticks, u64 frequency) {
    u64 nanoseconds = ticks * 1000000000ULL / frequency;
    write_text(name);
    write_text(": ");
    write_u64(nanoseconds / 1000U);
    write_text(".");
    write_fraction3(nanoseconds % 1000U);
    write_text(" us\n");
}

static void write_hex64(u64 value) {
    static const char hex[] = "0123456789abcdef";
    char digits[16];
    usize index;
    for (index = 0U; index < 16U; ++index) {
        digits[15U - index] = hex[value & 15U];
        value >>= 4U;
    }
    write_text("0x");
    write_bytes(digits, 16U);
}

static void write_version(const QnnVersion *version) {
    write_u32(version->major);
    write_text(".");
    write_u32(version->minor);
    write_text(".");
    write_u32(version->patch);
}

static void write_call_status(const char *name, u64 status) {
    write_text(name);
    write_text(": ");
    write_hex64(status);
    write_text("\n");
}

static QnnTensor make_tensor(const char *name, u32 type, u32 *dimensions, u32 rank) {
    QnnTensor tensor = {0};
    tensor.version = QNN_TENSOR_VERSION_1;
    tensor.data.v1.name = name;
    tensor.data.v1.type = type;
    tensor.data.v1.data_format = QNN_TENSOR_DATA_FORMAT_DENSE;
    tensor.data.v1.data_type = QNN_DATATYPE_UFIXED_POINT_8;
    tensor.data.v1.quantize_params.encoding_definition = QNN_DEFINITION_DEFINED;
    tensor.data.v1.quantize_params.quantization_encoding = QNN_QUANTIZATION_ENCODING_SCALE_OFFSET;
    tensor.data.v1.quantize_params.encoding.scale_offset.scale = 1.0f;
    tensor.data.v1.quantize_params.encoding.scale_offset.offset = 0;
    tensor.data.v1.rank = rank;
    tensor.data.v1.dimensions = dimensions;
    tensor.data.v1.memory_type = QNN_TENSORMEMTYPE_RAW;
    return tensor;
}

static QnnTensor make_plain_tensor(
    const char *name,
    u32 type,
    u32 data_type,
    u32 *dimensions,
    u32 rank
) {
    QnnTensor tensor = {0};
    tensor.version = QNN_TENSOR_VERSION_1;
    tensor.data.v1.name = name;
    tensor.data.v1.type = type;
    tensor.data.v1.data_format = QNN_TENSOR_DATA_FORMAT_DENSE;
    tensor.data.v1.data_type = data_type;
    tensor.data.v1.quantize_params.encoding_definition = QNN_DEFINITION_UNDEFINED;
    tensor.data.v1.quantize_params.quantization_encoding = QNN_QUANTIZATION_ENCODING_UNDEFINED;
    tensor.data.v1.rank = rank;
    tensor.data.v1.dimensions = dimensions;
    tensor.data.v1.memory_type = QNN_TENSORMEMTYPE_RAW;
    return tensor;
}

static QnnTensor make_axis_tensor(
    const char *name,
    u32 type,
    u32 data_type,
    u32 *dimensions,
    u32 rank,
    i32 axis,
    u32 scale_offset_count,
    QnnScaleOffset *scale_offsets
) {
    QnnTensor tensor = make_tensor(name, type, dimensions, rank);
    tensor.data.v1.data_type = data_type;
    tensor.data.v1.quantize_params.quantization_encoding = QNN_QUANTIZATION_ENCODING_AXIS_SCALE_OFFSET;
    tensor.data.v1.quantize_params.encoding.axis_scale_offset.axis = axis;
    tensor.data.v1.quantize_params.encoding.axis_scale_offset.scale_offset_count = scale_offset_count;
    tensor.data.v1.quantize_params.encoding.axis_scale_offset.scale_offsets = scale_offsets;
    return tensor;
}

static void set_tensor_scale_offset(QnnTensor *tensor, QnnScaleOffset encoding) {
    tensor->data.v1.quantize_params.encoding.scale_offset = encoding;
}

static QnnTensor make_encoder_tensor(
    const char *name,
    u32 type,
    u32 *dimensions,
    u32 rank,
    int fp16,
    QnnScaleOffset encoding
) {
    QnnTensor tensor;
    if (fp16) return make_plain_tensor(name, type, QNN_DATATYPE_FLOAT_16, dimensions, rank);
    tensor = make_tensor(name, type, dimensions, rank);
    set_tensor_scale_offset(&tensor, encoding);
    return tensor;
}

static void copy_tensor(QnnTensor *destination, const QnnTensor *source) {
    u32 index;
    for (index = 0U; index < sizeof(*destination); ++index) {
        ((u8 *)destination)[index] = ((const u8 *)source)[index];
    }
}

#define MODEL_PRIMARY(file) "experimental/snapdragon/models/whisper-tiny/encoder-layer-0-mlp-int8/" file
#define MODEL_LOCAL(file) "../models/whisper-tiny/encoder-layer-0-mlp-int8/" file

static int read_exact_file(const char *primary, const char *fallback, void *buffer, u32 size) {
    void *invalid_handle = (void *)(usize)-1;
    void *handle = CreateFileA(primary, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
    u32 total = 0U;
    u8 extra;
    u32 count;
    if (handle == invalid_handle) handle = CreateFileA(fallback, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
    if (handle == invalid_handle) return 0;
    while (total != size) {
        count = 0U;
        if (!ReadFile(handle, (u8 *)buffer + total, size - total, &count, 0) || count == 0U) {
            CloseHandle(handle);
            return -1;
        }
        total += count;
    }
    count = 0U;
    if (!ReadFile(handle, &extra, 1U, &count, 0) || count != 0U) {
        CloseHandle(handle);
        return -1;
    }
    CloseHandle(handle);
    return 1;
}

static int read_handle_exact(void *handle, void *buffer, u64 size) {
    u8 *bytes = buffer;
    while (size != 0U) {
        u32 chunk = size > 0x40000000ULL ? 0x40000000U : (u32)size;
        u32 count = 0U;
        if (!ReadFile(handle, bytes, chunk, &count, 0) || count != chunk) return 0;
        bytes += count;
        size -= count;
    }
    return 1;
}

static int write_handle_exact(void *handle, const void *buffer, u64 size) {
    const u8 *bytes = buffer;
    while (size != 0U) {
        u32 chunk = size > 0x40000000ULL ? 0x40000000U : (u32)size;
        u32 count = 0U;
        if (!WriteFile(handle, bytes, chunk, &count, 0) || count != chunk) return 0;
        bytes += count;
        size -= count;
    }
    return 1;
}

static u32 cache_read_u32(const u8 *bytes) {
    return (u32)bytes[0] | ((u32)bytes[1] << 8U) |
        ((u32)bytes[2] << 16U) | ((u32)bytes[3] << 24U);
}

static u64 cache_read_u64(const u8 *bytes) {
    return (u64)cache_read_u32(bytes) | ((u64)cache_read_u32(bytes + 4U) << 32U);
}

static void cache_write_u32(u8 *bytes, u32 value) {
    bytes[0] = (u8)value;
    bytes[1] = (u8)(value >> 8U);
    bytes[2] = (u8)(value >> 16U);
    bytes[3] = (u8)(value >> 24U);
}

static void cache_write_u64(u8 *bytes, u64 value) {
    cache_write_u32(bytes, (u32)value);
    cache_write_u32(bytes + 4U, (u32)(value >> 32U));
}

static void encode_model_context_metadata(
    u8 output[MODEL_CONTEXT_CACHE_METADATA_SIZE],
    const ModelContextCacheMetadata *metadata
) {
    u32 index;
    cache_write_u64(output, metadata->binary_size);
    cache_write_u32(output + 8U, metadata->encoder.model_id);
    cache_write_u32(output + 12U, metadata->decoder.model_id);
    cache_write_u32(output + 16U, metadata->decoder.output_count);
    cache_write_u32(output + 20U, metadata->encoder.frontend_input_ids[0]);
    cache_write_u32(output + 24U, metadata->encoder.frontend_output_ids[0]);
    cache_write_u32(output + 28U, metadata->encoder.frontend_input_ids[1]);
    cache_write_u32(output + 32U, metadata->encoder.frontend_output_ids[1]);
    cache_write_u32(output + 36U, metadata->encoder.encoder_input_id);
    cache_write_u32(output + 40U, metadata->encoder.encoder_output_id);
    cache_write_u32(output + 44U, metadata->decoder.input_id);
    for (index = 0U; index < WHISPER_DECODER_QNN_MAX_OUTPUTS; ++index) {
        cache_write_u32(output + 48U + index * 4U, metadata->decoder.output_ids[index]);
    }
    cache_write_u32(
        output + 48U + WHISPER_DECODER_QNN_MAX_OUTPUTS * 4U,
        metadata->decoder.mlp_layer_count
    );
    for (index = 0U; index < WHISPER_DECODER_QNN_MAX_LAYERS; ++index) {
        u32 base = 52U + WHISPER_DECODER_QNN_MAX_OUTPUTS * 4U;
        cache_write_u32(
            output + base + index * 4U,
            metadata->decoder.mlp_input_ids[index]
        );
        cache_write_u32(
            output + base + (WHISPER_DECODER_QNN_MAX_LAYERS + index) * 4U,
            metadata->decoder.mlp_output_ids[index]
        );
    }
    cache_write_u32(output + 436U, metadata->decoder.cross_layer_count);
    for (index = 0U; index < WHISPER_DECODER_QNN_MAX_LAYERS; ++index) {
        u32 base = 440U;
        cache_write_u32(output + base + index * 4U,
            metadata->decoder.cross_input_ids[index]);
        cache_write_u32(output + base +
            (WHISPER_DECODER_QNN_MAX_LAYERS + index) * 4U,
            metadata->decoder.cross_key_ids[index]);
        cache_write_u32(output + base +
            (WHISPER_DECODER_QNN_MAX_LAYERS * 2U + index) * 4U,
            metadata->decoder.cross_value_ids[index]);
        cache_write_u32(output + base +
            (WHISPER_DECODER_QNN_MAX_LAYERS * 3U + index) * 4U,
            metadata->decoder.cross_output_ids[index]);
    }
}

static void decode_model_context_metadata(
    const u8 input[MODEL_CONTEXT_CACHE_METADATA_SIZE],
    ModelContextCacheMetadata *metadata
) {
    u32 index;
    metadata->binary_size = cache_read_u64(input);
    metadata->encoder.model_id = cache_read_u32(input + 8U);
    metadata->decoder.model_id = cache_read_u32(input + 12U);
    metadata->decoder.output_count = cache_read_u32(input + 16U);
    metadata->encoder.frontend_input_ids[0] = cache_read_u32(input + 20U);
    metadata->encoder.frontend_output_ids[0] = cache_read_u32(input + 24U);
    metadata->encoder.frontend_input_ids[1] = cache_read_u32(input + 28U);
    metadata->encoder.frontend_output_ids[1] = cache_read_u32(input + 32U);
    metadata->encoder.encoder_input_id = cache_read_u32(input + 36U);
    metadata->encoder.encoder_output_id = cache_read_u32(input + 40U);
    metadata->decoder.input_id = cache_read_u32(input + 44U);
    for (index = 0U; index < WHISPER_DECODER_QNN_MAX_OUTPUTS; ++index) {
        metadata->decoder.output_ids[index] = cache_read_u32(input + 48U + index * 4U);
    }
    metadata->decoder.mlp_layer_count = cache_read_u32(
        input + 48U + WHISPER_DECODER_QNN_MAX_OUTPUTS * 4U
    );
    for (index = 0U; index < WHISPER_DECODER_QNN_MAX_LAYERS; ++index) {
        u32 base = 52U + WHISPER_DECODER_QNN_MAX_OUTPUTS * 4U;
        metadata->decoder.mlp_input_ids[index] = cache_read_u32(
            input + base + index * 4U
        );
        metadata->decoder.mlp_output_ids[index] = cache_read_u32(
            input + base + (WHISPER_DECODER_QNN_MAX_LAYERS + index) * 4U
        );
    }
    metadata->decoder.cross_layer_count = cache_read_u32(input + 436U);
    for (index = 0U; index < WHISPER_DECODER_QNN_MAX_LAYERS; ++index) {
        u32 base = 440U;
        metadata->decoder.cross_input_ids[index] = cache_read_u32(
            input + base + index * 4U);
        metadata->decoder.cross_key_ids[index] = cache_read_u32(
            input + base + (WHISPER_DECODER_QNN_MAX_LAYERS + index) * 4U);
        metadata->decoder.cross_value_ids[index] = cache_read_u32(
            input + base + (WHISPER_DECODER_QNN_MAX_LAYERS * 2U + index) * 4U);
        metadata->decoder.cross_output_ids[index] = cache_read_u32(
            input + base + (WHISPER_DECODER_QNN_MAX_LAYERS * 3U + index) * 4U);
    }
}

static int make_model_layer_path(
    char *path,
    u32 capacity,
    const char *prefix,
    u32 layer,
    const char *file
) {
    u32 used = 0U;
    u32 index = 0U;
    if (layer >= 10U) return 0;
    while (prefix[index] != '\0') {
        if (used + 1U >= capacity) return 0;
        path[used++] = prefix[index++];
    }
    if (used + 1U >= capacity) return 0;
    path[used++] = (char)('0' + layer);
    index = 0U;
    while (file[index] != '\0') {
        if (used + 1U >= capacity) return 0;
        path[used++] = file[index++];
    }
    path[used] = '\0';
    return 1;
}

static int read_model_layer_file(u32 layer, const char *file, void *buffer, u32 size) {
    char primary[192];
    char fallback[128];
    if (!make_model_layer_path(
            primary,
            sizeof(primary),
            "experimental/snapdragon/models/whisper-tiny/encoder-layer-",
            layer,
            "-mlp-int8/")) return -1;
    if (!make_model_layer_path(
            fallback,
            sizeof(fallback),
            "../models/whisper-tiny/encoder-layer-",
            layer,
            "-mlp-int8/")) return -1;
    {
        u32 primary_length = (u32)text_length(primary);
        u32 fallback_length = (u32)text_length(fallback);
        u32 index = 0U;
        while (file[index] != '\0') {
            if (primary_length + index + 1U >= sizeof(primary) ||
                fallback_length + index + 1U >= sizeof(fallback)) return -1;
            primary[primary_length + index] = file[index];
            fallback[fallback_length + index] = file[index];
            ++index;
        }
        primary[primary_length + index] = '\0';
        fallback[fallback_length + index] = '\0';
    }
    return read_exact_file(primary, fallback, buffer, size);
}

static int read_model_fp16_layer_file(u32 layer, const char *file, void *buffer, u32 size) {
    char primary[192];
    char fallback[128];
    if (!make_model_layer_path(
            primary,
            sizeof(primary),
            "experimental/snapdragon/models/whisper-tiny/encoder-layer-",
            layer,
            "-fp16/")) return -1;
    if (!make_model_layer_path(
            fallback,
            sizeof(fallback),
            "../models/whisper-tiny/encoder-layer-",
            layer,
            "-fp16/")) return -1;
    {
        u32 primary_length = (u32)text_length(primary);
        u32 fallback_length = (u32)text_length(fallback);
        u32 index = 0U;
        while (file[index] != '\0') {
            if (primary_length + index + 1U >= sizeof(primary) ||
                fallback_length + index + 1U >= sizeof(fallback)) return -1;
            primary[primary_length + index] = file[index];
            fallback[fallback_length + index] = file[index];
            ++index;
        }
        primary[primary_length + index] = '\0';
        fallback[fallback_length + index] = '\0';
    }
    return read_exact_file(primary, fallback, buffer, size);
}

#define READ_FRONTEND_FILE(file, buffer) read_exact_file( \
    "experimental/snapdragon/models/whisper-tiny/frontend-fp16/" file, \
    "../models/whisper-tiny/frontend-fp16/" file, buffer, sizeof(buffer))

static void write_buffer_fingerprint(const char *label, const void *buffer, u32 size) {
    const u8 *bytes = buffer;
    u64 hash = 1469598103934665603ULL;
    u32 index;
    for (index = 0U; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    write_text(label);
    write_hex64(hash);
    write_text("\n");
}

static u32 run_whisper_log_mel_frontend(
    u64 frequency,
    const char *primary_wav,
    const char *fallback_wav,
    u64 start_sample,
    u64 *total_samples,
    int validate_fixture,
    const char *label
) {
    static int constants_loaded;
    static int fixture_loaded;
    long long start_counter;
    long long end_counter;
    double maximum_delta = 0.0;
    double absolute_delta_sum = 0.0;
    u32 over_tolerance = 0U;
    u32 index;
    int loaded;

    loaded = 1;
    if (!constants_loaded) {
        loaded = READ_FRONTEND_FILE("hann-window-f64.bin", frontend_hann_window);
        if (loaded == 1) loaded = READ_FRONTEND_FILE("dft-roots-f64.bin", frontend_dft_roots);
        if (loaded == 1) loaded = READ_FRONTEND_FILE("mel-filters-f64.bin", frontend_mel_filters);
        if (loaded == 1) constants_loaded = 1;
    }
    if (loaded == 1 && validate_fixture && !fixture_loaded) {
        loaded = READ_FRONTEND_FILE("fixture-log-mel-f32.bin", frontend_log_mel_expected);
        if (loaded == 1) fixture_loaded = 1;
    }
    if (loaded == 0) {
        write_text("Whisper log-mel frontend: skipped (run --frontend-only exporter first)\n");
        return 0U;
    }
    if (loaded < 0) return 93U;
    QueryPerformanceCounter(&start_counter);
    loaded = whisper_frontend_log_mel_window(
        primary_wav,
        fallback_wav,
        start_sample,
        total_samples,
        frontend_hann_window,
        frontend_dft_roots,
        frontend_mel_filters,
        frontend_log_mel
    );
    QueryPerformanceCounter(&end_counter);
    if (loaded != 1) return 94U;
    write_text(label);
    write_text("\n");
    write_duration_us("  WAV to log-mel time", (u64)(end_counter - start_counter), frequency);
    if (!validate_fixture) {
        write_buffer_fingerprint(
            "  log-mel FNV-1a: ", frontend_log_mel, sizeof(frontend_log_mel)
        );
        return 0U;
    }
    for (index = 0U; index < WHISPER_MEL_BINS * WHISPER_FRAME_COUNT; ++index) {
        double delta = (double)frontend_log_mel[index] - frontend_log_mel_expected[index];
        if (delta < 0.0) delta = -delta;
        if (delta > maximum_delta) maximum_delta = delta;
        if (delta > 0.0001) over_tolerance += 1U;
        absolute_delta_sum += delta;
    }
    write_text("  maximum delta millionths: ");
    write_u64((u64)(maximum_delta * 1000000.0));
    write_text(", values over 0.0001: ");
    write_u32(over_tolerance);
    write_text("/");
    write_u32(WHISPER_MEL_BINS * WHISPER_FRAME_COUNT);
    write_text("\n  mean delta billionths: ");
    write_u64((u64)(absolute_delta_sum * 1000000000.0 /
        (WHISPER_MEL_BINS * WHISPER_FRAME_COUNT)));
    write_text("\n");
    return maximum_delta <= 0.001 && over_tolerance <= 100U ? 0U : 95U;
}

static int load_whisper_frontend_model(void) {
    int loaded = READ_FRONTEND_FILE("conv1-weight-fp16.bin", frontend_conv1_weight);
    if (loaded == 1) loaded = READ_FRONTEND_FILE("conv1-bias-fp16.bin", frontend_conv1_bias);
    if (loaded == 1) loaded = READ_FRONTEND_FILE("fixture-conv1-input-fp16.bin", frontend_conv1_input_expected);
    if (loaded == 1) loaded = READ_FRONTEND_FILE("fixture-conv1-output-fp16.bin", frontend_conv1_expected);
    if (loaded == 1) loaded = READ_FRONTEND_FILE("conv2-weight-fp16.bin", frontend_conv2_weight);
    if (loaded == 1) loaded = READ_FRONTEND_FILE("conv2-bias-fp16.bin", frontend_conv2_bias);
    if (loaded == 1) loaded = READ_FRONTEND_FILE("embed-positions-fp16.bin", frontend_positions);
    if (loaded == 1) loaded = READ_FRONTEND_FILE("fixture-output-fp16.bin", frontend_expected);
    return loaded;
}

static u32 compare_frontend_fp16(const char *label, const u16 *actual, const u16 *expected, u32 count) {
    u32 index;
    u32 over_one_hundredth = 0U;
    double squared_delta = 0.0;
    double squared_reference = 0.0;
    float maximum_delta = 0.0f;
    for (index = 0U; index < count; ++index) {
        float actual_value = whisper_frontend_half_to_float(actual[index]);
        float expected_value = whisper_frontend_half_to_float(expected[index]);
        float delta = actual_value - expected_value;
        float absolute_delta = delta < 0.0f ? -delta : delta;
        if (absolute_delta > maximum_delta) maximum_delta = absolute_delta;
        if (absolute_delta > 0.01f) ++over_one_hundredth;
        squared_delta += (double)delta * delta;
        squared_reference += (double)expected_value * expected_value;
    }
    write_text(label);
    write_text(": maximum delta millionths ");
    write_u64((u64)(maximum_delta * 1000000.0f));
    write_text(", values over 0.01 ");
    write_u32(over_one_hundredth);
    write_text("/");
    write_u32(count);
    write_text(", relative L2 squared ppm ");
    write_u64((u64)(squared_delta * 1000000.0 / squared_reference));
    write_text("\n");
    return squared_delta * 1000.0 <= squared_reference ? 0U : 106U;
}

static int load_model_fp16_encoder_block_artifacts(u32 layer) {
    int result = read_model_fp16_layer_file(
        layer, "fixture-input-fp16.bin", model_fp16_block_input,
        sizeof(model_fp16_block_input));
    if (result <= 0) return result;
#define LOAD_FP16_BLOCK_FILE(file, buffer) \
    if (read_model_fp16_layer_file(layer, file, buffer, sizeof(buffer)) != 1) return -1
    LOAD_FP16_BLOCK_FILE("fixture-output-fp16.bin", model_fp16_block_expected);
    LOAD_FP16_BLOCK_FILE("q_proj-weight-fp16.bin", model_fp16_attention_weights[layer][0]);
    LOAD_FP16_BLOCK_FILE("k_proj-weight-fp16.bin", model_fp16_attention_weights[layer][1]);
    LOAD_FP16_BLOCK_FILE("v_proj-weight-fp16.bin", model_fp16_attention_weights[layer][2]);
    LOAD_FP16_BLOCK_FILE("q_proj-bias-fp16.bin", model_fp16_attention_bias[layer][0]);
    LOAD_FP16_BLOCK_FILE("k_proj-bias-fp16.bin", model_fp16_attention_bias[layer][1]);
    LOAD_FP16_BLOCK_FILE("v_proj-bias-fp16.bin", model_fp16_attention_bias[layer][2]);
    LOAD_FP16_BLOCK_FILE("out_proj-weight-fp16.bin", model_fp16_out_proj_weights[layer]);
    LOAD_FP16_BLOCK_FILE("out_proj-bias-fp16.bin", model_fp16_out_proj_bias[layer]);
    LOAD_FP16_BLOCK_FILE("fc1-weight-fp16.bin", model_fp16_fc1_weights[layer]);
    LOAD_FP16_BLOCK_FILE("fc1-bias-fp16.bin", model_fp16_fc1_bias[layer]);
    LOAD_FP16_BLOCK_FILE("fc2-weight-fp16.bin", model_fp16_fc2_weights[layer]);
    LOAD_FP16_BLOCK_FILE("fc2-bias-fp16.bin", model_fp16_fc2_bias[layer]);
    LOAD_FP16_BLOCK_FILE("self_attn_layer_norm-scale-fp16.bin", model_fp16_layer_norm_scale[layer]);
    LOAD_FP16_BLOCK_FILE("self_attn_layer_norm-bias-fp16.bin", model_fp16_layer_norm_bias[layer]);
    LOAD_FP16_BLOCK_FILE("final_layer_norm-scale-fp16.bin", model_fp16_final_layer_norm_scale[layer]);
    LOAD_FP16_BLOCK_FILE("final_layer_norm-bias-fp16.bin", model_fp16_final_layer_norm_bias[layer]);
#undef LOAD_FP16_BLOCK_FILE
    return 1;
}

static int load_model_mlp_artifacts(u32 layer) {
    u32 index;
    int result = read_model_layer_file(
        layer, "fixture-input-uint8.bin", model_input, sizeof(model_input));
    if (result <= 0) return result;
#define LOAD_MODEL_FILE(file, buffer) \
    if (read_model_layer_file(layer, file, buffer, sizeof(buffer)) != 1) return -1
    LOAD_MODEL_FILE("fixture-output-uint8.bin", model_expected);
    LOAD_MODEL_FILE("fc1-weight-int8.bin", model_fc1_weights);
    LOAD_MODEL_FILE("fc2-weight-int8.bin", model_fc2_weights);
    LOAD_MODEL_FILE("fc1-scale-f32.bin", model_fc1_weight_scales);
    LOAD_MODEL_FILE("fc2-scale-f32.bin", model_fc2_weight_scales);
    LOAD_MODEL_FILE("fc1-bias-int32.bin", model_fc1_bias);
    LOAD_MODEL_FILE("fc2-bias-int32.bin", model_fc2_bias);
    LOAD_MODEL_FILE("activation-encodings-f32-i32.bin", model_activation_quant);
#undef LOAD_MODEL_FILE
    for (index = 0U; index < TINY_MLP_WIDTH; ++index) {
        model_fc1_weight_quant[index].scale = model_fc1_weight_scales[index];
        model_fc1_weight_quant[index].offset = 0;
        model_fc1_bias_quant[index].scale = model_activation_quant[0].scale * model_fc1_weight_scales[index];
        model_fc1_bias_quant[index].offset = 0;
    }
    for (index = 0U; index < TINY_WIDTH; ++index) {
        model_fc2_weight_quant[index].scale = model_fc2_weight_scales[index];
        model_fc2_weight_quant[index].offset = 0;
        model_fc2_bias_quant[index].scale = model_activation_quant[2].scale * model_fc2_weight_scales[index];
        model_fc2_bias_quant[index].offset = 0;
    }
    return 1;
}

static int load_model_attention_artifacts(u32 layer) {
    u32 projection;
    u32 channel;
    int result = read_model_layer_file(
        layer, "fixture-attention-input-uint8.bin", model_attention_input,
        sizeof(model_attention_input));
    if (result <= 0) return result;
#define LOAD_ATTENTION_FILE(file, buffer) \
    if (read_model_layer_file(layer, file, buffer, sizeof(buffer)) != 1) return -1
    LOAD_ATTENTION_FILE("fixture-q_proj-output-uint8.bin", model_attention_expected[0]);
    LOAD_ATTENTION_FILE("fixture-k_proj-output-uint8.bin", model_attention_expected[1]);
    LOAD_ATTENTION_FILE("fixture-v_proj-output-uint8.bin", model_attention_expected[2]);
    LOAD_ATTENTION_FILE("q_proj-weight-int8.bin", model_attention_weights[0]);
    LOAD_ATTENTION_FILE("k_proj-weight-int8.bin", model_attention_weights[1]);
    LOAD_ATTENTION_FILE("v_proj-weight-int8.bin", model_attention_weights[2]);
    LOAD_ATTENTION_FILE("q_proj-scale-f32.bin", model_attention_weight_scales[0]);
    LOAD_ATTENTION_FILE("k_proj-scale-f32.bin", model_attention_weight_scales[1]);
    LOAD_ATTENTION_FILE("v_proj-scale-f32.bin", model_attention_weight_scales[2]);
    LOAD_ATTENTION_FILE("q_proj-bias-int32.bin", model_attention_bias[0]);
    LOAD_ATTENTION_FILE("k_proj-bias-int32.bin", model_attention_bias[1]);
    LOAD_ATTENTION_FILE("v_proj-bias-int32.bin", model_attention_bias[2]);
    LOAD_ATTENTION_FILE("attention-projection-encodings-f32-i32.bin", model_attention_activation_quant);
#undef LOAD_ATTENTION_FILE
    for (projection = 0U; projection < ATTENTION_PROJECTION_COUNT; ++projection) {
        for (channel = 0U; channel < TINY_WIDTH; ++channel) {
            model_attention_weight_quant[projection][channel].scale =
                model_attention_weight_scales[projection][channel];
            model_attention_weight_quant[projection][channel].offset = 0;
            model_attention_bias_quant[projection][channel].scale =
                model_attention_activation_quant[0].scale *
                model_attention_weight_scales[projection][channel];
            model_attention_bias_quant[projection][channel].offset = 0;
        }
    }
    return 1;
}

static int load_model_attention_core_artifacts(void) {
    int result = read_exact_file(
        MODEL_PRIMARY("fixture-attention-core-q-uint8.bin"),
        MODEL_LOCAL("fixture-attention-core-q-uint8.bin"),
        model_attention_core_input[0],
        sizeof(model_attention_core_input[0])
    );
    if (result <= 0) return result;
#define LOAD_ATTENTION_CORE_FILE(file, buffer) \
    if (read_exact_file(MODEL_PRIMARY(file), MODEL_LOCAL(file), buffer, sizeof(buffer)) != 1) return -1
    LOAD_ATTENTION_CORE_FILE("fixture-attention-core-k-uint8.bin", model_attention_core_input[1]);
    LOAD_ATTENTION_CORE_FILE("fixture-attention-core-v-uint8.bin", model_attention_core_input[2]);
    LOAD_ATTENTION_CORE_FILE("fixture-attention-core-output-uint8.bin", model_attention_core_expected);
    LOAD_ATTENTION_CORE_FILE("attention-core-encodings-f32-i32.bin", model_attention_core_quant);
#undef LOAD_ATTENTION_CORE_FILE
    return 1;
}

static int load_model_encoder_block_artifacts(u32 layer) {
    u32 channel;
    int result = read_model_layer_file(
        layer, "fixture-encoder-block-input-uint8.bin", model_block_input,
        sizeof(model_block_input));
    if (result <= 0) return result;
#define LOAD_BLOCK_FILE(file, buffer) \
    if (read_model_layer_file(layer, file, buffer, sizeof(buffer)) != 1) return -1
    LOAD_BLOCK_FILE("fixture-encoder-block-attention-input-uint8.bin", model_block_attention_input);
    LOAD_BLOCK_FILE("fixture-encoder-block-output-uint8.bin", model_block_output);
    LOAD_BLOCK_FILE("encoder-block-encodings-f32-i32.bin", model_block_quant);
    LOAD_BLOCK_FILE("self_attn_layer_norm-scale-uint8.bin", model_layer_norm_scale);
    LOAD_BLOCK_FILE("self_attn_layer_norm-bias-int32.bin", model_layer_norm_bias);
    LOAD_BLOCK_FILE("out_proj-weight-int8.bin", model_out_proj_weights);
    LOAD_BLOCK_FILE("out_proj-scale-f32.bin", model_out_proj_weight_scales);
    LOAD_BLOCK_FILE("out_proj-bias-int32.bin", model_out_proj_bias);
    LOAD_BLOCK_FILE("final_layer_norm-scale-uint8.bin", model_final_layer_norm_scale);
    LOAD_BLOCK_FILE("final_layer_norm-bias-int32.bin", model_final_layer_norm_bias);
#undef LOAD_BLOCK_FILE
        if (read_model_layer_file(
            layer, "self_attn_layer_norm-scale-encoding-f32-i32.bin",
            &model_layer_norm_scale_quant,
            sizeof(model_layer_norm_scale_quant)) != 1) return -1;
        if (read_model_layer_file(
            layer, "final_layer_norm-scale-encoding-f32-i32.bin",
            &model_final_layer_norm_scale_quant,
            sizeof(model_final_layer_norm_scale_quant)) != 1) return -1;
    for (channel = 0U; channel < TINY_WIDTH; ++channel) {
        model_out_proj_weight_quant[channel].scale = model_out_proj_weight_scales[channel];
        model_out_proj_weight_quant[channel].offset = 0;
        model_out_proj_bias_quant[channel].scale =
            model_block_quant[7].scale * model_out_proj_weight_scales[channel];
        model_out_proj_bias_quant[channel].offset = 0;
    }
    return 1;
}

static int validate_output(const u8 *actual, const u8 *expected, u32 count) {
    u32 index;
    for (index = 0U; index < count; ++index) {
        if (actual[index] != expected[index]) {
            write_text("  output mismatch at ");
            write_u32(index);
            write_text(": expected ");
            write_u32(expected[index]);
            write_text(", got ");
            write_u32(actual[index]);
            write_text("\n");
            return 0;
        }
    }
    return 1;
}

static void reference_add(const u8 *left, const u8 *right, u8 *output, u32 count) {
    u32 index;
    for (index = 0U; index < count; ++index) output[index] = (u8)(left[index] + right[index]);
}

static void sort_u64(u64 *values, u32 count) {
    u32 index;
    for (index = 1U; index < count; ++index) {
        u64 value = values[index];
        u32 position = index;
        while (position != 0U && values[position - 1U] > value) {
            values[position] = values[position - 1U];
            --position;
        }
        values[position] = value;
    }
}

static void initialize_tiny_projection(u32 sequence_length, u32 inner_size, u32 output_size) {
    u32 input_stride = inner_size > TINY_WIDTH ? 8U : 4U;
    u32 batch;
    u32 row;
    u32 column;
    for (batch = 0U; batch < sequence_length; ++batch) {
        for (row = 0U; row < inner_size; ++row) {
            tiny_input[batch * inner_size + row] =
                (u8)((row + batch) % input_stride == 0U);
        }
    }
    for (row = 0U; row < inner_size; ++row) {
        for (column = 0U; column < output_size; ++column) {
            tiny_weights[row * output_size + column] =
                (u8)(((row * 17U + column * 29U + (row ^ column)) >> 2U) & 1U);
        }
    }
}

static void reference_tiny_projection(u32 sequence_length, u32 inner_size, u32 output_size) {
    u32 batch;
    for (batch = 0U; batch < sequence_length; ++batch) {
        u32 column;
        for (column = 0U; column < output_size; ++column) {
            u32 row;
            u32 sum = 0U;
            for (row = 0U; row < inner_size; ++row) {
                sum += (u32)tiny_input[batch * inner_size + row] *
                       (u32)tiny_weights[row * output_size + column];
            }
            tiny_expected[batch * output_size + column] = (u8)sum;
        }
    }
}

static void write_tiny_shape(u32 sequence_length, u32 inner_size, u32 output_size) {
    write_text("QNN Whisper Tiny UINT8 MatMul [");
    write_u32(sequence_length);
    write_text(",");
    write_u32(inner_size);
    write_text("] x [");
    write_u32(inner_size);
    write_text(",");
    write_u32(output_size);
    write_text("]\n");
}

static void write_gmac_per_second(const char *name, u64 macs, u64 ticks, u64 frequency) {
    u64 thousandths = macs * frequency / (ticks * 1000000U);
    write_text(name);
    write_text(": ");
    write_u64(thousandths / 1000U);
    write_text(".");
    write_fraction3(thousandths % 1000U);
    write_text(" GMAC/s\n");
}

static int validate_model_output(void) {
    u32 count = TINY_SEQUENCE_LENGTH * TINY_WIDTH;
    u32 maximum_delta = 0U;
    u32 over_two = 0U;
    u32 index;
    for (index = 0U; index < count; ++index) {
        u32 actual = model_output[index];
        u32 expected = model_expected[index];
        u32 delta = actual > expected ? actual - expected : expected - actual;
        if (delta > maximum_delta) maximum_delta = delta;
        if (delta > 2U) over_two += 1U;
    }
    write_text("  output maximum byte delta: ");
    write_u32(maximum_delta);
    write_text(", values over tolerance: ");
    write_u32(over_two);
    write_text("/");
    write_u32(count);
    write_text("\n");
    return over_two == 0U;
}

static int validate_attention_outputs(void) {
    static const char *names[ATTENTION_PROJECTION_COUNT] = {"Q", "K", "V"};
    u32 count = TINY_SEQUENCE_LENGTH * TINY_WIDTH;
    u32 valid = 1U;
    u32 projection;
    for (projection = 0U; projection < ATTENTION_PROJECTION_COUNT; ++projection) {
        u32 maximum_delta = 0U;
        u32 over_two = 0U;
        u32 index;
        for (index = 0U; index < count; ++index) {
            u32 actual = model_attention_output[projection][index];
            u32 expected = model_attention_expected[projection][index];
            u32 delta = actual > expected ? actual - expected : expected - actual;
            if (delta > maximum_delta) maximum_delta = delta;
            if (delta > 2U) over_two += 1U;
        }
        write_text("  ");
        write_text(names[projection]);
        write_text(" maximum byte delta: ");
        write_u32(maximum_delta);
        write_text(", values over tolerance: ");
        write_u32(over_two);
        write_text("/");
        write_u32(count);
        write_text("\n");
        if (over_two != 0U) valid = 0U;
    }
    return valid;
}

static int validate_attention_core_output(void) {
    u32 count = TINY_SEQUENCE_LENGTH * TINY_WIDTH;
    u32 maximum_delta = 0U;
    u32 over_two = 0U;
    u32 index;
    for (index = 0U; index < count; ++index) {
        u32 actual = model_attention_core_output[index];
        u32 expected = model_attention_core_expected[index];
        u32 delta = actual > expected ? actual - expected : expected - actual;
        if (delta > maximum_delta) maximum_delta = delta;
        if (delta > 2U) over_two += 1U;
    }
    write_text("  output maximum byte delta: ");
    write_u32(maximum_delta);
    write_text(", values over tolerance: ");
    write_u32(over_two);
    write_text("/");
    write_u32(count);
    write_text("\n");
    return maximum_delta <= 16U && over_two <= count / 1000U;
}

static int validate_layer_norm_output(void) {
    u32 count = TINY_SEQUENCE_LENGTH * TINY_WIDTH;
    u32 maximum_delta = 0U;
    u32 over_two = 0U;
    u32 index;
    for (index = 0U; index < count; ++index) {
        u32 actual = model_attention_core_output[index];
        u32 expected = model_block_attention_input[index];
        u32 delta = actual > expected ? actual - expected : expected - actual;
        if (delta > maximum_delta) maximum_delta = delta;
        if (delta > 2U) over_two += 1U;
    }
    write_text("  output maximum byte delta: ");
    write_u32(maximum_delta);
    write_text(", values over tolerance: ");
    write_u32(over_two);
    write_text("/");
    write_u32(count);
    write_text("\n");
    return over_two == 0U;
}

static int validate_encoder_output(
    const u8 *actual_output,
    const u8 *expected_output,
    u32 maximum_delta_limit,
    u32 over_four_limit
) {
    u32 count = TINY_SEQUENCE_LENGTH * TINY_WIDTH;
    u32 maximum_delta = 0U;
    u32 over_two = 0U;
    u32 over_four = 0U;
    u32 over_eight = 0U;
    u32 over_sixteen = 0U;
    u64 absolute_delta_sum = 0U;
    u64 squared_delta_sum = 0U;
    u64 squared_reference_sum = 0U;
    u32 index;
    for (index = 0U; index < count; ++index) {
        u32 actual = actual_output[index];
        u32 expected = expected_output[index];
        u32 delta = actual > expected ? actual - expected : expected - actual;
        i32 centered_reference = (i32)expected + model_block_quant[14].offset;
        if (delta > maximum_delta) maximum_delta = delta;
        if (delta > 2U) over_two += 1U;
        if (delta > 4U) over_four += 1U;
        if (delta > 8U) over_eight += 1U;
        if (delta > 16U) over_sixteen += 1U;
        absolute_delta_sum += delta;
        squared_delta_sum += (u64)delta * delta;
        squared_reference_sum += (u64)(centered_reference * centered_reference);
    }
    write_text("  output maximum byte delta: ");
    write_u32(maximum_delta);
    write_text(", values over 2/4/8/16: ");
    write_u32(over_two);
    write_text("/");
    write_u32(over_four);
    write_text("/");
    write_u32(over_eight);
    write_text("/");
    write_u32(over_sixteen);
    write_text(" of ");
    write_u32(count);
    write_text("\n  mean absolute byte delta: ");
    write_u64(absolute_delta_sum * 1000U / count / 1000U);
    write_text(".");
    write_fraction3(absolute_delta_sum * 1000U / count % 1000U);
    write_text("\n  relative L2 squared ppm: ");
    write_u64(squared_delta_sum * 1000000U / squared_reference_sum);
    write_text("\n");
    return maximum_delta <= maximum_delta_limit && over_four <= over_four_limit;
}

static float half_to_float(u16 value) {
    union { u32 bits; float value; } converted;
    u32 sign = (u32)(value & 0x8000U) << 16U;
    u32 exponent = (value >> 10U) & 31U;
    u32 fraction = value & 1023U;
    if (exponent == 0U) {
        if (fraction == 0U) {
            converted.bits = sign;
            return converted.value;
        }
        exponent = 113U;
        while ((fraction & 1024U) == 0U) {
            fraction <<= 1U;
            --exponent;
        }
        fraction &= 1023U;
    } else if (exponent == 31U) {
        converted.bits = sign | 0x7f800000U | (fraction << 13U);
        return converted.value;
    } else {
        exponent += 112U;
    }
    converted.bits = sign | (exponent << 23U) | (fraction << 13U);
    return converted.value;
}

static int validate_fp16_encoder_output(const u16 *actual, const u16 *expected) {
    u32 count = TINY_SEQUENCE_LENGTH * TINY_WIDTH;
    double squared_delta_sum = 0.0;
    double squared_reference_sum = 0.0;
    float maximum_absolute_delta = 0.0f;
    u32 over_one_hundredth = 0U;
    u32 index;
    for (index = 0U; index < count; ++index) {
        float actual_value = half_to_float(actual[index]);
        float expected_value = half_to_float(expected[index]);
        float delta = actual_value - expected_value;
        float absolute_delta = delta < 0.0f ? -delta : delta;
        if (absolute_delta > maximum_absolute_delta) maximum_absolute_delta = absolute_delta;
        if (absolute_delta > 0.01f) ++over_one_hundredth;
        squared_delta_sum += (double)delta * delta;
        squared_reference_sum += (double)expected_value * expected_value;
    }
    write_text("  FP16 maximum absolute delta millionths: ");
    write_u64((u64)(maximum_absolute_delta * 1000000.0f));
    write_text(", values over 0.01: ");
    write_u32(over_one_hundredth);
    write_text("/");
    write_u32(count);
    write_text("\n  FP16 relative L2 squared ppm: ");
    write_u64((u64)(squared_delta_sum * 1000000.0 / squared_reference_sum));
    write_text("\n");
    return squared_delta_sum * 1000.0 <= squared_reference_sum;
}

static u64 add_graph_node(
    const QnnInterfaceV2 *api,
    QnnGraphHandle graph,
    const char *name,
    const char *type,
    QnnTensor *input0,
    QnnTensor *input1,
    QnnTensor *input2,
    u32 input_count,
    QnnTensor *output,
    QnnParam *parameters,
    u32 parameter_count
) {
    QnnTensor inputs[3];
    QnnTensor outputs[1];
    QnnOpConfig operation = {0};
    copy_tensor(&inputs[0], input0);
    if (input_count > 1U) copy_tensor(&inputs[1], input1);
    if (input_count > 2U) copy_tensor(&inputs[2], input2);
    copy_tensor(&outputs[0], output);
    operation.version = QNN_OPCONFIG_VERSION_1;
    if (model_monolithic_name_layer >= 0 && model_monolithic_node_index < 22U) {
        char *unique_name = model_monolithic_node_names[model_monolithic_name_layer]
            [model_monolithic_node_index++];
        make_layer_name(unique_name, 64U, (u32)model_monolithic_name_layer, name);
        operation.data.v1.name = unique_name;
    } else {
        operation.data.v1.name = name;
    }
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = type;
    operation.data.v1.parameter_count = parameter_count;
    operation.data.v1.parameters = parameters;
    operation.data.v1.input_count = input_count;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = outputs;
    return api->graph_add_node(graph, operation);
}

static u32 __attribute__((unused)) build_whisper_frontend_graphs(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    QnnTensor weight;
    QnnTensor bias;
    QnnTensor fc_output;
    QnnTensor gelu_output;
    QnnTensor position;
    QnnTensor add_output;
    QnnTensor *tensors[6];
    u64 status;
    u32 graph_index;
    u32 tensor_index;
    int loaded = load_whisper_frontend_model();
    if (loaded == 0) {
        write_text("QNN Whisper frontend graphs: skipped (run --frontend-only exporter first)\n");
        return 0U;
    }
    if (loaded < 0) return 97U;

    for (graph_index = 0U; graph_index < 2U; ++graph_index) {
        status = api->graph_create(
            context,
            graph_index == 0U ? "whisper_frontend_conv1" : "whisper_frontend_conv2",
            0,
            &frontend_graphs[graph_index]
        );
        write_call_status("  frontend graphCreate", status);
        if (status != 0U) return 98U;
        frontend_inputs[graph_index] = make_plain_tensor(
            graph_index == 0U ? "frontend_conv1_input" : "frontend_conv2_input",
            QNN_TENSOR_TYPE_APP_WRITE, QNN_DATATYPE_FLOAT_16,
            frontend_input_dimensions[graph_index], 2U
        );
        weight = make_plain_tensor(
            graph_index == 0U ? "frontend_conv1_weight" : "frontend_conv2_weight",
            QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_FLOAT_16,
            frontend_weight_dimensions[graph_index], 2U
        );
        weight.data.v1.memory.client_buffer.data = graph_index == 0U
            ? (void *)frontend_conv1_weight : (void *)frontend_conv2_weight;
        weight.data.v1.memory.client_buffer.data_size = frontend_weight_dimensions[graph_index][0] *
            frontend_weight_dimensions[graph_index][1] * sizeof(u16);
        bias = make_plain_tensor(
            graph_index == 0U ? "frontend_conv1_bias" : "frontend_conv2_bias",
            QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_FLOAT_16, frontend_bias_dimensions, 1U
        );
        bias.data.v1.memory.client_buffer.data = graph_index == 0U
            ? (void *)frontend_conv1_bias : (void *)frontend_conv2_bias;
        bias.data.v1.memory.client_buffer.data_size = sizeof(frontend_conv1_bias);
        fc_output = make_plain_tensor(
            graph_index == 0U ? "frontend_conv1_fc" : "frontend_conv2_fc",
            QNN_TENSOR_TYPE_NATIVE, QNN_DATATYPE_FLOAT_16,
            frontend_output_dimensions[graph_index], 2U
        );
        gelu_output = make_plain_tensor(
            graph_index == 0U ? "frontend_conv1_output" : "frontend_conv2_gelu",
            graph_index == 0U ? QNN_TENSOR_TYPE_APP_READ : QNN_TENSOR_TYPE_NATIVE,
            QNN_DATATYPE_FLOAT_16, frontend_output_dimensions[graph_index], 2U
        );
        tensors[0] = &frontend_inputs[graph_index];
        tensors[1] = &weight;
        tensors[2] = &bias;
        tensors[3] = &fc_output;
        tensors[4] = &gelu_output;
        if (graph_index == 1U) {
            position = make_plain_tensor(
                "frontend_positions", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_FLOAT_16,
                frontend_output_dimensions[graph_index], 2U
            );
            position.data.v1.memory.client_buffer.data = frontend_positions;
            position.data.v1.memory.client_buffer.data_size = sizeof(frontend_positions);
            add_output = make_plain_tensor(
                "frontend_output", QNN_TENSOR_TYPE_APP_READ, QNN_DATATYPE_FLOAT_16,
                frontend_output_dimensions[graph_index], 2U
            );
            tensors[5] = &position;
        }
        for (tensor_index = 0U; tensor_index < (graph_index == 0U ? 5U : 6U); ++tensor_index) {
            status = api->tensor_create_graph_tensor(frontend_graphs[graph_index], tensors[tensor_index]);
            if (status != 0U) {
                write_call_status("  frontend tensorCreate", status);
                return 99U;
            }
        }
        if (graph_index == 1U) {
            status = api->tensor_create_graph_tensor(frontend_graphs[graph_index], &add_output);
            if (status != 0U) return 99U;
        }
        status = add_graph_node(
            api, frontend_graphs[graph_index],
            graph_index == 0U ? "frontend_conv1_fc_op" : "frontend_conv2_fc_op",
            "FullyConnected", &frontend_inputs[graph_index], &weight, &bias, 3U,
            &fc_output, 0, 0U
        );
        if (status == 0U) status = add_graph_node(
            api, frontend_graphs[graph_index],
            graph_index == 0U ? "frontend_conv1_gelu_op" : "frontend_conv2_gelu_op",
            "Gelu", &fc_output, 0, 0, 1U, &gelu_output, 0, 0U
        );
        if (status != 0U) {
            write_call_status("  frontend graphAddNode", status);
            return 100U;
        }
        if (graph_index == 1U) {
            status = add_graph_node(
                api, frontend_graphs[graph_index], "frontend_position_add",
                "ElementWiseAdd", &gelu_output, &position, 0, 2U, &add_output, 0, 0U
            );
            if (status != 0U) return 100U;
            copy_tensor(&frontend_outputs[graph_index], &add_output);
        } else {
            copy_tensor(&frontend_outputs[graph_index], &gelu_output);
        }
        status = api->graph_finalize(frontend_graphs[graph_index], 0, 0);
        write_call_status("  frontend graphFinalize", status);
        if (status != 0U) return 101U;
    }
    return 0U;
}

static u32 __attribute__((unused)) run_cached_whisper_frontend(
    const QnnInterfaceV2 *api,
    u64 frequency,
    int validate_fixture
) {
    QnnTensor input;
    QnnTensor output;
    long long start_counter;
    long long end_counter;
    u64 status;
    u32 result;

    if (validate_fixture && load_whisper_frontend_model() != 1) return 102U;
    whisper_frontend_pack_conv1(frontend_log_mel, frontend_conv1_input);
    if (validate_fixture) {
        result = compare_frontend_fp16(
            "  frontend conv1 packed input", frontend_conv1_input, frontend_conv1_input_expected,
            WHISPER_FRAME_COUNT * WHISPER_MEL_BINS * 3U
        );
        if (result != 0U) return result;
    }
    copy_tensor(&input, &frontend_inputs[0]);
    copy_tensor(&output, &frontend_outputs[0]);
    input.data.v1.memory.client_buffer.data = frontend_conv1_input;
    input.data.v1.memory.client_buffer.data_size = sizeof(frontend_conv1_input);
    output.data.v1.memory.client_buffer.data = frontend_conv1_output;
    output.data.v1.memory.client_buffer.data_size = sizeof(frontend_conv1_output);
    QueryPerformanceCounter(&start_counter);
    status = api->graph_execute(frontend_graphs[0], &input, 1U, &output, 1U, 0, 0);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  frontend conv1 graphExecute", status);
    write_duration_us("  frontend conv1 time", (u64)(end_counter - start_counter), frequency);
    if (status != 0U) return 102U;
    if (validate_fixture) {
        result = compare_frontend_fp16(
            "  frontend conv1", frontend_conv1_output, frontend_conv1_expected,
            WHISPER_FRAME_COUNT * WHISPER_HIDDEN_SIZE
        );
        if (result != 0U) return result;
    }

    whisper_frontend_pack_conv2(frontend_conv1_output, frontend_conv2_input);
    copy_tensor(&input, &frontend_inputs[1]);
    copy_tensor(&output, &frontend_outputs[1]);
    input.data.v1.memory.client_buffer.data = frontend_conv2_input;
    input.data.v1.memory.client_buffer.data_size = sizeof(frontend_conv2_input);
    output.data.v1.memory.client_buffer.data = frontend_output;
    output.data.v1.memory.client_buffer.data_size = sizeof(frontend_output);
    QueryPerformanceCounter(&start_counter);
    status = api->graph_execute(frontend_graphs[1], &input, 1U, &output, 1U, 0, 0);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  frontend conv2 graphExecute", status);
    write_duration_us("  frontend conv2/position time", (u64)(end_counter - start_counter), frequency);
    if (status != 0U) return 103U;
    if (validate_fixture) {
        return compare_frontend_fp16(
            "  frontend final", frontend_output, frontend_expected,
            WHISPER_ENCODER_FRAMES * WHISPER_HIDDEN_SIZE
        );
    }
    write_buffer_fingerprint("  frontend FNV-1a: ", frontend_output, sizeof(frontend_output));
    return 0U;
}

static u32 run_attention_layout_probe(const QnnInterfaceV2 *api, QnnContextHandle context) {
    QnnGraphHandle graph = 0;
    u32 flat_dimensions[2] = {TINY_SEQUENCE_LENGTH, TINY_WIDTH};
    u32 split_dimensions[3] = {
        TINY_SEQUENCE_LENGTH, TINY_ATTENTION_HEADS, TINY_ATTENTION_HEAD_WIDTH
    };
    u32 head_dimensions[3] = {
        TINY_ATTENTION_HEADS, TINY_SEQUENCE_LENGTH, TINY_ATTENTION_HEAD_WIDTH
    };
    u32 perm_dimensions[1] = {3U};
    static u32 perm_data[3] = {1U, 0U, 2U};
    static QnnTensor input;
    static QnnTensor split;
    static QnnTensor output;
    static QnnTensor reshape_inputs[1];
    static QnnTensor reshape_outputs[1];
    static QnnTensor transpose_inputs[1];
    static QnnTensor transpose_outputs[1];
    static QnnParam transpose_parameter;
    QnnOpConfig operation = {0};
    u64 status;
    u32 sequence;
    u32 head;
    u32 lane;

    write_text("QNN Whisper Tiny attention head layout probe\n");
    status = api->graph_create(context, "npu_probe_tiny_attention_layout", 0, &graph);
    write_call_status("  graphCreate", status);
    if (status != 0U) return 59U;

    input = make_tensor("layout_input", QNN_TENSOR_TYPE_APP_WRITE, flat_dimensions, 2U);
    split = make_tensor("layout_split", QNN_TENSOR_TYPE_NATIVE, split_dimensions, 3U);
    output = make_tensor("layout_output", QNN_TENSOR_TYPE_APP_READ, head_dimensions, 3U);
    status = api->tensor_create_graph_tensor(graph, &input);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &split);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &output);
    write_call_status("  tensorCreate", status);
    if (status != 0U) return 60U;

    copy_tensor(&reshape_inputs[0], &input);
    copy_tensor(&reshape_outputs[0], &split);
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = "layout_reshape";
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = "Reshape";
    operation.data.v1.input_count = 1U;
    operation.data.v1.inputs = reshape_inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = reshape_outputs;
    status = api->graph_add_node(graph, operation);
    write_call_status("  graphAddNode Reshape", status);
    if (status != 0U) return 61U;

    transpose_parameter.type = QNN_PARAMTYPE_TENSOR;
    transpose_parameter.name = "perm";
    transpose_parameter.value.tensor = make_plain_tensor(
        "layout_perm", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_UINT_32, perm_dimensions, 1U
    );
    transpose_parameter.value.tensor.data.v1.memory.client_buffer.data = perm_data;
    transpose_parameter.value.tensor.data.v1.memory.client_buffer.data_size = sizeof(perm_data);
    status = api->tensor_create_graph_tensor(graph, &transpose_parameter.value.tensor);
    write_call_status("  tensorCreate perm", status);
    if (status != 0U) return 62U;
    copy_tensor(&transpose_inputs[0], &split);
    copy_tensor(&transpose_outputs[0], &output);
    operation.data.v1.name = "layout_transpose";
    operation.data.v1.type_name = "Transpose";
    operation.data.v1.parameter_count = 1U;
    operation.data.v1.parameters = &transpose_parameter;
    operation.data.v1.inputs = transpose_inputs;
    operation.data.v1.outputs = transpose_outputs;
    status = api->graph_add_node(graph, operation);
    write_call_status("  graphAddNode Transpose", status);
    if (status != 0U) return 62U;

    status = api->graph_finalize(graph, 0, 0);
    write_call_status("  graphFinalize", status);
    if (status != 0U) return 63U;
    for (sequence = 0U; sequence < TINY_SEQUENCE_LENGTH; ++sequence) {
        for (head = 0U; head < TINY_ATTENTION_HEADS; ++head) {
            for (lane = 0U; lane < TINY_ATTENTION_HEAD_WIDTH; ++lane) {
                u32 index = (sequence * TINY_ATTENTION_HEADS + head) *
                    TINY_ATTENTION_HEAD_WIDTH + lane;
                model_attention_input[index] = (u8)(index * 37U + 11U);
            }
        }
    }
    input.data.v1.memory.client_buffer.data = model_attention_input;
    input.data.v1.memory.client_buffer.data_size = sizeof(model_attention_input);
    output.data.v1.memory.client_buffer.data = model_attention_core_output;
    output.data.v1.memory.client_buffer.data_size = sizeof(model_attention_core_output);
    status = api->graph_execute(graph, &input, 1U, &output, 1U, 0, 0);
    write_call_status("  graphExecute", status);
    if (status != 0U) return 64U;
    for (head = 0U; head < TINY_ATTENTION_HEADS; ++head) {
        for (sequence = 0U; sequence < TINY_SEQUENCE_LENGTH; ++sequence) {
            for (lane = 0U; lane < TINY_ATTENTION_HEAD_WIDTH; ++lane) {
                u32 output_index = (head * TINY_SEQUENCE_LENGTH + sequence) *
                    TINY_ATTENTION_HEAD_WIDTH + lane;
                u32 input_index = (sequence * TINY_ATTENTION_HEADS + head) *
                    TINY_ATTENTION_HEAD_WIDTH + lane;
                if (model_attention_core_output[output_index] != model_attention_input[input_index]) {
                    write_text("  output mismatch at ");
                    write_u32(output_index);
                    write_text("\n");
                    return 65U;
                }
            }
        }
    }
    write_text("  output matches [1500,6,64] -> [6,1500,64] reference\n");
    return 0U;
}

static u32 run_model_layer_norm_probe(const QnnInterfaceV2 *api, QnnContextHandle context) {
    QnnGraphHandle graph = 0;
    u32 activation_dimensions[2] = {TINY_SEQUENCE_LENGTH, TINY_WIDTH};
    u32 parameter_dimensions[1] = {TINY_WIDTH};
    u32 axes_dimensions[1] = {1U};
    static u32 axes_data[1] = {1U};
    static QnnTensor input;
    static QnnTensor scale;
    static QnnTensor bias;
    static QnnTensor output;
    static QnnTensor node_inputs[3];
    static QnnTensor node_outputs[1];
    static QnnParam parameters[2];
    QnnOpConfig operation = {0};
    u64 status;
    int loaded = load_model_encoder_block_artifacts(0U);

    if (loaded == 0) {
        write_text("QNN model-derived Whisper Tiny LayerNorm: skipped (run calibration tooling first)\n");
        return 0U;
    }
    if (loaded < 0) {
        write_text("QNN model-derived Whisper Tiny LayerNorm: incomplete or invalid artifact set\n");
        return 66U;
    }
    write_text("QNN model-derived Whisper Tiny encoder layer 0 LayerNorm\n");
    status = api->graph_create(context, "npu_probe_tiny_model_layer_norm", 0, &graph);
    write_call_status("  graphCreate", status);
    if (status != 0U) return 67U;

    input = make_tensor("model_layer_norm_input", QNN_TENSOR_TYPE_APP_WRITE, activation_dimensions, 2U);
    set_tensor_scale_offset(&input, model_block_quant[0]);
    scale = make_tensor("model_layer_norm_scale", QNN_TENSOR_TYPE_STATIC, parameter_dimensions, 1U);
    set_tensor_scale_offset(&scale, model_layer_norm_scale_quant);
    scale.data.v1.memory.client_buffer.data = model_layer_norm_scale;
    scale.data.v1.memory.client_buffer.data_size = sizeof(model_layer_norm_scale);
    bias = make_tensor("model_layer_norm_bias", QNN_TENSOR_TYPE_STATIC, parameter_dimensions, 1U);
    bias.data.v1.data_type = QNN_DATATYPE_SFIXED_POINT_32;
    bias.data.v1.quantize_params.encoding.scale_offset.scale =
        model_block_quant[0].scale * model_layer_norm_scale_quant.scale;
    bias.data.v1.quantize_params.encoding.scale_offset.offset = 0;
    bias.data.v1.memory.client_buffer.data = model_layer_norm_bias;
    bias.data.v1.memory.client_buffer.data_size = sizeof(model_layer_norm_bias);
    output = make_tensor("model_layer_norm_output", QNN_TENSOR_TYPE_APP_READ, activation_dimensions, 2U);
    set_tensor_scale_offset(&output, model_block_quant[1]);
    status = api->tensor_create_graph_tensor(graph, &input);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &scale);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &bias);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &output);
    write_call_status("  tensorCreate", status);
    if (status != 0U) return 68U;

    parameters[0].type = QNN_PARAMTYPE_SCALAR;
    parameters[0].name = "epsilon";
    parameters[0].value.scalar.data_type = QNN_DATATYPE_FLOAT_32;
    parameters[0].value.scalar.value.float_value = 0.00001f;
    parameters[1].type = QNN_PARAMTYPE_TENSOR;
    parameters[1].name = "axes";
    parameters[1].value.tensor = make_plain_tensor(
        "model_layer_norm_axes", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_UINT_32,
        axes_dimensions, 1U
    );
    parameters[1].value.tensor.data.v1.memory.client_buffer.data = axes_data;
    parameters[1].value.tensor.data.v1.memory.client_buffer.data_size = sizeof(axes_data);
    status = api->tensor_create_graph_tensor(graph, &parameters[1].value.tensor);
    write_call_status("  tensorCreate axes", status);
    if (status != 0U) return 69U;

    copy_tensor(&node_inputs[0], &input);
    copy_tensor(&node_inputs[1], &scale);
    copy_tensor(&node_inputs[2], &bias);
    copy_tensor(&node_outputs[0], &output);
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = "model_layer_norm";
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = "LayerNorm";
    operation.data.v1.parameter_count = 2U;
    operation.data.v1.parameters = parameters;
    operation.data.v1.input_count = 3U;
    operation.data.v1.inputs = node_inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = node_outputs;
    status = api->graph_add_node(graph, operation);
    write_call_status("  graphAddNode LayerNorm", status);
    if (status != 0U) return 70U;
    status = api->graph_finalize(graph, 0, 0);
    write_call_status("  graphFinalize", status);
    if (status != 0U) return 71U;
    input.data.v1.memory.client_buffer.data = model_block_input;
    input.data.v1.memory.client_buffer.data_size = sizeof(model_block_input);
    output.data.v1.memory.client_buffer.data = model_attention_core_output;
    output.data.v1.memory.client_buffer.data_size = sizeof(model_attention_core_output);
    status = api->graph_execute(graph, &input, 1U, &output, 1U, 0, 0);
    write_call_status("  graphExecute", status);
    if (status != 0U) return 72U;
    if (!validate_layer_norm_output()) return 73U;
    return 0U;
}

static u32 run_model_attention_projections(
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    u64 frequency
) {
    static const char *weight_names[ATTENTION_PROJECTION_COUNT] = {
        "model_q_weight", "model_k_weight", "model_v_weight"
    };
    static const char *bias_names[ATTENTION_PROJECTION_COUNT] = {
        "model_q_bias", "model_k_bias", "model_v_bias"
    };
    static const char *output_names[ATTENTION_PROJECTION_COUNT] = {
        "model_q_output", "model_k_output", "model_v_output"
    };
    static const char *node_names[ATTENTION_PROJECTION_COUNT] = {
        "model_q_projection", "model_k_projection", "model_v_projection"
    };
    QnnGraphHandle graph = 0;
    u32 activation_dimensions[2] = {TINY_SEQUENCE_LENGTH, TINY_WIDTH};
    u32 weight_dimensions[2] = {TINY_WIDTH, TINY_WIDTH};
    u32 bias_dimensions[1] = {TINY_WIDTH};
    static QnnTensor input;
    static QnnTensor weights[ATTENTION_PROJECTION_COUNT];
    static QnnTensor biases[ATTENTION_PROJECTION_COUNT];
    static QnnTensor outputs[ATTENTION_PROJECTION_COUNT];
    static QnnTensor node_inputs[ATTENTION_PROJECTION_COUNT][3];
    static QnnTensor node_outputs[ATTENTION_PROJECTION_COUNT][1];
    QnnOpConfig operation = {0};
    static u64 samples[BENCHMARK_SAMPLES];
    u64 sample_total = 0U;
    long long start_counter;
    long long end_counter;
    u64 status;
    u32 projection;
    u32 index;
    int loaded = load_model_attention_artifacts(0U);

    if (loaded == 0) {
        write_text("QNN model-derived Whisper Tiny attention projections: skipped (run calibration tooling first)\n");
        return 0U;
    }
    if (loaded < 0) {
        write_text("QNN model-derived Whisper Tiny attention projections: incomplete or invalid artifact set\n");
        return 43U;
    }
    write_text("QNN model-derived Whisper Tiny encoder layer 0 Q/K/V projections\n");
    QueryPerformanceCounter(&start_counter);
    status = api->graph_create(context, "npu_probe_tiny_model_qkv", 0, &graph);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphCreate", status);
    write_duration_us("  graphCreate time", (u64)(end_counter - start_counter), frequency);
    if (status != 0U) return 44U;

    input = make_tensor("model_attention_input", QNN_TENSOR_TYPE_APP_WRITE, activation_dimensions, 2U);
    set_tensor_scale_offset(&input, model_attention_activation_quant[0]);
    status = api->tensor_create_graph_tensor(graph, &input);
    if (status != 0U) {
        write_call_status("  tensorCreate input", status);
        return 45U;
    }
    for (projection = 0U; projection < ATTENTION_PROJECTION_COUNT; ++projection) {
        weights[projection] = make_axis_tensor(
            weight_names[projection], QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_8,
            weight_dimensions, 2U, 0, TINY_WIDTH, model_attention_weight_quant[projection]
        );
        weights[projection].data.v1.memory.client_buffer.data = model_attention_weights[projection];
        weights[projection].data.v1.memory.client_buffer.data_size = sizeof(model_attention_weights[projection]);
        biases[projection] = make_axis_tensor(
            bias_names[projection], QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_32,
            bias_dimensions, 1U, 0, TINY_WIDTH, model_attention_bias_quant[projection]
        );
        biases[projection].data.v1.memory.client_buffer.data = model_attention_bias[projection];
        biases[projection].data.v1.memory.client_buffer.data_size = sizeof(model_attention_bias[projection]);
        outputs[projection] = make_tensor(
            output_names[projection], QNN_TENSOR_TYPE_APP_READ, activation_dimensions, 2U
        );
        set_tensor_scale_offset(&outputs[projection], model_attention_activation_quant[projection + 1U]);
        status = api->tensor_create_graph_tensor(graph, &weights[projection]);
        if (status == 0U) status = api->tensor_create_graph_tensor(graph, &biases[projection]);
        if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[projection]);
        if (status != 0U) {
            write_call_status("  tensorCreate projection", status);
            return 45U;
        }
    }
    write_text("  tensorCreate: all attention projection tensors registered\n");

    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = "FullyConnected";
    operation.data.v1.input_count = 3U;
    operation.data.v1.output_count = 1U;
    for (projection = 0U; projection < ATTENTION_PROJECTION_COUNT; ++projection) {
        copy_tensor(&node_inputs[projection][0], &input);
        copy_tensor(&node_inputs[projection][1], &weights[projection]);
        copy_tensor(&node_inputs[projection][2], &biases[projection]);
        copy_tensor(&node_outputs[projection][0], &outputs[projection]);
        operation.data.v1.name = node_names[projection];
        operation.data.v1.inputs = node_inputs[projection];
        operation.data.v1.outputs = node_outputs[projection];
        status = api->graph_add_node(graph, operation);
        write_call_status("  graphAddNode FullyConnected", status);
        if (status != 0U) return 46U;
    }

    QueryPerformanceCounter(&start_counter);
    status = api->graph_finalize(graph, 0, 0);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphFinalize", status);
    write_duration_us("  graphFinalize time", (u64)(end_counter - start_counter), frequency);
    if (status != 0U) return 47U;

    input.data.v1.memory.client_buffer.data = model_attention_input;
    input.data.v1.memory.client_buffer.data_size = sizeof(model_attention_input);
    for (projection = 0U; projection < ATTENTION_PROJECTION_COUNT; ++projection) {
        outputs[projection].data.v1.memory.client_buffer.data = model_attention_output[projection];
        outputs[projection].data.v1.memory.client_buffer.data_size = sizeof(model_attention_output[projection]);
    }
    QueryPerformanceCounter(&start_counter);
    status = api->graph_execute(graph, &input, 1U, outputs, ATTENTION_PROJECTION_COUNT, 0, 0);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphExecute", status);
    if (status != 0U) return 48U;
    write_duration_us("  first execution", (u64)(end_counter - start_counter), frequency);
    if (!validate_attention_outputs()) return 49U;

    for (index = 0U; index < BENCHMARK_WARMUPS; ++index) {
        status = api->graph_execute(graph, &input, 1U, outputs, ATTENTION_PROJECTION_COUNT, 0, 0);
        if (status != 0U) return 48U;
    }
    for (index = 0U; index < BENCHMARK_SAMPLES; ++index) {
        QueryPerformanceCounter(&start_counter);
        status = api->graph_execute(graph, &input, 1U, outputs, ATTENTION_PROJECTION_COUNT, 0, 0);
        QueryPerformanceCounter(&end_counter);
        if (status != 0U) return 48U;
        samples[index] = (u64)(end_counter - start_counter);
        sample_total += samples[index];
    }
    if (!validate_attention_outputs()) return 49U;
    sort_u64(samples, BENCHMARK_SAMPLES);
    write_text("QNN model-derived Tiny Q/K/V projection latency (10 warmups, 100 samples)\n");
    write_duration_us("  min", samples[0], frequency);
    write_duration_us("  median", samples[50], frequency);
    write_duration_us("  p95", samples[94], frequency);
    write_duration_us("  max", samples[99], frequency);
    write_duration_us("  mean", sample_total / BENCHMARK_SAMPLES, frequency);
    write_gmac_per_second(
        "  median throughput",
        3ULL * TINY_SEQUENCE_LENGTH * TINY_WIDTH * TINY_WIDTH,
        samples[50],
        frequency
    );
    return 0U;
}

static u32 run_model_attention_core(
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    u64 frequency
) {
    QnnGraphHandle graph = 0;
    u32 query_dimensions[3] = {
        TINY_ATTENTION_HEADS, TINY_SEQUENCE_LENGTH, TINY_ATTENTION_HEAD_WIDTH
    };
    u32 key_dimensions[3] = {
        TINY_ATTENTION_HEADS, TINY_ATTENTION_HEAD_WIDTH, TINY_SEQUENCE_LENGTH
    };
    u32 score_dimensions[3] = {
        TINY_ATTENTION_HEADS, TINY_SEQUENCE_LENGTH, TINY_SEQUENCE_LENGTH
    };
    static QnnTensor inputs[3];
    static QnnTensor score;
    static QnnTensor probability;
    static QnnTensor output[1];
    static QnnTensor score_inputs[2];
    static QnnTensor score_outputs[1];
    static QnnTensor probability_inputs[1];
    static QnnTensor probability_outputs[1];
    static QnnTensor value_inputs[2];
    static QnnTensor value_outputs[1];
    QnnOpConfig operation = {0};
    static u64 samples[BENCHMARK_SAMPLES];
    u64 sample_total = 0U;
    long long start_counter;
    long long end_counter;
    u64 status;
    u32 index;
    int loaded = load_model_attention_core_artifacts();

    if (loaded == 0) {
        write_text("QNN model-derived Whisper Tiny attention core: skipped (run calibration tooling first)\n");
        return 0U;
    }
    if (loaded < 0) {
        write_text("QNN model-derived Whisper Tiny attention core: incomplete or invalid artifact set\n");
        return 50U;
    }
    write_text("QNN model-derived Whisper Tiny encoder layer 0 attention core\n");
    QueryPerformanceCounter(&start_counter);
    status = api->graph_create(context, "npu_probe_tiny_model_attention_core", 0, &graph);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphCreate", status);
    write_duration_us("  graphCreate time", (u64)(end_counter - start_counter), frequency);
    if (status != 0U) return 51U;

    inputs[0] = make_tensor("model_attention_q", QNN_TENSOR_TYPE_APP_WRITE, query_dimensions, 3U);
    inputs[1] = make_tensor("model_attention_k", QNN_TENSOR_TYPE_APP_WRITE, key_dimensions, 3U);
    inputs[2] = make_tensor("model_attention_v", QNN_TENSOR_TYPE_APP_WRITE, query_dimensions, 3U);
    for (index = 0U; index < ATTENTION_PROJECTION_COUNT; ++index) {
        set_tensor_scale_offset(&inputs[index], model_attention_core_quant[index]);
        status = api->tensor_create_graph_tensor(graph, &inputs[index]);
        if (status != 0U) {
            write_call_status("  tensorCreate input", status);
            return 52U;
        }
    }
    score = make_tensor("model_attention_scores", QNN_TENSOR_TYPE_NATIVE, score_dimensions, 3U);
    set_tensor_scale_offset(&score, model_attention_core_quant[3]);
    probability = make_tensor("model_attention_probabilities", QNN_TENSOR_TYPE_NATIVE, score_dimensions, 3U);
    set_tensor_scale_offset(&probability, model_attention_core_quant[4]);
    output[0] = make_tensor("model_attention_values", QNN_TENSOR_TYPE_APP_READ, query_dimensions, 3U);
    set_tensor_scale_offset(&output[0], model_attention_core_quant[5]);
    status = api->tensor_create_graph_tensor(graph, &score);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &probability);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &output[0]);
    if (status != 0U) {
        write_call_status("  tensorCreate intermediate/output", status);
        return 52U;
    }
    write_text("  tensorCreate: all attention core tensors registered\n");

    copy_tensor(&score_inputs[0], &inputs[0]);
    copy_tensor(&score_inputs[1], &inputs[1]);
    copy_tensor(&score_outputs[0], &score);
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = "model_attention_scores";
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = "MatMul";
    operation.data.v1.input_count = 2U;
    operation.data.v1.inputs = score_inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = score_outputs;
    status = api->graph_add_node(graph, operation);
    write_call_status("  graphAddNode MatMul scores", status);
    if (status != 0U) return 53U;

    copy_tensor(&probability_inputs[0], &score);
    copy_tensor(&probability_outputs[0], &probability);
    operation.data.v1.name = "model_attention_softmax";
    operation.data.v1.type_name = "Softmax";
    operation.data.v1.input_count = 1U;
    operation.data.v1.inputs = probability_inputs;
    operation.data.v1.outputs = probability_outputs;
    status = api->graph_add_node(graph, operation);
    write_call_status("  graphAddNode Softmax", status);
    if (status != 0U) return 54U;

    copy_tensor(&value_inputs[0], &probability);
    copy_tensor(&value_inputs[1], &inputs[2]);
    copy_tensor(&value_outputs[0], &output[0]);
    operation.data.v1.name = "model_attention_values";
    operation.data.v1.type_name = "MatMul";
    operation.data.v1.input_count = 2U;
    operation.data.v1.inputs = value_inputs;
    operation.data.v1.outputs = value_outputs;
    status = api->graph_add_node(graph, operation);
    write_call_status("  graphAddNode MatMul values", status);
    if (status != 0U) return 55U;

    QueryPerformanceCounter(&start_counter);
    status = api->graph_finalize(graph, 0, 0);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphFinalize", status);
    write_duration_us("  graphFinalize time", (u64)(end_counter - start_counter), frequency);
    if (status != 0U) return 56U;

    for (index = 0U; index < ATTENTION_PROJECTION_COUNT; ++index) {
        inputs[index].data.v1.memory.client_buffer.data = model_attention_core_input[index];
        inputs[index].data.v1.memory.client_buffer.data_size = sizeof(model_attention_core_input[index]);
    }
    output[0].data.v1.memory.client_buffer.data = model_attention_core_output;
    output[0].data.v1.memory.client_buffer.data_size = sizeof(model_attention_core_output);
    QueryPerformanceCounter(&start_counter);
    status = api->graph_execute(graph, inputs, ATTENTION_PROJECTION_COUNT, output, 1U, 0, 0);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphExecute", status);
    if (status != 0U) return 57U;
    write_duration_us("  first execution", (u64)(end_counter - start_counter), frequency);
    if (!validate_attention_core_output()) return 58U;

    for (index = 0U; index < BENCHMARK_WARMUPS; ++index) {
        status = api->graph_execute(graph, inputs, ATTENTION_PROJECTION_COUNT, output, 1U, 0, 0);
        if (status != 0U) return 57U;
    }
    for (index = 0U; index < BENCHMARK_SAMPLES; ++index) {
        QueryPerformanceCounter(&start_counter);
        status = api->graph_execute(graph, inputs, ATTENTION_PROJECTION_COUNT, output, 1U, 0, 0);
        QueryPerformanceCounter(&end_counter);
        if (status != 0U) return 57U;
        samples[index] = (u64)(end_counter - start_counter);
        sample_total += samples[index];
    }
    if (!validate_attention_core_output()) return 58U;
    sort_u64(samples, BENCHMARK_SAMPLES);
    write_text("QNN model-derived Tiny attention core latency (10 warmups, 100 samples)\n");
    write_duration_us("  min", samples[0], frequency);
    write_duration_us("  median", samples[50], frequency);
    write_duration_us("  p95", samples[94], frequency);
    write_duration_us("  max", samples[99], frequency);
    write_duration_us("  mean", sample_total / BENCHMARK_SAMPLES, frequency);
    write_gmac_per_second(
        "  median throughput",
        2ULL * TINY_ATTENTION_HEADS * TINY_SEQUENCE_LENGTH *
            TINY_SEQUENCE_LENGTH * TINY_ATTENTION_HEAD_WIDTH,
        samples[50],
        frequency
    );
    return 0U;
}

static u32 run_model_mlp(const QnnInterfaceV2 *api, QnnContextHandle context, u64 frequency) {
    QnnGraphHandle graph = 0;
    u32 input_dimensions[2] = {TINY_SEQUENCE_LENGTH, TINY_WIDTH};
    u32 hidden_dimensions[2] = {TINY_SEQUENCE_LENGTH, TINY_MLP_WIDTH};
    u32 fc1_weight_dimensions[2] = {TINY_MLP_WIDTH, TINY_WIDTH};
    u32 fc2_weight_dimensions[2] = {TINY_WIDTH, TINY_MLP_WIDTH};
    u32 fc1_bias_dimensions[1] = {TINY_MLP_WIDTH};
    u32 fc2_bias_dimensions[1] = {TINY_WIDTH};
    static QnnTensor fc1_inputs[3];
    static QnnTensor fc1_outputs[1];
    static QnnTensor fc2_inputs[3];
    static QnnTensor fc2_outputs[1];
    static QnnTensor *tensors[8];
    QnnOpConfig operation = {0};
    static u64 samples[BENCHMARK_SAMPLES];
    u64 sample_total = 0U;
    long long start_counter;
    long long end_counter;
    u64 status;
    u32 index;
    int loaded = load_model_mlp_artifacts(0U);

    if (loaded == 0) {
        write_text("QNN model-derived Whisper Tiny MLP: skipped (run calibration tooling first)\n");
        return 0U;
    }
    if (loaded < 0) {
        write_text("QNN model-derived Whisper Tiny MLP: incomplete or invalid artifact set\n");
        return 34U;
    }
    write_text("QNN model-derived Whisper Tiny encoder layer 0 MLP\n");
    QueryPerformanceCounter(&start_counter);
    status = api->graph_create(context, "npu_probe_tiny_model_mlp", 0, &graph);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphCreate", status);
    write_duration_us("  graphCreate time", (u64)(end_counter - start_counter), frequency);
    if (status != 0U) return 35U;

    fc1_inputs[0] = make_tensor("model_mlp_input", QNN_TENSOR_TYPE_APP_WRITE, input_dimensions, 2U);
    set_tensor_scale_offset(&fc1_inputs[0], model_activation_quant[0]);
    fc1_inputs[1] = make_axis_tensor(
        "model_fc1_weight", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_8,
        fc1_weight_dimensions, 2U, 0, TINY_MLP_WIDTH, model_fc1_weight_quant
    );
    fc1_inputs[1].data.v1.memory.client_buffer.data = model_fc1_weights;
    fc1_inputs[1].data.v1.memory.client_buffer.data_size = sizeof(model_fc1_weights);
    fc1_inputs[2] = make_axis_tensor(
        "model_fc1_bias", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_32,
        fc1_bias_dimensions, 1U, 0, TINY_MLP_WIDTH, model_fc1_bias_quant
    );
    fc1_inputs[2].data.v1.memory.client_buffer.data = model_fc1_bias;
    fc1_inputs[2].data.v1.memory.client_buffer.data_size = sizeof(model_fc1_bias);
    fc1_outputs[0] = make_tensor("model_fc1_output", QNN_TENSOR_TYPE_NATIVE, hidden_dimensions, 2U);
    set_tensor_scale_offset(&fc1_outputs[0], model_activation_quant[1]);
    fc2_inputs[0] = make_tensor("model_gelu_output", QNN_TENSOR_TYPE_NATIVE, hidden_dimensions, 2U);
    set_tensor_scale_offset(&fc2_inputs[0], model_activation_quant[2]);
    fc2_inputs[1] = make_axis_tensor(
        "model_fc2_weight", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_8,
        fc2_weight_dimensions, 2U, 0, TINY_WIDTH, model_fc2_weight_quant
    );
    fc2_inputs[1].data.v1.memory.client_buffer.data = model_fc2_weights;
    fc2_inputs[1].data.v1.memory.client_buffer.data_size = sizeof(model_fc2_weights);
    fc2_inputs[2] = make_axis_tensor(
        "model_fc2_bias", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_32,
        fc2_bias_dimensions, 1U, 0, TINY_WIDTH, model_fc2_bias_quant
    );
    fc2_inputs[2].data.v1.memory.client_buffer.data = model_fc2_bias;
    fc2_inputs[2].data.v1.memory.client_buffer.data_size = sizeof(model_fc2_bias);
    fc2_outputs[0] = make_tensor("model_mlp_output", QNN_TENSOR_TYPE_APP_READ, input_dimensions, 2U);
    set_tensor_scale_offset(&fc2_outputs[0], model_activation_quant[3]);

    tensors[0] = &fc1_inputs[0];
    tensors[1] = &fc1_inputs[1];
    tensors[2] = &fc1_inputs[2];
    tensors[3] = &fc1_outputs[0];
    tensors[4] = &fc2_inputs[0];
    tensors[5] = &fc2_inputs[1];
    tensors[6] = &fc2_inputs[2];
    tensors[7] = &fc2_outputs[0];
    for (index = 0U; index < 8U; ++index) {
        status = api->tensor_create_graph_tensor(graph, tensors[index]);
        if (status != 0U) {
            write_call_status("  tensorCreate", status);
            return 36U;
        }
    }
    write_text("  tensorCreate: all model tensors registered\n");

    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = "model_fc1";
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = "FullyConnected";
    operation.data.v1.input_count = 3U;
    operation.data.v1.inputs = fc1_inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = fc1_outputs;
    status = api->graph_add_node(graph, operation);
    write_call_status("  graphAddNode FullyConnected fc1", status);
    if (status != 0U) return 37U;

    operation.data.v1.name = "model_gelu";
    operation.data.v1.type_name = "Gelu";
    operation.data.v1.input_count = 1U;
    operation.data.v1.inputs = fc1_outputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = fc2_inputs;
    status = api->graph_add_node(graph, operation);
    write_call_status("  graphAddNode Gelu", status);
    if (status != 0U) return 38U;

    operation.data.v1.name = "model_fc2";
    operation.data.v1.type_name = "FullyConnected";
    operation.data.v1.input_count = 3U;
    operation.data.v1.inputs = fc2_inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = fc2_outputs;
    status = api->graph_add_node(graph, operation);
    write_call_status("  graphAddNode FullyConnected fc2", status);
    if (status != 0U) return 39U;

    QueryPerformanceCounter(&start_counter);
    status = api->graph_finalize(graph, 0, 0);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphFinalize", status);
    write_duration_us("  graphFinalize time", (u64)(end_counter - start_counter), frequency);
    if (status != 0U) return 40U;

    fc1_inputs[0].data.v1.memory.client_buffer.data = model_input;
    fc1_inputs[0].data.v1.memory.client_buffer.data_size = sizeof(model_input);
    fc2_outputs[0].data.v1.memory.client_buffer.data = model_output;
    fc2_outputs[0].data.v1.memory.client_buffer.data_size = sizeof(model_output);
    QueryPerformanceCounter(&start_counter);
    status = api->graph_execute(graph, fc1_inputs, 1U, fc2_outputs, 1U, 0, 0);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphExecute", status);
    if (status != 0U) return 41U;
    write_duration_us("  first execution", (u64)(end_counter - start_counter), frequency);
    if (!validate_model_output()) return 42U;

    for (index = 0U; index < BENCHMARK_WARMUPS; ++index) {
        status = api->graph_execute(graph, fc1_inputs, 1U, fc2_outputs, 1U, 0, 0);
        if (status != 0U) return 41U;
    }
    for (index = 0U; index < BENCHMARK_SAMPLES; ++index) {
        QueryPerformanceCounter(&start_counter);
        status = api->graph_execute(graph, fc1_inputs, 1U, fc2_outputs, 1U, 0, 0);
        QueryPerformanceCounter(&end_counter);
        if (status != 0U) return 41U;
        samples[index] = (u64)(end_counter - start_counter);
        sample_total += samples[index];
    }
    if (!validate_model_output()) return 42U;
    sort_u64(samples, BENCHMARK_SAMPLES);
    write_text("QNN model-derived Tiny MLP latency (10 warmups, 100 samples)\n");
    write_duration_us("  min", samples[0], frequency);
    write_duration_us("  median", samples[50], frequency);
    write_duration_us("  p95", samples[94], frequency);
    write_duration_us("  max", samples[99], frequency);
    write_duration_us("  mean", sample_total / BENCHMARK_SAMPLES, frequency);
    write_gmac_per_second(
        "  median throughput",
        2ULL * TINY_SEQUENCE_LENGTH * TINY_WIDTH * TINY_MLP_WIDTH,
        samples[50],
        frequency
    );
    return 0U;
}

static u32 run_model_encoder_block(
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    u64 frequency,
    u32 layer,
    int fp16,
    int monolithic
) {
    enum {
        BLOCK_INPUT = 0,
        BLOCK_LN1_SCALE = 1,
        BLOCK_LN1_BIAS = 2,
        BLOCK_LN1_OUTPUT = 3,
        BLOCK_PROJECTION_BASE = 4,
        BLOCK_SCORES = 19,
        BLOCK_PROBABILITIES = 20,
        BLOCK_ATTENDED_HEADS = 21,
        BLOCK_ATTENDED_SPLIT = 22,
        BLOCK_ATTENDED_FLAT = 23,
        BLOCK_OUT_WEIGHT = 24,
        BLOCK_OUT_BIAS = 25,
        BLOCK_ATTENTION_OUTPUT = 26,
        BLOCK_ATTENTION_RESIDUAL = 27,
        BLOCK_LN2_SCALE = 28,
        BLOCK_LN2_BIAS = 29,
        BLOCK_LN2_OUTPUT = 30,
        BLOCK_FC1_WEIGHT = 31,
        BLOCK_FC1_BIAS = 32,
        BLOCK_FC1_OUTPUT = 33,
        BLOCK_GELU_OUTPUT = 34,
        BLOCK_FC2_WEIGHT = 35,
        BLOCK_FC2_BIAS = 36,
        BLOCK_FC2_OUTPUT = 37,
        BLOCK_OUTPUT = 38,
        BLOCK_TENSOR_COUNT = 39
    };
#define BLOCK_PROJECTION_WEIGHT(projection) block_tensors[BLOCK_PROJECTION_BASE + (projection) * 5U]
#define BLOCK_PROJECTION_BIAS(projection) block_tensors[BLOCK_PROJECTION_BASE + (projection) * 5U + 1U]
#define BLOCK_PROJECTION_OUTPUT(projection) block_tensors[BLOCK_PROJECTION_BASE + (projection) * 5U + 2U]
#define BLOCK_PROJECTION_SPLIT(projection) block_tensors[BLOCK_PROJECTION_BASE + (projection) * 5U + 3U]
#define BLOCK_PROJECTION_HEADS(projection) block_tensors[BLOCK_PROJECTION_BASE + (projection) * 5U + 4U]
    static const char *projection_names[ATTENTION_PROJECTION_COUNT] = {
        "block_q_projection", "block_k_projection", "block_v_projection"
    };
    static const char *weight_names[ATTENTION_PROJECTION_COUNT] = {
        "block_q_weight", "block_k_weight", "block_v_weight"
    };
    static const char *bias_names[ATTENTION_PROJECTION_COUNT] = {
        "block_q_bias", "block_k_bias", "block_v_bias"
    };
    static const char *output_names[ATTENTION_PROJECTION_COUNT] = {
        "block_q_output", "block_k_output", "block_v_output"
    };
    static const char *split_names[ATTENTION_PROJECTION_COUNT] = {
        "block_q_split", "block_k_split", "block_v_split"
    };
    static const char *head_names[ATTENTION_PROJECTION_COUNT] = {
        "block_q_heads", "block_k_heads", "block_v_heads"
    };
    static const char *reshape_names[ATTENTION_PROJECTION_COUNT] = {
        "block_q_reshape", "block_k_reshape", "block_v_reshape"
    };
    static const char *transpose_names[ATTENTION_PROJECTION_COUNT] = {
        "block_q_transpose", "block_k_transpose", "block_v_transpose"
    };
    static const char *graph_names[4] = {
        "npu_probe_tiny_encoder_block_0",
        "npu_probe_tiny_encoder_block_1",
        "npu_probe_tiny_encoder_block_2",
        "npu_probe_tiny_encoder_block_3"
    };
    static const char *fp16_graph_names[4] = {
        "npu_probe_tiny_encoder_block_fp16_0",
        "npu_probe_tiny_encoder_block_fp16_1",
        "npu_probe_tiny_encoder_block_fp16_2",
        "npu_probe_tiny_encoder_block_fp16_3"
    };
    QnnGraphHandle graph = 0;
    u32 activation_dimensions[2] = {TINY_SEQUENCE_LENGTH, TINY_WIDTH};
    u32 hidden_dimensions[2] = {TINY_SEQUENCE_LENGTH, TINY_MLP_WIDTH};
    u32 projection_weight_dimensions[2] = {TINY_WIDTH, TINY_WIDTH};
    u32 fc1_weight_dimensions[2] = {TINY_MLP_WIDTH, TINY_WIDTH};
    u32 fc2_weight_dimensions[2] = {TINY_WIDTH, TINY_MLP_WIDTH};
    u32 width_dimensions[1] = {TINY_WIDTH};
    u32 hidden_bias_dimensions[1] = {TINY_MLP_WIDTH};
    u32 split_dimensions[3] = {
        TINY_SEQUENCE_LENGTH, TINY_ATTENTION_HEADS, TINY_ATTENTION_HEAD_WIDTH
    };
    u32 head_dimensions[3] = {
        TINY_ATTENTION_HEADS, TINY_SEQUENCE_LENGTH, TINY_ATTENTION_HEAD_WIDTH
    };
    u32 key_dimensions[3] = {
        TINY_ATTENTION_HEADS, TINY_ATTENTION_HEAD_WIDTH, TINY_SEQUENCE_LENGTH
    };
    u32 score_dimensions[3] = {
        TINY_ATTENTION_HEADS, TINY_SEQUENCE_LENGTH, TINY_SEQUENCE_LENGTH
    };
    u32 vector_dimensions[1] = {1U};
    u32 perm_dimensions[1] = {3U};
    static u32 axes_data[1] = {1U};
    static u32 head_perm_data[3] = {1U, 0U, 2U};
    static u32 key_perm_data[3] = {1U, 2U, 0U};
    static QnnTensor block_tensors[BLOCK_TENSOR_COUNT];
    static QnnParam layer_norm_parameters[2][2];
    static QnnParam transpose_parameters[2];
    static QnnTensor *registered[BLOCK_TENSOR_COUNT + 4U];
    static char monolithic_tensor_names[4][BLOCK_TENSOR_COUNT + 4U][64];
    static u64 samples[BENCHMARK_SAMPLES];
    u64 sample_total = 0U;
    long long start_counter;
    long long end_counter;
    u64 status;
    u32 registered_count = 0U;
    u32 projection;
    u32 channel;
    u32 index;
    int loaded = fp16
        ? load_model_fp16_encoder_block_artifacts(layer)
        : load_model_encoder_block_artifacts(layer);

    if (!fp16 && loaded == 1) loaded = load_model_attention_artifacts(layer);
    if (!fp16 && loaded == 1) loaded = load_model_mlp_artifacts(layer);
    if (loaded == 0) {
        write_text("QNN model-derived Whisper Tiny encoder block: skipped (run calibration tooling first)\n");
        return 0U;
    }
    if (loaded < 0) {
        write_text("QNN model-derived Whisper Tiny encoder block: incomplete or invalid artifact set\n");
        return 74U;
    }
    if (!fp16 && layer == 0U) {
        for (index = 0U; index < sizeof(model_encoder_stack_input); ++index) {
            model_encoder_stack_input[index] = model_block_input[index];
        }
    } else if (fp16 && layer == 0U) {
        for (index = 0U; index < TINY_SEQUENCE_LENGTH * TINY_WIDTH; ++index) {
            model_fp16_encoder_stack_input[index] = model_fp16_block_input[index];
        }
    }
    for (projection = 0U; !fp16 && projection < ATTENTION_PROJECTION_COUNT; ++projection) {
        for (channel = 0U; channel < TINY_WIDTH; ++channel) {
            model_attention_bias_quant[projection][channel].scale =
                model_block_quant[1].scale * model_attention_weight_scales[projection][channel];
        }
    }
    for (channel = 0U; !fp16 && channel < TINY_MLP_WIDTH; ++channel) {
        model_fc1_bias_quant[channel].scale =
            model_block_quant[10].scale * model_fc1_weight_scales[channel];
    }
    for (channel = 0U; !fp16 && channel < TINY_WIDTH; ++channel) {
        model_fc2_bias_quant[channel].scale =
            model_block_quant[12].scale * model_fc2_weight_scales[channel];
    }

    write_text(monolithic ? "QNN monolithic FP16 encoder layer " :
        (fp16 ? "QNN FP16 model-derived Whisper Tiny encoder layer " :
        "QNN complete model-derived Whisper Tiny encoder layer "));
    write_u32(layer);
    write_text("\n");
    if (monolithic && layer != 0U) {
        graph = model_fp16_monolithic_graph;
    } else {
        QueryPerformanceCounter(&start_counter);
        status = api->graph_create(
            context,
            monolithic ? "npu_probe_tiny_encoder_monolithic_fp16" :
                (fp16 ? fp16_graph_names[layer] : graph_names[layer]),
            0,
            &graph
        );
        QueryPerformanceCounter(&end_counter);
        write_call_status("  graphCreate", status);
        write_duration_us("  graphCreate time", (u64)(end_counter - start_counter), frequency);
        if (status != 0U) return 75U;
        if (monolithic) model_fp16_monolithic_graph = graph;
    }

    block_tensors[BLOCK_INPUT] = make_encoder_tensor(
        "block_input", QNN_TENSOR_TYPE_APP_WRITE, activation_dimensions, 2U,
        fp16, model_block_quant[0]
    );
    block_tensors[BLOCK_LN1_SCALE] = make_encoder_tensor(
        "block_attention_norm_scale", QNN_TENSOR_TYPE_STATIC, width_dimensions, 1U,
        fp16, model_layer_norm_scale_quant
    );
    block_tensors[BLOCK_LN1_SCALE].data.v1.memory.client_buffer.data =
        fp16 ? (void *)model_fp16_layer_norm_scale[layer] : (void *)model_layer_norm_scale;
    block_tensors[BLOCK_LN1_SCALE].data.v1.memory.client_buffer.data_size =
        fp16 ? sizeof(model_fp16_layer_norm_scale[layer]) : sizeof(model_layer_norm_scale);
    block_tensors[BLOCK_LN1_BIAS] = make_encoder_tensor(
        "block_attention_norm_bias", QNN_TENSOR_TYPE_STATIC, width_dimensions, 1U,
        fp16, (QnnScaleOffset){
            model_block_quant[0].scale * model_layer_norm_scale_quant.scale, 0
        }
    );
    if (!fp16) block_tensors[BLOCK_LN1_BIAS].data.v1.data_type = QNN_DATATYPE_SFIXED_POINT_32;
    block_tensors[BLOCK_LN1_BIAS].data.v1.memory.client_buffer.data =
        fp16 ? (void *)model_fp16_layer_norm_bias[layer] : (void *)model_layer_norm_bias;
    block_tensors[BLOCK_LN1_BIAS].data.v1.memory.client_buffer.data_size =
        fp16 ? sizeof(model_fp16_layer_norm_bias[layer]) : sizeof(model_layer_norm_bias);
    block_tensors[BLOCK_LN1_OUTPUT] = make_encoder_tensor(
        "block_attention_norm_output", QNN_TENSOR_TYPE_NATIVE, activation_dimensions, 2U,
        fp16, model_block_quant[1]
    );

    for (projection = 0U; projection < ATTENTION_PROJECTION_COUNT; ++projection) {
        BLOCK_PROJECTION_WEIGHT(projection) = fp16
            ? make_plain_tensor(
                weight_names[projection], QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_FLOAT_16,
                projection_weight_dimensions, 2U)
            : make_axis_tensor(
                weight_names[projection], QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_8,
                projection_weight_dimensions, 2U, 0, TINY_WIDTH,
                model_attention_weight_quant[projection]);
        BLOCK_PROJECTION_WEIGHT(projection).data.v1.memory.client_buffer.data =
            fp16 ? (void *)model_fp16_attention_weights[layer][projection] :
                (void *)model_attention_weights[projection];
        BLOCK_PROJECTION_WEIGHT(projection).data.v1.memory.client_buffer.data_size =
            fp16 ? sizeof(model_fp16_attention_weights[layer][projection]) :
                sizeof(model_attention_weights[projection]);
        BLOCK_PROJECTION_BIAS(projection) = fp16
            ? make_plain_tensor(
                bias_names[projection], QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_FLOAT_16,
                width_dimensions, 1U)
            : make_axis_tensor(
                bias_names[projection], QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_32,
                width_dimensions, 1U, 0, TINY_WIDTH, model_attention_bias_quant[projection]);
        BLOCK_PROJECTION_BIAS(projection).data.v1.memory.client_buffer.data =
            fp16 ? (void *)model_fp16_attention_bias[layer][projection] :
                (void *)model_attention_bias[projection];
        BLOCK_PROJECTION_BIAS(projection).data.v1.memory.client_buffer.data_size =
            fp16 ? sizeof(model_fp16_attention_bias[layer][projection]) :
                sizeof(model_attention_bias[projection]);
        BLOCK_PROJECTION_OUTPUT(projection) = make_encoder_tensor(
            output_names[projection], QNN_TENSOR_TYPE_NATIVE, activation_dimensions, 2U,
            fp16, model_block_quant[2U + projection]
        );
        BLOCK_PROJECTION_SPLIT(projection) = make_encoder_tensor(
            split_names[projection], QNN_TENSOR_TYPE_NATIVE, split_dimensions, 3U,
            fp16, model_block_quant[2U + projection]
        );
        BLOCK_PROJECTION_HEADS(projection) = make_encoder_tensor(
            head_names[projection], QNN_TENSOR_TYPE_NATIVE,
            projection == 1U ? key_dimensions : head_dimensions, 3U,
            fp16, model_block_quant[2U + projection]
        );
    }
    block_tensors[BLOCK_SCORES] = make_encoder_tensor(
        "block_attention_scores", QNN_TENSOR_TYPE_NATIVE, score_dimensions, 3U,
        fp16, model_block_quant[5]
    );
    block_tensors[BLOCK_PROBABILITIES] = make_encoder_tensor(
        "block_attention_probabilities", QNN_TENSOR_TYPE_NATIVE, score_dimensions, 3U,
        fp16, model_block_quant[6]
    );
    block_tensors[BLOCK_ATTENDED_HEADS] = make_encoder_tensor(
        "block_attended_heads", QNN_TENSOR_TYPE_NATIVE, head_dimensions, 3U,
        fp16, model_block_quant[7]
    );
    block_tensors[BLOCK_ATTENDED_SPLIT] = make_encoder_tensor(
        "block_attended_split", QNN_TENSOR_TYPE_NATIVE, split_dimensions, 3U,
        fp16, model_block_quant[7]
    );
    block_tensors[BLOCK_ATTENDED_FLAT] = make_encoder_tensor(
        "block_attended_flat", QNN_TENSOR_TYPE_NATIVE, activation_dimensions, 2U,
        fp16, model_block_quant[7]
    );
    block_tensors[BLOCK_OUT_WEIGHT] = fp16
        ? make_plain_tensor(
            "block_out_weight", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_FLOAT_16,
            projection_weight_dimensions, 2U)
        : make_axis_tensor(
            "block_out_weight", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_8,
            projection_weight_dimensions, 2U, 0, TINY_WIDTH, model_out_proj_weight_quant);
    block_tensors[BLOCK_OUT_WEIGHT].data.v1.memory.client_buffer.data =
        fp16 ? (void *)model_fp16_out_proj_weights[layer] : (void *)model_out_proj_weights;
    block_tensors[BLOCK_OUT_WEIGHT].data.v1.memory.client_buffer.data_size =
        fp16 ? sizeof(model_fp16_out_proj_weights[layer]) : sizeof(model_out_proj_weights);
    block_tensors[BLOCK_OUT_BIAS] = fp16
        ? make_plain_tensor(
            "block_out_bias", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_FLOAT_16,
            width_dimensions, 1U)
        : make_axis_tensor(
            "block_out_bias", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_32,
            width_dimensions, 1U, 0, TINY_WIDTH, model_out_proj_bias_quant);
    block_tensors[BLOCK_OUT_BIAS].data.v1.memory.client_buffer.data =
        fp16 ? (void *)model_fp16_out_proj_bias[layer] : (void *)model_out_proj_bias;
    block_tensors[BLOCK_OUT_BIAS].data.v1.memory.client_buffer.data_size =
        fp16 ? sizeof(model_fp16_out_proj_bias[layer]) : sizeof(model_out_proj_bias);
    block_tensors[BLOCK_ATTENTION_OUTPUT] = make_encoder_tensor(
        "block_attention_output", QNN_TENSOR_TYPE_NATIVE, activation_dimensions, 2U,
        fp16, model_block_quant[8]
    );
    block_tensors[BLOCK_ATTENTION_RESIDUAL] = make_encoder_tensor(
        "block_attention_residual", QNN_TENSOR_TYPE_NATIVE, activation_dimensions, 2U,
        fp16, model_block_quant[9]
    );

    block_tensors[BLOCK_LN2_SCALE] = make_encoder_tensor(
        "block_final_norm_scale", QNN_TENSOR_TYPE_STATIC, width_dimensions, 1U,
        fp16, model_final_layer_norm_scale_quant
    );
    block_tensors[BLOCK_LN2_SCALE].data.v1.memory.client_buffer.data =
        fp16 ? (void *)model_fp16_final_layer_norm_scale[layer] :
            (void *)model_final_layer_norm_scale;
    block_tensors[BLOCK_LN2_SCALE].data.v1.memory.client_buffer.data_size =
        fp16 ? sizeof(model_fp16_final_layer_norm_scale[layer]) :
            sizeof(model_final_layer_norm_scale);
    block_tensors[BLOCK_LN2_BIAS] = make_encoder_tensor(
        "block_final_norm_bias", QNN_TENSOR_TYPE_STATIC, width_dimensions, 1U,
        fp16, (QnnScaleOffset){
            model_block_quant[9].scale * model_final_layer_norm_scale_quant.scale, 0
        }
    );
    if (!fp16) block_tensors[BLOCK_LN2_BIAS].data.v1.data_type = QNN_DATATYPE_SFIXED_POINT_32;
    block_tensors[BLOCK_LN2_BIAS].data.v1.memory.client_buffer.data =
        fp16 ? (void *)model_fp16_final_layer_norm_bias[layer] :
            (void *)model_final_layer_norm_bias;
    block_tensors[BLOCK_LN2_BIAS].data.v1.memory.client_buffer.data_size =
        fp16 ? sizeof(model_fp16_final_layer_norm_bias[layer]) :
            sizeof(model_final_layer_norm_bias);
    block_tensors[BLOCK_LN2_OUTPUT] = make_encoder_tensor(
        "block_final_norm_output", QNN_TENSOR_TYPE_NATIVE, activation_dimensions, 2U,
        fp16, model_block_quant[10]
    );
    block_tensors[BLOCK_FC1_WEIGHT] = fp16
        ? make_plain_tensor(
            "block_fc1_weight", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_FLOAT_16,
            fc1_weight_dimensions, 2U)
        : make_axis_tensor(
            "block_fc1_weight", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_8,
            fc1_weight_dimensions, 2U, 0, TINY_MLP_WIDTH, model_fc1_weight_quant);
    block_tensors[BLOCK_FC1_WEIGHT].data.v1.memory.client_buffer.data =
        fp16 ? (void *)model_fp16_fc1_weights[layer] : (void *)model_fc1_weights;
    block_tensors[BLOCK_FC1_WEIGHT].data.v1.memory.client_buffer.data_size =
        fp16 ? sizeof(model_fp16_fc1_weights[layer]) : sizeof(model_fc1_weights);
    block_tensors[BLOCK_FC1_BIAS] = fp16
        ? make_plain_tensor(
            "block_fc1_bias", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_FLOAT_16,
            hidden_bias_dimensions, 1U)
        : make_axis_tensor(
            "block_fc1_bias", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_32,
            hidden_bias_dimensions, 1U, 0, TINY_MLP_WIDTH, model_fc1_bias_quant);
    block_tensors[BLOCK_FC1_BIAS].data.v1.memory.client_buffer.data =
        fp16 ? (void *)model_fp16_fc1_bias[layer] : (void *)model_fc1_bias;
    block_tensors[BLOCK_FC1_BIAS].data.v1.memory.client_buffer.data_size =
        fp16 ? sizeof(model_fp16_fc1_bias[layer]) : sizeof(model_fc1_bias);
    block_tensors[BLOCK_FC1_OUTPUT] = make_encoder_tensor(
        "block_fc1_output", QNN_TENSOR_TYPE_NATIVE, hidden_dimensions, 2U,
        fp16, model_block_quant[11]
    );
    block_tensors[BLOCK_GELU_OUTPUT] = make_encoder_tensor(
        "block_gelu_output", QNN_TENSOR_TYPE_NATIVE, hidden_dimensions, 2U,
        fp16, model_block_quant[12]
    );
    block_tensors[BLOCK_FC2_WEIGHT] = fp16
        ? make_plain_tensor(
            "block_fc2_weight", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_FLOAT_16,
            fc2_weight_dimensions, 2U)
        : make_axis_tensor(
            "block_fc2_weight", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_8,
            fc2_weight_dimensions, 2U, 0, TINY_WIDTH, model_fc2_weight_quant);
    block_tensors[BLOCK_FC2_WEIGHT].data.v1.memory.client_buffer.data =
        fp16 ? (void *)model_fp16_fc2_weights[layer] : (void *)model_fc2_weights;
    block_tensors[BLOCK_FC2_WEIGHT].data.v1.memory.client_buffer.data_size =
        fp16 ? sizeof(model_fp16_fc2_weights[layer]) : sizeof(model_fc2_weights);
    block_tensors[BLOCK_FC2_BIAS] = fp16
        ? make_plain_tensor(
            "block_fc2_bias", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_FLOAT_16,
            width_dimensions, 1U)
        : make_axis_tensor(
            "block_fc2_bias", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_32,
            width_dimensions, 1U, 0, TINY_WIDTH, model_fc2_bias_quant);
    block_tensors[BLOCK_FC2_BIAS].data.v1.memory.client_buffer.data =
        fp16 ? (void *)model_fp16_fc2_bias[layer] : (void *)model_fc2_bias;
    block_tensors[BLOCK_FC2_BIAS].data.v1.memory.client_buffer.data_size =
        fp16 ? sizeof(model_fp16_fc2_bias[layer]) : sizeof(model_fc2_bias);
    block_tensors[BLOCK_FC2_OUTPUT] = make_encoder_tensor(
        "block_fc2_output", QNN_TENSOR_TYPE_NATIVE, activation_dimensions, 2U,
        fp16, model_block_quant[13]
    );
    block_tensors[BLOCK_OUTPUT] = make_encoder_tensor(
        "block_output", monolithic && layer != 3U ? QNN_TENSOR_TYPE_NATIVE :
            QNN_TENSOR_TYPE_APP_READ, activation_dimensions, 2U,
        fp16, model_block_quant[14]
    );

    if (monolithic && layer != 0U) {
        copy_tensor(&block_tensors[BLOCK_INPUT], &model_fp16_monolithic_previous_output);
    }
    for (index = monolithic && layer != 0U ? 1U : 0U; index < BLOCK_TENSOR_COUNT; ++index) {
        registered[registered_count++] = &block_tensors[index];
    }
    for (index = 0U; index < 2U; ++index) {
        layer_norm_parameters[index][0].type = QNN_PARAMTYPE_SCALAR;
        layer_norm_parameters[index][0].name = "epsilon";
        layer_norm_parameters[index][0].value.scalar.data_type = QNN_DATATYPE_FLOAT_32;
        layer_norm_parameters[index][0].value.scalar.value.float_value = 0.00001f;
        layer_norm_parameters[index][1].type = QNN_PARAMTYPE_TENSOR;
        layer_norm_parameters[index][1].name = "axes";
        layer_norm_parameters[index][1].value.tensor = make_plain_tensor(
            index == 0U ? "block_attention_norm_axes" : "block_final_norm_axes",
            QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_UINT_32, vector_dimensions, 1U
        );
        layer_norm_parameters[index][1].value.tensor.data.v1.memory.client_buffer.data = axes_data;
        layer_norm_parameters[index][1].value.tensor.data.v1.memory.client_buffer.data_size =
            sizeof(axes_data);
        registered[registered_count++] = &layer_norm_parameters[index][1].value.tensor;
    }
    transpose_parameters[0].type = QNN_PARAMTYPE_TENSOR;
    transpose_parameters[0].name = "perm";
    transpose_parameters[0].value.tensor = make_plain_tensor(
        "block_head_perm", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_UINT_32,
        perm_dimensions, 1U
    );
    transpose_parameters[0].value.tensor.data.v1.memory.client_buffer.data = head_perm_data;
    transpose_parameters[0].value.tensor.data.v1.memory.client_buffer.data_size =
        sizeof(head_perm_data);
    registered[registered_count++] = &transpose_parameters[0].value.tensor;
    transpose_parameters[1].type = QNN_PARAMTYPE_TENSOR;
    transpose_parameters[1].name = "perm";
    transpose_parameters[1].value.tensor = make_plain_tensor(
        "block_key_perm", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_UINT_32,
        perm_dimensions, 1U
    );
    transpose_parameters[1].value.tensor.data.v1.memory.client_buffer.data = key_perm_data;
    transpose_parameters[1].value.tensor.data.v1.memory.client_buffer.data_size =
        sizeof(key_perm_data);
    registered[registered_count++] = &transpose_parameters[1].value.tensor;

    if (monolithic) {
        for (index = 0U; index < registered_count; ++index) {
            make_layer_name(
                monolithic_tensor_names[layer][index], 64U, layer,
                registered[index]->data.v1.name
            );
            registered[index]->data.v1.name = monolithic_tensor_names[layer][index];
        }
        model_monolithic_name_layer = (i32)layer;
        model_monolithic_node_index = 0U;
    }

    for (index = 0U; index < registered_count; ++index) {
        status = api->tensor_create_graph_tensor(graph, registered[index]);
        if (status != 0U) {
            write_call_status("  tensorCreate", status);
            write_text("  tensor index: ");
            write_u32(index);
            write_text("\n");
            return 76U;
        }
    }
    write_text("  tensorCreate: complete encoder block registered\n");

#define ADD_BLOCK_NODE(name, type, input0, input1, input2, count, result, params, param_count) \
    status = add_graph_node(api, graph, name, type, input0, input1, input2, count, result, params, param_count); \
    write_call_status("  graphAddNode " name, status); \
    if (status != 0U) return 77U
    ADD_BLOCK_NODE(
        "attention LayerNorm", "LayerNorm", &block_tensors[BLOCK_INPUT],
        &block_tensors[BLOCK_LN1_SCALE], &block_tensors[BLOCK_LN1_BIAS], 3U,
        &block_tensors[BLOCK_LN1_OUTPUT], layer_norm_parameters[0], 2U
    );
    for (projection = 0U; projection < ATTENTION_PROJECTION_COUNT; ++projection) {
        status = add_graph_node(
            api, graph, projection_names[projection], "FullyConnected",
            &block_tensors[BLOCK_LN1_OUTPUT], &BLOCK_PROJECTION_WEIGHT(projection),
            &BLOCK_PROJECTION_BIAS(projection), 3U, &BLOCK_PROJECTION_OUTPUT(projection), 0, 0U
        );
        write_call_status("  graphAddNode Q/K/V FullyConnected", status);
        if (status != 0U) return 77U;
        status = add_graph_node(
            api, graph, reshape_names[projection], "Reshape", &BLOCK_PROJECTION_OUTPUT(projection),
            0, 0, 1U, &BLOCK_PROJECTION_SPLIT(projection), 0, 0U
        );
        write_call_status("  graphAddNode Q/K/V Reshape", status);
        if (status != 0U) return 77U;
        status = add_graph_node(
            api, graph, transpose_names[projection], "Transpose", &BLOCK_PROJECTION_SPLIT(projection),
            0, 0, 1U, &BLOCK_PROJECTION_HEADS(projection),
            &transpose_parameters[projection == 1U ? 1U : 0U], 1U
        );
        write_call_status("  graphAddNode Q/K/V Transpose", status);
        if (status != 0U) return 77U;
    }
    ADD_BLOCK_NODE(
        "attention scores", "MatMul", &BLOCK_PROJECTION_HEADS(0),
        &BLOCK_PROJECTION_HEADS(1), 0, 2U, &block_tensors[BLOCK_SCORES], 0, 0U
    );
    ADD_BLOCK_NODE(
        "attention Softmax", "Softmax", &block_tensors[BLOCK_SCORES], 0, 0, 1U,
        &block_tensors[BLOCK_PROBABILITIES], 0, 0U
    );
    ADD_BLOCK_NODE(
        "attention values", "MatMul", &block_tensors[BLOCK_PROBABILITIES],
        &BLOCK_PROJECTION_HEADS(2), 0, 2U, &block_tensors[BLOCK_ATTENDED_HEADS], 0, 0U
    );
    ADD_BLOCK_NODE(
        "attention inverse Transpose", "Transpose", &block_tensors[BLOCK_ATTENDED_HEADS],
        0, 0, 1U, &block_tensors[BLOCK_ATTENDED_SPLIT], &transpose_parameters[0], 1U
    );
    ADD_BLOCK_NODE(
        "attention flatten Reshape", "Reshape", &block_tensors[BLOCK_ATTENDED_SPLIT],
        0, 0, 1U, &block_tensors[BLOCK_ATTENDED_FLAT], 0, 0U
    );
    ADD_BLOCK_NODE(
        "attention output FullyConnected", "FullyConnected", &block_tensors[BLOCK_ATTENDED_FLAT],
        &block_tensors[BLOCK_OUT_WEIGHT], &block_tensors[BLOCK_OUT_BIAS], 3U,
        &block_tensors[BLOCK_ATTENTION_OUTPUT], 0, 0U
    );
    ADD_BLOCK_NODE(
        "attention residual Add", "ElementWiseAdd", &block_tensors[BLOCK_INPUT],
        &block_tensors[BLOCK_ATTENTION_OUTPUT], 0, 2U,
        &block_tensors[BLOCK_ATTENTION_RESIDUAL], 0, 0U
    );
    ADD_BLOCK_NODE(
        "final LayerNorm", "LayerNorm", &block_tensors[BLOCK_ATTENTION_RESIDUAL],
        &block_tensors[BLOCK_LN2_SCALE], &block_tensors[BLOCK_LN2_BIAS], 3U,
        &block_tensors[BLOCK_LN2_OUTPUT], layer_norm_parameters[1], 2U
    );
    ADD_BLOCK_NODE(
        "MLP fc1", "FullyConnected", &block_tensors[BLOCK_LN2_OUTPUT],
        &block_tensors[BLOCK_FC1_WEIGHT], &block_tensors[BLOCK_FC1_BIAS], 3U,
        &block_tensors[BLOCK_FC1_OUTPUT], 0, 0U
    );
    ADD_BLOCK_NODE(
        "MLP Gelu", "Gelu", &block_tensors[BLOCK_FC1_OUTPUT], 0, 0, 1U,
        &block_tensors[BLOCK_GELU_OUTPUT], 0, 0U
    );
    ADD_BLOCK_NODE(
        "MLP fc2", "FullyConnected", &block_tensors[BLOCK_GELU_OUTPUT],
        &block_tensors[BLOCK_FC2_WEIGHT], &block_tensors[BLOCK_FC2_BIAS], 3U,
        &block_tensors[BLOCK_FC2_OUTPUT], 0, 0U
    );
    ADD_BLOCK_NODE(
        "MLP residual Add", "ElementWiseAdd", &block_tensors[BLOCK_ATTENTION_RESIDUAL],
        &block_tensors[BLOCK_FC2_OUTPUT], 0, 2U, &block_tensors[BLOCK_OUTPUT], 0, 0U
    );
#undef ADD_BLOCK_NODE

    model_monolithic_name_layer = -1;
    if (monolithic) {
        if (layer == 0U) {
            copy_tensor(&model_fp16_monolithic_input, &block_tensors[BLOCK_INPUT]);
            model_fp16_monolithic_input.data.v1.dimensions = model_encoder_activation_dimensions;
        }
        copy_tensor(&model_fp16_monolithic_previous_output, &block_tensors[BLOCK_OUTPUT]);
        model_fp16_monolithic_previous_output.data.v1.dimensions =
            model_encoder_activation_dimensions;
        if (layer != 3U) return 0U;
        copy_tensor(&model_fp16_monolithic_output, &block_tensors[BLOCK_OUTPUT]);
        model_fp16_monolithic_output.data.v1.dimensions = model_encoder_activation_dimensions;
    }

    QueryPerformanceCounter(&start_counter);
    status = api->graph_finalize(graph, 0, 0);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphFinalize", status);
    write_duration_us("  graphFinalize time", (u64)(end_counter - start_counter), frequency);
    if (status != 0U) return 78U;
    if (monolithic) {
        copy_tensor(&block_tensors[BLOCK_INPUT], &model_fp16_monolithic_input);
        copy_tensor(&block_tensors[BLOCK_OUTPUT], &model_fp16_monolithic_output);
    }
    block_tensors[BLOCK_INPUT].data.v1.memory.client_buffer.data =
        monolithic ? (void *)model_fp16_encoder_stack_input :
            (fp16 ? (void *)model_fp16_block_input : (void *)model_block_input);
    block_tensors[BLOCK_INPUT].data.v1.memory.client_buffer.data_size =
        fp16 ? sizeof(model_fp16_block_input) : sizeof(model_block_input);
    block_tensors[BLOCK_OUTPUT].data.v1.memory.client_buffer.data =
        fp16 ? (void *)model_fp16_block_output : (void *)model_attention_core_output;
    block_tensors[BLOCK_OUTPUT].data.v1.memory.client_buffer.data_size =
        fp16 ? sizeof(model_fp16_block_output) : sizeof(model_attention_core_output);
    if (!fp16) {
        model_encoder_graphs[layer] = graph;
        copy_tensor(&model_encoder_inputs[layer], &block_tensors[BLOCK_INPUT]);
        copy_tensor(&model_encoder_outputs[layer], &block_tensors[BLOCK_OUTPUT]);
        model_encoder_inputs[layer].data.v1.dimensions = model_encoder_activation_dimensions;
        model_encoder_outputs[layer].data.v1.dimensions = model_encoder_activation_dimensions;
    } else {
        model_fp16_encoder_graphs[layer] = graph;
        copy_tensor(&model_fp16_encoder_inputs[layer], &block_tensors[BLOCK_INPUT]);
        copy_tensor(&model_fp16_encoder_outputs[layer], &block_tensors[BLOCK_OUTPUT]);
        model_fp16_encoder_inputs[layer].data.v1.dimensions = model_encoder_activation_dimensions;
        model_fp16_encoder_outputs[layer].data.v1.dimensions = model_encoder_activation_dimensions;
    }
    QueryPerformanceCounter(&start_counter);
    status = api->graph_execute(
        graph, &block_tensors[BLOCK_INPUT], 1U, &block_tensors[BLOCK_OUTPUT], 1U, 0, 0
    );
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphExecute", status);
    write_duration_us("  first execution", (u64)(end_counter - start_counter), frequency);
    if (status != 0U) return 79U;
    if (fp16) {
        if (!validate_fp16_encoder_output(model_fp16_block_output, model_fp16_block_expected)) {
            return 80U;
        }
    } else if (!validate_encoder_output(
            model_attention_core_output, model_block_output, 20U,
            TINY_SEQUENCE_LENGTH * TINY_WIDTH / 40U)) {
        return 80U;
    }
    for (index = 0U; index < BENCHMARK_WARMUPS; ++index) {
        status = api->graph_execute(
            graph, &block_tensors[BLOCK_INPUT], 1U, &block_tensors[BLOCK_OUTPUT], 1U, 0, 0
        );
        if (status != 0U) return 79U;
    }
    for (index = 0U; index < BENCHMARK_SAMPLES; ++index) {
        QueryPerformanceCounter(&start_counter);
        status = api->graph_execute(
            graph, &block_tensors[BLOCK_INPUT], 1U, &block_tensors[BLOCK_OUTPUT], 1U, 0, 0
        );
        QueryPerformanceCounter(&end_counter);
        if (status != 0U) return 79U;
        samples[index] = (u64)(end_counter - start_counter);
        sample_total += samples[index];
    }
    if (fp16) {
        if (!validate_fp16_encoder_output(model_fp16_block_output, model_fp16_block_expected)) {
            return 80U;
        }
    } else if (!validate_encoder_output(
            model_attention_core_output, model_block_output, 20U,
            TINY_SEQUENCE_LENGTH * TINY_WIDTH / 40U)) {
        return 80U;
    }
    sort_u64(samples, BENCHMARK_SAMPLES);
    write_text(fp16 ? "QNN FP16 Whisper Tiny encoder layer " :
        "QNN complete Whisper Tiny encoder layer ");
    write_u32(layer);
    write_text(" latency (10 warmups, 100 samples)\n");
    write_duration_us("  min", samples[0], frequency);
    write_duration_us("  median", samples[50], frequency);
    write_duration_us("  p95", samples[94], frequency);
    write_duration_us("  max", samples[99], frequency);
    write_duration_us("  mean", sample_total / BENCHMARK_SAMPLES, frequency);
    write_gmac_per_second(
        "  median throughput",
        4ULL * TINY_SEQUENCE_LENGTH * TINY_WIDTH * TINY_WIDTH +
            2ULL * TINY_ATTENTION_HEADS * TINY_SEQUENCE_LENGTH *
                TINY_SEQUENCE_LENGTH * TINY_ATTENTION_HEAD_WIDTH +
            2ULL * TINY_SEQUENCE_LENGTH * TINY_WIDTH * TINY_MLP_WIDTH,
        samples[50],
        frequency
    );
#undef BLOCK_PROJECTION_WEIGHT
#undef BLOCK_PROJECTION_BIAS
#undef BLOCK_PROJECTION_OUTPUT
#undef BLOCK_PROJECTION_SPLIT
#undef BLOCK_PROJECTION_HEADS
    return 0U;
}

static u32 run_model_encoder_stack(const QnnInterfaceV2 *api, u64 frequency) {
    static const char *expected_primary[4] = {
        "experimental/snapdragon/models/whisper-tiny/encoder-stack-int8/fixture-layer-0-output-uint8.bin",
        "experimental/snapdragon/models/whisper-tiny/encoder-stack-int8/fixture-layer-1-output-uint8.bin",
        "experimental/snapdragon/models/whisper-tiny/encoder-stack-int8/fixture-layer-2-output-uint8.bin",
        "experimental/snapdragon/models/whisper-tiny/encoder-stack-int8/fixture-layer-3-output-uint8.bin"
    };
    static const char *expected_fallback[4] = {
        "../models/whisper-tiny/encoder-stack-int8/fixture-layer-0-output-uint8.bin",
        "../models/whisper-tiny/encoder-stack-int8/fixture-layer-1-output-uint8.bin",
        "../models/whisper-tiny/encoder-stack-int8/fixture-layer-2-output-uint8.bin",
        "../models/whisper-tiny/encoder-stack-int8/fixture-layer-3-output-uint8.bin"
    };
    static u64 samples[BENCHMARK_SAMPLES];
    u64 sample_total = 0U;
    long long start_counter;
    long long end_counter;
    u64 status;
    u32 iteration;
    u32 layer;
    int loaded;

    for (layer = 0U; layer < 4U; ++layer) {
        if (model_encoder_graphs[layer] == 0) {
            write_text("QNN complete Whisper Tiny encoder stack: skipped (layer artifacts unavailable)\n");
            return 0U;
        }
    }
    loaded = read_exact_file(
        "experimental/snapdragon/models/whisper-tiny/encoder-stack-int8/fixture-input-uint8.bin",
        "../models/whisper-tiny/encoder-stack-int8/fixture-input-uint8.bin",
        model_encoder_stack_input,
        sizeof(model_encoder_stack_input)
    );
    if (loaded == 0) {
        write_text("QNN complete Whisper Tiny encoder stack: skipped (stack fixture unavailable)\n");
        return 0U;
    }
    if (loaded < 0) {
        write_text("QNN complete Whisper Tiny encoder stack: invalid stack fixture\n");
        return 81U;
    }
    write_text("QNN complete model-derived Whisper Tiny four-layer encoder stack\n");
    QueryPerformanceCounter(&start_counter);
    for (layer = 0U; layer < 4U; ++layer) {
        u8 *input_data = layer == 0U
            ? model_encoder_stack_input
            : model_encoder_stack_buffers[(layer - 1U) & 1U];
        u8 *output_data = model_encoder_stack_buffers[layer & 1U];
        model_encoder_inputs[layer].data.v1.memory.client_buffer.data = input_data;
        model_encoder_inputs[layer].data.v1.memory.client_buffer.data_size =
            sizeof(model_encoder_stack_input);
        model_encoder_outputs[layer].data.v1.memory.client_buffer.data = output_data;
        model_encoder_outputs[layer].data.v1.memory.client_buffer.data_size =
            sizeof(model_encoder_stack_input);
        status = api->graph_execute(
            model_encoder_graphs[layer], &model_encoder_inputs[layer], 1U,
            &model_encoder_outputs[layer], 1U, 0, 0
        );
        if (status != 0U) {
            write_call_status("  graphExecute", status);
            return 82U;
        }
        loaded = read_exact_file(
            expected_primary[layer], expected_fallback[layer],
            model_encoder_stack_expected, sizeof(model_encoder_stack_expected)
        );
        if (loaded != 1) {
            write_text("  missing or invalid cascaded layer fixture\n");
            return 81U;
        }
        write_text("  cumulative layer ");
        write_u32(layer);
        write_text(" comparison\n");
        (void)validate_encoder_output(
            output_data, model_encoder_stack_expected, 255U,
            TINY_SEQUENCE_LENGTH * TINY_WIDTH
        );
    }
    QueryPerformanceCounter(&end_counter);
    write_duration_us(
        "  first chained execution", (u64)(end_counter - start_counter), frequency
    );

    for (iteration = 0U; iteration < BENCHMARK_WARMUPS; ++iteration) {
        for (layer = 0U; layer < 4U; ++layer) {
            status = api->graph_execute(
                model_encoder_graphs[layer], &model_encoder_inputs[layer], 1U,
                &model_encoder_outputs[layer], 1U, 0, 0
            );
            if (status != 0U) return 82U;
        }
    }
    for (iteration = 0U; iteration < BENCHMARK_SAMPLES; ++iteration) {
        QueryPerformanceCounter(&start_counter);
        for (layer = 0U; layer < 4U; ++layer) {
            status = api->graph_execute(
                model_encoder_graphs[layer], &model_encoder_inputs[layer], 1U,
                &model_encoder_outputs[layer], 1U, 0, 0
            );
            if (status != 0U) return 82U;
        }
        QueryPerformanceCounter(&end_counter);
        samples[iteration] = (u64)(end_counter - start_counter);
        sample_total += samples[iteration];
    }
        if (!validate_encoder_output(
                model_encoder_stack_buffers[1], model_encoder_stack_expected, 70U,
                TINY_SEQUENCE_LENGTH * TINY_WIDTH / 50U)) return 83U;
    sort_u64(samples, BENCHMARK_SAMPLES);
    write_text("QNN complete Whisper Tiny four-layer encoder latency (10 warmups, 100 samples)\n");
    write_duration_us("  min", samples[0], frequency);
    write_duration_us("  median", samples[50], frequency);
    write_duration_us("  p95", samples[94], frequency);
    write_duration_us("  max", samples[99], frequency);
    write_duration_us("  mean", sample_total / BENCHMARK_SAMPLES, frequency);
    write_gmac_per_second(
        "  median throughput",
        4ULL * (
            4ULL * TINY_SEQUENCE_LENGTH * TINY_WIDTH * TINY_WIDTH +
            2ULL * TINY_ATTENTION_HEADS * TINY_SEQUENCE_LENGTH *
                TINY_SEQUENCE_LENGTH * TINY_ATTENTION_HEAD_WIDTH +
            2ULL * TINY_SEQUENCE_LENGTH * TINY_WIDTH * TINY_MLP_WIDTH),
        samples[50],
        frequency
    );
    return 0U;
}

static u32 run_model_fp16_encoder_stack(const QnnInterfaceV2 *api, u64 frequency) {
    static u64 samples[BENCHMARK_SAMPLES];
    u64 sample_total = 0U;
    long long start_counter;
    long long end_counter;
    u64 status;
    u32 iteration;
    u32 layer;

    for (layer = 0U; layer < 4U; ++layer) {
        if (model_fp16_encoder_graphs[layer] == 0) {
            write_text("QNN FP16 Whisper Tiny encoder stack: skipped (layer artifacts unavailable)\n");
            return 0U;
        }
    }
    write_text("QNN FP16 model-derived Whisper Tiny four-layer encoder stack\n");
    QueryPerformanceCounter(&start_counter);
    for (layer = 0U; layer < 4U; ++layer) {
        u16 *input_data = layer == 0U
            ? model_fp16_encoder_stack_input
            : model_fp16_encoder_stack_buffers[(layer - 1U) & 1U];
        u16 *output_data = model_fp16_encoder_stack_buffers[layer & 1U];
        int loaded;
        model_fp16_encoder_inputs[layer].data.v1.memory.client_buffer.data = input_data;
        model_fp16_encoder_inputs[layer].data.v1.memory.client_buffer.data_size =
            sizeof(model_fp16_encoder_stack_input);
        model_fp16_encoder_outputs[layer].data.v1.memory.client_buffer.data = output_data;
        model_fp16_encoder_outputs[layer].data.v1.memory.client_buffer.data_size =
            sizeof(model_fp16_encoder_stack_input);
        status = api->graph_execute(
            model_fp16_encoder_graphs[layer], &model_fp16_encoder_inputs[layer], 1U,
            &model_fp16_encoder_outputs[layer], 1U, 0, 0
        );
        if (status != 0U) {
            write_call_status("  graphExecute", status);
            return 102U;
        }
        loaded = read_model_fp16_layer_file(
            layer, "fixture-output-fp16.bin", model_fp16_block_expected,
            sizeof(model_fp16_block_expected)
        );
        if (loaded != 1) {
            write_text("  missing or invalid FP16 layer fixture\n");
            return 103U;
        }
        write_text("  cumulative FP16 layer ");
        write_u32(layer);
        write_text(" comparison\n");
        if (!validate_fp16_encoder_output(output_data, model_fp16_block_expected)) return 104U;
    }
    QueryPerformanceCounter(&end_counter);
    write_duration_us(
        "  first chained execution", (u64)(end_counter - start_counter), frequency
    );

    for (iteration = 0U; iteration < BENCHMARK_WARMUPS; ++iteration) {
        for (layer = 0U; layer < 4U; ++layer) {
            status = api->graph_execute(
                model_fp16_encoder_graphs[layer], &model_fp16_encoder_inputs[layer], 1U,
                &model_fp16_encoder_outputs[layer], 1U, 0, 0
            );
            if (status != 0U) return 102U;
        }
    }
    for (iteration = 0U; iteration < BENCHMARK_SAMPLES; ++iteration) {
        QueryPerformanceCounter(&start_counter);
        for (layer = 0U; layer < 4U; ++layer) {
            status = api->graph_execute(
                model_fp16_encoder_graphs[layer], &model_fp16_encoder_inputs[layer], 1U,
                &model_fp16_encoder_outputs[layer], 1U, 0, 0
            );
            if (status != 0U) return 102U;
        }
        QueryPerformanceCounter(&end_counter);
        samples[iteration] = (u64)(end_counter - start_counter);
        sample_total += samples[iteration];
    }
    if (!validate_fp16_encoder_output(
            model_fp16_encoder_stack_buffers[1], model_fp16_block_expected)) return 104U;
    sort_u64(samples, BENCHMARK_SAMPLES);
    write_text("QNN FP16 Whisper Tiny four-layer encoder latency (10 warmups, 100 samples)\n");
    write_duration_us("  min", samples[0], frequency);
    write_duration_us("  median", samples[50], frequency);
    write_duration_us("  p95", samples[94], frequency);
    write_duration_us("  max", samples[99], frequency);
    write_duration_us("  mean", sample_total / BENCHMARK_SAMPLES, frequency);
    write_gmac_per_second(
        "  median throughput",
        4ULL * (
            4ULL * TINY_SEQUENCE_LENGTH * TINY_WIDTH * TINY_WIDTH +
            2ULL * TINY_ATTENTION_HEADS * TINY_SEQUENCE_LENGTH *
                TINY_SEQUENCE_LENGTH * TINY_ATTENTION_HEAD_WIDTH +
            2ULL * TINY_SEQUENCE_LENGTH * TINY_WIDTH * TINY_MLP_WIDTH),
        samples[50],
        frequency
    );
    return 0U;
}

static u32 run_tiny_projection(
    const QnnInterfaceV2 *api,
    QnnContextHandle context,
    u64 frequency,
    u32 sequence_length,
    u32 inner_size,
    u32 output_size,
    const char *graph_name
) {
    QnnGraphHandle graph = 0;
    u32 activation_dimensions[2] = {sequence_length, inner_size};
    u32 weight_dimensions[2] = {inner_size, output_size};
    u32 output_dimensions[2] = {sequence_length, output_size};
    QnnTensor inputs[2];
    QnnTensor outputs[1];
    QnnOpConfig operation = {0};
    u64 samples[BENCHMARK_SAMPLES];
    u64 sample_total = 0U;
    long long start_counter;
    long long end_counter;
    u64 status;
    u32 index;

    initialize_tiny_projection(sequence_length, inner_size, output_size);
    reference_tiny_projection(sequence_length, inner_size, output_size);
    write_tiny_shape(sequence_length, inner_size, output_size);

    QueryPerformanceCounter(&start_counter);
    status = api->graph_create(context, graph_name, 0, &graph);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphCreate", status);
    write_duration_us("  graphCreate time", (u64)(end_counter - start_counter), frequency);
    if (status != 0U) return 26U;

    inputs[0] = make_tensor("tiny_activation", QNN_TENSOR_TYPE_APP_WRITE, activation_dimensions, 2U);
    inputs[1] = make_tensor("tiny_weights", QNN_TENSOR_TYPE_STATIC, weight_dimensions, 2U);
    outputs[0] = make_tensor("tiny_output", QNN_TENSOR_TYPE_APP_READ, output_dimensions, 2U);
    inputs[1].data.v1.memory.client_buffer.data = tiny_weights;
    inputs[1].data.v1.memory.client_buffer.data_size = inner_size * output_size;

    status = api->tensor_create_graph_tensor(graph, &inputs[0]);
    write_call_status("  tensorCreate activation", status);
    if (status != 0U) return 27U;
    status = api->tensor_create_graph_tensor(graph, &inputs[1]);
    write_call_status("  tensorCreate weights", status);
    if (status != 0U) return 28U;
    status = api->tensor_create_graph_tensor(graph, &outputs[0]);
    write_call_status("  tensorCreate output", status);
    if (status != 0U) return 29U;

    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = "tiny_projection";
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = "MatMul";
    operation.data.v1.input_count = 2U;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = outputs;
    status = api->graph_add_node(graph, operation);
    write_call_status("  graphAddNode", status);
    if (status != 0U) return 30U;

    QueryPerformanceCounter(&start_counter);
    status = api->graph_finalize(graph, 0, 0);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphFinalize", status);
    write_duration_us("  graphFinalize time", (u64)(end_counter - start_counter), frequency);
    if (status != 0U) return 31U;

    inputs[0].data.v1.memory.client_buffer.data = tiny_input;
    inputs[0].data.v1.memory.client_buffer.data_size = sequence_length * inner_size;
    outputs[0].data.v1.memory.client_buffer.data = tiny_output;
    outputs[0].data.v1.memory.client_buffer.data_size = sequence_length * output_size;
    QueryPerformanceCounter(&start_counter);
    status = api->graph_execute(graph, inputs, 1U, outputs, 1U, 0, 0);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphExecute", status);
    if (status != 0U) return 32U;
    write_duration_us("  first execution", (u64)(end_counter - start_counter), frequency);
    if (!validate_output(tiny_output, tiny_expected, sequence_length * output_size)) return 33U;
    write_text("  output matches scalar UINT8 GEMM reference\n");

    for (index = 0U; index < BENCHMARK_WARMUPS; ++index) {
        status = api->graph_execute(graph, inputs, 1U, outputs, 1U, 0, 0);
        if (status != 0U) {
            write_call_status("  warmup graphExecute", status);
            return 32U;
        }
    }
    for (index = 0U; index < BENCHMARK_SAMPLES; ++index) {
        QueryPerformanceCounter(&start_counter);
        status = api->graph_execute(graph, inputs, 1U, outputs, 1U, 0, 0);
        QueryPerformanceCounter(&end_counter);
        if (status != 0U) {
            write_call_status("  measured graphExecute", status);
            return 32U;
        }
        samples[index] = (u64)(end_counter - start_counter);
        sample_total += samples[index];
    }
    if (!validate_output(tiny_output, tiny_expected, sequence_length * output_size)) return 33U;
    sort_u64(samples, BENCHMARK_SAMPLES);
    write_text("QNN Tiny projection latency (10 warmups, 100 samples)\n");
    write_duration_us("  min", samples[0], frequency);
    write_duration_us("  median", samples[50], frequency);
    write_duration_us("  p95", samples[94], frequency);
    write_duration_us("  max", samples[99], frequency);
    write_duration_us("  mean", sample_total / BENCHMARK_SAMPLES, frequency);
    write_gmac_per_second(
        "  median throughput",
        (u64)sequence_length * inner_size * output_size,
        samples[50],
        frequency
    );
    return 0U;
}

static u32 write_model_context_cache(const QnnInterfaceV2 *api, QnnContextHandle context) {
    void *invalid_handle = (void *)(usize)-1;
    const WhisperModelConfig *model = active_model;
    static ModelContextCacheMetadata metadata;
    WhisperArtifactHeader artifact = {0};
    u8 artifact_bytes[WHISPER_ARTIFACT_HEADER_SIZE];
    u8 metadata_bytes[MODEL_CONTEXT_CACHE_METADATA_SIZE];
    void *buffer;
    void *handle;
    u64 written_size = 0U;
    u64 status;

    metadata.binary_size = 0U;

    status = api->context_get_binary_size(context, &metadata.binary_size);
    write_call_status("  contextGetBinarySize", status);
    if (status != 0U || metadata.binary_size == 0U) return 81U;
    buffer = VirtualAlloc(0, (usize)metadata.binary_size, 0x3000U, 0x04U);
    if (buffer == 0) return 82U;
    status = api->context_get_binary(context, buffer, metadata.binary_size, &written_size);
    write_call_status("  contextGetBinary", status);
    if (status != 0U || written_size != metadata.binary_size) {
        VirtualFree(buffer, 0U, 0x8000U);
        return 83U;
    }
    metadata.encoder = encoder_qnn_ids;
    metadata.decoder = decoder_qnn_ids;
    encode_model_context_metadata(metadata_bytes, &metadata);
    artifact.model_id = model->model_id;
    artifact.payload_type = WHISPER_ARTIFACT_PAYLOAD_QNN_CONTEXT;
    artifact.element_type = WHISPER_ARTIFACT_ELEMENT_BLOB;
    artifact.element_count = MODEL_CONTEXT_CACHE_METADATA_SIZE + metadata.binary_size;
    artifact.payload_size = artifact.element_count;
    artifact.payload_hash = whisper_artifact_hash_update(
        WHISPER_ARTIFACT_HASH_OFFSET_BASIS,
        metadata_bytes, sizeof(metadata_bytes)
    );
    artifact.payload_hash = whisper_artifact_hash_update(
        artifact.payload_hash, buffer, metadata.binary_size
    );
    artifact.width = model->width;
    artifact.ffn_width = model->ffn_width;
    artifact.attention_heads = model->attention_heads;
    artifact.encoder_layers = model->encoder_layers;
    artifact.decoder_layers = model->decoder_layers;
    artifact.vocabulary_size = model->vocabulary_size;
    artifact.text_context = model->text_context;
    artifact.mel_bins = model->mel_bins;
    artifact.encoder_frames = model->encoder_frames;
    whisper_artifact_encode_header(artifact_bytes, &artifact);
    handle = CreateFileA(
        model_context_cache_primary, 0x40000000U, 0U, 0, 2U, 0x80U, 0
    );
    if (handle == invalid_handle) {
        handle = CreateFileA(
            model_context_cache_local, 0x40000000U, 0U, 0, 2U, 0x80U, 0
        );
    }
    if (handle == invalid_handle ||
        !write_handle_exact(handle, artifact_bytes, sizeof(artifact_bytes)) ||
        !write_handle_exact(handle, metadata_bytes, sizeof(metadata_bytes)) ||
        !write_handle_exact(handle, buffer, metadata.binary_size)) {
        if (handle != invalid_handle) CloseHandle(handle);
        VirtualFree(buffer, 0U, 0x8000U);
        return 84U;
    }
    CloseHandle(handle);
    VirtualFree(buffer, 0U, 0x8000U);
    write_text("  cached context binary bytes: ");
    write_u64(metadata.binary_size);
    write_text("\n");
    return 0U;
}

static u32 load_model_context_cache(
    const QnnInterfaceV2 *api,
    QnnBackendHandle backend,
    QnnDeviceHandle device,
    QnnProfileHandle profile,
    QnnContextHandle *context,
    u64 frequency,
    int *loaded
) {
    void *invalid_handle = (void *)(usize)-1;
    const WhisperModelConfig *model = active_model;
    static ModelContextCacheMetadata metadata;
    WhisperArtifactHeader artifact;
    u8 artifact_bytes[WHISPER_ARTIFACT_HEADER_SIZE];
    u8 metadata_bytes[MODEL_CONTEXT_CACHE_METADATA_SIZE];
    u64 expected_payload_size;
    u64 payload_hash;
    void *buffer;
    void *handle = CreateFileA(
        model_context_cache_primary, 0x80000000U, 1U, 0, 3U, 0x80U, 0
    );
    long long start_counter;
    long long end_counter;
    u64 status;

    *loaded = 0;
    if (handle == invalid_handle) {
        handle = CreateFileA(
            model_context_cache_local, 0x80000000U, 1U, 0, 3U, 0x80U, 0
        );
    }
    if (handle == invalid_handle) return 0U;
    if (!read_handle_exact(handle, artifact_bytes, sizeof(artifact_bytes)) ||
        !whisper_artifact_decode_header(artifact_bytes, &artifact) ||
        artifact.payload_size < MODEL_CONTEXT_CACHE_METADATA_SIZE ||
        artifact.payload_size > 0x80000000ULL ||
        !whisper_artifact_header_valid(
            &artifact, model, WHISPER_ARTIFACT_PAYLOAD_QNN_CONTEXT,
            WHISPER_ARTIFACT_ELEMENT_BLOB, artifact.payload_size,
            artifact.payload_size
        ) || !read_handle_exact(handle, metadata_bytes, sizeof(metadata_bytes))) {
        CloseHandle(handle);
        return 0U;
    }
    decode_model_context_metadata(metadata_bytes, &metadata);
    if (!whisper_model_size_add(
            MODEL_CONTEXT_CACHE_METADATA_SIZE, metadata.binary_size,
            &expected_payload_size
        ) || expected_payload_size != artifact.payload_size ||
        metadata.encoder.model_id != model->model_id ||
        metadata.decoder.model_id != model->model_id ||
        metadata.decoder.output_count != model->decoder_layers * 2U ||
        metadata.decoder.cross_layer_count != model->decoder_layers ||
        metadata.binary_size == 0U || metadata.binary_size > 0x80000000ULL) {
        CloseHandle(handle);
        return 0U;
    }
    buffer = VirtualAlloc(0, (usize)metadata.binary_size, 0x3000U, 0x04U);
    if (buffer == 0 || !read_handle_exact(handle, buffer, metadata.binary_size)) {
        CloseHandle(handle);
        if (buffer != 0) VirtualFree(buffer, 0U, 0x8000U);
        return 85U;
    }
    CloseHandle(handle);
    payload_hash = whisper_artifact_hash_update(
        WHISPER_ARTIFACT_HASH_OFFSET_BASIS,
        metadata_bytes, sizeof(metadata_bytes)
    );
    payload_hash = whisper_artifact_hash_update(
        payload_hash, buffer, metadata.binary_size
    );
    if (payload_hash != artifact.payload_hash) {
        VirtualFree(buffer, 0U, 0x8000U);
        return 0U;
    }
    status = api->context_free(*context, profile);
    write_call_status("  contextFree before cache load", status);
    *context = 0;
    if (status != 0U) {
        VirtualFree(buffer, 0U, 0x8000U);
        return 86U;
    }
    QueryPerformanceCounter(&start_counter);
    status = api->context_create_from_binary(
        backend, device, 0, buffer, metadata.binary_size, context, profile
    );
    QueryPerformanceCounter(&end_counter);
    VirtualFree(buffer, 0U, 0x8000U);
    write_call_status("  contextCreateFromBinary", status);
    write_duration_us(
        "  contextCreateFromBinary time", (u64)(end_counter - start_counter), frequency
    );
    if (status != 0U) return 87U;
    if (!whisper_encoder_qnn_restore(
            whisper_encoder_qnn, api, *context, &metadata.encoder
        )) return 88U;
    if (!whisper_decoder_qnn_restore(
            whisper_decoder_qnn, api, *context, &metadata.decoder
        )) return 88U;
    *loaded = 1;
    return 0U;
}

static u32 run_cached_model_frontend(
    const QnnInterfaceV2 *api,
    u64 frequency
) {
    long long start_counter;
    long long end_counter;
    u64 status;
    QueryPerformanceCounter(&start_counter);
    status = whisper_encoder_qnn_execute_frontend(
        whisper_encoder_qnn, api, frontend_log_mel
    );
    QueryPerformanceCounter(&end_counter);
    write_call_status("  cached frontend graphExecute", status);
    write_duration_us(
        "  cached frontend NPU time", (u64)(end_counter - start_counter), frequency
    );
    if (status != 0U) return 102U;
    write_buffer_fingerprint(
        "  frontend FNV-1a: ",
        whisper_encoder_qnn_frontend_output(whisper_encoder_qnn),
        (u32)whisper_encoder_qnn_activation_bytes(whisper_encoder_qnn)
    );
    return 0U;
}

static u32 run_cached_model_encoder(
    const QnnInterfaceV2 *api,
    u64 frequency
) {
    long long start_counter;
    long long end_counter;
    u64 status;
    QueryPerformanceCounter(&start_counter);
    status = whisper_encoder_qnn_execute_encoder(whisper_encoder_qnn, api);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  cached encoder graphExecute", status);
    write_duration_us(
        "  cached encoder NPU time", (u64)(end_counter - start_counter), frequency
    );
    if (status != 0U) return 89U;
    write_buffer_fingerprint(
        "  encoder output FNV-1a: ",
        whisper_encoder_qnn_output(whisper_encoder_qnn),
        (u32)whisper_encoder_qnn_activation_bytes(whisper_encoder_qnn)
    );
    return 0U;
}

static u32 __attribute__((unused)) run_cached_model_fp16_encoder(
    const QnnInterfaceV2 *api,
    u64 frequency,
    const u16 *encoder_input,
    const char *latency_label,
    int validate_fixture,
    int benchmark
) {
    static u64 samples[BENCHMARK_SAMPLES];
    QnnTensor input;
    QnnTensor output;
    u64 sample_total = 0U;
    long long start_counter;
    long long end_counter;
    u64 status;
    u32 index;

    copy_tensor(&input, &model_fp16_monolithic_input);
    copy_tensor(&output, &model_fp16_monolithic_output);
    input.data.v1.memory.client_buffer.data = (void *)encoder_input;
    input.data.v1.memory.client_buffer.data_size = sizeof(model_fp16_encoder_stack_input);
    output.data.v1.memory.client_buffer.data = model_fp16_block_output;
    output.data.v1.memory.client_buffer.data_size = sizeof(model_fp16_block_output);
    QueryPerformanceCounter(&start_counter);
    status = api->graph_execute(model_fp16_monolithic_graph, &input, 1U, &output, 1U, 0, 0);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  cached graphExecute", status);
    write_duration_us("  cached first execution", (u64)(end_counter - start_counter), frequency);
    if (status != 0U) return 89U;
    if (validate_fixture &&
        !validate_fp16_encoder_output(model_fp16_block_output, model_fp16_block_expected)) {
        return 90U;
    }
    if (!validate_fixture) {
        write_buffer_fingerprint(
            "  encoder output FNV-1a: ", model_fp16_block_output, sizeof(model_fp16_block_output)
        );
    }
    if (!benchmark) return 0U;
    for (index = 0U; index < BENCHMARK_WARMUPS; ++index) {
        status = api->graph_execute(model_fp16_monolithic_graph, &input, 1U, &output, 1U, 0, 0);
        if (status != 0U) return 89U;
    }
    for (index = 0U; index < BENCHMARK_SAMPLES; ++index) {
        QueryPerformanceCounter(&start_counter);
        status = api->graph_execute(model_fp16_monolithic_graph, &input, 1U, &output, 1U, 0, 0);
        QueryPerformanceCounter(&end_counter);
        if (status != 0U) return 89U;
        samples[index] = (u64)(end_counter - start_counter);
        sample_total += samples[index];
    }
    sort_u64(samples, BENCHMARK_SAMPLES);
    write_text(latency_label);
    write_text(" (10 warmups, 100 samples)\n");
    write_duration_us("  min", samples[0], frequency);
    write_duration_us("  median", samples[50], frequency);
    write_duration_us("  p95", samples[94], frequency);
    write_duration_us("  max", samples[99], frequency);
    write_duration_us("  mean", sample_total / BENCHMARK_SAMPLES, frequency);
    return 0U;
}

static u32 run_external_wav_window(
    const QnnInterfaceV2 *api,
    u64 frequency,
    const char *wav_path,
    u64 start_sample,
    u64 *total_samples
) {
    long long window_start;
    long long window_end;
    long long decoder_start;
    long long decoder_end;
    long long decoder_qnn_start;
    long long decoder_qnn_end;
    int decoder_tokens;
    u64 status;
    u32 result;
    const WhisperDecoderProfile *decoder_profile;

    QueryPerformanceCounter(&window_start);
    write_text("Whisper external WAV fast path: ");
    write_text(wav_path);
    write_text("\n");
    result = run_whisper_log_mel_frontend(
        frequency, wav_path, 0, start_sample, total_samples, 0,
        "Whisper external WAV log-mel frontend"
    );
    if (result == 0U) result = run_cached_model_frontend(api, frequency);
    if (result == 0U) result = run_cached_model_encoder(api, frequency);
    if (result == 0U) {
        QueryPerformanceCounter(&decoder_qnn_start);
        status = whisper_decoder_qnn_execute(
            whisper_decoder_qnn, api,
            whisper_encoder_qnn_output(whisper_encoder_qnn)
        );
        QueryPerformanceCounter(&decoder_qnn_end);
        write_call_status("  decoder cross K/V graphExecute", status);
        write_duration_us(
            "  decoder cross K/V NPU time",
            (u64)(decoder_qnn_end - decoder_qnn_start), frequency
        );
        if (status != 0U) result = 110U;
    }
    if (result == 0U) {
        if (!capture_decoder_output) write_text("Transcript (German):\n");
        QueryPerformanceCounter(&decoder_start);
        decoder_tokens = whisper_decoder_transcribe_with_cross_cache(
            whisper_decoder,
            whisper_decoder_qnn_keys(whisper_decoder_qnn),
            whisper_decoder_qnn_values(whisper_decoder_qnn),
            256U, write_decoder_bytes
        );
        QueryPerformanceCounter(&decoder_end);
        decoder_profile = whisper_decoder_get_profile(whisper_decoder);
        if (!capture_decoder_output) write_text("\n");
        write_duration_us(
            "  decoder time", (u64)(decoder_end - decoder_start), frequency
        );
        write_text("  generated tokens: ");
        write_u32(decoder_tokens < 0 ? 0U : (u32)decoder_tokens);
        write_text("\n");
        write_text("  decoder steps: ");
        write_u32(decoder_profile->decoder_steps);
        write_text("\n");
        write_text("  decoder workers: ");
        write_u32(decoder_profile->worker_count);
        write_text("\n");
        write_duration_us(
            "  encoder normalization", decoder_profile->encoder_normalize_ticks,
            frequency
        );
        write_duration_us(
            "  cross K/V precompute", decoder_profile->cross_cache_ticks,
            frequency
        );
        write_duration_us(
            "  CPU self-attention", decoder_profile->self_attention_ticks,
            frequency
        );
        write_duration_us(
            "  CPU cross-attention", decoder_profile->cross_attention_ticks,
            frequency
        );
        write_duration_us(
            "  NPU cross-attention", decoder_profile->npu_cross_attention_ticks,
            frequency
        );
        write_duration_us(
            "  NPU cross-attention graphExecute",
            decoder_profile->npu_cross_attention_execute_ticks, frequency
        );
        write_duration_us(
            "  CPU feed-forward", decoder_profile->feed_forward_ticks,
            frequency
        );
        write_duration_us(
            "  NPU feed-forward", decoder_profile->npu_feed_forward_ticks,
            frequency
        );
        write_duration_us(
            "  NPU MLP graphExecute", decoder_profile->npu_mlp_execute_ticks,
            frequency
        );
        write_duration_us(
            "  CPU final norm/logits", decoder_profile->logits_ticks,
            frequency
        );
        write_process_memory();
        if (decoder_tokens < 0) result = 109U;
    }
    QueryPerformanceCounter(&window_end);
    write_duration_us(
        "  window total time", (u64)(window_end - window_start), frequency
    );
    return result;
}

static void write_batch_marker(u32 index, const char *state) {
    write_text("=== WHISPER BATCH SEGMENT ");
    write_u32(index);
    write_text(" ");
    write_text(state);
    write_text(" ===\n");
}

static void finish(u32 status) {
    if (console_output_cp_changed) SetConsoleOutputCP(original_console_output_cp);
    ExitProcess(status);
}

static u32 run_uint16_add_probe(const QnnInterfaceV2 *api, QnnContextHandle context);
static u32 run_w16a16_fully_connected_probe(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
);
static u32 run_fp16_fully_connected_probe(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
);

void mainCRTStartup(void) {
    void *module;
    void *symbol;
    QnnInterfaceGetProviders get_providers;
    const QnnInterfacePrefix **providers = 0;
    u32 provider_count = 0U;
    u64 status;
    u32 index;
    const QnnInterfaceProviderV2 *provider = 0;
    const QnnInterfaceV2 *api;
    QnnLogHandle log_handle = 0;
    QnnBackendHandle backend_handle = 0;
    QnnDeviceHandle device_handle = 0;
    QnnProfileHandle profile_handle = 0;
    QnnContextHandle context_handle = 0;
    QnnGraphHandle graph_handle = 0;
    u32 dimensions[1] = {8U};
    u8 input_a[8] = {1U, 2U, 3U, 4U, 5U, 10U, 20U, 30U};
    u8 input_b[8] = {10U, 20U, 30U, 40U, 50U, 60U, 70U, 80U};
    u8 output[8] = {0U};
    u8 expected[8];
    QnnTensor inputs[2];
    QnnTensor outputs[1];
    QnnOpConfig operation = {0};
    u64 samples[100];
    u64 sample_total = 0U;
    long long counter_frequency;
    long long start_counter;
    long long end_counter;
    u32 exit_status = 0U;
    int cache_loaded = 0;
    int decoder_qnn_built = 0;
    const char *wav_argument;
    u32 console_mode;

    stdout_handle = GetStdHandle(0xfffffff5U);
    active_model = whisper_model_tiny();
    if (GetConsoleMode(stdout_handle, &console_mode)) {
        original_console_output_cp = GetConsoleOutputCP();
        if (original_console_output_cp != 0U && original_console_output_cp != 65001U) {
            console_output_cp_changed = SetConsoleOutputCP(65001U);
        }
    }
    wav_argument = first_command_argument();
    if (!initialize_model_context_paths(active_model)) finish(117U);
    if (command_argument_error) {
        static const char usage[] =
            "Usage: npu_probe.exe [--model=tiny|base|small] [--decoder-workers=1..32] [--quiet] <wav-path>\n";
        write_raw_bytes(usage, sizeof(usage) - 1U);
        finish(116U);
    }
    if (quiet_output) silence_standard_error();
    write_text("QNN HTP provider probe\n");
    if (!QueryPerformanceFrequency(&counter_frequency) || counter_frequency <= 0) {
        write_text("QueryPerformanceFrequency failed.\n");
        finish(25U);
    }
    reference_add(input_a, input_b, expected, 8U);
    module = LoadLibraryA("QnnHtp.dll");
    if (module == 0) {
        write_text("QnnHtp.dll: not loadable (Win32 error ");
        write_u32(GetLastError());
        write_text(")\nStage the official Windows ARM64 QNN runtime beside this executable.\n");
        finish(2U);
    }
    write_text("QnnHtp.dll: loaded\n");

    symbol = GetProcAddress(module, "QnnInterface_getProviders");
    if (symbol == 0) {
        write_text("QnnInterface_getProviders: missing (Win32 error ");
        write_u32(GetLastError());
        write_text(")\n");
        FreeLibrary(module);
        finish(3U);
    }

    get_providers = (QnnInterfaceGetProviders)symbol;
    status = get_providers(&providers, &provider_count);
    write_text("QnnInterface_getProviders: ");
    write_hex64(status);
    write_text("\n");
    if (status != 0U || providers == 0 || provider_count == 0U) {
        write_text("No usable QNN interface providers returned.\n");
        FreeLibrary(module);
        finish(4U);
    }

    write_text("providers: ");
    write_u32(provider_count);
    write_text("\n");
    for (index = 0U; index < provider_count; ++index) {
        const QnnApiVersion *version = &providers[index]->api_version;
        write_text("  provider ");
        write_u32(index);
        write_text(": ");
        write_text(providers[index]->provider_name);
        write_text(", backend id ");
        write_u32(providers[index]->backend_id);
        write_text(", core ");
        write_version(&version->core_api_version);
        write_text(", backend ");
        write_version(&version->backend_api_version);
        write_text("\n");
        if (provider == 0 && version->core_api_version.major == 2U &&
            version->core_api_version.minor >= 32U) {
            provider = (const QnnInterfaceProviderV2 *)providers[index];
        }
    }

    if (provider == 0) {
        write_text("No ABI-compatible QNN 2.32+ provider found.\n");
        FreeLibrary(module);
        finish(5U);
    }

    api = &provider->api;
    if (api->log_create == 0 || api->log_free == 0 ||
        api->backend_create == 0 || api->backend_free == 0 ||
        api->device_create == 0 || api->device_free == 0 ||
        api->profile_create == 0 || api->profile_free == 0 ||
        api->context_create == 0 || api->context_free == 0 ||
        api->graph_create == 0 || api->graph_add_node == 0 ||
        api->graph_finalize == 0 || api->graph_execute == 0 ||
        api->tensor_create_graph_tensor == 0) {
        write_text("Provider is missing one or more required lifecycle or graph functions.\n");
        FreeLibrary(module);
        finish(6U);
    }

    write_text("QNN HTP lifecycle\n");
    status = api->log_create(0, quiet_output ? 0U : 2U, &log_handle);
    write_call_status("  logCreate", status);
    if (status != 0U) {
        exit_status = 7U;
        goto cleanup;
    }

    status = api->backend_create(log_handle, 0, &backend_handle);
    write_call_status("  backendCreate", status);
    if (status != 0U) {
        exit_status = 8U;
        goto cleanup;
    }

    status = api->device_create(log_handle, 0, &device_handle);
    write_call_status("  deviceCreate", status);
    if (status != 0U) {
        exit_status = 9U;
        goto cleanup;
    }

    status = api->profile_create(backend_handle, 1U, &profile_handle);
    write_call_status("  profileCreate", status);
    if (status != 0U) {
        exit_status = 10U;
        goto cleanup;
    }

    status = api->context_create(backend_handle, device_handle, 0, &context_handle);
    write_call_status("  contextCreate", status);
    if (status != 0U) {
        exit_status = 11U;
        goto cleanup;
    }
    write_text("  HTP device/context connection established\n");

    whisper_decoder_qnn = whisper_decoder_qnn_create(active_model);
    whisper_encoder_qnn = whisper_encoder_qnn_create(active_model);
    if (whisper_decoder_qnn == 0 || whisper_encoder_qnn == 0) {
        exit_status = 117U;
        goto cleanup;
    }

    if (wav_argument != 0) {
        if (api->context_create_from_binary != 0 &&
            api->graph_retrieve != 0) {
            exit_status = load_model_context_cache(
                api, backend_handle, device_handle, profile_handle, &context_handle,
                (u64)counter_frequency, &cache_loaded
            );
        }
        if (exit_status == 0U && !cache_loaded) {
            write_text("Whisper graph cache is missing; run npu_probe.exe once without arguments.\n");
            exit_status = 107U;
        }
        if (exit_status == 0U &&
            (whisper_decoder = whisper_decoder_load_with_workers(
                active_model, decoder_worker_count
            )) == 0) {
            write_text("Whisper decoder artifacts are missing or invalid.\n");
            exit_status = 108U;
        }
        if (exit_status == 0U) {
            whisper_decoder_set_cross_attention_offload(
                whisper_decoder, whisper_decoder_qnn_cross_attention_offload,
                whisper_decoder_qnn
            );
            whisper_decoder_set_mlp_offload(
                whisper_decoder, whisper_decoder_qnn_mlp_offload,
                whisper_decoder_qnn
            );
        }
        if (exit_status == 0U && wav_argument[0] == '@') {
            u32 segment_index = 0U;
            int next;
            if (!wav_manifest_open(&wav_manifest_reader, wav_argument + 1)) {
                write_text("Whisper WAV manifest is not readable.\n");
                exit_status = 111U;
            }
            while (exit_status == 0U &&
                   (next = wav_manifest_next(
                        &wav_manifest_reader, wav_manifest_path,
                        sizeof(wav_manifest_path))) == 1) {
                write_batch_marker(segment_index, "BEGIN");
                exit_status = run_external_wav_window(
                    api, (u64)counter_frequency, wav_manifest_path, 0U, 0
                );
                write_batch_marker(segment_index, "END");
                ++segment_index;
            }
            if (wav_manifest_reader.handle != 0 &&
                wav_manifest_reader.handle != (void *)(usize)-1) {
                CloseHandle(wav_manifest_reader.handle);
            }
            if (exit_status == 0U && next < 0) {
                write_text("Whisper WAV manifest contains an invalid path.\n");
                exit_status = 112U;
            }
            if (exit_status == 0U && segment_index == 0U) {
                write_text("Whisper WAV manifest is empty.\n");
                exit_status = 113U;
            }
        } else if (exit_status == 0U &&
                   text_has_prefix(wav_argument, "--single-window=")) {
            exit_status = run_external_wav_window(
                api, (u64)counter_frequency, wav_argument + 16U, 0U, 0
            );
            if (quiet_output && exit_status == 0U) write_raw_bytes("\n", 1U);
        } else if (exit_status == 0U) {
            u64 start_sample = 0U;
            u64 total_samples = 0U;
            u32 segment_index = 0U;
            stitched_transcript_size = 0U;
            stitched_transcript[0] = '\0';
            for (;;) {
                u32 appended_start = stitched_transcript_size;
                segment_transcript_size = 0U;
                segment_transcript[0] = '\0';
                segment_transcript_overflow = 0;
                capture_decoder_output = 1;
                write_batch_marker(segment_index, "BEGIN");
                write_text("  window start seconds: ");
                write_u64(start_sample / WHISPER_SAMPLE_RATE);
                write_text("\n");
                exit_status = run_external_wav_window(
                    api, (u64)counter_frequency, wav_argument,
                    start_sample, &total_samples
                );
                capture_decoder_output = 0;
                if (exit_status == 0U && segment_transcript_overflow) {
                    write_text("Whisper segment transcript exceeds native capture capacity.\n");
                    exit_status = 114U;
                }
                if (exit_status == 0U && !stitch_segment_transcript(&appended_start)) {
                    write_text("Whisper stitched transcript exceeds native capture capacity.\n");
                    exit_status = 115U;
                }
                if (exit_status == 0U && appended_start < stitched_transcript_size) {
                    if (quiet_output) {
                        write_raw_bytes(
                            stitched_transcript + appended_start,
                            stitched_transcript_size - appended_start
                        );
                    } else {
                        u32 visible_start = appended_start;
                        if (stitched_transcript[visible_start] == ' ') ++visible_start;
                        write_text("Transcript update (stitched):\n");
                        write_bytes(
                            stitched_transcript + visible_start,
                            stitched_transcript_size - visible_start
                        );
                        write_text("\n");
                    }
                }
                write_batch_marker(segment_index, "END");
                if (exit_status != 0U) break;
                ++segment_index;
                start_sample += LONG_FORM_STEP_SAMPLES;
                if (start_sample + LONG_FORM_OVERLAP_SAMPLES >= total_samples) break;
            }
            if (exit_status == 0U) {
                if (quiet_output) {
                    write_raw_bytes("\n", 1U);
                } else {
                    write_text("Full transcript (German):\n");
                    write_bytes(stitched_transcript, stitched_transcript_size);
                    write_text("\n  transcribed windows: ");
                    write_u32(segment_index);
                    write_text("\n");
                }
            }
        }
        goto cleanup;
    }

#if defined(WHISPER_RUNTIME_ONLY)
    write_text("Usage: npu_probe.exe [--model=tiny|base|small] [--decoder-workers=1..32] [--quiet] <wav-path>\n");
    exit_status = 116U;
    goto cleanup;
#else
    write_text("QNN quantized ElementWiseAdd graph\n");
    QueryPerformanceCounter(&start_counter);
    status = api->graph_create(context_handle, "npu_probe_add", 0, &graph_handle);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphCreate", status);
    write_duration_us("  graphCreate time", (u64)(end_counter - start_counter), (u64)counter_frequency);
    if (status != 0U) {
        exit_status = 17U;
        goto cleanup;
    }

    inputs[0] = make_tensor("input_a", QNN_TENSOR_TYPE_APP_WRITE, dimensions, 1U);
    inputs[1] = make_tensor("input_b", QNN_TENSOR_TYPE_APP_WRITE, dimensions, 1U);
    outputs[0] = make_tensor("output", QNN_TENSOR_TYPE_APP_READ, dimensions, 1U);

    status = api->tensor_create_graph_tensor(graph_handle, &inputs[0]);
    write_call_status("  tensorCreate input_a", status);
    if (status != 0U) {
        exit_status = 18U;
        goto cleanup;
    }
    status = api->tensor_create_graph_tensor(graph_handle, &inputs[1]);
    write_call_status("  tensorCreate input_b", status);
    if (status != 0U) {
        exit_status = 19U;
        goto cleanup;
    }
    status = api->tensor_create_graph_tensor(graph_handle, &outputs[0]);
    write_call_status("  tensorCreate output", status);
    if (status != 0U) {
        exit_status = 20U;
        goto cleanup;
    }

    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = "add";
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = "ElementWiseAdd";
    operation.data.v1.input_count = 2U;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = outputs;
    status = api->graph_add_node(graph_handle, operation);
    write_call_status("  graphAddNode", status);
    if (status != 0U) {
        exit_status = 21U;
        goto cleanup;
    }

    QueryPerformanceCounter(&start_counter);
    status = api->graph_finalize(graph_handle, 0, 0);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphFinalize", status);
    write_duration_us("  graphFinalize time", (u64)(end_counter - start_counter), (u64)counter_frequency);
    if (status != 0U) {
        exit_status = 22U;
        goto cleanup;
    }

    inputs[0].data.v1.memory.client_buffer.data = input_a;
    inputs[0].data.v1.memory.client_buffer.data_size = sizeof(input_a);
    inputs[1].data.v1.memory.client_buffer.data = input_b;
    inputs[1].data.v1.memory.client_buffer.data_size = sizeof(input_b);
    outputs[0].data.v1.memory.client_buffer.data = output;
    outputs[0].data.v1.memory.client_buffer.data_size = sizeof(output);
    QueryPerformanceCounter(&start_counter);
    status = api->graph_execute(graph_handle, inputs, 2U, outputs, 1U, 0, 0);
    QueryPerformanceCounter(&end_counter);
    write_call_status("  graphExecute", status);
    if (status != 0U) {
        exit_status = 23U;
        goto cleanup;
    }
    write_duration_us("  first execution", (u64)(end_counter - start_counter), (u64)counter_frequency);
    if (!validate_output(output, expected, 8U)) {
        exit_status = 24U;
        goto cleanup;
    }
    write_text("  output matches CPU reference\n");

    for (index = 0U; index < 10U; ++index) {
        status = api->graph_execute(graph_handle, inputs, 2U, outputs, 1U, 0, 0);
        if (status != 0U) {
            write_call_status("  warmup graphExecute", status);
            exit_status = 23U;
            goto cleanup;
        }
    }
    for (index = 0U; index < 100U; ++index) {
        QueryPerformanceCounter(&start_counter);
        status = api->graph_execute(graph_handle, inputs, 2U, outputs, 1U, 0, 0);
        QueryPerformanceCounter(&end_counter);
        if (status != 0U) {
            write_call_status("  measured graphExecute", status);
            exit_status = 23U;
            goto cleanup;
        }
        samples[index] = (u64)(end_counter - start_counter);
        sample_total += samples[index];
    }
    if (!validate_output(output, expected, 8U)) {
        exit_status = 24U;
        goto cleanup;
    }
    sort_u64(samples, 100U);
    write_text("QNN warm execution latency (10 warmups, 100 samples)\n");
    write_duration_us("  min", samples[0], (u64)counter_frequency);
    write_duration_us("  median", samples[50], (u64)counter_frequency);
    write_duration_us("  p95", samples[94], (u64)counter_frequency);
    write_duration_us("  max", samples[99], (u64)counter_frequency);
    write_duration_us("  mean", sample_total / 100U, (u64)counter_frequency);

    exit_status = run_uint16_add_probe(api, context_handle);
    if (exit_status == 0U) {
        exit_status = run_w16a16_fully_connected_probe(api, context_handle);
    }
    if (exit_status == 0U) {
        exit_status = run_fp16_fully_connected_probe(api, context_handle);
    }
    if (exit_status == 0U) {
        exit_status = run_tiny_projection(
            api,
            context_handle,
            (u64)counter_frequency,
            1U,
            TINY_WIDTH,
            TINY_WIDTH,
            "npu_probe_tiny_token"
        );
    }
    if (exit_status == 0U) {
        exit_status = run_tiny_projection(
            api,
            context_handle,
            (u64)counter_frequency,
            TINY_SEQUENCE_LENGTH,
            TINY_WIDTH,
            TINY_WIDTH,
            "npu_probe_tiny_sequence"
        );
    }
    if (exit_status == 0U) {
        exit_status = run_tiny_projection(
            api,
            context_handle,
            (u64)counter_frequency,
            TINY_SEQUENCE_LENGTH,
            TINY_WIDTH,
            TINY_MLP_WIDTH,
            "npu_probe_tiny_mlp_expand"
        );
    }
    if (exit_status == 0U) {
        exit_status = run_tiny_projection(
            api,
            context_handle,
            (u64)counter_frequency,
            TINY_SEQUENCE_LENGTH,
            TINY_MLP_WIDTH,
            TINY_WIDTH,
            "npu_probe_tiny_mlp_contract"
        );
    }
    if (exit_status == 0U) {
        exit_status = run_attention_layout_probe(api, context_handle);
    }
    if (exit_status == 0U) {
        exit_status = run_model_layer_norm_probe(api, context_handle);
    }
    if (exit_status == 0U) {
        exit_status = run_model_mlp(api, context_handle, (u64)counter_frequency);
    }
    if (exit_status == 0U) {
        exit_status = run_model_attention_projections(api, context_handle, (u64)counter_frequency);
    }
    if (exit_status == 0U) {
        exit_status = run_model_attention_core(api, context_handle, (u64)counter_frequency);
    }
    for (index = 0U; exit_status == 0U && index < 4U; ++index) {
        exit_status = run_model_encoder_block(
            api, context_handle, (u64)counter_frequency, index, 0, 0
        );
    }
    if (exit_status == 0U) {
        exit_status = run_model_encoder_stack(api, (u64)counter_frequency);
    }
    for (index = 0U; exit_status == 0U && index < 4U; ++index) {
        exit_status = run_model_encoder_block(
            api, context_handle, (u64)counter_frequency, index, 1, 0
        );
    }
    if (exit_status == 0U) {
        exit_status = run_model_fp16_encoder_stack(api, (u64)counter_frequency);
    }
    if (exit_status == 0U) {
        exit_status = run_whisper_log_mel_frontend(
            (u64)counter_frequency,
            "experimental/snapdragon/models/calibration/fleurs/ar_eg-1606.wav",
            "../models/calibration/fleurs/ar_eg-1606.wav",
            0U,
            0,
            1,
            "Whisper freestanding log-mel frontend"
        );
    }
    if (exit_status == 0U && api->context_get_binary_size != 0 &&
        api->context_get_binary != 0 && api->context_create_from_binary != 0 &&
        api->graph_retrieve != 0) {
        exit_status = load_model_context_cache(
            api, backend_handle, device_handle, profile_handle, &context_handle,
            (u64)counter_frequency, &cache_loaded
        );
    }
    if (exit_status == 0U && cache_loaded) {
        write_text("QNN cached model frontend/encoder\n");
        exit_status = run_cached_model_frontend(api, (u64)counter_frequency);
        if (exit_status == 0U) {
            exit_status = run_cached_model_encoder(api, (u64)counter_frequency);
        }
    } else if (exit_status == 0U) {
        if (api->context_get_binary_size != 0 && api->context_get_binary != 0 &&
            api->context_create_from_binary != 0 && api->graph_retrieve != 0) {
            status = api->context_free(context_handle, profile_handle);
            write_call_status("  contextFree before monolithic build", status);
            context_handle = 0;
            if (status != 0U) exit_status = 91U;
            if (exit_status == 0U) {
                status = api->context_create(
                    backend_handle, device_handle, 0, &context_handle
                );
                write_call_status("  dedicated contextCreate", status);
                if (status != 0U) exit_status = 92U;
            }
        }
        if (exit_status == 0U) {
            int encoder_qnn_status = whisper_encoder_qnn_build(
                whisper_encoder_qnn, api, context_handle, &encoder_qnn_ids
            );
            write_text("QNN model frontend/encoder graph build: ");
            write_text(encoder_qnn_status == 1 ? "complete\n" :
                (encoder_qnn_status == 0 ? "skipped (artifacts unavailable)\n" : "failed\n"));
            if (encoder_qnn_status < 0) {
                exit_status = 110U;
            } else if (encoder_qnn_status == 0) {
                exit_status = 107U;
            }
        }
        if (exit_status == 0U) {
            int decoder_qnn_status = whisper_decoder_qnn_build(
                whisper_decoder_qnn, api, context_handle, &decoder_qnn_ids
            );
            write_text("QNN decoder cross K/V and MLP graph build: ");
            write_text(decoder_qnn_status == 1 ? "complete\n" :
                (decoder_qnn_status == 0 ? "skipped (artifacts unavailable)\n" : "failed\n"));
            if (decoder_qnn_status < 0) {
                exit_status = 110U;
            } else if (decoder_qnn_status == 1) {
                decoder_qnn_built = 1;
            }
        }
        if (exit_status == 0U && decoder_qnn_built &&
            api->context_get_binary_size != 0 && api->context_get_binary != 0 &&
            api->context_create_from_binary != 0 && api->graph_retrieve != 0) {
            exit_status = write_model_context_cache(api, context_handle);
            if (exit_status == 0U) {
                exit_status = load_model_context_cache(
                    api, backend_handle, device_handle, profile_handle, &context_handle,
                    (u64)counter_frequency, &cache_loaded
                );
            }
            if (exit_status == 0U && cache_loaded) {
                write_text("QNN reloaded model frontend/encoder\n");
                exit_status = run_cached_model_frontend(api, (u64)counter_frequency);
                if (exit_status == 0U) {
                    exit_status = run_cached_model_encoder(api, (u64)counter_frequency);
                }
            }
        }
    }
#endif
cleanup:
    whisper_decoder_shutdown(whisper_decoder);
    whisper_decoder = 0;
    whisper_encoder_qnn_shutdown(whisper_encoder_qnn);
    whisper_encoder_qnn = 0;
    whisper_decoder_qnn_shutdown(whisper_decoder_qnn);
    whisper_decoder_qnn = 0;
    if (context_handle != 0) {
        status = api->context_free(context_handle, profile_handle);
        write_call_status("  contextFree", status);
        if (status != 0U && exit_status == 0U) exit_status = 12U;
    }
    if (profile_handle != 0) {
        status = api->profile_free(profile_handle);
        write_call_status("  profileFree", status);
        if (status != 0U && exit_status == 0U) exit_status = 13U;
    }
    if (device_handle != 0) {
        status = api->device_free(device_handle);
        write_call_status("  deviceFree", status);
        if (status != 0U && exit_status == 0U) exit_status = 14U;
    }
    if (backend_handle != 0) {
        status = api->backend_free(backend_handle);
        write_call_status("  backendFree", status);
        if (status != 0U && exit_status == 0U) exit_status = 15U;
    }
    if (log_handle != 0) {
        status = api->log_free(log_handle);
        write_call_status("  logFree", status);
        if (status != 0U && exit_status == 0U) exit_status = 16U;
    }

    FreeLibrary(module);
    if (quiet_stderr_handle != 0) CloseHandle(quiet_stderr_handle);
    finish(exit_status);
}

static u32 run_uint16_add_probe(const QnnInterfaceV2 *api, QnnContextHandle context) {
    QnnGraphHandle graph = 0;
    u32 dimensions[1] = {8U};
    u16 input_a[8] = {1U, 257U, 1024U, 4096U, 10000U, 20000U, 30000U, 32000U};
    u16 input_b[8] = {2U, 513U, 2048U, 8192U, 11000U, 21000U, 31000U, 33000U};
    u16 output[8] = {0U};
    u16 expected[8];
    QnnTensor inputs[2];
    QnnTensor outputs[1];
    QnnOpConfig operation = {0};
    u64 status;
    u32 index;

    write_text("QNN quantized UINT16 ElementWiseAdd capability probe\n");
    status = api->graph_create(context, "npu_probe_uint16_add", 0, &graph);
    write_call_status("  graphCreate", status);
    if (status != 0U) return 84U;
    inputs[0] = make_tensor("uint16_input_a", QNN_TENSOR_TYPE_APP_WRITE, dimensions, 1U);
    inputs[1] = make_tensor("uint16_input_b", QNN_TENSOR_TYPE_APP_WRITE, dimensions, 1U);
    outputs[0] = make_tensor("uint16_output", QNN_TENSOR_TYPE_APP_READ, dimensions, 1U);
    inputs[0].data.v1.data_type = QNN_DATATYPE_UFIXED_POINT_16;
    inputs[1].data.v1.data_type = QNN_DATATYPE_UFIXED_POINT_16;
    outputs[0].data.v1.data_type = QNN_DATATYPE_UFIXED_POINT_16;
    status = api->tensor_create_graph_tensor(graph, &inputs[0]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &inputs[1]);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[0]);
    write_call_status("  tensorCreate", status);
    if (status != 0U) return 85U;
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = "uint16_add";
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = "ElementWiseAdd";
    operation.data.v1.input_count = 2U;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = outputs;
    status = api->graph_add_node(graph, operation);
    write_call_status("  graphAddNode", status);
    if (status != 0U) return 86U;
    status = api->graph_finalize(graph, 0, 0);
    write_call_status("  graphFinalize", status);
    if (status != 0U) return 87U;
    for (index = 0U; index < 8U; ++index) expected[index] = input_a[index] + input_b[index];
    inputs[0].data.v1.memory.client_buffer.data = input_a;
    inputs[0].data.v1.memory.client_buffer.data_size = sizeof(input_a);
    inputs[1].data.v1.memory.client_buffer.data = input_b;
    inputs[1].data.v1.memory.client_buffer.data_size = sizeof(input_b);
    outputs[0].data.v1.memory.client_buffer.data = output;
    outputs[0].data.v1.memory.client_buffer.data_size = sizeof(output);
    status = api->graph_execute(graph, inputs, 2U, outputs, 1U, 0, 0);
    write_call_status("  graphExecute", status);
    if (status != 0U) return 88U;
    for (index = 0U; index < 8U; ++index) {
        if (output[index] != expected[index]) {
            write_text("  output mismatch at ");
            write_u32(index);
            write_text("\n");
            return 89U;
        }
    }
    write_text("  output matches UINT16 reference\n");
    return 0U;
}

static u32 run_w16a16_fully_connected_probe(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    QnnGraphHandle graph = 0;
    u32 input_dimensions[2] = {1U, 4U};
    u32 weight_dimensions[2] = {3U, 4U};
    u32 bias_dimensions[1] = {3U};
    u32 output_dimensions[2] = {1U, 3U};
    u16 input_data[4] = {1U, 2U, 3U, 4U};
    u16 weight_data[12] = {
        32769U, 32770U, 32771U, 32772U,
        32767U, 32769U, 32767U, 32769U,
        32770U, 32768U, 32769U, 32767U
    };
    i32 bias_data[3] = {0, 0, 0};
    u16 output_data[3] = {0U};
    u16 expected[3] = {30U, 2U, 1U};
    QnnScaleOffset bias_quant[3] = {{1.0f, 0}, {1.0f, 0}, {1.0f, 0}};
    QnnTensor inputs[3];
    QnnTensor converted_weight;
    QnnTensor convert_inputs[1];
    QnnTensor convert_outputs[1];
    QnnTensor outputs[1];
    QnnOpConfig operation = {0};
    u64 status;
    u32 index;

    write_text("QNN W16A16 FullyConnected capability probe\n");
    status = api->graph_create(context, "npu_probe_w16a16_fully_connected", 0, &graph);
    write_call_status("  graphCreate", status);
    if (status != 0U) return 90U;
    inputs[0] = make_tensor("w16a16_input", QNN_TENSOR_TYPE_APP_WRITE, input_dimensions, 2U);
    inputs[0].data.v1.data_type = QNN_DATATYPE_UFIXED_POINT_16;
    inputs[1] = make_tensor("w16a16_weight", QNN_TENSOR_TYPE_STATIC, weight_dimensions, 2U);
    inputs[1].data.v1.data_type = QNN_DATATYPE_UFIXED_POINT_16;
    inputs[1].data.v1.quantize_params.encoding.scale_offset.offset = -32768;
    inputs[1].data.v1.memory.client_buffer.data = weight_data;
    inputs[1].data.v1.memory.client_buffer.data_size = sizeof(weight_data);
    inputs[2] = make_axis_tensor(
        "w16a16_bias", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_SFIXED_POINT_32,
        bias_dimensions, 1U, 0, 3U, bias_quant
    );
    inputs[2].data.v1.memory.client_buffer.data = bias_data;
    inputs[2].data.v1.memory.client_buffer.data_size = sizeof(bias_data);
    converted_weight = make_tensor(
        "w16a16_converted_weight", QNN_TENSOR_TYPE_NATIVE, weight_dimensions, 2U
    );
    converted_weight.data.v1.data_type = QNN_DATATYPE_SFIXED_POINT_16;
    outputs[0] = make_tensor("w16a16_output", QNN_TENSOR_TYPE_APP_READ, output_dimensions, 2U);
    outputs[0].data.v1.data_type = QNN_DATATYPE_UFIXED_POINT_16;
    for (index = 0U; index < 3U; ++index) {
        status = api->tensor_create_graph_tensor(graph, &inputs[index]);
        if (status != 0U) break;
    }
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &converted_weight);
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[0]);
    write_call_status("  tensorCreate", status);
    if (status != 0U) return 91U;
    copy_tensor(&convert_inputs[0], &inputs[1]);
    copy_tensor(&convert_outputs[0], &converted_weight);
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = "w16a16_weight_convert";
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = "Convert";
    operation.data.v1.input_count = 1U;
    operation.data.v1.inputs = convert_inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = convert_outputs;
    status = api->graph_add_node(graph, operation);
    write_call_status("  graphAddNode Convert", status);
    if (status != 0U) return 92U;
    copy_tensor(&inputs[1], &converted_weight);
    operation.data.v1.name = "w16a16_fully_connected";
    operation.data.v1.type_name = "FullyConnected";
    operation.data.v1.input_count = 3U;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = outputs;
    status = api->graph_add_node(graph, operation);
    write_call_status("  graphAddNode", status);
    if (status != 0U) return 92U;
    status = api->graph_finalize(graph, 0, 0);
    write_call_status("  graphFinalize", status);
    if (status != 0U) return 93U;
    inputs[0].data.v1.memory.client_buffer.data = input_data;
    inputs[0].data.v1.memory.client_buffer.data_size = sizeof(input_data);
    outputs[0].data.v1.memory.client_buffer.data = output_data;
    outputs[0].data.v1.memory.client_buffer.data_size = sizeof(output_data);
    status = api->graph_execute(graph, inputs, 1U, outputs, 1U, 0, 0);
    write_call_status("  graphExecute", status);
    if (status != 0U) return 94U;
    for (index = 0U; index < 3U; ++index) {
        if (output_data[index] != expected[index]) {
            write_text("  output mismatch at ");
            write_u32(index);
            write_text(": expected ");
            write_u32(expected[index]);
            write_text(", got ");
            write_u32(output_data[index]);
            write_text("; quantized 16-bit FC is not precision-viable\n");
            return 0U;
        }
    }
    write_text("  output matches W16A16 reference\n");
    return 0U;
}

static u32 run_fp16_fully_connected_probe(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
) {
    QnnGraphHandle graph = 0;
    u32 input_dimensions[2] = {1U, 4U};
    u32 weight_dimensions[2] = {3U, 4U};
    u32 bias_dimensions[1] = {3U};
    u32 output_dimensions[2] = {1U, 3U};
    u16 input_data[4] = {0x3c00U, 0x4000U, 0x4200U, 0x4400U};
    u16 weight_data[12] = {
        0x3c00U, 0x4000U, 0x4200U, 0x4400U,
        0xbc00U, 0x3c00U, 0xbc00U, 0x3c00U,
        0x4000U, 0U, 0x3c00U, 0xbc00U
    };
    u16 bias_data[3] = {0U, 0U, 0U};
    u16 output_data[3] = {0U, 0U, 0U};
    u16 expected[3] = {0x4f80U, 0x4000U, 0x3c00U};
    QnnTensor inputs[3];
    QnnTensor outputs[1];
    QnnOpConfig operation = {0};
    u64 status;
    u32 index;

    write_text("QNN FP16 FullyConnected capability probe\n");
    status = api->graph_create(context, "npu_probe_fp16_fully_connected", 0, &graph);
    write_call_status("  graphCreate", status);
    if (status != 0U) return 96U;
    inputs[0] = make_tensor("fp16_input", QNN_TENSOR_TYPE_APP_WRITE, input_dimensions, 2U);
    inputs[1] = make_tensor("fp16_weight", QNN_TENSOR_TYPE_STATIC, weight_dimensions, 2U);
    inputs[2] = make_tensor("fp16_bias", QNN_TENSOR_TYPE_STATIC, bias_dimensions, 1U);
    outputs[0] = make_tensor("fp16_output", QNN_TENSOR_TYPE_APP_READ, output_dimensions, 2U);
    for (index = 0U; index < 3U; ++index) inputs[index].data.v1.data_type = QNN_DATATYPE_FLOAT_16;
    outputs[0].data.v1.data_type = QNN_DATATYPE_FLOAT_16;
    inputs[1].data.v1.memory.client_buffer.data = weight_data;
    inputs[1].data.v1.memory.client_buffer.data_size = sizeof(weight_data);
    inputs[2].data.v1.memory.client_buffer.data = bias_data;
    inputs[2].data.v1.memory.client_buffer.data_size = sizeof(bias_data);
    for (index = 0U; index < 3U; ++index) {
        status = api->tensor_create_graph_tensor(graph, &inputs[index]);
        if (status != 0U) break;
    }
    if (status == 0U) status = api->tensor_create_graph_tensor(graph, &outputs[0]);
    write_call_status("  tensorCreate", status);
    if (status != 0U) return 97U;
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = "fp16_fully_connected";
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = "FullyConnected";
    operation.data.v1.input_count = 3U;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = 1U;
    operation.data.v1.outputs = outputs;
    status = api->graph_add_node(graph, operation);
    write_call_status("  graphAddNode", status);
    if (status != 0U) return 98U;
    status = api->graph_finalize(graph, 0, 0);
    write_call_status("  graphFinalize", status);
    if (status != 0U) return 99U;
    inputs[0].data.v1.memory.client_buffer.data = input_data;
    inputs[0].data.v1.memory.client_buffer.data_size = sizeof(input_data);
    outputs[0].data.v1.memory.client_buffer.data = output_data;
    outputs[0].data.v1.memory.client_buffer.data_size = sizeof(output_data);
    status = api->graph_execute(graph, inputs, 1U, outputs, 1U, 0, 0);
    write_call_status("  graphExecute", status);
    if (status != 0U) return 100U;
    for (index = 0U; index < 3U; ++index) {
        if (output_data[index] != expected[index]) {
            write_text("  output mismatch at ");
            write_u32(index);
            write_text(": expected bits ");
            write_hex64(expected[index]);
            write_text(", got ");
            write_hex64(output_data[index]);
            write_text("\n");
            return 101U;
        }
    }
    write_text("  output matches FP16 reference\n");
    return 0U;
}