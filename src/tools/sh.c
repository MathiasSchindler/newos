#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#if __STDC_HOSTED__
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#endif

#define SH_MAX_LINE 4096
#define SH_MAX_COMMANDS 8
#define SH_MAX_ARGS 64
#define SH_MAX_JOBS 16
#define SH_MAX_HISTORY 64
#define SH_CAPTURE_CAPACITY 2048
#define SH_ENTRY_CAPACITY 1024
#define SH_MAX_ALIASES 32
#define SH_MAX_FUNCTIONS 32

typedef enum {
    SH_NEXT_ALWAYS = 0,
    SH_NEXT_AND = 1,
    SH_NEXT_OR = 2,
    SH_NEXT_BACKGROUND = 3
} ShNextMode;

typedef struct {
    char *argv[SH_MAX_ARGS + 1];
    int argc;
    char *input_path;
    char *output_path;
    int output_append;
    int no_expand[SH_MAX_ARGS];
} ShCommand;

typedef struct {
    ShCommand commands[SH_MAX_COMMANDS];
    size_t count;
} ShPipeline;

typedef struct {
    int active;
    int job_id;
    int pid_count;
    int pids[SH_MAX_COMMANDS];
    char command[SH_MAX_LINE];
} ShJob;

typedef struct {
    int active;
    char name[64];
    char value[SH_MAX_LINE];
} ShAlias;

typedef struct {
    int active;
    char name[64];
    char body[SH_MAX_LINE];
} ShFunction;

static int contains_slash(const char *text);
static char *scan_quoted_text(char *scan, char quote);
static int run_line(char *line);

static int shell_should_exit = 0;
static int shell_exit_status = 0;
static int shell_last_status = 0;
static int shell_next_job_id = 1;
static char shell_self_path[SH_MAX_LINE];
static char shell_history[SH_MAX_HISTORY][SH_MAX_LINE];
static int shell_history_count = 0;
static int shell_next_heredoc_id = 1;
static ShJob shell_jobs[SH_MAX_JOBS];
static ShAlias shell_aliases[SH_MAX_ALIASES];
static ShFunction shell_functions[SH_MAX_FUNCTIONS];

static int is_operator_char(char ch) {
    return ch == '|' || ch == '<' || ch == '>';
}

