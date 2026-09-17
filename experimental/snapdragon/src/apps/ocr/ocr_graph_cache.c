__declspec(dllimport) void *VirtualAlloc(void *, u64, u32, u32);
__declspec(dllimport) int VirtualFree(void *, u64, u32);
__declspec(dllimport) u32 GetModuleFileNameW(void *, unsigned short *, u32);
__declspec(dllimport) int CreateDirectoryW(const unsigned short *, void *);
__declspec(dllimport) void *GetCurrentProcess(void);
__declspec(dllimport) int K32GetProcessMemoryInfo(void *, void *, u32);
__declspec(dllimport) int GlobalMemoryStatusEx(void *);

typedef struct {
    u32 count, ids[OCR_ATTENTION_TENSORS];
    u8 key[32];
} OcrGraphBinding;

typedef struct {
    u32 version, bytes, count, reserved;
    u8 key[32], digest[32];
    OcrGraphBinding bindings[24];
} OcrBundleHeader;

typedef struct {
    QnnContextHandle context;
    QnnGraphHandle graphs[24];
    u8 *binary;
    u32 binary_bytes, ready;
    OcrBundleHeader header;
} OcrGraphBundle;

static int graph_cache_active;
static unsigned short graph_cache_directory[32768];
static u32 graph_cache_directory_length;
static u8 graph_cache_identity[32];
static QnnBackendHandle graph_cache_backend;
static QnnDeviceHandle graph_cache_device;
static const QnnInterfaceV2 *graph_cache_api;
static OcrGraphBundle graph_bundles[20];
static u64 graph_cache_bytes;

static u32 graph_bundle_slot(u32 phase, u32 layer) {
    return (phase == 4 ? 12U : 0U)+layer/2;
}

static OcrGraphBundle *graph_cache_current(void) {
    return &graph_bundles[graph_bundle_slot(profile_phase,profile_layer)];
}

static int graph_cache_memory_available(u64 additional, int report) {
    struct { u32 size, faults; u64 counters[9]; } process = {sizeof(process),0,{0}};
    struct { u32 size, load; u64 physical, available, pagefile, available_pagefile, virtual_bytes, available_virtual, extended; } memory = {0};
    memory.size = sizeof(memory);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(),&process,sizeof(process)) || !GlobalMemoryStatusEx(&memory) ||
        process.counters[8]+additional > 12ULL*1024*1024*1024 || memory.available < additional+512ULL*1024*1024) {
        if (report) {
            checked("resident_private_bytes",process.counters[8]); checked("resident_available_bytes",memory.available);
            checked("resident_requested_bytes",additional); text("FAIL QNN resident memory budget\n");
        }
        return 0;
    }
    return 1;
}

static int graph_cache_budget(u64 additional) {
    return graph_cache_memory_available(additional,1);
}

static void graph_cache_memory_trace(u32 layer) {
    struct { u32 size, faults; u64 counters[9]; } process = {sizeof(process),0,{0}};
    struct { u32 size, load; u64 counters[7]; } memory = {sizeof(memory),0,{0}};
    checked("decode_build_layer",layer);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),&process,sizeof(process))) checked("decode_private_bytes",process.counters[8]);
    if (GlobalMemoryStatusEx(&memory)) checked("decode_available_bytes",memory.counters[1]);
}

static int graph_bundle_release(const QnnInterfaceV2 *api, OcrGraphBundle *bundle) {
    if (bundle->context && (!api || !checked("bundle_context_free",api->context_free(bundle->context,0)))) return 0;
    bundle->context = 0;
    if (bundle->binary && !VirtualFree(bundle->binary,0,0x8000)) return 0;
    graph_cache_bytes -= bundle->binary_bytes;
    *bundle = (OcrGraphBundle){0};
    return 1;
}

static int graph_resident_release(const QnnInterfaceV2 *api) {
    int good = 1;
    for (u32 index = 0; index < 20; ++index)
        if (!graph_bundle_release(api,&graph_bundles[index])) good = 0;
    return good;
}

static int graph_cache_trim(const QnnInterfaceV2 *api, u32 keep) {
    for (u32 index = 0; index < 20; ++index)
        if (index != keep && index != 0 && index != 12 && !graph_bundle_release(api,&graph_bundles[index])) return 0;
    return 1;
}

