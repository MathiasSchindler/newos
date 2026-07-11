#include "runtime.h"

typedef struct {
    unsigned int first;
    unsigned int last;
} UnicodeRange;

static size_t rt_utf8_sequence_length(unsigned char lead) {
    if ((lead & 0x80U) == 0U) {
        return 1U;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2U;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3U;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return 4U;
    }
    return 0U;
}

static int codepoint_in_ranges(unsigned int codepoint, const UnicodeRange *ranges, size_t range_count) {
    size_t low = 0U;
    size_t high = range_count;

    while (low < high) {
        size_t mid = low + (high - low) / 2U;

        if (codepoint < ranges[mid].first) {
            high = mid;
        } else if (codepoint > ranges[mid].last) {
            low = mid + 1U;
        } else {
            return 1;
        }
    }

    return 0;
}

static const UnicodeRange unicode_zero_width_ranges[] = {
    {0x0300U, 0x036fU}, {0x0483U, 0x0489U}, {0x0591U, 0x05bdU}, {0x05bfU, 0x05bfU},
        {0x05c1U, 0x05c2U}, {0x05c4U, 0x05c5U}, {0x05c7U, 0x05c7U}, {0x0610U, 0x061aU},
        {0x064bU, 0x065fU}, {0x0670U, 0x0670U}, {0x06d6U, 0x06dcU}, {0x06dfU, 0x06e4U},
        {0x06e7U, 0x06e8U}, {0x06eaU, 0x06edU}, {0x0711U, 0x0711U}, {0x0730U, 0x074aU},
        {0x07a6U, 0x07b0U}, {0x07ebU, 0x07f3U}, {0x0816U, 0x0819U}, {0x081bU, 0x0823U},
        {0x0825U, 0x0827U}, {0x0829U, 0x082dU}, {0x0859U, 0x085bU}, {0x08d3U, 0x08e1U},
        {0x08e3U, 0x0903U}, {0x093aU, 0x093cU}, {0x093eU, 0x094fU}, {0x0951U, 0x0957U},
        {0x0962U, 0x0963U}, {0x0981U, 0x0983U}, {0x09bcU, 0x09bcU}, {0x09beU, 0x09c4U},
        {0x09c7U, 0x09c8U}, {0x09cbU, 0x09cdU}, {0x0a01U, 0x0a03U}, {0x0a3cU, 0x0a3cU},
        {0x0a3eU, 0x0a42U}, {0x0a47U, 0x0a48U}, {0x0a4bU, 0x0a4dU}, {0x0abcU, 0x0abcU},
        {0x0abeU, 0x0ac5U}, {0x0ac7U, 0x0ac9U}, {0x0acbU, 0x0acdU}, {0x0b01U, 0x0b03U},
        {0x0b3cU, 0x0b3cU}, {0x0b3eU, 0x0b44U}, {0x0b47U, 0x0b48U}, {0x0b4bU, 0x0b4dU},
        {0x0b56U, 0x0b57U}, {0x0bbeU, 0x0bc2U}, {0x0bc6U, 0x0bc8U}, {0x0bcaU, 0x0bcdU},
        {0x0c00U, 0x0c04U}, {0x0c3eU, 0x0c44U}, {0x0c46U, 0x0c48U}, {0x0c4aU, 0x0c4dU},
        {0x0c55U, 0x0c56U}, {0x0c81U, 0x0c83U}, {0x0cbcU, 0x0cbcU}, {0x0cbeU, 0x0cc4U},
        {0x0cc6U, 0x0cc8U}, {0x0ccaU, 0x0ccdU}, {0x0d00U, 0x0d03U}, {0x0d3bU, 0x0d3cU},
        {0x0d3eU, 0x0d44U}, {0x0d46U, 0x0d48U}, {0x0d4aU, 0x0d4dU}, {0x0d57U, 0x0d57U},
        {0x0e31U, 0x0e31U}, {0x0e34U, 0x0e3aU}, {0x0e47U, 0x0e4eU}, {0x0eb1U, 0x0eb1U},
        {0x0eb4U, 0x0ebcU}, {0x0ec8U, 0x0ecdU}, {0x0f18U, 0x0f19U}, {0x0f35U, 0x0f35U},
        {0x0f37U, 0x0f37U}, {0x0f39U, 0x0f39U}, {0x0f71U, 0x0f84U}, {0x0f86U, 0x0f87U},
        {0x0f8dU, 0x0fbcU}, {0x0fc6U, 0x0fc6U}, {0x102bU, 0x103eU}, {0x1056U, 0x1059U},
        {0x105eU, 0x1060U}, {0x1062U, 0x1064U}, {0x1067U, 0x106dU}, {0x1071U, 0x1074U},
        {0x1082U, 0x108dU}, {0x109aU, 0x109dU}, {0x135dU, 0x135fU}, {0x1712U, 0x1715U},
        {0x1732U, 0x1734U}, {0x1752U, 0x1753U}, {0x1772U, 0x1773U}, {0x17b4U, 0x17d3U},
        {0x180bU, 0x180fU}, {0x1885U, 0x1886U}, {0x18a9U, 0x18a9U}, {0x1920U, 0x192bU},
        {0x1930U, 0x193bU}, {0x1a17U, 0x1a1bU}, {0x1a55U, 0x1a5eU}, {0x1a60U, 0x1a7cU},
        {0x1a7fU, 0x1a7fU}, {0x1ab0U, 0x1aceU}, {0x1b00U, 0x1b04U}, {0x1b34U, 0x1b44U},
        {0x1b6bU, 0x1b73U}, {0x1b80U, 0x1b82U}, {0x1ba1U, 0x1badU}, {0x1be6U, 0x1bf3U},
        {0x1c24U, 0x1c37U}, {0x1cd0U, 0x1cd2U}, {0x1cd4U, 0x1ce8U}, {0x1cedU, 0x1cedU},
        {0x1cf2U, 0x1cf4U}, {0x1cf7U, 0x1cf9U}, {0x1dc0U, 0x1dffU}, {0x200bU, 0x200fU},
        {0x202aU, 0x202eU}, {0x2060U, 0x2064U}, {0x2066U, 0x206fU}, {0xfe00U, 0xfe0fU},
        {0xfe20U, 0xfe2fU}, {0xfff9U, 0xfffbU}, {0x101fdU, 0x101fdU}, {0x102e0U, 0x102e0U},
        {0x10a01U, 0x10a03U}, {0x10a05U, 0x10a06U}, {0x10a0cU, 0x10a0fU}, {0x10a38U, 0x10a3aU},
        {0x10a3fU, 0x10a3fU}, {0x10ae5U, 0x10ae6U}, {0x10d24U, 0x10d27U}, {0x10eabU, 0x10eacU},
        {0x10f46U, 0x10f50U}, {0x11000U, 0x11002U}, {0x11038U, 0x11046U}, {0x11070U, 0x11082U},
        {0x110b0U, 0x110baU}, {0x11100U, 0x11102U}, {0x11127U, 0x11134U}, {0x11145U, 0x11146U},
        {0x11173U, 0x11173U}, {0x11180U, 0x11182U}, {0x111b3U, 0x111c0U}, {0x111c9U, 0x111ccU},
        {0x1122cU, 0x11237U}, {0x112dfU, 0x112eaU}, {0x11300U, 0x11303U}, {0x1133bU, 0x1133cU},
        {0x1133eU, 0x11344U}, {0x11347U, 0x11348U}, {0x1134bU, 0x1134dU}, {0x11357U, 0x11357U},
        {0x11435U, 0x11446U}, {0x1145eU, 0x1145eU}, {0x11630U, 0x11640U}, {0x116abU, 0x116b7U},
        {0x16af0U, 0x16af4U}, {0x16b30U, 0x16b36U}, {0x1bc9dU, 0x1bc9eU}, {0x1d165U, 0x1d169U},
        {0x1d16dU, 0x1d172U}, {0x1d17bU, 0x1d182U}, {0x1d185U, 0x1d18bU}, {0x1d1aaU, 0x1d1adU},
        {0x1da00U, 0x1da36U}, {0x1da3bU, 0x1da6cU}, {0x1da75U, 0x1da75U}, {0x1da84U, 0x1da84U},
        {0x1da9bU, 0x1da9fU}, {0x1daa1U, 0x1daafU}, {0x1e000U, 0x1e02aU}, {0x1e130U, 0x1e136U},
        {0x1e2aeU, 0x1e2efU}, {0x1e8d0U, 0x1e8d6U}, {0x1e944U, 0x1e94aU}, {0xe0100U, 0xe01efU}
    };

