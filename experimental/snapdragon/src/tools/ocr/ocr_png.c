#include "ocr_image.h"
#include "compression/zlib.h"
#include "runtime.h"

__declspec(dllimport) void *VirtualAlloc(void *, unsigned long long, unsigned int, unsigned int);
__declspec(dllimport) int VirtualFree(void *, unsigned long long, unsigned int);

void *rt_malloc(size_t size) {
    return size && size <= 128U*1024U*1024U ? VirtualAlloc(0,size,0x3000,4) : 0;
}

void rt_free(void *pointer) {
    if (pointer) VirtualFree(pointer,0,0x8000);
}

void rt_memset(void *destination, int value, size_t length) {
    unsigned char *output = destination;
    for (size_t index = 0; index < length; ++index) output[index] = (unsigned char)value;
}

static unsigned int png_number(const unsigned char *data) {
    return (unsigned int)data[0]<<24 | (unsigned int)data[1]<<16 | (unsigned int)data[2]<<8 | data[3];
}

static unsigned int png_crc(const unsigned char *data, unsigned long long size) {
    unsigned int value = ~0U;
    for (unsigned long long index = 0; index < size; ++index) {
        value ^= data[index];
        for (unsigned int bit = 0; bit < 8; ++bit) value = (value>>1) ^ (0xedb88320U & (0U-(value&1)));
    }
    return ~value;
}

static unsigned int png_paeth(unsigned int left, unsigned int above, unsigned int diagonal) {
    int estimate = (int)left+(int)above-(int)diagonal;
    int first = estimate-(int)left, second = estimate-(int)above, third = estimate-(int)diagonal;
    if (first < 0) first = -first;
    if (second < 0) second = -second;
    if (third < 0) third = -third;
    return first <= second && first <= third ? left : second <= third ? above : diagonal;
}

int ocr_image_png(const unsigned char *data, unsigned long long size, unsigned char *rgb,
                  unsigned long long capacity, unsigned int *height, unsigned int *width) {
    static const unsigned char signature[8] = {137,80,78,71,13,10,26,10};
    if (!height || !width) return 0;
    *height = *width = 0;
    if (!data || size < 57 || size > 64ULL*1024*1024) return 0;
    for (unsigned int index = 0; index < 8; ++index) if (data[index] != signature[index]) return 0;
    unsigned int columns = 0, rows = 0, channels = 0, color = 0, state = 0;
    unsigned long long cursor = 8, compressed_size = 0;
    int ended = 0, palette = 0;
    while (cursor+12 <= size) {
        unsigned int length = png_number(data+cursor), kind = png_number(data+cursor+4);
        if (length > size-cursor-12 || png_crc(data+cursor+4,(unsigned long long)length+4) != png_number(data+cursor+8+length)) return 0;
        for (unsigned int index = 4; index < 8; ++index) {
            unsigned int character = data[cursor+index];
            if (!((character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z'))) return 0;
        }
        if (data[cursor+6]&32) return 0;
        const unsigned char *payload = data+cursor+8;
        if (cursor == 8) {
            if (kind != 0x49484452 || length != 13 || payload[8] != 8 || payload[10] || payload[11] || payload[12]) return 0;
            columns = png_number(payload); rows = png_number(payload+4); color = payload[9];
            channels = color == 0 ? 1 : color == 2 ? 3 : color == 4 ? 2 : color == 6 ? 4 : 0;
            if (!channels || !columns || !rows || columns > 10000 || rows > 10000 || (unsigned long long)columns*rows > 16ULL*1024*1024) return 0;
        } else if (kind == 0x49444154) {
            if (state == 2) return 0;
            state = 1; compressed_size += length;
        } else if (kind == 0x49454e44) {
            if (length || !compressed_size || cursor+12 != size) return 0;
            ended = 1; break;
        } else {
            if (state == 1) state = 2;
            if (kind == 0x504c5445) {
                if (palette || state || (color != 2 && color != 6) || !length || length > 768 || length%3) return 0;
                palette = 1;
            } else if (!(data[cursor+4]&32) || kind == 0x74524e53 || kind == 0x65584966 || kind == 0x6163544c || kind == 0x69434350) return 0;
        }
        cursor += length+12ULL;
    }
    if (!ended) return 0;
    if (!rgb) { *height = rows; *width = columns; return 1; }
    if (capacity < (unsigned long long)rows*columns*3) return 0;
    unsigned long long stride = (unsigned long long)columns*channels, raw_size = (stride+1)*rows;
    if (compressed_size+raw_size > 128ULL*1024*1024) return 0;
    unsigned char *workspace = VirtualAlloc(0,compressed_size+raw_size,0x3000,4);
    if (!workspace) return 0;
    unsigned long long offset = 0; cursor = 8;
    while (cursor+12 <= size) {
        unsigned int length = png_number(data+cursor);
        if (png_number(data+cursor+4) == 0x49444154) {
            for (unsigned int index = 0; index < length; ++index) workspace[offset++] = data[cursor+8+index];
        }
        cursor += length+12ULL;
    }
    unsigned char *raw = workspace+compressed_size;
    size_t actual = 0, consumed = 0;
    int good = compression_zlib_inflate_consumed(workspace,(size_t)compressed_size,raw,(size_t)raw_size,&actual,&consumed) == 0 && actual == raw_size && consumed == compressed_size;
    for (unsigned int row = 0; good && row < rows; ++row) {
        unsigned char *current = raw+row*(stride+1)+1;
        unsigned char *previous = row ? current-stride-1 : 0;
        unsigned int filter = current[-1];
        if (filter > 4) { good = 0; break; }
        for (unsigned long long index = 0; index < stride; ++index) {
            unsigned int left = index >= channels ? current[index-channels] : 0;
            unsigned int above = previous ? previous[index] : 0;
            unsigned int diagonal = previous && index >= channels ? previous[index-channels] : 0;
            unsigned int predictor = filter == 1 ? left : filter == 2 ? above : filter == 3 ? (left+above)/2 : filter == 4 ? png_paeth(left,above,diagonal) : 0;
            current[index] = (unsigned char)(current[index]+predictor);
        }
        for (unsigned int column = 0; column < columns; ++column) {
            unsigned int alpha = color == 4 || color == 6 ? current[column*channels+channels-1] : 255;
            for (unsigned int channel = 0; channel < 3; ++channel) {
                unsigned int value = current[column*channels+((color == 0 || color == 4) ? 0 : channel)];
                rgb[((unsigned long long)row*columns+column)*3+channel] = (unsigned char)((value*alpha+255*(255-alpha)+127)/255);
            }
        }
    }
    if (!VirtualFree(workspace,0,0x8000)) good = 0;
    if (good) { *height = rows; *width = columns; }
    return good;
}