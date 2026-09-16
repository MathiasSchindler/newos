#define GEMMA_TRANSLATE 1
#include "../../../tools/test-gemma-block.c"

__declspec(dllimport) const u16 *GetCommandLineW(void);
__declspec(dllimport) int WideCharToMultiByte(u32, u32, const u16 *, int, char *, int, const char *, int *);
__declspec(dllimport) u32 GetModuleFileNameW(void *, u16 *, u32);
__declspec(dllimport) int GetConsoleMode(void *, u32 *);
__declspec(dllimport) int MultiByteToWideChar(u32, u32, const char *, int, u16 *, int);
__declspec(dllimport) int WriteConsoleW(void *, const u16 *, u32, u32 *, void *);

static char arguments[16][32768];
static char tokenizer_path[1024];
static const char *source_language, *target_language, *source_text;
static u32 maximum_tokens = 64;
static GemmaTokenizer tokenizer;
static u32 request_tokens[512], request_count;

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
    int count = parse_arguments(GetCommandLineW());
    if (count == 2 && equal(arguments[1], "--help")) {
        text("Usage: translate.exe --from de --to en [--max-tokens 64] TEXT\n"
             "Optional: --bindings PATH --tokenizer PATH\n"
             "Experimental W4 NPU runtime; 512 total tokens. Exit 2 means token limit.\n");
        ExitProcess(0);
    }
    u32 size = GetModuleFileNameW(0, executable, 1024);
    if (!size || size >= 1024 || !WideCharToMultiByte(65001, 0x80U, executable, -1, directory, sizeof(directory), 0, 0)) return 0;
    for (u32 index = 0; directory[index]; ++index) if (directory[index] == '\\' || directory[index] == '/') last = index + 1;
    if (!last || last > 800) return 0;
    directory[last] = 0;
    join(binding_path, directory, "gemma-block/prompt-512.gmb");
    join(tokenizer_path, directory, "../models/translategemma-4b-stage4/tokenizer.gta");
    for (int index = 1; index < count; ++index) {
        const char *option = arguments[index];
        if (equal(option, "--from") || equal(option, "--to") || equal(option, "--max-tokens") ||
            equal(option, "--bindings") || equal(option, "--tokenizer")) {
            if (++index == count) return 0;
            const char *value = arguments[index];
            if (equal(option, "--from")) { if (source_language) return 0; source_language = value; }
            else if (equal(option, "--to")) { if (target_language) return 0; target_language = value; }
            else if (equal(option, "--max-tokens")) {
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
    if (count < 0 || !source_language || !target_language || !source_text || !*source_text) {
        text("Expected: translate.exe --from de --to en TEXT\n"); return 0;
    }
    u32 bytes; u8 *data = read_file(tokenizer_path, &bytes);
    GemmaTokenizerWork *work = allocate(0, sizeof(*work));
    if (!data || !work || !gemma_tokenizer_open(&tokenizer, data, bytes) ||
        !gemma_tokenizer_prompt(&tokenizer, work, source_language, target_language,
            (const u8 *)source_text, length(source_text), maximum_tokens, request_tokens, 512, &request_count) ||
        request_count + maximum_tokens > 512) {
        text("Invalid language, tokenizer, UTF-8 text, or 512-token request budget\n"); return 0;
    }
    join(failure_point, "restore-512", "");
    return 1;
}

static int translate_step(const QnnInterfaceV2 *api, const u8 *embedding_weights,
                           const GemmaArtifactHeader *header, u32 position, const u32 *tokens, u32 count) {
    u16 *embedding = prompt_tensor(0, "input")->buffer;
    float *positions = prompt_tensor(0, "positions")->buffer;
    memset(embedding, 0, 128U * 2560U * 2U);
    for (u32 row = 0; row < 128; ++row) positions[row] = row < count ? (float)(position + row) : 0;
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
        for (u32 head = 0; head < 8; ++head) for (u32 row = 0; row < 128; ++row) for (u32 key = 0; key < 640; ++key) {
            u32 absolute = key < 512 ? key : position + key - 512;
            int occupied = key < 512 ? key < position : key < 512 + count;
            int visible = occupied && gemma_numeric_visible(owner, (u32)positions[row], absolute);
            mask[(head * 128 + row) * 640 + key] = visible ? 0 : 0xfbffU;
        }
    }
    *(float *)prompt_tensor(33, "last-token")->buffer = (float)(count - 1);
    u64 code = api->graph_execute(prompt_blocks[0].graph, prompt_inputs, prompt_input_count, prompt_outputs, prompt_output_count, 0, 0);
    if (code) { status("translation graphExecute", code); return 0; }
    for (u32 layer_index = 0; layer_index < 34; ++layer_index) for (u32 kind = 0; kind < 2; ++kind) {
        GemmaBlockTensor *current = prompt_tensor(layer_index, kind ? "v-projection" : "k-rope");
        GemmaBlockTensor *past = prompt_tensor(layer_index, kind ? "past-value" : "past-key");
        for (u32 byte = 0; byte < 4096; ++byte)
            if (((u8 *)current->buffer)[current->bytes + byte] != 0xa5 || ((u8 *)past->buffer)[past->bytes + byte] != 0xa5) return 0;
        for (u32 head = 0; head < 4; ++head)
            memcpy((u16 *)past->buffer + (head * 512 + position) * 256,
                   (u16 *)current->buffer + head * 128 * 256, count * 512);
    }
    return 1;
}

static int translate_run(const QnnInterfaceV2 *api, QnnContextHandle context, i32 fd) {
    GemmaArtifactHeader header;
    const u8 *embedding = artifact("language_model.model.embed_tokens.weight", &header);
    if (!embedding || header.element_type != GEMMA_ARTIFACT_ELEMENT_S4 || header.rank != 2 ||
        header.dimensions[0] != 262208 || header.dimensions[1] != 2560 || header.group_size != 2560 ||
        !prompt_buffers(api, context, fd, 512)) return 0;
    u32 generated[256], generated_count = 0, position = 0;
    u64 started = now(); int stopped = 0;
    while (position < request_count) {
        u32 count = request_count - position < 128 ? request_count - position : 128;
        if (!translate_step(api, embedding, &header, position, request_tokens + position, count)) return 0;
        position += count;
    }
    timing("prefill us", started); started = now();
    while (generated_count < maximum_tokens) {
        u16 *logits = prompt_tensor(33, "logits")->buffer;
        u32 best = 0; float maximum = -65505.0f;
        for (u32 token = 0; token < 262208; ++token) {
            float value = gemma_numeric_f16(logits[token]);
            if (!(value >= -65504 && value <= 65504)) return 0;
            if (value > maximum) { maximum = value; best = token; }
        }
        if (best >= 262145) return 0;
        generated[generated_count++] = best;
        status("generated token", best);
        if (gemma_model_is_stop_token(best)) { stopped = 1; break; }
        if (generated_count < maximum_tokens) {
            if (position >= 512 || !translate_step(api, embedding, &header, position, &best, 1)) return 0;
            ++position;
        }
    }
    timing("generation us", started);
    u8 *output = allocate(0, GEMMA_TOKENIZER_MAX_BYTES); u32 output_size, written;
    if (!output || !gemma_tokenizer_decode(&tokenizer, generated, generated_count, 1, output,
        GEMMA_TOKENIZER_MAX_BYTES, &output_size) || !output_size || !gemma_utf8_valid(output, output_size)) return 0;
    void *destination = GetStdHandle((u32)-11); u32 console_mode;
    if (GetConsoleMode(destination, &console_mode)) {
        u16 *wide = allocate(0, GEMMA_TOKENIZER_MAX_BYTES * 2U);
        int wide_size = wide ? MultiByteToWideChar(65001, 8, (const char *)output, (int)output_size,
                                                  wide, GEMMA_TOKENIZER_MAX_BYTES) : 0;
        if (wide_size <= 0) return 0;
        for (u32 offset = 0; offset < (u32)wide_size;) {
            u32 count = (u32)wide_size - offset;
            if (count > 16384) count = 16384;
            if (offset + count < (u32)wide_size && wide[offset + count - 1] >= 0xd800 && wide[offset + count - 1] <= 0xdbff) --count;
            if (!WriteConsoleW(destination, wide + offset, count, &written, 0) || !written) return 0;
            offset += written;
        }
        const u16 newline[] = {'\n'};
        if (!WriteConsoleW(destination, newline, 1, &written, 0) || written != 1) return 0;
    } else if (!WriteFile(destination, output, output_size, &written, 0) || written != output_size ||
               !WriteFile(destination, "\n", 1, &written, 0) || written != 1) return 0;
    if (!stopped) { text("Token limit reached; translation may be incomplete\n"); return 2; }
    return 1;
}