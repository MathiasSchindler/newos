static u8 generation_tokenizer_data[16U*1024U*1024U], generation_hashes[8][32];
static OcrTokenizer generation_tokenizer;
static const unsigned short *generation_directory;
static u32 generation_limit, generation_reason;
static u16 generation_logits[59392], generation_hidden[1536];
static u32 generation_ids[64];
static u8 generation_utf8[64*96*3];

static int generation_asset(const unsigned short *directory, u32 index, int remember) {
    unsigned short path[32768]; char name[] = "head-00.got";
    if (index >= 8) return 0;
    name[6] = '0'+index;
    u32 size = 164+7424*1536*2;
    if (!prefill_path(directory,name,path) || read_blob(path,fixture_data,size) != size ||
        !ocr_artifact(fixture_data,size,17) || load32(fixture_data+160) != index) return 0;
    static const char identity[] = "a16eb0de98d199293371c560f95f83130d2a2c9612449df16839f08ff9498815";
    static const char hex[] = "0123456789abcdef";
    for (u32 offset = 0; offset < 32; ++offset) {
        u8 value = fixture_data[128+offset];
        if (hex[value >> 4] != identity[offset*2] || hex[value & 15] != identity[offset*2+1]) return 0;
        if (remember) generation_hashes[index][offset] = fixture_data[96+offset];
        else if (generation_hashes[index][offset] != fixture_data[96+offset]) return 0;
    }
    for (u32 offset = 164; offset < size; offset += 2) if ((fixture_data[offset+1] & 0x7c) == 0x7c) return 0;
    return 1;
}

static int generation_check(const unsigned short *directory) {
    unsigned short path[32768];
    for (u32 index = 0; index < 8; ++index) if (!generation_asset(directory,index,1)) return 0;
    if (!prefill_path(directory,"tokenizer.got",path)) return 0;
    u32 size = read_blob(path,generation_tokenizer_data,sizeof(generation_tokenizer_data));
    return ocr_tokenizer_open(&generation_tokenizer,generation_tokenizer_data,size);
}

static int generation_argmax(const u16 *values, u32 count, u32 *selected) {
    if (!values || !count || !selected) return 0;
    union {u16 bits; _Float16 value;} best, candidate;
    best.bits = values[0]; *selected = 0;
    for (u32 index = 0; index < count; ++index) {
        candidate.bits = values[index];
        if ((candidate.bits & 0x7c00) == 0x7c00) return 0;
        if (candidate.value > best.value) { best = candidate; *selected = index; }
    }
    return 1;
}

static u32 generation_stop(u32 token, u32 count, u32 limit, u32 prompt) {
    if (token == 59246 || token == 59253) return 1;
    if (count >= limit) return 2;
    if (prompt+count >= 64) return 3;
    return 0;
}

int ocr_generation_test(const unsigned short *directory) {
    u16 values[] = {0xbc00,0x3c00,0x3c00,0}; u32 selected;
    if (!generation_argmax(values,4,&selected) || selected != 1 || generation_argmax(values,0,&selected)) return 0;
    values[3] = 0x7e00;
    if (generation_argmax(values,4,&selected)) return 0;
    values[3] = 0xfc00;
    if (generation_argmax(values,4,&selected)) return 0;
    if (generation_stop(59246,1,1,63) != 1 || generation_stop(59253,1,64,1) != 1 ||
        generation_stop(10,3,3,28) != 2 || generation_stop(10,20,64,44) != 3 || generation_stop(10,1,32,28)) return 0;
    if (!generation_check(directory)) return 0;
    text("PASS generation weights/tokenizer, finite argmax/ties and EOS/token/context limits\n"); return 1;
}

static int generation_context(const QnnInterfaceV2 *api, QnnBackendHandle backend, QnnDeviceHandle device, QnnContextHandle *context) {
    if (*context && !checked("generation_context_free",api->context_free(*context,0))) return 0;
    *context = 0;
    return checked("generation_context_create",api->context_create(backend,device,0,context));
}

static int generation_head(const QnnInterfaceV2 *api, QnnBackendHandle backend, QnnDeviceHandle device,
                           QnnContextHandle *context, u32 step, u32 *selected) {
    for (u32 index = 0; index < 8; ++index) {
        if (!generation_context(api,backend,device,context) || !generation_asset(generation_directory,index,0)) return 0;
        OcrAttentionGraph builder = {0}; builder.api = api; builder.good = 1;
        if (!checked("head_graph",api->graph_create(*context,"ocr_lm_head",0,&builder.graph))) return 0;
        u32 shape[2] = {1,1536}; u8 *constants = fixture_data+164;
        u32 input = attention_tensor(&builder,QNN_TENSOR_TYPE_APP_WRITE,QNN_DATATYPE_FLOAT_16,2,shape,0);
        u32 logits = vision_linear(&builder,input,1,1536,7424,&constants,0,1);
        if (!vision_execution(&builder,input,generation_hidden,1536,&logits,1,generation_logits+index*7424)) return 0;
    }
    return capture_tensor(step,".head-input.f16",generation_hidden,sizeof(generation_hidden)) &&
        capture_tensor(step,".logits.f16",generation_logits,sizeof(generation_logits)) &&
        generation_argmax(generation_logits,59392,selected);
}