static const UnicodeRange unicode_emoji_modifier_ranges[] = {
    {0x1f3fbU, 0x1f3ffU}
};

static const UnicodeRange unicode_wide_ranges[] = {
    {0x1100U, 0x115fU}, {0x231aU, 0x231bU}, {0x2329U, 0x232aU}, {0x23e9U, 0x23ecU},
    {0x23f0U, 0x23f0U}, {0x23f3U, 0x23f3U}, {0x25fdU, 0x25feU}, {0x2614U, 0x2615U},
    {0x2648U, 0x2653U}, {0x267fU, 0x267fU}, {0x2693U, 0x2693U}, {0x26a1U, 0x26a1U},
    {0x26aaU, 0x26abU}, {0x26bdU, 0x26beU}, {0x26c4U, 0x26c5U}, {0x26ceU, 0x26ceU},
    {0x26d4U, 0x26d4U}, {0x26eaU, 0x26eaU}, {0x26f2U, 0x26f3U}, {0x26f5U, 0x26f5U},
    {0x26faU, 0x26faU}, {0x26fdU, 0x26fdU}, {0x2705U, 0x2705U}, {0x270aU, 0x270bU},
    {0x2728U, 0x2728U}, {0x274cU, 0x274cU}, {0x274eU, 0x274eU}, {0x2753U, 0x2755U},
    {0x2757U, 0x2757U}, {0x2795U, 0x2797U}, {0x27b0U, 0x27b0U}, {0x27bfU, 0x27bfU},
    {0x2b1bU, 0x2b1cU}, {0x2b50U, 0x2b50U}, {0x2b55U, 0x2b55U}, {0x2e80U, 0x303eU},
    {0x3040U, 0xa4cfU}, {0xac00U, 0xd7a3U}, {0xf900U, 0xfaffU}, {0xfe10U, 0xfe19U},
    {0xfe30U, 0xfe6fU}, {0xff00U, 0xff60U}, {0xffe0U, 0xffe6U}, {0x16fe0U, 0x16fe4U},
    {0x17000U, 0x187f7U}, {0x18800U, 0x18cd5U}, {0x1f004U, 0x1f004U}, {0x1f0cfU, 0x1f0cfU},
    {0x1f18eU, 0x1f18eU}, {0x1f191U, 0x1f19aU}, {0x1f200U, 0x1f251U}, {0x1f300U, 0x1f64fU},
    {0x1f680U, 0x1f6ffU}, {0x1f700U, 0x1f773U}, {0x1f780U, 0x1f7d8U}, {0x1f7e0U, 0x1f7ebU},
    {0x1f800U, 0x1f80bU}, {0x1f810U, 0x1f847U}, {0x1f850U, 0x1f859U}, {0x1f860U, 0x1f887U},
    {0x1f890U, 0x1f8adU}, {0x1f900U, 0x1f9ffU}, {0x1fa70U, 0x1faffU}, {0x20000U, 0x2fffdU},
    {0x30000U, 0x3fffdU}
};

