#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

static unsigned long long shuf_rng_state = 1ULL;
static int shuf_random_source_fd = -1;
static ToolArray shuf_items;

static void print_usage(const char *program_name) {
    tool_write_usage(program_name, "[-n COUNT] [-r] [-z] [-o FILE] [--random-source=FILE] [file]");
    tool_write_usage(program_name, "-e [-n COUNT] [-r] [-z] [-o FILE] [--random-source=FILE] arg ...");
    tool_write_usage(program_name, "-i LO-HI [-n COUNT] [-r] [-o FILE] [--random-source=FILE]");
}

static void seed_rng(void) {
    if (shuf_random_source_fd >= 0) {
        shuf_rng_state = 1ULL;
        return;
    }

    unsigned long long seed = (unsigned long long)platform_get_epoch_time();
    unsigned long long addr = (unsigned long long)(unsigned long)(&seed);

    shuf_rng_state = seed ^ (addr << 1U) ^ 0x9e3779b97f4a7c15ULL;
    if (shuf_rng_state == 0ULL) {
        shuf_rng_state = 1ULL;
    }
}

static unsigned long long next_random(void) {
    if (shuf_random_source_fd >= 0) {
        unsigned char bytes[8];
        size_t used = 0U;
        unsigned long long value = 0ULL;

        while (used < sizeof(bytes)) {
            long bytes_read = platform_read(shuf_random_source_fd, bytes + used, sizeof(bytes) - used);
            if (bytes_read < 0) {
                break;
            }
            if (bytes_read == 0) {
                if (platform_seek(shuf_random_source_fd, 0, PLATFORM_SEEK_SET) < 0) {
                    break;
                }
                continue;
            }
            used += (size_t)bytes_read;
        }

        if (used == sizeof(bytes)) {
            size_t i;
            for (i = 0U; i < sizeof(bytes); ++i) {
                value = (value << 8U) | (unsigned long long)bytes[i];
            }
            if (value != 0ULL) {
                return value;
            }
        }
    }

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

static int shuf_add_item(size_t *count, const char *text) {
    size_t len = rt_strlen(text);
    char **slot = (char **)tool_array_append(&shuf_items);

    if (slot == 0) return -1;
    *slot = (char *)rt_malloc(len + 1U);
    if (*slot == 0) {
        shuf_items.count -= 1U;
        return -1;
    }
    memcpy(*slot, text, len + 1U);
    *count += 1U;
    return 0;
}

static int collect_items_from_fd(int fd,
                                 size_t *count_out,
                                 char delimiter) {
    ToolRecordReader reader;
    ToolByteBuffer record;
    int result = 0;

    *count_out = 0U;
    tool_record_reader_init(&reader, fd, delimiter);
    tool_byte_buffer_init(&record);

    for (;;) {
        int has_record = 0;
        if (tool_record_reader_next_buffer(&reader, &record, &has_record) != 0) {
            result = -1;
            break;
        }
        if (!has_record) break;
        if (shuf_add_item(count_out, (const char *)record.data) != 0) {
            result = -1;
            break;
        }
    }
    tool_byte_buffer_free(&record);
    return result;
}

static void swap_items(size_t a, size_t b) {
    char **left;
    char **right;
    char *tmp;

    if (a == b) return;
    left = (char **)tool_array_get(&shuf_items, a);
    right = (char **)tool_array_get(&shuf_items, b);
    tmp = *left;
    *left = *right;
    *right = tmp;
}

static const char *shuf_item(size_t index) {
    const char *const *slot = (const char *const *)tool_array_get_const(&shuf_items, index);
    return slot != 0 ? *slot : 0;
}

static int write_item(int fd, const char *text, char delimiter) {
    size_t len = rt_strlen(text);

    if (len > 0U && rt_write_all(fd, text, len) != 0) {
        return -1;
    }
    return rt_write_char(fd, delimiter);
}

int main(int argc, char **argv) {
    size_t item_count = 0U;
    unsigned long long limit = 0ULL;
    int have_limit = 0;
    int repeat = 0;
    int mode = 0;
    int argi = 1;
    int zero_terminated = 0;
    const char *output_path = 0;
    const char *random_source_path = 0;
    int output_fd = 1;

    tool_array_init(&shuf_items, sizeof(char *));

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
        } else if (rt_strcmp(argv[argi], "-z") == 0 || rt_strcmp(argv[argi], "--zero-terminated") == 0) {
            zero_terminated = 1;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "-o") == 0 || rt_strcmp(argv[argi], "--output") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            output_path = argv[argi + 1];
            argi += 2;
        } else if (tool_starts_with(argv[argi], "--output=")) {
            output_path = argv[argi] + 9;
            argi += 1;
        } else if (rt_strcmp(argv[argi], "--random-source") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            random_source_path = argv[argi + 1];
            argi += 2;
        } else if (tool_starts_with(argv[argi], "--random-source=")) {
            random_source_path = argv[argi] + 16;
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

                if (shuf_add_item(&item_count, text) != 0) {
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
            if (shuf_add_item(&item_count, argv[argi]) != 0) {
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

        if (collect_items_from_fd(fd, &item_count, zero_terminated ? '\0' : '\n') != 0) {
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

    if (random_source_path != 0) {
        shuf_random_source_fd = platform_open_read(random_source_path);
        if (shuf_random_source_fd < 0) {
            tool_write_error("shuf", "cannot open random source ", random_source_path);
            return 1;
        }
    }

    if (output_path != 0) {
        output_fd = platform_open_write(output_path, 0644U);
        if (output_fd < 0) {
            if (shuf_random_source_fd >= 0) {
                platform_close(shuf_random_source_fd);
            }
            tool_write_error("shuf", "cannot create ", output_path);
            return 1;
        }
    }

    seed_rng();

    if (repeat) {
        unsigned long long i;
        for (i = 0ULL; i < limit; ++i) {
            size_t index = (size_t)(next_random() % (unsigned long long)item_count);
            if (write_item(output_fd, shuf_item(index), zero_terminated ? '\0' : '\n') != 0) {
                tool_write_error("shuf", "write error", 0);
                if (output_fd != 1) {
                    platform_close(output_fd);
                }
                if (shuf_random_source_fd >= 0) {
                    platform_close(shuf_random_source_fd);
                }
                return 1;
            }
        }
        if (output_fd != 1) {
            platform_close(output_fd);
        }
        if (shuf_random_source_fd >= 0) {
            platform_close(shuf_random_source_fd);
        }
        return 0;
    }

    {
        size_t i;
        size_t output_count = (limit > (unsigned long long)item_count) ? item_count : (size_t)limit;

        for (i = item_count; i > 1U; --i) {
            size_t j = (size_t)(next_random() % (unsigned long long)i);
            swap_items(i - 1U, j);
        }

        for (i = 0U; i < output_count; ++i) {
            if (write_item(output_fd, shuf_item(i), zero_terminated ? '\0' : '\n') != 0) {
                tool_write_error("shuf", "write error", 0);
                if (output_fd != 1) {
                    platform_close(output_fd);
                }
                if (shuf_random_source_fd >= 0) {
                    platform_close(shuf_random_source_fd);
                }
                return 1;
            }
        }
    }

    if (output_fd != 1) {
        platform_close(output_fd);
    }
    if (shuf_random_source_fd >= 0) {
        platform_close(shuf_random_source_fd);
    }

    return 0;
}
