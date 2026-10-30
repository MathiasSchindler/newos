#include "fontrender/font_backend.h"
#include "fontrender/fr_raster.h"
#include "fontrender/fr_ttf.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        tool_write_error("fontrendertest", message, 0); \
        return 1; \
    } \
} while (0)

enum {
    TEST_HEAD = 124,
    TEST_MAXP = 180,
    TEST_HHEA = 212,
    TEST_HMTX = 248,
    TEST_CMAP = 264,
    TEST_LOCA = 328,
    TEST_GLYF = 340,
    TEST_FONT_SIZE = 392
};

static void make_font(unsigned char data[TEST_FONT_SIZE]) {
    static const struct {
        const char *tag;
        unsigned int offset;
        unsigned int size;
    } tables[] = {
        {"head", TEST_HEAD, 54}, {"maxp", TEST_MAXP, 32},
        {"hhea", TEST_HHEA, 36}, {"hmtx", TEST_HMTX, 16},
        {"cmap", TEST_CMAP, 64}, {"loca", TEST_LOCA, 10},
        {"glyf", TEST_GLYF, 52}
    };
    static const unsigned int codepoints[] = {32, 63, 65};
    static const unsigned int glyphs[] = {3, 1, 2};
    static const unsigned int offsets[] = {0, 0, 17, 26, 26};
    size_t i;

    memset(data, 0, TEST_FONT_SIZE);
    tool_store_u32_be(data, 0x00010000U);
    tool_store_u16_be(data + 4, 7);
    tool_store_u16_be(data + 6, 64);
    tool_store_u16_be(data + 8, 2);
    tool_store_u16_be(data + 10, 48);
    for (i = 0; i < sizeof(tables) / sizeof(tables[0]); ++i) {
        unsigned char *record = data + 12 + i * 16;
        memcpy(record, tables[i].tag, 4);
        tool_store_u32_be(record + 8, tables[i].offset);
        tool_store_u32_be(record + 12, tables[i].size);
    }
    tool_store_u32_be(data + TEST_HEAD, 0x00010000U);
    tool_store_u32_be(data + TEST_HEAD + 12, 0x5f0f3cf5U);
    tool_store_u16_be(data + TEST_HEAD + 18, 1024);
    tool_store_u32_be(data + TEST_MAXP, 0x00010000U);
    tool_store_u16_be(data + TEST_MAXP + 4, 4);
    tool_store_u32_be(data + TEST_HHEA, 0x00010000U);
    tool_store_u16_be(data + TEST_HHEA + 4, 768);
    tool_store_u16_be(data + TEST_HHEA + 6, (unsigned short)-256);
    tool_store_u16_be(data + TEST_HHEA + 34, 4);
    for (i = 0; i < 4; ++i) {
        tool_store_u16_be(data + TEST_HMTX + i * 4, 640);
    }

    tool_store_u16_be(data + TEST_CMAP + 2, 1);
    tool_store_u16_be(data + TEST_CMAP + 4, 3);
    tool_store_u16_be(data + TEST_CMAP + 6, 10);
    tool_store_u32_be(data + TEST_CMAP + 8, 12);
    tool_store_u16_be(data + TEST_CMAP + 12, 12);
    tool_store_u32_be(data + TEST_CMAP + 16, 52);
    tool_store_u32_be(data + TEST_CMAP + 24, 3);
    for (i = 0; i < 3; ++i) {
        unsigned char *group = data + TEST_CMAP + 28 + i * 12;
        tool_store_u32_be(group, codepoints[i]);
        tool_store_u32_be(group + 4, codepoints[i]);
        tool_store_u32_be(group + 8, glyphs[i]);
    }
    for (i = 0; i < 5; ++i) {
        tool_store_u16_be(data + TEST_LOCA + i * 2, offsets[i]);
    }

    /* A four-point square, a compound reference to it, and an empty space. */
    tool_store_u16_be(data + TEST_GLYF, 1);
    tool_store_u16_be(data + TEST_GLYF + 6, 512);
    tool_store_u16_be(data + TEST_GLYF + 8, 512);
    tool_store_u16_be(data + TEST_GLYF + 10, 3);
    memset(data + TEST_GLYF + 14, 1, 4);
    tool_store_u16_be(data + TEST_GLYF + 20, 512);
    tool_store_u16_be(data + TEST_GLYF + 24, (unsigned short)-512);
    tool_store_u16_be(data + TEST_GLYF + 30, 512);
    tool_store_u16_be(data + TEST_GLYF + 34, 0xffffU);
    tool_store_u16_be(data + TEST_GLYF + 40, 512);
    tool_store_u16_be(data + TEST_GLYF + 42, 512);
    tool_store_u16_be(data + TEST_GLYF + 44, 3);
    tool_store_u16_be(data + TEST_GLYF + 46, 1);
}

