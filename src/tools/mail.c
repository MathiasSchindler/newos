#include "platform.h"
#include "runtime.h"
#include "mail/imap.h"
#include "tui.h"

#include <stddef.h>

#define MAIL_STATUS_CAPACITY 160U

typedef enum {
    MAIL_COMMAND_INTERACTIVE = 0,
    MAIL_COMMAND_LIST,
    MAIL_COMMAND_FETCH,
    MAIL_COMMAND_CHECK_SMTP
} MailCommand;

typedef struct {
    MailCommand command;
    const char *config_path;
    const char *folder_override;
    int ask_password;
    int verbose;
} MailOptions;

typedef enum {
    MAIL_FOCUS_FOLDERS = 0,
    MAIL_FOCUS_MESSAGES,
    MAIL_FOCUS_BODY
} MailFocus;

typedef struct {
    MailConfig config;
    MailFolder folders[MAIL_FOLDER_CAPACITY];
    size_t folder_count;
    MailMessage messages[MAIL_MESSAGE_CAPACITY];
    size_t message_count;
    size_t selected_message;
    MailFocus focus;
    int verbose;
    char password[MAIL_PASSWORD_CAPACITY];
    char status[MAIL_STATUS_CAPACITY];
    TuiTerminal terminal;
} MailState;

static int mail_write_fill(TuiTerminal *terminal, unsigned int count);
static int mail_write_clipped(TuiTerminal *terminal, const char *text, unsigned int width);
static int mail_write_line_clipped(TuiTerminal *terminal, const char *text, unsigned int width);
static int mail_refresh(MailState *mail);

static void mail_write_diag_text(const char *label, const char *value) {
    rt_write_cstr(2, "mail: diag: ");
    rt_write_cstr(2, label);
    rt_write_cstr(2, value);
    rt_write_cstr(2, "\n");
}

static void mail_write_diag_uint(const char *label, unsigned int value) {
    char buffer[32];

    rt_unsigned_to_string(value, buffer, sizeof(buffer));
    mail_write_diag_text(label, buffer);
}

static void mail_diag_config(const MailConfig *config, const char *config_path, int verbose) {
    if (!verbose) {
        return;
    }
    mail_write_diag_text("config=", config_path != 0 ? config_path : "[defaults]");
    mail_write_diag_text("user=", config->username[0] != '\0' ? config->username : "[missing]");
    mail_write_diag_text("folder=", config->folder);
    mail_write_diag_text("imap.host=", config->imap_host[0] != '\0' ? config->imap_host : "[missing]");
    mail_write_diag_uint("imap.port=", config->imap_port);
    mail_write_diag_text("smtp.host=", config->smtp_host[0] != '\0' ? config->smtp_host : "[missing]");
    mail_write_diag_uint("smtp.port=", config->smtp_port);
    mail_write_diag_text("tls=", config->require_tls ? "required" : "not-required");
    mail_write_diag_text("password=", config->password[0] != '\0' ? "configured" : "not-configured");
}

static void mail_set_status(MailState *mail, const char *message) {
    rt_copy_string(mail->status, sizeof(mail->status), message);
}

static int mail_parse_port(const char *text, unsigned int *port_out) {
    unsigned long long value;

    if (rt_parse_uint(text, &value) != 0 || value == 0ULL || value > 65535ULL) {
        return -1;
    }
    *port_out = (unsigned int)value;
    return 0;
}

static void mail_trim_text(char *text) {
    size_t start = 0U;
    size_t length;

    while (text[start] != '\0' && rt_is_space(text[start])) {
        start += 1U;
    }
    length = rt_strlen(text + start);
    while (length > 0U && rt_is_space(text[start + length - 1U])) {
        length -= 1U;
    }
    if (start > 0U && length > 0U) {
        memmove(text, text + start, length);
    }
    text[length] = '\0';
}

static void mail_config_defaults(MailConfig *config) {
    memset(config, 0, sizeof(*config));
    config->imap_port = MAIL_DEFAULT_IMAP_PORT;
    config->smtp_port = MAIL_DEFAULT_SMTP_PORT;
    config->require_tls = 1;
    rt_copy_string(config->folder, sizeof(config->folder), "INBOX");
}

static int mail_config_apply(MailConfig *config, const char *key, const char *value) {
    if (rt_strcmp(key, "imap.host") == 0) {
        rt_copy_string(config->imap_host, sizeof(config->imap_host), value);
    } else if (rt_strcmp(key, "imap.port") == 0) {
        if (mail_parse_port(value, &config->imap_port) != 0) return -1;
    } else if (rt_strcmp(key, "smtp.host") == 0) {
        rt_copy_string(config->smtp_host, sizeof(config->smtp_host), value);
    } else if (rt_strcmp(key, "smtp.port") == 0) {
        if (mail_parse_port(value, &config->smtp_port) != 0) return -1;
    } else if (rt_strcmp(key, "user") == 0) {
        rt_copy_string(config->username, sizeof(config->username), value);
    } else if (rt_strcmp(key, "from") == 0) {
        rt_copy_string(config->from, sizeof(config->from), value);
    } else if (rt_strcmp(key, "folder") == 0) {
        rt_copy_string(config->folder, sizeof(config->folder), value);
    } else if (rt_strcmp(key, "password") == 0) {
        rt_copy_string(config->password, sizeof(config->password), value);
    } else if (rt_strcmp(key, "tls") == 0) {
        config->require_tls = rt_strcmp(value, "required") == 0 || rt_strcmp(value, "yes") == 0 || rt_strcmp(value, "true") == 0;
    } else {
        return 1;
    }
    return 0;
}

