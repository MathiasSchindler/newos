#include "linker_internal.h"

static uint64_t section_trailing_zero_bytes(const LinkObject *object, const LinkSection *section) {
    uint64_t count = 0ULL;

    if (section->type == SHT_NOBITS || section->size == 0ULL) {
        return 0ULL;
    }
    while (count < section->size && object->file[section->offset + section->size - count - 1ULL] == 0U) {
        count += 1ULL;
    }
    return count;
}

static int layout_section_is_better(const LinkObject *candidate_object,
                                    const LinkSection *candidate,
                                    const LinkObject *best_object,
                                    const LinkSection *best,
                                    LinkSectionKind kind) {
    if (best == 0) {
        return 1;
    }
    if (candidate->align != best->align) {
        return candidate->align > best->align;
    }
    if (kind == LINK_SECTION_DATA) {
        uint64_t candidate_zero_tail = section_trailing_zero_bytes(candidate_object, candidate);
        uint64_t best_zero_tail = section_trailing_zero_bytes(best_object, best);

        if (candidate_zero_tail != best_zero_tail) {
            return candidate_zero_tail < best_zero_tail;
        }
    }
    return candidate->size > best->size;
}

typedef struct {
    LinkObject *object;
    LinkSection *section;
    size_t object_index;
    size_t section_index;
    uint64_t zero_tail;
} LinkLayoutEntry;

#define LINKER_ORDER_FILE_CAPACITY (1024U * 1024U)
#define LINKER_PROFILE_NAME_CAPACITY 256U
#define LINKER_PROFILE_MAX_NODES 8192U
#define LINKER_PROFILE_MAX_EDGES 32768U

typedef struct {
    char name[LINKER_PROFILE_NAME_CAPACITY];
    uint64_t weight;
    LinkSection *section;
} LinkProfileNode;

typedef struct {
    size_t caller;
    size_t callee;
    uint64_t weight;
} LinkProfileEdge;

static LinkSectionKind layout_sort_kind;
static int layout_sort_call_graph;
uint64_t linker_profile_nodes_total;
uint64_t linker_profile_nodes_matched;
uint64_t linker_profile_edges_total;
uint64_t linker_profile_edges_matched;
uint64_t linker_profile_sections_ordered;
uint64_t linker_profile_bytes_ordered;

static int compare_layout_entries(const void *left_ptr, const void *right_ptr) {
    const LinkLayoutEntry *left = (const LinkLayoutEntry *)left_ptr;
    const LinkLayoutEntry *right = (const LinkLayoutEntry *)right_ptr;

    if (layout_sort_call_graph && layout_sort_kind == LINK_SECTION_TEXT) {
        int left_ranked = left->section->layout_rank != LINKER_UNPLACED_OFFSET;
        int right_ranked = right->section->layout_rank != LINKER_UNPLACED_OFFSET;

        if (left_ranked != right_ranked) {
            return left_ranked ? -1 : 1;
        }
        if (left_ranked && left->section->layout_rank != right->section->layout_rank) {
            return left->section->layout_rank < right->section->layout_rank ? -1 : 1;
        }
    }
    if (left->section->align != right->section->align) {
        return left->section->align > right->section->align ? -1 : 1;
    }
    if (layout_sort_kind == LINK_SECTION_DATA && left->zero_tail != right->zero_tail) {
        return left->zero_tail < right->zero_tail ? -1 : 1;
    }
    if (left->section->size != right->section->size) {
        return left->section->size > right->section->size ? -1 : 1;
    }
    if (left->object_index != right->object_index) {
        return left->object_index < right->object_index ? -1 : 1;
    }
    if (left->section_index != right->section_index) {
        return left->section_index < right->section_index ? -1 : 1;
    }
    return 0;
}

