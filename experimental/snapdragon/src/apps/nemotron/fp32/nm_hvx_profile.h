#ifndef NM_HVX_OUTPUT_PROFILE_H
#define NM_HVX_OUTPUT_PROFILE_H
#include <hexagon_types.h>
#include <hvx_hexagon_protos.h>
extern volatile unsigned long long nm_rpc_hvx_output_vectors[64];
static __attribute__((always_inline)) inline HVX_Vector nm_rpc_profile_output(HVX_Vector value, unsigned int slice_number) {
    ++nm_rpc_hvx_output_vectors[(slice_number & 3U) * 16U];
    return Q6_Vsf_equals_Vqf32(value);
}
#undef Q6_Vsf_equals_Vqf32
#define Q6_Vsf_equals_Vqf32(value) nm_rpc_profile_output(value, slice)
#endif
