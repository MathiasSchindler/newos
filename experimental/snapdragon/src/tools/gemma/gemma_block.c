#include "gemma_block.h"
#include "gemma_numeric.h"

static int same_name(const char *left, const char *right) {
    while (*left && *left == *right) { ++left; ++right; }
    return *left == *right;
}

static int qualified_name(GemmaBlock *block, GemmaBlockTensor *entry, const char *name) {
    u32 used = 0, index = 0;
    while (block->prefix[index]) {
        if (used == sizeof(entry->name) - 1) return 0;
        entry->name[used++] = block->prefix[index++];
    }
    while (*name) {
        if (used == sizeof(entry->name) - 1) return 0;
        entry->name[used++] = *name++;
    }
    entry->name[used] = 0;
    return 1;
}

u32 gemma_block_tensor(GemmaBlock *block, const char *name, u32 type, u32 dtype,
                       const u32 *dimensions, u32 rank, void *buffer) {
    u32 index, id = block->count;
    u64 bytes = dtype == QNN_DATATYPE_FLOAT_16 ? 2U : 4U;
    GemmaBlockTensor *entry;
    if (block->error) return 0;
    if (id >= GEMMA_BLOCK_TENSORS || !rank || rank > 4U) { block->error = 1; return 0; }
    entry = &block->tensors[id];
    if (!qualified_name(block, entry, name)) { block->error = 2; return 0; }
    if (block->internal && type == QNN_TENSOR_TYPE_APP_READ &&
        !same_name(name, "k-rope") && !same_name(name, "v-projection") && !same_name(name, "logits"))
        type = QNN_TENSOR_TYPE_NATIVE;
    for (index = 0; index < rank; ++index) {
        if (!dimensions[index]) { block->error = 3; return 0; }
        entry->dimensions[index] = dimensions[index]; bytes *= dimensions[index];
        if (bytes > 0xffffffffU) { block->error = 4; return 0; }
    }
    entry->bytes = (u32)bytes;
    entry->buffer = buffer;
    entry->tensor.version = QNN_TENSOR_VERSION_1;
    entry->tensor.data.v1.name = entry->name;
    entry->tensor.data.v1.type = type;
    entry->tensor.data.v1.data_type = dtype;
    entry->tensor.data.v1.quantize_params.encoding_definition = QNN_DEFINITION_UNDEFINED;
    entry->tensor.data.v1.quantize_params.quantization_encoding = QNN_QUANTIZATION_ENCODING_UNDEFINED;
    entry->tensor.data.v1.rank = rank;
    entry->tensor.data.v1.dimensions = entry->dimensions;
    if (type == QNN_TENSOR_TYPE_STATIC) {
        entry->tensor.data.v1.memory.client_buffer.data = buffer;
        entry->tensor.data.v1.memory.client_buffer.data_size = entry->bytes;
    }
    ++block->count;
    block->error = block->api->tensor_create_graph_tensor(block->graph, &entry->tensor);
    if (block->error) block->host.status(name, block->error);
    return id;
}

u32 gemma_block_node(GemmaBlock *block, const char *name, const char *type,
                     const u32 *ids, u32 count, u32 output, QnnParam *params, u32 param_count) {
    QnnTensor inputs[4];
    QnnOpConfig operation = {0};
    u32 index;
    if (block->error) return output;
    if (count > 4 || output >= block->count) { block->error = 5; return output; }
    for (index = 0; index < count; ++index) {
        if (ids[index] >= block->count) { block->error = 6; return output; }
        inputs[index] = block->tensors[ids[index]].tensor;
    }
    operation.version = QNN_OPCONFIG_VERSION_1;
    operation.data.v1.name = block->tensors[output].tensor.data.v1.name;
    operation.data.v1.package_name = "qti.aisw";
    operation.data.v1.type_name = type;
    operation.data.v1.input_count = count;
    operation.data.v1.inputs = inputs;
    operation.data.v1.output_count = 1;
    operation.data.v1.outputs = &block->tensors[output].tensor;
    operation.data.v1.parameter_count = param_count;
    operation.data.v1.parameters = params;
    block->error = block->api->graph_add_node(block->graph, operation);
    if (block->error) block->host.status(name, block->error);
    return output;
}