static void mail_config_warn_unknown_key(const char *key, unsigned int line_number) {
    rt_write_cstr(2, "mail: warning: unsupported config key on line ");
    rt_write_uint(2, line_number);
    rt_write_cstr(2, ": ");
    rt_write_cstr(2, key);
    rt_write_cstr(2, "\n");
}

static void mail_config_error_line(unsigned int line_number, const char *message) {
    rt_write_cstr(2, "mail: config error on line ");
    rt_write_uint(2, line_number);
    rt_write_cstr(2, ": ");
    rt_write_cstr(2, message);
    rt_write_cstr(2, "\n");
}

static void mail_config_error_key(unsigned int line_number, const char *message, const char *key) {
    rt_write_cstr(2, "mail: config error on line ");
    rt_write_uint(2, line_number);
    rt_write_cstr(2, ": ");
    rt_write_cstr(2, message);
    rt_write_cstr(2, key);
    rt_write_cstr(2, "\n");
}

static int mail_config_line_has_char(const char *line, char needle) {
    while (*line != '\0') {
        if (*line == needle) {
            return 1;
        }
        line += 1;
    }
    return 0;
}

static int mail_config_parse_buffer(MailConfig *config, char *buffer) {
    char *cursor = buffer;
    unsigned int line_number = 0U;

    while (*cursor != '\0') {
        char *line = cursor;
        char *line_end = cursor;
        char *equals;
        int apply_result;

        line_number += 1U;
        while (*line_end != '\0' && *line_end != '\n') {
            line_end += 1;
        }
        if (*line_end == '\n') {
            *line_end = '\0';
            cursor = line_end + 1;
        } else {
            cursor = line_end;
        }
        mail_trim_text(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        equals = line;
        while (*equals != '\0' && *equals != '=') {
            equals += 1;
        }
        if (*equals != '=') {
            if (mail_config_line_has_char(line, ':')) {
                mail_config_error_line(line_number, "expected key=value, found ':' instead of '='");
            } else {
                mail_config_error_line(line_number, "expected key=value");
            }
            return -1;
        }
        *equals = '\0';
        mail_trim_text(line);
        mail_trim_text(equals + 1);
        if (line[0] == '\0') {
            mail_config_error_line(line_number, "missing key before '='");
            return -1;
        }
        apply_result = mail_config_apply(config, line, equals + 1);
        if (apply_result < 0) {
            mail_config_error_key(line_number, "invalid value for ", line);
            return -1;
        }
        if (apply_result > 0) {
            mail_config_warn_unknown_key(line, line_number);
        }
    }
    return 0;
}

static int mail_config_load(MailConfig *config, const char *path) {
    char buffer[4096];
    size_t used = 0U;
    int fd;

    mail_config_defaults(config);
    if (path == 0 || path[0] == '\0') {
        return 0;
    }
    fd = platform_open_read(path);
    if (fd < 0) {
        return -1;
    }
    while (used + 1U < sizeof(buffer)) {
        long bytes = platform_read(fd, buffer + used, sizeof(buffer) - used - 1U);
        if (bytes < 0) {
            (void)platform_close(fd);
            return -1;
        }
        if (bytes == 0) {
            break;
        }
        used += (size_t)bytes;
    }
    (void)platform_close(fd);
    if (used + 1U >= sizeof(buffer)) {
        return -1;
    }
    buffer[used] = '\0';
    if (mail_config_parse_buffer(config, buffer) != 0) {
        return -1;
    }
    config->valid = config->imap_host[0] != '\0' && config->smtp_host[0] != '\0' && config->username[0] != '\0';
    return 0;
}

static int mail_read_password_fd(int input_fd, int output_fd, char *buffer, size_t buffer_size) {
    PlatformTerminalState state;
    size_t length = 0U;
    int raw_enabled = 0;

    if (buffer_size == 0U) {
        return -1;
    }
    buffer[0] = '\0';
    if (rt_write_cstr(output_fd, "Password: ") != 0) {
        return -1;
    }
    if (platform_isatty(input_fd) && platform_terminal_enable_raw_mode(input_fd, &state) == 0) {
        raw_enabled = 1;
    }
    for (;;) {
        char ch;
        long bytes = platform_read(input_fd, &ch, 1U);
        if (bytes != 1) {
            if (raw_enabled) (void)platform_terminal_restore_mode(input_fd, &state);
            return -1;
        }
        if (ch == '\r' || ch == '\n') {
            break;
        }
        if ((ch == 127 || ch == 8) && length > 0U) {
            length -= 1U;
            buffer[length] = '\0';
            continue;
        }
        if ((unsigned char)ch >= 32U && length + 1U < buffer_size) {
            buffer[length++] = ch;
            buffer[length] = '\0';
        }
    }
    if (raw_enabled) {
        (void)platform_terminal_restore_mode(input_fd, &state);
    }
    (void)rt_write_cstr(output_fd, "\n");
    return length > 0U ? 0 : -1;
}

static int mail_prompt_password_tui(MailState *mail) {
    size_t length = 0U;

    mail->password[0] = '\0';
    for (;;) {
        TuiKeyEvent event;
        if (tui_move_cursor(&mail->terminal, mail->terminal.rows, 1U) != 0) return -1;
        if (tui_clear_line(&mail->terminal) != 0) return -1;
        if (tui_write_cstr(&mail->terminal, "Password: ") != 0) return -1;
        if (mail_write_fill(&mail->terminal, (unsigned int)length) != 0) return -1;
        if (tui_read_key(&mail->terminal, &event) != 0) return -1;
        if (event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_ENTER) {
            return length > 0U ? 0 : -1;
        }
        if (event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_ESCAPE) {
            return -1;
        }
        if (event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_BACKSPACE) {
            if (length > 0U) {
                length -= 1U;
                mail->password[length] = '\0';
            }
        } else if (event.type == TUI_KEY_CHARACTER && event.text_length == 1U && length + 1U < sizeof(mail->password)) {
            mail->password[length++] = event.text[0];
            mail->password[length] = '\0';
        }
    }
}

static int mail_has_password(const MailState *mail) {
    return mail->password[0] != '\0' || mail->config.password[0] != '\0';
}

static int mail_ensure_password(MailState *mail) {
    if (mail_has_password(mail)) {
        return 0;
    }
    if (mail_prompt_password_tui(mail) != 0) {
        mail_set_status(mail, "Password canceled");
        return -1;
    }
    mail_set_status(mail, "Password captured for this session");
    return 0;
}

static const char *mail_state_password(const MailState *mail) {
    return mail->password[0] != '\0' ? mail->password : mail->config.password;
}

static int mail_compose_prompt_line(MailState *mail, const char *prompt, char *buffer, size_t buffer_size) {
    TuiKeyEvent event;
    size_t length = rt_strlen(buffer);

    if (buffer_size == 0U) return -1;
    if (length >= buffer_size) length = buffer_size - 1U;
    if (tui_show_cursor(&mail->terminal) != 0) return -1;
    for (;;) {
        if (tui_hide_cursor(&mail->terminal) != 0) return -1;
        if (tui_move_cursor(&mail->terminal, mail->terminal.rows, 1U) != 0) return -1;
        if (tui_clear_line(&mail->terminal) != 0) return -1;
        if (tui_write_cstr(&mail->terminal, prompt) != 0) return -1;
        if (tui_write_cstr(&mail->terminal, buffer) != 0) return -1;
        if (tui_show_cursor(&mail->terminal) != 0) return -1;
        if (tui_read_key(&mail->terminal, &event) != 0) return -1;
        if (event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_ENTER) {
            (void)tui_hide_cursor(&mail->terminal);
            return buffer[0] != '\0' ? 0 : -1;
        }
        if (event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_ESCAPE) {
            (void)tui_hide_cursor(&mail->terminal);
            return -1;
        }
        if ((event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_BACKSPACE) || (event.type == TUI_KEY_CTRL && event.codepoint == 'H')) {
            if (length > 0U) {
                length -= 1U;
                while (length > 0U && ((unsigned char)buffer[length] & 0xc0U) == 0x80U) length -= 1U;
                buffer[length] = '\0';
            }
        } else if (event.type == TUI_KEY_CHARACTER && event.text_length > 0U && length + event.text_length < buffer_size) {
            memcpy(buffer + length, event.text, event.text_length);
            length += event.text_length;
            buffer[length] = '\0';
        }
    }
}

static int mail_compose_draw_body(MailState *mail, const char *to, const char *subject, const char *body) {
    unsigned int row;
    const char *cursor = body;
    unsigned int visible_rows = mail->terminal.rows > 8U ? mail->terminal.rows - 8U : 1U;
    unsigned int total_lines = 1U;
    unsigned int skip_lines = 0U;
    unsigned int skipped = 0U;
    unsigned int cursor_row;
    unsigned int cursor_column = 1U;
    const char *line_start = body;

    while (*cursor != '\0') {
        if (*cursor == '\n') total_lines += 1U;
        cursor += 1;
    }
    while (*line_start != '\0') {
        if (*line_start == '\n') cursor_column = 1U;
        else cursor_column += 1U;
        line_start += 1;
    }
    if (cursor_column > mail->terminal.columns) cursor_column = mail->terminal.columns;
    if (total_lines > visible_rows) skip_lines = total_lines - visible_rows;
    cursor_row = 8U + total_lines - skip_lines - 1U;
    if (cursor_row >= mail->terminal.rows) cursor_row = mail->terminal.rows - 1U;
    cursor = body;
    while (skipped < skip_lines && *cursor != '\0') {
        while (*cursor != '\0' && *cursor != '\n') cursor += 1;
        if (*cursor == '\n') cursor += 1;
        skipped += 1U;
    }

    if (tui_hide_cursor(&mail->terminal) != 0) return -1;
    if (tui_move_cursor(&mail->terminal, 1U, 1U) != 0) return -1;
    if (tui_clear_line(&mail->terminal) != 0) return -1;
    if (tui_set_style(&mail->terminal, TUI_STYLE_INVERSE) != 0) return -1;
    if (mail_write_clipped(&mail->terminal, "Compose text/plain mail", mail->terminal.columns) != 0) return -1;
    if (tui_set_style(&mail->terminal, TUI_STYLE_NORMAL) != 0) return -1;
    if (tui_move_cursor(&mail->terminal, 2U, 1U) != 0) return -1;
    if (tui_clear_line(&mail->terminal) != 0) return -1;
    if (tui_move_cursor(&mail->terminal, 3U, 1U) != 0) return -1;
    if (tui_clear_line(&mail->terminal) != 0) return -1;
    if (tui_write_cstr(&mail->terminal, "To: ") != 0 || tui_write_cstr(&mail->terminal, to) != 0) return -1;
    if (tui_move_cursor(&mail->terminal, 4U, 1U) != 0) return -1;
    if (tui_clear_line(&mail->terminal) != 0) return -1;
    if (tui_write_cstr(&mail->terminal, "Subject: ") != 0 || tui_write_cstr(&mail->terminal, subject) != 0) return -1;
    if (tui_move_cursor(&mail->terminal, 5U, 1U) != 0) return -1;
    if (tui_clear_line(&mail->terminal) != 0) return -1;
    if (tui_move_cursor(&mail->terminal, 6U, 1U) != 0) return -1;
    if (tui_clear_line(&mail->terminal) != 0) return -1;
    if (tui_write_cstr(&mail->terminal, "Body: Enter=new line, Ctrl+D=send, Esc=cancel") != 0) return -1;
    if (tui_move_cursor(&mail->terminal, 7U, 1U) != 0) return -1;
    if (tui_clear_line(&mail->terminal) != 0) return -1;
    row = 8U;
    while (row < mail->terminal.rows) {
        if (tui_move_cursor(&mail->terminal, row, 1U) != 0) return -1;
        if (tui_clear_line(&mail->terminal) != 0) return -1;
        if (*cursor == '\0') {
            row += 1U;
            continue;
        }
        if (mail_write_line_clipped(&mail->terminal, cursor, mail->terminal.columns) != 0) return -1;
        while (*cursor != '\0' && *cursor != '\n') cursor += 1;
        if (*cursor == '\n') cursor += 1;
        row += 1U;
    }
    if (tui_move_cursor(&mail->terminal, mail->terminal.rows, 1U) != 0) return -1;
    if (tui_clear_line(&mail->terminal) != 0) return -1;
    if (tui_move_cursor(&mail->terminal, cursor_row, cursor_column) != 0) return -1;
    if (tui_show_cursor(&mail->terminal) != 0) return -1;
    return 0;
}

static int mail_compose_body(MailState *mail, const char *to, const char *subject, char *body, size_t body_size) {
    TuiKeyEvent event;
    size_t length = 0U;

    if (body_size == 0U) return -1;
    body[0] = '\0';
    for (;;) {
        if (mail_compose_draw_body(mail, to, subject, body) != 0) return -1;
        if (tui_read_key(&mail->terminal, &event) != 0) return -1;
        if (event.type == TUI_KEY_CTRL && event.codepoint == 'D') {
            (void)tui_hide_cursor(&mail->terminal);
            return body[0] != '\0' ? 0 : -1;
        }
        if (event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_ESCAPE) {
            (void)tui_hide_cursor(&mail->terminal);
            return -1;
        }
        if (event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_ENTER) {
            if (length + 1U < body_size) {
                body[length++] = '\n';
                body[length] = '\0';
            }
        } else if ((event.type == TUI_KEY_SPECIAL && event.codepoint == TUI_KEY_BACKSPACE) || (event.type == TUI_KEY_CTRL && event.codepoint == 'H')) {
            if (length > 0U) {
                length -= 1U;
                while (length > 0U && ((unsigned char)body[length] & 0xc0U) == 0x80U) length -= 1U;
                body[length] = '\0';
            }
        } else if (event.type == TUI_KEY_CHARACTER && event.text_length > 0U && length + event.text_length < body_size) {
            memcpy(body + length, event.text, event.text_length);
            length += event.text_length;
            body[length] = '\0';
        }
    }
}

static int mail_refresh_inbox(MailState *mail) {
    size_t loaded_count = 0U;
    size_t loaded_folders = 0U;

    if (!mail->config.valid) {
        mail_set_status(mail, "Config needs imap.host, smtp.host, and user");
        return -1;
    }
    if (!mail->config.require_tls) {
        mail_set_status(mail, "Refusing plaintext IMAP");
        return -1;
    }
    if (mail_ensure_password(mail) != 0) {
        return -1;
    }
    if (mail_imap_load_mailboxes_for_config(&mail->config, mail_state_password(mail), mail->folders, MAIL_FOLDER_CAPACITY, &loaded_folders, mail->verbose) == 0) {
        mail->folder_count = loaded_folders;
    }
    if (mail_imap_fetch_messages_for_config(&mail->config, mail_state_password(mail), mail->messages, MAIL_MESSAGE_CAPACITY, &loaded_count, mail->verbose, 0) != 0) {
        mail_set_status(mail, "IMAP fetch failed");
        return -1;
    }
    mail->message_count = loaded_count;
    if (mail->selected_message >= mail->message_count) {
        mail->selected_message = mail->message_count > 0U ? mail->message_count - 1U : 0U;
    }
    mail_set_status(mail, loaded_count > 0U ? "Inbox loaded over verified TLS" : "Inbox is empty");
    return 0;
}

static int mail_send_message(MailState *mail) {
    char to[MAIL_TEXT_CAPACITY];
    char subject[MAIL_TEXT_CAPACITY];
    char body[MAIL_COMPOSE_BODY_CAPACITY];
    char smtp_error[MAIL_STATUS_CAPACITY];

    if (!mail->config.valid) {
        mail_set_status(mail, "Config needs imap.host, smtp.host, and user");
        return -1;
    }
    if (!mail->config.require_tls) {
        mail_set_status(mail, "Refusing plaintext SMTP");
        return -1;
    }
    if (mail_ensure_password(mail) != 0) {
        return -1;
    }
    memset(to, 0, sizeof(to));
    memset(subject, 0, sizeof(subject));
    memset(body, 0, sizeof(body));
    memset(smtp_error, 0, sizeof(smtp_error));
    if (mail_compose_prompt_line(mail, "To: ", to, sizeof(to)) != 0) {
        mail_set_status(mail, "Compose canceled");
        return -1;
    }
    if (mail_compose_prompt_line(mail, "Subject: ", subject, sizeof(subject)) != 0) {
        mail_set_status(mail, "Compose canceled");
        return -1;
    }
    if (mail_compose_body(mail, to, subject, body, sizeof(body)) != 0) {
        mail_set_status(mail, "Compose canceled");
        return -1;
    }
    mail_set_status(mail, "Sending message over SMTP TLS");
    (void)mail_refresh(mail);
    if (mail_smtp_send_text_for_config(&mail->config, mail_state_password(mail), to, subject, body, smtp_error, sizeof(smtp_error), mail->verbose) != 0) {
        mail_set_status(mail, smtp_error[0] != '\0' ? smtp_error : "SMTP send failed");
        return -1;
    }
    mail_set_status(mail, "Message sent");
    return 0;
}

static int mail_write_fill(TuiTerminal *terminal, unsigned int count) {
    while (count > 0U) {
        if (tui_write_cstr(terminal, " ") != 0) return -1;
        count -= 1U;
    }
    return 0;
}

static int mail_write_clipped(TuiTerminal *terminal, const char *text, unsigned int width) {
    unsigned int used = 0U;

    while (text[used] != '\0' && used < width) {
        used += 1U;
    }
    if (used > 0U && tui_write(terminal, text, used) != 0) return -1;
    return mail_write_fill(terminal, width - used);
}

static int mail_write_line_clipped(TuiTerminal *terminal, const char *text, unsigned int width) {
    unsigned int used = 0U;

    while (text[used] != '\0' && text[used] != '\n' && used < width) {
        used += 1U;
    }
    if (used > 0U && tui_write(terminal, text, used) != 0) return -1;
    return mail_write_fill(terminal, width - used);
}

static int mail_draw_header(MailState *mail) {
    char port_text[32];
    unsigned int tail_width = mail->terminal.columns > 58U ? mail->terminal.columns - 58U : 0U;

    if (tui_set_style(&mail->terminal, TUI_STYLE_KEYWORD) != 0) return -1;
    if (mail_write_clipped(&mail->terminal, "mail", 6U) != 0) return -1;
    if (tui_set_style(&mail->terminal, TUI_STYLE_NORMAL) != 0) return -1;
    if (mail_write_clipped(&mail->terminal, mail->config.username[0] != '\0' ? mail->config.username : "[no account]", 24U) != 0) return -1;
    if (mail_write_clipped(&mail->terminal, mail->config.imap_host[0] != '\0' ? mail->config.imap_host : "imap.host missing", 28U) != 0) return -1;
    rt_unsigned_to_string(mail->config.imap_port, port_text, sizeof(port_text));
    return mail_write_clipped(&mail->terminal, port_text, tail_width);
}

static int mail_draw_status(MailState *mail) {
    const char *status = mail->status[0] != '\0' ? mail->status : "r Refresh  c Send  Tab Focus  q Quit";

    if (tui_set_inverse(&mail->terminal, 1) != 0) return -1;
    if (mail_write_clipped(&mail->terminal, status, mail->terminal.columns) != 0) return -1;
    return tui_set_inverse(&mail->terminal, 0);
}

static int mail_draw_folder_pane(MailState *mail, unsigned int row, unsigned int height, unsigned int width) {
    unsigned int index;

    for (index = 0U; index < height; ++index) {
        if (tui_move_cursor(&mail->terminal, row + index, 1U) != 0) return -1;
        if (tui_clear_line(&mail->terminal) != 0) return -1;
        if (index == 0U) {
            if (tui_set_style(&mail->terminal, mail->focus == MAIL_FOCUS_FOLDERS ? TUI_STYLE_INVERSE : TUI_STYLE_NORMAL) != 0) return -1;
            if (mail_write_clipped(&mail->terminal, "Folders", width) != 0) return -1;
            if (tui_set_style(&mail->terminal, TUI_STYLE_NORMAL) != 0) return -1;
        } else if (mail->folder_count > 0U && (size_t)(index - 1U) < mail->folder_count) {
            MailFolder *folder = &mail->folders[index - 1U];
            if (rt_strcmp(folder->name, mail->config.folder) == 0 && tui_set_inverse(&mail->terminal, 1) != 0) return -1;
            if (mail_write_clipped(&mail->terminal, folder->name, width) != 0) return -1;
            if (rt_strcmp(folder->name, mail->config.folder) == 0 && tui_set_inverse(&mail->terminal, 0) != 0) return -1;
        } else if (mail->folder_count == 0U && index == 2U) {
            if (mail_write_clipped(&mail->terminal, mail->config.folder, width) != 0) return -1;
        } else if (mail_write_fill(&mail->terminal, width) != 0) {
            return -1;
        }
    }
    return 0;
}

static int mail_draw_message_pane(MailState *mail, unsigned int row, unsigned int height, unsigned int column, unsigned int width) {
    unsigned int index;

    for (index = 0U; index < height; ++index) {
        if (tui_move_cursor(&mail->terminal, row + index, column) != 0) return -1;
        if (index == 0U) {
            if (tui_set_style(&mail->terminal, mail->focus == MAIL_FOCUS_MESSAGES ? TUI_STYLE_INVERSE : TUI_STYLE_NORMAL) != 0) return -1;
            if (mail_write_clipped(&mail->terminal, "Messages", width) != 0) return -1;
            if (tui_set_style(&mail->terminal, TUI_STYLE_NORMAL) != 0) return -1;
        } else if ((size_t)(index - 1U) < mail->message_count) {
            MailMessage *message = &mail->messages[index - 1U];
            unsigned int from_width = width > 14U ? 14U : width;
            if ((size_t)(index - 1U) == mail->selected_message && tui_set_inverse(&mail->terminal, 1) != 0) return -1;
            if (mail_write_clipped(&mail->terminal, message->from, from_width) != 0) return -1;
            if (width > from_width && mail_write_clipped(&mail->terminal, message->subject, width - from_width) != 0) return -1;
            if ((size_t)(index - 1U) == mail->selected_message && tui_set_inverse(&mail->terminal, 0) != 0) return -1;
        } else if (mail->message_count == 0U && index == 2U) {
            if (mail_write_clipped(&mail->terminal, "No messages loaded", width) != 0) return -1;
        } else if (mail_write_fill(&mail->terminal, width) != 0) {
            return -1;
        }
    }
    return 0;
}

static int mail_draw_body_pane(MailState *mail, unsigned int row, unsigned int height, unsigned int column, unsigned int width) {
    MailMessage *message = mail->message_count > 0U ? &mail->messages[mail->selected_message] : 0;
    unsigned int index;
    unsigned int body_row = 0U;
    unsigned int date_row = message != 0 && message->cc[0] != '\0' ? 7U : 6U;
    unsigned int first_body_row = date_row + 2U;
    const char *body_cursor = message != 0 ? message->body : "";

    for (index = 0U; index < height; ++index) {
        if (tui_move_cursor(&mail->terminal, row + index, column) != 0) return -1;
        if (index == 0U) {
            if (tui_set_style(&mail->terminal, mail->focus == MAIL_FOCUS_BODY ? TUI_STYLE_INVERSE : TUI_STYLE_NORMAL) != 0) return -1;
            if (mail_write_clipped(&mail->terminal, "Message", width) != 0) return -1;
            if (tui_set_style(&mail->terminal, TUI_STYLE_NORMAL) != 0) return -1;
        } else if (message == 0 && index == 2U) {
            if (mail_write_clipped(&mail->terminal, "Select the message list and press Enter to load the inbox", width) != 0) return -1;
        } else if (message != 0 && index == 2U) {
            if (mail_write_clipped(&mail->terminal, message->subject, width) != 0) return -1;
        } else if (message != 0 && index == 4U) {
            if (mail_write_clipped(&mail->terminal, "From: ", width > 6U ? 6U : width) != 0) return -1;
            if (width > 6U && mail_write_clipped(&mail->terminal, message->from, width - 6U) != 0) return -1;
        } else if (message != 0 && index == 5U) {
            if (mail_write_clipped(&mail->terminal, "To: ", width > 4U ? 4U : width) != 0) return -1;
            if (width > 4U && mail_write_clipped(&mail->terminal, message->to, width - 4U) != 0) return -1;
        } else if (message != 0 && index == 6U && message->cc[0] != '\0') {
            if (mail_write_clipped(&mail->terminal, "Cc: ", width > 4U ? 4U : width) != 0) return -1;
            if (width > 4U && mail_write_clipped(&mail->terminal, message->cc, width - 4U) != 0) return -1;
        } else if (message != 0 && index == date_row) {
            if (mail_write_clipped(&mail->terminal, "Date: ", width > 6U ? 6U : width) != 0) return -1;
            if (width > 6U && mail_write_clipped(&mail->terminal, message->date, width - 6U) != 0) return -1;
        } else if (message != 0 && index >= first_body_row) {
            while (body_row + first_body_row < index && *body_cursor != '\0') {
                while (*body_cursor != '\0' && *body_cursor != '\n') body_cursor += 1;
                if (*body_cursor == '\n') body_cursor += 1;
                body_row += 1U;
            }
            if (mail_write_line_clipped(&mail->terminal, body_cursor, width) != 0) return -1;
        } else if (mail_write_fill(&mail->terminal, width) != 0) {
            return -1;
        }
    }
    return 0;
}

static int mail_refresh(MailState *mail) {
    unsigned int rows;
    unsigned int columns;
    unsigned int content_rows;
    unsigned int folder_width;
    unsigned int right_width;
    unsigned int list_height;
    unsigned int body_height;

    (void)tui_terminal_refresh_size(&mail->terminal);
    rows = mail->terminal.rows;
    columns = mail->terminal.columns;
    content_rows = rows > 3U ? rows - 3U : 1U;
    folder_width = columns >= 72U ? 20U : 0U;
    right_width = columns > folder_width ? columns - folder_width : columns;
    list_height = content_rows > 8U ? content_rows / 2U : content_rows;
    body_height = content_rows > list_height ? content_rows - list_height : 0U;

    if (tui_hide_cursor(&mail->terminal) != 0) return -1;
    if (tui_move_cursor(&mail->terminal, 1U, 1U) != 0) return -1;
    if (tui_clear_line(&mail->terminal) != 0) return -1;
    if (mail_draw_header(mail) != 0) return -1;
    if (folder_width > 0U && mail_draw_folder_pane(mail, 2U, content_rows, folder_width) != 0) return -1;
    if (mail_draw_message_pane(mail, 2U, list_height, folder_width + 1U, right_width) != 0) return -1;
    if (body_height > 0U && mail_draw_body_pane(mail, 2U + list_height, body_height, folder_width + 1U, right_width) != 0) return -1;
    if (tui_move_cursor(&mail->terminal, rows, 1U) != 0) return -1;
    return mail_draw_status(mail);
}

static void mail_focus_next(MailState *mail) {
    if (mail->focus == MAIL_FOCUS_BODY) {
        mail->focus = MAIL_FOCUS_FOLDERS;
    } else {
        mail->focus = (MailFocus)((int)mail->focus + 1);
    }
}

static void mail_move_selection(MailState *mail, int direction) {
    int count = direction < 0 ? -direction : direction;

    while (count > 0 && mail->message_count > 0U) {
        if (direction < 0 && mail->selected_message > 0U) {
            mail->selected_message -= 1U;
        } else if (direction > 0 && mail->selected_message + 1U < mail->message_count) {
            mail->selected_message += 1U;
        }
        count -= 1;
    }
}

static void mail_activate_focus(MailState *mail) {
    if (mail->focus == MAIL_FOCUS_BODY) {
        mail->focus = MAIL_FOCUS_MESSAGES;
    } else if (mail->focus == MAIL_FOCUS_MESSAGES && mail->message_count > 0U) {
        mail->focus = MAIL_FOCUS_BODY;
        mail_set_status(mail, "Message preview selected");
    } else {
        (void)mail_refresh_inbox(mail);
    }
}

static int mail_handle_key(MailState *mail, const TuiKeyEvent *event, int *quit_out) {
    *quit_out = 0;
    if (event->type == TUI_KEY_CHARACTER) {
        if (event->codepoint == 'q') {
            *quit_out = 1;
        } else if (event->codepoint == 'r') {
            (void)mail_refresh_inbox(mail);
        } else if (event->codepoint == 'c') {
            (void)mail_send_message(mail);
        } else if (event->codepoint == '\t') {
            mail_focus_next(mail);
        }
    } else if (event->type == TUI_KEY_CTRL && event->codepoint == 'Q') {
        *quit_out = 1;
    } else if (event->type == TUI_KEY_SPECIAL && event->codepoint == TUI_KEY_ENTER) {
        mail_activate_focus(mail);
    } else if (event->type == TUI_KEY_SPECIAL && event->codepoint == TUI_KEY_ARROW_UP) {
        mail_move_selection(mail, -1);
    } else if (event->type == TUI_KEY_SPECIAL && event->codepoint == TUI_KEY_ARROW_DOWN) {
        mail_move_selection(mail, 1);
    } else if (event->type == TUI_KEY_SPECIAL && event->codepoint == TUI_KEY_PAGE_UP) {
        mail_move_selection(mail, -5);
    } else if (event->type == TUI_KEY_SPECIAL && event->codepoint == TUI_KEY_PAGE_DOWN) {
        mail_move_selection(mail, 5);
    }
    return 0;
}

static void mail_options_init(MailOptions *options) {
    memset(options, 0, sizeof(*options));
    options->command = MAIL_COMMAND_INTERACTIVE;
}

static void mail_write_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_cstr(2, " [-v] [--list|--fetch|--check-smtp] [--folder FOLDER] [--ask-password] [config]\n");
}

