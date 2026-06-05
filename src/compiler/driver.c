#include "compiler.h"

#include "backend.h"
#include "linker.h"
#include "lexer.h"
#include "object_writer.h"
#include "parser.h"
#include "platform.h"
#include "preprocessor.h"
#include "runtime.h"
#include "source.h"
#include "source_manifest.h"
#include "tool_util.h"

#define COMPILER_MAX_OPTION_DEFINES 32
#define COMPILER_MAX_INPUT_FILES 256
#define COMPILER_MAX_EXTRA_LINK_ARGS 32
#define COMPILER_MAX_LINK_ARGS (COMPILER_MAX_INPUT_FILES * 4 + 64)
#define COMPILER_MANIFEST_MAX_BYTES (1024 * 1024)
#define COMPILER_LTO_MAX_TRACKED_NAMES 1024

typedef enum {
    COMPILER_LTO_BUILD_OK = 0,
    COMPILER_LTO_BUILD_FAILED = 1,
    COMPILER_LTO_BUILD_FALLBACK = 2
} CompilerLtoBuildStatus;

typedef struct {
    char names[COMPILER_LTO_MAX_TRACKED_NAMES][COMPILER_IR_NAME_CAPACITY];
    size_t count;
} CompilerLtoNameSet;

typedef struct {
    char old_names[COMPILER_LTO_MAX_TRACKED_NAMES][COMPILER_IR_NAME_CAPACITY];
    char new_names[COMPILER_LTO_MAX_TRACKED_NAMES][COMPILER_IR_NAME_CAPACITY];
    size_t count;
} CompilerLtoRenameMap;

typedef struct {
    const char *program_name;
    const char *input_path;
    const char *output_path;
    const char *compile_manifest_path;
    const char *input_paths[COMPILER_MAX_INPUT_FILES];
    size_t input_count;
    CompilerTarget target;
    int compile_only;
    int syntax_only;
    int dump_tokens;
    int preprocess_only;
    int dump_ast;
    int dump_ir;
    int emit_assembly;
    int freestanding;
    int no_stdlib;
    int static_link;
    int function_sections;
    int data_sections;
    int lto;
    int time_report;
    char include_dirs[COMPILER_MAX_INCLUDE_DIRS][COMPILER_PATH_CAPACITY];
    size_t include_dir_count;
    const char *extra_link_args[COMPILER_MAX_EXTRA_LINK_ARGS];
    size_t extra_link_arg_count;
    char define_names[COMPILER_MAX_OPTION_DEFINES][COMPILER_MACRO_NAME_CAPACITY];
    char define_values[COMPILER_MAX_OPTION_DEFINES][COMPILER_MACRO_VALUE_CAPACITY];
    size_t define_count;
} CompilerOptions;

static int text_equals(const char *lhs, const char *rhs) {
    size_t i = 0;

    while (lhs[i] != '\0' && rhs[i] != '\0') {
        if (lhs[i] != rhs[i]) {
            return 0;
        }
        i += 1;
    }

    return lhs[i] == rhs[i];
}

static int starts_with(const char *text, const char *prefix) {
    size_t i = 0;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1;
    }

    return 1;
}

static int ends_with(const char *text, const char *suffix) {
    size_t text_length = rt_strlen(text);
    size_t suffix_length = rt_strlen(suffix);

    if (suffix_length > text_length) {
        return 0;
    }

    return text_equals(text + text_length - suffix_length, suffix);
}

static int is_ignored_option(const char *arg) {
    if ((starts_with(arg, "-W") && !starts_with(arg, "-Wl,")) ||
        text_equals(arg, "-ffreestanding") ||
        text_equals(arg, "-fno-builtin") ||
        text_equals(arg, "-fno-stack-protector") ||
        text_equals(arg, "-fno-unwind-tables") ||
        text_equals(arg, "-fno-asynchronous-unwind-tables") ||
        text_equals(arg, "-nostdlib") ||
        text_equals(arg, "-static") ||
        text_equals(arg, "-fsyntax-only")) {
        return 1;
    }

    if (starts_with(arg, "-std=") || starts_with(arg, "-O")) {
        return 1;
    }

    return 0;
}

static int add_extra_link_arg(CompilerOptions *options, const char *value) {
    if (options->extra_link_arg_count >= COMPILER_MAX_EXTRA_LINK_ARGS) {
        return -1;
    }

    options->extra_link_args[options->extra_link_arg_count++] = value;
    return 0;
}

static void set_native_linker_string(const char **field, char *buffer, size_t buffer_size, const char *value) {
    rt_copy_string(buffer, buffer_size, value);
    *field = buffer;
}

static void apply_native_linker_option_token(CompilerLinkerOptions *link_options,
                                             char *entry_buffer,
                                             size_t entry_buffer_size,
                                             char *lto_cc_buffer,
                                             size_t lto_cc_buffer_size,
                                             const char *token) {
    if (text_equals(token, "--tiny") || text_equals(token, "--pack-segments")) {
        link_options->tiny = 1;
        return;
    }
    if (text_equals(token, "--separate-code") || text_equals(token, "--page-align")) {
        link_options->tiny = 0;
        return;
    }
    if (text_equals(token, "--gc-sections")) {
        link_options->gc_sections = 1;
        return;
    }
    if (text_equals(token, "--no-gc-sections")) {
        link_options->gc_sections = 0;
        return;
    }
    if (text_equals(token, "--icf=safe") || text_equals(token, "--icf")) {
        link_options->icf_safe = 1;
        return;
    }
    if (text_equals(token, "--icf=none")) {
        link_options->icf_safe = 0;
        return;
    }
    if (starts_with(token, "--entry=")) {
        set_native_linker_string(&link_options->entry_symbol, entry_buffer, entry_buffer_size, token + 8);
        return;
    }
    if (starts_with(token, "--lto-cc=")) {
        set_native_linker_string(&link_options->lto_cc, lto_cc_buffer, lto_cc_buffer_size, token + 9);
        return;
    }
}

static void apply_native_wl_arg(CompilerLinkerOptions *link_options,
                                char *entry_buffer,
                                size_t entry_buffer_size,
                                char *lto_cc_buffer,
                                size_t lto_cc_buffer_size,
                                const char *arg) {
    const char *cursor;
    int expect_entry = 0;
    int expect_lto_cc = 0;
    char token[COMPILER_PATH_CAPACITY];

    if (!starts_with(arg, "-Wl,")) {
        return;
    }

    cursor = arg + 4;
    while (*cursor != '\0') {
        size_t length = 0;

        while (cursor[length] != '\0' && cursor[length] != ',') {
            if (length + 1U < sizeof(token)) {
                token[length] = cursor[length];
            }
            length += 1U;
        }
        if (length >= sizeof(token)) {
            token[sizeof(token) - 1U] = '\0';
        } else {
            token[length] = '\0';
        }

        if (expect_entry) {
            set_native_linker_string(&link_options->entry_symbol, entry_buffer, entry_buffer_size, token);
            expect_entry = 0;
        } else if (expect_lto_cc) {
            set_native_linker_string(&link_options->lto_cc, lto_cc_buffer, lto_cc_buffer_size, token);
            expect_lto_cc = 0;
        } else if (text_equals(token, "-e") || text_equals(token, "--entry") || text_equals(token, "--lto-cc")) {
            if (text_equals(token, "--lto-cc")) {
                expect_lto_cc = 1;
            } else {
                expect_entry = 1;
            }
        } else {
            apply_native_linker_option_token(link_options, entry_buffer, entry_buffer_size, lto_cc_buffer, lto_cc_buffer_size, token);
        }

        cursor += length;
        if (*cursor == ',') {
            cursor += 1;
        }
    }
}

static void configure_native_linker_options(const CompilerOptions *options,
                                            CompilerLinkerOptions *link_options,
                                            char *entry_buffer,
                                            size_t entry_buffer_size,
                                            char *lto_cc_buffer,
                                            size_t lto_cc_buffer_size) {
    size_t i;

    rt_memset(link_options, 0, sizeof(*link_options));
    link_options->target = COMPILER_LINKER_TARGET_ELF64_X86_64;

    for (i = 0; i < options->extra_link_arg_count; ++i) {
        apply_native_wl_arg(link_options, entry_buffer, entry_buffer_size, lto_cc_buffer, lto_cc_buffer_size, options->extra_link_args[i]);
    }
}

static int is_direct_link_input(const char *path) {
    return ends_with(path, ".o") ||
           ends_with(path, ".a") ||
           ends_with(path, ".s") ||
           ends_with(path, ".S");
}

static int is_assembly_input(const char *path) {
    return ends_with(path, ".s") || ends_with(path, ".S");
}

static int is_c_input(const char *path) {
    return ends_with(path, ".c");
}

static int looks_like_ncc_driver(const char *path) {
    const char *base_name;

    if (path == 0 || path[0] == '\0') {
        return 0;
    }

    base_name = tool_base_name(path);
    return ends_with(base_name, "ncc");
}

