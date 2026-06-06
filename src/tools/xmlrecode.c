#include "platform.h"
#include "runtime.h"
#include "tool_util.h"


typedef enum {
    XML_RECODE_UTF8 = 0,
    XML_RECODE_LATIN1,
    XML_RECODE_WINDOWS_1252
} XmlRecodeEncoding;

static int parse_encoding(const char *text, XmlRecodeEncoding *encoding_out) {
    if (text == 0 || encoding_out == 0) return -1;
    if (rt_strcmp(text, "utf-8") == 0 || rt_strcmp(text, "UTF-8") == 0 || rt_strcmp(text, "utf8") == 0 || rt_strcmp(text, "UTF8") == 0) {
        *encoding_out = XML_RECODE_UTF8;
        return 0;
    }
    if (rt_strcmp(text, "iso-8859-1") == 0 || rt_strcmp(text, "ISO-8859-1") == 0 || rt_strcmp(text, "latin1") == 0 || rt_strcmp(text, "LATIN1") == 0) {
        *encoding_out = XML_RECODE_LATIN1;
        return 0;
    }
    if (rt_strcmp(text, "windows-1252") == 0 || rt_strcmp(text, "WINDOWS-1252") == 0 || rt_strcmp(text, "cp1252") == 0 || rt_strcmp(text, "CP1252") == 0) {
        *encoding_out = XML_RECODE_WINDOWS_1252;
        return 0;
    }
    return -1;
}

static int read_all_bytes(const char *path, unsigned char **buffer_out, size_t *length_out) {
    int fd;
    int should_close;
    unsigned char *buffer = 0;
    size_t length = 0U;
    size_t capacity = 0U;

    if (tool_open_input(path, &fd, &should_close) != 0) return -1;
    for (;;) {
        long count;
        if (length + 8192U > capacity) {
            size_t next_capacity;
            unsigned char *resized;
            if (length > (size_t)~(size_t)0 - 8192U) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                return -1;
            }
            next_capacity = capacity == 0U ? 8192U : capacity;
            while (next_capacity < length + 8192U) {
                if (next_capacity > (size_t)(~(size_t)0 / 2U)) {
                    rt_free(buffer);
                    tool_close_input(fd, should_close);
                    return -1;
                }
                next_capacity *= 2U;
            }
            resized = (unsigned char *)rt_malloc(next_capacity);
            if (resized == 0) {
                rt_free(buffer);
                tool_close_input(fd, should_close);
                return -1;
            }
            if (length > 0U) memcpy(resized, buffer, length);
            rt_free(buffer);
            buffer = resized;
            capacity = next_capacity;
        }
        count = platform_read(fd, (char *)buffer + length, capacity - length);
        if (count < 0) {
            rt_free(buffer);
            tool_close_input(fd, should_close);
            return -1;
        }
        if (count == 0) break;
        length += (size_t)count;
    }
    tool_close_input(fd, should_close);
    *buffer_out = buffer;
    *length_out = length;
    return 0;
}

static int is_name_char(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
}

static int detect_declared_encoding(const unsigned char *buffer, size_t length, char *encoding, size_t encoding_size) {
    size_t i;
    if (length < 5U || buffer[0] != '<' || buffer[1] != '?' || buffer[2] != 'x' || buffer[3] != 'm' || buffer[4] != 'l') return 0;
    for (i = 5U; i + 8U < length && !(buffer[i] == '?' && buffer[i + 1U] == '>'); ++i) {
        if ((buffer[i] == 'e' || buffer[i] == 'E') && i + 8U < length &&
            (buffer[i + 1U] == 'n' || buffer[i + 1U] == 'N') &&
            (buffer[i + 2U] == 'c' || buffer[i + 2U] == 'C') &&
            (buffer[i + 3U] == 'o' || buffer[i + 3U] == 'O') &&
            (buffer[i + 4U] == 'd' || buffer[i + 4U] == 'D') &&
            (buffer[i + 5U] == 'i' || buffer[i + 5U] == 'I') &&
            (buffer[i + 6U] == 'n' || buffer[i + 6U] == 'N') &&
            (buffer[i + 7U] == 'g' || buffer[i + 7U] == 'G')) {
            size_t j = i + 8U;
            unsigned char quote;
            size_t used = 0U;
            while (j < length && (buffer[j] == ' ' || buffer[j] == '\t' || buffer[j] == '\r' || buffer[j] == '\n')) j += 1U;
            if (j >= length || buffer[j] != '=') continue;
            j += 1U;
            while (j < length && (buffer[j] == ' ' || buffer[j] == '\t' || buffer[j] == '\r' || buffer[j] == '\n')) j += 1U;
            if (j >= length || (buffer[j] != '"' && buffer[j] != '\'')) continue;
            quote = buffer[j++];
            while (j < length && buffer[j] != quote && used + 1U < encoding_size) {
                if (!is_name_char(buffer[j])) break;
                encoding[used++] = (char)buffer[j++];
            }
            encoding[used] = '\0';
            return used > 0U ? 1 : 0;
        }
    }
    return 0;
}

