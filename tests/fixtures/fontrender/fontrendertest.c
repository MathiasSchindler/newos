#include "fontrender/font_backend.h"
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
