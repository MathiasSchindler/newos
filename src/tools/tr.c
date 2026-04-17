#include "platform.h"
#include "runtime.h"

#define TR_SET_CAPACITY 512

typedef struct {
    int delete_chars;
    int squeeze_chars;
    int translate_chars;
    char set1[TR_SET_CAPACITY];
    char set2[TR_SET_CAPACITY];
    char squeeze_set[TR_SET_CAPACITY];
} TrOptions;

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " [-d] [-s] SET1 [SET2]");
}

static char decode_escape_char(char ch) {
    if (ch == 'n') {
        return '\n';
    }
    if (ch == 't') {
        return '\t';
    }
    if (ch == 'r') {
        return '\r';
    }
    if (ch == '0') {
        return '\0';
    }
    return ch;
}

static int append_char(char *buffer, size_t buffer_size, size_t *length, char ch) {
    if (*length + 1 >= buffer_size) {
        return -1;
    }
    buffer[*length] = ch;
    *length += 1;
    buffer[*length] = '\0';
    return 0;
}

static int expand_set(const char *text, char *buffer, size_t buffer_size) {
    size_t length = 0;
    size_t i = 0;

    if (buffer_size == 0) {
        return -1;
    }

    buffer[0] = '\0';

    while (text[i] != '\0') {
        char first = text[i];

        if (first == '\\' && text[i + 1] != '\0') {
            i += 1;
            first = decode_escape_char(text[i]);
        }

        if (text[i + 1] == '-' && text[i + 2] != '\0') {
            char last = text[i + 2];
            int step = 1;

            if (last == '\\' && text[i + 3] != '\0') {
                last = decode_escape_char(text[i + 3]);
                i += 1;
            }

            if ((unsigned char)first > (unsigned char)last) {
                step = -1;
            }

            while (1) {
                if (append_char(buffer, buffer_size, &length, first) != 0) {
                    return -1;
                }
                if (first == last) {
                    break;
                }
                first = (char)(first + step);
            }

            i += 3;
            continue;
        }

        if (append_char(buffer, buffer_size, &length, first) != 0) {
            return -1;
        }
        i += 1;
    }

    return 0;
}

static int char_in_set(char ch, const char *set) {
    size_t i = 0;
    while (set[i] != '\0') {
        if (set[i] == ch) {
            return 1;
        }
        i += 1;
    }
    return 0;
}

static char translate_char(char ch, const char *set1, const char *set2) {
    size_t i;
    size_t set2_len = rt_strlen(set2);

    for (i = 0; set1[i] != '\0'; ++i) {
        if (set1[i] == ch) {
            if (set2_len == 0) {
                return ch;
            }
            if (i < set2_len) {
                return set2[i];
            }
            return set2[set2_len - 1];
        }
    }

    return ch;
}

static int parse_options(int argc, char **argv, TrOptions *options) {
    int arg_index = 1;
    int remaining;

    rt_memset(options, 0, sizeof(*options));

    while (arg_index < argc && argv[arg_index][0] == '-' && argv[arg_index][1] != '\0') {
        const char *flag = argv[arg_index] + 1;

        if (rt_strcmp(argv[arg_index], "--") == 0) {
            arg_index += 1;
            break;
        }

        while (*flag != '\0') {
            if (*flag == 'd') {
                options->delete_chars = 1;
            } else if (*flag == 's') {
                options->squeeze_chars = 1;
            } else {
                return -1;
            }
            flag += 1;
        }

        arg_index += 1;
    }

    remaining = argc - arg_index;
    if (!options->delete_chars && !options->squeeze_chars) {
        if (remaining != 2) {
            return -1;
        }
        if (expand_set(argv[arg_index], options->set1, sizeof(options->set1)) != 0 ||
            expand_set(argv[arg_index + 1], options->set2, sizeof(options->set2)) != 0) {
            return -1;
        }
        options->translate_chars = 1;
        return 0;
    }

    if (options->delete_chars && !options->squeeze_chars) {
        if (remaining != 1) {
            return -1;
        }
        return expand_set(argv[arg_index], options->set1, sizeof(options->set1));
    }

    if (!options->delete_chars && options->squeeze_chars) {
        if (remaining != 1 && remaining != 2) {
            return -1;
        }
        if (expand_set(argv[arg_index], options->set1, sizeof(options->set1)) != 0) {
            return -1;
        }
        if (remaining == 2) {
            if (expand_set(argv[arg_index + 1], options->set2, sizeof(options->set2)) != 0) {
                return -1;
            }
            options->translate_chars = 1;
            rt_copy_string(options->squeeze_set, sizeof(options->squeeze_set), options->set2);
        } else {
            rt_copy_string(options->squeeze_set, sizeof(options->squeeze_set), options->set1);
        }
        return 0;
    }

    if (remaining != 1 && remaining != 2) {
        return -1;
    }

    if (expand_set(argv[arg_index], options->set1, sizeof(options->set1)) != 0) {
        return -1;
    }

    if (remaining == 2) {
        return expand_set(argv[arg_index + 1], options->squeeze_set, sizeof(options->squeeze_set));
    }

    rt_copy_string(options->squeeze_set, sizeof(options->squeeze_set), options->set1);
    return 0;
}

int main(int argc, char **argv) {
    TrOptions options;
    char buffer[4096];
    long bytes_read;
    int have_last_output = 0;
    char last_output = '\0';

    if (parse_options(argc, argv, &options) != 0) {
        print_usage(argv[0]);
        return 1;
    }

    while ((bytes_read = platform_read(0, buffer, sizeof(buffer))) > 0) {
        long i;
        char out[4096];
        size_t out_len = 0;

        for (i = 0; i < bytes_read; ++i) {
            char current = buffer[i];

            if (options.translate_chars) {
                current = translate_char(current, options.set1, options.set2);
            }

            if (options.delete_chars && char_in_set(buffer[i], options.set1)) {
                continue;
            }

            if (options.squeeze_chars &&
                have_last_output &&
                current == last_output &&
                char_in_set(current, options.squeeze_set)) {
                continue;
            }

            out[out_len++] = current;
            have_last_output = 1;
            last_output = current;
        }

        if (out_len > 0 && rt_write_all(1, out, out_len) != 0) {
            return 1;
        }
    }

    return bytes_read < 0 ? 1 : 0;
}
