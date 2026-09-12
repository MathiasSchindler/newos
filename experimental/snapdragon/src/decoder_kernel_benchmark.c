#include "../../../src/shared/concurrency.h"

typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long long usize;
typedef float f32x4 __attribute__((vector_size(16)));
typedef _Float16 f16x4 __attribute__((vector_size(8)));

enum {
    MAX_WIDTH = 768,
    MAX_ROWS = 3072,
    WARMUPS = 2,
    SAMPLES = 10
};

__declspec(dllimport) void ExitProcess(u32 status);
__declspec(dllimport) void *GetStdHandle(u32 handle_id);
__declspec(dllimport) int QueryPerformanceCounter(long long *value);
__declspec(dllimport) int QueryPerformanceFrequency(long long *value);
__declspec(dllimport) void *VirtualAlloc(
    void *address, usize size, u32 allocation_type, u32 protect
);
__declspec(dllimport) int VirtualFree(void *address, usize size, u32 free_type);
__declspec(dllimport) int WriteFile(
    void *handle, const void *buffer, u32 size, u32 *written, void *overlapped
);

typedef float (*DotProduct)(const u16 *left, const float *right, u32 count);

typedef struct KernelContext {
    const u16 *matrix;
    const float *input;
    float *output;
    u32 columns;
    DotProduct dot;
} KernelContext;

static void *output_handle;

static void write_bytes(const char *data, u32 size) {
    while (size != 0U) {
        u32 written = 0U;
        if (!WriteFile(output_handle, data, size, &written, 0) || written == 0U) {
            return;
        }
        data += written;
        size -= written;
    }
}

static void write_text(const char *text) {
    const char *end = text;
    while (*end != '\0') ++end;
    write_bytes(text, (u32)(end - text));
}

static void write_u64(u64 value) {
    char digits[20];
    u32 count = 0U;
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) write_bytes(&digits[--count], 1U);
}

static float dot_1(const u16 *left, const float *right, u32 count) {
    f32x4 sum = {0.0f, 0.0f, 0.0f, 0.0f};
    u32 index;
    for (index = 0U; index < count; index += 4U) {
        sum += __builtin_convertvector(*(const f16x4 *)(left + index), f32x4) *
            *(const f32x4 *)(right + index);
    }
    return sum[0] + sum[1] + sum[2] + sum[3];
}

static float dot_2(const u16 *left, const float *right, u32 count) {
    f32x4 sums[2] = {{0.0f}, {0.0f}};
    u32 index;
    for (index = 0U; index < count; index += 8U) {
        sums[0] += __builtin_convertvector(
            *(const f16x4 *)(left + index), f32x4
        ) * *(const f32x4 *)(right + index);
        sums[1] += __builtin_convertvector(
            *(const f16x4 *)(left + index + 4U), f32x4
        ) * *(const f32x4 *)(right + index + 4U);
    }
    sums[0] += sums[1];
    return sums[0][0] + sums[0][1] + sums[0][2] + sums[0][3];
}

static float dot_4(const u16 *left, const float *right, u32 count) {
    f32x4 sums[4] = {{0.0f}, {0.0f}, {0.0f}, {0.0f}};
    u32 index;
    for (index = 0U; index < count; index += 16U) {
        u32 lane;
        for (lane = 0U; lane < 4U; ++lane) {
            sums[lane] += __builtin_convertvector(
                *(const f16x4 *)(left + index + lane * 4U), f32x4
            ) * *(const f32x4 *)(right + index + lane * 4U);
        }
    }
    sums[0] += sums[1];
    sums[2] += sums[3];
    sums[0] += sums[2];
    return sums[0][0] + sums[0][1] + sums[0][2] + sums[0][3];
}

static float dot_8(const u16 *left, const float *right, u32 count) {
    f32x4 sums[8] = {{0.0f}, {0.0f}, {0.0f}, {0.0f},
                    {0.0f}, {0.0f}, {0.0f}, {0.0f}};
    u32 index;
    for (index = 0U; index < count; index += 32U) {
        u32 lane;
        for (lane = 0U; lane < 8U; ++lane) {
            sums[lane] += __builtin_convertvector(
                *(const f16x4 *)(left + index + lane * 4U), f32x4
            ) * *(const f32x4 *)(right + index + lane * 4U);
        }
    }
    sums[0] += sums[1];
    sums[2] += sums[3];
    sums[4] += sums[5];
    sums[6] += sums[7];
    sums[0] += sums[2];
    sums[4] += sums[6];
    sums[0] += sums[4];
    return sums[0][0] + sums[0][1] + sums[0][2] + sums[0][3];
}