u32 gemma_block_projection(GemmaBlock *block, u32 input, const char *suffix,
                           const char *name, u32 width, u32 rows) {
    GemmaArtifactHeader header;
    const u8 *source;
    i8 *transposed;
    float *scales;
    u32 column, row, weight_id, output_id;
    u32 dimensions[2] = {width, rows};
    u32 output_dimensions[2] = {block->tokens, rows};
    QnnTensor weight = {0};
    GemmaBlockTensor *entry;
    if (block->error) return 0;
    source = block->host.weight(block->host.user, suffix, &header);
    if (!source || header.rank != 2 || header.dimensions[0] != rows || header.dimensions[1] != width ||
        header.scale_count != rows || header.quantization_axis != 1 || header.group_size != width ||
        header.element_type != (block->bits == 4 ? GEMMA_ARTIFACT_ELEMENT_S4 : GEMMA_ARTIFACT_ELEMENT_S8)) {
        block->error = 7; block->host.status(suffix, block->error); return 0;
    }
    transposed = block->host.allocate(block->host.user, (u64)rows * width);
    scales = block->host.allocate(block->host.user, (u64)rows * sizeof(float));
    if (!transposed || !scales || block->count >= GEMMA_BLOCK_TENSORS) { block->error = 8; return 0; }
    for (row = 0; row < rows; ++row) {
        const u16 *half_scales = (const u16 *)(source + header.scale_offset);
        scales[row] = gemma_numeric_f16(half_scales[row]);
        if (!(scales[row] > 0.0f && scales[row] <= 65504.0f)) { block->error = 9; return 0; }
        for (column = 0; column < width; ++column) {
            u64 offset = (u64)row * width + column;
            i32 value;
            if (block->bits == 4) {
                value = (source[offset / 2] >> ((offset & 1) * 4)) & 15;
                if (value >= 8) value -= 16;
            } else value = ((const i8 *)source)[offset];
            transposed[(u64)column * rows + row] = (i8)value;
        }
    }
    weight_id = block->count++;
    entry = &block->tensors[weight_id];
    if (!qualified_name(block, entry, suffix)) { block->error = 2; return 0; }
    entry->dimensions[0] = dimensions[0]; entry->dimensions[1] = dimensions[1];
    weight.version = QNN_TENSOR_VERSION_1;
    weight.data.v1.name = entry->name;
    weight.data.v1.type = QNN_TENSOR_TYPE_STATIC;
    weight.data.v1.data_type = QNN_DATATYPE_SFIXED_POINT_8;
    weight.data.v1.rank = 2;
    weight.data.v1.dimensions = entry->dimensions;
    weight.data.v1.quantize_params.encoding_definition = QNN_DEFINITION_DEFINED;
    weight.data.v1.quantize_params.quantization_encoding = QNN_QUANTIZATION_ENCODING_BW_AXIS_SCALE_OFFSET;
    weight.data.v1.quantize_params.encoding.bw_axis_scale_offset.bitwidth = block->bits;
    weight.data.v1.quantize_params.encoding.bw_axis_scale_offset.axis = 1;
    weight.data.v1.quantize_params.encoding.bw_axis_scale_offset.element_count = rows;
    weight.data.v1.quantize_params.encoding.bw_axis_scale_offset.scales = scales;
    if (block->bits == 8) {
        QnnScaleOffset *pairs = block->host.allocate(block->host.user, rows * sizeof(QnnScaleOffset));
        if (!pairs) { block->error = 8; return 0; }
        for (row = 0; row < rows; ++row) { pairs[row].scale = scales[row]; pairs[row].offset = 0; }
        weight.data.v1.quantize_params.quantization_encoding = QNN_QUANTIZATION_ENCODING_AXIS_SCALE_OFFSET;
        weight.data.v1.quantize_params.encoding.axis_scale_offset.axis = 1;
        weight.data.v1.quantize_params.encoding.axis_scale_offset.scale_offset_count = rows;
        weight.data.v1.quantize_params.encoding.axis_scale_offset.scale_offsets = pairs;
    }
    weight.data.v1.memory.client_buffer.data = transposed;
    weight.data.v1.memory.client_buffer.data_size = rows * width;
    entry->tensor = weight;
    block->error = block->api->tensor_create_graph_tensor(block->graph, &entry->tensor);
    if (block->error) block->host.status(suffix, block->error);
    output_id = gemma_block_tensor(block, name, QNN_TENSOR_TYPE_APP_READ, QNN_DATATYPE_FLOAT_16, output_dimensions, 2, 0);
    { u32 inputs[2] = {input, weight_id};
      return gemma_block_node(block, name, "MatMul", inputs, 2, output_id, 0, 0); }
}

