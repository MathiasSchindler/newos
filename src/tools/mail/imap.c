#include "imap.h"

#include "message.h"
#include "platform.h"
#include "runtime.h"

#include <stddef.h>

#define MAIL_IMAP_LINE_CAPACITY 2048U
#define MAIL_IMAP_AUTH_CAPACITY 768U
#define MAIL_SMTP_TEXT_CAPACITY 1536U

typedef struct {
    PlatformTlsClient tls;
    int connected;
} MailTransport;

static int mail_base64_encode(char *out, size_t out_size, const unsigned char *input, size_t input_size);

static void mail_imap_diag_text(const char *label, const char *value) {
    rt_write_cstr(2, "mail: diag: ");
    rt_write_cstr(2, label);
    rt_write_cstr(2, value);
    rt_write_cstr(2, "\n");
}

static void mail_imap_diag_uint(const char *label, unsigned int value) {
    char buffer[32];

    rt_unsigned_to_string(value, buffer, sizeof(buffer));
    mail_imap_diag_text(label, buffer);
}

static int mail_transport_connect_tls(MailTransport *transport, const char *host, unsigned int port, const char *purpose, int verbose) {
    if (verbose) {
        mail_imap_diag_text("transport=", "tls");
        mail_imap_diag_text("phase=", "tcp connect pending");
        mail_imap_diag_text("server.purpose=", purpose);
        mail_imap_diag_text("server.host=", host);
        mail_imap_diag_uint("server.port=", port);
    }
    if (platform_tls_connect(&transport->tls, host, port) != 0) {
        if (verbose) {
            mail_imap_diag_text("tls.handshake=", "failed");
            mail_imap_diag_text("tls.error=", platform_tls_last_error());
            mail_imap_diag_text("tls.certificate.verify=", platform_tls_peer_verification_status());
        }
        return -1;
    }
    transport->connected = 1;
    if (verbose) {
        mail_imap_diag_text("tls.handshake=", "complete");
        mail_imap_diag_text("tls.certificate.verify=", platform_tls_peer_verification_status());
    }
    return 0;
}

static void mail_transport_close(MailTransport *transport) {
    platform_tls_close(&transport->tls);
    transport->connected = 0;
}

static int mail_transport_write_all(MailTransport *transport, const char *data, size_t length) {
    size_t written = 0U;

    while (written < length) {
        long result = platform_tls_write(&transport->tls, data + written, length - written);
        if (result <= 0) {
            return -1;
        }
        written += (size_t)result;
    }
    return 0;
}

static int mail_transport_read_line(MailTransport *transport, char *buffer, size_t buffer_size) {
    size_t used = 0U;

    if (buffer_size == 0U) {
        return -1;
    }
    while (used + 1U < buffer_size) {
        char ch;
        long result = platform_tls_read(&transport->tls, &ch, 1U);
        if (result <= 0) {
            return -1;
        }
        if (ch == '\n') {
            if (used > 0U && buffer[used - 1U] == '\r') {
                used -= 1U;
            }
            buffer[used] = '\0';
            return 0;
        }
        buffer[used++] = ch;
    }
    buffer[used] = '\0';
    return -1;
}

static int mail_imap_quote(char *buffer, size_t buffer_size, const char *text) {
    size_t used = 0U;
    size_t index;

    if (buffer_size < 3U) {
        return -1;
    }
    buffer[used++] = '"';
    for (index = 0U; text[index] != '\0'; ++index) {
        if (text[index] == '"' || text[index] == '\\') {
            if (used + 1U >= buffer_size) return -1;
            buffer[used++] = '\\';
        }
        if (used + 1U >= buffer_size) return -1;
        buffer[used++] = text[index];
    }
    if (used + 1U >= buffer_size) {
        return -1;
    }
    buffer[used++] = '"';
    buffer[used] = '\0';
    return 0;
}

static int mail_starts_with(const char *text, const char *prefix) {
    return rt_strncmp(text, prefix, rt_strlen(prefix)) == 0;
}

static int mail_imap_line_is_tagged(const char *line, const char *tag) {
    size_t tag_len = rt_strlen(tag);

    return rt_strncmp(line, tag, tag_len) == 0 && line[tag_len] == ' ';
}