static int matrix_range(
    size_t begin,
    size_t end,
    unsigned int worker_index,
    void *arg
) {
    KernelContext *context = (KernelContext *)arg;
    size_t row;
    (void)worker_index;
    for (row = begin; row < end; ++row) {
        context->output[row] = context->dot(
            context->matrix + row * context->columns,
            context->input,
            context->columns
        );
    }
    return 0;
}

static void report(
    const char *kind,
    u32 width,
    u32 rows,
    u32 accumulators,
    u32 workers,
    u64 ticks,
    u64 frequency,
    const float *output
) {
    union { float value; u32 bits; } checksum;
    u32 row;
    checksum.value = 0.0f;
    for (row = 0U; row < rows; ++row) checksum.value += output[row];
    write_text(kind);
    write_text(",");
    write_u64(width);
    write_text(",");
    write_u64(rows);
    write_text(",");
    write_u64(accumulators);
    write_text(",");
    write_u64(workers);
    write_text(",");
    write_u64(ticks * 1000000ULL / frequency);
    write_text(",");
    write_u64(checksum.bits);
    write_text("\n");
}

static u64 run_samples(
    RtTaskPool *pool,
    KernelContext *context,
    u32 rows
) {
    long long start;
    long long end;
    u64 total = 0U;
    u32 sample;
    for (sample = 0U; sample < WARMUPS; ++sample) {
        (void)rt_parallel_for(pool, rows, 1U, matrix_range, context);
    }
    for (sample = 0U; sample < SAMPLES; ++sample) {
        QueryPerformanceCounter(&start);
        (void)rt_parallel_for(pool, rows, 1U, matrix_range, context);
        QueryPerformanceCounter(&end);
        total += (u64)(end - start);
    }
    return total / SAMPLES;
}

void mainCRTStartup(void) {
    static const u32 widths[] = {384U, 512U, 768U};
    static const u32 worker_counts[] = {1U, 4U, 8U, 12U};
    static const DotProduct dots[] = {dot_1, dot_2, dot_4, dot_8};
    u16 *matrix;
    float *input;
    float *output;
    KernelContext context;
    long long frequency;
    u32 index;
    output_handle = GetStdHandle(0xfffffff5U);
    matrix = VirtualAlloc(
        0, (usize)MAX_ROWS * MAX_WIDTH * sizeof(u16), 0x3000U, 0x04U
    );
    input = VirtualAlloc(0, MAX_WIDTH * sizeof(float), 0x3000U, 0x04U);
    output = VirtualAlloc(0, MAX_ROWS * sizeof(float), 0x3000U, 0x04U);
    if (matrix == 0 || input == 0 || output == 0 ||
        !QueryPerformanceFrequency(&frequency) || frequency <= 0) {
        ExitProcess(1U);
    }
    for (index = 0U; index < MAX_ROWS * MAX_WIDTH; ++index) {
        ((_Float16 *)matrix)[index] = (_Float16)((int)(index % 17U) - 8) / 32.0f;
    }
    for (index = 0U; index < MAX_WIDTH; ++index) {
        input[index] = ((int)(index % 13U) - 6) / 16.0f;
    }
    context.matrix = matrix;
    context.input = input;
    context.output = output;
    write_text("kind,width,rows,accumulators,workers,microseconds,checksum\n");
    for (index = 0U; index < sizeof(widths) / sizeof(widths[0]); ++index) {
        RtTaskPool pool;
        u32 dot_index;
        u32 width = widths[index];
        u32 rows = width * 4U;
        context.columns = width;
        (void)rt_task_pool_init(&pool, 1U);
        for (dot_index = 0U; dot_index < sizeof(dots) / sizeof(dots[0]);
             ++dot_index) {
            u64 ticks;
            context.dot = dots[dot_index];
            ticks = run_samples(&pool, &context, rows);
            report(
                "tile", width, rows, 1U << dot_index, 1U,
                ticks, (u64)frequency, output
            );
        }
        rt_task_pool_destroy(&pool);
    }
    context.dot = dot_4;
    for (index = 0U; index < sizeof(widths) / sizeof(widths[0]); ++index) {
        u32 worker_index;
        u32 width = widths[index];
        u32 rows = width * 4U;
        context.columns = width;
        for (worker_index = 0U;
             worker_index < sizeof(worker_counts) / sizeof(worker_counts[0]);
             ++worker_index) {
            RtTaskPool pool;
            u64 ticks;
            u32 workers = worker_counts[worker_index];
            (void)rt_task_pool_init(&pool, workers);
            ticks = run_samples(&pool, &context, rows);
            report(
                "workers", width, rows, 4U, rt_task_pool_width(&pool),
                ticks, (u64)frequency, output
            );
            rt_task_pool_destroy(&pool);
        }
    }
    VirtualFree(output, 0U, 0x8000U);
    VirtualFree(input, 0U, 0x8000U);
    VirtualFree(matrix, 0U, 0x8000U);
    ExitProcess(0U);
}