#include "internal.h"

static const ExpackLzssProfile expack_lzss_profiles[] = {
    {COMPRESSION_LZSS_PROFILE_WIDE_WINDOW, 3U, 0x07U},
    {COMPRESSION_LZSS_PROFILE_WIDE_MATCH, 4U, 0x0fU},
    {COMPRESSION_LZSS_PROFILE_MEDIUM_MATCH, 5U, 0x1fU},
    {COMPRESSION_LZSS_PROFILE_LONG_MATCH, 6U, 0x3fU}
};

static const unsigned char expack_stub_x86_64[] = {
    0x31, 0xed, 0x49, 0x89, 0xe4, 0x4c, 0x8b, 0x35, 0x5f, 0x01, 0x00, 0x00,
    0x31, 0xff, 0x4c, 0x89, 0xf6, 0xba, 0x03, 0x00, 0x00, 0x00, 0x41, 0xba,
    0x22, 0x00, 0x00, 0x00, 0x41, 0xb8, 0xff, 0xff, 0xff, 0xff, 0x45, 0x31,
    0xc9, 0xb8, 0x09, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x0f,
    0x88, 0x2a, 0x01, 0x00, 0x00, 0x49, 0x89, 0xc5, 0x4f, 0x8d, 0x7c, 0x35,
    0x00, 0x48, 0x8d, 0x1d, 0x3f, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x2d, 0x28,
    0x01, 0x00, 0x00, 0x48, 0x01, 0xdd, 0x4c, 0x89, 0xef, 0x45, 0x31, 0xc0,
    0x45, 0x31, 0xd2, 0x4c, 0x39, 0xff, 0x0f, 0x83, 0x9d, 0x00, 0x00, 0x00,
    0x45, 0x85, 0xc0, 0x75, 0x16, 0x48, 0x39, 0xeb, 0x0f, 0x83, 0xf1, 0x00,
    0x00, 0x00, 0x44, 0x0f, 0xb6, 0x13, 0x48, 0xff, 0xc3, 0x41, 0xb8, 0x01,
    0x00, 0x00, 0x00, 0x45, 0x84, 0xc2, 0x75, 0x15, 0x48, 0x39, 0xeb, 0x0f,
    0x83, 0xd6, 0x00, 0x00, 0x00, 0x8a, 0x03, 0x48, 0xff, 0xc3, 0x88, 0x07,
    0x48, 0xff, 0xc7, 0xeb, 0x50, 0x48, 0x8d, 0x43, 0x02, 0x48, 0x39, 0xe8,
    0x0f, 0x87, 0xbd, 0x00, 0x00, 0x00, 0x0f, 0xb6, 0x03, 0x0f, 0xb6, 0x4b,
    0x01, 0x48, 0x83, 0xc3, 0x02, 0x89, 0xca, 0xc1, 0xea, 0x03, 0xc1, 0xe2,
    0x08, 0x09, 0xc2, 0xff, 0xc2, 0x83, 0xe1, 0x07, 0x83, 0xc1, 0x03, 0x48,
    0x89, 0xfe, 0x48, 0x29, 0xd6, 0x4c, 0x39, 0xee, 0x0f, 0x82, 0x91, 0x00,
    0x00, 0x00, 0x4c, 0x39, 0xff, 0x0f, 0x83, 0x88, 0x00, 0x00, 0x00, 0x8a,
    0x06, 0x48, 0xff, 0xc6, 0x88, 0x07, 0x48, 0xff, 0xc7, 0xff, 0xc9, 0x75,
    0xe9, 0x41, 0xd1, 0xe0, 0x41, 0x81, 0xf8, 0x00, 0x01, 0x00, 0x00, 0x0f,
    0x85, 0x62, 0xff, 0xff, 0xff, 0x45, 0x31, 0xc0, 0xe9, 0x5a, 0xff, 0xff,
    0xff, 0x48, 0x39, 0xeb, 0x75, 0x5d, 0xb8, 0x3f, 0x01, 0x00, 0x00, 0x48,
    0x8d, 0x3d, 0x6d, 0x00, 0x00, 0x00, 0x31, 0xf6, 0x0f, 0x05, 0x48, 0x85,
    0xc0, 0x78, 0x48, 0x48, 0x89, 0xc3, 0x4c, 0x89, 0xee, 0x4c, 0x89, 0xf2,
    0x48, 0x85, 0xd2, 0x74, 0x16, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x89, 0xdf,
    0x0f, 0x05, 0x48, 0x85, 0xc0, 0x7e, 0x2c, 0x48, 0x01, 0xc6, 0x48, 0x29,
    0xc2, 0xeb, 0xe5, 0xb8, 0x42, 0x01, 0x00, 0x00, 0x89, 0xdf, 0x48, 0x8d,
    0x35, 0x39, 0x00, 0x00, 0x00, 0x49, 0x8d, 0x54, 0x24, 0x08, 0x4d, 0x8b,
    0x14, 0x24, 0x4f, 0x8d, 0x54, 0xd4, 0x10, 0x41, 0xb8, 0x00, 0x10, 0x00,
    0x00, 0x0f, 0x05, 0xb8, 0x3c, 0x00, 0x00, 0x00, 0xbf, 0x7f, 0x00, 0x00,
    0x00, 0x0f, 0x05, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x88,
    0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x65, 0x78, 0x70, 0x61, 0x63,
    0x6b, 0x00, 0x00
};

static const unsigned char expack_lzrep_stub_x86_64[] = {
    0x31, 0xed, 0x49, 0x89, 0xe4, 0x4c, 0x8b, 0x35, 0x6e, 0x01, 0x00, 0x00,
    0x31, 0xff, 0x4c, 0x89, 0xf6, 0xba, 0x03, 0x00, 0x00, 0x00, 0x41, 0xba,
    0x22, 0x00, 0x00, 0x00, 0x41, 0xb8, 0xff, 0xff, 0xff, 0xff, 0x45, 0x31,
    0xc9, 0xb8, 0x09, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x0f,
    0x88, 0x39, 0x01, 0x00, 0x00, 0x49, 0x89, 0xc5, 0x4f, 0x8d, 0x7c, 0x35,
    0x00, 0x48, 0x8d, 0x1d, 0x4e, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x2d, 0x37,
    0x01, 0x00, 0x00, 0x48, 0x01, 0xdd, 0x4c, 0x89, 0xef, 0x45, 0x31, 0xc0,
    0x45, 0x31, 0xc9, 0x41, 0xbb, 0x01, 0x00, 0x00, 0x00, 0x4c, 0x39, 0xff,
    0x0f, 0x83, 0xa7, 0x00, 0x00, 0x00, 0x45, 0x85, 0xc9, 0x75, 0x16, 0x48,
    0x39, 0xeb, 0x0f, 0x83, 0xfa, 0x00, 0x00, 0x00, 0x44, 0x0f, 0xb6, 0x03,
    0x48, 0xff, 0xc3, 0x41, 0xb9, 0x08, 0x00, 0x00, 0x00, 0x44, 0x89, 0xc0,
    0x83, 0xe0, 0x01, 0x41, 0xd1, 0xe8, 0x41, 0xff, 0xc9, 0x85, 0xc0, 0x75,
    0x15, 0x48, 0x39, 0xeb, 0x0f, 0x83, 0xd4, 0x00, 0x00, 0x00, 0x8a, 0x03,
    0x48, 0xff, 0xc3, 0x88, 0x07, 0x48, 0xff, 0xc7, 0xeb, 0xb7, 0x48, 0x39,
    0xeb, 0x0f, 0x83, 0xbf, 0x00, 0x00, 0x00, 0x0f, 0xb6, 0x03, 0x48, 0xff,
    0xc3, 0xa8, 0x80, 0x74, 0x0d, 0x89, 0xc1, 0x83, 0xe1, 0x7f, 0x83, 0xc1,
    0x03, 0x44, 0x89, 0xda, 0xeb, 0x24, 0x48, 0x39, 0xeb, 0x0f, 0x83, 0x9f,
    0x00, 0x00, 0x00, 0x0f, 0xb6, 0x13, 0x48, 0xff, 0xc3, 0x89, 0xc1, 0x83,
    0xe1, 0x0f, 0x83, 0xc1, 0x03, 0xc1, 0xe8, 0x04, 0xc1, 0xe0, 0x08, 0x09,
    0xc2, 0xff, 0xc2, 0x41, 0x89, 0xd3, 0x48, 0x89, 0xfe, 0x48, 0x29, 0xd6,
    0x4c, 0x39, 0xee, 0x72, 0x79, 0x4c, 0x39, 0xff, 0x73, 0x74, 0x8a, 0x06,
    0x48, 0xff, 0xc6, 0x88, 0x07, 0x48, 0xff, 0xc7, 0xff, 0xc9, 0x75, 0xed,
    0xe9, 0x50, 0xff, 0xff, 0xff, 0x48, 0x39, 0xeb, 0x75, 0x5c, 0xb8, 0x3f,
    0x01, 0x00, 0x00, 0x48, 0x8d, 0x3d, 0x6c, 0x00, 0x00, 0x00, 0x31, 0xf6,
    0x0f, 0x05, 0x48, 0x85, 0xc0, 0x78, 0x47, 0x89, 0xc3, 0x4c, 0x89, 0xee,
    0x4c, 0x89, 0xf2, 0x48, 0x85, 0xd2, 0x74, 0x16, 0xb8, 0x01, 0x00, 0x00,
    0x00, 0x89, 0xdf, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x7e, 0x2c, 0x48, 0x01,
    0xc6, 0x48, 0x29, 0xc2, 0xeb, 0xe5, 0xb8, 0x42, 0x01, 0x00, 0x00, 0x89,
    0xdf, 0x48, 0x8d, 0x35, 0x39, 0x00, 0x00, 0x00, 0x49, 0x8d, 0x54, 0x24,
    0x08, 0x4d, 0x8b, 0x14, 0x24, 0x4f, 0x8d, 0x54, 0xd4, 0x10, 0x41, 0xb8,
    0x00, 0x10, 0x00, 0x00, 0x0f, 0x05, 0xb8, 0x3c, 0x00, 0x00, 0x00, 0xbf,
    0x7f, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
    0x77, 0x88, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x65, 0x78,
    0x70, 0x61, 0x63, 0x6b, 0x00, 0x00
};

static const unsigned char expack_lzss_bcj_stub_x86_64[] = {
    0x31, 0xed, 0x49, 0x89, 0xe4, 0x4c, 0x8b, 0x35, 0xb3, 0x01, 0x00, 0x00,
    0x31, 0xff, 0x4c, 0x89, 0xf6, 0xba, 0x03, 0x00, 0x00, 0x00, 0x41, 0xba,
    0x22, 0x00, 0x00, 0x00, 0x41, 0xb8, 0xff, 0xff, 0xff, 0xff, 0x45, 0x31,
    0xc9, 0xb8, 0x09, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x0f,
    0x88, 0x7e, 0x01, 0x00, 0x00, 0x49, 0x89, 0xc5, 0x4f, 0x8d, 0x7c, 0x35,
    0x00, 0x48, 0x8d, 0x1d, 0x93, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x2d, 0x7c,
    0x01, 0x00, 0x00, 0x48, 0x01, 0xdd, 0x4c, 0x89, 0xef, 0x45, 0x31, 0xc0,
    0x45, 0x31, 0xc9, 0x4c, 0x39, 0xff, 0x0f, 0x83, 0x95, 0x00, 0x00, 0x00,
    0x45, 0x85, 0xc9, 0x75, 0x16, 0x48, 0x39, 0xeb, 0x0f, 0x83, 0x45, 0x01,
    0x00, 0x00, 0x44, 0x0f, 0xb6, 0x03, 0x48, 0xff, 0xc3, 0x41, 0xb9, 0x08,
    0x00, 0x00, 0x00, 0x44, 0x89, 0xc0, 0x83, 0xe0, 0x01, 0x41, 0xd1, 0xe8,
    0x41, 0xff, 0xc9, 0x85, 0xc0, 0x75, 0x15, 0x48, 0x39, 0xeb, 0x0f, 0x83,
    0x1f, 0x01, 0x00, 0x00, 0x8a, 0x03, 0x48, 0xff, 0xc3, 0x88, 0x07, 0x48,
    0xff, 0xc7, 0xeb, 0xb7, 0x48, 0x8d, 0x43, 0x02, 0x48, 0x39, 0xe8, 0x0f,
    0x87, 0x06, 0x01, 0x00, 0x00, 0x0f, 0xb6, 0x03, 0x0f, 0xb6, 0x4b, 0x01,
    0x48, 0x83, 0xc3, 0x02, 0x89, 0xca, 0xc1, 0xea, 0x03, 0xc1, 0xe2, 0x08,
    0x09, 0xc2, 0xff, 0xc2, 0x83, 0xe1, 0x07, 0x83, 0xc1, 0x03, 0x48, 0x89,
    0xfe, 0x48, 0x29, 0xd6, 0x4c, 0x39, 0xee, 0x0f, 0x82, 0xda, 0x00, 0x00,
    0x00, 0x4c, 0x39, 0xff, 0x0f, 0x83, 0xd1, 0x00, 0x00, 0x00, 0x8a, 0x06,
    0x48, 0xff, 0xc6, 0x88, 0x07, 0x48, 0xff, 0xc7, 0xff, 0xc9, 0x75, 0xe9,
    0xe9, 0x62, 0xff, 0xff, 0xff, 0x48, 0x39, 0xeb, 0x0f, 0x85, 0xb5, 0x00,
    0x00, 0x00, 0x4c, 0x89, 0xee, 0x49, 0x8d, 0x7f, 0xfa, 0x48, 0x39, 0xfe,
    0x77, 0x4d, 0x8a, 0x06, 0x3c, 0xe8, 0x74, 0x28, 0x3c, 0xe9, 0x74, 0x24,
    0x3c, 0x0f, 0x75, 0x37, 0x8a, 0x46, 0x01, 0x24, 0xf0, 0x3c, 0x80, 0x75,
    0x2e, 0x8b, 0x46, 0x02, 0x48, 0x89, 0xf2, 0x4c, 0x29, 0xea, 0x83, 0xc2,
    0x06, 0x29, 0xd0, 0x89, 0x46, 0x02, 0x48, 0x83, 0xc6, 0x06, 0xeb, 0x1a,
    0x8b, 0x46, 0x01, 0x48, 0x89, 0xf2, 0x4c, 0x29, 0xea, 0x83, 0xc2, 0x05,
    0x29, 0xd0, 0x89, 0x46, 0x01, 0x48, 0x83, 0xc6, 0x05, 0xeb, 0x03, 0x48,
    0xff, 0xc6, 0x48, 0x39, 0xfe, 0x76, 0xb3, 0xb8, 0x3f, 0x01, 0x00, 0x00,
    0x48, 0x8d, 0x3d, 0x6c, 0x00, 0x00, 0x00, 0x31, 0xf6, 0x0f, 0x05, 0x48,
    0x85, 0xc0, 0x78, 0x47, 0x89, 0xc3, 0x4c, 0x89, 0xee, 0x4c, 0x89, 0xf2,
    0x48, 0x85, 0xd2, 0x74, 0x16, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x89, 0xdf,
    0x0f, 0x05, 0x48, 0x85, 0xc0, 0x7e, 0x2c, 0x48, 0x01, 0xc6, 0x48, 0x29,
    0xc2, 0xeb, 0xe5, 0xb8, 0x42, 0x01, 0x00, 0x00, 0x89, 0xdf, 0x48, 0x8d,
    0x35, 0x39, 0x00, 0x00, 0x00, 0x49, 0x8d, 0x54, 0x24, 0x08, 0x4d, 0x8b,
    0x14, 0x24, 0x4f, 0x8d, 0x54, 0xd4, 0x10, 0x41, 0xb8, 0x00, 0x10, 0x00,
    0x00, 0x0f, 0x05, 0xb8, 0x3c, 0x00, 0x00, 0x00, 0xbf, 0x7f, 0x00, 0x00,
    0x00, 0x0f, 0x05, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x88,
    0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x65, 0x78, 0x70, 0x61, 0x63,
    0x6b, 0x00, 0x00
};