static int mail_parse_options(int argc, char **argv, MailOptions *options) {
    int index;

    mail_options_init(options);
    for (index = 1; index < argc; ++index) {
        const char *arg = argv[index];
        if (rt_strcmp(arg, "--help") == 0) {
            return 1;
        }
        if (rt_strcmp(arg, "-v") == 0 || rt_strcmp(arg, "--verbose") == 0) {
            options->verbose = 1;
        } else if (rt_strcmp(arg, "--list") == 0) {
            if (options->command != MAIL_COMMAND_INTERACTIVE) return -1;
            options->command = MAIL_COMMAND_LIST;
        } else if (rt_strcmp(arg, "--fetch") == 0) {
            if (options->command != MAIL_COMMAND_INTERACTIVE) return -1;
            options->command = MAIL_COMMAND_FETCH;
        } else if (rt_strcmp(arg, "--check-smtp") == 0) {
            if (options->command != MAIL_COMMAND_INTERACTIVE) return -1;
            options->command = MAIL_COMMAND_CHECK_SMTP;
        } else if (rt_strcmp(arg, "--folder") == 0) {
            if (index + 1 >= argc) return -1;
            options->folder_override = argv[++index];
        } else if (rt_strcmp(arg, "--ask-password") == 0) {
            options->ask_password = 1;
        } else if (arg[0] == '-') {
            return -1;
        } else if (options->config_path == 0) {
            options->config_path = arg;
        } else {
            return -1;
        }
    }
    return 0;
}

