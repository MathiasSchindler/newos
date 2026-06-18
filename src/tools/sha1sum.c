#include "hash_util.h"

int main(int argc, char **argv) {
    return hash_sum_main(argc, argv, "sha1sum", HASH_SHA1_SIZE, hash_sha1_stream, 1);
}