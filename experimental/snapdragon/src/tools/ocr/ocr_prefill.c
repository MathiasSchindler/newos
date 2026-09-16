#include "ocr_text.h"
#include "crypto/sha256.h"

static u8 prefill_embeddings[160+OCR_TEXT_VOCAB*OCR_TEXT_WIDTH*2];
static u8 prefill_shared[68768];
static u8 prefill_hashes[18][32];
static OcrTextInput prefill_input;
static u16 prefill_output[688128];
static float prefill_cosine[64*128], prefill_sine[64*128];
static u32 prefill_grid_width;
static u16 text_keys[16][64*1024], text_values[16][64*1024];
static u32 text_cache_count[16];

static int prefill_path(const unsigned short *directory, const char *name, unsigned short *path) {
    u32 length = 0;
    while (directory[length]) {
        if (length >= 32700) return 0;
        path[length] = directory[length]; ++length;
    }
    path[length++] = '\\';
    for (u32 index = 0; name[index]; ++index) path[length++] = (u8)name[index];
    path[length] = 0;
    return 1;
}

static int prefill_asset(const unsigned short *directory, u32 index, int remember) {
    unsigned short path[32768]; char layer[] = "text-00.got";
    layer[5] = '0'+index/10; layer[6] = '0'+index%10;
    const char *name = index < 16 ? layer : index == 16 ? "embeddings.got" : "text-shared.got";
    u8 *data = index == 16 ? prefill_embeddings : fixture_data;
    u32 expected = index < 16 ? 61354148 : index == 16 ? sizeof(prefill_embeddings) : sizeof(prefill_shared);
    u32 kind = index < 16 ? 14 : index == 16 ? 13 : 15;
    if (index >= 18 || !prefill_path(directory,name,path) || read_blob(path,data,expected) != expected ||
        !ocr_artifact(data,expected,kind) || (index < 16 && load32(data+160) != index)) return 0;
    static const char source_sha[] = "a16eb0de98d199293371c560f95f83130d2a2c9612449df16839f08ff9498815";
    static const char hex[] = "0123456789abcdef";
    for (u32 offset = 0; offset < 32; ++offset) {
        u8 value = data[128+offset];
        if (hex[value >> 4] != source_sha[offset*2] || hex[value & 15] != source_sha[offset*2+1]) return 0;
        if (remember) prefill_hashes[index][offset] = data[96+offset];
        else if (prefill_hashes[index][offset] != data[96+offset]) return 0;
    }
    u32 end = index == 17 ? 160+3072 : expected;
    for (u32 offset = index < 16 ? 164 : 160; offset < end; offset += 2)
        if ((data[offset+1] & 0x7c) == 0x7c) return 0;
    if (index == 17) {
        for (u32 offset = end; offset < expected; offset += 4) {
            union {u32 bits; float value;} coefficient; coefficient.bits = load32(data+offset);
            if (!(coefficient.value >= -1 && coefficient.value <= 1)) return 0;
        }
        for (u32 offset = 0; offset < expected; ++offset) prefill_shared[offset] = data[offset];
    }
    return 1;
}

static int prefill_check(const unsigned short *directory) {
    for (u32 index = 0; index < 18; ++index) if (!prefill_asset(directory,index,1)) return 0;
    text("PASS all 18 text weight artifacts\n"); return 1;
}