static uint64_t layout_sections_of_kind_slow(LinkObject *objects, size_t object_count, LinkSectionKind kind) {
    uint64_t size = 0ULL;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            LinkSection *section = &objects[i].sections[section_index];

            if (section->live && !section->folded && section->kind == kind) {
                section->out_offset = LINKER_UNPLACED_OFFSET;
            }
        }
    }
    for (;;) {
        LinkSection *best = 0;
        LinkObject *best_object = 0;
        size_t best_object_index = 0U;
        size_t best_section_index = 0U;

        for (i = 0; i < object_count; ++i) {
            size_t section_index;

            if (!objects[i].live) {
                continue;
            }
            for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
                LinkSection *section = &objects[i].sections[section_index];

                if (!section->live || section->folded || section->kind != kind || section->out_offset != LINKER_UNPLACED_OFFSET) {
                    continue;
                }
                if (layout_section_is_better(&objects[i], section, best_object, best, kind)) {
                    best = section;
                    best_object = &objects[i];
                    best_object_index = i;
                    best_section_index = section_index;
                }
            }
        }
        if (best == 0) {
            break;
        }
        (void)best_object_index;
        (void)best_section_index;
        size = align_u64(size, best->align);
        best->out_offset = size;
        size += best->size;
    }
    return size;
}

static uint64_t layout_sections_of_kind(LinkObject *objects, size_t object_count, LinkSectionKind kind, int call_graph_order) {
    LinkLayoutEntry *entries;
    size_t entry_count = 0U;
    size_t entry_index = 0U;
    size_t i;
    uint64_t size = 0ULL;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            LinkSection *section = &objects[i].sections[section_index];

            if (section->live && !section->folded && section->kind == kind) {
                section->out_offset = LINKER_UNPLACED_OFFSET;
                entry_count += 1U;
            }
        }
    }
    if (entry_count == 0U) {
        return 0ULL;
    }
    entries = (LinkLayoutEntry *)rt_malloc_array(entry_count, sizeof(*entries));
    if (entries == 0) {
        return layout_sections_of_kind_slow(objects, object_count, kind);
    }
    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            LinkSection *section = &objects[i].sections[section_index];

            if (!section->live || section->folded || section->kind != kind) {
                continue;
            }
            entries[entry_index].object = &objects[i];
            entries[entry_index].section = section;
            entries[entry_index].object_index = i;
            entries[entry_index].section_index = section_index;
            entries[entry_index].zero_tail = kind == LINK_SECTION_DATA ? section_trailing_zero_bytes(&objects[i], section) : 0ULL;
            entry_index += 1U;
        }
    }
    layout_sort_kind = kind;
    layout_sort_call_graph = call_graph_order;
    rt_sort(entries, entry_count, sizeof(*entries), compare_layout_entries);
    for (entry_index = 0U; entry_index < entry_count; ++entry_index) {
        LinkSection *section = entries[entry_index].section;

        size = align_u64(size, section->align);
        section->out_offset = size;
        size += section->size;
    }
    rt_free(entries);
    return size;
}

static LinkSection *layout_resolve_symbol_section(LinkObject *objects,
                                                  size_t object_count,
                                                  LinkObject *source,
                                                  const unsigned char *symbol,
                                                  LinkObject **object_out) {
    uint16_t shndx = read_u16(symbol + 6);
    LinkObject *object = source;

    if (shndx == SHN_UNDEF) {
        int owner = find_defined_symbol_owner(symbol_name(source, symbol));
        uint32_t count;
        uint32_t symbol_index;

        if (owner < 0 || (size_t)owner >= object_count) {
            return 0;
        }
        object = &objects[owner];
        count = (uint32_t)(object->symtab_size / object->symtab_entsize);
        for (symbol_index = 0; symbol_index < count; ++symbol_index) {
            const unsigned char *definition = symbol_entry(object, symbol_index);

            if (definition != 0 && read_u16(definition + 6) != SHN_UNDEF &&
                rt_strcmp(symbol_name(object, definition), symbol_name(source, symbol)) == 0) {
                shndx = read_u16(definition + 6);
                break;
            }
        }
        if (symbol_index == count) {
            return 0;
        }
    }
    if (shndx == SHN_ABS || shndx == SHN_UNDEF) {
        return 0;
    }
    *object_out = object;
    return find_link_section(object, shndx);
}

static int layout_queue_contains(LinkSection *const *queue, size_t count, const LinkSection *section) {
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (queue[index] == section) {
            return 1;
        }
    }
    return 0;
}

static uint64_t next_layout_rank(const LinkObject *objects, size_t object_count) {
    uint64_t rank = 0ULL;
    size_t object_index;

    for (object_index = 0U; object_index < object_count; ++object_index) {
        size_t section_index;
        for (section_index = 0U; section_index < objects[object_index].section_count; ++section_index) {
            uint64_t section_rank = objects[object_index].sections[section_index].layout_rank;
            if (section_rank != LINKER_UNPLACED_OFFSET && section_rank >= rank) {
                rank = section_rank + 1ULL;
            }
        }
    }
    return rank;
}

