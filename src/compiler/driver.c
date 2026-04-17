#include "compiler.h"

#include "backend.h"
#include "lexer.h"
#include "object_writer.h"
#include "parser.h"
#include "platform.h"
#include "preprocessor.h"
#include "runtime.h"
#include "source.h"
#include "tool_util.h"

#define COMPILER_MAX_OPTION_DEFINES 32

typedef enum {
    COMPILER_TARGET_LINUX_X86_64 = 0,
    COMPILER_TARGET_LINUX_AARCH64,
    COMPILER_TARGET_MACOS_AARCH64
} CompilerTarget;

typedef struct {
    const char *program_name;
    const char *input_path;
    const char *output_path;
    CompilerTarget target;
    int compile_only;
    int dump_tokens;
    int preprocess_only;
    int dump_ast;
    int dump_ir;
    int emit_assembly;
    char include_dirs[COMPILER_MAX_INCLUDE_DIRS][COMPILER_PATH_CAPACITY];
    size_t include_dir_count;
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

static CompilerTarget default_target(void) {
    char sysname[64];
    char nodename[64];
    char release[64];
    char version[64];
    char machine[64];

    if (platform_get_uname(sysname, sizeof(sysname), nodename, sizeof(nodename), release, sizeof(release), version, sizeof(version), machine, sizeof(machine)) != 0) {
        return COMPILER_TARGET_MACOS_AARCH64;
    }

    (void)nodename;
    (void)release;
    (void)version;

    if (text_equals(sysname, "Linux")) {
        if (text_equals(machine, "x86_64")) {
            return COMPILER_TARGET_LINUX_X86_64;
        }
        return COMPILER_TARGET_LINUX_AARCH64;
    }

    return COMPILER_TARGET_MACOS_AARCH64;
}

static const char *target_name(CompilerTarget target) {
    switch (target) {
        case COMPILER_TARGET_LINUX_X86_64:
            return "linux-x86_64";
        case COMPILER_TARGET_LINUX_AARCH64:
            return "linux-aarch64";
        case COMPILER_TARGET_MACOS_AARCH64:
            return "macos-aarch64";
    }

    return "unknown";
}

static int parse_target(const char *text, CompilerTarget *target_out) {
    if (text_equals(text, "linux-x86_64") || text_equals(text, "linux-x64")) {
        *target_out = COMPILER_TARGET_LINUX_X86_64;
        return 0;
    }
    if (text_equals(text, "linux-aarch64") || text_equals(text, "linux-arm64")) {
        *target_out = COMPILER_TARGET_LINUX_AARCH64;
        return 0;
    }
    if (text_equals(text, "macos-aarch64") || text_equals(text, "macos-arm64") || text_equals(text, "darwin-aarch64") || text_equals(text, "darwin-arm64")) {
        *target_out = COMPILER_TARGET_MACOS_AARCH64;
        return 0;
    }
    return -1;
}

static void write_usage(const char *program_name) {
    tool_write_usage(program_name, "[-E|--preprocess] [-S|--emit-asm] [--dump-tokens|--dump-ast|--dump-ir] [--target TARGET] [-I DIR] [-DNAME=VALUE] [-c] [-o OUTPUT] FILE");
    rt_write_line(2, "Targets: linux-x86_64, linux-aarch64, macos-aarch64");
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

static const char *pick_link_output_path(const CompilerOptions *options, char *buffer, size_t buffer_size) {
    if (options->output_path != 0) {
        return options->output_path;
    }
    rt_copy_string(buffer, buffer_size, "a.out");
    return buffer;
}

static void append_link_arg(char **argv, size_t *count, size_t capacity, char *value) {
    if (*count + 1U < capacity) {
        argv[*count] = value;
        *count += 1U;
    }
}

static int write_temp_object(
    const CompilerOptions *options,
    const CompilerIr *ir,
    char *path_buffer,
    size_t path_buffer_size
) {
    CompilerObjectWriter object_writer;
    int fd;

    compiler_object_writer_init(&object_writer);
    fd = platform_create_temp_file(path_buffer, path_buffer_size, "/tmp/newos-ncc-link-", 0600U);
    if (fd < 0) {
        tool_write_error(options->program_name, "failed to create temporary object for ", options->input_path);
        return -1;
    }

    switch (options->target) {
        case COMPILER_TARGET_LINUX_X86_64:
            if (compiler_object_write_elf64_x86_64(&object_writer, ir, fd) != 0) {
                (void)platform_close(fd);
                (void)platform_remove_file(path_buffer);
                tool_write_error(options->program_name, "failed while writing temporary object output: ", compiler_object_writer_error_message(&object_writer));
                return -1;
            }
            break;
        case COMPILER_TARGET_MACOS_AARCH64:
            if (compiler_object_write_macho64_aarch64(&object_writer, ir, fd) != 0) {
                (void)platform_close(fd);
                (void)platform_remove_file(path_buffer);
                tool_write_error(options->program_name, "failed while writing temporary object output: ", compiler_object_writer_error_message(&object_writer));
                return -1;
            }
            break;
        case COMPILER_TARGET_LINUX_AARCH64:
            (void)platform_close(fd);
            (void)platform_remove_file(path_buffer);
            tool_write_error(options->program_name, "object emission is not implemented yet for target ", target_name(options->target));
            return -1;
    }

    if (platform_close(fd) != 0) {
        (void)platform_remove_file(path_buffer);
        tool_write_error(options->program_name, "failed to finalize temporary object for ", options->input_path);
        return -1;
    }

    return 0;
}

static int link_executable_output(const CompilerOptions *options, const CompilerIr *ir) {
    static char *const shared_sources[] = {
        "src/shared/runtime/memory.c",
        "src/shared/runtime/string.c",
        "src/shared/runtime/parse.c",
        "src/shared/runtime/io.c",
        "src/shared/tool_util.c",
        "src/shared/archive_util.c",
        "src/shared/hash_util.c"
    };
    static char *const host_platform_sources[] = {
        "src/platform/posix/fs.c",
        "src/platform/posix/process.c",
        "src/platform/posix/identity.c",
        "src/platform/posix/net.c",
        "src/platform/posix/time.c"
    };
    static char *const compiler_sources[] = {
        "src/compiler/backend.c",
        "src/compiler/driver.c",
        "src/compiler/ir.c",
        "src/compiler/object_writer.c",
        "src/compiler/parser.c",
        "src/compiler/preprocessor.c",
        "src/compiler/semantic.c",
        "src/compiler/source.c",
        "src/compiler/lexer.c"
    };
    static char *const shell_sources[] = {
        "src/shared/shell_parser.c",
        "src/shared/shell_execution.c",
        "src/shared/shell_builtins.c",
        "src/shared/shell_interactive.c"
    };
    char derived_output_path[COMPILER_PATH_CAPACITY];
    char object_path[COMPILER_PATH_CAPACITY];
    const char *output_path = pick_link_output_path(options, derived_output_path, sizeof(derived_output_path));
    const char *input_name = tool_base_name(options->input_path);
    char *argv[64];
    size_t argc = 0;
    size_t i;
    int pid = -1;
    int exit_status = 0;

    if (write_temp_object(options, ir, object_path, sizeof(object_path)) != 0) {
        return 1;
    }

    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "clang");
    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-target");
    switch (options->target) {
        case COMPILER_TARGET_MACOS_AARCH64:
            append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "arm64-apple-darwin");
            break;
        case COMPILER_TARGET_LINUX_X86_64:
            append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "x86_64-linux-gnu");
            break;
        case COMPILER_TARGET_LINUX_AARCH64:
            (void)platform_remove_file(object_path);
            tool_write_error(options->program_name, "linking is not implemented yet for target ", target_name(options->target));
            return 1;
    }

    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-std=c11");
    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-O2");
    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-Isrc/shared");
    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-Isrc/compiler");
    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-Isrc/platform/posix");
    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-Isrc/platform/linux");
    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-Isrc/arch/aarch64/linux");
    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), object_path);

    for (i = 0; i < sizeof(shared_sources) / sizeof(shared_sources[0]); ++i) {
        append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), shared_sources[i]);
    }
    for (i = 0; i < sizeof(host_platform_sources) / sizeof(host_platform_sources[0]); ++i) {
        append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), host_platform_sources[i]);
    }
    if (text_equals(input_name, "sh.c")) {
        for (i = 0; i < sizeof(shell_sources) / sizeof(shell_sources[0]); ++i) {
            append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), shell_sources[i]);
        }
    } else if (text_equals(input_name, "ncc.c")) {
        for (i = 0; i < sizeof(compiler_sources) / sizeof(compiler_sources[0]); ++i) {
            append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), compiler_sources[i]);
        }
    }
    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), "-o");
    append_link_arg(argv, &argc, sizeof(argv) / sizeof(argv[0]), (char *)output_path);
    argv[argc] = 0;

    if (platform_spawn_process(argv, -1, -1, 0, 0, 0, &pid) != 0) {
        (void)platform_remove_file(object_path);
        tool_write_error(options->program_name, "failed to invoke linker driver for ", target_name(options->target));
        return 1;
    }
    if (platform_wait_process(pid, &exit_status) != 0) {
        (void)platform_remove_file(object_path);
        tool_write_error(options->program_name, "failed while waiting for linker driver for ", target_name(options->target));
        return 1;
    }
    (void)platform_remove_file(object_path);

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
        if (arg[0] == '-') {
            tool_write_error(options->program_name, "unknown option ", arg);
            return -1;
        }
        if (options->input_path != 0) {
            tool_write_error(options->program_name, "only one input file is supported for now: ", arg);
            return -1;
        }
        options->input_path = arg;
    }

    if (options->input_path == 0) {
        write_usage(options->program_name);
        return -1;
    }

    return 0;
}

