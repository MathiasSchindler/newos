#ifndef NEWOS_TOOL_JSON_PARSER_H
#define NEWOS_TOOL_JSON_PARSER_H

#include "tool_util.h"

#define TOOL_JSON_NONE ((size_t)-1)
#define TOOL_JSON_MAX_DEPTH 64U
#define TOOL_JSON_MAX_TOKENS 2000000U

typedef enum {
    TOOL_JSON_OBJECT, TOOL_JSON_ARRAY, TOOL_JSON_STRING,
    TOOL_JSON_NUMBER, TOOL_JSON_BOOL, TOOL_JSON_NULL
} ToolJsonKind;

typedef struct {
    ToolJsonKind kind;
    size_t start;
    size_t end;
    size_t next;
} ToolJsonToken;

typedef struct {
    const char *text;
    size_t length;
    ToolArray tokens;
} ToolJson;

/* Input is borrowed until free; tokens are owned and freed on parse failure. */
int tool_json_parse(ToolJson *json, const char *text, size_t length);
void tool_json_free(ToolJson *json);
const ToolJsonToken *tool_json_token(const ToolJson *json, size_t index);
/* Member names and the allocation-free comparison decode at most 255 bytes. */
size_t tool_json_get(const ToolJson *json, size_t object, const char *key);
int tool_json_string(const ToolJson *json, size_t index, char *out, size_t capacity);
int tool_json_equals(const ToolJson *json, size_t index, const char *text);
int tool_json_equals_alloc(const ToolJson *json, size_t index, const char *text);
char *tool_json_dup_string(const ToolJson *json, size_t index, size_t limit);
int tool_json_uint(const ToolJson *json, size_t index, unsigned long long *out);
int tool_json_true(const ToolJson *json, size_t index);

#endif