static int mail_cli_prepare_config(MailConfig *config, const MailOptions *options) {
    if (mail_config_load(config, options->config_path) != 0) {
        rt_write_cstr(2, "mail: could not load config\n");
        return -1;
    }
    if (options->folder_override != 0) {
        rt_copy_string(config->folder, sizeof(config->folder), options->folder_override);
    }
    mail_diag_config(config, options->config_path, options->verbose);
    if (!config->valid) {
        rt_write_cstr(2, "mail: config needs imap.host, smtp.host, and user\n");
        return -1;
    }
    if (!config->require_tls) {
        rt_write_cstr(2, "mail: refusing plaintext IMAP/SMTP\n");
        return -1;
    }
    if (options->ask_password && mail_read_password_fd(0, 2, config->password, sizeof(config->password)) != 0) {
        rt_write_cstr(2, "mail: could not read password\n");
        return -1;
    }
    if (options->verbose && options->ask_password) {
        mail_write_diag_text("password=", "prompted");
    }
    if (options->command != MAIL_COMMAND_CHECK_SMTP && config->password[0] == '\0') {
        rt_write_cstr(2, "mail: password missing; add password=... to config or use --ask-password\n");
        return -1;
    }
    return 0;
}

static int mail_run_command(const MailOptions *options) {
    MailConfig config;
    char port_text[32];
    char smtp_error[MAIL_STATUS_CAPACITY];
    int result = 1;

    if (mail_cli_prepare_config(&config, options) != 0) {
        return 1;
    }
    memset(smtp_error, 0, sizeof(smtp_error));
    if (options->command == MAIL_COMMAND_CHECK_SMTP) {
        rt_write_cstr(1, "check smtp tls via ");
        rt_write_cstr(1, config.smtp_host);
        rt_write_cstr(1, ":");
        rt_unsigned_to_string(config.smtp_port, port_text, sizeof(port_text));
        rt_write_cstr(1, port_text);
        rt_write_cstr(1, "\n");
        result = mail_smtp_check_tls_for_config(&config, smtp_error, sizeof(smtp_error), options->verbose) == 0 ? 0 : 1;
        if (result != 0) {
            rt_write_cstr(2, "mail: ");
            rt_write_cstr(2, smtp_error[0] != '\0' ? smtp_error : "SMTP TLS connection failed");
            rt_write_cstr(2, "\n");
        }
        memset(config.password, 0, sizeof(config.password));
        return result;
    }
    if (options->command == MAIL_COMMAND_FETCH) {
        rt_write_cstr(1, "fetch ");
        rt_write_cstr(1, config.folder);
    } else {
        rt_write_cstr(1, "list folders");
    }
    rt_write_cstr(1, " via ");
    rt_write_cstr(1, config.imap_host);
    rt_write_cstr(1, ":");
    rt_unsigned_to_string(config.imap_port, port_text, sizeof(port_text));
    rt_write_cstr(1, port_text);
    rt_write_cstr(1, "\n");
    if (options->command == MAIL_COMMAND_LIST) {
        result = mail_imap_list_mailboxes_for_config(&config, config.password, options->verbose) == 0 ? 0 : 1;
    } else {
        MailMessage messages[MAIL_MESSAGE_CAPACITY];
        size_t count = 0U;
        size_t index;

        memset(messages, 0, sizeof(messages));
        if (mail_imap_fetch_messages_for_config(&config, config.password, messages, MAIL_MESSAGE_CAPACITY, &count, options->verbose, options->verbose) == 0) {
            if (count == 0U) {
                rt_write_cstr(1, "mail: folder has no messages\n");
            }
            for (index = 0U; index < count; ++index) {
                rt_write_cstr(1, messages[index].from);
                rt_write_cstr(1, " | ");
                rt_write_cstr(1, messages[index].subject);
                rt_write_cstr(1, " | ");
                rt_write_cstr(1, messages[index].preview);
                rt_write_cstr(1, "\n");
            }
            result = 0;
        }
    }
    if (result != 0) {
        rt_write_cstr(2, "mail: IMAP operation failed\n");
    }
    memset(config.password, 0, sizeof(config.password));
    return result;
}

