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

static const unsigned char expack_zero_stub_x86_64[] = {
    0x31, 0xed, 0x49, 0x89, 0xe4, 0x4c, 0x8b, 0x35, 0x05, 0x01, 0x00, 0x00,
    0x31, 0xff, 0x4c, 0x89, 0xf6, 0xba, 0x03, 0x00, 0x00, 0x00, 0x41, 0xba,
    0x22, 0x00, 0x00, 0x00, 0x41, 0xb8, 0xff, 0xff, 0xff, 0xff, 0x45, 0x31,
    0xc9, 0xb8, 0x09, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x0f,
    0x88, 0xd0, 0x00, 0x00, 0x00, 0x49, 0x89, 0xc5, 0x4f, 0x8d, 0x7c, 0x35,
    0x00, 0x48, 0x8d, 0x1d, 0xe5, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x2d, 0xce,
    0x00, 0x00, 0x00, 0x48, 0x01, 0xdd, 0x4c, 0x89, 0xef, 0x48, 0x39, 0xeb,
    0x74, 0x4e, 0x48, 0x8d, 0x43, 0x05, 0x48, 0x39, 0xe8, 0x0f, 0x87, 0xa2,
    0x00, 0x00, 0x00, 0x0f, 0xb6, 0x03, 0x8b, 0x53, 0x01, 0x48, 0x83, 0xc3,
    0x05, 0x4c, 0x89, 0xf9, 0x48, 0x29, 0xf9, 0x48, 0x39, 0xd1, 0x0f, 0x82,
    0x89, 0x00, 0x00, 0x00, 0x85, 0xc0, 0x75, 0x17, 0x48, 0x89, 0xe9, 0x48,
    0x29, 0xd9, 0x48, 0x39, 0xd1, 0x72, 0x7a, 0x48, 0x89, 0xde, 0x89, 0xd1,
    0xf3, 0xa4, 0x48, 0x89, 0xf3, 0xeb, 0xba, 0x83, 0xf8, 0x01, 0x75, 0x69,
    0x31, 0xc0, 0x89, 0xd1, 0xf3, 0xaa, 0xeb, 0xad, 0x4c, 0x39, 0xff, 0x75,
    0x5c, 0xb8, 0x3f, 0x01, 0x00, 0x00, 0x48, 0x8d, 0x3d, 0x6c, 0x00, 0x00,
    0x00, 0x31, 0xf6, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x78, 0x47, 0x89, 0xc3,
    0x4c, 0x89, 0xee, 0x4c, 0x89, 0xf2, 0x48, 0x85, 0xd2, 0x74, 0x16, 0xb8,
    0x01, 0x00, 0x00, 0x00, 0x89, 0xdf, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x7e,
    0x2c, 0x48, 0x01, 0xc6, 0x48, 0x29, 0xc2, 0xeb, 0xe5, 0xb8, 0x42, 0x01,
    0x00, 0x00, 0x89, 0xdf, 0x48, 0x8d, 0x35, 0x39, 0x00, 0x00, 0x00, 0x49,
    0x8d, 0x54, 0x24, 0x08, 0x4d, 0x8b, 0x14, 0x24, 0x4f, 0x8d, 0x54, 0xd4,
    0x10, 0x41, 0xb8, 0x00, 0x10, 0x00, 0x00, 0x0f, 0x05, 0xb8, 0x3c, 0x00,
    0x00, 0x00, 0xbf, 0x7f, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x11, 0x22, 0x33,
    0x44, 0x55, 0x66, 0x77, 0x88, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22,
    0x11, 0x65, 0x78, 0x70, 0x61, 0x63, 0x6b, 0x00, 0x00
};