static const UnicodeRange unicode_ambiguous_ranges[] = {
    {0x00a1U, 0x00a1U}, {0x00a4U, 0x00a4U}, {0x00a7U, 0x00a8U}, {0x00aaU, 0x00aaU},
    {0x00adU, 0x00aeU}, {0x00b0U, 0x00b4U}, {0x00b6U, 0x00baU}, {0x00bcU, 0x00bfU},
    {0x00c6U, 0x00c6U}, {0x00d0U, 0x00d0U}, {0x00d7U, 0x00d8U}, {0x00deU, 0x00e1U},
    {0x00e6U, 0x00e6U}, {0x00e8U, 0x00eaU}, {0x00ecU, 0x00edU}, {0x00f0U, 0x00f0U},
    {0x00f2U, 0x00f3U}, {0x00f7U, 0x00faU}, {0x00fcU, 0x00fcU}, {0x00feU, 0x00feU},
    {0x0101U, 0x0101U}, {0x0111U, 0x0111U}, {0x0113U, 0x0113U}, {0x011bU, 0x011bU},
    {0x0126U, 0x0127U}, {0x012bU, 0x012bU}, {0x0131U, 0x0133U}, {0x0138U, 0x0138U},
    {0x013fU, 0x0142U}, {0x0144U, 0x0144U}, {0x0148U, 0x014bU}, {0x014dU, 0x014dU},
    {0x0152U, 0x0153U}, {0x0166U, 0x0167U}, {0x016bU, 0x016bU}, {0x01ceU, 0x01ceU},
    {0x01d0U, 0x01d0U}, {0x01d2U, 0x01d2U}, {0x01d4U, 0x01d4U}, {0x01d6U, 0x01d6U},
    {0x01d8U, 0x01d8U}, {0x01daU, 0x01daU}, {0x01dcU, 0x01dcU}, {0x0251U, 0x0251U},
    {0x0261U, 0x0261U}, {0x02c4U, 0x02c4U}, {0x02c7U, 0x02c7U}, {0x02c9U, 0x02cbU},
    {0x02cdU, 0x02cdU}, {0x02d0U, 0x02d0U}, {0x02d8U, 0x02dbU}, {0x02ddU, 0x02ddU},
    {0x02dfU, 0x02dfU}, {0x0300U, 0x036fU}, {0x0391U, 0x03a1U}, {0x03a3U, 0x03a9U},
    {0x03b1U, 0x03c1U}, {0x03c3U, 0x03c9U}, {0x0401U, 0x0401U}, {0x0410U, 0x044fU},
    {0x0451U, 0x0451U}, {0x2010U, 0x2010U}, {0x2013U, 0x2016U}, {0x2018U, 0x2019U},
    {0x201cU, 0x201dU}, {0x2020U, 0x2022U}, {0x2024U, 0x2027U}, {0x2030U, 0x2030U},
    {0x2032U, 0x2033U}, {0x2035U, 0x2035U}, {0x203bU, 0x203bU}, {0x203eU, 0x203eU},
    {0x2074U, 0x2074U}, {0x207fU, 0x207fU}, {0x2081U, 0x2084U}, {0x20acU, 0x20acU},
    {0x2103U, 0x2103U}, {0x2105U, 0x2105U}, {0x2109U, 0x2109U}, {0x2113U, 0x2113U},
    {0x2116U, 0x2116U}, {0x2121U, 0x2122U}, {0x2126U, 0x2126U}, {0x212bU, 0x212bU},
    {0x2153U, 0x2154U}, {0x215bU, 0x215eU}, {0x2160U, 0x216bU}, {0x2170U, 0x2179U},
    {0x2189U, 0x2189U}, {0x2190U, 0x2199U}, {0x21b8U, 0x21b9U}, {0x21d2U, 0x21d2U},
    {0x21d4U, 0x21d4U}, {0x21e7U, 0x21e7U}, {0x2200U, 0x2200U}, {0x2202U, 0x2203U},
    {0x2207U, 0x2208U}, {0x220bU, 0x220bU}, {0x220fU, 0x220fU}, {0x2211U, 0x2211U},
    {0x2215U, 0x2215U}, {0x221aU, 0x221aU}, {0x221dU, 0x2220U}, {0x2223U, 0x2223U},
    {0x2225U, 0x2225U}, {0x2227U, 0x222cU}, {0x222eU, 0x222eU}, {0x2234U, 0x2237U},
    {0x223cU, 0x223dU}, {0x2248U, 0x2248U}, {0x224cU, 0x224cU}, {0x2252U, 0x2252U},
    {0x2260U, 0x2261U}, {0x2264U, 0x2267U}, {0x226aU, 0x226bU}, {0x226eU, 0x226fU},
    {0x2282U, 0x2283U}, {0x2286U, 0x2287U}, {0x2295U, 0x2295U}, {0x2299U, 0x2299U},
    {0x22a5U, 0x22a5U}, {0x22bfU, 0x22bfU}, {0x2312U, 0x2312U}, {0x2460U, 0x24e9U},
    {0x24ebU, 0x254bU}, {0x2550U, 0x2573U}, {0x2580U, 0x258fU}, {0x2592U, 0x2595U},
    {0x25a0U, 0x25a1U}, {0x25a3U, 0x25a9U}, {0x25b2U, 0x25b3U}, {0x25b6U, 0x25b7U},
    {0x25bcU, 0x25bdU}, {0x25c0U, 0x25c1U}, {0x25c6U, 0x25c8U}, {0x25cbU, 0x25cbU},
    {0x25ceU, 0x25d1U}, {0x25e2U, 0x25e5U}, {0x25efU, 0x25efU}, {0x2605U, 0x2606U},
    {0x2609U, 0x2609U}, {0x260eU, 0x260fU}, {0x261cU, 0x261cU}, {0x261eU, 0x261eU},
    {0x2640U, 0x2640U}, {0x2642U, 0x2642U}, {0x2660U, 0x2661U}, {0x2663U, 0x2665U},
    {0x2667U, 0x266aU}, {0x266cU, 0x266dU}, {0x266fU, 0x266fU}, {0x269eU, 0x269fU},
    {0x26bfU, 0x26bfU}, {0x26c6U, 0x26cdU}, {0x26cfU, 0x26d3U}, {0x26d5U, 0x26e1U},
    {0x26e3U, 0x26e3U}, {0x26e8U, 0x26e9U}, {0x26ebU, 0x26f1U}, {0x26f4U, 0x26f4U},
    {0x26f6U, 0x26f9U}, {0x26fbU, 0x26fcU}, {0x26feU, 0x26ffU}, {0x273dU, 0x273dU},
    {0x2776U, 0x277fU}, {0x2b56U, 0x2b59U}, {0x3248U, 0x324fU}, {0xe000U, 0xf8ffU},
    {0xfe00U, 0xfe0fU}, {0xfffdU, 0xfffdU}, {0x1f100U, 0x1f10aU}, {0x1f110U, 0x1f12dU},
    {0x1f130U, 0x1f169U}, {0x1f170U, 0x1f18dU}, {0x1f18fU, 0x1f190U}, {0x1f19bU, 0x1f1acU},
    {0xe0100U, 0xe01efU}, {0xf0000U, 0xffffdU}, {0x100000U, 0x10fffdU}
};

