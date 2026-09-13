#ifndef NEWOS_EXPERIMENTAL_SNAPDRAGON_QNN_ABI_H
#define NEWOS_EXPERIMENTAL_SNAPDRAGON_QNN_ABI_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef signed char i8;
typedef signed short i16;
typedef int i32;
typedef unsigned int u32;
typedef unsigned long long u64;

enum {
    QNN_CORE_API_VERSION_MAJOR = 2,
    QNN_CORE_API_VERSION_MINOR = 39,
    QNN_CORE_API_VERSION_PATCH = 0,
    QNN_LOG_LEVEL_ERROR = 1,
    QNN_LOG_LEVEL_WARN = 2,
    QNN_LOG_LEVEL_INFO = 3,
    QNN_LOG_LEVEL_VERBOSE = 4,
    QNN_LOG_LEVEL_DEBUG = 5
};

typedef struct QnnVersion {
    u32 major;
    u32 minor;
    u32 patch;
} QnnVersion;

typedef struct QnnApiVersion {
    QnnVersion core_api_version;
    QnnVersion backend_api_version;
} QnnApiVersion;

typedef struct QnnInterfacePrefix {
    u32 backend_id;
    const char *provider_name;
    QnnApiVersion api_version;
} QnnInterfacePrefix;

typedef void *QnnHandle;
typedef QnnHandle QnnLogHandle;
typedef QnnHandle QnnBackendHandle;
typedef QnnHandle QnnDeviceHandle;
typedef QnnHandle QnnProfileHandle;
typedef QnnHandle QnnContextHandle;
typedef QnnHandle QnnGraphHandle;
typedef QnnHandle QnnMemHandle;

typedef struct QnnBackendConfig QnnBackendConfig;
typedef struct QnnDeviceConfig QnnDeviceConfig;
typedef struct QnnContextConfig QnnContextConfig;
typedef struct QnnGraphConfig QnnGraphConfig;

enum {
    QNN_TENSOR_TYPE_APP_WRITE = 0,
    QNN_TENSOR_TYPE_APP_READ = 1,
    QNN_TENSOR_TYPE_NATIVE = 3,
    QNN_TENSOR_TYPE_STATIC = 4,
    QNN_PARAMTYPE_SCALAR = 0,
    QNN_PARAMTYPE_TENSOR = 1,
    QNN_DEFINITION_DEFINED = 1,
    QNN_DEFINITION_UNDEFINED = 0x7fffffff,
    QNN_QUANTIZATION_ENCODING_SCALE_OFFSET = 0,
    QNN_QUANTIZATION_ENCODING_AXIS_SCALE_OFFSET = 1,
    QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET = 3,
    QNN_QUANTIZATION_ENCODING_UNDEFINED = 0x7fffffff,
    QNN_TENSOR_DATA_FORMAT_DENSE = 0,
    QNN_DATATYPE_INT_32 = 0x0032,
    QNN_DATATYPE_UINT_32 = 0x0132,
    QNN_DATATYPE_FLOAT_16 = 0x0216,
    QNN_DATATYPE_FLOAT_32 = 0x0232,
    QNN_DATATYPE_SFIXED_POINT_4 = 0x0304,
    QNN_DATATYPE_SFIXED_POINT_8 = 0x0308,
    QNN_DATATYPE_SFIXED_POINT_16 = 0x0316,
    QNN_DATATYPE_SFIXED_POINT_32 = 0x0332,
    QNN_DATATYPE_UFIXED_POINT_8 = 0x0408,
    QNN_DATATYPE_UFIXED_POINT_16 = 0x0416,
    QNN_DATATYPE_BOOL_8 = 0x0508,
    QNN_TENSORMEMTYPE_RAW = 0,
    QNN_TENSORMEMTYPE_MEMHANDLE = 1,
    QNN_MEM_TYPE_CUSTOM = 2,
    QNN_HTP_MEM_SHARED_BUFFER = 1,
    QNN_TENSOR_VERSION_1 = 1,
    QNN_OPCONFIG_VERSION_1 = 1
};

typedef struct QnnMemShape {
    u32 rank;
    u32 *dimensions;
    void *shape_config;
} QnnMemShape;

typedef struct QnnMemDescriptor {
    QnnMemShape shape;
    u32 data_type;
    u32 memory_type;
    union {
        struct {
            i32 fd;
        } ion;
        void *custom_info;
        u64 reserved[2];
    } memory;
} QnnMemDescriptor;

typedef struct QnnHtpSharedBufferConfig {
    i32 fd;
    u64 offset;
} QnnHtpSharedBufferConfig;

typedef struct QnnHtpMemDescriptor {
    u32 type;
    u64 size;
    union {
        QnnHtpSharedBufferConfig shared_buffer;
        u64 reserved[2];
    } config;
} QnnHtpMemDescriptor;

typedef struct QnnScaleOffset {
    float scale;
    i32 offset;
} QnnScaleOffset;

