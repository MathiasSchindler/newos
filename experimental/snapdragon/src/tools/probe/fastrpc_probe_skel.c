typedef unsigned int u32;
typedef unsigned long long u64;
typedef __SIZE_TYPE__ usize;

typedef union RemoteArg {
    struct { void *data; usize size; } buffer;
    u32 handle;
    u64 handle64;
    struct { int fd; u32 offset; } dma;
} RemoteArg;

#ifndef FASTRPC_SKEL_TEST
_Static_assert(sizeof(void *) == 4 && sizeof(RemoteArg) == 8, "Hexagon FastRPC ABI");
#endif

int fastrpc_probe_skel_invoke(u64 handle, u32 scalars, RemoteArg *arguments) {
    u32 method = (scalars >> 24) & 31U;
    const u32 *input;
    u32 *output;
    (void)handle;
    if (method == 0 || method == 1) return 0;
    if (scalars != 0x02010100U || !arguments ||
        !arguments[0].buffer.data || !arguments[1].buffer.data ||
        arguments[0].buffer.size != 64 || arguments[1].buffer.size != 68 ||
        ((usize)arguments[0].buffer.data & 3U) || ((usize)arguments[1].buffer.data & 3U)) return 14;
    input = (const u32 *)arguments[0].buffer.data;
    output = (u32 *)arguments[1].buffer.data;
    for (u32 index = 0; index < 16; ++index) {
        output[index] = (input[index] * 1664525U + 1013904223U) ^ (0x9e3779b9U + index);
    }
    output[16] = 0x46525043U;
    return 0;
}

#ifdef FASTRPC_SKEL_TEST
__declspec(dllimport) void ExitProcess(u32);

void mainCRTStartup(void) {
    u32 input[16];
    u32 output[17];
    RemoteArg arguments[2];
    arguments[0].buffer.data = input;
    arguments[0].buffer.size = sizeof(input);
    arguments[1].buffer.data = output;
    arguments[1].buffer.size = sizeof(output);
    for (u32 trial = 0; trial < 3; ++trial) {
        for (u32 index = 0; index < 16; ++index) input[index] = trial * 65537U + index;
        for (u32 index = 0; index < 17; ++index) output[index] = 0xdeadbeefU;
        if (fastrpc_probe_skel_invoke(0, 0x02010100U, arguments)) ExitProcess(1);
        for (u32 index = 0; index < 16; ++index) {
            if (output[index] != ((input[index] * 1664525U + 1013904223U) ^ (0x9e3779b9U + index))) ExitProcess(2);
        }
        if (output[16] != 0x46525043U) ExitProcess(3);
    }
    if (fastrpc_probe_skel_invoke(0, 0x02010100U, 0) != 14) ExitProcess(4);
    if (fastrpc_probe_skel_invoke(0, 0x03010100U, arguments) != 14) ExitProcess(5);
    arguments[0].buffer.size = 60;
    if (fastrpc_probe_skel_invoke(0, 0x02010100U, arguments) != 14) ExitProcess(6);
    arguments[0].buffer.size = 64;
    arguments[1].buffer.size = 64;
    if (fastrpc_probe_skel_invoke(0, 0x02010100U, arguments) != 14) ExitProcess(7);
    arguments[1].buffer.size = 68;
    arguments[1].buffer.data = (char *)output + 1;
    if (fastrpc_probe_skel_invoke(0, 0x02010100U, arguments) != 14) ExitProcess(8);
    arguments[1].buffer.data = 0;
    if (fastrpc_probe_skel_invoke(0, 0x02010100U, arguments) != 14) ExitProcess(9);
    ExitProcess(0);
}
#endif