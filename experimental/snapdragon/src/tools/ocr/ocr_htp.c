#include "../../shared/qnn_abi.h"
#include "ocr_tokenizer.h"

__declspec(dllimport) void *LoadLibraryExW(const unsigned short *, void *, u32);
__declspec(dllimport) void *GetProcAddress(void *, const char *);
__declspec(dllimport) int FreeLibrary(void *);
__declspec(dllimport) void *GetStdHandle(u32);
__declspec(dllimport) int WriteFile(void *, const void *, u32, u32 *, void *);
__declspec(dllimport) void *CreateFileW(const unsigned short *, u32, u32, void *, u32, u32, void *);
__declspec(dllimport) int GetFileSizeEx(void *, long long *);
__declspec(dllimport) int ReadFile(void *, void *, u32, u32 *, void *);
__declspec(dllimport) int CloseHandle(void *);

static u8 fixture_data[16U * 1024U * 1024U];
static u16 oracle_output[16384];

static u32 load32(const u8 *data) {
    return data[0] | (u32)data[1] << 8 | (u32)data[2] << 16 | (u32)data[3] << 24;
}

static u32 read_fixtures(const unsigned short *path) {
    void *file = CreateFileW(path,0x80000000U,1,0,3,0x08000080U,0);
    long long size = 0;
    u32 total = 0, received;
    int valid = 0;
    if (file == (void *)~0ULL) return 0;
    if (!GetFileSizeEx(file,&size) || size < 132 || (u64)size > sizeof(fixture_data)) goto done;
    while (total < (u32)size) {
        if (!ReadFile(file,fixture_data+total,(u32)size-total,&received,0) || !received) goto done;
        total += received;
    }
    u8 extra;
    if (!ReadFile(file,&extra,1,&received,0) || received) goto done;
    valid = 1;
done:
    if (!CloseHandle(file)) valid = 0;
    return valid ? total : 0;
}

static int output_good = 1;
static QnnProfileHandle execution_profile;

static void text(const char *message) {
    u32 length = 0, written;
    while (message[length]) ++length;
    if (!WriteFile(GetStdHandle((u32)-11), message, length, &written, 0) || written != length) output_good = 0;
}

static int checked(const char *label, u64 code) {
    static const char hex[] = "0123456789abcdef";
    char digits[19] = "0x0000000000000000";
    text(label); text(": ");
    for (u32 index = 0; index < 16; ++index) digits[17 - index] = hex[(code >> (index * 4)) & 15];
    text(digits); text("\n");
    return !code;
}

static int profile_events(const QnnInterfaceV2 *api, const u64 *events, u32 count, u32 depth, u64 *cycles, u64 *microseconds) {
    if (depth > 8 || count > 4096 || (count && !events)) return 0;
    for (u32 index = 0; index < count; ++index) {
        QnnProfileEventData data;
        const u64 *children = 0;
        u32 child_count = 0;
        if (api->profile_get_event_data(events[index],&data)) return 0;
        if (data.type == 3003) *cycles += data.value;
        if (data.type == 3004) *microseconds += data.value;
        if (api->profile_get_sub_events(events[index],&children,&child_count) ||
            !profile_events(api,children,child_count,depth+1,cycles,microseconds)) return 0;
    }
    return 1;
}

static int execute(const QnnInterfaceV2 *api, QnnGraphHandle graph, QnnTensor *inputs, u32 count, QnnTensor *output) {
    const u64 *events = 0;
    u32 event_count = 0;
    u64 cycles = 0, microseconds = 0;
    if (!checked("graph_execute",api->graph_execute(graph,inputs,count,output,1,execution_profile,0)) ||
        api->profile_get_events(execution_profile,&events,&event_count) ||
        !profile_events(api,events,event_count,0,&cycles,&microseconds) || (!cycles && !microseconds)) {
        text("FAIL accelerator profile evidence\n"); return 0;
    }
    checked("accelerator_cycles",cycles);
    checked("accelerator_microseconds",microseconds);
    return 1;
}

