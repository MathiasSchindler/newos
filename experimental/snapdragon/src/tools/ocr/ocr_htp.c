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

static u8 fixture_data[144U * 1024U * 1024U];
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
static const unsigned short *capture_directory;

static int capture_tensor(u32 block_index, const char *suffix, const void *data, u32 bytes) {
    unsigned short path[32768];
    u32 length = 0, written = 0;
    while (capture_directory[length]) {
        if (length >= 32700) return 0;
        path[length] = capture_directory[length]; ++length;
    }
    path[length++] = '\\'; path[length++] = '0'+block_index;
    for (u32 index = 0; suffix[index]; ++index) path[length++] = (unsigned char)suffix[index];
    path[length] = 0;
    void *file = CreateFileW(path,0x40000000U,0,0,1,0x80,0);
    if (file == (void *)~0ULL) return 0;
    int good = WriteFile(file,data,bytes,&written,0) && written == bytes;
    if (!CloseHandle(file)) good = 0;
    return good;
}

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
    u32 bias_dimensions[1] = {output_width};
    QnnGraphHandle graph = 0;
    QnnTensor inputs[3] = {tensor("input",QNN_TENSOR_TYPE_APP_WRITE,dimensions),
        tensor("weight",QNN_TENSOR_TYPE_STATIC,weight_dimensions),tensor("bias",QNN_TENSOR_TYPE_STATIC,weight_dimensions)};
    QnnTensor output = tensor("output",QNN_TENSOR_TYPE_APP_READ,output_dimensions);
    QnnParam parameters[2] = {0};
    u32 scalar_dimensions[2] = {1,1};
    _Float16 one_value = 1;
    u32 input_count = operation == 1 || operation == 2 ? 2 : operation == 3 || operation == 7 ? 3 : 1;
    u32 parameter_count = 0;
    text(name); text("\n");
    if (operation == 2 || operation == 3) {
        weight_dimensions[0] = width;
        inputs[1].data.v1.rank = inputs[2].data.v1.rank = 1;
    }
    inputs[1].data.v1.memory.client_buffer.data = weights;
    inputs[1].data.v1.memory.client_buffer.data_size = (operation == 1 || operation == 7 ? width * output_width : width) * 2;
    inputs[2].data.v1.memory.client_buffer.data = weights + width * 2;
    inputs[2].data.v1.memory.client_buffer.data_size = width * 2;
    if (operation == 7) {
        inputs[2].data.v1.rank = 1;
        inputs[2].data.v1.dimensions = bias_dimensions;
        inputs[2].data.v1.memory.client_buffer.data = weights + width * output_width * 2;
        inputs[2].data.v1.memory.client_buffer.data_size = output_width * 2;
    }
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
    if (operation == 7) {
        QnnTensor product = tensor("product",QNN_TENSOR_TYPE_NATIVE,output_dimensions);
        if (!checked("product_tensor",api->tensor_create_graph_tensor(graph,&product)) ||
            !node(api,graph,"projection","MatMul",inputs,2,&product,0,0)) return 0;
        QnnTensor sum_inputs[2] = {product,inputs[2]};
        if (!node(api,graph,"bias_add","ElementWiseAdd",sum_inputs,2,&output,0,0)) return 0;
    } else if (operation == 4) {
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

#ifdef OCR_MATRIX_RESIDUAL
#define OCR_ATTENTION_TENSORS 160
#else
#define OCR_ATTENTION_TENSORS 128
#endif

typedef struct {
    const QnnInterfaceV2 *api;
    QnnGraphHandle graph;
    QnnTensor tensors[OCR_ATTENTION_TENSORS];
    u32 dimensions[OCR_ATTENTION_TENSORS][4];
    char names[OCR_ATTENTION_TENSORS][5];
    u32 count;
    int good;
#ifdef OCR_MATRIX_RESIDUAL
    u32 diagnostic_quotient;
    u32 diagnostic_residual;
#endif
} OcrAttentionGraph;

static u16 attention_output[4456448];
static u16 chain_input[131072];
#ifdef OCR_CAPTURE_INTERNALS
static u16 internal_output[3674112];
static u16 internal_previous[3674112];
#endif
#ifdef OCR_MATRIX_RESIDUAL
#define OCR_RESIDUAL_GROUP 256
static _Float16 residual_identity[524288*OCR_RESIDUAL_GROUP];
#endif
#ifdef OCR_MATRIX_ROPE
static float rope_matrix[128*128*64];
#endif

static u32 attention_elements(u32 tap, u32 tokens) {
    return tap == 6 || tap == 7 ? 16*tokens*tokens : tokens*(tap == 1 ? 3072 : tap >= 12 && tap <= 15 ? 4096 : 1024);
}

static u32 attention_tensor(OcrAttentionGraph *builder, u32 type, u32 dtype, u32 rank, const u32 *shape, void *data) {
    u32 index = builder->count, elements = 1;
    if (!builder->good || index >= OCR_ATTENTION_TENSORS || !rank || rank > 4) { builder->good = 0; return 0; }
    ++builder->count;
    builder->names[index][0] = 't';
    builder->names[index][1] = '0'+index/100;
    builder->names[index][2] = '0'+index/10%10;
    builder->names[index][3] = '0'+index%10;
    builder->names[index][4] = 0;
    for (u32 axis = 0; axis < rank; ++axis) { builder->dimensions[index][axis] = shape[axis]; elements *= shape[axis]; }
    QnnTensor *value = &builder->tensors[index];
    *value = tensor(builder->names[index],type,builder->dimensions[index]);
    value->data.v1.rank = rank;
    value->data.v1.data_type = dtype;
    value->data.v1.memory.client_buffer.data = data;
    value->data.v1.memory.client_buffer.data_size = data ? elements * (dtype == QNN_DATATYPE_FLOAT_16 ? 2 : 4) : 0;
    if (!checked("attention_tensor",builder->api->tensor_create_graph_tensor(builder->graph,value))) builder->good = 0;
    return index;
}

static u32 attention_op(OcrAttentionGraph *builder, const char *operation, const u32 *ids, u32 count, u32 rank, const u32 *shape,
                        u32 dtype, int tap, QnnParam *parameters, u32 parameter_count) {
    QnnTensor inputs[3];
    if (!builder->good || count > 3) { builder->good = 0; return 0; }
    for (u32 index = 0; index < count; ++index) inputs[index] = builder->tensors[ids[index]];
    u32 output = attention_tensor(builder,tap ? QNN_TENSOR_TYPE_APP_READ : QNN_TENSOR_TYPE_NATIVE,dtype,rank,shape,0);
    if (builder->good && !node(builder->api,builder->graph,builder->names[output],operation,inputs,count,&builder->tensors[output],parameters,parameter_count)) builder->good = 0;
    return output;
}

static QnnParam attention_axis(const char *name, u32 axis);

static u32 attention_divide(OcrAttentionGraph *builder, const u32 *operands, u32 rank, const u32 *shape, int tap, u32 block_index) {
#ifdef OCR_REFINE_DIVIDE
    if (block_index == 1) {
#ifdef OCR_MATRIX_RESIDUAL
        u32 elements = 1;
        for (u32 axis = 0; axis < rank; ++axis) elements *= shape[axis];
        if (rank != 2 || elements > 524288 || elements%OCR_RESIDUAL_GROUP) { builder->good = 0; return 0; }
        u32 quotient = attention_op(builder,"ElementWiseDivide",operands,2,rank,shape,QNN_DATATYPE_FLOAT_16,1,0,0);
        u32 row_shape[3] = {elements/OCR_RESIDUAL_GROUP,1,OCR_RESIDUAL_GROUP};
        u32 diagonal_shape[3] = {elements/OCR_RESIDUAL_GROUP,OCR_RESIDUAL_GROUP,OCR_RESIDUAL_GROUP};
        u32 left_shape[3] = {elements/OCR_RESIDUAL_GROUP,1,2*OCR_RESIDUAL_GROUP};
        u32 right_shape[3] = {elements/OCR_RESIDUAL_GROUP,2*OCR_RESIDUAL_GROUP,OCR_RESIDUAL_GROUP};
        for (u32 batch = 0; batch < elements/OCR_RESIDUAL_GROUP; ++batch)
            for (u32 row = 0; row < OCR_RESIDUAL_GROUP; ++row)
                for (u32 column = 0; column < OCR_RESIDUAL_GROUP; ++column)
                    residual_identity[(batch*OCR_RESIDUAL_GROUP+row)*OCR_RESIDUAL_GROUP+column] = (_Float16)(row == column);
        u32 dividend_column = attention_op(builder,"Reshape",&operands[0],1,3,row_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
        u32 quotient_column = attention_op(builder,"Reshape",&quotient,1,3,row_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
        u32 divisor_column = attention_op(builder,"Reshape",&operands[1],1,3,row_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
        u32 negative_divisor = attention_op(builder,"ElementWiseNeg",&divisor_column,1,3,row_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
        u32 identity = attention_tensor(builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,3,diagonal_shape,residual_identity);
        QnnParam concat_axis = attention_axis("axis",2);
        u32 ids[2] = {dividend_column,quotient_column};
        u32 left = attention_op(builder,"Concat",ids,2,3,left_shape,QNN_DATATYPE_FLOAT_16,0,&concat_axis,1);
        ids[0] = negative_divisor; ids[1] = identity;
        u32 diagonal = attention_op(builder,"ElementWiseMultiply",ids,2,3,diagonal_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
        concat_axis = attention_axis("axis",1);
        ids[0] = identity; ids[1] = diagonal;
        u32 right = attention_op(builder,"Concat",ids,2,3,right_shape,QNN_DATATYPE_FLOAT_16,0,&concat_axis,1);
        ids[0] = left; ids[1] = right;
        u32 dot = attention_op(builder,"MatMul",ids,2,3,row_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
        u32 residual = attention_op(builder,"Reshape",&dot,1,rank,shape,QNN_DATATYPE_FLOAT_16,1,0,0);
        builder->diagnostic_quotient = quotient;
        builder->diagnostic_residual = residual;
#else
        u32 quotient = attention_op(builder,"ElementWiseDivide",operands,2,rank,shape,QNN_DATATYPE_FLOAT_16,0,0,0);
        u32 ids[2] = {quotient,operands[1]};
        u32 product = attention_op(builder,"ElementWiseMultiply",ids,2,rank,shape,QNN_DATATYPE_FLOAT_16,0,0,0);
        ids[0] = operands[0]; ids[1] = product;
        u32 residual = attention_op(builder,"ElementWiseSubtract",ids,2,rank,shape,QNN_DATATYPE_FLOAT_16,0,0,0);
    #endif
        ids[0] = residual; ids[1] = operands[1];
        u32 correction = attention_op(builder,"ElementWiseDivide",ids,2,rank,shape,QNN_DATATYPE_FLOAT_16,0,0,0);
        ids[0] = quotient; ids[1] = correction;
        return attention_op(builder,"ElementWiseAdd",ids,2,rank,shape,QNN_DATATYPE_FLOAT_16,tap,0,0);
    }
#else
    (void)block_index;
#endif
    return attention_op(builder,"ElementWiseDivide",operands,2,rank,shape,QNN_DATATYPE_FLOAT_16,tap,0,0);
}

static QnnParam attention_axis(const char *name, u32 axis) {
    QnnParam result = {0};
    result.name = name; result.type = QNN_PARAMTYPE_SCALAR;
    result.value.scalar.data_type = QNN_DATATYPE_INT_32;
    result.value.scalar.value.int32_value = (int)axis;
    return result;
}

static u32 attention_norm(OcrAttentionGraph *builder, u32 input, u32 gamma, u32 rank, const u32 *shape, u32 *axis) {
    u32 one[1] = {1}, ids[2] = {input,gamma};
    u32 axis_id = attention_tensor(builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_UINT_32,1,one,axis);
    QnnParam parameters[2] = {0};
    parameters[0].name = "epsilon"; parameters[0].type = QNN_PARAMTYPE_SCALAR;
    parameters[0].value.scalar.data_type = QNN_DATATYPE_FLOAT_32;
    parameters[0].value.scalar.value.float_value = 1e-5f;
    parameters[1].name = "axes"; parameters[1].type = QNN_PARAMTYPE_TENSOR;
    parameters[1].value.tensor = builder->tensors[axis_id];
        return attention_op(builder,"RmsNorm",ids,2,rank,shape,QNN_DATATYPE_FLOAT_16,1,parameters,2);
}

static u32 attention_transpose(OcrAttentionGraph *builder, u32 input, const u32 *shape, u32 *permutation) {
    u32 shape_perm[1] = {3};
    u32 perm_id = attention_tensor(builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_UINT_32,1,shape_perm,permutation);
    QnnParam parameter = {0}; parameter.name = "perm"; parameter.type = QNN_PARAMTYPE_TENSOR;
    parameter.value.tensor = builder->tensors[perm_id];
        return attention_op(builder,"Transpose",&input,1,3,shape,QNN_DATATYPE_FLOAT_16,0,&parameter,1);
}

static int attention_probe(const QnnInterfaceV2 *api, QnnContextHandle context, u8 *source, u8 *constants, const u8 *references, int full_block, u32 tokens, u32 block_index, u16 *next_input) {
    OcrAttentionGraph builder = {0}; builder.api = api; builder.good = 1;
    u32 flat[2] = {tokens,1024}, fused[2] = {tokens,3072}, heads[3] = {tokens,16,64}, batched[3] = {16,tokens,64};
    u32 key_shape[3] = {16,64,tokens}, score_shape[3] = {16,tokens,tokens};
    u32 shape_qkv[2] = {1024,3072}, shape_proj[2] = {1024,1024}, width[1] = {1024}, qkv_width[1] = {3072}, head_width[1] = {64};
    u32 frequency_shape[3] = {tokens,1,64}, scalar_shape[1] = {1}, scalar_axis = 1, head_axis = 2;
    u32 permutation[3] = {1,0,2}, key_permutation[3] = {0,2,1};
    u32 selections[3][1024], rotation[64]; float signs[64]; _Float16 scale = (_Float16)0.125f;
    for (u32 part = 0; part < 3; ++part) for (u32 index = 0; index < 1024; ++index) selections[part][index] = part*1024+index;
    for (u32 index = 0; index < 64; ++index) { rotation[index] = (index+32)%64; signs[index] = index < 32 ? -1.0f : 1.0f; }
    checked("vision_block_index",block_index);
    if (!checked("attention_graph",api->graph_create(context,block_index ? "vision_attention_1" : "vision_attention_0",0,&builder.graph))) return 0;
    u32 input = attention_tensor(&builder,QNN_TENSOR_TYPE_APP_WRITE,QNN_DATATYPE_FLOAT_16,2,flat,0);
    u32 gamma = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,width,constants); constants += 2048;
    u32 qkv_weight = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,2,shape_qkv,constants); constants += 6291456;
    u32 qkv_bias = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,qkv_width,constants); constants += 6144;
    u32 q_gamma = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,head_width,constants); constants += 128;
    u32 k_gamma = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,head_width,constants); constants += 128;
    u32 cosine = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_32,3,frequency_shape,constants); constants += tokens*256;
    u32 sine = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_32,3,frequency_shape,constants); constants += tokens*256;
    u32 proj_weight = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,2,shape_proj,constants); constants += 2097152;
    u32 proj_bias = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,width,constants); constants += 2048;
    u32 rotate_indices = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_UINT_32,1,head_width,rotation);
    u32 sign_tensor = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_32,1,head_width,signs);
    u32 scale_tensor = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,scalar_shape,&scale);
    u32 taps[18], branches[3], ids[2];
    int internal_tap = 0;
