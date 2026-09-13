typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long long usize;
typedef int HResult;
typedef unsigned long WinUlong;
typedef unsigned char DxBool;

typedef struct Guid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
} Guid;

typedef struct DxCoreHardwareIdParts {
    u32 vendor_id;
    u32 device_id;
    u32 subsystem_id;
    u32 subvendor_id;
    u32 revision_id;
} DxCoreHardwareIdParts;

typedef struct DxCoreAdapter DxCoreAdapter;
typedef struct DxCoreAdapterList DxCoreAdapterList;
typedef struct DxCoreAdapterFactory DxCoreAdapterFactory;
typedef struct Unknown Unknown;

typedef struct UnknownVtable {
    HResult (*query_interface)(Unknown *, const Guid *, void **);
    WinUlong (*add_ref)(Unknown *);
    WinUlong (*release)(Unknown *);
} UnknownVtable;

struct Unknown {
    const UnknownVtable *vtable;
};

typedef struct DxCoreAdapterVtable {
    HResult (*query_interface)(DxCoreAdapter *, const Guid *, void **);
    WinUlong (*add_ref)(DxCoreAdapter *);
    WinUlong (*release)(DxCoreAdapter *);
    DxBool (*is_valid)(DxCoreAdapter *);
    DxBool (*is_attribute_supported)(DxCoreAdapter *, const Guid *);
    DxBool (*is_property_supported)(DxCoreAdapter *, u32);
    HResult (*get_property)(DxCoreAdapter *, u32, usize, void *);
    HResult (*get_property_size)(DxCoreAdapter *, u32, usize *);
    void *is_query_state_supported;
    void *query_state;
    void *is_set_state_supported;
    void *set_state;
    void *get_factory;
} DxCoreAdapterVtable;

struct DxCoreAdapter {
    const DxCoreAdapterVtable *vtable;
};

typedef struct DxCoreAdapterListVtable {
    HResult (*query_interface)(DxCoreAdapterList *, const Guid *, void **);
    WinUlong (*add_ref)(DxCoreAdapterList *);
    WinUlong (*release)(DxCoreAdapterList *);
    HResult (*get_adapter)(DxCoreAdapterList *, u32, const Guid *, void **);
    u32 (*get_adapter_count)(DxCoreAdapterList *);
    void *is_stale;
    void *get_factory;
    void *sort;
    void *is_adapter_preference_supported;
} DxCoreAdapterListVtable;

struct DxCoreAdapterList {
    const DxCoreAdapterListVtable *vtable;
};

typedef struct DxCoreAdapterFactoryVtable {
    HResult (*query_interface)(DxCoreAdapterFactory *, const Guid *, void **);
    WinUlong (*add_ref)(DxCoreAdapterFactory *);
    WinUlong (*release)(DxCoreAdapterFactory *);
    HResult (*create_adapter_list)(DxCoreAdapterFactory *, u32, const Guid *, const Guid *, void **);
    void *get_adapter_by_luid;
    void *is_notification_type_supported;
    void *register_event_notification;
    void *unregister_event_notification;
    HResult (*create_adapter_list_by_workload)(DxCoreAdapterFactory *, u32, u32, u32, const Guid *, void **);
} DxCoreAdapterFactoryVtable;

struct DxCoreAdapterFactory {
    const DxCoreAdapterFactoryVtable *vtable;
};

__declspec(dllimport) void ExitProcess(u32 exit_code);
__declspec(dllimport) void *GetStdHandle(u32 handle_id);
__declspec(dllimport) int IsProcessorFeaturePresent(u32 feature);
__declspec(dllimport) int WriteFile(void *handle, const void *buffer, u32 size, u32 *written, void *overlapped);
__declspec(dllimport) HResult DXCoreCreateAdapterFactory(const Guid *interface_id, void **factory);
__declspec(dllimport) HResult D3D12CreateDevice(Unknown *adapter, u32 minimum_feature_level, const Guid *interface_id, void **device);
__declspec(dllimport) HResult DMLCreateDevice(Unknown *d3d12_device, u32 flags, const Guid *interface_id, void **device);

