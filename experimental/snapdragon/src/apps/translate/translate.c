#define GEMMA_TRANSLATE 1
#ifdef GEMMA_GUI
static int gui_next(void);
static int gui_write(const unsigned char *, unsigned int);
static void gui_ready(void);
static void gui_done(int);
static int gui_cancelled(void);
static void gui_finished(unsigned int);
#define mainCRTStartup translate_engine_main
#endif
#include "gemma_runner.c"

__declspec(dllimport) const u16 *GetCommandLineW(void);
__declspec(dllimport) int WideCharToMultiByte(u32, u32, const u16 *, int, char *, int, const char *, int *);
__declspec(dllimport) u32 GetModuleFileNameW(void *, u16 *, u32);
__declspec(dllimport) int GetConsoleMode(void *, u32 *);
__declspec(dllimport) int MultiByteToWideChar(u32, u32, const char *, int, u16 *, int);
__declspec(dllimport) int WriteConsoleW(void *, const u16 *, u32, u32 *, void *);
__declspec(dllimport) u32 GetLastError(void);
__declspec(dllimport) int SetStdHandle(u32, void *);

static char arguments[16][32768];
static char tokenizer_path[1024];
static const char *source_language, *target_language, *source_text;
static u32 maximum_tokens = 256;
static int automatic_text = 1, stdin_document;
static u8 document_output[4194304];
static u32 document_output_size;
static GemmaTokenizer tokenizer;
static u32 request_tokens[512], request_count;
static GemmaTokenizerWork *tokenizer_work;
static int batch_mode, padded_decode, force_decode;
static int qnn_profile_requested;
static int verify_decode;
static int no_stream;
static int performance_requested, power_active;
static u32 power_id;
typedef struct TranslatePerformance {
    u32 type;
    u64 (*create)(u32, u32, u32 *);
    u64 (*destroy)(u32);
    u64 (*set)(u32, const void *const *);
    void *memory;
} TranslatePerformance;
static const TranslatePerformance *performance;
static QnnContextHandle selection_context;
static QnnTensor selection_input, selection_outputs[2];
static u32 selected_token;
static u16 selected_finite;
static int cpu_selection;
static int compile_selection;
static void *quiet_sink, *saved_stderr;
static u8 translation_output[GEMMA_TOKENIZER_MAX_BYTES];
static u16 console_output[GEMMA_TOKENIZER_MAX_BYTES];
static QnnProfileHandle execution_profile;
static u32 qnn_sampled, qnn_event_count;
static char batch_line[GEMMA_TOKENIZER_MAX_BYTES];
static QnnContextHandle decode_context;
static u8 *decode_shared;
typedef struct TranslateGraphState {
    GemmaBlock blocks[34];
    QnnTensor inputs[256], outputs[80];
    u32 input_count, output_count, rows;
} TranslateGraphState;
static TranslateGraphState *prefill_state, *decode_state;

static void graph_save(TranslateGraphState *state) {
    memcpy(state->blocks, prompt_blocks, sizeof(prompt_blocks));
    memcpy(state->inputs, prompt_inputs, sizeof(prompt_inputs));
    memcpy(state->outputs, prompt_outputs, sizeof(prompt_outputs));
    state->input_count = prompt_input_count; state->output_count = prompt_output_count; state->rows = prompt_rows;
}

static void graph_load(const TranslateGraphState *state) {
    memcpy(prompt_blocks, state->blocks, sizeof(prompt_blocks));
    memcpy(prompt_inputs, state->inputs, sizeof(prompt_inputs));
    memcpy(prompt_outputs, state->outputs, sizeof(prompt_outputs));
    prompt_input_count = state->input_count; prompt_output_count = state->output_count; prompt_rows = state->rows;
}

static int batch_read(void) {
    u32 used = 0, received; char byte;
    for (;;) {
        if (!ReadFile(GetStdHandle((u32)-10), &byte, 1, &received, 0)) {
            if (GetLastError() != 109) return -1;
            received = 0;
        }
        if (!received) { if (!used) return 0; break; }
        if (byte == '\n') break;
        if (!byte || used == sizeof(batch_line) - 1) return -1;
        batch_line[used++] = byte;
    }
    if (used && batch_line[used - 1] == '\r') --used;
    batch_line[used] = 0;
    if (!used || !gemma_utf8_valid((const u8 *)batch_line, used)) return -1;
    source_text = batch_line;
    return 1;
}

static int request_range(const char *input, u32 size) {
    return gemma_tokenizer_prompt(&tokenizer, tokenizer_work, source_language, target_language,
        (const u8 *)input, size, maximum_tokens, request_tokens, 512, &request_count) &&
        request_count + maximum_tokens <= 512;
}

