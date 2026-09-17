__declspec(dllimport) void *VirtualAlloc(void *, u64, u32, u32);
__declspec(dllimport) int VirtualFree(void *, u64, u32);
__declspec(dllimport) u32 GetModuleFileNameW(void *, unsigned short *, u32);
__declspec(dllimport) int CreateDirectoryW(const unsigned short *, void *);

typedef struct {
    u32 version, bytes, count, reserved;
    u8 key[32], digest[32];
    u32 ids[OCR_ATTENTION_TENSORS];
} OcrGraphCacheHeader;

static int graph_cache_active;
static unsigned short graph_cache_directory[32768];
static u32 graph_cache_directory_length;
static u8 graph_cache_identity[32];
static QnnBackendHandle graph_cache_backend;
static QnnDeviceHandle graph_cache_device;
static QnnContextHandle *graph_cache_context;
static u8 *graph_cache_binary;

static void graph_cache_digest(const OcrGraphCacheHeader *header, const u8 *binary, u8 digest[32]) {
    CryptoSha256Context hash; crypto_sha256_init(&hash);
    crypto_sha256_update(&hash,(const u8 *)header,48);
    crypto_sha256_update(&hash,(const u8 *)header->ids,sizeof(header->ids));
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
    graph_cache_backend = backend; graph_cache_device = device; graph_cache_context = context;
    graph_cache_active = 1;
    return 1;
}

static int graph_cache_end(void) {
    graph_cache_active = 0; graph_cache_context = 0;
    int good = !graph_cache_binary || VirtualFree(graph_cache_binary,0,0x8000);
    graph_cache_binary = 0;
    return good;
}

static int graph_cache_finalize(OcrAttentionGraph *builder) {
    const QnnInterfaceV2 *api = builder->api;
    if (!graph_cache_active || (profile_phase != 2 && profile_phase != 4))
        return checked("graph_finalize",api->graph_finalize(builder->graph,0,0));
    if (!api->context_get_binary_size || !api->context_get_binary || !api->context_create_from_binary || !api->graph_retrieve) return 0;
    if (graph_cache_binary) {
        if (!VirtualFree(graph_cache_binary,0,0x8000)) return 0;
        graph_cache_binary = 0;
    }
    unsigned short path[32768]; u32 length = graph_cache_directory_length;
    for (u32 index = 0; index < length; ++index) path[index] = graph_cache_directory[index];
    path[length++] = '0'+profile_phase; path[length++] = '-';
    path[length++] = '0'+profile_layer/10; path[length++] = '0'+profile_layer%10;
    const char *extension = ".qoc";
    for (u32 index = 0; ; ++index) { path[length++] = extension[index]; if (!extension[index]) break; }
    OcrGraphCacheHeader expected = {0};
    expected.version = 2; expected.count = builder->count;
    crypto_sha256_final(&builder->cache_hash,expected.key);
    for (u32 index = 0; index < builder->count; ++index) expected.ids[index] = builder->tensors[index].data.v1.id;
    void *file = CreateFileW(path,0x80000000U,1,0,3,0x08000080U,0);
    if (file != (void *)~0ULL) {
        OcrGraphCacheHeader stored; long long size = 0; u32 received = 0;
        int match = GetFileSizeEx(file,&size) && size > (long long)sizeof(stored) && size <= 128*1024*1024 &&
            ReadFile(file,&stored,sizeof(stored),&received,0) && received == sizeof(stored) &&
            stored.version == expected.version && stored.count == expected.count && !stored.reserved && stored.bytes == (u64)size-sizeof(stored);
        for (u32 index = 0; match && index < 32; ++index) if (stored.key[index] != expected.key[index]) match = 0;
        if (match) {
            graph_cache_binary = VirtualAlloc(0,stored.bytes,0x3000,4);
            match = graph_cache_binary && ReadFile(file,graph_cache_binary,stored.bytes,&received,0) && received == stored.bytes;
            if (match) {
                u8 digest[32]; graph_cache_digest(&stored,graph_cache_binary,digest);
                for (u32 index = 0; index < 32; ++index) if (stored.digest[index] != digest[index]) match = 0;
            }
        }
        if (!CloseHandle(file)) return 0;
        if (match) {
            if (!checked("cache_unfinalized_free",api->context_free(*graph_cache_context,0))) return 0;
            *graph_cache_context = 0;
            if (!checked("cache_restore",api->context_create_from_binary(graph_cache_backend,graph_cache_device,0,graph_cache_binary,stored.bytes,graph_cache_context,0))) return 0;
            const char *name = profile_phase == 4 ? "text_prefill_layer" : profile_layer ? "vision_attention_1" : "vision_attention_0";
            if (!checked("cache_graph",api->graph_retrieve(*graph_cache_context,name,&builder->graph))) return 0;
            for (u32 index = 0; index < builder->count; ++index) builder->tensors[index].data.v1.id = stored.ids[index];
            text("CACHE hit\n"); return 1;
        }
        if (graph_cache_binary && !VirtualFree(graph_cache_binary,0,0x8000)) return 0;
        graph_cache_binary = 0;
    }
    if (!checked("cache_finalize",api->graph_finalize(builder->graph,0,0))) return 0;
    u64 bytes = 0, written_bytes = 0;
    if (!checked("cache_binary_size",api->context_get_binary_size(*graph_cache_context,&bytes)) || !bytes || bytes > 128*1024*1024-sizeof(expected)) return 0;
    graph_cache_binary = VirtualAlloc(0,bytes,0x3000,4);
    if (!graph_cache_binary || !checked("cache_binary",api->context_get_binary(*graph_cache_context,graph_cache_binary,bytes,&written_bytes)) || bytes != written_bytes) return 0;
    expected.bytes = (u32)bytes;
    graph_cache_digest(&expected,graph_cache_binary,expected.digest);
    file = CreateFileW(path,0x40000000U,0,0,2,0x80,0);
    if (file == (void *)~0ULL) { text("CACHE store unavailable\n"); return 1; }
    u32 written = 0;
    int good = WriteFile(file,&expected,sizeof(expected),&written,0) && written == sizeof(expected) &&
        WriteFile(file,graph_cache_binary,(u32)bytes,&written,0) && written == bytes;
    if (!CloseHandle(file)) good = 0;
    text(good ? "CACHE stored\n" : "CACHE store incomplete\n");
    return 1;
}