static void rank_named_sections(LinkObject *objects, size_t object_count, const char *name, uint64_t *rank) {
    size_t object_index;

    for (object_index = 0U; object_index < object_count; ++object_index) {
        LinkObject *object = &objects[object_index];
        uint32_t symbol_count = (uint32_t)(object->symtab_size / object->symtab_entsize);
        uint32_t symbol_index;
        size_t section_index;

        for (section_index = 0U; section_index < object->section_count; ++section_index) {
            LinkSection *section = &object->sections[section_index];
            if (section->live && !section->folded && (section->flags & SHF_EXECINSTR) != 0ULL &&
                section->layout_rank == LINKER_UNPLACED_OFFSET && rt_strcmp(section_name(object, section->index), name) == 0) {
                section->layout_rank = (*rank)++;
            }
        }
        for (symbol_index = 0U; symbol_index < symbol_count; ++symbol_index) {
            const unsigned char *symbol = symbol_entry(object, symbol_index);
            uint16_t shndx;
            LinkSection *section;

            if (symbol == 0 || rt_strcmp(symbol_name(object, symbol), name) != 0) {
                continue;
            }
            shndx = read_u16(symbol + 6);
            if (shndx == SHN_UNDEF || shndx == SHN_ABS) {
                continue;
            }
            section = find_link_section(object, shndx);
            if (section != 0 && section->folded) {
                section = &objects[section->fold_object_index].sections[section->fold_section_index];
            }
            if (section != 0 && section->live && (section->flags & SHF_EXECINSTR) != 0ULL && section->layout_rank == LINKER_UNPLACED_OFFSET) {
                section->layout_rank = (*rank)++;
            }
        }
    }
}

static LinkSection *find_named_executable_section(LinkObject *objects, size_t object_count, const char *name) {
    size_t object_index;

    for (object_index = 0U; object_index < object_count; ++object_index) {
        LinkObject *object = &objects[object_index];
        uint32_t symbol_count = (uint32_t)(object->symtab_size / object->symtab_entsize);
        uint32_t symbol_index;
        size_t section_index;

        for (section_index = 0U; section_index < object->section_count; ++section_index) {
            LinkSection *section = &object->sections[section_index];
            if (section->live && (section->flags & SHF_EXECINSTR) != 0ULL && rt_strcmp(section_name(object, section->index), name) == 0) {
                return section->folded ? &objects[section->fold_object_index].sections[section->fold_section_index] : section;
            }
        }
        for (symbol_index = 0U; symbol_index < symbol_count; ++symbol_index) {
            const unsigned char *symbol = symbol_entry(object, symbol_index);
            uint16_t shndx;
            LinkSection *section;

            if (symbol == 0 || rt_strcmp(symbol_name(object, symbol), name) != 0) {
                continue;
            }
            shndx = read_u16(symbol + 6);
            if (shndx == SHN_UNDEF || shndx == SHN_ABS) {
                continue;
            }
            section = find_link_section(object, shndx);
            if (section != 0 && section->folded) {
                section = &objects[section->fold_object_index].sections[section->fold_section_index];
            }
            if (section != 0 && section->live && (section->flags & SHF_EXECINSTR) != 0ULL) {
                return section;
            }
        }
    }
    return 0;
}

static int profile_find_or_add_node(LinkProfileNode *nodes, size_t *count, const char *name, size_t *index_out) {
    size_t index;

    for (index = 0U; index < *count; ++index) {
        if (rt_strcmp(nodes[index].name, name) == 0) {
            *index_out = index;
            return 0;
        }
    }
    if (*count >= LINKER_PROFILE_MAX_NODES) {
        return -1;
    }
    rt_copy_string(nodes[*count].name, sizeof(nodes[*count].name), name);
    nodes[*count].weight = 0ULL;
    nodes[*count].section = 0;
    *index_out = (*count)++;
    return 0;
}

static char *layout_next_token(char **cursor_io) {
    char *cursor = *cursor_io;
    char *token;

    while (*cursor == ' ' || *cursor == '\t') {
        cursor += 1;
    }
    if (*cursor == '\0' || *cursor == '#') {
        *cursor_io = cursor;
        return 0;
    }
    token = cursor;
    while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' && *cursor != '#') {
        cursor += 1;
    }
    if (*cursor != '\0') {
        *cursor++ = '\0';
    }
    *cursor_io = cursor;
    return token;
}