static int make_ring_outline(FrOutline *outline, int size) {
    static const int points[8][2] = {
        {0, 0}, {4, 0}, {4, 4}, {0, 4},
        {1, 1}, {1, 3}, {3, 3}, {3, 1}
    };
    size_t i;

    fr_outline_reset(outline);
    for (i = 0; i < 8; ++i) {
        CHECK(fr_outline_add_point(outline,
                                   (points[i][0] * size / 4 - size / 2) * FR_OUTLINE_ONE,
                                   (points[i][1] * size / 4 - size / 2) * FR_OUTLINE_ONE,
                                   FR_OUTLINE_POINT_ON_CURVE) == 0,
              "could not build raster outline");
    }
    CHECK(fr_outline_add_contour(outline, 0, 4) == 0 &&
          fr_outline_add_contour(outline, 4, 4) == 0, "could not build raster contours");
    return 0;
}

static int check_ring_bitmap(const FrBitmap *bitmap, int size) {
    int x;
    int y;

    CHECK(bitmap->pixels != 0 && bitmap->width == size && bitmap->height == size &&
          bitmap->stride == size && bitmap->left == -size / 2 && bitmap->top == size / 2,
          "incorrect ring raster metrics");
    for (y = 0; y < size; ++y) {
        for (x = 0; x < size; ++x) {
            int hole = x >= size / 4 && x < size * 3 / 4 &&
                       y >= size / 4 && y < size * 3 / 4;
            CHECK(bitmap->pixels[(size_t)y * (size_t)bitmap->stride + (size_t)x] ==
                  (hole ? 0 : 255), "incorrect ring raster coverage");
        }
    }
    return 0;
}

static int check_raster_memory(void) {
    FrOutline outline;
    FrOutlinePoint *points;
    FrOutlineContour *contours;
    FrBitmap bitmap;
    FrBitmap saved;
    RtArena scratch;
    RtArena failing;
    RtArenaBlock *blocks;
    uint8_t *pixels;
    size_t point_capacity;
    size_t contour_capacity;
    size_t default_block_size;
    unsigned int i;

    fr_outline_init(&outline);
    fr_bitmap_init(&bitmap);
    rt_arena_init(&scratch, 16384u);
    /* Reject backing-block header overflow without requesting OS pages. */
    rt_arena_init(&failing, SIZE_MAX - 15u);
    CHECK(make_ring_outline(&outline, 16) == 0, "could not build small raster outline");

    points = outline.points;
    contours = outline.contours;
    point_capacity = outline.point_capacity;
    contour_capacity = outline.contour_capacity;
    CHECK(fr_outline_reserve(&outline, SIZE_MAX, contour_capacity) != 0 &&
          fr_outline_reserve(&outline, point_capacity, SIZE_MAX) != 0,
          "overflowing outline allocation succeeded");
    CHECK(outline.points == points && outline.contours == contours &&
          outline.point_capacity == point_capacity && outline.contour_capacity == contour_capacity &&
          fr_outline_validate(&outline) == 0, "failed outline growth damaged existing storage");

    CHECK(fr_raster_render(&bitmap, &outline, &scratch) == 0 &&
          check_ring_bitmap(&bitmap, 16) == 0, "initial scratch rendering failed");
    blocks = scratch.blocks;
    CHECK(blocks != 0, "raster scratch was not allocated");
    for (i = 0; i < 16; ++i) {
        void *probe = rt_arena_alloc(&scratch, 16);
        CHECK(probe != 0, "scratch was not reset after rendering");
        memset(probe, 0xa5, 16);
        CHECK(fr_raster_render(&bitmap, &outline, &scratch) == 0 &&
              check_ring_bitmap(&bitmap, 16) == 0, "reused scratch changed rendering");
        CHECK(scratch.blocks == blocks, "repeated rendering allocated more scratch blocks");
    }

    pixels = bitmap.pixels;
    CHECK(fr_raster_render(&bitmap, &outline, &failing) != 0 &&
          bitmap.pixels == pixels && check_ring_bitmap(&bitmap, 16) == 0,
          "scratch allocation failure damaged the previous bitmap");
    CHECK(failing.blocks == 0, "failed scratch allocation retained a block");
    rt_arena_destroy(&failing);

    default_block_size = scratch.default_block_size;
    scratch.default_block_size = SIZE_MAX - 15u;
    CHECK(make_ring_outline(&outline, 128) == 0 &&
          fr_raster_render(&bitmap, &outline, &scratch) != 0 &&
          bitmap.pixels == pixels && check_ring_bitmap(&bitmap, 16) == 0,
          "partial scratch allocation failure damaged the previous bitmap");
    CHECK(make_ring_outline(&outline, 16) == 0 &&
          fr_raster_render(&bitmap, &outline, &scratch) == 0 &&
          check_ring_bitmap(&bitmap, 16) == 0 && scratch.blocks == blocks,
          "scratch was not reset after partial allocation failure");
    scratch.default_block_size = default_block_size;

    CHECK(make_ring_outline(&outline, 128) == 0 &&
          fr_raster_render(&bitmap, &outline, &scratch) == 0 &&
          check_ring_bitmap(&bitmap, 128) == 0, "scratch growth changed rendering");
    CHECK(scratch.blocks != blocks, "large raster did not grow scratch storage");
    blocks = scratch.blocks;
    saved = bitmap;
    fr_bitmap_init(&bitmap);
    CHECK(make_ring_outline(&outline, 16) == 0 &&
          fr_raster_render(&bitmap, &outline, &scratch) == 0 &&
          check_ring_bitmap(&bitmap, 16) == 0, "rendering after scratch growth failed");
    CHECK(scratch.blocks == blocks && check_ring_bitmap(&saved, 128) == 0,
          "scratch reuse changed an independently owned bitmap");

    pixels = bitmap.pixels;
    outline.contours[0].point_count = outline.point_count + 1;
    CHECK(fr_raster_render(&bitmap, &outline, &scratch) != 0 && bitmap.pixels == pixels,
          "invalid outline damaged the previous bitmap");
    CHECK(make_ring_outline(&outline, 8192) == 0 &&
          fr_raster_render(&bitmap, &outline, &scratch) != 0 &&
          bitmap.pixels == pixels && check_ring_bitmap(&bitmap, 16) == 0,
          "oversized raster damaged the previous bitmap");
    CHECK(make_ring_outline(&outline, 16) == 0 &&
          fr_raster_render(&bitmap, &outline, &scratch) == 0 &&
          check_ring_bitmap(&bitmap, 16) == 0, "scratch did not recover after rendering errors");

    fr_outline_reset(&outline);
    CHECK(fr_raster_render(&bitmap, &outline, &scratch) == 0 &&
          bitmap.pixels == 0 && bitmap.width == 0 && bitmap.height == 0,
          "empty raster retained pixels from an earlier render");
    CHECK(scratch.blocks == blocks, "empty raster discarded reusable scratch");
    rt_arena_destroy(&scratch);
    CHECK(scratch.blocks == 0 && check_ring_bitmap(&saved, 128) == 0,
          "destroying scratch damaged an independently owned bitmap");
    fr_bitmap_free(&saved);
    fr_bitmap_free(&bitmap);
    fr_outline_free(&outline);
    rt_write_line(1, "raster memory tests passed");
    return 0;
}