static int document_space(u8 value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static u32 document_boundary(const char *input, u32 size) {
    u32 sentence = 0, space = 0, paragraph = 0;
    for (u32 index = size / 2; index < size; ++index) {
        if (document_space((u8)input[index])) {
            space = index;
            if (input[index] == '\n') paragraph = index;
            if (index && (input[index - 1] == '.' || input[index - 1] == '!' || input[index - 1] == '?')) sentence = index;
        }
        if (index >= 3 && (u8)input[index - 3] == 0xe3 && (u8)input[index - 2] == 0x80 && (u8)input[index - 1] == 0x82) sentence = index;
    }
    if (paragraph) return paragraph;
    if (sentence) return sentence;
    if (space) return space;
    while (size && ((u8)input[size] & 0xc0) == 0x80) --size;
    return size;
}

static u32 document_piece(const char *input, u32 size) {
    int multiline = 0;
    for (u32 index = 0; index < size && index < 128; ++index)
        if (input[index] == '\r' || input[index] == '\n') multiline = 1;
    if (input == source_text && size <= 128 && !multiline && request_range(input, size)) return size;
    u32 candidate = size < 256 ? size : 256;
    for (u32 index = 0; index < candidate; ++index) {
        if (input[index] == '\r' || input[index] == '\n' ||
            (index && document_space((u8)input[index]) &&
             (input[index - 1] == '.' || input[index - 1] == '!' || input[index - 1] == '?')) ||
            (index >= 3 && (u8)input[index - 3] == 0xe3 && (u8)input[index - 2] == 0x80 && (u8)input[index - 1] == 0x82)) {
            if (index) { candidate = index; break; }
        }
    }
    while (candidate) {
        while (candidate && ((u8)input[candidate] & 0xc0) == 0x80) --candidate;
        if (candidate && request_range(input, candidate)) {
            if (candidate == size || document_space((u8)input[candidate]) ||
                (candidate >= 3 && (u8)input[candidate - 3] == 0xe3 && (u8)input[candidate - 2] == 0x80 && (u8)input[candidate - 1] == 0x82)) return candidate;
            candidate = document_boundary(input, candidate);
            if (candidate && request_range(input, candidate)) return candidate;
        }
        candidate /= 2;
    }
    return 0;
}

static int request_prepare(void) {
    u32 size = length(source_text);
    if (!size || size >= GEMMA_TOKENIZER_MAX_BYTES || !gemma_utf8_valid((const u8 *)source_text, size)) return 0;
    if (!automatic_text) return request_range(source_text, size);
    u32 first = 0;
    while (first < size && document_space((u8)source_text[first])) ++first;
    return first < size && document_piece(source_text + first, size - first) != 0;
}

static int document_read(void) {
    u32 used = 0, received;
    for (;;) {
        u8 byte;
        if (!ReadFile(GetStdHandle((u32)-10), &byte, 1, &received, 0)) {
            if (GetLastError() != 109) return 0;
            received = 0;
        }
        if (!received) break;
        if (!byte || used == sizeof(batch_line) - 1) return 0;
        batch_line[used++] = (char)byte;
    }
    batch_line[used] = 0; source_text = batch_line;
    return used != 0;
}

static int profile_events(const QnnInterfaceV2 *api, const u64 *events, u32 count, u32 depth) {
    if (depth > 8 || count > 4096 - qnn_event_count) return 0;
    for (u32 index = 0; index < count; ++index) {
        QnnProfileEventData event = {0}; const u64 *children = 0; u32 child_count = 0;
        ++qnn_event_count;
        if (api->profile_get_event_data(events[index], &event)) return 0;
        text("QNN_EVENT "); text(event.identifier ? event.identifier : "unnamed");
        text(" type="); number(event.type); text(" unit="); number(event.unit);
        text(" value="); number(event.value); text(" depth="); number(depth); text("\n");
        if (api->profile_get_sub_events(events[index], &children, &child_count) ||
            !profile_events(api, children, child_count, depth + 1)) return 0;
    }
    return 1;
}

static int parse_arguments(const u16 *command) {
    u32 count = 0;
    while (*command) {
        u16 wide[32768]; u32 used = 0; int quoted = 0;
        while (*command == ' ' || *command == '\t') ++command;
        if (!*command) break;
        if (count == 16) return -1;
        while (*command && (quoted || (*command != ' ' && *command != '\t'))) {
            u32 slashes = 0;
            while (*command == '\\') { ++slashes; ++command; }
            if (*command == '"') {
                for (u32 index = 0; index < slashes / 2; ++index) {
                    if (used == 32767) return -1; wide[used++] = '\\';
                }
                if (slashes % 2) { if (used == 32767) return -1; wide[used++] = '"'; ++command; }
                else if (quoted && command[1] == '"') { if (used == 32767) return -1; wide[used++] = '"'; command += 2; }
                else { quoted = !quoted; ++command; }
            } else {
                while (slashes--) { if (used == 32767) return -1; wide[used++] = '\\'; }
                if (!*command || (!quoted && (*command == ' ' || *command == '\t'))) break;
                if (used == 32767) return -1; wide[used++] = *command++;
            }
        }
        if (quoted) return -1;
        wide[used] = 0;
        if (!WideCharToMultiByte(65001, 0x80U, wide, -1, arguments[count], sizeof(arguments[count]), 0, 0)) return -1;
        ++count;
    }
    return (int)count;
}

static int translate_arguments(void) {
    u16 executable[1024]; char directory[1024]; u32 last = 0;
#ifdef GEMMA_GUI
    int count = 1;
    (void)parse_arguments; (void)batch_read; (void)arguments; (void)document_read; (void)stdin_document;
#else
    int count = parse_arguments(GetCommandLineW());
#endif
    if (count == 2 && equal(arguments[1], "--help")) {
        text("Usage: translate.exe --from de --to en TEXT\n"
             "Optional: --bindings PATH --tokenizer PATH --decode --padded-decode --qnn-profile --verify-decode\n"
             "Output: --quiet suppresses all diagnostics; --no-stream buffers until complete.\n"
             "Shared-weight bundle is automatic when installed; --bundle requires it.\n"
             "Diagnostics: --cpu-selection --compile-selection --serial-load --performance (scoped HTP power vote).\n"
             "Use --qnn-profile-detailed for per-operation events.\n"
             "Use --batch instead of TEXT for UTF-8 lines on stdin; languages stay fixed.\n"
             "Use --stdin instead of TEXT for a multiline UTF-8 document (up to 196607 bytes).\n"
             "Long text is split automatically; completed pieces are streamed.\n"
             "--max-tokens 1..256 opts into a strict single-request output limit.\n"
             "Experimental W4 NPU runtime; 512 tokens per piece, 4 MiB output cap. Exit 2 means incomplete.\n");
        ExitProcess(0);
    }
    u32 size = GetModuleFileNameW(0, executable, 1024);
    if (!size || size >= 1024 || !WideCharToMultiByte(65001, 0x80U, executable, -1, directory, sizeof(directory), 0, 0)) return 0;
    for (u32 index = 0; directory[index]; ++index) if (directory[index] == '\\' || directory[index] == '/') last = index + 1;
    if (!last || last > 800) return 0;
    directory[last] = 0;
    join(binding_path, directory, "gemma-block/prompt-512.gmb");
    join(tokenizer_path, directory, "../models/translategemma-4b-stage4/tokenizer.gta");
#ifdef GEMMA_GUI
    translate_quiet = 1; prompt_bundle = 1; force_decode = 1;
    u32 bytes; u8 *data = read_file(tokenizer_path, &bytes);
    tokenizer_work = allocate(0, sizeof(*tokenizer_work));
    if (!data || !tokenizer_work || !gemma_tokenizer_open(&tokenizer, data, bytes)) return 0;
    join(failure_point, "restore-512", "");
    return 1;
#else
    for (int index = 1; index < count; ++index) {
        const char *option = arguments[index];
        if (equal(option, "--quiet")) { translate_quiet = 1; continue; }
        if (equal(option, "--no-stream")) { no_stream = 1; continue; }
        if (equal(option, "--performance")) { performance_requested = 1; continue; }
        if (equal(option, "--cpu-selection")) { cpu_selection = 1; continue; }
        if (equal(option, "--compile-selection")) { compile_selection = 1; continue; }
        if (equal(option, "--serial-load")) { serial_bundle_read = 1; continue; }
        if (equal(option, "--batch")) { batch_mode = 1; continue; }
        if (equal(option, "--stdin")) { stdin_document = 1; continue; }
        if (equal(option, "--padded-decode")) { padded_decode = 1; continue; }
        if (equal(option, "--decode")) { force_decode = 1; continue; }
        if (equal(option, "--bundle")) { prompt_bundle = 1; force_decode = 1; continue; }
        if (equal(option, "--verify-decode")) { force_decode = 1; verify_decode = 1; continue; }
        if (equal(option, "--qnn-profile")) { qnn_profile_requested = 1; continue; }
        if (equal(option, "--qnn-profile-detailed")) { qnn_profile_requested = 2; continue; }
        if (equal(option, "--from") || equal(option, "--to") || equal(option, "--max-tokens") ||
            equal(option, "--bindings") || equal(option, "--tokenizer")) {
            if (++index == count) return 0;
            const char *value = arguments[index];
            if (equal(option, "--from")) { if (source_language) return 0; source_language = value; }
            else if (equal(option, "--to")) { if (target_language) return 0; target_language = value; }
            else if (equal(option, "--max-tokens")) {
                automatic_text = 0;
                maximum_tokens = 0;
                if (!*value) return 0;
                while (*value) {
                    if (*value < '0' || *value > '9' || maximum_tokens > 512) return 0;
                    maximum_tokens = maximum_tokens * 10 + (u32)(*value++ - '0');
                }
                if (!maximum_tokens || maximum_tokens > 256) return 0;
            } else {
                if (length(value) >= 1024) return 0;
                join(equal(option, "--bindings") ? binding_path : tokenizer_path, value, "");
            }
        } else {
            if (source_text || (option[0] == '-' && option[1] == '-')) return 0;
            source_text = option;
        }
    }
    if (translate_quiet) {
        saved_stderr = GetStdHandle((u32)-12);
        quiet_sink = CreateFileA("NUL", 0x40000000U, 3, 0, 3, 0x80U, 0);
        if (quiet_sink == (void *)(u64)-1) { quiet_sink = 0; return 0; }
        if (!SetStdHandle((u32)-12, quiet_sink)) return 0;
    }
    if (force_decode && padded_decode) return 0;
    if (!force_decode && !padded_decode && maximum_tokens > 1) {
        char path[1100]; join(path, binding_path, ".bundle.context");
        void *file = CreateFileA(path, 0x80000000U, 1, 0, 3, 0x80U, 0);
        if (file != (void *)(u64)-1) {
            if (!CloseHandle(file)) return 0;
            prompt_bundle = 1; force_decode = 1;
        } else {
            u32 error = GetLastError();
            if (error != 2 && error != 3) return 0;
        }
    }
    if (!force_decode && !batch_mode) padded_decode = 1;
    if (maximum_tokens == 1) padded_decode = 1;
    if (stdin_document && (batch_mode || source_text || !document_read())) return 0;
    if (batch_mode && (source_text || batch_read() != 1)) return 0;
    if (count < 0 || !source_language || !target_language || !source_text || !*source_text) {
        text("Expected: translate.exe --from de --to en TEXT\n"); return 0;
    }
    u32 bytes; u8 *data = read_file(tokenizer_path, &bytes);
    tokenizer_work = allocate(0, sizeof(*tokenizer_work));
    if (!data || !tokenizer_work || !gemma_tokenizer_open(&tokenizer, data, bytes) || !request_prepare()) {
        text("Invalid language, tokenizer, UTF-8 text, input size, or explicit token budget\n"); return 0;
    }
    join(failure_point, "restore-512", "");
    return 1;
#endif
}

static int translate_step(const QnnInterfaceV2 *api, const u8 *embedding_weights,
                           const GemmaArtifactHeader *header, u32 position, const u32 *tokens, u32 count) {
    PROFILE_START(prepare_started);
    u16 *embedding = prompt_tensor(0, "input")->buffer;
    float *positions = prompt_tensor(0, "positions")->buffer;
    memset(embedding, 0, prompt_rows * 2560U * 2U);
    for (u32 row = 0; row < prompt_rows; ++row) positions[row] = row < count ? (float)(position + row) : 0;
    for (u32 row = 0; row < count; ++row) {
        u32 token = tokens[row]; float values[2560];
        if (token >= 262145) return 0;
        const u8 *scale = embedding_weights + header->scale_offset + token * 2U;
        if (!gemma_numeric_dequantize(embedding_weights + (u64)token * 1280, 4, 2560,
                                      (u16)(scale[0] | (u16)scale[1] << 8), values)) return 0;
        for (u32 channel = 0; channel < 2560; ++channel)
            embedding[row * 2560 + channel] = half(gemma_numeric_f16(half(values[channel])) *
                                                   (gemma_numeric_f16(half(50.59644256269407f)) / 32.0f));
    }
    for (u32 owner = 0; owner <= 5; owner += 5) {
        u16 *mask = prompt_tensor(owner, "mask")->buffer;
        for (u32 head = 0; head < 8; ++head) for (u32 row = 0; row < prompt_rows; ++row) for (u32 key = 0; key < 512 + prompt_rows; ++key) {
            u32 absolute = key < 512 ? key : position + key - 512;
            int occupied = key < 512 ? key < position : key < 512 + count;
            int visible = occupied && gemma_numeric_visible(owner, (u32)positions[row], absolute);
            mask[(head * prompt_rows + row) * (512 + prompt_rows) + key] = visible ? 0 : 0xfbffU;
        }
    }
    *(float *)prompt_tensor(33, "last-token")->buffer = (float)(count - 1);
    PROFILE_END(position < request_count ? PROFILE_PREFILL_PREPARE : PROFILE_DECODE_PREPARE, prepare_started);
    PROFILE_START(execute_started);
    u32 sample_bit = position < request_count ? 1U : 2U;
    QnnProfileHandle sample = execution_profile && !(qnn_sampled & sample_bit) ? execution_profile : 0;
    u64 code = api->graph_execute(prompt_blocks[0].graph, prompt_inputs, prompt_input_count, prompt_outputs, prompt_output_count, sample, 0);
    PROFILE_END(position < request_count ? PROFILE_PREFILL_EXECUTE : PROFILE_DECODE_EXECUTE, execute_started);
#ifdef GEMMA_TRANSLATE_PROFILE
    if (position >= request_count) {
        u64 elapsed = profile_clock() - execute_started;
        if (elapsed < profile_decode_min) profile_decode_min = elapsed;
        if (elapsed > profile_decode_max) profile_decode_max = elapsed;
    }
#endif
    if (code) { status("translation graphExecute", code); return 0; }
    if (sample) {
        const u64 *events = 0; u32 count_events = 0;
        qnn_sampled |= sample_bit; qnn_event_count = 0;
        text(sample_bit == 1 ? "QNN_SAMPLE prefill\n" : "QNN_SAMPLE decode\n");
        if (api->profile_get_events(sample, &events, &count_events) || !profile_events(api, events, count_events, 0)) return 0;
    }
    PROFILE_START(kv_started);
    for (u32 layer_index = 0; layer_index < 34; ++layer_index) for (u32 kind = 0; kind < 2; ++kind) {
        GemmaBlockTensor *current = prompt_tensor(layer_index, kind ? "v-projection" : "k-rope");
        GemmaBlockTensor *past = prompt_tensor(layer_index, kind ? "past-value" : "past-key");
        for (u32 byte = 0; byte < 4096; ++byte)
            if (((u8 *)current->buffer)[current->bytes + byte] != 0xa5 || ((u8 *)past->buffer)[past->bytes + byte] != 0xa5) return 0;
        for (u32 head = 0; head < 4; ++head)
            memcpy((u16 *)past->buffer + (head * 512 + position) * 256,
                   (u16 *)current->buffer + head * prompt_rows * 256, count * 512);
    }
    PROFILE_END(position < request_count ? PROFILE_PREFILL_KV : PROFILE_DECODE_KV, kv_started);
    return 1;
}

static int decode_compare(const u16 *actual, const u16 *expected, u32 heads, u32 width, u32 expected_stride) {
    double error_sum = 0, reference_sum = 0; float maximum = 0, reference_maximum = 0;
    for (u32 head = 0; head < heads; ++head) for (u32 channel = 0; channel < width; ++channel) {
        float reference = gemma_numeric_f16(expected[head * expected_stride + channel]);
        float value = gemma_numeric_f16(actual[head * width + channel]);
        float error = value - reference;
        if (!(value >= -65504 && value <= 65504 && reference >= -65504 && reference <= 65504)) return 0;
        error_sum += (double)error * error; reference_sum += (double)reference * reference;
        if (error < 0) error = -error;
        if (reference < 0) reference = -reference;
        if (error > maximum) maximum = error;
        if (reference > reference_maximum) reference_maximum = reference;
    }
    return error_sum <= 0.000625 * reference_sum + 0.000001 * heads * width && maximum <= 0.15f * reference_maximum + 0.02f;
}

static int translation_write(const u8 *bytes, u32 size) {
    if (!size) return 1;
    if (!gemma_utf8_valid(bytes, size)) return 0;
#ifdef GEMMA_GUI
    (void)console_output;
    return gui_write(bytes, size);
#else
    void *destination = GetStdHandle((u32)-11); u32 console_mode, written;
    if (GetConsoleMode(destination, &console_mode)) {
        int wide_size = MultiByteToWideChar(65001, 8, (const char *)bytes, (int)size,
                                           console_output, GEMMA_TOKENIZER_MAX_BYTES);
        if (wide_size <= 0) return 0;
        for (u32 offset = 0; offset < (u32)wide_size;) {
            u32 count = (u32)wide_size - offset;
            if (count > 16384) count = 16384;
            if (offset + count < (u32)wide_size && console_output[offset + count - 1] >= 0xd800 && console_output[offset + count - 1] <= 0xdbff) --count;
            if (!WriteConsoleW(destination, console_output + offset, count, &written, 0) || !written) return 0;
            offset += written;
        }
    } else {
        for (u32 offset = 0; offset < size;) {
            if (!WriteFile(destination, bytes + offset, size - offset, &written, 0) || !written) return 0;
            offset += written;
        }
    }
    return 1;
#endif
}

static int translation_emit(const u32 *tokens, u32 count, u32 *emitted, int final) {
    PROFILE_START(output_started);
    u32 size;
    int ok = final ? gemma_tokenizer_decode(&tokenizer, tokens, count, 1, translation_output, sizeof(translation_output), &size) :
        gemma_tokenizer_decode_stable(&tokenizer, tokens, count, 1, translation_output, sizeof(translation_output), &size);
    if (!ok || size < *emitted || (final && !size) || !translation_write(translation_output + *emitted, size - *emitted)) return 0;
    *emitted = size;
    if (final && !translation_write((const u8 *)"\n", 1)) return 0;
    PROFILE_END(PROFILE_OUTPUT, output_started);
    return 1;
}

static int translate_request(const QnnInterfaceV2 *api, const u8 *embedding, const GemmaArtifactHeader *header) {
#ifdef GEMMA_DOCUMENT_TEST
    return document_test_generate(api, embedding, header);
#endif
    PROFILE_START(reset_started);
    graph_load(prefill_state);
    for (u32 layer_index = 0; layer_index < 34; ++layer_index) for (u32 kind = 0; kind < 2; ++kind) {
        GemmaBlockTensor *past = prompt_tensor(layer_index, kind ? "past-value" : "past-key");
        memset(past->buffer, 0, past->bytes);
    }
    PROFILE_END(PROFILE_CACHE_RESET, reset_started);
#ifdef GEMMA_TRANSLATE_PROFILE
    status("PROFILE prompt_tokens", request_count);
#endif
    u32 generated[256], generated_count = 0, position = 0, emitted = 0;
    u64 started = now(); int stopped = 0;
    while (position < request_count) {
        u32 count = request_count - position < 128 ? request_count - position : 128;
        if (!translate_step(api, embedding, header, position, request_tokens + position, count)) return 0;
        position += count;
    }
    timing("prefill us", started); started = now();
    while (generated_count < maximum_tokens) {
    #ifdef GEMMA_GUI
        if (gui_cancelled()) return 0;
    #endif
        PROFILE_START(argmax_started);
        u16 *logits = prompt_tensor(33, "logits")->buffer;
        GemmaBlockTensor *selection = prompt_tensor(33, "selected-token");
        u32 best = 0;
        if (selection_context) {
            selection_input.data.v1.memory.client_buffer.data = logits;
            if (api->graph_execute(block.graph, &selection_input, 1, selection_outputs, 2, 0, 0) ||
                selected_finite != 0x3c00) return 0;
            best = selected_token;
        }
        if (selection) {
            GemmaBlockTensor *finite = prompt_tensor(33, "finite-logits");
            if (!finite || *(u16 *)finite->buffer != 0x3c00) return 0;
            best = *(u32 *)selection->buffer;
        }
        if ((!selection && !selection_context) || verify_decode) {
            u32 reference = 0; float maximum = -65505.0f;
            for (u32 token = 0; token < 262208; ++token) {
                float value = gemma_numeric_f16(logits[token]);
                if (!(value >= -65504 && value <= 65504)) return 0;
                if (value > maximum) { maximum = value; reference = token; }
            }
            if ((selection || selection_context) && best != reference) { text("NPU argmax parity failed\n"); return 0; }
            best = reference;
        }
        if (best >= 262145) return 0;
        PROFILE_END(PROFILE_ARGMAX, argmax_started);
    #ifdef GEMMA_TRANSLATE_PROFILE
        if (!profile_first_token) profile_first_token = profile_clock();
    #endif
        generated[generated_count++] = best;
        if (!automatic_text && !no_stream && !translation_emit(generated, generated_count, &emitted, 0)) return 0;
        status("generated token", best);
        if (gemma_model_is_stop_token(best)) { stopped = 1; break; }
        if (generated_count < maximum_tokens) {
            if (generated_count == 1 && decode_state) {
                PROFILE_START(transfer_started);
                void *past_buffers[68];
                for (u32 layer_index = 0; layer_index < 34; ++layer_index) for (u32 kind = 0; kind < 2; ++kind)
                    past_buffers[layer_index * 2 + kind] = prompt_tensor(layer_index, kind ? "past-value" : "past-key")->buffer;
                graph_load(decode_state);
                for (u32 layer_index = 0; layer_index < 34; ++layer_index) for (u32 kind = 0; kind < 2; ++kind) {
                    GemmaBlockTensor *past = prompt_tensor(layer_index, kind ? "past-value" : "past-key");
                    if (past->buffer == past_buffers[layer_index * 2 + kind]) continue;
                    memset(past->buffer, 0, past->bytes);
                    for (u32 head = 0; head < 4; ++head)
                        memcpy((u16 *)past->buffer + head * 512 * 256,
                            (u16 *)past_buffers[layer_index * 2 + kind] + head * 512 * 256, position * 512);
                }
                PROFILE_END(PROFILE_CACHE_TRANSFER, transfer_started);
            }
            if (position >= 512 || !translate_step(api, embedding, header, position, &best, 1)) return 0;
            if (verify_decode && decode_state) {
                const u16 *actual[69];
                for (u32 layer_index = 0; layer_index < 34; ++layer_index) for (u32 kind = 0; kind < 2; ++kind)
                    actual[layer_index * 2 + kind] = prompt_tensor(layer_index, kind ? "v-projection" : "k-rope")->buffer;
                actual[68] = prompt_tensor(33, "logits")->buffer;
                graph_load(prefill_state);
                if (!translate_step(api, embedding, header, position, &best, 1)) return 0;
                for (u32 layer_index = 0; layer_index < 34; ++layer_index) for (u32 kind = 0; kind < 2; ++kind)
                    if (!decode_compare(actual[layer_index * 2 + kind], prompt_tensor(layer_index, kind ? "v-projection" : "k-rope")->buffer,
                                        4, 256, 128 * 256)) { status("Decode KV parity failed layer", layer_index); return 0; }
                if (!decode_compare(actual[68], prompt_tensor(33, "logits")->buffer, 1, 262208, 262208)) {
                    text("Decode logits parity failed\n"); return 0;
                }
                graph_load(decode_state);
                status("PASS decode KV/logits parity position", position);
            }
            ++position;
        }
    }
    timing("generation us", started);
    if (automatic_text) {
        if (!stopped) return 2;
        u32 size;
        if (!gemma_tokenizer_decode(&tokenizer, generated, generated_count, 1, translation_output,
                                   sizeof(translation_output) - 1, &size) || !size) return 0;
        translation_output[size] = 0;
        return 1;
    }
    if (!translation_emit(generated, generated_count, &emitted, 1)) return 0;
    if (!stopped) { text("Token limit reached; translation may be incomplete\n"); return 2; }
    return 1;
}

static int document_emit(const u8 *bytes, u32 size) {
    if (size > sizeof(document_output) - document_output_size) return 0;
    if (no_stream) memcpy(document_output + document_output_size, bytes, size);
    else if (!translation_write(bytes, size)) return 0;
    document_output_size += size;
    return 1;
}

static int translate_piece(const QnnInterfaceV2 *api, const u8 *embedding, const GemmaArtifactHeader *header,
                           const char *input, u32 size, u32 depth) {
#ifdef GEMMA_GUI
    if (gui_cancelled()) return 0;
#endif
    u32 leading = 0;
    while (leading < size && document_space((u8)input[leading])) ++leading;
    if (leading && !document_emit((const u8 *)input, leading)) return 0;
    input += leading; size -= leading;
    u32 content = size;
    while (content && document_space((u8)input[content - 1])) --content;
    if (content) {
        if (!request_range(input, content)) return 0;
        int result = translate_request(api, embedding, header);
        if (!result) return 0;
        if (result == 2) {
            if (depth >= 20) return 2;
            u32 split = document_boundary(input, content / 2);
            if (!split || split >= content) return 2;
            text("Retrying smaller translation pieces\n");
            result = translate_piece(api, embedding, header, input, split, depth + 1);
            if (result != 1) return result;
            result = translate_piece(api, embedding, header, input + split, content - split, depth + 1);
            if (result != 1) return result;
        } else if (!document_emit(translation_output, length((const char *)translation_output))) return 0;
    }
    return document_emit((const u8 *)input + content, size - content) ? 1 : 0;
}

static int translate_document(const QnnInterfaceV2 *api, const u8 *embedding, const GemmaArtifactHeader *header) {
    if (!automatic_text) return translate_request(api, embedding, header);
    document_output_size = 0;
    u32 size = length(source_text), position = 0;
    while (position < size) {
        u32 first = position;
        while (position < size && document_space((u8)source_text[position])) ++position;
        if (!document_emit((const u8 *)source_text + first, position - first)) return 0;
        if (position == size) break;
        u32 piece = document_piece(source_text + position, size - position);
        if (!piece) return 0;
        int result = translate_piece(api, embedding, header, source_text + position, piece, 0);
        if (result != 1) return result;
        position += piece;
        status("translated source bytes", position);
    }
    if (no_stream && !translation_write(document_output, document_output_size)) return 0;
    return translation_write((const u8 *)"\n", 1) ? 1 : 0;
}

static void translate_quiet_finish(void) {
    if (quiet_sink) {
        SetStdHandle((u32)-12, saved_stderr);
        CloseHandle(quiet_sink);
        quiet_sink = 0;
    }
}

static int translate_release(const QnnInterfaceV2 *api, void (*rpc_free)(void *)) {
    int ok = !decode_context || !api->context_free(decode_context, 0);
    if (selection_context && api->context_free(selection_context, 0)) ok = 0;
    selection_context = 0;
    if (power_active && performance->destroy(power_id)) ok = 0;
    power_active = 0;
    if (execution_profile && api->profile_free(execution_profile)) ok = 0;
    execution_profile = 0;
    decode_context = 0;
    if (decode_shared && rpc_free) rpc_free(decode_shared);
    decode_shared = 0;
    return ok;
}

static int selection_restore(const QnnInterfaceV2 *api, QnnBackendHandle backend, QnnDeviceHandle device) {
    char path[1100]; join(path, binding_path, ".selection.context");
    void *file = CreateFileA(path, 0x80000000U, 1, 0, 3, 0x80U, 0);
    if (file == (void *)(u64)-1) {
        u32 error = GetLastError();
        return error == 2 || error == 3 ? 0 : -1;
    }
    SelectionHeader header; long long size = 0; u8 digest[32];
    int ok = SetFilePointerEx(file, 0, &size, 2) && SetFilePointerEx(file, 0, 0, 0) &&
        transfer_file(file, &header, sizeof(header), 0);
    if (ok) {
        ok = header.magic == 0x31534d47 && header.version == 1 && header.binary_size &&
            header.binary_size <= 16777216 && (u64)size == sizeof(header) + header.binary_size &&
            header.ids[0] != header.ids[1] && header.ids[0] != header.ids[2] && header.ids[1] != header.ids[2];
        for (u32 index = 0; index < sizeof(runtime_version); ++index)
            if (((u8 *)&header.qnn)[index] != ((u8 *)&runtime_version)[index]) ok = 0;
    }
    void *binary = ok ? VirtualAlloc(0, header.binary_size, 0x3000U, 4U) : 0;
    if (!binary || !transfer_file(file, binary, header.binary_size, 0)) ok = 0;
    if (!CloseHandle(file)) ok = 0;
    if (ok) {
        selection_digest(&header, binary, digest);
        for (u32 index = 0; index < 32; ++index) if (digest[index] != header.digest[index]) ok = 0;
    }
    if (ok) {
        ok = !api->context_create_from_binary(backend, device, 0, binary, header.binary_size, &selection_context, 0) &&
            !api->graph_retrieve(selection_context, "selection_test", &block.graph);
        status("selection cache restore", ok ? 0 : 1);
    }
    if (binary && !VirtualFree(binary, 0, 0x8000U)) ok = 0;
    if (!ok) return -1;
    static u32 input_shape[] = {1, 262208}, output_shape[] = {1};
    QnnTensor *tensors[] = {&selection_input, &selection_outputs[0], &selection_outputs[1]};
    for (u32 index = 0; index < 3; ++index) {
        QnnTensor *tensor = tensors[index]; memset(tensor, 0, sizeof(*tensor));
        tensor->version = QNN_TENSOR_VERSION_1;
        tensor->data.v1.id = header.ids[index];
        tensor->data.v1.type = index ? QNN_TENSOR_TYPE_APP_READ : QNN_TENSOR_TYPE_APP_WRITE;
        tensor->data.v1.data_type = index == 1 ? QNN_DATATYPE_INT_32 : QNN_DATATYPE_FLOAT_16;
        tensor->data.v1.rank = index ? 1 : 2;
        tensor->data.v1.dimensions = index ? output_shape : input_shape;
        tensor->data.v1.memory.client_buffer.data_size = index ? (index == 1 ? 4 : 2) : 524416;
        tensor->data.v1.memory.client_buffer.data = index == 1 ? (void *)&selected_token : (void *)&selected_finite;
    }
    return 1;
}

static int translate_run(const QnnInterfaceV2 *api, QnnBackendHandle backend, QnnDeviceHandle device, QnnContextHandle context, i32 fd) {
    if (prompt_bundle && !cpu_selection && prompt_bundle_header.graphs[0].version == 4) {
        PROFILE_START(selection_setup_started);
        u64 selection_started = now();
        int restored = compile_selection ? 0 : selection_restore(api, backend, device);
        if (restored < 0) { text("Invalid selection cache\n"); return 0; }
        if (!restored) {
            if (api->context_create(backend, device, 0, &selection_context)) return 0;
            memset(&block, 0, sizeof(block)); block.api = api; block.internal = 1;
            block.host.allocate = allocate; block.host.status = status;
            if (api->graph_create(selection_context, "greedy_selection", 0, &block.graph)) return 0;
            const u32 shape[] = {1, 262208};
            u32 input = gemma_block_tensor(&block, "test-logits", QNN_TENSOR_TYPE_APP_WRITE,
                                          QNN_DATATYPE_FLOAT_16, shape, 2, 0);
            if (!gemma_block_select(&block, input) || api->graph_finalize(block.graph, 0, 0)) return 0;
            selection_input = block.tensors[input].tensor;
            selection_input.data.v1.memory.client_buffer.data_size = 262208 * sizeof(u16);
            u32 output_count = 0;
            for (u32 index = 0; index < block.count; ++index) {
                GemmaBlockTensor *entry = &block.tensors[index];
                if (entry->tensor.data.v1.type != QNN_TENSOR_TYPE_APP_READ) continue;
                if (output_count == 2) return 0;
                entry->tensor.data.v1.memory.client_buffer.data = output_count ? (void *)&selected_finite : (void *)&selected_token;
                entry->tensor.data.v1.memory.client_buffer.data_size = entry->bytes;
                selection_outputs[output_count++] = entry->tensor;
            }
            if (output_count != 2) return 0;
        }
        timing("selection setup us", selection_started);
        PROFILE_END(PROFILE_DECODE_SETUP, selection_setup_started);
    }
    if (performance_requested) {
        typedef u64 (*GetInfrastructure)(const TranslatePerformance **);
        GetInfrastructure get_infrastructure = (GetInfrastructure)api->device_get_infrastructure;
        if (!get_infrastructure || get_infrastructure(&performance) || !performance || performance->type != 0 ||
            !performance->create || !performance->destroy || !performance->set || performance->create(0, 0, &power_id)) return 0;
        power_active = 1;
        struct {
            u32 option, context_id, set_dcvs, dcvs, mode, set_latency, latency, set_sleep, sleep;
            u32 set_bus, bus_min, bus_target, bus_max, set_core, core_min, core_target, core_max;
        } config = {0};
        _Static_assert(sizeof(config) == 68, "QNN HTP power config ABI");
        config.option = 1; config.context_id = power_id; config.set_dcvs = 1; config.mode = 0x10;
        config.set_latency = 1; config.latency = 40; config.set_sleep = 1; config.sleep = 1;
        config.set_bus = 1; config.bus_min = 0x80; config.bus_target = 0x80; config.bus_max = 0x80;
        config.set_core = 1; config.core_min = 0x80; config.core_target = 0x80; config.core_max = 0x80;
        const void *configs[] = {&config, 0};
        u64 code = performance->set(power_id, configs);
        status("performance vote", code);
        if (code) return 0;
    }
    if (qnn_profile_requested && (!api->profile_create || !api->profile_free || !api->profile_get_events ||
        !api->profile_get_sub_events || !api->profile_get_event_data || api->profile_create(backend, (u32)qnn_profile_requested, &execution_profile))) return 0;
    PROFILE_START(embedding_started);
    GemmaArtifactHeader header;
    const u8 *embedding = artifact("language_model.model.embed_tokens.weight", &header);
    PROFILE_END(PROFILE_EMBEDDING_LOAD, embedding_started);
    if (!embedding || header.element_type != GEMMA_ARTIFACT_ELEMENT_S4 || header.rank != 2 ||
        header.dimensions[0] != 262208 || header.dimensions[1] != 2560 || header.group_size != 2560) return 0;
    PROFILE_START(buffers_started);
    if (!prompt_buffers(api, context, fd, 512)) return 0;
    prefill_state = allocate(0, sizeof(*prefill_state));
    if (!prefill_state) return 0;
    graph_save(prefill_state);
    if (prompt_bundle) {
        for (u32 layer_index = 0; layer_index < 34; ++layer_index) {
            GemmaBlock *saved = &prefill_state->blocks[layer_index];
            for (u32 index = 0; index < saved->count; ++index) {
                GemmaBlockTensor *entry = &saved->tensors[index];
                if (equal(entry->name + 9, "past-key")) bundle_past[layer_index * 2] = entry;
                if (equal(entry->name + 9, "past-value")) bundle_past[layer_index * 2 + 1] = entry;
            }
        }
    }
    PROFILE_END(PROFILE_BUFFERS, buffers_started);
    if (!padded_decode) {
        PROFILE_START(decode_setup_started);
        typedef void *(*RpcAlloc)(i32, u32, i32);
        typedef i32 (*RpcFd)(void *);
        u8 *prefill_shared = shared; u64 prefill_bytes = prompt_shared_bytes;
        void *rpc = LoadLibraryA("libcdsprpc.dll");
        if (!rpc) rpc = LoadLibraryA("libadsprpc.dll");
        if (!rpc) return 0;
        RpcAlloc rpc_alloc = (RpcAlloc)GetProcAddress(rpc, "rpcmem_alloc");
        RpcFd rpc_fd = (RpcFd)GetProcAddress(rpc, "rpcmem_to_fd");
        if (!rpc_alloc || !rpc_fd) { FreeLibrary(rpc); return 0; }
        prompt_rows = 1; memset(prompt_blocks, 0, sizeof(prompt_blocks));
        int ok = prompt_bundle ? prompt_bind(api, context, &prompt_bundle_header.graphs[1]) :
            (!api->context_create(backend, device, 0, &decode_context) &&
             prompt_restore(api, backend, device, &decode_context, 512));
        prompt_shared_bytes = 34ULL * (2 * (4 * 512 * 512 + 4096) + 2 * (4096 + 4096));
        if (prompt_bundle) prompt_shared_bytes = 34ULL * 2 * (4096 + 4096);
        if (ok) decode_shared = rpc_alloc(25, 1, (i32)prompt_shared_bytes);
        if (!decode_shared || (u64)decode_shared % 4096) ok = 0;
        if (ok) {
            shared = decode_shared; memset(shared, 0xa5, prompt_shared_bytes);
            prompt_input_count = 0; prompt_output_count = 0;
            ok = prompt_buffers(api, prompt_bundle ? context : decode_context, rpc_fd(shared), 512);
        }
        shared = prefill_shared; prompt_shared_bytes = prefill_bytes;
        if (!FreeLibrary(rpc)) ok = 0;
        if (!ok) return 0;
        decode_state = allocate(0, sizeof(*decode_state));
        if (!decode_state) return 0;
        graph_save(decode_state);
        PROFILE_END(PROFILE_DECODE_SETUP, decode_setup_started);
    }
    int final_result = 1;
#ifdef GEMMA_GUI
    (void)final_result;
    gui_ready();
#endif
    for (;;) {
#ifdef GEMMA_GUI
        if (!gui_next()) return 1;
        if (!request_prepare()) { gui_done(-1); continue; }
#endif
        u32 checkpoint = allocation_count;
        u64 started = now();
        int result = translate_document(api, embedding, &header);
        timing("request us", started);
        while (allocation_count > checkpoint) if (!VirtualFree(allocations[--allocation_count], 0, 0x8000U)) return 0;
    #ifdef GEMMA_GUI
        gui_done(result);
        if (!result) return 0;
    #else
        if (!result) return 0;
        if (result == 2) final_result = 2;
        if (!batch_mode) return final_result;
        int next = batch_read();
        if (!next) return final_result;
        if (next < 0 || !request_prepare()) { text("Invalid batch request\n"); return 0; }
    #endif
    }
}