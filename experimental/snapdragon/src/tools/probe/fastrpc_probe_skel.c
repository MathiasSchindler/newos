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

#if defined(FASTRPC_HMX) && !defined(FASTRPC_SKEL_TEST)
#include <HAP_compute_res.h>
#include <HAP_power.h>
#pragma weak HAP_power_set
#pragma weak HAP_power_destroy_client
#endif

#if defined(FASTRPC_HMX) || defined(FASTRPC_SKEL_TEST)
typedef struct HmxReply {
    u32 detail[12];
    unsigned short product[1024];
} HmxReply;
_Static_assert(sizeof(HmxReply) == 2096, "HMX reply ABI");

static int invoke_hmx(RemoteArg *arguments) {
    HmxReply *reply;
    if (!arguments || !arguments[1].buffer.data || arguments[1].buffer.size != sizeof(HmxReply) ||
        ((usize)arguments[1].buffer.data & 3U)) return 14;
    reply = (HmxReply *)arguments[1].buffer.data;
    for (u32 index = 0; index < 12; ++index) reply->detail[index] = 0;
    reply->detail[0] = 0x484d5831U;
    if (!arguments[0].buffer.data || arguments[0].buffer.size != 4096 ||
        ((usize)arguments[0].buffer.data & 1U)) {
        reply->detail[2] = 14;
        return 0;
    }
#ifdef FASTRPC_SKEL_TEST
    return 20;
#else
    const unsigned short *input = (const unsigned short *)arguments[0].buffer.data;
    compute_res_attr_t attributes;
    HAP_power_request_t power;
    u32 context = 0;
    int power_client = 0, locked = 0, status;
    unsigned short *vtcm;
    reply->detail[1] = 1;
    if (!HAP_power_set || !HAP_power_destroy_client ||
        !compute_resource_attr_set_vtcm_param || !compute_resource_attr_set_hmx_param ||
        !compute_resource_attr_get_vtcm_ptr || !compute_resource_acquire ||
        !compute_resource_release || !compute_resource_hmx_lock || !compute_resource_hmx_unlock) {
        reply->detail[2] = 20;
        return 0;
    }
    reply->detail[1] = 2;
    status = HAP_compute_res_attr_init(&attributes);
    if (status) { reply->detail[2] = (u32)status; return 0; }
    reply->detail[1] = 3;
    status = HAP_compute_res_attr_set_vtcm_param(&attributes, 8192, 1);
    if (status) { reply->detail[2] = (u32)status; return 0; }
    reply->detail[1] = 4;
    status = HAP_compute_res_attr_set_hmx_param(&attributes, 1);
    if (status) { reply->detail[2] = (u32)status; return 0; }
    reply->detail[1] = 5;
    context = HAP_compute_res_acquire(&attributes, 1000000);
    reply->detail[3] = context != 0;
    if (!context) { reply->detail[2] = 2; return 0; }
    vtcm = (unsigned short *)HAP_compute_res_attr_get_vtcm_ptr(&attributes);
    reply->detail[4] = vtcm && !((usize)vtcm & 2047U);
    if (!reply->detail[4]) { reply->detail[2] = 14; goto cleanup; }
    for (u32 index = 0; index < sizeof(power); ++index) ((volatile unsigned char *)&power)[index] = 0;
    power.type = HAP_power_set_HMX;
    power.hmx.power_up = 1;
    power_client = 1;
    reply->detail[1] = 6;
    status = HAP_power_set(&attributes, &power);
    if (status) { reply->detail[2] = (u32)status; goto cleanup; }
    reply->detail[1] = 7;
    status = HAP_compute_res_hmx_lock(context);
    if (status) { reply->detail[2] = (u32)status; goto cleanup; }
    locked = 1;
    reply->detail[1] = 8;
    for (u32 index = 0; index < 4096; ++index) vtcm[index] = 0x7e00;
    for (u32 row = 0; row < 32; ++row) {
        for (u32 column = 0; column < 32; ++column) {
            u32 packed = (row / 2) * 64 + column * 2 + row % 2;
            vtcm[packed] = input[row * 32 + column];
            vtcm[1024 + packed] = input[1024 + row * 32 + column];
        }
    }
    for (u32 index = 0; index < 128; ++index) vtcm[3072 + index] = index < 64 && !(index & 1) ? 0x3c00 : 0;
    __asm__ volatile(
        "barrier\n"
        "bias = mxmem2(%3)\n"
        "mxclracc.hf\n"
        "{\n"
        "activation.hf = mxmem(%1, %4)\n"
        "weight.hf = mxmem(%2, %4)\n"
        "}\n"
        "mxmem(%0, %5):after.hf = acc\n"
        "barrier\n"
        : : "r"(vtcm + 2048), "r"(vtcm), "r"(vtcm + 1024),
            "r"(vtcm + 3072), "r"(2047), "r"(0) : "memory");
    reply->detail[9] = 1;
    for (u32 row = 0; row < 32; ++row) {
        for (u32 column = 0; column < 32; ++column) {
            reply->product[row * 32 + column] = vtcm[2048 + (row / 2) * 64 + column * 2 + row % 2];
        }
    }
    for (u32 index = 3200; index < 4096; ++index) reply->detail[10] += vtcm[index] != 0x7e00;
    reply->detail[1] = 9;
cleanup:
    if (locked) reply->detail[5] = (u32)HAP_compute_res_hmx_unlock(context);
    if (context) reply->detail[6] = (u32)HAP_compute_res_release(context);
    if (power_client) {
        power.hmx.power_up = 0;
        reply->detail[7] = (u32)HAP_power_set(&attributes, &power);
        reply->detail[8] = (u32)HAP_power_destroy_client(&attributes);
    }
    return 0;
#endif
}
#endif

int fastrpc_probe_skel_invoke(u64 handle, u32 scalars, RemoteArg *arguments) {
    u32 method = (scalars >> 24) & 31U;
    const u32 *input;
    u32 *output;
    (void)handle;
    if (method == 0 || method == 1) return 0;
#if defined(FASTRPC_HMX) || defined(FASTRPC_SKEL_TEST)
    if (scalars == 0x03010100U) return invoke_hmx(arguments);
#endif
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
    {
        static unsigned short matrices[2048];
        static HmxReply reply;
        arguments[0].buffer.data = matrices;
        arguments[0].buffer.size = sizeof(matrices) / 2;
        arguments[1].buffer.data = &reply;
        arguments[1].buffer.size = sizeof(reply);
        reply.product[0] = 0xa5a5;
        if (fastrpc_probe_skel_invoke(0, 0x03010100U, arguments) ||
            reply.detail[0] != 0x484d5831U || reply.detail[1] || reply.detail[2] != 14 ||
            reply.detail[9] || reply.product[0] != 0xa5a5) ExitProcess(10);
        arguments[0].buffer.size = sizeof(matrices);
        if (fastrpc_probe_skel_invoke(0, 0x03010100U, arguments) != 20) ExitProcess(11);
        arguments[0].buffer.data = (char *)matrices + 1;
        if (fastrpc_probe_skel_invoke(0, 0x03010100U, arguments) || reply.detail[2] != 14) ExitProcess(12);
        arguments[1].buffer.size = sizeof(reply) - 4;
        if (fastrpc_probe_skel_invoke(0, 0x03010100U, arguments) != 14) ExitProcess(13);
    }
    ExitProcess(0);
}
#endif