static int is_name_start_char(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

static int is_name_char(char ch) {
    return is_name_start_char(ch) || (ch >= '0' && ch <= '9');
}

static void add_history_entry(const char *line) {
    int index;

    if (line == 0 || line[0] == '\0') {
        return;
    }

    if (shell_history_count > 0 && rt_strcmp(shell_history[shell_history_count - 1], line) == 0) {
        return;
    }

    if (shell_history_count < SH_MAX_HISTORY) {
        rt_copy_string(shell_history[shell_history_count], sizeof(shell_history[0]), line);
        shell_history_count += 1;
        return;
    }

    for (index = 1; index < SH_MAX_HISTORY; ++index) {
        rt_copy_string(shell_history[index - 1], sizeof(shell_history[0]), shell_history[index]);
    }
    rt_copy_string(shell_history[SH_MAX_HISTORY - 1], sizeof(shell_history[0]), line);
}

static int path_exists_as_file(const char *path) {
    PlatformDirEntry entry;
    return platform_get_path_info(path, &entry) == 0 && !entry.is_dir;
}

static void get_shell_tool_dir(char *buffer, size_t buffer_size) {
    size_t len = rt_strlen(shell_self_path);
    size_t i;

    if (!contains_slash(shell_self_path)) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    if (len + 1 > buffer_size) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    memcpy(buffer, shell_self_path, len + 1);
    for (i = len; i > 0; --i) {
        if (buffer[i - 1] == '/') {
            if (i == 1) {
                buffer[1] = '\0';
            } else {
                buffer[i - 1] = '\0';
            }
            return;
        }
    }

    rt_copy_string(buffer, buffer_size, ".");
}

static int search_command_in_path_list(const char *path_list, const char *name, char *buffer, size_t buffer_size) {
    size_t i = 0;

    while (path_list != 0 && path_list[i] != '\0') {
        char dir[SH_MAX_LINE];
        size_t length = 0;

        while (path_list[i] != '\0' && path_list[i] != ':') {
            if (length + 1 < sizeof(dir)) {
                dir[length++] = path_list[i];
            }
            i += 1;
        }

        dir[length] = '\0';
        if (path_list[i] == ':') {
            i += 1;
        }

        if (dir[0] == '\0') {
            rt_copy_string(dir, sizeof(dir), ".");
        }

        if (tool_join_path(dir, name, buffer, buffer_size) == 0 && path_exists_as_file(buffer)) {
            return 0;
        }
    }

    return -1;
}

static int is_shell_builtin_name(const char *name) {
    static const char *names[] = {
        "cd", "exit", "jobs", "history", "fg", "bg", "export", "unset", "command", "alias"
    };
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (rt_strcmp(name, names[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

static const char *lookup_shell_alias(const char *name) {
    int i;
    for (i = 0; i < SH_MAX_ALIASES; ++i) {
        if (shell_aliases[i].active && rt_strcmp(shell_aliases[i].name, name) == 0) {
            return shell_aliases[i].value;
        }
    }
    return 0;
}

static int set_shell_alias(const char *assignment) {
    char name[64];
    char value[SH_MAX_LINE];
    size_t i = 0;
    size_t name_len = 0;
    size_t value_len = 0;
    int slot = -1;
    int index;

    while (assignment[i] != '\0' && assignment[i] != '=') {
        if (name_len + 1 < sizeof(name)) {
            name[name_len++] = assignment[i];
        }
        i += 1;
    }

    if (assignment[i] != '=' || name_len == 0) {
        return -1;
    }

    name[name_len] = '\0';
    i += 1;
    while (assignment[i] != '\0' && value_len + 1 < sizeof(value)) {
        value[value_len++] = assignment[i++];
    }
    value[value_len] = '\0';

    for (index = 0; index < SH_MAX_ALIASES; ++index) {
        if (shell_aliases[index].active && rt_strcmp(shell_aliases[index].name, name) == 0) {
            slot = index;
            break;
        }
        if (!shell_aliases[index].active && slot < 0) {
            slot = index;
        }
    }

    if (slot < 0) {
        return -1;
    }

    shell_aliases[slot].active = 1;
    rt_copy_string(shell_aliases[slot].name, sizeof(shell_aliases[slot].name), name);
    rt_copy_string(shell_aliases[slot].value, sizeof(shell_aliases[slot].value), value);
    return 0;
}

static const char *lookup_shell_function(const char *name) {
    int i;
    for (i = 0; i < SH_MAX_FUNCTIONS; ++i) {
        if (shell_functions[i].active && rt_strcmp(shell_functions[i].name, name) == 0) {
            return shell_functions[i].body;
        }
    }
    return 0;
}

static int set_shell_function(const char *name, const char *body) {
    int slot = -1;
    int i;

    for (i = 0; i < SH_MAX_FUNCTIONS; ++i) {
        if (shell_functions[i].active && rt_strcmp(shell_functions[i].name, name) == 0) {
            slot = i;
            break;
        }
        if (!shell_functions[i].active && slot < 0) {
            slot = i;
        }
    }

    if (slot < 0) {
        return -1;
    }

    shell_functions[slot].active = 1;
    rt_copy_string(shell_functions[slot].name, sizeof(shell_functions[slot].name), name);
    rt_copy_string(shell_functions[slot].body, sizeof(shell_functions[slot].body), body);
    return 0;
}

static int resolve_shell_command_path(const char *name, char *buffer, size_t buffer_size) {
    char self_dir[SH_MAX_LINE];
    const char *path_list = "/bin:/usr/bin:/usr/local/bin";

    if (contains_slash(name)) {
        if (path_exists_as_file(name)) {
            rt_copy_string(buffer, buffer_size, name);
            return 0;
        }
        return -1;
    }

    get_shell_tool_dir(self_dir, sizeof(self_dir));
    if (tool_join_path(self_dir, name, buffer, buffer_size) == 0 && path_exists_as_file(buffer)) {
        return 0;
    }

#if __STDC_HOSTED__
    {
        const char *host_path = getenv("PATH");
        if (host_path != 0) {
            path_list = host_path;
        }
    }
#endif

    return search_command_in_path_list(path_list, name, buffer, buffer_size);
}

static int read_line_from_fd(int fd, char *buffer, size_t buffer_size, int *eof_out) {
    size_t length = 0;
    *eof_out = 0;

    while (length + 1 < buffer_size) {
        char ch = '\0';
        long bytes_read = platform_read(fd, &ch, 1);

        if (bytes_read < 0) {
            return -1;
        }

        if (bytes_read == 0) {
            *eof_out = 1;
            break;
        }

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            break;
        }

        buffer[length++] = ch;
    }

    buffer[length] = '\0';
    return 0;
}

static int build_heredoc_temp_path(char *buffer, size_t buffer_size) {
    char id_text[32];
    size_t prefix_len;
    size_t i = 0;

    rt_unsigned_to_string((unsigned long long)shell_next_heredoc_id++, id_text, sizeof(id_text));
    rt_copy_string(buffer, buffer_size, "/tmp/newos-sh-heredoc-");
    prefix_len = rt_strlen(buffer);

    while (id_text[i] != '\0') {
        if (prefix_len + i + 1 >= buffer_size) {
            return -1;
        }
        buffer[prefix_len + i] = id_text[i];
        i += 1;
    }
    buffer[prefix_len + i] = '\0';
    return 0;
}

static void skip_spaces(char **cursor) {
    while (**cursor != '\0' && rt_is_space(**cursor)) {
        *cursor += 1;
    }
}

static int contains_slash(const char *text) {
    size_t i = 0;

    while (text[i] != '\0') {
        if (text[i] == '/') {
            return 1;
        }
        i += 1;
    }

    return 0;
}

static int contains_wildcards(const char *text) {
    size_t i = 0;

    while (text[i] != '\0') {
        if (text[i] == '*' || text[i] == '?') {
            return 1;
        }
        i += 1;
    }

    return 0;
}

static int set_shell_self_path(const char *argv0) {
    if (argv0 == 0 || argv0[0] == '\0') {
        rt_copy_string(shell_self_path, sizeof(shell_self_path), "sh");
        return 0;
    }

    if (argv0[0] == '/' || !contains_slash(argv0)) {
        rt_copy_string(shell_self_path, sizeof(shell_self_path), argv0);
        return 0;
    }

    {
        char cwd[SH_MAX_LINE];
        if (platform_get_current_directory(cwd, sizeof(cwd)) != 0 || tool_join_path(cwd, argv0, shell_self_path, sizeof(shell_self_path)) != 0) {
            rt_copy_string(shell_self_path, sizeof(shell_self_path), argv0);
        }
    }

    return 0;
}

static int run_command_substitution(const char *command, char *output, size_t output_size) {
    int pipe_fds[2];
    int pid;
    int status = 0;
    size_t length = 0;
    char *const argv[] = { shell_self_path, "-c", (char *)command, 0 };

    if (platform_create_pipe(pipe_fds) != 0) {
        return -1;
    }

    if (platform_spawn_process(argv, -1, pipe_fds[1], 0, 0, 0, &pid) != 0) {
        platform_close(pipe_fds[0]);
        platform_close(pipe_fds[1]);
        return -1;
    }

    platform_close(pipe_fds[1]);

    while (length + 1 < output_size) {
        long bytes = platform_read(pipe_fds[0], output + length, output_size - length - 1);
        if (bytes < 0) {
            platform_close(pipe_fds[0]);
            return -1;
        }
        if (bytes == 0) {
            break;
        }
        length += (size_t)bytes;
    }

    output[length] = '\0';
    platform_close(pipe_fds[0]);
    (void)platform_wait_process(pid, &status);

    while (length > 0 && (output[length - 1] == '\n' || output[length - 1] == '\r' || output[length - 1] == ' ')) {
        output[length - 1] = '\0';
        length -= 1;
    }

    {
        size_t i;
        for (i = 0; i < length; ++i) {
            if (output[i] == '\n' || output[i] == '\r' || output[i] == '\t') {
                output[i] = ' ';
            }
        }
    }

    return 0;
}

static int expand_command_substitutions(char *line) {
    char result[SH_MAX_LINE];
    size_t out = 0;
    size_t i = 0;

    while (line[i] != '\0') {
        if (line[i] == '\\' && line[i + 1] != '\0') {
            if (out + 2 >= sizeof(result)) {
                return -1;
            }
            result[out++] = line[i++];
            result[out++] = line[i++];
            continue;
        }

        if (line[i] == '\'') {
            if (out + 1 >= sizeof(result)) {
                return -1;
            }
            result[out++] = line[i++];
            while (line[i] != '\0') {
                if (out + 1 >= sizeof(result)) {
                    return -1;
                }
                result[out++] = line[i];
                if (line[i++] == '\'') {
                    break;
                }
            }
            continue;
        }

        if (line[i] == '$' && line[i + 1] == '(') {
            char command[SH_MAX_LINE];
            char captured[SH_CAPTURE_CAPACITY];
            size_t cmd_len = 0;
            int depth = 1;

            i += 2;
            while (line[i] != '\0' && depth > 0) {
                if (line[i] == '\\' && line[i + 1] != '\0') {
                    if (cmd_len + 2 >= sizeof(command)) {
                        return -1;
                    }
                    command[cmd_len++] = line[i++];
                    command[cmd_len++] = line[i++];
                    continue;
                }
                if (line[i] == '\'') {
                    if (cmd_len + 1 >= sizeof(command)) {
                        return -1;
                    }
                    command[cmd_len++] = line[i++];
                    while (line[i] != '\0') {
                        if (cmd_len + 1 >= sizeof(command)) {
                            return -1;
                        }
                        command[cmd_len++] = line[i];
                        if (line[i++] == '\'') {
                            break;
                        }
                    }
                    continue;
                }
                if (line[i] == '$' && line[i + 1] == '(') {
                    depth += 1;
                    if (cmd_len + 2 >= sizeof(command)) {
                        return -1;
                    }
                    command[cmd_len++] = line[i++];
                    command[cmd_len++] = line[i++];
                    continue;
                }
                if (line[i] == ')') {
                    depth -= 1;
                    if (depth == 0) {
                        i += 1;
                        break;
                    }
                }
                if (cmd_len + 1 >= sizeof(command)) {
                    return -1;
                }
                command[cmd_len++] = line[i++];
            }

            if (depth != 0) {
                return -1;
            }

            command[cmd_len] = '\0';
            if (run_command_substitution(command, captured, sizeof(captured)) != 0) {
                return -1;
            }

            {
                size_t j = 0;
                while (captured[j] != '\0') {
                    if (out + 1 >= sizeof(result)) {
                        return -1;
                    }
                    result[out++] = captured[j++];
                }
            }
            continue;
        }

        if (out + 1 >= sizeof(result)) {
            return -1;
        }
        result[out++] = line[i++];
    }

    result[out] = '\0';
    memcpy(line, result, out + 1);
    return 0;
}

static int append_expanded_text(char *result, size_t *out, size_t result_size, const char *text) {
    size_t i = 0;

    while (text[i] != '\0') {
        if (*out + 1 >= result_size) {
            return -1;
        }
        result[(*out)++] = text[i++];
    }

    return 0;
}

static int expand_shell_parameters(char *line) {
    char result[SH_MAX_LINE];
    size_t out = 0;
    size_t i = 0;

    while (line[i] != '\0') {
        if (line[i] == '\\' && line[i + 1] != '\0') {
            if (out + 2 >= sizeof(result)) {
                return -1;
            }
            result[out++] = line[i++];
            result[out++] = line[i++];
            continue;
        }

        if (line[i] == '\'') {
            if (out + 1 >= sizeof(result)) {
                return -1;
            }
            result[out++] = line[i++];
            while (line[i] != '\0') {
                if (out + 1 >= sizeof(result)) {
                    return -1;
                }
                result[out++] = line[i];
                if (line[i++] == '\'') {
                    break;
                }
            }
            continue;
        }

        if (line[i] == '$') {
            char name[128];
            char value[64];
            size_t name_len = 0;
            const char *replacement = "";

            if (line[i + 1] == '?') {
                rt_unsigned_to_string((unsigned long long)(shell_last_status < 0 ? 0 : shell_last_status), value, sizeof(value));
                replacement = value;
                i += 2;
            } else if (line[i + 1] == '0') {
                replacement = shell_self_path;
                i += 2;
            } else if (line[i + 1] == '{') {
                i += 2;
                while (line[i] != '\0' && line[i] != '}' && name_len + 1 < sizeof(name)) {
                    name[name_len++] = line[i++];
                }
                if (line[i] != '}') {
                    return -1;
                }
                name[name_len] = '\0';
                i += 1;
#if __STDC_HOSTED__
                replacement = getenv(name);
                if (replacement == 0) {
                    replacement = "";
                }
#endif
            } else if (is_name_start_char(line[i + 1])) {
                i += 1;
                while (is_name_char(line[i]) && name_len + 1 < sizeof(name)) {
                    name[name_len++] = line[i++];
                }
                name[name_len] = '\0';
#if __STDC_HOSTED__
                replacement = getenv(name);
                if (replacement == 0) {
                    replacement = "";
                }
#endif
            } else {
                if (out + 1 >= sizeof(result)) {
                    return -1;
                }
                result[out++] = line[i++];
                continue;
            }

            if (append_expanded_text(result, &out, sizeof(result), replacement) != 0) {
                return -1;
            }
            continue;
        }

        if (out + 1 >= sizeof(result)) {
            return -1;
        }
        result[out++] = line[i++];
    }

    result[out] = '\0';
    memcpy(line, result, out + 1);
    return 0;
}

static void copy_trimmed_range(char *buffer, size_t buffer_size, const char *text, size_t length) {
    size_t start = 0;
    size_t end = length;
    size_t out = 0;

    while (start < length && rt_is_space(text[start])) {
        start += 1;
    }
    while (end > start && rt_is_space(text[end - 1])) {
        end -= 1;
    }

    while (start < end && out + 1 < buffer_size) {
        buffer[out++] = text[start++];
    }
    buffer[out] = '\0';
}

static int apply_alias_expansion(char *segment) {
    size_t start = 0;
    size_t end;
    char name[64];
    const char *replacement;
    char expanded[SH_MAX_LINE];
    size_t out = 0;

    while (segment[start] != '\0' && rt_is_space(segment[start])) {
        start += 1;
    }

    end = start;
    while (is_name_char(segment[end])) {
        end += 1;
    }

    if (end == start || end - start >= sizeof(name)) {
        return 0;
    }

    memcpy(name, segment + start, end - start);
    name[end - start] = '\0';
    replacement = lookup_shell_alias(name);
    if (replacement == 0) {
        return 0;
    }

    while (out < start && out + 1 < sizeof(expanded)) {
        expanded[out] = segment[out];
        out += 1;
    }

    if (append_expanded_text(expanded, &out, sizeof(expanded), replacement) != 0) {
        return -1;
    }

    if (append_expanded_text(expanded, &out, sizeof(expanded), segment + end) != 0) {
        return -1;
    }

    expanded[out] = '\0';
    memcpy(segment, expanded, out + 1);
    return 0;
}

static int maybe_store_function_definition(char *segment, int *handled_out) {
    char *cursor = segment;
    char *body_start;
    char function_name[64];
    char function_body[SH_MAX_LINE];
    size_t name_len = 0;
    int depth = 1;
    char *body_end;

    *handled_out = 0;
    skip_spaces(&cursor);
    if (!is_name_start_char(*cursor)) {
        return 0;
    }

    while (is_name_char(*cursor) && name_len + 1 < sizeof(function_name)) {
        function_name[name_len++] = *cursor++;
    }
    function_name[name_len] = '\0';

    skip_spaces(&cursor);
    if (cursor[0] != '(' || cursor[1] != ')') {
        return 0;
    }
    cursor += 2;
    skip_spaces(&cursor);
    if (*cursor != '{') {
        return 0;
    }

    body_start = cursor + 1;
    cursor += 1;
    while (*cursor != '\0' && depth > 0) {
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor += 2;
            continue;
        }
        if (*cursor == '\'' || *cursor == '"') {
            cursor = scan_quoted_text(cursor, *cursor);
            continue;
        }
        if (*cursor == '{') {
            depth += 1;
        } else if (*cursor == '}') {
            depth -= 1;
            if (depth == 0) {
                break;
            }
        }
        cursor += 1;
    }

    if (depth != 0 || *cursor != '}') {
        return 0;
    }

    body_end = cursor;
    cursor += 1;
    skip_spaces(&cursor);
    if (*cursor != '\0') {
        return 0;
    }

    copy_trimmed_range(function_body, sizeof(function_body), body_start, (size_t)(body_end - body_start));
    {
        size_t body_len = rt_strlen(function_body);
        if (body_len > 0 && function_body[body_len - 1] == ';') {
            function_body[body_len - 1] = '\0';
        }
    }

    if (set_shell_function(function_name, function_body) != 0) {
        return -1;
    }

    *handled_out = 1;
    return 0;
}

static int find_heredoc_marker(const char *line, size_t *start_out, size_t *end_out, char *delimiter, size_t delimiter_size) {
    size_t i = 0;

    while (line[i] != '\0') {
        if (line[i] == '\\' && line[i + 1] != '\0') {
            i += 2;
            continue;
        }
        if (line[i] == '\'' || line[i] == '"') {
            char quote = line[i++];
            while (line[i] != '\0' && line[i] != quote) {
                if (quote == '"' && line[i] == '\\' && line[i + 1] != '\0') {
                    i += 2;
                } else {
                    i += 1;
                }
            }
            if (line[i] == quote) {
                i += 1;
            }
            continue;
        }
        if (line[i] == '<' && line[i + 1] == '<') {
            size_t j;
            size_t out = 0;
            *start_out = i;
            i += 2;
            while (line[i] != '\0' && rt_is_space(line[i])) {
                i += 1;
            }
            if (line[i] == '\0') {
                return -1;
            }
            j = i;
            while (line[j] != '\0' && !rt_is_space(line[j]) && line[j] != '<' && line[j] != '>' && line[j] != '|' && line[j] != '&' && line[j] != ';') {
                if (out + 1 < delimiter_size) {
                    delimiter[out++] = line[j];
                }
                j += 1;
            }
            delimiter[out] = '\0';
            *end_out = j;
            return (out == 0) ? -1 : 0;
        }
        i += 1;
    }

    return -1;
}

static int prepare_heredoc_from_fd(int fd, char *line, size_t line_size) {
    char delimiter[128];
    size_t start = 0;
    size_t end = 0;

    while (find_heredoc_marker(line, &start, &end, delimiter, sizeof(delimiter)) == 0) {
        char temp_path[SH_MAX_LINE];
        char replacement[SH_MAX_LINE];
        int heredoc_fd;
        int eof = 0;
        size_t out = 0;

        if (build_heredoc_temp_path(temp_path, sizeof(temp_path)) != 0) {
            return -1;
        }

        heredoc_fd = platform_open_write(temp_path, 0600U);
        if (heredoc_fd < 0) {
            return -1;
        }

        for (;;) {
            char input_line[SH_MAX_LINE];
            if (read_line_from_fd(fd, input_line, sizeof(input_line), &eof) != 0) {
                platform_close(heredoc_fd);
                return -1;
            }
            if (rt_strcmp(input_line, delimiter) == 0) {
                break;
            }
            if (rt_write_all(heredoc_fd, input_line, rt_strlen(input_line)) != 0 || rt_write_char(heredoc_fd, '\n') != 0) {
                platform_close(heredoc_fd);
                return -1;
            }
            if (eof) {
                break;
            }
        }

        platform_close(heredoc_fd);

        if (start >= sizeof(replacement)) {
            return -1;
        }
        memcpy(replacement, line, start);
        out = start;
        if (append_expanded_text(replacement, &out, sizeof(replacement), "< ") != 0 ||
            append_expanded_text(replacement, &out, sizeof(replacement), temp_path) != 0 ||
            append_expanded_text(replacement, &out, sizeof(replacement), line + end) != 0) {
            return -1;
        }
        replacement[out] = '\0';

        if (rt_strlen(replacement) + 1 > line_size) {
            return -1;
        }
        memcpy(line, replacement, rt_strlen(replacement) + 1);
    }

    return 0;
}

static int parse_word(char **cursor, char **word_out, int *no_expand_out) {
    char *src = *cursor;
    char *dst = *cursor;
    char terminator;
    int saw_any = 0;
    int no_expand = 0;

    while (*src != '\0' && !rt_is_space(*src) && !is_operator_char(*src)) {
        if (*src == '\\' && src[1] != '\0') {
            src += 1;
            *dst++ = *src++;
            saw_any = 1;
            no_expand = 1;
        } else if (*src == '\'') {
            src += 1;
            no_expand = 1;
            while (*src != '\0' && *src != '\'') {
                *dst++ = *src++;
                saw_any = 1;
            }
            if (*src != '\'') {
                return -1;
            }
            src += 1;
        } else if (*src == '"') {
            src += 1;
            no_expand = 1;
            while (*src != '\0' && *src != '"') {
                if (*src == '\\' && src[1] != '\0') {
                    src += 1;
                }
                *dst++ = *src++;
                saw_any = 1;
            }
            if (*src != '"') {
                return -1;
            }
            src += 1;
        } else {
            *dst++ = *src++;
            saw_any = 1;
        }
    }

    if (!saw_any) {
        return -1;
    }

    terminator = *src;
    *dst = '\0';
    *word_out = *cursor;
    *no_expand_out = no_expand;

    if (terminator != '\0' && !is_operator_char(terminator)) {
        src += 1;
    }

    *cursor = src;
    return 0;
}

static int expand_command_globs(ShCommand *command) {
    char *expanded_argv[SH_MAX_ARGS + 1];
    char storage[SH_MAX_ARGS][SH_MAX_LINE];
    int new_argc = 0;
    int arg_index;

    for (arg_index = 0; arg_index < command->argc; ++arg_index) {
        const char *arg = command->argv[arg_index];

        if (command->no_expand[arg_index] || !contains_wildcards(arg)) {
            expanded_argv[new_argc++] = command->argv[arg_index];
            continue;
        }

        {
            PlatformDirEntry entries[SH_ENTRY_CAPACITY];
            size_t count = 0;
            int is_directory = 0;
            char dir_path[SH_MAX_LINE];
            char pattern[SH_MAX_LINE];
            const char *slash = 0;
            size_t i = 0;
            int matched = 0;

            while (arg[i] != '\0') {
                if (arg[i] == '/') {
                    slash = arg + i;
                }
                i += 1;
            }

            if (slash != 0) {
                size_t dir_len = (size_t)(slash - arg);
                if (dir_len + 1 > sizeof(dir_path)) {
                    return -1;
                }
                memcpy(dir_path, arg, dir_len);
                dir_path[dir_len] = '\0';
                rt_copy_string(pattern, sizeof(pattern), slash + 1);
            } else {
                rt_copy_string(dir_path, sizeof(dir_path), ".");
                rt_copy_string(pattern, sizeof(pattern), arg);
            }

            if (platform_collect_entries(dir_path, pattern[0] == '.', entries, SH_ENTRY_CAPACITY, &count, &is_directory) == 0 && is_directory) {
                size_t entry_index;

                for (entry_index = 0; entry_index < count; ++entry_index) {
                    if (entries[entry_index].name[0] == '\0') {
                        continue;
                    }
                    if (tool_wildcard_match(pattern, entries[entry_index].name)) {
                        int slot;

                        if (new_argc >= SH_MAX_ARGS) {
                            return -1;
                        }
                        slot = new_argc;
                        if (slash != 0) {
                            if (tool_join_path(dir_path, entries[entry_index].name, storage[slot], sizeof(storage[slot])) != 0) {
                                return -1;
                            }
                        } else {
                            rt_copy_string(storage[slot], sizeof(storage[slot]), entries[entry_index].name);
                        }
                        expanded_argv[slot] = storage[slot];
                        new_argc += 1;
                        matched = 1;
                    }
                }
            }

            if (!matched) {
                expanded_argv[new_argc++] = command->argv[arg_index];
            }
        }
    }

    for (arg_index = 0; arg_index < new_argc; ++arg_index) {
        command->argv[arg_index] = expanded_argv[arg_index];
    }
    command->argv[new_argc] = 0;
    command->argc = new_argc;
    return 0;
}

static void cleanup_pipeline_temp_inputs(const ShPipeline *pipeline) {
    size_t i;

    for (i = 0; i < pipeline->count; ++i) {
        const char *path = pipeline->commands[i].input_path;
        if (path != 0 && path[0] == '/' && rt_strcmp(path, "/tmp/newos-sh-heredoc-") != 0) {
            const char *prefix = "/tmp/newos-sh-heredoc-";
            size_t j = 0;
            while (prefix[j] != '\0' && path[j] == prefix[j]) {
                j += 1;
            }
            if (prefix[j] == '\0') {
                (void)platform_remove_file(path);
            }
        }
    }
}

static int parse_pipeline(char *line, ShPipeline *pipeline, int *empty_out) {
    char *cursor = line;
    ShCommand *current;
    size_t i;

    rt_memset(pipeline, 0, sizeof(*pipeline));
    pipeline->count = 1;
    current = &pipeline->commands[0];
    *empty_out = 0;

    for (;;) {
        char *word;
        int no_expand = 0;

        skip_spaces(&cursor);
        if (*cursor == '\0') {
            break;
        }

        if (*cursor == '|') {
            if (current->argc == 0) {
                return -1;
            }
            current->argv[current->argc] = 0;
            cursor += 1;
            if (pipeline->count >= SH_MAX_COMMANDS) {
                return -1;
            }
            current = &pipeline->commands[pipeline->count++];
            continue;
        }

        if (*cursor == '<') {
            cursor += 1;
            skip_spaces(&cursor);
            if (parse_word(&cursor, &word, &no_expand) != 0) {
                return -1;
            }
            current->input_path = word;
            continue;
        }

        if (*cursor == '>') {
            int append = 0;
            cursor += 1;
            if (*cursor == '>') {
                append = 1;
                cursor += 1;
            }
            skip_spaces(&cursor);
            if (parse_word(&cursor, &word, &no_expand) != 0) {
                return -1;
            }
            current->output_path = word;
            current->output_append = append;
            continue;
        }

        if (parse_word(&cursor, &word, &no_expand) != 0) {
            return -1;
        }

        if (current->argc >= SH_MAX_ARGS) {
            return -1;
        }

        current->argv[current->argc] = word;
        current->no_expand[current->argc] = no_expand;
        current->argc += 1;
    }

    if (pipeline->count == 1 && current->argc == 0 && current->input_path == 0 && current->output_path == 0) {
        *empty_out = 1;
        return 0;
    }

    for (i = 0; i < pipeline->count; ++i) {
        if (pipeline->commands[i].argc == 0) {
            return -1;
        }
        if (expand_command_globs(&pipeline->commands[i]) != 0) {
            return -1;
        }
        pipeline->commands[i].argv[pipeline->commands[i].argc] = 0;
    }

    return 0;
}

static ShJob *allocate_job_slot(void) {
    int i;
    for (i = 0; i < SH_MAX_JOBS; ++i) {
        if (!shell_jobs[i].active) {
            return &shell_jobs[i];
        }
    }
    return 0;
}

static ShJob *find_job_by_id(int job_id) {
    int i;
    ShJob *fallback = 0;

    for (i = 0; i < SH_MAX_JOBS; ++i) {
        if (shell_jobs[i].active) {
            fallback = &shell_jobs[i];
            if (shell_jobs[i].job_id == job_id) {
                return &shell_jobs[i];
            }
        }
    }

    return (job_id == 0) ? fallback : 0;
}

static void remember_job(const int *pids, size_t pid_count, const char *command_text) {
    ShJob *job = allocate_job_slot();
    size_t i;

    if (job == 0) {
        return;
    }

    job->active = 1;
    job->job_id = shell_next_job_id++;
    job->pid_count = (int)pid_count;
    for (i = 0; i < pid_count; ++i) {
        job->pids[i] = pids[i];
    }
    rt_copy_string(job->command, sizeof(job->command), command_text);
}

static int execute_pipeline(const ShPipeline *pipeline, int background, const char *command_text) {
    int pids[SH_MAX_COMMANDS];
    int prev_read_fd = -1;
    int exit_status = 0;
    size_t i;

    for (i = 0; i < pipeline->count; ++i) {
        int pipe_fds[2] = { -1, -1 };
        int stdin_fd = prev_read_fd;
        int stdout_fd = -1;

        if (i + 1 < pipeline->count) {
            if (platform_create_pipe(pipe_fds) != 0) {
                if (prev_read_fd >= 0) {
                    platform_close(prev_read_fd);
                }
                rt_write_line(2, "sh: pipe creation failed");
                return 1;
            }
            stdout_fd = pipe_fds[1];
        }

        if (platform_spawn_process(
                pipeline->commands[i].argv,
                stdin_fd,
                stdout_fd,
                pipeline->commands[i].input_path,
                pipeline->commands[i].output_path,
                pipeline->commands[i].output_append,
                &pids[i]) != 0) {
            if (stdin_fd >= 0) {
                platform_close(stdin_fd);
            }
            if (pipe_fds[0] >= 0) {
                platform_close(pipe_fds[0]);
            }
            if (pipe_fds[1] >= 0) {
                platform_close(pipe_fds[1]);
            }
            rt_write_cstr(2, "sh: failed to execute ");
            rt_write_line(2, pipeline->commands[i].argv[0]);
            return 127;
        }

        if (stdin_fd >= 0) {
            platform_close(stdin_fd);
        }
        if (stdout_fd >= 0) {
            platform_close(stdout_fd);
        }

        prev_read_fd = (i + 1 < pipeline->count) ? pipe_fds[0] : -1;
    }

    if (prev_read_fd >= 0) {
        platform_close(prev_read_fd);
    }

    if (background) {
        remember_job(pids, pipeline->count, command_text);
        return 0;
    }

    for (i = 0; i < pipeline->count; ++i) {
        int status = 1;
        if (platform_wait_process(pids[i], &status) != 0) {
            rt_write_line(2, "sh: wait failed");
            return 1;
        }
        if (i + 1 == pipeline->count) {
            exit_status = status;
        }
    }

    return exit_status;
}

static int builtin_jobs(void) {
    int i;

    for (i = 0; i < SH_MAX_JOBS; ++i) {
        if (shell_jobs[i].active) {
            rt_write_char(1, '[');
            rt_write_uint(1, (unsigned long long)shell_jobs[i].job_id);
            rt_write_cstr(1, "] running ");
            rt_write_line(1, shell_jobs[i].command);
        }
    }

    return 0;
}

static int builtin_history(void) {
    int i;

    for (i = 0; i < shell_history_count; ++i) {
        rt_write_uint(1, (unsigned long long)(i + 1));
        rt_write_cstr(1, "  ");
        rt_write_line(1, shell_history[i]);
    }

    return 0;
}

static int builtin_fg(int argc, char **argv) {
    unsigned long long value = 0;
    ShJob *job;
    int last_status = 0;
    int i;

    if (argc >= 2 && rt_parse_uint(argv[1], &value) != 0) {
        rt_write_line(2, "sh: fg requires numeric job id");
        return 2;
    }

    job = find_job_by_id((argc >= 2) ? (int)value : 0);
    if (job == 0) {
        rt_write_line(2, "sh: no such job");
        return 1;
    }

    for (i = 0; i < job->pid_count; ++i) {
        int status = 0;
        if (platform_wait_process(job->pids[i], &status) != 0) {
            return 1;
        }
        last_status = status;
    }

    job->active = 0;
    return last_status;
}

static int builtin_bg(int argc, char **argv) {
    unsigned long long value = 0;
    ShJob *job;

    if (argc >= 2 && rt_parse_uint(argv[1], &value) != 0) {
        rt_write_line(2, "sh: bg requires numeric job id");
        return 2;
    }

    job = find_job_by_id((argc >= 2) ? (int)value : 0);
    if (job == 0) {
        rt_write_line(2, "sh: no such job");
        return 1;
    }

    return 0;
}

static int builtin_cd(const ShCommand *cmd) {
    const char *path = (cmd->argc >= 2) ? cmd->argv[1] : ".";

    if (platform_change_directory(path) != 0) {
        rt_write_cstr(2, "sh: cd failed: ");
        rt_write_line(2, path);
        return 1;
    }

    return 0;
}

static int builtin_exit_command(const ShCommand *cmd) {
    unsigned long long code = 0;

    if (cmd->argc >= 2 && rt_parse_uint(cmd->argv[1], &code) != 0) {
        rt_write_line(2, "sh: exit requires numeric status");
        return 2;
    }

    shell_should_exit = 1;
    shell_exit_status = (cmd->argc >= 2) ? (int)code : 0;
    return shell_exit_status;
}

static int builtin_export_command(const ShCommand *cmd) {
#if __STDC_HOSTED__
    int i;

    for (i = 1; i < cmd->argc; ++i) {
        char *arg = cmd->argv[i];
        char *eq = arg;

        while (*eq != '\0' && *eq != '=') {
            eq += 1;
        }

        if (*eq == '=') {
            *eq = '\0';
            setenv(arg, eq + 1, 1);
            *eq = '=';
        }
    }
#else
    (void)cmd;
#endif

    return 0;
}

static int builtin_unset_command(const ShCommand *cmd) {
#if __STDC_HOSTED__
    int i;

    for (i = 1; i < cmd->argc; ++i) {
        unsetenv(cmd->argv[i]);
    }
#else
    (void)cmd;
#endif

    return 0;
}

static int builtin_alias_command(const ShCommand *cmd) {
    int i;

    if (cmd->argc == 1) {
        for (i = 0; i < SH_MAX_ALIASES; ++i) {
            if (shell_aliases[i].active) {
                rt_write_cstr(1, shell_aliases[i].name);
                rt_write_cstr(1, "='");
                rt_write_cstr(1, shell_aliases[i].value);
                rt_write_line(1, "'");
            }
        }
        return 0;
    }

    for (i = 1; i < cmd->argc; ++i) {
        if (set_shell_alias(cmd->argv[i]) != 0) {
            const char *value = lookup_shell_alias(cmd->argv[i]);
            if (value != 0) {
                rt_write_line(1, value);
            } else {
                rt_write_cstr(2, "sh: invalid alias: ");
                rt_write_line(2, cmd->argv[i]);
                return 1;
            }
        }
    }

    return 0;
}

static int builtin_command_command(const ShCommand *cmd) {
    int i;
    int exit_code = 0;

    if (cmd->argc < 3 || rt_strcmp(cmd->argv[1], "-v") != 0) {
        rt_write_line(2, "Usage: command -v NAME...");
        return 2;
    }

    for (i = 2; i < cmd->argc; ++i) {
        char path[SH_MAX_LINE];
        if (is_shell_builtin_name(cmd->argv[i]) || lookup_shell_alias(cmd->argv[i]) != 0 || lookup_shell_function(cmd->argv[i]) != 0) {
            rt_write_line(1, cmd->argv[i]);
        } else if (resolve_shell_command_path(cmd->argv[i], path, sizeof(path)) == 0) {
            rt_write_line(1, path);
        } else {
            exit_code = 1;
        }
    }

    return exit_code;
}

static int try_run_builtin(const ShPipeline *pipeline, int *status_out) {
    ShCommand *cmd;

    if (pipeline->count != 1 || pipeline->commands[0].argc == 0) {
        return 0;
    }

    cmd = (ShCommand *)&pipeline->commands[0];

    if (rt_strcmp(cmd->argv[0], "cd") == 0) {
        *status_out = builtin_cd(cmd);
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "exit") == 0) {
        *status_out = builtin_exit_command(cmd);
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "jobs") == 0) {
        *status_out = builtin_jobs();
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "history") == 0) {
        *status_out = builtin_history();
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "fg") == 0) {
        *status_out = builtin_fg(cmd->argc, cmd->argv);
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "bg") == 0) {
        *status_out = builtin_bg(cmd->argc, cmd->argv);
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "export") == 0) {
        *status_out = builtin_export_command(cmd);
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "unset") == 0) {
        *status_out = builtin_unset_command(cmd);
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "alias") == 0) {
        *status_out = builtin_alias_command(cmd);
        return 1;
    }

    if (rt_strcmp(cmd->argv[0], "command") == 0) {
        *status_out = builtin_command_command(cmd);
        return 1;
    }

    return 0;
}

