#include "backend_internal.h"

void backend_set_error(CompilerBackend *backend, const char *message) {
    rt_copy_string(backend->error_message, sizeof(backend->error_message), message != 0 ? message : "backend error");
}

void backend_set_error_with_line(CompilerBackend *backend, const char *message, const char *line) {
    char buffer[COMPILER_ERROR_CAPACITY];
    size_t used;
    size_t i = 0;

    rt_copy_string(buffer, sizeof(buffer), message != 0 ? message : "backend error");
    used = rt_strlen(buffer);
    rt_copy_string(buffer + used, sizeof(buffer) - used, " near `");
    used = rt_strlen(buffer);
    while (line != 0 && line[i] != '\0' && line[i] != '\n' && used + 4 < sizeof(buffer)) {
        buffer[used++] = line[i++];
    }
    if (line != 0 && line[i] != '\0' && used + 4 < sizeof(buffer)) {
        buffer[used++] = '.';
        buffer[used++] = '.';
        buffer[used++] = '.';
    }
    buffer[used++] = '`';
    buffer[used] = '\0';
    backend_set_error(backend, buffer);
}

int emit_text(BackendState *state, const char *text) {
    return rt_write_cstr(state->fd, text);
}

int emit_line(BackendState *state, const char *text) {
    return rt_write_line(state->fd, text);
}

int emit_instruction(BackendState *state, const char *text) {
    if (emit_text(state, "    ") != 0 || emit_line(state, text) != 0) {
        backend_set_error(state->backend, "failed to write assembly output");
        return -1;
    }
    return 0;
}

int names_equal(const char *lhs, const char *rhs) {
    return rt_strcmp(lhs, rhs) == 0;
}

int text_contains(const char *text, const char *needle) {
    size_t i;
    size_t needle_length = rt_strlen(needle);

    if (needle_length == 0U) {
        return 1;
    }
    for (i = 0; text[i] != '\0'; ++i) {
        size_t j = 0;
        while (needle[j] != '\0' && text[i + j] == needle[j]) {
            j += 1U;
        }
        if (j == needle_length) {
            return 1;
        }
    }
    return 0;
}

int starts_with(const char *text, const char *prefix) {
    size_t i = 0;
    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }
    return 1;
}

int name_looks_like_macro_constant(const char *name) {
    size_t i = 0;
    int saw_alpha = 0;

    if (name == 0 || name[0] == '\0') {
        return 0;
    }

    while (name[i] != '\0') {
        char ch = name[i];
        if (ch >= 'a' && ch <= 'z') {
            return 0;
        }
        if ((ch >= 'A' && ch <= 'Z') || ch == '_') {
            saw_alpha = 1;
        } else if (ch < '0' || ch > '9') {
            return 0;
        }
        i += 1U;
    }

    return saw_alpha;
}

const char *skip_spaces(const char *text) {
    while (*text == ' ' || *text == '\t') {
        text += 1;
    }
    return text;
}

int backend_is_aarch64(const BackendState *state) {
    return compiler_target_is_aarch64(state->backend->target);
}

int backend_is_darwin(const BackendState *state) {
    return compiler_target_is_darwin(state->backend->target);
}

const char *backend_private_label_prefix(const BackendState *state) {
    if (state != 0 &&
        state->backend != 0 &&
        backend_is_darwin(state) &&
        (state->backend->function_sections || state->backend->data_sections)) {
        return "L";
    }
    return ".L";
}

int backend_stack_slot_size(const BackendState *state) {
    const CompilerTargetInfo *info = compiler_target_get_info(state->backend->target);
    return info != 0 ? (int)info->stack_slot_size : 8;
}

int backend_register_arg_limit(const BackendState *state) {
    const CompilerTargetInfo *info = compiler_target_get_info(state->backend->target);
    return info != 0 ? (int)info->register_arg_limit : 6;
}

void format_symbol_name(const BackendState *state, const char *name, char *buffer, size_t buffer_size) {
    const CompilerTargetInfo *info = compiler_target_get_info(state->backend->target);
    const char *prefix = (info != 0 && info->global_symbol_prefix != 0) ? info->global_symbol_prefix : "";

    rt_copy_string(buffer, buffer_size, prefix);
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), name);
    if (prefix[0] == '\0') {
        rt_copy_string(buffer, buffer_size, name);
    }
}

void build_static_local_symbol_name(const BackendState *state, const char *function_name, const char *name, char *buffer, size_t buffer_size) {
    (void)state;
    rt_copy_string(buffer, buffer_size, "__static_");
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), function_name != 0 && function_name[0] != '\0' ? function_name : "global");
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "_");
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), name != 0 ? name : "obj");
}

const char *copy_next_word(const char *cursor, char *buffer, size_t buffer_size) {
    size_t length = 0;

    while (*cursor != '\0' && *cursor != ' ' && length + 1 < buffer_size) {
        buffer[length++] = *cursor++;
    }
    buffer[length] = '\0';
    return skip_spaces(cursor);
}

void copy_last_word(const char *text, char *buffer, size_t buffer_size) {
    size_t start = 0;
    size_t end = rt_strlen(text);
    size_t i;
    size_t out = 0;

    while (end > 0 && text[end - 1] == ' ') {
        end -= 1U;
    }
    for (i = end; i > 0; --i) {
        if (text[i - 1] == ' ') {
            start = i;
            break;
        }
    }

    for (i = start; i < end && out + 1 < buffer_size; ++i) {
        buffer[out++] = text[i];
    }
    buffer[out] = '\0';
}

