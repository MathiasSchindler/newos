#include <HTP/core/pco_declarations.h>
#include "nm_fp32.h"

extern "C" {
__declspec(dllexport) volatile unsigned int NmFp32LegacyEntryReached = 0;

/* Loader diagnostic only: deliberately refuses registration instead of providing CPU kernels. */
__declspec(dllexport) int op_pkg_init(PackageOpIf &interface) {
    NmFp32LegacyEntryReached = 1;
    interface._name = NM_FP32_PACKAGE;
    return GraphStatus::ErrorUnsupported;
}
}