static int rank_call_graph_profile(LinkObject *objects, size_t object_count, const char *path, char *error_out, size_t error_size) {
    unsigned char *file = 0;
    LinkProfileNode *nodes = 0;
    LinkProfileEdge *edges = 0;
    size_t size = 0U;
    size_t offset = 0U;
    size_t node_count = 0U;
    size_t edge_count = 0U;
    uint64_t rank = next_layout_rank(objects, object_count);
    int result = -1;

    nodes = (LinkProfileNode *)rt_malloc_array(LINKER_PROFILE_MAX_NODES, sizeof(*nodes));
    edges = (LinkProfileEdge *)rt_malloc_array(LINKER_PROFILE_MAX_EDGES, sizeof(*edges));
    if (nodes == 0 || edges == 0) {
        set_link_error(error_out, error_size, "failed to allocate call graph profile", path);
        goto done;
    }
    rt_memset(nodes, 0, LINKER_PROFILE_MAX_NODES * sizeof(*nodes));
    rt_memset(edges, 0, LINKER_PROFILE_MAX_EDGES * sizeof(*edges));
    if (read_file_alloc(path, LINKER_ORDER_FILE_CAPACITY, &file, &size, error_out, error_size) != 0) {
        goto done;
    }
    file[size] = '\0';
    while (offset < size) {
        char *line = (char *)file + offset;
        char *cursor;
        char *kind;
        char *weight_text;
        char *first_name;
        char *second_name;
        unsigned long long parsed_weight;
        size_t first_index;
        size_t second_index;

        while (offset < size && file[offset] != '\n' && file[offset] != '\r') {
            offset += 1U;
        }
        while (offset < size && (file[offset] == '\n' || file[offset] == '\r')) {
            file[offset++] = '\0';
        }
        cursor = line;
        kind = layout_next_token(&cursor);
        if (kind == 0) {
            continue;
        }
        weight_text = layout_next_token(&cursor);
        first_name = layout_next_token(&cursor);
        second_name = rt_strcmp(kind, "edge") == 0 ? layout_next_token(&cursor) : 0;
        if (weight_text == 0 || first_name == 0 || rt_parse_uint(weight_text, &parsed_weight) != 0 ||
            (rt_strcmp(kind, "node") != 0 && rt_strcmp(kind, "edge") != 0) ||
            (rt_strcmp(kind, "edge") == 0 && second_name == 0)) {
            set_link_error(error_out, error_size, "malformed call graph profile", path);
            goto done;
        }
        if (profile_find_or_add_node(nodes, &node_count, first_name, &first_index) != 0) {
            set_link_error(error_out, error_size, "too many call graph profile nodes", path);
            goto done;
        }
        if (rt_strcmp(kind, "node") == 0) {
            nodes[first_index].weight += (uint64_t)parsed_weight;
            continue;
        }
        if (profile_find_or_add_node(nodes, &node_count, second_name, &second_index) != 0 || edge_count >= LINKER_PROFILE_MAX_EDGES) {
            set_link_error(error_out, error_size, "too many call graph profile edges", path);
            goto done;
        }
        edges[edge_count].caller = first_index;
        edges[edge_count].callee = second_index;
        edges[edge_count].weight = (uint64_t)parsed_weight;
        edge_count += 1U;
    }
    {
        size_t index;
        for (index = 0U; index < node_count; ++index) {
            nodes[index].section = find_named_executable_section(objects, object_count, nodes[index].name);
            if (nodes[index].section != 0) {
                linker_profile_nodes_matched += 1ULL;
            }
        }
        for (index = 0U; index < edge_count; ++index) {
            if (nodes[edges[index].caller].section != 0 && nodes[edges[index].callee].section != 0) {
                linker_profile_edges_matched += 1ULL;
            }
        }
    }
    linker_profile_nodes_total = (uint64_t)node_count;
    linker_profile_edges_total = (uint64_t)edge_count;
    for (;;) {
        size_t root = node_count;
        size_t index;
        size_t current;

        for (index = 0U; index < node_count; ++index) {
            if (nodes[index].section == 0 || nodes[index].section->layout_rank != LINKER_UNPLACED_OFFSET) {
                continue;
            }
            if (root == node_count || nodes[index].weight > nodes[root].weight ||
                (nodes[index].weight == nodes[root].weight && rt_strcmp(nodes[index].name, nodes[root].name) < 0)) {
                root = index;
            }
        }
        if (root == node_count) {
            break;
        }
        nodes[root].section->layout_rank = rank++;
        linker_profile_sections_ordered += 1ULL;
        linker_profile_bytes_ordered += nodes[root].section->size;
        current = root;
        for (;;) {
            size_t best_edge = edge_count;

            for (index = 0U; index < edge_count; ++index) {
                LinkProfileNode *callee;
                if (edges[index].caller != current) {
                    continue;
                }
                callee = &nodes[edges[index].callee];
                if (callee->section == 0 || callee->section->layout_rank != LINKER_UNPLACED_OFFSET) {
                    continue;
                }
                if (best_edge == edge_count || edges[index].weight > edges[best_edge].weight ||
                    (edges[index].weight == edges[best_edge].weight && rt_strcmp(callee->name, nodes[edges[best_edge].callee].name) < 0)) {
                    best_edge = index;
                }
            }
            if (best_edge == edge_count) {
                break;
            }
            current = edges[best_edge].callee;
            nodes[current].section->layout_rank = rank++;
            linker_profile_sections_ordered += 1ULL;
            linker_profile_bytes_ordered += nodes[current].section->size;
        }
    }
    result = 0;
done:
    rt_free(file);
    rt_free(edges);
    rt_free(nodes);
    return result;
}

