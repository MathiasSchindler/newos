#include "ocr_text.h"
#include "ocr_image.h"
#include "ocr_tokenizer.h"

int ocr_text_prepare(const unsigned short *features, unsigned int image_tokens,
                      const unsigned short *embeddings, unsigned long long embedding_values,
                      unsigned int grid_height, unsigned int grid_width,
                      unsigned int task, unsigned int no_think, OcrTextInput *output) {
    if (!output) return 0;
    output->count = output->image_tokens = 0; output->delta = 0;
    if (!features || !embeddings || embedding_values != (unsigned long long)OCR_TEXT_VOCAB*OCR_TEXT_WIDTH ||
        grid_height != 8 || (grid_width != 8 && grid_width != 16) || image_tokens != grid_height*grid_width/4) return 0;
    int count = ocr_prompt(task,image_tokens,no_think,output->ids,OCR_TEXT_CONTEXT);
    if (count <= 0 || count > (int)OCR_TEXT_CONTEXT) return 0;
    int compact_positions[3*OCR_TEXT_CONTEXT], delta;
    if (!ocr_image_positions(output->ids,(unsigned int)count,grid_height,grid_width,compact_positions,
                              3*OCR_TEXT_CONTEXT,output->modalities,OCR_TEXT_CONTEXT,&delta)) return 0;
    unsigned int image_index = 0;
    for (unsigned int row = 0; row < OCR_TEXT_CONTEXT; ++row) {
        const unsigned short *source = 0;
        if (row < (unsigned int)count) {
            if (output->ids[row] >= OCR_TEXT_VOCAB) return 0;
            source = output->modalities[row] ? features+image_index++*OCR_TEXT_WIDTH : embeddings+output->ids[row]*OCR_TEXT_WIDTH;
        } else {
            output->ids[row] = 59246;
            output->modalities[row] = 0;
        }
        for (unsigned int axis = 0; axis < 3; ++axis)
            output->positions[axis*OCR_TEXT_CONTEXT+row] = row < (unsigned int)count ? compact_positions[axis*(unsigned int)count+row] : 0;
        for (unsigned int channel = 0; channel < OCR_TEXT_WIDTH; ++channel) {
            unsigned short value = source ? source[channel] : 0;
            if ((value & 0x7c00) == 0x7c00) return 0;
            output->embeddings[row*OCR_TEXT_WIDTH+channel] = value;
        }
        for (unsigned int column = 0; column < OCR_TEXT_CONTEXT; ++column)
            output->mask[row*OCR_TEXT_CONTEXT+column] = column <= row && column < (unsigned int)count ? 0 : 0xfbff;
    }
    if (image_index != image_tokens) return 0;
    output->count = (unsigned int)count; output->image_tokens = image_tokens; output->delta = delta;
    return 1;
}