int parse_signed_value(const char *text, long long *value_out) {
    int negative = 0;
    unsigned long long magnitude = 0;
    unsigned int base = 10;
    int saw_digit = 0;

    text = skip_spaces(text);
    if (*text == '-') {
        negative = 1;
        text += 1;
    } else if (*text == '+') {
        text += 1;
    }

    if (text[0] == '0') {
        saw_digit = 1;
        if (text[1] == 'x' || text[1] == 'X') {
            base = 16;
            saw_digit = 0;
            text += 2;
        } else if (text[1] >= '0' && text[1] <= '7') {
            base = 8;
            text += 1;
        }
    }

    while (*text != '\0') {
        unsigned int digit = 0;
        if (*text >= '0' && *text <= '9') {
            digit = (unsigned int)(*text - '0');
        } else if (*text >= 'a' && *text <= 'f') {
            digit = 10U + (unsigned int)(*text - 'a');
        } else if (*text >= 'A' && *text <= 'F') {
            digit = 10U + (unsigned int)(*text - 'A');
        } else if (*text == 'u' || *text == 'U' || *text == 'l' || *text == 'L') {
            text += 1;
            continue;
        } else {
            return -1;
        }

        if (digit >= base) {
            return -1;
        }
        magnitude = magnitude * (unsigned long long)base + (unsigned long long)digit;
        saw_digit = 1;
        text += 1;
    }

    if (!saw_digit) {
        return -1;
    }

    *value_out = negative ? -(long long)magnitude : (long long)magnitude;
    return 0;
}

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

static int lookup_builtin_macro_value(const char *name, long long *value_out) {
    if (name == 0 || value_out == 0) {
        return -1;
    }

    if (names_equal(name, "ULLONG_MAX") || names_equal(name, "ULONG_MAX") ||
        names_equal(name, "SIZE_MAX") || names_equal(name, "UINTPTR_MAX")) {
        *value_out = -1LL;
        return 0;
    }
    if (names_equal(name, "UINT_MAX")) {
        *value_out = 4294967295LL;
        return 0;
    }
    if (names_equal(name, "USHRT_MAX")) {
        *value_out = 65535LL;
        return 0;
    }
    if (names_equal(name, "UCHAR_MAX")) {
        *value_out = 255LL;
        return 0;
    }
    if (names_equal(name, "LLONG_MAX") || names_equal(name, "LONG_MAX")) {
        *value_out = 0x7fffffffffffffffLL;
        return 0;
    }
    if (names_equal(name, "INT_MAX")) {
        *value_out = 2147483647LL;
        return 0;
    }
    if (names_equal(name, "SHRT_MAX")) {
        *value_out = 32767LL;
        return 0;
    }
    if (names_equal(name, "LLONG_MIN") || names_equal(name, "LONG_MIN")) {
        *value_out = (-9223372036854775807LL - 1LL);
        return 0;
    }
    if (names_equal(name, "INT_MIN")) {
        *value_out = -2147483647LL - 1LL;
        return 0;
    }
    if (names_equal(name, "SHRT_MIN")) {
        *value_out = -32768LL;
        return 0;
    }
    if (names_equal(name, "CHAR_BIT")) {
        *value_out = 8LL;
        return 0;
    }
    return -1;
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
    state->stack_size += slot_size;
    remember_local_index(state, name, (unsigned int)state->local_count);
    state->local_count += 1U;

    if (state->reserved_stack_size > 0 && state->stack_size > state->reserved_stack_size) {
        backend_set_error(state->backend, "local stack reservation mismatch");
        return -1;
    }

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

int write_label_name(const BackendState *state, char *buffer, size_t buffer_size, const char *label) {
    rt_copy_string(buffer, buffer_size, backend_private_label_prefix(state));
    if (state != 0 && state->current_function[0] != '\0') {
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), state->current_function);
        rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), "_");
    }
    rt_copy_string(buffer + rt_strlen(buffer), buffer_size - rt_strlen(buffer), label);
    return 0;
}