static int rank_symbol_ordering_file(LinkObject *objects, size_t object_count, const char *path, char *error_out, size_t error_size) {
    unsigned char *file = 0;
    size_t size = 0U;
    size_t offset = 0U;
    uint64_t rank = 0ULL;

    if (read_file_alloc(path, LINKER_ORDER_FILE_CAPACITY, &file, &size, error_out, error_size) != 0) {
        return -1;
    }
    file[size] = '\0';
    while (offset < size) {
        char *line = (char *)file + offset;
        char *end;
        char *cursor;

        while (offset < size && file[offset] != '\n' && file[offset] != '\r') {
            offset += 1U;
        }
        end = (char *)file + offset;
        while (offset < size && (file[offset] == '\n' || file[offset] == '\r')) {
            file[offset++] = '\0';
        }
        while (line < end && (*line == ' ' || *line == '\t')) {
            line += 1;
        }
        cursor = line;
        while (cursor < end && *cursor != ' ' && *cursor != '\t' && *cursor != '#') {
            cursor += 1;
        }
        *cursor = '\0';
        if (*line != '\0' && *line != '#') {
            rank_named_sections(objects, object_count, line, &rank);
        }
    }
    rt_free(file);
    return 0;
}

static void rank_call_graph_sections(LinkObject *objects, size_t object_count, const char *entry_symbol) {
    LinkSection **queue;
    LinkObject **queue_objects;
    size_t capacity = 0U;
    size_t head = 0U;
    size_t tail = 0U;
    uint64_t rank = next_layout_rank(objects, object_count);
    size_t object_index;

    for (object_index = 0; object_index < object_count; ++object_index) {
        size_t section_index;
        for (section_index = 0; section_index < objects[object_index].section_count; ++section_index) {
            capacity += 1U;
        }
    }
    queue = (LinkSection **)rt_malloc_array(capacity == 0U ? 1U : capacity, sizeof(*queue));
    queue_objects = (LinkObject **)rt_malloc_array(capacity == 0U ? 1U : capacity, sizeof(*queue_objects));
    if (queue == 0 || queue_objects == 0) {
        rt_free(queue);
        rt_free(queue_objects);
        return;
    }
    for (object_index = 0; object_index < object_count && tail == 0U; ++object_index) {
        LinkObject *object = &objects[object_index];
        uint32_t count = (uint32_t)(object->symtab_size / object->symtab_entsize);
        uint32_t symbol_index;

        for (symbol_index = 0; symbol_index < count; ++symbol_index) {
            const unsigned char *symbol = symbol_entry(object, symbol_index);
            LinkSection *section;

            if (symbol == 0 || read_u16(symbol + 6) == SHN_UNDEF || rt_strcmp(symbol_name(object, symbol), entry_symbol) != 0) {
                continue;
            }
            section = find_link_section(object, read_u16(symbol + 6));
            if (section != 0 && section->live && !section->folded && (section->flags & SHF_EXECINSTR) != 0ULL) {
                if (section->layout_rank == LINKER_UNPLACED_OFFSET) {
                    section->layout_rank = rank++;
                }
                queue[tail] = section;
                queue_objects[tail++] = object;
            }
            break;
        }
    }
    while (head < tail) {
        LinkSection *section = queue[head];
        LinkObject *object = queue_objects[head++];
        const LinkRelaSection *rela = find_rela_section_const(object, section->index);
        uint64_t relocation_count;
        uint64_t relocation_index;

        if (rela == 0 || rela->entsize == 0ULL) {
            continue;
        }
        relocation_count = rela->size / rela->entsize;
        for (relocation_index = 0ULL; relocation_index < relocation_count; ++relocation_index) {
            const unsigned char *relocation = object->file + rela->offset + relocation_index * rela->entsize;
            const unsigned char *symbol = symbol_entry(object, (uint32_t)(read_u64(relocation + 8) >> 32U));
            LinkSection *target;
            LinkObject *target_object;

            if (symbol == 0) {
                continue;
            }
            target = layout_resolve_symbol_section(objects, object_count, object, symbol, &target_object);
            if (target != 0 && target->folded) {
                target_object = &objects[target->fold_object_index];
                target = &target_object->sections[target->fold_section_index];
            }
            if (target == 0 || !target->live || (target->flags & SHF_EXECINSTR) == 0ULL || layout_queue_contains(queue, tail, target)) {
                continue;
            }
            if (target->layout_rank == LINKER_UNPLACED_OFFSET) {
                target->layout_rank = rank++;
            }
            queue[tail] = target;
            queue_objects[tail++] = target_object;
        }
    }
    rt_free(queue);
    rt_free(queue_objects);
}