static int generation_decode(const QnnInterfaceV2 *api, QnnBackendHandle backend, QnnDeviceHandle device,
                             QnnContextHandle *context, const unsigned short *directory, u32 token, u32 past) {
    int position = (int)past+prefill_input.delta;
    if (token >= 59282 || !past || past >= 64 || position < 0 || position >= 64) return 0;
    for (u32 offset = 0; offset < 1536; ++offset) prefill_input.embeddings[offset] = ((const u16 *)(prefill_embeddings+160))[token*1536+offset];
    for (u32 offset = 0; offset < past+1; ++offset) prefill_input.mask[offset] = 0;
    for (u32 channel = 0; channel < 128; ++channel) {
        union {u32 bits; float value;} cosine, sine;
        cosine.bits = load32(prefill_shared+160+3072+(position*128+channel)*4);
        sine.bits = load32(prefill_shared+160+3072+64*128*4+(position*128+channel)*4);
        prefill_cosine[channel] = cosine.value; prefill_sine[channel] = sine.value;
    }
    u32 layout[4] = {past,(u32)position,token,prefill_input.count};
    if (!capture_tensor(past,".decode-layout.u32",layout,sizeof(layout))) return 0;
    for (u32 index = 0; index < 16; ++index) {
        if (!generation_context(api,backend,device,context) || !prefill_asset(directory,index,0) ||
            !prefill_layer(api,*context,index,past)) return 0;
    }
    if (!generation_context(api,backend,device,context)) return 0;
    OcrAttentionGraph builder = {0}; builder.api = api; builder.good = 1;
    if (!checked("decode_norm_graph",api->graph_create(*context,"decode_final_norm",0,&builder.graph))) return 0;
    u32 shape[2] = {1,1536}, axis = 1; u8 *gamma = prefill_shared+160;
    u32 input = attention_tensor(&builder,QNN_TENSOR_TYPE_APP_WRITE,QNN_DATATYPE_FLOAT_16,2,shape,0);
    u32 normalized = prefill_norm(&builder,input,&gamma,&axis,1);
    return vision_execution(&builder,input,prefill_input.embeddings,1536,&normalized,1,generation_hidden) &&
        capture_tensor(past,".decode-norm.f16",generation_hidden,sizeof(generation_hidden));
}

static int generation_emit(u32 count, u32 *emitted, int final) {
    int size = ocr_decode_prefix(&generation_tokenizer,generation_ids,count,1,generation_utf8,sizeof(generation_utf8),final);
    if (size < 0 || (u32)size < *emitted) return 0;
    u32 written = 0, bytes = (u32)size-*emitted;
    if (bytes && (!WriteFile(GetStdHandle((u32)-11),generation_utf8+*emitted,bytes,&written,0) || written != bytes)) return 0;
    *emitted = (u32)size; return 1;
}

static int generation_forward(const QnnInterfaceV2 *api, QnnBackendHandle backend, QnnDeviceHandle device,
                              QnnContextHandle *context, const unsigned short *directory) {
    u32 count = 0, emitted = 0;
    generation_reason = 0;
    for (u32 offset = 0; offset < 1536; ++offset) generation_hidden[offset] = prefill_output[(prefill_input.count-1)*1536+offset];
    while (count < generation_limit && prefill_input.count+count < 64) {
        u32 selected;
        if (!generation_head(api,backend,device,context,count,&selected)) return 0;
        if (selected >= 59282) { text("FAIL selected vocabulary ID has no tokenizer piece\n"); return 0; }
        generation_ids[count++] = selected;
        checked("generated_token_id",selected);
        generation_reason = generation_stop(selected,count,generation_limit,prefill_input.count);
        if (!generation_emit(count,&emitted,generation_reason != 0)) return 0;
        if (generation_reason) break;
        if (!generation_decode(api,backend,device,context,directory,selected,prefill_input.count+count-1)) return 0;
    }
    if (!generation_reason) return 0;
    u32 result[4] = {count,generation_reason,prefill_input.count,generation_limit};
    if (!capture_tensor(0,".generated-ids.u32",generation_ids,count*4) || !capture_tensor(0,".generation-result.u32",result,sizeof(result)) ||
        !capture_tensor(0,".generated.txt",generation_utf8,emitted)) return 0;
    text(generation_reason == 1 ? "STOP EOS\n" : generation_reason == 2 ? "STOP max-new-tokens (incomplete)\n" : "STOP context-limit (incomplete)\n");
    return 1;
}