static void graph_cache_digest(const OcrBundleHeader *header, const u8 *binary, u8 digest[32]) {
    CryptoSha256Context hash; crypto_sha256_init(&hash);
    crypto_sha256_update(&hash,(const u8 *)header,48);
    crypto_sha256_update(&hash,(const u8 *)header->bindings,sizeof(header->bindings));
    crypto_sha256_update(&hash,binary,header->bytes);
    crypto_sha256_final(&hash,digest);
}

static int graph_cache_hash_file(CryptoSha256Context *hash, const unsigned short *path) {
    void *file = CreateFileW(path,0x80000000U,1,0,3,0x08000080U,0);
    u8 buffer[65536]; u32 received = 0; int good = 1;
    if (file == (void *)~0ULL) return 0;
    for (;;) {
        if (!ReadFile(file,buffer,sizeof(buffer),&received,0)) { good = 0; break; }
        if (!received) break;
        crypto_sha256_update(hash,buffer,received);
    }
    if (!CloseHandle(file)) good = 0;
    return good;
}

static int graph_cache_begin(const unsigned short *library, QnnBackendHandle backend, QnnDeviceHandle device, QnnContextHandle *context) {
    (void)context;
    CryptoSha256Context hash; crypto_sha256_init(&hash);
    unsigned short path[32768];
    u32 length = GetModuleFileNameW(0,path,32768);
    if (!length || length >= 32700 || !graph_cache_hash_file(&hash,path)) return 0;
    while (length && path[length-1] != '\\' && path[length-1] != '/') --length;
    if (!length) return 0;
    for (u32 index = 0; index < length; ++index) graph_cache_directory[index] = path[index];
    const char *directory = "graph-cache\\";
    for (u32 index = 0; directory[index]; ++index) graph_cache_directory[length++] = directory[index];
    graph_cache_directory[length-1] = 0;
    CreateDirectoryW(graph_cache_directory,0);
    graph_cache_directory[length-1] = '\\'; graph_cache_directory[length] = 0;
    graph_cache_directory_length = length;
    length = 0; while (library[length]) { if (length >= 32700) return 0; path[length] = library[length]; ++length; }
    path[length] = 0;
    if (!graph_cache_hash_file(&hash,path)) return 0;
    while (length && path[length-1] != '\\' && path[length-1] != '/') --length;
    if (!length) return 0;
    const char *companions[] = {"QnnHtpPrepare.dll","QnnHtpV73Stub.dll","libQnnHtpV73Skel.so"};
    for (u32 item = 0; item < 3; ++item) {
        u32 end = length;
        for (u32 index = 0; companions[item][index]; ++index) path[end++] = companions[item][index];
        path[end] = 0;
        if (!graph_cache_hash_file(&hash,path)) return 0;
    }
    crypto_sha256_final(&hash,graph_cache_identity);
    graph_cache_backend = backend; graph_cache_device = device;
    graph_cache_active = 1;
    return 1;
}

static int graph_cache_end(void) {
    graph_cache_active = 0;
    return ocr_resident || graph_resident_release(graph_cache_api);
}

static void graph_cache_path(unsigned short *path, u32 phase) {
    u32 length = graph_cache_directory_length;
    for (u32 index = 0; index < length; ++index) path[index] = graph_cache_directory[index];
    char name[] = "2-00.qob";
    name[0] = '0'+phase; name[2] = '0'+(profile_layer/2)/10; name[3] = '0'+(profile_layer/2)%10;
    for (u32 index = 0; ; ++index) { path[length++] = name[index]; if (!name[index]) break; }
}

static void graph_cache_name(char name[8], u32 phase, u32 layer) {
    name[0] = 'g'; name[1] = '0'+phase; name[2] = '_';
    name[3] = '0'+layer/10; name[4] = '0'+layer%10; name[5] = 0;
}

static int graph_bundle_restore(const QnnInterfaceV2 *api, OcrGraphBundle *bundle, u32 phase) {
    if (bundle->context && !checked("bundle_prepared_free",api->context_free(bundle->context,0))) return 0;
    bundle->context = 0;
    if (!checked("bundle_restore",api->context_create_from_binary(graph_cache_backend,graph_cache_device,0,
        bundle->binary,bundle->header.bytes,&bundle->context,0))) return 0;
    for (u32 layer = 0; layer < bundle->header.count; ++layer) {
        char name[8]; graph_cache_name(name,phase,(profile_layer/2)*2+layer);
        if (!checked("bundle_graph",api->graph_retrieve(bundle->context,name,&bundle->graphs[layer]))) return 0;
    }
    bundle->ready = 1;
    return graph_cache_budget(0);
}

