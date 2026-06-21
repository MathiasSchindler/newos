#include "backend_internal.h"

static unsigned int backend_hash_text(const char *text) {
    unsigned int hash = 2166136261U;

    while (text != 0 && *text != '\0') {
        hash ^= (unsigned int)(unsigned char)*text++;
        hash *= 16777619U;
    }
    return hash;
}

static unsigned int backend_hash_pair(const char *left, const char *right) {
    unsigned int hash = backend_hash_text(left);

    hash ^= 0xffU;
    hash *= 16777619U;
    hash ^= backend_hash_text(right);
    hash *= 16777619U;
    return hash;
}

static size_t backend_index_bucket(unsigned int hash, size_t capacity) {
    return (size_t)(hash & (unsigned int)(capacity - 1U));
}

static int find_function_indexed(const BackendState *state, const char *name) {
    size_t bucket = backend_index_bucket(backend_hash_text(name), COMPILER_BACKEND_FUNCTION_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < COMPILER_BACKEND_FUNCTION_INDEX_CAPACITY; ++probe) {
        unsigned int stored = state->function_index[bucket];
        int index;

        if (stored == 0U) {
            return -1;
        }
        index = (int)stored - 1;
        if (index >= 0 && (size_t)index < state->function_count && names_equal(state->functions[index].name, name)) {
            return index;
        }
        bucket = (bucket + 1U) & (COMPILER_BACKEND_FUNCTION_INDEX_CAPACITY - 1U);
    }
    return -1;
}

static void remember_function_index(BackendState *state, const char *name, unsigned int index) {
    size_t bucket = backend_index_bucket(backend_hash_text(name), COMPILER_BACKEND_FUNCTION_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < COMPILER_BACKEND_FUNCTION_INDEX_CAPACITY; ++probe) {
        unsigned int stored = state->function_index[bucket];
        int existing = (int)stored - 1;

        if (stored == 0U || (existing >= 0 && (size_t)existing < state->function_count && names_equal(state->functions[existing].name, name))) {
            state->function_index[bucket] = index + 1U;
            return;
        }
        bucket = (bucket + 1U) & (COMPILER_BACKEND_FUNCTION_INDEX_CAPACITY - 1U);
    }
}

static int find_global_indexed(const BackendState *state, const char *name) {
    size_t bucket = backend_index_bucket(backend_hash_text(name), COMPILER_BACKEND_GLOBAL_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < COMPILER_BACKEND_GLOBAL_INDEX_CAPACITY; ++probe) {
        unsigned int stored = state->global_index[bucket];
        int index;

        if (stored == 0U) {
            return -1;
        }
        index = (int)stored - 1;
        if (index >= 0 && (size_t)index < state->global_count && names_equal(state->globals[index].name, name)) {
            return index;
        }
        bucket = (bucket + 1U) & (COMPILER_BACKEND_GLOBAL_INDEX_CAPACITY - 1U);
    }
    return -1;
}

static void remember_global_index(BackendState *state, const char *name, unsigned int index) {
    size_t bucket = backend_index_bucket(backend_hash_text(name), COMPILER_BACKEND_GLOBAL_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < COMPILER_BACKEND_GLOBAL_INDEX_CAPACITY; ++probe) {
        unsigned int stored = state->global_index[bucket];
        int existing = (int)stored - 1;

        if (stored == 0U || (existing >= 0 && (size_t)existing < state->global_count && names_equal(state->globals[existing].name, name))) {
            state->global_index[bucket] = index + 1U;
            return;
        }
        bucket = (bucket + 1U) & (COMPILER_BACKEND_GLOBAL_INDEX_CAPACITY - 1U);
    }
}

static int find_constant_indexed(const BackendState *state, const char *name) {
    size_t bucket = backend_index_bucket(backend_hash_text(name), COMPILER_BACKEND_CONSTANT_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < COMPILER_BACKEND_CONSTANT_INDEX_CAPACITY; ++probe) {
        unsigned int stored = state->constant_index[bucket];
        int index;

        if (stored == 0U) {
            return -1;
        }
        index = (int)stored - 1;
        if (index >= 0 && (size_t)index < state->constant_count && names_equal(state->constants[index].name, name)) {
            return index;
        }
        bucket = (bucket + 1U) & (COMPILER_BACKEND_CONSTANT_INDEX_CAPACITY - 1U);
    }
    return -1;
}