static int mail_imap_tagged_ok(const char *line, const char *tag) {
    size_t tag_len = rt_strlen(tag);

    return mail_imap_line_is_tagged(line, tag) && rt_strncmp(line + tag_len + 1U, "OK", 2U) == 0;
}

static int mail_imap_tagged_no(const char *line, const char *tag) {
    size_t tag_len = rt_strlen(tag);

    return mail_imap_line_is_tagged(line, tag) && rt_strncmp(line + tag_len + 1U, "NO", 2U) == 0;
}

static void mail_print_response_line(const char *line, int print_output) {
    if (!print_output) {
        return;
    }
    rt_write_cstr(1, line);
    rt_write_cstr(1, "\n");
}

static int mail_smtp_response_code(const char *line) {
    if (line[0] < '0' || line[0] > '9' || line[1] < '0' || line[1] > '9' || line[2] < '0' || line[2] > '9') {
        return 0;
    }
    return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
}

static int mail_smtp_read_response(MailTransport *transport, int expected, int alternate_expected, char *response, size_t response_size, int verbose) {
    char line[MAIL_IMAP_LINE_CAPACITY];
    int code = 0;

    if (response != 0 && response_size > 0U) response[0] = '\0';
    for (;;) {
        if (mail_transport_read_line(transport, line, sizeof(line)) != 0) {
            if (response != 0 && response_size > 0U) rt_copy_string(response, response_size, "read failed");
            return -1;
        }
        if (verbose) mail_imap_diag_text("smtp.response=", line);
        if (response != 0 && response_size > 0U) rt_copy_string(response, response_size, line);
        code = mail_smtp_response_code(line);
        if (line[3] != '-') break;
    }
    return code == expected || (alternate_expected != 0 && code == alternate_expected) ? 0 : -1;
}

static int mail_smtp_send_cstr(MailTransport *transport, const char *text) {
    return mail_transport_write_all(transport, text, rt_strlen(text));
}

static int mail_smtp_send_line(MailTransport *transport, const char *line, int verbose) {
    if (verbose) mail_imap_diag_text("smtp.command=", line);
    return mail_smtp_send_cstr(transport, line) != 0 || mail_smtp_send_cstr(transport, "\r\n") != 0 ? -1 : 0;
}

static void mail_smtp_set_error(char *error, size_t error_size, const char *message) {
    if (error != 0 && error_size > 0U) {
        rt_copy_string(error, error_size, message);
    }
}

static void mail_smtp_set_response_error(char *error, size_t error_size, const char *message, const char *response) {
    if (error == 0 || error_size == 0U) return;
    rt_copy_string(error, error_size, message);
    if (response != 0 && response[0] != '\0') {
        rt_copy_string(error + rt_strlen(error), error_size - rt_strlen(error), ": ");
        rt_copy_string(error + rt_strlen(error), error_size - rt_strlen(error), response);
    }
}

static void mail_smtp_set_tls_error(char *error, size_t error_size, const char *message) {
    const char *tls_error = platform_tls_last_error();
    const char *peer_status = platform_tls_peer_verification_status();

    if (error == 0 || error_size == 0U) return;
    rt_copy_string(error, error_size, message);
    if (tls_error != 0 && tls_error[0] != '\0' && rt_strcmp(tls_error, "none") != 0) {
        rt_copy_string(error + rt_strlen(error), error_size - rt_strlen(error), ": ");
        rt_copy_string(error + rt_strlen(error), error_size - rt_strlen(error), tls_error);
    }
    if (peer_status != 0 && peer_status[0] != '\0' && rt_strcmp(peer_status, "trusted") != 0) {
        rt_copy_string(error + rt_strlen(error), error_size - rt_strlen(error), " (");
        rt_copy_string(error + rt_strlen(error), error_size - rt_strlen(error), peer_status);
        rt_copy_string(error + rt_strlen(error), error_size - rt_strlen(error), ")");
    }
}

static void mail_smtp_copy_path_address(char *dst, size_t dst_size, const char *address) {
    const char *start = address;
    const char *end;
    size_t length;

    while (*start != '\0' && *start != '<') start += 1;
    if (*start == '<') {
        start += 1;
        end = start;
        while (*end != '\0' && *end != '>') end += 1;
    } else {
        start = address;
        while (*start != '\0' && rt_is_space(*start)) start += 1;
        end = start + rt_strlen(start);
        while (end > start && rt_is_space(end[-1])) end -= 1;
    }
    length = (size_t)(end - start);
    if (length >= dst_size) length = dst_size - 1U;
    memcpy(dst, start, length);
    dst[length] = '\0';
}