static int graph_cache_stage(const QnnInterfaceV2 *api, u32 phase, u32 rows, u32 length,
                             const void *weights, u32 weight_bytes, const void *constants, u32 constant_bytes,
                             const void *extra, u32 extra_bytes) {
    u32 slot = graph_bundle_slot(phase,profile_layer);
    OcrGraphBundle *bundle = &graph_bundles[slot];
    u32 geometry[4] = {phase,rows,length,profile_layer/2}, count = 2;
    u8 key[32]; CryptoSha256Context hash; crypto_sha256_init(&hash);
    crypto_sha256_update(&hash,graph_cache_identity,32);
    crypto_sha256_update(&hash,(const u8 *)geometry,sizeof(geometry));
    crypto_sha256_update(&hash,weights,weight_bytes);
    crypto_sha256_update(&hash,constants,constant_bytes);
    if (extra_bytes) crypto_sha256_update(&hash,extra,extra_bytes);
    crypto_sha256_final(&hash,key);
    graph_cache_api = api;
    if (!graph_cache_trim(api,slot)) return 0;
    int match = bundle->ready != 0;
    for (u32 index = 0; match && index < 32; ++index) if (key[index] != bundle->header.key[index]) match = 0;
    if (match) return graph_cache_budget(0);
    if (!graph_bundle_release(api,bundle)) return 0;
    unsigned short path[32768]; graph_cache_path(path,phase);
    void *file = CreateFileW(path,0x80000000U,1,0,3,0x08000080U,0);
    if (file != (void *)~0ULL) {
        long long size = 0; u32 received = 0;
        OcrBundleHeader *stored = &bundle->header;
        match = GetFileSizeEx(file,&size) && size > (long long)sizeof(*stored) && size <= 1536LL*1024*1024 &&
            ReadFile(file,stored,sizeof(*stored),&received,0) && received == sizeof(*stored) &&
            stored->version == 5 && stored->count == count && !stored->reserved && stored->bytes == (u64)size-sizeof(*stored);
        for (u32 index = 0; match && index < 32; ++index) if (stored->key[index] != key[index]) match = 0;
        for (u32 layer = 0; match && layer < count; ++layer)
            if (!stored->bindings[layer].count || stored->bindings[layer].count > OCR_ATTENTION_TENSORS) match = 0;
        if (match && !graph_cache_memory_available((u64)stored->bytes*2,0)) {
            text("CACHE evict inactive bundle\n");
            for (u32 index = 0; index < 20; ++index)
                if (index != slot && !graph_bundle_release(api,&graph_bundles[index])) { CloseHandle(file); return 0; }
            if (!graph_cache_budget((u64)stored->bytes*2)) {
                CloseHandle(file); return 0;
            }
        }
        if (match) {
            if (stored->bytes > 3ULL*1024*1024*1024-graph_cache_bytes) { CloseHandle(file); return 0; }
            bundle->binary = VirtualAlloc(0,stored->bytes,0x3000,4);
            if (bundle->binary) { bundle->binary_bytes = stored->bytes; graph_cache_bytes += stored->bytes; }
            match = bundle->binary && ReadFile(file,bundle->binary,stored->bytes,&received,0) && received == stored->bytes;
            if (match) {
                u8 digest[32]; graph_cache_digest(stored,bundle->binary,digest);
                for (u32 index = 0; index < 32; ++index) if (stored->digest[index] != digest[index]) match = 0;
            }
        }
        if (!CloseHandle(file)) return 0;
        if (match) {
            text("CACHE bundle restored\n"); return graph_bundle_restore(api,bundle,phase);
        }
        if (!graph_bundle_release(api,bundle)) return 0;
    }
    bundle->header.version = 5; bundle->header.count = count;
    for (u32 index = 0; index < 32; ++index) bundle->header.key[index] = key[index];
    return graph_cache_budget(0) && checked("bundle_context_create",api->context_create(graph_cache_backend,graph_cache_device,0,&bundle->context));
}

