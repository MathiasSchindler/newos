#include "gemma_tokenizer.h"
#include "crypto/sha256.h"

typedef unsigned long long TestSize;

__declspec(dllimport) void *GetStdHandle(unsigned int identifier);
__declspec(dllimport) char *GetCommandLineA(void);
__declspec(dllimport) void *CreateFileA(const char *, unsigned int, unsigned int, void *, unsigned int, unsigned int, void *);
__declspec(dllimport) int SetFilePointerEx(void *, long long, long long *, unsigned int);
__declspec(dllimport) int ReadFile(void *, void *, unsigned int, unsigned int *, void *);
__declspec(dllimport) int WriteFile(void *, const void *, unsigned int, unsigned int *, void *);
__declspec(dllimport) int CloseHandle(void *);
__declspec(dllimport) void *VirtualAlloc(void *, TestSize, unsigned int, unsigned int);
__declspec(dllimport) int VirtualFree(void *, TestSize, unsigned int);
__declspec(dllimport) void ExitProcess(unsigned int);

void *memset(void *destination, int value, TestSize size) {
    unsigned char *bytes = destination;
    TestSize index;
    for (index = 0; index < size; ++index) bytes[index] = (unsigned char)value;
    return destination;
}

void *memcpy(void *destination, const void *source, TestSize size) {
    unsigned char *output = destination;
    const unsigned char *input = source;
    TestSize index;
    for (index = 0; index < size; ++index) output[index] = input[index];
    return destination;
}

static unsigned int tokens[2048];
static unsigned int decode_tokens[2048];
static unsigned char decoded[GEMMA_TOKENIZER_MAX_BYTES];
int gemma_numeric_test(const unsigned char *artifact, unsigned int size, unsigned int *passed);
static char arguments[4][1024];
static unsigned int argument_count;
static unsigned int current_case;

static unsigned int read_u32(const unsigned char *bytes) {
    return bytes[0] | ((unsigned int)bytes[1] << 8) |
        ((unsigned int)bytes[2] << 16) | ((unsigned int)bytes[3] << 24);
}

static int equal(const unsigned char *left, const unsigned char *right, unsigned int size) {
    unsigned int index;
    for (index = 0; index < size; ++index) if (left[index] != right[index]) return 0;
    return 1;
}

static void report(const char *message, unsigned int value) {
    unsigned int size = 0, written, digits = 0;
    char number[16], output[160];
    while (message[size] && size < 128U) { output[size] = message[size]; ++size; }
    do { number[digits++] = (char)('0' + value % 10U); value /= 10U; } while (value);
    while (digits) output[size++] = number[--digits];
    output[size++] = '\n';
    WriteFile(GetStdHandle((unsigned int)-12), output, size, &written, 0);
}

static int parse_arguments(void) {
    const char *command = GetCommandLineA();
    unsigned int count = 0;
    while (*command) {
        unsigned int size = 0;
        int quoted = 0;
        while (*command == ' ' || *command == '\t') ++command;
        if (!*command) break;
        if (count == 4U) return 0;
        while (*command && (quoted || (*command != ' ' && *command != '\t'))) {
            if (*command == '"') { quoted = !quoted; ++command; continue; }
            if (size == sizeof(arguments[0]) - 1U) return 0;
            arguments[count][size++] = *command++;
        }
        if (quoted) return 0;
        arguments[count++][size] = 0;
    }
    argument_count = count;
    return count == 3U || count == 4U;
}

static unsigned char *read_artifact(const char *path, unsigned int *size) {
    void *file = CreateFileA(path, 0x80000000U, 1U, 0, 3U, 0x80U, 0);
    long long length;
    unsigned int offset = 0;
    unsigned char *data = 0;
    if (file == (void *)(TestSize)-1) return 0;
    if (!SetFilePointerEx(file, 0, &length, 2U) || length < 256 ||
        length > GEMMA_TOKENIZER_MAX_ARTIFACT_BYTES || !SetFilePointerEx(file, 0, 0, 0)) goto done;
    data = VirtualAlloc(0, (TestSize)length, 0x3000U, 4U);
    if (!data) goto done;
    while (offset < (unsigned int)length) {
        unsigned int received;
        if (!ReadFile(file, data + offset, (unsigned int)length - offset, &received, 0) || !received) {
            VirtualFree(data, 0, 0x8000U); data = 0; goto done;
        }
        offset += received;
    }
    *size = offset;
done:
    CloseHandle(file);
    return data;
}