static int run_simple_command(char *segment, int background) {
    ShPipeline pipeline;
    int empty = 0;
    int status = 0;
    int handled = 0;

    if (expand_command_substitutions(segment) != 0 || expand_shell_parameters(segment) != 0) {
        rt_write_line(2, "sh: expansion failed");
        return 2;
    }

    if (maybe_store_function_definition(segment, &handled) != 0) {
        rt_write_line(2, "sh: function definition failed");
        return 2;
    }
    if (handled) {
        return 0;
    }

    if (apply_alias_expansion(segment) != 0) {
        rt_write_line(2, "sh: alias expansion failed");
        return 2;
    }

    if (parse_pipeline(segment, &pipeline, &empty) != 0) {
        rt_write_line(2, "sh: syntax error");
        return 2;
    }

    if (empty) {
        return 0;
    }

    if (pipeline.count == 1 && pipeline.commands[0].argc > 0) {
        const char *body = lookup_shell_function(pipeline.commands[0].argv[0]);
        if (body != 0) {
            char body_copy[SH_MAX_LINE];
            rt_copy_string(body_copy, sizeof(body_copy), body);
            cleanup_pipeline_temp_inputs(&pipeline);
            return run_line(body_copy);
        }
    }

    if (try_run_builtin(&pipeline, &status)) {
        cleanup_pipeline_temp_inputs(&pipeline);
        return status;
    }

    status = execute_pipeline(&pipeline, background, segment);
    cleanup_pipeline_temp_inputs(&pipeline);
    return status;
}

