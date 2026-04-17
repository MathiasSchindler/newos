#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define SHUF_MAX_ITEMS 4096
#define SHUF_MAX_ITEM_LENGTH 1024

static unsigned long long shuf_rng_state = 1ULL;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-n COUNT] [-r] [file]");
    tool_write_usage(program_name, "-e [-n COUNT] [-r] arg ...");
    tool_write_usage(program_name, "-i LO-HI [-n COUNT] [-r]");
}

static void seed_rng(void) {
    unsigned long long seed = (unsigned long long)platform_get_epoch_time();
    unsigned long long addr = (unsigned long long)(unsigned long)(&seed);

    shuf_rng_state = seed ^ (addr << 1U) ^ 0x9e3779b97f4a7c15ULL;
    if (shuf_rng_state == 0ULL) {
        shuf_rng_state = 1ULL;
    }
}

static unsigned long long next_random(void) {
    shuf_rng_state = shuf_rng_state * 6364136223846793005ULL + 1ULL;
    return shuf_rng_state;
}

static int parse_signed_value(const char *text, long long *value_out) {
    long long sign = 1;
    unsigned long long value = 0ULL;
    int saw_digit = 0;

    if (text == 0 || text[0] == '\0') {
        return -1;
    }

    if (*text == '-') {
        sign = -1;
        text += 1;
    } else if (*text == '+') {
        text += 1;
    }

    while (*text >= '0' && *text <= '9') {
        saw_digit = 1;
        value = value * 10ULL + (unsigned long long)(*text - '0');
        text += 1;
    }

    if (!saw_digit || *text != '\0') {
        return -1;
    }

    *value_out = (long long)value * sign;
    return 0;
}

static int parse_range_arg(const char *text, long long *low_out, long long *high_out) {
    const char *dash = text + 1;
    char left[32];
    char right[32];
    size_t left_len;
    size_t right_len = 0;

    while (*dash != '\0' && *dash != '-') {
        dash += 1;
    }

    if (*dash != '-') {
        return -1;
    }

    left_len = (size_t)(dash - text);
    if (left_len == 0U || left_len + 1U > sizeof(left)) {
        return -1;
    }
    memcpy(left, text, left_len);
    left[left_len] = '\0';

    dash += 1;
    while (dash[right_len] != '\0' && right_len + 1U < sizeof(right)) {
        right[right_len] = dash[right_len];
        right_len += 1U;
    }
    right[right_len] = '\0';

    if (parse_signed_value(left, low_out) != 0 || parse_signed_value(right, high_out) != 0 || *low_out > *high_out) {
        return -1;
    }

    return 0;
}

static int add_item(char items[SHUF_MAX_ITEMS][SHUF_MAX_ITEM_LENGTH], size_t *count, const char *text) {
    size_t len = rt_strlen(text);

    if (*count >= SHUF_MAX_ITEMS) {
        return -1;
    }

    if (len >= SHUF_MAX_ITEM_LENGTH) {
        len = SHUF_MAX_ITEM_LENGTH - 1U;
    }

    memcpy(items[*count], text, len);
    items[*count][len] = '\0';
    *count += 1U;
    return 0;
}

static int collect_lines_from_fd(int fd, char items[SHUF_MAX_ITEMS][SHUF_MAX_ITEM_LENGTH], size_t *count_out) {
    char chunk[2048];
    char current[SHUF_MAX_ITEM_LENGTH];
    size_t current_len = 0U;

    *count_out = 0U;

    for (;;) {
        long bytes_read = platform_read(fd, chunk, sizeof(chunk));
        long i;

        if (bytes_read < 0) {
            return -1;
        }
        if (bytes_read == 0) {
            break;
        }

        for (i = 0; i < bytes_read; ++i) {
            char ch = chunk[i];

            if (ch == '\n') {
                current[current_len] = '\0';
                if (add_item(items, count_out, current) != 0) {
                    return -1;
                }
                current_len = 0U;
            } else if (current_len + 1U < sizeof(current)) {
                current[current_len++] = ch;
            }
        }
    }

    if (current_len > 0U) {
        current[current_len] = '\0';
        if (add_item(items, count_out, current) != 0) {
            return -1;
        }
    }

    return 0;
}