static QnnTensor tensor(const char *name, u32 type, u32 *dimensions) {
    QnnTensor result = {0};
    result.version = QNN_TENSOR_VERSION_1;
    result.data.v1.name = name;
    result.data.v1.type = type;
    result.data.v1.data_format = QNN_TENSOR_DATA_FORMAT_DENSE;
    result.data.v1.data_type = QNN_DATATYPE_FLOAT_16;
    result.data.v1.quantize_params.encoding_definition = QNN_DEFINITION_UNDEFINED;
    result.data.v1.quantize_params.quantization_encoding = QNN_QUANTIZATION_ENCODING_UNDEFINED;
    result.data.v1.rank = 2;
    result.data.v1.dimensions = dimensions;
    result.data.v1.memory_type = QNN_TENSORMEMTYPE_RAW;
    return result;
}

static int node(const QnnInterfaceV2 *api, QnnGraphHandle graph, const char *name, const char *type,
                 QnnTensor *inputs, u32 count, QnnTensor *output, QnnParam *parameters, u32 parameter_count) {
    QnnOpConfig operation = {0};
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = name;
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = type;
    operation.data.v1.input_count = count;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = 1;
    operation.data.v1.outputs = output;
    operation.data.v1.parameters = parameters;
    operation.data.v1.parameter_count = parameter_count;
    return checked(type,api->graph_add_node(graph,operation));
}

