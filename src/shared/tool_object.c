#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define MACHO_ARM64_RELOC_UNSIGNED 0U
#define MACHO_ARM64_RELOC_SUBTRACTOR 1U
#define MACHO_ARM64_RELOC_BRANCH26 2U
#define MACHO_ARM64_RELOC_PAGE21 3U
#define MACHO_ARM64_RELOC_PAGEOFF12 4U
#define MACHO_ARM64_RELOC_GOT_LOAD_PAGE21 5U
#define MACHO_ARM64_RELOC_GOT_LOAD_PAGEOFF12 6U
#define MACHO_ARM64_RELOC_POINTER_TO_GOT 7U
#define MACHO_ARM64_RELOC_TLVP_LOAD_PAGE21 8U
#define MACHO_ARM64_RELOC_TLVP_LOAD_PAGEOFF12 9U
#define MACHO_ARM64_RELOC_ADDEND 10U

const char *tool_macho_arm64_relocation_name(unsigned int type) {
    if (type == MACHO_ARM64_RELOC_UNSIGNED) return "UNSIGNED";
    if (type == MACHO_ARM64_RELOC_SUBTRACTOR) return "SUBTRACTOR";
    if (type == MACHO_ARM64_RELOC_BRANCH26) return "BRANCH26";
    if (type == MACHO_ARM64_RELOC_PAGE21) return "PAGE21";
    if (type == MACHO_ARM64_RELOC_PAGEOFF12) return "PAGEOFF12";
    if (type == MACHO_ARM64_RELOC_GOT_LOAD_PAGE21) return "GOT_LOAD_PAGE21";
    if (type == MACHO_ARM64_RELOC_GOT_LOAD_PAGEOFF12) return "GOT_LOAD_PAGEOFF12";
    if (type == MACHO_ARM64_RELOC_POINTER_TO_GOT) return "POINTER_TO_GOT";
    if (type == MACHO_ARM64_RELOC_TLVP_LOAD_PAGE21) return "TLVP_LOAD_PAGE21";
    if (type == MACHO_ARM64_RELOC_TLVP_LOAD_PAGEOFF12) return "TLVP_LOAD_PAGEOFF12";
    if (type == MACHO_ARM64_RELOC_ADDEND) return "ADDEND";
    return "UNKNOWN";
}

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