static void swap_items(char items[SHUF_MAX_ITEMS][SHUF_MAX_ITEM_LENGTH], size_t a, size_t b) {
    char tmp[SHUF_MAX_ITEM_LENGTH];

    if (a == b) {
        return;
    }

    memcpy(tmp, items[a], sizeof(tmp));
    memcpy(items[a], items[b], sizeof(tmp));
    memcpy(items[b], tmp, sizeof(tmp));
}

int main(int argc, char **argv) {
    char items[SHUF_MAX_ITEMS][SHUF_MAX_ITEM_LENGTH];
    size_t item_count = 0U;
    unsigned long long limit = 0ULL;
    int have_limit = 0;
    int repeat = 0;
    int mode = 0;
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] != '\0') {
        if (rt_strcmp(argv[argi], "-n") == 0) {
            if (argi + 1 >= argc || tool_parse_uint_arg(argv[argi + 1], &limit, "shuf", "count") != 0) {
                return 1;
            }
            have_limit = 1;
            argi += 2;
        } else if (rt_strcmp(argv[argi], "-r") == 0) {
            repeat = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-e") == 0) {
            mode = 1;
            argi += 1;
            break;
        } else if (rt_strcmp(argv[argi], "-i") == 0) {
            long long low;
            long long high;
            long long value;

            if (argi + 1 >= argc || parse_range_arg(argv[argi + 1], &low, &high) != 0) {
                print_usage(argv[0]);
                return 1;
            }

            mode = 2;
            for (value = low; value <= high; ++value) {
                char text[32];

                if (value < 0) {
                    text[0] = '-';
                    rt_unsigned_to_string((unsigned long long)(-value), text + 1, sizeof(text) - 1U);
                } else {
                    rt_unsigned_to_string((unsigned long long)value, text, sizeof(text));
                }

                if (add_item(items, &item_count, text) != 0) {
                    tool_write_error("shuf", "too many items", 0);
                    return 1;
                }
            }

            argi += 2;
        } else if (rt_strcmp(argv[argi], "--") == 0) {
            argi += 1;
            break;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (mode == 1) {
        while (argi < argc) {
            if (add_item(items, &item_count, argv[argi]) != 0) {
                tool_write_error("shuf", "too many items", 0);
                return 1;
            }
            argi += 1;
        }
    } else if (mode == 0) {
        int fd;
        int should_close = 0;
        const char *input_path = (argi < argc) ? argv[argi] : 0;

        if (argi + 1 < argc) {
            print_usage(argv[0]);
            return 1;
        }

        if (tool_open_input(input_path, &fd, &should_close) != 0) {
            tool_write_error("shuf", "cannot open ", input_path != 0 ? input_path : "stdin");
            return 1;
        }

        if (collect_lines_from_fd(fd, items, &item_count) != 0) {
            tool_close_input(fd, should_close);
            tool_write_error("shuf", "failed to read input", 0);
            return 1;
        }

        tool_close_input(fd, should_close);
    } else if (argi != argc) {
        print_usage(argv[0]);
        return 1;
    }

    if (item_count == 0U) {
        return 0;
    }

    if (!have_limit) {
        limit = (unsigned long long)item_count;
    }

    seed_rng();

    if (repeat) {
        unsigned long long i;
        for (i = 0ULL; i < limit; ++i) {
            size_t index = (size_t)(next_random() % (unsigned long long)item_count);
            rt_write_line(1, items[index]);
        }
        return 0;
    }

    {
        size_t i;
        size_t output_count = (limit > (unsigned long long)item_count) ? item_count : (size_t)limit;

        for (i = item_count; i > 1U; --i) {
            size_t j = (size_t)(next_random() % (unsigned long long)i);
            swap_items(items, i - 1U, j);
        }

        for (i = 0U; i < output_count; ++i) {
            rt_write_line(1, items[i]);
        }
    }

    return 0;
}
