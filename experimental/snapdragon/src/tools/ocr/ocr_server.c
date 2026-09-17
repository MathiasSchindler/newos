__declspec(dllimport) int SetStdHandle(u32, void *);

static int server_read(void *input, void *data, u32 bytes) {
    u32 offset = 0;
    while (offset < bytes) {
        u32 received = 0;
        if (!ReadFile(input,(u8 *)data+offset,bytes-offset,&received,0) || !received) return offset ? -1 : 0;
        offset += received;
    }
    return 1;
}

int ocr_generate_server(const unsigned short *library, const unsigned short *vision, const unsigned short *decoder, const unsigned short *head) {
    void *input = GetStdHandle((u32)-10);
    void *original_output = GetStdHandle((u32)-11), *original_error = GetStdHandle((u32)-12);
    static unsigned short image[32768], capture[32768], path[32768];
    int good = 1; ocr_resident = 1;
    for (;;) {
        u32 header[4];
        int received = server_read(input,header,sizeof(header));
        if (received != 1) { if (received < 0) good = 0; break; }
        if (!header[0] || header[0] >= 32768 || !header[1] || header[1] >= 32700 || header[2] > 2 || !header[3] || header[3] > 256 ||
            server_read(input,image,header[0]*2) != 1 || server_read(input,capture,header[1]*2) != 1) { good = 0; break; }
        for (u32 index = 0; index < header[0]; ++index) if (!image[index]) good = 0;
        for (u32 index = 0; index < header[1]; ++index) if (!capture[index]) good = 0;
        if (!good) break;
        image[header[0]] = 0; capture[header[1]] = 0;
        if (!prefill_path(capture,"stdout.txt",path)) { good = 0; break; }
        void *output = CreateFileW(path,0x40000000U,1,0,1,0x80,0);
        if (!prefill_path(capture,"execution.log",path)) { if (output != (void *)~0ULL) CloseHandle(output); good = 0; break; }
        void *error = CreateFileW(path,0x40000000U,1,0,1,0x80,0);
        int result = 0;
        if (output != (void *)~0ULL && error != (void *)~0ULL && SetStdHandle((u32)-11,output) && SetStdHandle((u32)-12,error))
            result = ocr_generate_run(library,vision,decoder,head,image,capture,header[2],header[3]);
        if (!SetStdHandle((u32)-11,original_output) || !SetStdHandle((u32)-12,original_error)) good = 0;
        if (output != (void *)~0ULL && !CloseHandle(output)) good = 0;
        if (error != (void *)~0ULL && !CloseHandle(error)) good = 0;
        u32 code = !good || !result ? 1 : result == 1 ? 0 : 3, written = 0;
        if (!prefill_path(capture,"done.u32",path)) { good = 0; break; }
        void *done = CreateFileW(path,0x40000000U,0,0,1,0x80,0);
        if (done == (void *)~0ULL) { good = 0; break; }
        if (!WriteFile(done,&code,sizeof(code),&written,0) || written != sizeof(code)) good = 0;
        if (!CloseHandle(done)) good = 0;
        if (!result || !good) { good = 0; break; }
    }
    diagnostic_stream = (u32)-12;
#ifdef OCR_GRAPH_CACHE
    if (!graph_resident_release(ocr_resident_api)) good = 0;
#endif
    if (!generation_release(ocr_resident_api)) good = 0;
    if (ocr_resident_api) {
        if (execution_profile && !checked("server_profile_free",ocr_resident_api->profile_free(execution_profile))) good = 0;
        if (!checked("server_device_free",ocr_resident_api->device_free(ocr_resident_device)) ||
            !checked("server_backend_free",ocr_resident_api->backend_free(ocr_resident_backend))) good = 0;
        if (!FreeLibrary(ocr_resident_module)) good = 0;
    }
    ocr_resident = 0; ocr_resident_module = 0; ocr_resident_api = 0; ocr_resident_backend = 0; ocr_resident_device = 0; execution_profile = 0;
    if (!resident_files_release()) good = 0;
    return good;
}