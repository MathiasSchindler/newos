#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

int tool_symbol_type_is_function(const char *type) {
    return rt_strcmp(type, "T") == 0 || rt_strcmp(type, "t") == 0 ||
           rt_strcmp(type, "W") == 0 || rt_strcmp(type, "w") == 0;
}

const char *tool_display_symbol_name(const char *name) {
    if (name != 0 && name[0] == '_' && name[1] != '\0') {
        return name + 1;
    }
    return name;
}

const char *tool_newlinker_text_symbol_name(const char *section) {
    const char *name;

    if (!tool_starts_with(section, ".text.")) {
        return 0;
    }
    name = section + 6;
    if (tool_starts_with(name, "startup.")) {
        name += 8;
    } else if (tool_starts_with(name, "unlikely.")) {
        name += 9;
    } else if (tool_starts_with(name, "hot.")) {
        name += 4;
    }
    return name[0] == '\0' ? 0 : name;
}
