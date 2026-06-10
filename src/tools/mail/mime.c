#include "mime.h"

#include "runtime.h"
#include "tool_util.h"

#define MAIL_MIME_MAX_PARTS 16U

typedef enum {
    MAIL_MIME_KIND_OTHER = 0,
    MAIL_MIME_KIND_TEXT,
    MAIL_MIME_KIND_HTML
} MailMimeKind;

typedef enum {
    MAIL_MIME_ENCODING_7BIT = 0,
    MAIL_MIME_ENCODING_QUOTED_PRINTABLE,
    MAIL_MIME_ENCODING_BASE64
} MailMimeEncoding;

typedef struct {
    MailMimeKind kind;
    MailMimeEncoding encoding;
    size_t body_start;
    size_t body_end;
} MailMimePart;

static int mail_mime_ascii_lower(int ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 'a';
    return ch;
}

static int mail_mime_starts_with_ci(const char *text, const char *prefix) {
    size_t index;

    for (index = 0U; prefix[index] != '\0'; ++index) {
        if (text[index] == '\0' || mail_mime_ascii_lower((unsigned char)text[index]) != mail_mime_ascii_lower((unsigned char)prefix[index])) {
            return 0;
        }
    }
    return 1;
}

static int mail_mime_contains_ci(const char *text, const char *needle) {
    size_t index;

    if (needle[0] == '\0') return 1;
    for (index = 0U; text[index] != '\0'; ++index) {
        if (mail_mime_starts_with_ci(text + index, needle)) return 1;
    }
    return 0;
}

static int mail_mime_line_starts_ci(const char *raw, size_t start, size_t end, const char *prefix) {
    size_t index;

    for (index = 0U; prefix[index] != '\0'; ++index) {
        if (start + index >= end || mail_mime_ascii_lower((unsigned char)raw[start + index]) != mail_mime_ascii_lower((unsigned char)prefix[index])) {
            return 0;
        }
    }
    return 1;
}

static int mail_mime_line_is_blank(const char *raw, size_t start, size_t end) {
    while (start < end) {
        if (!rt_is_space(raw[start])) return 0;
        start += 1U;
    }
    return 1;
}

static int mail_mime_line_is_boundary(const char *raw, size_t start, size_t end) {
    return end > start + 2U && raw[start] == '-' && raw[start + 1U] == '-' && raw[start + 2U] != ' ';
}

static void mail_mime_set_content_type(MailMimePart *part, const char *raw, size_t start, size_t end) {
    char line[256];
    size_t length = end > start ? end - start : 0U;

    if (length >= sizeof(line)) length = sizeof(line) - 1U;
    memcpy(line, raw + start, length);
    line[length] = '\0';
    if (mail_mime_contains_ci(line, "text/plain")) {
        part->kind = MAIL_MIME_KIND_TEXT;
    } else if (mail_mime_contains_ci(line, "text/html")) {
        part->kind = MAIL_MIME_KIND_HTML;
    }
}

static void mail_mime_set_encoding(MailMimePart *part, const char *raw, size_t start, size_t end) {
    char line[160];
    size_t length = end > start ? end - start : 0U;

    if (length >= sizeof(line)) length = sizeof(line) - 1U;
    memcpy(line, raw + start, length);
    line[length] = '\0';
    if (mail_mime_contains_ci(line, "quoted-printable")) {
        part->encoding = MAIL_MIME_ENCODING_QUOTED_PRINTABLE;
    } else if (mail_mime_contains_ci(line, "base64")) {
        part->encoding = MAIL_MIME_ENCODING_BASE64;
    }
}