#ifdef OCR_CAPTURE_INTERNALS
    internal_tap = 1;
    u32 internal_ids[10], internal_count = full_block ? 8 : 4, internal_elements = 0;
#endif
    u32 tap_count = full_block ? 18 : 11, total_elements = 0, tap_offsets[18];
    for (u32 tap = 0; tap < tap_count; ++tap) { tap_offsets[tap] = total_elements; total_elements += attention_elements(tap,tokens); }
    u32 wide[2] = {tokens,4096}, expanded_weight[2] = {1024,4096}, reduced_weight[2] = {4096,1024}, expanded_width[1] = {4096};
    _Float16 zero = 0, one = 1;
    taps[0] = attention_norm(&builder,input,gamma,2,flat,&scalar_axis);
    ids[0] = taps[0]; ids[1] = qkv_weight;
    u32 product = attention_op(&builder,"MatMul",ids,2,2,fused,QNN_DATATYPE_FLOAT_16,0,0,0);
    ids[0] = product; ids[1] = qkv_bias;
    taps[1] = attention_op(&builder,"ElementWiseAdd",ids,2,2,fused,QNN_DATATYPE_FLOAT_16,1,0,0);
    for (u32 part = 0; part < 3; ++part) {
        ids[0] = taps[1]; ids[1] = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_UINT_32,1,width,selections[part]);
        QnnParam axis = attention_axis("axis",1);
        u32 selected = attention_op(&builder,"Gather",ids,2,2,flat,QNN_DATATYPE_FLOAT_16,0,&axis,1);
        branches[part] = attention_op(&builder,"Reshape",&selected,1,3,heads,QNN_DATATYPE_FLOAT_16,0,0,0);
    }
    taps[2] = attention_norm(&builder,branches[0],q_gamma,3,heads,&head_axis);
    taps[3] = attention_norm(&builder,branches[1],k_gamma,3,heads,&head_axis);