static const Guid iid_adapter_factory1 = {0xd5682e19U, 0x6d21U, 0x401cU, {0x82U, 0x7aU, 0x9aU, 0x51U, 0xa4U, 0xeaU, 0x35U, 0xd7U}};
static const Guid iid_adapter_list = {0x526c7776U, 0x40e9U, 0x459bU, {0xb7U, 0x11U, 0xf3U, 0x2aU, 0xd7U, 0x6dU, 0xfcU, 0x28U}};
static const Guid iid_adapter = {0xf0db4c7fU, 0xfe5aU, 0x42a2U, {0xbdU, 0x62U, 0xf2U, 0xa6U, 0xcfU, 0x6fU, 0xc8U, 0x3eU}};
static const Guid iid_d3d12_device1 = {0x77acce80U, 0x638eU, 0x4e65U, {0x88U, 0x95U, 0xc1U, 0xf2U, 0x33U, 0x86U, 0x86U, 0x3eU}};
static const Guid iid_dml_device = {0x6dbd6437U, 0x96fdU, 0x423fU, {0xa9U, 0x8cU, 0xaeU, 0x5eU, 0x7cU, 0x2aU, 0x57U, 0x3fU}};

static const Guid attribute_d3d12_graphics = {0x0c9ece4dU, 0x2f6eU, 0x4f01U, {0x8cU, 0x96U, 0xe8U, 0x9eU, 0x33U, 0x1bU, 0x47U, 0xb1U}};
static const Guid attribute_d3d12_core_compute = {0x248e2800U, 0xa793U, 0x4724U, {0xabU, 0xaaU, 0x23U, 0xa6U, 0xdeU, 0x1bU, 0xe0U, 0x90U}};
static const Guid attribute_d3d12_generic_ml = {0xb71b0d41U, 0x1088U, 0x422fU, {0xa2U, 0x7cU, 0x02U, 0x50U, 0xb7U, 0xd3U, 0xa9U, 0x88U}};
static const Guid attribute_gpu = {0xb69eb219U, 0x3dedU, 0x4464U, {0x97U, 0x9fU, 0xa0U, 0x0bU, 0xd4U, 0x68U, 0x70U, 0x06U}};
static const Guid attribute_compute_accelerator = {0xe0b195daU, 0x58efU, 0x4a22U, {0x90U, 0xf1U, 0x1fU, 0x28U, 0x16U, 0x9cU, 0xabU, 0x8dU}};
static const Guid attribute_npu = {0xd46140c4U, 0xadd7U, 0x451bU, {0x9eU, 0x56U, 0x06U, 0xfeU, 0x8cU, 0x3bU, 0x58U, 0xedU}};

enum {
    dxcore_property_driver_version = 1,
    dxcore_property_driver_description = 2,
    dxcore_property_dedicated_adapter_memory = 7,
    dxcore_property_dedicated_system_memory = 8,
    dxcore_property_shared_system_memory = 9,
    dxcore_property_is_hardware = 11,
    dxcore_property_is_integrated = 12,
    dxcore_property_hardware_id_parts = 14,
    dxcore_workload_machine_learning = 3,
    dxcore_hardware_type_npu = 4,
    d3d_feature_level_1_0_generic = 0x0100
};

typedef struct CpuFeature {
    const char *name;
    u32 id;
} CpuFeature;

static const CpuFeature cpu_features[] = {
    {"neon", 19U}, {"divide", 24U}, {"atomic64", 25U}, {"fmac", 27U},
    {"armv8", 29U}, {"crypto", 30U}, {"crc32", 31U}, {"lse", 34U},
    {"dotprod", 43U}, {"jscvt", 44U}, {"lrcpc", 45U}, {"sve", 46U},
    {"sve2", 47U}, {"lse2", 62U}, {"sha3", 64U}, {"sha512", 65U},
    {"i8mm", 66U}, {"fp16", 67U}, {"bf16", 68U}, {"ebf16", 69U},
    {"sme", 70U}, {"sme2", 71U}
};

static void *stdout_handle;

static usize text_length(const char *text) {
    usize length = 0U;
    while (text[length] != '\0') length += 1U;
    return length;
}

static void write_bytes(const char *data, usize size) {
    while (size != 0U) {
        u32 chunk = size > 0xffffffffULL ? 0xffffffffU : (u32)size;
        u32 written = 0U;
        if (!WriteFile(stdout_handle, data, chunk, &written, 0) || written == 0U) return;
        data += written;
        size -= written;
    }
}

static void write_text(const char *text) {
    write_bytes(text, text_length(text));
}

