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
    size_t i;

    for (i = 0; i < range_count; ++i) {
        if (codepoint >= ranges[i].first && codepoint <= ranges[i].last) {
            return 1;
        }
    }

    return 0;
}

int rt_utf8_decode(const char *text, size_t text_length, size_t *index_io, unsigned int *codepoint_out) {
    size_t index;
    unsigned char lead;
    size_t length;
    unsigned int codepoint;

    if (text == 0 || index_io == 0 || codepoint_out == 0) {
        return -1;
    }

    index = *index_io;
    if (index >= text_length) {
        return -1;
    }

    lead = (unsigned char)text[index];
    length = rt_utf8_sequence_length(lead);
    if (length == 0U || index + length > text_length) {
        *index_io = index + 1U;
        *codepoint_out = 0xfffdU;
        return -1;
    }

    if (length == 1U) {
        *index_io = index + 1U;
        *codepoint_out = (unsigned int)lead;
        return 0;
    }

    if (length == 2U) {
        unsigned char b1 = (unsigned char)text[index + 1U];
        if ((b1 & 0xc0U) != 0x80U) {
            *index_io = index + 1U;
            *codepoint_out = 0xfffdU;
            return -1;
        }
        codepoint = ((unsigned int)(lead & 0x1fU) << 6) | (unsigned int)(b1 & 0x3fU);
        if (codepoint < 0x80U) {
            *index_io = index + 1U;
            *codepoint_out = 0xfffdU;
            return -1;
        }
    } else if (length == 3U) {
        unsigned char b1 = (unsigned char)text[index + 1U];
        unsigned char b2 = (unsigned char)text[index + 2U];
        if ((b1 & 0xc0U) != 0x80U || (b2 & 0xc0U) != 0x80U) {
            *index_io = index + 1U;
            *codepoint_out = 0xfffdU;
            return -1;
        }
        codepoint = ((unsigned int)(lead & 0x0fU) << 12) |
                    ((unsigned int)(b1 & 0x3fU) << 6) |
                    (unsigned int)(b2 & 0x3fU);
        if (codepoint < 0x800U || (codepoint >= 0xd800U && codepoint <= 0xdfffU)) {
            *index_io = index + 1U;
            *codepoint_out = 0xfffdU;
            return -1;
        }
    } else {
        unsigned char b1 = (unsigned char)text[index + 1U];
        unsigned char b2 = (unsigned char)text[index + 2U];
        unsigned char b3 = (unsigned char)text[index + 3U];
        if ((b1 & 0xc0U) != 0x80U || (b2 & 0xc0U) != 0x80U || (b3 & 0xc0U) != 0x80U) {
            *index_io = index + 1U;
            *codepoint_out = 0xfffdU;
            return -1;
        }
        codepoint = ((unsigned int)(lead & 0x07U) << 18) |
                    ((unsigned int)(b1 & 0x3fU) << 12) |
                    ((unsigned int)(b2 & 0x3fU) << 6) |
                    (unsigned int)(b3 & 0x3fU);
        if (codepoint < 0x10000U || codepoint > 0x10ffffU) {
            *index_io = index + 1U;
            *codepoint_out = 0xfffdU;
            return -1;
        }
    }

    *index_io = index + length;
    *codepoint_out = codepoint;
    return 0;
}

int rt_utf8_validate(const char *text, size_t text_length) {
    size_t index = 0;

    while (index < text_length) {
        unsigned int codepoint = 0;
        if (rt_utf8_decode(text, text_length, &index, &codepoint) != 0) {
            return -1;
        }
    }

    return 0;
}

unsigned long long rt_utf8_codepoint_count(const char *text, size_t text_length) {
    unsigned long long count = 0;
    size_t index = 0;

    while (text != 0 && index < text_length) {
        unsigned int codepoint = 0;
        size_t before = index;
        (void)rt_utf8_decode(text, text_length, &index, &codepoint);
        if (index == before) {
            index += 1U;
        }
        count += 1ULL;
    }

    return count;
}

int rt_utf8_encode(unsigned int codepoint, char *buffer, size_t buffer_size, size_t *length_out) {
    if (buffer == 0 || buffer_size == 0 || length_out == 0 ||
        (codepoint >= 0xd800U && codepoint <= 0xdfffU) || codepoint > 0x10ffffU) {
        return -1;
    }

    if (codepoint <= 0x7fU) {
        if (buffer_size < 1U) {
            return -1;
        }
        buffer[0] = (char)codepoint;
        *length_out = 1U;
        return 0;
    }

    if (codepoint <= 0x7ffU) {
        if (buffer_size < 2U) {
            return -1;
        }
        buffer[0] = (char)(0xc0U | (codepoint >> 6));
        buffer[1] = (char)(0x80U | (codepoint & 0x3fU));
        *length_out = 2U;
        return 0;
    }

    if (codepoint <= 0xffffU) {
        if (buffer_size < 3U) {
            return -1;
        }
        buffer[0] = (char)(0xe0U | (codepoint >> 12));
        buffer[1] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
        buffer[2] = (char)(0x80U | (codepoint & 0x3fU));
        *length_out = 3U;
        return 0;
    }

    if (buffer_size < 4U) {
        return -1;
    }
    buffer[0] = (char)(0xf0U | (codepoint >> 18));
    buffer[1] = (char)(0x80U | ((codepoint >> 12) & 0x3fU));
    buffer[2] = (char)(0x80U | ((codepoint >> 6) & 0x3fU));
    buffer[3] = (char)(0x80U | (codepoint & 0x3fU));
    *length_out = 4U;
    return 0;
}