int emit_pop_to_register(BackendState *state, const char *reg) {
    char line[64];
    if (backend_is_aarch64(state)) {
        rt_copy_string(line, sizeof(line), "ldr ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", [sp]");
        return emit_instruction(state, line) == 0 &&
               emit_instruction(state, "add sp, sp, #16") == 0 ? 0 : -1;
    }
    rt_copy_string(line, sizeof(line), "popq ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
    return emit_instruction(state, line);
}

int emit_local_address(BackendState *state, int offset, const char *reg) {
    char line[128];
    char offset_text[32];

    rt_unsigned_to_string((unsigned long long)offset, offset_text, sizeof(offset_text));
    if (backend_is_aarch64(state)) {
        if (offset <= 4095) {
            rt_copy_string(line, sizeof(line), "sub ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", x29, #");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
            return emit_instruction(state, line);
        }
        if (emit_load_immediate_register(state, names_equal(reg, "x9") ? "x10" : "x9", offset) != 0) {
            return -1;
        }
        rt_copy_string(line, sizeof(line), "sub ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", x29, ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), names_equal(reg, "x9") ? "x10" : "x9");
        return emit_instruction(state, line);
    }

    rt_copy_string(line, sizeof(line), "leaq -");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rbp), ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
    return emit_instruction(state, line);
}

static int emit_aarch64_offset_address(BackendState *state,
                                       const char *dst_reg,
                                       const char *base_reg,
                                       int offset,
                                       const char *scratch_reg) {
    char line[128];
    char offset_text[32];

    if (!backend_is_aarch64(state)) {
        return 0;
    }
    if (offset == 0) {
        if (names_equal(dst_reg, base_reg)) {
            return 0;
        }
        rt_copy_string(line, sizeof(line), "mov ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), base_reg);
        return emit_instruction(state, line);
    }

    rt_unsigned_to_string((unsigned long long)offset, offset_text, sizeof(offset_text));
    if (offset <= 4095) {
        rt_copy_string(line, sizeof(line), "add ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), base_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", #");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), offset_text);
        return emit_instruction(state, line);
    }

    if (emit_load_immediate_register(state, scratch_reg != 0 ? scratch_reg : "x13", offset) != 0) {
        return -1;
    }
    rt_copy_string(line, sizeof(line), "add ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), base_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
    rt_copy_string(line + rt_strlen(line),
                   sizeof(line) - rt_strlen(line),
                   scratch_reg != 0 ? scratch_reg : "x13");
    return emit_instruction(state, line);
}

static int aarch64_object_copy_chunk_size(int remaining) {
    if (remaining >= 8) return 8;
    if (remaining >= 4) return 4;
    if (remaining >= 2) return 2;
    return 1;
}

static int emit_aarch64_load_copy_chunk(BackendState *state, int chunk_size) {
    const char *load_op;

    if (chunk_size == 8) {
        load_op = "ldr x11, [x14]";
    } else if (chunk_size == 4) {
        load_op = "ldr w11, [x14]";
    } else if (chunk_size == 2) {
        load_op = "ldrh w11, [x14]";
    } else {
        load_op = "ldrb w11, [x14]";
    }

    return emit_instruction(state, load_op);
}

static int emit_aarch64_store_copy_chunk(BackendState *state, int chunk_size) {
    const char *store_op;

    if (chunk_size == 8) {
        store_op = "str x11, [x14]";
    } else if (chunk_size == 4) {
        store_op = "str w11, [x14]";
    } else if (chunk_size == 2) {
        store_op = "strh w11, [x14]";
    } else {
        store_op = "strb w11, [x14]";
    }

    return emit_instruction(state, store_op);
}

static const char *x86_reg32_name(const char *reg) {
    if (names_equal(reg, "%rax")) return "%eax";
    if (names_equal(reg, "%rbx")) return "%ebx";
    if (names_equal(reg, "%rcx")) return "%ecx";
    if (names_equal(reg, "%rdx")) return "%edx";
    if (names_equal(reg, "%rsi")) return "%esi";
    if (names_equal(reg, "%rdi")) return "%edi";
    if (names_equal(reg, "%r8")) return "%r8d";
    if (names_equal(reg, "%r9")) return "%r9d";
    if (names_equal(reg, "%r10")) return "%r10d";
    if (names_equal(reg, "%r11")) return "%r11d";
    return "%eax";
}

static int scalar_type_access_size(const char *type_text, int word_index) {
    return backend_type_access_size(type_text, word_index);
}

int emit_load_from_address_into_register(BackendState *state, const char *address_reg, const char *dst_reg, int byte_value) {
    char line[64];
    int access_size = byte_value;
    int sign_extend = 0;

    if (access_size == 0) {
        access_size = backend_stack_slot_size(state);
    } else if (access_size < 0) {
        sign_extend = 1;
        access_size = -access_size;
    }

    if (backend_is_aarch64(state)) {
        if (access_size == 1 && sign_extend) {
            rt_copy_string(line, sizeof(line), "ldrsb ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
        } else if (access_size == 1) {
            rt_copy_string(line, sizeof(line), "ldrb w");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg + 1);
        } else if (access_size == 2 && sign_extend) {
            rt_copy_string(line, sizeof(line), "ldrsh ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
        } else if (access_size == 2) {
            rt_copy_string(line, sizeof(line), "ldrh w");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg + 1);
        } else if (access_size == 4 && sign_extend) {
            rt_copy_string(line, sizeof(line), "ldrsw ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
        } else if (access_size == 4) {
            rt_copy_string(line, sizeof(line), "ldr w");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg + 1);
        } else {
            rt_copy_string(line, sizeof(line), "ldr x");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg + 1);
        }
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", [");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), address_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
        return emit_instruction(state, line);
    }

    if (access_size == 1 && sign_extend) {
        rt_copy_string(line, sizeof(line), "movsbq (");
    } else if (access_size == 1) {
        rt_copy_string(line, sizeof(line), "movzbq (");
    } else if (access_size == 2 && sign_extend) {
        rt_copy_string(line, sizeof(line), "movswq (");
    } else if (access_size == 2) {
        rt_copy_string(line, sizeof(line), "movzwq (");
    } else if (access_size == 4 && sign_extend) {
        rt_copy_string(line, sizeof(line), "movslq (");
    } else if (access_size == 4) {
        rt_copy_string(line, sizeof(line), "movl (");
    } else {
        rt_copy_string(line, sizeof(line), "movq (");
    }
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), address_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "), ");
    rt_copy_string(line + rt_strlen(line),
                   sizeof(line) - rt_strlen(line),
                   (access_size == 4 && !sign_extend) ? x86_reg32_name(dst_reg) : dst_reg);
    return emit_instruction(state, line);
}

int emit_load_from_address_register(BackendState *state, const char *reg, int byte_value) {
    return emit_load_from_address_into_register(state, reg, backend_is_aarch64(state) ? "x0" : "%rax", byte_value);
}

