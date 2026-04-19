#define _POSIX_C_SOURCE 200809L

#include "shell_shared.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define SH_COMPLETION_MAX_MATCHES 128

typedef struct {
    char text[SH_MAX_LINE];
    int is_dir;
} ShCompletionMatch;

int sh_shell_is_interactive(int fd) {
    return fd == 0 && platform_isatty(fd);
}

static int shell_is_token_separator(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
           ch == '|' || ch == '&' || ch == ';' || ch == '<' ||
           ch == '>' || ch == '(' || ch == ')';
}

static int shell_starts_with(const char *text, const char *prefix) {
    size_t i = 0;

    while (prefix[i] != '\0') {
        if (text[i] != prefix[i]) {
            return 0;
        }
        i += 1U;
    }

    return 1;
}

static void shell_append_string(char *dst, size_t dst_size, const char *suffix) {
    size_t used = rt_strlen(dst);

    if (used >= dst_size) {
        return;
    }
    rt_copy_string(dst + used, dst_size - used, suffix);
}

static void shell_append_char(char *dst, size_t dst_size, char ch) {
    size_t used = rt_strlen(dst);

    if (used + 1U >= dst_size) {
        return;
    }

    dst[used] = ch;
    dst[used + 1U] = '\0';
}

static const char *shell_base_name(const char *path) {
    size_t i;
    const char *result = path;

    if (path == 0 || path[0] == '\0') {
        return ".";
    }

    for (i = 0; path[i] != '\0'; ++i) {
        if (path[i] == '/' && path[i + 1U] != '\0') {
            result = path + i + 1U;
        }
    }

    return result;
}

static void shell_build_prompt(char *buffer, size_t buffer_size) {
    const char *template_text = platform_getenv("PS1");
    const char *user = platform_getenv("USER");
    char cwd[SH_MAX_LINE];
    char host[PLATFORM_NAME_CAPACITY];
    int have_cwd = platform_get_current_directory(cwd, sizeof(cwd)) == 0;
    size_t i = 0;

    if (buffer_size == 0U) {
        return;
    }

    buffer[0] = '\0';
    if (user == 0 || user[0] == '\0') {
        user = "user";
    }
    if (platform_get_hostname(host, sizeof(host)) != 0 || host[0] == '\0') {
        rt_copy_string(host, sizeof(host), "newos");
    }

    while (template_text != 0 && template_text[i] != '\0') {
        if (template_text[i] == '\\' && template_text[i + 1U] != '\0') {
            i += 1U;
            if (template_text[i] == 'w') {
                shell_append_string(buffer, buffer_size, have_cwd ? cwd : ".");
            } else if (template_text[i] == 'W') {
                shell_append_string(buffer, buffer_size, have_cwd ? shell_base_name(cwd) : ".");
            } else if (template_text[i] == 'u') {
                shell_append_string(buffer, buffer_size, user);
            } else if (template_text[i] == 'h') {
                shell_append_string(buffer, buffer_size, host);
            } else if (template_text[i] == '$') {
                shell_append_char(buffer, buffer_size, '$');
            } else if (template_text[i] == '\\') {
                shell_append_char(buffer, buffer_size, '\\');
            } else {
                shell_append_char(buffer, buffer_size, template_text[i]);
            }
            i += 1U;
            continue;
        }

        shell_append_char(buffer, buffer_size, template_text[i]);
        i += 1U;
    }

    if (buffer[0] == '\0') {
        if (have_cwd) {
            rt_copy_string(buffer, buffer_size, cwd);
            shell_append_string(buffer, buffer_size, "$ ");
        } else {
            rt_copy_string(buffer, buffer_size, "$ ");
        }
    }
}