static CompilerTarget default_target(void) {
    return compiler_target_default();
}

static const char *target_name(CompilerTarget target) {
    return compiler_target_name(target);
}

static int parse_target(const char *text, CompilerTarget *target_out) {
    return compiler_target_parse(text, target_out);
}

static void write_usage(const char *program_name) {
    tool_write_usage(program_name, "[-E|--preprocess] [-S|--emit-asm] [--dump-tokens|--dump-ast|--dump-ir] [--target TARGET] [-I DIR] [-DNAME=VALUE] [-c] [--compile-manifest FILE] [-fsyntax-only] [-flto] [-o OUTPUT] FILE...");
    rt_write_cstr(2, "Targets: ");
    compiler_target_write_names(2);
    rt_write_char(2, '\n');
}

static const char *pick_output_path(const CompilerOptions *options, char *buffer, size_t buffer_size) {
    size_t i = 0;
    size_t last_slash = 0;
    size_t last_dot = 0;
    const char *suffix = options->compile_only ? ".o" : ".s";

    if (options->output_path != 0) {
        return options->output_path;
    }
    if (!options->compile_only && !options->emit_assembly) {
        return 0;
    }

    while (options->input_path[i] != '\0') {
        if (options->input_path[i] == '/') {
            last_slash = i + 1U;
            last_dot = 0;
        } else if (options->input_path[i] == '.') {
            last_dot = i;
        }
        i += 1U;
    }

    if (last_dot <= last_slash) {
        last_dot = i;
    }
    if (last_dot + rt_strlen(suffix) + 1U > buffer_size) {
        return 0;
    }

    memcpy(buffer, options->input_path, last_dot);
    buffer[last_dot] = '\0';
    rt_copy_string(buffer + last_dot, buffer_size - last_dot, suffix);
    return buffer;
}

static int parse_translation_unit(const CompilerOptions *options);
static void write_preprocessor_error(const char *program_name, const CompilerPreprocessor *preprocessor);
static int configure_preprocessor(CompilerPreprocessor *preprocessor, const CompilerOptions *options);

static void write_time_report(const CompilerOptions *options,
                              unsigned long long preprocess_ns,
                              unsigned long long parse_ns,
                              unsigned long long optimize_ns,
                              unsigned long long output_ns,
                              unsigned long long total_ns) {
    if (!options->time_report) {
        return;
    }

    rt_write_cstr(2, "ncc time ");
    rt_write_cstr(2, options->input_path != 0 ? options->input_path : "<input>");
    rt_write_cstr(2, ": preprocess_us=");
    rt_write_uint(2, preprocess_ns / 1000ULL);
    rt_write_cstr(2, " parse_us=");
    rt_write_uint(2, parse_ns / 1000ULL);
    rt_write_cstr(2, " optimize_us=");
    rt_write_uint(2, optimize_ns / 1000ULL);
    rt_write_cstr(2, " output_us=");
    rt_write_uint(2, output_ns / 1000ULL);
    rt_write_cstr(2, " total_us=");
    rt_write_uint(2, total_ns / 1000ULL);
    rt_write_char(2, '\n');
}

static const char *pick_link_output_path(const CompilerOptions *options, char *buffer, size_t buffer_size) {
    if (options->output_path != 0) {
        return options->output_path;
    }
    rt_copy_string(buffer, buffer_size, "a.out");
    return buffer;
}

static void append_link_arg(char **argv, size_t *count, size_t capacity, const char *value) {
    if (*count + 1U < capacity) {
        argv[*count] = (char *)value;
        *count += 1U;
    }
}

static void append_target_link_arg(char **argv, size_t *count, size_t capacity, CompilerTarget target, const char *value) {
    if (compiler_target_is_darwin(target)) {
        if (text_equals(value, "-Wl,--gc-sections")) {
            append_link_arg(argv, count, capacity, "-Wl,-dead_strip");
            return;
        }
        if (text_equals(value, "-Wl,--no-gc-sections")) {
            return;
        }
    }
    append_link_arg(argv, count, capacity, value);
}

static void cleanup_temp_paths(char paths[][COMPILER_PATH_CAPACITY], size_t count) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (paths[i][0] != '\0') {
            (void)platform_remove_file(paths[i]);
        }
    }
}

static size_t count_c_inputs(const CompilerOptions *options) {
    size_t count = 0;
    size_t i;

    for (i = 0; i < options->input_count; ++i) {
        if (is_c_input(options->input_paths[i])) {
            count += 1U;
        }
    }
    return count;
}

static int driver_ir_reserve_lines(CompilerIr *ir, size_t needed) {
    size_t new_capacity;
    char **new_lines;

    if (needed <= ir->capacity) {
        return 0;
    }
    new_capacity = ir->capacity > 0U ? ir->capacity : 64U;
    while (new_capacity < needed) {
        if (new_capacity > ((size_t)-1) / 2U) {
            return -1;
        }
        new_capacity *= 2U;
    }
    new_lines = (char **)rt_realloc_array(ir->lines, new_capacity, sizeof(char *));
    if (new_lines == 0) {
        return -1;
    }
    ir->lines = new_lines;
    ir->capacity = new_capacity;
    return 0;
}

static int driver_ir_append_line(CompilerIr *dst, const char *line) {
    size_t length = rt_strlen(line);
    char *copy;

    if (driver_ir_reserve_lines(dst, dst->count + 1U) != 0) {
        return -1;
    }
    copy = (char *)rt_malloc(length + 1U);
    if (copy == 0) {
        return -1;
    }
    memcpy(copy, line, length + 1U);
    dst->lines[dst->count++] = copy;
    return 0;
}

static int driver_ir_append_module(CompilerIr *dst, const CompilerIr *src) {
    size_t i;

    for (i = 0; i < src->count; ++i) {
        if (driver_ir_append_line(dst, src->lines[i]) != 0) {
            return -1;
        }
    }
    if (dst->label_counter < src->label_counter) {
        dst->label_counter = src->label_counter;
    }
    if (dst->temp_counter < src->temp_counter) {
        dst->temp_counter = src->temp_counter;
    }
    return 0;
}

static int copy_last_word(const char *text, char *buffer, size_t buffer_size) {
    const char *word = text;
    const char *cursor = text;
    size_t length;

    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t') {
            cursor += 1;
        }
        if (*cursor == '\0') {
            break;
        }
        word = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
            cursor += 1;
        }
    }

    length = 0U;
    while (word[length] != '\0' && word[length] != ' ' && word[length] != '\t') {
        if (length + 1U >= buffer_size) {
            return -1;
        }
        buffer[length] = word[length];
        length += 1U;
    }
    buffer[length] = '\0';
    return length > 0U ? 0 : -1;
}

static int lto_name_set_add(CompilerLtoNameSet *set, const char *name) {
    size_t i;

    for (i = 0; i < set->count; ++i) {
        if (text_equals(set->names[i], name)) {
            return 1;
        }
    }
    if (set->count >= COMPILER_LTO_MAX_TRACKED_NAMES) {
        return -1;
    }
    rt_copy_string(set->names[set->count], sizeof(set->names[set->count]), name);
    set->count += 1U;
    return 0;
}

static int lto_identifier_char(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_';
}

static int lto_identifier_start(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           ch == '_';
}

static int lto_append_span(char *buffer, size_t buffer_size, size_t *offset, const char *text, size_t length) {
    if (*offset + length + 1U > buffer_size) {
        return -1;
    }
    memcpy(buffer + *offset, text, length);
    *offset += length;
    buffer[*offset] = '\0';
    return 0;
}

static int lto_append_text(char *buffer, size_t buffer_size, size_t *offset, const char *text) {
    return lto_append_span(buffer, buffer_size, offset, text, rt_strlen(text));
}

static const char *lto_find_text(const char *text, const char *needle) {
    size_t needle_length = rt_strlen(needle);

    if (needle_length == 0U) {
        return text;
    }
    while (*text != '\0') {
        size_t i = 0;
        while (i < needle_length && text[i] == needle[i]) {
            i += 1U;
        }
        if (i == needle_length) {
            return text;
        }
        text += 1;
    }
    return 0;
}

static const char *lto_find_separator_outside_quotes(const char *text, const char *separator) {
    const char *cursor = text;
    size_t separator_length = rt_strlen(separator);
    int in_string = 0;
    int in_char = 0;

    while (*cursor != '\0') {
        if ((in_string || in_char) && *cursor == '\\' && cursor[1] != '\0') {
            cursor += 2;
            continue;
        }
        if (!in_char && *cursor == '"') {
            in_string = !in_string;
            cursor += 1;
            continue;
        }
        if (!in_string && *cursor == '\'') {
            in_char = !in_char;
            cursor += 1;
            continue;
        }
        if (!in_string && !in_char) {
            size_t i = 0;
            while (i < separator_length && cursor[i] == separator[i]) {
                i += 1U;
            }
            if (i == separator_length) {
                return cursor;
            }
        }
        cursor += 1;
    }
    return 0;
}