static const unsigned char expack_lzss_bcj_rip_stub_x86_64[] = {
    0x31, 0xed, 0x49, 0x89, 0xe4, 0x4c, 0x8b, 0x35, 0x29, 0x02, 0x00, 0x00,
    0x31, 0xff, 0x4c, 0x89, 0xf6, 0xba, 0x03, 0x00, 0x00, 0x00, 0x41, 0xba,
    0x22, 0x00, 0x00, 0x00, 0x41, 0xb8, 0xff, 0xff, 0xff, 0xff, 0x45, 0x31,
    0xc9, 0xb8, 0x09, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x0f,
    0x88, 0xf4, 0x01, 0x00, 0x00, 0x49, 0x89, 0xc5, 0x4f, 0x8d, 0x7c, 0x35,
    0x00, 0x48, 0x8d, 0x1d, 0x09, 0x02, 0x00, 0x00, 0x48, 0x8b, 0x2d, 0xf2,
    0x01, 0x00, 0x00, 0x48, 0x01, 0xdd, 0x4c, 0x89, 0xef, 0x45, 0x31, 0xc0,
    0x45, 0x31, 0xc9, 0x4c, 0x39, 0xff, 0x0f, 0x83, 0x95, 0x00, 0x00, 0x00,
    0x45, 0x85, 0xc9, 0x75, 0x16, 0x48, 0x39, 0xeb, 0x0f, 0x83, 0xbb, 0x01,
    0x00, 0x00, 0x44, 0x0f, 0xb6, 0x03, 0x48, 0xff, 0xc3, 0x41, 0xb9, 0x08,
    0x00, 0x00, 0x00, 0x44, 0x89, 0xc0, 0x83, 0xe0, 0x01, 0x41, 0xd1, 0xe8,
    0x41, 0xff, 0xc9, 0x85, 0xc0, 0x75, 0x15, 0x48, 0x39, 0xeb, 0x0f, 0x83,
    0x95, 0x01, 0x00, 0x00, 0x8a, 0x03, 0x48, 0xff, 0xc3, 0x88, 0x07, 0x48,
    0xff, 0xc7, 0xeb, 0xb7, 0x48, 0x8d, 0x43, 0x02, 0x48, 0x39, 0xe8, 0x0f,
    0x87, 0x7c, 0x01, 0x00, 0x00, 0x0f, 0xb6, 0x03, 0x0f, 0xb6, 0x4b, 0x01,
    0x48, 0x83, 0xc3, 0x02, 0x89, 0xca, 0xc1, 0xea, 0x03, 0xc1, 0xe2, 0x08,
    0x09, 0xc2, 0xff, 0xc2, 0x83, 0xe1, 0x07, 0x83, 0xc1, 0x03, 0x48, 0x89,
    0xfe, 0x48, 0x29, 0xd6, 0x4c, 0x39, 0xee, 0x0f, 0x82, 0x50, 0x01, 0x00,
    0x00, 0x4c, 0x39, 0xff, 0x0f, 0x83, 0x47, 0x01, 0x00, 0x00, 0x8a, 0x06,
    0x48, 0xff, 0xc6, 0x88, 0x07, 0x48, 0xff, 0xc7, 0xff, 0xc9, 0x75, 0xe9,
    0xe9, 0x62, 0xff, 0xff, 0xff, 0x48, 0x39, 0xeb, 0x0f, 0x85, 0x2b, 0x01,
    0x00, 0x00, 0x4c, 0x89, 0xee, 0x4c, 0x39, 0xfe, 0x0f, 0x83, 0xc3, 0x00,
    0x00, 0x00, 0x8a, 0x06, 0x3c, 0xe8, 0x0f, 0x84, 0x8e, 0x00, 0x00, 0x00,
    0x3c, 0xe9, 0x0f, 0x84, 0x86, 0x00, 0x00, 0x00, 0x3c, 0x0f, 0x74, 0x56,
    0x48, 0x89, 0xf1, 0x88, 0xc2, 0x80, 0xfa, 0x40, 0x72, 0x19, 0x80, 0xfa,
    0x4f, 0x77, 0x14, 0x48, 0x8d, 0x46, 0x07, 0x4c, 0x39, 0xf8, 0x0f, 0x87,
    0x89, 0x00, 0x00, 0x00, 0x48, 0xff, 0xc1, 0x8a, 0x11, 0xeb, 0x09, 0x48,
    0x8d, 0x46, 0x06, 0x4c, 0x39, 0xf8, 0x77, 0x79, 0x80, 0xfa, 0x8b, 0x74,
    0x05, 0x80, 0xfa, 0x8d, 0x75, 0x6f, 0x8a, 0x41, 0x01, 0x24, 0xc7, 0x3c,
    0x05, 0x75, 0x66, 0x8b, 0x41, 0x02, 0x48, 0x89, 0xca, 0x4c, 0x29, 0xea,
    0x83, 0xc2, 0x06, 0x29, 0xd0, 0x89, 0x41, 0x02, 0x48, 0x8d, 0x71, 0x06,
    0xeb, 0x8b, 0x48, 0x8d, 0x46, 0x06, 0x4c, 0x39, 0xf8, 0x77, 0x46, 0x8a,
    0x46, 0x01, 0x24, 0xf0, 0x3c, 0x80, 0x75, 0x3d, 0x8b, 0x46, 0x02, 0x48,
    0x89, 0xf2, 0x4c, 0x29, 0xea, 0x83, 0xc2, 0x06, 0x29, 0xd0, 0x89, 0x46,
    0x02, 0x48, 0x83, 0xc6, 0x06, 0xe9, 0x5f, 0xff, 0xff, 0xff, 0x48, 0x8d,
    0x46, 0x05, 0x4c, 0x39, 0xf8, 0x77, 0x1a, 0x8b, 0x46, 0x01, 0x48, 0x89,
    0xf2, 0x4c, 0x29, 0xea, 0x83, 0xc2, 0x05, 0x29, 0xd0, 0x89, 0x46, 0x01,
    0x48, 0x83, 0xc6, 0x05, 0xe9, 0x3c, 0xff, 0xff, 0xff, 0x48, 0xff, 0xc6,
    0xe9, 0x34, 0xff, 0xff, 0xff, 0xb8, 0x3f, 0x01, 0x00, 0x00, 0x48, 0x8d,
    0x3d, 0x6c, 0x00, 0x00, 0x00, 0x31, 0xf6, 0x0f, 0x05, 0x48, 0x85, 0xc0,
    0x78, 0x47, 0x89, 0xc3, 0x4c, 0x89, 0xee, 0x4c, 0x89, 0xf2, 0x48, 0x85,
    0xd2, 0x74, 0x16, 0xb8, 0x01, 0x00, 0x00, 0x00, 0x89, 0xdf, 0x0f, 0x05,
    0x48, 0x85, 0xc0, 0x7e, 0x2c, 0x48, 0x01, 0xc6, 0x48, 0x29, 0xc2, 0xeb,
    0xe5, 0xb8, 0x42, 0x01, 0x00, 0x00, 0x89, 0xdf, 0x48, 0x8d, 0x35, 0x39,
    0x00, 0x00, 0x00, 0x49, 0x8d, 0x54, 0x24, 0x08, 0x4d, 0x8b, 0x14, 0x24,
    0x4f, 0x8d, 0x54, 0xd4, 0x10, 0x41, 0xb8, 0x00, 0x10, 0x00, 0x00, 0x0f,
    0x05, 0xb8, 0x3c, 0x00, 0x00, 0x00, 0xbf, 0x7f, 0x00, 0x00, 0x00, 0x0f,
    0x05, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x88, 0x77, 0x66,
    0x55, 0x44, 0x33, 0x22, 0x11, 0x65, 0x78, 0x70, 0x61, 0x63, 0x6b, 0x00,
    0x00
};

static const unsigned char expack_lz4_stub_x86_64[] = {
    0xfc, 0x31, 0xed, 0x49, 0x89, 0xe4, 0x4c, 0x8b, 0x35, 0x86, 0x01, 0x00,
    0x00, 0x31, 0xff, 0x4c, 0x89, 0xf6, 0xba, 0x03, 0x00, 0x00, 0x00, 0x41,
    0xba, 0x22, 0x00, 0x00, 0x00, 0x41, 0xb8, 0xff, 0xff, 0xff, 0xff, 0x45,
    0x31, 0xc9, 0xb8, 0x09, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x85, 0xc0,
    0x0f, 0x88, 0x51, 0x01, 0x00, 0x00, 0x49, 0x89, 0xc5, 0x4f, 0x8d, 0x7c,
    0x35, 0x00, 0x48, 0x8d, 0x1d, 0x66, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x2d,
    0x4f, 0x01, 0x00, 0x00, 0x48, 0x01, 0xdd, 0x4c, 0x89, 0xef, 0x48, 0x39,
    0xeb, 0x0f, 0x84, 0xcb, 0x00, 0x00, 0x00, 0x4c, 0x39, 0xff, 0x0f, 0x83,
    0x23, 0x01, 0x00, 0x00, 0x44, 0x0f, 0xb6, 0x03, 0x48, 0xff, 0xc3, 0x44,
    0x89, 0xc1, 0xc1, 0xe9, 0x04, 0x83, 0xf9, 0x0f, 0x75, 0x16, 0x48, 0x39,
    0xeb, 0x0f, 0x83, 0x08, 0x01, 0x00, 0x00, 0x0f, 0xb6, 0x03, 0x48, 0xff,
    0xc3, 0x48, 0x01, 0xc1, 0x3c, 0xff, 0x74, 0xea, 0x48, 0x89, 0xe8, 0x48,
    0x29, 0xd8, 0x48, 0x39, 0xc8, 0x0f, 0x82, 0xec, 0x00, 0x00, 0x00, 0x4c,
    0x89, 0xf8, 0x48, 0x29, 0xf8, 0x48, 0x39, 0xc8, 0x0f, 0x82, 0xdd, 0x00,
    0x00, 0x00, 0x48, 0x89, 0xde, 0xf3, 0xa4, 0x48, 0x89, 0xf3, 0x48, 0x39,
    0xeb, 0x74, 0x6f, 0x48, 0x8d, 0x43, 0x02, 0x48, 0x39, 0xe8, 0x0f, 0x87,
    0xc3, 0x00, 0x00, 0x00, 0x0f, 0xb7, 0x13, 0x48, 0x83, 0xc3, 0x02, 0x85,
    0xd2, 0x0f, 0x84, 0xb4, 0x00, 0x00, 0x00, 0x44, 0x89, 0xc1, 0x83, 0xe1,
    0x0f, 0x83, 0xf9, 0x0f, 0x75, 0x16, 0x48, 0x39, 0xeb, 0x0f, 0x83, 0xa0,
    0x00, 0x00, 0x00, 0x0f, 0xb6, 0x03, 0x48, 0xff, 0xc3, 0x48, 0x01, 0xc1,
    0x3c, 0xff, 0x74, 0xea, 0x48, 0x83, 0xc1, 0x04, 0x48, 0x89, 0xfe, 0x48,
    0x29, 0xd6, 0x4c, 0x39, 0xee, 0x0f, 0x82, 0x80, 0x00, 0x00, 0x00, 0x4c,
    0x89, 0xf8, 0x48, 0x29, 0xf8, 0x48, 0x39, 0xc8, 0x72, 0x75, 0x8a, 0x06,
    0x48, 0xff, 0xc6, 0x88, 0x07, 0x48, 0xff, 0xc7, 0x48, 0xff, 0xc9, 0x75,
    0xf1, 0xe9, 0x2c, 0xff, 0xff, 0xff, 0x4c, 0x39, 0xff, 0x75, 0x5c, 0xb8,
    0x3f, 0x01, 0x00, 0x00, 0x48, 0x8d, 0x3d, 0x6c, 0x00, 0x00, 0x00, 0x31,
    0xf6, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x78, 0x47, 0x89, 0xc3, 0x4c, 0x89,
    0xee, 0x4c, 0x89, 0xf2, 0x48, 0x85, 0xd2, 0x74, 0x16, 0xb8, 0x01, 0x00,
    0x00, 0x00, 0x89, 0xdf, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x7e, 0x2c, 0x48,
    0x01, 0xc6, 0x48, 0x29, 0xc2, 0xeb, 0xe5, 0xb8, 0x42, 0x01, 0x00, 0x00,
    0x89, 0xdf, 0x48, 0x8d, 0x35, 0x39, 0x00, 0x00, 0x00, 0x49, 0x8d, 0x54,
    0x24, 0x08, 0x4d, 0x8b, 0x14, 0x24, 0x4f, 0x8d, 0x54, 0xd4, 0x10, 0x41,
    0xb8, 0x00, 0x10, 0x00, 0x00, 0x0f, 0x05, 0xb8, 0x3c, 0x00, 0x00, 0x00,
    0xbf, 0x7f, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x11, 0x22, 0x33, 0x44, 0x55,
    0x66, 0x77, 0x88, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x65,
    0x78, 0x70, 0x61, 0x63, 0x6b, 0x00, 0x00
};