static int stable_prefixes(const GemmaTokenizer *tokenizer, const unsigned int *input,
                           unsigned int count, int skip_special,
                           const unsigned char *expected, unsigned int expected_size) {
    unsigned int previous = 0;
    for (unsigned int prefix = 0; prefix <= count; ++prefix) {
        unsigned int size;
        if (!gemma_tokenizer_decode_stable(tokenizer, input, prefix, skip_special, decoded, sizeof(decoded), &size) ||
            size < previous || size > expected_size || !equal(decoded, expected, size) ||
            !gemma_utf8_valid(decoded, size)) return 0;
        previous = size;
    }
    return 1;
}

static int fixtures(const GemmaTokenizer *tokenizer, GemmaTokenizerWork *work,
                    const unsigned char *artifact, unsigned int size) {
    GemmaArtifactHeader header;
    unsigned int offset = 268U, case_count;
    if (size < offset || !gemma_artifact_decode_header(artifact, &header) ||
        !gemma_artifact_header_valid(&header, gemma_model_translategemma_4b()) ||
        !gemma_artifact_header_matches_name(&header, "fixture/tokenizer-v1") ||
        header.kind != GEMMA_ARTIFACT_KIND_FIXTURE || header.payload_size != size - 256U ||
        !gemma_artifact_payload_valid(&header, artifact + 256U, size - 256U) ||
        read_u32(artifact + 256U) != 0x34524647U || read_u32(artifact + 260U) != 1U) return 1;
    case_count = read_u32(artifact + 264U);
    if (!case_count || case_count > 100000U) return 2;
    {
        unsigned int split[] = {tokenizer->byte_ids[0xc3], 1, tokenizer->byte_ids[0xbc], 2};
        unsigned int malformed[] = {tokenizer->byte_ids['A'], tokenizer->byte_ids[0xff], 2};
        unsigned int incomplete[] = {tokenizer->byte_ids[0xc3]};
        const unsigned char valid[] = {0xc3, 0xbc};
        const unsigned char replacements[] = {0xef, 0xbf, 0xbd, 0xef, 0xbf, 0xbd};
        unsigned int output_size;
        if (!stable_prefixes(tokenizer, split, 4, 1, valid, sizeof(valid)) ||
            !stable_prefixes(tokenizer, malformed, 3, 1, replacements, sizeof(replacements)) ||
            !stable_prefixes(tokenizer, incomplete, 1, 1, replacements, 3) ||
            !gemma_tokenizer_decode(tokenizer, split, 4, 1, decoded, sizeof(decoded), &output_size) ||
            output_size != sizeof(valid) || !equal(decoded, valid, sizeof(valid)) ||
            !gemma_tokenizer_decode(tokenizer, malformed, 3, 1, decoded, sizeof(decoded), &output_size) ||
            output_size != sizeof(replacements) || !equal(decoded, replacements, sizeof(replacements))) return 15;
        report("PASS byte-fallback streaming cases: ", 3);
    }
    for (current_case = 0; current_case < case_count; ++current_case) {
        unsigned int kind, flags, maximum, source_size, target_size, input_size, expected_count, expected_size;
        unsigned int count = 0, output_size = 0, index;
        const unsigned char *input, *expected_tokens, *expected_text;
        char source[32], target[32];
        TestSize total;
        int success;
        if (size - offset < 32U) return 3;
        kind = read_u32(artifact + offset); flags = read_u32(artifact + offset + 4U);
        maximum = read_u32(artifact + offset + 8U); source_size = read_u32(artifact + offset + 12U);
        target_size = read_u32(artifact + offset + 16U); input_size = read_u32(artifact + offset + 20U);
        expected_count = read_u32(artifact + offset + 24U); expected_size = read_u32(artifact + offset + 28U);
        offset += 32U;
        total = source_size + (TestSize)target_size + input_size + expected_count * 4ULL + expected_size;
        if (source_size >= 32U || target_size >= 32U || expected_count > 2048U || total > size - offset) return 4;
        for (index = 0; index < source_size; ++index) source[index] = (char)artifact[offset++];
        source[source_size] = 0;
        for (index = 0; index < target_size; ++index) target[index] = (char)artifact[offset++];
        target[target_size] = 0;
        input = artifact + offset; offset += input_size;
        expected_tokens = artifact + offset; offset += expected_count * 4U;
        expected_text = artifact + offset; offset += expected_size;
        if (kind == 1U || kind == 4U) {
            success = gemma_tokenizer_encode(tokenizer, work, input, input_size, flags & 1U, tokens, 2048, &count);
        } else if (kind == 2U || kind == 5U) {
            success = gemma_tokenizer_prompt(tokenizer, work, source, target, input, input_size, maximum, tokens, 2048, &count);
        } else if (kind == 3U || kind == 6U) {
            for (index = 0; index < expected_count; ++index) decode_tokens[index] = read_u32(expected_tokens + index * 4U);
            success = gemma_tokenizer_decode(tokenizer, decode_tokens, expected_count, flags & 2U, decoded, sizeof(decoded), &output_size);
            if (kind == 6U) { if (success || output_size) return 5; continue; }
            if (!success || output_size != expected_size || !equal(decoded, expected_text, expected_size)) return 6;
            if (!stable_prefixes(tokenizer, decode_tokens, expected_count, flags & 2U, expected_text, expected_size)) return 16;
            continue;
        } else return 7;
        if (kind == 4U || kind == 5U) { if (success || count) return 8; continue; }
        if (!success || count != expected_count) {
            report("Expected token count: ", expected_count); report("Actual token count: ", count); return 9;
        }
        for (index = 0; index < count; ++index) {
            if (tokens[index] != read_u32(expected_tokens + index * 4U)) {
                report("Token position: ", index); report("Expected token: ", read_u32(expected_tokens + index * 4U));
                report("Actual token: ", tokens[index]); return 10;
            }
        }
        if (!gemma_tokenizer_decode(tokenizer, tokens, count, flags & 2U, decoded, sizeof(decoded), &output_size) ||
            output_size != expected_size || !equal(decoded, expected_text, expected_size) ||
            !gemma_utf8_valid(decoded, output_size)) return 11;
        if (!stable_prefixes(tokenizer, tokens, count, flags & 2U, expected_text, expected_size)) return 16;
        if (output_size && gemma_tokenizer_decode(tokenizer, tokens, count, flags & 2U,
                                                decoded, output_size - 1U, &output_size)) return 12;
        if (count && kind == 1U && gemma_tokenizer_encode(tokenizer, work, input, input_size,
                flags & 1U, tokens, count - 1U, &count)) return 13;
    }
    if (offset != size) return 14;
    report("PASS tokenizer reference cases: ", case_count);
    return 0;
}

