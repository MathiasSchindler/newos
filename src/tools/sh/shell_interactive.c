#define _POSIX_C_SOURCE 200809L

#include "shell_shared.h"
#include "platform.h"
#include "runtime.h"

int sh_shell_is_interactive(int fd) {
    return fd == 0 && platform_isatty(fd);
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
    PlatformTerminalState saved;
    size_t length = 0;
    size_t cursor = 0;
    int history_index = shell_history_count;
    char saved_current[SH_MAX_LINE];
    const char *prompt = "$ ";
    int result = 0;

    *eof_out = 0;
    saved_current[0] = '\0';
    line[0] = '\0';

    if (platform_terminal_enable_raw_mode(0, &saved) != 0) {
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

    (void)platform_terminal_restore_mode(0, &saved);
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