static void write_u64(u64 value) {
    char digits[32];
    usize used = 0U;
    do {
        digits[used++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (used != 0U) write_bytes(&digits[--used], 1U);
}

static void write_hex32(u32 value) {
    static const char hex[] = "0123456789abcdef";
    char digits[8];
    usize index;
    for (index = 0U; index < 8U; ++index) {
        digits[7U - index] = hex[value & 15U];
        value >>= 4U;
    }
    write_text("0x");
    write_bytes(digits, sizeof(digits));
}

static void write_key_text(const char *key, const char *value) {
    write_text(key);
    write_text("=");
    write_text(value);
    write_text("\n");
}

static void write_key_u64(const char *key, u64 value) {
    write_text(key);
    write_text("=");
    write_u64(value);
    write_text("\n");
}

static int succeeded(HResult result) {
    return result >= 0;
}

static void probe_directml(DxCoreAdapter *adapter) {
    Unknown *d3d12_device = 0;
    Unknown *dml_device = 0;
    HResult d3d12_result = D3D12CreateDevice(
        (Unknown *)adapter, d3d_feature_level_1_0_generic,
        &iid_d3d12_device1, (void **)&d3d12_device);
    HResult dml_result = -1;

    write_text("\n[directml]\n");
    write_text("d3d12_create_hresult=");
    write_hex32((u32)d3d12_result);
    write_text("\n");
    write_key_text("d3d12_device", succeeded(d3d12_result) && d3d12_device != 0 ? "true" : "false");

    if (succeeded(d3d12_result) && d3d12_device != 0) {
        dml_result = DMLCreateDevice(d3d12_device, 0U, &iid_dml_device, (void **)&dml_device);
        write_key_text("dml_create_attempted", "true");
        write_text("dml_create_hresult=");
        write_hex32((u32)dml_result);
        write_text("\n");
    } else {
        write_key_text("dml_create_attempted", "false");
    }
    write_key_text("dml_device", succeeded(dml_result) && dml_device != 0 ? "true" : "false");

    if (dml_device != 0) dml_device->vtable->release(dml_device);
    if (d3d12_device != 0) d3d12_device->vtable->release(d3d12_device);
}

static void report_adapter(DxCoreAdapter *adapter, const char *filter, u32 index) {
    char description[256];
    usize description_size = 0U;
    DxCoreHardwareIdParts hardware_id;
    u64 value64;
    DxBool value_bool;

    write_text("\n[adapter]\n");
    write_key_text("filter", filter);
    write_key_u64("index", index);

    if (adapter->vtable->is_property_supported(adapter, dxcore_property_driver_description) &&
        succeeded(adapter->vtable->get_property_size(adapter, dxcore_property_driver_description, &description_size)) &&
        description_size > 0U && description_size <= sizeof(description) &&
        succeeded(adapter->vtable->get_property(adapter, dxcore_property_driver_description, description_size, description))) {
        description[sizeof(description) - 1U] = '\0';
        write_key_text("description", description);
    }
    if (adapter->vtable->is_property_supported(adapter, dxcore_property_hardware_id_parts) &&
        succeeded(adapter->vtable->get_property(adapter, dxcore_property_hardware_id_parts, sizeof(hardware_id), &hardware_id))) {
        write_text("vendor_id="); write_hex32(hardware_id.vendor_id); write_text("\n");
        write_text("device_id="); write_hex32(hardware_id.device_id); write_text("\n");
        write_text("subvendor_id="); write_hex32(hardware_id.subvendor_id); write_text("\n");
        write_text("subsystem_id="); write_hex32(hardware_id.subsystem_id); write_text("\n");
        write_key_u64("revision_id", hardware_id.revision_id);
    }
    if (adapter->vtable->is_property_supported(adapter, dxcore_property_driver_version) &&
        succeeded(adapter->vtable->get_property(adapter, dxcore_property_driver_version, sizeof(value64), &value64))) {
        write_text("driver_version_raw=");
        write_hex32((u32)(value64 >> 32U));
        write_hex32((u32)value64);
        write_text("\n");
    }
    if (adapter->vtable->is_property_supported(adapter, dxcore_property_is_hardware) &&
        succeeded(adapter->vtable->get_property(adapter, dxcore_property_is_hardware, sizeof(value_bool), &value_bool))) {
        write_key_text("hardware", value_bool ? "true" : "false");
    }
    if (adapter->vtable->is_property_supported(adapter, dxcore_property_is_integrated) &&
        succeeded(adapter->vtable->get_property(adapter, dxcore_property_is_integrated, sizeof(value_bool), &value_bool))) {
        write_key_text("integrated", value_bool ? "true" : "false");
    }
    if (adapter->vtable->is_property_supported(adapter, dxcore_property_dedicated_adapter_memory) &&
        succeeded(adapter->vtable->get_property(adapter, dxcore_property_dedicated_adapter_memory, sizeof(value64), &value64))) {
        write_key_u64("dedicated_adapter_bytes", value64);
    }
    if (adapter->vtable->is_property_supported(adapter, dxcore_property_dedicated_system_memory) &&
        succeeded(adapter->vtable->get_property(adapter, dxcore_property_dedicated_system_memory, sizeof(value64), &value64))) {
        write_key_u64("dedicated_system_bytes", value64);
    }
    if (adapter->vtable->is_property_supported(adapter, dxcore_property_shared_system_memory) &&
        succeeded(adapter->vtable->get_property(adapter, dxcore_property_shared_system_memory, sizeof(value64), &value64))) {
        write_key_u64("shared_system_bytes", value64);
    }
    write_key_text("attribute_gpu", adapter->vtable->is_attribute_supported(adapter, &attribute_gpu) ? "true" : "false");
    write_key_text("attribute_npu", adapter->vtable->is_attribute_supported(adapter, &attribute_npu) ? "true" : "false");
    write_key_text("attribute_d3d12_graphics", adapter->vtable->is_attribute_supported(adapter, &attribute_d3d12_graphics) ? "true" : "false");
    write_key_text("attribute_d3d12_core_compute", adapter->vtable->is_attribute_supported(adapter, &attribute_d3d12_core_compute) ? "true" : "false");
    write_key_text("attribute_d3d12_generic_ml", adapter->vtable->is_attribute_supported(adapter, &attribute_d3d12_generic_ml) ? "true" : "false");
}

static void enumerate_attribute(DxCoreAdapterFactory *factory, const char *name, const Guid *attribute, int probe_ml) {
    DxCoreAdapterList *list = 0;
    u32 index;
    HResult result = factory->vtable->create_adapter_list(factory, 1U, attribute, &iid_adapter_list, (void **)&list);

    write_text("\n[list]\n");
    write_key_text("filter", name);
    if (!succeeded(result) || list == 0) {
        write_text("status=unavailable\n");
        return;
    }
    write_key_u64("count", list->vtable->get_adapter_count(list));
    for (index = 0U; index < list->vtable->get_adapter_count(list); ++index) {
        DxCoreAdapter *adapter = 0;
        if (succeeded(list->vtable->get_adapter(list, index, &iid_adapter, (void **)&adapter)) && adapter != 0) {
            report_adapter(adapter, name, index);
            if (probe_ml) probe_directml(adapter);
            adapter->vtable->release(adapter);
        }
    }
    list->vtable->release(list);
}

static void enumerate_ml_npus(DxCoreAdapterFactory *factory) {
    DxCoreAdapterList *list = 0;
    HResult result = factory->vtable->create_adapter_list_by_workload(
        factory, dxcore_workload_machine_learning, 0U, dxcore_hardware_type_npu,
        &iid_adapter_list, (void **)&list);
    u32 index;

    write_text("\n[list]\nfilter=workload.machine_learning.npu\n");
    if (!succeeded(result) || list == 0) {
        write_text("status=unavailable\n");
        return;
    }
    write_key_u64("count", list->vtable->get_adapter_count(list));
    for (index = 0U; index < list->vtable->get_adapter_count(list); ++index) {
        DxCoreAdapter *adapter = 0;
        if (succeeded(list->vtable->get_adapter(list, index, &iid_adapter, (void **)&adapter)) && adapter != 0) {
            report_adapter(adapter, "workload.machine_learning.npu", index);
            adapter->vtable->release(adapter);
        }
    }
    list->vtable->release(list);
}

void mainCRTStartup(void) {
    DxCoreAdapterFactory *factory = 0;
    usize index;
    HResult result;
    u64 counter_frequency;

    stdout_handle = GetStdHandle((u32)-11);
    write_text("[cpu]\n");
    for (index = 0U; index < sizeof(cpu_features) / sizeof(cpu_features[0]); ++index) {
        write_key_text(cpu_features[index].name, IsProcessorFeaturePresent(cpu_features[index].id) ? "true" : "false");
    }
#if defined(__aarch64__)
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(counter_frequency));
    write_key_u64("counter_frequency_hz", counter_frequency);
#endif

    write_text("\n[dxcore]\n");
    result = DXCoreCreateAdapterFactory(&iid_adapter_factory1, (void **)&factory);
    if (!succeeded(result) || factory == 0) {
        write_text("factory1=false\n");
        ExitProcess(1U);
    }
    write_text("factory1=true\n");
    enumerate_attribute(factory, "hardware.gpu", &attribute_gpu, 0);
    enumerate_attribute(factory, "hardware.compute_accelerator", &attribute_compute_accelerator, 0);
    enumerate_attribute(factory, "hardware.npu", &attribute_npu, 0);
    enumerate_attribute(factory, "d3d12.graphics", &attribute_d3d12_graphics, 0);
    enumerate_attribute(factory, "d3d12.core_compute", &attribute_d3d12_core_compute, 0);
    enumerate_attribute(factory, "d3d12.generic_ml", &attribute_d3d12_generic_ml, 1);
    enumerate_ml_npus(factory);
    factory->vtable->release(factory);
    ExitProcess(0U);
}