#ifndef NEWOS_EDITOR_HIGHLIGHT_H
#define NEWOS_EDITOR_HIGHLIGHT_H

#include "tui.h"

#include <stddef.h>

int editor_highlight_enabled(const char *path);
int editor_highlight_style_at(const char *path, const char *line, size_t line_length, size_t offset);

#endif