static void shell_get_tool_dir(char *buffer, size_t buffer_size) {
    size_t len = rt_strlen(shell_self_path);
    size_t i;

    if (!sh_contains_slash(shell_self_path)) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    if (len + 1U > buffer_size) {
        rt_copy_string(buffer, buffer_size, ".");
        return;
    }

    memcpy(buffer, shell_self_path, len + 1U);
    for (i = len; i > 0; --i) {
        if (buffer[i - 1] == '/') {
            if (i == 1U) {
                buffer[1] = '\0';
            } else {
                buffer[i - 1] = '\0';
            }
            return;
        }
    }

    rt_copy_string(buffer, buffer_size, ".");
}

static size_t shell_find_token_start(const char *line, size_t cursor) {
    size_t start = cursor;

    while (start > 0 && !shell_is_token_separator(line[start - 1])) {
        start -= 1U;
    }
    return start;
}

static size_t shell_find_token_end(const char *line, size_t length, size_t cursor) {
    size_t end = cursor;

    while (end < length && !shell_is_token_separator(line[end])) {
        end += 1U;
    }
    return end;
}

static int shell_token_is_command_position(const char *line, size_t token_start) {
    size_t i = token_start;

    while (i > 0) {
        char ch = line[i - 1];
        if (ch == ' ' || ch == '\t') {
            i -= 1U;
            continue;
        }
        return ch == '|' || ch == '&' || ch == ';' || ch == '(';
    }

    return 1;
}

static int shell_completion_has_match(const ShCompletionMatch *matches, size_t count, const char *text) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (rt_strcmp(matches[i].text, text) == 0) {
            return 1;
        }
    }
    return 0;
}

static void shell_add_completion_match(ShCompletionMatch *matches, size_t *count_io, const char *text, int is_dir) {
    if (*count_io >= SH_COMPLETION_MAX_MATCHES || text == 0 || text[0] == '\0') {
        return;
    }
    if (shell_completion_has_match(matches, *count_io, text)) {
        return;
    }

    rt_copy_string(matches[*count_io].text, sizeof(matches[*count_io].text), text);
    matches[*count_io].is_dir = is_dir;
    *count_io += 1U;
}

static void shell_sort_completion_matches(ShCompletionMatch *matches, size_t count) {
    size_t i;
    size_t j;

    for (i = 0; i < count; ++i) {
        for (j = i + 1U; j < count; ++j) {
            if (rt_strcmp(matches[j].text, matches[i].text) < 0) {
                ShCompletionMatch tmp = matches[i];
                matches[i] = matches[j];
                matches[j] = tmp;
            }
        }
    }
}

static size_t shell_completion_common_prefix(const ShCompletionMatch *matches, size_t count) {
    size_t prefix = 0;

    if (count == 0) {
        return 0;
    }

    while (matches[0].text[prefix] != '\0') {
        size_t i;
        char ch = matches[0].text[prefix];
        for (i = 1; i < count; ++i) {
            if (matches[i].text[prefix] != ch) {
                return prefix;
            }
        }
        prefix += 1U;
    }

    return prefix;
}

static void shell_print_completion_list(const ShCompletionMatch *matches, size_t count) {
    size_t i;

    rt_write_char(1, '\n');
    for (i = 0; i < count; ++i) {
        rt_write_cstr(1, matches[i].text);
        if (i + 1U < count) {
            rt_write_cstr(1, "  ");
        }
    }
    rt_write_char(1, '\n');
}

static int shell_replace_span(char *line, size_t line_size, size_t *length_io, size_t *cursor_io, size_t start, size_t end, const char *replacement, int add_space) {
    size_t old_length = *length_io;
    size_t replace_length = rt_strlen(replacement);
    size_t tail_length;
    size_t new_length;

    if (end < start || end > old_length) {
        return -1;
    }

    tail_length = old_length - end;
    new_length = start + replace_length + tail_length + (add_space ? 1U : 0U);
    if (new_length + 1U > line_size) {
        return -1;
    }

    memmove(line + start + replace_length + (add_space ? 1U : 0U), line + end, tail_length + 1U);
    memcpy(line + start, replacement, replace_length);
    if (add_space) {
        line[start + replace_length] = ' ';
    }

    *length_io = new_length;
    *cursor_io = start + replace_length + (add_space ? 1U : 0U);
    return 0;
}