int ocr_prefill_input_test(const unsigned short *directory) {
    unsigned short path[32768];
    if (!prefill_check(directory) || !prefill_path(directory,"input-fixtures.got",path)) return 0;
    u32 size = read_blob(path,vision_constants,sizeof(vision_constants)), stride = 205916;
    if (size != 132+12*stride || !ocr_artifact(vision_constants,size,16) || load32(vision_constants+128) != 12) return 0;
    for (u32 index = 0; index < 12; ++index) {
        const u8 *record = vision_constants+132+index*stride;
        u32 task = load32(record), no_think = load32(record+4), height = load32(record+8), width = load32(record+12);
        if (height != 8 || (width != 8 && width != 16) || !prefill_path(directory,width == 8 ? "pattern.features.f16" : "receipt.features.f16",path) ||
            read_blob(path,(u8 *)prefill_output,sizeof(prefill_output)) != width*2*1536*2 ||
            !ocr_text_prepare(prefill_output,width*2,(const u16 *)(prefill_embeddings+160),(u64)OCR_TEXT_VOCAB*OCR_TEXT_WIDTH,
                              height,width,task,no_think,&prefill_input)) return 0;
        if (prefill_input.count != load32(record+16) || prefill_input.image_tokens != load32(record+20) || (u32)prefill_input.delta != load32(record+24)) return 0;
        const void *parts[] = {prefill_input.ids,prefill_input.positions,prefill_input.modalities,prefill_input.mask,prefill_input.embeddings};
        u32 lengths[] = {256,768,64,8192,196608}, cursor = 28;
        for (u32 part = 0; part < 5; ++part) {
            const u8 *actual = parts[part];
            for (u32 offset = 0; offset < lengths[part]; ++offset) if (actual[offset] != record[cursor+offset]) return 0;
            cursor += lengths[part];
        }
    }
    text("PASS 12 exact multimodal prompt/embedding/position/mask oracles\n"); return 1;
}

static u32 prefill_norm(OcrAttentionGraph *builder, u32 input, u8 **constants, u32 *axis, u32 rows) {
    u32 width[1] = {1536}, shape[2] = {rows,1536};
    u32 gamma = attention_tensor(builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,width,*constants); *constants += 3072;
    return attention_norm(builder,input,gamma,2,shape,axis);
}

static u32 prefill_rope(OcrAttentionGraph *builder, u32 input, u32 heads, u32 cosine, u32 sine, u32 rotation, u32 signs, int tap, u32 rows) {
    u32 shape[3] = {rows,heads,128};
    u32 layout = attention_op(builder,"Reshape",&input,1,3,shape,QNN_DATATYPE_FLOAT_16,0,0,0);
    u32 promoted = attention_op(builder,"Cast",&layout,1,3,shape,QNN_DATATYPE_FLOAT_32,0,0,0);
    u32 ids[2] = {promoted,rotation}; QnnParam axis = attention_axis("axis",2);
    u32 swapped = attention_op(builder,"Gather",ids,2,3,shape,QNN_DATATYPE_FLOAT_32,0,&axis,1);
    ids[0] = swapped; ids[1] = signs;
    u32 rotated = attention_op(builder,"ElementWiseMultiply",ids,2,3,shape,QNN_DATATYPE_FLOAT_32,0,0,0);
    ids[0] = promoted; ids[1] = cosine;
    u32 real = attention_op(builder,"ElementWiseMultiply",ids,2,3,shape,QNN_DATATYPE_FLOAT_32,0,0,0);
    ids[0] = rotated; ids[1] = sine;
    u32 imaginary = attention_op(builder,"ElementWiseMultiply",ids,2,3,shape,QNN_DATATYPE_FLOAT_32,0,0,0);
    ids[0] = real; ids[1] = imaginary;
    u32 sum = attention_op(builder,"ElementWiseAdd",ids,2,3,shape,QNN_DATATYPE_FLOAT_32,0,0,0);
    return attention_op(builder,"Cast",&sum,1,3,shape,QNN_DATATYPE_FLOAT_16,tap,0,0);
}

