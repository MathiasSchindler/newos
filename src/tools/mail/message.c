#include "message.h"

#include "mime.h"

#include "runtime.h"
#include "tool_util.h"


static int mail_ascii_lower(int ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A' + 'a';
    }
    return ch;
}

static int mail_starts_with_ci(const char *text, const char *prefix) {
    size_t index;

    for (index = 0U; prefix[index] != '\0'; ++index) {
        if (text[index] == '\0' || mail_ascii_lower((unsigned char)text[index]) != mail_ascii_lower((unsigned char)prefix[index])) {
            return 0;
        }
    }
    return 1;
}


static void mail_decode_rfc2047_q(char *text) {
    char decoded[MAIL_TEXT_CAPACITY];
    size_t input = 0U;
    size_t output = 0U;

    while (text[input] != '\0' && output + 1U < sizeof(decoded)) {
        if (text[input] == '=' && text[input + 1U] == '?' &&
            mail_starts_with_ci(text + input + 2U, "UTF-8?Q?")) {
            size_t word = input + 10U;

            while (text[word] != '\0' && !(text[word] == '?' && text[word + 1U] == '=') && output + 1U < sizeof(decoded)) {
                if (text[word] == '_') {
                    decoded[output++] = ' ';
                    word += 1U;
                } else if (text[word] == '=' && tool_hex_value(text[word + 1U]) >= 0 && tool_hex_value(text[word + 2U]) >= 0) {
                    decoded[output++] = (char)((tool_hex_value(text[word + 1U]) << 4) | tool_hex_value(text[word + 2U]));
                    word += 3U;
                } else {
                    decoded[output++] = text[word++];
                }
            }
            if (text[word] == '?' && text[word + 1U] == '=') {
                input = word + 2U;
                while (text[input] == ' ') {
                    input += 1U;
                }
            } else {
                input += 1U;
            }
        } else {
            decoded[output++] = text[input++];
        }
    }
    decoded[output] = '\0';
    rt_copy_string(text, MAIL_TEXT_CAPACITY, decoded);
}

static void mail_decode_quoted_printable_line(char *text, size_t text_size) {
    char decoded[MAIL_BODY_CAPACITY];
    size_t input = 0U;
    size_t output = 0U;

    while (text[input] != '\0' && output + 1U < sizeof(decoded)) {
        if (text[input] == '=' && tool_hex_value(text[input + 1U]) >= 0 && tool_hex_value(text[input + 2U]) >= 0) {
            decoded[output++] = (char)((tool_hex_value(text[input + 1U]) << 4) | tool_hex_value(text[input + 2U]));
            input += 3U;
        } else if (text[input] == '=' && text[input + 1U] == '\0') {
            break;
        } else {
            decoded[output++] = text[input++];
        }
    }
    decoded[output] = '\0';
    rt_copy_string(text, text_size, decoded);
}

static void mail_copy_header_value(char *dst, size_t dst_size, const char *line, const char *prefix) {
    const char *value = line + rt_strlen(prefix);

    while (*value != '\0' && rt_is_space(*value)) {
        value += 1;
    }
    rt_copy_string(dst, dst_size, value);
}

static void mail_append_text(char *dst, size_t dst_size, const char *text) {
    size_t used = rt_strlen(dst);

    if (used + 1U >= dst_size) {
        return;
    }
    if (used > 0U) {
        dst[used++] = ' ';
        dst[used] = '\0';
    }
    rt_copy_string(dst + used, dst_size - used, text);
}

static void mail_append_body_line(MailMessage *message, const char *line) {
    size_t used;

    used = rt_strlen(message->body);
    if (used + 2U >= sizeof(message->body)) {
        return;
    }
    if (used > 0U) {
        message->body[used++] = '\n';
        message->body[used] = '\0';
    }
    rt_copy_string(message->body + used, sizeof(message->body) - used, line);
}