static const unsigned char expack_xlz_stub_x86_64[] = {
    0x49, 0x89, 0xe4, 0x4c, 0x8b, 0x35, 0x71, 0x01, 0x00, 0x00, 0x31, 0xff,
    0x4c, 0x89, 0xf6, 0x6a, 0x03, 0x5a, 0x6a, 0x22, 0x41, 0x5a, 0x6a, 0xff,
    0x41, 0x58, 0x45, 0x31, 0xc9, 0x6a, 0x09, 0x58, 0x0f, 0x05, 0x48, 0x85,
    0xc0, 0x0f, 0x88, 0x48, 0x01, 0x00, 0x00, 0x49, 0x89, 0xc5, 0x4f, 0x8d,
    0x7c, 0x35, 0x00, 0x48, 0x8d, 0x35, 0x59, 0x01, 0x00, 0x00, 0x48, 0x8b,
    0x2d, 0x42, 0x01, 0x00, 0x00, 0x48, 0x01, 0xf5, 0x4c, 0x89, 0xef, 0x4c,
    0x39, 0xff, 0x0f, 0x83, 0xc5, 0x00, 0x00, 0x00, 0x48, 0x39, 0xee, 0x0f,
    0x83, 0x1a, 0x01, 0x00, 0x00, 0xac, 0x0f, 0xb6, 0xc0, 0x41, 0x89, 0xc0,
    0x89, 0xc1, 0xc1, 0xe9, 0x05, 0x83, 0xf9, 0x07, 0x75, 0x14, 0x48, 0x39,
    0xee, 0x0f, 0x83, 0x00, 0x01, 0x00, 0x00, 0xac, 0x0f, 0xb6, 0xc0, 0x48,
    0x01, 0xc1, 0x3c, 0xff, 0x74, 0xec, 0x48, 0x89, 0xe8, 0x48, 0x29, 0xf0,
    0x48, 0x39, 0xc8, 0x0f, 0x82, 0xe6, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xf8,
    0x48, 0x29, 0xf8, 0x48, 0x39, 0xc8, 0x0f, 0x82, 0xd7, 0x00, 0x00, 0x00,
    0xf3, 0xa4, 0x4c, 0x39, 0xff, 0x73, 0x72, 0x41, 0xf6, 0xc0, 0x10, 0x75,
    0x11, 0x48, 0x39, 0xee, 0x0f, 0x83, 0xc1, 0x00, 0x00, 0x00, 0xac, 0x0f,
    0xb6, 0xd0, 0xff, 0xc2, 0xeb, 0x14, 0x48, 0x8d, 0x46, 0x02, 0x48, 0x39,
    0xe8, 0x0f, 0x87, 0xac, 0x00, 0x00, 0x00, 0x66, 0xad, 0x0f, 0xb7, 0xd0,
    0xff, 0xc2, 0x44, 0x89, 0xc1, 0x83, 0xe1, 0x0f, 0x83, 0xc1, 0x03, 0x83,
    0xf9, 0x12, 0x75, 0x14, 0x48, 0x39, 0xee, 0x0f, 0x83, 0x8e, 0x00, 0x00,
    0x00, 0xac, 0x0f, 0xb6, 0xc0, 0x48, 0x01, 0xc1, 0x3c, 0xff, 0x74, 0xec,
    0x48, 0x89, 0xf8, 0x4c, 0x29, 0xe8, 0x48, 0x39, 0xd0, 0x72, 0x78, 0x4c,
    0x89, 0xf8, 0x48, 0x29, 0xf8, 0x48, 0x39, 0xc8, 0x72, 0x6d, 0x56, 0x48,
    0x89, 0xfe, 0x48, 0x29, 0xd6, 0xf3, 0xa4, 0x5e, 0xe9, 0x32, 0xff, 0xff,
    0xff, 0x48, 0x39, 0xee, 0x75, 0x59, 0x68, 0x3f, 0x01, 0x00, 0x00, 0x58,
    0x48, 0x8d, 0x3d, 0x64, 0x00, 0x00, 0x00, 0x31, 0xf6, 0x0f, 0x05, 0x85,
    0xc0, 0x78, 0x44, 0x93, 0x4c, 0x89, 0xee, 0x4c, 0x89, 0xf2, 0x48, 0x85,
    0xd2, 0x74, 0x13, 0x6a, 0x01, 0x58, 0x89, 0xdf, 0x0f, 0x05, 0x85, 0xc0,
    0x7e, 0x2d, 0x48, 0x01, 0xc6, 0x48, 0x29, 0xc2, 0xeb, 0xe8, 0x68, 0x42,
    0x01, 0x00, 0x00, 0x58, 0x89, 0xdf, 0x48, 0x8d, 0x35, 0x35, 0x00, 0x00,
    0x00, 0x49, 0x8d, 0x54, 0x24, 0x08, 0x4d, 0x8b, 0x14, 0x24, 0x4f, 0x8d,
    0x54, 0xd4, 0x10, 0x41, 0xb8, 0x00, 0x10, 0x00, 0x00, 0x0f, 0x05, 0x6a,
    0x3c, 0x58, 0x6a, 0x7f, 0x5f, 0x0f, 0x05, 0x11, 0x22, 0x33, 0x44, 0x55,
    0x66, 0x77, 0x88, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x65,
    0x78, 0x70, 0x61, 0x63, 0x6b, 0x00, 0x00
};

static const unsigned char expack_xlz_bcj_stub_x86_64[] = {
    0x49, 0x89, 0xe4, 0x4c, 0x8b, 0x35, 0xdf, 0x01, 0x00, 0x00, 0x31, 0xff,
    0x4c, 0x89, 0xf6, 0x6a, 0x03, 0x5a, 0x6a, 0x22, 0x41, 0x5a, 0x6a, 0xff,
    0x41, 0x58, 0x45, 0x31, 0xc9, 0x6a, 0x09, 0x58, 0x0f, 0x05, 0x48, 0x85,
    0xc0, 0x0f, 0x88, 0xb6, 0x01, 0x00, 0x00, 0x49, 0x89, 0xc5, 0x4f, 0x8d,
    0x7c, 0x35, 0x00, 0x48, 0x8d, 0x35, 0xc7, 0x01, 0x00, 0x00, 0x48, 0x8b,
    0x2d, 0xb0, 0x01, 0x00, 0x00, 0x48, 0x01, 0xf5, 0x4c, 0x89, 0xef, 0x4c,
    0x39, 0xff, 0x0f, 0x83, 0xcd, 0x00, 0x00, 0x00, 0x48, 0x39, 0xee, 0x0f,
    0x83, 0x88, 0x01, 0x00, 0x00, 0xac, 0x0f, 0xb6, 0xc0, 0x41, 0x89, 0xc0,
    0x89, 0xc1, 0xc1, 0xe9, 0x05, 0x83, 0xf9, 0x07, 0x75, 0x14, 0x48, 0x39,
    0xee, 0x0f, 0x83, 0x6e, 0x01, 0x00, 0x00, 0xac, 0x0f, 0xb6, 0xc0, 0x48,
    0x01, 0xc1, 0x3c, 0xff, 0x74, 0xec, 0x48, 0x89, 0xe8, 0x48, 0x29, 0xf0,
    0x48, 0x39, 0xc8, 0x0f, 0x82, 0x54, 0x01, 0x00, 0x00, 0x4c, 0x89, 0xf8,
    0x48, 0x29, 0xf8, 0x48, 0x39, 0xc8, 0x0f, 0x82, 0x45, 0x01, 0x00, 0x00,
    0xf3, 0xa4, 0x4c, 0x39, 0xff, 0x73, 0x7a, 0x41, 0xf6, 0xc0, 0x10, 0x75,
    0x11, 0x48, 0x39, 0xee, 0x0f, 0x83, 0x2f, 0x01, 0x00, 0x00, 0xac, 0x0f,
    0xb6, 0xd0, 0xff, 0xc2, 0xeb, 0x14, 0x48, 0x8d, 0x46, 0x02, 0x48, 0x39,
    0xe8, 0x0f, 0x87, 0x1a, 0x01, 0x00, 0x00, 0x66, 0xad, 0x0f, 0xb7, 0xd0,
    0xff, 0xc2, 0x44, 0x89, 0xc1, 0x83, 0xe1, 0x0f, 0x83, 0xc1, 0x03, 0x83,
    0xf9, 0x12, 0x75, 0x14, 0x48, 0x39, 0xee, 0x0f, 0x83, 0xfc, 0x00, 0x00,
    0x00, 0xac, 0x0f, 0xb6, 0xc0, 0x48, 0x01, 0xc1, 0x3c, 0xff, 0x74, 0xec,
    0x48, 0x89, 0xf8, 0x4c, 0x29, 0xe8, 0x48, 0x39, 0xd0, 0x0f, 0x82, 0xe2,
    0x00, 0x00, 0x00, 0x4c, 0x89, 0xf8, 0x48, 0x29, 0xf8, 0x48, 0x39, 0xc8,
    0x0f, 0x82, 0xd3, 0x00, 0x00, 0x00, 0x56, 0x48, 0x89, 0xfe, 0x48, 0x29,
    0xd6, 0xf3, 0xa4, 0x5e, 0xe9, 0x2a, 0xff, 0xff, 0xff, 0x48, 0x39, 0xee,
    0x0f, 0x85, 0xbb, 0x00, 0x00, 0x00, 0x4c, 0x89, 0xee, 0x4c, 0x39, 0xfe,
    0x73, 0x5a, 0x8a, 0x06, 0x3c, 0xe8, 0x74, 0x30, 0x3c, 0xe9, 0x74, 0x2c,
    0x3c, 0x0f, 0x75, 0x47, 0x48, 0x8d, 0x46, 0x06, 0x4c, 0x39, 0xf8, 0x77,
    0x3e, 0x8a, 0x46, 0x01, 0x24, 0xf0, 0x3c, 0x80, 0x75, 0x35, 0x8b, 0x46,
    0x02, 0x89, 0xf2, 0x44, 0x29, 0xea, 0x83, 0xc2, 0x06, 0x29, 0xd0, 0x89,
    0x46, 0x02, 0x48, 0x83, 0xc6, 0x06, 0xeb, 0xc5, 0x48, 0x8d, 0x46, 0x05,
    0x4c, 0x39, 0xf8, 0x77, 0x16, 0x8b, 0x46, 0x01, 0x89, 0xf2, 0x44, 0x29,
    0xea, 0x83, 0xc2, 0x05, 0x29, 0xd0, 0x89, 0x46, 0x01, 0x48, 0x83, 0xc6,
    0x05, 0xeb, 0xa6, 0x48, 0xff, 0xc6, 0xeb, 0xa1, 0x68, 0x3f, 0x01, 0x00,
    0x00, 0x58, 0x48, 0x8d, 0x3d, 0x64, 0x00, 0x00, 0x00, 0x31, 0xf6, 0x0f,
    0x05, 0x85, 0xc0, 0x78, 0x44, 0x93, 0x4c, 0x89, 0xee, 0x4c, 0x89, 0xf2,
    0x48, 0x85, 0xd2, 0x74, 0x13, 0x6a, 0x01, 0x58, 0x89, 0xdf, 0x0f, 0x05,
    0x85, 0xc0, 0x7e, 0x2d, 0x48, 0x01, 0xc6, 0x48, 0x29, 0xc2, 0xeb, 0xe8,
    0x68, 0x42, 0x01, 0x00, 0x00, 0x58, 0x89, 0xdf, 0x48, 0x8d, 0x35, 0x35,
    0x00, 0x00, 0x00, 0x49, 0x8d, 0x54, 0x24, 0x08, 0x4d, 0x8b, 0x14, 0x24,
    0x4f, 0x8d, 0x54, 0xd4, 0x10, 0x41, 0xb8, 0x00, 0x10, 0x00, 0x00, 0x0f,
    0x05, 0x6a, 0x3c, 0x58, 0x6a, 0x7f, 0x5f, 0x0f, 0x05, 0x11, 0x22, 0x33,
    0x44, 0x55, 0x66, 0x77, 0x88, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22,
    0x11, 0x65, 0x78, 0x70, 0x61, 0x63, 0x6b, 0x00, 0x00
};


static size_t expack_match_length_at(const unsigned char *data, size_t size, size_t position, size_t distance, size_t max_length) {
    size_t length = 0U;

    if (distance == 0U || distance > position) {
        return 0U;
    }
    if (max_length > size - position) {
        max_length = size - position;
    }
    while (length < max_length && data[position + length] == data[position - distance + length]) {
        length += 1U;
    }
    return length;
}

#define EXPACK_LZREP_TOKEN_LITERAL 0U
#define EXPACK_LZREP_TOKEN_REPEAT 1U
#define EXPACK_LZREP_TOKEN_EXPLICIT 2U
#define EXPACK_LZREP_FAST_LOOKAHEAD 64U
#define EXPACK_LZREP_FAST_BEAM_WIDTH 12U
#define EXPACK_LZREP_FAST_EXPLICIT_CHOICES 3U
#define EXPACK_LZREP_OPT_LOOKAHEAD 96U
#define EXPACK_LZREP_OPT_BEAM_WIDTH 20U
#define EXPACK_LZREP_OPT_EXPLICIT_CHOICES 6U
#define EXPACK_LZREP_COST_MAX (((unsigned int)-1) / 4U)