static int mail_mime_base64_value(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static void mail_mime_append_char(char *output, size_t output_size, size_t *used, char ch) {
    if (*used + 1U >= output_size) return;
    output[*used] = ch;
    *used += 1U;
    output[*used] = '\0';
}

static void mail_mime_append_space(char *output, size_t output_size, size_t *used) {
    if (*used == 0U || output[*used - 1U] == ' ' || output[*used - 1U] == '\n') return;
    mail_mime_append_char(output, output_size, used, ' ');
}

static void mail_mime_append_newline(char *output, size_t output_size, size_t *used) {
    if (*used == 0U || output[*used - 1U] == '\n') return;
    if (*used >= 2U && output[*used - 1U] == ' ' ) {
        *used -= 1U;
        output[*used] = '\0';
    }
    mail_mime_append_char(output, output_size, used, '\n');
}

static void mail_mime_decode_7bit_range(const char *raw, size_t start, size_t end, char *output, size_t output_size) {
    size_t used = 0U;

    output[0] = '\0';
    while (start < end && used + 1U < output_size) {
        output[used++] = raw[start++];
    }
    output[used] = '\0';
}

static void mail_mime_decode_qp_range(const char *raw, size_t start, size_t end, char *output, size_t output_size) {
    size_t used = 0U;

    output[0] = '\0';
    while (start < end && used + 1U < output_size) {
        if (raw[start] == '=' && start + 2U < end && tool_hex_value(raw[start + 1U]) >= 0 && tool_hex_value(raw[start + 2U]) >= 0) {
            output[used++] = (char)((tool_hex_value(raw[start + 1U]) << 4) | tool_hex_value(raw[start + 2U]));
            start += 3U;
        } else if (raw[start] == '=' && start + 1U < end && raw[start + 1U] == '\n') {
            start += 2U;
        } else if (raw[start] == '=' && start + 2U < end && raw[start + 1U] == '\r' && raw[start + 2U] == '\n') {
            start += 3U;
        } else {
            output[used++] = raw[start++];
        }
    }
    output[used] = '\0';
}

static void mail_mime_decode_base64_range(const char *raw, size_t start, size_t end, char *output, size_t output_size) {
    unsigned int accum = 0U;
    unsigned int bits = 0U;
    size_t used = 0U;

    output[0] = '\0';
    while (start < end && used + 1U < output_size) {
        int value;
        if (raw[start] == '=') break;
        value = mail_mime_base64_value(raw[start]);
        start += 1U;
        if (value < 0) continue;
        accum = (accum << 6) | (unsigned int)value;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            output[used++] = (char)((accum >> bits) & 0xffU);
        }
    }
    output[used] = '\0';
}

static void mail_mime_decode_part(const char *raw, const MailMimePart *part, char *output, size_t output_size) {
    if (output_size == 0U) return;
    if (part->encoding == MAIL_MIME_ENCODING_QUOTED_PRINTABLE) {
        mail_mime_decode_qp_range(raw, part->body_start, part->body_end, output, output_size);
    } else if (part->encoding == MAIL_MIME_ENCODING_BASE64) {
        mail_mime_decode_base64_range(raw, part->body_start, part->body_end, output, output_size);
    } else {
        mail_mime_decode_7bit_range(raw, part->body_start, part->body_end, output, output_size);
    }
}

static int mail_mime_tag_is_block(const char *tag) {
    return mail_mime_starts_with_ci(tag, "br") ||
           mail_mime_starts_with_ci(tag, "p") ||
           mail_mime_starts_with_ci(tag, "/p") ||
           mail_mime_starts_with_ci(tag, "div") ||
           mail_mime_starts_with_ci(tag, "/div") ||
           mail_mime_starts_with_ci(tag, "tr") ||
           mail_mime_starts_with_ci(tag, "/tr") ||
           mail_mime_starts_with_ci(tag, "li") ||
           mail_mime_starts_with_ci(tag, "/li") ||
           mail_mime_starts_with_ci(tag, "h1") ||
           mail_mime_starts_with_ci(tag, "h2") ||
           mail_mime_starts_with_ci(tag, "h3") ||
           mail_mime_starts_with_ci(tag, "h4") ||
           mail_mime_starts_with_ci(tag, "h5") ||
           mail_mime_starts_with_ci(tag, "h6");
}

static void mail_mime_append_entity(const char *html, size_t *index, char *output, size_t output_size, size_t *used) {
    size_t start = *index + 1U;
    size_t end = start;

    while (html[end] != '\0' && html[end] != ';' && end - start < 16U) end += 1U;
    if (html[end] != ';') {
        mail_mime_append_char(output, output_size, used, '&');
        *index += 1U;
        return;
    }
    if (rt_strncmp(html + start, "amp", 3U) == 0 && end == start + 3U) mail_mime_append_char(output, output_size, used, '&');
    else if (rt_strncmp(html + start, "lt", 2U) == 0 && end == start + 2U) mail_mime_append_char(output, output_size, used, '<');
    else if (rt_strncmp(html + start, "gt", 2U) == 0 && end == start + 2U) mail_mime_append_char(output, output_size, used, '>');
    else if (rt_strncmp(html + start, "quot", 4U) == 0 && end == start + 4U) mail_mime_append_char(output, output_size, used, '"');
    else if (rt_strncmp(html + start, "apos", 4U) == 0 && end == start + 4U) mail_mime_append_char(output, output_size, used, '\'');
    else if (rt_strncmp(html + start, "nbsp", 4U) == 0 && end == start + 4U) mail_mime_append_space(output, output_size, used);
    else if (html[start] == '#') {
        unsigned int value = 0U;
        size_t cursor = start + 1U;
        int hex = 0;
        if (html[cursor] == 'x' || html[cursor] == 'X') {
            hex = 1;
            cursor += 1U;
        }
        while (cursor < end) {
            int digit = hex ? tool_hex_value(html[cursor]) : (html[cursor] >= '0' && html[cursor] <= '9' ? html[cursor] - '0' : -1);
            if (digit < 0) break;
            value = hex ? value * 16U + (unsigned int)digit : value * 10U + (unsigned int)digit;
            cursor += 1U;
        }
        if (cursor == end && value > 0U && value < 128U) mail_mime_append_char(output, output_size, used, (char)value);
    }
    *index = end + 1U;
}