int layout_objects(LinkObject *objects, size_t object_count, const char *entry_symbol, int call_graph_order, const char *symbol_ordering_file, const char *call_graph_profile, uint64_t *text_size_out, uint64_t *data_size_out, uint64_t *bss_size_out, char *error_out, size_t error_size) {
    size_t object_index;

    linker_profile_nodes_total = 0ULL;
    linker_profile_nodes_matched = 0ULL;
    linker_profile_edges_total = 0ULL;
    linker_profile_edges_matched = 0ULL;
    linker_profile_sections_ordered = 0ULL;
    linker_profile_bytes_ordered = 0ULL;
    for (object_index = 0U; object_index < object_count; ++object_index) {
        size_t section_index;
        for (section_index = 0U; section_index < objects[object_index].section_count; ++section_index) {
            objects[object_index].sections[section_index].layout_rank = LINKER_UNPLACED_OFFSET;
        }
    }
    if (symbol_ordering_file != 0 && symbol_ordering_file[0] != '\0' &&
        rank_symbol_ordering_file(objects, object_count, symbol_ordering_file, error_out, error_size) != 0) {
        return -1;
    }
    if (call_graph_profile != 0 && call_graph_profile[0] != '\0' &&
        rank_call_graph_profile(objects, object_count, call_graph_profile, error_out, error_size) != 0) {
        return -1;
    }
    if (call_graph_order) {
        rank_call_graph_sections(objects, object_count, entry_symbol);
    }
    *text_size_out = layout_sections_of_kind(objects, object_count, LINK_SECTION_TEXT,
                                             call_graph_order ||
                                             (symbol_ordering_file != 0 && symbol_ordering_file[0] != '\0') ||
                                             (call_graph_profile != 0 && call_graph_profile[0] != '\0'));
    *data_size_out = layout_sections_of_kind(objects, object_count, LINK_SECTION_DATA, 0);
    *bss_size_out = layout_sections_of_kind(objects, object_count, LINK_SECTION_BSS, 0);
    return 0;
}

uint64_t max_live_section_alignment(const LinkObject *objects, size_t object_count, LinkSectionKind kind) {
    uint64_t alignment = 1ULL;
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];

            if (section->live && !section->folded && section->kind == kind && section->align > alignment) {
                alignment = section->align;
            }
        }
    }
    return alignment;
}