static int mail_smtp_auth_plain(MailTransport *transport, const MailConfig *config, const char *password, char *response, size_t response_size, int verbose) {
    unsigned char payload[MAIL_IMAP_AUTH_CAPACITY];
    char encoded[MAIL_IMAP_AUTH_CAPACITY * 2U];
    char command[MAIL_IMAP_AUTH_CAPACITY * 2U + 16U];
    size_t used = 0U;
    size_t username_length = rt_strlen(config->username);
    size_t password_length = rt_strlen(password);
    int result;

    if (username_length + password_length + 2U > sizeof(payload)) return -1;
    payload[used++] = 0U;
    memcpy(payload + used, config->username, username_length);
    used += username_length;
    payload[used++] = 0U;
    memcpy(payload + used, password, password_length);
    used += password_length;
    if (mail_base64_encode(encoded, sizeof(encoded), payload, used) != 0) return -1;
    rt_copy_string(command, sizeof(command), "AUTH PLAIN ");
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), encoded);
    if (verbose) mail_imap_diag_text("smtp.command=", "AUTH PLAIN [redacted]");
    result = mail_smtp_send_cstr(transport, command) != 0 || mail_smtp_send_cstr(transport, "\r\n") != 0 ? -1 : mail_smtp_read_response(transport, 235, 503, response, response_size, verbose);
    memset(payload, 0, sizeof(payload));
    memset(encoded, 0, sizeof(encoded));
    memset(command, 0, sizeof(command));
    return result;
}

static int mail_smtp_write_data_text(MailTransport *transport, const char *text) {
    size_t index = 0U;
    int line_start = 1;

    while (text[index] != '\0') {
        if (line_start && text[index] == '.') {
            if (mail_smtp_send_cstr(transport, ".") != 0) return -1;
        }
        if (text[index] == '\r') {
            index += 1U;
            continue;
        }
        if (text[index] == '\n') {
            if (mail_smtp_send_cstr(transport, "\r\n") != 0) return -1;
            line_start = 1;
            index += 1U;
            continue;
        }
        if (mail_transport_write_all(transport, text + index, 1U) != 0) return -1;
        line_start = 0;
        index += 1U;
    }
    if (!line_start && mail_smtp_send_cstr(transport, "\r\n") != 0) return -1;
    return 0;
}

static int mail_smtp_send_message_data(MailTransport *transport, const MailConfig *config, const char *to, const char *subject, const char *body) {
    char date_text[64];

    if (platform_format_time(platform_get_epoch_time(), 0, "%F %T", date_text, sizeof(date_text)) != 0) {
        rt_copy_string(date_text, sizeof(date_text), "unknown date");
    }
    if (mail_smtp_send_cstr(transport, "From: ") != 0 || mail_smtp_send_cstr(transport, config->from[0] != '\0' ? config->from : config->username) != 0 || mail_smtp_send_cstr(transport, "\r\n") != 0) return -1;
    if (mail_smtp_send_cstr(transport, "To: ") != 0 || mail_smtp_send_cstr(transport, to) != 0 || mail_smtp_send_cstr(transport, "\r\n") != 0) return -1;
    if (mail_smtp_send_cstr(transport, "Subject: ") != 0 || mail_smtp_send_cstr(transport, subject) != 0 || mail_smtp_send_cstr(transport, "\r\n") != 0) return -1;
    if (mail_smtp_send_cstr(transport, "Date: ") != 0 || mail_smtp_send_cstr(transport, date_text) != 0 || mail_smtp_send_cstr(transport, " +0000\r\n") != 0) return -1;
    if (mail_smtp_send_cstr(transport, "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n") != 0) return -1;
    return mail_smtp_write_data_text(transport, body);
}