#ifdef OCR_MATRIX_ROPE
    u32 matrix_width = 64;
#ifdef OCR_SPLIT_ROPE
    matrix_width = 128;
#endif
    u32 matrix_shape[3] = {tokens,matrix_width,64};
    const float *cosine_values = builder.tensors[cosine].data.v1.memory.client_buffer.data;
    const float *sine_values = builder.tensors[sine].data.v1.memory.client_buffer.data;
    for (u32 token = 0; token < tokens; ++token) {
        for (u32 row = 0; row < matrix_width; ++row) {
            for (u32 column = 0; column < 64; ++column) {
                float coefficient = row%64 == column ? cosine_values[token*64+column] :
                    row%64 == (column+32)%64 ? (column < 32 ? -1.0f : 1.0f)*sine_values[token*64+column] : 0.0f;
#ifdef OCR_SPLIT_ROPE
                float high = (float)(_Float16)coefficient;
                coefficient = row < 64 ? high : coefficient-high;
#endif
                rope_matrix[(token*matrix_width+row)*64+column] = coefficient;
            }
        }
    }
    u32 matrix_tensor = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_32,3,matrix_shape,rope_matrix);
    int use_matrix_rope = 1;
#ifdef OCR_ROPE_BLOCK1_ONLY
    use_matrix_rope = block_index != 0;