static const char *lto_rename_lookup_span(const CompilerLtoRenameMap *map, const char *name, size_t length) {
    size_t i;

    for (i = 0; i < map->count; ++i) {
        if (rt_strlen(map->old_names[i]) == length && rt_strncmp(map->old_names[i], name, length) == 0) {
            return map->new_names[i];
        }
    }
    return 0;
}

static const char *lto_rename_lookup(const CompilerLtoRenameMap *map, const char *name) {
    return lto_rename_lookup_span(map, name, rt_strlen(name));
}

static int lto_rename_map_add(CompilerLtoRenameMap *map, const char *name, size_t module_index) {
    char module_text[32];
    char ordinal_text[32];
    size_t offset = 0;
    size_t i;

    for (i = 0; i < map->count; ++i) {
        if (text_equals(map->old_names[i], name)) {
            return 0;
        }
    }
    if (map->count >= COMPILER_LTO_MAX_TRACKED_NAMES) {
        return -1;
    }

    rt_unsigned_to_string((unsigned long long)module_index, module_text, sizeof(module_text));
    rt_unsigned_to_string((unsigned long long)map->count, ordinal_text, sizeof(ordinal_text));
    rt_copy_string(map->old_names[map->count], sizeof(map->old_names[map->count]), name);
    map->new_names[map->count][0] = '\0';
    if (lto_append_text(map->new_names[map->count], sizeof(map->new_names[map->count]), &offset, "__ncc_lto_") != 0 ||
        lto_append_text(map->new_names[map->count], sizeof(map->new_names[map->count]), &offset, module_text) != 0 ||
        lto_append_text(map->new_names[map->count], sizeof(map->new_names[map->count]), &offset, "_") != 0 ||
        lto_append_text(map->new_names[map->count], sizeof(map->new_names[map->count]), &offset, ordinal_text) != 0) {
        return -1;
    }
    map->count += 1U;
    return 0;
}

