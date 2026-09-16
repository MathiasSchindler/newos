#define GEMMA_DOCUMENT_TEST 1
#define mainCRTStartup document_unused_main
static int document_test_generate(const void *, const void *, const void *);
#include "../src/tools/gemma/translate.c"
#undef mainCRTStartup

static u32 test_calls, test_retries, test_mode, test_checks;
static u8 test_input[2048];

static void check(int condition) {
    ++test_checks;
    if (!condition) { status("FAIL document check", test_checks); ExitProcess(1); }
}

static int document_test_generate(const void *api, const void *embedding, const void *header) {
    (void)api; (void)embedding; (void)header;
    ++test_calls;
    u32 size;
    if (!gemma_tokenizer_decode(&tokenizer, request_tokens, request_count, 0,
                               test_input, sizeof(test_input) - 1, &size)) return 0;
    test_input[size] = 0;
    const char *begin = (const char *)test_input;
    while (*begin && !(begin[0] == '\n' && begin[1] == '\n' && begin[2] == '\n')) ++begin;
    if (!*begin) return 0;
    begin += 3;
    const char *end = begin;
    while (*end && *end != '<') ++end;
    u32 content = (u32)(end - begin);
    if (test_mode == 2 || (test_mode == 1 && content > 12)) { ++test_retries; return 2; }
    memcpy(translation_output, begin, content); translation_output[content] = 0;
    return 1;
}

void mainCRTStartup(void) {
    u32 bytes;
    u8 *data = read_file("experimental/snapdragon/models/translategemma-4b-stage4/tokenizer.gta", &bytes);
    tokenizer_work = allocate(0, sizeof(*tokenizer_work));
    check(data && tokenizer_work && gemma_tokenizer_open(&tokenizer, data, bytes));
    source_language = "de"; target_language = "en"; no_stream = 1;
    const char *fixtures[] = {
        "Guten Tag.\r\n\r\nDie T\xc3\xbcr ist offen.",
        "\t Hallo Welt.  Noch ein Satz. \r\n",
        "Ein langer Satz ohne Satzzeichen mit mehreren Worten und genuegend Inhalt fuer wiederholtes Aufteilen",
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x80\x82\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x80\x82"
    };
    for (u32 mode = 0; mode < 2; ++mode) for (u32 index = 0; index < 4; ++index) {
        test_mode = mode; test_calls = 0; test_retries = 0; document_output_size = 0;
        u32 size = length(fixtures[index]);
        check(translate_piece(0, 0, 0, fixtures[index], size, 0) == 1);
        check(document_output_size == size);
        for (u32 offset = 0; offset < size; ++offset) check(document_output[offset] == (u8)fixtures[index][offset]);
        if (mode) check(test_retries && test_calls > 1);
    }
    char unbroken[1201];
    for (u32 index = 0; index < 1200; ++index) unbroken[index] = "\xe6\x97\xa5"[index % 3];
    unbroken[1200] = 0;
    u32 consumed = 0;
    while (consumed < 1200) {
        u32 piece = document_piece(unbroken + consumed, 1200 - consumed);
        check(piece > 0 && piece <= 256 && piece % 3 == 0);
        check(request_count + maximum_tokens <= 512);
        consumed += piece;
    }
    test_mode = 2; test_calls = 0; document_output_size = 0;
    check(translate_piece(0, 0, 0, "x", 1, 0) == 2 && test_calls == 1 && !document_output_size);
    source_text = " \r\n\t"; check(!request_prepare());
    source_text = "\xff"; check(!request_prepare());
    source_text = "Hallo"; source_language = "invalid"; check(!request_prepare());
    document_output_size = sizeof(document_output); check(!document_emit((const u8 *)"x", 1));
    status("PASS native document checks", test_checks);
    ExitProcess(0);
}