unsigned int rt_unicode_display_width_mode(unsigned int codepoint, unsigned int ambiguous_width) {
    if (codepoint == 0U) {
        return 0U;
    }
    if ((codepoint < 32U) || (codepoint >= 0x7fU && codepoint < 0xa0U)) {
        return 0U;
    }
    if (codepoint_in_ranges(codepoint, unicode_emoji_modifier_ranges, sizeof(unicode_emoji_modifier_ranges) / sizeof(unicode_emoji_modifier_ranges[0]))) {
        return 0U;
    }
    if (codepoint_in_ranges(codepoint, unicode_zero_width_ranges, sizeof(unicode_zero_width_ranges) / sizeof(unicode_zero_width_ranges[0]))) {
        return 0U;
    }
    if (codepoint_in_ranges(codepoint, unicode_wide_ranges, sizeof(unicode_wide_ranges) / sizeof(unicode_wide_ranges[0]))) {
        return 2U;
    }
    if (ambiguous_width == 2U &&
        codepoint_in_ranges(codepoint, unicode_ambiguous_ranges, sizeof(unicode_ambiguous_ranges) / sizeof(unicode_ambiguous_ranges[0]))) {
        return 2U;
    }

    return 1U;
}

unsigned int rt_unicode_display_width(unsigned int codepoint) {
    return rt_unicode_display_width_mode(codepoint, 1U);
}