typedef struct QnnAxisScaleOffset {
    i32 axis;
    u32 scale_offset_count;
    QnnScaleOffset *scale_offsets;
} QnnAxisScaleOffset;

typedef struct QnnBwAxisScaleOffset {
    u32 bitwidth;
    i32 axis;
    u32 element_count;
    float *scales;
    i32 *offsets;
} QnnBwAxisScaleOffset;

typedef union QnnQuantizeEncoding {
    QnnScaleOffset scale_offset;
    QnnAxisScaleOffset axis_scale_offset;
    QnnBwAxisScaleOffset bw_axis_scale_offset;
    u64 reserved[4];
} QnnQuantizeEncoding;

typedef struct QnnQuantizeParams {
    u32 encoding_definition;
    u32 quantization_encoding;
    QnnQuantizeEncoding encoding;
} QnnQuantizeParams;

typedef struct QnnClientBuffer {
    void *data;
    u32 data_size;
} QnnClientBuffer;

typedef struct QnnTensorV1 {
    u32 id;
    const char *name;
    u32 type;
    u32 data_format;
    u32 data_type;
    QnnQuantizeParams quantize_params;
    u32 rank;
    u32 *dimensions;
    u32 memory_type;
    union {
        QnnClientBuffer client_buffer;
        QnnHandle memory_handle;
    } memory;
} QnnTensorV1;

typedef union QnnTensorData {
    QnnTensorV1 v1;
    u64 reserved[17];
} QnnTensorData;

typedef struct QnnTensor {
    u32 version;
    QnnTensorData data;
} QnnTensor;

typedef struct QnnScalar {
    u32 data_type;
    union {
        float float_value;
        i32 int32_value;
        u32 uint32_value;
        u8 bool8_value;
        u64 uint64_value;
    } value;
} QnnScalar;

typedef union QnnParamValue {
    QnnScalar scalar;
    QnnTensor tensor;
    u64 reserved[18];
} QnnParamValue;

typedef struct QnnParam {
    u32 type;
    const char *name;
    QnnParamValue value;
} QnnParam;

typedef struct QnnOpConfigV1 {
    const char *name;
    const char *package_name;
    const char *type_name;
    u32 parameter_count;
    void *parameters;
    u32 input_count;
    QnnTensor *inputs;
    u32 output_count;
    QnnTensor *outputs;
} QnnOpConfigV1;

typedef union QnnOpConfigData {
    QnnOpConfigV1 v1;
} QnnOpConfigData;

typedef struct QnnOpConfig {
    u32 version;
    QnnOpConfigData data;
} QnnOpConfig;

typedef void (*QnnUnusedFunction)(void);
typedef u64 (*QnnLogCreate)(void *, u32, QnnLogHandle *);
typedef u64 (*QnnLogFree)(QnnLogHandle);
typedef u64 (*QnnBackendCreate)(QnnLogHandle, const QnnBackendConfig **, QnnBackendHandle *);
typedef u64 (*QnnBackendFree)(QnnBackendHandle);
typedef u64 (*QnnDeviceCreate)(QnnLogHandle, const QnnDeviceConfig **, QnnDeviceHandle *);
typedef u64 (*QnnDeviceFree)(QnnDeviceHandle);
typedef u64 (*QnnProfileCreate)(QnnBackendHandle, u32, QnnProfileHandle *);
typedef u64 (*QnnProfileFree)(QnnProfileHandle);
typedef u64 (*QnnContextCreate)(QnnBackendHandle, QnnDeviceHandle, const QnnContextConfig **, QnnContextHandle *);
typedef u64 (*QnnContextGetBinarySize)(QnnContextHandle, u64 *);
typedef u64 (*QnnContextGetBinary)(QnnContextHandle, void *, u64, u64 *);
typedef u64 (*QnnContextCreateFromBinary)(
    QnnBackendHandle,
    QnnDeviceHandle,
    const QnnContextConfig **,
    const void *,
    u64,
    QnnContextHandle *,
    QnnProfileHandle
);
typedef u64 (*QnnContextFree)(QnnContextHandle, QnnProfileHandle);
typedef u64 (*QnnGraphCreate)(QnnContextHandle, const char *, const QnnGraphConfig **, QnnGraphHandle *);
typedef u64 (*QnnGraphAddNode)(QnnGraphHandle, QnnOpConfig);
typedef u64 (*QnnGraphFinalize)(QnnGraphHandle, QnnProfileHandle, QnnHandle);
typedef u64 (*QnnGraphRetrieve)(QnnContextHandle, const char *, QnnGraphHandle *);
typedef u64 (*QnnGraphExecute)(QnnGraphHandle, const QnnTensor *, u32, QnnTensor *, u32, QnnProfileHandle, QnnHandle);
typedef u64 (*QnnTensorCreateGraphTensor)(QnnGraphHandle, QnnTensor *);
typedef u64 (*QnnMemRegister)(QnnContextHandle, const QnnMemDescriptor *, u32, QnnMemHandle *);
typedef u64 (*QnnMemDeregister)(QnnMemHandle *, u32);

