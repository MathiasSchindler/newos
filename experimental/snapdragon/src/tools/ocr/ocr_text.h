#ifndef OCR_TEXT_H
#define OCR_TEXT_H

#define OCR_TEXT_CONTEXT 64U
#define OCR_TEXT_WIDTH 1536U
#define OCR_TEXT_VOCAB 59392U

typedef struct {
    unsigned int count, image_tokens;
    int delta;
    unsigned int ids[OCR_TEXT_CONTEXT];
    int positions[3*OCR_TEXT_CONTEXT];
    unsigned char modalities[OCR_TEXT_CONTEXT];
    unsigned short embeddings[OCR_TEXT_CONTEXT*OCR_TEXT_WIDTH];
    unsigned short mask[OCR_TEXT_CONTEXT*OCR_TEXT_CONTEXT];
} OcrTextInput;

int ocr_text_prepare(const unsigned short *features, unsigned int image_tokens,
                      const unsigned short *embeddings, unsigned long long embedding_values,
                      unsigned int grid_height, unsigned int grid_width,
                      unsigned int task, unsigned int no_think, OcrTextInput *output);
#endif