#define _POSIX_C_SOURCE 200809L

#include "platform.h"
#include "runtime.h"
#include "tool_util.h"
#include "shell_shared.h"

#define SH_MAX_POSITIONAL_ARGS 16
#define SH_POSITIONAL_ARG_CAPACITY 256

static int run_line(char *line);

int shell_should_exit = 0;
int shell_exit_status = 0;
int shell_last_status = 0;
int shell_next_job_id = 1;
char shell_self_path[SH_MAX_LINE];
char shell_history[SH_MAX_HISTORY][SH_MAX_LINE];
int shell_history_count = 0;
ShJob shell_jobs[SH_MAX_JOBS];
ShAlias shell_aliases[SH_MAX_ALIASES];
ShFunction shell_functions[SH_MAX_FUNCTIONS];
static int shell_positional_argc = 0;
static char shell_positional_args[SH_MAX_POSITIONAL_ARGS][SH_POSITIONAL_ARG_CAPACITY];

int sh_is_name_start_char(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

int sh_is_name_char(char ch) {
    return sh_is_name_start_char(ch) || (ch >= '0' && ch <= '9');
}

void sh_add_history_entry(const char *line) {
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

static const char *shell_get_positional_parameter(unsigned long long index) {
    if (index == 0 || index > (unsigned long long)shell_positional_argc || index > SH_MAX_POSITIONAL_ARGS) {
        return "";
    }

    return shell_positional_args[index - 1];
}

static void shell_set_positional_parameters(const ShCommand *cmd) {
    int i;

    shell_positional_argc = 0;
    for (i = 1; i < cmd->argc && shell_positional_argc < SH_MAX_POSITIONAL_ARGS; ++i) {
        rt_copy_string(shell_positional_args[shell_positional_argc], sizeof(shell_positional_args[0]), cmd->argv[i]);
        shell_positional_argc += 1;
    }
}

static void shell_restore_positional_parameters(int saved_argc, char saved_args[SH_MAX_POSITIONAL_ARGS][SH_POSITIONAL_ARG_CAPACITY]) {
    int i;

    shell_positional_argc = saved_argc;
    for (i = 0; i < saved_argc && i < SH_MAX_POSITIONAL_ARGS; ++i) {
        rt_copy_string(shell_positional_args[i], sizeof(shell_positional_args[0]), saved_args[i]);
    }
    for (; i < SH_MAX_POSITIONAL_ARGS; ++i) {
        shell_positional_args[i][0] = '\0';
    }
}

static int path_exists_as_file(const char *path) {
    PlatformDirEntry entry;
    return platform_get_path_info(path, &entry) == 0 && !entry.is_dir;
}

static void get_shell_tool_dir(char *buffer, size_t buffer_size) {
    size_t len = rt_strlen(shell_self_path);
    size_t i;

    if (!sh_contains_slash(shell_self_path)) {
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

const char *sh_lookup_shell_alias(const char *name) {
    int i;
    for (i = 0; i < SH_MAX_ALIASES; ++i) {
        if (shell_aliases[i].active && rt_strcmp(shell_aliases[i].name, name) == 0) {
            return shell_aliases[i].value;
        }
    }
    return 0;
}

int sh_set_shell_alias(const char *assignment) {
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

const char *sh_lookup_shell_function(const char *name) {
    int i;
    for (i = 0; i < SH_MAX_FUNCTIONS; ++i) {
        if (shell_functions[i].active && rt_strcmp(shell_functions[i].name, name) == 0) {
            return shell_functions[i].body;
        }
    }
    return 0;
}

int sh_set_shell_function(const char *name, const char *body) {
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

int sh_resolve_shell_command_path(const char *name, char *buffer, size_t buffer_size) {
    char self_dir[SH_MAX_LINE];
    const char *path_list = "/bin:/usr/bin:/usr/local/bin";

    if (sh_contains_slash(name)) {
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

    {
        const char *host_path = platform_getenv("PATH");
        if (host_path != 0) {
            path_list = host_path;
        }
    }

    return search_command_in_path_list(path_list, name, buffer, buffer_size);
}

int sh_read_line_from_fd(int fd, char *buffer, size_t buffer_size, int *eof_out) {
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

static int create_heredoc_temp_file(char *buffer, size_t buffer_size) {
    return platform_create_temp_file(buffer, buffer_size, SH_HEREDOC_PREFIX, 0600U);
}

void sh_skip_spaces(char **cursor) {
    while (**cursor != '\0' && rt_is_space(**cursor)) {
        *cursor += 1;
    }
}

int sh_contains_slash(const char *text) {
    size_t i = 0;

    while (text[i] != '\0') {
        if (text[i] == '/') {
            return 1;
        }
        i += 1;
    }

    return 0;
}

int sh_contains_wildcards(const char *text) {
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

    if (argv0[0] == '/' || !sh_contains_slash(argv0)) {
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
            } else if (line[i + 1] == '#') {
                rt_unsigned_to_string((unsigned long long)shell_positional_argc, value, sizeof(value));
                replacement = value;
                i += 2;
            } else if (line[i + 1] >= '1' && line[i + 1] <= '9') {
                replacement = shell_get_positional_parameter((unsigned long long)(line[i + 1] - '0'));
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
                if (rt_strcmp(name, "#") == 0) {
                    rt_unsigned_to_string((unsigned long long)shell_positional_argc, value, sizeof(value));
                    replacement = value;
                } else if (rt_is_digit_string(name)) {
                    unsigned long long index = 0;
                    if (rt_parse_uint(name, &index) != 0) {
                        return -1;
                    }
                    replacement = shell_get_positional_parameter(index);
                }
                else {
                    replacement = platform_getenv(name);
                    if (replacement == 0) {
                        replacement = "";
                    }
                }
            } else if (sh_is_name_start_char(line[i + 1])) {
                i += 1;
                while (sh_is_name_char(line[i]) && name_len + 1 < sizeof(name)) {
                    name[name_len++] = line[i++];
                }
                name[name_len] = '\0';
                replacement = platform_getenv(name);
                if (replacement == 0) {
                    replacement = "";
                }
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
    while (sh_is_name_char(segment[end])) {
        end += 1;
    }

    if (end == start || end - start >= sizeof(name)) {
        return 0;
    }

    memcpy(name, segment + start, end - start);
    name[end - start] = '\0';
    replacement = sh_lookup_shell_alias(name);
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
    sh_skip_spaces(&cursor);
    if (!sh_is_name_start_char(*cursor)) {
        return 0;
    }

    while (sh_is_name_char(*cursor) && name_len + 1 < sizeof(function_name)) {
        function_name[name_len++] = *cursor++;
    }
    function_name[name_len] = '\0';

    sh_skip_spaces(&cursor);
    if (cursor[0] != '(' || cursor[1] != ')') {
        return 0;
    }
    cursor += 2;
    sh_skip_spaces(&cursor);
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
            cursor = sh_scan_quoted_text(cursor, *cursor);
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
    sh_skip_spaces(&cursor);
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

    if (sh_set_shell_function(function_name, function_body) != 0) {
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

int sh_prepare_heredoc_from_fd(int fd, char *line, size_t line_size) {
    char delimiter[128];
    size_t start = 0;
    size_t end = 0;

    while (find_heredoc_marker(line, &start, &end, delimiter, sizeof(delimiter)) == 0) {
        char temp_path[SH_MAX_LINE];
        char replacement[SH_MAX_LINE];
        int heredoc_fd;
        int eof = 0;
        size_t out = 0;

        heredoc_fd = create_heredoc_temp_file(temp_path, sizeof(temp_path));
        if (heredoc_fd < 0) {
            return -1;
        }

        for (;;) {
            char input_line[SH_MAX_LINE];
            if (sh_read_line_from_fd(fd, input_line, sizeof(input_line), &eof) != 0) {
                platform_close(heredoc_fd);
                (void)platform_remove_file(temp_path);
                return -1;
            }
            if (rt_strcmp(input_line, delimiter) == 0) {
                break;
            }
            if (rt_write_all(heredoc_fd, input_line, rt_strlen(input_line)) != 0 || rt_write_char(heredoc_fd, '\n') != 0) {
                platform_close(heredoc_fd);
                (void)platform_remove_file(temp_path);
                return -1;
            }
            if (eof) {
                break;
            }
        }

        platform_close(heredoc_fd);

        if (start >= sizeof(replacement)) {
            (void)platform_remove_file(temp_path);
            return -1;
        }
        memcpy(replacement, line, start);
        out = start;
        if (append_expanded_text(replacement, &out, sizeof(replacement), "< ") != 0 ||
            append_expanded_text(replacement, &out, sizeof(replacement), temp_path) != 0 ||
            append_expanded_text(replacement, &out, sizeof(replacement), line + end) != 0) {
            (void)platform_remove_file(temp_path);
            return -1;
        }
        replacement[out] = '\0';

        if (rt_strlen(replacement) + 1 > line_size) {
            (void)platform_remove_file(temp_path);
            return -1;
        }
        memcpy(line, replacement, rt_strlen(replacement) + 1);
    }

    return 0;
}

static int run_simple_command(char *segment, int background) {
    ShPipeline pipeline;
    int empty = 0;
    int status = 0;
    int handled = 0;

    if (maybe_store_function_definition(segment, &handled) != 0) {
        rt_write_line(2, "sh: function definition failed");
        return 2;
    }
    if (handled) {
        return 0;
    }

    if (expand_command_substitutions(segment) != 0 || expand_shell_parameters(segment) != 0) {
        rt_write_line(2, "sh: expansion failed");
        return 2;
    }

    if (apply_alias_expansion(segment) != 0) {
        rt_write_line(2, "sh: alias expansion failed");
        return 2;
    }

    if (sh_parse_pipeline(segment, &pipeline, &empty) != 0) {
        rt_write_line(2, "sh: syntax error");
        return 2;
    }

    if (empty) {
        return 0;
    }

    if (pipeline.count == 1 && pipeline.commands[0].argc > 0) {
        const char *body = sh_lookup_shell_function(pipeline.commands[0].argv[0]);
        if (body != 0) {
            char body_copy[SH_MAX_LINE];
            char saved_args[SH_MAX_POSITIONAL_ARGS][SH_POSITIONAL_ARG_CAPACITY];
            int saved_argc = shell_positional_argc;
            int i;

            for (i = 0; i < saved_argc && i < SH_MAX_POSITIONAL_ARGS; ++i) {
                rt_copy_string(saved_args[i], sizeof(saved_args[0]), shell_positional_args[i]);
            }

            shell_set_positional_parameters(&pipeline.commands[0]);
            rt_copy_string(body_copy, sizeof(body_copy), body);
            sh_cleanup_pipeline_temp_inputs(&pipeline);
            status = run_line(body_copy);
            shell_restore_positional_parameters(saved_argc, saved_args);
            return status;
        }
    }

    if (sh_try_run_builtin(&pipeline, &status)) {
        sh_cleanup_pipeline_temp_inputs(&pipeline);
        return status;
    }

    status = sh_execute_pipeline(&pipeline, background, segment);
    sh_cleanup_pipeline_temp_inputs(&pipeline);
    return status;
}

char *sh_scan_quoted_text(char *scan, char quote) {
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
            scan = sh_scan_quoted_text(scan, *scan);
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
        char segment_copy[SH_MAX_LINE];
        ShNextMode next_mode;

        sh_skip_spaces(&cursor);
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
            rt_copy_string(segment_copy, sizeof(segment_copy), segment_start);
            last_status = run_simple_command(segment_copy, next_mode == SH_NEXT_BACKGROUND);
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

static int process_stream(int fd) {
    char line[SH_MAX_LINE];
    int last_status = 0;

    if (sh_shell_is_interactive(fd)) {
        return sh_process_interactive_stream(run_line);
    }

    for (;;) {
        int eof = 0;

        if (sh_read_line_from_fd(fd, line, sizeof(line), &eof) != 0) {
            rt_write_line(2, "sh: read failed");
            return 1;
        }

        if (eof && line[0] == '\0') {
            break;
        }

        if (line[0] != '\0') {
            if (sh_prepare_heredoc_from_fd(fd, line, sizeof(line)) != 0) {
                rt_write_line(2, "sh: here-document failed");
                return 2;
            }
            sh_add_history_entry(line);
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
