#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_QNN_GEMMA_CAPABILITIES_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_QNN_GEMMA_CAPABILITIES_H

#include "qnn_abi.h"

u32 qnn_gemma_stage1_probe(
    const QnnInterfaceV2 *api,
    QnnContextHandle context
);

#endif