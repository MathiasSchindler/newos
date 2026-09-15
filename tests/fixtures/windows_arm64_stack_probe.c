typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

typedef struct MemoryInformation {
    void *base;
    void *allocation_base;
    u32 allocation_protection;
    unsigned short partition;
    unsigned short padding;
    u64 region_size;
    u32 state;
    u32 protection;
    u32 type;
} MemoryInformation;

typedef struct ProbeBounds {
    u64 low;
    u64 high;
} ProbeBounds;

_Static_assert(sizeof(MemoryInformation) == 48, "Windows ARM64 memory query ABI");
__declspec(dllimport) void *LoadLibraryA(const char *name);
__declspec(dllimport) void *GetProcAddress(void *module, const char *name);
__declspec(dllimport) void *CreateThread(void *attributes, u64 stack_size,
    u32 (*entry)(void *), void *argument, u32 flags, u32 *thread_id);
__declspec(dllimport) int CloseHandle(void *handle);
__declspec(dllimport) void ExitProcess(u32 status);
extern u32 stack_probe_abi(u64 units, ProbeBounds *bounds);
static u64 (*query_memory)(const void *, MemoryInformation *, u64);
static u32 (*wait_thread)(void *, u32);
static int (*thread_exit_code)(void *, u32 *);

static u32 __attribute__((noinline)) compiler_frame(void) {
    volatile u8 values[131072];
    for (u32 index = 0U; index < sizeof(values); ++index) values[index] = (u8)(index * 17U + 3U);
    for (u32 index = 0U; index < sizeof(values); ++index) {
        if (values[index] != (u8)(index * 17U + 3U)) return 40U;
    }
    return 0U;
}

static u32 test_thread(void *argument) {
    u64 bytes = *(const u64 *)argument;
    ProbeBounds bounds;
    MemoryInformation information;
    volatile u8 marker = 0U;
    if (bytes == ~0ULL) return compiler_frame();
    if (bytes == 131072U) {
        const void *below = (const void *)((u64)&marker - bytes);
        if (!query_memory(below, &information, sizeof(information)) || information.state != 0x2000U) return 10U;
    }
    u32 status = stack_probe_abi(bytes / 16U, &bounds);
    if (status != 0U) return status;
    if (bounds.high - bounds.low != bytes) return 21U;
    for (u64 address = bounds.low; address < bounds.high; address += 4096U) {
        if (!query_memory((const void *)address, &information, sizeof(information)) ||
            information.state != 0x1000U || (information.protection & 0x101U) != 0U) return 30U;
    }
    return 0U;
}

void mainCRTStartup(void) {
    static const u64 sizes[] = {0U, 16U, 4080U, 4096U, 4112U, 8192U, 8208U, 131072U, ~0ULL};
    void *module = LoadLibraryA("kernel32.dll");
    if (!module) ExitProcess(1U);
    query_memory = (u64 (*)(const void *, MemoryInformation *, u64))GetProcAddress(module, "VirtualQuery");
    wait_thread = (u32 (*)(void *, u32))GetProcAddress(module, "WaitForSingleObject");
    thread_exit_code = (int (*)(void *, u32 *))GetProcAddress(module, "GetExitCodeThread");
    if (!query_memory || !wait_thread || !thread_exit_code) ExitProcess(2U);
    for (u32 index = 0U; index < sizeof(sizes) / sizeof(sizes[0]); ++index) {
        void *thread = CreateThread(0, 1048576U, test_thread, (void *)&sizes[index], 0x10000U, 0);
        u32 status = 0U;
        if (!thread) ExitProcess(3U);
        if (wait_thread(thread, 30000U) != 0U || !thread_exit_code(thread, &status)) ExitProcess(4U);
        if (!CloseHandle(thread)) ExitProcess(5U);
        if (status != 0U) ExitProcess(status);
    }
    ExitProcess(0U);
}