#endif
#endif
    for (u32 part = 0; part < 2; ++part) {
        u32 promoted = attention_op(&builder,"Cast",&taps[2+part],1,3,heads,QNN_DATATYPE_FLOAT_32,0,0,0);
        u32 sum;
#ifdef OCR_MATRIX_ROPE
        if (use_matrix_rope) {
            u32 rope_input = promoted;
#ifdef OCR_SPLIT_ROPE
            u32 duplicated_shape[3] = {tokens,16,128};
            QnnParam concat_axis = attention_axis("axis",2);
            ids[0] = promoted; ids[1] = promoted;
            rope_input = attention_op(&builder,"Concat",ids,2,3,duplicated_shape,QNN_DATATYPE_FLOAT_32,0,&concat_axis,1);
#endif
            ids[0] = rope_input; ids[1] = matrix_tensor;
            sum = attention_op(&builder,"MatMul",ids,2,3,heads,QNN_DATATYPE_FLOAT_32,0,0,0);
        } else
#endif
        {
            ids[0] = promoted; ids[1] = rotate_indices;
            QnnParam axis = attention_axis("axis",2);
            u32 swapped = attention_op(&builder,"Gather",ids,2,3,heads,QNN_DATATYPE_FLOAT_32,0,&axis,1);
            ids[0] = swapped; ids[1] = sign_tensor;
            u32 rotated = attention_op(&builder,"ElementWiseMultiply",ids,2,3,heads,QNN_DATATYPE_FLOAT_32,0,0,0);
            ids[0] = promoted; ids[1] = cosine;
            u32 real = attention_op(&builder,"ElementWiseMultiply",ids,2,3,heads,QNN_DATATYPE_FLOAT_32,0,0,0);
            ids[0] = rotated; ids[1] = sine;
            u32 imaginary = attention_op(&builder,"ElementWiseMultiply",ids,2,3,heads,QNN_DATATYPE_FLOAT_32,0,0,0);
            ids[0] = real; ids[1] = imaginary;
            sum = attention_op(&builder,"ElementWiseAdd",ids,2,3,heads,QNN_DATATYPE_FLOAT_32,0,0,0);
        }
        taps[4+part] = attention_op(&builder,"Cast",&sum,1,3,heads,QNN_DATATYPE_FLOAT_16,1,0,0);
        branches[part] = attention_transpose(&builder,taps[4+part],batched,permutation);
    }
    branches[2] = attention_transpose(&builder,branches[2],batched,permutation);
    u32 key_transposed = attention_transpose(&builder,branches[1],key_shape,key_permutation);
    ids[0] = branches[0]; ids[1] = key_transposed;
    product = attention_op(&builder,"MatMul",ids,2,3,score_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
    ids[0] = product; ids[1] = scale_tensor;
    taps[6] = attention_op(&builder,"ElementWiseMultiply",ids,2,3,score_shape,QNN_DATATYPE_FLOAT_16,1,0,0);
    u32 reduced_shape[2] = {16,tokens}, broadcast_shape[3] = {16,tokens,1};
    u32 reduction_axis = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_UINT_32,1,scalar_shape,&head_axis);
    QnnParam reduction = {0}; reduction.name = "axes"; reduction.type = QNN_PARAMTYPE_TENSOR;
    reduction.value.tensor = builder.tensors[reduction_axis];
    u32 maximum = attention_op(&builder,"ReduceMax",&taps[6],1,2,reduced_shape,QNN_DATATYPE_FLOAT_16,internal_tap,&reduction,1);
    u32 maximum_broadcast = attention_op(&builder,"Reshape",&maximum,1,3,broadcast_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
    ids[0] = taps[6]; ids[1] = maximum_broadcast;
    u32 shifted = attention_op(&builder,"ElementWiseSubtract",ids,2,3,score_shape,QNN_DATATYPE_FLOAT_16,internal_tap,0,0);
    u32 exponential = attention_op(&builder,"ElementWiseExp",&shifted,1,3,score_shape,QNN_DATATYPE_FLOAT_16,internal_tap,0,0);
    u32 denominator = attention_op(&builder,"ReduceSum",&exponential,1,2,reduced_shape,QNN_DATATYPE_FLOAT_16,internal_tap,&reduction,1);
#ifdef OCR_CAPTURE_INTERNALS
    internal_ids[0] = maximum; internal_ids[1] = shifted;
    internal_ids[2] = exponential; internal_ids[3] = denominator;
#endif
    u32 denominator_broadcast = attention_op(&builder,"Reshape",&denominator,1,3,broadcast_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
    ids[0] = exponential; ids[1] = denominator_broadcast;
#ifdef OCR_REFINE_SILU_ONLY
    taps[7] = attention_divide(&builder,ids,3,score_shape,1,0);
#else
    taps[7] = attention_divide(&builder,ids,3,score_shape,1,block_index);
#endif
    ids[0] = taps[7]; ids[1] = branches[2];
    u32 attended = attention_op(&builder,"MatMul",ids,2,3,batched,QNN_DATATYPE_FLOAT_16,0,0,0);
    u32 context_layout = attention_transpose(&builder,attended,heads,permutation);
    taps[8] = attention_op(&builder,"Reshape",&context_layout,1,2,flat,QNN_DATATYPE_FLOAT_16,1,0,0);
    ids[0] = taps[8]; ids[1] = proj_weight;
    product = attention_op(&builder,"MatMul",ids,2,2,flat,QNN_DATATYPE_FLOAT_16,0,0,0);
    ids[0] = product; ids[1] = proj_bias;
    taps[9] = attention_op(&builder,"ElementWiseAdd",ids,2,2,flat,QNN_DATATYPE_FLOAT_16,1,0,0);
    ids[0] = input; ids[1] = taps[9];
    taps[10] = attention_op(&builder,"ElementWiseAdd",ids,2,2,flat,QNN_DATATYPE_FLOAT_16,1,0,0);
    if (full_block) {
        u32 second_gamma = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,width,constants); constants += 2048;
        taps[11] = attention_norm(&builder,taps[10],second_gamma,2,flat,&scalar_axis);
        for (u32 branch = 0; branch < 2; ++branch) {
            u32 weight = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,2,expanded_weight,constants); constants += 8388608;
            u32 bias = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,expanded_width,constants); constants += 8192;
            ids[0] = taps[11]; ids[1] = weight;
            u32 projected = attention_op(&builder,"MatMul",ids,2,2,wide,QNN_DATATYPE_FLOAT_16,0,0,0);
            ids[0] = projected; ids[1] = bias;
            taps[12+branch] = attention_op(&builder,"ElementWiseAdd",ids,2,2,wide,QNN_DATATYPE_FLOAT_16,1,0,0);
        }
        u32 zero_tensor = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,scalar_shape,&zero);
        u32 one_tensor = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,scalar_shape,&one);
        u32 absolute = attention_op(&builder,"ElementWiseAbs",&taps[12],1,2,wide,QNN_DATATYPE_FLOAT_16,0,0,0);
        u32 negative_absolute = attention_op(&builder,"ElementWiseNeg",&absolute,1,2,wide,QNN_DATATYPE_FLOAT_16,0,0,0);
        u32 decay = attention_op(&builder,"ElementWiseExp",&negative_absolute,1,2,wide,QNN_DATATYPE_FLOAT_16,internal_tap,0,0);
        ids[0] = one_tensor; ids[1] = decay;
        u32 divisor = attention_op(&builder,"ElementWiseAdd",ids,2,2,wide,QNN_DATATYPE_FLOAT_16,internal_tap,0,0);
        ids[0] = taps[12]; ids[1] = zero_tensor;
        u32 negative_part = attention_op(&builder,"ElementWiseMinimum",ids,2,2,wide,QNN_DATATYPE_FLOAT_16,0,0,0);
        u32 numerator_scale = attention_op(&builder,"ElementWiseExp",&negative_part,1,2,wide,QNN_DATATYPE_FLOAT_16,internal_tap,0,0);
        ids[0] = numerator_scale; ids[1] = divisor;
        u32 sigmoid = attention_divide(&builder,ids,2,wide,internal_tap,block_index);
    #ifdef OCR_CAPTURE_INTERNALS
        internal_ids[4] = decay; internal_ids[5] = divisor;
        internal_ids[6] = numerator_scale; internal_ids[7] = sigmoid;
    #endif
        ids[0] = taps[12]; ids[1] = sigmoid;
        taps[14] = attention_op(&builder,"ElementWiseMultiply",ids,2,2,wide,QNN_DATATYPE_FLOAT_16,1,0,0);
        ids[0] = taps[14]; ids[1] = taps[13];
        taps[15] = attention_op(&builder,"ElementWiseMultiply",ids,2,2,wide,QNN_DATATYPE_FLOAT_16,1,0,0);
        u32 down_weight = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,2,reduced_weight,constants); constants += 8388608;
        u32 down_bias = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,width,constants);
        ids[0] = taps[15]; ids[1] = down_weight;
        u32 down_product = attention_op(&builder,"MatMul",ids,2,2,flat,QNN_DATATYPE_FLOAT_16,0,0,0);
        ids[0] = down_product; ids[1] = down_bias;
        taps[16] = attention_op(&builder,"ElementWiseAdd",ids,2,2,flat,QNN_DATATYPE_FLOAT_16,1,0,0);
        ids[0] = taps[10]; ids[1] = taps[16];
        taps[17] = attention_op(&builder,"ElementWiseAdd",ids,2,2,flat,QNN_DATATYPE_FLOAT_16,1,0,0);
    }
