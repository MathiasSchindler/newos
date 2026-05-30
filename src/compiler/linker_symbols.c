#include "linker_internal.h"

LinkGlobal        linker_globals[LINKER_MAX_GLOBALS];
LinkDefinedSymbol linker_defined_symbols[LINKER_MAX_GLOBALS];
unsigned int      linker_global_buckets[LINKER_SYMBOL_BUCKETS];
unsigned int      linker_defined_symbol_buckets[LINKER_SYMBOL_BUCKETS];
size_t            linker_global_count;
size_t            linker_defined_symbol_count;

static unsigned int linker_hash_name(const char *name) {
    unsigned int hash = 2166136261U;
    const unsigned char *cursor = (const unsigned char *)name;

    while (*cursor != 0U) {
        hash ^= (unsigned int)*cursor;
        hash *= 16777619U;
        cursor += 1;
    }
    return hash == 0U ? 1U : hash;
}

static unsigned int linker_symbol_bucket(unsigned int hash) {
    return hash & (LINKER_SYMBOL_BUCKETS - 1U);
}

void reset_global_index(void) {
    linker_global_count = 0U;
    rt_memset(linker_global_buckets, 0, sizeof(linker_global_buckets));
}

static void reset_defined_symbol_index(void) {
    linker_defined_symbol_count = 0U;
    rt_memset(linker_defined_symbol_buckets, 0, sizeof(linker_defined_symbol_buckets));
}

int linker_find_global(const char *name) {
    unsigned int hash;
    unsigned int entry;

    if (name == 0 || name[0] == '\0') {
        return -1;
    }
    hash = linker_hash_name(name);
    entry = linker_global_buckets[linker_symbol_bucket(hash)];
    while (entry != 0U) {
        unsigned int index = entry - 1U;

        if (linker_globals[index].hash == hash && rt_strcmp(linker_globals[index].name, name) == 0) {
            return (int)index;
        }
        entry = linker_globals[index].next_index;
    }
    return -1;
}

int linker_add_global(const char *name, uint64_t value, char *error_out, size_t error_size) {
    unsigned int hash;
    unsigned int bucket;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }
    hash = linker_hash_name(name);
    bucket = linker_symbol_bucket(hash);
    if (linker_find_global(name) >= 0) {
        set_link_error(error_out, error_size, "duplicate global symbol", name);
        return -1;
    }
    if (linker_global_count >= LINKER_MAX_GLOBALS) {
        set_link_error(error_out, error_size, "too many global symbols", name);
        return -1;
    }
    rt_copy_string(linker_globals[linker_global_count].name, sizeof(linker_globals[linker_global_count].name), name);
    linker_globals[linker_global_count].value = value;
    linker_globals[linker_global_count].hash = hash;
    linker_globals[linker_global_count].next_index = linker_global_buckets[bucket];
    linker_global_buckets[bucket] = (unsigned int)linker_global_count + 1U;
    linker_global_count += 1U;
    return 0;
}

int find_defined_symbol_owner(const char *name) {
    unsigned int hash;
    unsigned int entry;

    if (name == 0 || name[0] == '\0') {
        return -1;
    }
    hash = linker_hash_name(name);
    entry = linker_defined_symbol_buckets[linker_symbol_bucket(hash)];
    while (entry != 0U) {
        unsigned int index = entry - 1U;

        if (linker_defined_symbols[index].hash == hash && rt_strcmp(linker_defined_symbols[index].name, name) == 0) {
            return (int)linker_defined_symbols[index].object_index;
        }
        entry = linker_defined_symbols[index].next_index;
    }
    return -1;
}

int add_defined_symbol_owner(const char *name, size_t object_index, char *error_out, size_t error_size) {
    unsigned int hash;
    unsigned int bucket;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }
    hash = linker_hash_name(name);
    bucket = linker_symbol_bucket(hash);
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
    linker_defined_symbols[linker_defined_symbol_count].hash = hash;
    linker_defined_symbols[linker_defined_symbol_count].next_index = linker_defined_symbol_buckets[bucket];
    linker_defined_symbol_buckets[bucket] = (unsigned int)linker_defined_symbol_count + 1U;
    linker_defined_symbol_count += 1U;
    return 0;
}

int collect_defined_symbol_owners(LinkObject *objects, size_t object_count, char *error_out, size_t error_size) {
    size_t i;

    reset_defined_symbol_index();
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