static int prefill_layer(const QnnInterfaceV2 *api, QnnContextHandle context, u32 index, u32 past) {
    if (index >= 16 || past >= 64 || (past && text_cache_count[index] != past)) return 0;
    u32 rows = past ? 1 : 64, length = past ? past+1 : 64;
    if (past) {
        CryptoSha256Context hash; u8 digests[64]; char suffix[] = ".decode-00-cache.u8";
        suffix[8] = '0'+past/10; suffix[9] = '0'+past%10;
        crypto_sha256_init(&hash); crypto_sha256_update(&hash,(const u8 *)text_keys[index],past*2048); crypto_sha256_final(&hash,digests);
        crypto_sha256_init(&hash); crypto_sha256_update(&hash,(const u8 *)text_values[index],past*2048); crypto_sha256_final(&hash,digests+32);
        if (!capture_tensor(index,suffix,digests,sizeof(digests))) return 0;
    }
    OcrAttentionGraph builder = {0}; builder.api = api; builder.good = 1;
    if (!checked("text_graph_create",api->graph_create(context,"text_prefill_layer",0,&builder.graph))) return 0;
    u32 flat[2] = {rows,1536}, head_shape[3] = {rows,16,128}, batch[3] = {16,rows,128}, keys[3] = {16,128,length};
    u32 all_heads[3] = {length,16,128}, all_batch[3] = {16,length,128};
    u32 scores_shape[3] = {16,rows,length}, frequency_shape[3] = {rows,1,128}, one_shape[1] = {1}, head_width[1] = {128}, group_width[1] = {16};
    u32 scalar_axis = 1, reduce_axis = 2, permutation[3] = {1,0,2}, key_permutation[3] = {0,2,1};
    u32 rotations[128], groups[16], selections[2][4608]; float signs[128];
    for (u32 channel = 0; channel < 128; ++channel) { rotations[channel] = channel^1; signs[channel] = channel%2 ? 1 : -1; }
    for (u32 head = 0; head < 16; ++head) groups[head] = head/2;
    for (u32 part = 0; part < 2; ++part) for (u32 channel = 0; channel < 4608; ++channel) selections[part][channel] = part*4608+channel;
    u32 input = attention_tensor(&builder,QNN_TENSOR_TYPE_APP_WRITE,QNN_DATATYPE_FLOAT_16,2,flat,0);
    u8 *constants = fixture_data+164;
    u32 norm0 = prefill_norm(&builder,input,&constants,&scalar_axis,rows);
    u32 query_raw = vision_linear(&builder,norm0,rows,1536,2048,&constants,0,0);
    u32 key_raw = vision_linear(&builder,norm0,rows,1536,1024,&constants,0,0);
    u32 value_raw = vision_linear(&builder,norm0,rows,1536,1024,&constants,0,1);
    u32 cosine = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_32,3,frequency_shape,prefill_cosine);
    u32 sine = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_32,3,frequency_shape,prefill_sine);
    u32 rotation = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_UINT_32,1,head_width,rotations);
    u32 sign = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_32,1,head_width,signs);
    u32 query = prefill_rope(&builder,query_raw,16,cosine,sine,rotation,sign,0,rows);
    u32 key = prefill_rope(&builder,key_raw,8,cosine,sine,rotation,sign,1,rows);
    u32 kv_shape[3] = {rows,8,128};
    u32 value = attention_op(&builder,"Reshape",&value_raw,1,3,kv_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
    u32 all_key = key, all_value = value;
    if (past) {
        u32 previous[3] = {past,8,128}, combined[3] = {length,8,128};
        QnnParam concatenate_axis = attention_axis("axis",0);
        u32 parts[2] = {attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,3,previous,text_keys[index]),key};
        all_key = attention_op(&builder,"Concat",parts,2,3,combined,QNN_DATATYPE_FLOAT_16,0,&concatenate_axis,1);
        parts[0] = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,3,previous,text_values[index]); parts[1] = value;
        all_value = attention_op(&builder,"Concat",parts,2,3,combined,QNN_DATATYPE_FLOAT_16,0,&concatenate_axis,1);
    }
    u32 group = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_UINT_32,1,group_width,groups);
    u32 ids[2] = {all_key,group}; QnnParam group_axis = attention_axis("axis",1);
    u32 expanded_key = attention_op(&builder,"Gather",ids,2,3,all_heads,QNN_DATATYPE_FLOAT_16,0,&group_axis,1);
    ids[0] = all_value;
    u32 expanded_value = attention_op(&builder,"Gather",ids,2,3,all_heads,QNN_DATATYPE_FLOAT_16,0,&group_axis,1);
    u32 query_batch = attention_transpose(&builder,query,batch,permutation);
    u32 key_batch = attention_transpose(&builder,expanded_key,all_batch,permutation);
    u32 key_transposed = attention_transpose(&builder,key_batch,keys,key_permutation);
    u32 value_batch = attention_transpose(&builder,expanded_value,all_batch,permutation);
    ids[0] = query_batch; ids[1] = key_transposed;
    u32 products = attention_op(&builder,"MatMul",ids,2,3,scores_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
    _Float16 scale = (_Float16)0.08838834764831845f, zero = 0, one = 1;
    ids[0] = products; ids[1] = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,one_shape,&scale);
    u32 scores = attention_op(&builder,"ElementWiseMultiply",ids,2,3,scores_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
    u32 mask_shape[3] = {1,rows,length}; ids[0] = scores;
    ids[1] = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,3,mask_shape,prefill_input.mask);
    u32 masked = attention_op(&builder,"ElementWiseAdd",ids,2,3,scores_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
    u32 reduced[2] = {16,rows}, broadcast[3] = {16,rows,1};
    u32 reduction_id = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_UINT_32,1,one_shape,&reduce_axis);
    QnnParam reduction = {0}; reduction.name = "axes"; reduction.type = QNN_PARAMTYPE_TENSOR; reduction.value.tensor = builder.tensors[reduction_id];
    u32 maximum = attention_op(&builder,"ReduceMax",&masked,1,2,reduced,QNN_DATATYPE_FLOAT_16,0,&reduction,1);
    u32 maximum_broadcast = attention_op(&builder,"Reshape",&maximum,1,3,broadcast,QNN_DATATYPE_FLOAT_16,0,0,0);
    ids[0] = masked; ids[1] = maximum_broadcast;
    u32 shifted = attention_op(&builder,"ElementWiseSubtract",ids,2,3,scores_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
    u32 exponential = attention_op(&builder,"ElementWiseExp",&shifted,1,3,scores_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
    u32 sum = attention_op(&builder,"ReduceSum",&exponential,1,2,reduced,QNN_DATATYPE_FLOAT_16,0,&reduction,1);
    u32 denominator = attention_op(&builder,"Reshape",&sum,1,3,broadcast,QNN_DATATYPE_FLOAT_16,0,0,0);
    ids[0] = exponential; ids[1] = denominator;
    u32 probabilities = attention_op(&builder,"ElementWiseDivide",ids,2,3,scores_shape,QNN_DATATYPE_FLOAT_16,1,0,0);
    ids[0] = probabilities; ids[1] = value_batch;
    u32 attended = attention_op(&builder,"MatMul",ids,2,3,batch,QNN_DATATYPE_FLOAT_16,0,0,0);
    u32 ordered = attention_transpose(&builder,attended,head_shape,permutation), context_shape[2] = {rows,2048};
    u32 flattened = attention_op(&builder,"Reshape",&ordered,1,2,context_shape,QNN_DATATYPE_FLOAT_16,0,0,0);
    u32 projected = vision_linear(&builder,flattened,rows,2048,1536,&constants,0,0);
    u32 norm1 = prefill_norm(&builder,projected,&constants,&scalar_axis,rows);
    ids[0] = input; ids[1] = norm1;
    u32 residual = attention_op(&builder,"ElementWiseAdd",ids,2,2,flat,QNN_DATATYPE_FLOAT_16,0,0,0);
    u32 norm2 = prefill_norm(&builder,residual,&constants,&scalar_axis,rows);
    u32 gate_up = vision_linear(&builder,norm2,rows,1536,9216,&constants,0,0);
    u32 wide[2] = {rows,4608}, wide_width[1] = {4608}, branches[2]; QnnParam select_axis = attention_axis("axis",1);
    for (u32 part = 0; part < 2; ++part) {
        ids[0] = gate_up; ids[1] = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_UINT_32,1,wide_width,selections[part]);
        branches[part] = attention_op(&builder,"Gather",ids,2,2,wide,QNN_DATATYPE_FLOAT_16,0,&select_axis,1);
    }
    u32 zero_id = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,one_shape,&zero);
    u32 one_id = attention_tensor(&builder,QNN_TENSOR_TYPE_STATIC,QNN_DATATYPE_FLOAT_16,1,one_shape,&one);
    u32 absolute = attention_op(&builder,"ElementWiseAbs",branches,1,2,wide,QNN_DATATYPE_FLOAT_16,0,0,0);
    u32 negative = attention_op(&builder,"ElementWiseNeg",&absolute,1,2,wide,QNN_DATATYPE_FLOAT_16,0,0,0);
    u32 decay = attention_op(&builder,"ElementWiseExp",&negative,1,2,wide,QNN_DATATYPE_FLOAT_16,0,0,0);
    ids[0] = decay; ids[1] = one_id;
    u32 divisor = attention_op(&builder,"ElementWiseAdd",ids,2,2,wide,QNN_DATATYPE_FLOAT_16,0,0,0);
    ids[0] = branches[0]; ids[1] = zero_id;
    u32 minimum = attention_op(&builder,"ElementWiseMinimum",ids,2,2,wide,QNN_DATATYPE_FLOAT_16,0,0,0);
    u32 numerator = attention_op(&builder,"ElementWiseExp",&minimum,1,2,wide,QNN_DATATYPE_FLOAT_16,0,0,0);
    ids[0] = numerator; ids[1] = divisor;
    u32 factor = attention_op(&builder,"ElementWiseDivide",ids,2,2,wide,QNN_DATATYPE_FLOAT_16,0,0,0);
    ids[0] = branches[0]; ids[1] = factor;
    u32 silu = attention_op(&builder,"ElementWiseMultiply",ids,2,2,wide,QNN_DATATYPE_FLOAT_16,0,0,0);
    ids[0] = silu; ids[1] = branches[1];
    u32 gated = attention_op(&builder,"ElementWiseMultiply",ids,2,2,wide,QNN_DATATYPE_FLOAT_16,0,0,0);
    u32 down = vision_linear(&builder,gated,rows,4608,1536,&constants,0,0);
    u32 norm3 = prefill_norm(&builder,down,&constants,&scalar_axis,rows);
    ids[0] = residual; ids[1] = norm3;
    u32 output = attention_op(&builder,"ElementWiseAdd",ids,2,2,flat,QNN_DATATYPE_FLOAT_16,1,0,0);
    u32 outputs[8] = {norm0,norm1,norm2,norm3,output,key,value_raw,probabilities};
    if (constants != fixture_data+61354148 || !vision_execution(&builder,input,prefill_input.embeddings,rows*1536,outputs,8,prefill_output)) return 0;
    u32 probability_offset = rows*(5*1536+2*1024);
    for (u32 row = 0; row < 16*rows; ++row) {
        float total = 0;
        for (u32 column = 0; column < length; ++column) {
            union {u16 bits; _Float16 value;} probability; probability.bits = prefill_output[probability_offset+row*length+column];
            if (!(probability.value >= 0 && probability.value <= 1) ||
                (!past && (column > row%rows || column >= prefill_input.count) && probability.value != 0)) return 0;
            total += (float)probability.value;
        }
        if (!(total >= 0.997f && total <= 1.003f)) return 0;
    }
    char input_suffix[] = ".decode-00-input.f16", taps_suffix[] = ".decode-00-taps.f16";
    input_suffix[8] = taps_suffix[8] = '0'+past/10; input_suffix[9] = taps_suffix[9] = '0'+past%10;
    if (!capture_tensor(index,past ? input_suffix : ".text-input.f16",prefill_input.embeddings,rows*1536*2) ||
        !capture_tensor(index,past ? taps_suffix : ".text-taps.f16",prefill_output,(probability_offset+16*rows*length)*2)) return 0;
    u32 retained = past ? 1 : prefill_input.count;
    for (u32 offset = 0; offset < retained*1024; ++offset) {
        text_keys[index][past*1024+offset] = prefill_output[5*rows*1536+offset];
        text_values[index][past*1024+offset] = prefill_output[5*rows*1536+rows*1024+offset];
    }
    text_cache_count[index] = past+retained;
    for (u32 offset = 0; offset < rows*1536; ++offset) prefill_input.embeddings[offset] = prefill_output[4*rows*1536+offset];
    checked("text_layer_completed",index); text("PASS causal/padding attention and exact text handoff\n"); return 1;
}