typedef struct {
    unsigned char kind;
    unsigned char length;
    unsigned short distance;
} ExpackLzrepToken;

typedef struct {
    size_t length;
    unsigned int distance;
} ExpackLzrepMatch;

typedef struct {
    unsigned int cost;
    unsigned short last_distance;
    unsigned char bit_index;
    unsigned char used;
    unsigned char have_first;
    ExpackLzrepToken first;
} ExpackLzrepBeamState;

typedef struct {
    unsigned int lookahead;
    unsigned int beam_width;
    unsigned int explicit_choices;
} ExpackLzrepParseParams;

typedef struct {
    size_t *head;
    size_t *next;
    int available;
} ExpackLzrepMatchIndex;

static const ExpackLzrepParseParams expack_lzrep_fast_parse = {
    EXPACK_LZREP_FAST_LOOKAHEAD,
    EXPACK_LZREP_FAST_BEAM_WIDTH,
    EXPACK_LZREP_FAST_EXPLICIT_CHOICES
};

static const ExpackLzrepParseParams expack_lzrep_opt_parse = {
    EXPACK_LZREP_OPT_LOOKAHEAD,
    EXPACK_LZREP_OPT_BEAM_WIDTH,
    EXPACK_LZREP_OPT_EXPLICIT_CHOICES
};

static unsigned int expack_lzrep_hash3(const unsigned char *data) {
    unsigned int value = (unsigned int)data[0] | ((unsigned int)data[1] << 8U) | ((unsigned int)data[2] << 16U);

    return (value * 2246822519U) >> (32U - EXPACK_LZREP_HASH_BITS);
}

static void expack_lzrep_match_index_release(ExpackLzrepMatchIndex *index) {
    if (index->head != 0) {
        rt_free(index->head);
    }
    if (index->next != 0) {
        rt_free(index->next);
    }
    index->head = 0;
    index->next = 0;
    index->available = 0;
}

static int expack_lzrep_match_index_build(const unsigned char *data, size_t size, ExpackLzrepMatchIndex *index) {
    size_t hash_count = (size_t)1U << EXPACK_LZREP_HASH_BITS;
    size_t *tail;
    size_t position;

    index->head = 0;
    index->next = 0;
    index->available = 0;
    if (hash_count > ((size_t)-1) / sizeof(*index->head) || (size == 0U ? 1U : size) > ((size_t)-1) / sizeof(*index->next)) {
        return -1;
    }
    index->head = (size_t *)rt_malloc(hash_count * sizeof(*index->head));
    tail = (size_t *)rt_malloc(hash_count * sizeof(*tail));
    index->next = (size_t *)rt_malloc((size == 0U ? 1U : size) * sizeof(*index->next));
    if (index->head == 0 || tail == 0 || index->next == 0) {
        if (tail != 0) {
            rt_free(tail);
        }
        expack_lzrep_match_index_release(index);
        return -1;
    }
    memset(index->head, 0, hash_count * sizeof(*index->head));
    memset(tail, 0, hash_count * sizeof(*tail));
    memset(index->next, 0, (size == 0U ? 1U : size) * sizeof(*index->next));
    if (size >= COMPRESSION_LZSS_MIN_MATCH) {
        for (position = 0U; position <= size - COMPRESSION_LZSS_MIN_MATCH; ++position) {
            unsigned int hash = expack_lzrep_hash3(data + position);

            if (tail[hash] == 0U) {
                index->head[hash] = position + 1U;
            } else {
                index->next[tail[hash] - 1U] = position + 1U;
            }
            tail[hash] = position + 1U;
        }
    }
    rt_free(tail);
    index->available = 1;
    return 0;
}

static void expack_lzrep_add_match(ExpackLzrepMatch *matches, unsigned int match_capacity, size_t length, unsigned int distance) {
    unsigned int index;

    if (length < COMPRESSION_LZSS_MIN_MATCH) {
        return;
    }
    for (index = 0U; index < match_capacity; ++index) {
        if (length > matches[index].length || (length == matches[index].length && distance < matches[index].distance)) {
            unsigned int shift;

            for (shift = match_capacity - 1U; shift > index; --shift) {
                matches[shift] = matches[shift - 1U];
            }
            matches[index].length = length;
            matches[index].distance = distance;
            break;
        }
    }
}

static void expack_find_lzrep_matches(const unsigned char *data, size_t size, size_t position, const ExpackLzrepMatchIndex *match_index, ExpackLzrepMatch *matches, unsigned int match_capacity) {
    size_t window_start = position > EXPACK_LZREP_WINDOW_SIZE ? position - EXPACK_LZREP_WINDOW_SIZE : 0U;
    size_t candidate;
    unsigned int index;

    for (index = 0U; index < match_capacity; ++index) {
        matches[index].length = 0U;
        matches[index].distance = 0U;
    }
    if (position > size || size - position < COMPRESSION_LZSS_MIN_MATCH) {
        return;
    }
    if (match_index != 0 && match_index->available) {
        size_t link = match_index->head[expack_lzrep_hash3(data + position)];

        while (link != 0U) {
            candidate = link - 1U;
            if (candidate >= position) {
                break;
            }
            if (candidate >= window_start) {
                size_t distance = position - candidate;
                size_t length = expack_match_length_at(data, size, position, distance, EXPACK_LZREP_MAX_EXPLICIT_MATCH);

                expack_lzrep_add_match(matches, match_capacity, length, (unsigned int)distance);
            }
            link = match_index->next[candidate];
        }
        return;
    }
    for (candidate = window_start; candidate < position; ++candidate) {
        size_t distance = position - candidate;
        size_t length = expack_match_length_at(data, size, position, distance, EXPACK_LZREP_MAX_EXPLICIT_MATCH);

        expack_lzrep_add_match(matches, match_capacity, length, (unsigned int)distance);
    }
}

static unsigned int expack_lzrep_token_cost(const ExpackLzrepToken *token, unsigned int bit_index) {
    unsigned int cost = bit_index == 0U ? 1U : 0U;

    return cost + (token->kind == EXPACK_LZREP_TOKEN_EXPLICIT ? 2U : 1U);
}

static int expack_lzrep_better_first_token(const ExpackLzrepToken *left, const ExpackLzrepToken *right) {
    if (left->length != right->length) {
        return left->length > right->length;
    }
    if (left->kind != right->kind) {
        return left->kind > right->kind;
    }
    return left->distance < right->distance;
}

static void expack_lzrep_insert_state(ExpackLzrepBeamState *bucket, unsigned int beam_width, const ExpackLzrepBeamState *state) {
    unsigned int slot;
    unsigned int worst_slot = 0U;

    for (slot = 0U; slot < beam_width; ++slot) {
        if (bucket[slot].used && bucket[slot].last_distance == state->last_distance && bucket[slot].bit_index == state->bit_index) {
            if (state->cost < bucket[slot].cost ||
                (state->cost == bucket[slot].cost && expack_lzrep_better_first_token(&state->first, &bucket[slot].first))) {
                bucket[slot] = *state;
            }
            return;
        }
    }
    for (slot = 0U; slot < beam_width; ++slot) {
        if (!bucket[slot].used) {
            bucket[slot] = *state;
            return;
        }
        if (bucket[slot].cost > bucket[worst_slot].cost ||
            (bucket[slot].cost == bucket[worst_slot].cost && expack_lzrep_better_first_token(&bucket[worst_slot].first, &bucket[slot].first))) {
            worst_slot = slot;
        }
    }
    if (state->cost < bucket[worst_slot].cost ||
        (state->cost == bucket[worst_slot].cost && expack_lzrep_better_first_token(&state->first, &bucket[worst_slot].first))) {
        bucket[worst_slot] = *state;
    }
}

static void expack_lzrep_add_transition(ExpackLzrepBeamState states[EXPACK_LZREP_OPT_LOOKAHEAD + 1U][EXPACK_LZREP_OPT_BEAM_WIDTH],
                                        unsigned int beam_width,
                                        unsigned int limit,
                                        unsigned int rel_position,
                                        const ExpackLzrepBeamState *state,
                                        const ExpackLzrepToken *token) {
    ExpackLzrepBeamState next_state;
    unsigned int next_position = rel_position + (unsigned int)token->length;
    unsigned int token_cost = expack_lzrep_token_cost(token, state->bit_index);

    if (next_position > limit) {
        next_position = limit;
    }
    if (state->cost > EXPACK_LZREP_COST_MAX - token_cost) {
        return;
    }
    next_state.cost = state->cost + token_cost;
    next_state.last_distance = token->kind == EXPACK_LZREP_TOKEN_EXPLICIT ? token->distance : state->last_distance;
    next_state.bit_index = (unsigned char)((state->bit_index + 1U) & 7U);
    next_state.used = 1U;
    next_state.have_first = 1U;
    next_state.first = state->have_first ? state->first : *token;
    expack_lzrep_insert_state(states[next_position], beam_width, &next_state);
}

static void expack_choose_lzrep_token(const unsigned char *data, size_t size, size_t position, const ExpackLzrepMatchIndex *match_index, const ExpackLzrepParseParams *params, unsigned int last_distance, unsigned int bit_index, ExpackLzrepToken *token_out) {
    ExpackLzrepBeamState states[EXPACK_LZREP_OPT_LOOKAHEAD + 1U][EXPACK_LZREP_OPT_BEAM_WIDTH];
    size_t remaining = size - position;
    unsigned int limit = remaining > params->lookahead ? params->lookahead : (unsigned int)remaining;
    unsigned int rel_position;
    unsigned int slot;
    unsigned int best_cost = EXPACK_LZREP_COST_MAX;
    int have_best = 0;

    token_out->kind = EXPACK_LZREP_TOKEN_LITERAL;
    token_out->length = 1U;
    token_out->distance = 0U;
    if (remaining == 0U) {
        return;
    }
    for (rel_position = 0U; rel_position <= limit; ++rel_position) {
        for (slot = 0U; slot < params->beam_width; ++slot) {
            states[rel_position][slot].used = 0U;
        }
    }
    states[0][0].cost = 0U;
    states[0][0].last_distance = (unsigned short)last_distance;
    states[0][0].bit_index = (unsigned char)bit_index;
    states[0][0].used = 1U;
    states[0][0].have_first = 0U;
    states[0][0].first = *token_out;

    for (rel_position = 0U; rel_position < limit; ++rel_position) {
        size_t absolute_position = position + (size_t)rel_position;
        ExpackLzrepMatch matches[EXPACK_LZREP_OPT_EXPLICIT_CHOICES];

        expack_find_lzrep_matches(data, size, absolute_position, match_index, matches, params->explicit_choices);
        for (slot = 0U; slot < params->beam_width; ++slot) {
            ExpackLzrepBeamState state = states[rel_position][slot];
            ExpackLzrepToken token;
            unsigned int match_index;
            size_t repeat_length;

            if (!state.used) {
                continue;
            }
            token.kind = EXPACK_LZREP_TOKEN_LITERAL;
            token.length = 1U;
            token.distance = 0U;
            expack_lzrep_add_transition(states, params->beam_width, limit, rel_position, &state, &token);

            repeat_length = expack_match_length_at(data, size, absolute_position, state.last_distance, EXPACK_LZREP_MAX_REPEAT_MATCH);
            if (repeat_length >= COMPRESSION_LZSS_MIN_MATCH) {
                token.kind = EXPACK_LZREP_TOKEN_REPEAT;
                token.length = (unsigned char)repeat_length;
                token.distance = state.last_distance;
                expack_lzrep_add_transition(states, params->beam_width, limit, rel_position, &state, &token);
            }

            for (match_index = 0U; match_index < params->explicit_choices && matches[match_index].length >= COMPRESSION_LZSS_MIN_MATCH; ++match_index) {
                token.kind = EXPACK_LZREP_TOKEN_EXPLICIT;
                token.length = (unsigned char)matches[match_index].length;
                token.distance = (unsigned short)matches[match_index].distance;
                expack_lzrep_add_transition(states, params->beam_width, limit, rel_position, &state, &token);
            }
        }
    }

    for (slot = 0U; slot < params->beam_width; ++slot) {
        if (states[limit][slot].used && states[limit][slot].have_first &&
            (!have_best || states[limit][slot].cost < best_cost ||
             (states[limit][slot].cost == best_cost && expack_lzrep_better_first_token(&states[limit][slot].first, token_out)))) {
            best_cost = states[limit][slot].cost;
            *token_out = states[limit][slot].first;
            have_best = 1;
        }
    }
}

static int expack_verify_lzrep_payload(const unsigned char *payload, size_t payload_size, const unsigned char *data, size_t size) {
    unsigned char *decoded = (unsigned char *)rt_malloc(size == 0U ? 1U : size);
    size_t input_offset = 0U;
    size_t output_offset = 0U;
    unsigned int last_distance = 1U;
    int result = -1;

    if (decoded == 0) {
        return -1;
    }
    while (input_offset < payload_size && output_offset < size) {
        unsigned char flags = payload[input_offset++];
        unsigned int bit;

        for (bit = 0U; bit < 8U && output_offset < size; ++bit) {
            if ((flags & (unsigned char)(1U << bit)) != 0U) {
                unsigned int length;
                unsigned int distance;
                unsigned char token;

                if (input_offset >= payload_size) goto done;
                token = payload[input_offset++];
                if ((token & 0x80U) != 0U) {
                    length = (unsigned int)(token & 0x7fU) + COMPRESSION_LZSS_MIN_MATCH;
                    distance = last_distance;
                } else {
                    if (input_offset >= payload_size) goto done;
                    length = (unsigned int)(token & 0x0fU) + COMPRESSION_LZSS_MIN_MATCH;
                    distance = ((((unsigned int)token >> 4U) << 8U) | (unsigned int)payload[input_offset++]) + 1U;
                    last_distance = distance;
                }
                if (distance == 0U || (size_t)distance > output_offset || (size_t)length > size - output_offset) goto done;
                while (length > 0U) {
                    decoded[output_offset] = decoded[output_offset - (size_t)distance];
                    output_offset += 1U;
                    length -= 1U;
                }
            } else {
                if (input_offset >= payload_size) goto done;
                decoded[output_offset++] = payload[input_offset++];
            }
        }
    }
    if (input_offset == payload_size && output_offset == size && memcmp(decoded, data, size) == 0) {
        result = 0;
    }

done:
    rt_free(decoded);
    return result;
}