#ifdef OCR_MATRIX_RESIDUAL
    if (block_index == 1 && full_block) {
        internal_ids[8] = builder.diagnostic_quotient;
        internal_ids[9] = builder.diagnostic_residual;
        internal_count = 10;
    }
#endif
    if (!builder.good || !checked("attention_finalize",api->graph_finalize(builder.graph,0,0))) return 0;
    QnnTensor outputs[28], source_tensor = builder.tensors[input];
    u32 output_count = tap_count;
    source_tensor.data.v1.memory.client_buffer.data = source;
    source_tensor.data.v1.memory.client_buffer.data_size = tokens*2048;
    u32 offset = 0;
    for (u32 tap = 0; tap < tap_count; ++tap) {
        u32 elements = attention_elements(tap,tokens);
        outputs[tap] = builder.tensors[taps[tap]];
        outputs[tap].data.v1.memory.client_buffer.data = attention_output+offset;
        outputs[tap].data.v1.memory.client_buffer.data_size = elements*2;
        offset += elements;
    }
#ifdef OCR_CAPTURE_INTERNALS
    for (u32 index = 0; index < internal_count; ++index) {
        QnnTensor value = builder.tensors[internal_ids[index]];
        u32 elements = 1;
        for (u32 axis = 0; axis < value.data.v1.rank; ++axis) elements *= value.data.v1.dimensions[axis];
        if (elements > 3674112-internal_elements) return 0;
        value.data.v1.memory.client_buffer.data = internal_output+internal_elements;
        value.data.v1.memory.client_buffer.data_size = elements*2;
        outputs[output_count++] = value;
        internal_elements += elements;
    }