static u32 tensor(GemmaBlock *block, const char *name, u32 type, const u32 *shape, u32 rank) {
    return gemma_block_tensor(block, name, type, QNN_DATATYPE_FLOAT_16, shape, rank, 0);
}

static u32 unary(GemmaBlock *block, const char *name, const char *operation, u32 input,
                 const u32 *shape, u32 rank) {
    u32 output = tensor(block, name, QNN_TENSOR_TYPE_APP_READ, shape, rank);
    return gemma_block_node(block, name, operation, &input, 1, output, 0, 0);
}

static u32 binary(GemmaBlock *block, const char *name, const char *operation, u32 left, u32 right,
                  const u32 *shape, u32 rank) {
    u32 inputs[2] = {left, right};
    u32 output = tensor(block, name, QNN_TENSOR_TYPE_APP_READ, shape, rank);
    return gemma_block_node(block, name, operation, inputs, 2, output, 0, 0);
}

static QnnParam scalar(const char *name, u32 dtype, u32 value) {
    QnnParam parameter = {0};
    parameter.type = QNN_PARAMTYPE_SCALAR; parameter.name = name;
    parameter.value.scalar.data_type = dtype;
    parameter.value.scalar.value.uint32_value = value;
    return parameter;
}

static QnnParam parameter_tensor(GemmaBlock *block, const char *name, const char *parameter,
                                 const u32 *values, u32 count) {
    QnnParam result = {0};
    u32 shape[1] = {count}, index;
    u32 *storage = block->host.allocate(block->host.user, count * sizeof(u32));
    if (!storage) { block->error = 10; return result; }
    for (index = 0; index < count; ++index) storage[index] = values[index];
    index = gemma_block_tensor(block, name, QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_UINT_32, shape, 1, storage);
    result.type = QNN_PARAMTYPE_TENSOR; result.name = parameter;
    result.value.tensor = block->tensors[index].tensor;
    return result;
}

static u32 transpose(GemmaBlock *block, const char *name, const char *perm_name, u32 input,
                     const u32 *shape, const u32 *permutation, u32 rank) {
    QnnParam perm = parameter_tensor(block, perm_name, "perm", permutation, rank);
    u32 output = tensor(block, name, QNN_TENSOR_TYPE_APP_READ, shape, rank);
    return gemma_block_node(block, name, "Transpose", &input, 1, output, &perm, 1);
}