static int lto_collect_static_renames(const CompilerIr *module, CompilerLtoRenameMap *map, size_t module_index) {
    size_t i;

    for (i = 0; i < module->count; ++i) {
        const char *line = module->lines[i];
        char name[COMPILER_IR_NAME_CAPACITY];

        if (starts_with(line, "decl static func ") || starts_with(line, "decl static obj ")) {
            if (copy_last_word(line, name, sizeof(name)) != 0 || lto_rename_map_add(map, name, module_index) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int lto_module_has_shadowed_static_name(const CompilerIr *module, const CompilerLtoRenameMap *map) {
    size_t i;

    for (i = 0; i < module->count; ++i) {
        const char *line = module->lines[i];
        char name[COMPILER_IR_NAME_CAPACITY];

        if (starts_with(line, "decl local ") || starts_with(line, "decl param ")) {
            if (copy_last_word(line, name, sizeof(name)) == 0 && lto_rename_lookup(map, name) != 0) {
                return 1;
            }
        }
    }
    return 0;
}

static int lto_replace_ir_line(CompilerIr *ir, size_t index, const char *line) {
    char *copy;
    size_t length = rt_strlen(line);

    copy = (char *)rt_malloc(length + 1U);
    if (copy == 0) {
        return -1;
    }
    memcpy(copy, line, length + 1U);
    rt_free(ir->lines[index]);
    ir->lines[index] = copy;
    return 0;
}

static int lto_rewrite_expr_identifiers(const CompilerLtoRenameMap *map, const char *expr, const char *end, char *buffer, size_t buffer_size, int *changed_out) {
    size_t offset = 0;
    const char *cursor = expr;
    int in_string = 0;
    int in_char = 0;

    *changed_out = 0;
    buffer[0] = '\0';
    while (*cursor != '\0' && (end == 0 || cursor < end)) {
        if ((in_string || in_char) && *cursor == '\\' && cursor[1] != '\0' && (end == 0 || cursor + 1 < end)) {
            if (lto_append_span(buffer, buffer_size, &offset, cursor, 2U) != 0) {
                return -1;
            }
            cursor += 2;
            continue;
        }
        if (!in_char && *cursor == '"') {
            in_string = !in_string;
            if (lto_append_span(buffer, buffer_size, &offset, cursor, 1U) != 0) {
                return -1;
            }
            cursor += 1;
            continue;
        }
        if (!in_string && *cursor == '\'') {
            in_char = !in_char;
            if (lto_append_span(buffer, buffer_size, &offset, cursor, 1U) != 0) {
                return -1;
            }
            cursor += 1;
            continue;
        }
        if (!in_string && !in_char && lto_identifier_start(*cursor)) {
            const char *start = cursor;
            const char *replacement;
            while (*cursor != '\0' && (end == 0 || cursor < end) && lto_identifier_char(*cursor)) {
                cursor += 1;
            }
            replacement = lto_rename_lookup_span(map, start, (size_t)(cursor - start));
            if (replacement != 0 && start > expr && (start[-1] == '.' || start[-1] == '>')) {
                replacement = 0;
            }
            if (replacement != 0) {
                if (lto_append_text(buffer, buffer_size, &offset, replacement) != 0) {
                    return -1;
                }
                *changed_out = 1;
            } else if (lto_append_span(buffer, buffer_size, &offset, start, (size_t)(cursor - start)) != 0) {
                return -1;
            }
            continue;
        }
        if (lto_append_span(buffer, buffer_size, &offset, cursor, 1U) != 0) {
            return -1;
        }
        cursor += 1;
    }
    return 0;
}

static int lto_rewrite_prefixed_expr_line(CompilerIr *ir, size_t index, const CompilerLtoRenameMap *map, const char *prefix) {
    char expr[COMPILER_IR_LINE_CAPACITY];
    char line[COMPILER_IR_LINE_CAPACITY];
    size_t offset = 0;
    int changed = 0;

    if (lto_rewrite_expr_identifiers(map, ir->lines[index] + rt_strlen(prefix), 0, expr, sizeof(expr), &changed) != 0) {
        return -1;
    }
    if (!changed) {
        return 0;
    }
    line[0] = '\0';
    if (lto_append_text(line, sizeof(line), &offset, prefix) != 0 ||
        lto_append_text(line, sizeof(line), &offset, expr) != 0) {
        return -1;
    }
    return lto_replace_ir_line(ir, index, line);
}

static int lto_rewrite_decl_line(CompilerIr *ir, size_t index, const CompilerLtoRenameMap *map) {
    const char *line = ir->lines[index];
    const char *cursor = line;
    const char *word = line;
    const char *replacement;
    char rewritten[COMPILER_IR_LINE_CAPACITY];
    size_t offset = 0;

    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t') {
            cursor += 1;
        }
        if (*cursor == '\0') {
            break;
        }
        word = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
            cursor += 1;
        }
    }

    replacement = lto_rename_lookup_span(map, word, rt_strlen(word));
    if (replacement == 0) {
        return 0;
    }
    rewritten[0] = '\0';
    if (lto_append_span(rewritten, sizeof(rewritten), &offset, line, (size_t)(word - line)) != 0 ||
        lto_append_text(rewritten, sizeof(rewritten), &offset, replacement) != 0) {
        return -1;
    }
    return lto_replace_ir_line(ir, index, rewritten);
}

static int lto_rewrite_function_boundary_line(CompilerIr *ir, size_t index, const CompilerLtoRenameMap *map, int begin) {
    const char *line = ir->lines[index];
    const char *name = line + (begin ? 5 : 8);
    const char *suffix = begin ? lto_find_text(name, " : ") : name + rt_strlen(name);
    const char *replacement;
    char rewritten[COMPILER_IR_LINE_CAPACITY];
    size_t offset = 0;

    if (suffix == 0) {
        return 0;
    }
    replacement = lto_rename_lookup_span(map, name, (size_t)(suffix - name));
    if (replacement == 0) {
        return 0;
    }
    rewritten[0] = '\0';
    if (lto_append_span(rewritten, sizeof(rewritten), &offset, line, (size_t)(name - line)) != 0 ||
        lto_append_text(rewritten, sizeof(rewritten), &offset, replacement) != 0 ||
        lto_append_text(rewritten, sizeof(rewritten), &offset, suffix) != 0) {
        return -1;
    }
    return lto_replace_ir_line(ir, index, rewritten);
}

static int lto_rewrite_store_line(CompilerIr *ir, size_t index, const CompilerLtoRenameMap *map) {
    const char *line = ir->lines[index];
    const char *name = line + 6;
    const char *expr = lto_find_text(name, " <- ");
    const char *replacement;
    char rewritten_expr[COMPILER_IR_LINE_CAPACITY];
    char rewritten[COMPILER_IR_LINE_CAPACITY];
    size_t offset = 0;
    int expr_changed = 0;

    if (expr == 0) {
        return 0;
    }
    replacement = lto_rename_lookup_span(map, name, (size_t)(expr - name));
    expr += 4;
    if (lto_rewrite_expr_identifiers(map, expr, 0, rewritten_expr, sizeof(rewritten_expr), &expr_changed) != 0) {
        return -1;
    }
    if (replacement == 0 && !expr_changed) {
        return 0;
    }
    rewritten[0] = '\0';
    if (replacement == 0) {
        size_t name_length = (size_t)((expr - 4) - name);
        if (lto_append_text(rewritten, sizeof(rewritten), &offset, "store ") != 0 ||
            lto_append_span(rewritten, sizeof(rewritten), &offset, name, name_length) != 0 ||
            lto_append_text(rewritten, sizeof(rewritten), &offset, " <- ") != 0 ||
            lto_append_text(rewritten, sizeof(rewritten), &offset, rewritten_expr) != 0) {
            return -1;
        }
    } else if (lto_append_text(rewritten, sizeof(rewritten), &offset, "store ") != 0 ||
               lto_append_text(rewritten, sizeof(rewritten), &offset, replacement) != 0 ||
               lto_append_text(rewritten, sizeof(rewritten), &offset, " <- ") != 0 ||
               lto_append_text(rewritten, sizeof(rewritten), &offset, rewritten_expr) != 0) {
        return -1;
    }
    return lto_replace_ir_line(ir, index, rewritten);
}

static int lto_rewrite_branch_line(CompilerIr *ir, size_t index, const CompilerLtoRenameMap *map) {
    const char *line = ir->lines[index];
    const char *expr = line + 8;
    const char *arrow = lto_find_separator_outside_quotes(expr, " -> ");
    char rewritten_expr[COMPILER_IR_LINE_CAPACITY];
    char rewritten[COMPILER_IR_LINE_CAPACITY];
    size_t offset = 0;
    int changed = 0;

    if (arrow == 0) {
        return 0;
    }
    if (lto_rewrite_expr_identifiers(map, expr, arrow, rewritten_expr, sizeof(rewritten_expr), &changed) != 0) {
        return -1;
    }
    if (!changed) {
        return 0;
    }
    rewritten[0] = '\0';
    if (lto_append_text(rewritten, sizeof(rewritten), &offset, "brfalse ") != 0 ||
        lto_append_text(rewritten, sizeof(rewritten), &offset, rewritten_expr) != 0 ||
        lto_append_text(rewritten, sizeof(rewritten), &offset, arrow) != 0) {
        return -1;
    }
    return lto_replace_ir_line(ir, index, rewritten);
}

static int lto_rewrite_module_line(CompilerIr *module, size_t index, const CompilerLtoRenameMap *map) {
    const char *line = module->lines[index];

    if (starts_with(line, "decl ")) {
        return lto_rewrite_decl_line(module, index, map);
    }
    if (starts_with(line, "func ")) {
        return lto_rewrite_function_boundary_line(module, index, map, 1);
    }
    if (starts_with(line, "endfunc ")) {
        return lto_rewrite_function_boundary_line(module, index, map, 0);
    }
    if (starts_with(line, "store ")) {
        return lto_rewrite_store_line(module, index, map);
    }
    if (starts_with(line, "eval ")) {
        return lto_rewrite_prefixed_expr_line(module, index, map, "eval ");
    }
    if (starts_with(line, "ret ")) {
        return lto_rewrite_prefixed_expr_line(module, index, map, "ret ");
    }
    if (starts_with(line, "brfalse ")) {
        return lto_rewrite_branch_line(module, index, map);
    }
    if (starts_with(line, "case ")) {
        return lto_rewrite_prefixed_expr_line(module, index, map, "case ");
    }
    if (starts_with(line, "switch ")) {
        return lto_rewrite_prefixed_expr_line(module, index, map, "switch ");
    }
    return 0;
}

static int lto_rename_module_internal_symbols(CompilerIr *module, size_t module_index) {
    CompilerLtoRenameMap map;
    size_t i;

    rt_memset(&map, 0, sizeof(map));
    if (lto_collect_static_renames(module, &map, module_index) != 0) {
        return 1;
    }
    if (map.count == 0U) {
        return 0;
    }
    if (lto_module_has_shadowed_static_name(module, &map)) {
        return 1;
    }
    for (i = 0; i < module->count; ++i) {
        if (lto_rewrite_module_line(module, i, &map) != 0) {
            return 1;
        }
    }
    return 0;
}

static int lto_module_is_safe_to_merge(const CompilerIr *module, CompilerLtoNameSet *defined_names) {
    size_t i;

    for (i = 0; i < module->count; ++i) {
        const char *line = module->lines[i];
        char name[COMPILER_IR_NAME_CAPACITY];

        if (starts_with(line, "func ")) {
            const char *cursor = line + 5;
            size_t out = 0;

            while (cursor[out] != '\0' && !(cursor[out] == ' ' && cursor[out + 1U] == ':') && out + 1U < sizeof(name)) {
                name[out] = cursor[out];
                out += 1U;
            }
            name[out] = '\0';
            if (lto_name_set_add(defined_names, name) != 0) {
                return 0;
            }
            continue;
        }

        if (starts_with(line, "decl static obj ") || starts_with(line, "decl global obj ")) {
            if (copy_last_word(line, name, sizeof(name)) != 0 || lto_name_set_add(defined_names, name) != 0) {
                return 0;
            }
            continue;
        }

        if (starts_with(line, "decl local_static ")) {
            return 0;
        }
    }
    return 1;
}

static int compile_input_to_object(
    const CompilerOptions *options,
    const char *input_path,
    char *path_buffer,
    size_t path_buffer_size
) {
    CompilerOptions compile_options;
    int fd;

    fd = platform_create_temp_file(path_buffer, path_buffer_size, "/tmp/newos-ncc-link-", 0600U);
    if (fd < 0) {
        tool_write_error(options->program_name, "failed to create temporary object for ", input_path);
        return 1;
    }
    if (platform_close(fd) != 0) {
        (void)platform_remove_file(path_buffer);
        tool_write_error(options->program_name, "failed to finalize temporary object for ", input_path);
        return 1;
    }

    {
        size_t length = rt_strlen(path_buffer);
        if (length + 3U >= path_buffer_size) {
            (void)platform_remove_file(path_buffer);
            tool_write_error(options->program_name, "temporary object path too long for ", input_path);
            return 1;
        }
        (void)platform_remove_file(path_buffer);
        path_buffer[length] = '.';
        path_buffer[length + 1] = 'o';
        path_buffer[length + 2] = '\0';
    }

    compile_options = *options;
    compile_options.input_path = input_path;
    compile_options.output_path = path_buffer;
    compile_options.compile_only = 1;
    compile_options.syntax_only = 0;
    compile_options.dump_tokens = 0;
    compile_options.preprocess_only = 0;
    compile_options.dump_ast = 0;
    compile_options.dump_ir = 0;
    compile_options.emit_assembly = 0;

    if (parse_translation_unit(&compile_options) != 0) {
        (void)platform_remove_file(path_buffer);
        return 1;
    }

    return 0;
}

static int parse_lto_input_to_ir(
    const CompilerOptions *options,
    const char *input_path,
    CompilerIr *combined_ir,
    CompilerLtoNameSet *defined_names,
    size_t module_index
) {
    CompilerOptions parse_options = *options;
    CompilerPreprocessor preprocessor;
    static CompilerParser parser;
    static CompilerSource source;

    parse_options.input_path = input_path;
    parse_options.output_path = 0;
    parse_options.input_paths[0] = input_path;
    parse_options.input_count = 1U;
    parse_options.compile_only = 0;
    parse_options.syntax_only = 0;
    parse_options.dump_tokens = 0;
    parse_options.preprocess_only = 0;
    parse_options.dump_ast = 0;
    parse_options.dump_ir = 0;
    parse_options.emit_assembly = 0;

    if (configure_preprocessor(&preprocessor, &parse_options) != 0) {
        tool_write_error(options->program_name, "failed to initialize preprocessor for target ", target_name(options->target));
        return 1;
    }
    if (compiler_preprocess_file(&preprocessor, input_path, &source) != 0) {
        write_preprocessor_error(options->program_name, &preprocessor);
        return 1;
    }

    compiler_ir_destroy(&parser.ir);
    compiler_parser_init(&parser, &source, 0, 0, -1);
    parser.ir.label_counter = combined_ir->label_counter;
    parser.ir.temp_counter = combined_ir->temp_counter;
    if (compiler_parse_translation_unit(&parser) != 0) {
        rt_write_cstr(2, options->program_name);
        rt_write_cstr(2, ": ");
        rt_write_cstr(2, source.path);
        rt_write_char(2, ':');
        rt_write_uint(2, compiler_parser_error_line(&parser));
        rt_write_char(2, ':');
        rt_write_uint(2, compiler_parser_error_column(&parser));
        rt_write_cstr(2, ": ");
        rt_write_line(2, compiler_parser_error_message(&parser));
        return 1;
    }
    if (compiler_ir_optimize(&parser.ir) != 0) {
        tool_write_error(options->program_name, "failed while optimizing LTO IR: ", compiler_ir_error_message(&parser.ir));
        return 1;
    }
    {
        int rename_status = lto_rename_module_internal_symbols(&parser.ir, module_index);
        if (rename_status < 0) {
            tool_write_error(options->program_name, "failed while renaming LTO internal symbols for ", input_path);
            return 1;
        }
        if (rename_status > 0) {
            return 2;
        }
    }
    if (!lto_module_is_safe_to_merge(&parser.ir, defined_names)) {
        return 2;
    }
    if (driver_ir_append_module(combined_ir, &parser.ir) != 0) {
        tool_write_error(options->program_name, "failed while appending LTO IR for ", input_path);
        return 1;
    }
    return 0;
}

static CompilerLtoBuildStatus compile_c_inputs_to_lto_object(
    const CompilerOptions *options,
    char *path_buffer,
    size_t path_buffer_size
) {
    CompilerObjectWriter writer;
    CompilerLtoNameSet defined_names;
    CompilerIr combined_ir;
    int fd;
    size_t i;

    rt_memset(&defined_names, 0, sizeof(defined_names));
    compiler_ir_init(&combined_ir);

    fd = platform_create_temp_file(path_buffer, path_buffer_size, "/tmp/newos-ncc-lto-", 0600U);
    if (fd < 0) {
        compiler_ir_destroy(&combined_ir);
        tool_write_error(options->program_name, "failed to create temporary LTO object", "");
        return COMPILER_LTO_BUILD_FAILED;
    }
    if (platform_close(fd) != 0) {
        (void)platform_remove_file(path_buffer);
        compiler_ir_destroy(&combined_ir);
        tool_write_error(options->program_name, "failed to finalize temporary LTO object", "");
        return COMPILER_LTO_BUILD_FAILED;
    }

    {
        size_t length = rt_strlen(path_buffer);
        if (length + 3U >= path_buffer_size) {
            (void)platform_remove_file(path_buffer);
            compiler_ir_destroy(&combined_ir);
            tool_write_error(options->program_name, "temporary LTO object path too long", "");
            return COMPILER_LTO_BUILD_FAILED;
        }
        (void)platform_remove_file(path_buffer);
        path_buffer[length] = '.';
        path_buffer[length + 1U] = 'o';
        path_buffer[length + 2U] = '\0';
    }

    for (i = 0; i < options->input_count; ++i) {
        int parse_status;

        if (!is_c_input(options->input_paths[i])) {
            continue;
        }
        parse_status = parse_lto_input_to_ir(options, options->input_paths[i], &combined_ir, &defined_names, i);
        if (parse_status == 2) {
            compiler_ir_destroy(&combined_ir);
            return COMPILER_LTO_BUILD_FALLBACK;
        }
        if (parse_status != 0) {
            compiler_ir_destroy(&combined_ir);
            return COMPILER_LTO_BUILD_FAILED;
        }
    }

    if (compiler_ir_optimize(&combined_ir) != 0) {
        tool_write_error(options->program_name, "failed while optimizing combined LTO IR: ", compiler_ir_error_message(&combined_ir));
        compiler_ir_destroy(&combined_ir);
        return COMPILER_LTO_BUILD_FAILED;
    }

    fd = platform_open_write(path_buffer, 0644U);
    if (fd < 0) {
        compiler_ir_destroy(&combined_ir);
        tool_write_error(options->program_name, "cannot open temporary LTO object ", path_buffer);
        return COMPILER_LTO_BUILD_FAILED;
    }
    compiler_object_writer_init(&writer);
    if (compiler_object_write_target(&writer, options->target, &combined_ir, fd, options->function_sections, options->data_sections) != 0) {
        (void)platform_close(fd);
        (void)platform_remove_file(path_buffer);
        compiler_ir_destroy(&combined_ir);
        tool_write_error(options->program_name, "failed while writing LTO object output: ", compiler_object_writer_error_message(&writer));
        return COMPILER_LTO_BUILD_FAILED;
    }
    if (platform_close(fd) != 0) {
        (void)platform_remove_file(path_buffer);
        compiler_ir_destroy(&combined_ir);
        tool_write_error(options->program_name, "failed to finalize temporary LTO object ", path_buffer);
        return COMPILER_LTO_BUILD_FAILED;
    }

    compiler_ir_destroy(&combined_ir);
    return COMPILER_LTO_BUILD_OK;
}

static int assemble_input_to_object(
    const CompilerOptions *options,
    const char *input_path,
    char *path_buffer,
    size_t path_buffer_size
) {
    CompilerObjectWriter writer;
    int fd;

    fd = platform_create_temp_file(path_buffer, path_buffer_size, "/tmp/newos-ncc-asmobj-", 0600U);
    if (fd < 0) {
        tool_write_error(options->program_name, "failed to create temporary assembly object for ", input_path);
        return 1;
    }

    if (options->target != COMPILER_TARGET_LINUX_X86_64 || compiler_object_assemble_elf64_x86_64(&writer, input_path, fd) != 0) {
        (void)platform_close(fd);
        (void)platform_remove_file(path_buffer);
        tool_write_error(options->program_name, compiler_object_writer_error_message(&writer), "");
        return 1;
    }
    if (platform_close(fd) != 0) {
        (void)platform_remove_file(path_buffer);
        tool_write_error(options->program_name, "failed to finalize temporary assembly object for ", input_path);
        return 1;
    }

    return 0;
}

static int can_use_native_static_linker(const CompilerOptions *options) {
    return options->target == COMPILER_TARGET_LINUX_X86_64 && options->no_stdlib && options->static_link;
}

static int link_executable_output_native(const CompilerOptions *options) {
    char derived_output_path[COMPILER_PATH_CAPACITY];
    char temp_paths[COMPILER_MAX_INPUT_FILES][COMPILER_PATH_CAPACITY];
    const char *object_paths[COMPILER_MAX_INPUT_FILES];
    CompilerLinkerOptions link_options;
    char native_entry_symbol[COMPILER_IR_NAME_CAPACITY];
    char native_lto_cc[COMPILER_PATH_CAPACITY];
    size_t object_count = 0;
    size_t temp_count = 0;
    size_t i;
    int use_lto_object = 0;
    char error[COMPILER_ERROR_CAPACITY];
    const char *output_path = pick_link_output_path(options, derived_output_path, sizeof(derived_output_path));

    rt_memset(temp_paths, 0, sizeof(temp_paths));
    native_entry_symbol[0] = '\0';
    native_lto_cc[0] = '\0';
    configure_native_linker_options(options, &link_options, native_entry_symbol, sizeof(native_entry_symbol), native_lto_cc, sizeof(native_lto_cc));

    if (options->lto && count_c_inputs(options) > 1U) {
        CompilerLtoBuildStatus lto_status = compile_c_inputs_to_lto_object(options, temp_paths[temp_count], sizeof(temp_paths[temp_count]));

        if (lto_status == COMPILER_LTO_BUILD_FAILED) {
            cleanup_temp_paths(temp_paths, temp_count + 1U);
            return 1;
        }
        if (lto_status == COMPILER_LTO_BUILD_OK) {
            object_paths[object_count++] = temp_paths[temp_count++];
            use_lto_object = 1;
        }
    }

    for (i = 0; i < options->input_count; ++i) {
        const char *input_path = options->input_paths[i];

        if (use_lto_object && is_c_input(input_path)) {
            continue;
        }

        if (ends_with(input_path, ".o")) {
            object_paths[object_count++] = input_path;
            continue;
        }
        if (ends_with(input_path, ".a")) {
            object_paths[object_count++] = input_path;
            continue;
        }
        if (is_assembly_input(input_path)) {
            if (temp_count >= COMPILER_MAX_INPUT_FILES || assemble_input_to_object(options, input_path, temp_paths[temp_count], sizeof(temp_paths[temp_count])) != 0) {
                cleanup_temp_paths(temp_paths, temp_count + 1U);
                return 1;
            }
            object_paths[object_count++] = temp_paths[temp_count++];
            continue;
        }
        if (!is_c_input(input_path)) {
            cleanup_temp_paths(temp_paths, temp_count);
            tool_write_error(options->program_name, "unsupported native linker input ", input_path);
            return 1;
        }
        if (temp_count >= COMPILER_MAX_INPUT_FILES || compile_input_to_object(options, input_path, temp_paths[temp_count], sizeof(temp_paths[temp_count])) != 0) {
            cleanup_temp_paths(temp_paths, temp_count + 1U);
            return 1;
        }
        object_paths[object_count++] = temp_paths[temp_count++];
    }

    if (compiler_link_elf64_x86_64_static_options(object_paths, object_count, output_path, &link_options, error, sizeof(error)) != 0) {
        cleanup_temp_paths(temp_paths, temp_count);
        tool_write_error(options->program_name, error[0] != '\0' ? error : "native link failed", "");
        return 1;
    }

    cleanup_temp_paths(temp_paths, temp_count);
    return 0;
}

static int link_executable_output(const CompilerOptions *options) {
    char derived_output_path[COMPILER_PATH_CAPACITY];
    char temp_paths[COMPILER_MAX_INPUT_FILES][COMPILER_PATH_CAPACITY];
    const char *output_path = pick_link_output_path(options, derived_output_path, sizeof(derived_output_path));
    const char *link_driver = platform_getenv("NEWOS_NCC_LINKER");
    char *argv[COMPILER_MAX_LINK_ARGS];
    size_t argc = 0;
    size_t temp_count = 0;
    size_t i;
    int use_lto_object = 0;
    int pid = -1;
    int exit_status = 0;

    if (can_use_native_static_linker(options)) {
        return link_executable_output_native(options);
    }

    rt_memset(temp_paths, 0, sizeof(temp_paths));

    if (link_driver == 0 || link_driver[0] == '\0') {
        const char *cc_driver = platform_getenv("CC");
        if (!looks_like_ncc_driver(cc_driver)) {
            link_driver = cc_driver;
        }
    }
    if (link_driver == 0 || link_driver[0] == '\0') {
        link_driver = "cc";
    }

    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), link_driver);
    if (options->freestanding) {
        append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-ffreestanding");
    }
    if (options->no_stdlib) {
        append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-nostdlib");
    }
    if (options->static_link) {
        append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-static");
    }
    if (options->lto) {
        append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-flto");
    }
    for (i = 0; i < options->extra_link_arg_count; ++i) {
        append_target_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), options->target, options->extra_link_args[i]);
    }

    if (options->lto && count_c_inputs(options) > 1U) {
        CompilerLtoBuildStatus lto_status = compile_c_inputs_to_lto_object(options, temp_paths[temp_count], sizeof(temp_paths[temp_count]));

        if (lto_status == COMPILER_LTO_BUILD_FAILED) {
            cleanup_temp_paths(temp_paths, temp_count + 1U);
            return 1;
        }
        if (lto_status == COMPILER_LTO_BUILD_OK) {
            append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), temp_paths[temp_count]);
            temp_count += 1U;
            use_lto_object = 1;
        }
    }

    for (i = 0; i < options->input_count; ++i) {
        const char *input_path = options->input_paths[i];

        if (use_lto_object && is_c_input(input_path)) {
            continue;
        }

        if (is_direct_link_input(input_path)) {
            append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), input_path);
            continue;
        }

        if (!is_c_input(input_path)) {
            cleanup_temp_paths(temp_paths, temp_count);
            tool_write_error(options->program_name, "unsupported linker input ", input_path);
            return 1;
        }

        if (temp_count >= COMPILER_MAX_INPUT_FILES || compile_input_to_object(options, input_path, temp_paths[temp_count], sizeof(temp_paths[temp_count])) != 0) {
            cleanup_temp_paths(temp_paths, temp_count + 1U);
            return 1;
        }

        append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), temp_paths[temp_count]);
        temp_count += 1U;
    }

    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-o");
    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), output_path);
    argv[argc] = 0;

    if (platform_spawn_process(argv, -1, -1, 0, 0, 0, &pid) != 0) {
        cleanup_temp_paths(temp_paths, temp_count);
        tool_write_error(options->program_name, "failed to invoke linker driver for ", target_name(options->target));
        return 1;
    }
    if (platform_wait_process(pid, &exit_status) != 0) {
        cleanup_temp_paths(temp_paths, temp_count);
        tool_write_error(options->program_name, "failed while waiting for linker driver for ", target_name(options->target));
        return 1;
    }

    cleanup_temp_paths(temp_paths, temp_count);

    if (exit_status != 0) {
        tool_write_error(options->program_name, "linking failed for target ", target_name(options->target));
        return 1;
    }

    return 0;
}

