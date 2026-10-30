#ifndef NEWOS_TOOL_XML_H
#define NEWOS_TOOL_XML_H

#include "xml.h"

int tool_xml_name_stack_push(XmlNameStack *stack, XmlName name, const char *tool_name);

typedef enum {
    TOOL_XML_KEY_ATTR = 1,
    TOOL_XML_KEY_TEXT,
    TOOL_XML_KEY_CHILD
} ToolXmlKeyKind;

typedef struct {
    ToolXmlKeyKind kind;
    const char *name;
} ToolXmlKeySpec;

typedef struct {
    const char *key;
    size_t key_length;
    unsigned int active_child_depth;
    int found;
} ToolXmlKeyState;

int tool_xml_key_parse(const char *text, ToolXmlKeySpec *spec, const char *tool_name);
int tool_xml_selector_compile(XmlSelector *selector_out, const char *selector, const char *tool_name);
void tool_xml_key_state_init(ToolXmlKeyState *state);
void tool_xml_key_start(const ToolXmlKeySpec *spec, const XmlToken *token, unsigned int depth, unsigned int capture_depth, ToolXmlKeyState *state);
void tool_xml_key_text(const ToolXmlKeySpec *spec, const XmlToken *token, unsigned int depth, unsigned int capture_depth, ToolXmlKeyState *state);
void tool_xml_key_end(const ToolXmlKeySpec *spec, unsigned int depth, ToolXmlKeyState *state);

#endif
