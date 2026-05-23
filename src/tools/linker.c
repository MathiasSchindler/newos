#include "linker.h"
#include "runtime.h"
#include "tool_util.h"

#define LINKER_TOOL_MAX_INPUTS 512U
#define LINKER_TOOL_MAX_ARGS 1024U
#define LINKER_TOOL_MAX_LIBRARY_DIRS 64U
#define LINKER_TOOL_MAX_RESPONSE_FILES 32U
#define LINKER_TOOL_RESPONSE_CAPACITY (1024U * 1024U)
#define LINKER_TOOL_PATH_CAPACITY 4096U

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-o output] [-m elf_x86_64] [--target=elf64-x86_64|mach-o-arm64] [--tiny] [--gc-sections] [--stats] [--map FILE] [--print-gc-sections] [--lto-cc=<cc>] object-or-archive ...");
}

static int starts_with(const char *text, const char *prefix) {
    return rt_strncmp(text, prefix, rt_strlen(prefix)) == 0;
}

static int read_response_file(const char *path, char **buffer_out) {
    int fd;
    long bytes_read;
    size_t total = 0U;
    char *buffer;

    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }
    buffer = (char *)rt_malloc(LINKER_TOOL_RESPONSE_CAPACITY + 1U);
    if (buffer == 0) {
        (void)platform_close(fd);
        return -1;
    }
    while ((bytes_read = platform_read(fd, buffer + total, LINKER_TOOL_RESPONSE_CAPACITY - total)) > 0) {
        total += (size_t)bytes_read;
        if (total == LINKER_TOOL_RESPONSE_CAPACITY) {
            break;
        }
    }
    (void)platform_close(fd);
    if (bytes_read < 0 || total == LINKER_TOOL_RESPONSE_CAPACITY) {
        rt_free(buffer);
        return -1;
    }
    buffer[total] = '\0';
    *buffer_out = buffer;
    return 0;
}

static int response_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int append_expanded_arg(const char *program_name,
                               const char *arg,
                               int depth,
                               const char **expanded_args,
                               int *expanded_argc,
                               char **response_buffers,
                               size_t *response_count);

static int expand_response_text(const char *program_name,
                                char *buffer,
                                int depth,
                                const char **expanded_args,
                                int *expanded_argc,
                                char **response_buffers,
                                size_t *response_count) {
    char *read = buffer;

    while (*read != '\0') {
        char *token;
        char *write;

        while (response_space(*read)) {
            read += 1;
        }
        if (*read == '\0') {
            break;
        }
        token = read;
        write = read;
        while (*read != '\0' && !response_space(*read)) {
            if (*read == '\'' || *read == '"') {
                char quote = *read++;
                while (*read != '\0' && *read != quote) {
                    if (*read == '\\' && read[1] != '\0') {
                        read += 1;
                    }
                    *write++ = *read++;
                }
                if (*read == quote) {
                    read += 1;
                }
            } else {
                if (*read == '\\' && read[1] != '\0') {
                    read += 1;
                }
                *write++ = *read++;
            }
        }
        if (*read != '\0') {
            read += 1;
        }
        *write = '\0';
        if (append_expanded_arg(program_name, token, depth, expanded_args, expanded_argc, response_buffers, response_count) != 0) {
            return -1;
        }
    }
    return 0;
}

static int append_expanded_arg(const char *program_name,
                               const char *arg,
                               int depth,
                               const char **expanded_args,
                               int *expanded_argc,
                               char **response_buffers,
                               size_t *response_count) {
    if (arg[0] == '@' && arg[1] != '\0') {
        char *buffer;

        if (depth >= 8 || *response_count >= LINKER_TOOL_MAX_RESPONSE_FILES || read_response_file(arg + 1, &buffer) != 0) {
            tool_write_error(program_name, "failed to read response file: ", arg + 1);
            return -1;
        }
        response_buffers[*response_count] = buffer;
        *response_count += 1U;
        return expand_response_text(program_name, buffer, depth + 1, expanded_args, expanded_argc, response_buffers, response_count);
    }
    if ((size_t)*expanded_argc >= LINKER_TOOL_MAX_ARGS) {
        tool_write_error(program_name, "too many command line arguments", "");
        return -1;
    }
    expanded_args[*expanded_argc] = arg;
    *expanded_argc += 1;
    return 0;
}

