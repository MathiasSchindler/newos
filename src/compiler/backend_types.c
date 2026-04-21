/* Shared backend type/layout/literal helpers. */

#include "backend_internal.h"

static int hex_digit_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
    if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
    return -1;
}

char backend_decode_escaped_char(const char **cursor_inout) {
    const char *cursor = *cursor_inout;
    unsigned int value = 0U;
    int digits = 0;
    int hex = 0;

    if (*cursor == 'n') {
        *cursor_inout = cursor + 1;
        return '\n';
    }
    if (*cursor == 't') {
        *cursor_inout = cursor + 1;
        return '\t';
    }
    if (*cursor == 'r') {
        *cursor_inout = cursor + 1;
        return '\r';
    }
    if (*cursor == 'v') {
        *cursor_inout = cursor + 1;
        return '\v';
    }
    if (*cursor == 'f') {
        *cursor_inout = cursor + 1;
        return '\f';
    }
    if (*cursor == 'a') {
        *cursor_inout = cursor + 1;
        return '\a';
    }
    if (*cursor == 'b') {
        *cursor_inout = cursor + 1;
        return '\b';
    }
    if (*cursor == 'x' || *cursor == 'X') {
        cursor += 1;
        while ((hex = hex_digit_value(*cursor)) >= 0) {
            value = (value * 16U) + (unsigned int)hex;
            cursor += 1;
            digits += 1;
        }
        *cursor_inout = cursor;
        return digits > 0 ? (char)(unsigned char)value : 'x';
    }
    if (*cursor >= '0' && *cursor <= '7') {
        while (digits < 3 && *cursor >= '0' && *cursor <= '7') {
            value = (value * 8U) + (unsigned int)(*cursor - '0');
            cursor += 1;
            digits += 1;
        }
        *cursor_inout = cursor;
        return (char)(unsigned char)value;
    }

    *cursor_inout = cursor + (*cursor != '\0' ? 1 : 0);
    return *cursor;
}

int backend_type_access_size(const char *type_text, int word_index) {
    const char *type = skip_spaces(type_text != 0 ? type_text : "");
    int is_unsigned = text_contains(type, "unsigned");

    if (type[0] == '\0') {
        return word_index ? 0 : -1;
    }
    if (text_contains(type, "char") && !text_contains(type, "*") &&
        !starts_with(type, "struct") && !starts_with(type, "union") && !starts_with(type, "enum")) {
        return is_unsigned ? 1 : -1;
    }
    if (text_contains(type, "short") && !text_contains(type, "*")) {
        return is_unsigned ? 2 : -2;
    }
    if ((text_contains(type, "int") || starts_with(type, "enum")) && !text_contains(type, "*")) {
        return is_unsigned ? 4 : -4;
    }
    return 0;
}

int backend_member_prefers_word_index(const char *name, const char *type_text) {
    if (type_text != 0 && should_prefer_word_index(name != 0 ? name : "", type_text)) {
        return 1;
    }
    if (type_text != 0 && type_text[0] != '\0') {
        if (text_contains(type_text, "char") && !text_contains(type_text, "*") && !text_contains(type_text, "struct") &&
            !text_contains(type_text, "union") && !text_contains(type_text, "enum")) {
            return 0;
        }
        return 1;
    }
    if (names_equal(name, "argv") || names_equal(name, "envp") || names_equal(name, "commands") ||
         names_equal(name, "jobs") || names_equal(name, "aliases") || names_equal(name, "functions") ||
         names_equal(name, "entries") || names_equal(name, "fields") ||
         names_equal(name, "input_path") || names_equal(name, "output_path") ||
         names_equal(name, "no_expand") || names_equal(name, "pids") ||
         names_equal(name, "argc") || names_equal(name, "count") ||
         names_equal(name, "output_append") || names_equal(name, "active") ||
         names_equal(name, "job_id") || names_equal(name, "pid_count") ||
         names_equal(name, "is_dir") || names_equal(name, "is_hidden")) {
        return 1;
    }
    return 0;
}

int backend_member_result_decays_to_address(const char *type_text) {
    return type_text != 0 && text_contains(type_text, "[");
}