int emit_move_value_register(BackendState *state, const char *dst_reg) {
    char line[64];

    if (backend_is_aarch64(state)) {
        if (names_equal(dst_reg, "x0")) {
            return 0;
        }
        rt_copy_string(line, sizeof(line), "mov ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", x0");
        return emit_instruction(state, line);
    }

    if (names_equal(dst_reg, "%rax")) {
        return 0;
    }
    rt_copy_string(line, sizeof(line), "movq %rax, ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
    return emit_instruction(state, line);
}

int emit_store_to_address_register(BackendState *state, const char *reg, int byte_value) {
    char line[64];
    int access_size = byte_value;

    if (access_size == 0) {
        access_size = backend_stack_slot_size(state);
    } else if (access_size < 0) {
        access_size = -access_size;
    }
    if (backend_is_aarch64(state)) {
        if (access_size == 1) {
            rt_copy_string(line, sizeof(line), "strb w0, [");
        } else if (access_size == 2) {
            rt_copy_string(line, sizeof(line), "strh w0, [");
        } else if (access_size == 4) {
            rt_copy_string(line, sizeof(line), "str w0, [");
        } else {
            rt_copy_string(line, sizeof(line), "str x0, [");
        }
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
        return emit_instruction(state, line);
    }

    if (access_size == 1) {
        rt_copy_string(line, sizeof(line), "movb %al, (");
    } else if (access_size == 2) {
        rt_copy_string(line, sizeof(line), "movw %ax, (");
    } else if (access_size == 4) {
        rt_copy_string(line, sizeof(line), "movl %eax, (");
    } else {
        rt_copy_string(line, sizeof(line), "movq %rax, (");
    }
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ")");
    return emit_instruction(state, line);
}

int emit_pop_address_and_store(BackendState *state, int byte_value) {
    return emit_pop_to_register(state, backend_is_aarch64(state) ? "x1" : "%rcx") == 0 &&
           emit_store_to_address_register(state, backend_is_aarch64(state) ? "x1" : "%rcx", byte_value) == 0 ? 0 : -1;
}

int find_string_literal(const BackendState *state, const char *text) {
    size_t i;
    for (i = 0; i < state->string_count; ++i) {
        if (names_equal(state->strings[i].text, text)) {
            return (int)i;
        }
    }
    return -1;
}

int add_string_literal(BackendState *state, const char *text) {
    char digits[32];
    int existing = find_string_literal(state, text);

    if (existing >= 0) {
        return existing;
    }
    if (state->string_count >= COMPILER_BACKEND_MAX_STRINGS) {
        backend_set_error(state->backend, "too many string literals for backend");
        return -1;
    }

    rt_copy_string(state->strings[state->string_count].label, sizeof(state->strings[state->string_count].label), "str");
    rt_unsigned_to_string((unsigned long long)state->string_count, digits, sizeof(digits));
    rt_copy_string(state->strings[state->string_count].label + rt_strlen(state->strings[state->string_count].label),
                   sizeof(state->strings[state->string_count].label) - rt_strlen(state->strings[state->string_count].label),
                   digits);
    rt_copy_string(state->strings[state->string_count].text, sizeof(state->strings[state->string_count].text), text);
    state->string_count += 1U;
    return (int)(state->string_count - 1U);
}

static int emit_darwin_global_address(BackendState *state, const char *symbol, const char *dst_reg) {
    char line[128];

    rt_copy_string(line, sizeof(line), "adrp ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@GOTPAGE");
    if (emit_instruction(state, line) != 0) {
        return -1;
    }

    rt_copy_string(line, sizeof(line), "ldr ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", [");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), dst_reg);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@GOTPAGEOFF]");
    return emit_instruction(state, line);
}

int emit_address_of_name(BackendState *state, const char *name) {
    char line[128];
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index >= 0) {
        if (state->locals[local_index].static_storage) {
            char formatted_symbol[COMPILER_IR_NAME_CAPACITY];
            const char *symbol = state->locals[local_index].symbol_name[0] != '\0'
                                     ? state->locals[local_index].symbol_name
                                     : state->locals[local_index].name;
            format_symbol_name(state, symbol, formatted_symbol, sizeof(formatted_symbol));
            if (backend_is_aarch64(state)) {
                if (backend_is_darwin(state)) {
                    return emit_darwin_global_address(state, formatted_symbol, "x0");
                }
                rt_copy_string(line, sizeof(line), "adrp x0, ");
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), formatted_symbol);
                if (emit_instruction(state, line) != 0) {
                    return -1;
                }
                rt_copy_string(line, sizeof(line), "add x0, x0, :lo12:");
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), formatted_symbol);
                return emit_instruction(state, line);
            }
            rt_copy_string(line, sizeof(line), "leaq ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), formatted_symbol);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip), %rax");
            return emit_instruction(state, line);
        }
        return emit_local_address(state, state->locals[local_index].offset, backend_is_aarch64(state) ? "x0" : "%rax");
    }

    if (global_index >= 0) {
        char symbol[COMPILER_IR_NAME_CAPACITY];
        format_symbol_name(state, name, symbol, sizeof(symbol));
        if (backend_is_aarch64(state)) {
            if (backend_is_darwin(state)) {
                return emit_darwin_global_address(state, symbol, "x0");
            }
            rt_copy_string(line, sizeof(line), "adrp x0, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? "@PAGE" : "");
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
            rt_copy_string(line, sizeof(line), "add x0, x0, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? symbol : ":lo12:");
            if (!backend_is_darwin(state)) {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            } else {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@PAGEOFF");
            }
            return emit_instruction(state, line);
        }
        rt_copy_string(line, sizeof(line), "leaq ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip), %rax");
        return emit_instruction(state, line);
    }

    if (is_function_name(state, name)) {
        char symbol[COMPILER_IR_NAME_CAPACITY];
        format_symbol_name(state, name, symbol, sizeof(symbol));
        if (backend_is_aarch64(state)) {
            rt_copy_string(line, sizeof(line), "adrp x0, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? "@PAGE" : "");
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
            rt_copy_string(line, sizeof(line), "add x0, x0, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? symbol : ":lo12:");
            if (!backend_is_darwin(state)) {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            } else {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@PAGEOFF");
            }
            return emit_instruction(state, line);
        }
        rt_copy_string(line, sizeof(line), "leaq ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip), %rax");
        return emit_instruction(state, line);
    }

    backend_set_error(state->backend, "backend only supports address-of on known storage");
    return -1;
}

int emit_load_string_literal(BackendState *state, const char *text) {
    char line[128];
    int index = add_string_literal(state, text);
    const char *label;

    if (index < 0) {
        return -1;
    }
    label = state->strings[index].label;
    if (backend_is_aarch64(state)) {
        rt_copy_string(line, sizeof(line), "adrp x0, ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), backend_private_label_prefix(state));
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), label);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                       backend_is_darwin(state) ? "@PAGE" : "");
        if (emit_instruction(state, line) != 0) {
            return -1;
        }
        rt_copy_string(line, sizeof(line), "add x0, x0, ");
        if (backend_is_darwin(state)) {
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), backend_private_label_prefix(state));
        } else {
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ":lo12:.L");
        }
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), label);
        if (backend_is_darwin(state)) {
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@PAGEOFF");
        }
        return emit_instruction(state, line);
    }
    rt_copy_string(line, sizeof(line), "leaq .L");
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), label);
    rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip), %rax");
    return emit_instruction(state, line);
}