static int primitive(const QnnInterfaceV2 *api, QnnContextHandle context, u32 operation,
                      u32 rows, u32 width, u32 output_width, u8 *source, u8 *weights, const u8 *reference, const char *name) {
    static const char *const operations[] = {"", "MatMul", "RmsNorm", "LayerNorm", "Sigmoid", "Gelu", "Softmax"};
    u32 dimensions[2] = {rows,width}, output_dimensions[2] = {rows,output_width};
    u32 weight_dimensions[2] = {width,output_width}, axis_dimensions[1] = {1}, axes[1] = {1};
    QnnGraphHandle graph = 0;
    QnnTensor inputs[3] = {tensor("input",QNN_TENSOR_TYPE_APP_WRITE,dimensions),
        tensor("weight",QNN_TENSOR_TYPE_STATIC,weight_dimensions),tensor("bias",QNN_TENSOR_TYPE_STATIC,weight_dimensions)};
    QnnTensor output = tensor("output",QNN_TENSOR_TYPE_APP_READ,output_dimensions);
    QnnParam parameters[2] = {0};
    u32 scalar_dimensions[2] = {1,1};
    _Float16 one_value = 1;
    u32 input_count = operation == 1 || operation == 2 ? 2 : operation == 3 ? 3 : 1;
    u32 parameter_count = 0;
    text(name); text("\n");
    if (operation == 2 || operation == 3) {
        weight_dimensions[0] = width;
        inputs[1].data.v1.rank = inputs[2].data.v1.rank = 1;
    }
    inputs[1].data.v1.memory.client_buffer.data = weights;
    inputs[1].data.v1.memory.client_buffer.data_size = (operation == 1 ? width * output_width : width) * 2;
    inputs[2].data.v1.memory.client_buffer.data = weights + width * 2;
    inputs[2].data.v1.memory.client_buffer.data_size = width * 2;
    if (!checked("graph_create",api->graph_create(context,name,0,&graph))) return 0;
    for (u32 index = 0; index < input_count; ++index)
        if (!checked("tensor_create",api->tensor_create_graph_tensor(graph,&inputs[index]))) return 0;
    if (!checked("output_create",api->tensor_create_graph_tensor(graph,&output))) return 0;
    if (operation == 2 || operation == 3) {
        parameter_count = 2;
        parameters[0].type = QNN_PARAMTYPE_SCALAR;
        parameters[0].name = "epsilon";
        parameters[0].value.scalar.data_type = QNN_DATATYPE_FLOAT_32;
        parameters[0].value.scalar.value.float_value = 1e-5f;
        parameters[1].type = QNN_PARAMTYPE_TENSOR;
        parameters[1].name = "axes";
        parameters[1].value.tensor = tensor("axes",QNN_TENSOR_TYPE_STATIC,axis_dimensions);
        parameters[1].value.tensor.data.v1.rank = 1;
        parameters[1].value.tensor.data.v1.data_type = QNN_DATATYPE_UINT_32;
        parameters[1].value.tensor.data.v1.memory.client_buffer.data = axes;
        parameters[1].value.tensor.data.v1.memory.client_buffer.data_size = sizeof(axes);
        if (!checked("axes_create",api->tensor_create_graph_tensor(graph,&parameters[1].value.tensor))) return 0;
    }
    if (operation == 6) {
        parameter_count = 1;
        parameters[0].type = QNN_PARAMTYPE_SCALAR;
        parameters[0].name = "axis";
        parameters[0].value.scalar.data_type = QNN_DATATYPE_UINT_32;
        parameters[0].value.scalar.value.uint32_value = 1;
    }
    if (operation == 4) {
        QnnTensor negative = tensor("negative",QNN_TENSOR_TYPE_NATIVE,dimensions);
        QnnTensor exponential = tensor("exponential",QNN_TENSOR_TYPE_NATIVE,dimensions);
        QnnTensor denominator = tensor("denominator",QNN_TENSOR_TYPE_NATIVE,dimensions);
        QnnTensor one = tensor("one",QNN_TENSOR_TYPE_STATIC,scalar_dimensions);
        one.data.v1.memory.client_buffer.data = &one_value;
        one.data.v1.memory.client_buffer.data_size = 2;
        if (!checked("negative_tensor",api->tensor_create_graph_tensor(graph,&negative)) ||
            !checked("exponential_tensor",api->tensor_create_graph_tensor(graph,&exponential)) ||
            !checked("denominator_tensor",api->tensor_create_graph_tensor(graph,&denominator)) ||
            !checked("one_tensor",api->tensor_create_graph_tensor(graph,&one)) ||
            !node(api,graph,"negate","ElementWiseNeg",inputs,1,&negative,0,0) ||
            !node(api,graph,"exp","ElementWiseExp",&negative,1,&exponential,0,0)) return 0;
        QnnTensor sum_inputs[2] = {exponential,one}, divide_inputs[2] = {inputs[0],denominator};
        if (!node(api,graph,"plus_one","ElementWiseAdd",sum_inputs,2,&denominator,0,0) ||
            !node(api,graph,"silu","ElementWiseDivide",divide_inputs,2,&output,0,0)) return 0;
    } else if (!node(api,graph,"operation",operations[operation],inputs,input_count,&output,parameters,parameter_count)) return 0;
    if (!checked("graph_finalize",api->graph_finalize(graph,0,0))) return 0;
    inputs[0].data.v1.memory.client_buffer.data = source;
    inputs[0].data.v1.memory.client_buffer.data_size = rows * width * 2;
    output.data.v1.memory.client_buffer.data = oracle_output;
    output.data.v1.memory.client_buffer.data_size = rows * output_width * 2;
    float maximum = 0;
    for (u32 run = 0; run < 3; ++run) {
        for (u32 index = 0; index < rows * output_width; ++index) oracle_output[index] = 0x7e00;
        if (!execute(api,graph,inputs,1,&output)) return 0;
        for (u32 index = 0; index < rows * output_width; ++index) {
            union {u32 bits; float value;} expected;
            union {u16 bits; _Float16 value;} actual;
            expected.bits = load32(reference + index * 4);
            actual.bits = oracle_output[index];
            float error = (float)actual.value - expected.value;
            if (error < 0) error = -error;
            float magnitude = expected.value < 0 ? -expected.value : expected.value;
            if ((actual.bits & 0x7c00) == 0x7c00 || !(error <= 0.003f + 0.005f * magnitude)) {
                checked("FAIL numerical element",index);
                checked("actual_fp16_bits",actual.bits);
                checked("reference_fp32_bits",expected.bits);
                return 0;
            }
            if (error > maximum) maximum = error;
        }
    }
    checked("max_absolute_error_micro_units_ceil",(u64)(maximum * 1000000.0f + 0.999f));
    text("PASS FP32 oracle comparison, three executions\n");
    return 1;
}