static int type_matches_named_aggregate(const char *base_type, const char *name) {
    return base_type != 0 && name != 0 && base_type[0] != '\0' &&
           text_contains(base_type, name);
}

int backend_member_byte_offset(const BackendState *state, const char *base_type, const char *member_name) {
    int offset = 0;

    if (lookup_aggregate_member(state, base_type, member_name, &offset, 0) == 0) {
        return offset;
    }

    if (type_matches_named_aggregate(base_type, "ShCommand")) {
        if (names_equal(member_name, "argv")) return 0;
        if (names_equal(member_name, "argc")) return 520;
        if (names_equal(member_name, "input_path")) return 528;
        if (names_equal(member_name, "output_path")) return 536;
        if (names_equal(member_name, "output_append")) return 544;
        if (names_equal(member_name, "no_expand")) return 548;
    }
    if (type_matches_named_aggregate(base_type, "ShPipeline")) {
        if (names_equal(member_name, "commands")) return 0;
        if (names_equal(member_name, "count")) return 6464;
    }
    if (type_matches_named_aggregate(base_type, "ShJob")) {
        if (names_equal(member_name, "active")) return 0;
        if (names_equal(member_name, "job_id")) return 4;
        if (names_equal(member_name, "pid_count")) return 8;
        if (names_equal(member_name, "pids")) return 12;
        if (names_equal(member_name, "command")) return 44;
    }
    if (type_matches_named_aggregate(base_type, "ShAlias") || type_matches_named_aggregate(base_type, "ShFunction")) {
        if (names_equal(member_name, "active")) return 0;
        if (names_equal(member_name, "name")) return 4;
        if (names_equal(member_name, "value") || names_equal(member_name, "body")) return 68;
    }
    if (type_matches_named_aggregate(base_type, "ExprParser")) {
        if (names_equal(member_name, "argc")) return 0;
        if (names_equal(member_name, "argv")) return 8;
        if (names_equal(member_name, "index")) return 16;
        if (names_equal(member_name, "error")) return 24;
    }
    if (type_matches_named_aggregate(base_type, "PlatformDirEntry")) {
        if (names_equal(member_name, "name")) return 0;
        if (names_equal(member_name, "owner")) return 328;
        if (names_equal(member_name, "group")) return 360;
        if (names_equal(member_name, "is_dir")) return 392;
        if (names_equal(member_name, "is_hidden")) return 396;
    }
    return 0;
}

void backend_copy_member_result_type(const BackendState *state,
                                     const char *base_type,
                                     const char *member_name,
                                     char *buffer,
                                     size_t buffer_size) {
    const char *member_type = 0;

    if (buffer_size == 0) {
        return;
    }

    if (lookup_aggregate_member(state, base_type, member_name, 0, &member_type) == 0 &&
        member_type != 0 && member_type[0] != '\0') {
        rt_copy_string(buffer, buffer_size, member_type);
        return;
    }

    if (type_matches_named_aggregate(base_type, "ShPipeline") && names_equal(member_name, "commands")) {
        rt_copy_string(buffer, buffer_size, "struct:ShCommand[8]");
        return;
    }
    if (type_matches_named_aggregate(base_type, "ShCommand")) {
        if (names_equal(member_name, "argv")) {
            rt_copy_string(buffer, buffer_size, "char*[65]");
            return;
        }
        if (names_equal(member_name, "input_path") || names_equal(member_name, "output_path")) {
            rt_copy_string(buffer, buffer_size, "char*");
            return;
        }
        if (names_equal(member_name, "no_expand")) {
            rt_copy_string(buffer, buffer_size, "int[64]");
            return;
        }
    }
    if (type_matches_named_aggregate(base_type, "ShJob") && names_equal(member_name, "pids")) {
        rt_copy_string(buffer, buffer_size, "int[8]");
        return;
    }
    if (type_matches_named_aggregate(base_type, "ShJob") && names_equal(member_name, "command")) {
        rt_copy_string(buffer, buffer_size, "char[4096]");
        return;
    }
    if (type_matches_named_aggregate(base_type, "ShAlias")) {
        if (names_equal(member_name, "name")) {
            rt_copy_string(buffer, buffer_size, "char[64]");
            return;
        }
        if (names_equal(member_name, "value")) {
            rt_copy_string(buffer, buffer_size, "char[4096]");
            return;
        }
    }
    if (type_matches_named_aggregate(base_type, "ShFunction")) {
        if (names_equal(member_name, "name")) {
            rt_copy_string(buffer, buffer_size, "char[64]");
            return;
        }
        if (names_equal(member_name, "body")) {
            rt_copy_string(buffer, buffer_size, "char[4096]");
            return;
        }
    }
    if (type_matches_named_aggregate(base_type, "PlatformDirEntry")) {
        if (names_equal(member_name, "name")) {
            rt_copy_string(buffer, buffer_size, "char[256]");
            return;
        }
        if (names_equal(member_name, "owner") || names_equal(member_name, "group")) {
            rt_copy_string(buffer, buffer_size, "char[32]");
            return;
        }
    }
    if (names_equal(member_name, "bytes") || names_equal(member_name, "data") ||
        names_equal(member_name, "text") || names_equal(member_name, "buffer") ||
        names_equal(member_name, "line") || names_equal(member_name, "pattern") ||
        names_equal(member_name, "name") || names_equal(member_name, "value") ||
        names_equal(member_name, "body") || names_equal(member_name, "command") ||
        names_equal(member_name, "user") || names_equal(member_name, "owner") ||
        names_equal(member_name, "group") || names_equal(member_name, "state")) {
        rt_copy_string(buffer, buffer_size, "char[4096]");
        return;
    }
    rt_copy_string(buffer, buffer_size, base_type != 0 ? base_type : "");
}

