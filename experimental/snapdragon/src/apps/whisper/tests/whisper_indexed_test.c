#include "whisper_indexed.h"
#include "whisper_artifact.h"

__declspec(dllimport) void __stdcall ExitProcess(unsigned int status);

static unsigned char file[64 + 192 + 4];
static unsigned int position;
static int opened;

static void write_u32(unsigned char *bytes, unsigned int value) {
    for (unsigned int index = 0; index < 4; ++index) bytes[index] = (unsigned char)(value >> (index * 8));
}

static void write_u64(unsigned char *bytes, unsigned long long value) {
    for (unsigned int index = 0; index < 8; ++index) bytes[index] = (unsigned char)(value >> (index * 8));
}

int platform_open_read(const char *path) {
    if (path == 0 || path[0] != 't' || path[1]) return -1;
    position = 0;
    opened = 1;
    return 7;
}

long platform_read(int fd, void *output, unsigned long long count) {
    unsigned char *bytes = (unsigned char *)output;
    if (fd != 7 || !opened || position >= sizeof(file)) return 0;
    if (count > 5) count = 5;
    if (count > sizeof(file) - position) count = sizeof(file) - position;
    for (unsigned int index = 0; index < count; ++index) bytes[index] = file[position + index];
    position += (unsigned int)count;
    return (long)count;
}

long long platform_seek(int fd, long long offset, int whence) {
    long long next = offset + (whence == 2 ? (long long)sizeof(file) : 0);
    if (fd != 7 || !opened || next < 0 || (whence != 0 && whence != 2)) return -1;
    position = (unsigned int)next;
    return next;
}

int platform_close(int fd) {
    if (fd != 7 || !opened) return -1;
    opened = 0;
    return 0;
}

static void make_file(void) {
    unsigned char *entry = file + 64;
    for (unsigned int index = 0; index < sizeof(file); ++index) file[index] = 0;
    file[0] = 'W'; file[1] = 'T'; file[2] = 'I'; file[3] = 'N';
    file[4] = 'D'; file[5] = 'E'; file[6] = 'X'; file[7] = '1';
    entry[0] = 'p'; entry[1] = 'r'; entry[2] = 'o'; entry[3] = 'b'; entry[4] = 'e';
    file[256] = 0; file[257] = 0; file[258] = 128; file[259] = 63;
    write_u32(file + 8, 1);
    write_u32(file + 12, 1);
    write_u32(file + 16, 1);
    write_u32(file + 20, 192);
    write_u64(file + 24, 256);
    write_u64(file + 32, 4);
    write_u32(entry + 96, WHISPER_TENSOR_F32);
    write_u32(entry + 100, 1);
    write_u64(entry + 104, 1);
    write_u64(entry + 176, 4);
    write_u64(entry + 184, whisper_artifact_hash_update(
        WHISPER_ARTIFACT_HASH_OFFSET_BASIS, file + 256, 4
    ));
    write_u64(file + 40, whisper_artifact_hash_update(
        WHISPER_ARTIFACT_HASH_OFFSET_BASIS, entry, 192
    ));
    write_u64(file + 48, whisper_artifact_hash_update(
        WHISPER_ARTIFACT_HASH_OFFSET_BASIS, file + 256, 4
    ));
}

void mainCRTStartup(void) {
    static WhisperIndexed indexed;
    const WhisperTensorIndex *tensor;
    float value = 0.0f;
    make_file();
    if (!whisper_indexed_open(&indexed, "t", whisper_model_tiny())) ExitProcess(1);
    tensor = whisper_indexed_find(&indexed, "probe");
    if (tensor == 0 || !whisper_indexed_read(&indexed, tensor, &value, sizeof(value)) ||
        value != 1.0f || whisper_indexed_read(&indexed, tensor, &value, 3) ||
        whisper_indexed_read(&indexed, (const WhisperTensorIndex *)file, &value, sizeof(value))) ExitProcess(2);
    whisper_indexed_close(&indexed);
    file[64] ^= 1;
    if (whisper_indexed_open(&indexed, "t", whisper_model_tiny()) || opened) ExitProcess(3);
    make_file();
    file[256] ^= 1;
    if (whisper_indexed_open(&indexed, "t", whisper_model_tiny()) || opened) ExitProcess(4);
    make_file();
    if (whisper_indexed_open(&indexed, "t", whisper_model_base()) || opened) ExitProcess(5);
    ExitProcess(0);
}