void copy_sections(LinkObject *objects, size_t object_count, unsigned char *output) {
    size_t i;

    for (i = 0; i < object_count; ++i) {
        size_t section_index;

        if (!objects[i].live) {
            continue;
        }
        for (section_index = 0; section_index < objects[i].section_count; ++section_index) {
            const LinkSection *section = &objects[i].sections[section_index];

            if (!section->live || section->folded || section->kind == LINK_SECTION_BSS || section->size == 0ULL) {
                continue;
            }
            if (linker_merge_string_pool_active && section == &linker_objects[linker_merge_master_object_index].sections[linker_merge_master_section_index]) {
                memcpy(output + section->out_offset, linker_merge_string_pool, (size_t)section->size);
#if COMPILER_LINKER_ENABLE_CONST_MERGE
            } else if (linker_merge_const_pool_active && section == &linker_objects[linker_merge_const_master_object_index].sections[linker_merge_const_master_section_index]) {
                memcpy(output + section->out_offset, linker_merge_const_pool, (size_t)section->size);
#endif
            } else {
                memcpy(output + section->out_offset, objects[i].file + section->offset, (size_t)section->size);
            }
        }
    }
}

static void write_load_header(unsigned char *output, uint64_t program_header_offset, uint16_t index, uint32_t flags, uint64_t offset, uint64_t file_size, uint64_t memory_size, uint64_t alignment) {
    unsigned char *program = output + program_header_offset + ((uint64_t)index * ELF64_PHDR_SIZE);

    write_u32(program + 0, PT_LOAD);
    write_u32(program + 4, flags);
    write_u64(program + 8, offset);
    write_u64(program + 16, LINKER_BASE_VADDR + offset);
    write_u64(program + 24, LINKER_BASE_VADDR + offset);
    write_u64(program + 32, file_size);
    write_u64(program + 40, memory_size);
    write_u64(program + 48, alignment);
}

void write_elf_header(unsigned char *output,
                      uint64_t entry,
                      uint64_t text_file_offset,
                      uint64_t text_size,
                      uint64_t data_file_offset,
                      uint64_t data_size,
                      uint64_t bss_size,
                      uint64_t file_size,
                      uint64_t memory_size,
                      int tiny) {
    uint16_t program_count = tiny ? 1U : (data_size != 0 || bss_size != 0 ? 2U : 1U);
    uint64_t segment_alignment = tiny ? 1U : 0x1000U;
    uint64_t program_header_offset = tiny ? ELF64_EHDR_SIZE - ELF64_TINY_PHDR_OVERLAP : ELF64_EHDR_SIZE;

    output[0] = 0x7fU;
    output[1] = 'E';
    output[2] = 'L';
    output[3] = 'F';
    output[4] = ELFCLASS64;
    output[5] = ELFDATA2LSB;
    output[6] = EV_CURRENT;
    output[7] = 0U;
    write_u16(output + 16, ET_EXEC);
    write_u16(output + 18, EM_X86_64);
    write_u32(output + 20, EV_CURRENT);
    write_u64(output + 24, entry);
    write_u64(output + 32, program_header_offset);
    write_u64(output + 40, 0U);
    write_u32(output + 48, 0U);
    write_u16(output + 52, ELF64_EHDR_SIZE);
    write_u16(output + 54, ELF64_PHDR_SIZE);
    write_u16(output + 56, program_count);
    write_u16(output + 58, 0U);
    write_u16(output + 60, 0U);
    write_u16(output + 62, 0U);

    if (tiny) {
        uint32_t flags = PF_R | PF_X;
        if (data_size != 0 || bss_size != 0) {
            flags |= PF_W;
        }
        write_load_header(output, program_header_offset, 0U, flags, 0U, file_size, memory_size, segment_alignment);
        return;
    }

    write_load_header(output, program_header_offset, 0U, PF_R | PF_X, 0U, file_size < text_file_offset + text_size ? file_size : text_file_offset + text_size, text_file_offset + text_size, segment_alignment);
    if (program_count > 1U) {
        uint64_t data_file_size = 0ULL;

        if (file_size > data_file_offset) {
            data_file_size = file_size - data_file_offset;
            if (data_file_size > data_size) {
                data_file_size = data_size;
            }
        }
        write_load_header(output, program_header_offset, 1U, PF_R | PF_W, data_file_offset, data_file_size, data_size + bss_size, segment_alignment);
    }
}