static unsigned int expack_lz4_hash4(const unsigned char *data) {
    return (tool_read_u32_le(data) * 2654435761U) >> (32U - EXPACK_LZ4_HASH_BITS);
}

static int expack_lz4_write_length(unsigned char *payload, size_t capacity, size_t *offset, size_t length) {
    while (length >= 255U) {
        if (*offset >= capacity) {
            return -1;
        }
        payload[(*offset)++] = 255U;
        length -= 255U;
    }
    if (*offset >= capacity) {
        return -1;
    }
    payload[(*offset)++] = (unsigned char)length;
    return 0;
}

static size_t expack_lz4_match_length(const unsigned char *data, size_t size, size_t position, size_t reference) {
    size_t length = EXPACK_LZ4_MIN_MATCH;
    size_t max_length = size - position;

    if (max_length > size - reference) {
        max_length = size - reference;
    }
    while (length < max_length && data[reference + length] == data[position + length]) {
        length += 1U;
    }
    return length;
}

static int expack_compress_lz4_block(const unsigned char *data, size_t size, unsigned char **payload_out, size_t *payload_size_out) {
    size_t hash_count = (size_t)1U << EXPACK_LZ4_HASH_BITS;
    size_t bound;
    size_t *hash_table;
    unsigned char *payload;
    size_t position = 0U;
    size_t anchor = 0U;
    size_t output_offset = 0U;

    if (size > (((size_t)-1) - 32U) / 2U) {
        return -1;
    }
    bound = size * 2U + 32U;
    hash_table = (size_t *)rt_malloc(hash_count * sizeof(*hash_table));
    if (hash_table == 0) {
        return -1;
    }
    payload = (unsigned char *)rt_malloc(bound == 0U ? 1U : bound);
    if (payload == 0) {
        rt_free(hash_table);
        return -1;
    }
    memset(hash_table, 0, hash_count * sizeof(*hash_table));

    while (position + EXPACK_LZ4_MIN_MATCH <= size) {
        unsigned int hash = expack_lz4_hash4(data + position);
        size_t reference_plus_one = hash_table[hash];
        size_t reference = reference_plus_one == 0U ? 0U : reference_plus_one - 1U;
        hash_table[hash] = position + 1U;

        if (reference_plus_one != 0U && position > reference && position - reference <= EXPACK_LZ4_MAX_DISTANCE &&
            data[reference] == data[position] && data[reference + 1U] == data[position + 1U] &&
            data[reference + 2U] == data[position + 2U] && data[reference + 3U] == data[position + 3U]) {
            size_t literal_length = position - anchor;
            size_t match_length = expack_lz4_match_length(data, size, position, reference);
            size_t match_token_length = match_length - EXPACK_LZ4_MIN_MATCH;
            size_t token_offset;
            unsigned char token = 0U;

            if (output_offset >= bound) {
                rt_free(payload);
                rt_free(hash_table);
                return -1;
            }
            token_offset = output_offset++;
            if (literal_length >= 15U) {
                token = 0xf0U;
                if (expack_lz4_write_length(payload, bound, &output_offset, literal_length - 15U) != 0) {
                    rt_free(payload);
                    rt_free(hash_table);
                    return -1;
                }
            } else {
                token = (unsigned char)(literal_length << 4U);
            }
            if (output_offset + literal_length + 2U > bound) {
                rt_free(payload);
                rt_free(hash_table);
                return -1;
            }
            memcpy(payload + output_offset, data + anchor, literal_length);
            output_offset += literal_length;
            payload[output_offset++] = (unsigned char)((position - reference) & 0xffU);
            payload[output_offset++] = (unsigned char)(((position - reference) >> 8U) & 0xffU);
            if (match_token_length >= 15U) {
                token |= 0x0fU;
                if (expack_lz4_write_length(payload, bound, &output_offset, match_token_length - 15U) != 0) {
                    rt_free(payload);
                    rt_free(hash_table);
                    return -1;
                }
            } else {
                token |= (unsigned char)match_token_length;
            }
            payload[token_offset] = token;
            position += match_length;
            anchor = position;
        } else {
            position += 1U;
        }
    }

    if (anchor < size) {
        size_t literal_length = size - anchor;
        unsigned char token;
        if (output_offset >= bound) {
            rt_free(payload);
            rt_free(hash_table);
            return -1;
        }
        if (literal_length >= 15U) {
            token = 0xf0U;
            payload[output_offset++] = token;
            if (expack_lz4_write_length(payload, bound, &output_offset, literal_length - 15U) != 0) {
                rt_free(payload);
                rt_free(hash_table);
                return -1;
            }
        } else {
            token = (unsigned char)(literal_length << 4U);
            payload[output_offset++] = token;
        }
        if (output_offset + literal_length > bound) {
            rt_free(payload);
            rt_free(hash_table);
            return -1;
        }
        memcpy(payload + output_offset, data + anchor, literal_length);
        output_offset += literal_length;
    }

    rt_free(hash_table);
    *payload_out = payload;
    *payload_size_out = output_offset;
    return 0;
}

static unsigned int expack_xlz_hash4(const unsigned char *data) {
    return (tool_read_u32_le(data) * 2246822519U) >> (32U - EXPACK_XLZ_HASH_BITS);
}

static size_t expack_xlz_extension_size(size_t length) {
    size_t bytes = 1U;

    while (length >= 255U) {
        bytes += 1U;
        length -= 255U;
    }
    return bytes;
}

static size_t expack_xlz_match_overhead(size_t match_length, size_t distance) {
    size_t token_length = match_length - EXPACK_XLZ_MIN_MATCH;
    size_t overhead = distance <= 256U ? 1U : 2U;

    if (token_length >= 15U) {
        overhead += expack_xlz_extension_size(token_length - 15U);
    }
    return overhead;
}

static size_t expack_xlz_literal_extension_size(size_t literal_length) {
    return literal_length >= 7U ? expack_xlz_extension_size(literal_length - 7U) : 0U;
}

static size_t expack_xlz_literal_step_cost(size_t literal_length) {
    size_t next_length = literal_length + 1U;
    size_t previous_extension = expack_xlz_literal_extension_size(literal_length);
    size_t next_extension = expack_xlz_literal_extension_size(next_length);

    return 1U + next_extension - previous_extension;
}

static int expack_xlz_write_extension(unsigned char *payload, size_t capacity, size_t *offset, size_t value) {
    while (value >= 255U) {
        if (*offset >= capacity) {
            return -1;
        }
        payload[(*offset)++] = 255U;
        value -= 255U;
    }
    if (*offset >= capacity) {
        return -1;
    }
    payload[(*offset)++] = (unsigned char)value;
    return 0;
}

static int expack_xlz_emit_sequence(unsigned char *payload, size_t capacity, size_t *offset, const unsigned char *data, size_t literal_start, size_t literal_length, size_t match_length, size_t distance) {
    size_t token_offset;
    size_t token_match_length = match_length == 0U ? 0U : match_length - EXPACK_XLZ_MIN_MATCH;
    unsigned char token = 0U;

    if (*offset >= capacity || literal_start > ((size_t)-1) - literal_length) {
        return -1;
    }
    token_offset = (*offset)++;
    if (literal_length >= 7U) {
        token = 0xe0U;
        if (expack_xlz_write_extension(payload, capacity, offset, literal_length - 7U) != 0) {
            return -1;
        }
    } else {
        token = (unsigned char)(literal_length << 5U);
    }
    if (*offset > capacity || literal_length > capacity - *offset) {
        return -1;
    }
    memcpy(payload + *offset, data + literal_start, literal_length);
    *offset += literal_length;
    if (match_length != 0U) {
        size_t encoded_distance = distance - 1U;

        if (distance == 0U || distance > EXPACK_XLZ_MAX_DISTANCE) {
            return -1;
        }
        if (distance <= 256U) {
            if (*offset >= capacity) {
                return -1;
            }
            payload[(*offset)++] = (unsigned char)encoded_distance;
        } else {
            if (*offset + 2U > capacity) {
                return -1;
            }
            token |= 0x10U;
            payload[(*offset)++] = (unsigned char)(encoded_distance & 0xffU);
            payload[(*offset)++] = (unsigned char)((encoded_distance >> 8U) & 0xffU);
        }
        if (token_match_length >= 15U) {
            token |= 0x0fU;
            if (expack_xlz_write_extension(payload, capacity, offset, token_match_length - 15U) != 0) {
                return -1;
            }
        } else {
            token |= (unsigned char)token_match_length;
        }
    }
    payload[token_offset] = token;
    return 0;
}

static void expack_xlz_insert_position(const unsigned char *data, size_t size, size_t position, size_t *head, size_t *previous) {
    unsigned int hash;

    if (position + 4U > size) {
        return;
    }
    hash = expack_xlz_hash4(data + position);
    previous[position] = head[hash];
    head[hash] = position + 1U;
}

typedef struct {
    size_t length;
    size_t distance;
} ExpackXlzMatch;

typedef struct {
    unsigned long long cost;
    size_t anchor;
    size_t previous_position;
    unsigned int previous_slot;
    size_t match_position;
    size_t match_length;
    size_t distance;
    unsigned char used;
} ExpackXlzParseState;

typedef struct {
    size_t position;
    size_t length;
    size_t distance;
} ExpackXlzSequence;

static void expack_xlz_add_match(ExpackXlzMatch *matches, unsigned int *match_count, size_t length, size_t distance) {
    unsigned int i;
    unsigned long long gain;
    unsigned int worst = 0U;
    unsigned long long worst_gain = 0ULL;

    if (length < EXPACK_XLZ_MIN_MATCH || length <= expack_xlz_match_overhead(length, distance)) {
        return;
    }
    for (i = 0U; i < *match_count; ++i) {
        if (matches[i].length == length && matches[i].distance == distance) {
            return;
        }
    }
    if (*match_count < EXPACK_XLZ_PARSE_MATCHES) {
        matches[*match_count].length = length;
        matches[*match_count].distance = distance;
        *match_count += 1U;
        return;
    }

    gain = (unsigned long long)(length - expack_xlz_match_overhead(length, distance));
    for (i = 0U; i < *match_count; ++i) {
        unsigned long long candidate_gain = (unsigned long long)(matches[i].length - expack_xlz_match_overhead(matches[i].length, matches[i].distance));

        if (i == 0U || candidate_gain < worst_gain || (candidate_gain == worst_gain && matches[i].distance > matches[worst].distance)) {
            worst = i;
            worst_gain = candidate_gain;
        }
    }
    if (gain > worst_gain || (gain == worst_gain && distance < matches[worst].distance)) {
        matches[worst].length = length;
        matches[worst].distance = distance;
    }
}

static unsigned int expack_xlz_find_matches(const unsigned char *data, size_t size, size_t position, const size_t *head, const size_t *previous, ExpackXlzMatch *matches) {
    unsigned int match_count = 0U;
    size_t link;
    unsigned int attempts = 0U;

    if (position + EXPACK_XLZ_MIN_MATCH > size) {
        return 0U;
    }
    link = head[expack_xlz_hash4(data + position)];
    while (link != 0U && attempts < EXPACK_XLZ_PARSE_ATTEMPTS) {
        size_t candidate = link - 1U;
        size_t distance;
        size_t length;

        if (candidate >= position) {
            break;
        }
        distance = position - candidate;
        if (distance > EXPACK_XLZ_MAX_DISTANCE) {
            break;
        }
        length = expack_match_length_at(data, size, position, distance, size - position);
        if (length >= EXPACK_XLZ_MIN_MATCH) {
            size_t short_length;
            size_t boundary_length = 18U;

            expack_xlz_add_match(matches, &match_count, length, distance);
            for (short_length = EXPACK_XLZ_MIN_MATCH; short_length <= 18U && short_length <= length; ++short_length) {
                expack_xlz_add_match(matches, &match_count, short_length, distance);
            }
            while (boundary_length <= length) {
                expack_xlz_add_match(matches, &match_count, boundary_length, distance);
                if (boundary_length > EXPACK_XLZ_MIN_MATCH) {
                    expack_xlz_add_match(matches, &match_count, boundary_length - 1U, distance);
                }
                if (boundary_length > ((size_t)-1) - 255U) {
                    break;
                }
                boundary_length += 255U;
            }
        }
        link = previous[candidate];
        attempts += 1U;
    }
    return match_count;
}

static ExpackXlzParseState *expack_xlz_state_at(ExpackXlzParseState *states, size_t position, unsigned int slot) {
    return states + position * EXPACK_XLZ_PARSE_BEAM + slot;
}

static int expack_xlz_insert_state(ExpackXlzParseState *states, unsigned int *state_counts, size_t position, const ExpackXlzParseState *state) {
    unsigned int count = state_counts[position];
    unsigned int slot;
    unsigned int insert_at = count;

    for (slot = 0U; slot < count; ++slot) {
        ExpackXlzParseState *existing = expack_xlz_state_at(states, position, slot);

        if (existing->used && existing->anchor == state->anchor) {
            if (existing->cost <= state->cost) {
                return 0;
            }
            *existing = *state;
            return 0;
        }
        if (state->cost < existing->cost && insert_at == count) {
            insert_at = slot;
        }
    }
    if (count < EXPACK_XLZ_PARSE_BEAM) {
        unsigned int move;

        for (move = count; move > insert_at; --move) {
            *expack_xlz_state_at(states, position, move) = *expack_xlz_state_at(states, position, move - 1U);
        }
        *expack_xlz_state_at(states, position, insert_at) = *state;
        state_counts[position] = count + 1U;
        return 0;
    }
    if (insert_at < EXPACK_XLZ_PARSE_BEAM) {
        unsigned int move;

        for (move = EXPACK_XLZ_PARSE_BEAM - 1U; move > insert_at; --move) {
            *expack_xlz_state_at(states, position, move) = *expack_xlz_state_at(states, position, move - 1U);
        }
        *expack_xlz_state_at(states, position, insert_at) = *state;
    }
    return 0;
}