static char *scan_quoted_text(char *scan, char quote) {
    scan += 1;
    while (*scan != '\0' && *scan != quote) {
        if (quote == '"' && *scan == '\\' && scan[1] != '\0') {
            scan += 2;
        } else {
            scan += 1;
        }
    }

    if (*scan == quote) {
        scan += 1;
    }

    return scan;
}

static char *find_segment_end(char *scan, ShNextMode *next_mode) {
    int brace_depth = 0;
    *next_mode = SH_NEXT_ALWAYS;

    while (*scan != '\0') {
        if (*scan == '\\' && scan[1] != '\0') {
            scan += 2;
            continue;
        }

        if (*scan == '\'' || *scan == '"') {
            scan = scan_quoted_text(scan, *scan);
            continue;
        }

        if (*scan == '{') {
            brace_depth += 1;
            scan += 1;
            continue;
        }

        if (*scan == '}') {
            if (brace_depth > 0) {
                brace_depth -= 1;
            }
            scan += 1;
            continue;
        }

        if (brace_depth == 0 && *scan == ';') {
            *next_mode = SH_NEXT_ALWAYS;
            break;
        }

        if (brace_depth == 0 && scan[0] == '&' && scan[1] == '&') {
            *next_mode = SH_NEXT_AND;
            break;
        }

        if (brace_depth == 0 && scan[0] == '|' && scan[1] == '|') {
            *next_mode = SH_NEXT_OR;
            break;
        }

        if (brace_depth == 0 && *scan == '&') {
            *next_mode = SH_NEXT_BACKGROUND;
            break;
        }

        scan += 1;
    }

    return scan;
}