static void mail_mime_html_to_text(const char *html, char *output, size_t output_size) {
    size_t index = 0U;
    size_t used = 0U;
    int suppress = 0;

    if (output_size == 0U) return;
    output[0] = '\0';
    while (html[index] != '\0' && used + 1U < output_size) {
        if (html[index] == '<') {
            char tag[24];
            size_t tag_used = 0U;
            size_t cursor = index + 1U;
            int closing_comment = 0;

            if (html[cursor] == '!' && html[cursor + 1U] == '-' && html[cursor + 2U] == '-') {
                cursor += 3U;
                while (html[cursor] != '\0' && !(html[cursor] == '-' && html[cursor + 1U] == '-' && html[cursor + 2U] == '>')) cursor += 1U;
                index = html[cursor] != '\0' ? cursor + 3U : cursor;
                continue;
            }
            while (html[cursor] != '\0' && html[cursor] != '>' && !rt_is_space(html[cursor]) && tag_used + 1U < sizeof(tag)) {
                tag[tag_used++] = html[cursor++];
            }
            tag[tag_used] = '\0';
            if (mail_mime_starts_with_ci(tag, "script") || mail_mime_starts_with_ci(tag, "style")) suppress = 1;
            if (mail_mime_starts_with_ci(tag, "/script") || mail_mime_starts_with_ci(tag, "/style")) suppress = 0;
            closing_comment = mail_mime_tag_is_block(tag);
            while (html[cursor] != '\0' && html[cursor] != '>') cursor += 1U;
            index = html[cursor] == '>' ? cursor + 1U : cursor;
            if (!suppress && closing_comment) mail_mime_append_newline(output, output_size, &used);
        } else if (suppress) {
            index += 1U;
        } else if (html[index] == '&') {
            mail_mime_append_entity(html, &index, output, output_size, &used);
        } else if (rt_is_space(html[index])) {
            mail_mime_append_space(output, output_size, &used);
            index += 1U;
        } else {
            mail_mime_append_char(output, output_size, &used, html[index]);
            index += 1U;
        }
    }
    while (used > 0U && (output[used - 1U] == ' ' || output[used - 1U] == '\n')) used -= 1U;
    output[used] = '\0';
}

static size_t mail_mime_parse_parts(const char *raw, MailMimePart *parts, size_t part_capacity) {
    size_t raw_len = rt_strlen(raw);
    size_t line_start = 0U;
    size_t count = 0U;
    MailMimePart current;
    int in_part = 0;
    int in_headers = 0;

    memset(&current, 0, sizeof(current));
    while (line_start <= raw_len) {
        size_t line_end = line_start;
        while (line_end < raw_len && raw[line_end] != '\n') line_end += 1U;
        if (line_end > line_start && raw[line_end - 1U] == '\r') line_end -= 1U;

        if (mail_mime_line_is_boundary(raw, line_start, line_end)) {
            if (in_part && count < part_capacity) {
                current.body_end = line_start > 0U && raw[line_start - 1U] == '\n' ? line_start - 1U : line_start;
                parts[count++] = current;
            }
            if (line_end >= line_start + 4U && raw[line_end - 1U] == '-' && raw[line_end - 2U] == '-') {
                in_part = 0;
                in_headers = 0;
            } else {
                memset(&current, 0, sizeof(current));
                current.kind = MAIL_MIME_KIND_OTHER;
                current.encoding = MAIL_MIME_ENCODING_7BIT;
                current.body_start = line_end < raw_len ? line_end + 1U : line_end;
                current.body_end = current.body_start;
                in_part = 1;
                in_headers = 1;
            }
        } else if (in_part && in_headers) {
            if (mail_mime_line_is_blank(raw, line_start, line_end)) {
                in_headers = 0;
                current.body_start = line_end < raw_len ? line_end + 1U : line_end;
            } else if (mail_mime_line_starts_ci(raw, line_start, line_end, "Content-Type:")) {
                mail_mime_set_content_type(&current, raw, line_start, line_end);
            } else if (mail_mime_line_starts_ci(raw, line_start, line_end, "Content-Transfer-Encoding:")) {
                mail_mime_set_encoding(&current, raw, line_start, line_end);
            }
        }

        if (line_end >= raw_len) break;
        line_start = line_end + 1U;
    }
    if (in_part && count < part_capacity) {
        current.body_end = raw_len;
        parts[count++] = current;
    }
    return count;
}