static int add_include_dir(CompilerOptions *options, const char *path) {
    if (options->include_dir_count >= COMPILER_MAX_INCLUDE_DIRS) {
        return -1;
    }

    rt_copy_string(options->include_dirs[options->include_dir_count], sizeof(options->include_dirs[options->include_dir_count]), path);
    options->include_dir_count += 1U;
    return 0;
}

static int add_define(CompilerOptions *options, const char *spec) {
    size_t i = 0;
    size_t name_length = 0;
    size_t value_length = 0;

    if (options->define_count >= COMPILER_MAX_OPTION_DEFINES) {
        return -1;
    }

    while (spec[i] != '\0' && spec[i] != '=' && name_length + 1 < sizeof(options->define_names[0])) {
        options->define_names[options->define_count][name_length++] = spec[i++];
    }
    options->define_names[options->define_count][name_length] = '\0';

    if (options->define_names[options->define_count][0] == '\0') {
        return -1;
    }

    if (spec[i] == '=') {
        i += 1;
        while (spec[i] != '\0' && value_length + 1 < sizeof(options->define_values[0])) {
            options->define_values[options->define_count][value_length++] = spec[i++];
        }
    } else {
        options->define_values[options->define_count][0] = '1';
        value_length = 1;
    }
    options->define_values[options->define_count][value_length] = '\0';

    options->define_count += 1U;
    return 0;
}