static int run_line(char *line) {
    char *cursor = line;
    int last_status = 0;
    int should_run = 1;

    while (*cursor != '\0') {
        char *segment_start;
        char *scan;
        ShNextMode next_mode;

        skip_spaces(&cursor);
        if (*cursor == '\0') {
            break;
        }

        segment_start = cursor;
        scan = find_segment_end(cursor, &next_mode);

        if (*scan != '\0') {
            if (next_mode == SH_NEXT_AND || next_mode == SH_NEXT_OR) {
                *scan = '\0';
                scan[1] = '\0';
                cursor = scan + 2;
            } else {
                *scan = '\0';
                cursor = scan + 1;
            }
        } else {
            cursor = scan;
        }

        if (should_run) {
            last_status = run_simple_command(segment_start, next_mode == SH_NEXT_BACKGROUND);
            shell_last_status = last_status;
            if (shell_should_exit) {
                return last_status;
            }
        }

        if (next_mode == SH_NEXT_AND) {
            should_run = (last_status == 0);
        } else if (next_mode == SH_NEXT_OR) {
            should_run = (last_status != 0);
        } else {
            should_run = 1;
        }
    }

    return last_status;
}

#if __STDC_HOSTED__
static int shell_is_interactive(int fd) {
    return fd == 0 && isatty(fd);
}