static const unsigned char expack_byte_run_stub_x86_64[] = {
    0x31, 0xed, 0x49, 0x89, 0xe4, 0x4c, 0x8b, 0x35, 0x11, 0x01, 0x00, 0x00,
    0x31, 0xff, 0x4c, 0x89, 0xf6, 0xba, 0x03, 0x00, 0x00, 0x00, 0x41, 0xba,
    0x22, 0x00, 0x00, 0x00, 0x41, 0xb8, 0xff, 0xff, 0xff, 0xff, 0x45, 0x31,
    0xc9, 0xb8, 0x09, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x0f,
    0x88, 0xdc, 0x00, 0x00, 0x00, 0x49, 0x89, 0xc5, 0x4f, 0x8d, 0x7c, 0x35,
    0x00, 0x48, 0x8d, 0x1d, 0xf1, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x2d, 0xda,
    0x00, 0x00, 0x00, 0x48, 0x01, 0xdd, 0x4c, 0x89, 0xef, 0x48, 0x39, 0xeb,
    0x74, 0x5a, 0x48, 0x8d, 0x43, 0x05, 0x48, 0x39, 0xe8, 0x0f, 0x87, 0xae,
    0x00, 0x00, 0x00, 0x0f, 0xb6, 0x03, 0x8b, 0x53, 0x01, 0x48, 0x83, 0xc3,
    0x05, 0x4c, 0x89, 0xf9, 0x48, 0x29, 0xf9, 0x48, 0x39, 0xd1, 0x0f, 0x82,
    0x95, 0x00, 0x00, 0x00, 0x85, 0xc0, 0x75, 0x1b, 0x48, 0x89, 0xe9, 0x48,
    0x29, 0xd9, 0x48, 0x39, 0xd1, 0x0f, 0x82, 0x82, 0x00, 0x00, 0x00, 0x48,
    0x89, 0xde, 0x89, 0xd1, 0xf3, 0xa4, 0x48, 0x89, 0xf3, 0xeb, 0xb6, 0x83,
    0xf8, 0x01, 0x75, 0x71, 0x48, 0x39, 0xeb, 0x73, 0x6c, 0x8a, 0x03, 0x48,
    0xff, 0xc3, 0x89, 0xd1, 0xf3, 0xaa, 0xeb, 0xa1, 0x4c, 0x39, 0xff, 0x75,
    0x5c, 0xb8, 0x3f, 0x01, 0x00, 0x00, 0x48, 0x8d, 0x3d, 0x6c, 0x00, 0x00,
    0x00, 0x31, 0xf6, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x78, 0x47, 0x89, 0xc3,
    0x4c, 0x89, 0xee, 0x4c, 0x89, 0xf2, 0x48, 0x85, 0xd2, 0x74, 0x16, 0xb8,
    0x01, 0x00, 0x00, 0x00, 0x89, 0xdf, 0x0f, 0x05, 0x48, 0x85, 0xc0, 0x7e,
    0x2c, 0x48, 0x01, 0xc6, 0x48, 0x29, 0xc2, 0xeb, 0xe5, 0xb8, 0x42, 0x01,
    0x00, 0x00, 0x89, 0xdf, 0x48, 0x8d, 0x35, 0x39, 0x00, 0x00, 0x00, 0x49,
    0x8d, 0x54, 0x24, 0x08, 0x4d, 0x8b, 0x14, 0x24, 0x4f, 0x8d, 0x54, 0xd4,
    0x10, 0x41, 0xb8, 0x00, 0x10, 0x00, 0x00, 0x0f, 0x05, 0xb8, 0x3c, 0x00,
    0x00, 0x00, 0xbf, 0x7f, 0x00, 0x00, 0x00, 0x0f, 0x05, 0x11, 0x22, 0x33,
    0x44, 0x55, 0x66, 0x77, 0x88, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22,
    0x11, 0x65, 0x78, 0x70, 0x61, 0x63, 0x6b, 0x00, 0x00
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


static unsigned long long expack_zero_run_length(const unsigned char *data, size_t size, size_t offset) {
    size_t position = offset;

    while (position < size && data[position] == 0U) {
        position += 1U;
    }
    return (unsigned long long)(position - offset);
}

static unsigned long long expack_byte_run_length(const unsigned char *data, size_t size, size_t offset) {
    size_t position = offset;
    unsigned char value;

    if (offset >= size) {
        return 0ULL;
    }
    value = data[offset];
    while (position < size && data[position] == value) {
        position += 1U;
    }
    return (unsigned long long)(position - offset);
}

static int expack_should_start_zero_record(const unsigned char *data, size_t size, size_t offset) {
    return expack_zero_run_length(data, size, offset) >= EXPACK_ZERO_RUN_MIN;
}

static int expack_should_start_byte_run_record(const unsigned char *data, size_t size, size_t offset) {
    return expack_byte_run_length(data, size, offset) >= EXPACK_BYTE_RUN_MIN;
}

static unsigned long long expack_zero_encoded_size(const unsigned char *data, size_t size) {
    size_t position = 0U;
    unsigned long long encoded_size = 0ULL;

    while (position < size) {
        unsigned long long zero_count = expack_zero_run_length(data, size, position);
        if (zero_count >= EXPACK_ZERO_RUN_MIN) {
            while (zero_count > 0ULL) {
                unsigned int chunk = zero_count > 0xffffffffULL ? 0xffffffffU : (unsigned int)zero_count;
                encoded_size += 5ULL;
                position += (size_t)chunk;
                zero_count -= (unsigned long long)chunk;
            }
        } else {
            size_t literal_start = position;

            position += 1U;
            while (position < size && position - literal_start < EXPACK_LITERAL_MAX && !expack_should_start_zero_record(data, size, position)) {
                position += 1U;
            }
            encoded_size += 5ULL + (unsigned long long)(position - literal_start);
        }
    }
    return encoded_size;
}

static int expack_zero_store_record(unsigned char *payload, size_t payload_capacity, size_t *payload_offset, unsigned char type, unsigned int length) {
    if (*payload_offset + 5U > payload_capacity) {
        return -1;
    }
    payload[*payload_offset] = type;
    archive_store_u32_le(payload + *payload_offset + 1U, length);
    *payload_offset += 5U;
    return 0;
}

static int expack_compress_zero_run(const unsigned char *data, size_t size, unsigned char **payload_out, size_t *payload_size_out) {
    unsigned long long encoded_bound = expack_zero_encoded_size(data, size);
    unsigned char *payload;
    size_t position = 0U;
    size_t payload_offset = 0U;

    if (encoded_bound > (unsigned long long)((size_t)-1)) {
        return -1;
    }
    payload = (unsigned char *)rt_malloc((size_t)encoded_bound == 0U ? 1U : (size_t)encoded_bound);
    if (payload == 0) {
        return -1;
    }

    while (position < size) {
        unsigned long long zero_count = expack_zero_run_length(data, size, position);
        if (zero_count >= EXPACK_ZERO_RUN_MIN) {
            while (zero_count > 0ULL) {
                unsigned int chunk = zero_count > 0xffffffffULL ? 0xffffffffU : (unsigned int)zero_count;
                if (expack_zero_store_record(payload, (size_t)encoded_bound, &payload_offset, 1U, chunk) != 0) {
                    rt_free(payload);
                    return -1;
                }
                position += (size_t)chunk;
                zero_count -= (unsigned long long)chunk;
            }
        } else {
            size_t literal_start = position;

            position += 1U;
            while (position < size && position - literal_start < EXPACK_LITERAL_MAX && !expack_should_start_zero_record(data, size, position)) {
                position += 1U;
            }
            if (expack_zero_store_record(payload, (size_t)encoded_bound, &payload_offset, 0U, (unsigned int)(position - literal_start)) != 0 ||
                payload_offset + (position - literal_start) > (size_t)encoded_bound) {
                rt_free(payload);
                return -1;
            }
            memcpy(payload + payload_offset, data + literal_start, position - literal_start);
            payload_offset += position - literal_start;
        }
    }

    *payload_out = payload;
    *payload_size_out = payload_offset;
    return 0;
}

static unsigned long long expack_byte_run_encoded_size(const unsigned char *data, size_t size) {
    size_t position = 0U;
    unsigned long long encoded_size = 0ULL;

    while (position < size) {
        unsigned long long run_count = expack_byte_run_length(data, size, position);
        if (run_count >= EXPACK_BYTE_RUN_MIN) {
            while (run_count > 0ULL) {
                unsigned int chunk = run_count > 0xffffffffULL ? 0xffffffffU : (unsigned int)run_count;
                encoded_size += 6ULL;
                position += (size_t)chunk;
                run_count -= (unsigned long long)chunk;
            }
        } else {
            size_t literal_start = position;

            position += 1U;
            while (position < size && position - literal_start < EXPACK_LITERAL_MAX && !expack_should_start_byte_run_record(data, size, position)) {
                position += 1U;
            }
            encoded_size += 5ULL + (unsigned long long)(position - literal_start);
        }
    }
    return encoded_size;
}

static int expack_compress_byte_run(const unsigned char *data, size_t size, unsigned char **payload_out, size_t *payload_size_out) {
    unsigned long long encoded_bound = expack_byte_run_encoded_size(data, size);
    unsigned char *payload;
    size_t position = 0U;
    size_t payload_offset = 0U;

    if (encoded_bound > (unsigned long long)((size_t)-1)) {
        return -1;
    }
    payload = (unsigned char *)rt_malloc((size_t)encoded_bound == 0U ? 1U : (size_t)encoded_bound);
    if (payload == 0) {
        return -1;
    }

    while (position < size) {
        unsigned long long run_count = expack_byte_run_length(data, size, position);
        if (run_count >= EXPACK_BYTE_RUN_MIN) {
            unsigned char value = data[position];
            while (run_count > 0ULL) {
                unsigned int chunk = run_count > 0xffffffffULL ? 0xffffffffU : (unsigned int)run_count;
                if (expack_zero_store_record(payload, (size_t)encoded_bound, &payload_offset, 1U, chunk) != 0 || payload_offset >= (size_t)encoded_bound) {
                    rt_free(payload);
                    return -1;
                }
                payload[payload_offset++] = value;
                position += (size_t)chunk;
                run_count -= (unsigned long long)chunk;
            }
        } else {
            size_t literal_start = position;

            position += 1U;
            while (position < size && position - literal_start < EXPACK_LITERAL_MAX && !expack_should_start_byte_run_record(data, size, position)) {
                position += 1U;
            }
            if (expack_zero_store_record(payload, (size_t)encoded_bound, &payload_offset, 0U, (unsigned int)(position - literal_start)) != 0 ||
                payload_offset + (position - literal_start) > (size_t)encoded_bound) {
                rt_free(payload);
                return -1;
            }
            memcpy(payload + payload_offset, data + literal_start, position - literal_start);
            payload_offset += position - literal_start;
        }
    }

    *payload_out = payload;
    *payload_size_out = payload_offset;
    return 0;
}

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

static void expack_find_lzrep_matches(const unsigned char *data, size_t size, size_t position, ExpackLzrepMatch *matches, unsigned int match_capacity) {
    size_t window_start = position > EXPACK_LZREP_WINDOW_SIZE ? position - EXPACK_LZREP_WINDOW_SIZE : 0U;
    size_t candidate;
    unsigned int index;

    for (index = 0U; index < match_capacity; ++index) {
        matches[index].length = 0U;
        matches[index].distance = 0U;
    }
    for (candidate = window_start; candidate < position; ++candidate) {
        size_t distance = position - candidate;
        size_t length = expack_match_length_at(data, size, position, distance, EXPACK_LZREP_MAX_EXPLICIT_MATCH);
        if (length >= COMPRESSION_LZSS_MIN_MATCH) {
            for (index = 0U; index < match_capacity; ++index) {
                if (length > matches[index].length || (length == matches[index].length && (unsigned int)distance < matches[index].distance)) {
                    unsigned int shift;

                    for (shift = match_capacity - 1U; shift > index; --shift) {
                        matches[shift] = matches[shift - 1U];
                    }
                    matches[index].length = length;
                    matches[index].distance = (unsigned int)distance;
                    break;
                }
            }
        }
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

static void expack_choose_lzrep_token(const unsigned char *data, size_t size, size_t position, const ExpackLzrepParseParams *params, unsigned int last_distance, unsigned int bit_index, ExpackLzrepToken *token_out) {
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

        expack_find_lzrep_matches(data, size, absolute_position, matches, params->explicit_choices);
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

static unsigned int expack_lz4_read_u32(const unsigned char *data) {
    return (unsigned int)data[0] | ((unsigned int)data[1] << 8U) | ((unsigned int)data[2] << 16U) | ((unsigned int)data[3] << 24U);
}

static unsigned int expack_lz4_hash4(const unsigned char *data) {
    return (expack_lz4_read_u32(data) * 2654435761U) >> (32U - EXPACK_LZ4_HASH_BITS);
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

    if (anchor < size || output_offset == 0U) {
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

static int expack_compress_lzrep_parse(const unsigned char *data, size_t size, const ExpackLzrepParseParams *params, unsigned char **payload_out, size_t *payload_size_out) {
    size_t bound = compression_lzss_bound(size);
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

            expack_choose_lzrep_token(data, size, input_offset, params, last_distance, bit, &token);
            if (token.kind == EXPACK_LZREP_TOKEN_REPEAT) {
                if (output_offset >= bound) {
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
                    rt_free(payload);
                    return -1;
                }
                payload[output_offset++] = data[input_offset++];
            }
        }
        payload[flag_offset] = flags;
    }

    if (expack_verify_lzrep_payload(payload, output_offset, data, size) != 0) {
        rt_free(payload);
        return -1;
    }
    *payload_out = payload;
    *payload_size_out = output_offset;
    return 0;
}

static int expack_compress_lzrep(const unsigned char *data, size_t size, unsigned char **payload_out, size_t *payload_size_out) {
    return expack_compress_lzrep_parse(data, size, &expack_lzrep_fast_parse, payload_out, payload_size_out);
}

static void expack_x86_bcj_transform(unsigned char *data, size_t size, int include_rip_relative) {
    size_t position = 0U;

    while (position + 4U < size) {
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
    if (!*have_selected || candidate->packed_size < selected->packed_size) {
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

static int expack_lzss_profile_in_normal_portfolio(const ExpackLzssProfile *profile) {
    return profile->profile_id == COMPRESSION_LZSS_PROFILE_LONG_MATCH;
}

static int expack_bcj_profile_in_normal_portfolio(const ExpackLzssProfile *profile) {
    return profile->profile_id == COMPRESSION_LZSS_PROFILE_WIDE_WINDOW ||
           profile->profile_id == COMPRESSION_LZSS_PROFILE_WIDE_MATCH ||
           profile->profile_id == COMPRESSION_LZSS_PROFILE_MEDIUM_MATCH;
}

static int expack_bcj_rip_profile_in_normal_portfolio(const ExpackLzssProfile *profile) {
    return profile->profile_id == COMPRESSION_LZSS_PROFILE_WIDE_WINDOW ||
           profile->profile_id == COMPRESSION_LZSS_PROFILE_WIDE_MATCH;
}

static int expack_select_best_payload(const ExpackInputFormat *format, const ExpackOutputBackend *backend, const unsigned char *input_data, size_t input_size, ExpackCandidate *selected_out, int report_candidates, int allow_x86_bcj, int try_all_candidates) {
    unsigned int profile_index;
    int have_selected = 0;

    expack_candidate_release(selected_out);
    for (profile_index = 0U; profile_index < sizeof(expack_lzss_profiles) / sizeof(expack_lzss_profiles[0]); ++profile_index) {
        ExpackCandidate candidate;

        if (!try_all_candidates && !expack_lzss_profile_in_normal_portfolio(&expack_lzss_profiles[profile_index])) {
            continue;
        }
        candidate.codec = EXPACK_CODEC_LZSS;
        candidate.lzss_profile = &expack_lzss_profiles[profile_index];
        candidate.lzrep_parse = EXPACK_LZREP_PARSE_FAST;
        candidate.stub = expack_stub_x86_64;
        candidate.stub_size = sizeof(expack_stub_x86_64);
        candidate.payload = 0;
        candidate.payload_size = 0U;
        candidate.packed_size = 0ULL;

        if (expack_compress_lzss_profile(candidate.lzss_profile, input_data, input_size, &candidate.payload, &candidate.payload_size) != 0) {
            continue;
        }
        candidate.packed_size = expack_score_candidate(format, backend, &candidate);
        if (report_candidates) expack_report_candidate(&candidate);
        if (candidate.packed_size != EXPACK_CANDIDATE_UNSUPPORTED_SIZE) expack_consider_candidate(selected_out, &have_selected, &candidate);
        else expack_candidate_release(&candidate);
    }

    if (try_all_candidates) {
        ExpackCandidate candidate;

        candidate.codec = EXPACK_CODEC_ZERO_RUN;
        candidate.lzss_profile = 0;
        candidate.lzrep_parse = EXPACK_LZREP_PARSE_FAST;
        candidate.stub = expack_zero_stub_x86_64;
        candidate.stub_size = sizeof(expack_zero_stub_x86_64);
        candidate.payload = 0;
        candidate.payload_size = 0U;
        candidate.packed_size = 0ULL;
        if (expack_compress_zero_run(input_data, input_size, &candidate.payload, &candidate.payload_size) == 0) {
            candidate.packed_size = expack_score_candidate(format, backend, &candidate);
            if (report_candidates) expack_report_candidate(&candidate);
            if (candidate.packed_size != EXPACK_CANDIDATE_UNSUPPORTED_SIZE) expack_consider_candidate(selected_out, &have_selected, &candidate);
            else expack_candidate_release(&candidate);
        }
    }

    if (try_all_candidates) {
        ExpackCandidate candidate;

        candidate.codec = EXPACK_CODEC_BYTE_RUN;
        candidate.lzss_profile = 0;
        candidate.lzrep_parse = EXPACK_LZREP_PARSE_FAST;
        candidate.stub = expack_byte_run_stub_x86_64;
        candidate.stub_size = sizeof(expack_byte_run_stub_x86_64);
        candidate.payload = 0;
        candidate.payload_size = 0U;
        candidate.packed_size = 0ULL;
        if (expack_compress_byte_run(input_data, input_size, &candidate.payload, &candidate.payload_size) == 0) {
            candidate.packed_size = expack_score_candidate(format, backend, &candidate);
            if (report_candidates) expack_report_candidate(&candidate);
            if (candidate.packed_size != EXPACK_CANDIDATE_UNSUPPORTED_SIZE) expack_consider_candidate(selected_out, &have_selected, &candidate);
            else expack_candidate_release(&candidate);
        }
    }

    if (try_all_candidates) {
        ExpackCandidate candidate;

        candidate.codec = EXPACK_CODEC_LZ4;
        candidate.lzss_profile = 0;
        candidate.lzrep_parse = EXPACK_LZREP_PARSE_FAST;
        candidate.stub = expack_lz4_stub_x86_64;
        candidate.stub_size = sizeof(expack_lz4_stub_x86_64);
        candidate.payload = 0;
        candidate.payload_size = 0U;
        candidate.packed_size = 0ULL;
        if (expack_compress_lz4_block(input_data, input_size, &candidate.payload, &candidate.payload_size) == 0) {
            candidate.packed_size = expack_score_candidate(format, backend, &candidate);
            if (report_candidates) expack_report_candidate(&candidate);
            if (candidate.packed_size != EXPACK_CANDIDATE_UNSUPPORTED_SIZE) expack_consider_candidate(selected_out, &have_selected, &candidate);
            else expack_candidate_release(&candidate);
        }
    }

    {
        ExpackCandidate candidate;

        candidate.codec = EXPACK_CODEC_LZREP;
        candidate.lzss_profile = 0;
        candidate.lzrep_parse = EXPACK_LZREP_PARSE_FAST;
        candidate.stub = expack_lzrep_stub_x86_64;
        candidate.stub_size = sizeof(expack_lzrep_stub_x86_64);
        candidate.payload = 0;
        candidate.payload_size = 0U;
        candidate.packed_size = 0ULL;
        if (expack_compress_lzrep(input_data, input_size, &candidate.payload, &candidate.payload_size) == 0) {
            candidate.packed_size = expack_score_candidate(format, backend, &candidate);
            if (report_candidates) expack_report_candidate(&candidate);
            if (candidate.packed_size != EXPACK_CANDIDATE_UNSUPPORTED_SIZE) expack_consider_candidate(selected_out, &have_selected, &candidate);
            else expack_candidate_release(&candidate);
        }
    }

    {
        ExpackCandidate candidate;

        candidate.codec = EXPACK_CODEC_LZREP;
        candidate.lzss_profile = 0;
        candidate.lzrep_parse = EXPACK_LZREP_PARSE_OPT;
        candidate.stub = expack_lzrep_stub_x86_64;
        candidate.stub_size = sizeof(expack_lzrep_stub_x86_64);
        candidate.payload = 0;
        candidate.payload_size = 0U;
        candidate.packed_size = 0ULL;
        if (expack_compress_lzrep_parse(input_data, input_size, &expack_lzrep_opt_parse, &candidate.payload, &candidate.payload_size) == 0) {
            candidate.packed_size = expack_score_candidate(format, backend, &candidate);
            if (report_candidates) expack_report_candidate(&candidate);
            if (candidate.packed_size != EXPACK_CANDIDATE_UNSUPPORTED_SIZE) expack_consider_candidate(selected_out, &have_selected, &candidate);
            else expack_candidate_release(&candidate);
        }
    }

    if (allow_x86_bcj) {
        for (profile_index = 0U; profile_index < sizeof(expack_lzss_profiles) / sizeof(expack_lzss_profiles[0]); ++profile_index) {
            ExpackCandidate candidate;

            if (!try_all_candidates && !expack_bcj_profile_in_normal_portfolio(&expack_lzss_profiles[profile_index])) {
                continue;
            }
            candidate.codec = EXPACK_CODEC_LZSS_BCJ;
            candidate.lzss_profile = &expack_lzss_profiles[profile_index];
            candidate.lzrep_parse = EXPACK_LZREP_PARSE_FAST;
            candidate.stub = expack_lzss_bcj_stub_x86_64;
            candidate.stub_size = sizeof(expack_lzss_bcj_stub_x86_64);
            candidate.payload = 0;
            candidate.payload_size = 0U;
            candidate.packed_size = 0ULL;
            if (expack_compress_lzss_bcj_profile(candidate.lzss_profile, input_data, input_size, &candidate.payload, &candidate.payload_size) == 0) {
                candidate.packed_size = expack_score_candidate(format, backend, &candidate);
                if (report_candidates) expack_report_candidate(&candidate);
                if (candidate.packed_size != EXPACK_CANDIDATE_UNSUPPORTED_SIZE) expack_consider_candidate(selected_out, &have_selected, &candidate);
                else expack_candidate_release(&candidate);
            }
        }
    }

    if (allow_x86_bcj && format->kind == EXPACK_FORMAT_ELF64_X86_64) {
        for (profile_index = 0U; profile_index < sizeof(expack_lzss_profiles) / sizeof(expack_lzss_profiles[0]); ++profile_index) {
            ExpackCandidate candidate;

            if (!try_all_candidates && !expack_bcj_rip_profile_in_normal_portfolio(&expack_lzss_profiles[profile_index])) {
                continue;
            }
            candidate.codec = EXPACK_CODEC_LZSS_BCJ_RIP;
            candidate.lzss_profile = &expack_lzss_profiles[profile_index];
            candidate.lzrep_parse = EXPACK_LZREP_PARSE_FAST;
            candidate.stub = expack_lzss_bcj_rip_stub_x86_64;
            candidate.stub_size = sizeof(expack_lzss_bcj_rip_stub_x86_64);
            candidate.payload = 0;
            candidate.payload_size = 0U;
            candidate.packed_size = 0ULL;
            if (expack_compress_lzss_bcj_rip_profile(candidate.lzss_profile, input_data, input_size, &candidate.payload, &candidate.payload_size) == 0) {
                candidate.packed_size = expack_score_candidate(format, backend, &candidate);
                if (report_candidates) expack_report_candidate(&candidate);
                if (candidate.packed_size != EXPACK_CANDIDATE_UNSUPPORTED_SIZE) expack_consider_candidate(selected_out, &have_selected, &candidate);
                else expack_candidate_release(&candidate);
            }
        }
    }

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
    } else if (candidate->codec == EXPACK_CODEC_ZERO_RUN) {
        rt_write_cstr(1, "zero-run");
    } else if (candidate->codec == EXPACK_CODEC_BYTE_RUN) {
        rt_write_cstr(1, "byte-run");
    } else if (candidate->codec == EXPACK_CODEC_LZREP) {
        if (candidate->lzrep_parse == EXPACK_LZREP_PARSE_OPT) {
            rt_write_cstr(1, "lzrep-opt");
        } else {
            rt_write_cstr(1, "lzrep");
        }
    } else if (candidate->codec == EXPACK_CODEC_LZ4) {
        rt_write_cstr(1, "lz4-block");
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