static u32 norm(GemmaBlock *block, const char *name, const char *suffix, const char *axes_name,
                u32 input, const u32 *shape, u32 rank, float epsilon, float divisor) {
    GemmaArtifactHeader header;
    const u16 *weights;
    u16 *gain;
    u32 dimensions[1] = {shape[rank - 1]}, axis[1] = {rank - 1}, index, inputs[2], output;
    QnnParam parameters[2] = {0};
    if (block->error) return 0;
    weights = block->host.weight(block->host.user, suffix, &header);
    if (!weights || header.rank != 1 || header.dimensions[0] != dimensions[0] ||
        header.element_type != GEMMA_ARTIFACT_ELEMENT_F16) { block->error = 11; return 0; }
    gain = block->host.allocate(block->host.user, dimensions[0] * sizeof(u16));
    if (!gain) { block->error = 12; return 0; }
    for (index = 0; index < dimensions[0]; ++index) {
        union { _Float16 value; u16 bits; } converted;
        converted.value = (_Float16)((1.0f + gemma_numeric_f16(weights[index])) / divisor);
        gain[index] = converted.bits;
    }
    inputs[0] = input;
    inputs[1] = gemma_block_tensor(block, suffix, QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_FLOAT_16, dimensions, 1, gain);
    parameters[0].type = QNN_PARAMTYPE_SCALAR; parameters[0].name = "epsilon";
    parameters[0].value.scalar.data_type = QNN_DATATYPE_FLOAT_32;
    parameters[0].value.scalar.value.float_value = epsilon;
    parameters[1] = parameter_tensor(block, axes_name, "axes", axis, 1);
    output = tensor(block, name, QNN_TENSOR_TYPE_APP_READ, shape, rank);
    return gemma_block_node(block, name, "RmsNorm", inputs, 2, output, parameters, 2);
}

static u32 rotary(GemmaBlock *block, const char *name, u32 input, u32 heads, u32 cosine, u32 sine, u32 position) {
    u32 shape[4] = {1, heads, block->tokens, 256};
    u32 inputs[4] = {input, cosine, sine, position};
    QnnParam parameters[2];
    u32 output = tensor(block, name, QNN_TENSOR_TYPE_APP_READ, shape, 4);
    parameters[0] = scalar("interleaved", QNN_DATATYPE_BOOL_8, 0);
    parameters[1] = scalar("rotary_embedding_dim", QNN_DATATYPE_UINT_32, 256);
    return gemma_block_node(block, name, "RotaryEmbedding", inputs, 4, output, parameters, 2);
}

static u32 concat(GemmaBlock *block, const char *name, u32 past, u32 current) {
    u32 shape[3] = {4, block->cache + block->tokens, 256};
    u32 inputs[2] = {past, current};
    QnnParam axis = scalar("axis", QNN_DATATYPE_UINT_32, 1);
    u32 output = tensor(block, name, QNN_TENSOR_TYPE_NATIVE, shape, 3);
    return gemma_block_node(block, name, "Concat", inputs, 2, output, &axis, 1);
}

static u32 expand_heads(GemmaBlock *block, const char *name, u32 input, u32 indices) {
    u32 shape[3] = {8, block->cache + block->tokens, 256};
    u32 inputs[2] = {input, indices};
    QnnParam axis = scalar("axis", QNN_DATATYPE_INT_32, 0);
    u32 output = tensor(block, name, QNN_TENSOR_TYPE_NATIVE, shape, 3);
    return gemma_block_node(block, name, "Gather", inputs, 2, output, &axis, 1);
}

int gemma_block_build(GemmaBlock *block, const QnnInterfaceV2 *api, QnnContextHandle context,
                      GemmaBlockHost host, u32 bits) {
    return gemma_block_build_shape(block, api, context, host, bits, GEMMA_BLOCK_TOKENS, GEMMA_BLOCK_CACHE);
}