static void mail_imap_capture_exists(const char *line, unsigned int *exists_out) {
    unsigned long long value = 0ULL;
    size_t index = 2U;

    if (exists_out == 0 || line[0] != '*' || line[1] != ' ') {
        return;
    }
    while (line[index] >= '0' && line[index] <= '9') {
        value = value * 10ULL + (unsigned long long)(line[index] - '0');
        index += 1U;
    }
    if (line[index] == ' ' && mail_starts_with(line + index + 1U, "EXISTS") && value <= 0xffffffffULL) {
        *exists_out = (unsigned int)value;
    }
}

static int mail_imap_read_until_tag_ex(MailTransport *transport, const char *tag, int print_output, char *tagged_line, size_t tagged_line_size) {
    char line[MAIL_IMAP_LINE_CAPACITY];

    for (;;) {
        if (mail_transport_read_line(transport, line, sizeof(line)) != 0) {
            return -1;
        }
        mail_print_response_line(line, print_output);
        if (mail_imap_line_is_tagged(line, tag)) {
            if (tagged_line != 0 && tagged_line_size > 0U) {
                rt_copy_string(tagged_line, tagged_line_size, line);
            }
            return mail_imap_tagged_ok(line, tag) ? 0 : -1;
        }
    }
}

static int mail_imap_read_message_fetch(MailTransport *transport, const char *tag, MailMessage *message, int print_output) {
    char line[MAIL_IMAP_LINE_CAPACITY];

    for (;;) {
        if (mail_transport_read_line(transport, line, sizeof(line)) != 0) {
            return -1;
        }
        mail_print_response_line(line, print_output);
        mail_message_capture_line(message, line);
        if (mail_imap_line_is_tagged(line, tag)) {
            mail_message_finalize(message);
            return mail_imap_tagged_ok(line, tag) ? 0 : -1;
        }
    }
}

static int mail_imap_read_select(MailTransport *transport, const char *tag, unsigned int *exists_out, int print_output) {
    char line[MAIL_IMAP_LINE_CAPACITY];

    if (exists_out != 0) {
        *exists_out = 0U;
    }
    for (;;) {
        if (mail_transport_read_line(transport, line, sizeof(line)) != 0) {
            return -1;
        }
        mail_print_response_line(line, print_output);
        mail_imap_capture_exists(line, exists_out);
        if (mail_imap_line_is_tagged(line, tag)) {
            return mail_imap_tagged_ok(line, tag) ? 0 : -1;
        }
    }
}

static int mail_imap_read_until_tag(MailTransport *transport, const char *tag, int print_output) {
    return mail_imap_read_until_tag_ex(transport, tag, print_output, 0, 0U);
}

static void mail_imap_capture_list_folder(const char *line, MailFolder *folders, size_t folder_capacity, size_t *folder_count_out) {
    const char *cursor = line;
    const char *name_start;
    size_t name_length = 0U;

    if (folders == 0 || folder_count_out == 0 || *folder_count_out >= folder_capacity || !mail_starts_with(line, "* LIST ")) {
        return;
    }
    while (*cursor != '\0' && *cursor != ')') {
        cursor += 1;
    }
    if (*cursor != ')') {
        return;
    }
    cursor += 1;
    while (*cursor == ' ') cursor += 1;
    if (*cursor == '"') {
        cursor += 1;
        while (*cursor != '\0' && *cursor != '"') cursor += 1;
        if (*cursor != '"') return;
        cursor += 1;
    } else {
        while (*cursor != '\0' && *cursor != ' ') cursor += 1;
    }
    while (*cursor == ' ') cursor += 1;
    if (*cursor == '\0') {
        return;
    }
    if (*cursor == '"') {
        name_start = cursor + 1;
        while (name_start[name_length] != '\0' && name_start[name_length] != '"' && name_length + 1U < MAIL_TEXT_CAPACITY) {
            name_length += 1U;
        }
    } else {
        name_start = cursor;
        while (name_start[name_length] != '\0' && name_length + 1U < MAIL_TEXT_CAPACITY) {
            name_length += 1U;
        }
    }
    if (name_length == 0U) {
        return;
    }
    memcpy(folders[*folder_count_out].name, name_start, name_length);
    folders[*folder_count_out].name[name_length] = '\0';
    *folder_count_out += 1U;
}

