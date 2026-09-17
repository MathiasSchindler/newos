#ifndef OCR_TOKENIZER_H
#define OCR_TOKENIZER_H

#define OCR_TOKENIZER_LIMIT 8192U
typedef struct {
    const unsigned char *pieces, *lexical, *merges, *ranges, *bytes, *strings;
    unsigned int range_count;
} OcrTokenizer;

int ocr_artifact(const unsigned char *data, unsigned int size, unsigned int kind);
int ocr_tokenizer_open(OcrTokenizer *tokenizer, const unsigned char *data, unsigned int size);
int ocr_encode(const OcrTokenizer *tokenizer, const unsigned char *text, unsigned int size,
               unsigned int *ids, unsigned int capacity);
int ocr_decode(const OcrTokenizer *tokenizer, const unsigned int *ids, unsigned int count,
               int skip_special, unsigned char *output, unsigned int capacity);
int ocr_decode_prefix(const OcrTokenizer *tokenizer, const unsigned int *ids, unsigned int count,
                      int skip_special, unsigned char *output, unsigned int capacity, int final);
int ocr_prompt(unsigned int task, unsigned int images, unsigned int no_think,
               unsigned int *ids, unsigned int capacity);
int ocr_tokenizer_test(const unsigned short *table_path, const unsigned short *fixture_path);
#endif