static int parse_options(int argc, char **argv, CompilerOptions *options) {
    int i;

    rt_memset(options, 0, sizeof(*options));
    options->program_name = (argc > 0) ? tool_base_name(argv[0]) : "ncc";
    options->target = default_target();

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (text_equals(arg, "--help")) {
            write_usage(options->program_name);
            return 1;
        }
        if (text_equals(arg, "--version")) {
            rt_write_line(1, "ncc stage0");
            return 2;
        }
        if (text_equals(arg, "--dump-tokens") || text_equals(arg, "--lex")) {
            options->dump_tokens = 1;
            continue;
        }
        if (text_equals(arg, "--dump-ast") || text_equals(arg, "--parse")) {
            options->dump_ast = 1;
            continue;
        }
        if (text_equals(arg, "--dump-ir") || text_equals(arg, "--ir")) {
            options->dump_ir = 1;
            continue;
        }
        if (text_equals(arg, "--time") || text_equals(arg, "-ftime-report")) {
            options->time_report = 1;
            continue;
        }
        if (text_equals(arg, "--compile-manifest")) {
            if (i + 1 >= argc) {
                tool_write_error(options->program_name, "missing path after ", "--compile-manifest");
                return -1;
            }
            options->compile_manifest_path = argv[++i];
            options->compile_only = 1;
            continue;
        }
        if (starts_with(arg, "--compile-manifest=")) {
            options->compile_manifest_path = arg + 19;
            options->compile_only = 1;
            continue;
        }
        if (text_equals(arg, "-S") || text_equals(arg, "--emit-asm")) {
            options->emit_assembly = 1;
            continue;
        }
        if (text_equals(arg, "-E") || text_equals(arg, "--preprocess")) {
            options->preprocess_only = 1;
            continue;
        }
        if (text_equals(arg, "-c")) {
            options->compile_only = 1;
            continue;
        }
        if (text_equals(arg, "-fsyntax-only")) {
            options->syntax_only = 1;
            continue;
        }
        if (text_equals(arg, "-ffreestanding")) {
            options->freestanding = 1;
            continue;
        }
        if (text_equals(arg, "-ffunction-sections")) {
            options->function_sections = 1;
            continue;
        }
        if (text_equals(arg, "-fdata-sections")) {
            options->data_sections = 1;
            continue;
        }
        if (text_equals(arg, "-flto") || starts_with(arg, "-flto=") || text_equals(arg, "--lto")) {
            options->lto = 1;
            continue;
        }
        if (text_equals(arg, "-fno-function-sections")) {
            options->function_sections = 0;
            continue;
        }
        if (text_equals(arg, "-fno-data-sections")) {
            options->data_sections = 0;
            continue;
        }
        if (text_equals(arg, "-nostdlib")) {
            options->no_stdlib = 1;
            continue;
        }
        if (text_equals(arg, "-static")) {
            options->static_link = 1;
            continue;
        }
        if (starts_with(arg, "-Wl,")) {
            if (add_extra_link_arg(options, arg) != 0) {
                tool_write_error(options->program_name, "too many linker options; last one was ", arg);
                return -1;
            }
            continue;
        }
        if (starts_with(arg, "-fuse-ld=")) {
            if (add_extra_link_arg(options, arg) != 0) {
                tool_write_error(options->program_name, "too many linker options; last one was ", arg);
                return -1;
            }
            continue;
        }
        if (text_equals(arg, "-I")) {
            if (i + 1 >= argc || add_include_dir(options, argv[++i]) != 0) {
                tool_write_error(options->program_name, "invalid include directory ", (i < argc) ? argv[i] : "-I");
                return -1;
            }
            continue;
        }
        if (arg[0] == '-' && arg[1] == 'I' && arg[2] != '\0') {
            if (add_include_dir(options, arg + 2) != 0) {
                tool_write_error(options->program_name, "invalid include directory ", arg + 2);
                return -1;
            }
            continue;
        }
        if (text_equals(arg, "-D")) {
            if (i + 1 >= argc || add_define(options, argv[++i]) != 0) {
                tool_write_error(options->program_name, "invalid macro definition ", (i < argc) ? argv[i] : "-D");
                return -1;
            }
            continue;
        }
        if (arg[0] == '-' && arg[1] == 'D' && arg[2] != '\0') {
            if (add_define(options, arg + 2) != 0) {
                tool_write_error(options->program_name, "invalid macro definition ", arg + 2);
                return -1;
            }
            continue;
        }
        if (text_equals(arg, "-o")) {
            if (i + 1 >= argc) {
                tool_write_error(options->program_name, "missing output path after ", "-o");
                return -1;
            }
            options->output_path = argv[++i];
            continue;
        }
        if (text_equals(arg, "--target")) {
            if (i + 1 >= argc || parse_target(argv[i + 1], &options->target) != 0) {
                tool_write_error(options->program_name, "unsupported target ", (i + 1 < argc) ? argv[i + 1] : "");
                return -1;
            }
            i += 1;
            continue;
        }
        if (starts_with(arg, "--target=")) {
            if (parse_target(arg + 9, &options->target) != 0) {
                tool_write_error(options->program_name, "unsupported target ", arg + 9);
                return -1;
            }
            continue;
        }
        if (is_ignored_option(arg)) {
            continue;
        }
        if (arg[0] == '-') {
            tool_write_error(options->program_name, "unknown option ", arg);
            return -1;
        }
        if (options->input_count >= COMPILER_MAX_INPUT_FILES) {
            tool_write_error(options->program_name, "too many input files; last one was ", arg);
            return -1;
        }
        options->input_paths[options->input_count++] = arg;
        if (options->input_path == 0) {
            options->input_path = arg;
        }
    }

    if (options->input_count == 0 && options->compile_manifest_path == 0) {
        write_usage(options->program_name);
        return -1;
    }

    return 0;
}