static void remember_constant_index(BackendState *state, const char *name, unsigned int index) {
    size_t bucket = backend_index_bucket(backend_hash_text(name), COMPILER_BACKEND_CONSTANT_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < COMPILER_BACKEND_CONSTANT_INDEX_CAPACITY; ++probe) {
        unsigned int stored = state->constant_index[bucket];
        int existing = (int)stored - 1;

        if (stored == 0U || (existing >= 0 && (size_t)existing < state->constant_count && names_equal(state->constants[existing].name, name))) {
            state->constant_index[bucket] = index + 1U;
            return;
        }
        bucket = (bucket + 1U) & (COMPILER_BACKEND_CONSTANT_INDEX_CAPACITY - 1U);
    }
}

static int find_aggregate_indexed(const BackendState *state, const char *name) {
    size_t bucket = backend_index_bucket(backend_hash_text(name), COMPILER_BACKEND_AGGREGATE_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < COMPILER_BACKEND_AGGREGATE_INDEX_CAPACITY; ++probe) {
        unsigned int stored = state->aggregate_index[bucket];
        int index;

        if (stored == 0U) {
            return -1;
        }
        index = (int)stored - 1;
        if (index >= 0 && (size_t)index < state->aggregate_count && names_equal(state->aggregates[index].name, name)) {
            return index;
        }
        bucket = (bucket + 1U) & (COMPILER_BACKEND_AGGREGATE_INDEX_CAPACITY - 1U);
    }
    return -1;
}

static void remember_aggregate_index(BackendState *state, const char *name, unsigned int index) {
    size_t bucket = backend_index_bucket(backend_hash_text(name), COMPILER_BACKEND_AGGREGATE_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < COMPILER_BACKEND_AGGREGATE_INDEX_CAPACITY; ++probe) {
        unsigned int stored = state->aggregate_index[bucket];
        int existing = (int)stored - 1;

        if (stored == 0U || (existing >= 0 && (size_t)existing < state->aggregate_count && names_equal(state->aggregates[existing].name, name))) {
            state->aggregate_index[bucket] = index + 1U;
            return;
        }
        bucket = (bucket + 1U) & (COMPILER_BACKEND_AGGREGATE_INDEX_CAPACITY - 1U);
    }
}

static int find_local_indexed(const BackendState *state, const char *name) {
    size_t bucket = backend_index_bucket(backend_hash_text(name), COMPILER_BACKEND_LOCAL_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < COMPILER_BACKEND_LOCAL_INDEX_CAPACITY; ++probe) {
        unsigned int stored = state->local_index[bucket];
        int index;

        if (stored == 0U) {
            return -1;
        }
        index = (int)stored - 1;
        if (index >= 0 && (size_t)index < state->local_count && names_equal(state->locals[index].name, name)) {
            return index;
        }
        bucket = (bucket + 1U) & (COMPILER_BACKEND_LOCAL_INDEX_CAPACITY - 1U);
    }
    return -1;
}

static void remember_local_index(BackendState *state, const char *name, unsigned int index) {
    size_t bucket = backend_index_bucket(backend_hash_text(name), COMPILER_BACKEND_LOCAL_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < COMPILER_BACKEND_LOCAL_INDEX_CAPACITY; ++probe) {
        unsigned int stored = state->local_index[bucket];
        int existing = (int)stored - 1;

        if (stored == 0U || (existing >= 0 && (size_t)existing < state->local_count && names_equal(state->locals[existing].name, name))) {
            state->local_index[bucket] = index + 1U;
            return;
        }
        bucket = (bucket + 1U) & (COMPILER_BACKEND_LOCAL_INDEX_CAPACITY - 1U);
    }
}

static int find_string_indexed(const BackendState *state, const char *text) {
    size_t bucket = backend_index_bucket(backend_hash_text(text), COMPILER_BACKEND_STRING_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < COMPILER_BACKEND_STRING_INDEX_CAPACITY; ++probe) {
        unsigned int stored = state->string_index[bucket];
        int index;

        if (stored == 0U) {
            return -1;
        }
        index = (int)stored - 1;
        if (index >= 0 && (size_t)index < state->string_count && names_equal(state->strings[index].text, text)) {
            return index;
        }
        bucket = (bucket + 1U) & (COMPILER_BACKEND_STRING_INDEX_CAPACITY - 1U);
    }
    return -1;
}

