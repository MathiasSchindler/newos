#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define BASE64_BUFFER_SIZE 4096U

static const char BASE64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int decode_mode;
static int wrap_columns = 76;

static void print_usage(void) {
    tool_write_usage("base64", "[-d] [-w COLS] [FILE]");
}

static int base64_value(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

static int encode_fd(int fd) {
    unsigned char input[BASE64_BUFFER_SIZE];
    unsigned char carry[3];
    int carry_count = 0;
    long bytes;
    int column = 0;

    while ((bytes = platform_read(fd, input, sizeof(input))) > 0) {
        long i;
        for (i = 0; i < bytes; ++i) {
            carry[carry_count++] = input[i];
            if (carry_count == 3) {
                unsigned int b0 = carry[0];
                unsigned int b1 = carry[1];
                unsigned int b2 = carry[2];
                char out[4];
                int j;

                out[0] = BASE64_ALPHABET[(b0 >> 2U) & 0x3fU];
                out[1] = BASE64_ALPHABET[((b0 << 4U) | (b1 >> 4U)) & 0x3fU];
                out[2] = BASE64_ALPHABET[((b1 << 2U) | (b2 >> 6U)) & 0x3fU];
                out[3] = BASE64_ALPHABET[b2 & 0x3fU];
                for (j = 0; j < 4; ++j) {
                    if (rt_write_char(1, out[j]) != 0) return -1;
                    column += 1;
                    if (wrap_columns > 0 && column >= wrap_columns) {
                        if (rt_write_char(1, '\n') != 0) return -1;
                        column = 0;
                    }
                }
                carry_count = 0;
            }
        }
    }
    if (bytes < 0) return -1;
    if (carry_count > 0) {
            unsigned int b0 = carry[0];
            unsigned int b1 = carry_count > 1 ? carry[1] : 0U;
            unsigned int b2 = 0U;
            char out[4];
            int j;

            out[0] = BASE64_ALPHABET[(b0 >> 2U) & 0x3fU];
            out[1] = BASE64_ALPHABET[((b0 << 4U) | (b1 >> 4U)) & 0x3fU];
            out[2] = carry_count > 1 ? BASE64_ALPHABET[((b1 << 2U) | (b2 >> 6U)) & 0x3fU] : '=';
            out[3] = '=';
            for (j = 0; j < 4; ++j) {
                if (rt_write_char(1, out[j]) != 0) return -1;
                column += 1;
                if (wrap_columns > 0 && column >= wrap_columns) {
                    if (rt_write_char(1, '\n') != 0) return -1;
                    column = 0;
                }
            }
    }
    if (column != 0) {
        return rt_write_char(1, '\n');
    }
    return 0;
}

static int decode_quad(const int values[4], int pads) {
    unsigned char out[3];

    out[0] = (unsigned char)((values[0] << 2U) | ((values[1] >> 4U) & 0x03U));
    out[1] = (unsigned char)(((values[1] & 0x0fU) << 4U) | ((values[2] >> 2U) & 0x0fU));
    out[2] = (unsigned char)(((values[2] & 0x03U) << 6U) | values[3]);
    if (rt_write_all(1, out, (size_t)(3 - pads)) != 0) return -1;
    return 0;
}

static int decode_fd(int fd) {
    char input[BASE64_BUFFER_SIZE];
    int values[4];
    int value_count = 0;
    long bytes;

    while ((bytes = platform_read(fd, input, sizeof(input))) > 0) {
        long i;
        for (i = 0; i < bytes; ++i) {
            char ch = input[i];
            int value;

            if (rt_is_space(ch)) continue;
            if (ch == '=') {
                values[value_count++] = 0;
            } else {
                value = base64_value(ch);
                if (value < 0) return -1;
                values[value_count++] = value;
            }
            if (value_count == 4) {
                int pads = (input[i] == '=') ? 1 : 0;
                if (i > 0 && input[i - 1] == '=') pads = 2;
                if (decode_quad(values, pads) != 0) return -1;
                value_count = 0;
            }
        }
    }
    return bytes < 0 || value_count != 0 ? -1 : 0;
}

int main(int argc, char **argv) {
    int argi = 1;
    const char *path = "-";
    int fd;
    int should_close;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-d") == 0 || rt_strcmp(argv[argi], "--decode") == 0) {
            decode_mode = 1;
        } else if (rt_strcmp(argv[argi], "-w") == 0) {
            unsigned long long value;
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &value, "base64", "wrap") != 0 || value > 4096ULL) {
                return 1;
            }
            wrap_columns = (int)value;
            argi += 1;
        } else if (rt_strncmp(argv[argi], "-w", 2U) == 0) {
            unsigned long long value;
            if (tool_parse_uint_arg(argv[argi] + 2, &value, "base64", "wrap") != 0 || value > 4096ULL) return 1;
            wrap_columns = (int)value;
        } else if (rt_strcmp(argv[argi], "-h") == 0 || rt_strcmp(argv[argi], "--help") == 0) {
            print_usage();
            return 0;
        } else {
            tool_write_error("base64", "unknown option: ", argv[argi]);
            return 1;
        }
        argi += 1;
    }
    if (argi < argc) path = argv[argi++];
    if (argi != argc) {
        print_usage();
        return 1;
    }
    if (tool_open_input(path, &fd, &should_close) != 0) {
        tool_write_error("base64", "cannot open ", path);
        return 1;
    }
    if ((decode_mode ? decode_fd(fd) : encode_fd(fd)) != 0) {
        tool_close_input(fd, should_close);
        tool_write_error("base64", decode_mode ? "decode failed" : "encode failed", 0);
        return 1;
    }
    tool_close_input(fd, should_close);
    return 0;
}