static void write_preprocessor_error(const char *program_name, const CompilerPreprocessor *preprocessor) {
    rt_write_cstr(2, program_name);
    rt_write_cstr(2, ": ");
    rt_write_cstr(2, compiler_preprocessor_error_path(preprocessor));
    rt_write_char(2, ':');
    rt_write_uint(2, compiler_preprocessor_error_line(preprocessor));
    rt_write_cstr(2, ": ");
    rt_write_line(2, compiler_preprocessor_error_message(preprocessor));
}

static int configure_preprocessor(CompilerPreprocessor *preprocessor, const CompilerOptions *options) {
    size_t i;

    compiler_preprocessor_init(preprocessor);
    if (compiler_preprocessor_add_include_dir(preprocessor, ".") != 0 ||
        compiler_preprocessor_add_include_dir(preprocessor, "src/shared") != 0 ||
        compiler_preprocessor_add_include_dir(preprocessor, "src/compiler") != 0 ||
        compiler_preprocessor_define(preprocessor, "__STDC_HOSTED__", options->freestanding ? "0" : "1") != 0 ||
        compiler_target_apply_preprocessor_defaults(preprocessor, options->target, options->freestanding) != 0) {
        return -1;
    }

    for (i = 0; i < options->include_dir_count; ++i) {
        if (compiler_preprocessor_add_include_dir(preprocessor, options->include_dirs[i]) != 0) {
            return -1;
        }
    }

    for (i = 0; i < options->define_count; ++i) {
        if (compiler_preprocessor_define(preprocessor, options->define_names[i], options->define_values[i]) != 0) {
            return -1;
        }
    }

    return 0;
}

static int emit_preprocessed_output(const CompilerOptions *options) {
    CompilerPreprocessor preprocessor;
    CompilerSource source;
    int out_fd = 1;
    int should_close = 0;

    if (configure_preprocessor(&preprocessor, options) != 0) {
        tool_write_error(options->program_name, "failed to initialize preprocessor for target ", target_name(options->target));
        return 1;
    }

    if (options->output_path != 0) {
        out_fd = platform_open_write(options->output_path, 0644U);
        if (out_fd < 0) {
            tool_write_error(options->program_name, "cannot open output ", options->output_path);
            return 1;
        }
        should_close = 1;
    }

    if (compiler_preprocess_file(&preprocessor, options->input_path, &source) != 0) {
        if (should_close) {
            (void)platform_close(out_fd);
        }
        write_preprocessor_error(options->program_name, &preprocessor);
        return 1;
    }

    if (rt_write_all(out_fd, source.data, source.size) != 0) {
        if (should_close) {
            (void)platform_close(out_fd);
        }
        tool_write_error(options->program_name, "failed while writing preprocessed output for ", options->input_path);
        return 1;
    }

    if (should_close) {
        (void)platform_close(out_fd);
    }
    return 0;
}

