#include "hash_util.h"
#include "runtime.h"
#include "tool_util.h"

static int print_digest_line(const unsigned char *digest, size_t digest_size, const char *label) {
    char hex[HASH_SHA512_SIZE * 2 + 1];

    hash_to_hex(digest, digest_size, hex);
    if (rt_write_cstr(1, hex) != 0 || rt_write_cstr(1, "  ") != 0 || rt_write_line(1, label) != 0) {
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    int exit_code = 0;
    int i;

    if (argc == 1) {
        unsigned char digest[HASH_SHA512_SIZE];
        if (hash_sha512_stream(0, digest) != 0 || print_digest_line(digest, HASH_SHA512_SIZE, "-") != 0) {
            return 1;
        }
        return 0;
    }

    for (i = 1; i < argc; ++i) {
        int fd;
        int should_close;
        unsigned char digest[HASH_SHA512_SIZE];

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "sha512sum: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (hash_sha512_stream(fd, digest) != 0 || print_digest_line(digest, HASH_SHA512_SIZE, argv[i]) != 0) {
            rt_write_cstr(2, "sha512sum: failed on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