static int corruption(unsigned char *artifact, unsigned int size) {
    GemmaTokenizer tokenizer;
    unsigned int index;
    static const unsigned int envelope_offsets[] = {0, 8, 16, 28, 44, 96, 160, 192, 224};
    static const unsigned int payload_offsets[] = {256, 260, 264, 284, 288, 296};
    if (gemma_tokenizer_open(&tokenizer, artifact, size - 1U) ||
        gemma_tokenizer_open(&tokenizer, artifact, size + 1ULL)) return 1;
    for (index = 0; index < sizeof(envelope_offsets) / sizeof(envelope_offsets[0]); ++index) {
        unsigned int offset = envelope_offsets[index];
        artifact[offset] ^= 1U;
        if (gemma_tokenizer_open(&tokenizer, artifact, size)) return 2;
        artifact[offset] ^= 1U;
    }
    artifact[size - 1U] ^= 1U;
    if (gemma_tokenizer_open(&tokenizer, artifact, size)) return 3;
    artifact[size - 1U] ^= 1U;
    for (index = 0; index < sizeof(payload_offsets) / sizeof(payload_offsets[0]); ++index) {
        unsigned int offset = payload_offsets[index];
        unsigned char saved = artifact[offset];
        artifact[offset] = 0xffU;
        crypto_sha256_hash(artifact + 256U, size - 256U, artifact + 224U);
        if (gemma_tokenizer_open(&tokenizer, artifact, size)) return 4;
        artifact[offset] = saved;
    }
    crypto_sha256_hash(artifact + 256U, size - 256U, artifact + 224U);
    if (!gemma_tokenizer_open(&tokenizer, artifact, size)) return 5;
    report("PASS tokenizer corruption cases: ", 18);
    return 0;
}