static int mail_imap_read_list(MailTransport *transport, const char *tag, MailFolder *folders, size_t folder_capacity, size_t *folder_count_out, int print_output) {
    char line[MAIL_IMAP_LINE_CAPACITY];

    if (folder_count_out != 0) {
        *folder_count_out = 0U;
    }
    for (;;) {
        if (mail_transport_read_line(transport, line, sizeof(line)) != 0) {
            return -1;
        }
        mail_print_response_line(line, print_output);
        mail_imap_capture_list_folder(line, folders, folder_capacity, folder_count_out);
        if (mail_imap_line_is_tagged(line, tag)) {
            return mail_imap_tagged_ok(line, tag) ? 0 : -1;
        }
    }
}

static int mail_imap_send_command(MailTransport *transport, const char *command) {
    return mail_transport_write_all(transport, command, rt_strlen(command));
}

static size_t mail_base64_encoded_size(size_t input_size) {
    return ((input_size + 2U) / 3U) * 4U;
}

static int mail_base64_encode(char *out, size_t out_size, const unsigned char *input, size_t input_size) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t in_index = 0U;
    size_t out_index = 0U;

    if (mail_base64_encoded_size(input_size) + 1U > out_size) {
        return -1;
    }
    while (in_index < input_size) {
        size_t remaining = input_size - in_index;
        unsigned int first = input[in_index++];
        unsigned int second = remaining > 1U ? input[in_index++] : 0U;
        unsigned int third = remaining > 2U ? input[in_index++] : 0U;

        out[out_index++] = alphabet[(first >> 2) & 0x3fU];
        out[out_index++] = alphabet[((first & 0x03U) << 4) | ((second >> 4) & 0x0fU)];
        out[out_index++] = remaining > 1U ? alphabet[((second & 0x0fU) << 2) | ((third >> 6) & 0x03U)] : '=';
        out[out_index++] = remaining > 2U ? alphabet[third & 0x3fU] : '=';
    }
    out[out_index] = '\0';
    return 0;
}

static int mail_imap_auth_plain_token(char *token, size_t token_size, const char *username, const char *password) {
    unsigned char raw[MAIL_TEXT_CAPACITY + MAIL_TEXT_CAPACITY + MAIL_PASSWORD_CAPACITY + 2U];
    size_t used = 0U;
    size_t index;
    int result;

    raw[used++] = 0U;
    for (index = 0U; username[index] != '\0' && used < sizeof(raw); ++index) {
        raw[used++] = (unsigned char)username[index];
    }
    if (used >= sizeof(raw)) return -1;
    raw[used++] = 0U;
    for (index = 0U; password[index] != '\0' && used < sizeof(raw); ++index) {
        raw[used++] = (unsigned char)password[index];
    }
    result = mail_base64_encode(token, token_size, raw, used);
    memset(raw, 0, sizeof(raw));
    return result;
}

static int mail_imap_login(MailTransport *transport, const MailConfig *config, const char *password, int verbose) {
    char user[MAIL_TEXT_CAPACITY * 2U];
    char pass[MAIL_PASSWORD_CAPACITY * 2U];
    char command[MAIL_TEXT_CAPACITY * 4U + MAIL_PASSWORD_CAPACITY * 2U];
    char tagged_line[MAIL_IMAP_LINE_CAPACITY];
    char token[MAIL_IMAP_AUTH_CAPACITY];
    int login_result;

    if (mail_imap_quote(user, sizeof(user), config->username) != 0 || mail_imap_quote(pass, sizeof(pass), password) != 0) {
        return -1;
    }
    rt_copy_string(command, sizeof(command), "a001 LOGIN ");
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), user);
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), " ");
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), pass);
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), "\r\n");
    if (verbose) {
        mail_imap_diag_text("imap.command=", "LOGIN");
    }
    if (mail_imap_send_command(transport, command) != 0) {
        return -1;
    }
    memset(pass, 0, sizeof(pass));
    memset(command, 0, sizeof(command));
    tagged_line[0] = '\0';
    login_result = mail_imap_read_until_tag_ex(transport, "a001", 0, tagged_line, sizeof(tagged_line));
    if (login_result == 0) {
        return 0;
    }
    if (verbose && tagged_line[0] != '\0') {
        mail_imap_diag_text("imap.login.response=", tagged_line);
    }
    if (!mail_imap_tagged_no(tagged_line, "a001")) {
        return -1;
    }
    if (mail_imap_auth_plain_token(token, sizeof(token), config->username, password) != 0) {
        return -1;
    }
    if (verbose) {
        mail_imap_diag_text("imap.command=", "AUTHENTICATE PLAIN");
    }
    if (mail_imap_send_command(transport, "a00p AUTHENTICATE PLAIN\r\n") != 0 || mail_transport_read_line(transport, tagged_line, sizeof(tagged_line)) != 0) {
        memset(token, 0, sizeof(token));
        return -1;
    }
    if (tagged_line[0] != '+') {
        if (verbose) {
            mail_imap_diag_text("imap.auth.response=", tagged_line);
        }
        memset(token, 0, sizeof(token));
        return -1;
    }
    if (mail_imap_send_command(transport, token) != 0 || mail_imap_send_command(transport, "\r\n") != 0) {
        memset(token, 0, sizeof(token));
        return -1;
    }
    memset(token, 0, sizeof(token));
    tagged_line[0] = '\0';
    login_result = mail_imap_read_until_tag_ex(transport, "a00p", 0, tagged_line, sizeof(tagged_line));
    if (login_result != 0 && verbose && tagged_line[0] != '\0') {
        mail_imap_diag_text("imap.auth.response=", tagged_line);
    }
    return login_result;
}