int emit_load_name_into_register(BackendState *state, const char *name, const char *dst_reg) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);
    int constant_index = find_constant(state, name);
    long long builtin_value = 0;

    if (local_index >= 0) {
        if (state->locals[local_index].static_storage) {
            if (state->locals[local_index].is_array) {
                return emit_address_of_name(state, name) == 0 &&
                       emit_move_value_register(state, dst_reg) == 0 ? 0 : -1;
            }
            {
                int access_size =
                    scalar_type_access_size(state->locals[local_index].type_text, state->locals[local_index].prefers_word_index);
                const char *address_reg = dst_reg;
                if (emit_address_of_name(state, name) != 0) {
                    return -1;
                }
                if ((backend_is_aarch64(state) && !names_equal(dst_reg, "x0")) ||
                    (!backend_is_aarch64(state) && !names_equal(dst_reg, "%rax"))) {
                    if (emit_move_value_register(state, dst_reg) != 0) {
                        return -1;
                    }
                } else {
                    address_reg = backend_is_aarch64(state) ? "x0" : "%rax";
                }
                return emit_load_from_address_into_register(state, address_reg, dst_reg, access_size);
            }
        }
        const char *address_reg = backend_is_aarch64(state) ? "x9" : "%rax";
        if (state->locals[local_index].is_array) {
            return emit_local_address(state, state->locals[local_index].offset, dst_reg);
        }
        {
            int access_size =
                scalar_type_access_size(state->locals[local_index].type_text, state->locals[local_index].prefers_word_index);
            if (state->locals[local_index].pointer_depth > 0 && !state->locals[local_index].is_array) {
                access_size = 0;
            }
            return emit_local_address(state, state->locals[local_index].offset, address_reg) == 0 &&
                   emit_load_from_address_into_register(state, address_reg, dst_reg, access_size) == 0 ? 0 : -1;
        }
    }

    if (global_index >= 0) {
        if (state->globals[global_index].is_array) {
            return emit_address_of_name(state, name) == 0 &&
                   emit_move_value_register(state, dst_reg) == 0 ? 0 : -1;
        }
        {
            int access_size =
                scalar_type_access_size(state->globals[global_index].type_text, state->globals[global_index].prefers_word_index);
            if (state->globals[global_index].pointer_depth > 0 && !state->globals[global_index].is_array) {
                access_size = 0;
            }
            const char *address_reg = dst_reg;
            if (emit_address_of_name(state, name) != 0) {
                return -1;
            }
            if ((backend_is_aarch64(state) && !names_equal(dst_reg, "x0")) ||
                (!backend_is_aarch64(state) && !names_equal(dst_reg, "%rax"))) {
                if (emit_move_value_register(state, dst_reg) != 0) {
                    return -1;
                }
            } else {
                address_reg = backend_is_aarch64(state) ? "x0" : "%rax";
            }
            return emit_load_from_address_into_register(state, address_reg, dst_reg, access_size);
        }
    }

    if (constant_index >= 0) {
        return emit_load_immediate_register(state, dst_reg, state->constants[constant_index].value);
    }

    if (is_function_name(state, name)) {
        return emit_address_of_name(state, name) == 0 &&
               emit_move_value_register(state, dst_reg) == 0 ? 0 : -1;
    }

    if (lookup_builtin_macro_value(name, &builtin_value) == 0) {
        return emit_load_immediate_register(state, dst_reg, builtin_value);
    }

    if (names_equal(name, "NULL") || names_equal(name, "errno") || name_looks_like_macro_constant(name)) {
        return emit_load_immediate_register(state, dst_reg, 0);
    }

    backend_set_error(state->backend, "unsupported value reference in backend");
    return -1;
}

int emit_load_name(BackendState *state, const char *name) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);
    int constant_index = find_constant(state, name);
    long long builtin_value = 0;

    if (local_index >= 0) {
        if (state->locals[local_index].static_storage) {
            if (state->locals[local_index].is_array) {
                const char *type_text = skip_spaces(state->locals[local_index].type_text);
                if (!((starts_with(type_text, "struct:") || starts_with(type_text, "union:")) &&
                      !text_contains(type_text, "*") &&
                      !text_contains(type_text, "["))) {
                    return emit_address_of_name(state, name);
                }
            }
            {
                int access_size =
                    scalar_type_access_size(state->locals[local_index].type_text, state->locals[local_index].prefers_word_index);
                return emit_address_of_name(state, name) == 0 &&
                       emit_load_from_address_register(state, backend_is_aarch64(state) ? "x0" : "%rax", access_size) == 0 ? 0 : -1;
            }
        }
        if (state->locals[local_index].is_array) {
            const char *type_text = skip_spaces(state->locals[local_index].type_text);
            if ((starts_with(type_text, "struct:") || starts_with(type_text, "union:")) &&
                !text_contains(type_text, "*") &&
                !text_contains(type_text, "[")) {
                return emit_local_address(state, state->locals[local_index].offset, backend_is_aarch64(state) ? "x9" : "%rax") == 0 &&
                       emit_load_from_address_register(state, backend_is_aarch64(state) ? "x9" : "%rax", 0) == 0 ? 0 : -1;
            }
            return emit_address_of_name(state, name);
        }
        {
            int access_size =
                scalar_type_access_size(state->locals[local_index].type_text, state->locals[local_index].prefers_word_index);
            if (state->locals[local_index].pointer_depth > 0 && !state->locals[local_index].is_array) {
                access_size = 0;
            }
            return emit_local_address(state, state->locals[local_index].offset, backend_is_aarch64(state) ? "x9" : "%rax") == 0 &&
                   emit_load_from_address_register(state, backend_is_aarch64(state) ? "x9" : "%rax", access_size) == 0 ? 0 : -1;
        }
    }

    if (global_index >= 0) {
        if (state->globals[global_index].is_array) {
            const char *type_text = skip_spaces(state->globals[global_index].type_text);
            if (!((starts_with(type_text, "struct:") || starts_with(type_text, "union:")) &&
                  !text_contains(type_text, "*") &&
                  !text_contains(type_text, "["))) {
                return emit_address_of_name(state, name);
            }
        }
        {
            int access_size =
                scalar_type_access_size(state->globals[global_index].type_text, state->globals[global_index].prefers_word_index);
            if (state->globals[global_index].pointer_depth > 0 && !state->globals[global_index].is_array) {
                access_size = 0;
            }
            return emit_address_of_name(state, name) == 0 &&
                   emit_load_from_address_register(state, backend_is_aarch64(state) ? "x0" : "%rax", access_size) == 0 ? 0 : -1;
        }
    }

    if (constant_index >= 0) {
        return emit_load_immediate(state, state->constants[constant_index].value);
    }

    if (is_function_name(state, name)) {
        return emit_address_of_name(state, name);
    }

    if (lookup_builtin_macro_value(name, &builtin_value) == 0) {
        return emit_load_immediate(state, builtin_value);
    }

    if (names_equal(name, "NULL") || names_equal(name, "errno") || name_looks_like_macro_constant(name)) {
        return backend_is_aarch64(state) ? emit_instruction(state, "mov x0, #0") :
                                           emit_instruction(state, "movq $0, %rax");
    }

    backend_set_error(state->backend, "unsupported value reference in backend");
    return -1;
}