static int check_font(FrFont *font) {
    const uint32_t codepoints[] = {'A', '?', ' '};
    const FrGlyph *glyph;
    size_t i;

    CHECK(fr_font_line_height(font, 32) == 32, "incorrect line height");
    glyph = fr_font_get_glyph(font, 'A', 32, 0);
    CHECK(glyph != 0, "could not render compound glyph");
    CHECK(glyph->width == 16 && glyph->height == 16 &&
          glyph->left == 0 && glyph->top == 16 && glyph->advance == 20,
          "incorrect glyph metrics");
    CHECK(glyph->bitmap != 0, "missing glyph bitmap");
    for (i = 0; i < 256; ++i) {
        CHECK(glyph->bitmap[i] == 255, "incorrect glyph coverage");
    }
    CHECK(fr_font_get_glyph(font, 'A', 32, 0) == glyph, "glyph cache missed");
    CHECK(fr_font_prefetch_glyph(font, '?', 32, 0) == 0, "glyph prefetch failed");
    CHECK(fr_font_prefetch_codepoints(font, codepoints, 3, 32, 0) == 0,
          "batch prefetch failed");
    glyph = fr_font_get_glyph(font, 0x10ffffU, 32, 0);
    CHECK(glyph != 0 && glyph->codepoint == '?' && glyph->width == 16,
          "missing-glyph fallback failed");
    glyph = fr_font_get_glyph(font, ' ', 32, 0);
    CHECK(glyph != 0 && glyph->width == 0 && glyph->height == 0 &&
          glyph->bitmap == 0 && glyph->advance == 20, "empty glyph failed");
    CHECK(fr_font_get_glyph(font, 'A', 0, 0) == 0, "accepted zero pixel size");
    CHECK(fr_font_get_glyph(font, 0x110000U, 32, 0) == 0, "accepted invalid codepoint");
    return 0;
}