static int rt_unicode_is_grapheme_extend(unsigned int codepoint) {
    return codepoint == 0x200dU ||
           codepoint_in_ranges(codepoint, unicode_emoji_modifier_ranges, sizeof(unicode_emoji_modifier_ranges) / sizeof(unicode_emoji_modifier_ranges[0])) ||
           codepoint_in_ranges(codepoint, unicode_zero_width_ranges, sizeof(unicode_zero_width_ranges) / sizeof(unicode_zero_width_ranges[0]));
}

static int rt_unicode_is_regional_indicator(unsigned int codepoint) {
    return codepoint >= 0x1f1e6U && codepoint <= 0x1f1ffU;
}

static int rt_unicode_is_control(unsigned int codepoint) {
    return codepoint == '\r' || codepoint == '\n' || codepoint < 0x20U ||
           (codepoint >= 0x7fU && codepoint < 0xa0U);
}

static int rt_unicode_is_hangul_l(unsigned int codepoint) {
    return (codepoint >= 0x1100U && codepoint <= 0x115fU) || (codepoint >= 0xa960U && codepoint <= 0xa97cU);
}

static int rt_unicode_is_hangul_v(unsigned int codepoint) {
    return (codepoint >= 0x1160U && codepoint <= 0x11a7U) || (codepoint >= 0xd7b0U && codepoint <= 0xd7c6U);
}

static int rt_unicode_is_hangul_t(unsigned int codepoint) {
    return (codepoint >= 0x11a8U && codepoint <= 0x11ffU) || (codepoint >= 0xd7cbU && codepoint <= 0xd7fbU);
}

static int rt_unicode_hangul_no_break(unsigned int previous, unsigned int current) {
    unsigned int syllable_index;
    int previous_lv = 0;
    int previous_lvt = 0;

    if (previous >= 0xac00U && previous <= 0xd7a3U) {
        syllable_index = previous - 0xac00U;
        previous_lv = (syllable_index % 28U) == 0U;
        previous_lvt = !previous_lv;
    }
    if (rt_unicode_is_hangul_l(previous)) {
        return rt_unicode_is_hangul_l(current) || rt_unicode_is_hangul_v(current) ||
               (current >= 0xac00U && current <= 0xd7a3U);
    }
    if (rt_unicode_is_hangul_v(previous) || previous_lv) {
        return rt_unicode_is_hangul_v(current) || rt_unicode_is_hangul_t(current);
    }
    if (rt_unicode_is_hangul_t(previous) || previous_lvt) {
        return rt_unicode_is_hangul_t(current);
    }
    return 0;
}

static int rt_grapheme_decode_at(const char *text, size_t text_length, size_t start, size_t *end_out, unsigned int *codepoint_out) {
    size_t end = start;

    if (rt_utf8_decode(text, text_length, &end, codepoint_out) != 0) {
        *end_out = start + 1U;
        *codepoint_out = (unsigned char)text[start];
        return -1;
    }
    *end_out = end;
    return 0;
}

void rt_grapheme_state_reset(RtGraphemeState *state) {
    if (state != 0) rt_memset(state, 0, sizeof(*state));
}