int emit_store_name(BackendState *state, const char *name) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index >= 0) {
        if (state->locals[local_index].is_array) {
            backend_set_error(state->backend, "unsupported local array assignment in backend");
            return -1;
        }
        if (state->locals[local_index].static_storage) {
            char line[128];
            char symbol[COMPILER_IR_NAME_CAPACITY];
            const char *static_symbol = state->locals[local_index].symbol_name[0] != '\0'
                                            ? state->locals[local_index].symbol_name
                                            : state->locals[local_index].name;
            format_symbol_name(state, static_symbol, symbol, sizeof(symbol));
            if (backend_is_aarch64(state)) {
                if (backend_is_darwin(state)) {
                    return emit_darwin_global_address(state, symbol, "x9") == 0 &&
                           emit_instruction(state, "str x0, [x9]") == 0 ? 0 : -1;
                }
                rt_copy_string(line, sizeof(line), "adrp x9, ");
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
                if (emit_instruction(state, line) != 0) {
                    return -1;
                }
                rt_copy_string(line, sizeof(line), "str x0, [x9, :lo12:");
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
                return emit_instruction(state, line);
            }
            rt_copy_string(line, sizeof(line), "movq %rax, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip)");
            return emit_instruction(state, line);
        }
        return emit_local_address(state, state->locals[local_index].offset, backend_is_aarch64(state) ? "x9" : "%rcx") == 0 &&
               emit_store_to_address_register(state, backend_is_aarch64(state) ? "x9" : "%rcx", 0) == 0 ? 0 : -1;
    }

    if (global_index >= 0) {
        char line[128];
        char symbol[COMPILER_IR_NAME_CAPACITY];
        format_symbol_name(state, name, symbol, sizeof(symbol));
        if (backend_is_aarch64(state)) {
            if (backend_is_darwin(state)) {
                return emit_darwin_global_address(state, symbol, "x9") == 0 &&
                       emit_instruction(state, "str x0, [x9]") == 0 ? 0 : -1;
            }
            rt_copy_string(line, sizeof(line), "adrp x9, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? "@PAGE" : "");
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
            rt_copy_string(line, sizeof(line), "str x0, [x9, ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line),
                           backend_is_darwin(state) ? symbol : ":lo12:");
            if (!backend_is_darwin(state)) {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
            } else {
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "@PAGEOFF");
            }
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "]");
            return emit_instruction(state, line);
        }
        rt_copy_string(line, sizeof(line), "movq %rax, ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), symbol);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), "(%rip)");
        return emit_instruction(state, line);
    }

    if (names_equal(name, "errno") || name_looks_like_macro_constant(name)) {
        return 0;
    }

    backend_set_error(state->backend, "unknown assignment target in backend");
    return -1;
}

int emit_copy_object_to_name(BackendState *state, const char *name) {
    int local_index = find_local(state, name);
    int bytes;
    int offset;
    int chunk;

    if (local_index < 0 || !state->locals[local_index].is_array) {
        backend_set_error(state->backend, "unsupported object assignment target in backend");
        return -1;
    }

    bytes = state->locals[local_index].stack_bytes;
    offset = state->locals[local_index].offset;
    if (bytes <= 0) {
        return 0;
    }

    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "mov x12, x0") != 0 ||
            (state->locals[local_index].static_storage ? emit_address_of_name(state, name)
                                                       : emit_local_address(state, offset, "x13")) != 0) {
            return -1;
        }
        if (state->locals[local_index].static_storage && emit_instruction(state, "mov x13, x0") != 0) {
            return -1;
        }
        for (chunk = 0; chunk < bytes; chunk += aarch64_object_copy_chunk_size(bytes - chunk)) {
            int chunk_size = aarch64_object_copy_chunk_size(bytes - chunk);
            if (emit_aarch64_offset_address(state, "x14", "x12", chunk, "x15") != 0 ||
                emit_aarch64_load_copy_chunk(state, chunk_size) != 0 ||
                emit_aarch64_offset_address(state, "x14", "x13", chunk, "x15") != 0 ||
                emit_aarch64_store_copy_chunk(state, chunk_size) != 0) {
                return -1;
            }
        }
        return emit_instruction(state, "mov x0, x13");
    }

    if (emit_instruction(state, "movq %rax, %rdx") != 0 ||
        (state->locals[local_index].static_storage ? emit_address_of_name(state, name) : emit_local_address(state, offset, "%rcx")) != 0) {
        return -1;
    }
    if (state->locals[local_index].static_storage && emit_instruction(state, "movq %rax, %rcx") != 0) {
        return -1;
    }
    for (chunk = 0; chunk < bytes; chunk += 8) {
        char load_line[64];
        char store_line[64];
        char chunk_text[32];

        rt_unsigned_to_string((unsigned long long)chunk, chunk_text, sizeof(chunk_text));
        rt_copy_string(load_line, sizeof(load_line), "movq ");
        rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line), chunk_text);
        rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line), "(%rdx), %r11");
        rt_copy_string(store_line, sizeof(store_line), "movq %r11, ");
        rt_copy_string(store_line + rt_strlen(store_line), sizeof(store_line) - rt_strlen(store_line), chunk_text);
        rt_copy_string(store_line + rt_strlen(store_line), sizeof(store_line) - rt_strlen(store_line), "(%rcx)");
        if (emit_instruction(state, load_line) != 0 || emit_instruction(state, store_line) != 0) {
            return -1;
        }
    }
    return emit_instruction(state, "movq %rcx, %rax");
}