static void shell_collect_path_matches(const char *token, ShCompletionMatch *matches, size_t *count_io) {
    PlatformDirEntry entries[128];
    size_t entry_count = 0;
    int path_is_directory = 0;
    char dir_path[SH_MAX_LINE];
    char prefix[SH_MAX_LINE];
    char replace_prefix[SH_MAX_LINE];
    size_t token_length = rt_strlen(token);
    size_t slash_index = token_length;
    int include_hidden;
    size_t i;

    for (i = token_length; i > 0; --i) {
        if (token[i - 1] == '/') {
            slash_index = i - 1U;
            break;
        }
    }

    if (slash_index < token_length) {
        size_t dir_length = slash_index;
        if (dir_length == 0) {
            rt_copy_string(dir_path, sizeof(dir_path), "/");
            rt_copy_string(replace_prefix, sizeof(replace_prefix), "/");
        } else {
            if (dir_length + 1U > sizeof(dir_path) || slash_index + 2U > sizeof(replace_prefix)) {
                return;
            }
            memcpy(dir_path, token, dir_length);
            dir_path[dir_length] = '\0';
            memcpy(replace_prefix, token, slash_index + 1U);
            replace_prefix[slash_index + 1U] = '\0';
        }
        rt_copy_string(prefix, sizeof(prefix), token + slash_index + 1U);
    } else {
        rt_copy_string(dir_path, sizeof(dir_path), ".");
        rt_copy_string(replace_prefix, sizeof(replace_prefix), "");
        rt_copy_string(prefix, sizeof(prefix), token);
    }

    include_hidden = prefix[0] == '.';
    if (platform_collect_entries(dir_path, include_hidden, entries, sizeof(entries) / sizeof(entries[0]), &entry_count, &path_is_directory) != 0 || !path_is_directory) {
        return;
    }

    for (i = 0; i < entry_count; ++i) {
        char candidate[SH_MAX_LINE];

        if (!include_hidden && entries[i].is_hidden) {
            continue;
        }
        if (!shell_starts_with(entries[i].name, prefix)) {
            continue;
        }
        if (rt_strcmp(replace_prefix, "/") == 0) {
            if (rt_strcmp(entries[i].name, "/") == 0) {
                continue;
            }
        }

        rt_copy_string(candidate, sizeof(candidate), replace_prefix);
        shell_append_string(candidate, sizeof(candidate), entries[i].name);
        if (entries[i].is_dir) {
            shell_append_string(candidate, sizeof(candidate), "/");
        }
        shell_add_completion_match(matches, count_io, candidate, entries[i].is_dir);
    }
}

static void shell_collect_commands_from_dir(const char *dir, const char *prefix, ShCompletionMatch *matches, size_t *count_io) {
    PlatformDirEntry entries[128];
    size_t entry_count = 0;
    int path_is_directory = 0;
    size_t i;

    if (platform_collect_entries(dir, prefix[0] == '.', entries, sizeof(entries) / sizeof(entries[0]), &entry_count, &path_is_directory) != 0 || !path_is_directory) {
        return;
    }

    for (i = 0; i < entry_count; ++i) {
        char full_path[SH_MAX_LINE];

        if (entries[i].is_dir || !shell_starts_with(entries[i].name, prefix)) {
            continue;
        }
        if (tool_join_path(dir, entries[i].name, full_path, sizeof(full_path)) != 0) {
            continue;
        }
        if (platform_path_access(full_path, PLATFORM_ACCESS_EXECUTE) != 0) {
            continue;
        }
        shell_add_completion_match(matches, count_io, entries[i].name, 0);
    }
}