static void refresh_input_line(const char *prompt, const char *line, size_t length, size_t cursor) {
    size_t backspaces;

    rt_write_char(1, '\r');
    rt_write_cstr(1, prompt);
    rt_write_all(1, line, length);
    rt_write_cstr(1, "\033[K");

    backspaces = length - cursor;
    while (backspaces > 0) {
        rt_write_char(1, '\b');
        backspaces -= 1;
    }
}

static int read_interactive_line(char *line, size_t line_size, int *eof_out) {
    struct termios saved;
    struct termios raw;
    size_t length = 0;
    size_t cursor = 0;
    int history_index = shell_history_count;
    char saved_current[SH_MAX_LINE];
    const char *prompt = "$ ";
    int result = 0;

    *eof_out = 0;
    saved_current[0] = '\0';
    line[0] = '\0';

    if (tcgetattr(0, &saved) != 0) {
        return -1;
    }

    raw = saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(0, TCSANOW, &raw) != 0) {
        return -1;
    }

    refresh_input_line(prompt, line, length, cursor);

    for (;;) {
        char ch = '\0';
        long bytes_read = platform_read(0, &ch, 1);

        if (bytes_read <= 0) {
            *eof_out = 1;
            result = 0;
            break;
        }

        if (ch == '\n' || ch == '\r') {
            rt_write_char(1, '\n');
            line[length] = '\0';
            result = 0;
            break;
        }

        if (ch == 4) {
            if (length == 0) {
                rt_write_char(1, '\n');
                *eof_out = 1;
                result = 0;
                break;
            }
            continue;
        }

        if (ch == 127 || ch == 8) {
            if (cursor > 0) {
                memmove(line + cursor - 1, line + cursor, length - cursor + 1);
                cursor -= 1;
                length -= 1;
                refresh_input_line(prompt, line, length, cursor);
            }
            continue;
        }

        if (ch == 27) {
            char seq[2];

            if (platform_read(0, &seq[0], 1) <= 0 || platform_read(0, &seq[1], 1) <= 0) {
                continue;
            }

            if (seq[0] == '[' && seq[1] == 'A') {
                if (history_index > 0) {
                    if (history_index == shell_history_count) {
                        rt_copy_string(saved_current, sizeof(saved_current), line);
                    }
                    history_index -= 1;
                    rt_copy_string(line, line_size, shell_history[history_index]);
                    length = rt_strlen(line);
                    cursor = length;
                    refresh_input_line(prompt, line, length, cursor);
                }
            } else if (seq[0] == '[' && seq[1] == 'B') {
                if (history_index < shell_history_count) {
                    history_index += 1;
                    if (history_index == shell_history_count) {
                        rt_copy_string(line, line_size, saved_current);
                    } else {
                        rt_copy_string(line, line_size, shell_history[history_index]);
                    }
                    length = rt_strlen(line);
                    cursor = length;
                    refresh_input_line(prompt, line, length, cursor);
                }
            } else if (seq[0] == '[' && seq[1] == 'C') {
                if (cursor < length) {
                    cursor += 1;
                    refresh_input_line(prompt, line, length, cursor);
                }
            } else if (seq[0] == '[' && seq[1] == 'D') {
                if (cursor > 0) {
                    cursor -= 1;
                    refresh_input_line(prompt, line, length, cursor);
                }
            }
            continue;
        }

        if (ch >= 32 && ch <= 126) {
            if (length + 1 >= line_size) {
                result = 2;
                break;
            }

            memmove(line + cursor + 1, line + cursor, length - cursor + 1);
            line[cursor] = ch;
            cursor += 1;
            length += 1;
            refresh_input_line(prompt, line, length, cursor);
        }
    }

    tcsetattr(0, TCSANOW, &saved);
    return result;
}