int emit_copy_name_to_pointer_name(BackendState *state, const char *src_name, const char *dst_pointer_name) {
    int local_index = find_local(state, src_name);
    int dst_index = find_local(state, dst_pointer_name);
    int bytes;
    int offset;
    int chunk;

    if (local_index < 0 || dst_index < 0) {
        backend_set_error(state->backend, "unsupported object return in backend");
        return -1;
    }

    bytes = state->locals[local_index].stack_bytes;
    offset = state->locals[local_index].offset;
    if (bytes <= 0) {
        return emit_load_name(state, dst_pointer_name);
    }

    if (backend_is_aarch64(state)) {
        if (emit_load_name_into_register(state, dst_pointer_name, "x13") != 0 ||
            (state->locals[local_index].static_storage ? emit_address_of_name(state, src_name)
                                                       : emit_local_address(state, offset, "x12")) != 0) {
            return -1;
        }
        if (state->locals[local_index].static_storage && emit_instruction(state, "mov x12, x0") != 0) {
            return -1;
        }
        for (chunk = 0; chunk < bytes; chunk += aarch64_object_copy_chunk_size(bytes - chunk)) {
            int chunk_size = aarch64_object_copy_chunk_size(bytes - chunk);
            if (emit_aarch64_offset_address(state, "x14", "x12", chunk, "x15") != 0 ||
                emit_aarch64_load_copy_chunk(state, chunk_size) != 0 ||
                emit_aarch64_offset_address(state, "x14", "x13", chunk, "x15") != 0 ||
                emit_aarch64_store_copy_chunk(state, chunk_size) != 0) {
                return -1;
            }
        }
        return emit_instruction(state, "mov x0, x13");
    }

    if (emit_load_name_into_register(state, dst_pointer_name, "%rcx") != 0 ||
        (state->locals[local_index].static_storage ? emit_address_of_name(state, src_name)
                                                   : emit_local_address(state, offset, "%rdx")) != 0) {
        return -1;
    }
    if (state->locals[local_index].static_storage && emit_instruction(state, "movq %rax, %rdx") != 0) {
        return -1;
    }
    for (chunk = 0; chunk < bytes; chunk += 8) {
        char load_line[64];
        char store_line[64];
        char chunk_text[32];

        rt_unsigned_to_string((unsigned long long)chunk, chunk_text, sizeof(chunk_text));
        rt_copy_string(load_line, sizeof(load_line), "movq ");
        rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line), chunk_text);
        rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line), "(%rdx), %r11");
        rt_copy_string(store_line, sizeof(store_line), "movq %r11, ");
        rt_copy_string(store_line + rt_strlen(store_line), sizeof(store_line) - rt_strlen(store_line), chunk_text);
        rt_copy_string(store_line + rt_strlen(store_line), sizeof(store_line) - rt_strlen(store_line), "(%rcx)");
        if (emit_instruction(state, load_line) != 0 || emit_instruction(state, store_line) != 0) {
            return -1;
        }
    }
    return emit_instruction(state, "movq %rcx, %rax");
}

int emit_copy_object_to_pushed_address(BackendState *state, int bytes) {
    int chunk;

    if (bytes <= 0) {
        return 0;
    }

    if (backend_is_aarch64(state)) {
        if (emit_instruction(state, "mov x12, x0") != 0 ||
            emit_instruction(state, "ldr x13, [sp]") != 0 ||
            emit_instruction(state, "add sp, sp, #16") != 0) {
            return -1;
        }
        for (chunk = 0; chunk < bytes; chunk += aarch64_object_copy_chunk_size(bytes - chunk)) {
            int chunk_size = aarch64_object_copy_chunk_size(bytes - chunk);
            if (emit_aarch64_offset_address(state, "x14", "x12", chunk, "x15") != 0 ||
                emit_aarch64_load_copy_chunk(state, chunk_size) != 0 ||
                emit_aarch64_offset_address(state, "x14", "x13", chunk, "x15") != 0 ||
                emit_aarch64_store_copy_chunk(state, chunk_size) != 0) {
                return -1;
            }
        }
        return emit_instruction(state, "mov x0, x13");
    }

    if (emit_instruction(state, "movq %rax, %rdx") != 0 ||
        emit_instruction(state, "popq %rcx") != 0) {
        return -1;
    }
    for (chunk = 0; chunk < bytes; chunk += 8) {
        char load_line[64];
        char store_line[64];
        char chunk_text[32];

        rt_unsigned_to_string((unsigned long long)chunk, chunk_text, sizeof(chunk_text));
        rt_copy_string(load_line, sizeof(load_line), "movq ");
        rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line), chunk_text);
        rt_copy_string(load_line + rt_strlen(load_line), sizeof(load_line) - rt_strlen(load_line), "(%rdx), %r11");
        rt_copy_string(store_line, sizeof(store_line), "movq %r11, ");
        rt_copy_string(store_line + rt_strlen(store_line), sizeof(store_line) - rt_strlen(store_line), chunk_text);
        rt_copy_string(store_line + rt_strlen(store_line), sizeof(store_line) - rt_strlen(store_line), "(%rcx)");
        if (emit_instruction(state, load_line) != 0 || emit_instruction(state, store_line) != 0) {
            return -1;
        }
    }
    return emit_instruction(state, "movq %rcx, %rax");
}

