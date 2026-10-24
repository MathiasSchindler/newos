#include "nemotron_reader.h"
#include "crypto/sha256.h"
#include "fp32/nm_fp32.h"
#include "nemotron_tokenizer.h"

typedef struct NmRange { u64 start, end; } NmRange;

u32 nm_u32(const u8 *p) {
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
u64 nm_u64(const u8 *p) { return nm_u32(p) | (u64)nm_u32(p + 4) << 32; }
void nm_put32(u8 *p, u32 v) { u32 i; for (i = 0; i < 4; ++i) p[i] = (u8)(v >> (i * 8)); }
void nm_put64(u8 *p, u64 v) { nm_put32(p, (u32)v); nm_put32(p + 4, (u32)(v >> 32)); }
u64 nm_align16(u64 n) { return (n + 15) & ~(u64)15; }
const u8 *nm_tensor(const NmArtifact *a, u32 i) { return a->payload + 64 + (u64)i * NM_TENSOR_BYTES; }
const u8 *nm_step(const NmArtifact *a, u32 s) {
    u64 prefix = a->kind == NM_RESULT ? NM_RESULT_STEP_BYTES : NM_STEP_BYTES;
    return a->payload + 64 + (u64)a->tensors * NM_TENSOR_BYTES +
        (u64)s * (prefix + (u64)a->tensors * NM_CELL_BYTES);
}
const u8 *nm_cell(const NmArtifact *a, u32 s, u32 i) {
    return nm_step(a, s) + (a->kind == NM_RESULT ? NM_RESULT_STEP_BYTES : NM_STEP_BYTES) + (u64)i * NM_CELL_BYTES;
}
static int zero(const u8 *p, u64 n) { while (n--) if (*p++) return 0; return 1; }
static int name(const u8 *p, u32 n) {
    u32 i;
    for (i = 0; i < n; ++i) {
        if (!p[i]) return i && zero(p + i, n - i);
        if (p[i] < 32 || p[i] > 126) return 0;
    }
    return 0;
}
static u32 width(u32 dtype) {
    switch (dtype) {
    case 0x0008: case 0x0108: case QNN_DATATYPE_BOOL_8: return 1;
    case 0x0016: case 0x0116: case QNN_DATATYPE_FLOAT_16:
    case QNN_DATATYPE_UFIXED_POINT_16: case QNN_DATATYPE_SFIXED_POINT_16: return 2;
    case QNN_DATATYPE_INT_32: case QNN_DATATYPE_UINT_32: case QNN_DATATYPE_FLOAT_32:
    case QNN_DATATYPE_SFIXED_POINT_32: return 4;
    case 0x0064: case 0x0164: return 8;
    default: return 0;
    }
}
int nm_quantized(const u8 *t) {
    u32 dtype = nm_u32(t + 68);
    return dtype == QNN_DATATYPE_UFIXED_POINT_16 || dtype == QNN_DATATYPE_SFIXED_POINT_16 ||
        dtype == QNN_DATATYPE_SFIXED_POINT_32;
}
u64 nm_tensor_bytes(const u8 *t) {
    u64 n = width(nm_u32(t + 68)); u32 r = nm_u32(t + 72), i;
    if (!n || !r || r > 4) return 0;
    for (i = 0; i < r; ++i) {
        u32 d = nm_u32(t + 80 + i * 4);
        if (!d || n > 0xffffffffULL / d) return 0;
        n *= d;
    }
    return n;
}
u64 nm_expected_bytes(const u8 *t) {
    u64 n = nm_tensor_bytes(t);
    return nm_quantized(t) || nm_u32(t + 68) == QNN_DATATYPE_FLOAT_16 ?
        n / width(nm_u32(t + 68)) * 4 : n;
}
void nm_zero(const u8 *t, u8 *data) {
    u64 n = nm_tensor_bytes(t), i;
    if (nm_quantized(t) && nm_u32(t + 124)) {
        u32 raw = 0U - nm_u32(t + 124), w = width(nm_u32(t + 68)), j;
        for (i = 0; i < n; i += w)
            for (j = 0; j < w; ++j) data[i + j] = (u8)(raw >> (j * 8));
    } else memset(data, 0, (size_t)n);
}
int nm_same_shape(const u8 *a, const u8 *b) {
    return nm_u32(a + 68) == nm_u32(b + 68) &&
        nm_u32(a + 72) == nm_u32(b + 72) && !memcmp(a + 80, b + 80, 16) &&
        !memcmp(a + 116, b + 116, 12);
}
int nm_active(const NmArtifact *a, u32 s, u32 g) {
    if (!a->pipeline) return g == 0;
    return g == 2 || g == ((nm_u32(nm_step(a, s) + (a->kind == NM_RESULT ? 4 : 0)) & NM_FIRST) ? 0U : 1U);
}
float nm_float(const u8 *p, u32 dtype) {
    union { u32 u; float f; } v;
    if (dtype == QNN_DATATYPE_FLOAT_32) v.u = nm_u32(p);
    else {
        u32 h = p[0] | (u32)p[1] << 8, sign = (h & 0x8000U) << 16;
        u32 exp = (h >> 10) & 31, fraction = h & 1023;
        if (!exp && fraction) {
            exp = 113;
            while (!(fraction & 1024)) { fraction <<= 1; --exp; }
            v.u = sign | exp << 23 | (fraction & 1023) << 13;
        } else v.u = sign | (exp ? (exp == 31 ? 255 : exp + 112) << 23 : 0) | fraction << 13;
    }
    return v.f;
}
int nm_finite(const u8 *p, u64 n, u32 dtype) {
    u64 i;
    if (dtype == QNN_DATATYPE_FLOAT_32) {
        for (i = 0; i < n; i += 4) if ((nm_u32(p + i) & 0x7f800000U) == 0x7f800000U) return 0;
    } else if (dtype == QNN_DATATYPE_FLOAT_16) {
        for (i = 0; i < n; i += 2) if ((p[i + 1] & 0x7cU) == 0x7cU) return 0;
    } else if (dtype == QNN_DATATYPE_BOOL_8) {
        for (i = 0; i < n; ++i) if (p[i] > 1) return 0;
    }
    return 1;
}
u32 nm_compare(const u8 *t, const u8 *actual, const u8 *expected, u8 *metric) {
    u32 dtype = nm_u32(t + 68), w = width(dtype), quant = (u32)nm_quantized(t);
    u64 i, count = nm_tensor_bytes(t) / w, compared = nm_u64(metric + 48);
    u32 violations = nm_u32(metric + 44), before = violations;
    float max = nm_float(metric + 40, QNN_DATATYPE_FLOAT_32);
    if (quant || dtype == QNN_DATATYPE_FLOAT_16 || dtype == QNN_DATATYPE_FLOAT_32) {
        for (i = 0; i < count; ++i) {
            const u8 *raw = actual + i * w;
            double a, e = nm_float(expected + i * 4, QNN_DATATYPE_FLOAT_32), error, magnitude;
            if (quant) {
                double integer;
                if (dtype == QNN_DATATYPE_SFIXED_POINT_32) integer = (i32)nm_u32(raw);
                else {
                    u32 bits = raw[0] | (u32)raw[1] << 8;
                    integer = dtype == QNN_DATATYPE_SFIXED_POINT_16 && (bits & 0x8000U) ?
                        (double)bits - 65536.0 : (double)bits;
                }
                a = (integer + (i32)nm_u32(t + 124)) * nm_float(t + 120, QNN_DATATYPE_FLOAT_32);
            } else a = nm_float(raw, dtype);
            error = a - e; magnitude = e < 0 ? -e : e;
            if (error < 0) error = -error;
            if (!nm_finite(raw, w, dtype) || error > 0.005 + 0.005 * magnitude) {
                if (violations != 0xffffffffU) ++violations;
                if (!nm_finite(raw, w, dtype)) error = 3.402823466e38;
            }
            if (error > max) max = error > 3.402823466e38 ? 3.402823466e38F : (float)error;
        }
    } else {
        for (i = 0; i < count; ++i) if (memcmp(actual + i * w, expected + i * w, w)) {
            if (violations != 0xffffffffU) ++violations;
            if (max < 1) max = 1;
        }
    }
    memcpy(metric + 40, &max, 4); nm_put32(metric + 44, violations); nm_put64(metric + 48, compared + count);
    return violations - before;
}
static u8 hex(char c) { return (u8)(c >= 'a' ? c - 'a' + 10 : c - '0'); }
static void digest(u8 *out, const char *s) {
    u32 i; for (i = 0; i < 32; ++i) out[i] = (u8)(hex(s[i * 2]) * 16 + hex(s[i * 2 + 1]));
}
static int fingerprint(const u8 *p, const char *s) {
    u8 expected[32]; digest(expected, s); return !memcmp(expected, p, 32);
}
const u8 *nm_runtime_entry(const NmArtifact *a, u32 index) {
    return a && a->kind == NM_RUNTIME && index < NM_RUNTIME_FILES ?
        a->payload + NM_RUNTIME_PREFIX + (u64)index * NM_RUNTIME_ENTRY : 0;
}
const float *nm_runtime_window(const NmArtifact *a) {
    return a && a->kind == NM_RUNTIME ? (const float *)(const void *)(a->payload + nm_u64(a->payload + 128)) : 0;
}
const float *nm_runtime_mel(const NmArtifact *a) {
    return a && a->kind == NM_RUNTIME ? (const float *)(const void *)(a->payload + nm_u64(a->payload + 136)) : 0;
}
const u8 *nm_runtime_tokenizer(const NmArtifact *a, u64 *bytes) {
    *bytes = 0;
    if (!a || a->kind != NM_RUNTIME ||
        (memcmp(a->payload + 64, "NMRT3202", 8) && memcmp(a->payload + 64, "NMRT3204", 8))) return 0;
    *bytes = nm_u64(a->payload + 488);
    return a->payload + nm_u64(a->payload + 480);
}
static int runtime_path(const u8 *path, int absolute) {
    u32 length = 0, start, i;
    while (length < 256 && path[length]) ++length;
    if (!length || length == 256 || !zero(path + length, 256 - length) ||
        rt_utf8_validate((const char *)path, length) != 0) return 0;
    start = 0;
    if (absolute) {
        if (length < 4 || !((path[0] >= 'A' && path[0] <= 'Z') ||
            (path[0] >= 'a' && path[0] <= 'z')) || path[1] != ':' || path[2] != '\\') return 0;
        start = 3;
    }
    for (i = start; i <= length; ++i) {
        if (i == length || path[i] == '\\') {
            u32 size = i - start;
            if (!size || path[i - 1] == '.' || path[i - 1] == ' ') return 0;
            start = i + 1;
        } else if (path[i] < 32 || path[i] == '/' || path[i] == ':' ||
                   path[i] == '<' || path[i] == '>' || path[i] == '"' ||
                   path[i] == '|' || path[i] == '?' || path[i] == '*') return 0;
    }
    return 1;
}
static int runtime_payload(NmArtifact *a) {
    const u8 *p = a->payload;
    u64 window, mel;
    u8 hash[32]; u32 i; int tokenizer;
#define RUNTIME_REQUIRE(condition, message) do { if (!(condition)) { a->error = message; return 0; } } while (0)
    RUNTIME_REQUIRE(a->payload_bytes >= NM_RUNTIME_PREFIX &&
                    (!memcmp(p + 64, "NMRT3201", 8) || !memcmp(p + 64, "NMRT3202", 8) ||
                     !memcmp(p + 64, "NMRT3204", 8)),
                    "invalid runtime package schema");
    tokenizer = memcmp(p + 64, "NMRT3201", 8) != 0;
    window = nm_u64(p + 128); mel = nm_u64(p + 136);
    RUNTIME_REQUIRE(a->precision == 32 && !a->tensors && !a->nodes && !a->steps &&
                    nm_u32(a->header + 8) == NM_VERSION, "invalid runtime package counts/precision");
    RUNTIME_REQUIRE(nm_u32(p + 72) == 1 && nm_u32(p + 76) == 16000 &&
                    nm_u32(p + 80) == 512 && nm_u32(p + 84) == 160 &&
                    nm_u32(p + 88) == 400 && nm_u32(p + 92) == 128 &&
                    nm_u32(p + 96) == 25 && nm_u32(p + 100) == 32 &&
                    nm_u32(p + 104) == 56 && nm_u32(p + 108) == 3 &&
                    nm_u32(p + 112) == 9 && nm_u32(p + 116) == 13087 &&
                    nm_u32(p + 120) == 13088 && nm_u32(p + 124) == NM_RUNTIME_FILES,
                    "runtime model/profile contract mismatch");
    RUNTIME_REQUIRE(!memcmp(p + 208, "de-DE", 5) && zero(p + 213, 11) &&
                    nm_float(p + 224, QNN_DATATYPE_FLOAT_32) == 0.97f &&
                    nm_float(p + 228, QNN_DATATYPE_FLOAT_32) == 0x1p-24f &&
                    nm_float(p + 232, QNN_DATATYPE_FLOAT_32) == 2e-5f &&
                    nm_float(p + 236, QNN_DATATYPE_FLOAT_32) == 2e-5f &&
                    nm_u32(p + 240) == 1 && nm_u32(p + 244) == 1 && zero(p + 248, 8) &&
                    (!memcmp(p + 64, "NMRT3204", 8) ? (nm_u32(p + 496) == 1 && zero(p + 500, 12)) :
                     zero(p + (tokenizer ? 496 : 480), tokenizer ? 16 : 32)),
                    "runtime frontend/language contract mismatch");
    RUNTIME_REQUIRE(fingerprint(p + 256, "9eebdd6590289cb3030f310858f3df93256600a800a3e8200c5993d5f967e174") &&
                    fingerprint(p + 288, "3835036f569cec3491d29af6ac6f9316e7f5ee4972588fcd0775b8d644e72adc") &&
                    fingerprint(p + 320, "a1dbe6855bec2894a7082508cd4d724976c4d71db53c851de5d3c10307bcc1cd") &&
                    fingerprint(p + 352, "c0bd70c022c2dc78e579079e729eb209b3aa7876723a891c2b37f7be34601daf") &&
                    fingerprint(p + 384, "9d6788f37c4bded4e5b591dadc188ff14432c97d388f122b2973ebdb3a03318f") &&
                    fingerprint(p + 416, "239ed7e115e147f0b4fa89d532c271e4bd6a06452ce418e66c201377c6880c38") &&
                    fingerprint(p + 448, "3f3d481deb073b64c2082e8c7860d487a3a62774bf4e9e4faac83007e181f246"),
                    "runtime pinned model/module identity mismatch");
    a->metadata_bytes = NM_RUNTIME_PREFIX + (u64)NM_RUNTIME_FILES * NM_RUNTIME_ENTRY;
    RUNTIME_REQUIRE(window == a->metadata_bytes && mel == window + 512 * 4 &&
                    a->payload_bytes >= mel + 128 * 257 * 4, "runtime coefficient offset/size mismatch");
    if (tokenizer) {
        NmTokenizer parsed;
        u64 offset = nm_u64(p + 480), bytes = nm_u64(p + 488);
        RUNTIME_REQUIRE(offset == mel + 128 * 257 * 4 && bytes == a->payload_bytes - offset &&
                        nm_tokenizer_open(&parsed, p + offset, bytes), "invalid runtime tokenizer table");
    } else RUNTIME_REQUIRE(a->payload_bytes == mel + 128 * 257 * 4, "unexpected runtime package trailing data");
    crypto_sha256_hash(p + window, 512 * 4, hash);
    RUNTIME_REQUIRE(!memcmp(hash, p + 144, 32), "runtime window checksum mismatch");
    RUNTIME_REQUIRE(zero(p + window, 56 * 4) && zero(p + window + 456 * 4, 56 * 4),
                    "runtime centered window padding mismatch");
    crypto_sha256_hash(p + window + 56 * 4, 400 * 4, hash);
    RUNTIME_REQUIRE(fingerprint(hash, NM_WINDOW_SHA256), "runtime frozen Hann mismatch");
    crypto_sha256_hash(p + mel, 128 * 257 * 4, hash);
    RUNTIME_REQUIRE(!memcmp(hash, p + 176, 32) && fingerprint(hash, NM_MEL_SHA256),
                    "runtime frozen mel filters mismatch");
    for (i = 0; i < NM_RUNTIME_FILES; ++i) {
        const u8 *entry = nm_runtime_entry(a, i);
        u32 role = nm_u32(entry + 256), flags = nm_u32(entry + 260), j;
        u64 bytes = nm_u64(entry + 264);
        RUNTIME_REQUIRE(role == i + 1 && flags == (role == 15 ? 1U : 0U) &&
                        bytes && bytes <= NM_MAX_PAYLOAD && !zero(entry + 272, 32) && zero(entry + 304, 16),
                        "invalid runtime resource descriptor");
        RUNTIME_REQUIRE(runtime_path(entry, flags != 0), "invalid runtime resource path");
        a->error_name = (const char *)entry;
        for (j = 0; j < i; ++j)
            RUNTIME_REQUIRE(rt_strcmp((const char *)entry, (const char *)nm_runtime_entry(a, j)),
                            "duplicate runtime resource path");
        if (role == 3) RUNTIME_REQUIRE(!memcmp(entry + 272, p + 448, 32), "tokenizer binding mismatch");
        if (role == 1) RUNTIME_REQUIRE(fingerprint(entry + 272,
            "62d186fd91f518e00e7867500f1f5819225e8ee95ea3e21b546514bf2048e845"), "model configuration binding mismatch");
        if (role == 2) RUNTIME_REQUIRE(fingerprint(entry + 272,
            "ec47870f1091ea4f25539208387b45b902c92d0e3f997a30061ef88f73437ab0"), "processor configuration binding mismatch");
        if (role == 4) RUNTIME_REQUIRE(fingerprint(entry + 272,
            "5c641c5b3f50702a60082690d27c1ce7fcb5a92c4a624793bcae0f21eda3d6e0"), "tokenizer configuration binding mismatch");
        if (role == 5) RUNTIME_REQUIRE(fingerprint(entry + 272, NM_CONTRACT_SHA256), "M0 contract resource mismatch");
        if (role == 11) RUNTIME_REQUIRE(!memcmp(entry + 272, p + 320, 32), "DSP module binding mismatch");
        if (role == 12) RUNTIME_REQUIRE(!memcmp(entry + 272, p + 384, 32), "signing catalog binding mismatch");
        if (role == 13) RUNTIME_REQUIRE(!memcmp(entry + 272, p + 416, 32), "host executable binding mismatch");
        if (role == 14) RUNTIME_REQUIRE(!memcmp(entry + 272, p + 288, 32), "signed manifest binding mismatch");
        if (role == 15) RUNTIME_REQUIRE(!memcmp(entry + 272, p + 352, 32), "OEM driver binding mismatch");
        if (role == 16) RUNTIME_REQUIRE(fingerprint(entry + 272,
            "c3d44ac8c354901453976e25a970ea70f29da9ff9ac2b56a4712facfac051e8c"), "model license binding mismatch");
    }
    a->error = 0; a->error_name = 0; return 1;
#undef RUNTIME_REQUIRE
}
static int io_contract(const NmArtifact *a, const char *label, u32 type, u32 dtype,
                       u32 rank, const u32 dimensions[4]) {
    u32 i;
    for (i = 0; i < a->tensors; ++i) {
        const u8 *t = nm_tensor(a, i);
        if (!rt_strcmp((const char *)t, label)) {
            u32 axis;
            if (nm_u32(t + 64) != type || nm_u32(t + 68) != dtype || nm_u32(t + 72) != rank) return 0;
            for (axis = 0; axis < 4; ++axis) if (nm_u32(t + 80 + axis * 4) != dimensions[axis]) return 0;
            return 1;
        }
    }
    return 0;
}
static int runtime_node_metadata(const NmArtifact *a) {
    u32 index;
    for (index = 0; index < a->nodes; ++index) {
        const u8 *node = a->payload + 64 + (u64)a->tensors * NM_TENSOR_BYTES + (u64)index * NM_NODE_BYTES;
        u32 inputs = nm_u32(node + 128), input, count, cursor, seen = 0, opcode, version;
        const u8 *tensor, *metadata, *output;
        u64 words;
        if (inputs < 2 || inputs > NM_FP32_MAX_INPUTS + 1 || nm_u32(node + 132) != 1 ||
            nm_u32(node + 136) || (rt_strcmp((const char *)node + 64, NM_FP32_OPERATOR) &&
                                  rt_strcmp((const char *)node + 64, NM_FP32_GATHER))) return 0;
        tensor = nm_tensor(a, nm_u32(node + 144));
        if (nm_u32(tensor + 64) != QNN_TENSOR_TYPE_STATIC ||
            nm_u32(tensor + 68) != QNN_DATATYPE_INT_32 || nm_u32(tensor + 72) != 1) return 0;
        metadata = a->payload + nm_u64(tensor + 96); words = nm_u64(tensor + 104) / 4;
        if (words < NM_FP32_META_BASE || nm_u32(metadata) != NM_FP32_META_MAGIC) return 0;
        version = nm_u32(metadata + 4); opcode = nm_u32(metadata + 8);
        if ((version != NM_FP32_META_VERSION && version != NM_FP32_PACKED_VERSION) ||
            opcode < NM_FP32_MATMUL || opcode > NM_FP32_SELECT || nm_u32(metadata + 12) != inputs - 1 ||
            ((opcode == NM_FP32_GATHER_OP) != !rt_strcmp((const char *)node + 64, NM_FP32_GATHER))) return 0;
        output = nm_tensor(a, nm_u32(node + 208));
        if (nm_u32(output + 68) != QNN_DATATYPE_FLOAT_32 || nm_u32(metadata + 16) != nm_u32(output + 72) ||
            memcmp(metadata + 20, output + 80, 16)) return 0;
        for (input = 0; input < NM_FP32_MAX_INPUTS; ++input) {
            const u8 *record = metadata + (9 + input * 6) * 4;
            if (input >= inputs - 1) { if (!zero(record, 24)) return 0; }
            else {
                u32 dtype = opcode == NM_FP32_GATHER_OP && input == 1 ? QNN_DATATYPE_INT_32 : QNN_DATATYPE_FLOAT_32;
                const u8 *value = nm_tensor(a, nm_u32(node + 144 + (input + 1) * 4));
                if (nm_u32(value + 68) != dtype || nm_u32(record) != dtype ||
                    nm_u32(record + 4) != nm_u32(value + 72) || memcmp(record + 8, value + 80, 16)) return 0;
            }
        }
        count = nm_u32(metadata + 33 * 4); cursor = NM_FP32_META_BASE;
        if (count > NM_FP32_MAX_PARAMS) return 0;
        while (count--) {
            u32 code, length;
            if (cursor + 2 > words) return 0;
            code = nm_u32(metadata + cursor++ * 4); length = nm_u32(metadata + cursor++ * 4);
            if (!code || code > NM_FP32_WEIGHT_TILES || !length || length > 16 ||
                (seen & (1U << code)) || length > words - cursor) return 0;
            seen |= 1U << code;
            if ((code == NM_FP32_EPSILON || code == NM_FP32_PAD_VALUE || code == NM_FP32_BETA) &&
                (length != 1 || !nm_finite(metadata + cursor * 4, 4, QNN_DATATYPE_FLOAT_32))) return 0;
            if (code == NM_FP32_WEIGHT_TILES && (length != 1 || version != NM_FP32_PACKED_VERSION ||
                opcode != NM_FP32_MATMUL || inputs != 3 || nm_u32(metadata + cursor * 4) != 32)) return 0;
            cursor += length;
        }
        if (cursor != words) return 0;
    }
    return 1;
}
int nm_runtime_graph_contract(NmArtifact *a, u32 role) {
    int ok = a && a->kind == NM_GRAPH && a->precision == 32 && runtime_node_metadata(a);
    if (!a) return 0;
    if (ok && (role == 6 || role == 7)) {
        u32 cache;
        ok = io_contract(a, "features", 0, QNN_DATATYPE_FLOAT_32, 2, (u32[4]){role == 6 ? 25 : 32, 128, 0, 0}) &&
             io_contract(a, "hidden", 1, QNN_DATATYPE_FLOAT_32, 2, (u32[4]){4, 1024, 0, 0});
        for (cache = 0; ok && cache < 3; ++cache) {
            char label[16] = "sub-0-out";
            u32 dimensions[4] = {1, 1, cache == 0 ? 131 : cache == 1 ? 68 : 36, cache ? 256 : 1};
            label[4] = (char)('0' + cache);
            ok = io_contract(a, label, 1, QNN_DATATYPE_FLOAT_32, 4, dimensions);
            if (role == 7) {
                rt_copy_string(label + 6, sizeof(label) - 6, "in");
                ok = ok && io_contract(a, label, 0, QNN_DATATYPE_FLOAT_32, 4, dimensions);
            }
        }
    }
    else if (ok && role == 8) {
        u32 layer;
        ok = io_contract(a, "hidden", 0, QNN_DATATYPE_FLOAT_32, 2, (u32[4]){4, 1024, 0, 0}) &&
             io_contract(a, "mask", 0, QNN_DATATYPE_FLOAT_32, 3, (u32[4]){8, 4, 60, 0}) &&
             io_contract(a, "raw", 1, QNN_DATATYPE_FLOAT_32, 2, (u32[4]){4, 1024, 0, 0}) &&
             io_contract(a, "conditioned", 1, QNN_DATATYPE_FLOAT_32, 2, (u32[4]){4, 1024, 0, 0}) &&
             io_contract(a, "projected", 1, QNN_DATATYPE_FLOAT_32, 2, (u32[4]){4, 640, 0, 0});
        for (layer = 0; ok && layer < 24; ++layer) {
            const char *suffixes[6] = {"-key-in", "-key-out", "-value-in", "-value-out", "-conv-in", "-conv-out"};
            u32 j;
            for (j = 0; ok && j < 6; ++j) {
                char label[64] = "layer-00";
                label[6] = (char)('0' + layer / 10); label[7] = (char)('0' + layer % 10);
                rt_copy_string(label + 8, sizeof(label) - 8, suffixes[j]);
                ok = io_contract(a, label, j & 1U, QNN_DATATYPE_FLOAT_32,
                                 j < 4 ? 3 : 2, (u32[4]){8, j < 4 ? 56 : 1024, j < 4 ? 128 : 0, 0});
            }
        }
    } else if (ok && role == 9) {
        u32 layer, index; int fused = 0;
        ok = io_contract(a, "token", 0, QNN_DATATYPE_INT_32, 1, (u32[4]){1, 0, 0, 0}) &&
             io_contract(a, "prediction", 1, QNN_DATATYPE_FLOAT_32, 2, (u32[4]){1, 640, 0, 0});
        for (index = 0; index < a->tensors; ++index) {
            const char *name = (const char *)nm_tensor(a, index);
            if (!rt_strcmp(name, "encoder") || !rt_strcmp(name, "logits")) fused = 1;
        }
        if (fused) ok = ok &&
            io_contract(a, "encoder", 0, QNN_DATATYPE_FLOAT_32, 2, (u32[4]){1, 640, 0, 0}) &&
            io_contract(a, "logits", 1, QNN_DATATYPE_FLOAT_32, 2, (u32[4]){1, 13088, 0, 0});
        for (layer = 0; ok && layer < 2; ++layer) {
            const char *suffixes[4] = {"-h-in", "-h-out", "-c-in", "-c-out"};
            u32 j;
            for (j = 0; ok && j < 4; ++j) {
                char label[32] = "lstm-0";
                label[5] = (char)('0' + layer); rt_copy_string(label + 6, sizeof(label) - 6, suffixes[j]);
                ok = io_contract(a, label, j & 1U, QNN_DATATYPE_FLOAT_32, 2, (u32[4]){1, 640, 0, 0});
            }
        }
    }
    else if (ok && role == 10)
        ok = io_contract(a, "encoder", 0, QNN_DATATYPE_FLOAT_32, 2, (u32[4]){1, 640, 0, 0}) &&
             io_contract(a, "prediction", 0, QNN_DATATYPE_FLOAT_32, 2, (u32[4]){1, 640, 0, 0}) &&
             io_contract(a, "logits", 1, QNN_DATATYPE_FLOAT_32, 2, (u32[4]){1, 13088, 0, 0});
    else ok = 0;
    if (!ok) a->error = "runtime graph streaming/cache/decoder contract mismatch";
    return ok;
}
void nm_tensor_binding(QnnTensor *q, const u8 *t, void *buffer, const u8 *payload) {
    memset(q, 0, sizeof(*q)); q->version = QNN_TENSOR_VERSION_1;
    q->data.v1.id = nm_u32(t + 76); q->data.v1.name = (const char *)t;
    q->data.v1.type = nm_u32(t + 64); q->data.v1.data_type = nm_u32(t + 68);
    q->data.v1.quantize_params.encoding_definition = QNN_DEFINITION_UNDEFINED;
    q->data.v1.quantize_params.quantization_encoding = QNN_QUANTIZATION_ENCODING_UNDEFINED;
    if (nm_quantized(t)) {
        q->data.v1.quantize_params.encoding_definition = QNN_DEFINITION_DEFINED;
        if (nm_u32(t + 116) == NM_QUANT_AXIS_SCALE_OFFSET) {
            q->data.v1.quantize_params.quantization_encoding = QNN_QUANTIZATION_ENCODING_AXIS_SCALE_OFFSET;
            q->data.v1.quantize_params.encoding.axis_scale_offset.axis = (i32)nm_u32(t + 120);
            q->data.v1.quantize_params.encoding.axis_scale_offset.scale_offset_count = nm_u32(t + 124);
            q->data.v1.quantize_params.encoding.axis_scale_offset.scale_offsets =
                (QnnScaleOffset *)(void *)(payload + nm_u64(t + 128));
        } else {
            q->data.v1.quantize_params.quantization_encoding = QNN_QUANTIZATION_ENCODING_SCALE_OFFSET;
            q->data.v1.quantize_params.encoding.scale_offset.scale = nm_float(t + 120, QNN_DATATYPE_FLOAT_32);
            q->data.v1.quantize_params.encoding.scale_offset.offset = (i32)nm_u32(t + 124);
        }
    }
    q->data.v1.rank = nm_u32(t + 72); q->data.v1.dimensions = (u32 *)(void *)(t + 80);
    q->data.v1.memory.client_buffer.data = buffer;
    if (buffer) q->data.v1.memory.client_buffer.data_size = (u32)nm_tensor_bytes(t);
}
void nm_header(u8 *h, u32 kind, u32 precision, u32 tensors, u32 nodes,
               u32 steps, const u8 *p, u64 bytes) {
    u32 i, version = NM_VERSION;
    if (bytes >= 64 && tensors <= (bytes - 64) / NM_TENSOR_BYTES)
        for (i = 0; i < tensors; ++i)
            if (nm_quantized(p + 64 + (u64)i * NM_TENSOR_BYTES)) { version = NM_QUANT_VERSION; break; }
    memset(h, 0, NM_HEADER_BYTES); memcpy(h, NM_MAGIC, 8);
    nm_put32(h + 8, version); nm_put32(h + 12, kind); nm_put32(h + 16, NM_HEADER_BYTES);
    nm_put32(h + 20, 2); nm_put32(h + 24, 39); nm_put32(h + 28, 2); nm_put32(h + 32, 18);
    nm_put32(h + 36, 73); nm_put32(h + 40, 9); nm_put32(h + 44, 320);
    nm_put32(h + 48, precision); nm_put32(h + 52, tensors); nm_put32(h + 56, nodes);
    nm_put32(h + 60, steps); nm_put64(h + 64, bytes);
    digest(h + 72, NM_MODEL_SHA256); digest(h + 104, NM_CONTRACT_SHA256);
    crypto_sha256_hash(p, (size_t)bytes, h + 136); digest(h + 168, NM_RUNTIME_SHA256);
}
static void sift(NmRange *r, u32 root, u32 count) {
    for (;;) {
        u32 c = root * 2 + 1; NmRange tmp;
        if (c >= count) break;
        if (c + 1 < count && r[c].start < r[c + 1].start) ++c;
        if (r[root].start >= r[c].start) break;
        tmp = r[root]; r[root] = r[c]; r[c] = tmp; root = c;
    }
}
static int ranges(NmRange *r, u32 n) {
    u32 i;
    for (i = n / 2; i; --i) sift(r, i - 1, n);
    for (i = n; i > 1; --i) {
        NmRange tmp = r[0]; r[0] = r[i - 1]; r[i - 1] = tmp; sift(r, 0, i - 1);
    }
    for (i = 1; i < n; ++i) if (r[i].start < r[i - 1].end) return 0;
    return 1;
}
static int blob(NmArtifact *a, NmRange *r, u32 *n, u64 offset, u64 size) {
    if (!size || (offset & 15) || offset < nm_align16(a->metadata_bytes) ||
        offset > a->payload_bytes || size > a->payload_bytes - offset) return 0;
    r[*n].start = offset; r[*n].end = offset + size; ++*n; return 1;
}
static const char *depthwise_preflight(const NmArtifact *a, const u8 *node) {
    const u8 *input, *filter; u32 channels, i;
    if (rt_strcmp((const char *)node + 64, "DepthWiseConv2d")) return 0;
    if (nm_u32(node + 128) < 2) return "DepthWiseConv2d requires data and filter inputs";
    input = nm_tensor(a, nm_u32(node + 144));
    filter = nm_tensor(a, nm_u32(node + 148));
    if (nm_u32(input + 72) != 4) return "DepthWiseConv2d input must be rank4 NHWC";
    if (nm_u32(filter + 72) != 4 || nm_u32(filter + 88) != 1)
        return "DepthWiseConv2d filter must be rank4 [H,W,1,channel_out]";
    channels = nm_u32(input + 92);
    if (!channels || nm_u32(filter + 92) % channels)
        return "DepthWiseConv2d filter channel_out must be a multiple of input channels";
    for (i = 0; i < nm_u32(node + 136); ++i) {
        const u8 *param = node + 272 + i * NM_PARAM_BYTES, *t, *data;
        u32 dtype, j;
        if (rt_strcmp((const char *)param, "stride") && rt_strcmp((const char *)param, "dilation")) continue;
        if (nm_u32(param + 32) != QNN_PARAMTYPE_TENSOR)
            return "DepthWiseConv2d stride/dilation must be a two-element integer tensor";
        t = nm_tensor(a, nm_u32(param + 40)); dtype = nm_u32(t + 68);
        if ((dtype != QNN_DATATYPE_UINT_32 && dtype != QNN_DATATYPE_INT_32) ||
            nm_u32(t + 72) != 1 || nm_u32(t + 80) != 2)
            return "DepthWiseConv2d stride/dilation must be a two-element integer tensor";
        data = a->payload + nm_u64(t + 96);
        for (j = 0; j < 2; ++j) {
            u32 value = nm_u32(data + j * 4);
            if (!value || (dtype == QNN_DATATYPE_INT_32 && value > 0x7fffffffU))
                return "DepthWiseConv2d stride/dilation values must be positive";
        }
    }
    return 0;
}
#define REQUIRE(test, message) do { if (!(test)) { a->error = message; goto fail; } } while (0)
int nm_validate(NmArtifact *a, const void *file, u64 bytes) {
    const u8 *h = file; u8 hash[32]; NmRange *r = 0;
    u32 i, j, k, s, nr = 0; u8 *available = 0;
    memset(a, 0, sizeof(*a)); a->error = "invalid artifact";
    REQUIRE(h && bytes >= NM_HEADER_BYTES, "truncated header");
    REQUIRE(!memcmp(h, NM_MAGIC, 8), "bad magic");
    REQUIRE((nm_u32(h + 8) == NM_VERSION || nm_u32(h + 8) == NM_QUANT_VERSION) &&
            nm_u32(h + 16) == NM_HEADER_BYTES, "unsupported envelope version/header size");
    REQUIRE(nm_u32(h + 20) == 2 && nm_u32(h + 24) == 39 &&
            nm_u32(h + 28) == 2 && nm_u32(h + 32) == 18, "QNN ABI version mismatch");
    REQUIRE(nm_u32(h + 36) == 73 && nm_u32(h + 40) == 9 && nm_u32(h + 44) == 320, "SoC/language/chunk mismatch");
    REQUIRE(fingerprint(h + 72, NM_MODEL_SHA256), "model fingerprint mismatch");
    REQUIRE(fingerprint(h + 104, NM_CONTRACT_SHA256), "accepted M0 contract fingerprint mismatch");
    REQUIRE(fingerprint(h + 168, NM_RUNTIME_SHA256), "QnnHtp runtime fingerprint mismatch");
    a->header = h; a->payload = h + NM_HEADER_BYTES; a->payload_bytes = nm_u64(h + 64);
    a->kind = nm_u32(h + 12); a->precision = nm_u32(h + 48);
    a->tensors = nm_u32(h + 52); a->nodes = nm_u32(h + 56); a->steps = nm_u32(h + 60);
    REQUIRE(a->kind >= NM_GRAPH && a->kind <= NM_RUNTIME, "unknown artifact kind");
    REQUIRE(a->precision == 16 || a->precision == 32, "unsupported precision");
    REQUIRE(zero(h + (a->kind == NM_RESULT ? 232 : 200), a->kind == NM_RESULT ? 24 : 56), "nonzero reserved header");
    REQUIRE(a->payload_bytes >= 64 && a->payload_bytes <= NM_MAX_PAYLOAD &&
            bytes - NM_HEADER_BYTES == a->payload_bytes, "payload length/bounds mismatch");
    if (a->kind == NM_RUNTIME) {
        crypto_sha256_hash(a->payload, (size_t)a->payload_bytes, hash);
        REQUIRE(!memcmp(hash, h + 136, 32), "payload SHA256 mismatch");
        REQUIRE(name(a->payload, 64), "invalid artifact name");
        REQUIRE(runtime_payload(a), a->error);
        return 1;
    }
    REQUIRE(a->tensors && a->tensors <= (a->kind == NM_GRAPH ? NM_MAX_TENSORS : NM_MAX_IO) &&
            a->nodes <= NM_MAX_NODES && a->steps <= NM_MAX_STEPS, "counts exceed bounded limits");
    REQUIRE((a->kind == NM_GRAPH && !a->steps && a->nodes) ||
            (a->kind == NM_CONTEXT && !a->steps && !a->nodes) ||
            ((a->kind == NM_FIXTURE || a->kind == NM_RESULT) && a->steps && !a->nodes), "invalid kind/count combination");
    a->metadata_bytes = 64 + (u64)a->tensors * NM_TENSOR_BYTES + (u64)a->nodes * NM_NODE_BYTES +
        (u64)a->steps * ((a->kind == NM_RESULT ? NM_RESULT_STEP_BYTES : NM_STEP_BYTES) + (u64)a->tensors * NM_CELL_BYTES);
    REQUIRE(a->metadata_bytes <= a->payload_bytes, "truncated metadata tables");
    crypto_sha256_hash(a->payload, (size_t)a->payload_bytes, hash);
    REQUIRE(!memcmp(hash, h + 136, 32), "payload SHA256 mismatch");
    REQUIRE(name(a->payload, 64), "invalid artifact name");
    r = rt_malloc((size_t)(2 * a->tensors + (u64)a->steps * a->tensors) * sizeof(*r));
    available = rt_malloc(a->tensors);
    REQUIRE(r && available, "reader allocation failed");
    memset(available, 0, a->tensors);
    for (i = 0; i < a->tensors; ++i) {
        const u8 *t = nm_tensor(a, i); u32 type = nm_u32(t + 64), rank = nm_u32(t + 72);
        u64 off = nm_u64(t + 96), size = nm_u64(t + 104), native = nm_tensor_bytes(t);
        REQUIRE(name(t, 64), "invalid tensor name");
        REQUIRE(rank <= 4 && zero(t + 80 + rank * 4, (4 - rank) * 4), "tensor reserved/dimension padding");
        if (nm_u32(h + 8) == NM_VERSION) {
            REQUIRE(zero(t + 116, 44) && !nm_quantized(t), "v1 tensor quantization forbidden");
        } else if (nm_quantized(t)) {
            u32 encoding = nm_u32(t + 116);
            if (encoding == NM_QUANT_AXIS_SCALE_OFFSET) {
                u32 axis = nm_u32(t + 120), count = nm_u32(t + 124), entry;
                u64 table = nm_u64(t + 128);
                REQUIRE(zero(t + 136, 24), "tensor reserved/dimension padding");
                REQUIRE(type == QNN_TENSOR_TYPE_STATIC &&
                        (nm_u32(t + 68) == QNN_DATATYPE_SFIXED_POINT_16 ||
                         nm_u32(t + 68) == QNN_DATATYPE_SFIXED_POINT_32), "per-axis quantization requires signed static tensor");
                REQUIRE(axis < rank && count && count == nm_u32(t + 80 + axis * 4), "invalid quantization axis/count");
                REQUIRE(blob(a, r, &nr, table, (u64)count * 8), "quantization table range/size/alignment");
                for (entry = 0; entry < count; ++entry) {
                    const u8 *q = a->payload + table + (u64)entry * 8;
                    REQUIRE(nm_finite(q, 4, QNN_DATATYPE_FLOAT_32) &&
                            nm_float(q, QNN_DATATYPE_FLOAT_32) > 0, "invalid quantization scale");
                    REQUIRE(!nm_u32(q + 4), "invalid quantization offset");
                }
            } else {
                i32 offset = (i32)nm_u32(t + 124);
                REQUIRE(encoding == NM_QUANT_SCALE_OFFSET, "quantized tensor requires supported encoding");
                REQUIRE(zero(t + 128, 32), "tensor reserved/dimension padding");
                REQUIRE(nm_finite(t + 120, 4, QNN_DATATYPE_FLOAT_32) &&
                        nm_float(t + 120, QNN_DATATYPE_FLOAT_32) > 0, "invalid quantization scale");
                REQUIRE(nm_u32(t + 68) == QNN_DATATYPE_UFIXED_POINT_16 ?
                        offset >= -65535 && offset <= 0 : offset == 0, "invalid quantization offset");
            }
        } else REQUIRE(zero(t + 116, 44), "nonquantized tensor has quantization");
        REQUIRE(native, "invalid dtype/shape or tensor byte overflow");
        REQUIRE(type == QNN_TENSOR_TYPE_APP_WRITE || type == QNN_TENSOR_TYPE_APP_READ ||
                (a->kind == NM_GRAPH && (type == QNN_TENSOR_TYPE_NATIVE || type == QNN_TENSOR_TYPE_STATIC)), "unsupported tensor type");
        REQUIRE(nm_u32(t + 112) <= ((a->kind == NM_FIXTURE || a->kind == NM_RESULT) ? 2U : 0U), "invalid graph index");
        if (nm_u32(t + 112)) a->pipeline = 1;
        for (j = 0; j < i; ++j) {
            const u8 *previous = nm_tensor(a, j);
            if (nm_u32(t + 112) == nm_u32(previous + 112)) {
                REQUIRE(rt_strcmp((const char *)t, (const char *)previous), "duplicate tensor name");
                REQUIRE(nm_u32(t + 76) != nm_u32(previous + 76), "duplicate tensor id");
            }
        }
        if (type == QNN_TENSOR_TYPE_STATIC) {
            REQUIRE(size == native && blob(a, r, &nr, off, size), "static tensor data range/size/alignment");
            REQUIRE(nm_finite(a->payload + off, size, nm_u32(t + 68)), "nonfinite static tensor");
            available[i] = 1;
        } else {
            REQUIRE(!off && !size, "nonstatic tensor has data");
            if (type == QNN_TENSOR_TYPE_APP_WRITE) available[i] = 1;
        }
    }
    if (a->kind == NM_GRAPH) {
        for (i = 0; i < a->nodes; ++i) {
            const u8 *n = a->payload + 64 + (u64)a->tensors * NM_TENSOR_BYTES + (u64)i * NM_NODE_BYTES;
            u32 ni = nm_u32(n + 128), no = nm_u32(n + 132), np = nm_u32(n + 136);
            REQUIRE(name(n, 64) && name(n + 64, 64), "invalid node name/type");
            for (j = 0; j < i; ++j) REQUIRE(rt_strcmp((const char *)n,
                (const char *)(a->payload + 64 + (u64)a->tensors * NM_TENSOR_BYTES + (u64)j * NM_NODE_BYTES)), "duplicate node name");
            REQUIRE(ni <= NM_MAX_NODE_IO && no && no <= NM_MAX_NODE_IO && np <= NM_MAX_PARAMS, "node arity exceeds limits");
            REQUIRE(zero(n + 140, 4) && zero(n + 144 + ni * 4, (16 - ni) * 4) &&
                    zero(n + 208 + no * 4, (16 - no) * 4) &&
                    zero(n + 272 + np * 64, (6 - np) * 64) && zero(n + 656, 16), "node reserved/index padding");
            for (j = 0; j < ni; ++j) {
                k = nm_u32(n + 144 + j * 4);
                REQUIRE(k < a->tensors && available[k], "node input index/order");
            }
            for (j = 0; j < np; ++j) {
                const u8 *p = n + 272 + j * 64; u32 pt = nm_u32(p + 32), dt = nm_u32(p + 36), value = nm_u32(p + 40);
                REQUIRE(name(p, 32) && zero(p + 44, 20), "parameter name/reserved bytes");
                for (k = 0; k < j; ++k) REQUIRE(rt_strcmp((const char *)p, (const char *)(n + 272 + k * 64)), "duplicate parameter name");
                REQUIRE(pt <= 1, "invalid parameter type");
                if (pt) {
                    REQUIRE(value < a->tensors && nm_u32(nm_tensor(a, value) + 64) == QNN_TENSOR_TYPE_STATIC &&
                            dt == nm_u32(nm_tensor(a, value) + 68), "parameter tensor index/type/dtype");
                } else {
                    REQUIRE(dt == QNN_DATATYPE_FLOAT_32 || dt == QNN_DATATYPE_INT_32 ||
                            dt == QNN_DATATYPE_UINT_32 || dt == QNN_DATATYPE_BOOL_8, "unsupported scalar parameter dtype");
                    REQUIRE(nm_finite(p + 40, dt == QNN_DATATYPE_BOOL_8 ? 1 : 4, dt) &&
                            (dt != QNN_DATATYPE_BOOL_8 || value <= 1), "invalid scalar parameter value");
                }
            }
            {
                const char *error = depthwise_preflight(a, n);
                if (error) { a->error_name = (const char *)n; REQUIRE(0, error); }
            }
            for (j = 0; j < no; ++j) {
                k = nm_u32(n + 208 + j * 4);
                REQUIRE(k < a->tensors && !available[k] &&
                        (nm_u32(nm_tensor(a, k) + 64) == QNN_TENSOR_TYPE_NATIVE ||
                         nm_u32(nm_tensor(a, k) + 64) == QNN_TENSOR_TYPE_APP_READ), "node output index/type/duplicate producer");
                available[k] = 1;
            }
        }
        for (i = 0; i < a->tensors; ++i) REQUIRE(available[i], "unproduced graph tensor");
    } else if (a->kind == NM_CONTEXT) {
        a->binary_offset = nm_align16(a->metadata_bytes);
        REQUIRE(a->binary_offset < a->payload_bytes &&
                zero(a->payload + a->metadata_bytes, a->binary_offset - a->metadata_bytes), "empty context binary/nonzero alignment padding");
    } else {
        a->benchmark = a->kind == NM_RESULT && !nm_u64(nm_step(a, 0) + 48);
        for (s = 0; s < a->steps; ++s) {
            const u8 *step = nm_step(a, s);
            u32 flags = nm_u32(step + (a->kind == NM_RESULT ? 4 : 0));
            REQUIRE(!(flags & ~(NM_FIRST | NM_FINAL)) && (!(flags & NM_FIRST) || !s) &&
                    (!(flags & NM_FINAL) || s + 1 == a->steps), "invalid FIRST/FINAL step flags");
            REQUIRE(s || (flags & NM_FIRST), "initial step missing FIRST");
            REQUIRE(a->kind == NM_RESULT ? (nm_u32(step) == s && zero(step + 56, 8) && nm_finite(step + 40, 4, QNN_DATATYPE_FLOAT_32)) :
                    zero(step + 12, 4), "step metrics/count/reserved fields");
            if (a->kind == NM_RESULT) {
                REQUIRE(a->benchmark == !nm_u64(step + 48), "mixed benchmark/correctness result steps");
                REQUIRE(!a->benchmark || zero(step + 40, 16), "benchmark result has comparison metrics");
            }
            for (i = 0; i < a->tensors; ++i) {
                const u8 *t = nm_tensor(a, i), *c = nm_cell(a, s, i);
                u32 mode = nm_u32(c), source = nm_u32(c + 4), age = nm_u32(c + 24);
                u32 graph = nm_u32(t + 112), type = nm_u32(t + 64);
                u64 off = nm_u64(c + 8), size = nm_u64(c + 16);
                REQUIRE(mode <= NM_ZERO && zero(c + 28, 4), "cell mode/reserved bytes");
                REQUIRE(!a->benchmark || mode == NM_ZERO, "benchmark result contains tensor data");
                REQUIRE(nm_active(a, s, graph) || mode == NM_ZERO, "inactive graph cell is not ZERO");
                if (mode == NM_ZERO) {
                    REQUIRE(!source && !age && !off && !size, "ZERO cell has fields/data");
                    REQUIRE(type != QNN_TENSOR_TYPE_APP_READ || !nm_active(a, s, graph) || a->benchmark, "active output missing");
                } else if (mode == NM_DATA) {
                    u32 dt = nm_u32(t + 68);
                    u64 expected = a->kind == NM_FIXTURE && type == QNN_TENSOR_TYPE_APP_READ ? nm_expected_bytes(t) : nm_tensor_bytes(t);
                    REQUIRE(!source && !age && size == expected && blob(a, r, &nr, off, size), "DATA cell range/size/alignment");
                    REQUIRE(a->kind != NM_RESULT || type == QNN_TENSOR_TYPE_APP_READ, "RESULT contains input data");
                    if (a->kind == NM_FIXTURE && type == QNN_TENSOR_TYPE_APP_READ &&
                        (dt == QNN_DATATYPE_FLOAT_16 || nm_quantized(t))) dt = QNN_DATATYPE_FLOAT_32;
                    REQUIRE(a->kind == NM_RESULT || nm_finite(a->payload + off, size, dt), "nonfinite fixture data");
                } else {
                    const u8 *from;
                    REQUIRE(a->kind == NM_FIXTURE && type == QNN_TENSOR_TYPE_APP_WRITE &&
                            source < a->tensors && age <= 1 && !off && !size, "invalid CARRY fields");
                    from = nm_tensor(a, source);
                    REQUIRE(nm_u32(from + 64) == QNN_TENSOR_TYPE_APP_READ && nm_same_shape(t, from), "CARRY source type/shape/dtype mismatch");
                    REQUIRE((age ? s && nm_active(a, s - 1, nm_u32(from + 112)) :
                            nm_active(a, s, nm_u32(from + 112)) && nm_u32(from + 112) < graph), "CARRY exact age/availability mismatch");
                }
            }
        }
    }
    REQUIRE(ranges(r, nr), "overlapping data blobs");
    rt_free(r); rt_free(available); a->error = 0; return 1;
fail:
    rt_free(r); rt_free(available); return 0;
}