int rt_grapheme_state_push(RtGraphemeState *state, unsigned int codepoint, unsigned int ambiguous_width, unsigned int *completed_width_out) {
    int include = 0;
    unsigned int width;

    if (state == 0 || completed_width_out == 0) return -1;
    *completed_width_out = 0U;
    width = rt_unicode_display_width_mode(codepoint, ambiguous_width);
    if (!state->active) {
        state->previous_codepoint = codepoint;
        state->display_width = width;
        state->regional_count = rt_unicode_is_regional_indicator(codepoint) ? 1U : 0U;
        state->after_joiner = codepoint == 0x200dU;
        state->active = 1U;
        return 0;
    }
    if (state->previous_codepoint == '\r' && codepoint == '\n') include = 1;
    else if (rt_unicode_is_control(state->previous_codepoint) || rt_unicode_is_control(codepoint)) include = 0;
    else if (rt_unicode_is_grapheme_extend(codepoint)) include = 1;
    else if (state->after_joiner) include = 1;
    else if (rt_unicode_hangul_no_break(state->previous_codepoint, codepoint)) include = 1;
    else if (rt_unicode_is_regional_indicator(state->previous_codepoint) && rt_unicode_is_regional_indicator(codepoint) &&
             (state->regional_count & 1U) != 0U) include = 1;

    if (!include) {
        *completed_width_out = state->display_width;
        state->display_width = width;
        state->regional_count = rt_unicode_is_regional_indicator(codepoint) ? 1U : 0U;
    } else {
        if (width > state->display_width) state->display_width = width;
        if (rt_unicode_is_regional_indicator(codepoint)) state->regional_count += 1U;
        else state->regional_count = 0U;
    }
    state->after_joiner = codepoint == 0x200dU;
    state->previous_codepoint = codepoint;
    return include ? 0 : 1;
}

unsigned int rt_grapheme_state_finish(RtGraphemeState *state) {
    unsigned int width;
    if (state == 0 || !state->active) return 0U;
    width = state->display_width;
    rt_grapheme_state_reset(state);
    return width;
}

int rt_grapheme_next_width(const char *text, size_t text_length, size_t start, unsigned int ambiguous_width, RtGraphemeCluster *cluster_out) {
    size_t end;
    unsigned int first;
    unsigned int previous;
    unsigned int width;
    unsigned int regional_count;
    int after_joiner = 0;

    if (text == 0 || cluster_out == 0 || start >= text_length) return -1;
    (void)rt_grapheme_decode_at(text, text_length, start, &end, &first);
    previous = first;
    width = rt_unicode_display_width_mode(first, ambiguous_width);
    regional_count = rt_unicode_is_regional_indicator(first) ? 1U : 0U;

    while (end < text_length) {
        size_t next_end;
        unsigned int current;
        int include = 0;

        (void)rt_grapheme_decode_at(text, text_length, end, &next_end, &current);
        if (previous == '\r' && current == '\n') include = 1;
        else if (rt_unicode_is_control(previous) || rt_unicode_is_control(current)) include = 0;
        else if (rt_unicode_is_grapheme_extend(current)) include = 1;
        else if (after_joiner) include = 1;
        else if (rt_unicode_hangul_no_break(previous, current)) include = 1;
        else if (rt_unicode_is_regional_indicator(previous) && rt_unicode_is_regional_indicator(current) && (regional_count & 1U) != 0U) include = 1;

        if (!include) break;
        if (rt_unicode_is_regional_indicator(current)) regional_count += 1U;
        else regional_count = 0U;
        if (rt_unicode_display_width_mode(current, ambiguous_width) > width) width = rt_unicode_display_width_mode(current, ambiguous_width);
        after_joiner = current == 0x200dU;
        previous = current;
        end = next_end;
    }

    cluster_out->start = start;
    cluster_out->end = end;
    cluster_out->first_codepoint = first;
    cluster_out->display_width = width;
    return 0;
}

int rt_grapheme_next(const char *text, size_t text_length, size_t start, RtGraphemeCluster *cluster_out) {
    return rt_grapheme_next_width(text, text_length, start, 1U, cluster_out);
}

int rt_grapheme_previous(const char *text, size_t text_length, size_t end, RtGraphemeCluster *cluster_out) {
    size_t index = 0U;
    RtGraphemeCluster cluster;

    if (text == 0 || cluster_out == 0 || end == 0U || end > text_length) return -1;
    while (index < end) {
        if (rt_grapheme_next(text, end, index, &cluster) != 0 || cluster.end <= index) return -1;
        if (cluster.end >= end) {
            *cluster_out = cluster;
            return cluster.end == end ? 0 : -1;
        }
        index = cluster.end;
    }
    return -1;
}

