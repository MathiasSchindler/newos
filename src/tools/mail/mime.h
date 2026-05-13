#ifndef NEWOS_TOOL_MAIL_MIME_H
#define NEWOS_TOOL_MAIL_MIME_H

#include <stddef.h>

int mail_mime_extract_text(const char *raw, char *output, size_t output_size);
void mail_mime_first_preview_line(const char *text, char *preview, size_t preview_size);

#endif