static int primitives(const QnnInterfaceV2 *api, QnnContextHandle context, u32 size) {
    static const char *const names[] = {"patch_projection","vision_qkv","text_query","rmsnorm_64","rmsnorm_128","rmsnorm_1024","rmsnorm_1536",
        "connector_layernorm","mlp_silu","connector_gelu","attention_softmax"};
    u32 cursor = 132, count = load32(fixture_data + 128);
    if (count != sizeof(names) / sizeof(names[0])) return 0;
    for (u32 index = 0; index < count; ++index) {
        if (size - cursor < 28) return 0;
        u32 operation = load32(fixture_data+cursor), rows = load32(fixture_data+cursor+4), width = load32(fixture_data+cursor+8);
        u32 output_width = load32(fixture_data+cursor+12), input_bytes = load32(fixture_data+cursor+16);
        u32 weight_bytes = load32(fixture_data+cursor+20), reference_bytes = load32(fixture_data+cursor+24);
        cursor += 28;
        if (!operation || operation > 6 || !rows || rows > 16 || !width || width > 4608 || !output_width || output_width > 4608 ||
            rows * output_width > 16384 || rows * width * 2 != input_bytes || rows * output_width * 4 != reference_bytes ||
            (operation != 1 && width != output_width) || weight_bytes != (operation == 1 ? width * output_width * 2 : operation == 2 ? width * 2 : operation == 3 ? width * 4 : 0) ||
            (u64)input_bytes + weight_bytes + reference_bytes > size - cursor) return 0;
        if (!primitive(api,context,operation,rows,width,output_width,fixture_data+cursor,fixture_data+cursor+input_bytes,
                       fixture_data+cursor+input_bytes+weight_bytes,names[index])) return 0;
        cursor += input_bytes + weight_bytes + reference_bytes;
    }
    return cursor == size;
}

static int addition(const QnnInterfaceV2 *api, QnnContextHandle context, u32 width, const char *name) {
    _Float16 left[1536], right[1536], result[1536];
    u32 dimensions[2] = {1,width};
    QnnGraphHandle graph = 0;
    QnnTensor inputs[2] = {tensor("left",QNN_TENSOR_TYPE_APP_WRITE,dimensions), tensor("right",QNN_TENSOR_TYPE_APP_WRITE,dimensions)};
    QnnTensor destination = tensor("sum",QNN_TENSOR_TYPE_APP_READ,dimensions);
    QnnOpConfig operation = {0};
    text(name); text("\n");
    if (!checked("graph_create",api->graph_create(context,name,0,&graph))) return 0;
    for (u32 index = 0; index < 2; ++index)
        if (!checked("input_tensor",api->tensor_create_graph_tensor(graph,&inputs[index]))) return 0;
    if (!checked("output_tensor",api->tensor_create_graph_tensor(graph,&destination))) return 0;
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = "add";
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = "ElementWiseAdd";
    operation.data.v1.input_count = 2;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = 1;
    operation.data.v1.outputs = &destination;
    if (!checked("graph_add_node",api->graph_add_node(graph,operation)) || !checked("graph_finalize",api->graph_finalize(graph,0,0))) return 0;
    inputs[0].data.v1.memory.client_buffer.data = left;
    inputs[1].data.v1.memory.client_buffer.data = right;
    inputs[0].data.v1.memory.client_buffer.data_size = width * 2;
    inputs[1].data.v1.memory.client_buffer.data_size = width * 2;
    destination.data.v1.memory.client_buffer.data = result;
    destination.data.v1.memory.client_buffer.data_size = width * 2;
    for (u32 run = 0; run < 3; ++run) {
        for (u32 index = 0; index < width; ++index) {
            left[index] = (_Float16)(((int)(index % 31) - 15) * 0.125f + run * 0.25f);
            right[index] = (_Float16)(((int)(index % 17) - 8) * 0.0625f);
            result[index] = (_Float16)-999.0f;
        }
        if (!execute(api,graph,inputs,2,&destination)) return 0;
        for (u32 index = 0; index < width; ++index)
            if ((float)result[index] != (float)left[index] + (float)right[index]) { text("FAIL numerical comparison\n"); return 0; }
    }
    text("PASS exact FP16 output, three distinct executions\n");
    return 1;
}