int backend_type_is_pointer_like(const char *base_type) {
    return base_type != 0 && text_contains(base_type, "*");
}

void backend_copy_indexed_type_text(const char *base_type, char *buffer, size_t buffer_size) {
    size_t i = 0;
    size_t out = 0;
    const char *suffix = 0;
    int had_index_suffix = 0;

    if (buffer_size == 0) {
        return;
    }
    if (base_type == 0) {
        buffer[0] = '\0';
        return;
    }

    while (base_type[i] != '\0' && base_type[i] != '[' && out + 1 < buffer_size) {
        buffer[out++] = base_type[i++];
    }
    while (base_type[i] != '\0' && base_type[i] != ']') {
        i += 1U;
    }
    if (base_type[i] == ']') {
        suffix = base_type + i + 1U;
        had_index_suffix = 1;
    }
    while (out > 0U && buffer[out - 1U] == ' ') {
        out -= 1U;
    }
    buffer[out] = '\0';
    if (suffix != 0 && suffix[0] != '\0') {
        rt_copy_string(buffer + out, buffer_size - out, suffix);
        return;
    }
    if (had_index_suffix) {
        return;
    }

    while (out > 0U && buffer[out - 1U] == ' ') {
        out -= 1U;
    }
    while (out > 0U && buffer[out - 1U] == '*') {
        out -= 1U;
        break;
    }
    while (out > 0U && buffer[out - 1U] == ' ') {
        out -= 1U;
    }
    buffer[out] = '\0';
}

static int named_aggregate_element_size(const BackendState *state, const char *base_type) {
    int generic_size = lookup_aggregate_size(state, base_type);

    if (generic_size > 0) {
        return generic_size;
    }
    if (type_matches_named_aggregate(base_type, "ShCommand")) return 808;
    if (type_matches_named_aggregate(base_type, "ShPipeline")) return 6472;
    if (type_matches_named_aggregate(base_type, "ShJob")) return 4140;
    if (type_matches_named_aggregate(base_type, "ShAlias") || type_matches_named_aggregate(base_type, "ShFunction")) return 4164;
    if (type_matches_named_aggregate(base_type, "PlatformDirEntry")) return 400;
    return 0;
}

void backend_copy_dereferenced_type_text(const char *base_type, char *buffer, size_t buffer_size) {
    size_t length;

    if (buffer_size == 0) {
        return;
    }
    if (base_type == 0) {
        buffer[0] = '\0';
        return;
    }

    rt_copy_string(buffer, buffer_size, base_type);
    length = rt_strlen(buffer);
    while (length > 0U && buffer[length - 1U] == ' ') {
        buffer[--length] = '\0';
    }
    while (length > 0U && buffer[length - 1U] == '*') {
        buffer[--length] = '\0';
        break;
    }
    while (length > 0U && buffer[length - 1U] == ' ') {
        buffer[--length] = '\0';
    }
    if (text_contains(buffer, "[")) {
        char indexed[128];
        backend_copy_indexed_type_text(buffer, indexed, sizeof(indexed));
        rt_copy_string(buffer, buffer_size, indexed);
    }
}