typedef struct QnnInterfaceV2 {
    QnnUnusedFunction property_has_capability;
    QnnBackendCreate backend_create;
    QnnUnusedFunction backend_set_config;
    QnnUnusedFunction backend_get_api_version;
    QnnUnusedFunction backend_get_build_id;
    QnnUnusedFunction backend_register_op_package;
    QnnUnusedFunction backend_get_supported_operations;
    QnnUnusedFunction backend_validate_op_config;
    QnnBackendFree backend_free;
    QnnContextCreate context_create;
    QnnUnusedFunction context_set_config;
    QnnContextGetBinarySize context_get_binary_size;
    QnnContextGetBinary context_get_binary;
    QnnContextCreateFromBinary context_create_from_binary;
    QnnContextFree context_free;
    QnnGraphCreate graph_create;
    QnnUnusedFunction graph_create_subgraph;
    QnnUnusedFunction graph_set_config;
    QnnGraphAddNode graph_add_node;
    QnnGraphFinalize graph_finalize;
    QnnGraphRetrieve graph_retrieve;
    QnnGraphExecute graph_execute;
    QnnUnusedFunction graph_execute_async;
    QnnUnusedFunction tensor_create_context_tensor;
    QnnTensorCreateGraphTensor tensor_create_graph_tensor;
    QnnLogCreate log_create;
    QnnUnusedFunction log_set_log_level;
    QnnLogFree log_free;
    QnnProfileCreate profile_create;
    QnnUnusedFunction profile_set_config;
    QnnUnusedFunction profile_get_events;
    QnnUnusedFunction profile_get_sub_events;
    QnnUnusedFunction profile_get_event_data;
    QnnUnusedFunction profile_get_extended_event_data;
    QnnProfileFree profile_free;
    QnnMemRegister mem_register;
    QnnMemDeregister mem_deregister;
    QnnUnusedFunction device_get_platform_info;
    QnnUnusedFunction device_free_platform_info;
    QnnUnusedFunction device_get_infrastructure;
    QnnDeviceCreate device_create;
    QnnUnusedFunction device_set_config;
    QnnUnusedFunction device_get_info;
    QnnDeviceFree device_free;
} QnnInterfaceV2;

typedef struct QnnInterfaceProviderV2 {
    QnnInterfacePrefix prefix;
    QnnInterfaceV2 api;
} QnnInterfaceProviderV2;

typedef u64 (*QnnInterfaceGetProviders)(const QnnInterfacePrefix ***, u32 *);

_Static_assert(sizeof(QnnQuantizeParams) == 40, "QNN 2.39 quantization ABI mismatch");
_Static_assert(sizeof(QnnAxisScaleOffset) == 16, "QNN 2.39 axis quantization ABI mismatch");
_Static_assert(sizeof(QnnBwAxisScaleOffset) == 32, "QNN 2.39 bit-width axis quantization ABI mismatch");
_Static_assert(sizeof(QnnTensorV1) == 112, "QNN 2.39 tensor V1 ABI mismatch");
_Static_assert(sizeof(QnnTensor) == 144, "QNN 2.39 tensor ABI mismatch");
_Static_assert(sizeof(QnnScalar) == 16, "QNN 2.39 scalar ABI mismatch");
_Static_assert(sizeof(QnnParam) == 160, "QNN 2.39 parameter ABI mismatch");
_Static_assert(__builtin_offsetof(QnnParam, value) == 16, "QNN 2.39 parameter field mismatch");
_Static_assert(__builtin_offsetof(QnnTensorV1, quantize_params) == 32, "QNN 2.39 tensor field mismatch");
_Static_assert(__builtin_offsetof(QnnTensorV1, memory) == 96, "QNN 2.39 tensor memory mismatch");
_Static_assert(sizeof(QnnOpConfigV1) == 72, "QNN 2.39 op config V1 ABI mismatch");
_Static_assert(sizeof(QnnOpConfig) == 80, "QNN 2.39 op config ABI mismatch");
_Static_assert(sizeof(QnnMemShape) == 24, "QNN 2.39 memory shape ABI mismatch");
_Static_assert(sizeof(QnnMemDescriptor) == 48, "QNN 2.39 memory descriptor ABI mismatch");
_Static_assert(sizeof(QnnHtpSharedBufferConfig) == 16, "QNN 2.39 HTP shared buffer ABI mismatch");
_Static_assert(sizeof(QnnHtpMemDescriptor) == 32, "QNN 2.39 HTP memory descriptor ABI mismatch");
_Static_assert(__builtin_offsetof(QnnInterfaceV2, graph_create) == 120, "QNN graphCreate slot mismatch");
_Static_assert(__builtin_offsetof(QnnInterfaceV2, graph_add_node) == 144, "QNN graphAddNode slot mismatch");
_Static_assert(__builtin_offsetof(QnnInterfaceV2, graph_execute) == 168, "QNN graphExecute slot mismatch");
_Static_assert(__builtin_offsetof(QnnInterfaceV2, tensor_create_graph_tensor) == 192, "QNN tensor slot mismatch");

#endif