int ocr_htp_test(const unsigned short *library, const unsigned short *fixtures) {
    void *module = 0;
    QnnBackendHandle backend = 0;
    QnnDeviceHandle device = 0;
    QnnContextHandle context = 0;
    const QnnInterfaceV2 *api = 0;
    const QnnInterfacePrefix **providers = 0;
    QnnInterfaceGetProviders get_providers;
    u32 count = 0;
    int good = 0, clean = 1;
    output_good = 1;
    execution_profile = 0;
    u32 fixture_size = read_fixtures(fixtures);
    if (!ocr_artifact(fixture_data,fixture_size,5)) { text("FAIL HTP fixture verification\n"); return 0; }
    module = LoadLibraryExW(library,0,0x1100);
    if (!module) { text("FAIL loading explicit HTP library\n"); goto done; }
    get_providers = (QnnInterfaceGetProviders)GetProcAddress(module,"QnnInterface_getProviders");
    if (!get_providers || !checked("get_providers",get_providers(&providers,&count)) || !providers || !count || count > 32) goto done;
    for (u32 index = 0; index < count; ++index) {
        if (providers[index] && providers[index]->provider_name && providers[index]->backend_id == 6 &&
            providers[index]->api_version.core_api_version.major == 2 && providers[index]->api_version.core_api_version.minor == 39) {
            api = &((const QnnInterfaceProviderV2 *)providers[index])->api;
            text("Provider: "); text(providers[index]->provider_name); text("\n");
            checked("backend_id",providers[index]->backend_id);
            break;
        }
    }
    if (!api || !api->backend_create || !api->backend_free || !api->device_create || !api->device_free || !api->context_create || !api->context_free ||
        !api->graph_create || !api->tensor_create_graph_tensor || !api->graph_add_node || !api->graph_finalize || !api->graph_execute ||
        !api->profile_create || !api->profile_free || !api->profile_get_events || !api->profile_get_sub_events || !api->profile_get_event_data) goto done;
    if (!checked("backend_create",api->backend_create(0,0,&backend)) || !checked("device_create",api->device_create(0,0,&device)) ||
        !checked("context_create",api->context_create(backend,device,0,&context))) goto done;
    if (!checked("profile_create",api->profile_create(backend,2,&execution_profile))) goto done;
    good = addition(api,context,1024,"ocr_vision_add") && addition(api,context,1536,"ocr_text_add") && primitives(api,context,fixture_size);
done:
    if (context && !checked("context_free",api->context_free(context,0))) clean = 0;
    if (execution_profile && !checked("profile_free",api->profile_free(execution_profile))) clean = 0;
    if (device && !checked("device_free",api->device_free(device))) clean = 0;
    if (backend && !checked("backend_free",api->backend_free(backend))) clean = 0;
    if (module && !FreeLibrary(module)) clean = 0;
    text(good && clean ? "PASS OCR HTP probe\n" : "FAIL OCR HTP probe\n");
    return good && clean && output_good;
}