static int write_escaped_span(int fd, const char *text, size_t length) {
    size_t i;

    for (i = 0; i < length; ++i) {
        char ch = text[i];

        if (ch == '\n') {
            if (rt_write_cstr(fd, "\\n") != 0) {
                return -1;
            }
        } else if (ch == '\r') {
            if (rt_write_cstr(fd, "\\r") != 0) {
                return -1;
            }
        } else if (ch == '\t') {
            if (rt_write_cstr(fd, "\\t") != 0) {
                return -1;
            }
        } else if (ch == '\\') {
            if (rt_write_cstr(fd, "\\\\") != 0) {
                return -1;
            }
        } else {
            if (rt_write_char(fd, ch) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

static int write_token(int fd, const CompilerToken *token) {
    if (rt_write_uint(fd, token->line) != 0 ||
        rt_write_char(fd, ':') != 0 ||
        rt_write_uint(fd, token->column) != 0 ||
        rt_write_char(fd, ' ') != 0 ||
        rt_write_cstr(fd, compiler_token_kind_name(token->kind)) != 0) {
        return -1;
    }

    if (token->kind == COMPILER_TOKEN_EOF) {
        return rt_write_line(fd, " <eof>");
    }

    if (rt_write_char(fd, ' ') != 0) {
        return -1;
    }

    if (write_escaped_span(fd, token->start, token->length) != 0) {
        return -1;
    }

    return rt_write_char(fd, '\n');
}

static int dump_tokens(const CompilerOptions *options) {
    CompilerPreprocessor preprocessor;
    static CompilerSource source;
    CompilerLexer lexer;
    CompilerToken token;
    int out_fd = 1;
    int should_close = 0;

    if (options->output_path != 0) {
        out_fd = platform_open_write(options->output_path, 0644U);
        if (out_fd < 0) {
            tool_write_error(options->program_name, "cannot open output ", options->output_path);
            return 1;
        }
        should_close = 1;
    }

    if (configure_preprocessor(&preprocessor, options) != 0) {
        if (should_close) {
            (void)platform_close(out_fd);
        }
        tool_write_error(options->program_name, "failed to initialize preprocessor for target ", target_name(options->target));
        return 1;
    }

    if (compiler_preprocess_file(&preprocessor, options->input_path, &source) != 0) {
        if (should_close) {
            (void)platform_close(out_fd);
        }
        write_preprocessor_error(options->program_name, &preprocessor);
        return 1;
    }

    compiler_lexer_init(&lexer, &source);
    for (;;) {
        if (compiler_lexer_next(&lexer, &token) != 0) {
            if (should_close) {
                (void)platform_close(out_fd);
            }
            rt_write_cstr(2, options->program_name);
            rt_write_cstr(2, ": ");
            rt_write_cstr(2, source.path);
            rt_write_char(2, ':');
            rt_write_uint(2, lexer.line);
            rt_write_char(2, ':');
            rt_write_uint(2, lexer.column);
            rt_write_cstr(2, ": ");
            rt_write_line(2, compiler_lexer_error_message(&lexer));
            return 1;
        }

        if (write_token(out_fd, &token) != 0) {
            if (should_close) {
                (void)platform_close(out_fd);
            }
            tool_write_error(options->program_name, "failed while writing token stream for ", options->input_path);
            return 1;
        }

        if (token.kind == COMPILER_TOKEN_EOF) {
            break;
        }
    }

    if (should_close) {
        (void)platform_close(out_fd);
    }
    return 0;
}

static int parse_translation_unit(const CompilerOptions *options) {
    CompilerBackend backend;
    CompilerObjectWriter object_writer;
    CompilerPreprocessor preprocessor;
    static CompilerParser parser;
    static CompilerSource source;
    char derived_output_path[COMPILER_PATH_CAPACITY];
    const char *output_path;
    int out_fd = 1;
    int should_close = 0;
    unsigned long long total_start = platform_get_monotonic_time_ns();
    unsigned long long stage_start;
    unsigned long long after_preprocess;
    unsigned long long after_parse;
    unsigned long long after_optimize;
    unsigned long long after_output;

    output_path = pick_output_path(options, derived_output_path, sizeof(derived_output_path));

    if ((options->compile_only || options->emit_assembly) && (options->dump_ast || options->dump_ir)) {
        tool_write_error(options->program_name, "cannot combine dump modes with ", "-c or -S");
        return 1;
    }

    if (configure_preprocessor(&preprocessor, options) != 0) {
        tool_write_error(options->program_name, "failed to initialize preprocessor for target ", target_name(options->target));
        return 1;
    }

    if ((options->dump_ast || options->dump_ir || options->emit_assembly || options->compile_only) && output_path != 0) {
        out_fd = platform_open_write(output_path, 0644U);
        if (out_fd < 0) {
            tool_write_error(options->program_name, "cannot open output ", output_path);
            return 1;
        }
        should_close = 1;
    }

    stage_start = platform_get_monotonic_time_ns();
    if (compiler_preprocess_file(&preprocessor, options->input_path, &source) != 0) {
        if (should_close) {
            (void)platform_close(out_fd);
        }
        write_preprocessor_error(options->program_name, &preprocessor);
        return 1;
    }
    after_preprocess = platform_get_monotonic_time_ns();

    compiler_ir_destroy(&parser.ir);
    compiler_parser_init(&parser, &source, options->dump_ast, options->dump_ir, out_fd);
    if (compiler_parse_translation_unit(&parser) != 0) {
        if (should_close) {
            (void)platform_close(out_fd);
        }
        rt_write_cstr(2, options->program_name);
        rt_write_cstr(2, ": ");
        rt_write_cstr(2, source.path);
        rt_write_char(2, ':');
        rt_write_uint(2, compiler_parser_error_line(&parser));
        rt_write_char(2, ':');
        rt_write_uint(2, compiler_parser_error_column(&parser));
        rt_write_cstr(2, ": ");
        rt_write_line(2, compiler_parser_error_message(&parser));
        return 1;
    }
    after_parse = platform_get_monotonic_time_ns();

    if (compiler_ir_optimize(&parser.ir) != 0) {
        if (should_close) {
            (void)platform_close(out_fd);
        }
        tool_write_error(options->program_name, "failed while optimizing IR: ", compiler_ir_error_message(&parser.ir));
        return 1;
    }
    after_optimize = platform_get_monotonic_time_ns();

    if (options->dump_ir && compiler_ir_write_dump(&parser.ir, out_fd) != 0) {
        if (should_close) {
            (void)platform_close(out_fd);
        }
        tool_write_error(options->program_name, "failed while writing IR output for ", options->input_path);
        return 1;
    }

    if (options->emit_assembly) {
        compiler_backend_init(&backend, options->target, options->function_sections, options->data_sections);
        if (compiler_backend_emit_assembly(&backend, &parser.ir, out_fd) != 0) {
            if (should_close) {
                (void)platform_close(out_fd);
            }
            tool_write_error(options->program_name, "failed while writing assembly output: ", compiler_backend_error_message(&backend));
            return 1;
        }
    }

    if (options->compile_only) {
        compiler_object_writer_init(&object_writer);
        if (compiler_object_write_target(&object_writer, options->target, &parser.ir, out_fd, options->function_sections, options->data_sections) != 0) {
            if (should_close) {
                (void)platform_close(out_fd);
            }
            tool_write_error(options->program_name, "failed while writing object output: ", compiler_object_writer_error_message(&object_writer));
            return 1;
        }
    }

    after_output = platform_get_monotonic_time_ns();

    if (should_close) {
        (void)platform_close(out_fd);
    }

    write_time_report(options,
                      after_preprocess - stage_start,
                      after_parse - after_preprocess,
                      after_optimize - after_parse,
                      after_output - after_optimize,
                      after_output - total_start);

    return 0;
}

static int copy_manifest_path(const char *start, const char *end, char *buffer, size_t buffer_size) {
    size_t length;

    while (start < end && (*start == ' ' || *start == '\t')) {
        start += 1;
    }
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
        end -= 1;
    }
    length = (size_t)(end - start);
    if (length == 0U || length >= buffer_size) {
        return -1;
    }
    memcpy(buffer, start, length);
    buffer[length] = '\0';
    return 0;
}

static int compile_manifest_inputs(const CompilerOptions *options) {
    static char manifest_data[COMPILER_MANIFEST_MAX_BYTES + 1U];
    char input_path[COMPILER_PATH_CAPACITY];
    char output_path[COMPILER_PATH_CAPACITY];
    int fd;
    long bytes_read;
    size_t size = 0;
    const char *cursor;
    int status = 0;

    fd = platform_open_read(options->compile_manifest_path);
    if (fd < 0) {
        tool_write_error(options->program_name, "cannot open compile manifest ", options->compile_manifest_path);
        return 1;
    }
    while (size < COMPILER_MANIFEST_MAX_BYTES &&
           (bytes_read = platform_read(fd, manifest_data + size, COMPILER_MANIFEST_MAX_BYTES - size)) > 0) {
        size += (size_t)bytes_read;
    }
    (void)platform_close(fd);
    if (bytes_read < 0) {
        tool_write_error(options->program_name, "cannot read compile manifest ", options->compile_manifest_path);
        return 1;
    }
    if (size == COMPILER_MANIFEST_MAX_BYTES) {
        tool_write_error(options->program_name, "compile manifest is too large ", options->compile_manifest_path);
        return 1;
    }
    manifest_data[size] = '\0';

    compiler_source_cache_clear();
    compiler_source_cache_set_enabled(1);

    cursor = manifest_data;
    while (*cursor != '\0') {
        const char *line_start = cursor;
        const char *line_end;
        const char *separator;
        CompilerOptions per_input;

        while (*cursor != '\0' && *cursor != '\n') {
            cursor += 1;
        }
        line_end = cursor;
        if (*cursor == '\n') {
            cursor += 1;
        }
        while (line_start < line_end && (*line_start == ' ' || *line_start == '\t' || *line_start == '\r')) {
            line_start += 1;
        }
        if (line_start == line_end || *line_start == '#') {
            continue;
        }

        separator = line_start;
        while (separator < line_end && *separator != '\t' && *separator != ' ') {
            separator += 1;
        }
        if (separator == line_end || copy_manifest_path(line_start, separator, input_path, sizeof(input_path)) != 0 ||
            copy_manifest_path(separator, line_end, output_path, sizeof(output_path)) != 0) {
            tool_write_error(options->program_name, "invalid compile manifest line in ", options->compile_manifest_path);
            status = 1;
            break;
        }

        per_input = *options;
        per_input.input_path = input_path;
        per_input.output_path = output_path;
        per_input.input_paths[0] = input_path;
        per_input.input_count = 1U;
        per_input.compile_manifest_path = 0;
        per_input.compile_only = 1;
        per_input.syntax_only = 0;
        per_input.dump_tokens = 0;
        per_input.preprocess_only = 0;
        per_input.dump_ast = 0;
        per_input.dump_ir = 0;
        per_input.emit_assembly = 0;

        if (parse_translation_unit(&per_input) != 0) {
            status = 1;
            break;
        }
    }

    compiler_source_cache_set_enabled(0);
    compiler_source_cache_clear();
    return status;
}

int compiler_main(int argc, char **argv) {
    CompilerOptions options;
    size_t i;
    int parse_result = parse_options(argc, argv, &options);

    if (parse_result == 1 || parse_result == 2) {
        return 0;
    }
    if (parse_result != 0) {
        return 1;
    }

    if (options.compile_manifest_path != 0) {
        if (options.output_path != 0 || options.preprocess_only || options.dump_tokens || options.dump_ast || options.dump_ir || options.emit_assembly || options.syntax_only) {
            tool_write_error(options.program_name, "--compile-manifest only supports object compilation", "");
            return 1;
        }
        return compile_manifest_inputs(&options);
    }

    if ((options.preprocess_only || options.dump_tokens || options.dump_ast || options.dump_ir) && options.input_count > 1U) {
        tool_write_error(options.program_name, "this mode currently accepts only one input file: ", options.input_paths[1]);
        return 1;
    }

    if ((options.compile_only || options.emit_assembly) && options.output_path != 0 && options.input_count > 1U) {
        tool_write_error(options.program_name, "cannot use -o when compiling multiple input files with ", options.compile_only ? "-c" : "-S");
        return 1;
    }

    if (options.preprocess_only) {
        return emit_preprocessed_output(&options);
    }

    if (options.dump_tokens) {
        return dump_tokens(&options);
    }

    if (options.compile_only || options.emit_assembly || options.syntax_only || options.dump_ast || options.dump_ir) {
        for (i = 0; i < options.input_count; ++i) {
            CompilerOptions per_input = options;
            per_input.input_path = options.input_paths[i];
            if (parse_translation_unit(&per_input) != 0) {
                return 1;
            }
        }
        return 0;
    }

    return link_executable_output(&options);
}