static int mail_mime_parse_single_part(const char *raw, MailMimePart *part) {
    size_t raw_len = rt_strlen(raw);
    size_t line_start = 0U;
    int saw_header = 0;

    memset(part, 0, sizeof(*part));
    part->kind = MAIL_MIME_KIND_TEXT;
    part->encoding = MAIL_MIME_ENCODING_7BIT;
    part->body_start = 0U;
    part->body_end = raw_len;
    while (line_start < raw_len) {
        size_t line_end = line_start;
        while (line_end < raw_len && raw[line_end] != '\n') line_end += 1U;
        if (line_end > line_start && raw[line_end - 1U] == '\r') line_end -= 1U;
        if (mail_mime_line_is_blank(raw, line_start, line_end)) {
            if (saw_header) {
                part->body_start = line_end < raw_len ? line_end + 1U : line_end;
            }
            return saw_header;
        }
        if (mail_mime_line_starts_ci(raw, line_start, line_end, "Content-Type:")) {
            saw_header = 1;
            mail_mime_set_content_type(part, raw, line_start, line_end);
        } else if (mail_mime_line_starts_ci(raw, line_start, line_end, "Content-Transfer-Encoding:")) {
            saw_header = 1;
            mail_mime_set_encoding(part, raw, line_start, line_end);
        } else if (!mail_mime_line_starts_ci(raw, line_start, line_end, "MIME-Version:")) {
            return 0;
        }
        if (line_end >= raw_len) break;
        line_start = line_end + 1U;
    }
    return saw_header;
}

static const MailMimePart *mail_mime_choose_part(const MailMimePart *parts, size_t count) {
    size_t index;
    const MailMimePart *first_html = 0;
    const MailMimePart *first_text = 0;

    for (index = 0U; index < count; ++index) {
        if (parts[index].body_end <= parts[index].body_start) continue;
        if (parts[index].kind == MAIL_MIME_KIND_TEXT) return &parts[index];
        if (parts[index].kind == MAIL_MIME_KIND_HTML && first_html == 0) first_html = &parts[index];
        if (parts[index].kind != MAIL_MIME_KIND_OTHER && first_text == 0) first_text = &parts[index];
    }
    if (first_html != 0) return first_html;
    return first_text;
}

void mail_mime_first_preview_line(const char *text, char *preview, size_t preview_size) {
    size_t input = 0U;
    size_t output = 0U;

    if (preview_size == 0U) return;
    preview[0] = '\0';
    while (text[input] != '\0' && rt_is_space(text[input])) input += 1U;
    while (text[input] != '\0' && text[input] != '\n' && output + 1U < preview_size) {
        preview[output++] = text[input++];
    }
    while (output > 0U && rt_is_space(preview[output - 1U])) output -= 1U;
    preview[output] = '\0';
}

int mail_mime_extract_text(const char *raw, char *output, size_t output_size) {
    MailMimePart parts[MAIL_MIME_MAX_PARTS];
    MailMimePart single;
    const MailMimePart *chosen;
    size_t count;
    char decoded[4096];

    if (raw == 0 || output == 0 || output_size == 0U) return -1;
    output[0] = '\0';
    count = mail_mime_parse_parts(raw, parts, MAIL_MIME_MAX_PARTS);
    if (count == 0U) {
        if (mail_mime_parse_single_part(raw, &single)) {
            chosen = &single;
        } else {
            single.kind = MAIL_MIME_KIND_TEXT;
            single.encoding = mail_mime_contains_ci(raw, "=\n") ? MAIL_MIME_ENCODING_QUOTED_PRINTABLE : MAIL_MIME_ENCODING_7BIT;
            single.body_start = 0U;
            single.body_end = rt_strlen(raw);
            chosen = &single;
        }
    } else {
        chosen = mail_mime_choose_part(parts, count);
    }
    if (chosen == 0) return -1;
    mail_mime_decode_part(raw, chosen, decoded, sizeof(decoded));
    if (chosen->kind == MAIL_MIME_KIND_HTML) {
        mail_mime_html_to_text(decoded, output, output_size);
    } else {
        rt_copy_string(output, output_size, decoded);
    }
    return output[0] != '\0' ? 0 : -1;
}