static int configure_preprocessor(CompilerPreprocessor *preprocessor, const CompilerOptions *options) {
    size_t i;

    compiler_preprocessor_init(preprocessor);
    if (compiler_preprocessor_add_include_dir(preprocessor, ".") != 0 ||
        compiler_preprocessor_add_include_dir(preprocessor, "src/shared") != 0 ||
        compiler_preprocessor_add_include_dir(preprocessor, "src/compiler") != 0 ||
        compiler_preprocessor_add_include_dir(preprocessor, "src/platform/posix") != 0 ||
        compiler_preprocessor_add_include_dir(preprocessor, "src/platform/linux") != 0 ||
        compiler_preprocessor_add_include_dir(preprocessor, "src/arch/aarch64/linux") != 0) {
        return -1;
    }

    if (compiler_preprocessor_define(preprocessor, "__STDC_HOSTED__", "1") != 0) {
        return -1;
    }

    switch (options->target) {
        case COMPILER_TARGET_LINUX_X86_64:
            if (compiler_preprocessor_define(preprocessor, "__linux__", "1") != 0 ||
                compiler_preprocessor_define(preprocessor, "__x86_64__", "1") != 0) {
                return -1;
            }
            break;
        case COMPILER_TARGET_LINUX_AARCH64:
            if (compiler_preprocessor_define(preprocessor, "__linux__", "1") != 0 ||
                compiler_preprocessor_define(preprocessor, "__aarch64__", "1") != 0) {
                return -1;
            }
            break;
        case COMPILER_TARGET_MACOS_AARCH64:
            if (compiler_preprocessor_define(preprocessor, "__APPLE__", "1") != 0 ||
                compiler_preprocessor_define(preprocessor, "__aarch64__", "1") != 0) {
                return -1;
            }
            break;
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
        rt_write_cstr(2, options->program_name);
        rt_write_cstr(2, ": ");
        rt_write_cstr(2, compiler_preprocessor_error_path(&preprocessor));
        rt_write_char(2, ':');
        rt_write_uint(2, compiler_preprocessor_error_line(&preprocessor));
        rt_write_cstr(2, ": ");
        rt_write_line(2, compiler_preprocessor_error_message(&preprocessor));
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
        rt_write_cstr(2, options->program_name);
        rt_write_cstr(2, ": ");
        rt_write_cstr(2, compiler_preprocessor_error_path(&preprocessor));
        rt_write_char(2, ':');
        rt_write_uint(2, compiler_preprocessor_error_line(&preprocessor));
        rt_write_cstr(2, ": ");
        rt_write_line(2, compiler_preprocessor_error_message(&preprocessor));
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
    CompilerBackendTarget backend_target = COMPILER_BACKEND_TARGET_LINUX_X86_64;
    int out_fd = 1;
    int should_close = 0;

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

    if (compiler_preprocess_file(&preprocessor, options->input_path, &source) != 0) {
        if (should_close) {
            (void)platform_close(out_fd);
        }
        rt_write_cstr(2, options->program_name);
        rt_write_cstr(2, ": ");
        rt_write_cstr(2, compiler_preprocessor_error_path(&preprocessor));
        rt_write_char(2, ':');
        rt_write_uint(2, compiler_preprocessor_error_line(&preprocessor));
        rt_write_cstr(2, ": ");
        rt_write_line(2, compiler_preprocessor_error_message(&preprocessor));
        return 1;
    }

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

    if (options->dump_ir && compiler_ir_write_dump(&parser.ir, out_fd) != 0) {
        if (should_close) {
            (void)platform_close(out_fd);
        }
        tool_write_error(options->program_name, "failed while writing IR output for ", options->input_path);
        return 1;
    }

    if (options->emit_assembly) {
        switch (options->target) {
            case COMPILER_TARGET_LINUX_X86_64:
                backend_target = COMPILER_BACKEND_TARGET_LINUX_X86_64;
                break;
            case COMPILER_TARGET_LINUX_AARCH64:
                backend_target = COMPILER_BACKEND_TARGET_LINUX_AARCH64;
                break;
            case COMPILER_TARGET_MACOS_AARCH64:
                backend_target = COMPILER_BACKEND_TARGET_MACOS_AARCH64;
                break;
            default:
                if (should_close) {
                    (void)platform_close(out_fd);
                }
                tool_write_error(options->program_name, "assembly backend is not implemented for target ", target_name(options->target));
                return 1;
        }

        compiler_backend_init(&backend, backend_target);
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
        switch (options->target) {
            case COMPILER_TARGET_LINUX_X86_64:
                if (compiler_object_write_elf64_x86_64(&object_writer, &parser.ir, out_fd) != 0) {
                    if (should_close) {
                        (void)platform_close(out_fd);
                    }
                    tool_write_error(options->program_name, "failed while writing object output: ", compiler_object_writer_error_message(&object_writer));
                    return 1;
                }
                break;
            case COMPILER_TARGET_MACOS_AARCH64:
                if (compiler_object_write_macho64_aarch64(&object_writer, &parser.ir, out_fd) != 0) {
                    if (should_close) {
                        (void)platform_close(out_fd);
                    }
                    tool_write_error(options->program_name, "failed while writing object output: ", compiler_object_writer_error_message(&object_writer));
                    return 1;
                }
                break;
            default:
                if (should_close) {
                    (void)platform_close(out_fd);
                }
                tool_write_error(options->program_name, "object emission is not implemented yet for target ", target_name(options->target));
                return 1;
            }
    }

    if (should_close) {
        (void)platform_close(out_fd);
    }

    if (!options->dump_ast && !options->dump_ir && !options->emit_assembly) {
        if (options->compile_only) {
            return 0;
        } else {
            return link_executable_output(options, &parser.ir);
        }
    }

    return 0;
}

int compiler_main(int argc, char **argv) {
    CompilerOptions options;
    int parse_result = parse_options(argc, argv, &options);

    if (parse_result == 1 || parse_result == 2) {
        return 0;
    }
    if (parse_result != 0) {
        return 1;
    }

    if (options.preprocess_only) {
        return emit_preprocessed_output(&options);
    }

    if (options.dump_tokens) {
        return dump_tokens(&options);
    }

    return parse_translation_unit(&options);
}