static int font_worker(void *arg) {
    FrFont *font = (FrFont *)arg;
    const uint32_t codepoints[] = {'A', '?', ' ', 0x10ffffU};
    unsigned int i;

    for (i = 0; i < 64; ++i) {
        if (fr_font_prefetch_codepoints(font, codepoints, 4, 16 + (int)(i % 17), (uint16_t)(i % 2)) != 0) {
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    unsigned char data[TEST_FONT_SIZE];
    unsigned char original[TEST_FONT_SIZE];
    FrTtfFont *ttf = 0;
    FrFont *font = 0;
    FrTtfInfo info;
    FrTtfGlyph raw_glyph;
    PlatformThread threads[4];
    unsigned int i;

    CHECK(argc == 2, "expected a scratch font path");
    CHECK(check_raster_memory() == 0, "raster memory regression failed");
    make_font(data);
    memcpy(original, data, sizeof(data));
    CHECK(fr_ttf_open_memory(&ttf, data, sizeof(data)) == FR_TTF_OK,
          "could not parse memory font without platform installation");
    CHECK(fr_ttf_get_info(ttf, &info) == FR_TTF_OK && info.units_per_em == 1024 &&
          info.num_glyphs == 4, "incorrect font info");
    CHECK(fr_ttf_get_glyph(ttf, 2, &raw_glyph) == FR_TTF_OK &&
          raw_glyph.number_of_contours == -1, "compound glyph was not parsed");
    fr_ttf_close(ttf);
    ttf = 0;

    for (i = 0; i < 4; ++i) {
        CHECK(fr_font_open_memory(&font, data, sizeof(data)) == 0,
              "could not open memory font");
        CHECK(check_font(font) == 0, "memory font rendering failed");
        fr_font_close(font);
        font = 0;
    }
    CHECK(memcmp(data, original, sizeof(data)) == 0, "borrowed font data changed");
    CHECK(tool_write_file_all(argv[1], data, sizeof(data)) == 0, "could not write fixture");
    CHECK(fr_font_open(&font, argv[1]) == 0, "could not open file font");
    CHECK(platform_remove_file(argv[1]) == 0, "font file was left open");
    CHECK(check_font(font) == 0, "file font rendering failed");
    if (platform_worker_threads_supported()) {
        for (i = 0; i < 4; ++i) {
            CHECK(platform_thread_start(&threads[i], font_worker, font, 512U * 1024U) == 0,
                  "could not start font worker");
        }
        for (i = 0; i < 4; ++i) {
            int result = -1;
            CHECK(platform_thread_join(&threads[i], &result) == 0 && result == 0,
                  "concurrent glyph prefetch failed");
        }
        CHECK(check_font(font) == 0, "concurrent prefetch corrupted the glyph cache");
        rt_write_line(1, "concurrent glyph prefetch passed");
    }
    fr_font_close(font);
    font = 0;
    CHECK(fr_ttf_open(&ttf, argv[1]) == FR_TTF_ERR_IO && ttf == 0,
          "missing file did not report an I/O error");
    CHECK(tool_write_file_all(argv[1], data, 0) == 0, "could not write empty fixture");
    CHECK(fr_ttf_open(&ttf, argv[1]) == FR_TTF_ERR_MALFORMED && ttf == 0,
          "empty font was accepted");
    CHECK(tool_write_file_all(argv[1], data, 8) == 0, "could not write truncated fixture");
    CHECK(fr_ttf_open(&ttf, argv[1]) == FR_TTF_ERR_MALFORMED && ttf == 0,
          "truncated font was accepted");
    CHECK(fr_ttf_open_memory(&ttf, data, 8) == FR_TTF_ERR_MALFORMED && ttf == 0,
          "truncated memory font was accepted");
    tool_store_u32_be(data + TEST_CMAP + 48, 0);
    CHECK(fr_font_open_memory(&font, data, sizeof(data)) == 0, "could not open font without fallback");
    CHECK(fr_font_get_glyph(font, 0x10ffffU, 32, 0) == 0, "missing fallback was ignored");
    fr_font_close(font);
    font = 0;
    memcpy(data, original, sizeof(data));
    data[TEST_HEAD + 12] = 0;
    CHECK(fr_font_open_memory(&font, data, sizeof(data)) != 0 && font == 0,
          "malformed font was accepted");
    CHECK(fr_font_get_glyph(0, 'A', 32, 0) == 0 &&
          fr_font_prefetch_glyph(0, 'A', 32, 0) != 0, "null font was accepted");
    CHECK(platform_remove_file(argv[1]) == 0, "could not remove fixture");
    rt_write_line(1, "fontrender tests passed");
    return 0;
}