static int process_interactive_stream(void) {
    char line[SH_MAX_LINE];
    int last_status = 0;

    for (;;) {
        int eof = 0;
        int read_status = read_interactive_line(line, sizeof(line), &eof);

        if (read_status != 0) {
            rt_write_line(2, "sh: interactive input failed");
            return read_status;
        }

        if (eof) {
            break;
        }

        if (line[0] == '\0') {
            continue;
        }

        if (prepare_heredoc_from_fd(0, line, sizeof(line)) != 0) {
            rt_write_line(2, "sh: here-document failed");
            return 2;
        }

        add_history_entry(line);
        last_status = run_line(line);
        if (shell_should_exit) {
            return shell_exit_status;
        }
    }

    return shell_should_exit ? shell_exit_status : last_status;
}
#endif

static int process_stream(int fd) {
    char line[SH_MAX_LINE];
    int last_status = 0;

#if __STDC_HOSTED__
    if (shell_is_interactive(fd)) {
        return process_interactive_stream();
    }
#endif

    for (;;) {
        int eof = 0;

        if (read_line_from_fd(fd, line, sizeof(line), &eof) != 0) {
            rt_write_line(2, "sh: read failed");
            return 1;
        }

        if (eof && line[0] == '\0') {
            break;
        }

        if (line[0] != '\0') {
            if (prepare_heredoc_from_fd(fd, line, sizeof(line)) != 0) {
                rt_write_line(2, "sh: here-document failed");
                return 2;
            }
            add_history_entry(line);
        }

        last_status = run_line(line);
        if (shell_should_exit) {
            return shell_exit_status;
        }

        if (eof) {
            break;
        }
    }

    return shell_should_exit ? shell_exit_status : last_status;
}

int main(int argc, char **argv) {
    set_shell_self_path((argc > 0) ? argv[0] : "sh");

    if (argc >= 3 && rt_strcmp(argv[1], "-c") == 0) {
        char buffer[SH_MAX_LINE];
        size_t len = rt_strlen(argv[2]);

        if (len + 1 > sizeof(buffer)) {
            rt_write_line(2, "sh: command too long");
            return 2;
        }

        memcpy(buffer, argv[2], len + 1);
        return run_line(buffer);
    }

    if (argc >= 2) {
        int fd = platform_open_read(argv[1]);
        int result;

        if (fd < 0) {
            rt_write_cstr(2, "sh: cannot open ");
            rt_write_line(2, argv[1]);
            return 1;
        }

        result = process_stream(fd);
        platform_close(fd);
        return result;
    }

    return process_stream(0);
}