static int expack_verify_xlz_payload(const unsigned char *payload, size_t payload_size, const unsigned char *data, size_t size) {
    unsigned char *decoded = (unsigned char *)rt_malloc(size == 0U ? 1U : size);
    size_t input_offset = 0U;
    size_t output_offset = 0U;
    int result = -1;

    if (decoded == 0) {
        return -1;
    }
    if (size == 0U && payload_size == 0U) {
        result = 0;
        goto done;
    }
    while (output_offset < size && input_offset < payload_size) {
        unsigned char token = payload[input_offset++];
        size_t literal_length = (size_t)(token >> 5U);
        size_t match_length = (size_t)(token & 0x0fU) + EXPACK_XLZ_MIN_MATCH;
        size_t distance;

        if (literal_length == 7U) {
            unsigned char extra;
            do {
                if (input_offset >= payload_size) goto done;
                extra = payload[input_offset++];
                literal_length += (size_t)extra;
            } while (extra == 255U);
        }
        if (literal_length > payload_size - input_offset || literal_length > size - output_offset) goto done;
        memcpy(decoded + output_offset, payload + input_offset, literal_length);
        input_offset += literal_length;
        output_offset += literal_length;
        if (output_offset == size) {
            if (input_offset == payload_size && memcmp(decoded, data, size) == 0) {
                result = 0;
            }
            goto done;
        }
        if ((token & 0x10U) != 0U) {
            if (payload_size - input_offset < 2U) goto done;
            distance = (size_t)payload[input_offset] | ((size_t)payload[input_offset + 1U] << 8U);
            input_offset += 2U;
        } else {
            if (input_offset >= payload_size) goto done;
            distance = (size_t)payload[input_offset++];
        }
        distance += 1U;
        if ((token & 0x0fU) == 0x0fU) {
            unsigned char extra;
            do {
                if (input_offset >= payload_size) goto done;
                extra = payload[input_offset++];
                match_length += (size_t)extra;
            } while (extra == 255U);
        }
        if (distance > output_offset || match_length > size - output_offset) goto done;
        while (match_length > 0U) {
            decoded[output_offset] = decoded[output_offset - distance];
            output_offset += 1U;
            match_length -= 1U;
        }
    }
    if (input_offset == payload_size && output_offset == size && memcmp(decoded, data, size) == 0) {
        result = 0;
    }

done:
    rt_free(decoded);
    return result;
}

static int expack_compress_xlz(const unsigned char *data, size_t size, unsigned char **payload_out, size_t *payload_size_out) {
    size_t hash_count = (size_t)1U << EXPACK_XLZ_HASH_BITS;
    size_t bound;
    size_t *head;
    size_t *previous;
    ExpackXlzParseState *states;
    unsigned int *state_counts;
    ExpackXlzSequence *sequences;
    unsigned char *payload;
    size_t position;
    size_t output_offset = 0U;
    unsigned int best_slot = 0U;
    unsigned int sequence_count = 0U;
    unsigned int sequence_index;

    if (size > (((size_t)-1) - 64U) / 2U || size > ((size_t)-1) / sizeof(*previous) || hash_count > ((size_t)-1) / sizeof(*head) ||
        size + 1U == 0U || (size + 1U) > ((size_t)-1) / EXPACK_XLZ_PARSE_BEAM ||
        (size + 1U) * EXPACK_XLZ_PARSE_BEAM > ((size_t)-1) / sizeof(*states) ||
        (size + 1U) > ((size_t)-1) / sizeof(*state_counts) || (size == 0U ? 1U : size) > ((size_t)-1) / sizeof(*sequences)) {
        return -1;
    }
    bound = size * 2U + 64U;
    head = (size_t *)rt_malloc(hash_count * sizeof(*head));
    if (head == 0) {
        return -1;
    }
    previous = (size_t *)rt_malloc((size == 0U ? 1U : size) * sizeof(*previous));
    if (previous == 0) {
        rt_free(head);
        return -1;
    }
    states = (ExpackXlzParseState *)rt_malloc((size + 1U) * EXPACK_XLZ_PARSE_BEAM * sizeof(*states));
    if (states == 0) {
        rt_free(previous);
        rt_free(head);
        return -1;
    }
    state_counts = (unsigned int *)rt_malloc((size + 1U) * sizeof(*state_counts));
    if (state_counts == 0) {
        rt_free(states);
        rt_free(previous);
        rt_free(head);
        return -1;
    }
    sequences = (ExpackXlzSequence *)rt_malloc((size == 0U ? 1U : size) * sizeof(*sequences));
    if (sequences == 0) {
        rt_free(state_counts);
        rt_free(states);
        rt_free(previous);
        rt_free(head);
        return -1;
    }
    payload = (unsigned char *)rt_malloc(bound == 0U ? 1U : bound);
    if (payload == 0) {
        rt_free(sequences);
        rt_free(state_counts);
        rt_free(states);
        rt_free(previous);
        rt_free(head);
        return -1;
    }
    memset(head, 0, hash_count * sizeof(*head));
    memset(state_counts, 0, (size + 1U) * sizeof(*state_counts));

    {
        ExpackXlzParseState initial_state;

        memset(&initial_state, 0, sizeof(initial_state));
        initial_state.cost = size == 0U ? 0ULL : 1ULL;
        initial_state.used = 1U;
        expack_xlz_insert_state(states, state_counts, 0U, &initial_state);
    }

    for (position = 0U; position < size; ++position) {
        unsigned int slot;
        ExpackXlzMatch matches[EXPACK_XLZ_PARSE_MATCHES];
        unsigned int match_count;

        if (position > 0U) {
            expack_xlz_insert_position(data, size, position - 1U, head, previous);
        }
        if (state_counts[position] == 0U) {
            continue;
        }
        match_count = expack_xlz_find_matches(data, size, position, head, previous, matches);
        for (slot = 0U; slot < state_counts[position]; ++slot) {
            ExpackXlzParseState *state = expack_xlz_state_at(states, position, slot);
            ExpackXlzParseState next_state;
            size_t literal_length = position - state->anchor;
            unsigned int match_index;

            next_state = *state;
            next_state.cost += (unsigned long long)expack_xlz_literal_step_cost(literal_length);
            next_state.previous_position = position;
            next_state.previous_slot = slot;
            next_state.match_length = 0U;
            next_state.distance = 0U;
            next_state.match_position = 0U;
            next_state.used = 1U;
            expack_xlz_insert_state(states, state_counts, position + 1U, &next_state);

            for (match_index = 0U; match_index < match_count; ++match_index) {
                size_t end_position = position + matches[match_index].length;
                size_t match_overhead;

                if (end_position > size || matches[match_index].distance > position) {
                    continue;
                }
                match_overhead = expack_xlz_match_overhead(matches[match_index].length, matches[match_index].distance);
                memset(&next_state, 0, sizeof(next_state));
                next_state.cost = state->cost + (unsigned long long)match_overhead;
                if (end_position < size) {
                    next_state.cost += 1ULL;
                }
                next_state.anchor = end_position;
                next_state.previous_position = position;
                next_state.previous_slot = slot;
                next_state.match_position = position;
                next_state.match_length = matches[match_index].length;
                next_state.distance = matches[match_index].distance;
                next_state.used = 1U;
                expack_xlz_insert_state(states, state_counts, end_position, &next_state);
            }
        }
    }

    if (state_counts[size] == 0U) {
        rt_free(payload);
        rt_free(sequences);
        rt_free(state_counts);
        rt_free(states);
        rt_free(previous);
        rt_free(head);
        return -1;
    }
    best_slot = 0U;

    {
        size_t trace_position = size;
        unsigned int trace_slot = best_slot;

        while (trace_position != 0U || trace_slot != 0U) {
            ExpackXlzParseState *state = expack_xlz_state_at(states, trace_position, trace_slot);

            if (state->match_length != 0U) {
                if (sequence_count >= size) {
                    rt_free(payload);
                    rt_free(sequences);
                    rt_free(state_counts);
                    rt_free(states);
                    rt_free(previous);
                    rt_free(head);
                    return -1;
                }
                sequences[sequence_count].position = state->match_position;
                sequences[sequence_count].length = state->match_length;
                sequences[sequence_count].distance = state->distance;
                sequence_count += 1U;
            }
            if (state->previous_position == 0U && state->previous_slot == 0U && trace_position == 0U) {
                break;
            }
            trace_position = state->previous_position;
            trace_slot = state->previous_slot;
        }
    }

    {
        size_t anchor = 0U;

        for (sequence_index = sequence_count; sequence_index > 0U; --sequence_index) {
            ExpackXlzSequence *sequence = sequences + sequence_index - 1U;

            if (expack_xlz_emit_sequence(payload, bound, &output_offset, data, anchor, sequence->position - anchor, sequence->length, sequence->distance) != 0) {
                rt_free(payload);
                rt_free(sequences);
                rt_free(state_counts);
                rt_free(states);
                rt_free(previous);
                rt_free(head);
                return -1;
            }
            anchor = sequence->position + sequence->length;
        }
        if (anchor < size || (output_offset == 0U && size != 0U)) {
            if (expack_xlz_emit_sequence(payload, bound, &output_offset, data, anchor, size - anchor, 0U, 0U) != 0) {
                rt_free(payload);
                rt_free(sequences);
                rt_free(state_counts);
                rt_free(states);
                rt_free(previous);
                rt_free(head);
                return -1;
            }
        }
        if (size == 0U) {
            output_offset = 0U;
        }
    }
    rt_free(sequences);
    rt_free(state_counts);
    rt_free(states);
    rt_free(previous);
    rt_free(head);
    if (expack_verify_xlz_payload(payload, output_offset, data, size) != 0) {
        rt_free(payload);
        return -1;
    }
    *payload_out = payload;
    *payload_size_out = output_offset;
    return 0;
}

static int expack_compress_lzrep_parse(const unsigned char *data, size_t size, const ExpackLzrepParseParams *params, unsigned char **payload_out, size_t *payload_size_out) {
    size_t bound = compression_lzss_bound(size);
    ExpackLzrepMatchIndex match_index;
    unsigned char *payload;
    size_t input_offset = 0U;
    size_t output_offset = 0U;
    unsigned int last_distance = 1U;

    if (bound == 0U && size != 0U) {
        return -1;
    }
    payload = (unsigned char *)rt_malloc(bound == 0U ? 1U : bound);
    if (payload == 0) {
        return -1;
    }
    if (expack_lzrep_match_index_build(data, size, &match_index) != 0) {
        match_index.head = 0;
        match_index.next = 0;
        match_index.available = 0;
    }

    while (input_offset < size) {
        size_t flag_offset;
        unsigned char flags = 0U;
        unsigned int bit;

        if (output_offset >= bound) {
            rt_free(payload);
            return -1;
        }
        flag_offset = output_offset++;

        for (bit = 0U; bit < 8U && input_offset < size; ++bit) {
            ExpackLzrepToken token;

            expack_choose_lzrep_token(data, size, input_offset, &match_index, params, last_distance, bit, &token);
            if (token.kind == EXPACK_LZREP_TOKEN_REPEAT) {
                if (output_offset >= bound) {
                    expack_lzrep_match_index_release(&match_index);
                    rt_free(payload);
                    return -1;
                }
                flags |= (unsigned char)(1U << bit);
                payload[output_offset++] = (unsigned char)(0x80U | (unsigned int)(token.length - COMPRESSION_LZSS_MIN_MATCH));
                input_offset += (size_t)token.length;
            } else if (token.kind == EXPACK_LZREP_TOKEN_EXPLICIT) {
                unsigned int token_distance = (unsigned int)token.distance - 1U;
                unsigned int token_length = (unsigned int)(token.length - COMPRESSION_LZSS_MIN_MATCH);

                if (output_offset + 2U > bound) {
                    expack_lzrep_match_index_release(&match_index);
                    rt_free(payload);
                    return -1;
                }
                flags |= (unsigned char)(1U << bit);
                payload[output_offset++] = (unsigned char)(((token_distance >> 8U) << 4U) | token_length);
                payload[output_offset++] = (unsigned char)(token_distance & 0xffU);
                last_distance = (unsigned int)token.distance;
                input_offset += (size_t)token.length;
            } else {
                if (output_offset >= bound) {
                    expack_lzrep_match_index_release(&match_index);
                    rt_free(payload);
                    return -1;
                }
                payload[output_offset++] = data[input_offset++];
            }
        }
        payload[flag_offset] = flags;
    }

    if (expack_verify_lzrep_payload(payload, output_offset, data, size) != 0) {
        expack_lzrep_match_index_release(&match_index);
        rt_free(payload);
        return -1;
    }
    expack_lzrep_match_index_release(&match_index);
    *payload_out = payload;
    *payload_size_out = output_offset;
    return 0;
}

static int expack_compress_lzrep(const unsigned char *data, size_t size, unsigned char **payload_out, size_t *payload_size_out) {
    return expack_compress_lzrep_parse(data, size, &expack_lzrep_fast_parse, payload_out, payload_size_out);
}

