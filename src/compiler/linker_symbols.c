#include "linker_internal.h"

LinkGlobal        linker_globals[LINKER_MAX_GLOBALS];
LinkDefinedSymbol linker_defined_symbols[LINKER_MAX_GLOBALS];
size_t            linker_global_count;
size_t            linker_defined_symbol_count;

int linker_find_global(const char *name) {
    size_t i;

    for (i = 0; i < linker_global_count; ++i) {
        if (rt_strcmp(linker_globals[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int linker_add_global(const char *name, uint64_t value, char *error_out, size_t error_size) {
    int existing;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }
    existing = linker_find_global(name);
    if (existing >= 0) {
        set_link_error(error_out, error_size, "duplicate global symbol", name);
        return -1;
    }
    if (linker_global_count >= LINKER_MAX_GLOBALS) {
        set_link_error(error_out, error_size, "too many global symbols", name);
        return -1;
    }
    rt_copy_string(linker_globals[linker_global_count].name, sizeof(linker_globals[linker_global_count].name), name);
    linker_globals[linker_global_count].value = value;
    linker_global_count += 1U;
    return 0;
}

int find_defined_symbol_owner(const char *name) {
    size_t i;

    for (i = 0; i < linker_defined_symbol_count; ++i) {
        if (rt_strcmp(linker_defined_symbols[i].name, name) == 0) {
            return (int)linker_defined_symbols[i].object_index;
        }
    }
    return -1;
}

int add_defined_symbol_owner(const char *name, size_t object_index, char *error_out, size_t error_size) {
    if (name == 0 || name[0] == '\0') {
        return 0;
    }
    if (find_defined_symbol_owner(name) >= 0) {
        return 0;
    }
    if (linker_defined_symbol_count >= LINKER_MAX_GLOBALS) {
        set_link_error(error_out, error_size, "too many global symbols", name);
        return -1;
    }
    rt_copy_string(linker_defined_symbols[linker_defined_symbol_count].name,
                   sizeof(linker_defined_symbols[linker_defined_symbol_count].name),
                   name);
    linker_defined_symbols[linker_defined_symbol_count].object_index = object_index;
    linker_defined_symbol_count += 1U;
    return 0;
}

int collect_defined_symbol_owners(LinkObject *objects, size_t object_count, char *error_out, size_t error_size) {
    size_t i;

    linker_defined_symbol_count = 0;
    for (i = 0; i < object_count; ++i) {
        LinkObject *object = &objects[i];
        uint32_t count = (uint32_t)(object->symtab_size / object->symtab_entsize);
        uint32_t symbol_index;

        for (symbol_index = 0; symbol_index < count; ++symbol_index) {
            const unsigned char *symbol = symbol_entry(object, symbol_index);
            if (symbol == 0) {
                set_link_error(error_out, error_size, "invalid symbol table", object->path);
                return -1;
            }
            if ((symbol[4] >> 4U) == STB_GLOBAL && read_u16(symbol + 6) != SHN_UNDEF &&
                add_defined_symbol_owner(symbol_name(object, symbol), i, error_out, error_size) != 0) {
                return -1;
            }
        }
    }
    return 0;
}
