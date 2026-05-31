#include "object_writer_internal.h"

static int object_writer_names_equal(const char *lhs, const char *rhs) {
    return rt_strcmp(lhs, rhs) == 0;
}

static unsigned int object_hash_text(const char *text) {
    unsigned int hash = 2166136261U;

    while (text != 0 && *text != '\0') {
        hash ^= (unsigned int)(unsigned char)*text++;
        hash *= 16777619U;
    }
    return hash;
}

static size_t object_index_bucket(unsigned int hash, size_t capacity) {
    return (size_t)(hash & (unsigned int)(capacity - 1U));
}

static int find_symbol_indexed(const ObjectAssembler *assembler, const char *name) {
    size_t bucket = object_index_bucket(object_hash_text(name), OBJECT_WRITER_SYMBOL_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < OBJECT_WRITER_SYMBOL_INDEX_CAPACITY; ++probe) {
        unsigned int stored = assembler->symbol_index[bucket];
        int index;

        if (stored == 0U) {
            return -1;
        }
        index = (int)stored - 1;
        if (index >= 0 && (size_t)index < assembler->symbol_count && object_writer_names_equal(assembler->symbols[index].name, name)) {
            return index;
        }
        bucket = (bucket + 1U) & (OBJECT_WRITER_SYMBOL_INDEX_CAPACITY - 1U);
    }
    return -1;
}

static void remember_symbol_index(ObjectAssembler *assembler, const char *name, unsigned int index) {
    size_t bucket = object_index_bucket(object_hash_text(name), OBJECT_WRITER_SYMBOL_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < OBJECT_WRITER_SYMBOL_INDEX_CAPACITY; ++probe) {
        unsigned int stored = assembler->symbol_index[bucket];
        int existing = (int)stored - 1;

        if (stored == 0U || (existing >= 0 && (size_t)existing < assembler->symbol_count && object_writer_names_equal(assembler->symbols[existing].name, name))) {
            assembler->symbol_index[bucket] = index + 1U;
            return;
        }
        bucket = (bucket + 1U) & (OBJECT_WRITER_SYMBOL_INDEX_CAPACITY - 1U);
    }
}

int object_writer_find_symbol(const ObjectAssembler *assembler, const char *name) {
    int indexed = find_symbol_indexed(assembler, name);
    size_t i;

    if (indexed >= 0) {
        return indexed;
    }

    for (i = 0; i < assembler->symbol_count; ++i) {
        if (object_writer_names_equal(assembler->symbols[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

int object_writer_get_symbol(ObjectAssembler *assembler, const char *name) {
    int existing = object_writer_find_symbol(assembler, name);
    unsigned int index;

    if (existing >= 0) {
        return existing;
    }
    if (assembler->symbol_count >= OBJECT_WRITER_MAX_SYMBOLS) {
        object_writer_set_error(assembler->writer, "too many symbols for current object writer");
        return -1;
    }

    index = (unsigned int)assembler->symbol_count;
    rt_copy_string(assembler->symbols[index].name, sizeof(assembler->symbols[index].name), name);
    assembler->symbols[index].section = OBJECT_SECTION_NONE;
    assembler->symbols[index].offset = 0;
    assembler->symbols[index].defined = 0;
    assembler->symbols[index].global = 0;
    assembler->symbols[index].is_function = 0;
    assembler->symbols[index].name_offset = 0U;
    assembler->symbols[index].sym_index = 0;
    remember_symbol_index(assembler, name, index);
    assembler->symbol_count += 1U;
    return (int)index;
}

static int find_label_indexed(const ObjectAssembler *assembler, const char *name) {
    size_t bucket = object_index_bucket(object_hash_text(name), OBJECT_WRITER_LABEL_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < OBJECT_WRITER_LABEL_INDEX_CAPACITY; ++probe) {
        unsigned int stored = assembler->label_index[bucket];
        int index;

        if (stored == 0U) {
            return -1;
        }
        index = (int)stored - 1;
        if (index >= 0 && (size_t)index < assembler->label_count && object_writer_names_equal(assembler->labels[index].name, name)) {
            return index;
        }
        bucket = (bucket + 1U) & (OBJECT_WRITER_LABEL_INDEX_CAPACITY - 1U);
    }
    return -1;
}

static void remember_label_index(ObjectAssembler *assembler, const char *name, unsigned int index) {
    size_t bucket = object_index_bucket(object_hash_text(name), OBJECT_WRITER_LABEL_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < OBJECT_WRITER_LABEL_INDEX_CAPACITY; ++probe) {
        unsigned int stored = assembler->label_index[bucket];
        int existing = (int)stored - 1;

        if (stored == 0U || (existing >= 0 && (size_t)existing < assembler->label_count && object_writer_names_equal(assembler->labels[existing].name, name))) {
            assembler->label_index[bucket] = index + 1U;
            return;
        }
        bucket = (bucket + 1U) & (OBJECT_WRITER_LABEL_INDEX_CAPACITY - 1U);
    }
}

int object_writer_find_label(const ObjectAssembler *assembler, const char *name) {
    int indexed = find_label_indexed(assembler, name);
    size_t i;

    if (indexed >= 0) {
        return indexed;
    }

    for (i = 0; i < assembler->label_count; ++i) {
        if (object_writer_names_equal(assembler->labels[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

int object_writer_add_label(ObjectAssembler *assembler, const char *name, ObjectSection section, size_t offset) {
    int existing = object_writer_find_label(assembler, name);

    if (existing >= 0) {
        assembler->labels[existing].section = section;
        assembler->labels[existing].offset = offset;
        return 0;
    }

    if (assembler->label_count >= OBJECT_WRITER_MAX_LABELS) {
        object_writer_set_error(assembler->writer, "too many labels for current object writer");
        return -1;
    }

    rt_copy_string(assembler->labels[assembler->label_count].name, sizeof(assembler->labels[assembler->label_count].name), name);
    assembler->labels[assembler->label_count].section = section;
    assembler->labels[assembler->label_count].offset = offset;
    remember_label_index(assembler, name, (unsigned int)assembler->label_count);
    assembler->label_count += 1U;
    return 0;
}

int object_writer_add_fixup(ObjectAssembler *assembler, const char *name, size_t offset, uint32_t type, int64_t addend) {
    if (assembler->fixup_count >= OBJECT_WRITER_MAX_FIXUPS) {
        object_writer_set_error(assembler->writer, "too many relocations/fixups for current object writer");
        return -1;
    }

    rt_copy_string(assembler->fixups[assembler->fixup_count].name, sizeof(assembler->fixups[assembler->fixup_count].name), name);
    assembler->fixups[assembler->fixup_count].offset = offset;
    assembler->fixups[assembler->fixup_count].type = type;
    assembler->fixups[assembler->fixup_count].addend = addend;
    assembler->fixup_count += 1U;
    return 0;
}

int object_writer_add_relocation(ObjectAssembler *assembler, ObjectSection section, const char *name, size_t offset, uint32_t type, int64_t addend) {
    if (assembler->reloc_count >= OBJECT_WRITER_MAX_RELOCS) {
        object_writer_set_error(assembler->writer, "too many relocation entries for current object writer");
        return -1;
    }

    assembler->relocs[assembler->reloc_count].offset = offset;
    assembler->relocs[assembler->reloc_count].section = section;
    assembler->relocs[assembler->reloc_count].type = type;
    assembler->relocs[assembler->reloc_count].addend = addend;
    rt_copy_string(assembler->relocs[assembler->reloc_count].name, sizeof(assembler->relocs[assembler->reloc_count].name), name);
    assembler->reloc_count += 1U;
    return 0;
}