void mail_message_capture_line(MailMessage *message, const char *line) {
    if (message == 0 || line == 0 || line[0] == ')' || tool_starts_with(line, "a004 ")) {
        return;
    }
    if (tool_starts_with(line, " BODY[TEXT]")) {
        message->body_started = 1;
        return;
    }
    if (message->body_started) {
        mail_append_body_line(message, line);
        return;
    }
    if (line[0] == '*' || tool_starts_with(line, " BODY[")) {
        return;
    }
    if (line[0] == '\0') {
        return;
    }
    if ((line[0] == ' ' || line[0] == '\t') && message->subject[0] != '\0' && message->preview[0] == '\0') {
        const char *folded = line;
        while (*folded != '\0' && rt_is_space(*folded)) {
            folded += 1;
        }
        if (tool_starts_with(folded, "BODY[") || tool_starts_with(folded, "FLAGS ")) {
            return;
        }
        while (*line != '\0' && rt_is_space(*line)) {
            line += 1;
        }
        mail_append_text(message->subject, MAIL_TEXT_CAPACITY, line);
        mail_decode_rfc2047_q(message->subject);
    } else if (mail_starts_with_ci(line, "From:")) {
        mail_copy_header_value(message->from, MAIL_TEXT_CAPACITY, line, "From:");
        mail_decode_rfc2047_q(message->from);
    } else if (mail_starts_with_ci(line, "To:")) {
        mail_copy_header_value(message->to, MAIL_TEXT_CAPACITY, line, "To:");
        mail_decode_rfc2047_q(message->to);
    } else if (mail_starts_with_ci(line, "Cc:")) {
        mail_copy_header_value(message->cc, MAIL_TEXT_CAPACITY, line, "Cc:");
        mail_decode_rfc2047_q(message->cc);
    } else if (mail_starts_with_ci(line, "Subject:")) {
        mail_copy_header_value(message->subject, MAIL_TEXT_CAPACITY, line, "Subject:");
        mail_decode_rfc2047_q(message->subject);
    } else if (mail_starts_with_ci(line, "Date:")) {
        mail_copy_header_value(message->date, MAIL_TEXT_CAPACITY, line, "Date:");
        return;
    } else if (!mail_starts_with_ci(line, "To:") &&
               !mail_starts_with_ci(line, "Cc:") &&
               !mail_starts_with_ci(line, "Message-ID:") &&
               !mail_starts_with_ci(line, "Content-") &&
               !mail_starts_with_ci(line, "MIME-") &&
               !tool_starts_with(line, " BODY[") &&
               !tool_starts_with(line, "--") &&
               message->preview[0] == '\0') {
        rt_copy_string(message->preview, MAIL_PREVIEW_CAPACITY, line);
        mail_decode_quoted_printable_line(message->preview, MAIL_PREVIEW_CAPACITY);
    }
}

void mail_message_finalize(MailMessage *message) {
    char decoded_body[MAIL_BODY_CAPACITY];

    if (message == 0) {
        return;
    }
    if (message->body[0] != '\0' && mail_mime_extract_text(message->body, decoded_body, sizeof(decoded_body)) == 0) {
        rt_copy_string(message->body, sizeof(message->body), decoded_body);
    }
    if (message->preview[0] == '\0') {
        mail_mime_first_preview_line(message->body, message->preview, sizeof(message->preview));
    }
    if (message->from[0] == '\0') rt_copy_string(message->from, sizeof(message->from), "[unknown]");
    if (message->to[0] == '\0') rt_copy_string(message->to, sizeof(message->to), "[unknown]");
    if (message->date[0] == '\0') rt_copy_string(message->date, sizeof(message->date), "[unknown date]");
    if (message->subject[0] == '\0') rt_copy_string(message->subject, sizeof(message->subject), "[no subject]");
    if (message->preview[0] == '\0') rt_copy_string(message->preview, sizeof(message->preview), "[no preview]");
    if (message->body[0] == '\0') rt_copy_string(message->body, sizeof(message->body), message->preview);
}
