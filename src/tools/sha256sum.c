#include "hash_util.h"
#include "platform.h"
#include "runtime.h"
#include "tool_util.h"

#define print_digest_line(digest, digest_size, label, zero_terminated) \
    hash_print_digest_line((digest), (digest_size), (label), 0, (zero_terminated))
#define verify_manifest(fd, zero_terminated, quiet, status_only) \
    hash_verify_manifest("sha256sum", (fd), HASH_SHA256_SIZE, hash_sha256_stream, (zero_terminated), (quiet), (status_only))

int main(int argc, char **argv) {
    int check_mode = 0;
    int quiet = 0;
    int status_only = 0;
    int zero_terminated = 0;
    int exit_code = 0;
    int argi = 1;
    int i;

    while (argi < argc) {
        const char *arg = argv[argi];
        size_t j;

        if (rt_strcmp(arg, "--") == 0) {
            argi += 1;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') {
            break;
        }
        if (rt_strcmp(arg, "--check") == 0) {
            check_mode = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "--quiet") == 0) {
            quiet = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "--status") == 0) {
            status_only = 1;
            argi += 1;
            continue;
        }
        if (rt_strcmp(arg, "--zero") == 0) {
            zero_terminated = 1;
            argi += 1;
            continue;
        }

        for (j = 1; arg[j] != '\0'; ++j) {
            if (arg[j] == 'c') {
                check_mode = 1;
            } else if (arg[j] == 'q') {
                quiet = 1;
            } else if (arg[j] == 's') {
                status_only = 1;
            } else if (arg[j] == 'z') {
                zero_terminated = 1;
            } else {
                tool_write_usage(argv[0], "[-c] [-q] [-s] [-z] [FILE ...]");
                return 1;
            }
        }
        argi += 1;
    }

    if (status_only) {
        quiet = 1;
    }

    if (check_mode) {
        if (argi >= argc) {
            return verify_manifest(0, zero_terminated, quiet, status_only);
        }

        for (i = argi; i < argc; ++i) {
            int fd;
            int should_close;

            if (tool_open_input(argv[i], &fd, &should_close) != 0) {
                rt_write_cstr(2, "sha256sum: cannot open ");
                rt_write_line(2, argv[i]);
                exit_code = 1;
                continue;
            }
            if (verify_manifest(fd, zero_terminated, quiet, status_only) != 0) {
                exit_code = 1;
            }
            tool_close_input(fd, should_close);
        }
        return exit_code;
    }

    if (argi >= argc) {
        unsigned char digest[HASH_SHA256_SIZE];
        if (hash_sha256_stream(0, digest) != 0 || print_digest_line(digest, HASH_SHA256_SIZE, "-", zero_terminated) != 0) {
            return 1;
        }
        return 0;
    }

    for (i = argi; i < argc; ++i) {
        int fd;
        int should_close;
        unsigned char digest[HASH_SHA256_SIZE];

        if (tool_open_input(argv[i], &fd, &should_close) != 0) {
            rt_write_cstr(2, "sha256sum: cannot open ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
            continue;
        }

        if (hash_sha256_stream(fd, digest) != 0 || print_digest_line(digest, HASH_SHA256_SIZE, argv[i], zero_terminated) != 0) {
            rt_write_cstr(2, "sha256sum: failed on ");
            rt_write_line(2, argv[i]);
            exit_code = 1;
        }

        tool_close_input(fd, should_close);
    }

    return exit_code;
}