static void expack_x86_bcj_transform(unsigned char *data, size_t size, int include_rip_relative) {
    size_t position = 0U;

    while (position + 5U < size) {
        if (data[position] == 0xe8U || data[position] == 0xe9U) {
            unsigned int value = archive_read_u32_le(data + position + 1U);
            value += (unsigned int)(position + 5U);
            archive_store_u32_le(data + position + 1U, value);
            position += 5U;
        } else if (position + 5U < size && data[position] == 0x0fU && (data[position + 1U] & 0xf0U) == 0x80U) {
            unsigned int value = archive_read_u32_le(data + position + 2U);
            value += (unsigned int)(position + 6U);
            archive_store_u32_le(data + position + 2U, value);
            position += 6U;
        } else if (include_rip_relative && position + 5U < size && (data[position] == 0x8bU || data[position] == 0x8dU) && (data[position + 1U] & 0xc7U) == 0x05U) {
            unsigned int value = archive_read_u32_le(data + position + 2U);
            value += (unsigned int)(position + 6U);
            archive_store_u32_le(data + position + 2U, value);
            position += 6U;
        } else if (include_rip_relative && position + 6U < size && data[position] >= 0x40U && data[position] <= 0x4fU && (data[position + 1U] == 0x8bU || data[position + 1U] == 0x8dU) && (data[position + 2U] & 0xc7U) == 0x05U) {
            unsigned int value = archive_read_u32_le(data + position + 3U);
            value += (unsigned int)(position + 7U);
            archive_store_u32_le(data + position + 3U, value);
            position += 7U;
        } else {
            position += 1U;
        }
    }
}

static unsigned char *expack_make_bcj_transformed_input(const unsigned char *input_data, size_t input_size, int include_rip_relative) {
    unsigned char *transformed = (unsigned char *)rt_malloc(input_size == 0U ? 1U : input_size);

    if (transformed == 0) {
        return 0;
    }
    memcpy(transformed, input_data, input_size);
    expack_x86_bcj_transform(transformed, input_size, include_rip_relative);
    return transformed;
}

#include "deflate.c"

static int expack_compress_xlz_bcj(const unsigned char *input_data, size_t input_size, unsigned char **payload_out, size_t *payload_size_out) {
    unsigned char *transformed;
    int result;

    transformed = (unsigned char *)rt_malloc(input_size == 0U ? 1U : input_size);
    if (transformed == 0) {
        return -1;
    }
    memcpy(transformed, input_data, input_size);
    expack_x86_bcj_transform(transformed, input_size, 0);
    result = expack_compress_xlz(transformed, input_size, payload_out, payload_size_out);
    rt_free(transformed);
    return result;
}

static int expack_compress_lzss_bcj_profile(const ExpackLzssProfile *profile, const unsigned char *input_data, size_t input_size, unsigned char **payload_out, size_t *payload_size_out) {
    unsigned char *transformed;
    int result;

    transformed = (unsigned char *)rt_malloc(input_size == 0U ? 1U : input_size);
    if (transformed == 0) {
        return -1;
    }
    memcpy(transformed, input_data, input_size);
    expack_x86_bcj_transform(transformed, input_size, 0);
    result = expack_compress_lzss_profile(profile, transformed, input_size, payload_out, payload_size_out);
    rt_free(transformed);
    return result;
}

static int expack_compress_lzss_bcj_rip_profile(const ExpackLzssProfile *profile, const unsigned char *input_data, size_t input_size, unsigned char **payload_out, size_t *payload_size_out) {
    unsigned char *transformed;
    int result;

    transformed = (unsigned char *)rt_malloc(input_size == 0U ? 1U : input_size);
    if (transformed == 0) {
        return -1;
    }
    memcpy(transformed, input_data, input_size);
    expack_x86_bcj_transform(transformed, input_size, 1);
    result = expack_compress_lzss_profile(profile, transformed, input_size, payload_out, payload_size_out);
    rt_free(transformed);
    return result;
}

static int expack_compress_lzss_profile(const ExpackLzssProfile *profile, const unsigned char *input_data, size_t input_size, unsigned char **payload_out, size_t *payload_size_out) {
    size_t bound = compression_lzss_bound(input_size);
    unsigned char *payload;

    if (bound == 0U && input_size != 0U) {
        return -1;
    }
    payload = (unsigned char *)rt_malloc(bound == 0U ? 1U : bound);
    if (payload == 0) {
        return -1;
    }
    if (compression_lzss_compress_profile(profile->profile_id, input_data, input_size, payload, bound, payload_size_out) != 0) {
        rt_free(payload);
        return -1;
    }
    *payload_out = payload;
    return 0;
}

static unsigned long long expack_packed_size(size_t stub_size, size_t payload_size) {
    return (unsigned long long)EXPACK_ELF_CODE_OFFSET + (unsigned long long)stub_size + (unsigned long long)payload_size;
}

static unsigned long long expack_score_candidate(const ExpackInputFormat *format, const ExpackOutputBackend *backend, const ExpackCandidate *candidate) {
    if (backend != 0 && backend->score_candidate != 0) {
        return backend->score_candidate(format, candidate);
    }
    (void)format;
    return expack_packed_size(candidate->stub_size, candidate->payload_size);
}

static void expack_candidate_release(ExpackCandidate *candidate) {
    if (candidate->payload != 0) {
        rt_free(candidate->payload);
    }
    candidate->codec = 0U;
    candidate->lzss_profile = 0;
    candidate->lzrep_parse = EXPACK_LZREP_PARSE_FAST;
    candidate->stub = 0;
    candidate->stub_size = 0U;
    candidate->payload = 0;
    candidate->payload_size = 0U;
    candidate->packed_size = 0ULL;
}

static void expack_candidate_take(ExpackCandidate *selected, ExpackCandidate *candidate) {
    expack_candidate_release(selected);
    *selected = *candidate;
    candidate->payload = 0;
}

static int expack_consider_candidate(ExpackCandidate *selected, int *have_selected, ExpackCandidate *candidate) {
    if (!*have_selected || candidate->packed_size < selected->packed_size ||
        (candidate->packed_size == selected->packed_size && candidate->payload_size < selected->payload_size)) {
        expack_candidate_take(selected, candidate);
        *have_selected = 1;
    }
    expack_candidate_release(candidate);
    return 0;
}

static int expack_make_raw_candidate(const unsigned char *input_data, size_t input_size, ExpackCandidate *candidate) {
    unsigned char *payload = (unsigned char *)rt_malloc(input_size == 0U ? 1U : input_size);
    if (payload == 0) {
        return -1;
    }
    if (input_size != 0U) {
        memcpy(payload, input_data, input_size);
    }
    expack_candidate_release(candidate);
    candidate->codec = EXPACK_CODEC_RAW;
    candidate->lzss_profile = 0;
    candidate->lzrep_parse = EXPACK_LZREP_PARSE_FAST;
    candidate->stub = 0;
    candidate->stub_size = 0U;
    candidate->payload = payload;
    candidate->payload_size = input_size;
    candidate->packed_size = (unsigned long long)input_size;
    return 0;
}

static void expack_report_candidate(const ExpackCandidate *candidate) {
    rt_write_cstr(1, "  ");
    expack_write_candidate_name(candidate);
    rt_write_cstr(1, ": payload ");
    rt_write_uint(1, (unsigned long long)candidate->payload_size);
    rt_write_cstr(1, ", stub ");
    rt_write_uint(1, (unsigned long long)candidate->stub_size);
    if (candidate->packed_size == EXPACK_CANDIDATE_UNSUPPORTED_SIZE) {
        rt_write_cstr(1, ", packed unsupported");
    } else {
        rt_write_cstr(1, ", packed ");
        rt_write_uint(1, candidate->packed_size);
    }
    rt_write_cstr(1, "\n");
}

static int expack_lzss_profile_in_normal_portfolio(const ExpackInputFormat *format, const ExpackLzssProfile *profile) {
    if (format->kind == EXPACK_FORMAT_MACHO) {
        return profile->profile_id == COMPRESSION_LZSS_PROFILE_WIDE_WINDOW ||
               profile->profile_id == COMPRESSION_LZSS_PROFILE_WIDE_MATCH ||
               profile->profile_id == COMPRESSION_LZSS_PROFILE_LONG_MATCH;
    }
    if (format->kind == EXPACK_FORMAT_PE_COFF) {
        return profile->profile_id == COMPRESSION_LZSS_PROFILE_WIDE_WINDOW;
    }
    return profile->profile_id == COMPRESSION_LZSS_PROFILE_LONG_MATCH;
}

static int expack_bcj_profile_in_normal_portfolio(const ExpackInputFormat *format, const ExpackLzssProfile *profile) {
    if (format->kind == EXPACK_FORMAT_PE_COFF) {
        return profile->profile_id == COMPRESSION_LZSS_PROFILE_WIDE_WINDOW;
    }
    return profile->profile_id == COMPRESSION_LZSS_PROFILE_WIDE_WINDOW ||
           profile->profile_id == COMPRESSION_LZSS_PROFILE_WIDE_MATCH ||
           profile->profile_id == COMPRESSION_LZSS_PROFILE_MEDIUM_MATCH;
}

static int expack_bcj_rip_profile_in_normal_portfolio(const ExpackLzssProfile *profile) {
    return profile->profile_id == COMPRESSION_LZSS_PROFILE_WIDE_WINDOW ||
           profile->profile_id == COMPRESSION_LZSS_PROFILE_WIDE_MATCH;
}

#define EXPACK_MAX_CANDIDATE_JOBS 20U
#define EXPACK_DEFAULT_MAX_WORKERS 8U

typedef struct {
    unsigned int codec;
    const ExpackLzssProfile *lzss_profile;
    unsigned int lzrep_parse;
    const unsigned char *input_data;
    size_t input_size;
    int input_pretransformed;
    ExpackCandidate candidate;
    unsigned long long elapsed_ns;
    int succeeded;
    int done;
} ExpackCandidateJob;

static void expack_candidate_job_init(ExpackCandidateJob *job, unsigned int codec, const ExpackLzssProfile *profile, unsigned int lzrep_parse, const unsigned char *input_data, size_t input_size, int input_pretransformed) {
    job->codec = codec;
    job->lzss_profile = profile;
    job->lzrep_parse = lzrep_parse;
    job->input_data = input_data;
    job->input_size = input_size;
    job->input_pretransformed = input_pretransformed;
    job->candidate.codec = codec;
    job->candidate.lzss_profile = profile;
    job->candidate.lzrep_parse = lzrep_parse;
    job->candidate.payload = 0;
    job->candidate.payload_size = 0U;
    job->candidate.packed_size = 0ULL;
    job->elapsed_ns = 0ULL;
    job->succeeded = 0;
    job->done = 0;
    if (codec == EXPACK_CODEC_LZSS) {
        job->candidate.stub = expack_stub_x86_64;
        job->candidate.stub_size = sizeof(expack_stub_x86_64);
    } else if (codec == EXPACK_CODEC_LZREP) {
        job->candidate.stub = expack_lzrep_stub_x86_64;
        job->candidate.stub_size = sizeof(expack_lzrep_stub_x86_64);
    } else if (codec == EXPACK_CODEC_LZSS_BCJ) {
        job->candidate.stub = expack_lzss_bcj_stub_x86_64;
        job->candidate.stub_size = sizeof(expack_lzss_bcj_stub_x86_64);
    } else if (codec == EXPACK_CODEC_LZSS_BCJ_RIP) {
        job->candidate.stub = expack_lzss_bcj_rip_stub_x86_64;
        job->candidate.stub_size = sizeof(expack_lzss_bcj_rip_stub_x86_64);
    } else if (codec == EXPACK_CODEC_LZ4) {
        job->candidate.stub = expack_lz4_stub_x86_64;
        job->candidate.stub_size = sizeof(expack_lz4_stub_x86_64);
    } else if (codec == EXPACK_CODEC_XLZ) {
        job->candidate.stub = expack_xlz_stub_x86_64;
        job->candidate.stub_size = sizeof(expack_xlz_stub_x86_64);
    } else if (codec == EXPACK_CODEC_XLZ_BCJ) {
        job->candidate.stub = expack_xlz_bcj_stub_x86_64;
        job->candidate.stub_size = sizeof(expack_xlz_bcj_stub_x86_64);
    } else if (codec == EXPACK_CODEC_DEFLATE_BCJ) {
        job->candidate.stub = expack_deflate_bcj_stub_x86_64;
        job->candidate.stub_size = sizeof(expack_deflate_bcj_stub_x86_64);
    } else {
        job->candidate.stub = 0;
        job->candidate.stub_size = 0U;
    }
}

static int expack_add_prepared_candidate_job(ExpackCandidateJob *jobs, unsigned int *job_count, unsigned int codec, const ExpackLzssProfile *profile, unsigned int lzrep_parse, const unsigned char *input_data, size_t input_size, int input_pretransformed) {
    if (*job_count >= EXPACK_MAX_CANDIDATE_JOBS) {
        return -1;
    }
    expack_candidate_job_init(jobs + *job_count, codec, profile, lzrep_parse, input_data, input_size, input_pretransformed);
    *job_count += 1U;
    return 0;
}

static int expack_add_candidate_job(ExpackCandidateJob *jobs, unsigned int *job_count, unsigned int codec, const ExpackLzssProfile *profile, unsigned int lzrep_parse, const unsigned char *input_data, size_t input_size) {
    return expack_add_prepared_candidate_job(jobs, job_count, codec, profile, lzrep_parse, input_data, input_size, 0);
}