static void remember_string_index(BackendState *state, const char *text, unsigned int index) {
    size_t bucket = backend_index_bucket(backend_hash_text(text), COMPILER_BACKEND_STRING_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < COMPILER_BACKEND_STRING_INDEX_CAPACITY; ++probe) {
        unsigned int stored = state->string_index[bucket];
        int existing = (int)stored - 1;

        if (stored == 0U || (existing >= 0 && (size_t)existing < state->string_count && names_equal(state->strings[existing].text, text))) {
            state->string_index[bucket] = index + 1U;
            return;
        }
        bucket = (bucket + 1U) & (COMPILER_BACKEND_STRING_INDEX_CAPACITY - 1U);
    }
}

static int find_aggregate_member_indexed(const BackendState *state, const char *aggregate_name, const char *member_name) {
    size_t bucket = backend_index_bucket(backend_hash_pair(aggregate_name, member_name), COMPILER_BACKEND_AGGREGATE_MEMBER_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < COMPILER_BACKEND_AGGREGATE_MEMBER_INDEX_CAPACITY; ++probe) {
        unsigned int stored = state->aggregate_member_index[bucket];
        int index;

        if (stored == 0U) {
            return -1;
        }
        index = (int)stored - 1;
        if (index >= 0 && (size_t)index < state->aggregate_member_count &&
            names_equal(state->aggregate_members[index].aggregate_name, aggregate_name) &&
            names_equal(state->aggregate_members[index].name, member_name)) {
            return index;
        }
        bucket = (bucket + 1U) & (COMPILER_BACKEND_AGGREGATE_MEMBER_INDEX_CAPACITY - 1U);
    }
    return -1;
}

static void remember_aggregate_member_index(BackendState *state, const char *aggregate_name, const char *member_name, unsigned int index) {
    size_t bucket = backend_index_bucket(backend_hash_pair(aggregate_name, member_name), COMPILER_BACKEND_AGGREGATE_MEMBER_INDEX_CAPACITY);
    size_t probe;

    for (probe = 0; probe < COMPILER_BACKEND_AGGREGATE_MEMBER_INDEX_CAPACITY; ++probe) {
        unsigned int stored = state->aggregate_member_index[bucket];
        int existing = (int)stored - 1;

        if (stored == 0U ||
            (existing >= 0 && (size_t)existing < state->aggregate_member_count &&
             names_equal(state->aggregate_members[existing].aggregate_name, aggregate_name) &&
             names_equal(state->aggregate_members[existing].name, member_name))) {
            state->aggregate_member_index[bucket] = index + 1U;
            return;
        }
        bucket = (bucket + 1U) & (COMPILER_BACKEND_AGGREGATE_MEMBER_INDEX_CAPACITY - 1U);
    }
}

static int find_function_index(const BackendState *state, const char *name) {
    int indexed = find_function_indexed(state, name);
    size_t i;

    if (indexed >= 0) {
        return indexed;
    }
    for (i = 0; i < state->function_count; ++i) {
        if (names_equal(state->functions[i].name, name)) {
            return (int)i;
        }
    }

    return -1;
}

static int function_type_returns_object(const char *type_text) {
    const char *type = skip_spaces(type_text != 0 ? type_text : "");

    return !text_contains(type, "*") &&
           (starts_with(type, "struct") || starts_with(type, "union"));
}

static int copy_aggregate_name_from_type(const char *type_text, char *buffer, size_t buffer_size) {
    const char *cursor;
    size_t out = 0;

    if (buffer_size == 0 || type_text == 0) {
        return -1;
    }
    buffer[0] = '\0';
    type_text = skip_spaces(type_text);
    if (starts_with(type_text, "struct:")) {
        cursor = type_text + 7;
    } else if (starts_with(type_text, "union:")) {
        cursor = type_text + 6;
    } else {
        return -1;
    }

    while (*cursor != '\0' && *cursor != '[' && *cursor != '*' && *cursor != ' ' && out + 1 < buffer_size) {
        buffer[out++] = *cursor++;
    }
    buffer[out] = '\0';
    return out > 0 ? 0 : -1;
}

