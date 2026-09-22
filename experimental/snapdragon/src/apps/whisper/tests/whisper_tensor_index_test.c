#include "whisper_tensor_index.h"

__declspec(dllimport) void __stdcall ExitProcess(unsigned int status);

static WhisperTensorIndex entries[4];

static int parse(const char *json, unsigned long long bytes, unsigned int expected) {
    unsigned int length = 0;
    unsigned int count = 0;
    while (json[length]) ++length;
    return whisper_tensor_index_parse((const unsigned char *)json, length, entries, 4, &count) &&
        count == expected && whisper_tensor_index_validate(entries, count, bytes);
}

void mainCRTStartup(void) {
    unsigned int status = 0;
    static const char valid[] =
        "{\"__metadata__\":{\"format\":\"pt\"},"
        "\"model.decoder.embed_tokens.weight\":{\"dtype\":\"F32\",\"shape\":[2,3],\"data_offsets\":[0,24]},"
        "\"b\":{\"shape\":[2],\"data_offsets\":[24,28],\"dtype\":\"F16\"}}";
    if (!parse(valid, 28, 2) || entries[0].rank != 2 || entries[0].shape[1] != 3 ||
        entries[1].type != WHISPER_TENSOR_F16) status = 1;
    if (!parse("{\"a\":{\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]}}   ", 4, 1)) status = 8;
    if (parse(valid, 27, 2)) status = 2;
    if (parse("{\"a\":{\"dtype\":\"F32\",\"shape\":[2],\"data_offsets\":[0,4]}}", 4, 1)) status = 3;
    if (parse("{\"a\":{\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]},"
              "\"b\":{\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]}}", 4, 2)) status = 4;
    if (parse("{\"a\":{\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]},"
              "\"a\":{\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[4,8]}}", 8, 2)) status = 5;
    if (parse("{\"a\":{\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]},"
              "\"b\":{\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[8,12]}}", 12, 2)) status = 6;
    if (parse("{\"a\":{\"dtype\":\"BF16\",\"shape\":[1],\"data_offsets\":[0,2]}}", 2, 1)) status = 7;
    if (parse("{\"a\":{\"dtype\":\"F32\",\"shape\":[1],\"data_offsets\":[0,4]}", 4, 1)) status = 9;
    ExitProcess(status);
}