static void expack_run_candidate_job(ExpackCandidateJob *job) {
    unsigned long long start_ns = platform_get_monotonic_time_ns();
    unsigned long long end_ns;
    int result = -1;

    if (job->codec == EXPACK_CODEC_LZSS) {
        result = expack_compress_lzss_profile(job->lzss_profile, job->input_data, job->input_size, &job->candidate.payload, &job->candidate.payload_size);
    } else if (job->codec == EXPACK_CODEC_LZREP) {
        if (job->lzrep_parse == EXPACK_LZREP_PARSE_OPT) {
            result = expack_compress_lzrep_parse(job->input_data, job->input_size, &expack_lzrep_opt_parse, &job->candidate.payload, &job->candidate.payload_size);
        } else {
            result = expack_compress_lzrep(job->input_data, job->input_size, &job->candidate.payload, &job->candidate.payload_size);
        }
    } else if (job->codec == EXPACK_CODEC_LZSS_BCJ) {
        if (job->input_pretransformed) {
            result = expack_compress_lzss_profile(job->lzss_profile, job->input_data, job->input_size, &job->candidate.payload, &job->candidate.payload_size);
        } else {
            result = expack_compress_lzss_bcj_profile(job->lzss_profile, job->input_data, job->input_size, &job->candidate.payload, &job->candidate.payload_size);
        }
    } else if (job->codec == EXPACK_CODEC_LZSS_BCJ_RIP) {
        if (job->input_pretransformed) {
            result = expack_compress_lzss_profile(job->lzss_profile, job->input_data, job->input_size, &job->candidate.payload, &job->candidate.payload_size);
        } else {
            result = expack_compress_lzss_bcj_rip_profile(job->lzss_profile, job->input_data, job->input_size, &job->candidate.payload, &job->candidate.payload_size);
        }
    } else if (job->codec == EXPACK_CODEC_LZ4) {
        result = expack_compress_lz4_block(job->input_data, job->input_size, &job->candidate.payload, &job->candidate.payload_size);
    } else if (job->codec == EXPACK_CODEC_XLZ) {
        result = expack_compress_xlz(job->input_data, job->input_size, &job->candidate.payload, &job->candidate.payload_size);
    } else if (job->codec == EXPACK_CODEC_XLZ_BCJ) {
        if (job->input_pretransformed) {
            result = expack_compress_xlz(job->input_data, job->input_size, &job->candidate.payload, &job->candidate.payload_size);
        } else {
            result = expack_compress_xlz_bcj(job->input_data, job->input_size, &job->candidate.payload, &job->candidate.payload_size);
        }
    } else if (job->codec == EXPACK_CODEC_DEFLATE_BCJ) {
        if (job->input_pretransformed) {
            result = expack_compress_deflate_transformed(job->input_data, job->input_size, &job->candidate.payload, &job->candidate.payload_size);
        } else {
            result = expack_compress_deflate_bcj(job->input_data, job->input_size, &job->candidate.payload, &job->candidate.payload_size);
        }
    }
    end_ns = platform_get_monotonic_time_ns();
    job->elapsed_ns = end_ns >= start_ns ? end_ns - start_ns : 0ULL;
    job->succeeded = result == 0;
    job->done = 1;
}

static void expack_run_candidate_jobs_serial(ExpackCandidateJob *jobs, unsigned int job_count) {
    unsigned int job_index;

    for (job_index = 0U; job_index < job_count; ++job_index) {
        expack_run_candidate_job(jobs + job_index);
    }
}

static unsigned int expack_worker_count_from_env(void) {
    const char *value_text = platform_getenv("NEWOS_EXPACK_WORKERS");
    unsigned long long value;
    unsigned int platform_width;

    if (value_text == 0 || value_text[0] == '\0') {
        platform_width = platform_worker_thread_count();
        if (platform_width == 0U) return 0U;
        return platform_width > EXPACK_DEFAULT_MAX_WORKERS ? EXPACK_DEFAULT_MAX_WORKERS : platform_width;
    }
    if (rt_parse_uint(value_text, &value) != 0) {
        return EXPACK_DEFAULT_MAX_WORKERS;
    }
    if (value > RT_TASK_POOL_MAX_WORKERS) {
        return RT_TASK_POOL_MAX_WORKERS;
    }
    return (unsigned int)value;
}

static int expack_candidate_task(unsigned int worker_index, void *arg) {
    (void)worker_index;
    expack_run_candidate_job((ExpackCandidateJob *)arg);
    return 0;
}

static void expack_run_candidate_jobs(ExpackCandidateJob *jobs, unsigned int job_count) {
    RtTaskPool pool;
    RtTaskGroup group;
    unsigned int job_index;
    int result;

    if (job_count < 2U) {
        expack_run_candidate_jobs_serial(jobs, job_count);
        return;
    }
    rt_memset(&pool, 0, sizeof(pool));
    if (rt_task_pool_init(&pool, expack_worker_count_from_env()) != 0) {
        expack_run_candidate_jobs_serial(jobs, job_count);
        return;
    }
    if (rt_task_group_begin(&pool, &group) != 0 || rt_task_group_reserve(&group, job_count) != 0) {
        rt_task_pool_destroy(&pool);
        expack_run_candidate_jobs_serial(jobs, job_count);
        return;
    }
    result = 0;
    for (job_index = 0U; job_index < job_count; ++job_index) {
        if (rt_task_group_submit(&group, expack_candidate_task, jobs + job_index) != 0) {
            result = -1;
            break;
        }
    }
    if (rt_task_group_wait(&group) != 0) {
        result = -1;
    }
    rt_task_pool_destroy(&pool);
    if (result != 0) {
        for (job_index = 0U; job_index < job_count; ++job_index) {
            if (!jobs[job_index].done) {
                expack_run_candidate_job(jobs + job_index);
            }
        }
    }
}

static int expack_report_candidate_timings(void) {
    const char *value_text = platform_getenv("NEWOS_EXPACK_TIMINGS");

    return value_text != 0 && value_text[0] != '\0' && !(value_text[0] == '0' && value_text[1] == '\0');
}

static void expack_report_candidate_timing(const ExpackCandidateJob *job) {
    rt_write_cstr(1, "  time ");
    expack_write_candidate_name(&job->candidate);
    rt_write_cstr(1, ": ");
    rt_write_uint(1, job->elapsed_ns);
    rt_write_cstr(1, " ns\n");
}

static int expack_select_best_payload(const ExpackInputFormat *format, const ExpackOutputBackend *backend, const unsigned char *input_data, size_t input_size, ExpackCandidate *selected_out, int report_candidates, int allow_x86_bcj, int try_all_candidates) {
    unsigned int profile_index;
    ExpackCandidateJob jobs[EXPACK_MAX_CANDIDATE_JOBS];
    unsigned int job_count = 0U;
    unsigned int job_index;
    int have_selected = 0;
    int report_timings = expack_report_candidate_timings();
    unsigned char *bcj_transformed = 0;
    unsigned char *bcj_rip_transformed = 0;

    expack_candidate_release(selected_out);
    for (profile_index = 0U; profile_index < sizeof(expack_lzss_profiles) / sizeof(expack_lzss_profiles[0]); ++profile_index) {
        if (!try_all_candidates && !expack_lzss_profile_in_normal_portfolio(format, &expack_lzss_profiles[profile_index])) {
            continue;
        }
        if (expack_add_candidate_job(jobs, &job_count, EXPACK_CODEC_LZSS, &expack_lzss_profiles[profile_index], EXPACK_LZREP_PARSE_FAST, input_data, input_size) != 0) {
            return -1;
        }
    }

    if (try_all_candidates && format->kind == EXPACK_FORMAT_MACHO) {
        if (expack_add_candidate_job(jobs, &job_count, EXPACK_CODEC_LZ4, 0, EXPACK_LZREP_PARSE_FAST, input_data, input_size) != 0) {
            return -1;
        }
    }

    if (try_all_candidates && format->kind == EXPACK_FORMAT_ELF64_X86_64) {
        if (expack_add_candidate_job(jobs, &job_count, EXPACK_CODEC_XLZ, 0, EXPACK_LZREP_PARSE_FAST, input_data, input_size) != 0) {
            return -1;
        }
    }

    if (allow_x86_bcj) {
        bcj_transformed = expack_make_bcj_transformed_input(input_data, input_size, 0);
    }

    if (try_all_candidates && allow_x86_bcj && format->kind == EXPACK_FORMAT_ELF64_X86_64) {
        const unsigned char *bcj_input_data = bcj_transformed != 0 ? bcj_transformed : input_data;
        if (expack_add_prepared_candidate_job(jobs, &job_count, EXPACK_CODEC_XLZ_BCJ, 0, EXPACK_LZREP_PARSE_FAST, bcj_input_data, input_size, bcj_transformed != 0) != 0) {
            if (bcj_transformed != 0) rt_free(bcj_transformed);
            return -1;
        }
    }

    if (allow_x86_bcj && format->kind == EXPACK_FORMAT_ELF64_X86_64 && (try_all_candidates || input_size >= 16384U)) {
        const unsigned char *bcj_input_data = bcj_transformed != 0 ? bcj_transformed : input_data;
        if (expack_add_prepared_candidate_job(jobs, &job_count, EXPACK_CODEC_DEFLATE_BCJ, 0, EXPACK_LZREP_PARSE_FAST, bcj_input_data, input_size, bcj_transformed != 0) != 0) {
            if (bcj_transformed != 0) rt_free(bcj_transformed);
            return -1;
        }
    }

    if (expack_add_candidate_job(jobs, &job_count, EXPACK_CODEC_LZREP, 0, EXPACK_LZREP_PARSE_FAST, input_data, input_size) != 0) {
        if (bcj_transformed != 0) rt_free(bcj_transformed);
        return -1;
    }

    if (try_all_candidates) {
        if (expack_add_candidate_job(jobs, &job_count, EXPACK_CODEC_LZREP, 0, EXPACK_LZREP_PARSE_OPT, input_data, input_size) != 0) {
            if (bcj_transformed != 0) rt_free(bcj_transformed);
            return -1;
        }
    }

    if (allow_x86_bcj) {
        const unsigned char *bcj_input_data = bcj_transformed != 0 ? bcj_transformed : input_data;

        for (profile_index = 0U; profile_index < sizeof(expack_lzss_profiles) / sizeof(expack_lzss_profiles[0]); ++profile_index) {
            if (!try_all_candidates && !expack_bcj_profile_in_normal_portfolio(format, &expack_lzss_profiles[profile_index])) {
                continue;
            }
            if (expack_add_prepared_candidate_job(jobs, &job_count, EXPACK_CODEC_LZSS_BCJ, &expack_lzss_profiles[profile_index], EXPACK_LZREP_PARSE_FAST, bcj_input_data, input_size, bcj_transformed != 0) != 0) {
                if (bcj_transformed != 0) rt_free(bcj_transformed);
                return -1;
            }
        }
    }

    if (allow_x86_bcj && format->kind == EXPACK_FORMAT_ELF64_X86_64) {
        bcj_rip_transformed = expack_make_bcj_transformed_input(input_data, input_size, 1);
        for (profile_index = 0U; profile_index < sizeof(expack_lzss_profiles) / sizeof(expack_lzss_profiles[0]); ++profile_index) {
            const unsigned char *bcj_rip_input_data = bcj_rip_transformed != 0 ? bcj_rip_transformed : input_data;

            if (!try_all_candidates && !expack_bcj_rip_profile_in_normal_portfolio(&expack_lzss_profiles[profile_index])) {
                continue;
            }
            if (expack_add_prepared_candidate_job(jobs, &job_count, EXPACK_CODEC_LZSS_BCJ_RIP, &expack_lzss_profiles[profile_index], EXPACK_LZREP_PARSE_FAST, bcj_rip_input_data, input_size, bcj_rip_transformed != 0) != 0) {
                if (bcj_rip_transformed != 0) rt_free(bcj_rip_transformed);
                if (bcj_transformed != 0) rt_free(bcj_transformed);
                return -1;
            }
        }
    }

    expack_run_candidate_jobs(jobs, job_count);
    for (job_index = 0U; job_index < job_count; ++job_index) {
        ExpackCandidateJob *job = jobs + job_index;

        if (!job->succeeded) {
            expack_candidate_release(&job->candidate);
            continue;
        }
        job->candidate.packed_size = expack_score_candidate(format, backend, &job->candidate);
        if (report_candidates) expack_report_candidate(&job->candidate);
        if (report_timings) expack_report_candidate_timing(job);
        if (job->candidate.packed_size != EXPACK_CANDIDATE_UNSUPPORTED_SIZE) expack_consider_candidate(selected_out, &have_selected, &job->candidate);
        else expack_candidate_release(&job->candidate);
    }

    if (bcj_rip_transformed != 0) rt_free(bcj_rip_transformed);
    if (bcj_transformed != 0) rt_free(bcj_transformed);
    return have_selected ? 0 : -1;
}

static void expack_write_lzss_profile_name(unsigned int profile_id) {
    if (profile_id == COMPRESSION_LZSS_PROFILE_WIDE_WINDOW) {
        rt_write_cstr(1, "wide-window");
    } else if (profile_id == COMPRESSION_LZSS_PROFILE_WIDE_MATCH) {
        rt_write_cstr(1, "wide-match");
    } else if (profile_id == COMPRESSION_LZSS_PROFILE_MEDIUM_MATCH) {
        rt_write_cstr(1, "medium-match");
    } else if (profile_id == COMPRESSION_LZSS_PROFILE_LONG_MATCH) {
        rt_write_cstr(1, "long-match");
    } else {
        rt_write_cstr(1, "unknown");
    }
}

static void expack_write_candidate_name(const ExpackCandidate *candidate) {
    if (candidate->codec == EXPACK_CODEC_LZSS) {
        rt_write_cstr(1, "lzss/");
        expack_write_lzss_profile_name(candidate->lzss_profile->profile_id);
    } else if (candidate->codec == EXPACK_CODEC_LZREP) {
        if (candidate->lzrep_parse == EXPACK_LZREP_PARSE_OPT) {
            rt_write_cstr(1, "lzrep-opt");
        } else {
            rt_write_cstr(1, "lzrep");
        }
    } else if (candidate->codec == EXPACK_CODEC_LZ4) {
        rt_write_cstr(1, "lz4-block");
    } else if (candidate->codec == EXPACK_CODEC_XLZ) {
        rt_write_cstr(1, "xlz-short");
    } else if (candidate->codec == EXPACK_CODEC_XLZ_BCJ) {
        rt_write_cstr(1, "xlz-bcj");
    } else if (candidate->codec == EXPACK_CODEC_DEFLATE_BCJ) {
        rt_write_cstr(1, "deflate-bcj");
    } else if (candidate->codec == EXPACK_CODEC_LZSS_BCJ) {
        rt_write_cstr(1, "lzss-bcj/");
        expack_write_lzss_profile_name(candidate->lzss_profile->profile_id);
    } else if (candidate->codec == EXPACK_CODEC_LZSS_BCJ_RIP) {
        rt_write_cstr(1, "lzss-bcj-rip/");
        expack_write_lzss_profile_name(candidate->lzss_profile->profile_id);
    } else if (candidate->codec == EXPACK_CODEC_RAW) {
        rt_write_cstr(1, "raw");
    } else {
        rt_write_cstr(1, "unknown");
    }
}