static int rt_text_is_ansi_final_byte(unsigned char ch) {
    return ch >= 0x40U && ch <= 0x7eU;
}

static int rt_text_ascii_fast_segment(unsigned char ch, size_t start, RtTextSegment *segment_out) {
    if (ch >= 0x80U || ch == '\033') {
        return 0;
    }

    segment_out->start = start;
    segment_out->end = start + 1U;
    segment_out->codepoint = (unsigned int)ch;
    segment_out->display_width = (ch >= ' ' && ch != 0x7fU) ? 1U : 0U;
    segment_out->flags = 0U;
    if (ch == '\b') {
        segment_out->flags = RT_TEXT_SEGMENT_BACKSPACE;
    } else if (ch == '\r') {
        segment_out->flags = RT_TEXT_SEGMENT_CARRIAGE_RETURN;
    }
    return 1;
}

static int rt_text_ansi_escape_end(const char *text, size_t text_length, size_t start, size_t *end_out, int *incomplete_out) {
    size_t index;

    if (start >= text_length || text[start] != '\033') {
        return 0;
    }
    *incomplete_out = 0;

    if (start + 1U >= text_length) {
        *end_out = text_length;
        *incomplete_out = 1;
        return 1;
    }

    if (text[start + 1U] == '[') {
        index = start + 2U;
        while (index < text_length) {
            if (rt_text_is_ansi_final_byte((unsigned char)text[index])) {
                *end_out = index + 1U;
                return 1;
            }
            index += 1U;
        }
        *end_out = text_length;
        *incomplete_out = 1;
        return 1;
    }

    if (text[start + 1U] == ']') {
        index = start + 2U;
        while (index < text_length) {
            if (text[index] == '\a') {
                *end_out = index + 1U;
                return 1;
            }
            if (text[index] == '\033' && index + 1U < text_length && text[index + 1U] == '\\') {
                *end_out = index + 2U;
                return 1;
            }
            index += 1U;
        }
        *end_out = text_length;
        *incomplete_out = 1;
        return 1;
    }

    *end_out = start + 2U;
    return 1;
}

int rt_text_next_segment_width(const char *text, size_t text_length, size_t start, unsigned int ambiguous_width, RtTextSegment *segment_out) {
    size_t end = start;
    int incomplete = 0;
    unsigned int codepoint = 0U;

    if (text == 0 || segment_out == 0 || start >= text_length) {
        return -1;
    }

    segment_out->start = start;
    segment_out->end = start + 1U;
    segment_out->codepoint = 0xfffdU;
    segment_out->display_width = 1U;
    segment_out->flags = 0U;

    if (rt_text_ascii_fast_segment((unsigned char)text[start], start, segment_out)) {
        return 0;
    }

    if (rt_text_ansi_escape_end(text, text_length, start, &end, &incomplete)) {
        segment_out->end = end;
        segment_out->codepoint = 0U;
        segment_out->display_width = 0U;
        segment_out->flags = RT_TEXT_SEGMENT_ANSI | (incomplete ? RT_TEXT_SEGMENT_INCOMPLETE : 0U);
        return 0;
    }

    end = start;
    if (rt_utf8_decode(text, text_length, &end, &codepoint) != 0) {
        unsigned char lead = (unsigned char)text[start];
        size_t expected_length = rt_utf8_sequence_length(lead);

        segment_out->end = start + 1U;
        segment_out->codepoint = 0xfffdU;
        segment_out->display_width = 1U;
        segment_out->flags = RT_TEXT_SEGMENT_INVALID;
        if (expected_length > 1U && start + expected_length > text_length) {
            segment_out->flags |= RT_TEXT_SEGMENT_INCOMPLETE;
        }
        return 0;
    }

    segment_out->end = end;
    segment_out->codepoint = codepoint;
    if (codepoint == '\b') {
        segment_out->display_width = 0U;
        segment_out->flags = RT_TEXT_SEGMENT_BACKSPACE;
    } else if (codepoint == '\r') {
        segment_out->display_width = 0U;
        segment_out->flags = RT_TEXT_SEGMENT_CARRIAGE_RETURN;
    } else if (codepoint == '\t') {
        segment_out->display_width = 0U;
    } else {
        segment_out->display_width = rt_unicode_display_width_mode(codepoint, ambiguous_width);
    }
    return 0;
}

int rt_text_next_segment(const char *text, size_t text_length, size_t start, RtTextSegment *segment_out) {
    return rt_text_next_segment_width(text, text_length, start, 1U, segment_out);
}

int rt_text_has_incomplete_tail(const char *text, size_t text_length) {
    size_t index = 0U;
    RtTextSegment segment;

    while (rt_text_next_segment(text, text_length, index, &segment) == 0) {
        if ((segment.flags & RT_TEXT_SEGMENT_INCOMPLETE) != 0U) {
            return 1;
        }
        index = segment.end;
    }
    return 0;
}

