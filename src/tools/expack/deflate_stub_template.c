typedef __SIZE_TYPE__ size_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef long int64_t;
typedef unsigned long uint64_t;

#define SYS_WRITE 1L
#define SYS_MMAP 9L
#define SYS_EXIT 60L
#define SYS_MEMFD_CREATE 319L
#define SYS_EXECVEAT 322L
#define PROT_READ 1L
#define PROT_WRITE 2L
#define MAP_PRIVATE 2L
#define MAP_ANONYMOUS 32L
#define AT_EMPTY_PATH 0x1000L

extern const uint8_t payload_start[];
const volatile uint64_t expack_deflate_original_size = 0x1122334455667788ULL;
const volatile uint64_t expack_deflate_payload_size = 0x8877665544332211ULL;

__asm__(
    ".section .payload,\"a\"\n"
    ".global payload_start\n"
    "payload_start:\n"
    ".previous\n");

typedef struct { const uint8_t *src; size_t src_size; size_t src_pos; uint32_t bits; unsigned int bit_count; } BitReader;
typedef struct { uint16_t count[16]; uint16_t first[16]; uint16_t index[16]; uint16_t symbols[320]; } HuffmanTable;

static long syscall6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return r;
}

static void die(void) { syscall6(SYS_EXIT, 127, 0, 0, 0, 0, 0); for (;;) {} }
static int br_need(BitReader *br, unsigned int need) {
    while (br->bit_count < need) { if (br->src_pos >= br->src_size) return -1; br->bits |= (uint32_t)br->src[br->src_pos++] << br->bit_count; br->bit_count += 8U; }
    return 0;
}
static unsigned int br_read(BitReader *br, unsigned int count) { unsigned int value = br->bits & ((1U << count) - 1U); br->bits >>= count; br->bit_count -= count; return value; }
static unsigned int bit_reverse(unsigned int value, unsigned int count) { unsigned int out = 0; while (count-- != 0U) { out = (out << 1U) | (value & 1U); value >>= 1U; } return out; }

static void huff_build(HuffmanTable *table, const uint8_t *lengths, unsigned int n) {
    unsigned int i; unsigned int code = 0;
    for (i = 0; i < 16U; ++i) { table->count[i] = 0; table->first[i] = 0; table->index[i] = 0; }
    for (i = 0; i < n; ++i) if (lengths[i] != 0U) table->count[lengths[i]]++;
    for (i = 1U; i < 16U; ++i) { table->first[i] = (uint16_t)code; table->index[i] = (uint16_t)(table->index[i - 1U] + table->count[i - 1U]); code = (code + table->count[i]) << 1U; }
    for (i = 0; i < n; ++i) { unsigned int len = lengths[i]; if (len != 0U) table->symbols[table->index[len]++] = (uint16_t)i; }
    for (i = 1U; i < 16U; ++i) table->index[i] = (uint16_t)(table->index[i - 1U] + table->count[i - 1U]);
}

static int huff_decode(BitReader *br, const HuffmanTable *table) {
    unsigned int code = 0; unsigned int index = 0; unsigned int len;
    for (len = 1U; len < 16U; ++len) { unsigned int canonical; if (br_need(br, 1U) != 0) return -1; code |= br_read(br, 1U) << (len - 1U); canonical = bit_reverse(code, len); if (canonical - table->first[len] < table->count[len]) return table->symbols[index + canonical - table->first[len]]; index += table->count[len]; }
    return -1;
}

static const uint16_t len_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const uint8_t len_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t dist_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t dist_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static int inflate_codes(BitReader *br, uint8_t *dst, size_t dst_cap, size_t *dst_pos, const HuffmanTable *lit_table, const HuffmanTable *dist_table) {
    for (;;) { int sym = huff_decode(br, lit_table); int dist_sym; unsigned int length; unsigned int distance; unsigned int extra; if (sym < 0) return -1; if (sym < 256) { if (*dst_pos >= dst_cap) return -1; dst[(*dst_pos)++] = (uint8_t)sym; continue; } if (sym == 256) return 0; if (sym > 285) return -1; sym -= 257; length = len_base[sym]; extra = len_extra[sym]; if (extra != 0U) { if (br_need(br, extra) != 0) return -1; length += br_read(br, extra); } dist_sym = huff_decode(br, dist_table); if (dist_sym < 0 || dist_sym >= 30) return -1; distance = dist_base[dist_sym]; extra = dist_extra[dist_sym]; if (extra != 0U) { if (br_need(br, extra) != 0) return -1; distance += br_read(br, extra); } if (distance == 0U || distance > *dst_pos || *dst_pos + length > dst_cap) return -1; while (length-- != 0U) { dst[*dst_pos] = dst[*dst_pos - distance]; ++*dst_pos; } }
}