static int mail_imap_simple_command(MailTransport *transport, const char *tag, const char *verb, const char *arg, int print_output, int verbose) {
    char quoted[MAIL_TEXT_CAPACITY * 2U];
    char command[MAIL_IMAP_LINE_CAPACITY];

    if (arg != 0 && mail_imap_quote(quoted, sizeof(quoted), arg) != 0) {
        return -1;
    }
    rt_copy_string(command, sizeof(command), tag);
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), " ");
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), verb);
    if (arg != 0) {
        rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), " ");
        rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), quoted);
    }
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), "\r\n");
    if (verbose) {
        mail_imap_diag_text("imap.command=", verb);
    }
    if (mail_imap_send_command(transport, command) != 0) {
        return -1;
    }
    return mail_imap_read_until_tag(transport, tag, print_output);
}

static int mail_imap_list_mailboxes(MailTransport *transport, MailFolder *folders, size_t folder_capacity, size_t *folder_count_out, int print_output, int verbose) {
    char quoted[MAIL_TEXT_CAPACITY * 2U];
    char command[MAIL_IMAP_LINE_CAPACITY];

    if (mail_imap_quote(quoted, sizeof(quoted), "*") != 0) {
        return -1;
    }
    rt_copy_string(command, sizeof(command), "a003 LIST \"\" ");
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), quoted);
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), "\r\n");
    if (verbose) {
        mail_imap_diag_text("imap.command=", "LIST");
    }
    if (mail_imap_send_command(transport, command) != 0) {
        return -1;
    }
    return mail_imap_read_list(transport, "a003", folders, folder_capacity, folder_count_out, print_output);
}

static int mail_imap_fetch_messages(MailTransport *transport, const char *folder, MailMessage *messages, size_t message_capacity, size_t *message_count_out, int verbose, int print_raw) {
    char quoted[MAIL_TEXT_CAPACITY * 2U];
    char command[MAIL_IMAP_LINE_CAPACITY];
    unsigned int exists_count = 0U;
    unsigned int first_sequence;
    unsigned int sequence;
    size_t index;

    if (messages == 0 || message_count_out == 0) {
        return -1;
    }
    *message_count_out = 0U;
    if (mail_imap_quote(quoted, sizeof(quoted), folder) != 0) {
        return -1;
    }
    rt_copy_string(command, sizeof(command), "a002 SELECT ");
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), quoted);
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), "\r\n");
    if (verbose) {
        mail_imap_diag_text("imap.command=", "SELECT");
    }
    if (mail_imap_send_command(transport, command) != 0 || mail_imap_read_select(transport, "a002", &exists_count, print_raw) != 0) {
        return -1;
    }
    if (exists_count == 0U) {
        return 0;
    }
    first_sequence = exists_count > (unsigned int)message_capacity ? exists_count - (unsigned int)message_capacity + 1U : 1U;
    index = 0U;
    for (sequence = first_sequence; sequence <= exists_count && index < message_capacity; ++sequence) {
        MailMessage *message = &messages[index];
        char sequence_text[32];

        memset(message, 0, sizeof(*message));
        rt_copy_string(command, sizeof(command), "a004 FETCH ");
        rt_unsigned_to_string(sequence, sequence_text, sizeof(sequence_text));
        rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), sequence_text);
        rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), " (BODY.PEEK[HEADER.FIELDS (FROM TO CC SUBJECT DATE)] BODY.PEEK[TEXT]<0.4096>)\r\n");
        if (verbose) {
            mail_imap_diag_text("imap.command=", "FETCH message");
        }
        if (mail_imap_send_command(transport, command) != 0 || mail_imap_read_message_fetch(transport, "a004", message, print_raw) != 0) {
            return -1;
        }
        *message_count_out += 1U;
        index += 1U;
    }
    return 0;
}

