#include "nm_fp32.h"
#include <QnnOpPackage.h>
#include <QnnSdkBuildId.h>
#include <HTP/QnnHtpCommon.h>
#include <HTP/core/qhpi.h>

#ifdef _WIN32
#define NM_EXPORT __declspec(dllexport)
__declspec(dllimport) int GetModuleHandleExW(uint32_t, const uint16_t *, void **);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
int NmFp32DllEntry(void *module, unsigned long reason, void *reserved) {
    (void)module; (void)reason; (void)reserved; return 1;
}
static QHPI_StatusCode (*nm_register)(uint32_t, QHPI_OpInfo_v1 *, const char *);
static QHPI_Shape (*nm_shape)(const QHPI_Tensor *);
static QHPI_Element_Type (*nm_type)(const QHPI_Tensor *);
static void *(*nm_data)(const QHPI_Tensor *);
static uint32_t (*nm_slices)(QHPI_RuntimeHandle *);
static uint32_t (*nm_slice)(QHPI_RuntimeHandle *);
#define qhpi_register_ops_v1 nm_register
#define qhpi_tensor_shape nm_shape
#define qhpi_tensor_type nm_type
#define qhpi_tensor_raw_data nm_data
#define qhpi_num_slices nm_slices
#define qhpi_slice_number nm_slice
static int resolve_qhpi(void) {
    static const uint16_t name[] = {'Q','n','n','H','t','p','P','r','e','p','a','r','e','.','d','l','l',0};
    void *m = 0;
    if (!GetModuleHandleExW(2, name, &m)) return 0;
    nm_register = (QHPI_StatusCode (*)(uint32_t, QHPI_OpInfo_v1 *, const char *))GetProcAddress(m, "qhpi_register_ops_v1");
    nm_shape = (QHPI_Shape (*)(const QHPI_Tensor *))GetProcAddress(m, "qhpi_tensor_shape");
    nm_type = (QHPI_Element_Type (*)(const QHPI_Tensor *))GetProcAddress(m, "qhpi_tensor_type");
    nm_data = (void *(*)(const QHPI_Tensor *))GetProcAddress(m, "qhpi_tensor_raw_data");
    nm_slices = (uint32_t (*)(QHPI_RuntimeHandle *))GetProcAddress(m, "qhpi_num_slices");
    nm_slice = (uint32_t (*)(QHPI_RuntimeHandle *))GetProcAddress(m, "qhpi_slice_number");
    return nm_register && nm_shape && nm_type && nm_data && nm_slices && nm_slice;
}
#else
#define NM_EXPORT __attribute__((visibility("default")))
static int resolve_qhpi(void) { return 1; }
#endif