#endif
    static const char *const names[] = {"norm1","qkv","q_norm","k_norm","q_rope","k_rope","scores","probabilities","context","projection","residual",
        "norm2","gate","up","silu","gated","down","block_output"};
    int good = 1;
    for (u32 run = 0; run < 3; ++run) {
        for (u32 index = 0; index < total_elements; ++index) attention_output[index] = 0x7e00;
    #ifdef OCR_CAPTURE_INTERNALS
        for (u32 index = 0; index < internal_elements; ++index) internal_output[index] = 0x7e00;
    #endif
        if (!checked("attention_execute",api->graph_execute(builder.graph,&source_tensor,1,outputs,output_count,execution_profile,0))) return 0;
    #ifdef OCR_CAPTURE_INTERNALS
        for (u32 index = 0; index < internal_elements; ++index) {
            if ((internal_output[index] & 0x7c00) == 0x7c00 || (run && internal_output[index] != internal_previous[index])) {
            text("FAIL nonfinite or unstable internal capture\n"); return 0;
            }
            internal_previous[index] = internal_output[index];
        }
        text("PASS finite and repeatable internal tensors\n");
    #endif
        const u64 *events = 0; u32 event_count = 0; u64 cycles = 0, microseconds = 0;
        if (api->profile_get_events(execution_profile,&events,&event_count) || !profile_events(api,events,event_count,0,&cycles,&microseconds) || (!cycles && !microseconds)) return 0;
        checked("accelerator_cycles",cycles); checked("accelerator_microseconds",microseconds);
        for (u32 row = 0; row < 16*tokens; ++row) {
            float sum = 0;
            for (u32 column = 0; column < tokens; ++column) {
                union {u16 bits; _Float16 value;} probability;
                probability.bits = attention_output[tap_offsets[7]+row*tokens+column];
                if (!(probability.value >= 0 && probability.value <= 1)) { text("FAIL probability range\n"); return 0; }
                sum += (float)probability.value;
            }
            if (!(sum >= 0.997f && sum <= 1.003f)) { text("FAIL probability row sum\n"); return 0; }
        }
        text("PASS probability range and row sums\n");
        for (u32 scope = 0; scope < 2; ++scope) {
            text(scope ? "candidate_fp32\n" : "original_fp32\n"); offset = 0;
            for (u32 tap = 0; tap < tap_count; ++tap) {
                float maximum = 0; u32 failures = 0, elements = attention_elements(tap,tokens);
                for (u32 index = 0; index < elements; ++index) {
                    union {u32 bits; float value;} expected;
                    union {u16 bits; _Float16 value;} actual;
                    expected.bits = load32(references+(scope*total_elements+offset+index)*4);
                    actual.bits = attention_output[offset+index];
                    float error = (float)actual.value-expected.value; if (error < 0) error = -error;
                    float magnitude = expected.value < 0 ? -expected.value : expected.value;
                    if ((actual.bits & 0x7c00) == 0x7c00 || !(error <= 0.003f+0.005f*magnitude)) {
                        ++failures;
                        if (!run && !scope && tap == 6 && failures <= 16) {
                            checked("score_failure_index",index);
                            checked("score_actual_fp16_bits",actual.bits);
                            checked("score_expected_fp32_bits",expected.bits);
                            u32 head = index/(tokens*tokens), row = index/tokens%tokens, column = index%tokens;
                            static const char hex[] = "0123456789abcdef";
                            for (u32 branch = 0; branch < 2; ++branch) {
                                char vector[257];
                                u32 start = tap_offsets[4+branch]+((branch ? column : row)*16+head)*64;
                                for (u32 channel = 0; channel < 64; ++channel) {
                                    u16 bits = attention_output[start+channel];
                                    for (u32 digit = 0; digit < 4; ++digit) vector[channel*4+digit] = hex[(bits >> (12-digit*4)) & 15];
                                }
                                vector[256] = 0;
                                text(branch ? "score_key_fp16_hex: " : "score_query_fp16_hex: "); text(vector); text("\n");
                            }
                        }
                        if (!run && tap == 17 && failures <= 16) {
                            checked("block_failure_index",index);
                            checked("block_actual_fp16_bits",actual.bits);
                            checked("block_expected_fp32_bits",expected.bits);
                            checked("block_residual_fp16_bits",attention_output[tap_offsets[10]+index]);
                            checked("block_down_fp16_bits",attention_output[tap_offsets[16]+index]);
                            checked("block_reference_residual_fp32_bits",load32(references+(scope*total_elements+tap_offsets[10]+index)*4));
                            checked("block_reference_down_fp32_bits",load32(references+(scope*total_elements+tap_offsets[16]+index)*4));
                        }
                        if (!run && !scope && tap == 10 && failures <= 16) {
                            checked("residual_failure_index",index);
                            checked("input_fp16_bits",source[index*2] | (u32)source[index*2+1] << 8);
                            checked("projection_fp16_bits",attention_output[tap_offsets[9]+index]);
                            checked("residual_fp16_bits",actual.bits);
                            checked("original_projection_fp32_bits",load32(references+(tap_offsets[9]+index)*4));
                            checked("original_residual_fp32_bits",expected.bits);
                            checked("candidate_projection_fp32_bits",load32(references+(total_elements+tap_offsets[9]+index)*4));
                            checked("candidate_residual_fp32_bits",load32(references+(total_elements+offset+index)*4));
                            static const char hex[] = "0123456789abcdef";
                            char context_row[4097];
                            for (u32 channel = 0; channel < 1024; ++channel) {
                                u16 bits = attention_output[tap_offsets[8]+(index/1024)*1024+channel];
                                for (u32 digit = 0; digit < 4; ++digit) context_row[channel*4+digit] = hex[(bits >> (12-digit*4)) & 15];
                            }
                            context_row[4096] = 0;
                            text("context_row_fp16_hex: "); text(context_row); text("\n");
                        }
                    }
                    if (error > maximum && error < 1e10f) maximum = error;
                }
                text(names[tap]); text("\n"); checked("max_absolute_error_micro_units_ceil",(u64)(maximum*1000000.0f+0.999f));
                checked("out_of_tolerance",failures); if (failures) good = 0;
                offset += elements;
            }
        }
    }
    if (capture_directory && (!capture_tensor(block_index,".input.f16",source,tokens*2048) ||
        !capture_tensor(block_index,".taps.f16",attention_output,total_elements*2))) {
        text("FAIL diagnostic tensor capture\n"); return 0;
    }