int main(int argc, char **argv) {
    MailState mail;
    MailOptions options;
    int quit = 0;
    int option_result;

    option_result = mail_parse_options(argc, argv, &options);
    if (option_result == 1) {
        mail_write_usage(argv[0]);
        return 0;
    }
    if (option_result != 0) {
        mail_write_usage(argv[0]);
        return 1;
    }
    if (options.command != MAIL_COMMAND_INTERACTIVE) {
        return mail_run_command(&options);
    }
    memset(&mail, 0, sizeof(mail));
    if (mail_config_load(&mail.config, options.config_path) != 0) {
        rt_write_cstr(2, "mail: could not load config\n");
        return 1;
    }
    if (options.folder_override != 0) {
        rt_copy_string(mail.config.folder, sizeof(mail.config.folder), options.folder_override);
    }
    mail.verbose = options.verbose;
    mail_diag_config(&mail.config, options.config_path, options.verbose);
    if (options.ask_password && mail_read_password_fd(0, 2, mail.password, sizeof(mail.password)) != 0) {
        rt_write_cstr(2, "mail: could not read password\n");
        return 1;
    }
    if (options.verbose && options.ask_password) {
        mail_write_diag_text("password=", "prompted");
    }
    mail.focus = MAIL_FOCUS_MESSAGES;
    mail_set_status(&mail, mail.config.valid ? "Enter loads inbox; r refreshes; c compose/send" : "No account configured");

    if (tui_terminal_open(&mail.terminal, 0, 1, 1) != 0) {
        rt_write_cstr(2, "mail: standard input and output must be a terminal\n");
        return 1;
    }
    (void)tui_clear_screen(&mail.terminal);
    while (!quit) {
        TuiKeyEvent event;
        (void)tui_terminal_check_resize(&mail.terminal);
        if (mail_refresh(&mail) != 0 || tui_read_key(&mail.terminal, &event) != 0) {
            break;
        }
        (void)mail_handle_key(&mail, &event, &quit);
    }
    tui_terminal_close(&mail.terminal);
    memset(mail.password, 0, sizeof(mail.password));
    memset(mail.config.password, 0, sizeof(mail.config.password));
    return 0;
}