static int equal(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
#ifndef _WIN32
static size_t elements(const QHPI_Tensor *tensor) {
    QHPI_Shape s = qhpi_tensor_shape(tensor);
    size_t n = 1;
    uint32_t i;
    if (s.rank > 4) return 0;
    for (i = 0; i < s.rank; ++i) {
        if (!s.dims[i] || n > SIZE_MAX / s.dims[i]) return 0;
        n *= s.dims[i];
    }
    return n;
}
#endif
static uint32_t execute(QHPI_RuntimeHandle *handle, uint32_t no, QHPI_Tensor **out,
                        uint32_t ni, const QHPI_Tensor *const *in) {
#ifdef _WIN32
    (void)handle; (void)no; (void)out; (void)ni; (void)in;
    return QHPI_ERROR_FATAL;
#else
    const uint32_t *meta;
    const void *values[NM_FP32_MAX_INPUTS];
    size_t counts[NM_FP32_MAX_INPUTS], words, count;
    uint32_t i;
    if (no != 1 || ni < 2 || ni > NM_FP32_MAX_INPUTS + 1 ||
        !out || !in || !out[0] || !in[0] ||
        qhpi_tensor_type(out[0]) != QHPI_FLOAT32 ||
        qhpi_tensor_type(in[0]) != QHPI_INT32) return QHPI_ERROR_FATAL;
    words = elements(in[0]); meta = qhpi_tensor_raw_data(in[0]);
    count = elements(out[0]);
    if (!meta || words < NM_FP32_META_BASE || meta[0] != NM_FP32_META_MAGIC ||
        meta[1] != NM_FP32_META_VERSION || meta[3] != ni - 1 || !count)
        return QHPI_ERROR_FATAL;
    for (i = 1; i < ni; ++i) {
        uint32_t dtype = meta[9 + (i - 1) * 6];
        uint32_t expected = dtype == NM_FP32_DTYPE_FLOAT ? QHPI_FLOAT32 :
                            dtype == NM_FP32_DTYPE_INT ? QHPI_INT32 : QHPI_UNKNOWN;
        if (!in[i] || expected == QHPI_UNKNOWN || qhpi_tensor_type(in[i]) != expected)
            return QHPI_ERROR_FATAL;
        values[i - 1] = qhpi_tensor_raw_data(in[i]);
        counts[i - 1] = elements(in[i]);
        if (!values[i - 1] || !counts[i - 1]) return QHPI_ERROR_FATAL;
    }
    if (!qhpi_tensor_raw_data(out[0])) return QHPI_ERROR_FATAL;
    return nm_fp32_execute(meta, words, values, counts, ni - 1,
                           qhpi_tensor_raw_data(out[0]), count,
                           qhpi_slice_number(handle), qhpi_num_slices(handle)) ?
           QHPI_ERROR_FATAL : QHPI_SUCCESS;
#endif
}

static QHPI_Tensor_Signature_v1 signatures[5] = {
    {QHPI_INT32, QHPI_LAYOUT_FLAT_4, QHPI_STORAGE_DIRECT, QHPI_MEM_LOC_DDR_ONLY},
    {QHPI_FLOAT32, QHPI_LAYOUT_FLAT_4, QHPI_STORAGE_DIRECT, QHPI_MEM_LOC_DDR_ONLY},
    {QHPI_FLOAT32, QHPI_LAYOUT_FLAT_4, QHPI_STORAGE_DIRECT, QHPI_MEM_LOC_DDR_ONLY},
    {QHPI_FLOAT32, QHPI_LAYOUT_FLAT_4, QHPI_STORAGE_DIRECT, QHPI_MEM_LOC_DDR_ONLY},
    {QHPI_FLOAT32, QHPI_LAYOUT_FLAT_4, QHPI_STORAGE_DIRECT, QHPI_MEM_LOC_DDR_ONLY}
};
static QHPI_Tensor_Signature_v1 gather_signatures[3] = {
    {QHPI_INT32, QHPI_LAYOUT_FLAT_4, QHPI_STORAGE_DIRECT, QHPI_MEM_LOC_DDR_ONLY},
    {QHPI_FLOAT32, QHPI_LAYOUT_FLAT_4, QHPI_STORAGE_DIRECT, QHPI_MEM_LOC_DDR_ONLY},
    {QHPI_INT32, QHPI_LAYOUT_FLAT_4, QHPI_STORAGE_DIRECT, QHPI_MEM_LOC_DDR_ONLY}
};
static QHPI_Tensor_Signature_v1 output_signature = {
    QHPI_FLOAT32, QHPI_LAYOUT_FLAT_4, QHPI_STORAGE_DIRECT, QHPI_MEM_LOC_DDR_ONLY
};
static QHPI_Kernel_v1 kernels[5];
static QHPI_OpInfo_v1 operators[2];
static QHPI_StatusCode registration_status;

#ifndef NM_FP32_LEGACY_LOADER_PROBE
NM_EXPORT
#endif
const char *qhpi_init(void) {
    static const char *function_names[] = {
        NM_FP32_PACKAGE "::execute1", NM_FP32_PACKAGE "::execute2",
        NM_FP32_PACKAGE "::execute3", NM_FP32_PACKAGE "::execute4",
        NM_FP32_PACKAGE "::gather"
    };
    uint32_t i;
    if (!resolve_qhpi()) return 0;
    for (i = 0; i < 5; ++i) {
        kernels[i].function_name = function_names[i];
        kernels[i].function = execute;
        kernels[i].resources = QHPI_RESOURCE_HVX;
        kernels[i].multithreaded = 1;
        kernels[i].min_inputs = i < 4 ? i + 2 : 3;
        kernels[i].input_signature = i < 4 ? signatures : gather_signatures;
        kernels[i].min_outputs = 1;
        kernels[i].output_signature = &output_signature;
    }
    operators[0].name = NM_FP32_PACKAGE "::" NM_FP32_OPERATOR;
    operators[0].num_kernels = 4; operators[0].kernels = kernels;
    operators[1].name = NM_FP32_PACKAGE "::" NM_FP32_GATHER;
    operators[1].num_kernels = 1; operators[1].kernels = kernels + 4;
    registration_status = qhpi_register_ops_v1(2, operators, NM_FP32_PACKAGE);
    return NM_FP32_PACKAGE;
}

static int initialized;
static const char *names[] = {NM_FP32_OPERATOR, NM_FP32_GATHER};
static Qnn_ApiVersion_t api_version = QNN_HTP_API_VERSION_INIT;
static Qnn_Version_t opset = {1, 0, 0};
static QnnOpPackage_Info_t info = {
    NM_FP32_PACKAGE, names, 0, 2, 0, 0, QNN_SDK_BUILD_ID, &api_version, 0, &opset, {0}
};
static Qnn_ErrorHandle_t package_init(QnnOpPackage_GlobalInfrastructure_t infrastructure) {
    (void)infrastructure;
    if (registration_status != QHPI_SUCCESS && registration_status != QHPI_REGISTRATION_WARNING)
        return QNN_OP_PACKAGE_ERROR_UNSUPPORTED_FEATURE;
    if (initialized) return QNN_OP_PACKAGE_ERROR_LIBRARY_ALREADY_INITIALIZED;
    initialized = 1; return QNN_SUCCESS;
}
static Qnn_ErrorHandle_t terminate(void) { initialized = 0; return QNN_SUCCESS; }
static Qnn_ErrorHandle_t get_info(const QnnOpPackage_Info_t **p) {
    if (!initialized) return QNN_OP_PACKAGE_ERROR_LIBRARY_NOT_INITIALIZED;
    if (!p) return QNN_OP_PACKAGE_ERROR_INVALID_ARGUMENT;
    *p = &info; return QNN_SUCCESS;
}
static Qnn_ErrorHandle_t validate(Qnn_OpConfig_t op) {
    uint32_t i;
    int gather = equal(op.v1.typeName, NM_FP32_GATHER);
    if (op.version != QNN_OPCONFIG_VERSION_1 || !equal(op.v1.packageName, NM_FP32_PACKAGE) ||
        (!gather && !equal(op.v1.typeName, NM_FP32_OPERATOR)) ||
        op.v1.numOfParams || op.v1.numOfOutputs != 1 ||
        op.v1.numOfInputs < 2 || op.v1.numOfInputs > 5 ||
        (gather && op.v1.numOfInputs != 3) || !op.v1.inputTensors || !op.v1.outputTensors)
        return QNN_OP_PACKAGE_ERROR_VALIDATION_FAILURE;
    if (op.v1.outputTensors[0].version != QNN_TENSOR_VERSION_1 ||
        op.v1.outputTensors[0].v1.dataType != QNN_DATATYPE_FLOAT_32)
        return QNN_OP_PACKAGE_ERROR_VALIDATION_FAILURE;
    for (i = 0; i < op.v1.numOfInputs; ++i) {
        Qnn_DataType_t type = !i || (gather && i == 2) ? QNN_DATATYPE_INT_32 : QNN_DATATYPE_FLOAT_32;
        if (op.v1.inputTensors[i].version != QNN_TENSOR_VERSION_1 ||
            op.v1.inputTensors[i].v1.dataType != type)
            return QNN_OP_PACKAGE_ERROR_VALIDATION_FAILURE;
    }
    if (op.v1.inputTensors[0].v1.type != QNN_TENSOR_TYPE_STATIC)
        return QNN_OP_PACKAGE_ERROR_VALIDATION_FAILURE;
    return QNN_SUCCESS;
}
static Qnn_ErrorHandle_t create_impl(QnnOpPackage_GraphInfrastructure_t g,
                                    QnnOpPackage_Node_t n, QnnOpPackage_OpImpl_t *p) {
    (void)g; (void)n; (void)p; return QNN_OP_PACKAGE_ERROR_UNSUPPORTED_FEATURE;
}
static Qnn_ErrorHandle_t free_impl(QnnOpPackage_OpImpl_t p) {
    (void)p; return QNN_OP_PACKAGE_ERROR_UNSUPPORTED_FEATURE;
}
static Qnn_ErrorHandle_t log_init(QnnLog_Callback_t cb, QnnLog_Level_t level) {
    return cb && level >= QNN_LOG_LEVEL_ERROR ? QNN_SUCCESS : QNN_LOG_ERROR_INVALID_ARGUMENT;
}
static Qnn_ErrorHandle_t log_level(QnnLog_Level_t level) {
    return level >= QNN_LOG_LEVEL_ERROR ? QNN_SUCCESS : QNN_LOG_ERROR_INVALID_ARGUMENT;
}
static Qnn_ErrorHandle_t log_end(void) { return QNN_SUCCESS; }
NM_EXPORT Qnn_ErrorHandle_t NmFp32InterfaceProvider(QnnOpPackage_Interface_t *p) {
    if (!p) return QNN_OP_PACKAGE_ERROR_INVALID_ARGUMENT;
    p->interfaceVersion = (Qnn_Version_t){1, 4, 0};
    p->v1_4 = (QnnOpPackage_ImplementationV1_4_t){
        package_init, terminate, get_info, validate, create_impl, free_impl, log_init, log_level, log_end
    };
    return QNN_SUCCESS;
}
