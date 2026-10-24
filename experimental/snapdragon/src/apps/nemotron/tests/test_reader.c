#include "nemotron_windows.h"
#include "crypto/sha256.h"

static u8 file[8192], saved[8192];
static u64 bytes;
static u32 passed, failed;
static void check(int condition, const char *label) {
    if (condition) ++passed;
    else { ++failed; nm_text("FAIL "); nm_text(label); nm_text("\n"); }
}
static void seal(u32 kind, u32 tensors, u32 nodes, u32 steps, u64 payload) {
    bytes = payload + NM_HEADER_BYTES;
    nm_header(file, kind, 32, tensors, nodes, steps, file + NM_HEADER_BYTES, payload);
}
static void rehash(void) {
    crypto_sha256_hash(file + NM_HEADER_BYTES, (size_t)nm_u64(file + 64), file + 136);
}
static void desc(u8 *t, const char *name, u32 type, u32 id, u32 graph) {
    memset(t, 0, NM_TENSOR_BYTES);
    memcpy(t, name, rt_strlen(name)); nm_put32(t + 64, type);
    nm_put32(t + 68, QNN_DATATYPE_FLOAT_32); nm_put32(t + 72, 1);
    nm_put32(t + 76, id); nm_put32(t + 80, 1); nm_put32(t + 112, graph);
}
static void graph(void) {
    u8 *p = file + NM_HEADER_BYTES, *n;
    memset(file, 0, sizeof(file)); memcpy(p, "reader_graph", 12);
    desc(p + 64, "input", QNN_TENSOR_TYPE_APP_WRITE, 1, 0);
    desc(p + 224, "output", QNN_TENSOR_TYPE_APP_READ, 2, 0);
    n = p + 384; memcpy(n, "identity", 8); memcpy(n + 64, "Identity", 8);
    nm_put32(n + 128, 1); nm_put32(n + 132, 1); nm_put32(n + 208, 1);
    seal(NM_GRAPH, 2, 1, 0, 1056);
}
static void fixture(void) {
    u8 *p = file + NM_HEADER_BYTES; u32 g, s, i; u64 cursor = 1440;
    memset(file, 0, sizeof(file)); memcpy(p, "pipeline_case", 13);
    for (g = 0; g < 3; ++g) {
        desc(p + 64 + g * 320, "input", QNN_TENSOR_TYPE_APP_WRITE, 1, g);
        desc(p + 224 + g * 320, "output", QNN_TENSOR_TYPE_APP_READ, 2, g);
    }
    for (s = 0; s < 2; ++s) {
        u8 *step = p + 1024 + s * 208;
        nm_put32(step, s ? NM_FINAL : NM_FIRST); nm_put32(step + 4, 320); nm_put32(step + 8, 2);
        for (i = 0; i < 6; ++i) {
            u8 *cell = step + 16 + i * 32;
            int active = i / 2 == (s ? 1U : 0U) || i / 2 == 2;
            if (!active) nm_put32(cell, NM_ZERO);
            else if (i == 4 || (s && i == 2)) {
                nm_put32(cell, NM_CARRY);
                nm_put32(cell + 4, s && i == 4 ? 5 : 1);
                nm_put32(cell + 24, s ? 1 : 0);
            } else {
                nm_put64(cell + 8, cursor); nm_put64(cell + 16, 4);
                nm_put32(p + cursor, 0x3f800000); cursor += 16;
            }
        }
    }
    seal(NM_FIXTURE, 6, 0, 2, cursor);
}
static void depthwise(void) {
    u8 *p = file + NM_HEADER_BYTES, *n = p + 864; u32 i;
    memset(file, 0, sizeof(file)); memcpy(p, "reader_depthwise", 16);
    desc(p + 64, "input", QNN_TENSOR_TYPE_APP_WRITE, 1, 0);
    desc(p + 224, "output", QNN_TENSOR_TYPE_APP_READ, 2, 0);
    desc(p + 384, "filter", QNN_TENSOR_TYPE_STATIC, 3, 0);
    desc(p + 544, "stride", QNN_TENSOR_TYPE_STATIC, 4, 0);
    desc(p + 704, "dilation", QNN_TENSOR_TYPE_STATIC, 5, 0);
    for (i = 0; i < 3; ++i) {
        u8 *t = p + 64 + i * NM_TENSOR_BYTES;
        nm_put32(t + 72, 4); nm_put32(t + 84, i == 2 ? 1 : 4);
        nm_put32(t + 88, 1); nm_put32(t + 92, 2);
    }
    for (i = 2; i < 5; ++i) {
        u8 *t = p + 64 + i * NM_TENSOR_BYTES;
        u64 offset = 1536 + (i - 2) * 16;
        nm_put64(t + 96, offset); nm_put64(t + 104, 8);
        if (i > 2) {
            nm_put32(t + 68, QNN_DATATYPE_UINT_32); nm_put32(t + 80, 2);
            nm_put32(p + offset, 1); nm_put32(p + offset + 4, 1);
        }
    }
    memcpy(n, "depthwise", 9); memcpy(n + 64, "DepthWiseConv2d", 15);
    nm_put32(n + 128, 2); nm_put32(n + 132, 1); nm_put32(n + 136, 2);
    nm_put32(n + 148, 2); nm_put32(n + 208, 1);
    for (i = 0; i < 2; ++i) {
        u8 *param = n + 272 + i * NM_PARAM_BYTES;
        const char *name = i ? "dilation" : "stride";
        memcpy(param, name, rt_strlen(name)); nm_put32(param + 32, QNN_PARAMTYPE_TENSOR);
        nm_put32(param + 36, QNN_DATATYPE_UINT_32); nm_put32(param + 40, 3 + i);
    }
    seal(NM_GRAPH, 5, 1, 0, 1584);
}
static int valid(void) {
    NmArtifact a; int ok = nm_validate(&a, file, bytes);
    if (!ok) { nm_text("unexpected reader error: "); nm_text(a.error); nm_text("\n"); }
    return ok;
}
static void reject(const char *label, const char *error) {
    NmArtifact a;
    check(!nm_validate(&a, file, bytes) && a.error && (!error || !rt_strcmp(a.error, error)), label);
}
static void save(void) { memcpy(saved, file, sizeof(file)); }
static void reset(void) { memcpy(file, saved, sizeof(file)); }
static void quantize(u8 *t, u32 dtype, float scale, i32 offset) {
    nm_put32(t + 68, dtype); nm_put32(t + 116, NM_QUANT_SCALE_OFFSET);
    memcpy(t + 120, &scale, 4); nm_put32(t + 124, (u32)offset);
}
static void quant_fixture(u32 dtype) {
    u8 *p = file + NM_HEADER_BYTES; u32 i;
    fixture();
    for (i = 0; i < 6; ++i)
        quantize(p + 64 + i * NM_TENSOR_BYTES, dtype, 0.5F,
                 dtype == QNN_DATATYPE_UFIXED_POINT_16 ? -100 : 0);
    nm_put64(p + 1024 + 16 + 16, dtype == QNN_DATATYPE_SFIXED_POINT_32 ? 4 : 2);
    nm_put32(p + 1440, dtype == QNN_DATATYPE_UFIXED_POINT_16 ? 102 : 2);
    seal(NM_FIXTURE, 6, 0, 2, bytes - NM_HEADER_BYTES);
}
static void quant_tests(void) {
    static const u32 dtypes[] = {QNN_DATATYPE_UFIXED_POINT_16,
        QNN_DATATYPE_SFIXED_POINT_16, QNN_DATATYPE_SFIXED_POINT_32};
    u8 *p = file + NM_HEADER_BYTES; u32 i, j;
    graph(); check(nm_u32(file + 8) == NM_VERSION, "float header stays v1");
    nm_put32(file + 8, NM_QUANT_VERSION); check(valid(), "v2 nonquantized graph");
    p[64 + 120] = 1; rehash(); reject("v2 nonquantized stray scale", 0);
    graph(); nm_put32(file + 8, NM_QUANT_VERSION);
    nm_put32(p + 64 + 116, NM_QUANT_SCALE_OFFSET); rehash();
    reject("v2 quantization on float dtype", 0);
    for (i = 0; i < 3; ++i) {
        u32 dtype = dtypes[i], w = dtype == QNN_DATATYPE_SFIXED_POINT_32 ? 4 : 2;
        u8 t[NM_TENSOR_BYTES], raw[16], expected[16], metric[NM_RESULT_STEP_BYTES];
        graph(); quantize(p + 64, dtype, 0.5F, 0);
        seal(NM_GRAPH, 2, 1, 0, bytes - NM_HEADER_BYTES);
        check(nm_u32(file + 8) == NM_QUANT_VERSION && nm_u32(file + 48) == 32 && valid(),
              "quant graph auto v2 with precision32 header");
        check(nm_tensor_bytes(p + 64) == w && nm_expected_bytes(p + 64) == 4,
              "quant native and expected widths"); save();
        nm_put32(file + 8, NM_VERSION); reject("v1 rejects quant tensors", 0); reset();
        memset(p + 64 + 116, 0, 12); rehash(); reject("v2 quant dtype missing encoding", 0); reset();
        nm_put32(p + 64 + 116, 2); rehash(); reject("v2 unknown quant encoding", 0); reset();
        nm_put32(p + 64 + 120, 0); rehash(); reject("v2 zero scale", 0); reset();
        nm_put32(p + 64 + 120, 0x80000000U); rehash(); reject("v2 negative zero scale", 0); reset();
        nm_put32(p + 64 + 120, 0xbf800000U); rehash(); reject("v2 negative scale", 0); reset();
        nm_put32(p + 64 + 120, 0x7f800000U); rehash(); reject("v2 infinite scale", 0); reset();
        nm_put32(p + 64 + 120, 0xff800000U); rehash(); reject("v2 negative infinite scale", 0); reset();
        nm_put32(p + 64 + 120, 0x7fc00000U); rehash(); reject("v2 NaN scale", 0); reset();
        nm_put32(p + 64 + 120, 1); rehash(); check(valid(), "v2 positive subnormal scale"); reset();
        nm_put32(p + 64 + 124, 1); rehash(); reject("v2 positive offset", 0); reset();
        nm_put32(p + 64 + 124, (u32)-65536); rehash(); reject("v2 out of range negative offset", 0); reset();
        if (dtype != QNN_DATATYPE_UFIXED_POINT_16) {
            nm_put32(p + 64 + 124, 0xffffffffU); rehash(); reject("v2 signed nonzero offset", 0); reset();
        } else {
            nm_put32(p + 64 + 124, (u32)-65535); rehash(); check(valid(), "v2 unsigned minimum offset"); reset();
        }
        p[64 + 128] = 1; rehash(); reject("v2 quant reserved head", 0); reset();
        p[64 + 159] = 1; rehash(); reject("v2 quant reserved tail", 0);
        quant_fixture(dtype); check(valid(), "quantized exact carry pipeline"); save();
        nm_put32(p + 64 + 4 * NM_TENSOR_BYTES + 120, 0x3f000001U); rehash();
        reject("carry scale mismatch", "CARRY source type/shape/dtype mismatch"); reset();
        if (dtype == QNN_DATATYPE_UFIXED_POINT_16) {
            nm_put32(p + 64 + 4 * NM_TENSOR_BYTES + 124, (u32)-99); rehash();
            reject("carry offset mismatch", "CARRY source type/shape/dtype mismatch"); reset();
        }
        quantize(p + 64 + 4 * NM_TENSOR_BYTES,
                 dtype == QNN_DATATYPE_SFIXED_POINT_16 ? QNN_DATATYPE_SFIXED_POINT_32 :
                 QNN_DATATYPE_SFIXED_POINT_16, 0.5F, 0);
        rehash(); reject("carry quantized dtype mismatch", "CARRY source type/shape/dtype mismatch"); reset();
        desc(p + 64 + 4 * NM_TENSOR_BYTES, "input", QNN_TENSOR_TYPE_APP_WRITE, 1, 2);
        rehash(); reject("carry quantized to float forbidden", "CARRY source type/shape/dtype mismatch"); reset();
        desc(p + 64, "input", QNN_TENSOR_TYPE_APP_WRITE, 1, 0);
        nm_put64(p + 1024 + 16 + 16, 4); nm_put32(p + 1440, 0x3f800000U);
        rehash(); check(valid(), "mixed float input and exact quantized carries"); reset();
        nm_put32(p + 1456, 0x7fc00000U); rehash(); reject("quant expected f32 NaN", "nonfinite fixture data"); reset();
        nm_put64(p + 1024 + 16 + 32 + 16, w == 2 ? 2 : 8); rehash();
        reject("quant expected requires f32 width", 0);
        desc(t, "primitive", QNN_TENSOR_TYPE_APP_WRITE, 1, 0);
        quantize(t, dtype, 0.5F, dtype == QNN_DATATYPE_UFIXED_POINT_16 ? -100 : 0);
        nm_put32(t + 80, 4); memset(raw, 0xa5, sizeof(raw)); nm_zero(t, raw);
        for (j = 0; j < 4; ++j)
            nm_put32(expected + j * 4, 0);
        memset(metric, 0, sizeof(metric));
        check(!nm_compare(t, raw, expected, metric) && nm_u64(metric + 48) == 4 &&
              !nm_u32(metric + 40), "quant zero fill dequantizes exactly to zero");
        check(raw[0] == (dtype == QNN_DATATYPE_UFIXED_POINT_16 ? 100 : 0) &&
              raw[1] == 0 && (w == 4 || raw[8] == 0xa5), "quant zero fill raw bytes and bounds");
        quantize(t, dtype, 0.5F, dtype == QNN_DATATYPE_UFIXED_POINT_16 ? -32768 : 0);
        if (dtype == QNN_DATATYPE_UFIXED_POINT_16) {
            raw[0] = 0; raw[1] = 0; raw[2] = 0xff; raw[3] = 0xff;
            nm_put32(expected, 0xc6800000U); nm_put32(expected + 4, 0x467ffe00U);
        } else if (dtype == QNN_DATATYPE_SFIXED_POINT_16) {
            raw[0] = 0; raw[1] = 0x80; raw[2] = 0xff; raw[3] = 0x7f;
            nm_put32(expected, 0xc6800000U); nm_put32(expected + 4, 0x467ffe00U);
        } else {
            nm_put32(raw, 0x80000000U); nm_put32(raw + 4, 0x7fffffffU);
            nm_put32(expected, 0xce800000U); nm_put32(expected + 4, 0x4e800000U);
        }
        nm_put32(t + 80, 2); memset(metric, 0, sizeof(metric));
        check(!nm_compare(t, raw, expected, metric) && nm_u64(metric + 48) == 2,
              "quant compare signed and unsigned extrema");
        check(nm_finite(raw, 2 * w, dtype), "quant raw never interpreted as floats");
        quantize(t, dtype, 0.0009765625F, 0); nm_put32(t + 80, 1);
        memset(raw, 0, sizeof(raw)); raw[0] = 5; nm_put32(expected, 0);
        memset(metric, 0, sizeof(metric));
        check(!nm_compare(t, raw, expected, metric), "quant compare unchanged absolute tolerance pass");
        raw[0] = 6; memset(metric, 0, sizeof(metric));
        check(nm_compare(t, raw, expected, metric) == 1 && nm_u32(metric + 44) == 1 &&
              nm_float(metric + 40, QNN_DATATYPE_FLOAT_32) == 0.005859375F,
              "quant compare unchanged absolute tolerance fail and metric");
        quantize(t, dtype, 0.5F, 0); raw[0] = 201; nm_put32(expected, 0x42c80000U);
        memset(metric, 0, sizeof(metric));
        check(!nm_compare(t, raw, expected, metric), "quant compare unchanged relative tolerance pass");
        raw[0] = 202; memset(metric, 0, sizeof(metric));
        check(nm_compare(t, raw, expected, metric) == 1, "quant compare unchanged relative tolerance fail");
        memset(file, 0, sizeof(file)); memcpy(p, "quant_result", 12);
        desc(p + 64, "output", QNN_TENSOR_TYPE_APP_READ, 1, 0);
        quantize(p + 64, dtype, 0.5F, 0);
        nm_put32(p + 224 + 4, NM_FIRST | NM_FINAL);
        nm_put64(p + 224 + 48, 1);
        nm_put64(p + 288 + 8, 320); nm_put64(p + 288 + 16, w);
        nm_put32(p + 320, 0x7fc0ffffU);
        seal(NM_RESULT, 1, 0, 1, 336);
        check(valid(), "quant result stores raw integer bytes");
    }
    {
        u8 t[NM_TENSOR_BYTES], raw[8];
        desc(t, "zero", QNN_TENSOR_TYPE_APP_WRITE, 1, 0); nm_put32(t + 80, 2);
        memset(raw, 0xff, sizeof(raw)); nm_zero(t, raw);
        check(!nm_u64(raw), "nonquantized zero remains all bits zero");
        quantize(t, QNN_DATATYPE_UFIXED_POINT_16, 0.5F, -65535);
        memset(raw, 0, sizeof(raw)); nm_zero(t, raw);
        check(nm_u32(raw) == 0xffffffffU && !nm_u32(raw + 4), "unsigned maximum zero point fill");
        quantize(t, QNN_DATATYPE_UFIXED_POINT_16, 0.5F, -32768); nm_zero(t, raw);
        check(nm_u32(raw) == 0x80008000U, "unsigned high byte zero point fill");
    }
}
static void benchmark_tests(void) {
    NmArtifact a; u8 *p = file + NM_HEADER_BYTES;
    memset(file, 0, sizeof(file)); memcpy(p, "benchmark_result", 16);
    desc(p + 64, "output", QNN_TENSOR_TYPE_APP_READ, 1, 0);
    nm_put32(p + 224 + 4, NM_FIRST | NM_FINAL);
    nm_put64(p + 224 + 16, 10); nm_put64(p + 224 + 24, 2); nm_put64(p + 224 + 32, 12);
    nm_put32(p + 288, NM_ZERO);
    seal(NM_RESULT, 1, 0, 1, 320);
    check(nm_validate(&a, file, bytes) && a.benchmark, "timing-only result identified without header flags"); save();
    nm_put32(p + 224 + 44, 1); rehash(); reject("benchmark violations forbidden", 0); reset();
    nm_put32(p + 224 + 40, 0x3f800000U); rehash(); reject("benchmark error metric forbidden", 0); reset();
    nm_put64(p + 224 + 48, 1); rehash(); reject("correctness result cannot omit output", "active output missing"); reset();
    nm_put32(p + 288, NM_DATA); nm_put64(p + 288 + 8, 320); nm_put64(p + 288 + 16, 4);
    seal(NM_RESULT, 1, 0, 1, 336); reject("benchmark cannot contain output data", "benchmark result contains tensor data");
    reset(); bytes = NM_HEADER_BYTES + 320;
    file[232] = 1; reject("benchmark reserved header remains forbidden", 0); reset();
    nm_put32(p + 224 + 4, NM_FIRST);
    nm_put32(p + 320, 1); nm_put32(p + 320 + 4, NM_FINAL); nm_put64(p + 320 + 48, 1);
    nm_put64(p + 384 + 8, 416); nm_put64(p + 384 + 16, 4);
    seal(NM_RESULT, 1, 0, 2, 432);
    reject("mixed timing and correctness result forbidden", "mixed benchmark/correctness result steps");
    memset(p + 320 + 48, 0, 8); memset(p + 384, 0, NM_CELL_BYTES); nm_put32(p + 384, NM_ZERO);
    rehash(); check(nm_validate(&a, file, bytes) && a.benchmark, "multi-step benchmark result");
    quantize(p + 64, QNN_DATATYPE_UFIXED_POINT_16, 0.5F, -100);
    seal(NM_RESULT, 1, 0, 2, 432);
    check(nm_validate(&a, file, bytes) && a.benchmark && nm_u32(file + 8) == NM_QUANT_VERSION,
          "quantized benchmark result has no raw output snapshots");
}
static void axis_graph(u32 dtype, u32 axis, int table_first) {
    u8 *p = file + NM_HEADER_BYTES, *t = p + 384, *n = p + 544;
    u64 raw, table, native; u32 i, count = axis ? 3 : 2;
    graph();
    memset(p + 384, 0, NM_TENSOR_BYTES + NM_NODE_BYTES);
    desc(t, "weights", QNN_TENSOR_TYPE_STATIC, 3, 0);
    nm_put32(t + 68, dtype); nm_put32(t + 72, 2);
    nm_put32(t + 80, 2); nm_put32(t + 84, 3);
    nm_put32(t + 116, NM_QUANT_AXIS_SCALE_OFFSET);
    nm_put32(t + 120, axis); nm_put32(t + 124, count);
    native = nm_tensor_bytes(t);
    table = table_first ? 1216 : nm_align16(1216 + native);
    raw = table_first ? nm_align16(table + (u64)count * 8) : 1216;
    nm_put64(t + 96, raw); nm_put64(t + 104, native); nm_put64(t + 128, table);
    for (i = 0; i < count; ++i) nm_put32(p + table + i * 8, i ? 0x3e800000U : 0x3f000000U);
    memset(p + raw, 0xff, (size_t)native);
    memcpy(n, "identity", 8); memcpy(n + 64, "Identity", 8);
    nm_put32(n + 128, 1); nm_put32(n + 132, 1); nm_put32(n + 208, 1);
    seal(NM_GRAPH, 3, 1, 0, nm_align16(table_first ? raw + native : table + (u64)count * 8));
}
static void axis_tests(void) {
    u8 *p = file + NM_HEADER_BYTES, *t = p + 384; u32 dtype, axis, first;
    u64 table;
    for (dtype = 0; dtype < 2; ++dtype) for (axis = 0; axis < 2; ++axis) for (first = 0; first < 2; ++first) {
        axis_graph(dtype ? QNN_DATATYPE_SFIXED_POINT_32 : QNN_DATATYPE_SFIXED_POINT_16, axis, (int)first);
        check(valid() && nm_u32(file + 8) == NM_QUANT_VERSION && nm_u32(file + 48) == 32,
              "per-axis signed static graph supports either table/raw order");
    }
    axis_graph(QNN_DATATYPE_SFIXED_POINT_16, 0, 1); table = nm_u64(t + 128); save();
    nm_put32(file + 8, NM_VERSION); reject("v1 per-axis forbidden", 0); reset();
    nm_put32(t + 116, 3); rehash(); reject("unknown static quantization encoding", 0); reset();
    nm_put32(t + 68, QNN_DATATYPE_UFIXED_POINT_16); rehash(); reject("unsigned per-axis forbidden", 0); reset();
    nm_put32(t + 68, QNN_DATATYPE_FLOAT_32); rehash(); reject("float per-axis forbidden", 0); reset();
    nm_put32(t + 64, QNN_TENSOR_TYPE_APP_WRITE); rehash(); reject("input per-axis forbidden", 0); reset();
    nm_put32(t + 64, QNN_TENSOR_TYPE_APP_READ); rehash(); reject("output per-axis forbidden", 0); reset();
    nm_put32(t + 64, QNN_TENSOR_TYPE_NATIVE); rehash(); reject("native nonstatic per-axis forbidden", 0); reset();
    nm_put32(t + 120, 2); rehash(); reject("axis equals rank", 0); reset();
    nm_put32(t + 120, 0xffffffffU); rehash(); reject("negative axis forbidden", 0); reset();
    nm_put32(t + 124, 0); rehash(); reject("zero scale count", 0); reset();
    nm_put32(t + 124, 1); rehash(); reject("short scale count", 0); reset();
    nm_put32(t + 124, 3); rehash(); reject("long scale count", 0); reset();
    nm_put32(t + 124, 0xffffffffU); rehash(); reject("scale count overflow", 0); reset();
    nm_put64(t + 128, 0); rehash(); reject("missing scale table", 0); reset();
    nm_put64(t + 128, 64); rehash(); reject("scale table aliases metadata", 0); reset();
    nm_put64(t + 128, table + 1); rehash(); reject("scale table alignment", 0); reset();
    nm_put64(t + 128, ~(u64)0 - 15); rehash(); reject("scale table offset wrap", 0); reset();
    nm_put64(t + 128, bytes - NM_HEADER_BYTES); rehash(); reject("scale table outside payload", 0); reset();
    nm_put64(t + 96, table); rehash(); reject("scale table overlaps raw weights", "overlapping data blobs"); reset();
    nm_put32(p + table, 0x7fc00000U); rehash(); reject("NaN axis scale", "invalid quantization scale"); reset();
    nm_put32(p + table + 8, 0x7f800000U); rehash(); reject("infinite final axis scale", "invalid quantization scale"); reset();
    nm_put32(p + table + 8, 0); rehash(); reject("zero final axis scale", "invalid quantization scale"); reset();
    nm_put32(p + table, 0xbf800000U); rehash(); reject("negative axis scale", "invalid quantization scale"); reset();
    nm_put32(p + table + 4, 1); rehash(); reject("positive axis offset", "invalid quantization offset"); reset();
    nm_put32(p + table + 12, 0xffffffffU); rehash(); reject("negative final axis offset", "invalid quantization offset"); reset();
    nm_put32(p + table, 1); rehash(); check(valid(), "positive subnormal axis scale"); reset();
    p[table] ^= 1; reject("scale table protected by payload checksum", "payload SHA256 mismatch"); reset();
    t[136] = 1; rehash(); reject("per-axis reserved head", 0); reset();
    t[159] = 1; rehash(); reject("per-axis reserved tail", 0); reset();
    axis_graph(QNN_DATATYPE_SFIXED_POINT_32, 1, 1);
    nm_put64(t + 128, bytes - NM_HEADER_BYTES - 16); rehash();
    reject("truncated three-entry axis table", "quantization table range/size/alignment");
}
static int smoke(const char *graph_path, const char *fixture_path, int half, int quant) {
    u8 *p = file + NM_HEADER_BYTES, *n; int ok;
    memset(file, 0, sizeof(file)); memcpy(p, "reader_add", 10);
    desc(p + 64, "input", QNN_TENSOR_TYPE_APP_WRITE, 1, 0);
    desc(p + 224, "output", QNN_TENSOR_TYPE_APP_READ, 2, 0);
    desc(p + 384, "bias", QNN_TENSOR_TYPE_STATIC, 3, 0);
    if (half) {
        nm_put32(p + 64 + 68, QNN_DATATYPE_FLOAT_16);
        nm_put32(p + 224 + 68, QNN_DATATYPE_FLOAT_16);
        nm_put32(p + 384 + 68, QNN_DATATYPE_FLOAT_16);
    }
    if (quant) {
        quantize(p + 64, QNN_DATATYPE_UFIXED_POINT_16, 0.000244140625F, -32768);
        quantize(p + 224, QNN_DATATYPE_UFIXED_POINT_16, 0.000244140625F, -32768);
        quantize(p + 384, QNN_DATATYPE_UFIXED_POINT_16, 0.000244140625F, -32768);
    }
    nm_put64(p + 384 + 96, 1216); nm_put64(p + 384 + 104, half || quant ? 2 : 4);
    nm_put32(p + 1216, quant ? 40960 : half ? 0x4000 : 0x40000000);
    n = p + 544; memcpy(n, "add", 3); memcpy(n + 64, "ElementWiseAdd", 14);
    nm_put32(n + 128, 2); nm_put32(n + 132, 1);
    nm_put32(n + 148, 2); nm_put32(n + 208, 1);
    seal(NM_GRAPH, 3, 1, 0, 1232);
    if (half) nm_put32(file + 48, 16);
    ok = valid() && nm_write_file(graph_path, file, bytes);
    memset(file, 0, sizeof(file)); memcpy(p, "reader_add_case", 15);
    desc(p + 64, "input", QNN_TENSOR_TYPE_APP_WRITE, 1, 0);
    desc(p + 224, "output", QNN_TENSOR_TYPE_APP_READ, 2, 0);
    if (half) {
        nm_put32(p + 64 + 68, QNN_DATATYPE_FLOAT_16);
        nm_put32(p + 224 + 68, QNN_DATATYPE_FLOAT_16);
    }
    if (quant) {
        quantize(p + 64, QNN_DATATYPE_UFIXED_POINT_16, 0.000244140625F, -32768);
        quantize(p + 224, QNN_DATATYPE_UFIXED_POINT_16, 0.000244140625F, -32768);
    }
    nm_put32(p + 384, NM_FIRST | NM_FINAL); nm_put32(p + 388, 320); nm_put32(p + 392, 1);
    nm_put64(p + 400 + 8, 464); nm_put64(p + 400 + 16, half || quant ? 2 : 4);
    nm_put64(p + 432 + 8, 480); nm_put64(p + 432 + 16, 4);
    nm_put32(p + 464, quant ? 36864 : half ? 0x3c00 : 0x3f800000); nm_put32(p + 480, 0x40400000);
    if (quant == 2) {
        memset(p + 400, 0, NM_CELL_BYTES); nm_put32(p + 400, NM_ZERO);
        nm_put32(p + 480, 0x40000000);
    }
    seal(NM_FIXTURE, 2, 0, 1, 496);
    if (half) nm_put32(file + 48, 16);
    return ok && valid() && nm_write_file(fixture_path, file, bytes);
}
static int mismatch_fixture(const char *source, const char *destination) {
    u64 size = 0; u8 *data = nm_read_file(source, &size); NmArtifact a; u32 i; int ok = 0;
    if (!data || !nm_validate(&a, data, size) || a.kind != NM_FIXTURE) goto done;
    for (i = 0; i < a.tensors; ++i) {
        const u8 *t = nm_tensor(&a, i), *c = nm_cell(&a, 0, i);
        if (nm_u32(t + 64) == QNN_TENSOR_TYPE_APP_READ && nm_u32(c) == NM_DATA &&
            (nm_u32(t + 68) == QNN_DATATYPE_FLOAT_16 || nm_u32(t + 68) == QNN_DATATYPE_FLOAT_32 || nm_quantized(t))) {
            nm_put32(data + NM_HEADER_BYTES + nm_u64(c + 8), 0x40800000);
            nm_header(data, NM_FIXTURE, a.precision, a.tensors, 0, a.steps, data + NM_HEADER_BYTES, size - NM_HEADER_BYTES);
            ok = nm_validate(&a, data, size) && nm_write_file(destination, data, size); break;
        }
    }
done:
    nm_free(data, size); return ok;
}
void mainCRTStartup(void) {
    NmArtifact a; u8 *p = file + NM_HEADER_BYTES;
    static char arguments[8][4096];
    int argc = nm_arguments(arguments);
    if (argc == 4 && !rt_strcmp(arguments[1], "--mismatch-fixture")) {
        ExitProcess(mismatch_fixture(arguments[2], arguments[3]) ? 0 : 1); return;
    }
    if (argc == 4 && (!rt_strcmp(arguments[1], "--emit-smoke") || !rt_strcmp(arguments[1], "--emit-smoke-half") ||
                      !rt_strcmp(arguments[1], "--emit-smoke-quant") ||
                      !rt_strcmp(arguments[1], "--emit-smoke-quant-zero"))) {
        ExitProcess(smoke(arguments[2], arguments[3], !rt_strcmp(arguments[1], "--emit-smoke-half"),
                         !rt_strcmp(arguments[1], "--emit-smoke-quant-zero") ? 2 :
                         !rt_strcmp(arguments[1], "--emit-smoke-quant")) ? 0 : 1); return;
    }
    if (argc == 3 && (!rt_strcmp(arguments[1], "--emit-pipeline") || !rt_strcmp(arguments[1], "--emit-pipeline-half") ||
                     !rt_strcmp(arguments[1], "--emit-pipeline-quant"))) {
        u32 i;
        fixture();
        nm_put32(p + 1456, 0x40400000);
        nm_put32(p + 1472, 0x40a00000);
        nm_put32(p + 1488, 0x40a00000);
        nm_put32(p + 1504, 0x40e00000);
        if (!rt_strcmp(arguments[1], "--emit-pipeline-half")) {
            for (i = 0; i < 6; ++i) nm_put32(p + 64 + i * NM_TENSOR_BYTES + 68, QNN_DATATYPE_FLOAT_16);
            nm_put64(p + 1024 + 16 + 16, 2);
            nm_put32(p + 1440, 0x3c00); nm_put32(file + 48, 16);
        }
        if (!rt_strcmp(arguments[1], "--emit-pipeline-quant")) {
            for (i = 0; i < 6; ++i)
                quantize(p + 64 + i * NM_TENSOR_BYTES, QNN_DATATYPE_UFIXED_POINT_16, 0.000244140625F, -32768);
            nm_put64(p + 1024 + 16 + 16, 2); nm_put32(p + 1440, 36864);
        }
        seal(NM_FIXTURE, 6, 0, 2, bytes - NM_HEADER_BYTES);
        if (!rt_strcmp(arguments[1], "--emit-pipeline-half")) nm_put32(file + 48, 16);
        ExitProcess(valid() && nm_write_file(arguments[2], file, bytes) ? 0 : 1); return;
    }
    graph(); check(valid(), "valid graph"); save();
    check(!nm_validate(&a, file, 255), "truncated header");
    check(!nm_validate(&a, file, bytes - 1), "truncated payload");
    file[0] ^= 1; reject("magic", "bad magic"); reset();
    nm_put32(file + 8, 3); reject("version", 0); reset();
    nm_put32(file + 24, 38); reject("ABI", 0); reset();
    file[72] ^= 1; reject("model", "model fingerprint mismatch"); reset();
    file[104] ^= 1; reject("contract", "accepted M0 contract fingerprint mismatch"); reset();
    file[168] ^= 1; reject("runtime", "QnnHtp runtime fingerprint mismatch"); reset();
    file[255] = 1; reject("header reserved", "nonzero reserved header"); reset();
    nm_put64(file + 64, NM_MAX_PAYLOAD + 1); reject("payload cap", 0); reset();
    nm_put32(file + 52, NM_MAX_TENSORS + 1); reject("tensor cap", 0); reset();
    p[100] ^= 1; reject("checksum", "payload SHA256 mismatch"); reset();
    memset(p, 'a', 64); rehash(); reject("unterminated name", 0); reset();
    p[12] = 0; p[13] = 1; rehash(); reject("name padding", 0); reset();
    nm_put32(p + 64 + 72, 5); rehash(); reject("rank overflow", 0); reset();
    nm_put32(p + 64 + 80, 0); rehash(); reject("zero dimension", 0); reset();
    nm_put32(p + 64 + 80, 0xffffffffU); rehash(); reject("tensor byte overflow", 0); reset();
    nm_put32(p + 64 + 68, 0x0416); rehash(); reject("unconfigured quantization dtype", 0); reset();
    p[64 + 116] = 1; rehash(); reject("tensor reserved", 0); reset();
    p[64 + 84] = 1; rehash(); reject("unused dims", 0); reset();
    memcpy(p + 224, "input", 6); rehash(); reject("duplicate tensor", 0); reset();
    nm_put32(p + 64 + 96, 1056); rehash(); reject("nonstatic data", 0); reset();
    nm_put32(p + 384 + 144, 2); rehash(); reject("input index", 0); reset();
    nm_put32(p + 384 + 144, 1); rehash(); reject("topological input", 0); reset();
    nm_put32(p + 384 + 208, 0); rehash(); reject("output producer", 0); reset();
    nm_put32(p + 384 + 128, 17); rehash(); reject("node arity", 0); reset();
    p[384 + 656] = 1; rehash(); reject("node tail", 0); reset();
    p[384 + 148] = 1; rehash(); reject("unused input slot", 0); reset();
    nm_put32(p + 384 + 136, 1); memcpy(p + 384 + 272, "epsilon", 7);
    nm_put32(p + 384 + 272 + 36, QNN_DATATYPE_FLOAT_32);
    nm_put32(p + 384 + 272 + 40, 0x7fc00000); rehash(); reject("NaN scalar", 0);
    nm_put32(p + 384 + 272 + 32, 1); nm_put32(p + 384 + 272 + 40, 0);
    rehash(); reject("parameter tensor must be static", 0);
    graph(); nm_put32(file + 12, NM_CONTEXT); nm_put32(file + 56, 0);
    nm_put64(file + 64, 400); bytes = 656;
    memset(p + 384, 0, 16); p[384] = 7; rehash(); check(valid(), "valid opaque context");
    nm_put64(file + 64, 384); bytes = 640; rehash(); reject("empty context binary", 0);
    graph();
    desc(p + 384, "static", QNN_TENSOR_TYPE_STATIC, 3, 0);
    /* A zero-input node emits the output; two distinct static tensors share a blob. */
    memset(p + 544, 0, 832);
    desc(p + 544, "static2", QNN_TENSOR_TYPE_STATIC, 4, 0);
    nm_put64(p + 384 + 96, 1376); nm_put64(p + 384 + 104, 4);
    nm_put64(p + 544 + 96, 1392); nm_put64(p + 544 + 104, 4);
    memcpy(p + 704, "constant", 8); memcpy(p + 768, "Identity", 8);
    nm_put32(p + 704 + 132, 1); nm_put32(p + 704 + 208, 1);
    seal(NM_GRAPH, 4, 1, 0, 1408); check(valid(), "valid aligned static data"); save();
    nm_put64(p + 544 + 96, 1376); rehash(); reject("static overlap", "overlapping data blobs"); reset();
    nm_put64(p + 544 + 96, 1393); rehash(); reject("static alignment", 0); reset();
    nm_put64(p + 544 + 96, ~(u64)0 - 15); rehash(); reject("static offset wrap", 0);
    fixture(); check(valid(), "valid pipeline carry"); save();
    nm_put32(p + 1024 + 16 + 4 * 32 + 24, 1); rehash(); reject("carry previous on step zero", 0); reset();
    nm_put32(p + 1024 + 16 + 4 * 32 + 4, 5); rehash(); reject("carry same graph current", 0); reset();
    nm_put32(p + 1024 + 208 + 16 + 4 * 32 + 4, 3); rehash(); reject("carry inactive previous graph", 0); reset();
    nm_put32(p + 1024 + 16 + 4 * 32 + 4, 0); rehash(); reject("carry input source", 0); reset();
    nm_put32(p + 1024 + 16 + 4 * 32 + 24, 2); rehash(); reject("carry invalid age", 0); reset();
    nm_put32(p + 64 + 4 * 160 + 68, QNN_DATATYPE_INT_32); rehash(); reject("carry dtype", 0); reset();
    nm_put32(p + 1024 + 208, NM_FIRST); rehash(); reject("FIRST later step", 0); reset();
    nm_put32(p + 1024, NM_FIRST | NM_FINAL); rehash(); reject("FINAL before last", 0); reset();
    nm_put32(p + 1024 + 16 + 2 * 32, NM_DATA); rehash(); reject("inactive DATA", 0); reset();
    nm_put32(p + 1024 + 16 + 28, 1); rehash(); reject("cell reserved", 0); reset();
    nm_put64(p + 1024 + 16 + 16, 8); rehash(); reject("cell wrong size", 0); reset();
    nm_put32(p + 1440, 0x7f800000); rehash(); reject("infinite input", 0); reset();
    nm_put32(p + 1456, 0x7fc00000); rehash(); reject("NaN expected", 0); reset();
    nm_put64(p + 1024 + 16 + 32 + 8, 1440); rehash(); reject("cell overlap", "overlapping data blobs"); reset();
    nm_put64(p + 1024 + 16 + 8, 1024); rehash(); reject("cell aliases metadata", 0);
    memset(file, 0, sizeof(file)); memcpy(p, "reader_result", 13);
    desc(p + 64, "input", QNN_TENSOR_TYPE_APP_WRITE, 1, 0);
    desc(p + 224, "output", QNN_TENSOR_TYPE_APP_READ, 2, 0);
    nm_put32(p + 388, NM_FIRST | NM_FINAL); nm_put64(p + 432, 1);
    nm_put32(p + 448, NM_ZERO);
    nm_put64(p + 480 + 8, 512); nm_put64(p + 480 + 16, 4);
    nm_put32(p + 512, 0x40400000); seal(NM_RESULT, 2, 0, 1, 528);
    nm_put64(file + 200, 10000000); nm_put64(file + 208, 100);
    check(valid(), "valid result metrics"); save();
    nm_put32(p + 384, 1); rehash(); reject("result step index", 0); reset();
    nm_put32(p + 384 + 40, 0x7fc00000); rehash(); reject("result nonfinite error metric", 0); reset();
    nm_put64(p + 384 + 56, 1); rehash(); reject("result metric reserved", 0); reset();
    file[232] = 1; reject("result header reserved", 0); reset();
    nm_put32(p + 480, NM_CARRY); rehash(); reject("result carry forbidden", 0); reset();
    nm_put32(p + 512, 0x7fc00000); rehash(); check(valid(), "diagnostic result retains actual NaN"); reset();
    nm_put32(p + 480, NM_ZERO); memset(p + 480 + 8, 0, 16); rehash(); reject("result active output missing", 0);
    { u8 h[] = {0, 0x3c, 0, 0x80, 1, 0, 0, 0x7c};
      check(nm_float(h, QNN_DATATYPE_FLOAT_16) == 1.0F, "half one");
      check(nm_float(h + 2, QNN_DATATYPE_FLOAT_16) == 0.0F, "half signed zero");
      check(nm_float(h + 4, QNN_DATATYPE_FLOAT_16) > 0.0F, "half subnormal");
      check(!nm_finite(h + 6, 2, QNN_DATATYPE_FLOAT_16), "half infinity"); }
    depthwise(); check(valid(), "valid depthwise preflight"); save();
    nm_put32(p + 384 + 88, 2); nm_put32(p + 384 + 92, 1); rehash();
    reject("depthwise transposed filter layout", "DepthWiseConv2d filter must be rank4 [H,W,1,channel_out]");
    check(!nm_validate(&a, file, bytes) && a.error_name && !rt_strcmp(a.error_name, "depthwise"), "depthwise error names node"); reset();
    nm_put32(p + 384 + 72, 3); nm_put32(p + 384 + 88, 2); nm_put32(p + 384 + 92, 0); rehash();
    reject("depthwise filter rank", "DepthWiseConv2d filter must be rank4 [H,W,1,channel_out]"); reset();
    nm_put32(p + 384 + 92, 1); nm_put64(p + 384 + 104, 4); rehash();
    reject("depthwise fractional multiplier", "DepthWiseConv2d filter channel_out must be a multiple of input channels"); reset();
    nm_put32(p + 64 + 72, 3); nm_put32(p + 64 + 88, 2); nm_put32(p + 64 + 92, 0); rehash();
    reject("depthwise input rank", "DepthWiseConv2d input must be rank4 NHWC"); reset();
    nm_put32(p + 1552, 0); rehash();
    reject("depthwise zero stride", "DepthWiseConv2d stride/dilation values must be positive"); reset();
    nm_put32(p + 1572, 0); rehash();
    reject("depthwise zero dilation", "DepthWiseConv2d stride/dilation values must be positive"); reset();
    nm_put32(p + 544 + 68, QNN_DATATYPE_INT_32); nm_put32(p + 864 + 272 + 36, QNN_DATATYPE_INT_32);
    nm_put32(p + 1556, 0xffffffffU); rehash();
    reject("depthwise negative signed stride", "DepthWiseConv2d stride/dilation values must be positive"); reset();
    nm_put32(p + 544 + 80, 1); nm_put64(p + 544 + 104, 4); rehash();
    reject("depthwise stride shape", "DepthWiseConv2d stride/dilation must be a two-element integer tensor"); reset();
    nm_put32(p + 864 + 272 + 32, QNN_PARAMTYPE_SCALAR); rehash();
    reject("depthwise scalar stride", "DepthWiseConv2d stride/dilation must be a two-element integer tensor"); reset();
    nm_put32(p + 384 + 92, 4); nm_put64(p + 384 + 104, 16); rehash();
    check(valid(), "depthwise multiplier two");
    nm_text("Legacy reader checks passed "); nm_number(passed); nm_text("\n");
    quant_tests();
    benchmark_tests();
    axis_tests();
    nm_text("Reader tests passed "); nm_number(passed); nm_text(" failed "); nm_number(failed); nm_text("\n");
    ExitProcess(failed ? 1 : 0);
}