static int mail_imap_read_greeting(MailTransport *transport, int verbose) {
    char line[MAIL_IMAP_LINE_CAPACITY];

    if (mail_transport_read_line(transport, line, sizeof(line)) != 0) {
        if (verbose) {
            mail_imap_diag_text("imap.greeting=", "read-failed");
            mail_imap_diag_text("tls.error=", platform_tls_last_error());
        }
        return -1;
    }
    if (verbose) {
        mail_imap_diag_text("imap.greeting=", mail_starts_with(line, "* OK") ? "ok" : "unexpected");
    }
    return mail_starts_with(line, "* OK") ? 0 : -1;
}

static int mail_imap_open_authenticated(MailTransport *transport, const MailConfig *config, const char *password, int verbose) {
    if (mail_transport_connect_tls(transport, config->imap_host, config->imap_port, "IMAP", verbose) != 0) {
        return -1;
    }
    if (mail_imap_read_greeting(transport, verbose) != 0) {
        return -1;
    }
    return mail_imap_login(transport, config, password, verbose);
}

static void mail_imap_logout(MailTransport *transport, int verbose) {
    if (transport->connected) {
        (void)mail_imap_simple_command(transport, "a999", "LOGOUT", 0, 0, verbose);
    }
}

int mail_imap_list_mailboxes_for_config(const MailConfig *config, const char *password, int verbose) {
    MailTransport transport;
    int result;

    memset(&transport, 0, sizeof(transport));
    if (mail_imap_open_authenticated(&transport, config, password, verbose) != 0) {
        mail_transport_close(&transport);
        return -1;
    }
    result = mail_imap_list_mailboxes(&transport, 0, 0U, 0, 1, verbose);
    mail_imap_logout(&transport, verbose);
    mail_transport_close(&transport);
    return result;
}

int mail_imap_load_mailboxes_for_config(const MailConfig *config, const char *password, MailFolder *folders, size_t folder_capacity, size_t *folder_count_out, int verbose) {
    MailTransport transport;
    int result;

    memset(&transport, 0, sizeof(transport));
    if (mail_imap_open_authenticated(&transport, config, password, verbose) != 0) {
        mail_transport_close(&transport);
        return -1;
    }
    result = mail_imap_list_mailboxes(&transport, folders, folder_capacity, folder_count_out, 0, verbose);
    mail_imap_logout(&transport, verbose);
    mail_transport_close(&transport);
    return result;
}

int mail_imap_fetch_messages_for_config(const MailConfig *config, const char *password, MailMessage *messages, size_t message_capacity, size_t *message_count_out, int verbose, int print_raw) {
    MailTransport transport;
    int result;

    memset(&transport, 0, sizeof(transport));
    if (mail_imap_open_authenticated(&transport, config, password, verbose) != 0) {
        mail_transport_close(&transport);
        return -1;
    }
    result = mail_imap_fetch_messages(&transport, config->folder, messages, message_capacity, message_count_out, verbose, print_raw);
    mail_imap_logout(&transport, verbose);
    mail_transport_close(&transport);
    return result;
}

int mail_smtp_check_tls_for_config(const MailConfig *config, char *error, size_t error_size, int verbose) {
    MailTransport transport;
    int result;

    mail_smtp_set_error(error, error_size, "SMTP TLS connection failed");
    memset(&transport, 0, sizeof(transport));
    result = mail_transport_connect_tls(&transport, config->smtp_host, config->smtp_port, "SMTP", verbose);
    if (result != 0) {
        mail_smtp_set_tls_error(error, error_size, "SMTP TLS connection failed");
    } else {
        mail_smtp_set_error(error, error_size, "");
    }
    mail_transport_close(&transport);
    return result;
}