unsigned int rt_unicode_simple_fold(unsigned int codepoint) {
    if (codepoint >= 'A' && codepoint <= 'Z') {
        return codepoint + 32U;
    }
    if ((codepoint >= 0x00c0U && codepoint <= 0x00d6U) ||
        (codepoint >= 0x00d8U && codepoint <= 0x00deU)) {
        return codepoint + 32U;
    }
    if (codepoint >= 0x0100U && codepoint <= 0x017fU) {
        if ((codepoint & 1U) == 0U) {
            return codepoint + 1U;
        }
        return codepoint;
    }
    if (codepoint >= 0x0391U && codepoint <= 0x03abU && codepoint != 0x03a2U) {
        return codepoint + 32U;
    }
    switch (codepoint) {
        case 0x0178U: return 0x00ffU;
        case 0x0181U: return 0x0253U;
        case 0x0186U: return 0x0254U;
        case 0x0189U: return 0x0256U;
        case 0x018aU: return 0x0257U;
        case 0x018eU: return 0x01ddU;
        case 0x018fU: return 0x0259U;
        case 0x0190U: return 0x025bU;
        case 0x0193U: return 0x0260U;
        case 0x0194U: return 0x0263U;
        case 0x0196U: return 0x0269U;
        case 0x0197U: return 0x0268U;
        case 0x019cU: return 0x026fU;
        case 0x019dU: return 0x0272U;
        case 0x019fU: return 0x0275U;
        case 0x01a6U: return 0x0280U;
        case 0x01a7U: return 0x01a8U;
        case 0x01acU: return 0x01adU;
        case 0x01aeU: return 0x0288U;
        case 0x01afU: return 0x01b0U;
        case 0x01b1U: return 0x028aU;
        case 0x01b2U: return 0x028bU;
        case 0x01b7U: return 0x0292U;
        case 0x03cfU: return 0x03d7U;
        default: return codepoint;
    }
}

int rt_unicode_is_space(unsigned int codepoint) {
    if (codepoint == ' ' || codepoint == '\t' || codepoint == '\n' || codepoint == '\r' ||
        codepoint == '\v' || codepoint == '\f' || codepoint == 0x85U || codepoint == 0xa0U ||
        codepoint == 0x1680U || codepoint == 0x2028U || codepoint == 0x2029U ||
        codepoint == 0x202fU || codepoint == 0x205fU || codepoint == 0x3000U) {
        return 1;
    }

    return codepoint >= 0x2000U && codepoint <= 0x200aU;
}

int rt_unicode_is_word(unsigned int codepoint) {
    if ((codepoint >= 'a' && codepoint <= 'z') ||
        (codepoint >= 'A' && codepoint <= 'Z') ||
        (codepoint >= '0' && codepoint <= '9') ||
        codepoint == '_') {
        return 1;
    }

    if (codepoint < 0x80U || rt_unicode_is_space(codepoint)) {
        return 0;
    }

    return rt_unicode_display_width(codepoint) > 0U;
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
    {0x1f890U, 0x1f8adU}, {0x1f900U, 0x1f978U}, {0x1f97aU, 0x1f9cbU}, {0x1f9cdU, 0x1fa53U},
    {0x1fa70U, 0x1fa7cU}, {0x1fa80U, 0x1fa88U}, {0x1fa90U, 0x1fabdU}, {0x1fabfU, 0x1fac5U},
    {0x1faceU, 0x1fadbU}, {0x1fae0U, 0x1fae8U}, {0x1faf0U, 0x1faf8U}, {0x20000U, 0x2fffdU},
    {0x30000U, 0x3fffdU}
};

unsigned int rt_unicode_display_width(unsigned int codepoint) {
    if (codepoint == 0U) {
        return 0U;
    }
    if ((codepoint < 32U) || (codepoint >= 0x7fU && codepoint < 0xa0U)) {
        return 0U;
    }
    if (codepoint_in_ranges(codepoint, unicode_zero_width_ranges, sizeof(unicode_zero_width_ranges) / sizeof(unicode_zero_width_ranges[0]))) {
        return 0U;
    }
    if (codepoint_in_ranges(codepoint, unicode_wide_ranges, sizeof(unicode_wide_ranges) / sizeof(unicode_wide_ranges[0]))) {
        return 2U;
    }

    return 1U;
}