static int dynamic_tables(BitReader *br, HuffmanTable *lit_table, HuffmanTable *dist_table) {
    static const uint8_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    uint8_t code_lengths[19]; uint8_t lengths[320]; HuffmanTable code_table; unsigned int hlit; unsigned int hdist; unsigned int hclen; unsigned int total; unsigned int i;
    if (br_need(br, 14U) != 0) return -1; hlit = br_read(br, 5U) + 257U; hdist = br_read(br, 5U) + 1U; hclen = br_read(br, 4U) + 4U; total = hlit + hdist; if (total > 320U) return -1; for (i = 0; i < 19U; ++i) code_lengths[i] = 0; for (i = 0; i < hclen; ++i) { if (br_need(br, 3U) != 0) return -1; code_lengths[order[i]] = (uint8_t)br_read(br, 3U); } huff_build(&code_table, code_lengths, 19U);
    for (i = 0; i < total;) { int sym = huff_decode(br, &code_table); unsigned int repeat; unsigned int value; if (sym < 0) return -1; if (sym < 16) { lengths[i++] = (uint8_t)sym; continue; } if (sym == 16) { if (i == 0U || br_need(br, 2U) != 0) return -1; repeat = br_read(br, 2U) + 3U; value = lengths[i - 1U]; } else if (sym == 17) { if (br_need(br, 3U) != 0) return -1; repeat = br_read(br, 3U) + 3U; value = 0U; } else { if (br_need(br, 7U) != 0) return -1; repeat = br_read(br, 7U) + 11U; value = 0U; } if (i + repeat > total) return -1; while (repeat-- != 0U) lengths[i++] = (uint8_t)value; }
    huff_build(lit_table, lengths, hlit); huff_build(dist_table, lengths + hlit, hdist); return 0;
}

static int inflate_dynamic(const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_cap, size_t *dst_size_out) {
    BitReader br; HuffmanTable lit_table; HuffmanTable dist_table; size_t dst_pos = 0; br.src = src; br.src_size = src_size; br.src_pos = 0; br.bits = 0; br.bit_count = 0;
    if (br_need(&br, 3U) != 0) return -1;
    if (br_read(&br, 1U) != 1U || br_read(&br, 2U) != 2U) return -1;
    if (dynamic_tables(&br, &lit_table, &dist_table) != 0 || inflate_codes(&br, dst, dst_cap, &dst_pos, &lit_table, &dist_table) != 0) return -1;
    *dst_size_out = dst_pos; return 0;
}

static uint32_t load32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U); }
static void store32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8U); p[2] = (uint8_t)(v >> 16U); p[3] = (uint8_t)(v >> 24U); }
static void inverse_bcj(uint8_t *data, size_t size) { size_t position = 0U; while (position + 5U < size) { if (data[position] == 0xe8U || data[position] == 0xe9U) { store32(data + position + 1U, load32(data + position + 1U) - (uint32_t)(position + 5U)); position += 5U; } else if (position + 5U < size && data[position] == 0x0fU && (data[position + 1U] & 0xf0U) == 0x80U) { store32(data + position + 2U, load32(data + position + 2U) - (uint32_t)(position + 6U)); position += 6U; } else position += 1U; } }
static void write_all(long fd, const uint8_t *data, size_t size) { while (size != 0U) { long n = syscall6(SYS_WRITE, fd, (long)data, (long)size, 0, 0, 0); if (n <= 0) die(); data += (size_t)n; size -= (size_t)n; } }

void entry(void *stack) {
    uint8_t *image; size_t out_size = 0; long fd; uint64_t argc; char **argv; char **envp; static const char name[] = "expack"; static const char empty[] = ""; size_t original_size = (size_t)expack_deflate_original_size; size_t payload_size = (size_t)expack_deflate_payload_size;
    image = (uint8_t *)syscall6(SYS_MMAP, 0, (long)original_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); if ((int64_t)image < 0) die();
    if (inflate_dynamic(payload_start, payload_size, image, original_size, &out_size) != 0 || out_size != original_size) die();
    inverse_bcj(image, original_size); fd = syscall6(SYS_MEMFD_CREATE, (long)name, 0, 0, 0, 0, 0); if (fd < 0) die(); write_all(fd, image, original_size); argc = *(uint64_t *)stack; argv = (char **)((uint8_t *)stack + 8U); envp = (char **)((uint8_t *)stack + 16U + argc * 8U); syscall6(SYS_EXECVEAT, fd, (long)empty, (long)argv, (long)envp, AT_EMPTY_PATH, 0); die();
}

__asm__(".global _start\n_start:\nxor %rbp,%rbp\nmov %rsp,%rdi\nand $-16,%rsp\ncall entry\n");