int gemma_block_build_shape(GemmaBlock *block, const QnnInterfaceV2 *api, QnnContextHandle context,
                            GemmaBlockHost host, u32 bits, u32 tokens, u32 cache) {
    const u32 hidden_shape[2] = {tokens, 2560}, qflat_shape[3] = {tokens, 8, 256};
    const u32 kvflat_shape[3] = {tokens, 4, 256}, qshape[3] = {8, tokens, 256}, kvshape[3] = {4, tokens, 256};
    const u32 q4shape[4] = {1, 8, tokens, 256}, k4shape[4] = {1, 4, tokens, 256};
    const u32 perm[3] = {1, 0, 2}, key_perm[3] = {0, 2, 1};
    const u32 cached_shape[3] = {4, cache, 256}, key_shape[3] = {8, 256, cache + tokens};
    const u32 scores_shape[3] = {8, tokens, cache + tokens}, attention_shape[2] = {tokens, 2048};
    const u32 rope_shape[2] = {cache, 128}, position_shape[2] = {1, tokens}, mlp_shape[2] = {tokens, 10240};
    const u32 indices_shape[1] = {8}, scale_shape[1] = {1};
    static u32 head_indices[8] = {0, 0, 1, 1, 2, 2, 3, 3};
    static u16 attention_scale = 0x2c00U;
    u32 hidden, position, cosine, sine, mask, past_key, past_value, indices, scale;
    u32 normalized, query, key, value, scores, attention, projected, residual, gate, up, down;
    block->api = api; block->host = host; block->bits = bits;
    block->tokens = tokens; block->cache = cache;
    if ((bits != 4 && bits != 8) || !tokens || tokens > 128 ||
        (cache != 512 && cache != 1024 && cache != 2048)) return 0;
    if (!block->graph) block->error = api->graph_create(context, "gemma_decoder_block", 0, &block->graph);
    if (block->error) { host.status("graphCreate", block->error); return 0; }
    if (block->hidden_input) {
        GemmaBlockTensor *entry = &block->tensors[block->count++];
        entry->tensor = *block->hidden_input;
        entry->dimensions[0] = tokens; entry->dimensions[1] = 2560;
        entry->tensor.data.v1.dimensions = entry->dimensions;
        hidden = 0;
    } else hidden = tensor(block, "input", QNN_TENSOR_TYPE_APP_WRITE, hidden_shape, 2);
    position = gemma_block_tensor(block, "positions", QNN_TENSOR_TYPE_APP_WRITE, QNN_DATATYPE_INT_32, position_shape, 2, 0);
    cosine = tensor(block, "cos-table", QNN_TENSOR_TYPE_APP_WRITE, rope_shape, 2);
    sine = tensor(block, "sin-table", QNN_TENSOR_TYPE_APP_WRITE, rope_shape, 2);
    mask = tensor(block, "mask", QNN_TENSOR_TYPE_APP_WRITE, scores_shape, 3);
    past_key = tensor(block, "past-key", QNN_TENSOR_TYPE_APP_WRITE, cached_shape, 3);
    past_value = tensor(block, "past-value", QNN_TENSOR_TYPE_APP_WRITE, cached_shape, 3);
    indices = gemma_block_tensor(block, "head-indices", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_INT_32, indices_shape, 1, head_indices);
    scale = gemma_block_tensor(block, "score-scale", QNN_TENSOR_TYPE_STATIC, QNN_DATATYPE_FLOAT_16, scale_shape, 1, &attention_scale);
    normalized = norm(block, "input-rmsnorm", "input_layernorm.weight", "input-axes", hidden, hidden_shape, 2, 9.765625e-10f, 1);
    query = gemma_block_projection(block, normalized, "self_attn.q_proj.weight", "query-flat", 2560, 2048);
    query = unary(block, "query-shaped", "Reshape", query, qflat_shape, 3);
    query = transpose(block, "q-projection", "query-perm", query, qshape, perm, 3);
    key = gemma_block_projection(block, normalized, "self_attn.k_proj.weight", "key-flat", 2560, 1024);
    key = unary(block, "key-shaped", "Reshape", key, kvflat_shape, 3);
    key = transpose(block, "k-projection", "key-perm", key, kvshape, perm, 3);
    value = gemma_block_projection(block, normalized, "self_attn.v_proj.weight", "value-flat", 2560, 1024);
    value = unary(block, "value-shaped", "Reshape", value, kvflat_shape, 3);
    value = transpose(block, "v-projection", "value-perm", value, kvshape, perm, 3);
    query = norm(block, "q-rmsnorm", "self_attn.q_norm.weight", "q-axes", query, qshape, 3, 1e-6f, 1);
    key = norm(block, "k-rmsnorm", "self_attn.k_norm.weight", "k-axes", key, kvshape, 3, 1e-6f, 1);
    query = unary(block, "query-4d", "Reshape", query, q4shape, 4);
    key = unary(block, "key-4d", "Reshape", key, k4shape, 4);
    query = rotary(block, "q-rope", query, 8, cosine, sine, position);
    key = rotary(block, "k-rope", key, 4, cosine, sine, position);
    query = unary(block, "query-3d", "Reshape", query, qshape, 3);
    key = unary(block, "key-3d", "Reshape", key, kvshape, 3);
    key = concat(block, "joined-key", past_key, key);
    value = concat(block, "joined-value", past_value, value);
    key = expand_heads(block, "expanded-key", key, indices);
    value = expand_heads(block, "expanded-value", value, indices);
    key = transpose(block, "transposed-key", "attention-key-perm", key, key_shape, key_perm, 3);
    scores = binary(block, "qk-dot", "MatMul", query, key, scores_shape, 3);
    scores = binary(block, "scaled-scores", "ElementWiseMultiply", scores, scale, scores_shape, 3);
    scores = binary(block, "attention-scores", "ElementWiseAdd", scores, mask, scores_shape, 3);
    scores = unary(block, "attention-softmax", "Softmax", scores, scores_shape, 3);
    attention = binary(block, "attention-heads", "MatMul", scores, value, qshape, 3);
    attention = transpose(block, "attention-transposed", "attention-perm", attention, qflat_shape, perm, 3);
    attention = unary(block, "gqa", "Reshape", attention, attention_shape, 2);
    projected = gemma_block_projection(block, attention, "self_attn.o_proj.weight", "attention-output", 2048, 2560);
    projected = norm(block, "post-attention-norm", "post_attention_layernorm.weight", "post-attention-axes", projected, hidden_shape, 2, 1e-6f, 32);
    residual = binary(block, "attention-residual", "ElementWiseAdd", hidden, projected, hidden_shape, 2);
    normalized = norm(block, "pre-mlp-rmsnorm", "pre_feedforward_layernorm.weight", "pre-mlp-axes", residual, hidden_shape, 2, 9.765625e-10f, 1);
    gate = gemma_block_projection(block, normalized, "mlp.gate_proj.weight", "gate-projection", 2560, 10240);
    up = gemma_block_projection(block, normalized, "mlp.up_proj.weight", "up-projection", 2560, 10240);
    gate = unary(block, "gelu", "Gelu", gate, mlp_shape, 2);
    gate = binary(block, "gated-gelu", "ElementWiseMultiply", gate, up, mlp_shape, 2);
    down = gemma_block_projection(block, gate, "mlp.down_proj.weight", "mlp", 10240, 2560);
    down = norm(block, "post-mlp-norm", "post_feedforward_layernorm.weight", "post-mlp-axes", down, hidden_shape, 2, 1e-6f, 32);
    (void)binary(block, "output", "ElementWiseAdd", residual, down, hidden_shape, 2);
    return block->error == 0;
}

int gemma_block_logits(GemmaBlock *block) {
    u32 input = block->count - 1, last, selected, logits;
    const u32 index_shape[1] = {1}, hidden_shape[2] = {1, 2560};
    u32 inputs[2]; QnnParam axis = scalar("axis", QNN_DATATYPE_INT_32, 0);
    last = gemma_block_tensor(block, "last-token", QNN_TENSOR_TYPE_APP_WRITE,
                             QNN_DATATYPE_INT_32, index_shape, 1, 0);
    selected = tensor(block, "last-hidden", QNN_TENSOR_TYPE_NATIVE, hidden_shape, 2);
    inputs[0] = input; inputs[1] = last;
    gemma_block_node(block, "last-hidden", "Gather", inputs, 2, selected, &axis, 1);
    selected = norm(block, "final-rmsnorm", "norm.weight", "final-axes", selected,
                      hidden_shape, 2, 9.765625e-10f, 1);
    block->tokens = 1;
    logits = gemma_block_projection(block, selected, "embed_tokens.weight", "logits", 2560, 262208);
    (void)logits;
    return block->error == 0;
}