long long backend_type_storage_bytes(const BackendState *state, const char *type_text) {
    const char *type = skip_spaces(type_text != 0 ? type_text : "");
    char element_type[128];
    const char *open = 0;
    unsigned long long length = 0ULL;
    long long element_size;
    int aggregate_size;

    if (type[0] == '\0') {
        return backend_stack_slot_size(state);
    }

    open = type;
    while (*open != '\0' && *open != '[') {
        open += 1;
    }
    if (*open == '[') {
        const char *cursor = open + 1;
        backend_copy_indexed_type_text(type, element_type, sizeof(element_type));
        while (*cursor >= '0' && *cursor <= '9') {
            length = length * 10ULL + (unsigned long long)(*cursor - '0');
            cursor += 1;
        }
        if (length == 0ULL) {
            length = 1ULL;
        }
        element_size = backend_type_storage_bytes(state, element_type);
        return element_size * (long long)length;
    }

    if (!text_contains(type, "*")) {
        aggregate_size = lookup_aggregate_size(state, type);
        if (aggregate_size > 0) {
            return aggregate_size;
        }
        aggregate_size = named_aggregate_element_size(state, type);
        if (aggregate_size > 0) {
            return aggregate_size;
        }
    }

    if (starts_with(type, "struct:") || starts_with(type, "union:")) {
        aggregate_size = lookup_aggregate_size(state, type);
        if (aggregate_size > 0) {
            return aggregate_size;
        }
        aggregate_size = named_aggregate_element_size(state, type);
        if (aggregate_size > 0) {
            return aggregate_size;
        }
        return BACKEND_STRUCT_STACK_BYTES;
    }
    if (text_contains(type, "*")) {
        return backend_stack_slot_size(state);
    }
    if (names_equal(type, "size_t") || names_equal(type, "ssize_t") || names_equal(type, "ptrdiff_t") ||
        names_equal(type, "intptr_t") || names_equal(type, "uintptr_t") || names_equal(type, "usize") ||
        names_equal(type, "off_t") || names_equal(type, "time_t")) {
        return 8;
    }
    if (text_contains(type, "char")) {
        return 1;
    }
    if (text_contains(type, "short")) {
        return 2;
    }
    if (text_contains(type, "__int128")) {
        return 16;
    }
    if (text_contains(type, "long") || text_contains(type, "double")) {
        return 8;
    }
    if (starts_with(type, "enum") || text_contains(type, "int")) {
        return 4;
    }

    rt_copy_string(element_type, sizeof(element_type), "struct:");
    rt_copy_string(element_type + rt_strlen(element_type), sizeof(element_type) - rt_strlen(element_type), type);
    aggregate_size = lookup_aggregate_size(state, element_type);
    if (aggregate_size > 0) {
        return aggregate_size;
    }

    return backend_stack_slot_size(state);
}

int backend_array_index_scale(const BackendState *state, const char *base_type, int word_index) {
    if (base_type != 0 && text_contains(base_type, "[")) {
        char element_type[128];
        long long element_size;

        backend_copy_indexed_type_text(base_type, element_type, sizeof(element_type));
        element_size = backend_type_storage_bytes(state, element_type);
        if (element_size > 0 && element_size <= 0x7fffffffLL) {
            return (int)element_size;
        }
        return backend_stack_slot_size(state);
    }
    if (base_type != 0 && text_contains(base_type, "*")) {
        char element_type[128];
        long long element_size;

        backend_copy_dereferenced_type_text(base_type, element_type, sizeof(element_type));
        element_size = backend_type_storage_bytes(state, element_type);
        if (element_size > 0 && element_size <= 0x7fffffffLL) {
            return (int)element_size;
        }
    }
    return word_index ? backend_stack_slot_size(state) : 1;
}
