typedef unsigned long long pack_size_t;

#define PACK_MIN_MATCH 3U
#define PACK_PAGE_EXECUTE_READ 0x20UL

typedef struct {
    unsigned char magic[8];
    unsigned long long metadata_rva;
    unsigned long long text_rva;
    unsigned long long text_size;
    unsigned long long text_payload_rva;
    unsigned long long text_payload_size;
    unsigned long long data_rva;
    unsigned long long data_size;
    unsigned long long data_payload_rva;
    unsigned long long data_payload_size;
    unsigned long long entry_rva;
} PackMetadata;

__declspec(dllimport) void __stdcall ExitProcess(unsigned int status);
__declspec(dllimport) int __stdcall FlushInstructionCache(void *process, const void *base, pack_size_t size);
__declspec(dllimport) int __stdcall VirtualProtect(void *address, pack_size_t size, unsigned long new_protect, unsigned long *old_protect);

__attribute__((used, section(".rdata$packmeta")))
volatile const PackMetadata pe_pack_metadata = {
    { 'N', 'P', 'A', 'C', 'K', 'A', '1', '\0' },
    0x1111111111111111ULL,
    0x2222222222222222ULL,
    0x3333333333333333ULL,
    0x4444444444444444ULL,
    0x5555555555555555ULL,
    0x6666666666666666ULL,
    0x7777777777777777ULL,
    0x8888888888888888ULL,
    0x9999999999999999ULL,
    0xaaaaaaaaaaaaaaaaULL
};

static int decode_payload(const unsigned char *payload, unsigned long long payload_size,
                          unsigned char *output, unsigned long long output_size) {
    unsigned long long input_offset = 0ULL;
    unsigned long long output_offset = 0ULL;

    while (output_offset < output_size) {
        unsigned char flags;
        unsigned int bit;
        if (input_offset >= payload_size) return -1;
        flags = payload[input_offset++];
        for (bit = 0U; bit < 8U && output_offset < output_size; ++bit) {
            if ((flags & (unsigned char)(1U << bit)) == 0U) {
                if (input_offset >= payload_size) return -1;
                output[output_offset++] = payload[input_offset++];
            } else {
                unsigned int token;
                unsigned int distance;
                unsigned int length;
                unsigned int index;
                if (input_offset + 2ULL > payload_size) return -1;
                token = (unsigned int)payload[input_offset] |
                        ((unsigned int)payload[input_offset + 1ULL] << 8U);
                input_offset += 2ULL;
                distance = ((token & 0x00ffU) | ((token >> 3U) & 0x1f00U)) + 1U;
                length = ((token >> 8U) & 0x07U) + PACK_MIN_MATCH;
                if ((unsigned long long)distance > output_offset ||
                    (unsigned long long)length > output_size - output_offset) return -1;
                for (index = 0U; index < length; ++index) {
                    output[output_offset] = output[output_offset - (unsigned long long)distance];
                    output_offset += 1ULL;
                }
            }
        }
    }
    return input_offset == payload_size ? 0 : -1;
}

void pePackStartup(void) {
    const volatile PackMetadata *metadata = &pe_pack_metadata;
    unsigned char *image_base = (unsigned char *)metadata - metadata->metadata_rva;
    unsigned char *text = image_base + metadata->text_rva;
    unsigned char *data = image_base + metadata->data_rva;
    const unsigned char *text_payload = image_base + metadata->text_payload_rva;
    const unsigned char *data_payload = image_base + metadata->data_payload_rva;
    unsigned long old_protect = 0UL;
    void (*entry)(void);

    if (decode_payload(text_payload, metadata->text_payload_size, text, metadata->text_size) != 0) {
        ExitProcess(127U);
    }
    if (metadata->data_size != 0ULL &&
        decode_payload(data_payload, metadata->data_payload_size, data, metadata->data_size) != 0) {
        ExitProcess(127U);
    }
    if (!VirtualProtect(text, metadata->text_size, PACK_PAGE_EXECUTE_READ, &old_protect) ||
        !FlushInstructionCache((void *)(long long)-1, text, metadata->text_size)) {
        ExitProcess(127U);
    }
    entry = (void (*)(void))(image_base + metadata->entry_rva);
    entry();
    ExitProcess(127U);
}