static void write_utf8_codepoint(unsigned int codepoint) {
    char encoded[4];
    size_t length;
    if (rt_utf8_encode(codepoint, encoded, sizeof(encoded), &length) == 0) rt_write_all(1, encoded, length);
}

static unsigned int windows_1252_codepoint(unsigned char ch) {
    static const unsigned int table[32] = {
        0x20acU, 0x0081U, 0x201aU, 0x0192U, 0x201eU, 0x2026U, 0x2020U, 0x2021U,
        0x02c6U, 0x2030U, 0x0160U, 0x2039U, 0x0152U, 0x008dU, 0x017dU, 0x008fU,
        0x0090U, 0x2018U, 0x2019U, 0x201cU, 0x201dU, 0x2022U, 0x2013U, 0x2014U,
        0x02dcU, 0x2122U, 0x0161U, 0x203aU, 0x0153U, 0x009dU, 0x017eU, 0x0178U
    };
    if (ch >= 0x80U && ch <= 0x9fU) return table[ch - 0x80U];
    return (unsigned int)ch;
}

static int write_replacement_decl(const unsigned char *buffer, size_t length, size_t *index_io) {
    size_t index = 0U;
    if (length < 5U || buffer[0] != '<' || buffer[1] != '?' || buffer[2] != 'x' || buffer[3] != 'm' || buffer[4] != 'l') return 0;
    while (index + 1U < length) {
        if (buffer[index] == '?' && buffer[index + 1U] == '>') {
            rt_write_cstr(1, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
            *index_io = index + 2U;
            return 1;
        }
        index += 1U;
    }
    return 0;
}

static int recode_one(const char *path, int have_from, XmlRecodeEncoding from) {
    unsigned char *buffer;
    size_t length;
    size_t i = 0U;
    char declared[64];

    if (read_all_bytes(path, &buffer, &length) != 0) {
        tool_write_error("xmlrecode", "cannot read input: ", path == 0 ? "-" : path);
        return 1;
    }
    if (!have_from) {
        if (detect_declared_encoding(buffer, length, declared, sizeof(declared))) {
            if (parse_encoding(declared, &from) != 0) {
                tool_write_error("xmlrecode", "unsupported declared encoding: ", declared);
                rt_free(buffer);
                return 1;
            }
        } else {
            from = XML_RECODE_UTF8;
        }
    }
    if (from == XML_RECODE_UTF8) {
        if (rt_utf8_validate((const char *)buffer, length) != 0) {
            tool_write_error("xmlrecode", "invalid UTF-8 input", 0);
            rt_free(buffer);
            return 1;
        }
        rt_write_all(1, (const char *)buffer, length);
        rt_free(buffer);
        return 0;
    }
    (void)write_replacement_decl(buffer, length, &i);
    while (i < length) {
        unsigned int codepoint = from == XML_RECODE_WINDOWS_1252 ? windows_1252_codepoint(buffer[i]) : (unsigned int)buffer[i];
        write_utf8_codepoint(codepoint);
        i += 1U;
    }
    rt_free(buffer);
    return 0;
}

int main(int argc, char **argv) {
    ToolOptState opt;
    XmlRecodeEncoding from = XML_RECODE_UTF8;
    int have_from = 0;
    int option_result;
    int exit_code = 0;
    int i;

    tool_opt_init(&opt, argc, argv, "xmlrecode", "[--from ENCODING] [FILE ...]");
    while ((option_result = tool_opt_next(&opt)) == TOOL_OPT_FLAG) {
        if (rt_strcmp(opt.flag, "--from") == 0) {
            if (tool_opt_require_value(&opt) != 0) return 1;
            if (parse_encoding(opt.value, &from) != 0) {
                tool_write_error("xmlrecode", "unsupported encoding: ", opt.value);
                return 1;
            }
            have_from = 1;
        } else {
            tool_write_error("xmlrecode", "unknown option: ", opt.flag);
            tool_write_usage("xmlrecode", "[--from ENCODING] [FILE ...]");
            return 1;
        }
    }
    if (option_result == TOOL_OPT_HELP) {
        tool_write_usage("xmlrecode", "[--from ENCODING] [FILE ...]");
        return 0;
    }
    if (opt.argi >= argc) return recode_one(0, have_from, from);
    for (i = opt.argi; i < argc; ++i) if (recode_one(argv[i], have_from, from) != 0) exit_code = 1;
    return exit_code;
}