int mail_smtp_send_text_for_config(const MailConfig *config, const char *password, const char *to, const char *subject, const char *body, char *error, size_t error_size, int verbose) {
    MailTransport transport;
    char command[MAIL_SMTP_TEXT_CAPACITY];
    char host[MAIL_TEXT_CAPACITY];
    char from_path[MAIL_TEXT_CAPACITY];
    char to_path[MAIL_TEXT_CAPACITY];
    char response[MAIL_TEXT_CAPACITY];
    int result = -1;

    mail_smtp_set_error(error, error_size, "SMTP send failed");
    if (config == 0 || password == 0 || to == 0 || to[0] == '\0' || subject == 0 || body == 0 || body[0] == '\0') {
        mail_smtp_set_error(error, error_size, "SMTP send missing message data");
        return -1;
    }
    memset(&transport, 0, sizeof(transport));
    if (mail_transport_connect_tls(&transport, config->smtp_host, config->smtp_port, "SMTP", verbose) != 0) {
        mail_smtp_set_tls_error(error, error_size, "SMTP TLS connection failed");
        mail_transport_close(&transport);
        return -1;
    }
    if (mail_smtp_read_response(&transport, 220, 0, response, sizeof(response), verbose) != 0) {
        mail_smtp_set_response_error(error, error_size, "SMTP greeting failed", response);
        goto done;
    }
    if (platform_get_hostname(host, sizeof(host)) != 0 || host[0] == '\0') {
        rt_copy_string(host, sizeof(host), "localhost");
    }
    rt_copy_string(command, sizeof(command), "EHLO ");
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), host);
    if (mail_smtp_send_line(&transport, command, verbose) != 0 || mail_smtp_read_response(&transport, 250, 0, response, sizeof(response), verbose) != 0) {
        mail_smtp_set_response_error(error, error_size, "SMTP EHLO failed", response);
        goto done;
    }
    if (mail_smtp_auth_plain(&transport, config, password, response, sizeof(response), verbose) != 0) {
        mail_smtp_set_response_error(error, error_size, "SMTP authentication failed", response);
        goto done;
    }
    mail_smtp_copy_path_address(from_path, sizeof(from_path), config->from[0] != '\0' ? config->from : config->username);
    mail_smtp_copy_path_address(to_path, sizeof(to_path), to);
    rt_copy_string(command, sizeof(command), "MAIL FROM:<");
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), from_path);
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), ">");
    if (mail_smtp_send_line(&transport, command, verbose) != 0 || mail_smtp_read_response(&transport, 250, 0, response, sizeof(response), verbose) != 0) {
        mail_smtp_set_response_error(error, error_size, "SMTP sender rejected", response);
        goto done;
    }
    rt_copy_string(command, sizeof(command), "RCPT TO:<");
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), to_path);
    rt_copy_string(command + rt_strlen(command), sizeof(command) - rt_strlen(command), ">");
    if (mail_smtp_send_line(&transport, command, verbose) != 0 || mail_smtp_read_response(&transport, 250, 251, response, sizeof(response), verbose) != 0) {
        mail_smtp_set_response_error(error, error_size, "SMTP recipient rejected", response);
        goto done;
    }
    if (mail_smtp_send_line(&transport, "DATA", verbose) != 0 || mail_smtp_read_response(&transport, 354, 0, response, sizeof(response), verbose) != 0) {
        mail_smtp_set_response_error(error, error_size, "SMTP DATA rejected", response);
        goto done;
    }
    if (mail_smtp_send_message_data(&transport, config, to, subject, body) != 0) {
        mail_smtp_set_error(error, error_size, "SMTP message write failed");
        goto done;
    }
    if (mail_smtp_send_cstr(&transport, ".\r\n") != 0 || mail_smtp_read_response(&transport, 250, 0, response, sizeof(response), verbose) != 0) {
        mail_smtp_set_response_error(error, error_size, "SMTP message rejected", response);
        goto done;
    }
    result = 0;
    mail_smtp_set_error(error, error_size, "");

done:
    (void)mail_smtp_send_line(&transport, "QUIT", verbose);
    mail_transport_close(&transport);
    return result;
}