static int prefill_forward(const QnnInterfaceV2 *api, QnnBackendHandle backend, QnnDeviceHandle device,
                            QnnContextHandle *context, const unsigned short *directory, u32 task) {
    u32 tokens = prefill_grid_width*8, image_tokens = tokens/4;
    u16 *features = vision_tail_output+tokens*1024+image_tokens*1536;
    if (!ocr_text_prepare(features,image_tokens,(const u16 *)(prefill_embeddings+160),(u64)OCR_TEXT_VOCAB*OCR_TEXT_WIDTH,
                          8,prefill_grid_width,task,0,&prefill_input)) return 0;
    u32 layout[7] = {prefill_input.count,image_tokens,task,0,8,prefill_grid_width,(u32)prefill_input.delta};
    if (!capture_tensor(0,".text-layout.u32",layout,sizeof(layout)) || !capture_tensor(0,".text-ids.u32",prefill_input.ids,sizeof(prefill_input.ids)) ||
        !capture_tensor(0,".text-positions.i32",prefill_input.positions,sizeof(prefill_input.positions)) ||
        !capture_tensor(0,".text-modalities.u8",prefill_input.modalities,sizeof(prefill_input.modalities)) ||
        !capture_tensor(0,".text-mask.f16",prefill_input.mask,sizeof(prefill_input.mask)) ||
        !capture_tensor(0,".text-embeddings.f16",prefill_input.embeddings,sizeof(prefill_input.embeddings))) return 0;
    for (u32 row = 0; row < 64; ++row) for (u32 channel = 0; channel < 128; ++channel) {
        u32 axis = channel < 32 ? 0 : channel < 80 ? 1 : 2;
        int position = prefill_input.positions[axis*64+row];
        if (position < 0 || position >= 64) return 0;
        union {u32 bits; float value;} cosine, sine;
        cosine.bits = load32(prefill_shared+160+3072+(position*128+channel)*4);
        sine.bits = load32(prefill_shared+160+3072+64*128*4+(position*128+channel)*4);
        prefill_cosine[row*128+channel] = cosine.value; prefill_sine[row*128+channel] = sine.value;
    }
    if (!capture_tensor(0,".text-cosine.f32",prefill_cosine,sizeof(prefill_cosine)) || !capture_tensor(0,".text-sine.f32",prefill_sine,sizeof(prefill_sine))) return 0;
    for (u32 index = 0; index <= 16; ++index) {
        if (!checked("text_context_free",api->context_free(*context,0))) return 0;
        *context = 0;
        if ((index < 16 && !prefill_asset(directory,index,0)) || !checked("text_context_create",api->context_create(backend,device,0,context))) return 0;
        if (index < 16) { if (!prefill_layer(api,*context,index,0)) return 0; }
        else {
            OcrAttentionGraph builder = {0}; builder.api = api; builder.good = 1;
            if (!checked("text_norm_graph",api->graph_create(*context,"text_final_norm",0,&builder.graph))) return 0;
            u32 shape[2] = {64,1536}, axis = 1;
            u32 input = attention_tensor(&builder,QNN_TENSOR_TYPE_APP_WRITE,QNN_DATATYPE_FLOAT_16,2,shape,0);
            u8 *gamma = prefill_shared+160; u32 normalized = prefill_norm(&builder,input,&gamma,&axis,64);
            if (!vision_execution(&builder,input,prefill_input.embeddings,64*1536,&normalized,1,prefill_output) ||
                !capture_tensor(16,".text-norm.f16",prefill_output,64*1536*2)) return 0;
        }
    }
    return 1;
}