void mainCRTStartup(void) {
    unsigned int table_size = 0, fixture_size = 0, result = 1;
    unsigned char *table = 0, *reference = 0;
    GemmaTokenizerWork *work = 0;
    GemmaTokenizer tokenizer;
    {
        static const char *messages[] = {"", "abc", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"};
        static const char *expected[] = {
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
        };
        static const char hex[] = "0123456789abcdef";
        for (unsigned int test = 0; test < 3; ++test) {
            CryptoSha256Context hash; unsigned char digest[32];
            crypto_sha256_init(&hash);
            for (unsigned int offset = 0; messages[test][offset]; ++offset)
                crypto_sha256_update(&hash, (const unsigned char *)messages[test] + offset, 1);
            crypto_sha256_final(&hash, digest);
            for (unsigned int byte = 0; byte < 32; ++byte)
                if (hex[digest[byte] >> 4] != expected[test][byte * 2] || hex[digest[byte] & 15] != expected[test][byte * 2 + 1]) {
                    report("SHA-256 known-answer failure: ", test); goto done;
                }
        }
        report("PASS SHA-256 known-answer cases: ", 3);
        {
            unsigned char data[1000], digest[32]; CryptoSha256Context hash;
            const char *million = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
            for (unsigned int index = 0; index < sizeof(data); ++index) data[index] = 'a';
            crypto_sha256_init(&hash);
            crypto_sha256_update(&hash, data, 1);
            for (unsigned int block_index = 0; block_index < 999; ++block_index)
                crypto_sha256_update(&hash, data, sizeof(data));
            crypto_sha256_update(&hash, data, 999);
            crypto_sha256_final(&hash, digest);
            for (unsigned int byte = 0; byte < 32; ++byte)
                if (hex[digest[byte] >> 4] != million[byte * 2] || hex[digest[byte] & 15] != million[byte * 2 + 1]) {
                    report("SHA-256 split million-byte failure: ", 1); goto done;
                }
            report("PASS SHA-256 split million-byte vector: ", 1);
        }
    }
    if (!gemma_model_is_stop_token(1U) || !gemma_model_is_stop_token(106U) ||
        gemma_model_is_stop_token(0U) || gemma_model_is_stop_token(2U)) goto done;
    if (!parse_arguments()) { report("Usage: test-gemma-tokenizer.exe tokenizer.gta tokenizer-fixtures.gta; error ", 1); goto done; }
    table = read_artifact(arguments[1], &table_size);
    reference = read_artifact(arguments[2], &fixture_size);
    if (!table || !reference) { report("Cannot read tokenizer artifacts: ", 1); goto done; }
    if (!gemma_tokenizer_open(&tokenizer, table, table_size)) { report("Tokenizer validation failed: ", 1); goto done; }
    work = VirtualAlloc(0, sizeof(*work), 0x3000U, 4U);
    if (!work) goto done;
    result = (unsigned int)fixtures(&tokenizer, work, reference, fixture_size);
    if (result) { report("Fixture failure code: ", result); report("Fixture index: ", current_case); goto done; }
    result = (unsigned int)corruption(table, table_size);
    if (result) report("Corruption failure code: ", result);
    report("Tokenizer workspace bytes: ", (unsigned int)sizeof(*work));
    if (!result && argument_count == 4U) {
        unsigned int numeric_size = 0, passed = 0;
        unsigned char *numeric = read_artifact(arguments[3], &numeric_size);
        if (!numeric || !gemma_numeric_test(numeric, numeric_size, &passed)) {
            result = 1;
            report("Numerical fixture failure after cases: ", passed);
        } else {
            report("PASS numerical scalar fixture cases: ", passed);
            numeric[numeric_size - 1U] ^= 1U;
            if (gemma_numeric_test(numeric, numeric_size, &passed)) result = 1;
        }
        if (numeric) VirtualFree(numeric, 0, 0x8000U);
    }
done:
    if (work) VirtualFree(work, 0, 0x8000U);
    if (reference) VirtualFree(reference, 0, 0x8000U);
    if (table) VirtualFree(table, 0, 0x8000U);
    ExitProcess(result);
}