unsigned long long rt_text_apply_segment_width_tabstop(unsigned long long current_width, const RtTextSegment *segment, unsigned int tab_width) {
    unsigned long long tab;

    if (segment == 0) {
        return current_width;
    }
    if ((segment->flags & RT_TEXT_SEGMENT_BACKSPACE) != 0U) {
        return current_width > 0ULL ? current_width - 1ULL : 0ULL;
    }
    if ((segment->flags & RT_TEXT_SEGMENT_CARRIAGE_RETURN) != 0U) {
        return 0ULL;
    }
    if ((segment->flags & RT_TEXT_SEGMENT_ANSI) != 0U) {
        return current_width;
    }
    if (segment->codepoint == '\t') {
        tab = tab_width == 0U ? 8ULL : (unsigned long long)tab_width;
        return current_width + (tab - (current_width % tab));
    }
    return current_width + (unsigned long long)segment->display_width;
}

unsigned long long rt_text_apply_segment_width(unsigned long long current_width, const RtTextSegment *segment) {
    return rt_text_apply_segment_width_tabstop(current_width, segment, 8U);
}

unsigned long long rt_text_display_width_n_mode(const char *text, size_t text_length, unsigned long long initial_width, unsigned int tab_width, unsigned int ambiguous_width) {
    size_t index = 0U;
    unsigned long long width = initial_width;
    RtTextSegment segment;
    unsigned long long tab = tab_width == 0U ? 8ULL : (unsigned long long)tab_width;

    while (index < text_length) {
        unsigned char ch = (unsigned char)text[index];

        if (ch >= ' ' && ch < 0x7fU) {
            width += 1ULL;
            index += 1U;
            continue;
        }
        if (ch == '\t') {
            width += tab - (width % tab);
            index += 1U;
            continue;
        }
        if (ch == '\b') {
            width = width > 0ULL ? width - 1ULL : 0ULL;
            index += 1U;
            continue;
        }
        if (ch == '\r') {
            width = 0ULL;
            index += 1U;
            continue;
        }
        if (ch < 0x80U && ch != '\033') {
            index += 1U;
            continue;
        }
        if (rt_text_next_segment_width(text, text_length, index, ambiguous_width, &segment) != 0) {
            break;
        }
        width = rt_text_apply_segment_width_tabstop(width, &segment, tab_width);
        index = segment.end;
    }
    return width;
}

unsigned long long rt_text_display_width_n_tabstop(const char *text, size_t text_length, unsigned long long initial_width, unsigned int tab_width) {
    return rt_text_display_width_n_mode(text, text_length, initial_width, tab_width, 1U);
}

unsigned long long rt_text_display_width_n(const char *text, size_t text_length, unsigned long long initial_width) {
    return rt_text_display_width_n_tabstop(text, text_length, initial_width, 8U);
}

size_t rt_text_prefix_bytes_for_width_mode(const char *text, size_t text_length, unsigned long long max_width, unsigned long long initial_width, unsigned int ambiguous_width) {
    size_t index = 0U;
    size_t last_complete = 0U;
    unsigned long long width = initial_width;
    RtTextSegment segment;

    while (index < text_length) {
        unsigned char ch = (unsigned char)text[index];
        unsigned long long next_width;

        if (ch >= ' ' && ch < 0x7fU) {
            next_width = width + 1ULL;
            if (next_width > max_width) {
                break;
            }
            width = next_width;
            last_complete = index + 1U;
            index += 1U;
            continue;
        }
        if (rt_text_next_segment_width(text, text_length, index, ambiguous_width, &segment) != 0) {
            break;
        }
        next_width = rt_text_apply_segment_width(width, &segment);

        if (next_width > max_width && (segment.flags & (RT_TEXT_SEGMENT_ANSI | RT_TEXT_SEGMENT_BACKSPACE | RT_TEXT_SEGMENT_CARRIAGE_RETURN)) == 0U) {
            break;
        }
        width = next_width;
        last_complete = segment.end;
        index = segment.end;
    }
    return last_complete;
}

size_t rt_text_prefix_bytes_for_width(const char *text, size_t text_length, unsigned long long max_width, unsigned long long initial_width) {
    return rt_text_prefix_bytes_for_width_mode(text, text_length, max_width, initial_width, 1U);
}

int rt_text_segment_is_space(const char *text, size_t text_length, const RtTextSegment *segment) {
    (void)text;
    (void)text_length;
    if (segment == 0 || (segment->flags & RT_TEXT_SEGMENT_ANSI) != 0U) {
        return 0;
    }
    return rt_unicode_is_space(segment->codepoint);
}