static int resolve_library(const char *name, const char **library_dirs, size_t library_dir_count, char output[LINKER_TOOL_PATH_CAPACITY]) {
    char archive_name[LINKER_TOOL_PATH_CAPACITY];
    size_t i;

    rt_copy_string(archive_name, sizeof(archive_name), "lib");
    if (rt_strlen(archive_name) + rt_strlen(name) + 3U >= sizeof(archive_name)) {
        return -1;
    }
    rt_copy_string(archive_name + rt_strlen(archive_name), sizeof(archive_name) - rt_strlen(archive_name), name);
    rt_copy_string(archive_name + rt_strlen(archive_name), sizeof(archive_name) - rt_strlen(archive_name), ".a");
    for (i = 0; i < library_dir_count; ++i) {
        if (tool_join_path(library_dirs[i], archive_name, output, LINKER_TOOL_PATH_CAPACITY) == 0 && tool_path_exists(output)) {
            return 0;
        }
    }
    rt_copy_string(output, LINKER_TOOL_PATH_CAPACITY, archive_name);
    return tool_path_exists(output) ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *inputs[LINKER_TOOL_MAX_INPUTS];
    const char *expanded_args[LINKER_TOOL_MAX_ARGS];
    char *response_buffers[LINKER_TOOL_MAX_RESPONSE_FILES];
    const char *library_dirs[LINKER_TOOL_MAX_LIBRARY_DIRS];
    char library_paths[LINKER_TOOL_MAX_INPUTS][LINKER_TOOL_PATH_CAPACITY];
    CompilerLinkerOptions options;
    size_t input_count = 0U;
    size_t response_count = 0U;
    size_t library_dir_count = 0U;
    size_t library_path_count = 0U;
    const char *output_path = "a.out";
    char error[COMPILER_ERROR_CAPACITY];
    int expanded_argc = 1;
    int parsing_options = 1;
    int i;

    rt_memset(&options, 0, sizeof(options));
    expanded_args[0] = argv[0];
    for (i = 1; i < argc; ++i) {
        if (append_expanded_arg(argv[0], argv[i], 0, expanded_args, &expanded_argc, response_buffers, &response_count) != 0) {
            return 1;
        }
    }
    for (i = 1; i < expanded_argc; ++i) {
        const char *arg = expanded_args[i];

        if (parsing_options && rt_strcmp(arg, "--") == 0) {
            parsing_options = 0;
            continue;
        }
        if (parsing_options && (rt_strcmp(arg, "--help") == 0 || rt_strcmp(arg, "-h") == 0)) {
            print_usage(expanded_args[0]);
            return 0;
        }
        if (parsing_options && rt_strcmp(arg, "-o") == 0) {
            if (i + 1 >= expanded_argc) {
                tool_write_error(expanded_args[0], "missing output path after ", arg);
                print_usage(expanded_args[0]);
                return 1;
            }
            output_path = expanded_args[++i];
            continue;
        }
        if (parsing_options && starts_with(arg, "--output=")) {
            output_path = arg + 9;
            continue;
        }
        if (parsing_options && rt_strcmp(arg, "-m") == 0) {
            if (i + 1 >= expanded_argc) {
                tool_write_error(expanded_args[0], "missing emulation after ", arg);
                print_usage(expanded_args[0]);
                return 1;
            }
            if (compiler_linker_target_parse(expanded_args[++i], &options.target) != 0) {
                tool_write_error(expanded_args[0], "unsupported emulation: ", expanded_args[i]);
                return 1;
            }
            continue;
        }
        if (parsing_options && starts_with(arg, "--target=")) {
            const char *target = arg + 9;
            if (compiler_linker_target_parse(target, &options.target) != 0) {
                tool_write_error(expanded_args[0], "unsupported target: ", target);
                return 1;
            }
            continue;
        }
        if (parsing_options && (rt_strcmp(arg, "--tiny") == 0 || rt_strcmp(arg, "--pack-segments") == 0)) {
            options.tiny = 1;
            continue;
        }
        if (parsing_options && (rt_strcmp(arg, "--separate-code") == 0 || rt_strcmp(arg, "--page-align") == 0)) {
            options.tiny = 0;
            continue;
        }
        if (parsing_options && rt_strcmp(arg, "--gc-sections") == 0) {
            options.gc_sections = 1;
            continue;
        }
        if (parsing_options && rt_strcmp(arg, "--no-gc-sections") == 0) {
            options.gc_sections = 0;
            continue;
        }
        if (parsing_options && (rt_strcmp(arg, "--icf=safe") == 0 || rt_strcmp(arg, "--icf") == 0)) {
            options.icf_safe = 1;
            continue;
        }
        if (parsing_options && rt_strcmp(arg, "--stats") == 0) {
            options.stats = 1;
            continue;
        }
        if (parsing_options && rt_strcmp(arg, "--print-gc-sections") == 0) {
            options.print_gc_sections = 1;
            continue;
        }
        if (parsing_options && rt_strcmp(arg, "--map") == 0) {
            if (i + 1 >= expanded_argc) {
                tool_write_error(expanded_args[0], "missing map path after ", arg);
                print_usage(expanded_args[0]);
                return 1;
            }
            options.map_path = expanded_args[++i];
            continue;
        }
        if (parsing_options && starts_with(arg, "--map=")) {
            options.map_path = arg + 6;
            continue;
        }
        if (parsing_options && rt_strcmp(arg, "--why-live") == 0) {
            if (i + 1 >= expanded_argc) {
                tool_write_error(expanded_args[0], "missing symbol or section after ", arg);
                print_usage(expanded_args[0]);
                return 1;
            }
            options.why_live = expanded_args[++i];
            continue;
        }
        if (parsing_options && starts_with(arg, "--why-live=")) {
            options.why_live = arg + 11;
            continue;
        }
        if (parsing_options && rt_strcmp(arg, "-e") == 0) {
            if (i + 1 >= expanded_argc) {
                tool_write_error(expanded_args[0], "missing entry symbol after ", arg);
                print_usage(expanded_args[0]);
                return 1;
            }
            options.entry_symbol = expanded_args[++i];
            continue;
        }
        if (parsing_options && starts_with(arg, "--entry=")) {
            options.entry_symbol = arg + 8;
            continue;
        }
        if (parsing_options && rt_strcmp(arg, "--lto-cc") == 0) {
            if (i + 1 >= expanded_argc) {
                tool_write_error(expanded_args[0], "missing compiler path after ", arg);
                print_usage(expanded_args[0]);
                return 1;
            }
            options.lto_cc = expanded_args[++i];
            continue;
        }
        if (parsing_options && starts_with(arg, "--lto-cc=")) {
            options.lto_cc = arg + 9;
            continue;
        }
        if (parsing_options && (rt_strcmp(arg, "--static") == 0 || rt_strcmp(arg, "-static") == 0)) {
            continue;
        }
        if (parsing_options && (rt_strcmp(arg, "--start-group") == 0 || rt_strcmp(arg, "--end-group") == 0 || rt_strcmp(arg, "--whole-archive") == 0 || rt_strcmp(arg, "--no-whole-archive") == 0)) {
            continue;
        }
        if (parsing_options && rt_strcmp(arg, "-L") == 0) {
            if (i + 1 >= expanded_argc || library_dir_count >= LINKER_TOOL_MAX_LIBRARY_DIRS) {
                tool_write_error(expanded_args[0], "invalid library search path after ", arg);
                return 1;
            }
            library_dirs[library_dir_count++] = expanded_args[++i];
            continue;
        }
        if (parsing_options && starts_with(arg, "-L") && arg[2] != '\0') {
            if (library_dir_count >= LINKER_TOOL_MAX_LIBRARY_DIRS) {
                tool_write_error(expanded_args[0], "too many library search paths", "");
                return 1;
            }
            library_dirs[library_dir_count++] = arg + 2;
            continue;
        }
        if (parsing_options && starts_with(arg, "-l") && arg[2] != '\0') {
            if (input_count >= LINKER_TOOL_MAX_INPUTS || library_path_count >= LINKER_TOOL_MAX_INPUTS || resolve_library(arg + 2, library_dirs, library_dir_count, library_paths[library_path_count]) != 0) {
                tool_write_error(expanded_args[0], "failed to resolve library: ", arg + 2);
                return 1;
            }
            inputs[input_count++] = library_paths[library_path_count++];
            continue;
        }
        if (parsing_options && arg[0] == '-') {
            tool_write_error(expanded_args[0], "unsupported option: ", arg);
            print_usage(expanded_args[0]);
            return 1;
        }
        if (input_count >= LINKER_TOOL_MAX_INPUTS) {
            tool_write_error(expanded_args[0], "too many input files", "");
            return 1;
        }
        inputs[input_count++] = arg;
    }

    if (input_count == 0U) {
        tool_write_error(expanded_args[0], "no input files", "");
        print_usage(expanded_args[0]);
        return 1;
    }
    if (compiler_link_static_options(inputs, input_count, output_path, &options, error, sizeof(error)) != 0) {
        tool_write_error(expanded_args[0], error[0] != '\0' ? error : "link failed", "");
        return 1;
    }
    return 0;
}