static int find_aggregate_index(const BackendState *state, const char *name) {
    int indexed = find_aggregate_indexed(state, name);
    size_t i;

    if (indexed >= 0) {
        return indexed;
    }
    for (i = 0; i < state->aggregate_count; ++i) {
        if (names_equal(state->aggregates[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

int add_function_name(BackendState *state, const char *name, int global, const char *return_type) {
    int existing = find_function_index(state, name);

    if (existing >= 0) {
        if (global) {
            state->functions[existing].global = 1;
        }
        if (return_type != 0 && return_type[0] != '\0') {
            rt_copy_string(state->functions[existing].return_type,
                           sizeof(state->functions[existing].return_type),
                           return_type);
            state->functions[existing].returns_object = function_type_returns_object(return_type);
        }
        return 0;
    }

    if (state->function_count >= COMPILER_BACKEND_MAX_FUNCTIONS) {
        backend_set_error(state->backend, "too many functions for backend");
        return -1;
    }

    rt_copy_string(state->functions[state->function_count].name, sizeof(state->functions[state->function_count].name), name);
    rt_copy_string(state->functions[state->function_count].return_type,
                   sizeof(state->functions[state->function_count].return_type),
                   return_type != 0 ? return_type : "");
    state->functions[state->function_count].global = global ? 1 : 0;
    state->functions[state->function_count].stack_bytes = 0;
    state->functions[state->function_count].returns_object = function_type_returns_object(return_type);
    state->functions[state->function_count].needs_callret = 0;
    state->functions[state->function_count].callret_bytes = 0;
    state->functions[state->function_count].has_call = 0;
    state->functions[state->function_count].unused_param_mask = 0;
    state->functions[state->function_count].cached_param_mask = 0;
    state->functions[state->function_count].cached_local_mask = 0;
    state->functions[state->function_count].cached_param_count = 0;
    state->functions[state->function_count].cached_local_count = 0;
    remember_function_index(state, name, (unsigned int)state->function_count);
    state->function_count += 1U;
    return 0;
}

int should_prefer_word_index(const char *name, const char *type_text) {
    if (names_equal(name, "argv") || names_equal(name, "envp")) {
        return 1;
    }
    if (text_contains(type_text, "[") && text_contains(type_text, "*")) {
        return 1;
    }
    if (text_contains(type_text, "**")) {
        return 1;
    }
    if (text_contains(type_text, "*") && !text_contains(type_text, "char")) {
        return 1;
    }
    return 0;
}

int is_function_name(const BackendState *state, const char *name) {
    return find_function_index(state, name) >= 0;
}

int function_returns_object(const BackendState *state, const char *name) {
    int index = find_function_index(state, name);
    return index >= 0 ? state->functions[index].returns_object : 0;
}

const char *function_return_type(const BackendState *state, const char *name) {
    int index = find_function_index(state, name);
    return index >= 0 ? state->functions[index].return_type : "";
}

int find_global(const BackendState *state, const char *name) {
    int indexed = find_global_indexed(state, name);
    size_t i;

    if (indexed >= 0) {
        return indexed;
    }
    for (i = 0; i < state->global_count; ++i) {
        if (names_equal(state->globals[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

int find_constant(const BackendState *state, const char *name) {
    int indexed = find_constant_indexed(state, name);
    size_t i;

    if (indexed >= 0) {
        return indexed;
    }
    for (i = 0; i < state->constant_count; ++i) {
        if (names_equal(state->constants[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

int add_constant(BackendState *state, const char *name, long long value) {
    int existing = find_constant(state, name);

    if (existing >= 0) {
        state->constants[existing].value = value;
        return 0;
    }

    if (state->constant_count >= COMPILER_BACKEND_MAX_CONSTANTS) {
        backend_set_error(state->backend, "too many constants for backend");
        return -1;
    }

    rt_copy_string(state->constants[state->constant_count].name, sizeof(state->constants[state->constant_count].name), name);
    state->constants[state->constant_count].value = value;
    remember_constant_index(state, name, (unsigned int)state->constant_count);
    state->constant_count += 1U;
    return 0;
}

int add_aggregate_layout(BackendState *state, const char *name, int is_union, int size_bytes, int align_bytes) {
    int existing;

    if (name == 0 || name[0] == '\0') {
        return -1;
    }

    existing = find_aggregate_index(state, name);
    if (existing >= 0) {
        if (size_bytes > 0) {
            state->aggregates[existing].size_bytes = size_bytes;
        }
        if (align_bytes > 0) {
            state->aggregates[existing].align_bytes = align_bytes;
        }
        state->aggregates[existing].is_union = is_union ? 1 : 0;
        return existing;
    }

    if (state->aggregate_count >= COMPILER_BACKEND_MAX_AGGREGATES) {
        backend_set_error(state->backend, "too many aggregate layouts for backend");
        return -1;
    }

    rt_copy_string(state->aggregates[state->aggregate_count].name,
                   sizeof(state->aggregates[state->aggregate_count].name),
                   name);
    state->aggregates[state->aggregate_count].is_union = is_union ? 1 : 0;
    state->aggregates[state->aggregate_count].size_bytes = size_bytes;
    state->aggregates[state->aggregate_count].align_bytes = align_bytes;
    remember_aggregate_index(state, name, (unsigned int)state->aggregate_count);
    state->aggregate_count += 1U;
    return (int)(state->aggregate_count - 1U);
}

int add_aggregate_member(BackendState *state, const char *aggregate_name, const char *name, const char *type_text, int offset_bytes) {
    int existing;
    size_t i;

    if (aggregate_name == 0 || aggregate_name[0] == '\0' || name == 0 || name[0] == '\0') {
        return -1;
    }

    existing = find_aggregate_member_indexed(state, aggregate_name, name);
    if (existing >= 0) {
        state->aggregate_members[existing].offset_bytes = offset_bytes;
        if (type_text != 0 && type_text[0] != '\0') {
            rt_copy_string(state->aggregate_members[existing].type_text,
                           sizeof(state->aggregate_members[existing].type_text),
                           type_text);
        }
        return existing;
    }

    for (i = 0; i < state->aggregate_member_count; ++i) {
        if (names_equal(state->aggregate_members[i].aggregate_name, aggregate_name) &&
            names_equal(state->aggregate_members[i].name, name)) {
            state->aggregate_members[i].offset_bytes = offset_bytes;
            if (type_text != 0 && type_text[0] != '\0') {
                rt_copy_string(state->aggregate_members[i].type_text,
                               sizeof(state->aggregate_members[i].type_text),
                               type_text);
            }
            return (int)i;
        }
    }

    if (state->aggregate_member_count >= COMPILER_BACKEND_MAX_AGGREGATE_MEMBERS) {
        backend_set_error(state->backend, "too many aggregate members for backend");
        return -1;
    }

    rt_copy_string(state->aggregate_members[state->aggregate_member_count].aggregate_name,
                   sizeof(state->aggregate_members[state->aggregate_member_count].aggregate_name),
                   aggregate_name);
    rt_copy_string(state->aggregate_members[state->aggregate_member_count].name,
                   sizeof(state->aggregate_members[state->aggregate_member_count].name),
                   name);
    rt_copy_string(state->aggregate_members[state->aggregate_member_count].type_text,
                   sizeof(state->aggregate_members[state->aggregate_member_count].type_text),
                   type_text != 0 ? type_text : "");
    state->aggregate_members[state->aggregate_member_count].offset_bytes = offset_bytes;
    remember_aggregate_member_index(state, aggregate_name, name, (unsigned int)state->aggregate_member_count);
    state->aggregate_member_count += 1U;
    return (int)(state->aggregate_member_count - 1U);
}

int lookup_aggregate_size(const BackendState *state, const char *type_text) {
    char name[COMPILER_IR_NAME_CAPACITY];
    int index;

    if (copy_aggregate_name_from_type(type_text, name, sizeof(name)) != 0) {
        return 0;
    }
    index = find_aggregate_index(state, name);
    if (index < 0) {
        return 0;
    }
    return state->aggregates[index].size_bytes;
}

int lookup_aggregate_member(const BackendState *state,
                            const char *base_type,
                            const char *member_name,
                            int *offset_out,
                            const char **type_text_out) {
    char aggregate_name[COMPILER_IR_NAME_CAPACITY];
    int indexed;
    size_t i;

    if (copy_aggregate_name_from_type(base_type, aggregate_name, sizeof(aggregate_name)) != 0) {
        return -1;
    }

    indexed = find_aggregate_member_indexed(state, aggregate_name, member_name);
    if (indexed >= 0) {
        if (offset_out != 0) {
            *offset_out = state->aggregate_members[indexed].offset_bytes;
        }
        if (type_text_out != 0) {
            *type_text_out = state->aggregate_members[indexed].type_text;
        }
        return 0;
    }

    for (i = 0; i < state->aggregate_member_count; ++i) {
        if (names_equal(state->aggregate_members[i].aggregate_name, aggregate_name) &&
            names_equal(state->aggregate_members[i].name, member_name)) {
            if (offset_out != 0) {
                *offset_out = state->aggregate_members[i].offset_bytes;
            }
            if (type_text_out != 0) {
                *type_text_out = state->aggregate_members[i].type_text;
            }
            return 0;
        }
    }

    return -1;
}

int add_global(
    BackendState *state,
    const char *name,
    const char *type_text,
    int is_array,
    int pointer_depth,
    int char_based,
    int prefers_word_index,
    int global,
    int has_storage
) {
    int existing = find_global(state, name);

    if (existing >= 0) {
        if (is_array) {
            state->globals[existing].is_array = 1;
        }
        if (type_text != 0 && type_text[0] != '\0') {
            rt_copy_string(state->globals[existing].type_text, sizeof(state->globals[existing].type_text), type_text);
        }
        if (prefers_word_index) {
            state->globals[existing].prefers_word_index = 1;
        }
        if (global) {
            state->globals[existing].global = 1;
        }
        if (has_storage) {
            state->globals[existing].has_storage = 1;
        }
        return existing;
    }

    if (state->global_count >= COMPILER_BACKEND_MAX_GLOBALS) {
        backend_set_error(state->backend, "too many globals for backend");
        return -1;
    }

    rt_copy_string(state->globals[state->global_count].name, sizeof(state->globals[state->global_count].name), name);
    rt_copy_string(state->globals[state->global_count].type_text, sizeof(state->globals[state->global_count].type_text), type_text != 0 ? type_text : "");
    state->globals[state->global_count].init_text[0] = '\0';
    state->globals[state->global_count].init_value = 0;
    state->globals[state->global_count].initialized = 0;
    state->globals[state->global_count].is_array = is_array;
    state->globals[state->global_count].pointer_depth = pointer_depth;
    state->globals[state->global_count].char_based = char_based;
    state->globals[state->global_count].prefers_word_index = prefers_word_index;
    state->globals[state->global_count].global = global ? 1 : 0;
    state->globals[state->global_count].has_storage = has_storage ? 1 : 0;
    remember_global_index(state, name, (unsigned int)state->global_count);
    state->global_count += 1U;
    return (int)(state->global_count - 1U);
}

int find_local(const BackendState *state, const char *name) {
    int indexed = find_local_indexed(state, name);
    size_t i = state->local_count;

    if (indexed >= 0) {
        return indexed;
    }
    while (i > 0) {
        i -= 1U;
        if (names_equal(state->locals[i].name, name)) {
            return (int)i;
        }
    }
    return -1;
}

void reset_local_index(BackendState *state) {
    rt_memset(state->local_index, 0, sizeof(state->local_index));
}

void rebuild_local_index(BackendState *state) {
    size_t i;

    reset_local_index(state);
    for (i = 0; i < state->local_count; ++i) {
        remember_local_index(state, state->locals[i].name, (unsigned int)i);
    }
}

int allocate_local(BackendState *state, const char *name, const char *type_text, int stack_bytes, int is_array, int pointer_depth, int char_based, int prefers_word_index) {
    int slot_size = stack_bytes > 0 ? stack_bytes : (is_array ? BACKEND_ARRAY_STACK_BYTES : backend_stack_slot_size(state));

    if (state->local_count >= COMPILER_BACKEND_MAX_LOCALS) {
        backend_set_error(state->backend, "too many local variables for backend");
        return -1;
    }

    rt_copy_string(state->locals[state->local_count].name, sizeof(state->locals[state->local_count].name), name);
    rt_copy_string(state->locals[state->local_count].type_text, sizeof(state->locals[state->local_count].type_text), type_text != 0 ? type_text : "");
    state->locals[state->local_count].symbol_name[0] = '\0';
    state->locals[state->local_count].stack_bytes = slot_size;
    state->locals[state->local_count].offset = state->stack_size + slot_size;
    state->locals[state->local_count].is_array = is_array;
    state->locals[state->local_count].pointer_depth = pointer_depth;
    state->locals[state->local_count].char_based = char_based;
    state->locals[state->local_count].prefers_word_index = prefers_word_index;
    state->locals[state->local_count].static_storage = 0;
    state->locals[state->local_count].cached_register = -1;
    state->locals[state->local_count].indirect_object = 0;
    state->stack_size += slot_size;
    remember_local_index(state, name, (unsigned int)state->local_count);
    state->local_count += 1U;

    if (state->reserved_stack_size > 0 && state->stack_size > state->reserved_stack_size) {
        backend_set_error(state->backend, "local stack reservation mismatch");
        return -1;
    }

    return 0;
}

int allocate_indirect_object_local(BackendState *state,
                                   const char *name,
                                   const char *type_text,
                                   int object_bytes,
                                   int char_based,
                                   int prefers_word_index) {
    int slot_size = backend_stack_slot_size(state);

    if (state->local_count >= COMPILER_BACKEND_MAX_LOCALS) {
        backend_set_error(state->backend, "too many local variables for backend");
        return -1;
    }

    rt_copy_string(state->locals[state->local_count].name, sizeof(state->locals[state->local_count].name), name);
    rt_copy_string(state->locals[state->local_count].type_text, sizeof(state->locals[state->local_count].type_text), type_text != 0 ? type_text : "");
    state->locals[state->local_count].symbol_name[0] = '\0';
    state->locals[state->local_count].stack_bytes = object_bytes > 0 ? object_bytes : BACKEND_STRUCT_STACK_BYTES;
    state->locals[state->local_count].offset = state->stack_size + slot_size;
    state->locals[state->local_count].is_array = 1;
    state->locals[state->local_count].pointer_depth = 0;
    state->locals[state->local_count].char_based = char_based;
    state->locals[state->local_count].prefers_word_index = prefers_word_index;
    state->locals[state->local_count].static_storage = 0;
    state->locals[state->local_count].cached_register = -1;
    state->locals[state->local_count].indirect_object = 1;
    state->stack_size += slot_size;
    remember_local_index(state, name, (unsigned int)state->local_count);
    state->local_count += 1U;

    if (state->reserved_stack_size > 0 && state->stack_size > state->reserved_stack_size) {
        backend_set_error(state->backend, "local stack reservation mismatch");
        return -1;
    }

    return 0;
}

int allocate_cached_indirect_object_local(BackendState *state,
                                          const char *name,
                                          const char *type_text,
                                          int object_bytes,
                                          int char_based,
                                          int prefers_word_index,
                                          int cached_register) {
    if (state->local_count >= COMPILER_BACKEND_MAX_LOCALS) {
        backend_set_error(state->backend, "too many local variables for backend");
        return -1;
    }

    rt_copy_string(state->locals[state->local_count].name, sizeof(state->locals[state->local_count].name), name);
    rt_copy_string(state->locals[state->local_count].type_text, sizeof(state->locals[state->local_count].type_text), type_text != 0 ? type_text : "");
    state->locals[state->local_count].symbol_name[0] = '\0';
    state->locals[state->local_count].offset = 0;
    state->locals[state->local_count].stack_bytes = object_bytes > 0 ? object_bytes : BACKEND_STRUCT_STACK_BYTES;
    state->locals[state->local_count].is_array = 1;
    state->locals[state->local_count].pointer_depth = 0;
    state->locals[state->local_count].char_based = char_based;
    state->locals[state->local_count].prefers_word_index = prefers_word_index;
    state->locals[state->local_count].static_storage = 0;
    state->locals[state->local_count].cached_register = cached_register;
    state->locals[state->local_count].indirect_object = 1;
    remember_local_index(state, name, (unsigned int)state->local_count);
    state->local_count += 1U;
    return 0;
}

int allocate_cached_local(BackendState *state, const char *name, const char *type_text, int pointer_depth, int char_based, int prefers_word_index, int cached_register) {
    if (state->local_count >= COMPILER_BACKEND_MAX_LOCALS) {
        backend_set_error(state->backend, "too many local variables for backend");
        return -1;
    }

    rt_copy_string(state->locals[state->local_count].name, sizeof(state->locals[state->local_count].name), name);
    rt_copy_string(state->locals[state->local_count].type_text, sizeof(state->locals[state->local_count].type_text), type_text != 0 ? type_text : "");
    state->locals[state->local_count].symbol_name[0] = '\0';
    state->locals[state->local_count].stack_bytes = 0;
    state->locals[state->local_count].offset = 0;
    state->locals[state->local_count].is_array = 0;
    state->locals[state->local_count].pointer_depth = pointer_depth;
    state->locals[state->local_count].char_based = char_based;
    state->locals[state->local_count].prefers_word_index = prefers_word_index;
    state->locals[state->local_count].static_storage = 0;
    state->locals[state->local_count].cached_register = cached_register;
    state->locals[state->local_count].indirect_object = 0;
    remember_local_index(state, name, (unsigned int)state->local_count);
    state->local_count += 1U;
    return 0;
}

int allocate_static_local(BackendState *state, const char *name, const char *symbol_name, const char *type_text, int storage_bytes, int is_array, int pointer_depth, int char_based, int prefers_word_index) {
    int slot_size = storage_bytes > 0 ? storage_bytes : (is_array ? BACKEND_ARRAY_STACK_BYTES : backend_stack_slot_size(state));

    if (state->local_count >= COMPILER_BACKEND_MAX_LOCALS) {
        backend_set_error(state->backend, "too many local variables for backend");
        return -1;
    }

    rt_copy_string(state->locals[state->local_count].name, sizeof(state->locals[state->local_count].name), name);
    rt_copy_string(state->locals[state->local_count].type_text, sizeof(state->locals[state->local_count].type_text), type_text != 0 ? type_text : "");
    rt_copy_string(state->locals[state->local_count].symbol_name, sizeof(state->locals[state->local_count].symbol_name), symbol_name != 0 ? symbol_name : name);
    state->locals[state->local_count].stack_bytes = slot_size;
    state->locals[state->local_count].offset = 0;
    state->locals[state->local_count].is_array = is_array;
    state->locals[state->local_count].pointer_depth = pointer_depth;
    state->locals[state->local_count].char_based = char_based;
    state->locals[state->local_count].prefers_word_index = prefers_word_index;
    state->locals[state->local_count].static_storage = 1;
    state->locals[state->local_count].cached_register = -1;
    state->locals[state->local_count].indirect_object = 0;
    remember_local_index(state, name, (unsigned int)state->local_count);
    state->local_count += 1U;
    return 0;
}

const char *lookup_name_type_text(const BackendState *state, const char *name) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index >= 0) {
        return state->locals[local_index].type_text;
    }
    if (global_index >= 0) {
        return state->globals[global_index].type_text;
    }
    return "";
}

int find_string_literal(const BackendState *state, const char *text) {
    int indexed = find_string_indexed(state, text);
    size_t i;

    if (indexed >= 0) {
        return indexed;
    }
    for (i = 0; i < state->string_count; ++i) {
        if (names_equal(state->strings[i].text, text)) {
            return (int)i;
        }
    }
    return -1;
}

int add_string_literal(BackendState *state, const char *text) {
    return add_string_literal_bytes(state, text, rt_strlen(text != 0 ? text : ""));
}

int add_string_literal_bytes(BackendState *state, const char *text, size_t length) {
    char digits[32];
    unsigned int index;
    int existing = -1;
    size_t i;

    text = text != 0 ? text : "";
    if (length + 1U > sizeof(state->strings[0].text)) {
        backend_set_error(state->backend, "string literal too large for backend");
        return -1;
    }
    if (length == rt_strlen(text)) {
        existing = find_string_literal(state, text);
        if (existing >= 0) {
            return existing;
        }
    } else {
        for (i = 0; i < state->string_count; ++i) {
            if (state->strings[i].length == length && memcmp(state->strings[i].text, text, length) == 0) {
                return (int)i;
            }
        }
    }
    if (state->string_count >= COMPILER_BACKEND_MAX_STRINGS) {
        backend_set_error(state->backend, "too many string literals for backend");
        return -1;
    }

    index = (unsigned int)state->string_count;
    rt_copy_string(state->strings[index].label, sizeof(state->strings[index].label), "str");
    rt_unsigned_to_string((unsigned long long)index, digits, sizeof(digits));
    rt_copy_string(state->strings[index].label + 3U,
                   sizeof(state->strings[index].label) - 3U,
                   digits);
    memcpy(state->strings[index].text, text, length);
    state->strings[index].text[length] = '\0';
    state->strings[index].length = length;
    if (length == rt_strlen(text)) {
        remember_string_index(state, text, index);
    }
    state->string_count += 1U;
    return (int)index;
}
