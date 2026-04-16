#include "platform.h"
#include "runtime.h"

static void print_usage(const char *program_name) {
    rt_write_cstr(2, "Usage: ");
    rt_write_cstr(2, program_name);
    rt_write_line(2, " SET1 SET2");
}

static int translate_char(char ch, const char *set1, const char *set2, char *out) {
    size_t i;
    size_t set2_len = rt_strlen(set2);

    for (i = 0; set1[i] != '\0'; ++i) {
        if (set1[i] == ch) {
            if (set2_len == 0) {
                return 0;
            }

            if (i < set2_len) {
                *out = set2[i];
            } else {
                *out = set2[set2_len - 1];
            }
            return 1;
        }
    }

    *out = ch;
    return 1;
}

int main(int argc, char **argv) {
    char buffer[4096];
    long bytes_read;

    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    while ((bytes_read = platform_read(0, buffer, sizeof(buffer))) > 0) {
        long i;
        char out[4096];
        size_t out_len = 0;

        for (i = 0; i < bytes_read; ++i) {
            char mapped;
            if (translate_char(buffer[i], argv[1], argv[2], &mapped)) {
                out[out_len++] = mapped;
            }
        }

        if (rt_write_all(1, out, out_len) != 0) {
            return 1;
        }
    }

    return bytes_read < 0 ? 1 : 0;
}