static void graph_cache_key_begin(OcrAttentionGraph *builder, u32 rows, u32 length) {
    u32 geometry[4] = {profile_phase,profile_layer,rows,length};
    crypto_sha256_init(&builder->cache_hash);
    crypto_sha256_update(&builder->cache_hash,graph_cache_identity,32);
    crypto_sha256_update(&builder->cache_hash,(const u8 *)geometry,sizeof(geometry));
}

static int graph_cache_open(OcrAttentionGraph *builder, QnnContextHandle context, const char *unused_name) {
    (void)context; (void)unused_name;
    OcrGraphBundle *bundle = graph_cache_current();
    u32 layer = profile_layer%2;
    if (!bundle->context || layer >= bundle->header.count) return 0;
    crypto_sha256_final(&builder->cache_hash,builder->cache_key);
    if (bundle->ready) {
        OcrGraphBinding *binding = &bundle->header.bindings[layer];
        for (u32 index = 0; index < 32; ++index) if (binding->key[index] != builder->cache_key[index]) return 0;
        builder->graph = bundle->graphs[layer]; builder->cached_count = binding->count;
        for (u32 index = 0; index < binding->count; ++index) builder->cached_ids[index] = binding->ids[index];
        text("CACHE resident hit\n"); return 1;
    }
    char name[8]; graph_cache_name(name,profile_phase,profile_layer);
    return checked("cache_graph_build",builder->api->graph_create(bundle->context,name,0,&builder->graph));
}

static int graph_cache_finalize(OcrAttentionGraph *builder) {
    const QnnInterfaceV2 *api = builder->api;
    if (builder->cached_count) return builder->good && builder->count == builder->cached_count;
    if (!graph_cache_active || (profile_phase != 2 && profile_phase != 4))
        return checked("graph_finalize",api->graph_finalize(builder->graph,0,0));
    OcrGraphBundle *bundle = graph_cache_current();
    u32 layer = profile_layer%2;
    if (!graph_cache_budget(0) || !checked("cache_finalize",api->graph_finalize(builder->graph,0,0))) return 0;
    OcrGraphBinding *binding = &bundle->header.bindings[layer]; binding->count = builder->count;
    for (u32 index = 0; index < 32; ++index) binding->key[index] = builder->cache_key[index];
    for (u32 index = 0; index < builder->count; ++index) binding->ids[index] = builder->tensors[index].data.v1.id;
    bundle->graphs[layer] = builder->graph;
    if (layer+1 != bundle->header.count) return 1;
    u64 bytes = 0, written_bytes = 0;
    if (!checked("cache_binary_size",api->context_get_binary_size(bundle->context,&bytes)) || !bytes ||
        bytes > 1536ULL*1024*1024-sizeof(bundle->header) || bytes > 3ULL*1024*1024*1024-graph_cache_bytes || !graph_cache_budget(bytes)) return 0;
    bundle->binary = VirtualAlloc(0,bytes,0x3000,4);
    if (!bundle->binary) return 0;
    bundle->binary_bytes = (u32)bytes; graph_cache_bytes += bytes;
    if (!checked("cache_binary",api->context_get_binary(bundle->context,bundle->binary,bytes,&written_bytes)) || bytes != written_bytes) return 0;
    bundle->header.bytes = (u32)bytes; bundle->ready = 1;
    graph_cache_digest(&bundle->header,bundle->binary,bundle->header.digest);
    unsigned short path[32768]; graph_cache_path(path,profile_phase);
    void *file = CreateFileW(path,0x40000000U,0,0,2,0x80,0);
    if (file == (void *)~0ULL) text("CACHE store unavailable\n");
    else {
        u32 written = 0;
        int good = WriteFile(file,&bundle->header,sizeof(bundle->header),&written,0) && written == sizeof(bundle->header) &&
            WriteFile(file,bundle->binary,(u32)bytes,&written,0) && written == bytes;
        if (!CloseHandle(file)) good = 0;
        text(good ? "CACHE bundle stored\n" : "CACHE store incomplete\n");
    }
    if (!graph_bundle_restore(api,bundle,profile_phase)) return 0;
    builder->graph = bundle->graphs[layer];
    return 1;
}