static void shell_collect_command_matches(const char *prefix, ShCompletionMatch *matches, size_t *count_io) {
    char self_dir[SH_MAX_LINE];
    const char *path_list = "/bin:/usr/bin:/usr/local/bin";
    size_t i;
    size_t index = 0;

    for (i = 0; i < sh_shell_builtin_count(); ++i) {
        const char *name = sh_shell_builtin_name_at(i);
        if (name != 0 && shell_starts_with(name, prefix)) {
            shell_add_completion_match(matches, count_io, name, 0);
        }
    }

    for (i = 0; i < SH_MAX_ALIASES; ++i) {
        if (shell_aliases[i].active && shell_starts_with(shell_aliases[i].name, prefix)) {
            shell_add_completion_match(matches, count_io, shell_aliases[i].name, 0);
        }
    }

    for (i = 0; i < SH_MAX_FUNCTIONS; ++i) {
        if (shell_functions[i].active && shell_starts_with(shell_functions[i].name, prefix)) {
            shell_add_completion_match(matches, count_io, shell_functions[i].name, 0);
        }
    }

    shell_get_tool_dir(self_dir, sizeof(self_dir));
    shell_collect_commands_from_dir(self_dir, prefix, matches, count_io);

    {
        const char *host_path = platform_getenv("PATH");
        if (host_path != 0 && host_path[0] != '\0') {
            path_list = host_path;
        }
    }

    while (path_list[index] != '\0') {
        char dir[SH_MAX_LINE];
        size_t used = 0;

        while (path_list[index] != '\0' && path_list[index] != ':') {
            if (used + 1U < sizeof(dir)) {
                dir[used++] = path_list[index];
            }
            index += 1U;
        }
        dir[used] = '\0';
        if (path_list[index] == ':') {
            index += 1U;
        }
        if (dir[0] == '\0') {
            rt_copy_string(dir, sizeof(dir), ".");
        }
        shell_collect_commands_from_dir(dir, prefix, matches, count_io);
    }
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

static void shell_attempt_completion(const char *prompt, char *line, size_t line_size, size_t *length_io, size_t *cursor_io) {
    ShCompletionMatch matches[SH_COMPLETION_MAX_MATCHES];
    size_t start = shell_find_token_start(line, *cursor_io);
    size_t end = shell_find_token_end(line, *length_io, *cursor_io);
    size_t token_length = end - start;
    char token[SH_MAX_LINE];
    size_t match_count = 0;
    int path_mode;

    if (token_length + 1U > sizeof(token)) {
        rt_write_char(1, '\a');
        return;
    }

    memcpy(token, line + start, token_length);
    token[token_length] = '\0';
    path_mode = sh_contains_slash(token) || !shell_token_is_command_position(line, start);

    if (path_mode) {
        shell_collect_path_matches(token, matches, &match_count);
    } else {
        shell_collect_command_matches(token, matches, &match_count);
    }

    if (match_count == 0) {
        rt_write_char(1, '\a');
        return;
    }

    shell_sort_completion_matches(matches, match_count);

    if (match_count == 1U) {
        int add_space = !matches[0].is_dir && (end == *length_io || !shell_is_token_separator(line[end]));
        if (shell_replace_span(line, line_size, length_io, cursor_io, start, end, matches[0].text, add_space) != 0) {
            rt_write_char(1, '\a');
            return;
        }
        refresh_input_line(prompt, line, *length_io, *cursor_io);
        return;
    }

    {
        size_t common_length = shell_completion_common_prefix(matches, match_count);
        if (common_length > token_length) {
            char prefix_text[SH_MAX_LINE];
            if (common_length + 1U > sizeof(prefix_text)) {
                rt_write_char(1, '\a');
                return;
            }
            memcpy(prefix_text, matches[0].text, common_length);
            prefix_text[common_length] = '\0';
            if (shell_replace_span(line, line_size, length_io, cursor_io, start, end, prefix_text, 0) != 0) {
                rt_write_char(1, '\a');
                return;
            }
            refresh_input_line(prompt, line, *length_io, *cursor_io);
            return;
        }
    }

    rt_write_char(1, '\a');
    shell_print_completion_list(matches, match_count);
    refresh_input_line(prompt, line, *length_io, *cursor_io);
}

static int read_interactive_line(char *line, size_t line_size, int *eof_out) {
    PlatformTerminalState saved;
    size_t length = 0;
    size_t cursor = 0;
    int history_index = shell_history_count;
    char saved_current[SH_MAX_LINE];
    char prompt[SH_MAX_LINE];
    int result = 0;
    int raw_mode_enabled = 0;

    shell_build_prompt(prompt, sizeof(prompt));
    *eof_out = 0;
    saved_current[0] = '\0';
    line[0] = '\0';

    if (platform_terminal_enable_raw_mode(0, &saved) == 0) {
        raw_mode_enabled = 1;
    } else {
        if (platform_isatty(0)) {
            rt_write_cstr(1, prompt);
        }
        return sh_read_line_from_fd(0, line, line_size, eof_out);
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

        if (ch == 1) {
            cursor = 0;
            refresh_input_line(prompt, line, length, cursor);
            continue;
        }

        if (ch == 5) {
            cursor = length;
            refresh_input_line(prompt, line, length, cursor);
            continue;
        }

        if (ch == 11) {
            line[cursor] = '\0';
            length = cursor;
            refresh_input_line(prompt, line, length, cursor);
            continue;
        }

        if (ch == 12) {
            rt_write_cstr(1, "\033[2J\033[H");
            refresh_input_line(prompt, line, length, cursor);
            continue;
        }

        if (ch == 21) {
            if (cursor > 0) {
                memmove(line, line + cursor, length - cursor + 1U);
                length -= cursor;
                cursor = 0;
                refresh_input_line(prompt, line, length, cursor);
            }
            continue;
        }

        if (ch == 23) {
            size_t word_start = cursor;
            while (word_start > 0 && (line[word_start - 1] == ' ' || line[word_start - 1] == '\t')) {
                word_start -= 1U;
            }
            while (word_start > 0 && !shell_is_token_separator(line[word_start - 1])) {
                word_start -= 1U;
            }
            if (word_start < cursor) {
                memmove(line + word_start, line + cursor, length - cursor + 1U);
                length -= (cursor - word_start);
                cursor = word_start;
                refresh_input_line(prompt, line, length, cursor);
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
            } else if ((seq[0] == '[' || seq[0] == 'O') && seq[1] == 'H') {
                cursor = 0;
                refresh_input_line(prompt, line, length, cursor);
            } else if ((seq[0] == '[' || seq[0] == 'O') && seq[1] == 'F') {
                cursor = length;
                refresh_input_line(prompt, line, length, cursor);
            } else if (seq[0] == '[' && seq[1] == '3') {
                char tail = '\0';
                if (platform_read(0, &tail, 1) > 0 && tail == '~' && cursor < length) {
                    memmove(line + cursor, line + cursor + 1, length - cursor);
                    length -= 1U;
                    refresh_input_line(prompt, line, length, cursor);
                }
            }
            continue;
        }

        if (ch == '\t') {
            shell_attempt_completion(prompt, line, line_size, &length, &cursor);
            continue;
        }

        if ((unsigned char)ch >= 32U && ch != 127) {
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

    if (raw_mode_enabled) {
        (void)platform_terminal_restore_mode(0, &saved);
    }
    return result;
}

int sh_process_interactive_stream(int (*run_line_fn)(char *line)) {
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

        if (sh_prepare_heredoc_from_fd(0, line, sizeof(line)) != 0) {
            rt_write_line(2, "sh: here-document failed");
            return 2;
        }

        sh_add_history_entry(line);
        last_status = run_line_fn(line);
        if (shell_should_exit) {
            return shell_exit_status;
        }
    }

    return shell_should_exit ? shell_exit_status : last_status;
}