int lookup_array_storage(const BackendState *state, const char *name, int *word_index_out) {
    int local_index = find_local(state, name);
    int global_index = find_global(state, name);

    if (local_index >= 0 &&
        (state->locals[local_index].is_array ||
         ((!text_contains(state->locals[local_index].type_text, "*")) &&
          (starts_with(skip_spaces(state->locals[local_index].type_text), "struct:") ||
           starts_with(skip_spaces(state->locals[local_index].type_text), "union:"))))) {
        *word_index_out = state->locals[local_index].prefers_word_index;
        return 1;
    }
    if (global_index >= 0 &&
        (state->globals[global_index].is_array ||
         ((!text_contains(state->globals[global_index].type_text, "*")) &&
          (starts_with(skip_spaces(state->globals[global_index].type_text), "struct:") ||
           starts_with(skip_spaces(state->globals[global_index].type_text), "union:"))))) {
        *word_index_out = state->globals[global_index].prefers_word_index;
        return 1;
    }
    return 0;
}

int emit_load_immediate_register(BackendState *state, const char *reg, long long value) {
    char digits[32];
    char line[96];

    if (!backend_is_aarch64(state)) {
        unsigned long long magnitude;
        int needs_movabs = value < -2147483648LL || value > 2147483647LL;

        if (value < 0) {
            magnitude = (unsigned long long)(-(value + 1LL)) + 1ULL;
            rt_unsigned_to_string(magnitude, digits, sizeof(digits));
            rt_copy_string(line, sizeof(line), needs_movabs ? "movabsq $-" : "movq $-");
        } else {
            magnitude = (unsigned long long)value;
            rt_unsigned_to_string(magnitude, digits, sizeof(digits));
            rt_copy_string(line, sizeof(line), needs_movabs ? "movabsq $" : "movq $");
        }
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), digits);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
        return emit_instruction(state, line);
    }

    {
        unsigned long long bits = (unsigned long long)value;
        unsigned int shifts[] = {0U, 16U, 32U, 48U};
        int emitted = 0;
        size_t i;

        for (i = 0; i < sizeof(shifts) / sizeof(shifts[0]); ++i) {
            unsigned long long part = (bits >> shifts[i]) & 0xffffULL;
            char part_text[32];

            if (part == 0ULL && emitted) {
                continue;
            }
            rt_unsigned_to_string(part, part_text, sizeof(part_text));
            if (!emitted) {
                rt_copy_string(line, sizeof(line), "movz ");
            } else {
                rt_copy_string(line, sizeof(line), "movk ");
            }
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", #");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), part_text);
            if (shifts[i] != 0U) {
                char shift_text[32];
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", lsl #");
                rt_unsigned_to_string((unsigned long long)shifts[i], shift_text, sizeof(shift_text));
                rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), shift_text);
            }
            if (emit_instruction(state, line) != 0) {
                return -1;
            }
            emitted = 1;
        }

        if (!emitted) {
            rt_copy_string(line, sizeof(line), "mov ");
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), reg);
            rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), ", #0");
            return emit_instruction(state, line);
        }
    }

    return 0;
}

int emit_load_immediate(BackendState *state, long long value) {
    return emit_load_immediate_register(state, backend_is_aarch64(state) ? "x0" : "%rax", value);
}

int emit_push_value(BackendState *state) {
    if (backend_is_aarch64(state)) {
        return emit_instruction(state, "sub sp, sp, #16") == 0 &&
               emit_instruction(state, "str x0, [sp]") == 0 ? 0 : -1;
    }
    return emit_instruction(state, "pushq %rax");
}

int emit_cmp_zero(BackendState *state) {
    return backend_is_aarch64(state) ? emit_instruction(state, "cmp x0, #0") :
                                       emit_instruction(state, "cmpq $0, %rax");
}

int emit_set_condition(BackendState *state, const char *condition) {
    if (backend_is_aarch64(state)) {
        char line[32];
        rt_copy_string(line, sizeof(line), "cset x0, ");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), condition);
        return emit_instruction(state, line);
    }

    {
        char line[32];
        const char *x86_condition = condition;
        if (names_equal(condition, "eq")) x86_condition = "e";
        else if (names_equal(condition, "gt")) x86_condition = "g";
        else if (names_equal(condition, "lt")) x86_condition = "l";
        rt_copy_string(line, sizeof(line), "set");
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), x86_condition);
        rt_copy_string(line + rt_strlen(line), sizeof(line) - rt_strlen(line), " %al");
        return emit_instruction(state, line) == 0 &&
               emit_instruction(state, "movzbq %al, %rax") == 0 ? 0 : -1;
    }
}

int emit_jump_to_label(BackendState *state, const char *mnemonic, const char *label) {
    char asm_label[96];
    char scoped_label[64];

    write_label_name(state, scoped_label, sizeof(scoped_label), label);

    if (backend_is_aarch64(state)) {
        rt_copy_string(asm_label, sizeof(asm_label), mnemonic);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), " ");
    } else {
        rt_copy_string(asm_label, sizeof(asm_label), mnemonic);
        rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), " ");
    }
    rt_copy_string(asm_label + rt_strlen(asm_label), sizeof(asm_label) - rt_strlen(asm_label), scoped_label);
    return emit_instruction(state, asm_label);
}