#ifdef OCR_CAPTURE_INTERNALS
    if (capture_directory && !capture_tensor(block_index,".internals.f16",internal_output,internal_elements*2)) {
        text("FAIL internal tensor capture\n"); return 0;
    }
#endif
    text(full_block ? (good ? "PASS learned vision block taps\n" : "FAIL learned vision block taps\n") :
                     (good ? "PASS learned vision attention taps\n" : "FAIL learned vision attention taps\n"));
    if (good && next_input) {
        for (u32 index = 0; index < tokens*1024; ++index) next_input[index] = attention_output[tap_offsets[17]+index];
        text("PASS unchanged FP16 block output handed to next block\n");
    }
    return good;
}

static int primitives(const QnnInterfaceV2 *api, QnnContextHandle context, u32 size, u32 kind) {
    if (kind == 9) {
        if (size < 192 || load32(fixture_data+160) != 2) return 0;
        u32 tokens = load32(fixture_data+168), cursor = 164, records[2];
        if (tokens != 64 && tokens != 128) return 0;
        u32 constants = 33585408+tokens*512, references = (30720*tokens+32*tokens*tokens)*8;
        for (u32 block_index = 0; block_index < 2; ++block_index) {
            u32 input_bytes = block_index ? 0 : tokens*2048;
            if (size-cursor < 28) return 0;
            records[block_index] = cursor+28;
            if (load32(fixture_data+cursor) != 10+block_index || load32(fixture_data+cursor+4) != tokens ||
                load32(fixture_data+cursor+8) != 1024 || load32(fixture_data+cursor+12) != 1024 ||
                load32(fixture_data+cursor+16) != input_bytes || load32(fixture_data+cursor+20) != constants ||
                load32(fixture_data+cursor+24) != references || (u64)input_bytes+constants+references > size-cursor-28) return 0;
            cursor += 28+input_bytes+constants+references;
        }
        if (cursor != size) return 0;
        if (!api) return 1;
        for (u32 block_index = 0; block_index < 2; ++block_index) {
            u8 *source = block_index ? (u8 *)chain_input : fixture_data+records[0];
            u8 *weights = fixture_data+records[block_index]+(block_index ? 0 : tokens*2048);
            if (!attention_probe(api,context,source,weights,weights+constants,1,tokens,block_index,block_index ? 0 : chain_input)) return 0;
        }
        text("PASS sequential vision blocks 0 and 1\n");
        return 1;
    }
    if (kind == 7 || kind == 8) {
        if (size < 192) return 0;
        u32 tokens = load32(fixture_data+168);
        if (tokens != 64 && tokens != 128) return 0;
        u32 input_bytes = tokens*2048, constant_bytes = (kind == 8 ? 33585408 : 8399104)+tokens*512;
        u32 reference_bytes = ((kind == 8 ? 30720 : 11264)*tokens+32*tokens*tokens)*8;
        if (size != 192+input_bytes+constant_bytes+reference_bytes || load32(fixture_data+160) != 1 || load32(fixture_data+164) != (kind == 8 ? 9U : 8U) ||
            load32(fixture_data+172) != 1024 || load32(fixture_data+176) != 1024 ||
            load32(fixture_data+180) != input_bytes || load32(fixture_data+184) != constant_bytes || load32(fixture_data+188) != reference_bytes) return 0;
        return !api || attention_probe(api,context,fixture_data+192,fixture_data+192+input_bytes,fixture_data+192+input_bytes+constant_bytes,kind == 8,tokens,0,0);
    }
    static const char *const names[] = {"patch_projection","vision_qkv","text_query","rmsnorm_64","rmsnorm_128","rmsnorm_1024","rmsnorm_1536",
        "connector_layernorm","mlp_silu","connector_gelu","attention_softmax"};
    static const char *const learned_names[] = {"patch_pattern_original_fp32","patch_pattern_candidate_fp32","patch_noise_original_fp32",
        "patch_noise_candidate_fp32","patch_text_original_fp32","patch_text_candidate_fp32"};
    u32 cursor = kind == 6 ? 164 : 132, count = load32(fixture_data + cursor - 4);
    if (count != (kind == 6 ? sizeof(learned_names) / sizeof(learned_names[0]) : sizeof(names) / sizeof(names[0]))) return 0;
    for (u32 index = 0; index < count; ++index) {
        if (size - cursor < 28) return 0;
        u32 operation = load32(fixture_data+cursor), rows = load32(fixture_data+cursor+4), width = load32(fixture_data+cursor+8);
        u32 output_width = load32(fixture_data+cursor+12), input_bytes = load32(fixture_data+cursor+16);
        u32 weight_bytes = load32(fixture_data+cursor+20), reference_bytes = load32(fixture_data+cursor+24);
        cursor += 28;
        if (!operation || operation > (kind == 6 ? 7U : 6U) || !rows || rows > 16 || !width || width > 4608 || !output_width || output_width > 4608 ||
            rows * output_width > 16384 || rows * width * 2 != input_bytes || rows * output_width * 4 != reference_bytes ||
            (operation != 1 && operation != 7 && width != output_width) ||
            weight_bytes != (operation == 7 ? (width+1) * output_width * 2 : operation == 1 ? width * output_width * 2 : operation == 2 ? width * 2 : operation == 3 ? width * 4 : 0) ||
            (u64)input_bytes + weight_bytes + reference_bytes > size - cursor) return 0;
        if (kind == 6 && (operation != 7 || rows != 16 || width != 1176 || output_width != 1024)) return 0;
        if (api && !primitive(api,context,operation,rows,width,output_width,fixture_data+cursor,fixture_data+cursor+input_bytes,
                       fixture_data+cursor+input_bytes+weight_bytes,kind == 6 ? learned_names[index] : names[index])) return 0;
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

int ocr_htp_test(const unsigned short *library, const unsigned short *fixtures, const unsigned short *capture) {
    capture_directory = capture;
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
    u32 kind = fixture_size >= 128 ? load32(fixture_data+12) : 0;
    if ((kind != 5 && kind != 6 && kind != 7 && kind != 8 && kind != 9) || !ocr_artifact(fixture_data,fixture_size,kind)) { text("FAIL HTP fixture verification\n"); return 0; }
    if (kind >= 6 && kind <= 9) {
        static const char source_sha[] = "a16eb0de98d199293371c560f95f83130d2a2c9612449df16839f08ff9498815";
        static const char hex[] = "0123456789abcdef";
        if (fixture_size < 164) { text("FAIL learned weight identity\n"); return 0; }
        for (u32 index = 0; index < 32; ++index)
            if (hex[fixture_data[128+index] >> 4] != source_sha[index*2] || hex[fixture_data[128+index] & 15] != source_sha[index*2+1]) {
                text("FAIL learned weight identity\n"); return 0;
            }
    }
    if (!primitives(0,0,fixture_size,kind)) { text("FAIL HTP fixture structure\n"); return 0; }
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
    good = addition(api,context,1024,"ocr_vision_add") && addition(api,context,1536,"ocr_text_add") && primitives(api,context,fixture_size,kind);
done:
    if (context && !checked("context_free",api->context_free(context,0))) clean = 0;
    if (execution_profile && !checked("profile_free",api->profile_free(execution_profile))) clean = 0;
    if (device && !checked("device_free",api->device_free(device))) clean = 0;
    if (backend && !checked("backend_free",api->backend_free(backend))) clean = 0;
    if (module && !FreeLibrary(module)) clean = 0;
    text(good && clean ? "PASS OCR HTP probe\n" : "FAIL OCR HTP probe\n");
    return good && clean && output_good;
}